// The retail host of the shared session-client core
// (docs/session-client-core-design.md 3.2; step R0 of 6.2).
//
// The core (BallanceMMOCommon/src/session/client_core*.cpp) holds the whole
// client session flow; a host owns the loop and implements the platform
// interface (session/client_platform.hpp) with what it has.  This file is the
// retail mod's host:
//
//   * the mailbox: the network thread copies the bytes of every Session*
//     message, SessionStart and SessionLead included, into one mutex-guarded
//     queue, in arrival order; the snapshot entries are capped like the
//     legacy queue (64).  Whether a session's messages go there is decided
//     once, at its SessionStart (the latch);
//   * the retail platform: every design 2.2 operation, each with the retail
//     column's means (physics_view, CK objects, the BML hooks' state), and
//     the host support templates (game/session_host_support.hpp) instantiated
//     with physics_view;
//   * the transition switch 'session core on|off' (design 6.2; off by
//     default) and the verbs 'session force-rollback E:D', 'net lag|blackout'
//     and 'replay session <file>' (the R steps' testing tools).
//
// Step R0 built the plumbing.  Step R1 hands a session to the core: with
// 'session core on' a session's messages are the mailbox's alone (the legacy
// handlers never see them), and this host runs the session the way the legacy
// path did - the countdown, the restart, the anchor work and the blocking
// anchor hold, then one frame per OnProcess (the host's clock guard and pause
// edits, core.post_step(T), the mailbox drained into the core, core.pre_step()
// for T + 1) - while the core decides everything the session does (design
// 1.3, 3.2).  The BML hooks and the automation verbs a session uses hand over
// to the core for as long as a core session runs.  With 'session core off'
// nothing here touches a session: the legacy path does not read the switch.
//
// The BML constraint (design 3.2): BallanceMMOClient hangs the engine at
// startup when a data member is added to it, so everything this host keeps is
// a file-scope static here.  The mailbox is one from the DLL's load on (the
// network thread pushes into it at once); the core is made lazily.
#include "../BallanceMMOClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#if defined(_MSC_VER) && defined(_M_IX86)
#include <float.h>
#endif

#include <game/array_reads.hpp>
#include <game/depth_test.hpp>
#include <game/force_block_alias.hpp>
#include <game/host_backend.hpp>
#include <game/hull_template.hpp>
#include <game/mirror_clones.hpp>
#include <game/proximity_union.hpp>
#include <game/sector_activation.hpp>
#include <game/sector_cell_split.hpp>
#include <game/sector_placement.hpp>
#include <game/session_host_support.hpp>
#include <game/trafo_sets.hpp>
#include <game/tutorial_exit.hpp>
#include <session/bridge_recipe.hpp>
#include <session/client_core.hpp>
#include <session/spawn_impulse.hpp>
#include <session/client_platform.hpp>
#include <session/io_pool.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <session/session_host_world.hpp>

#include "core_host.hpp"
#include "session_journal_client.hpp"

namespace {
    // Step B2SX (game/sector_cell_split.hpp): the checkpoint chain's own
    // IngameParameter[0][1] while the session core runs (the legacy path
    // keeps the cell it polls).  File scope: BallanceMMOClient keeps its
    // layout.
    bmmo::game::sector_cell_split core_chain_cell;
    // Step RK3 (record-format-gaps-design.md 8 row K3 = C4, option (c)): the
    // Level 1 tutorial's exit event, found at the core session's anchor and
    // activated by the core before frame 10 (platform tutorial_exit).
    bmmo::game::tutorial_exit core_tutorial_exit;
    // Step RK3H (the user, 2026-10-29: 'Hold frame-9 write'): the tutorial's
    // first time-factor write (0, frame 9) held at the in-play factor from
    // the anchor on; kept by the frame guard, handed back at the session's
    // end and before the next anchor's hold (base.cmo's Gameplay_Tutorial
    // outlives the session).  File scope: BallanceMMOClient keeps its layout.
    bmmo::game::tutorial_factor_hold core_tutorial_hold;
    // FULL step PRX (game/depth_test.hpp): Gameplay_Ingame/DepthTest's Test
    // pinned at the core session's anchor; the core runs its rule at one
    // point of every live and replayed tick (platform depth_test), as the
    // server's and the headless worlds do.  Kept by the frame guard, handed
    // back at the session's end and before the next anchor's pin (base.cmo's
    // Gameplay_Ingame outlives the session).  File scope: BallanceMMOClient
    // keeps its layout.
    bmmo::game::depth_test_rule core_depth_test;
    // FULL step RT2 (sector-placement-design 7.5 run 5's trace line): the
    // manager state the union's live pass reads, logged as "sector manager:
    // tick T active|idle" for the first frames from an anchor (a late
    // joiner's included).  The pass of tick T reads it right after the
    // frame's after_rewinds hook, which arms the line, so a re-derivation's
    // read inside a rewind (no hook before it) is not logged.  File scope.
    int core_manager_trace_left = 0;
    uint32_t core_manager_trace_tick = 0;
    bool core_manager_trace_due = false;
}


// Host logic written once (step HU1, docs/roadmap.md section 2): this host
// calls the shared placement helper (game/sector_placement.hpp
// place_at_anchor, release_placement; step S10), the shared force hooks
// (game/host_backend.hpp force_hooks_for) and the union pass's writes
// (game/sector_activation.hpp run_sector_pass) with its own platform - the
// bridge (physics_view), the logger, the file-scope registry.

namespace {
    namespace bs = bmmo::session;
    using clock_type = std::chrono::steady_clock;

    // ------------------------------------------------------------ report precision
    // Step TR4c (trafo design, As built (step TR4 = T4) residual (a)): the
    // retail game thread runs the x87 at 24-bit precision (Direct3D), and
    // the bridge's body read reports a rotation through IVP's
    // calc_at_quaternion, whose slerp branch (a core turning > 2.6 degrees
    // per PSI) then comes out ~1e-7 off unit length - the journal's rows
    // differ from the server's although the state is exact.  The x87 is
    // switched to 53 bits around the read that feeds the journal's
    // checkpoint rows only (platform.list_bodies: its one caller is
    // client_journal::local_checkpoint) and the caller's control word is
    // restored on scope exit.  Not around platform.body_state: the core's
    // corrections, resync and rollback capture read it, and those feed the
    // simulation.  The x64 and Linux hosts run SSE2 doubles (53-bit) already.
    // How many reads ran switched, and how many could not switch (the
    // session end's log line, step SP10: TR4c confirmed in a retail run).
    uint64_t report_precision_reads = 0, report_precision_failed = 0;

    // ------------------------------------------------------------ the spawn ghost display
    // Step SPGH: the own ball drawn translucent while it is a spawn ghost -
    // its current mesh's materials at half alpha with alpha blending, each
    // material's own diffuse and blend state kept here and put back when it
    // turns solid (or dies, or the session ends).  The mirrors are the spirit
    // balls' own materials (game_objects::set_ghost).  File scope:
    // BallanceMMOClient keeps its layout.
    struct own_ghost_material {
        VxColor diffuse;
        CKBOOL alpha_blend = FALSE;
        VXBLEND_MODE source = VXBLEND_ONE, dest = VXBLEND_ZERO;
    };
    // Called on every frame the own ball is a ghost (the core re-asserts it:
    // the level start's fade-in writes the same materials), so the entity
    // is looked up once per name and a material's own state is kept the
    // first time it is seen; its alpha comes back as 1 (a state kept while a
    // fade-in was still running would otherwise leave the ball see-through).
    std::map<CK_ID, own_ghost_material> own_ghost_materials;
    std::string own_ghost_name;
    CK_ID own_ghost_entity = 0;
    void show_own_spawn_ghost(CKContext* context, const std::string& name, bool ghost) {
        if (!ghost) {
            for (const auto& [id, saved]: own_ghost_materials)
                if (auto* mat = static_cast<CKMaterial*>(context ? context->GetObject(id) : nullptr)) {
                    VxColor color = saved.diffuse;
                    color.a = 1.0f;
                    mat->SetDiffuse(color);
                    mat->EnableAlphaBlend(saved.alpha_blend);
                    mat->SetSourceBlend(saved.source);
                    mat->SetDestBlend(saved.dest);
                }
            own_ghost_materials.clear();
            own_ghost_name.clear();
            own_ghost_entity = 0;
            return;
        }
        if (!context) return;
        auto* ball = CK3dObject::Cast(own_ghost_name == name ? context->GetObject(own_ghost_entity) : nullptr);
        if (!ball) {
            ball = CK3dObject::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()), CKCID_3DOBJECT,
                                                                           nullptr));
            own_ghost_name = name;
            own_ghost_entity = ball ? ball->GetID() : 0;
        }
        CKMesh* mesh = ball ? ball->GetCurrentMesh() : nullptr;
        if (!mesh) return;
        for (int i = 0; i < mesh->GetMaterialCount(); ++i) {
            CKMaterial* mat = mesh->GetMaterial(i);
            if (!mat) continue;
            if (!own_ghost_materials.count(mat->GetID())) {
                own_ghost_material saved;
                saved.diffuse = mat->GetDiffuse();
                saved.alpha_blend = mat->AlphaBlendEnabled();
                saved.source = mat->GetSourceBlend();
                saved.dest = mat->GetDestBlend();
                own_ghost_materials[mat->GetID()] = saved;
            }
            VxColor color = mat->GetDiffuse();
            color.a = 0.5f;
            mat->SetDiffuse(color);
            mat->EnableAlphaBlend(TRUE);
            mat->SetSourceBlend(VXBLEND_SRCALPHA);
            mat->SetDestBlend(VXBLEND_INVSRCALPHA);
        }
    }
    struct report_precision_53 {
#if defined(_MSC_VER) && defined(_M_IX86)
        unsigned int saved = 0;
        bool ok = false;
        report_precision_53() {
            unsigned int unused = 0;
            ok = _controlfp_s(&saved, 0, 0) == 0 && _controlfp_s(&unused, _PC_53, _MCW_PC) == 0;
            ++(ok ? report_precision_reads : report_precision_failed);
        }
        ~report_precision_53() {
            unsigned int unused = 0;
            if (ok) _controlfp_s(&unused, saved & _MCW_PC, _MCW_PC);
        }
#else
        report_precision_53() = default;
#endif
        report_precision_53(const report_precision_53&) = delete;
        report_precision_53& operator=(const report_precision_53&) = delete;
    };

    // ------------------------------------------------------------ the switch
    // Written on the game thread (the verb), read by the network thread
    // (core_host_offer): atomic.  `core_latched` is the switch as a session's
    // SessionStart found it, read once there (on the network thread, where
    // the arrival order is made): that session's messages go to the mailbox
    // up to its SessionEnd, and the verb refuses a change meanwhile.  On by
    // default (the user, 2026-10-27, once the server's placement defaulted on
    // at S10 and refuses a member without the core: "core在正式客户端开启吧");
    // 'session core off' still selects the legacy path for the next session.
    std::atomic<bool> core_on{true};
    std::atomic<bool> core_latched{false};

    // ------------------------------------------------------------ the mailbox
    // Design 3.2 "Receive": every Session* opcode's bytes, in arrival order.  A
    // load-time object: the network thread pushes before any session exists.
    struct mailbox_entry {
        int code = 0;
        std::string bytes;
    };
    class core_mailbox {
    public:
        static constexpr size_t kMaxSnapshots = bs::physics_session_state::kMaxQueuedSnapshots;

        void push(int code, const void* data, size_t size) {
            std::lock_guard lk(mutex_);
            if (code == bmmo::SessionSnapshot) {
                // The legacy queue's cap (PSH kMaxQueuedSnapshots), on the
                // snapshot entries only: the oldest one goes.
                if (snapshots_ >= kMaxSnapshots) {
                    for (auto it = entries_.begin(); it != entries_.end(); ++it)
                        if (it->code == bmmo::SessionSnapshot) {
                            entries_.erase(it);
                            --snapshots_;
                            ++dropped_;
                            break;
                        }
                }
                ++snapshots_;
            }
            entries_.push_back({code, std::string(static_cast<const char*>(data), size)});
            ++pushed_;
        }
        std::deque<mailbox_entry> take() {
            std::lock_guard lk(mutex_);
            std::deque<mailbox_entry> out;
            out.swap(entries_);
            snapshots_ = 0;
            return out;
        }
        uint64_t pushed() const {
            std::lock_guard lk(mutex_);
            return pushed_;
        }
        uint64_t dropped() const {
            std::lock_guard lk(mutex_);
            return dropped_;
        }

    private:
        mutable std::mutex mutex_;
        std::deque<mailbox_entry> entries_;
        size_t snapshots_ = 0;
        uint64_t pushed_ = 0, dropped_ = 0;
    };
    core_mailbox mailbox;

    // ------------------------------------------------------------ host state
    // The retail platform's own state (design 3.2 "Game objects", "Camera",
    // the anchor's script replay), kept apart from the legacy
    // physics_session_state, which keeps its layout until R7.
    struct retail_host {
        // The core, made at each SessionStart with the switch on (R1: a fresh
        // core per session, so no state of the last one carries over); the
        // login id is given to it before each begin (design 3.2, E11).
        std::unique_ptr<bs::session_client_core> core;
        std::string core_error;   // why it could not be made
        uint64_t sessions_seen = 0;

        // The session this host runs (R1): its phase - the legacy path's
        // counting_down / restarting / running, the core's own phase
        // inside it - and what the countdown, the restart and the frame
        // keep.
        enum class phase_type { idle, counting_down, restarting, running };
        phase_type phase = phase_type::idle;
        uint32_t session = 0;
        bool saw_ingame_inactive = false;
        clock_type::time_point restart_deadline{};
        uint64_t last_rebases = 0;
        uint64_t frames = 0, refused = 0;
        // (The transitional host triggers of design 3.2 are gone: the input
        // starvation detector at R4 - the core's held notices and
        // check_held_complete answer what it guessed at - and the run of
        // snapshots with no record of their tick at B2SW, a core rule for
        // both hosts since, design 7.1 decision 1.)

        // The mailbox drain: what the game thread took, dispatched to the
        // core or - while no core session runs (R0: always) - decoded, to
        // check the copy, and dropped.
        uint64_t drained = 0, discarded = 0, undecodable = 0, starts = 0;

        // The navigation graph with retail's unbound-leaf filter (design
        // 4.1), the own replica (design 9.6), the own collision group and
        // the body guard (engine change #6).
        bmmo::game::navigation_graph navigation;
        bool navigation_known = false;
        // Step DX3 (rollback-engine-gaps design 9.3 (b)): Gameplay_Ingame's
        // SetPhysicsForce blocks, read with the graph, for the core's check
        // that their controllers push nothing.
        std::vector<uint32_t> outside_force_blocks;
        bmmo::game::own_replica own_replica;
        // Step EL8 (gap L, L3): the navigations made for revived own lives
        // (the trafo's old ball), destroyed with the body at the replayed
        // death, and the own bodies make_own / unmake_own made and took out
        // (the guard's log line says what a rewind did).
        std::set<std::string> revive_navigations;
        uint64_t own_bodies_made = 0, own_bodies_unmade = 0;
        bool own_group_set = false;
        bool body_guard = false;
        std::string body_guard_entity;
        // The pause menu's mute in force (the replica's polling follows it;
        // design 2.2 observe_input / unforce_own).
        bool input_muted = false;
        // The mirrors this host made: entity -> player, player -> ball type
        // (the spirit ball shown, objects_.on_trafo's "from").
        std::map<std::string, uint32_t> mirror_player;
        std::map<uint32_t, uint8_t> mirror_type;
        // Step BMX: the shared rule's contract (game/mirror_clones.hpp) - the
        // spirit ball of (player, type) is the one entity of every life of
        // that player and type in a session; a different one is counted and
        // logged (game_objects' reload mid-session is the path that would).
        bmmo::game::mirror_entity_contract mirror_contract;
        // Physicalize_GameBall's "Force" per ball type, read at the anchor
        // (the navigation's force; the legacy ball_forces).
        std::vector<float> ball_forces;

        // Every body this world made (bind_session_world's birth log; the
        // retail feed is OnPhysicalize, design 2.4), every manager
        // activation the live frames made (replayed inside rewinds), and the
        // script replay's selection and bookkeeping (made at the anchor).
        bs::local_birth_log births;
        bmmo::game::manager_activations activations;
        uint64_t activations_replayed = 0;
        uint32_t cell_write_logs = 0;   // steps N02R, N01R: the [0][2] / [0][1] writes replayed, logged (the first 8 hook calls that wrote a cell)
        // Step RO1: the order the live frames ran the replay's scripts in
        // (session/script_order.hpp; host-local, recorded after each frame).
        bs::live_script_order script_order;
        bmmo::game::script_replay::selection script_ids;
        // The selected scripts whose pending activations are recorded and
        // re-issued (the trafo scripts, step P, and the keep-local ones, A1;
        // host support's session_script_activations).
        std::vector<CK_ID> script_activations;
        // The trafo explosion pieces' entities (step P; made at the anchor):
        // tracked by the core like any mechanism, never a BodyRevived.
        std::set<std::string> explosion_pieces;
        std::vector<CK_ID> scripts_replayed;
        bmmo::game::script_replay_hooks script_hooks;
        std::map<CK_ID, uint32_t> script_reset_ticks;
        uint64_t script_reset_skips = 0;
        uint32_t restore_logs = 0, savestate_logs = 0, contact_logs = 0, destroy_logs = 0, line_logs = 0;

        // 'session force-rollback E:D' (hooks.after_rewinds): every E ticks,
        // a rewind D ticks deep.
        uint32_t force_every = 0, force_depth = 12;
        uint64_t forced = 0;
        // 'session force-rollback at L:D[,L:D...]' (step EL8, rollback-engine
        // gaps design 7.4 L-R1's one-shot form): one rewind D deep after the
        // frame of live tick L, each once.
        std::vector<std::pair<uint32_t, uint32_t>> force_once;

        // 'session activate <root script> at b+N|<tick>' (FULL step RT2, a
        // testing tool: RK3F's retail trigger, the headless client's
        // --activate-script): the scene activates the script once, after the
        // frame whose live tick reaches the target (b+N: N ticks after this
        // session's tick base, so it can be armed before the anchor).  Live
        // only, as the headless switch: no replay re-applies it.
        std::string activate_script;
        bool activate_armed = false, activate_relative = false;
        uint32_t activate_at = 0;

        // 'session drop-birth' (a testing tool, R1 (c)): the next own
        // Physicalize the core sends is kept off the wire (hooks.
        // filter_event, every copy), so the server has no body for our ball
        // and own_body_backstop has something to do.
        int drop_births_armed = 0;
        uint64_t births_dropped = 0;

        // 'beam <x> <y> <z> at <tick>' (a testing tool, R2 (a)): a beam of
        // the running core session stamped exactly <tick>, so two runs of
        // one key record beam at the same tick (the plain verb's stamp is
        // the frame the command file happens to be read in).
        struct pending_beam {
            uint32_t stamp = 0;
            double position[3] = {};
        };
        std::vector<pending_beam> beams_pending;
        uint64_t beams_made = 0;
        // The armed beam handed to the core this frame, logged with the stamp
        // the core actually gave it (a refused post_step keeps the request
        // inside the core for a later one).
        bool beam_in_flight = false;
        uint32_t beam_in_flight_armed = 0;

        // R2's record sources against R1's (a coverage counter, review
        // finding R2-3): frames whose recorded NAV_ACTIVE (the replica's
        // leaves) differs from the BML hook's ball_nav_active_, and frames
        // whose recorded mute (the one in force during the step) differs from
        // the pause state decided in the same frame (R1's source).
        uint64_t nav_source_differs = 0;
        uint64_t mute_source_differs = 0;

        // The mirror-birth log (a diagnostic, R3's check "mirror birth pose
        // at its stamp"): after the frame's rewinds, a mirror made at its
        // stamp (birth_known) has its record of the stamp tick and the next
        // one logged whenever a replay changed them, so the last line per
        // birth is the pose the history keeps, comparable with the owner's
        // own checkpoints.  Key (player, stamp); value the last positions
        // logged (stamp, stamp + 1) and the lines so far.
        struct birth_seen {
            double at[2][3] = {};
            bool have[2] = {false, false};
            uint32_t lines = 0;
        };
        std::map<std::pair<uint32_t, uint32_t>, birth_seen> mirror_births;

        // 'net lag <ms> [up|down]' and 'net blackout <ms> [up|down]', each
        // now or 'at <tick>': GNS's fake lag and loss (global configuration
        // values), a deadline per direction.  A blackout ends at a frame
        // boundary (core_host_process), so an anchor hold in progress (which
        // waits inside one frame) stretches it.
        int lag_up_ms = 0, lag_down_ms = 0;
        bool blackout_up = false, blackout_down = false;
        clock_type::time_point blackout_up_until{}, blackout_down_until{};
        struct net_action {
            bool lag = false;   // lag, or else a blackout
            long ms = 0;
            bool up = false, down = false;
            uint32_t at = 0;
        };
        std::vector<net_action> net_pending;

        // 'replay session <file>': frame k of a key record fed to the input
        // poll of tick first_tick + k, first_tick being the session's first
        // assigned frame; no clock reset, nothing recorded.
        struct replay_state {
            bmmo::physics::tick_record source;
            std::string path;
            bool armed = false, started = false;
            uint32_t first_tick = 0, last_tick = 0;
            uint64_t fed = 0, jumps = 0;
            int64_t last_frame = -1;
        } replay;
    };
    retail_host host;

    // Step BALL (review finding 1): the airship ghosts handed to the legacy
    // display.  game_objects::update moves such a spirit ball's entity by the
    // owner's legacy stream - a wall-clock pose, different in every world -
    // but the proximity union's place point (proximity_union.hpp place,
    // entity->GetPosition) and the savestate's entity matrix read a ball's CK
    // matrix inside the core's frame, where the server and every other world
    // have the frozen body's last physics_RT write.  So the CK matrix belongs
    // to physics inside core_host_frame and to the stream outside it:
    //   * a ghost is handed to the legacy display only while its body is
    //     frozen (the zone's freeze; physics_RT then writes no CK matrix for
    //     it), so the matrix it had when it was handed over is the physics
    //     one, and it stays the same while the frozen state does;
    //   * captured inside the frame (CK physics-owned there: the entity was
    //     physicalized until handed over, pinned at the frame's start, or
    //     written by a restore / replay, which never run the stream), with
    //     the frozen body's pose it belongs to;
    //   * written back at the frame's start (objects_.update, after
    //     core_host_process in OnProcess, moved it for the render), so the
    //     records, restores, replays and place points of the frame see it;
    //   * a body no longer frozen at the frame's end (a replay or a beam took
    //     it out) is physicalized again at once: the stream never moves an
    //     entity whose body physics_RT writes.
    // File scope: BallanceMMOClient keeps its layout.
    struct ghost_pin {
        bool held = false;   // matrix and pose captured
        VxMatrix matrix;
        double position[3] = {};
        double rotation[4] = {};
    };
    std::map<std::string, ghost_pin> ghost_pins;

    CK3dEntity* ghost_entity(CKContext* context, const std::string& name) {
        return context ? CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()),
                                                                                 CKCID_3DENTITY, nullptr))
                       : nullptr;
    }

    bool ghost_pin_matches(const ghost_pin& pin, const bmmo_physics_body_state& state) {
        return pin.held && std::memcmp(pin.position, state.position, sizeof(pin.position)) == 0
               && std::memcmp(pin.rotation, state.rotation, sizeof(pin.rotation)) == 0;
    }

    // Inside the frame only: CK holds the physics matrix there.
    void ghost_pin_capture(ghost_pin& pin, CK3dEntity* entity, const bmmo_physics_body_state& state) {
        if (ghost_pin_matches(pin, state)) return;
        pin.matrix = entity->GetWorldMatrix();
        std::memcpy(pin.position, state.position, sizeof(pin.position));
        std::memcpy(pin.rotation, state.rotation, sizeof(pin.rotation));
        pin.held = true;
    }

    void ghost_release(game_objects& objects, const std::string& name) {
        if (const auto it = host.mirror_player.find(name); it != host.mirror_player.end())
            objects.set_physicalized(it->second, true);
    }

    // The frame's start: every held ghost's physics matrix back.  A frozen
    // body that moved since its capture (nothing between two frames can move
    // one) is released rather than pinned to a stale matrix.
    void ghost_pins_begin(CKContext* context, bmmo::physics::physics_view& view, game_objects& objects) {
        std::string error;
        for (auto it = ghost_pins.begin(); it != ghost_pins.end();) {
            bmmo_physics_body_state state{};
            CK3dEntity* entity = ghost_entity(context, it->first);
            if (!entity || !it->second.held) {
                ++it;
                continue;
            }
            const bool body = view.get_body_state(it->first.c_str(), state, error);
            if (!body || state.simulated || !ghost_pin_matches(it->second, state)) {
                if (body) ghost_release(objects, it->first);
                it = ghost_pins.erase(it);
                continue;
            }
            entity->SetWorldMatrix(it->second.matrix);
            ++it;
        }
    }

    // The frame's end, before objects_.update moves the ghosts for the
    // render: each frozen ghost's matrix (re)captured - a replay of this
    // frame may have frozen it elsewhere - and a body the frame unfroze
    // released.  A mirror without a body (a death) is left a legacy ball, as
    // unmake_mirror left it.
    void ghost_pins_end(CKContext* context, bmmo::physics::physics_view& view, game_objects& objects) {
        std::string error;
        for (auto it = ghost_pins.begin(); it != ghost_pins.end();) {
            bmmo_physics_body_state state{};
            CK3dEntity* entity = ghost_entity(context, it->first);
            if (!entity || !view.get_body_state(it->first.c_str(), state, error)) {
                it = ghost_pins.erase(it);
                continue;
            }
            if (state.simulated) {
                ghost_release(objects, it->first);
                it = ghost_pins.erase(it);
                continue;
            }
            ghost_pin_capture(it->second, entity, state);
            ++it;
        }
    }

    const char* opcode_name(int code) {
        switch (code) {
        case bmmo::SessionStart: return "SessionStart";
        case bmmo::SessionAssign: return "SessionAssign";
        case bmmo::SessionSnapshot: return "SessionSnapshot";
        case bmmo::SessionRemoteInput: return "SessionRemoteInput";
        case bmmo::SessionEvent: return "SessionEvent";
        case bmmo::SessionEnd: return "SessionEnd";
        case bmmo::SessionLead: return "SessionLead";
        case bmmo::SessionResyncState: return "SessionResyncState";
        case bmmo::SessionMember: return "SessionMember";
        default: return "?";
        }
    }

    // A mailbox entry's bytes into its message (the bytes as the network
    // thread copied them, m_cbSize of them).
    template <class Msg>
    bool decode_bytes(const std::string& bytes, Msg& msg) {
        msg.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return msg.deserialize();
    }

    // Whether an entry decodes as its opcode's message: the check the drain
    // makes of what it drops while no core session runs.
    bool entry_decodes(int code, const std::string& bytes) {
        switch (code) {
        case bmmo::SessionStart: { bmmo::session_start_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionAssign: { bmmo::session_assign_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionSnapshot: { bmmo::session_snapshot_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionRemoteInput: { bmmo::session_remote_input_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionEvent: { bmmo::session_event_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionLead: { bmmo::session_lead_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionEnd: { bmmo::session_end_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionResyncState: { bmmo::session_resync_state_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionMember: { bmmo::session_member_msg m{}; return decode_bytes(bytes, m); }
        default: return false;
        }
    }

    // The level's ball table (Physicalize_GameBall): a ball's row by entity
    // name, -1 for none - the type the wire and the journal carry (the legacy
    // path reads it through db_.get_ball_id, whose map is filled from these
    // rows).
    int ball_row_of(IBML* bml, const std::string& name) {
        if (name.empty()) return -1;
        CKDataArray* table = bml->GetArrayByName("Physicalize_GameBall");
        if (!table) return -1;
        const int rows = table->GetRowCount();
        for (int row = 0; row < rows; ++row) {
            char text[128] = {};
            table->GetElementStringValue(row, 0, text);
            text[sizeof(text) - 1] = 0;
            if (name == text) return row;
        }
        return -1;
    }

    // The retail "physicalize new Ball" recipe of a ball type from its table
    // row (design 2.2 ball_recipe_of: the beam's and the own birth's recipe).
    // The headless host fills the same rule from the same columns
    // (session_client.cpp fill_retail_recipe); both are candidates for host
    // support (design 2.4), which R0 left unchanged.
    void fill_ball_recipe(IBML* bml, int row, bs::ball_recipe& r) {
        r = {};
        CKDataArray* table = row >= 0 ? bml->GetArrayByName("Physicalize_GameBall") : nullptr;
        if (!table || row >= table->GetRowCount()) return;
        char text[128] = {};
        table->GetElementStringValue(row, 0, text);
        text[sizeof(text) - 1] = 0;
        const std::string name = text;
        r.fixed = false;
        r.start_frozen = false;
        r.enable_collision = true;
        r.calc_mass_center = false;
        table->GetElementValue(row, 1, &r.friction);
        table->GetElementValue(row, 2, &r.elasticity);
        table->GetElementValue(row, 3, &r.mass);
        table->GetElementValue(row, 5, &r.linear_damp);
        table->GetElementValue(row, 6, &r.rot_damp);
        r.collision_surface = name + "_Mesh";
        if (name == "Ball_Paper") {
            r.convex_meshes.push_back(name + "_Mesh");
        } else {
            bs::ball_recipe::sphere sphere;
            sphere.radius = 2.0f;
            r.balls.push_back(sphere);
        }
    }

    float ball_force(uint8_t ball_type) {
        if (ball_type < host.ball_forces.size()) return host.ball_forces[ball_type];
        return host.navigation.leaves.empty() ? 0.0f : host.navigation.leaves.front().force_value;
    }

    // The next tick the world steps, when a session is running and numbered:
    // the core's with the switch on and a core session running, the legacy
    // state's otherwise.  The 'replay session' feed reads it at the input
    // poll, before the step.
    bool next_session_tick(const bs::physics_session_state& legacy, uint32_t& tick) {
        if (host.core && host.core->session_phase() == bs::client_core_state::phase::running) {
            if (!host.core->assigned() || host.core->holding()) return false;
            tick = host.core->next_tick();
            return true;
        }
        if (legacy.phase != bs::physics_session_state::phase_type::running || !legacy.assigned || legacy.hold_active)
            return false;
        tick = legacy.tick_base + static_cast<uint32_t>(std::max<int64_t>(legacy.frames_since_anchor, 0));
        return true;
    }

    // The tick the world stepped last (the 'net' verbs' "from tick" and
    // their 'at <tick>'): the core's with a core session running, the legacy
    // state's otherwise.
    uint32_t session_tick_now(const bs::physics_session_state& legacy) {
        if (host.core && host.core->session_phase() != bs::client_core_state::phase::idle)
            return host.core->current_tick();
        return legacy.current_tick();
    }

    std::string join_flags(bool up, bool down) {
        if (up && down) return "both";
        if (up) return "up";
        if (down) return "down";
        return "none";
    }

    // A core session past its anchor whose core runs (the hooks' and verbs'
    // gate): the legacy path's `phase == running`.
    bool core_session_running() {
        return host.phase == retail_host::phase_type::running && host.core
            && host.core->session_phase() == bs::client_core_state::phase::running;
    }

    // ------------------------------------------------------------ the slew
    // Step R5 (late-player design 2.4 "Retail"): a SessionLead's slew of the
    // tick driver's schedule.  The core decides it (slew_step: the 2x raise,
    // the 0.9x lower, the ack when done; rebase_threshold: 33 + what is still
    // to move); the driver moves its origin by what the step returns, before
    // it reads the schedule, as the headless pacer does.  A raise then runs
    // two frames per slot through SkipRenderForNextTick, a lower waits.
    static_assert(bs::fixed_tick_driver::kMaxCatchUpTicks == bs::kPacerRebaseFrames,
                  "the retail rebase threshold is the core's lead_rebase_threshold base");
    class core_pacer_slew final : public bs::pacer_slew {
    public:
        int step(clock_type::time_point now, double behind) override {
            if (!core_session_running() || !host.core->slewing()) return 0;
            return host.core->slew_step(now, bs::kTickRate, behind);
        }
        double rebase_threshold() const override {
            return core_session_running() ? host.core->rebase_threshold() : bs::fixed_tick_driver::kMaxCatchUpTicks;
        }
    };
    core_pacer_slew slew_pacer;

    const char* host_phase_name(retail_host::phase_type phase) {
        switch (phase) {
        case retail_host::phase_type::counting_down: return "counting_down";
        case retail_host::phase_type::restarting: return "restarting";
        case retail_host::phase_type::running: return "running";
        default: return "idle";
        }
    }

    // The platform's own state of one session, back to a session's start
    // (the legacy reset_runtime's share of it).
    void reset_session_state() {
        host.navigation = {};
        host.navigation_known = false;
        host.outside_force_blocks.clear();   // step DX3: read again with the graph
        host.own_replica = {};
        host.revive_navigations.clear();
        host.own_group_set = false;
        host.input_muted = false;
        host.mirror_player.clear();
        host.mirror_contract.clear();   // step BMX
        ghost_pins.clear();   // step BALL
        host.mirror_type.clear();
        host.ball_forces.clear();
        host.births.clear();
        host.activations.clear();
        host.activations_replayed = 0;
        host.cell_write_logs = 0;
        host.script_order.clear();
        host.script_ids = {};
        host.script_activations.clear();
        host.explosion_pieces.clear();
        host.scripts_replayed.clear();
        host.script_hooks = {};
        host.script_reset_ticks.clear();
        host.script_reset_skips = 0;
        host.restore_logs = host.savestate_logs = host.contact_logs = host.destroy_logs = host.line_logs = 0;
        host.forced = 0;
        host.force_once.clear();
        // (Step RT2's 'session activate' is not cleared here: it is armed
        // before the anchor, after this reset of the session's begin; the
        // session's end clears it.)
        host.frames = host.refused = 0;
        host.drop_births_armed = 0;
        host.births_dropped = 0;
        host.beams_pending.clear();
        host.beams_made = 0;
        host.beam_in_flight = false;
        host.beam_in_flight_armed = 0;
        host.nav_source_differs = host.mute_source_differs = 0;
        host.mirror_births.clear();
    }

    // Step BMX: the shared mirror rule checked on the spirit ball about to
    // be used for (player, type) - the entity of that player's earlier lives
    // of the type in this session.  A different one is counted and logged
    // (the session goes on with it: the core names whatever entity this host
    // gives it).
    void check_mirror_contract(logger_wrapper* logger, uint32_t player, uint8_t ball_type, CK3dObject* entity) {
        CK_ID previous = 0;
        std::string previous_name;
        const std::string name = bmmo::game::entity_name(entity);
        if (host.mirror_contract.check(player, ball_type, entity->GetID(), name, &previous, &previous_name)) return;
        logger->Warn("Physics session: mirror contract broken #%llu: player %u type %u uses spirit ball %s (id %u), "
                     "not the entity of its earlier lives (%s, id %u)",
                     static_cast<unsigned long long>(host.mirror_contract.violations()), player,
                     static_cast<unsigned>(ball_type), name.c_str(), static_cast<unsigned>(entity->GetID()),
                     previous_name.c_str(), static_cast<unsigned>(previous));
    }

    // The mirror-birth log (R3, a diagnostic; see retail_host::mirror_births):
    // read after the frame's rewinds, nothing decided on it.
    void log_mirror_births(logger_wrapper* logger, const bs::session_client_core& core) {
        constexpr uint32_t kLinesPerBirth = 24;
        for (const auto& [player, remote]: core.remotes()) {
            if (!remote.physicalized || !remote.birth_known || remote.born_tick == 0 || remote.entity.empty()) continue;
            auto& seen = host.mirror_births[{player, remote.born_tick}];
            bool changed = false;
            double now[2][3] = {};
            bool have[2] = {false, false};
            for (int k = 0; k < 2; ++k) {
                bmmo_physics_body_state state{};
                if (!core.recorded_body_state(remote.entity, remote.born_tick + static_cast<uint32_t>(k), state)) continue;
                have[k] = true;
                for (int c = 0; c < 3; ++c) now[k][c] = state.position[c];
                if (!seen.have[k] || std::memcmp(seen.at[k], now[k], sizeof(now[k])) != 0) changed = true;
            }
            if (!changed || seen.lines > kLinesPerBirth) continue;
            if (seen.lines++ == kLinesPerBirth) {
                // The last line logged is no longer what the history keeps:
                // said once, so a reader can set this birth aside.
                logger->Info("Physics session: mirror birth record player=%u entity=%s stamp=%u at local %u: "
                             "cap reached, not logged further",
                             player, remote.entity.c_str(), remote.born_tick, core.current_tick());
                continue;
            }
            for (int k = 0; k < 2; ++k) {
                seen.have[k] = have[k];
                std::memcpy(seen.at[k], now[k], sizeof(now[k]));
            }
            logger->Info("Physics session: mirror birth record player=%u entity=%s stamp=%u at local %u: "
                         "t0=%s%.6f,%.6f,%.6f t1=%s%.6f,%.6f,%.6f",
                         player, remote.entity.c_str(), remote.born_tick, core.current_tick(),
                         have[0] ? "" : "none ", now[0][0], now[0][1], now[0][2],
                         have[1] ? "" : "none ", now[1][0], now[1][1], now[1][2]);
        }
    }

    // The ball table's "Force" column per ball type (the navigation's force;
    // the legacy anchor's ball_forces).
    void read_ball_forces(IBML* bml) {
        host.ball_forces.clear();
        if (CKDataArray* balls = bml->GetArrayByName("Physicalize_GameBall")) {
            const int rows = balls->GetRowCount();
            for (int row = 0; row < rows; ++row) {
                float force = 0.0f;
                balls->GetElementValue(row, 7, &force);
                host.ball_forces.push_back(force);
            }
        }
    }

    // The behavior the PreSimulate clock guard hangs on (the legacy
    // clock_guard_behavior_id): the navigation once known, else the level
    // script; and its pause sensor, the level script.
    uint32_t clock_guard_behavior(IBML* bml) {
        if (host.navigation_known && host.navigation.ball_navigation) return host.navigation.ball_navigation;
        CKBehavior* script = bml->GetScriptByName("Gameplay_Ingame");
        return script ? script->GetID() : 0;
    }
    uint32_t pause_sensor_behavior(IBML* bml) {
        CKBehavior* script = bml->GetScriptByName("Gameplay_Ingame");
        return script ? script->GetID() : 0;
    }

    // Step Q: the level's TT Scaleable Proximity countdowns as the level file
    // has them, recorded beside the Sequencer counters after the map load and
    // written back at both paths' anchors (game/script_state.hpp).  Here, not
    // beside level_sequencers_, because BallanceMMOClient takes no new member.
    bmmo::game::proximity_countdown_state level_proximity_state;

    // ------------------------------------------------------------ the level's lifetime
    // Step SP9 (sector-placement design 2.5): every retail anchor is on a level
    // loaded after the last session on it.  The server builds a fresh level for
    // every session; the retail anchor is an in-level restart, and the vanilla
    // restart deactivates the current sector only, so a level that ran a
    // session (the sector union keeps every sector any member entered running,
    // nothing deactivated) would anchor on a world the server does not have
    // ("world mismatch", measured at SP9 (1)).  So a countdown on such a level
    // reloads it - the pause menu's "Exit Level", then the menus' level start
    // (game/menu_driver.hpp, the automation's 'level N') - and "Go!" restarts
    // once the new load is in play and past the Sequencer capture window
    // (capture_level_sequencers), which makes the anchor exactly the measured
    // path: a fresh load, then the restart.  A clean level restarts as before.
    // File-scope (BallanceMMOClient takes no member); outlives sessions.
    struct level_lifetime {
        // A session anchored on this level since its load (design 2.5's
        // `level_ran_union`): set at a core anchor and while a legacy session
        // runs, cleared by a map load (OnLoadObject(isMap)).
        bool ran_union = false;
        // 'session reload on|off' (a testing tool; on by default): off makes
        // the countdown restart a dirty level as before SP9, which reproduces
        // the gap (SP9 (1)).
        bool enabled = true;
        // 'session reload delay <ms>' (a testing tool; 0 by default): the
        // reloaded level counts as ready only this long after its map load,
        // which makes the load slower than the countdown ("Go!" waits).
        long delay_ms = 0;
        // The reload of the current countdown: the exit sent and the level
        // requested through the menus, until the new load is ready.
        bool reloading = false;
        bool loaded = false;       // OnLoadObject(isMap) seen since the exit
        bool go_waiting = false;   // "Go!" came first: the restart waits for the load
        bool exit_seen = false;    // OnPostExitLevel of the reload's exit
        int level = 0;
        clock_type::time_point started{}, loaded_at{};
        uint64_t reloads = 0, clean_restarts = 0, abandoned = 0;
    };
    level_lifetime level_life;

    // The reload's own deadline: the exit, the menus, the load, the capture
    // window (30 frames) and "Go!".  The server's start barrier (8 s from its
    // SessionStart, session_runner.hpp) is the effective limit: past it the
    // server ends the session for this member; this give-up is the barrier
    // plus a margin, so the host's log names its own cause when the server's
    // SessionEnd does not come.
    constexpr std::chrono::seconds kReloadTimeout{10};

    // ------------------------------------------------------------ the sector placement
    // Step SP10 (sector-placement design 2.3-2.5): this process's placement -
    // the registry of the copies its anchors made (game/sector_placement.hpp),
    // which outlives the session (the PH rows name the copies until the level
    // goes) and is released at the level's exit (OnPostExitLevel) and at the
    // next map load (OnLoadObject(isMap)), whichever comes first.  The host
    // logic is the shared helper's (place_at_anchor, release_placement); this
    // host supplies the bridge, the logger and the registry.  File-scope
    // (BallanceMMOClient takes no member).
    struct placement_host_state {
        bmmo::game::sector_placement placement;
        bool active = false;          // the last anchor's: the core's placement_active()
        uint64_t signature = 0;       // the last anchor's signature (SessionReady's anchor_placement)
        size_t made = 0;              // copies the last anchor made
        uint64_t anchors = 0, refused = 0, releases = 0, released = 0;
    };
    placement_host_state placement_host;

    // ------------------------------------------------- the proximity union
    // Step WG4 (world-agreement-gaps design 2.3, 2.4 row RC, G4): under
    // WORLD_PROXIMITY_UNION the mechanism proximity blocks and their identity
    // reads are rewired at the anchor (right after the selection, after the
    // placement's apply), the core places them before every live and
    // replayed tick (platform place_probes), the hold travels with the
    // script records (v6) and the exact resync's portable record, and the
    // signature goes into SessionReady.  The logic is the shared
    // game/proximity_union.hpp, as the headless host has it; this host
    // supplies the bridge's body test and the logger.  Released at the
    // session's end (core_host_end_local) and, should that have been missed,
    // at the level's exit and the next map load, before the placement's
    // copies (it may have rewired a copy's blocks).  File-scope
    // (BallanceMMOClient takes no member).
    struct proximity_host_state {
        bmmo::game::proximity_union union_;
        bool active = false;          // the last anchor's: the core's proximity_union_active() and a rewire made
        uint64_t signature = 0;       // the last anchor's (SessionReady's anchor_probes)
        uint64_t anchors = 0, refused = 0, releases = 0, restored = 0;
    };
    proximity_host_state proximity_host;

    // FULL step WFA (world-agreement-gaps-design.md §4.3, "As built (step
    // WFA)"): every force block outside the units and the trafo sets by its
    // world-independent name ("force:<owner>/<n>"), built at the anchor after
    // the union's copy step, cleared at the session's end.  This process
    // allocates a level's CK_IDs later than the server's world (RT1: +1062 on
    // Level 11), so the controllers' own names ("rt:force/<block id>") never
    // matched a resync's blob.  File-scope (BallanceMMOClient takes no member).
    bmmo::physics::force_alias::table force_alias_host;

    // Step RK2 (record-format-gaps design 2.2): the last anchor's array-read
    // census (game/array_reads.hpp anchor_log), for the status line.
    // File-scope (BallanceMMOClient takes no member).
    bmmo::game::array_reads::anchor_summary array_reads_host;
    // ... and the script binding's written-cell columns (the ones the
    // records carry; the status line's written_columns).
    std::vector<bmmo::game::written_column> written_columns_host;

    int32_t controller_alias_cb(void* user, const char* name, char* out, uint32_t out_size);

    // The anchor's rewire (the headless rewire_proximity): the mechanism
    // class over the session's selection, the level's union options; false
    // with `error` when refused (nothing is rewired then).
    bool proximity_rewire(CKContext* context, const bmmo::physics::physics_view& view,
                          const bmmo::game::script_replay::selection& sel, uint8_t slots, logger_wrapper* logger,
                          std::string& error) {
        const bmmo::physics::physics_view* v = &view;
        proximity_host.union_.has_body = [v](CK3dEntity* entity) {
            return entity && entity->GetName() && v->has_body(entity->GetName());
        };
        ++proximity_host.anchors;
        auto options = bmmo::game::proximity_union::level_options(context);
        // Step Q7 (world-agreement-gaps-design.md 2.10): the per-player units
        // copied K times under WORLD_PLAYER_MECHANISMS (the core's K).
        options.slots = slots;
        error = proximity_host.union_.rewire(context, bmmo::game::probe_class::mechanism, sel, options);
        for (const std::string& line: proximity_host.union_.lines()) logger->Info("Physics session: %s", line.c_str());
        if (!error.empty()) {
            ++proximity_host.refused;
            proximity_host.active = false;
            proximity_host.signature = 0;
            error = "the proximity union (world option) refused: " + error;
            return false;
        }
        proximity_host.active = true;
        proximity_host.signature = proximity_host.union_.signature(bmmo::game::probe_class::mechanism);
        // §4.3: the units' force blocks by their world-independent names (the
        // one alias function of the manager, controller_alias_cb).
        if (proximity_host.union_.slots() && !view.set_controller_alias(&controller_alias_cb, nullptr, error)) {
            logger->Warn("Physics session: player mechanisms: the controller alias: %s", error.c_str());
            error.clear();
        }
        logger->Info("Physics session: proximity union: %zu mechanism blocks, %zu identity reads rewired, signature %016llx",
                     proximity_host.union_.probe_count(), proximity_host.union_.identity_count(),
                     static_cast<unsigned long long>(proximity_host.signature));
        return true;
    }

    // The union's sources back and its objects gone (the headless
    // release_proximity); nothing when nothing is rewired.
    void proximity_release(CKContext* context, logger_wrapper* logger, const char* why) {
        proximity_host.active = false;
        if (!proximity_host.union_.probe_count() && !proximity_host.union_.identity_count() && !proximity_host.union_.no_ball()
            && !proximity_host.union_.unit_count())
            return;
        const size_t copies = proximity_host.union_.copy_count();
        const int restored = proximity_host.union_.release(context);
        ++proximity_host.releases;
        proximity_host.restored += static_cast<uint64_t>(restored);
        logger->Info("Physics session: proximity union released (%s): %d inputs back, %zu unit copies destroyed", why, restored,
                     copies);
    }

    double seconds_since(clock_type::time_point t) {
        return std::chrono::duration<double>(clock_type::now() - t).count();
    }

    // ------------------------------------------------- the trafo (step T4)
    // Trafo-replication design 2.3-2.5, 2.11, 2.14: the retail platform's half
    // of the trafo as an input, over game/trafo_sets.hpp and the bridge
    // through physics_view (bridge API v21, the controller alias v22), as the
    // headless host has it over sim/trafo_hooks.hpp.  One file-scope static
    // (design 2.11 "retail state"; BallanceMMOClient takes no member).
    struct trafo_host_state {
        // This world's piece sets (the trafo list with them), the bridge's
        // side and the hold of the originals; `on` from the anchor of a
        // session whose core runs the trafo as an input to its end.
        bmmo::game::trafo_registry registry;
        bmmo::game::trafo_set_hooks hooks;
        bmmo::game::trafo_hold hold;
        bool on = false;
        // The level reference (design 2.4 step 6, T1's "As built"): the 8
        // original scripts' values as Balls.nmo has them, captured when its
        // scripts first exist (OnLoadObject), before any original explosion
        // of the process can have changed them; the registry takes a copy at
        // every trafo anchor.
        bmmo::game::trafo_reference loaded_reference;
        std::string loaded_where;
        // OnPhysicalize's birth tick while a replayed tick or a reconcile's
        // remake runs (0: the live frame's, current + 1): the headless host
        // logs a replayed tick's births under that tick, from the event log
        // it mutes around the replay (session_client.cpp), and a remake's at
        // the resync's tick.  `if_absent`: only a birth the log does not have
        // at that tick (a replay that agrees with the live frames adds
        // nothing).
        uint32_t birth_tick = 0;
        bool if_absent = false;
        uint32_t replay_birth_logs = 0;
    };
    trafo_host_state trafo_host;

    // The savestate's controller alias over the registry (bridge v22, the
    // headless hosts' set_controller_alias lambda): a set's force controller
    // "rt:force/<block id>" is "trafo:<player>/<k>" in every world.
    // Step Q7 (world agreement §4.3): the one alias function of the manager
    // - the trafo sets' names while the sets run, then the per-player units'
    // force blocks ("unit:<owner>/<n>/S<k>") while the union holds them,
    // else the own name.  Both registries are this file's statics (`user`
    // is unused).
    int32_t controller_alias_cb(void* user, const char* name, char* out, uint32_t out_size) {
        (void)user;
        std::string alias;
        const bool found = (trafo_host.on && bmmo::game::trafo_controller_alias(trafo_host.registry, name, alias))
            || proximity_host.union_.controller_alias(name, alias) || force_alias_host.alias(name, alias);
        if (!found || !out || alias.size() + 1 > out_size) return 0;
        std::memcpy(out, alias.c_str(), alias.size() + 1);
        return 1;
    }

    // The bridge as the retail host reaches it (the headless make_trafo_hooks
    // with physics_view).  restore_originals is left empty: the anchor runs
    // the quiesce right before the session clock reset, whose
    // restore_explosion_pieces is that restore (physics_state.cpp
    // reset_session_clock), and nothing else calls it.
    bmmo::game::trafo_set_hooks make_retail_trafo_hooks(const bmmo::physics::physics_view& view, logger_wrapper* logger) {
        bmmo::game::trafo_set_hooks hooks;
        const bmmo::physics::physics_view* v = &view;
        hooks.has_body = [v](const std::string& name) { return v->has_body(name.c_str()); };
        hooks.unphysicalize = [v](const std::string& name, std::string& error) { return v->unphysicalize(name.c_str(), error); };
        hooks.register_set = [v](uint32_t player, const uint32_t frames[3], const std::vector<uint32_t>& explosions,
                                 bmmo_physics_trafo_set_report& out, std::string& error) {
            return v->trafo_set_register(player, frames, explosions.data(), static_cast<int>(explosions.size()), out, error);
        };
        hooks.unregister_set = [v](uint32_t player, std::string& error) { return v->trafo_set_unregister(player, error); };
        hooks.seed = [v](uint32_t frame, int32_t seed) { v->trafo_set_seed(frame, seed); };
        hooks.unmake = [v](uint32_t frame, std::string& error) { return v->trafo_set_unmake(frame, error); };
        hooks.forces = bmmo::game::force_hooks_for(v);   // game/host_backend.hpp (step SP10)
        hooks.log = [logger](const std::string& line) { logger->Info("Physics session: trafo: %s", line.c_str()); };
        return hooks;
    }

    // The anchor's trafo work (design 2.3 "When the hold is armed"; the
    // headless anchor_trafo), with a core that runs the trafo as an input,
    // before the session clock reset: the hooks and the controller alias,
    // the level reference into the registry, the originals quiesced (in a
    // retail world a single-player trafo before the session leaves bodies,
    // a wind or a pending reset: counted, not a bug here), the hold armed and
    // the piece x local-body filter on.
    void trafo_anchor(const bmmo::physics::physics_view& view, CKContext* context, logger_wrapper* logger,
                      const bs::session_client_core& core) {
        if (!core.trafo_active() || trafo_host.on) return;
        trafo_host.hooks = make_retail_trafo_hooks(view, logger);
        std::string error;
        if (!view.set_controller_alias(&controller_alias_cb, nullptr, error))
            logger->Warn("Physics session: trafo: the controller alias: %s", error.c_str());
        if (trafo_host.loaded_reference.empty()) {
            // Balls.nmo's scripts were not seen loading (or were running
            // then): captured now, which an original explosion of this
            // process may have changed already (logged).
            bmmo::game::trafo_reference reference;
            if (bmmo::game::capture_trafo_reference(context, reference, error)) {
                trafo_host.loaded_reference = std::move(reference);
                trafo_host.loaded_where = "the anchor (fallback: none at the Balls.nmo load)";
            } else {
                logger->Warn("Physics session: trafo: no level reference of the trafo scripts at the anchor either: %s - no "
                             "set can be built", error.c_str());
            }
        }
        trafo_host.registry.set_reference(trafo_host.loaded_reference);
        if (!trafo_host.loaded_reference.empty()) {
            size_t blocks = 0;
            for (const auto& [name, b]: trafo_host.loaded_reference.scripts) blocks += b.size();
            logger->Info("Physics session: trafo: level reference from %s: %zu scripts, %zu blocks",
                         trafo_host.loaded_where.c_str(), trafo_host.loaded_reference.scripts.size(), blocks);
        }
        const auto quiesce = bmmo::game::quiesce_originals(context, trafo_host.hooks);
        logger->Info("Physics session: %s (the originals' restore follows in the session clock reset)", quiesce.text().c_str());
        trafo_host.hold.arm(context, 1000.0f / 66.0f);
        if (!view.trafo_set_filter(true, error)) logger->Warn("Physics session: trafo: the piece filter rule: %s", error.c_str());
        trafo_host.on = true;
        logger->Info("Physics session: trafo: inputs on: the originals held (option E %s, the Delayer %u frames), the piece x "
                     "local-body filter on", core.sector_lead_ticks() <= bs::kTrafoOptionEMaxLead ? "on" : "off (lead > 66)",
                     trafo_host.hold.delayer_frames());
    }

    // The session's end (design 2.4; the headless end_trafo_sets): every live
    // piece unphysicalized, the winds' controllers deleted, the sets
    // unregistered and destroyed, the alias and the filter rule off, the hold
    // disarmed.  Idempotent.
    void trafo_teardown(const bmmo::physics::physics_view& view, CKContext* context, logger_wrapper* logger) {
        trafo_host.replay_birth_logs = 0;   // each session's own log budget (a second session in one process, R3)
        if (!trafo_host.on) return;
        const int bodies = bmmo::game::teardown_trafo_sets(context, trafo_host.registry, trafo_host.hooks);
        std::string error;
        // Step Q7: the units' part of the alias stays while the union holds
        // them; step WFA: the force blocks' table while the session runs.
        if (proximity_host.union_.slots() || force_alias_host.built()) view.set_controller_alias(&controller_alias_cb, nullptr, error);
        else view.set_controller_alias(nullptr, nullptr, error);
        view.trafo_set_filter(false, error);
        trafo_host.hold.disarm();
        trafo_host.registry.clear();
        trafo_host.on = false;
        trafo_host.birth_tick = 0;
        trafo_host.if_absent = false;
        logger->Info("Physics session: trafo: the session's sets torn down (%d piece bodies removed)", bodies);
    }

    // ------------------------------------------- diagnostics (FULL step RT1)
    // BMMO_JOURNAL_FINAL_CHECKPOINTS=1: the core also journals each checkpoint
    // tick's FINAL rows (core_config::journal_final_checkpoints, step D0, the
    // headless --final-checkpoints), so a retail journal is judged on the
    // post-rewind state as the headless batches are.  Off by default.
    bool diag_final_checkpoints() {
        const char* v = std::getenv("BMMO_JOURNAL_FINAL_CHECKPOINTS");
        return v && *v && std::strcmp(v, "0") != 0;
    }
    // The frame times of a core session (RCX-x86), in the headless client's
    // frame_time line format but NOT its quantity: 'core_frame_time' =
    // core_host_frame's own duration, i.e. post_step(T) + pre_step(T + 1)
    // (rollbacks and resims included) WITHOUT the tick's live IVP step, which
    // CKContext::Process runs outside it - the headless frame() times
    // pre_step + step + post_step, so the two are not comparable; the only
    // whole-frame figure is 'frame_interval' = OnProcess to OnProcess while
    // the session runs (the game's frame period: render, live step, host).
    // 0.1 ms buckets up to 1 s; reset when a session starts
    // (core_host_session_start) and logged when the host's session ends.
    // Measures only; decides nothing.
    struct frame_time_stats {
        static constexpr size_t kBuckets = 10000;
        std::vector<uint32_t> hist = std::vector<uint32_t>(kBuckets, 0);
        uint64_t count = 0;
        double max_ms = 0.0;
        void reset() {
            std::fill(hist.begin(), hist.end(), 0u);
            count = 0;
            max_ms = 0.0;
        }
        void note(double ms) {
            const size_t bucket = std::min(kBuckets - 1, static_cast<size_t>(std::max(0.0, ms) * 10.0));
            ++hist[bucket];
            ++count;
            max_ms = std::max(max_ms, ms);
        }
        double quantile(double q) const {
            if (!count) return 0.0;
            uint64_t seen = 0;
            for (size_t b = 0; b < kBuckets; ++b) {
                seen += hist[b];
                if (static_cast<double>(seen) >= q * static_cast<double>(count)) return (b + 1) / 10.0;
            }
            return max_ms;
        }
        std::string line(const char* what) const {
            return std::format("{}: frames={} p50={:.1f} p99={:.1f} p999={:.1f} max={:.1f} ms", what, count,
                               quantile(0.5), quantile(0.99), quantile(0.999), max_ms);
        }
    };
    struct frame_time_host {
        frame_time_stats frame, interval;
        uint32_t session = 0;   // the host session the stats belong to
        bool have_last = false;
        std::chrono::steady_clock::time_point last;
    } frame_times;

    // ------------------------------------------------- diagnostics (step EG0)
    // Environment switches, all off by default; none changes what the session
    // does, only what it logs (rollback-engine-gaps-design.md, As built EG0):
    //   BMMO_TRACE_PLAN=1        the engine's plan trace (core_config::trace_plan,
    //                            its first 16 restores), every destroy_body the
    //                            binding refuses (a birth item's refusal is what
    //                            the engine counts as birth_recreate_failures;
    //                            the savestate-failure path's destroy_body(e, 0)
    //                            is uncounted, and a refused recreate_body, also
    //                            counted, is not logged here), and the rollback
    //                            log uncapped;
    //   BMMO_SCRIPT_DIFF_LOGS=N  the script replay's "differ" lines (default 8);
    //   BMMO_TRACE_ENTITY=a,b    each named 3D entity's world position and parent
    //                            after every live frame's post_step and before
    //                            every replayed tick (the state that tick's
    //                            scripts read).  Solo runs: the entity is the
    //                            first of that name, which in a session with
    //                            peers can be a remote clone's child.  The
    //                            entry @mirrors (FULL step RT3) stands for every
    //                            mirror entity this session made (their names
    //                            carry the peer's connection id, unknown before
    //                            the session): its whole world matrix, its first
    //                            material's alpha and alpha blending, shown /
    //                            in the view frustum, and whether a ghost pin
    //                            holds it - inside the frame, so the matrix is
    //                            the one the frame's place points read (the
    //                            pinned one for an airship ghost) and the alpha
    //                            the one the previous frame was drawn with.
    bool diag_trace_plan() {
        static const bool on = [] {
            const char* v = std::getenv("BMMO_TRACE_PLAN");
            return v && *v && std::strcmp(v, "0") != 0;
        }();
        return on;
    }
    uint32_t diag_script_diff_logs(uint32_t fallback) {
        const char* v = std::getenv("BMMO_SCRIPT_DIFF_LOGS");
        if (!v || !*v) return fallback;
        const long n = std::strtol(v, nullptr, 10);
        return n > 0 ? static_cast<uint32_t>(n) : fallback;
    }
    const std::vector<std::string>& diag_trace_entities() {
        static const std::vector<std::string> names = [] {
            std::vector<std::string> out;
            const char* v = std::getenv("BMMO_TRACE_ENTITY");
            std::string s = v ? v : "";
            size_t start = 0;
            while (start < s.size()) {
                size_t comma = s.find(',', start);
                if (comma == std::string::npos) comma = s.size();
                if (comma > start) out.push_back(s.substr(start, comma - start));
                start = comma + 1;
            }
            return out;
        }();
        return names;
    }
    // One line: "<when> t=<tick> name=(x,y,z)@parent ..." (hex floats, so a
    // live and a replayed line compare bit for bit).
    std::string diag_entity_line(CKContext* context, const char* when, uint32_t tick) {
        std::string out = std::format("entity trace {} t={}", when, tick);
        for (const std::string& name: diag_trace_entities()) {
            if (name == "@mirrors") {
                CKRenderContext* rc = context ? context->GetPlayerRenderContext() : nullptr;
                for (const auto& [mirror, player]: host.mirror_player) {
                    auto* entity = context ? CK3dEntity::Cast(context->GetObjectByNameAndParentClass(
                            const_cast<CKSTRING>(mirror.c_str()), CKCID_3DENTITY, nullptr)) : nullptr;
                    if (!entity) {
                        out += std::format(" {}=none", mirror);
                        continue;
                    }
                    const VxMatrix& m = entity->GetWorldMatrix();
                    float alpha = -1.0f;
                    int blend = -1;
                    if (CKMesh* mesh = entity->GetCurrentMesh(); mesh && mesh->GetMaterialCount() > 0)
                        if (CKMaterial* material = mesh->GetMaterial(0)) {
                            alpha = material->GetDiffuse().a;
                            blend = material->AlphaBlendEnabled() ? 1 : 0;
                        }
                    const auto pin = ghost_pins.find(mirror);
                    out += std::format(" {}=[{:a},{:a},{:a}][{:a},{:a},{:a}][{:a},{:a},{:a}]({:a},{:a},{:a}) a={:.4f} b={}"
                                       " v={} f={} pin={}",
                                       mirror, m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1],
                                       m[2][2], m[3][0], m[3][1], m[3][2], alpha, blend, entity->IsVisible() ? 1 : 0,
                                       rc && entity->IsInViewFrustrum(rc) ? 1 : 0,
                                       pin != ghost_pins.end() && pin->second.held ? 1 : 0);
                }
                continue;
            }
            auto* entity = context ? CK3dEntity::Cast(context->GetObjectByNameAndParentClass(
                    const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr)) : nullptr;
            if (!entity) {
                out += std::format(" {}=none", name);
                continue;
            }
            const VxMatrix& m = entity->GetWorldMatrix();
            CK3dEntity* parent = entity->GetParent();
            out += std::format(" {}=({:a},{:a},{:a})@{}", name, m[3][0], m[3][1], m[3][2],
                               parent && parent->GetName() ? parent->GetName() : "-");
        }
        return out;
    }
}

bool bmmo::session::retail::core_enabled() {
    return core_on.load();
}

// Step RK3: a core session past its anchor that switches this level's
// tutorial off (record-format-gaps-design.md 8 row K3 = C4).
bool bmmo::session::retail::core_tutorial_owned() {
    return core_session_running() && host.core->tutorial_off();
}

bmmo::game::proximity_countdown_state& bmmo::session::retail::level_proximities() {
    return level_proximity_state;
}

// Step T4 (trafo-replication design 2.4 step 6, T1's level reference):
// OnLoadObject of 3D Entities\Balls.nmo - the 8 original trafo scripts exist
// now, as the file has them, before any of them ran.  Read-only (the values
// are copied, nothing is written into the scripts), whatever the switch says;
// a session anchor with the trafo as an input hands it to its registry.
std::string bmmo::session::retail::trafo_scripts_loaded(CKContext* context) {
    bmmo::game::trafo_reference reference;
    std::string error;
    if (!bmmo::game::capture_trafo_reference(context, reference, error))
        return "trafo level reference not captured at the Balls.nmo load: " + error;
    size_t blocks = 0;
    for (const auto& [name, b]: reference.scripts) blocks += b.size();
    trafo_host.loaded_reference = std::move(reference);
    trafo_host.loaded_where = "the Balls.nmo load";
    return std::format("trafo level reference captured at the Balls.nmo load: {} scripts, {} blocks",
                       trafo_host.loaded_reference.scripts.size(), blocks);
}

// ---------------------------------------------------------------- network thread

// Design 3.2 "Receive": every Session* message of a session whose SessionStart
// found the switch on goes into the mailbox as its bytes, SessionStart first,
// so the game thread sees them in arrival order.  From R1 the core runs such
// a session, so the legacy handlers never see its messages (true: the caller
// stops there).  With the switch off and no session latched: two atomic
// loads, false.
bool BallanceMMOClient::core_host_offer(ISteamNetworkingMessage* network_msg) {
    if (!core_on.load() && !core_latched.load()) return false;
    const auto* raw = reinterpret_cast<const bmmo::general_message*>(network_msg->m_pData);
    if (raw->code == bmmo::SessionStart) {
        // The switch read once per session, here.
        const bool on = core_on.load();
        core_latched.store(on);
        if (!on) return false;
        mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        return true;
    }
    if (!core_latched.load()) return false;
    switch (raw->code) {
    case bmmo::SessionEnd:
        mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        core_latched.store(false);
        return true;
    case bmmo::SessionAssign:
    case bmmo::SessionSnapshot:
    case bmmo::SessionRemoteInput:
    case bmmo::SessionEvent:
    case bmmo::SessionLead:
    case bmmo::SessionResyncState:   // step A3 (protocol 10): the core path alone; the legacy one ignores it
    case bmmo::SessionMember:        // step M1 (protocol 18): the core path alone
        mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- game thread

bool BallanceMMOClient::core_host_running() const {
    return host.phase != retail_host::phase_type::idle;
}

// The legacy gates that read phase == running (update_current_sector's union
// cell): past the anchor, the core running the session; not during the
// countdown or the restart.
bool BallanceMMOClient::core_host_session_running() const {
    return core_session_running();
}

// The tick driver's slew (step R5): the core's while a core session runs,
// none otherwise - the legacy path's pacing is exactly what it was.
bmmo::session::pacer_slew* BallanceMMOClient::core_host_pacer() {
    return core_session_running() ? &slew_pacer : nullptr;
}

// A latched session's SessionStart, taken from the mailbox in arrival order
// (the legacy physics_session_begin, with the core's begin): a fresh core,
// its platform checked by the constructor, the login id handed over, the
// session's parameters and the core's configuration; then the not-in-level
// leave or the countdown.
void BallanceMMOClient::core_host_session_start(const bmmo::session_start_msg& msg) {
    ++host.sessions_seen;
    if (host.phase != retail_host::phase_type::idle) core_host_end_local("replaced by a new session");
    bs::session_capabilities caps;
    caps.own_body_control = false;   // the game's Physicalize block owns the own ball (design 2.3)
    // Step EL8 (rollback-engine gaps design 2.3.4 "RC capability"): make_own /
    // unmake_own on the bridge, called by the core from restore_lives and the
    // replay's drain only - an own life ending inside a window is revived and
    // removed again at its death; every body so made is gone by `now`.
    caps.own_body_in_replay = true;
    caps.slew = true;                // R5: the tick driver slews (core_host_pacer), SessionReady caps = CAP_LEAD
    caps.nonblocking_hold = false;   // the anchor hold blocks inside OnProcess
    caps.whole_frame = false;
    caps.own_lifecycle = bs::own_lifecycle_source::host_hooks;   // OnPhysicalize / OnUnphysicalize
    // Step T4: the per-player trafo piece sets (trafo-replication design
    // 2.11, 2.14): SessionReady declares CAP_TRAFO_INPUTS whatever the world
    // option (T3's rule).  The script replay it needs is bound only when the
    // anchor's script selection is not empty (w.bind_scripts); on a level with
    // the trafo the manager is always selected, and an empty selection with
    // the inputs on is logged at the anchor.
    caps.trafo_inputs = true;
    // Step SP10 (sector placement design 2.4): this host computes the
    // placement signature at every anchor and builds the copies when the
    // session's world option says so (the core's placement_active):
    // SessionReady declares CAP_PLACEMENT with the signature.
    caps.placement = true;
    // A previous session's sets, should its end have been skipped.
    trafo_teardown(physics_view_, m_bml->GetCKContext(), logger_);
    try {
        host.core.reset();
        host.core = std::make_unique<bs::session_client_core>(core_host_platform(), caps);
        host.core_error.clear();
    } catch (const std::exception& e) {
        host.core_error = e.what();
        logger_->Error("Physics session core: not made: %s", e.what());
        // The session's messages are dropped from here on: say so in game
        // too, as the legacy path's refusals do.
        SendIngameMessage(std::format("Physics session unavailable: the session core could not be made ({}).", e.what()),
                          bmmo::ansi::BrightRed);
        return;
    }
    auto& core = *host.core;
    core.set_own_id(db_.get_client_id());
    // The core's configuration (design 3.2 "core_config"): every field set
    // here.  The engine runs as the legacy path's does - its defaults, the
    // exact restore (savestate) on with the binding's lenient flags 0, no
    // clock rewind (the clock guard replaces it); the feature switches of
    // design 6.2 R3: snapshot rollback, the input rewind (a relayed frame
    // that differs from the prediction replays from its tick), the
    // lifecycle rewind (a late remote birth is made at its stamp inside a
    // replay, a late death rewinds) and the own echoes acted on with the
    // design 2.3 fallback this host's capabilities select (no own body
    // control: a moved own birth is the snapshots' or a resync, a rejected
    // or missing one re-reported while current, a superseded one nothing);
    // the own corrections (R4, late-player part one: a held notice of our
    // own substituted tick amends the tick's record and replays from it),
    // own_body_backstop on.  The lead (R5, late-player part two) is the
    // capability `slew`, not a switch: the core amends, forces and slews
    // a SessionLead, the tick driver moves its schedule (core_host_pacer).
    bs::core_config config;
    config.journal_checkpoint_ticks = static_cast<uint32_t>(bs::client_journal::kCheckpointTicks);
    if (const char* env = std::getenv("BMMO_JOURNAL_CHECKPOINT_TICKS")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value >= 1 && value <= static_cast<long>(bs::client_journal::kCheckpointTicks))
            config.journal_checkpoint_ticks = static_cast<uint32_t>(value);
    }
    config.journal_final_checkpoints = diag_final_checkpoints();   // FULL step RT1 diagnostics, off by default
    config.level = msg.map.level > 0 ? msg.map.level : 1;
    config.verbose = physics_session_.trace;   // 'session trace on|off' (not reset per session)
    config.trace = physics_session_.trace;
    config.restore_recheck = true;
    config.two_pass_recheck = false;
    config.restore_impulses = true;
    config.restore_quaternions = true;
    config.restore_clock = false;
    config.restore_calm = true;
    config.restore_unit = true;
    config.restore_contacts = true;
    config.contact_flags = BMMO_PHYSICS_CONTACT_DEFAULT;
    config.sleep_as_recorded = true;
    config.script_replay = true;
    config.savestate = true;
    config.script_diff_logs = diag_script_diff_logs(8);   // BMMO_SCRIPT_DIFF_LOGS (step EG0 diagnostics)
    config.trace_plan = diag_trace_plan();                  // BMMO_TRACE_PLAN (step EG0 diagnostics), off by default
    config.rollback = physics_session_.rollback_enabled;   // 'session rollback on|off'
    config.correct = true;
    config.input_rewind = true;   // R2
    config.lifecycle_rewind = true;   // R3
    config.own_echoes = true;   // R3
    config.own_corrections = true;   // R4
    config.own_body_backstop = true;
    // RC3 (step B5RX, the user: work as if snapshots did not exist): with
    // the session's snapshots off, a body-set change's full snapshot is no
    // correction, only names and rows.
    config.body_set_corrections = false;
    // R1's policy (the legacy frame's order) only stood in for the input
    // rewind: with it a key edge relayed between the frames replays from its
    // tick, so the drive of T + 1 is chosen at post_step(T) as on the
    // headless client (As built, step R2).
    config.drive_from_newest_relay = !config.input_rewind;
    config.window_shift = 0;
    if (core.begin(msg, config).refused) {
        SendIngameMessage(std::format("Physics session refused: the server runs physics protocol {}, this mod runs {}. "
                                      "Update the mod or the server.", msg.protocol,
                                      bmmo::session::PHYSICS_PROTOCOL_VERSION), bmmo::ansi::BrightRed);
        return;
    }
    reset_session_state();
    host.session = msg.session;
    // FULL step RT1 (RCX-x86): a new session's frame times start empty even
    // when the server reuses the id (a restarted server's session 1), and its
    // first interval does not span the idle time between sessions.
    frame_times.frame.reset();
    frame_times.interval.reset();
    frame_times.session = host.session;
    frame_times.have_last = false;
    std::string error;
    if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error)) {
        SendIngameMessage("Physics session unavailable: " + error, bmmo::ansi::BrightRed);
        return;   // the core stays idle: the session's messages are dropped
    }
    if (!m_bml->IsIngame()) {
        SendIngameMessage("Physics session: you must be in the level to take part.", bmmo::ansi::BrightRed);
        // The server's start barrier waits for this member's SessionReady,
        // which will never come: leaving the room re-runs it without us.
        bmmo::room_request_msg leave;
        leave.action = bmmo::room::action::Leave;
        leave.room = msg.room;
        leave.serialize();
        send(leave.raw.str().data(), leave.size(), k_nSteamNetworkingSend_Reliable);
        logger_->Info("Physics session %u: not in the level, left room %u", msg.session, msg.room);
        return;   // the core stays idle: the session's messages are dropped
    }
    core.level_requested();
    host.phase = retail_host::phase_type::counting_down;
    host.saw_ingame_inactive = false;
    host.restart_deadline = clock_type::now() + std::chrono::seconds(PHYSICS_SESSION_COUNTDOWN + 5);
    SendIngameMessage(std::format("Physics session {} starting: the level restarts on \"Go!\".", msg.session),
                      bmmo::ansi::BrightGreen);
    logger_->Info("Physics session %u (core): countdown started (players=%zu, join order %d, own id %u)", msg.session,
                  msg.players.size(), core.own_join_order(), db_.get_client_id());
    // Step SP9 (design 2.5): a level that ran a session since its load is
    // reloaded at the start of the countdown, overlapped with it; a clean one
    // restarts on "Go!" as before.
    level_life.reloading = level_life.loaded = level_life.go_waiting = level_life.exit_seen = false;
    if (level_life.ran_union && level_life.enabled) {
        // The session's level (what the server simulates); only an original
        // level slot can be started through the menus.
        const int level = msg.map.level;
        if (level < 1 || level > 13) {
            core_host_end_local(std::format("the level ran a session and cannot be reloaded (session map level {} is "
                                            "not an original level slot)", level));
            return;
        }
        CKContext* context = m_bml->GetCKContext();
        auto* level_object = static_cast<CKBeObject*>(context->GetObjectByNameAndParentClass(
            const_cast<CKSTRING>("Level"), CKCID_BEOBJECT, nullptr));
        auto* all_balls = static_cast<CKBeObject*>(context->GetObjectByNameAndParentClass(
            const_cast<CKSTRING>("All_Balls"), CKCID_BEOBJECT, nullptr));
        if (!level_object || !all_balls) {
            core_host_end_local("the level ran a session and cannot be reloaded (no Level / All_Balls object)");
            return;
        }
        // The pause menu's "Exit Level" (Menu_Pause: Exit Level -> Send Message
        // "Exit Level" to Level and to All_Balls), without its Yes/No; then the
        // level started through the menus as a player does.  A "Load Level"
        // message alone (the automation's 'level N direct') does not reload
        // from inside a level: measured at SP9, Event_handler fires
        // PreLoadLevel and the level goes on.
        auto* messages = m_bml->GetMessageManager();
        const CKMessageType exit_level = messages->AddMessageType(const_cast<CKSTRING>("Exit Level"));
        messages->SendMessageSingle(exit_level, level_object);
        messages->SendMessageSingle(exit_level, all_balls);
        level_request_.begin(level);
        level_life.reloading = true;
        level_life.level = level;
        level_life.started = clock_type::now();
        ++level_life.reloads;
        host.restart_deadline = level_life.started + kReloadTimeout;
        logger_->Info("Physics session %u: the level ran a union: reloading level %d (exit, then the menus) during "
                      "the countdown", msg.session, level);
    } else {
        if (level_life.ran_union)
            logger_->Info("Physics session %u: the level ran a union, but 'session reload off': restarting it as it is",
                          msg.session);
        else {
            ++level_life.clean_restarts;
            logger_->Info("Physics session %u: a clean level (no session since its load): the restart needs no load",
                          msg.session);
        }
    }
    core_host_countdown();
}

// Step SP9: the reload's level is ready for the restart - the map load seen
// since the exit, the Sequencer / proximity capture window after it passed
// (capture_level_sequencers), the menus' request done and the level in play.
bool BallanceMMOClient::core_host_reload_ready() {
    // No pause test: the restart opens the pause menu itself and runs from a
    // paused level as the clean path's "Go!" does (review of SP9).
    return level_life.loaded && level_sequencers_frames_ <= 0 && !level_request_.pending() && m_bml->IsIngame()
        && gameplay_ingame_script_active()
        && clock_type::now() >= level_life.loaded_at + std::chrono::milliseconds(level_life.delay_ms);
}

// Step SP9: OnLoadObject(isMap) - a fresh level: no session ran on it.
void BallanceMMOClient::core_host_map_loaded() {
    // Step SP10: copies a missed exit left are recorded leftovers of the
    // previous level: released before any anchor of this one.
    // Step WG4: a proximity union still recorded here (the session's end and
    // the level's exit both missed) names objects of the previous level by
    // CK_ID, which the load may have given to new objects: its record is
    // dropped without touching the scene.
    if (proximity_host.union_.probe_count() || proximity_host.union_.identity_count() || proximity_host.union_.no_ball()) {
        logger_->Warn("Physics session: the proximity union of the previous level was not released (%zu blocks, %zu "
                      "identity reads); its record is dropped at the map load", proximity_host.union_.probe_count(),
                      proximity_host.union_.identity_count());
        proximity_host.union_ = bmmo::game::proximity_union{};
        proximity_host.active = false;
    }
    core_host_release_placement("a map load");
    level_life.ran_union = false;
    if (!level_life.reloading) return;
    level_life.loaded = true;
    level_life.loaded_at = clock_type::now();
    logger_->Info("Physics session %u: the reload's map load %.3f s after the exit", host.session,
                  seconds_since(level_life.started));
}

// Step SP10 (sector placement design 2.5 "Level exit", "Orphans"): the copies
// this process's anchors made go with the level - at its exit
// (OnPostExitLevel, after Event_handler's Exit Level dephysicalized the
// scene; the reload's own exit too) and, should that have been missed, at the
// next map load.  Each recorded row back on its source, each recorded copy
// that still exists under its recorded name destroyed root first; one that
// is gone already is logged as such (which settles design 9 Q2 for the
// copies).  The shared helper (release_placement) does it; nothing when the
// registry is empty.
void BallanceMMOClient::core_host_release_placement(const char* why) {
    // Step WG4: the proximity union first (it may have rewired a copy's
    // blocks; the headless order).  The session's end released it already;
    // this catches a missed one.
    proximity_release(m_bml->GetCKContext(), logger_, why);
    if (placement_host.placement.copies().empty()) return;
    ++placement_host.releases;
    placement_host.released += static_cast<uint64_t>(bmmo::game::release_placement(
        placement_host.placement, m_bml->GetCKContext(), why,
        [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); }));
}

// Step SP9: OnPostExitLevel - the reload's own exit does not end the session
// it is made for (true: the caller leaves the session alone).  Anything else
// is a level left (false: the session ends, as before).
bool BallanceMMOClient::core_host_reload_exit() {
    if (!level_life.reloading || level_life.exit_seen || host.phase != retail_host::phase_type::counting_down)
        return false;
    level_life.exit_seen = true;
    logger_->Info("Physics session %u: the reload left the level %.3f s after the exit was sent; the session goes on",
                  host.session, seconds_since(level_life.started));
    return true;
}

// "Go!": the countdown's last step, the restart (the legacy
// physics_session_countdown's), with the restart deadline counted from here.
void BallanceMMOClient::core_host_go() {
    const uint32_t session = host.session;
    SendIngameMessage(std::format("Physics session {} - Go!", session), bmmo::ansi::BrightGreen);
    play_countdown_sound(bmmo::countdown_type::Go);
    host.phase = retail_host::phase_type::restarting;
    host.saw_ingame_inactive = false;
    host.restart_deadline = clock_type::now() + std::chrono::seconds(20);
    logger_->Info("Physics session %u: restart requested", session);
    if (!fixed_tick_.enabled()) fixed_tick_.enable(m_bml);
    restart_current_level();
}

// The "3 - 2 - 1 - Go!" lead-in, then the restart (the legacy
// physics_session_countdown, on this host's phase).
void BallanceMMOClient::core_host_countdown() {
    const uint32_t session = host.session;
    const auto still_starting = [session] {
        return host.session == session && host.phase == retail_host::phase_type::counting_down;
    };
    float delay = 0.0f;
    for (int i = PHYSICS_SESSION_COUNTDOWN; i >= 1; --i) {
        m_bml->AddTimer(delay, [this, session, i, still_starting] {
            if (!still_starting()) return;
            SendIngameMessage(std::format("Physics session {} - {}", session, i), bmmo::ansi::BrightGreen);
            play_countdown_sound(bmmo::countdown_type::Countdown_1);
        });
        delay += 1000.0f;
    }
    m_bml->AddTimer(delay, [this, session, still_starting] {
        if (!still_starting()) return;
        // Step SP9: a reload still loading takes "Go!" when it is ready
        // (core_host_process).
        if (level_life.reloading) {
            level_life.go_waiting = true;
            logger_->Info("Physics session %u: \"Go!\" waits for the reload (%.3f s since the exit, map %s)", session,
                          seconds_since(level_life.started), level_life.loaded ? "loaded" : "not loaded yet");
            return;
        }
        core_host_go();
    });
}

// Every OnProcess, after the legacy session's turn: the 'net' actions armed
// for a tick, the blackouts' ends, then the core session's phase - the frame
// while it runs (which drains the mailbox between post_step and pre_step),
// otherwise the mailbox drained and the countdown's or the restart's checks
// (the legacy process_physics_session).
void BallanceMMOClient::core_host_process() {
    // Step SP9: a legacy session running on this level makes it dirty too
    // (it runs the union as well); read here, so the legacy path is unchanged.
    if (physics_session_.phase == bs::physics_session_state::phase_type::running) level_life.ran_union = true;
    if (!host.net_pending.empty()) {
        const uint32_t tick = session_tick_now(physics_session_);
        for (auto it = host.net_pending.begin(); it != host.net_pending.end();) {
            if (tick < it->at) { ++it; continue; }
            const auto action = *it;
            it = host.net_pending.erase(it);
            const std::string direction = join_flags(action.up, action.down);
            const std::string outcome = core_host_net_verb(std::format("{} {} {}", action.lag ? "lag" : "blackout",
                                                                       action.ms, direction));
            logger_->Info("Automation: net %s %ld ms (%s) armed for tick %u, at tick %u: %s", action.lag ? "lag" : "blackout",
                          action.ms, direction.c_str(), action.at, tick, outcome.c_str());
        }
    }
    if (host.blackout_up || host.blackout_down) {
        const auto now = clock_type::now();
        auto* utils = SteamNetworkingUtils();
        if (host.blackout_up && now >= host.blackout_up_until) {
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
            host.blackout_up = false;
            logger_->Info("Automation: net blackout (up) over at tick %u", session_tick_now(physics_session_));
        }
        if (host.blackout_down && now >= host.blackout_down_until) {
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0.0f);
            host.blackout_down = false;
            logger_->Info("Automation: net blackout (down) over at tick %u", session_tick_now(physics_session_));
        }
    }
    if (core_session_running()) {
        core_host_frame();
        return;
    }
    core_host_drain();
    switch (host.phase) {
    case retail_host::phase_type::counting_down:
        // Step SP9: the reload's own exit and load are no leaving; when the
        // new level is ready, "Go!" (if it came already) restarts it.
        if (level_life.reloading) {
            if (core_host_reload_ready()) {
                level_life.reloading = false;
                logger_->Info("Physics session %u: the reloaded level %d is ready %.3f s after the exit (map load "
                              "+%.3f s, capture window passed)%s", host.session, level_life.level,
                              seconds_since(level_life.started),
                              std::chrono::duration<double>(level_life.loaded_at - level_life.started).count(),
                              level_life.go_waiting ? "; \"Go!\" came first: restarting now" : "");
                if (level_life.go_waiting) {
                    level_life.go_waiting = false;
                    core_host_go();
                }
            } else if (clock_type::now() > host.restart_deadline) {
                core_host_end_local(std::format("the level reload did not finish in {} s", kReloadTimeout.count()));
            } else if (!level_request_.error.empty()) {
                core_host_end_local("the level reload failed: " + level_request_.error);
            }
            return;
        }
        // Leaving the level during the lead-in cancels the session instead
        // of restarting something else on "Go!".
        if (!m_bml->IsIngame()) core_host_end_local("the level was left during the countdown");
        else if (clock_type::now() > host.restart_deadline) core_host_end_local("the countdown did not finish");
        return;
    case retail_host::phase_type::restarting: {
        const bool active = gameplay_ingame_script_active();
        if (!active) host.saw_ingame_inactive = true;
        else if (host.saw_ingame_inactive) core_host_anchor();
        else if (clock_type::now() > host.restart_deadline) core_host_end_local("the level did not restart");
        return;
    }
    case retail_host::phase_type::running:
        // The core ended under a running host (a SessionEnd is handled in
        // the drain; this is the safety net).
        if (!host.core || host.core->session_phase() != bs::client_core_state::phase::running)
            core_host_end_local("the core session ended");
        return;
    default:
        return;
    }
}

// The mailbox, in arrival order (design 3.2): a SessionStart begins a core
// session; a SessionEnd goes to the core, and the host's session ends with
// it; every other entry goes to the core while it has a session, or is
// decoded (to check the copy), counted and dropped.
void BallanceMMOClient::core_host_drain() {
    auto entries = mailbox.take();
    // Without the input rewind (R1; R2 turns it on), a relayed frame that reaches
    // the core after a snapshot of the same batch is stored after that
    // snapshot's replay ran on the prediction, and nothing replays it again
    // but the next snapshot: within each run of entries between two
    // numbering messages (SessionStart, SessionAssign, SessionEnd keep their
    // place), the relay goes first and the snapshots last, as the legacy
    // frame took them (PSC apply_queues: the snapshots' ball rows cached,
    // the events applied, the relayed inputs, the snapshots).  The events
    // and leads sit between the two here: the core only queues an event for
    // the next pre_step's drain (on_session_event), so by the time a birth
    // reads the snapshot rows every snapshot of the batch has been taken,
    // and the event's place in the batch changes nothing.  A departure from
    // design 3.2's arrival order (As built, step R1); with the input rewind
    // a late frame asks for its own replay, and arrival order stands.
    if (host.core && !host.core->config.input_rewind && entries.size() > 1) {
        const auto rank = [](int code) {
            return code == bmmo::SessionRemoteInput ? 0
                 : code == bmmo::SessionSnapshot || code == bmmo::SessionResyncState ? 2 : 1;
        };
        const auto fence = [](int code) {
            return code == bmmo::SessionStart || code == bmmo::SessionAssign || code == bmmo::SessionEnd;
        };
        auto begin = entries.begin();
        while (begin != entries.end()) {
            auto end = begin;
            while (end != entries.end() && !fence(end->code)) ++end;
            std::stable_sort(begin, end, [&](const mailbox_entry& a, const mailbox_entry& b) { return rank(a.code) < rank(b.code); });
            begin = end == entries.end() ? end : end + 1;
        }
    }
    for (auto& entry: entries) {
        ++host.drained;
        if (entry.code == bmmo::SessionStart) {
            ++host.starts;
            bmmo::session_start_msg msg{};
            if (!decode_bytes(entry.bytes, msg)) {
                ++host.undecodable;
                // The legacy on_message's refusal, word for word: a
                // SessionStart this build cannot read comes from a server of
                // another physics protocol, and nothing will start.
                logger_->Error("Physics session: the server's SessionStart could not be decoded; it runs a different "
                               "physics protocol than this mod (%u)", bmmo::session::PHYSICS_PROTOCOL_VERSION);
                SendIngameMessage("Physics session refused: the server runs a different physics protocol than this mod. "
                                  "Update the mod or the server.", bmmo::ansi::BrightRed);
                continue;
            }
            core_host_session_start(msg);
            continue;
        }
        if (!host.core || host.core->session_phase() == bs::client_core_state::phase::idle) {
            ++host.discarded;
            if (!entry_decodes(entry.code, entry.bytes) && host.undecodable++ < 8)
                logger_->Warn("Physics session core: a %s (%zu bytes) could not be decoded", opcode_name(entry.code),
                              entry.bytes.size());
            continue;
        }
        if (entry.code == bmmo::SessionEnd) {
            bmmo::session_end_msg msg{};
            if (!decode_bytes(entry.bytes, msg)) {
                if (host.undecodable++ < 8) logger_->Warn("Physics session core: a SessionEnd could not be decoded");
                continue;
            }
            host.core->on_session_end(msg);
            if (host.core->session_phase() == bs::client_core_state::phase::idle)
                core_host_end_local("ended by the server: " + msg.reason);
            continue;
        }
        core_host_dispatch(entry.code, entry.bytes);
    }
}

// The anchor frame (the legacy physics_session_anchor): the session clock
// reset, the clock guard and the pause pin, the script edits (pause, N1-N4),
// the Sequencers, the world hash and the collision filter - the host's anchor
// work, in the legacy order - then core.anchor (SessionReady, the journal,
// the hold armed) and the blocking hold: the mailbox drained every 1 ms
// until the tick base arrives (design 2.3 nonblocking_hold = no), the
// connection lost (core.release_hold) or the 10 s timeout.  Then the first
// pre_step, for the tick base.
void BallanceMMOClient::core_host_anchor() {
    auto& core = *host.core;
    std::string error;
    {
        auto* time_manager = m_bml->GetTimeManager();
        logger_->Info("Physics session anchor timing: driver_enabled=%d last_delta=%.4f cktime=%.3f frames=%d",
                      fixed_tick_.enabled() ? 1 : 0, time_manager->GetLastDeltaTime(), time_manager->GetTime(),
                      time_manager->GetMainTickCount());
    }
    if (fixed_tick_.enabled()) fixed_tick_.disable(m_bml);
    fixed_tick_.enable(m_bml);
    // FULL step RT2: the manager's state for the first 20 frames from here.
    core_manager_trace_left = 20;
    core_manager_trace_due = false;
    // FULL step H1X: the paper hull compiled from the template body before
    // the game's own first paper physicalization (Init Ingame's "physicalize
    // new Ball" runs after this frame's anchor); this process's cache keeps
    // whatever an earlier level or single-player play built, which the call
    // forgets while no body uses it (game/hull_template.hpp).
    // A hull that does not hold the template's build (NOT built, or a cached
    // one still in use that DIFFERS) would make this world differ from the
    // server's: logged as an error and the session ended here, as a failed
    // clock reset does (H1X review).
    if (bmmo::game::build_hull_templates(physics_view_, "the anchor", [this](const std::string& line, bool failed) {
            if (failed)
                logger_->Error("Physics session: %s (frame %d)", line.c_str(), m_bml->GetTimeManager()->GetMainTickCount());
            else
                logger_->Info("Physics session: %s (frame %d)", line.c_str(), m_bml->GetTimeManager()->GetMainTickCount());
        }) < bmmo::game::kTemplatedHullCount) {
        SendIngameMessage("Physics session ended: the paper ball's collision hull could not be built from the template "
                          "(see the log).", bmmo::ansi::BrightRed);
        core_host_end_local("the hull template did not hold");
        return;
    }
    // Step T4: with the trafo as an input, the originals quiesced and held
    // (trafo_anchor), right before the session clock reset, whose
    // restore_explosion_pieces is the quiesce's last step.
    trafo_anchor(physics_view_, m_bml->GetCKContext(), logger_, core);
    if (!physics_view_.reset_session_clock(core.seed(), error)) {
        core_host_end_local("session clock reset failed: " + error);
        return;
    }
    const uint32_t guard_id = clock_guard_behavior(m_bml);
    const uint32_t pause_id = pause_sensor_behavior(m_bml);
    if (!guard_id)
        logger_->Warn("Physics session: no behavior to anchor the clock guard on");
    else if (!pause_id)
        logger_->Warn("Physics session: clock guard has no pause sensor (Gameplay_Ingame)");
    else if (!physics_view_.set_clock_guard(true, 0.001f, guard_id, pause_id, error))
        logger_->Warn("Physics session: clock guard: %s", error.c_str());
    if (!pause_clock_resolve())
        logger_->Warn("Physics session: the pause chain's time factor block was not found");
    else {
        float factor = 0.0f, delta = 0.0f;
        if (physics_view_.get_clock(factor, delta, error)) pause_clock_apply(factor);
    }
    pause_scripts_restore();
    pause_scripts_resolve();
    death_reset_restore();
    sector_deactivate_restore();
    sector_activate_restore();
    checkpoint_phase_restore();
    const bool death_neutralized = death_reset_resolve();
    const bool sector_neutralized = sector_deactivate_resolve();
    if (death_neutralized && sector_neutralized)
        logger_->Info("Physics session: retail world resets neutralized: %d sector reset(s) in Gameplay_Ingame no "
                      "longer run Gameplay_SectorManager (a death and a level reset), and %d chain(s) activate a "
                      "sector without deactivating anything (a checkpoint and a level reset)",
                      death_reset_count_, sector_deactivate_count_);
    sector_activate_resolve();
    checkpoint_phase_resolve();
    // Step B2SX: the held chain's [0][1] write to a private cell, which the
    // core's own sector poll reads (platform poll_sector); the manager reads
    // only the union's passes.  Only where the hold is in force.
    core_chain_cell.restore(m_bml->GetCKContext());
    if (sector_activate_.block && !sector_activate_failed_) {
        const std::string trouble = core_chain_cell.apply(m_bml->GetCKContext());
        if (trouble.empty())
            logger_->Info("Physics session: the checkpoint chain's sector cell is split: %s",
                          core_chain_cell.describe().c_str());
        else
            logger_->Error("Physics session: the checkpoint chain's sector cell was NOT split (%s): an own crossing in "
                           "the frame of a pass of another sector makes the manager activate the crossed one here",
                           trouble.c_str());
    }
    // Step RK3 (record-format-gaps-design.md 8 row K3 = C4, option (c)): the
    // Level 1 tutorial's exit event, found now; the core activates it before
    // frame 10 (platform tutorial_exit).
    core_tutorial_exit.clear();
    if (!core_tutorial_hold.restore(m_bml->GetCKContext()))   // step RK3H: a previous anchor's hold
        logger_->Warn("Physics session: the Level 1 tutorial's previous frame-9 hold could not be handed back; its "
                      "constant stays");
    if (core.tutorial_off()) {
        const std::string trouble = core_tutorial_exit.resolve(m_bml->GetCKContext());
        if (trouble.empty())
            logger_->Info("Physics session: the Level 1 tutorial is switched off before frame %d (world option session "
                          "clock): %s", bmmo::game::kTutorialExitFrame, core_tutorial_exit.where.c_str());
        else
            logger_->Error("Physics session: the Level 1 tutorial's exit was NOT found (%s): the tutorial freezes this "
                           "world at its own moments", trouble.c_str());
        // Step RK3H (the user, 2026-10-29: 'Hold frame-9 write'): the
        // tutorial's first write (0, frame 9) held at the in-play factor from
        // this anchor on, so this world never freezes before the exit takes
        // in frame 16.
        const std::string hold_trouble = core_tutorial_hold.apply(m_bml->GetCKContext());
        if (hold_trouble.empty())
            logger_->Info("Physics session: the Level 1 tutorial's frame-%d freeze is held: %s",
                          bmmo::session::SESSION_CLOCK_TUTORIAL_FREEZE_FRAME, core_tutorial_hold.where.c_str());
        else
            logger_->Error("Physics session: the Level 1 tutorial's frame-%d freeze was NOT held (%s): this world "
                           "freezes from that frame until the exit takes", bmmo::session::SESSION_CLOCK_TUTORIAL_FREEZE_FRAME,
                           hold_trouble.c_str());
    }
    // FULL step PRX (game/depth_test.hpp): the level's fall handler for free
    // props as a rule at one tick in every world (the body guard refused the
    // block here, so only the server's world removed a fallen prop).
    if (!core_depth_test.restore(m_bml->GetCKContext()))
        logger_->Warn("Physics session: the depth test's previous pin could not be handed back; its constant stays");
    {
        const std::string trouble = core_depth_test.apply(m_bml->GetCKContext());
        if (trouble.empty())
            logger_->Info("Physics session: the depth test runs as a rule at one tick in every world: %s",
                          core_depth_test.where().c_str());
        else
            logger_->Error("Physics session: the depth test was NOT pinned (%s): the level's block runs here, refused by "
                           "the body guard", trouble.c_str());
    }
    {
        const int restored = level_sequencers_.restore(m_bml->GetCKContext());
        logger_->Info("Physics session: %d of %zu mechanism Sequencer counter(s) restored to the level file's (%s)",
                      restored, level_sequencers_.size(), level_sequencers_.describe().c_str());
    }
    proximity_countdowns_restore();
    // Step SP10 (sector placement design 2.3, 2.4): after the Sequencer and
    // proximity restores (a copy inherits its source's level-file counters),
    // before the hash, the selection and every pass.  The core decides
    // (placement_active: the world option and the capability); the apply
    // with its hot-source hooks, the option-off release policy and the
    // signature are the shared helper's (place_at_anchor, one copy for every
    // host); a refusal ends the session here, as a failed hash does.
    {
        const bmmo::physics::physics_view* view = &physics_view_;
        placement_host.active = core.placement_active();
        ++placement_host.anchors;
        if (!bmmo::game::place_at_anchor(placement_host.placement, m_bml->GetCKContext(), view, placement_host.active, -1,
                                         [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); },
                                         placement_host.signature, error)) {
            ++placement_host.refused;
            core_host_end_local(error);
            return;
        }
        placement_host.made = placement_host.placement.copies().size();
        // Copies in the level make it a level that ran a session even when
        // a later anchor step fails (the hash below): the next countdown
        // must reload it, or its apply refuses the copies left here
        // ("already holds N placement copies") on every session until the
        // player leaves the level (SP10 review).
        if (placement_host.made > 0) level_life.ran_union = true;
    }
    bmmo::physics::world_hash hash;
    if (!physics_view_.capture(hash, error)) {
        core_host_end_local("world hash failed: " + error);
        return;
    }
    if (!physics_view_.install_player_collision_filter("P#", error))
        logger_->Warn("Physics session: player collision filter: %s", error.c_str());
    // The world stands still from here until the tick base arrives.
    fixed_tick_.set_hold(true);
    physics_view_.drain_event_log();   // install the listener, discard history
    read_ball_forces(m_bml);
    // The script replay (game/script_replay.hpp): the mechanism scripts a
    // replayed tick re-runs, and the force blocks they drive.
    // The session core's selection (step P: the trafo scripts too).  Step T4:
    // with the trafo as an input the originals never run and are not
    // selected; the sets' scripts travel beside the selection as the trafo
    // part (trafo-replication design 4.3).
    host.script_ids = bmmo::game::session_script_selection(m_bml->GetCKContext(), trafo_host.on);
    if (trafo_host.on && host.script_ids.empty())
        logger_->Warn("Physics session: trafo: inputs on but the script selection is empty - no script replay, "
                      "a replayed tick's births are logged at the live tick + 1");
    host.script_activations = bmmo::game::session_script_activations(m_bml->GetCKContext(), host.script_ids);
    host.explosion_pieces = bmmo::game::explosion_pieces(m_bml->GetCKContext());
    // The force hooks over the bridge (game/host_backend.hpp, one copy for
    // every host; step SP10 moved this host onto it).
    host.script_hooks = bmmo::game::force_hooks_for(static_cast<const bmmo::physics::physics_view*>(&physics_view_));
    // Step WG4 (world-agreement-gaps design 2.3, 2.4 row RC): under
    // WORLD_PROXIMITY_UNION the mechanism proximity blocks and their
    // identity reads rewired right after the selection (after the
    // placement's apply), as the headless anchor does; the core places them
    // before every live and replayed tick (platform place_probes) and the
    // signature goes into SessionReady.  A refused rewire ends the session
    // here, as a refused placement does.  With the bit clear nothing is
    // rewired (a previous anchor's union was released at its session's end).
    uint64_t probe_signature = 0;
    proximity_release(m_bml->GetCKContext(), logger_, "a new anchor");
    if (core.proximity_union_active()) {
        const bmmo::game::script_replay::selection sel = host.script_ids.empty()
            ? bmmo::game::session_script_selection(m_bml->GetCKContext(), trafo_host.on) : host.script_ids;
        if (!proximity_rewire(m_bml->GetCKContext(), physics_view_, sel, core.mechanism_slots(), logger_, error)) {
            core_host_end_local(error);
            return;
        }
        probe_signature = proximity_host.signature;
    }
    // FULL step WFA: the force blocks outside the units and the trafo sets
    // by their world-independent names (game/force_block_alias.hpp, the
    // headless anchor's call), installed for the whole session.
    {
        // BMMO_FORCE_ALIAS_LIST=1: one line per force block (the census
        // SimTool --probe-census --force-census prints for a headless world).
        const char* list = std::getenv("BMMO_FORCE_ALIAS_LIST");
        std::vector<std::string> lines;
        bmmo::game::build_force_alias_table(force_alias_host, m_bml->GetCKContext(), proximity_host.union_.copied_blocks(),
                                            list && *list == '1' ? &lines : nullptr);
        for (const std::string& line: lines) logger_->Info("Physics session: %s", line.c_str());
    }
    logger_->Info("Physics session: %s", force_alias_host.summary().c_str());
    if (!physics_view_.set_controller_alias(&controller_alias_cb, nullptr, error)) {
        logger_->Warn("Physics session: the controller alias: %s", error.c_str());
        error.clear();
    }
    // Step RK2 (record-format-gaps design 2.2): the array-read census after
    // the rewire and the placement, as the headless anchor runs it.
    array_reads_host = bmmo::game::array_reads::anchor_log(
        m_bml->GetCKContext(), host.script_ids, static_cast<int>(core.config.level),
        [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); });
    {
        const auto st = bmmo::game::script_replay::describe(m_bml->GetCKContext(), host.script_ids);
        logger_->Info("Physics session: script replay: %d scripts (%d active) + %d satellite blocks, %d blocks, %d parameters, "
                      "%d force blocks, %d opaque: %s", st.scripts, st.active, st.satellites, st.blocks, st.parameters,
                      st.force_blocks, st.opaque, bmmo::game::script_replay::names(m_bml->GetCKContext(), host.script_ids).c_str());
    }
    bs::anchor_report report;
    report.hash = hash;
    report.physics_sha256 = physics_view_.dll_sha256();
    report.build_id = physics_view_.build_id();
    report.own_name = get_display_nickname();
    report.placement = placement_host.signature;   // step SP10 (caps.placement)
    report.probes = probe_signature;               // step WG4 (0 with the bit clear)
    if (CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter")) parameters->GetElementValue(0, 1, &report.sector_cell);
    host.phase = retail_host::phase_type::running;
    host.last_rebases = fixed_tick_.rebases();
    // Step SP9: the level ran a session from here on (the next countdown
    // reloads it); and the anchor's body list, comparable with the server's
    // world (physics_world's "anchor bodies" line, SimTool's).
    level_life.ran_union = true;
    {
        // In pieces of at most 900 characters (the legacy line's cut), every
        // body kept: "anchor bodies [i/n]: ...".
        const std::string bodies = physics_view_.describe_physics_objects();
        constexpr size_t kPiece = 900;
        const size_t pieces = bodies.empty() ? 1 : (bodies.size() + kPiece - 1) / kPiece;
        for (size_t i = 0; i < pieces; ++i)
            logger_->Info("Physics session %u: anchor bodies [%zu/%zu]: %s", host.session, i + 1, pieces,
                          bodies.substr(i * kPiece, kPiece).c_str());
    }
    core.anchor(report);
    logger_->Info("Physics session %u: anchored (hash %016llx surfaces %016llx), waiting for the tick assignment",
                  core.session_id(), static_cast<unsigned long long>(hash.pose), static_cast<unsigned long long>(hash.surfaces));
    SendIngameMessage("Physics session: level synchronized, waiting for the server.", bmmo::ansi::BrightGreen);
    // The blocking hold (design 2.3): no behaviour frame runs, nothing is
    // recorded or sent, so the next frame is the world's first step.
    while (host.core && host.phase == retail_host::phase_type::running && host.core->holding()) {
        core_host_drain();
        if (!host.core || host.phase != retail_host::phase_type::running) return;   // ended or replaced meanwhile
        if (!host.core->holding()) break;
        if (!connected()) {
            host.core->release_hold("the connection was lost");
            break;
        }
        if (host.core->poll_hold().timed_out) {
            SendIngameMessage("Physics session: the server did not assign a tick base; starting out of phase.",
                              bmmo::ansi::BrightYellow);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!host.core || host.phase != retail_host::phase_type::running) return;
    // The driver's schedule continues where the hold interrupted it: the
    // wait is not lag, and `rebases` must not move.
    fixed_tick_.set_hold(false);
    host.last_rebases = fixed_tick_.rebases();
    if (host.core->session_phase() != bs::client_core_state::phase::running) return;
    // The tick base's frame (design 1.3 "the anchor frame"): today's union
    // tick at the base is its step 9.
    core_manager_trace_due = false;   // step RT2: only this frame's after_rewinds arms the trace line
    host.core->pre_step();
}

// One OnProcess of a running core session (design 1.3, the retail loop): the
// game has stepped T.  Host code first - the clock guard re-attached, the
// pause clock pinned, the pause edits kept and the tick driver's rebase -
// then post_step(T), the pause menu's mute for T + 1 (R2: after T's record),
// the mailbox, the transitional resync triggers and pre_step(T + 1), which
// the next CKContext::Process steps.
void BallanceMMOClient::core_host_frame() {
    // FULL step RT1 (RCX-x86): this frame's time and the period since the
    // previous one, measured only.
    const auto frame_started = std::chrono::steady_clock::now();
    if (frame_times.session != host.session) {   // a host whose session_start did not reset them
        frame_times.frame.reset();
        frame_times.interval.reset();
        frame_times.session = host.session;
        frame_times.have_last = false;
    }
    if (frame_times.have_last)
        frame_times.interval.note(std::chrono::duration<double, std::milli>(frame_started - frame_times.last).count());
    frame_times.last = frame_started;
    frame_times.have_last = true;
    struct frame_time_scope {
        std::chrono::steady_clock::time_point started;
        ~frame_time_scope() {
            frame_times.frame.note(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
    } frame_time{frame_started};
    std::string error;
    // Step BALL: the airship ghosts' CK matrices are physics_RT's for the
    // whole frame (ghost_pins above), the stream's again after it.
    ghost_pins_begin(m_bml->GetCKContext(), physics_view_, objects_);
    struct pins_end_scope {
        std::function<void()> run;
        ~pins_end_scope() { run(); }
    } pins_end{[this] { ghost_pins_end(m_bml->GetCKContext(), physics_view_, objects_); }};
    {
        auto& core = *host.core;
        // Re-attached every frame: a sector change or a level reset destroys
        // the behavior it hung on.  Idempotent while it is still there.
        const uint32_t guard_id = clock_guard_behavior(m_bml);
        const uint32_t pause_id = pause_sensor_behavior(m_bml);
        if (guard_id && pause_id) physics_view_.set_clock_guard(true, 0.001f, guard_id, pause_id, error);
        {
            float factor = 0.0f, delta = 0.0f;
            if (physics_view_.get_clock(factor, delta, error)) pause_clock_apply(factor);
        }
        pause_scripts_apply();
        // The tick driver restarted its schedule (a pause, a long stall): our
        // tick numbers no longer line up with the server's.
        if (core.assigned() && fixed_tick_.rebases() != host.last_rebases) {
            host.last_rebases = fixed_tick_.rebases();
            core.schedule_rebased("tick driver rebased");
        }
        // 'beam ... at <tick>' (a testing tool, R2 (a)): requested before the
        // post_step that stamps it (post_step(T) makes a beam stamped T + 1).
        core_host_due_beams();
        ++host.frames;
        // post_step(T) records what the replica stepped T with: the mute
        // that was in force during the step (observe_input reads
        // host.input_muted before it changes below; design 2.2, 4.2 R2).
        const bool stepped = core.post_step(true);
        if (!diag_trace_entities().empty())   // step EG0 diagnostics (BMMO_TRACE_ENTITY), off by default
            logger_->Info("Physics session: %s",
                          diag_entity_line(m_bml->GetCKContext(), "live", core.current_tick()).c_str());
        if (!stepped) ++host.refused;
        else if (host.beam_in_flight) {
            // post_step(T) consumed the request and stamped it T + 1.
            host.beam_in_flight = false;
            const uint32_t stamped = core.current_tick() + 1;
            logger_->Info("Automation: beam armed for tick %u stamped %u%s", host.beam_in_flight_armed, stamped,
                          stamped == host.beam_in_flight_armed ? "" : " (later than armed)");
        }
        // The pause menu (ESC) mutes the own input from the next step on:
        // decided now, after T's record, and before anything this frame can
        // rewind (the mailbox, pre_step's replays), whose polling resumes as
        // the mute for T + 1 says (the binding's input_muted).
        const bool muted = m_bml->IsIngame() && m_bml->IsPaused();
        if (muted != host.input_muted) {
            logger_->Info("Physics session: input %s from tick %u (ingame script %d, paused %d)", muted ? "muted" : "live",
                          core.current_tick() + 1, gameplay_ingame_script_active() ? 1 : 0, m_bml->IsPaused() ? 1 : 0);
            if (host.own_replica.active
                    && !physics_view_.navigation_poll(host.own_replica.entity.c_str(), !muted, host.own_replica.key_codes,
                                                      host.own_replica.key_blocks, host.own_replica.key_count, error))
                logger_->Warn("Physics session: own polling: %s", error.c_str());
            host.input_muted = muted;
        }
    }
    core_host_drain();
    if (!core_session_running()) return;   // the drain ended the session
    auto& core = *host.core;
    // (30 snapshots in a row with no record of their tick - a numbering
    // desync, design 7.1 decision 1 - is the core's rule since step B2SW,
    // client_core_messages.cpp apply_snapshot.)
    // (The input-starvation detector that followed here is deleted at R4:
    // every own tick the server substituted is named by a held notice,
    // amended and replayed by the core, and a notice that never arrives is
    // the core's check_held_complete's - late-player design 1.3, 1.4.)
    // Step A3 (resync design 2.6.6): the resync hold.  While a pending exact
    // resync's anchor would leave the history with the next step, nothing
    // steps: the mailbox is drained (its state applies the resync and ends
    // the hold) under the tick driver's hold, so the wait is no lag and
    // `rebases` does not move (a rebase would ask for another resync).
    if (core.poll_resync_hold().held) {
        fixed_tick_.set_hold(true);
        while (core_session_running() && host.core->resync_held()) {
            core_host_drain();
            if (!core_session_running()) break;
            if (!host.core->poll_resync_hold().held) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fixed_tick_.set_hold(false);
        host.last_rebases = fixed_tick_.rebases();
        if (!core_session_running()) return;
    }
    // Step RT2: a line armed by a frame whose union pass did not read the
    // manager must not be taken by this frame's re-derivation read (inside
    // pre_step, before its after_rewinds hook arms the line again).
    core_manager_trace_due = false;
    host.core->pre_step();
}

// The host's session ends (the legacy physics_session_end_local): the script
// edits handed back, the clock guard dropped, the core ended when it still
// runs (journal, body guard, mirrors, status), the own replica gone, the tick
// driver off.  Nothing while no core session runs.
void BallanceMMOClient::core_host_end_local(const std::string& reason) {
    if (host.phase == retail_host::phase_type::idle) return;
    // Step SP9: a reload in progress is the session's no longer (the menus'
    // load goes on; the level it makes is a fresh one either way).
    if (level_life.reloading) {
        level_life.reloading = level_life.go_waiting = false;
        ++level_life.abandoned;
    }
    std::string error;
    pause_clock_restore();
    pause_scripts_restore();
    death_reset_restore();
    sector_deactivate_restore();
    sector_activate_restore();
    checkpoint_phase_restore();
    if (!core_chain_cell.restore(m_bml->GetCKContext()))   // step B2SX
        logger_->Warn("Physics session: the checkpoint chain's sector cell could not be handed back to IngameParameter; "
                      "the session's copy is left in place");
    if (!core_tutorial_hold.restore(m_bml->GetCKContext()))   // step RK3H
        logger_->Warn("Physics session: the Level 1 tutorial's frame-9 hold could not be handed back; its constant stays");
    if (core_depth_test.pinned())   // FULL step PRX
        logger_->Info("Physics session: the depth test removed %llu prop(s) and showed %llu again this session",
                      static_cast<unsigned long long>(core_depth_test.removals()),
                      static_cast<unsigned long long>(core_depth_test.shown_again()));
    if (!core_depth_test.restore(m_bml->GetCKContext()))   // FULL step PRX
        logger_->Warn("Physics session: the depth test's Test could not be handed back; its constant stays");
    physics_view_.set_clock_guard(false, 0.001f, 0, 0, error);
    const uint32_t session = host.session;
    logger_->Info("Physics session: record sources (R2): %llu frame(s) whose NAV_ACTIVE (the leaves) differs from the BML "
                  "hook's, %llu whose mute in force differs from the pause state decided in the frame",
                  static_cast<unsigned long long>(host.nav_source_differs),
                  static_cast<unsigned long long>(host.mute_source_differs));
    // Step SP10: RO1's live script order and TR4c's 53-bit checkpoint reads
    // ran in this session (counts; nothing else is decided from them).
    logger_->Info("Physics session: live script orders (RO1): %zu kept; journal checkpoint body reads at 53-bit "
                  "precision (TR4c): %llu, %llu could not switch; placement (S10): %s, %zu copies, signature %016llx",
                  host.script_order.entries(), static_cast<unsigned long long>(report_precision_reads),
                  static_cast<unsigned long long>(report_precision_failed), placement_host.active ? "on" : "off",
                  placement_host.made, static_cast<unsigned long long>(placement_host.signature));
    {
        // FULL step RT3 (RLC-r): the joint ledger of step RLC, as the headless
        // client's 'joints:' line (this process's counts, every session so far).
        const auto& j = bmmo::game::script_replay::joint_stats();
        logger_->Info("Physics session: joints: imaged=%llu recreated=%llu replaced=%llu missing=%llu unmatched=%llu "
                      "reordered=%llu order_failed=%llu",
                      static_cast<unsigned long long>(j.imaged), static_cast<unsigned long long>(j.recreated),
                      static_cast<unsigned long long>(j.replaced), static_cast<unsigned long long>(j.missing),
                      static_cast<unsigned long long>(j.unmatched), static_cast<unsigned long long>(j.reordered),
                      static_cast<unsigned long long>(j.order_failed));
    }
    if (frame_times.session == session && frame_times.frame.count) {   // FULL step RT1 (RCX-x86)
        logger_->Info("Physics session: %s", frame_times.frame.line("core_frame_time").c_str());
        logger_->Info("Physics session: %s", frame_times.interval.line("frame_interval").c_str());
    }
    if (host.core && host.core->session_phase() != bs::client_core_state::phase::idle) host.core->end(reason);
    // Step WG4 (world-agreement-gaps design 2.3 "Release"): the level
    // outlives the session, so the union's sources go back now - vanilla play
    // afterwards runs its mechanisms on Ball_Pos_Frame again, and the next
    // anchor's structural test finds the level's sources.  After the core's
    // end (its status line reads the union's counters).
    proximity_release(m_bml->GetCKContext(), logger_, "the session ends");
    // Step T4: the sets go with the session (hooks.ended did it when the core
    // still ran; a core that ended first - a SessionEnd - did too).
    trafo_teardown(physics_view_, m_bml->GetCKContext(), logger_);
    // Step WFA: the force blocks' table and the alias go with the session
    // (the union's units went with its release above).
    if (force_alias_host.built()) {
        force_alias_host.clear();
        if (physics_view_.available() && !proximity_host.union_.slots() && !trafo_host.on)
            physics_view_.set_controller_alias(nullptr, nullptr, error);
    }
    if (host.own_replica.active && physics_view_.available())
        physics_view_.navigation_destroy(host.own_replica.entity.c_str(), error);
    if (host.body_guard && physics_view_.available()) {
        physics_view_.set_body_guard(false, nullptr, error);
        host.body_guard = false;
        host.body_guard_entity.clear();
    }
    fixed_tick_.set_hold(false);
    if (fixed_tick_.enabled()) fixed_tick_.disable(m_bml);
    reset_session_state();
    host.activate_armed = false;   // step RT2: an unfired 'session activate' ends with its session
    host.activate_script.clear();
    host.phase = retail_host::phase_type::idle;
    host.session = 0;
    SendIngameMessage(std::format("Physics session {} over ({}).", session, reason), bmmo::ansi::BrightYellow);
    logger_->Info("Physics session %u ended: %s", session, reason.c_str());
}

// One mailbox entry, decoded and handed to the core (design 1.3 "Messages").
void BallanceMMOClient::core_host_dispatch(int code, const std::string& bytes) {
    auto& core = *host.core;
    const auto decode = [&](auto& msg) {
        if (decode_bytes(bytes, msg)) return true;
        if (host.undecodable++ < 8) logger_->Warn("Physics session core: a %s could not be decoded", opcode_name(code));
        return false;
    };
    switch (code) {
    case bmmo::SessionAssign: {
        bmmo::session_assign_msg msg{};
        if (decode(msg)) core.on_session_assign(msg);
        break;
    }
    case bmmo::SessionSnapshot: {
        bmmo::session_snapshot_msg msg{};
        if (decode(msg)) core.on_session_snapshot(msg);
        break;
    }
    case bmmo::SessionRemoteInput: {
        bmmo::session_remote_input_msg msg{};
        if (decode(msg)) core.on_session_remote_input(msg);
        break;
    }
    case bmmo::SessionEvent: {
        bmmo::session_event_msg msg{};
        if (decode(msg)) core.on_session_event(std::move(msg));
        break;
    }
    case bmmo::SessionLead: {
        bmmo::session_lead_msg msg{};
        if (decode(msg)) core.on_session_lead(msg);
        break;
    }
    case bmmo::SessionResyncState: {
        bmmo::session_resync_state_msg msg{};
        if (decode(msg)) core.on_session_resync_state(std::move(msg));
        break;
    }
    case bmmo::SessionMember: {   // step M1 (world-agreement-gaps-design.md 2.8)
        bmmo::session_member_msg msg{};
        if (decode(msg)) {
            core.on_session_member(msg);
            // A life re-announced with another join order ends the core's
            // session locally (design 2.8): the host's ends with it.
            if (core.session_phase() == bs::client_core_state::phase::idle && host.phase != retail_host::phase_type::idle)
                core_host_end_local("a member's life was re-announced with another join order");
        }
        break;
    }
    case bmmo::SessionEnd: {
        bmmo::session_end_msg msg{};
        if (decode(msg)) core.on_session_end(msg);
        break;
    }
    default:
        break;
    }
}

// A manager activation recorded for `tick`, re-issued (inside a replay, and
// for the frame about to run after one).  `inside`: a replayed tick, where an
// explosion's referential is placed (step P; host support's
// manager_activations), not the live frame a rewind's end re-issues for.
void BallanceMMOClient::core_host_replay_activation(uint32_t tick, bool inside) {
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
    // The trafo scripts' activations are re-issued with or without a manager.
    const uint64_t placed = host.activations.referentials_placed();
    if (host.activations.replay(context, manager ? manager->GetID() : 0, tick, inside)) ++host.activations_replayed;
    if (host.activations.referentials_placed() != placed && host.activations.referentials_placed() <= 8)
        logger_->Info("Physics session: replay: an explosion's referential placed where the live frame had it at tick %u", tick);
}

// The retail SetPhysicsForce leaves push zero while the own replica drives the
// ball (the legacy physics_session_zero_retail_forces over the host's graph).
void BallanceMMOClient::core_host_zero_retail_forces() {
    CKContext* context = m_bml->GetCKContext();
    const float zero = 0.0f;
    for (const auto& leaf: host.navigation.leaves) {
        auto* block = CKBehavior::Cast(context->GetObject(leaf.force_block));
        if (!block) continue;
        if (CKParameterIn* in = block->GetInputParameter(4))
            if (CKParameter* source = in->GetRealSource()) source->SetValue(&zero, sizeof(zero));
    }
}

// ---------------------------------------------------------------- the platform

// Design 2.2, the retail column: every operation the core calls, each with the
// means the legacy retail session uses for the same thing today.
bmmo::session::session_platform BallanceMMOClient::core_host_platform() {
    bs::session_platform p;

    // ---- transport and time ----
    p.send = [this](const std::string& bytes, bs::send_mode mode) {
        int flags = k_nSteamNetworkingSend_Reliable;
        if (mode == bs::send_mode::reliable_no_nagle) flags = k_nSteamNetworkingSend_ReliableNoNagle;
        else if (mode == bs::send_mode::unreliable_no_delay) flags = k_nSteamNetworkingSend_UnreliableNoDelay;
        std::string copy = bytes;   // client::send takes a mutable buffer
        send(copy.data(), copy.size(), flags);
    };
    p.now = [](bs::clock_site) { return clock_type::now(); };
    p.log = [this](bs::log_level level, const std::string& text) {
        if (level == bs::log_level::warn) logger_->Warn("%s", text.c_str());
        else logger_->Info("%s", text.c_str());
    };
    p.utc_ms = [](bs::clock_site) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    };

    // ---- reading the world ----
    p.body_state = [this](const std::string& entity, bmmo_physics_body_state& out) {
        std::string error;
        return physics_view_.get_body_state(entity.c_str(), out, error);
    };
    // The journal checkpoint's rows (its one caller): read at 53-bit x87
    // precision (step TR4c, report_precision_53 above).
    p.list_bodies = [this]() {
        report_precision_53 precision;
        return physics_view_.list_bodies();
    };
    p.sector_manager_active = [this]() {
        const bool active = physics_session_sector_manager_active();
        if (core_manager_trace_due) {   // step RT2: the live pass's read only
            core_manager_trace_due = false;
            logger_->Info("Physics session: sector manager: tick %u %s", core_manager_trace_tick,
                          active ? "active" : "idle");
        }
        return active;
    };
    // The cell is read only while the checkpoint hold is in force: otherwise
    // the checkpoint hook has reported the crossing after the fact already
    // (design 2.2 poll_sector).  The resetpoint is not watched (the hook
    // reports through request_sector).
    p.poll_sector = [this](uint32_t) {
        bs::sector_poll poll;
        if (sector_activate_.block && !sector_activate_failed_) {
            if (core_chain_cell.applied()) {
                // Step B2SX: the chain's own copy, which no pass writes.
                int cell = 0;
                poll.cell_read = core_chain_cell.read(m_bml->GetCKContext(), cell);
                poll.cell = cell;
                poll.cell_private = true;
                poll.checkpoint_held = poll.cell_read;
            } else if (CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter")) {
                int cell = 0;
                poll.cell_read = parameters->GetElementValue(0, 1, &cell) != 0;
                poll.cell = cell;
                poll.checkpoint_held = poll.cell_read;
            }
        }
        if (CKDataArray* level = m_bml->GetArrayByName("CurrentLevel")) {
            int phase = 0;
            poll.phase_read = level->GetElementValue(0, 4, &phase) != 0;
            poll.phase = phase;
        }
        return poll;
    };
    p.world_hash = [this](bmmo::physics::world_hash& out) {
        std::string error;
        return physics_view_.capture(out, error);
    };
    p.own_ball = [this]() {
        bs::own_ball_view own;
        CK3dObject* ball = get_current_ball();
        own.entity = ball && ball->GetName() ? ball->GetName() : "";
        own.ball_type = ball_row_of(m_bml, own.entity);
        std::string error;
        own.physicalized = !own.entity.empty() && physics_view_.get_body_state(own.entity.c_str(), own.state, error);
        return own;
    };
    // The body guard on the current ball (engine change #6), the own group,
    // and the own replica - always attached on retail (design 4.1), its
    // polling following the mute.
    p.own_body_upkeep = [this](const bs::own_ball_view& own) {
        std::string error;
        const std::string& ball_name = own.entity;
        if (!ball_name.empty() && (!host.body_guard || host.body_guard_entity != ball_name)) {
            if (physics_view_.set_body_guard(true, ball_name.c_str(), error)) {
                host.body_guard = true;
                host.body_guard_entity = ball_name;
            } else {
                logger_->Warn("Physics session core: body guard: %s", error.c_str());
            }
        }
        if (!own.physicalized) {
            host.own_group_set = false;
            return;
        }
        if (!host.own_group_set) {
            const std::string group = host.core ? host.core->own_group() : std::string();
            if (physics_view_.set_body_group(ball_name.c_str(), group.c_str(), error)) host.own_group_set = true;
            else logger_->Warn("Physics session: own ball group: %s", error.c_str());
        }
        if (!host.navigation_known || (host.own_replica.active && host.own_replica.entity == ball_name)) return;
        const uint8_t type = static_cast<uint8_t>(own.ball_type < 0 ? 0 : own.ball_type);
        switch (bmmo::game::attach_own_replica(physics_view_, host.navigation, host.own_replica, ball_name,
                                               ball_force(type), error)) {
        case bmmo::game::own_replica_attach::created:
            if (host.input_muted
                    && !physics_view_.navigation_poll(ball_name.c_str(), false, host.own_replica.key_codes,
                                                      host.own_replica.key_blocks, host.own_replica.key_count, error))
                logger_->Warn("Physics session: own ball navigation replica: %s", error.c_str());
            logger_->Info("Physics session: own ball %s driven by the navigation replica (type %u, force %.3f)",
                          ball_name.c_str(), type, ball_force(type));
            break;
        case bmmo::game::own_replica_attach::failed:
            logger_->Warn("Physics session: own ball navigation replica: %s", error.c_str());
            break;
        default:
            break;
        }
    };
    p.own_replica_name = []() { return host.own_replica.entity; };
    // Retail's leaf rule (design 4.1): unbound leaves other mods graft onto
    // Ball Navigation are dropped and the rest renumbered; four keyed leaves
    // make the graph known (the legacy frame's rule, PSC "Key bindings").
    p.discover_navigation = [this](uint32_t tick) {
        if (host.navigation_known) return false;
        auto graph = bmmo::game::read_navigation_graph(m_bml->GetCKContext());
        if (graph.valid()) {
            graph.leaves.erase(std::remove_if(graph.leaves.begin(), graph.leaves.end(),
                                              [](const bmmo::game::navigation_leaf& leaf) { return leaf.key == 0; }),
                               graph.leaves.end());
            for (size_t i = 0; i < graph.leaves.size(); ++i) graph.leaves[i].index = static_cast<int>(i);
        }
        if (!graph.valid() || graph.leaves.size() < 4) return false;
        host.navigation = graph;
        host.navigation_known = true;
        host.outside_force_blocks.clear();
        for (CK_ID id: bmmo::game::outside_force_blocks(m_bml->GetCKContext()))
            host.outside_force_blocks.push_back(static_cast<uint32_t>(id));
        logger_->Info("Physics session: %zu navigation leaves at tick %u, keys %d/%d/%d/%d", graph.leaves.size(), tick,
                      graph.leaves[0].key, graph.leaves[1].key, graph.leaves[2].key, graph.leaves[3].key);
        return true;
    };
    // The bridge's event log: created / deleted / script_wakeup to the core
    // in order (an own body made or gone sets the own group again); the
    // diagnostics under trace.  The birth recipes are not kept from here: the
    // retail birth log is fed by OnPhysicalize (design 2.4).
    p.drain_body_events = [this](uint32_t tick, const std::function<bool(const bs::body_event&)>& each) {
        const std::string events = physics_view_.drain_event_log();
        const bool trace = host.core && host.core->config.trace;
        bs::parse_event_log(events, [&](const std::string& kind, const std::string& name) {
            if (name.empty()) return;
            bs::body_event event;
            if (kind == "created") event.what = bs::body_event::kind::created;
            else if (kind == "deleted") event.what = bs::body_event::kind::deleted;
            else if (kind == "script_wakeup") event.what = bs::body_event::kind::wakeup;
            else {
                if (trace && (kind == "constraint" || kind == "birth" || kind == "psi" || kind == "force"))
                    logger_->Info("Physics session: %s %s at tick %u", kind.c_str(), name.c_str(), tick);
                return;
            }
            event.name = name;
            if (each(event)) host.own_group_set = false;
        });
    };
    // Not a ball (step P: the core's is_ball, not the "Ball_" prefix), not
    // the own ball, not a mirror, and not a trafo explosion piece: no other
    // world has the pieces, so their Physicalize's wake-up is nobody's
    // BodyRevived.
    p.reportable_wakeup = [](const std::string& name, const std::string& own_ball) {
        return !(host.core && host.core->is_ball(name)) && name != own_ball && name.find("_Peer_") == std::string::npos
            && !host.explosion_pieces.count(name);
    };
    // What the own replica stepped T with (design 2.2 observe_input, 4.2
    // R2), which every replay drives the own ball from: the mute in force
    // during the step (host.input_muted: core_host_frame changes it only
    // after post_step(T)) - with it the replica did not poll, so keys 0, the
    // navigation off, and the frame flagged PAUSED; otherwise the keys the
    // replica polled (frame_keys_, this frame's input poll) and whether any
    // of the leaves' Key Events was active, the source the replica reads at
    // PreSimulate (ball_navigation.cpp navigation_apply_pending; nothing
    // between that and OnProcess changes it), not the BML hook's
    // ball_nav_active_; and the camera reference's rows.
    p.observe_input = [this]() {
        bs::input_observation seen;
        const bool muted = host.input_muted;
        if (!muted && host.navigation_known && input_hook_installed_)
            seen.keys = host.navigation.keys_from_state(frame_keys_.data());
        if (auto* orient = m_bml->Get3dEntityByName("Cam_OrientRef")) {
            const VxMatrix& m = orient->GetWorldMatrix();
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) seen.cam[r][k] = m[r][k];
            seen.cam_valid = true;
        }
        if (!muted && host.navigation_known) {
            // The blocks the replica polls: its own list when it exists
            // (bmmo::game::navigation_leaves, leaf indices 0-7), else the
            // graph's leaves with the same index rule.
            CKContext* context = m_bml->GetCKContext();
            const auto active = [context](uint32_t block) {
                auto* key_event = block ? CKBehavior::Cast(context->GetObject(block)) : nullptr;
                return key_event && key_event->IsActive();
            };
            if (host.own_replica.active) {
                for (int i = 0; i < host.own_replica.key_count && i < 8 && !seen.nav_active; ++i)
                    seen.nav_active = active(host.own_replica.key_blocks[i]);
            } else {
                for (const auto& leaf: host.navigation.leaves)
                    if (leaf.index >= 0 && leaf.index < 8 && active(leaf.key_block)) {
                        seen.nav_active = true;
                        break;
                    }
            }
        }
        seen.paused = muted;
        if (!muted && seen.nav_active != ball_nav_active_) ++host.nav_source_differs;
        if (muted != (m_bml->IsIngame() && m_bml->IsPaused())) ++host.mute_source_differs;
        return seen;
    };
    p.own_camera_rows = [this](const float (*basis)[3]) {
        if (!host.own_replica.active) return;
        std::string error;
        if (!physics_view_.navigation_input(host.own_replica.entity.c_str(), 0, basis[0], basis[1], basis[2], false, error))
            logger_->Warn("Physics session: own navigation rows: %s", error.c_str());
        core_host_zero_retail_forces();
    };
    // Step DX3 (design 9.3 (b)): the outside force producers' controllers,
    // read through the plugin (force_block_get, API v14).
    p.outside_forces = [this](std::vector<bmmo::physics::outside_controllers::force_reading>& out) {
        bmmo::physics::outside_controllers::read_forces<bmmo::physics::physics_view, bmmo_physics_force_block>(
                physics_view_, host.outside_force_blocks, out);
    };
    // Every movable body (step P): the core leaves the balls out by what they
    // are, not by the "Ball_" prefix the trafo explosion pieces share.
    p.movable_bodies = [this]() {
        std::vector<std::string> names;
        for (const auto& body: physics_view_.list_bodies()) {
            if (!body.movable || !body.name[0]) continue;
            names.emplace_back(body.name);
        }
        return names;
    };
    p.note_script_resets = [this](uint32_t tick) {
        for (CK_ID id: bmmo::game::script_replay::resets_pending(m_bml->GetCKContext(), host.script_ids))
            host.script_reset_ticks[id] = tick;
    };
    // N1-N3 kept neutralized, and N4's split kept (design 1.3 step 4; the
    // clock guard and the pause edits stay host code at the top of the frame).
    p.script_guards = [this](uint32_t, bool resolved) {
        death_reset_apply(resolved);
        sector_deactivate_apply();
        sector_activate_apply();
        if (const int again = checkpoint_phase_keep())
            logger_->Info("Physics session: the checkpoint chain's phase cell: %d target(s) handed back to "
                          "CurrentLevel were moved to the copy again", again);
        if (core_chain_cell.keep(m_bml->GetCKContext()))   // step B2SX
            logger_->Info("Physics session: the checkpoint chain's sector cell: its Set Cell handed back to "
                          "IngameParameter was moved to the copy again");
        if (core_tutorial_hold.keep(m_bml->GetCKContext()))   // step RK3H
            logger_->Info("Physics session: the Level 1 tutorial's frame-9 hold: its input handed back was moved to the "
                          "constant again");
        if (core_depth_test.keep(m_bml->GetCKContext()))   // FULL step PRX
            logger_->Info("Physics session: the depth test: its Test's threshold input handed back was moved to the "
                          "constant again");
    };

    // ---- writing the world ----
    p.write_body = [this](const std::string& entity, const double position[3], const double rotation[4],
                          const float linear[3], const float angular[3], bool wake, std::string& error) {
        error.clear();
        return physics_view_.set_body_state(entity.c_str(), position, rotation, linear, angular, wake, error);
    };
    // One union pass: deactivate nothing, activate `sector`, run the manager
    // from its start (the legacy physics_session_run_sector's writes; the
    // core records the activation and logs).
    p.run_sector = [this](int sector) {
        CKContext* context = m_bml->GetCKContext();
        CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter");
        CKBehavior* manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
        if (!parameters || !manager) {
            logger_->Error("Physics session: sector %d could not be activated: IngameParameter or "
                           "Gameplay_SectorManager is missing", sector);
            return;
        }
        // A peer's sector in the cell is not this ball's (update_current_sector).
        if (sector != current_sector_) sector_cell_is_own_ = false;
        // The pass's writes (game/sector_activation.hpp, one copy for every
        // host; step SP10 moved this host onto it).
        bmmo::game::run_sector_pass(context, parameters, manager, sector);
    };
    p.record_activation = [this](uint32_t tick, bool reset) {
        host.activations.record(m_bml->GetCKContext(), tick, reset);
    };
    // Step B1: a pass a re-deriving replay made (or a live one it forgot) in
    // the tick's pass slot (host support); after that rewind, the activate
    // cell as the replay left it, with run_sector's bookkeeping: a sector
    // that is not this ball's is a peer's in the cell.
    p.set_union_pass = [this](uint32_t tick, int sector) {
        if (sector > 0) host.activations.set_pass(tick, sector);
        else host.activations.forget_pass(tick);
    };
    p.sector_cell_adopted = [this](int& cell) {
        CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter");
        if (!parameters || !parameters->GetElementValue(0, 1, &cell)) return false;
        if (cell != current_sector_) sector_cell_is_own_ = false;
        return true;
    };
    // Step A3: the exact resync's, over the file-scope host state (host
    // support): the server's script record localized, the old numbering's
    // activations and script resets forgotten at the re-assign, the births
    // of the bodies the server's state has at S forgotten after the
    // restore, the birth log for the reconcile, the trafo pieces as this
    // world's own.
    p.localize_scripts = [this](const std::vector<uint8_t>& portable, const std::vector<uint8_t>* anchor,
                                std::vector<uint8_t>& out, std::string& error, bs::resync_script_report& report) {
        // Step WG4 (world agreement 4.4): under the union the server's hold
        // at S goes into the record, localized by name (the headless host's).
        const bmmo::game::script_replay::portable_hold_localize hold =
            [this](const uint8_t* data, size_t size, std::vector<uint8_t>& section, std::string& why) {
                return proximity_host.union_.localize_hold(m_bml->GetCKContext(), data, size, section, why);
            };
        return bmmo::game::localize_session_scripts(m_bml->GetCKContext(), host.script_ids, portable, anchor, out, error,
                                                    report, trafo_host.on ? &trafo_host.registry.part() : nullptr,
                                                    proximity_host.active ? &hold : nullptr);
    };
    p.forget_activations_from = [this](uint32_t tick) {
        const size_t activations = host.activations.forget_from(tick);
        host.script_order.forget_from(tick);   // step RO1: the old numbering's live orders
        const size_t resets = bmmo::game::forget_reset_ticks_from(host.script_reset_ticks, tick);
        if (activations || resets)
            logger_->Info("Physics session: resync: %zu activation tick(s) and %zu script reset(s) of the old numbering "
                          "from tick %u forgotten", activations, resets, tick);
    };
    p.forget_births_after = [](uint32_t tick, const std::vector<std::string>& entities) {
        host.births.forget_after(tick, entities);
    };
    // Step A5: the level walk's pending activations inside a late joiner's
    // resync window (host support).
    p.forget_pending_activations = [](uint32_t from, uint32_t to) { return host.activations.forget_pending(from, to); };
    p.birth_tick_of = [](const std::string& entity) { return host.births.birth_tick_of(entity); };
    p.unpack_bytes = [](const std::vector<uint8_t>& packed, uint32_t size, std::vector<uint8_t>& out) {
        auto* bytes = CKUnPackData(static_cast<int>(size), reinterpret_cast<char*>(const_cast<uint8_t*>(packed.data())),
                                   static_cast<int>(packed.size()));
        if (!bytes) return false;
        out.assign(reinterpret_cast<const uint8_t*>(bytes), reinterpret_cast<const uint8_t*>(bytes) + size);
        CKDeletePointer(bytes);
        return true;
    };
    // Step T4 (as T3's headless host): with the trafo as an input every world
    // has every set's pieces (and no original has a body): none is this
    // world's alone.  Without it, step P's owner-only pieces.
    p.local_only_body = [](const std::string& entity) { return !trafo_host.on && host.explosion_pieces.count(entity) != 0; };

    // ---- the trafo as an input (trafo-replication design 2.3-2.5, 2.11,
    // 4.7; step T4): the headless host's operations over the file-scope
    // trafo_host and physics_view ----
    p.poll_trafo = [this](uint32_t tick, bool option_e) {
        bs::trafo_poll out;
        if (!trafo_host.on) return out;
        CKContext* context = m_bml->GetCKContext();
        for (const auto& r: trafo_host.hold.poll(context, tick, option_e)) {
            bs::trafo_report x;
            x.flags = r.flags;
            x.type = r.type;
            for (int k = 0; k < 3; ++k) x.position[k] = r.position[k];
            x.predicted_tick = r.predicted_tick;
            x.source = r.source;
            out.reports.push_back(std::move(x));
        }
        const auto effect = trafo_host.hold.effect_check(context, trafo_host.hooks);
        out.leaks = effect.leaks();
        out.first_leak = effect.first;
        return out;
    };
    p.make_trafo_set = [this](uint32_t player, uint16_t life, std::string& error) {
        if (!trafo_host.on) {
            error = "the trafo is not an input in this world";
            return false;
        }
        CKContext* context = m_bml->GetCKContext();
        const std::string label = bs::trafo_life_label(player, life);
        const auto r = bmmo::game::ensure_trafo_set(context, trafo_host.registry, player, life, trafo_host.hooks);
        if (!r.ok) {
            for (const auto& line: r.lines) logger_->Warn("Physics session: trafo build %s: %s", label.c_str(), line.c_str());
            error = r.error;
            return false;
        }
        if (r.how == bmmo::game::trafo_ensure::reselected)
            for (const auto& line: r.lines) logger_->Info("Physics session: trafo: set %s %s", label.c_str(), line.c_str());
        if (r.how == bmmo::game::trafo_ensure::built) {
            // Design 9.1's settlement on the retail CK2.dll: the builder's
            // lines (I0-I7) and the digests, comparable with the headless.
            for (const auto& line: r.lines) logger_->Info("Physics session: trafo build %s: %s", label.c_str(), line.c_str());
            if (const auto* set = trafo_host.registry.find(player, life))
                logger_->Info("Physics session: trafo: set %s built: %zu objects, %zu scripts, digest %s, value digest %s",
                              label.c_str(), set->objects.size(), set->scripts.size(), set->digest.c_str(),
                              set->value_digest.c_str());
        }
        bmmo::game::add_trafo_set_pieces(context, trafo_host.registry, host.explosion_pieces);
        return true;
    };
    p.run_trafo = [this](const bs::trafo_input& in, std::string& error) {
        return bmmo::game::run_trafo(m_bml->GetCKContext(), trafo_host.registry, trafo_host.hooks, in.player, in.life, in.flags,
                                     in.type, in.position, in.seed, error);
    };
    p.trafo_final = [](uint32_t player, uint16_t life, uint8_t type, uint32_t tick) {
        bmmo::game::trafo_set_final(trafo_host.registry, player, life, type, tick);
    };
    p.trafo_frame = [this](uint32_t) {
        if (trafo_host.on) bmmo::game::trafo_frame(m_bml->GetCKContext(), trafo_host.registry, 1000.0f / 66.0f);
    };
    // FULL step PRX (game/depth_test.hpp): the level's fall handler for free
    // props, live and in every replayed tick, where the server's world runs it.
    p.depth_test = [this](uint32_t tick) {
        if (!core_depth_test.pinned()) return;
        core_depth_test.step(m_bml->GetCKContext(), static_cast<const bmmo::physics::physics_view*>(&physics_view_), tick,
                             [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); });
    };
    p.trafo_type_set = [this](uint32_t player, uint16_t life, uint8_t type) {
        bs::trafo_type_view view;
        const bmmo::game::trafo_set* set = trafo_host.registry.find(player, life);
        if (!set || type >= bs::kTrafoTypes) return view;
        view.exists = true;
        CKContext* context = m_bml->GetCKContext();
        for (CK_ID id: set->types[type].pieces) {
            CKObject* o = context->GetObject(id);
            if (!o || !o->GetName()) continue;
            view.pieces.emplace_back(o->GetName());
            if (trafo_host.hooks.has_body && trafo_host.hooks.has_body(o->GetName())) ++view.bodies;
        }
        return view;
    };
    p.trafo_reconcile = [this](const bs::trafo_reconcile_spec& spec) {
        bs::trafo_reconcile_result r;
        CKContext* context = m_bml->GetCKContext();
        if (spec.action == bs::trafo_reconcile_action::unmake) {
            r.ok = bmmo::game::unmake_type_set(context, trafo_host.registry, trafo_host.hooks, spec.player, spec.life, spec.type,
                                               r.bodies_removed, r.error);
            return r;
        }
        if (spec.action == bs::trafo_reconcile_action::retire) {   // step TRL
            r.ok = bmmo::game::retire_trafo_set_now(context, trafo_host.registry, trafo_host.hooks, spec.player, spec.life,
                                                    spec.birth_tick, r.bodies_removed, r.error);
            if (r.ok)
                logger_->Info("Physics session: trafo: set %s retired at once (tick %u)",
                              bs::trafo_life_label(spec.player, spec.life).c_str(), spec.birth_tick);
            return r;
        }
        // The remake, inside the core's mute: the pieces' Physicalize
        // reaches OnPhysicalize, which logs their births at the resync's
        // tick (the headless host takes them from the muted event log).
        trafo_host.birth_tick = spec.birth_tick;
        trafo_host.if_absent = false;
        r.ok = bmmo::game::remake_type_set(context, trafo_host.registry, trafo_host.hooks, spec.player, spec.life, spec.type,
                                           spec.position, spec.seed, 1000.0f / 66.0f, r.error);
        trafo_host.birth_tick = 0;
        return r;
    };
    p.retire_trafo_sets = [this](uint32_t now, uint32_t history) {
        std::vector<bs::trafo_key> deselected;
        if (!trafo_host.on) return deselected;
        CKContext* context = m_bml->GetCKContext();
        std::set<bs::trafo_key> before;
        for (const auto& set: trafo_host.registry.sets())
            if (set.selected) before.insert({set.player, set.life});
        const size_t sets_before = trafo_host.registry.sets().size();
        for (const auto& line: bmmo::game::retire_trafo_sets(context, trafo_host.registry, trafo_host.hooks, now, history, false))
            logger_->Info("Physics session: trafo: %s", line.c_str());
        // A destroyed set's pieces are no explosion pieces any more: the
        // originals and the sets still held.
        if (trafo_host.registry.sets().size() != sets_before) {
            host.explosion_pieces = bmmo::game::explosion_pieces(context);
            bmmo::game::add_trafo_set_pieces(context, trafo_host.registry, host.explosion_pieces);
        }
        for (const auto& key: before) {
            const auto* set = trafo_host.registry.find(key.player, key.life);
            if (!set || !set->selected) deselected.push_back(key);
        }
        return deselected;
    };
    // The manager's and the trafo scripts' (step P: host support's
    // record_pending, the selection's `activations`).
    p.record_pending_activation = [this](uint32_t tick) {
        CKBehavior* manager = bmmo::game::find_root_script(m_bml->GetCKContext(), "Gameplay_SectorManager");
        host.activations.record_pending(m_bml->GetCKContext(), manager ? manager->GetID() : 0, host.script_activations, tick);
    };
    // Step RO1: the order this live frame ran the replay's scripts in.
    p.record_script_order = [this](uint32_t tick) {
        if (host.script_ids.empty()) return;
        host.script_order.record(tick, bmmo::game::script_replay::live_frame_order(m_bml->GetCKContext(), host.script_ids));
    };

    // ---- mirrors: the game_objects spirit balls ----
    p.mirror_entity = [this](uint32_t player, uint8_t ball_type, std::string& error) {
        objects_.ensure_player(player);
        CK3dObject* entity = objects_.get_ball_entity(player, ball_type);
        if (!entity || !entity->GetName()) {
            error = std::format("no spirit ball for player {} type {}", player, ball_type);
            return std::string();
        }
        check_mirror_contract(logger_, player, ball_type, entity);
        return std::string(entity->GetName());
    };
    p.make_mirror = [this](const bs::mirror_spec& spec) {
        bs::mirror_made made;
        objects_.ensure_player(spec.player);
        CK3dObject* entity = objects_.get_ball_entity(spec.player, spec.ball_type);
        if (!entity || !entity->GetName() || spec.entity != entity->GetName()) {
            made.error = "no spirit ball named '" + spec.entity + "'";
            return made;
        }
        check_mirror_contract(logger_, spec.player, spec.ball_type, entity);
        made = bmmo::game::make_mirror_body(physics_view_, entity, spec);
        if (!made.made) return made;
        const auto shown = host.mirror_type.find(spec.player);
        const uint32_t previous = shown != host.mirror_type.end() && shown->second != spec.ball_type
                ? shown->second : std::numeric_limits<uint32_t>::max();
        host.mirror_type[spec.player] = spec.ball_type;
        host.mirror_player[spec.entity] = spec.player;
        objects_.set_physicalized(spec.player, true);
        objects_.on_trafo(spec.player, previous, spec.ball_type);
        return made;
    };
    p.unmake_mirror = [this](const std::string& entity) {
        std::string error;
        physics_view_.unphysicalize(entity.c_str(), error);
        if (const auto it = host.mirror_player.find(entity); it != host.mirror_player.end())
            objects_.set_physicalized(it->second, false);
    };
    p.detach_mirror_navigation = [this](const std::string& entity) {
        std::string error;
        physics_view_.navigation_destroy(entity.c_str(), error);
    };
    p.attach_mirror_navigation = [this](uint32_t player, const std::string& entity, uint8_t ball_type, std::string& error) {
        if (!host.navigation_known) return bs::mirror_navigation::unavailable;
        return bmmo::game::attach_mirror_navigation(physics_view_, host.navigation, entity, player, ball_force(ball_type), error);
    };
    p.mute_event_log = [this](bool muted) {
        std::string error;
        physics_view_.set_event_log_muted(muted, error);
    };

    // ---- driving ----
    p.own_driven_entity = []() { return host.own_replica.active ? host.own_replica.entity : std::string(); };
    p.drive = [this](const std::string& entity, const bs::input_frame& frame, std::string& error) {
        const bool active = (frame.flags & bs::INPUT_FLAG_NAV_ACTIVE) != 0;
        return physics_view_.navigation_input(entity.c_str(), frame.keys, frame.cam_right, frame.cam_up, frame.cam_dir,
                                              active, error);
    };
    p.force_own = [this](const bs::input_frame& held, bool& ok, std::string& error) {
        if (!host.own_replica.active) return std::string();
        const std::string entity = host.own_replica.entity;
        ok = physics_view_.navigation_poll(entity.c_str(), false, host.own_replica.key_codes, host.own_replica.key_blocks,
                                           host.own_replica.key_count, error)
          && physics_view_.navigation_input(entity.c_str(), held.keys, held.cam_right, held.cam_up, held.cam_dir,
                                            (held.flags & bs::INPUT_FLAG_NAV_ACTIVE) != 0, error);
        return entity;
    };
    // The polling comes back as the mute says, never plain on (design 2.2).
    p.unforce_own = [this](const std::string& entity, std::string& error) {
        return physics_view_.navigation_poll(entity.c_str(), !host.input_muted, host.own_replica.key_codes,
                                             host.own_replica.key_blocks, host.own_replica.key_count, error);
    };
    // arm_own_capture, own_birth_capture: empty (the own births come from
    // OnPhysicalize, and the game's Physicalize block owns the own ball:
    // design 2.3).  make_own, unmake_own (step EL8, own_body_in_replay):
    // inside a rewind only, through host support's make_own_body /
    // unmake_own_body - the headless client's code - on the bridge.  The
    // replica: a revived life gets a navigation when the replica is not on
    // it (attach_revive_navigation), destroyed with the body at its
    // replayed death; any other take-out shuts the replica's leaves first,
    // a replayed death leaves them as the live death did.
    p.make_own = [this](const bs::own_body_spec& spec) {
        std::string error;
        if (host.own_replica.active && host.own_replica.entity == spec.entity) {
            const bmmo_physics_nav_state off{};
            if (!physics_view_.navigation_set_state(spec.entity.c_str(), off, error))
                logger_->Warn("Physics session core: own body %s: navigation shutdown: %s", spec.entity.c_str(), error.c_str());
        }
        auto* entity = CK3dEntity::Cast(m_bml->GetCKContext()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(spec.entity.c_str()), CKCID_3DENTITY, nullptr));
        bs::own_body_made made = bmmo::game::make_own_body(physics_view_, entity, spec);
        bool created = false;
        const int row = made.made && spec.with_replica ? ball_row_of(m_bml, spec.entity) : 0;
        if (made.made && spec.with_replica && row < 0) {
            // No ball table row: no force to give a navigation (never row
            // 0's); the revived body runs without one.
            logger_->Warn("Physics session core: own body %s: no ball table row, no revive navigation", spec.entity.c_str());
        } else if (made.made && spec.with_replica) {
            const float force = ball_force(static_cast<uint8_t>(row));
            if (!bmmo::game::attach_revive_navigation(physics_view_, host.navigation, host.own_replica, spec.entity, force,
                                                      created, error))
                logger_->Warn("Physics session core: own body %s: the revive's navigation: %s", spec.entity.c_str(),
                              error.c_str());
            if (created) host.revive_navigations.insert(spec.entity);
        }
        if (made.made) ++host.own_bodies_made;
        logger_->Info("Physics session core: own body %s made for tick %u (%s%s%s)", spec.entity.c_str(), spec.tick,
                      made.made ? "made" : (made.found ? "refused" : "no entity"),
                      spec.with_replica ? (created ? ", revive navigation created" : ", the replica drives it") : "",
                      spec.kick ? ", kicked" : "");
        return made;
    };
    p.unmake_own = [this](const std::string& name, const float position[3], const float rotation[9], uint32_t tick, bool death,
                          std::string& error) {
        std::string nav_error;
        const bool revive_nav = host.revive_navigations.erase(name) != 0;
        if (revive_nav) {
            if (!physics_view_.navigation_destroy(name.c_str(), nav_error))
                logger_->Warn("Physics session core: own body %s: the revive's navigation: %s", name.c_str(), nav_error.c_str());
        } else if (!death && host.own_replica.active && host.own_replica.entity == name) {
            const bmmo_physics_nav_state off{};
            if (!physics_view_.navigation_set_state(name.c_str(), off, nav_error))
                logger_->Warn("Physics session core: own body %s: navigation shutdown: %s", name.c_str(), nav_error.c_str());
        }
        auto* entity = CK3dEntity::Cast(m_bml->GetCKContext()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        const bool ok = bmmo::game::unmake_own_body(physics_view_, entity, name, position, rotation, error);
        if (ok) ++host.own_bodies_unmade;
        logger_->Info("Physics session core: own body %s taken out at tick %u (%s%s%s)", name.c_str(), tick,
                      ok ? "done" : error.c_str(), death ? ", replayed death" : "",
                      revive_nav ? ", revive navigation destroyed" : "");
        return ok;
    };
    // Step EL8 (design 9.7): an entity's CK world matrix.
    p.entity_matrix = [this](const std::string& name, float matrix[16]) {
        auto* entity = CK3dEntity::Cast(m_bml->GetCKContext()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        if (!entity) return false;
        const VxMatrix& world = entity->GetWorldMatrix();
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) matrix[r * 4 + k] = world[r][k];
        return true;
    };
    // Step BALA: the airship zone over this world (physics_state.hpp, bridge
    // v24) and the ghosts' display.
    p.airship_zone = [this](bool enable, const std::string& own_group, double box[6], std::string& error) {
        bmmo::physics::airship_zone_box zone;
        if (!physics_view_.airship_zone_set(enable, own_group.c_str(), zone, error)) return -1;
        for (int k = 0; k < 3; ++k) {
            box[k] = zone.min[k];
            box[3 + k] = zone.max[k];
        }
        return zone.active ? 1 : 0;
    };
    p.airship_zone_contains = [this](const std::string& name) { return physics_view_.airship_zone_contains(name.c_str()); };
    p.airship_zone_box = [this](double box[6]) { return physics_view_.airship_zone_get(box); };
    // Step BALL (protocol 17, the user's decision of 2026-10-28 "switch to the old path"):
    // a ghost is the spirit ball handed back to game_objects::update - not
    // marked physicalized, so the legacy ball stream (the owner's
    // OwnedCompressedBallState, relayed by the server inside a physics room
    // too) moves it with the legacy interpolation / extrapolation, and
    // translucent like any shadow-mode ball.  The body is parked by the zone
    // itself: frozen and collision-free where its copy entered, in every
    // world alike (physics_state.cpp airship_zone_listener), so nothing here
    // touches it.  Nothing reads the moved CK matrix back into it: physics_RT
    // writes a CK matrix from IVP only for a body in m_MovableObjects (a
    // frozen one is not) and never the other way; a mirror is re-made from
    // its lifecycle spec (make_mirror_body poses the entity first) and a
    // rollback's restore takes the IVP state from the blob.  And nothing in
    // the core's frame reads the moved matrix (the place point, the records):
    // the frame runs on the frozen body's physics matrix (ghost_pins).
    // Handed over only while the body is frozen (a body in the box is frozen
    // at the next PSI; until then it stays physicalized, so its matrix stays
    // physics_RT's).  Asked on every frame of the span (set_physicalized is
    // idempotent).  Off: a mirror that still has its body (a rewind to
    // before the entry, a resync, a beam) is solid and still again, with the
    // physics matrix the frame pinned (written back here when it is asked
    // outside a frame, the core's end); one whose body is gone (a death)
    // stays a legacy ball, as unmake_mirror left it, until its respawn
    // re-makes it from the lifecycle.
    p.show_ghost = [this](const std::string& name, bool ghost) {
        const auto it = host.mirror_player.find(name);
        if (it == host.mirror_player.end()) return;
        bmmo_physics_body_state state{};
        std::string error;
        const bool body = physics_view_.get_body_state(name.c_str(), state, error);
        auto* entity = ghost_entity(m_bml->GetCKContext(), name);
        const auto pin = ghost_pins.find(name);
        if (ghost) {
            if (!body || !entity || state.simulated) return;   // not frozen yet, or no body
            auto& held = ghost_pins[name];
            ghost_pin_capture(held, entity, state);   // inside the frame: the physics matrix
            objects_.set_physicalized(it->second, false);
            return;
        }
        if (pin != ghost_pins.end()) {
            if (body && entity && ghost_pin_matches(pin->second, state)) entity->SetWorldMatrix(pin->second.matrix);
            ghost_pins.erase(pin);
        }
        if (body) objects_.set_physicalized(it->second, true);
    };
    // Step SPGH: the spawn ghost over this world (physics_state.hpp, bridge
    // v25) and its display: a ghost is translucent, the own ball's materials
    // (show_own_spawn_ghost) or a spirit ball's (game_objects::set_ghost).
    p.spawn_ghost = [this](bool enable, bool respawns, double radius, double info[7], std::string& error) {
        bmmo::physics::spawn_ghost_info out;
        if (!physics_view_.spawn_ghost_set(enable, respawns, radius, out, error)) return -1;
        info[0] = static_cast<double>(out.areas);
        info[1] = out.radius;
        info[2] = static_cast<double>(out.solidified);
        info[3] = static_cast<double>(out.rechecks);
        info[4] = static_cast<double>(out.overlap_waits);
        info[5] = static_cast<double>(out.births);
        info[6] = static_cast<double>(out.resumed);
        return out.active ? 1 : 0;
    };
    p.spawn_ghost_state = [this](const std::string& name) { return physics_view_.spawn_ghost_state(name.c_str()); };
    p.spawn_ghost_birth = [this](const std::string& group) { return physics_view_.spawn_ghost_birth(group.c_str()); };
    p.show_spawn_ghost = [this](const std::string& name, bool own, bool ghost) {
        if (own) {
            show_own_spawn_ghost(m_bml->GetCKContext(), name, ghost);
            return;
        }
        if (const auto it = host.mirror_player.find(name); it != host.mirror_player.end())
            objects_.set_ghost(it->second, ghost);
    };
    p.set_entity_matrix = [this](const std::string& name, const float matrix[16]) {
        auto* entity = CK3dEntity::Cast(m_bml->GetCKContext()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        if (!entity) return false;
        VxMatrix world;
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) world[r][k] = matrix[r * 4 + k];
        entity->SetWorldMatrix(world);
        return true;
    };

    // ---- own ball ----
    p.ball_recipe_of = [this](const std::string& entity, bs::ball_recipe& recipe) {
        const int row = ball_row_of(m_bml, entity);
        fill_ball_recipe(m_bml, row, recipe);
        return row;
    };

    // ---- rollback ----
    // The shared binding (design 2.4) with the retail options of the legacy
    // binding (physics_session_rollback_world): no clock rewind, the lenient
    // savestate, the mute, the camera re-arm after a replay.
    p.bind_world = [this](const bs::core_world_hooks& hooks) {
        bs::session_world_options w;
        bs::rollback_binding_options& o = w.binding;
        o.delta_ms = 1000.0f / 66.0f;
        o.restore_clock = false;
        o.savestate_flags = 0u;
        o.own_key_codes = host.own_replica.key_codes;
        o.own_key_blocks = host.own_replica.key_blocks;
        o.own_key_count = host.own_replica.key_count;
        o.input_muted = []() { return host.input_muted; };
        o.on_write_failed = [this](const std::string& error) {
            if (host.line_logs++ < 16) logger_->Warn("Physics session: write failed: %s", error.c_str());
        };
        o.on_contacts_restored = [this](const std::string& entity, int, const std::string& error,
                                        const std::vector<bmmo_physics_contact>&) {
            if (!error.empty() && host.contact_logs++ < 8)
                logger_->Warn("Physics session: contact restore of %s: %s", entity.c_str(), error.c_str());
        };
        o.on_savestate_capture_failed = [this](const std::string& error) {
            if (host.savestate_logs++ < 8) logger_->Warn("Physics session: savestate capture failed: %s", error.c_str());
        };
        o.on_savestate_restored = [this](bool ok, const std::string& error, const bs::savestate_outcome&) {
            if (!ok && host.savestate_logs++ < 8) logger_->Warn("Physics session: savestate restore failed: %s", error.c_str());
        };
        o.on_poll_resumed = [this](const std::string&) {
            const float (*cam)[3] = host.core ? host.core->previous_cam() : nullptr;
            if (!host.own_replica.active || !cam) return;
            std::string error;
            physics_view_.navigation_input(host.own_replica.entity.c_str(), 0, cam[0], cam[1], cam[2], false, error);
        };
        o.log = [this](const std::string& text) {
            if (host.line_logs++ < 200 || diag_trace_plan()) logger_->Info("Physics session: %s", text.c_str());
        };
        // Inside the replay (an explosion's referential is placed); the
        // rewinds' end re-issues with inside = false (keep_live_cells).
        w.replay_activation = [this](uint32_t tick) { core_host_replay_activation(tick, true); };
        // Step N02R: after each replayed tick's step, the IngameParameter[0][2]
        // its live frame's chains wrote (host support's
        // manager_activations::after_replayed_step); step N01R: and the [0][1].
        w.after_replayed_step = [this](uint32_t tick) {
            const int written = host.activations.after_replayed_step(m_bml->GetCKContext(), tick);
            if (!written || host.cell_write_logs++ >= 8) return;
            CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter");
            for (int column = 1; column <= 2; ++column) {
                int value = 0;
                if (!(written & (1 << column))) continue;
                if (parameters) parameters->GetElementValue(0, column, &value);
                logger_->Info("Physics session: replay: IngameParameter[0][%d] = %d written after replayed tick %u (its live "
                              "frame's write)", column, value, tick);
            }
        };
        w.births = &host.births;
        // Step P: the trafo explosion pieces are made again at a restore
        // (the replayed reset takes them away again); T3: every set's too.
        // Gap L, L4 (step EL9): bind_script_replay adds every body of a
        // selected module (rollback_world::revivable).
        // FULL step PRX: and the depth test's props (game/depth_test.hpp):
        // one the rule removed inside the window is made again at the
        // restore and removed again by the replayed rule at its tick.
        w.revivable = [](const std::string& entity) {
            return host.explosion_pieces.count(entity) != 0 || core_depth_test.member(entity);
        };
        w.park_births = false;
        w.insert_births = false;
        w.put_back = [this](const std::string& entity, const bs::local_birth& birth) {
            bmmo::game::put_back_birth(m_bml->GetCKContext(), entity, birth);
        };
        w.on_own_destroy_failed = [this](const std::string& entity) {
            if (host.destroy_logs++ < 8) logger_->Warn("Physics session: own body %s could not be destroyed at the restore", entity.c_str());
        };
        w.on_destroy_failed = [this](const std::string& entity, const std::string& error) {
            if (host.destroy_logs++ < 8)
                logger_->Warn("Physics session: birth: destroying %s at the restore failed: %s", entity.c_str(), error.c_str());
        };
        if (!host.script_ids.empty()) w.bind_scripts = [this](bs::rollback_world& world) {
            bs::script_binding_options so;
            so.context = m_bml->GetCKContext();
            so.scripts = &host.script_ids;
            so.hooks = &host.script_hooks;
            so.scripts_replayed = &host.scripts_replayed;
            so.reset_ticks = &host.script_reset_ticks;
            so.reset_skips = &host.script_reset_skips;
            so.delta_ms = 1000.0f / 66.0f;
            so.diff_detail = host.core && host.core->config.trace;
            so.replay_activation = true;
            // Step T4: the sets' scripts, recorded (v3), restored and run
            // first in every replayed frame, as the trafo frame live.
            so.trafo = trafo_host.on ? &trafo_host.registry.part() : nullptr;
            // Step RO1: the live frames' order, and a string parameter
            // written back at its recorded length.
            so.live_order = &host.script_order;
            so.resize_strings = true;
            // Step B2SW (G1, F): the behaviour manager's list as live.
            so.live_membership = true;
            // Step B7EG (gap Q): the PreSimulate queue with the records.
            so.callback_queue = true;
            // Step RK2 (record-format decision Q4): the written-cell
            // section with every record, back at the anchor.
            so.written_cells = true;
            so.written_columns = &written_columns_host;
            // Step WG4 (world agreement 4.2): the proximity union's hold
            // section with every record (v6), back at the anchor.
            if (proximity_host.active) {
                so.hold.capture = [](std::vector<uint8_t>& out) { proximity_host.union_.capture(out); };
                so.hold.restore = [this](const uint8_t* data, size_t size, std::string& error) {
                    return proximity_host.union_.restore(m_bml->GetCKContext(), data, size, error);
                };
            }
            so.on_restore_failed = [this](const std::string& error) {
                if (host.restore_logs++ < 8) logger_->Warn("Physics session: script replay: restore failed: %s", error.c_str());
            };
            bs::bind_script_replay(world, physics_view_, so);
        };
        // Step EG0 diagnostics (off by default): the traced entities before
        // every replayed tick, after the core's births and the activation
        // (session_world_options::before_replayed_tick, the host's traces).
        if (!diag_trace_entities().empty())
            w.before_replayed_tick = [this](uint32_t tick) {
                logger_->Info("Physics session: %s", diag_entity_line(m_bml->GetCKContext(), "replay", tick).c_str());
            };
        bs::rollback_world world = bs::bind_session_world(physics_view_, hooks, w);
        // Step T4: the births a replayed tick made, in the birth log under
        // that tick (the headless host's rule, there from the muted event
        // log): a replay across an explosion physicalizes the set's pieces
        // again, and OnPhysicalize would log them at the live tick + 1, where
        // a later rewind would take them for bodies born after its anchor.
        // Step B2SW (T4 finding c): in every session, the trafo inputs on or
        // off - without them a replay across an original explosion (step P's
        // path) or a sector pass makes its bodies again the same way.
        if (world.pre_step) {
            const auto inner = world.pre_step;
            world.pre_step = [inner](uint32_t tick) {
                trafo_host.birth_tick = tick;
                trafo_host.if_absent = true;
                inner(tick);
                trafo_host.birth_tick = 0;
                trafo_host.if_absent = false;
            };
        }
        // Step EG0 diagnostics: every destroy_body the binding refuses (a
        // birth item's refusal counts birth_recreate_failures; since step
        // B8EG a ball's birth item never gets here - keeps_parked counts it
        // births_parked_balls; since step EL6 a refused blob no longer
        // destroys the revived bodies; a refused recreate_body is counted
        // but not logged here).
        if (diag_trace_plan() && world.destroy_body) {
            auto destroy = world.destroy_body;
            world.destroy_body = [this, destroy](const std::string& entity, uint32_t birth_tick) {
                const bool ok = destroy(entity, birth_tick);
                if (!ok) logger_->Info("Physics session: plan trace: refused destroy of %s@%u at the restore",
                                       entity.c_str(), birth_tick);
                return ok;
            };
        }
        return world;
    };
    p.keep_live_cells = [this](const bs::session_client_core& core) {
        auto guard = std::make_shared<bmmo::game::live_cells_guard>(
                m_bml->GetCKContext(), core, true, [this](uint32_t tick) { core_host_replay_activation(tick); },
                &host.activations);
        // Step EL8 (design 9.2): the guard's view after a rewind that made or
        // took out an own body - the replica and the own ball's matrix (the
        // core's scope puts the matrices of bodiless own entities back
        // before it ends this one: session_client_core::keep_live_cells).
        const uint64_t made_before = host.own_bodies_made, unmade_before = host.own_bodies_unmade;
        return bs::scope_guard([this, guard, made_before, unmade_before]() mutable {
            guard.reset();
            if (host.own_bodies_made == made_before && host.own_bodies_unmade == unmade_before) return;
            const std::string replica = host.own_replica.entity;
            float m[16] = {};
            bool have = false;
            if (auto* entity = CK3dEntity::Cast(m_bml->GetCKContext()->GetObjectByNameAndParentClass(
                        const_cast<CKSTRING>(replica.c_str()), CKCID_3DENTITY, nullptr))) {
                const VxMatrix& world = entity->GetWorldMatrix();
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) m[r * 4 + k] = world[r][k];
                have = true;
            }
            bmmo_physics_nav_state nav{};
            std::string error;
            const bool nav_ok = host.own_replica.active && physics_view_.navigation_get_state(replica.c_str(), nav, error);
            logger_->Info("Physics session core: rewind made %llu / took out %llu own bodies; replica %s active=%d "
                          "nav=%d/%02x/%02x/%02x; matrix %s (%a,%a,%a); revive navigations left %zu",
                          static_cast<unsigned long long>(host.own_bodies_made - made_before),
                          static_cast<unsigned long long>(host.own_bodies_unmade - unmade_before), replica.c_str(),
                          host.own_replica.active ? 1 : 0, nav_ok ? nav.active : -1, nav.key_mask, nav.controller_mask,
                          nav.create_pending_mask, have ? "" : "(no entity)", static_cast<double>(m[12]),
                          static_cast<double>(m[13]), static_cast<double>(m[14]), host.revive_navigations.size());
        });
    };

    // ---- journal: ModLoader/BMMOJournals, the newest 10 kept, and only
    // with the SessionJournal property on (read once per session, as the
    // legacy path does) ----
    // Step SIO (session I/O off the tick thread): the journal is written by
    // a strand of the mod's existing asio thread pool (thread_pool_); the
    // game thread only appends the records.  Step IOF: the strand flushes as
    // the SessionJournalFlush property says (none / batch / record; batch
    // when it is anything else), read when the journal opens.
    p.io_strand = [this] { return bs::strand_poster(thread_pool_.get_executor()); };
    p.journal_flush = [this] {
        auto mode = bs::io_flush::batch;
        if (auto* prop = config_manager_["session_journal_flush"]) {
            const std::string text = prop->GetString() ? prop->GetString() : "";
            if (!bs::parse_io_flush(text, mode)) {
                logger_->Warn("SessionJournalFlush \"%s\" is not none, batch or record: batch", text.c_str());
                mode = bs::io_flush::batch;
            }
        }
        // FULL step RT1 (IOF-c): the mode the journal's writer runs, logged.
        logger_->Info("Physics session: journal flush mode %s (SessionJournalFlush)", bs::io_flush_name(mode));
        return mode;
    };
    p.journal_path = [this](const bs::journal_header& header) -> std::filesystem::path {
        if (!config_manager_["session_journal"]->GetBoolean()) return {};
        const auto directory = bs::client_journal::directory();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        bs::client_journal::prune(directory, bs::client_journal::kKeepFiles - 1);
        return directory / bs::client_journal::file_name(header.session, header.level, header.own_player);
    };

    // ---- reports and hooks ----
    p.counters = [this]() {
        bs::host_counters h;
        h.rebases = static_cast<int>(fixed_tick_.rebases());
        h.activations_replayed = host.activations_replayed;
        h.script_reset_skips = host.script_reset_skips;
        h.probes_skipped = proximity_host.union_.skipped();     // step WG4
        h.probes_relatched = proximity_host.union_.relatched();
        h.unrecorded_array_reads = array_reads_host.unrecorded;   // step RK2
        h.written_cell_columns = written_columns_host.size();   // the binding's (step RK2's review)
        return h;
    };
    // Step WG4 (world agreement 2.5): the proximity union's place, live
    // (pre_step step 7b) and in every replayed tick; the core calls it only
    // while the union is active (the world option and this op).
    p.place_probes = [this](const std::vector<bs::probe_candidate>& candidates) {
        proximity_host.union_.place(m_bml->GetCKContext(), candidates);
    };
    // Step RK3: the Level 1 tutorial's exit, before frame 10 (the core
    // decides when; game/tutorial_exit.hpp activates).
    p.tutorial_exit = [this](uint32_t, std::string& error) {
        return core_tutorial_exit.activate(m_bml->GetCKContext(), error);
    };
    p.hooks.window = [this](const std::string& text) { logger_->Info("%s", text.c_str()); };
    p.hooks.status = [this](const std::string& text) { logger_->Info("%s", text.c_str()); };
    // 'session force-rollback E:D' (design 3.1's --force-rollback, the retail
    // verb): after the frame's own rewinds, every E ticks, D deep.
    p.hooks.after_rewinds = [this](uint32_t frame_tick) {
        if (host.core) log_mirror_births(logger_, *host.core);
        if (!host.core) return;
        // Step RT2: the union's live pass of frame_tick reads the manager next.
        core_manager_trace_due = core_manager_trace_left > 0;
        if (core_manager_trace_due) {
            --core_manager_trace_left;
            core_manager_trace_tick = frame_tick;
        }
        // Step RT2: 'session activate <script> at ...', once, after the frame
        // whose live tick reaches the target (the headless --activate-script's
        // "activated after tick T, frame T - base + 2 next").
        if (host.activate_armed && host.core->assigned()) {
            const uint32_t live = host.core->current_tick();
            const int64_t frames = host.core->frames_since_anchor();
            const uint32_t base = frames >= 1 ? live - static_cast<uint32_t>(frames - 1) : live;
            const uint32_t target = host.activate_relative ? base + host.activate_at : host.activate_at;
            if (static_cast<int32_t>(live - target) >= 0) {
                host.activate_armed = false;
                CKContext* context = m_bml->GetCKContext();
                CKBehavior* script = bmmo::game::find_root_script(context, host.activate_script.c_str());
                CKScene* scene = context ? context->GetCurrentScene() : nullptr;
                if (!script || !scene) {
                    logger_->Warn("Automation: session activate: %s not found (or no scene) at tick %u",
                                  host.activate_script.c_str(), live);
                } else {
                    const bool was_active = script->IsActive() != FALSE;
                    scene->Activate(script, TRUE);
                    logger_->Info("Automation: session activate: %s (#%u) activated after tick %u (tick base %u, frame %u "
                                  "next; it was %s)", host.activate_script.c_str(),
                                  static_cast<unsigned>(script->GetID()), live, base, live - base + 2,
                                  was_active ? "active" : "inactive");
                }
            }
        }
        // Step EL8: the one-shot rewinds due after this frame.
        if (!host.force_once.empty()) {
            const uint32_t live = host.core->current_tick();
            for (auto it = host.force_once.begin(); it != host.force_once.end();) {
                if (it->first != live) {
                    ++it;
                    continue;
                }
                const auto once = host.core->force_rewind(it->second);
                logger_->Info("Physics session: one-shot forced rewind at tick %u, %u deep (anchor %u): %s", live, it->second,
                              live - it->second, once.rolled ? "rolled" : "not rolled");
                it = host.force_once.erase(it);
            }
        }
        if (!host.force_every) return;
        // The cadence is the verb's; whether the rewind can run this early
        // is the core's (force_rewind does nothing before depth + 2 ticks).
        const uint32_t now = host.core->current_tick();
        if (now % host.force_every != 0) return;
        const auto forced = host.core->force_rewind(host.force_depth);
        if (forced.rolled && host.forced++ < 16)
            logger_->Info("Physics session: forced rewind at tick %u, %u deep", now, host.force_depth);
    };
    // 'session drop-birth' (testing): an armed drop keeps the next own
    // Physicalize off the wire; every other event goes out.
    p.hooks.filter_event = [this](const bmmo::session_event_msg& event) {
        if (host.drop_births_armed <= 0 || event.type != bmmo::session::event_type::Physicalize)
            return bs::event_send_verdict::send;
        --host.drop_births_armed;
        ++host.births_dropped;
        logger_->Info("Automation: own Physicalize stamped %u kept off the wire (session drop-birth)", event.tick);
        return bs::event_send_verdict::drop;
    };
    // The own body guard lifted after the journal closed (design 2.2
    // hooks.ended).
    p.hooks.ended = [this]() {
        std::string error;
        trafo_teardown(physics_view_, m_bml->GetCKContext(), logger_);   // step T4
        if (!host.body_guard) return;
        physics_view_.set_body_guard(false, nullptr, error);
        host.body_guard = false;
        host.body_guard_entity.clear();
    };
    return p;
}

// ---------------------------------------------------------------- the verbs

// session core on|off|status, session force-rollback E:D.
std::string BallanceMMOClient::core_host_session_verb(const std::string& rest) {
    std::istringstream args(rest);
    std::string what, value;
    args >> what >> value;
    if (what == "core") {
        if (value == "on" || value == "off") {
            const bool on = value == "on";
            // The switch takes effect between sessions only: not while the
            // legacy path or the core runs one.  A session latched to the
            // mailbox that no one runs (its SessionStart came and the host
            // gave it up - not in the level, no physics view, no core - and
            // its SessionEnd not yet, which a member that left the room may
            // never get) does not hold the switch: the latch stays, so that
            // session's remaining messages are still dropped, and the next
            // SessionStart reads the switch again.  A lost connection ends
            // the latch.
            if (physics_session_.phase != bs::physics_session_state::phase_type::idle)
                return "error the switch takes effect between sessions: end this one first";
            if (host.phase != retail_host::phase_type::idle
                    || (host.core && host.core->session_phase() != bs::client_core_state::phase::idle))
                return "error the switch takes effect between sessions: the core runs one";
            if (!connected()) core_latched.store(false);
            core_on.store(on);
            if (!on) return "ok session core off: the next session runs on the legacy path";
            return "ok session core on: the next session runs on the shared core (step R5: snapshot rollback, the "
                   "input and lifecycle rewinds, the own echoes, the own corrections, the lead slew)";
        }
        if (!value.empty() && value != "status") return "error usage: session core on|off|status";
        return std::format("ok core={} latched={} made={} error='{}' sessions={} host_phase={} frames={} refused={} "
                           "backstops={} births_dropped={} unmatched_resyncs={} mailbox: pushed={} "
                           "dropped={} drained={} starts={} discarded={} undecodable={} beams_made={} nav_source_differs={} "
                           "mute_source_differs={}",
                           core_on.load() ? "on" : "off", core_latched.load() ? 1 : 0, host.core ? 1 : 0,
                           host.core_error, host.sessions_seen, host_phase_name(host.phase), host.frames, host.refused,
                           host.core ? host.core->own_body_backstops() : 0, host.births_dropped,
                           host.core ? host.core->unmatched_resyncs() : 0,
                           mailbox.pushed(), mailbox.dropped(), host.drained,
                           host.starts, host.discarded, host.undecodable, host.beams_made, host.nav_source_differs,
                           host.mute_source_differs);
    }
    if (what == "reload") {
        // Step SP9: the countdown's reload of a level that ran a session (on
        // by default); off is a testing tool that reproduces the gap.
        if (value == "on" || value == "off") level_life.enabled = value == "on";
        else if (value == "delay") {
            long ms = -1;
            args >> ms;
            if (ms < 0 || ms > 20000) return "error usage: session reload delay <ms> (0..20000)";
            level_life.delay_ms = ms;
        } else if (!value.empty() && value != "status") return "error usage: session reload on|off|delay <ms>|status";
        return std::format("ok reload={} delay_ms={} level_ran_union={} reloading={} loaded={} go_waiting={} reloads={} "
                           "clean_restarts={} abandoned={}", level_life.enabled ? "on" : "off", level_life.delay_ms,
                           level_life.ran_union ? 1 : 0, level_life.reloading ? 1 : 0, level_life.loaded ? 1 : 0,
                           level_life.go_waiting ? 1 : 0, level_life.reloads, level_life.clean_restarts,
                           level_life.abandoned);
    }
    if (what == "placement") {
        // Step SP10: this process's sector placement (status only).
        if (!value.empty() && value != "status") return "error usage: session placement [status]";
        return std::format("ok placement active={} copies={} signature={:016x} anchors={} refused={} releases={} "
                           "released={}", placement_host.active ? 1 : 0, placement_host.placement.copies().size(),
                           placement_host.signature, placement_host.anchors, placement_host.refused,
                           placement_host.releases, placement_host.released);
    }
    if (what == "probes") {
        // Step WG4: this process's proximity union (status only).
        if (!value.empty() && value != "status") return "error usage: session probes [status]";
        return std::format("ok probes active={} blocks={} identities={} signature={:016x} anchors={} refused={} "
                           "releases={} restored={} skipped={} relatched={}", proximity_host.active ? 1 : 0,
                           proximity_host.union_.probe_count(), proximity_host.union_.identity_count(),
                           proximity_host.signature, proximity_host.anchors, proximity_host.refused,
                           proximity_host.releases, proximity_host.restored, proximity_host.union_.skipped(),
                           proximity_host.union_.relatched());
    }
    if (what == "force-rollback" && value == "at") {
        // Step EL8: 'session force-rollback at L:D[,L:D...]', one-shot.
        std::string items;
        args >> items;
        std::istringstream list(items);
        std::string item;
        std::vector<std::pair<uint32_t, uint32_t>> parsed;
        while (std::getline(list, item, ',')) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) return "error usage: session force-rollback at <live>:<depth>[,<live>:<depth>...]";
            const long live = std::strtol(item.c_str(), nullptr, 10);
            const long depth = std::strtol(item.c_str() + colon + 1, nullptr, 10);
            if (live < 1 || depth < 1 || depth > 1000)
                return "error usage: session force-rollback at <live>:<depth>[,<live>:<depth>...]";
            parsed.emplace_back(static_cast<uint32_t>(live), static_cast<uint32_t>(depth));
        }
        for (const auto& once: parsed) host.force_once.push_back(once);
        return std::format("ok {} one-shot forced rewind(s) armed ({} pending)", parsed.size(), host.force_once.size());
    }
    if (what == "activate") {
        // Step RT2: 'session activate <root script> at b+N|<tick>' (the
        // after_rewinds hook), armed before or during a core session.  Core
        // off: refused (only a core session fires or clears it).
        if (!core_on.load()) return "error session activate needs the session core on";
        std::string at, when;
        args >> at >> when;
        const bool relative = when.rfind("b+", 0) == 0;
        const char* digits = when.c_str() + (relative ? 2 : 0);
        char* end = nullptr;
        const unsigned long tick = std::strtoul(digits, &end, 10);
        if (value.empty() || at != "at" || !*digits || (end && *end) || tick > 1000000)
            return "error usage: session activate <root script> at b+<ticks>|<tick>";
        host.activate_script = value;
        host.activate_relative = relative;
        host.activate_at = static_cast<uint32_t>(tick);
        host.activate_armed = true;
        return std::format("ok {} armed for activation after tick {}{}", value, relative ? "base + " : "", tick);
    }
    if (what == "force-rollback") {
        const size_t colon = value.find(':');
        const long every = std::strtol(value.c_str(), nullptr, 10);
        const long depth = colon == std::string::npos ? 12 : std::strtol(value.c_str() + colon + 1, nullptr, 10);
        if (value.empty() || every < 0 || depth < 1 || depth > 1000)
            return "error usage: session force-rollback <every>:<depth> (0: off)";
        host.force_every = static_cast<uint32_t>(every);
        host.force_depth = static_cast<uint32_t>(depth);
        if (!host.force_every) return "ok forced rollbacks off";
        return std::format("ok a forced rewind {} ticks deep every {} ticks{}", host.force_depth, host.force_every,
                           core_on.load() ? "" : " (only with session core on: the legacy path has no forced rollback)");
    }
    if (what == "stall") {
        // A testing tool (R1 (c)): the game thread stands still for <ms>, so
        // the tick driver falls behind its schedule; past 33 ticks it
        // rebases, which a session answers with a resync (either path).
        const long ms = std::strtol(value.c_str(), nullptr, 10);
        if (ms <= 0 || ms > 10000) return "error usage: session stall <ms> (1..10000)";
        const uint32_t before = session_tick_now(physics_session_);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        logger_->Info("Automation: the game thread stalled %ld ms at tick %u", ms, before);
        return std::format("ok stalled {} ms at tick {}", ms, before);
    }
    if (what == "drop-birth") {
        // A testing tool (R1 (c)): the next own Physicalize of this core
        // session never reaches the server (hooks.filter_event).
        if (!core_session_running()) return "error session drop-birth: no session core runs";
        ++host.drop_births_armed;
        return std::format("ok the next own Physicalize is kept off the wire (armed {}, dropped so far {})",
                           host.drop_births_armed, host.births_dropped);
    }
    return "error usage: session core on|off|status | session force-rollback <every>:<depth> | "
           "session force-rollback at <live>:<depth>[,...] | session stall <ms> | "
           "session drop-birth | session placement | session probes | session activate <script> at b+<n>|<tick>";
}

// net lag <ms> [up|down] [at <tick>] | net blackout <ms> [up|down] [at <tick>]
// | net: GNS's fake lag (one way, in one or both directions) and a loss of
// every packet in one or both directions for a while (the retail counterpart
// of the headless BMMO_FAKE_LAG_MS).  'at <tick>' arms it for the first frame
// boundary at or after that session tick (core_host_process).  A blackout
// ends at a frame boundary, so an anchor hold in progress stretches it, and
// one as long as GNS's connected timeout drops the connection.
std::string BallanceMMOClient::core_host_net_verb(const std::string& rest) {
    std::istringstream args(rest);
    std::string what;
    long ms = -1;
    args >> what >> ms;
    std::string direction, word;
    long at = -1;
    while (args >> word) {
        if (word == "at") {
            if (!(args >> at) || at < 0) return "error usage: ... at <tick>";
        } else if (direction.empty()) {
            direction = word;
        } else {
            return "error usage: net lag|blackout <ms> [up|down] [at <tick>]";
        }
    }
    const bool up = direction.empty() || direction == "up" || direction == "both";
    const bool down = direction.empty() || direction == "down" || direction == "both";
    auto* utils = SteamNetworkingUtils();
    if (what == "lag" || what == "blackout") {
        const bool lag = what == "lag";
        if (lag ? ms < 0 || ms > 10000 : ms <= 0 || ms > 60000)
            return "error usage: net lag <ms> [up|down] [at <tick>] (one way; 0: none) | net blackout <ms> [up|down] "
                   "[at <tick>]";
        if (!up && !down) return "error usage: net " + what + " <ms> [up|down] [at <tick>]";
        if (at >= 0) {
            host.net_pending.push_back({lag, ms, up, down, static_cast<uint32_t>(at)});
            return std::format("ok net {} {} ms ({}) armed for tick {} (now {})", what, ms, join_flags(up, down), at,
                               session_tick_now(physics_session_));
        }
    }
    if (what == "lag") {
        const int32_t value = static_cast<int32_t>(ms);
        bool ok = true;
        if (up) ok = utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, value) && ok;
        if (down) ok = utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, value) && ok;
        if (!ok) return "error GNS refused the fake lag";
        if (up) host.lag_up_ms = value;
        if (down) host.lag_down_ms = value;
        const uint32_t tick = session_tick_now(physics_session_);
        logger_->Info("Automation: net lag %d ms (%s) from tick %u", value, join_flags(up, down).c_str(), tick);
        return std::format("ok net lag {} ms ({}) from tick {}: up {} ms, down {} ms", value, join_flags(up, down), tick,
                           host.lag_up_ms, host.lag_down_ms);
    }
    if (what == "blackout") {
        bool ok = true;
        if (up) ok = utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0f) && ok;
        if (down) ok = utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 100.0f) && ok;
        if (!ok) return "error GNS refused the fake loss";
        const auto until = clock_type::now() + std::chrono::milliseconds(ms);
        if (up) {
            host.blackout_up = true;
            host.blackout_up_until = until;
        }
        if (down) {
            host.blackout_down = true;
            host.blackout_down_until = until;
        }
        // GNS's connected timeout (role.hpp sets 7.2 s): a blackout that
        // long drops the connection, not just the link.
        int32_t timeout = 0;
        size_t size = sizeof(timeout);
        ESteamNetworkingConfigDataType type{};
        std::string warning;
        if (utils->GetConfigValue(k_ESteamNetworkingConfig_TimeoutConnected, k_ESteamNetworkingConfig_Global, 0, &type,
                                  &timeout, &size) == k_ESteamNetworkingGetConfigValue_OK
                && timeout > 0 && ms >= timeout)
            warning = std::format(" - at least GNS's connected timeout ({} ms): the connection will drop", timeout);
        const uint32_t tick = session_tick_now(physics_session_);
        logger_->Info("Automation: net blackout %ld ms (%s) from tick %u%s", ms, join_flags(up, down).c_str(), tick,
                      warning.c_str());
        return std::format("ok net blackout {} ms ({}) from tick {}{}", ms, join_flags(up, down), tick, warning);
    }
    if (what.empty() || what == "status") {
        return std::format("ok net lag up={} down={} blackout={} armed={}", host.lag_up_ms, host.lag_down_ms,
                           join_flags(host.blackout_up, host.blackout_down), host.net_pending.size());
    }
    return "error usage: net lag <ms> [up|down] [at <tick>] | net blackout <ms> [up|down] [at <tick>] | net status";
}

// replay session <file> | replay session stop | replay session: arm a key
// record for the next (or this) session.  Frame k is fed to the input poll of
// tick first + k, `first` being the first assigned frame's tick - the frame
// after the anchor hold - with no clock reset and nothing recorded (design
// 3.2; the legacy `replay` anchors at the first Gameplay_Ingame frame after
// the verb and resets the clock).
std::string BallanceMMOClient::core_host_replay_verb(const std::string& rest) {
    auto& r = host.replay;
    if (rest.empty() || rest == "status")
        return std::format("ok replay session armed={} started={} first_tick={} fed={} frames={} last_frame={} jumps={} "
                           "path={}",
                           r.armed ? 1 : 0, r.started ? 1 : 0, r.first_tick, r.fed, r.source.frames.size(), r.last_frame,
                           r.jumps, r.path.empty() ? "-" : r.path);
    if (rest == "stop") {
        const bool was = r.armed;
        r.armed = false;
        return std::format("ok replay session {} after {} frames", was ? "stopped" : "was not armed", r.fed);
    }
    std::string error;
    bmmo::physics::tick_record source;
    if (!source.load(rest, error)) return "error " + error;
    if (source.frames.empty()) return "error the record has no frames: " + rest;
    r = {};
    r.source = std::move(source);
    r.path = rest;
    r.armed = true;
    return std::format("ok replay session armed: {} frames from the first assigned frame", r.source.frames.size());
}

// The input poll (on_input_polled), after the automation keys: the armed key
// record's frame for the tick about to step.  The record ends the feed when
// it runs out, and so does the end of the session it started in.  The feed
// follows the session's tick numbers: a renumbering (a resync) skips record
// frames when it jumps forward and lets live keys through for the ticks
// before the first fed one when it jumps back - each jump is logged.
void BallanceMMOClient::core_host_feed_replay_keys(unsigned char* state) {
    auto& r = host.replay;
    if (!r.armed) return;
    uint32_t tick = 0;
    if (!next_session_tick(physics_session_, tick)) {
        if (r.started && physics_session_.phase != bs::physics_session_state::phase_type::running
                && host.phase != retail_host::phase_type::running) {
            r.armed = false;
            logger_->Info("Automation: replay session: the session ended after %llu frames",
                          static_cast<unsigned long long>(r.fed));
        }
        return;
    }
    if (!r.started) {
        r.started = true;
        r.first_tick = tick;
        logger_->Info("Automation: replay session: frame 0 of %zu fed at tick %u", r.source.frames.size(), tick);
    } else if (r.fed > 0 && tick != r.last_tick && tick != r.last_tick + 1) {
        const int64_t jump = static_cast<int64_t>(tick) - static_cast<int64_t>(r.last_tick) - 1;
        if (r.jumps++ < 16)
            logger_->Info("Automation: replay session: the session tick jumped from %u to %u (%lld record frame(s) %s)",
                          r.last_tick, tick, static_cast<long long>(jump < 0 ? -jump : jump),
                          jump < 0 ? "fed again, or live keys before the first" : "skipped");
    }
    r.last_tick = tick;
    const int64_t k = static_cast<int32_t>(tick - r.first_tick);
    if (k < 0) return;
    if (k >= static_cast<int64_t>(r.source.frames.size())) {
        r.armed = false;
        logger_->Info("Automation: replay session: the record ended at tick %u after %llu frames", tick,
                      static_cast<unsigned long long>(r.fed));
        return;
    }
    const auto& keys = r.source.frames[static_cast<size_t>(k)].keys;
    std::memcpy(state, keys.data(), keys.size());
    r.last_frame = k;
    ++r.fed;
}

// ---------------------------------------------------------------- hooks (R1)

// OnPhysicalize, while a core session runs (the legacy OnPhysicalize, with the
// own report going to the core, design 3.2 "Own capture"): every body's birth
// into the birth log (bind_session_world's), and for the current ball the
// spawn flag (the pose at the level's resetpoint), the birth cell's offset
// written into the entity before the body is made, the recipe from the
// block's arguments, core.own_physicalized (queued; committed at post_step
// step 19), and the spawn impulse queued for this frame's PreSimulate.
bool BallanceMMOClient::core_host_on_physicalize(CK3dEntity* target, CKBOOL fixed, float friction, float elasticity,
                                                 float mass, const char* collGroup, CKBOOL startFrozen,
                                                 CKBOOL enableColl, CKBOOL calcMassCenter, float linearDamp,
                                                 float rotDamp, const char* collSurface, VxVector massCenter,
                                                 int convexCnt, CKMesh** convexMesh, int ballCnt, VxVector* ballCenter,
                                                 float* ballRadius, int concaveCnt, CKMesh** concaveMesh) {
    if (!core_session_running() || !target) return false;
    auto& core = *host.core;
    const uint32_t tick = core.current_tick() + 1;   // this frame's physics step is still ahead
    bs::ball_recipe r;
    r.fixed = fixed != 0;
    r.friction = friction;
    r.elasticity = elasticity;
    r.mass = mass;
    r.start_frozen = startFrozen != 0;
    r.enable_collision = enableColl != 0;
    r.calc_mass_center = calcMassCenter != 0;
    r.linear_damp = linearDamp;
    r.rot_damp = rotDamp;
    r.mass_center[0] = massCenter.x; r.mass_center[1] = massCenter.y; r.mass_center[2] = massCenter.z;
    r.collision_surface = collSurface ? collSurface : "";
    for (int i = 0; i < convexCnt && convexMesh; ++i)
        r.convex_meshes.push_back(convexMesh[i] && convexMesh[i]->GetName() ? convexMesh[i]->GetName() : "");
    for (int i = 0; i < ballCnt; ++i) {
        bs::ball_recipe::sphere sphere;
        if (ballCenter) { sphere.center[0] = ballCenter[i].x; sphere.center[1] = ballCenter[i].y; sphere.center[2] = ballCenter[i].z; }
        sphere.radius = ballRadius ? ballRadius[i] : 0.0f;
        r.balls.push_back(sphere);
    }
    for (int i = 0; i < concaveCnt && concaveMesh; ++i)
        r.concave_meshes.push_back(concaveMesh[i] && concaveMesh[i]->GetName() ? concaveMesh[i]->GetName() : "");
    const std::string name = target->GetName() ? target->GetName() : "";
    // Step T4: inside a replayed tick or a reconcile's remake (the trafo as
    // an input), a level object's birth goes under that tick - a replayed
    // one only when the log lacks it there (trafo_host.birth_tick).
    uint32_t birth_at = tick;
    const auto record_birth = [&] {
        bs::local_birth birth;
        birth.name = name;
        birth.group = collGroup ? collGroup : "";
        birth.recipe = to_bridge_recipe(r);
        const VxMatrix& m = target->GetWorldMatrix();
        const VxMatrix& l = target->GetLocalMatrix();
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) {
                birth.matrix[row * 4 + k] = m[row][k];
                birth.local_matrix[row * 4 + k] = l[row][k];
            }
        birth.has_parent = target->GetParent() != nullptr;
        birth.has_local = true;
        host.births.put(birth_at, std::move(birth));
    };
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) {
        // A level object's retail Physicalize during the session.
        if (trafo_host.birth_tick) {
            birth_at = trafo_host.birth_tick;
            if (trafo_host.if_absent) {
                if (core.is_ball(name) || host.births.find(name, birth_at)) return true;
                if (trafo_host.replay_birth_logs++ < 8)
                    logger_->Info("Physics session: birth log: %s made by the replayed tick %u", name.c_str(), birth_at);
            }
        }
        record_birth();
        if (core.config.trace)
            logger_->Info("Physics session: retail physicalize of %s at tick %u (%d convex, %d balls, %d concave)",
                          name.c_str(), tick, convexCnt, ballCnt, concaveCnt);
        return true;
    }
    bs::own_birth_report report;
    report.entity = name;
    report.tick = tick;
    report.ball_type = ball_row_of(m_bml, name);
    report.recipe = r;
    report.pose.captured = true;   // taken before the body exists (design 2.3 host_hooks)
    const VxMatrix& world = target->GetWorldMatrix();
    for (int k = 0; k < 3; ++k) report.pose.position[k] = world[3][k];
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 3; ++k) report.pose.rotation[row * 3 + k] = world[row][k];
    // Design 9.10: a Physicalize at the level's current resetpoint is a spawn
    // or a respawn, flagged so every side applies the same impulse.
    VxMatrix reset;
    bool have_reset = false;
    if (CKDataArray* level = current_level_array()) {
        if (level->GetElementValue(0, 3, &reset)) {
            have_reset = true;
            const float dx = report.pose.position[0] - reset[3][0];
            const float dy = report.pose.position[1] - reset[3][1];
            const float dz = report.pose.position[2] - reset[3][2];
            report.pose.spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
        }
    }
    const uint8_t join_order = static_cast<uint8_t>(core.own_join_order() < 0 ? 63 : core.own_join_order());
    // The birth cell (session/spawn_impulse.hpp): the entity moved off the
    // resetpoint before the Physicalize block makes the body from its matrix.
    if (report.pose.spawn && core.spawn_spacing() > 0.0f && have_reset && join_order % bs::kSpawnOffsets != 0) {
        const float right[3] = {reset[0][0], reset[0][1], reset[0][2]};
        const float dir[3] = {reset[2][0], reset[2][1], reset[2][2]};
        float offset[3];
        bs::spawn_offset(join_order, core.spawn_spacing(), right, dir, offset);
        VxMatrix moved = world;
        for (int k = 0; k < 3; ++k) moved[3][k] += offset[k];
        target->SetWorldMatrix(moved);
        for (int k = 0; k < 3; ++k) report.pose.position[k] = moved[3][k];
        logger_->Info("Physics session: spawn offset (%.3f,%.3f,%.3f) for join order %u at tick %u",
                      static_cast<double>(offset[0]), static_cast<double>(offset[1]), static_cast<double>(offset[2]),
                      join_order, tick);
    }
    record_birth();   // after the offset: the matrix the body is made from
    // (Step SPGH2: own_physicalized also begins the own spawn ghost life
    // when the core counts the birth a spawn - before the block makes the
    // body, as the server's copy's does at this tick.)
    core.own_physicalized(report);
    host.own_group_set = false;
    // The PreSimulate pass's anchor: a live behavior (the Ball Navigation
    // script).
    uint32_t behavior_id = host.navigation.ball_navigation;
    if (behavior_id == 0) behavior_id = bmmo::game::read_navigation_graph(m_bml->GetCKContext()).ball_navigation;
    // Step OWG: the body in the player's group before this tick's PSIs, as
    // the server's clone is from its Physicalize (the block makes it in the
    // script's group; upkeep at post_step step 17 is after the PSIs), its
    // pairs re-asked: queued into this frame's PreSimulate pass ahead of the
    // spawn impulse, as the headless host does it in capture_own_physicalize.
    {
        std::string error;
        const int queued = physics_view_.own_group_queue(name.c_str(), core.own_group().c_str(), behavior_id, error);
        if (queued == 0)
            logger_->Warn("Physics session: own ball group at tick %u: %s", tick, error.c_str());
        else if (queued == 1)
            host.own_group_set = true;
    }
    if (report.pose.spawn && core.spawn_impulse() > 0.0f) {
        // Queued into this frame's PreSimulate pass as well.
        const uint32_t index = bs::spawn_direction_index(core.seed(), join_order, tick);
        std::string error;
        if (physics_view_.push_impulse(name.c_str(), bs::kSpawnDirectionTable[index], core.spawn_impulse(), behavior_id, error))
            logger_->Info("Physics session: spawn impulse index=%u speed=%.3f at tick %u", index, core.spawn_impulse(), tick);
        else
            logger_->Warn("Physics session: spawn impulse: %s", error.c_str());
    }
    // The replica drives the ball, the retail leaves push zero (design 9.6).
    if (host.own_replica.active) core_host_zero_retail_forces();
    logger_->Info("Physics session: own ball %s physicalized at tick %u (type %d%s, %d convex, %d balls, %d concave)",
                  name.c_str(), tick, report.ball_type, report.pose.spawn ? ", spawn" : "", convexCnt, ballCnt, concaveCnt);
    return true;
}

// OnUnphysicalize of the current ball: core.own_unphysicalized (queued).
bool BallanceMMOClient::core_host_on_unphysicalize(CK3dEntity* target) {
    if (!core_session_running() || !target) return false;
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) return true;
    const uint32_t tick = host.core->current_tick() + 1;
    host.core->own_unphysicalized(tick);
    host.own_group_set = false;
    logger_->Info("Physics session: own ball unphysicalized at tick %u", tick);
    return true;
}

// A sector change of this ball (OnPostCheckpointReached and the other
// reports): a crossed checkpoint with the chain's manager run held (N3) is an
// input, core.request_sector, once assigned; otherwise it is reported after
// the fact at once, as the legacy hook does (design 2.2 "request_sector").
bool BallanceMMOClient::core_host_on_sector(int sector, bool checkpoint) {
    if (!core_session_running()) return false;
    auto& core = *host.core;
    if (checkpoint && core.assigned() && sector_activate_.block && !sector_activate_failed_) {
        core.request_sector(sector, "checkpoint");
        return true;
    }
    bmmo::session_event_msg event;
    event.tick = core.current_tick();
    event.type = bmmo::session::event_type::Sector;
    event.sector = sector;
    core.send_event(event);
    return true;
}

bool BallanceMMOClient::core_host_on_finish() {
    if (!core_session_running()) return false;
    bmmo::session_event_msg event;
    event.tick = host.core->current_tick();
    event.type = bmmo::session::event_type::Finish;
    host.core->send_event(event);
    return true;
}

// PlayerLeft (room_client.cpp): a session the core runs takes the member's
// PLAYER(-) from its SessionMember(removed), at the server's R (step M1), so
// the room event writes nothing; the legacy journal is not this session's.
bool BallanceMMOClient::core_host_leave_player(uint32_t player) {
    (void) player;
    return host.phase != retail_host::phase_type::idle && host.core;
}

// The automation 'beam': core.request_beam, made in place at post_step step
// 29 of this frame and stamped T + 1 (design 2.3, 4.2 R1).
bool BallanceMMOClient::core_host_beam(const double position[3], std::string& answer, int64_t at) {
    if (!core_session_running()) return false;
    auto& core = *host.core;
    // Unassigned (the hold timed out, or a resync's assignment is awaited):
    // no tick to stamp the beam with.  Not the legacy verb either - its
    // in-place move would be one the core and the server never learn of.
    if (!core.assigned()) {
        answer = "error session core: not assigned (no tick base to stamp the beam with)";
        return true;
    }
    if (at >= 0) {
        // 'at <tick>': the earliest stamp a request can still get is the
        // next post_step's T + 1 = current + 2 (core_host_due_beams).
        if (at > static_cast<int64_t>(UINT32_MAX)) {
            answer = std::format("error beam at {}: not a session tick", at);
            return true;
        }
        if (at < static_cast<int64_t>(core.current_tick()) + 2) {
            answer = std::format("error beam at {}: too late (tick {} now, the earliest stamp is {})", at,
                                 core.current_tick(), core.current_tick() + 2);
            return true;
        }
        retail_host::pending_beam beam;
        beam.stamp = static_cast<uint32_t>(at);
        for (int k = 0; k < 3; ++k) beam.position[k] = position[k];
        host.beams_pending.push_back(beam);
        answer = std::format("ok beam to ({:.3f},{:.3f},{:.3f}) armed for tick {} (now {}; session core)", position[0],
                             position[1], position[2], at, core.current_tick());
        return true;
    }
    core.request_beam(position);
    CK3dObject* ball = get_current_ball();
    answer = std::format("ok beam of {} to ({:.3f},{:.3f},{:.3f}) requested at tick {}: made after this frame's step, "
                         "stamped {} (session core)", ball && ball->GetName() ? ball->GetName() : "-", position[0],
                         position[1], position[2], core.current_tick(), core.current_tick() + 1);
    return true;
}

// The armed 'beam ... at <tick>' requests whose stamp the coming post_step
// makes: post_step(T) stamps a request T + 1, and T is current + 1 here, so a
// beam armed for current + 2 (or one whose tick a renumbering skipped) goes
// now, the lowest armed tick first.  One at a time: the core holds one
// request, and a refused post_step keeps it there for a later frame, so the
// next one waits until core_host_frame has logged the stamp the core gave
// it.  (A plain 'beam' in the same frame still replaces the request: one
// request, as the core has it.)
void BallanceMMOClient::core_host_due_beams() {
    if (host.beams_pending.empty() || host.beam_in_flight || !host.core || !host.core->assigned()) return;
    auto& core = *host.core;
    const uint32_t stamp = core.current_tick() + 2;
    auto due = host.beams_pending.end();
    for (auto it = host.beams_pending.begin(); it != host.beams_pending.end(); ++it) {
        if (static_cast<int32_t>(stamp - it->stamp) < 0) continue;
        if (due == host.beams_pending.end() || static_cast<int32_t>(it->stamp - due->stamp) < 0) due = it;
    }
    if (due == host.beams_pending.end()) return;
    core.request_beam(due->position);
    ++host.beams_made;
    host.beam_in_flight = true;
    host.beam_in_flight_armed = due->stamp;
    logger_->Info("Automation: beam to (%.3f,%.3f,%.3f) armed for tick %u requested before post_step(%u)",
                  due->position[0], due->position[1], due->position[2], due->stamp, stamp - 1);
    host.beams_pending.erase(due);
}

// The automation 'sector N': core.request_sector while the checkpoint hold is
// in force and the session assigned (the legacy verb's condition).  Without
// the hold the legacy verb activates the sector by hand (the hook then
// reports it after the fact, as with the switch off); unassigned, nothing:
// an activation by hand would be one no replay records.
bool BallanceMMOClient::core_host_sector(int sector, std::string& answer) {
    if (!core_session_running()) return false;
    auto& core = *host.core;
    if (!core.assigned()) {
        answer = "error session core: not assigned (no tick to hold the sector for)";
        return true;
    }
    if (!sector_activate_.block || sector_activate_failed_) return false;
    const uint32_t due = core.current_tick() + 1 + core.sector_lead_ticks();
    core.request_sector(sector, "automation");
    answer = std::format("ok sector {} held for tick {} (Sector event sent ahead; session core)", sector, due);
    return true;
}

// 'journal off|mark' with a core session: the core's journal (design 1.2).
bool BallanceMMOClient::core_host_journal(const std::string& action, const std::string& text, std::string& answer) {
    if (host.phase == retail_host::phase_type::idle || !host.core) return false;
    auto& journal = host.core->journal();
    if (action == "mark") {
        if (text.empty()) { answer = "error usage: journal mark <text>"; return true; }
        if (!journal.journalling()) { answer = "error no session journal is open"; return true; }
        const uint32_t tick = host.core->current_tick();
        journal.note("mark: " + text);
        journal.local_checkpoint(tick, host.core->own_body_entity());
        answer = std::format("ok mark at tick {}: {} (session core)", tick, text);
        return true;
    }
    if (action == "off") {
        config_manager_["session_journal"]->SetBoolean(false);
        const bool was = journal.journalling();
        journal.close("recording turned off");
        answer = was ? "ok journal off (SessionJournal=false), closed the session core's journal"
                     : "ok journal off (SessionJournal=false), nothing was being recorded";
        return true;
    }
    return false;
}

// The 'session' verb and the /mmo session line while a core session runs:
// the host's phase and the fields the retail tools read first, then the
// core's status line (design 1.3 status_text).
std::string BallanceMMOClient::core_host_status_text() {
    if (!host.core) return std::format("session=0 phase={} core=1", host_phase_name(host.phase));
    auto& core = *host.core;
    bs::host_counters h;
    h.rebases = static_cast<int>(fixed_tick_.rebases());
    h.activations_replayed = host.activations_replayed;
    h.script_reset_skips = host.script_reset_skips;
    h.probes_skipped = proximity_host.union_.skipped();     // step WG4
    h.probes_relatched = proximity_host.union_.relatched();
    h.unrecorded_array_reads = array_reads_host.unrecorded;   // step RK2
    h.written_cell_columns = written_columns_host.size();   // the binding's (step RK2's review)
    // Gap Q (step B7EG): the PreSimulate queue's part of the records and
    // of every replay: rebuilt/removed/held/unhold_clamped/rebuild_fail/
    // hold_fail/no_record.
    // Step RTX: the two queue counters the headless 'cbq:' line has and
    // cbq= left out - entries captured into records, holds run (one per
    // rewind's replay, whether it found outside entries or none) - as
    // cbqx=captured/holds, and
    // RC4's dormant-script counters (step B6RC; the headless
    // 'trafo_dormancy:' line) as dorm=from_ledger/healed/walked/
    // guard_trips/restored_built.  Process-wide diagnostics, appended so
    // the fields before them keep their places.
    const auto& q = bmmo::game::script_replay::queue_stats();
    const auto& d = bmmo::game::script_replay::dormancy();
    return std::format("session={} phase={} core=1 tick={} assigned={} held={} keys_known={} own_phys={} "
                       "backstops={} refused={} unmatched_resyncs={} {} cbq={}/{}/{}/{}/{}/{}/{} cbqx={}/{} "
                       "dorm={}/{}/{}/{}/{}",
                       core.session_id(), host_phase_name(host.phase), core.current_tick(), core.assigned() ? 1 : 0,
                       core.holding() ? 1 : 0, host.navigation_known ? 1 : 0, core.own_physicalized() ? 1 : 0,
                       core.own_body_backstops(), host.refused, core.unmatched_resyncs(), core.status_text(h),
                       q.rebuilt, q.removed, q.held, q.unhold_clamped, q.rebuild_fail, q.hold_fail, q.no_record,
                       q.captured, q.holds, d.from_ledger, d.healed, d.walked, d.guard_trips, d.restored_built);
}

// The F3 panel's physics block for a core session.
std::string BallanceMMOClient::core_host_overlay_text() {
    std::string text = std::format("\nPhysics session {} ({}, core)\n", host.session, host_phase_name(host.phase));
    if (!core_session_running()) return text;
    auto& core = *host.core;
    const auto& rs = core.rollback_stats();
    text += std::format("Tick: {}\n", core.current_tick());
    text += std::format("Snapshots: {} ok, {} corrected\n", rs.matched, rs.mismatched);
    if (rs.snapshots)
        text += std::format("Rollbacks: {} ({:.0f}%), {} ticks resim\n", rs.rollbacks,
                            100.0 * static_cast<double>(rs.mismatched) / static_cast<double>(rs.snapshots), rs.resim_ticks);
    text += std::format("Max error: {:.3f} m\n", rs.max_error);
    if (rs.too_far || rs.frozen || core.resyncs_sent())
        text += std::format("Too far: {}  Frozen: {}  Resyncs: {}\n", rs.too_far, rs.frozen, core.resyncs_sent());
    if (!core.remotes().empty()) text += std::format("Remote balls: {}\n", core.remotes().size());
    return text;
}
