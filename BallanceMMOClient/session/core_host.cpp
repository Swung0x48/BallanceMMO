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
#include "host_clock.hpp"
#include "retail_capture.hpp"

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
#include <game/checkpoint_phase.hpp>
#include <game/depth_test.hpp>
#include <game/force_block_alias.hpp>
#include <game/host_backend.hpp>
#include <game/hull_template.hpp>
#include <game/mirror_clones.hpp>
#include <game/proximity_union.hpp>
#include <game/sector_activation.hpp>
#include <game/sector_cell_split.hpp>
#include <game/sector_deactivate_pin.hpp>
#include <game/sector_placement.hpp>
#include <game/sector_reset_guard.hpp>
#include <game/session_host_support.hpp>
#include <game/trafo_sets.hpp>
#include <game/tutorial_exit.hpp>
#include <session/bridge_recipe.hpp>
#include <session/client_core.hpp>
#include <session/session_clock.hpp>
#include <session/event_hold.hpp>
#include <session/spawn_impulse.hpp>

#include <BML/Bui.h>   // FULL step AHN: BML+'s ImGui context
#include "hold_panel.hpp"
#include <session/client_platform.hpp>
#include <session/io_pool.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <session/session_host_world.hpp>

#include "core_host.hpp"
#include "journal_files.hpp"
#include "menu_keys.hpp"
#include "menu_overlay.hpp"
#include "own_wake_guard.hpp"
#include "pause_chain.hpp"

namespace {
    // Step B2SX (game/sector_cell_split.hpp): the checkpoint chain's own
    // IngameParameter[0][1] while the session core runs (the legacy path
    // keeps the cell it polls).  File scope: BallanceMMOClient keeps its
    // layout.
    bmmo::game::sector_cell_split core_chain_cell;
    // FULL step N3K (host-platform audit D3; AUD-2): N3, the checkpoint's
    // run of Gameplay_SectorManager held, by the shared helper both session
    // clients use (game/sector_activation.hpp checkpoint_activation_hold):
    // made at the core anchor, kept every frame (platform script_guards),
    // handed back at the session's end.  It replaced the BallanceMMOClient
    // members sector_activate_ / sector_activate_failed_ (FULL step LGD
    // deleted them with the legacy path).  File scope: BallanceMMOClient
    // takes no data member.
    bmmo::game::checkpoint_activation_hold core_checkpoint_hold;
    // FULL step SLG: the own sectors core_host_report_own_sector has
    // reported in this core session (each once, as the core stamps each
    // once); cleared by reset_session_state at a session's start and end
    // (a restarted server hands out session 1 again).  File scope:
    // BallanceMMOClient keeps its layout.
    struct own_sector_reports_state {
        std::set<int> reported;
    } own_sector_reports;
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
    // FULL step ESCX (RK3E, session/menu_overlay.hpp): the ESC menu as an
    // overlay - found at the core session's anchor, kept by the frame guard
    // (the ESC block's Key State held while the menu is open), handed back at
    // the session's end and before the next anchor.  File scope:
    // BallanceMMOClient keeps its layout.
    bmmo::retail::menu_overlay core_menu_overlay;
    // FULL step ESCK (session/menu_keys.hpp): while that menu is open its
    // keys reach the menu only - the level's keyboard-reading blocks read no
    // key (and a key held across the close stays hidden from them until it
    // is let go).  Resolved with the overlay, kept by the same frame guard,
    // handed back at the session's end.  File scope, as above.
    bmmo::retail::menu_key_mask core_menu_keys;
    // FULL step RTDF (decision 65, session/own_wake_guard.hpp): the retail
    // Ball Navigation's shared Physics WakeUp acts on no own ball - the menu's
    // BallNav deactivate woke a resting own ball in its owner's world only.
    // Resolved with the overlay, kept by the same frame guard, handed back at
    // the session's end.  File scope, as above.
    bmmo::retail::own_wake_guard core_own_wake;
    // FULL step LGM (R7 part one): what the core host borrowed from the
    // legacy path's BallanceMMOClient members until then, as its own
    // file-scope state (FULL step LGD deleted the legacy path, its copies and
    // the members named below):
    //   * the pause chain's clock and script edits (session/pause_chain.hpp;
    //     retail's alone - no other host has a pause menu), for
    //     pause_clock_* / pause_scripts_*;
    //   * N1, the retail sector resets of Gameplay_Ingame neutralized, by the
    //     helper the headless client uses (game/sector_reset_guard.hpp), for
    //     death_reset_*; `core_reset_failed`: its resolve was refused;
    //   * N2, the two chains' [0][2] Set Cells pinned to 0, by the shared
    //     helper's chain list (game/sector_deactivate_pin.hpp
    //     pin_checkpoint_deactivation, as the headless client calls it), for
    //     sector_deactivate_*; `core_deactivate_failed`: not both pinned;
    //   * N4, the checkpoint chain's phase cell split
    //     (game/checkpoint_phase.hpp), for physics_session_client.cpp's own
    //     static behind checkpoint_phase_resolve / keep / restore;
    //   * Gameplay_SectorManager's id for the union pacer's per-tick read (the
    //     headless client's sector_manager_active, re-found when the id no
    //     longer names that script), for physics_session_sector_manager_active;
    //   * the level file's Sequencer counters (HU1 (b): game/script_state.hpp
    //     capture_level_file_counters / restore_level_file_counters, as the
    //     server's world and the headless client call them), recorded on the
    //     frames after a map load like the member level_sequencers_ the legacy
    //     anchor reads; the proximity countdowns were this file's already
    //     (level_proximity_state);
    //   * 'session trace on|off' and 'session rollback on|off', read at a core
    //     session's start, for physics_session_.trace / rollback_enabled.
    bmmo::retail::pause_chain core_pause_chain;
    bmmo::game::sector_reset_guard core_reset_guard;
    bool core_reset_failed = false;
    std::vector<bmmo::game::deactivate_pin> core_deactivate_pins;
    bool core_deactivate_failed = false;
    bmmo::game::checkpoint_phase_split core_checkpoint_phase;
    CK_ID core_manager_id = 0;
    bmmo::game::sequencer_state core_level_sequencers;
    int core_level_counter_frames = 0;
    constexpr int kLevelCounterFrames = 30;   // the legacy capture's window (BallanceMMOClient::OnLoadObject)
    bool core_trace = false;
    bool core_rollback = true;

    // ---- FULL step LGM: the host halves of the moved edits (the legacy
    // members' rules and log lines, physics_session_client.cpp) ----

    void core_log_line(logger_wrapper* logger, int severity, const std::string& line) {
        if (severity >= 2) logger->Error("Physics session: %s", line.c_str());
        else if (severity == 1) logger->Warn("Physics session: %s", line.c_str());
        else logger->Info("Physics session: %s", line.c_str());
    }

    // N1 kept: every call site's target emptied (a deferred one only once the
    // sector is up) and kept empty; each newly emptied one logged.
    void core_reset_apply(CKContext* context, logger_wrapper* logger, bool sector_up) {
        for (const std::string& chain: core_reset_guard.apply(context, sector_up)) {
            CK_ID retail = 0;
            for (const auto& site: core_reset_guard.sites())
                if (site.chain == chain) { retail = site.retail; break; }
            logger->Info("Physics session: the sector reset in %s (Execute Script -> %u) is neutralized", chain.c_str(),
                         static_cast<unsigned>(retail));
        }
    }

    // N1 resolved at the anchor (the legacy death_reset_resolve): both call
    // sites or nothing, then the non-deferred one emptied at once.  The
    // legacy resolve's own apply read the legacy session's mechanism count,
    // which is 0 on this path: "the sector is up" is false here too.
    bool core_reset_resolve(CKContext* context, logger_wrapper* logger) {
        const std::string trouble = core_reset_guard.resolve(context);
        if (!trouble.empty()) {
            core_reset_failed = true;
            logger->Error("Physics session: the retail sector resets were NOT neutralized (%s): a death or a level "
                          "reset will reset this client's sector and rebuild the mechanisms' joints under the body "
                          "guard", trouble.c_str());
            return false;
        }
        core_reset_failed = false;
        core_reset_apply(context, logger, false);
        return true;
    }

    void core_reset_restore(CKContext* context) {
        core_reset_guard.restore(context);
        core_reset_failed = false;
    }

    // N2 resolved at the anchor (the legacy sector_deactivate_resolve): both
    // chains' [0][2] Set Cells pinned to 0, in the legacy order (the
    // checkpoint's, then the level reset's), by the shared chain list.
    bool core_deactivate_resolve(CKContext* context, logger_wrapper* logger) {
        std::string report;
        int chains = 0;
        bmmo::game::deactivate_chains(chains);
        const int pinned = bmmo::game::pin_checkpoint_deactivation(context, report, &core_deactivate_pins);
        core_deactivate_failed = pinned < chains;
        if (core_deactivate_failed)
            logger->Error("Physics session: the sector deactivation was NOT pinned in every chain (%s): such a chain "
                          "will reset this client's sector and stop its mechanisms", report.c_str());
        else
            logger->Info("Physics session: the sector deactivation (Set Cell -> IngameParameter[0][2]) is pinned to 0: "
                         "%s", report.c_str());
        return !core_deactivate_failed;
    }

    void core_deactivate_restore(CKContext* context, logger_wrapper* logger) {
        // Destroying a constant an input still reads would leave a dangling
        // source behind: such an input keeps it (unpin_deactivate_cell).
        if (const int left = bmmo::game::unpin_checkpoint_deactivation(context, core_deactivate_pins))
            logger->Warn("Physics session: %d Value input(s) of the sector deactivation could not be handed back to "
                         "their retail source; the session's constant is left in place", left);
        core_deactivate_failed = false;
    }

    // Whether Gameplay_SectorManager is walking right now (the union pacer's
    // read, once per step): the id found once and re-found whenever it no
    // longer names a root script of that name (a level load recycles ids).
    bool core_manager_active(CKContext* context) {
        auto* manager = CKBehavior::Cast(core_manager_id ? context->GetObject(core_manager_id) : nullptr);
        if (!manager || manager->GetParent() || !manager->GetName()
                || std::strcmp(manager->GetName(), "Gameplay_SectorManager") != 0) {
            manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
            core_manager_id = manager ? manager->GetID() : 0;
        }
        return manager && manager->IsActive();
    }
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
    // switched to 53 bits around the reads that feed the journal's rows,
    // including the core's body reads and the rollback records (LBF4),
    // and the caller's control word is restored on scope exit.  The x64
    // and Linux hosts run SSE2 doubles (53-bit) already.
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

    // ------------------------------------------------------------ the latch
    // FULL step LGD (R7): the 'session core on|off' switch is gone with the
    // legacy path - every session is the core's.  `core_latched` is set by a
    // SessionStart on the network thread (where the arrival order is made):
    // that session's messages go to the mailbox up to its SessionEnd; a
    // Session* message outside a session is swallowed by core_host_offer and
    // counted in `core_stray` (e.g. a relayed input on the unreliable channel
    // that overtook nothing but arrived after the reliable SessionEnd).  A
    // latch whose SessionEnd never came stays until the next SessionStart, as
    // it did with the switch on; the drain discards what reaches no session.
    std::atomic<bool> core_latched{false};
    std::atomic<uint64_t> core_stray{0};
    // FULL step RCV: RC2 (b)'s capture cache mode for the NEXT core session
    // ('session cache off|on|verify'; core_config::capture_cache, read once
    // in the config block when a session begins).  On by default, as on the
    // headless client.  Host-local: nothing goes on the wire.
    std::atomic<uint8_t> capture_cache_next{
        static_cast<uint8_t>(bmmo::session::rollback_engine::capture_cache_mode::on)};
    // FULL step DEP (roadmap 4.7 "on pass: snapshots off ... in the retail
    // default"): the NEXT core session runs as a snapshots-off session
    // whatever interval the server sends (core_config::snapshots false: every
    // snapshot taken for its names only, none compared or restored from).
    // 'session snapshots on' brings the snapshot fallback back for the next
    // session (it then follows the server's interval, as before DEP).
    // Host-local: nothing goes on the wire.
    std::atomic<bool> snapshots_next{false};
    // K7e: a replay's journal cadence, the capture's (0: none).
    uint32_t replay_checkpoint_ticks = 0;
    // K7e: 'stop-at <tick>' - the replay stops after that tick's step with the
    // world frozen (the behaviour loop held as the anchor hold holds it, the
    // level rendered); the saved limits for the thaw.
    struct replay_stop_state {
        uint32_t tick = 0;   // 0: run to the end
        bool frozen = false;
        CKDWORD saved_limits = 0;
        float saved_rate = 0.0f;
    } replay_stop;
    // FULL step R6B: core_config::own_births_parked ('session own-births
    // parked|remade', a test verb: the same-build control arm of the
    // in-window own-birth remake).  Off (remade) by default: with
    // own_body_control an own birth the scripts made inside a rewind's
    // window is taken out at the restore and made again at its birth tick
    // (OWNB); parked keeps it on the parking path as before R6B.  Read at
    // every SessionStart and applied to a running core at once (the core
    // reads the field at each restore).  Host-local: nothing on the wire.
    std::atomic<bool> own_births_parked_next{false};
    // Step A2: core_config::remote_cadence_frames ('session cadence N'): 0 =
    // a remote's correction waits only while the pacer is behind.  Read at
    // every SessionStart, applied to a running core at once, kept in the
    // capture's header.  Host-local: nothing on the wire.
    std::atomic<uint32_t> remote_cadence_next{0};

    // ------------------------------------------------------------ the mailbox
    // Design 3.2 "Receive": every Session* opcode's bytes, in arrival order.  A
    // load-time object: the network thread pushes before any session exists.
    struct mailbox_entry {
        int code = 0;
        std::string bytes;
        bmmo::net::microseconds received = 0;   // the transport's stamp (step A5's measure)
    };
    class core_mailbox {
    public:
        // The snapshot cap: the legacy queue's 64 (its kMaxQueuedSnapshots),
        // the core host's own value since FULL step LGM; the legacy queue is
        // gone since FULL step LGD.
        static constexpr size_t kMaxSnapshots = 64;

        void push(int code, const void* data, size_t size, bmmo::net::microseconds received = 0) {
            std::lock_guard lk(mutex_);
            if (code == bmmo::SessionSnapshot) {
                // The cap, on the snapshot entries only: the oldest one goes.
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
            entries_.push_back({code, std::string(static_cast<const char*>(data), size), received});
            ++pushed_;
        }
        std::deque<mailbox_entry> take() {
            std::lock_guard lk(mutex_);
            std::deque<mailbox_entry> out;
            out.swap(entries_);
            snapshots_ = 0;
            return out;
        }
        // FULL step AHN (review): the entries before the first one `stop`
        // names; it and every later entry stay, in order.  `stopped` says
        // whether one was named.
        template <class Stop>
        std::deque<mailbox_entry> take_until(const Stop& stop, bool& stopped) {
            std::lock_guard lk(mutex_);
            std::deque<mailbox_entry> out;
            stopped = false;
            while (!entries_.empty()) {
                if (stop(entries_.front())) {
                    stopped = true;
                    break;
                }
                if (entries_.front().code == bmmo::SessionSnapshot && snapshots_ > 0) --snapshots_;
                out.push_back(std::move(entries_.front()));
                entries_.pop_front();
            }
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
    // the anchor's script replay): the host state of design 3.2 (FULL step
    // LGD: the legacy physics_session_state is gone, nothing of it remains).
    struct retail_host {
        // The core, made at each SessionStart (R1: a fresh
        // core per session, so no state of the last one carries over); the
        // login id is given to it before each begin (design 3.2, E11).
        std::unique_ptr<bs::session_client_core> core;
        std::string core_error;   // why it could not be made
        uint64_t sessions_seen = 0;

        // The session this host runs (R1): its phase - the legacy path's
        // counting_down / restarting / running, the core's own phase
        // inside it - and what the countdown, the restart and the frame
        // keep.
        // Step SPL: anchoring - the anchor's frozen preparation (the placement
        // over rendered frames, the 3-2-1) between the restart and running.
        enum class phase_type { idle, counting_down, restarting, anchoring, running };
        phase_type phase = phase_type::idle;
        uint32_t session = 0;
        // FULL step RTDF (decision 66): the room the session runs in (its
        // SessionStart's), which the level's exit leaves.
        uint32_t room = 0;
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
        // FULL step R6B (BEAMF-S (b), core design "As built (step R6D)" item
        // 1): Sound_Manager's Physics Collision Detection / Physics
        // Continuous Contact blocks - the only IVP listeners on the own ball
        // - collected at the anchor and re-armed after a frame whose
        // own_bodies_made advanced (a bridge-made own body has none bound).
        // `sound_rearm_check`: frames left until the re-arm's bind check.
        std::vector<uint32_t> sound_blocks;
        uint64_t sound_made_seen = 0;
        uint64_t sound_rearms = 0, sound_rearmed = 0, sound_skipped_off = 0, sound_skipped_target = 0,
                 sound_skipped_queued = 0, sound_reset_failed = 0;
        uint64_t sound_checks = 0, sound_bound = 0, sound_unbound = 0;
        int sound_rearm_check = 0;
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
        // Protocol 34 (core_host_legacy_mute): the mute set the server holds
        // for us, and the players physicalized at the last pass with their
        // shown entity.  Not reset with a session: they follow the server
        // connection (cleared at a login).
        std::vector<uint32_t> legacy_muted;
        std::map<uint32_t, CK_ID> legacy_physicalized;
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
        uint32_t recipe_parse_logs = 0;   // FULL step AUD3: the birth feed's unparsed recipe lines (the first 4)

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
        // and the own echo's deadline has something to do (the never-made
        // re-report, R6P; own_body_backstop until R6V).
        int drop_births_armed = 0;
        uint64_t births_dropped = 0;
        // 'session hold-event TYPE:N:MS|TYPE:N:drop|off' (FULL step R6H, a
        // testing tool; the headless --hold-event's helper,
        // session/event_hold.hpp): the N-th own event of TYPE leaves MS ms
        // late, or never (hooks.filter_event after drop-birth), released
        // from every OnProcess, discarded at the session's end
        // (hooks.ending).  Armed before or during a core session; the count
        // restarts at each session's anchor (as the headless client's).
        bmmo::session::event_hold event_hold;

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

    // FULL step AHN (session-client-core-design.md 2.3 `nonblocking_hold`,
    // "As built (step AHN)"): the anchor hold without the blocking loop.  At
    // the anchor frame's end the world is frozen - CKTimeManager's
    // behavioural rate limited to 0.001 Hz, so the Player loop skips
    // CKContext::Process (no behaviour, no manager pre/post-process, no IVP
    // step, no OnProcess) while it keeps rendering - and OnProcess returns.
    // The hold is polled from IMod::OnRender (BML+'s post-render callback):
    // the mailbox drained every 1 ms for up to 15 ms of each rendered frame,
    // then one ImGui frame in BML+'s context (BML's own UI through its
    // OnProcess, the message area included, and the wait panel,
    // session/hold_panel.hpp).  At the release the limits and the render
    // options go back, the world is captured again and compared with the
    // anchor's (one line per release), the tick driver starts from the start
    // instant and the first pre_step runs, as the blocking hold's tail did.
    // 'session hold blocking' keeps the blocking loop (the A/B control).
    struct hold_digest {
        bool ok = false;
        std::string error;
        uint64_t pose = 0, surfaces = 0;
        uint64_t blob = 0;
        size_t blob_size = 0;
        uint64_t record = 0;
        size_t record_size = 0;
        uint64_t e3d = 0, e2d = 0, arrays = 0;
        int e3d_count = 0, e2d_count = 0, array_count = 0;
        int main_ticks = 0;

        bool same_world(const hold_digest& o) const {
            return ok && o.ok && pose == o.pose && surfaces == o.surfaces && blob == o.blob && blob_size == o.blob_size
                && record == o.record && record_size == o.record_size && e3d == o.e3d && e2d == o.e2d
                && arrays == o.arrays && main_ticks == o.main_ticks;
        }
        // The parts that differ (empty: none).
        std::string diff(const hold_digest& o) const {
            std::string out;
            const auto add = [&](bool differs, const char* what) {
                if (!differs) return;
                if (!out.empty()) out += ",";
                out += what;
            };
            add(!ok || !o.ok, "capture");
            add(pose != o.pose || surfaces != o.surfaces, "hash");
            add(blob != o.blob || blob_size != o.blob_size, "blob");
            add(record != o.record || record_size != o.record_size, "script-record");
            add(e3d != o.e3d, "3d");
            add(e2d != o.e2d, "2d");
            add(arrays != o.arrays, "arrays");
            add(main_ticks != o.main_ticks, "ticks");
            return out;
        }
        std::string describe() const {
            return std::format("hash {:016x}/{:016x} blob {:016x} ({} B) script record {:016x} ({} B) 3d {:016x} ({}) "
                               "2d {:016x} ({}) arrays {:016x} ({}) main ticks {}{}",
                               pose, surfaces, blob, blob_size, record, record_size, e3d, e3d_count, e2d, e2d_count,
                               arrays, array_count, main_ticks, ok ? "" : " ERROR " + error);
        }
    };

    uint64_t hold_fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // The exactness criterion of the freeze: the world hash (pose and
    // surfaces), the IVP savestate blob, the selection's script record, every
    // 3D entity's world matrix and visibility, every 2D entity's rect,
    // visibility and colour, every data array's cells and the time manager's
    // tick count - read only.
    hold_digest hold_capture(CKContext* ctx, const bmmo::physics::physics_view& view,
                             const bmmo::game::script_replay::selection& ids, const bmmo::game::script_replay_hooks& hooks,
                             CKTimeManager* tm) {
        hold_digest s;
        s.ok = true;
        bmmo::physics::world_hash hash;
        std::string error;
        if (view.capture(hash, error)) {
            s.pose = hash.pose;
            s.surfaces = hash.surfaces;
        } else {
            s.ok = false;
            s.error += "hash: " + error + "; ";
        }
        std::vector<uint8_t> bytes;
        error.clear();
        if (view.savestate_get(bytes, error)) {
            s.blob = hold_fnv(bytes.data(), bytes.size());
            s.blob_size = bytes.size();
        } else {
            s.ok = false;
            s.error += "savestate: " + error + "; ";
        }
        bytes.clear();
        if (bmmo::game::script_replay::capture(ctx, ids, hooks, bytes)) {
            s.record = hold_fnv(bytes.data(), bytes.size());
            s.record_size = bytes.size();
        } else {
            s.ok = false;
            s.error += "script record failed; ";
        }
        {
            const int n = ctx->GetObjectsCountByClassID(CKCID_3DENTITY);
            CK_ID* list = ctx->GetObjectsListByClassID(CKCID_3DENTITY);
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < n; ++i) {
                auto* e = static_cast<CK3dEntity*>(ctx->GetObject(list[i]));
                if (!e) continue;
                const VxMatrix& m = e->GetWorldMatrix();
                const CKBOOL visible = e->IsVisible();
                h = hold_fnv(&list[i], sizeof(CK_ID), h);
                h = hold_fnv(&m, sizeof(VxMatrix), h);
                h = hold_fnv(&visible, sizeof(visible), h);
            }
            s.e3d = h;
            s.e3d_count = n;
        }
        {
            const int n = ctx->GetObjectsCountByClassID(CKCID_2DENTITY);
            CK_ID* list = ctx->GetObjectsListByClassID(CKCID_2DENTITY);
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < n; ++i) {
                auto* e = static_cast<CK2dEntity*>(ctx->GetObject(list[i]));
                if (!e) continue;
                VxRect r;
                e->GetRect(r);
                const CKBOOL visible = e->IsVisible();
                h = hold_fnv(&list[i], sizeof(CK_ID), h);
                h = hold_fnv(&r, sizeof(r), h);
                h = hold_fnv(&visible, sizeof(visible), h);
                if (CKMaterial* mat = e->GetMaterial()) {
                    const VxColor c = mat->GetDiffuse();
                    h = hold_fnv(&c, sizeof(c), h);
                }
            }
            s.e2d = h;
            s.e2d_count = n;
        }
        {
            const int n = ctx->GetObjectsCountByClassID(CKCID_DATAARRAY);
            CK_ID* list = ctx->GetObjectsListByClassID(CKCID_DATAARRAY);
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < n; ++i) {
                auto* a = static_cast<CKDataArray*>(ctx->GetObject(list[i]));
                if (!a) continue;
                const int rows = a->GetRowCount(), cols = a->GetColumnCount();
                h = hold_fnv(&list[i], sizeof(CK_ID), h);
                h = hold_fnv(&rows, sizeof(rows), h);
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c) {
                        CKDWORD* cell = a->GetElement(r, c);
                        if (!cell) continue;
                        const CK_ARRAYTYPE type = a->GetColumnType(c);
                        if (type == CKARRAYTYPE_STRING) {
                            const char* str = reinterpret_cast<const char*>(*cell);
                            if (str) h = hold_fnv(str, std::strlen(str), h);
                        } else if (type == CKARRAYTYPE_PARAMETER) {
                            // The cell holds the column's CKParameterOut: its
                            // value bytes, read without an update (review
                            // finding 5: the pointer alone missed a change).
                            auto* p = reinterpret_cast<CKParameter*>(static_cast<uintptr_t>(*cell));
                            const void* data = p ? p->GetReadDataPtr(FALSE) : nullptr;
                            const int size = p ? p->GetDataSize() : 0;
                            if (data && size > 0) h = hold_fnv(data, static_cast<size_t>(size), h);
                        } else {
                            h = hold_fnv(cell, sizeof(CKDWORD), h);
                        }
                    }
            }
            s.arrays = h;
            s.array_count = n;
        }
        s.main_ticks = tm->GetMainTickCount();
        return s;
    }

    struct anchor_freeze_state {
        bool nonblocking = true;       // 'session hold blocking|nonblocking': read at a SessionStart (caps)
        std::string shot_dir;          // 'session hold shots DIR': back-buffer dumps at 0.3 s and 1 s into a hold
        bool active = false;           // frozen: from the anchor frame's end to the release
        uint32_t session = 0;
        CKDWORD saved_limits = 0;
        float saved_beh_rate = 0.0f;
        CKDWORD saved_render = 0;
        bool render_changed = false;
        bool driver_released = false;  // the tick driver released by hooks.numbering_restarted
        bool driver_at_instant = false;
        bool notice_seen = false;      // a start-handshake notice came while a message was handled
        clock_type::time_point armed_at{}, last_frame{};
        uint64_t frames = 0, polls = 0;
        std::vector<double> gaps;      // ms between the hold's rendered frames
        hold_digest at_anchor;         // inside the anchor (core_host_anchor's end)
        hold_digest at_first;          // the first hold frame: the anchor frame's end, the freeze's reference
        double anchor_capture_ms = 0.0;
        size_t next_shot = 0;
        // Totals (this process): holds armed, releases whose world equals
        // the anchor's, those that differ, holds that ended unreleased.
        uint64_t holds = 0, equal = 0, different = 0, ended = 0;
        // The review's finding 1: a SessionStart or this session's SessionEnd
        // is never handled inside OnRender (core_host_end_local's teardown -
        // the script and IC restores, the trafo sets destroyed, the
        // navigation, the time manager - would run in the middle of
        // CKRenderContext::Render).  While frozen the drain stops before one
        // (`drain_frozen`, `fence_waiting`), the hold frame thaws, and the
        // next OnProcess drains it (`drain_next_process`) as the blocking
        // loop did.  `deferred` counts those holds.
        bool drain_frozen = false, fence_waiting = false, drain_next_process = false;
        uint64_t deferred = 0;
        std::string last;
        // Step SPL: the anchor's preparation inside the freeze, before the
        // core's anchor - the placement a slice per rendered frame, then the
        // 3-2-1 - and a failure in it, which ends the session in the next
        // OnProcess (finding 1: no teardown inside the render).
        enum class prep_stage { none, placing, countdown } prep = prep_stage::none;
        clock_type::time_point prep_from{}, countdown_from{};
        int countdown_said = 0;
        uint64_t prep_frames = 0;
        std::string end_next_process;
        bs::anchor_report report;      // made after the placement, sent after the countdown
    };
    anchor_freeze_state anchor_freeze;
    bmmo::retail::hold_panel hold_panel_ui;
    constexpr double kHoldShotSeconds[] = {0.3, 1.0};
    // Step SPL: a placement slice per rendered frame (at least one copy), how
    // long the server's boot is shown with no new phase and no SessionStart
    // (a server that died without its SessionEnd), and when each
    // start stage's screenshot is due ('session hold shots DIR').
    constexpr auto kPlacementSlice = std::chrono::milliseconds(10);
    constexpr auto kBootPanelTimeout = std::chrono::seconds(180);
    constexpr double kStageShotSeconds = 0.3;
    // The start's timeline (step SPL): one log line per panel stage.
    struct start_timeline_state {
        clock_type::time_point first{}, since{};
        std::string stage;
    } start_timeline;
    std::atomic<bool> core_booting{false};   // network thread: a boot status came, no SessionStart yet
    clock_type::time_point boot_last{};
    struct stage_shot_state {
        int stage = -1;
        bool taken = false;
        int count = 0;
    } stage_shot;

    // The hold's screenshots ('session hold shots DIR'): one post-sprite
    // callback, registered for the hold and removed at the thaw, dumps the
    // back buffer of the frame a shot is due in (BML's ImGui drawn by then).
    // ahn_r2 used one temporary callback per shot: they fired 0.4-0.7 s
    // after they were added while the world was frozen.
    struct hold_shot_request {
        bool registered = false;
        bool pending = false;
        std::string path;
        clock_type::time_point written_at{};
        bool written = false;
    } hold_shot;
    void hold_shot_cb(CKRenderContext* rc, void*) {
        if (!hold_shot.pending) return;
        hold_shot.pending = false;
        rc->DumpToFile(const_cast<CKSTRING>(hold_shot.path.c_str()), nullptr, VXBUFFER_BACKBUFFER);
        hold_shot.written_at = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
        hold_shot.written = true;
    }

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
            if (const auto type = host.mirror_type.find(it->second); type != host.mirror_type.end())
                objects.show_mirror(it->second, type->second);
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
        case bmmo::SessionStartAt: return "SessionStartAt";
        case bmmo::SessionStartStatus: return "SessionStartStatus";
        case bmmo::SessionBootStatus: return "SessionBootStatus";
        default: return "?";
        }
    }

    // A mailbox entry's bytes into its message (the bytes as the network
    // thread copied them, `size` of them).
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
        case bmmo::SessionStartAt: { bmmo::session_start_at_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionStartStatus: { bmmo::session_start_status_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionBootStatus: { bmmo::session_boot_status_msg m{}; return decode_bytes(bytes, m); }
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

    // The next tick the world steps, when a core session is running and
    // numbered.  The 'replay session' feed reads it at the input poll, before
    // the step.  (FULL step LGM: the legacy session's numbering is no longer
    // read here - a legacy session cannot start since B9DX, the server
    // refusing a member without the core.)
    bool next_session_tick(uint32_t& tick) {
        if (host.core && host.core->session_phase() == bs::client_core_state::phase::running) {
            if (!host.core->assigned() || host.core->holding()) return false;
            tick = host.core->next_tick();
            return true;
        }
        return false;
    }

    // The tick the world stepped last (the 'net' verbs' "from tick" and
    // their 'at <tick>'): the core's with a core session, 0 otherwise (FULL
    // step LGM: no legacy fallback, as above).
    uint32_t session_tick_now() {
        if (host.core && host.core->session_phase() != bs::client_core_state::phase::idle)
            return host.core->current_tick();
        return 0;
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
    // two frames per slot through SkipRenderForNextTick (one skip per raised
    // tick, so SRT1's every-other-render rule does not reach it), a lower
    // waits.
    static_assert(bs::fixed_tick_driver::kMaxCatchUpTicks == bs::kPacerRebaseFrames,
                  "the retail rebase threshold is the core's lead_rebase_threshold base");
    class core_pacer_slew final : public bs::pacer_slew {
    public:
        int step(clock_type::time_point now, double behind) override {
            if (!core_session_running()) return 0;
            host.core->note_pacer_behind(behind);
            if (!host.core->slewing()) return 0;
            return host.core->slew_step(now, bs::TICK_RATE, behind);
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
        case retail_host::phase_type::anchoring: return "anchoring";
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
        // FULL step R6B: read again at the anchor; the counters are the session's.
        host.sound_blocks.clear();
        host.sound_made_seen = host.own_bodies_made;   // the counter is the process's, not reset per session
        host.sound_rearms = host.sound_rearmed = host.sound_skipped_off = host.sound_skipped_target = 0;
        host.sound_skipped_queued = host.sound_reset_failed = 0;
        host.sound_checks = host.sound_bound = host.sound_unbound = 0;
        host.sound_rearm_check = 0;
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
        host.recipe_parse_logs = 0;
        host.forced = 0;
        host.force_once.clear();
        own_sector_reports = {};   // step SLG
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

    // ---- FULL step R6B: the own ball's collision-sound listeners re-armed
    // (BEAMF-S (b); core design "As built (step R6D)" item 1).  A host
    // nature, not a core decision: the headless world plays no sound.
    // Sound_Manager's Physics Collision Detection and Physics Continuous
    // Contact blocks hold the only IVP listeners on the own ball.  A body the
    // bridge makes (a beam's stamp, an in-window birth's remake, a revive, a
    // resync's remake) has none: a Collision Detection listener of the old
    // body is unhooked and its object nulled but stays in the block's local 0
    // (so the block's Create makes nothing), a Continuous Contact's contact
    // data dies with the old body (local 1 nulled).  CK API only.
    const CKGUID kSoundCollisionGuid(0x7435488d, 0x201d1188);   // physics_RT: Physics Collision Detection
    const CKGUID kSoundContactGuid(0x199e4cf1, 0x545a78fe);     // physics_RT: Physics Continuous Contact

    // The two prototypes' blocks of Sound_Manager, at any depth, in block
    // order (depth first); empty without the script.  Read once at the
    // anchor; each block is tested again at every re-arm.
    std::vector<uint32_t> sound_listener_blocks(CKContext* context) {
        std::vector<uint32_t> out;
        CKBehavior* root = bmmo::game::find_root_script(context, "Sound_Manager");
        if (!root) return out;
        std::vector<CKBehavior*> stack{root};
        while (!stack.empty()) {
            CKBehavior* behavior = stack.back();
            stack.pop_back();
            if (behavior != root) {
                const CKGUID guid = behavior->GetPrototypeGuid();
                if (guid == kSoundCollisionGuid || guid == kSoundContactGuid) out.push_back(behavior->GetID());
            }
            for (int i = behavior->GetSubBehaviorCount(); i-- > 0;)
                if (CKBehavior* sub = behavior->GetSubBehavior(i)) stack.push_back(sub);
        }
        return out;
    }

    // After a frame whose own_bodies_made advanced, outside any replay (the
    // end of core_host_frame): every collected block whose target is the
    // CURRENT own entity (a trafo changes it) and that is On (IsActive; the
    // Off input leaves a block inactive, and an ActivateInput on it would
    // switch back on what the script switched off - death, level end, ball
    // switch).  A Collision Detection block needs its stale listener in
    // local 0 (a null local 0 on an active block: its Create's callback is
    // still queued and binds to the current body anyway - re-arming it would
    // queue a second one, whose Execute news a listener unconditionally);
    // it is reset through its own CK callback (CKM_BEHAVIORRESET deletes the
    // listener inside physics_RT, no cross-module delete), then both kinds
    // get ActivateInput(0): their next live execution queues the callback,
    // which binds to the current body at that frame's PreSimulate.  A still
    // queued entry a rewind meets is an outside entry gap Q4 holds.
    void sound_rearm(CKContext* context, const bmmo::physics::physics_view& view, logger_wrapper* logger,
                     const std::string& own_entity, uint32_t tick) {
        auto* own = own_entity.empty() ? nullptr
                  : CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(own_entity.c_str()),
                                                                           CKCID_3DENTITY, nullptr));
        if (!own || !view.has_body(own_entity.c_str())) {
            logger->Info("Physics session core: sound re-arm at tick %u skipped: own entity '%s' %s", tick,
                         own_entity.c_str(), own ? "has no body" : "not found");
            return;
        }
        ++host.sound_rearms;
        uint32_t rearmed = 0, off = 0, target = 0, queued = 0, failed = 0, gone = 0;
        for (const uint32_t id: host.sound_blocks) {
            auto* behavior = CKBehavior::Cast(context->GetObject(id));
            if (!behavior) {
                ++gone;
                continue;
            }
            if (behavior->GetTarget() != own) {
                ++target;
                continue;
            }
            if (!behavior->IsActive()) {
                ++off;
                continue;
            }
            if (behavior->GetPrototypeGuid() == kSoundCollisionGuid) {
                void* listener = nullptr;
                behavior->GetLocalParameterValue(0, &listener);
                if (!listener) {
                    ++queued;
                    continue;
                }
                behavior->CallCallbackFunction(CKM_BEHAVIORRESET);
                listener = nullptr;
                behavior->GetLocalParameterValue(0, &listener);
                if (listener) {   // the reset route failed on this CK2: leave the block as it is
                    ++failed;
                    continue;
                }
            }
            behavior->ActivateInput(0, TRUE);
            ++rearmed;
        }
        host.sound_rearmed += rearmed;
        host.sound_skipped_off += off;
        host.sound_skipped_target += target;
        host.sound_skipped_queued += queued;
        host.sound_reset_failed += failed;
        if (rearmed) host.sound_rearm_check = 3;   // the bind check three frames on
        if (host.sound_rearms <= 32 || failed)
            logger->Info("Physics session core: sound re-arm at tick %u on %s: %u of %zu blocks re-armed, %u off, %u "
                         "another target, %u queued, %u reset failed, %u gone (re-arm %llu)", tick, own_entity.c_str(),
                         rearmed, host.sound_blocks.size(), off, target, queued, failed, gone,
                         static_cast<unsigned long long>(host.sound_rearms));
    }

    // Three frames after a re-arm: whether the re-armed blocks' listeners
    // are bound (Collision Detection local 0, Continuous Contact local 1
    // non-null) - the evidence that the remade ball sounds again.
    // `counted`: true after a re-arm (the counters), false at the session's
    // end (a census, logged only).
    void sound_bind_check(CKContext* context, logger_wrapper* logger, const std::string& own_entity, uint32_t tick,
                          bool counted = true) {
        auto* own = own_entity.empty() ? nullptr
                  : CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(own_entity.c_str()),
                                                                           CKCID_3DENTITY, nullptr));
        uint32_t bound = 0, unbound = 0, off = 0;
        for (const uint32_t id: host.sound_blocks) {
            auto* behavior = CKBehavior::Cast(context->GetObject(id));
            if (!behavior || !own || behavior->GetTarget() != own) continue;
            if (!behavior->IsActive()) {
                ++off;
                continue;
            }
            void* local = nullptr;
            behavior->GetLocalParameterValue(behavior->GetPrototypeGuid() == kSoundCollisionGuid ? 0 : 1, &local);
            (local ? bound : unbound)++;
        }
        if (counted) {
            ++host.sound_checks;
            host.sound_bound += bound;
            host.sound_unbound += unbound;
        }
        if (!counted || host.sound_checks <= 32 || unbound)
            logger->Info("Physics session core: sound listeners %s at tick %u on %s: %u bound, %u unbound, %u off",
                         counted ? "after a re-arm" : "at the session's end", tick, own_entity.c_str(), bound, unbound,
                         off);
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
    // written back at the anchor (game/script_state.hpp).  File scope:
    // BallanceMMOClient takes no data member.
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
    // (core_host_capture_level_counters), which makes the anchor exactly the measured
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
    // Step OPB (rewind-cost-design R.4 row B1): their memo, so a binding
    // walks the selection only when what the walk reads changed
    // (BMMO_CELLS_MEMO=off | on | verify, on when unset); dropped with every
    // new selection.  File-scope (BallanceMMOClient takes no member).
    bmmo::game::array_reads::cells_memo cells_memo_host;
    size_t cells_memo_notes_logged = 0;

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
        error = proximity_host.union_.rewire(context, bmmo::game::probe_class::mechanism, sel, options,
                                             bmmo::game::force_hooks_for(v));
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

    double seconds_since(clock_type::time_point t, bs::clock_site site = bs::clock_site::retail_display) {
        return std::chrono::duration<double>(bmmo::retail::host_clock().now(site) - t).count();
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
        // The "made by the replayed tick" lines of the birth log's feed (the
        // first 8 of a session; FULL step AUD3: the feed is host support's
        // birth_feed over the event log's recipes, as on the headless host,
        // no longer OnPhysicalize's birth tick).
        uint32_t replay_birth_logs = 0;
        // What the muted log's drains carried (AUD3 gate round, review
        // finding 1): the drains of the replayed ticks and of the resync
        // remakes, the "recipe" entries they returned and the names the feed
        // added from them; printed once at the session's end, so a run shows
        // the bridge v33 route carries the births (entries > 0 across a
        // rewind over an explosion) and not just an empty text.
        uint64_t muted_replay_drains = 0, muted_remake_drains = 0, muted_recipes = 0, muted_added = 0;
    };
    trafo_host_state trafo_host;

    // physics_view's muted-recipe pair for host support's feed_muted_births,
    // counting the "recipe" entries each drain returns (a diagnostic only:
    // the text goes on to the feed unchanged).
    struct counted_muted_recipes {
        const bmmo::physics::physics_view& view;
        uint64_t& drains;
        void set_event_log_keep_muted_recipes(bool keep) const { view.set_event_log_keep_muted_recipes(keep); }
        std::string drain_muted_recipes() const {
            std::string text = view.drain_muted_recipes();
            ++drains;
            bs::parse_event_log(text, [](const std::string& kind, const std::string&) {
                if (kind == "recipe") ++trafo_host.muted_recipes;
            });
            return text;
        }
    };

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

    // The bridge as the retail host reaches it: host support's one trafo
    // hooks factory over physics_view (FULL step AUD3, audit D6:
    // game/host_backend.hpp trafo_hooks_for, the headless hosts' too).
    // restore_originals stays empty there (physics_view has no
    // restore_trafo_originals): the anchor runs the quiesce right before the
    // session clock reset, whose restore_explosion_pieces is that restore
    // (physics_state.cpp reset_session_clock), and nothing else calls it.
    bmmo::game::trafo_set_hooks make_retail_trafo_hooks(const bmmo::physics::physics_view& view, logger_wrapper* logger) {
        return bmmo::game::trafo_hooks_for(&view, [logger](const std::string& line) {
            logger->Info("Physics session: trafo: %s", line.c_str());
        });
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
        trafo_host.hold.arm(context, bs::TICK_DELTA_MS);
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
        if (trafo_host.muted_replay_drains || trafo_host.muted_remake_drains)
            logger->Info("Physics session: birth log: muted drains replay=%llu remake=%llu carried %llu recipe entries, %llu names added",
                         static_cast<unsigned long long>(trafo_host.muted_replay_drains),
                         static_cast<unsigned long long>(trafo_host.muted_remake_drains),
                         static_cast<unsigned long long>(trafo_host.muted_recipes),
                         static_cast<unsigned long long>(trafo_host.muted_added));
        trafo_host.muted_replay_drains = trafo_host.muted_remake_drains = trafo_host.muted_recipes = trafo_host.muted_added = 0;
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
        logger->Info("Physics session: trafo: the session's sets torn down (%d piece bodies removed)", bodies);
    }

    // ------------------------------------------- diagnostics (FULL step RT1)
    // The core journals each checkpoint tick's FINAL rows
    // (core_config::journal_final_checkpoints, step D0), so a retail journal
    // is judged on the post-rewind state as the headless batches are.  On by
    // default since journal version 10; BMMO_JOURNAL_FINAL_CHECKPOINTS=0
    // turns it off.
    bool diag_final_checkpoints() {
        const char* v = std::getenv("BMMO_JOURNAL_FINAL_CHECKPOINTS");
        return !(v && std::strcmp(v, "0") == 0);
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

// Step RK3: a core session past its anchor that switches this level's
// tutorial off (record-format-gaps-design.md 8 row K3 = C4).
bool bmmo::session::retail::core_tutorial_owned() {
    return core_session_running() && host.core->tutorial_off();
}

// FULL step ESCX: the automation's 'pausechain' read in a core session -
// the core host's mute in force (the legacy line's 'muted' is the legacy
// path's) and the ESC menu overlay.  Empty while no core session runs.
std::string bmmo::session::retail::menu_overlay_status(CKContext* context) {
    if (!core_session_running()) return {};
    return std::format("core_muted={} ", host.input_muted ? 1 : 0) + core_menu_overlay.status(context) + " "
           + core_menu_keys.status() + " " + core_own_wake.status();
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

// Design 3.2 "Receive": every Session* message from a SessionStart up to its
// SessionEnd goes into the mailbox as its bytes, SessionStart first, so the
// game thread sees them in arrival order (true: the caller stops there).
// FULL step LGD: with the legacy path gone every session is the core's, and a
// Session* message outside a latched session (a relayed input or snapshot on
// the unreliable channel arriving after the reliable SessionEnd) is swallowed
// here and counted (`stray=` in 'session core status') instead of reaching
// on_message's default, which would log it as an invalid opcode.  Any other
// message: false.
bool BallanceMMOClient::core_host_offer(const bmmo::net::message* network_msg) {
    const auto* raw = reinterpret_cast<const bmmo::general_message*>(network_msg->data);
    if (raw->code == bmmo::SessionStart) {
        // Every session is the core's (FULL step LGD): latched here.
        core_latched.store(true);
        core_booting.store(false);
        mailbox.push(static_cast<int>(raw->code), network_msg->data, static_cast<size_t>(network_msg->size));
        return true;
    }
    if (raw->code == bmmo::SessionBootStatus) {   // protocol 32, step SPL: before the SessionStart
        core_booting.store(true);
        mailbox.push(static_cast<int>(raw->code), network_msg->data, static_cast<size_t>(network_msg->size));
        return true;
    }
    switch (raw->code) {
    case bmmo::SessionEnd:
        // Step SPL: a world boot that failed ends with a SessionEnd before any
        // SessionStart; it takes the boot's panel down (core_host_drain).
        if (!core_latched.load()) {
            if (core_booting.exchange(false))
                mailbox.push(static_cast<int>(raw->code), network_msg->data, static_cast<size_t>(network_msg->size));
            else
                core_stray.fetch_add(1);
            return true;
        }
        mailbox.push(static_cast<int>(raw->code), network_msg->data, static_cast<size_t>(network_msg->size));
        core_latched.store(false);
        return true;
    case bmmo::SessionAssign:
    case bmmo::SessionSnapshot:
    case bmmo::SessionRemoteInput:
    case bmmo::SessionEvent:
    case bmmo::SessionLead:
    case bmmo::SessionResyncState:   // step A3 (protocol 10)
    case bmmo::SessionMember:        // step M1 (protocol 18)
    case bmmo::SessionStartAt:       // FULL step SAXH (protocol 24)
    case bmmo::SessionStartStatus:
        if (!core_latched.load()) { core_stray.fetch_add(1); return true; }
        mailbox.push(static_cast<int>(raw->code), network_msg->data, static_cast<size_t>(network_msg->size),
                     network_msg->received);
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
    hold_panel_ui.hide();   // step SPL: the server's boot is over
    if (host.phase != retail_host::phase_type::idle) core_host_end_local("replaced by a new session");
    bs::session_capabilities caps;
    // FULL step R6B (core design 2.3 / 6.2 R6 row, "As built (step R6D)"
    // item 2): own body control - make_own / unmake_own are the bridge's
    // (below).  An own birth the scripts made inside a rewind's window is
    // taken out at the restore and made again at its birth tick, as the
    // server makes its clone (destroyable_own_birth, OWNB's remake), instead
    // of parked.  A live own birth stays the game's Physicalize block's.
    // FULL step R6P (core_config::own_birth_move on, below): a moved own
    // birth is moved inside the history (the body out live, the rewind to
    // its stamp, the body made again at the server's tick), a superseded one
    // taken out of the history (R6H-SUP (b)), a never-made one re-reported
    // once at the ball's current pose and answered by that re-report's echo
    // (Q-R6-NEVER (a)); a resync only past the reach.  'session own-births
    // parked' is the control arm of the in-window remake.
    caps.own_body_control = true;
    // FULL step R6P (Q-R6-NEVER (a) item 3.2): NOT own_life_take_out - the
    // game owns the life (Gameplay_Ingame goes on driving the ball the
    // Physicalize block made), so a current life the server never made is
    // never taken out live; the core keeps the body and resyncs.
    caps.own_life_take_out = false;
    // Step EL8 (rollback-engine gaps design 2.3.4 "RC capability"): make_own /
    // unmake_own on the bridge, called by the core from restore_lives and the
    // replay's drain - an own life ending inside a window is revived and
    // removed again at its death; every body so made is gone by `now`.  FULL
    // step BEAMF (the user's Q-R6-BEAM (a)): the core also calls them live at
    // an own beam's stamp - the fresh body the server makes, not an in-place
    // write (session_client_core::beam_remakes_body).
    caps.own_body_in_replay = true;
    caps.slew = true;                // R5: the tick driver slews (core_host_pacer), SessionReady caps = CAP_LEAD
    // FULL step AHN: the anchor hold returns to the host loop (the world
    // frozen, polled from OnRender) unless 'session hold blocking' asked
    // for the blocking loop inside OnProcess (the A/B control).
    caps.nonblocking_hold = anchor_freeze.nonblocking;
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
    {
        // FULL step AHN: the wait panel's members (display only).
        std::vector<bmmo::retail::hold_panel::member> members;
        for (const auto& player: msg.players) {
            bmmo::retail::hold_panel::member m;
            m.own = player.id == db_.get_client_id();
            const auto state = m.own ? std::nullopt : db_.get(static_cast<bmmo::net::connection>(player.id));
            m.name = m.own ? get_display_nickname() : state ? state->name : std::format("player {}", player.id);
            members.push_back(std::move(m));
        }
        hold_panel_ui.reset(static_cast<int>(msg.map.level), std::move(members));
    }
    // The core's configuration (design 3.2 "core_config"): every field set
    // here.  The engine runs as the legacy path's does - its defaults, the
    // exact restore (savestate) on with the binding's lenient flags 0, no
    // clock rewind (the clock guard replaces it); the feature switches of
    // design 6.2 R3: snapshot rollback, the input rewind (a relayed frame
    // that differs from the prediction replays from its tick), the
    // lifecycle rewind (a late remote birth is made at its stamp inside a
    // replay, a late death rewinds) and the own echoes acted on with the
    // answers this host's capabilities and switches select (own body
    // control since FULL step R6B, own_birth_move on since R6P: a moved own
    // birth moved inside the history, a superseded one taken out of it, a
    // rejected or missing one re-reported once at the ball's current pose
    // and answered by that echo, a resync only past the reach; an in-window
    // scripted own birth taken out and remade at its birth tick);
    // the own corrections (R4, late-player part one: a held notice of our
    // own substituted tick amends the tick's record and replays from it)
    // (own_body_backstop, on here until FULL step R6V, is deleted: decision
    // 6 (a)).  The lead (R5, late-player part two) is the
    // capability `slew`, not a switch: the core amends, forces and slews
    // a SessionLead, the tick driver moves its schedule (core_host_pacer).
    bs::core_config config;
    config.journal_checkpoint_ticks = journal_checkpoint_ticks();
    config.journal_final_checkpoints = diag_final_checkpoints();   // step D0's rows, on by default (journal v10)
    config.level = msg.map.level > 0 ? msg.map.level : 1;
    config.verbose = core_trace;   // 'session trace on|off' (not reset per session; FULL step LGM: the core host's own)
    config.trace = core_trace;
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
    config.rollback = core_rollback;   // 'session rollback on|off' (FULL step LGM: the core host's own)
    config.correct = true;
    config.input_rewind = true;   // R2
    config.lifecycle_rewind = true;   // R3
    config.own_echoes = true;   // R3
    config.own_corrections = true;   // R4
    // FULL step R6P (late-player design 3.3 / 3.5, core design 6.2 R6 row):
    // the own echoes' moves on - R6S built the switch off, R6B declared
    // own_body_control with it off.
    config.own_birth_move = true;
    // FULL step R6B: 'session own-births parked' keeps the in-window
    // scripted own birth on the parking path (the control arm).
    config.own_births_parked = own_births_parked_next.load();
    config.remote_cadence_frames = remote_cadence_next.load();   // step A2
    config.telemetry_host = bmmo::session_telemetry_msg::HostRetail;   // step A6
    // RC3 (step B5RX, the user: work as if snapshots did not exist): with
    // the session's snapshots off, a body-set change's full snapshot is no
    // correction, only names and rows.
    config.body_set_corrections = false;
    // FULL step DEP: no snapshot correction by default ('session snapshots').
    config.snapshots = snapshots_next.load();
    // RC2 (b) (step RCB, the user's Q-CPU (b)): the capture's per-body read
    // cache, on as on the headless client; 'session cache off|on|verify'
    // picks it for the next session (FULL step RCV).
    config.capture_cache =
        static_cast<bmmo::session::rollback_engine::capture_cache_mode>(capture_cache_next.load());
    // R1's policy (the legacy frame's order) only stood in for the input
    // rewind: with it a key edge relayed between the frames replays from its
    // tick, so the drive of T + 1 is chosen at post_step(T) as on the
    // headless client (As built, step R2) - but for the relayed frame of T + 1
    // itself, arrived after post_step(T): pre_step(T + 1) drives with it on
    // every host (FULL step SLX).
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
    host.room = msg.room;   // FULL step RTDF
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
        // The server acks the leave (RequestAccepted): queued like the
        // console's (send_room_request), so the ack reads "Left room #N."
        // and no earlier request takes it.
        send_room_request(bmmo::room::action::Leave, msg.room);
        logger_->Info("Physics session %u: not in the level, left room %u", msg.session, msg.room);
        return;   // the core stays idle: the session's messages are dropped
    }
    core.level_requested();
    host.phase = retail_host::phase_type::counting_down;
    host.saw_ingame_inactive = false;
    host.restart_deadline = bmmo::retail::host_clock().now(bs::clock_site::retail_deadline) + std::chrono::seconds(5);
    // Step SPL (the user, 2026-11-04): the level restarts now; the 3-2-1
    // comes after the anchor's placement, frozen, right before SessionReady.
    SendIngameMessage(std::format("Physics session {} starting: preparing the level.", msg.session), bmmo::ansi::BrightGreen);
    logger_->Info("Physics session %u (core): starting, the level restarts now (players=%zu, join order %d, own id %u)",
                  msg.session, msg.players.size(), core.own_join_order(), db_.get_client_id());
    // Step SP9 (design 2.5): a level that ran a session since its load is
    // reloaded at the session's start; a clean one restarts at once (step
    // SPL: there is no countdown before the restart any more).
    level_life.reloading = level_life.loaded = level_life.go_waiting = level_life.exit_seen = false;
    // The session's map, not the one this game has loaded: a member whose
    // game moved on (it finished the room's level and the game went to the
    // next one) joining late anchored that other level on the clean path,
    // and the server refused it with a placement mismatch (2026-11-08 public
    // test, sessions 2, 9, 11, 12, 13).  An original level is loaded through
    // the reload below; another custom map cannot be.
    const bool other_map = static_cast<const bmmo::map&>(current_map_) != msg.map;
    if (other_map && (!msg.map.is_original_level() || !level_life.enabled)) {
        core_host_end_local(std::format("your game is in {}, the session runs {}: load that map first",
                                        get_map_name(current_map_), get_map_name(msg.map)));
        return;
    }
    if (other_map)
        logger_->Info("Physics session %u: this game is in %s, the session runs level %d: loading it", msg.session,
                      get_map_name(current_map_).c_str(), msg.map.level);
    if ((level_life.ran_union || other_map) && level_life.enabled) {
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
        // Step CMAP: a level loaded from a custom map reloads that file into the same slot (BML+'s patch), not
        // the slot's own file.
        const std::string custom = other_map ? std::string() : bmmo::session::retail::loaded_custom_map();
        if (!custom.empty())
            logger_->Info("Physics session %u: the reload loads the custom map %s into slot %d", msg.session,
                          custom.c_str(), level);
        begin_level_request(level, custom);
        level_life.reloading = true;
        level_life.level = level;
        level_life.started = bmmo::retail::host_clock().now(bs::clock_site::retail_reload);
        ++level_life.reloads;
        host.restart_deadline = level_life.started + kReloadTimeout;
        logger_->Info("Physics session %u: %s: reloading level %d (exit, then the menus) during the countdown",
                      msg.session, other_map ? "another level is loaded" : "the level ran a union", level);
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
    hold_panel_ui.restarting(level_life.reloading, bmmo::retail::host_clock().now(bs::clock_site::retail_display));
    core_host_start_stage(level_life.reloading ? "reloading the level" : "restarting the level");
    if (level_life.reloading) level_life.go_waiting = true;   // core_host_process restarts when the reload is ready
    else core_host_go();
}

// Step SP9: the reload's level is ready for the restart - the map load seen
// since the exit, the Sequencer / proximity capture window after it passed
// (core_host_capture_level_counters), the menus' request done and the level in play.
bool BallanceMMOClient::core_host_reload_ready() {
    // No pause test: the restart opens the pause menu itself and runs from a
    // paused level as the clean path's "Go!" does (review of SP9).
    return level_life.loaded && core_level_counter_frames <= 0 && !level_request_.pending() && m_bml->IsIngame()
        && gameplay_ingame_script_active()
        && bmmo::retail::host_clock().now(bs::clock_site::retail_reload) >= level_life.loaded_at + std::chrono::milliseconds(level_life.delay_ms);
}

// Step SP9: OnLoadObject(isMap) - a fresh level: no session ran on it.
void BallanceMMOClient::core_host_map_loaded() {
    // FULL step LGM (HU1 (b)): the level file's counters as loaded, recorded
    // by core_host_capture_level_counters on the frames after this hook (the
    // behaviours are not enumerable from it yet), the legacy capture's window.
    core_level_sequencers.clear();
    level_proximity_state.clear();
    core_level_counter_frames = kLevelCounterFrames;
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
    level_life.loaded_at = bmmo::retail::host_clock().now(bs::clock_site::retail_reload);
    logger_->Info("Physics session %u: the reload's map load %.3f s after the exit", host.session,
                  seconds_since(level_life.started));
}

// FULL step LGM (HU1 (b)): one frame of the record window after a map load
// (OnProcess; the legacy capture_level_sequencers read the same frames until
// FULL step LGD deleted it): the Sequencer counters and the proximity countdowns the
// level file has, by the shared capture the server's world and the headless
// client call.  The anchor restores them (core_host_anchor).
void BallanceMMOClient::core_host_capture_level_counters() {
    if (core_level_counter_frames <= 0) return;
    --core_level_counter_frames;
    bmmo::game::capture_level_file_counters(m_bml->GetCKContext(), core_level_sequencers, level_proximity_state,
                                            [this](const std::string& line) {
                                                logger_->Info("Physics session: %s (session core's record)", line.c_str());
                                            });
    if (core_level_counter_frames == 0 && core_level_sequencers.size() == 0)
        logger_->Info("Physics session: no mechanism Sequencer in this level (session core's record)");
}

// FULL step LGM: the automation's 'pausechain' read while a core session
// exists - the legacy line's fields (BallanceMMOClient::pause_scripts_status)
// from the core host's own edits: whether the menu is open, whether the world
// scripts are still active, the time factor, the mute (the core's), the pause
// chain's entries, N1's call sites and N2's pins with what each block reads
// now; then the ESC menu overlay's status (FULL step ESCX) once the core runs
// the session.
std::string BallanceMMOClient::core_host_pause_status() {
    CKContext* context = m_bml->GetCKContext();
    const auto active = [](CKBehavior* script) { return script && script->IsActive() ? 1 : 0; };
    float factor = -1.0f;
    {
        float value = 0.0f, delta = 0.0f;
        std::string error;
        if (physics_view_.get_clock(value, delta, error)) factor = value;
    }
    std::string out = std::format(
            "paused={} ingame_script={} events_script={} tutorial_script={} muted={} time_factor={:.6f} "
            "entries={} failed={}",
            m_bml->IsPaused() ? 1 : 0, active(m_bml->GetScriptByName("Gameplay_Ingame")),
            active(m_bml->GetScriptByName("Gameplay_Events")), active(m_bml->GetScriptByName("Gameplay_Tutorial")),
            host.input_muted ? 1 : 0, factor, core_pause_chain.scripts_count(),
            core_pause_chain.scripts_failed() ? 1 : 0);
    out += std::format(" death_reset={}/{} sector_keep={}/{}", core_reset_guard.count(),
                       core_reset_failed ? "failed" : "resolved", core_deactivate_pins.size(),
                       core_deactivate_failed ? "failed" : "resolved");
    for (const auto& pin: core_deactivate_pins) {
        auto* block = CKBehavior::Cast(context->GetObject(pin.block));
        const bool set_cell = block && block->IsUsingFunction() && block->GetPrototypeName()
                           && std::strcmp(block->GetPrototypeName(), "Set Cell") == 0;
        CKBehavior* chain = set_cell ? block->GetParent() : nullptr;
        int sector_value = -1;
        if (set_cell)
            if (CKParameterIn* input = block->GetInputParameter(pin.input))
                if (CKParameter* param = input->GetRealSource())
                    if (param->GetGUID() == CKPGUID_INT) param->GetValue(&sector_value);
        out += std::format(" [{}/keep=applied/now={}]", chain && chain->GetName() ? chain->GetName() : "?",
                           sector_value);
    }
    for (const auto& site: core_reset_guard.sites()) {
        CK_ID now = 0;
        auto* block = CKBehavior::Cast(context->GetObject(site.block));
        if (block && block->IsUsingFunction() && block->GetPrototypeName()
                && std::strcmp(block->GetPrototypeName(), "Execute Script") == 0)
            if (CKParameterIn* input = block->GetInputParameter(1))
                if (CKParameter* param = input->GetRealSource()) param->GetValue(&now);
        out += std::format(" [{}={}/{}/now={}]", site.chain, site.retail, site.applied ? "applied" : "resolved", now);
    }
    out += core_pause_chain.scripts_entries(context);
    const std::string overlay = bmmo::session::retail::menu_overlay_status(context);
    return overlay.empty() ? out : out + " " + overlay;
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

// FULL step RTDF (the user's decision 66, 2026-11-01: option (a) of step
// RTD's finding).  OnPreExitLevel: leaving the level ends the session here,
// before Event_handler's Exit Level chain acts, and the member leaves the
// session's room - the leave the server already handles (its
// physics_session_member_left: the member removed at the runner's next tick,
// its ball's Unphysicalize relayed on its behalf, GHF's spawn-ghost forget),
// J4's path and the headless --leave-room-at's order (the core ended, then
// the room Leave).  Until RTDF the session ended at OnPostExitLevel, ~20
// ticks into that chain, so the chain's teardown - the sector deactivated
// outside a union pass ([0][2]=1), the own ball unphysicalized, the trafo
// sets reset - was stepped, recorded and sent, and nothing told the server,
// which held the member's input until the room closed.  Now nothing of the
// teardown is recorded or sent (every hook tests core_session_running()), the
// chain runs on the vanilla level (the session's script edits handed back
// first), and the copies of the sector placement still go at OnPostExitLevel.
// SP9's reload leaves the level on purpose: its exit is the reload's
// (core_host_reload_exit's test), not a leave.  There is no session-only
// leave on the wire (a new message would be a protocol change), so the player
// is out of the room afterwards, as after a room Leave; joining the room
// again late-joins a session that still runs.
void BallanceMMOClient::core_host_pre_exit_level() {
    if (host.phase == retail_host::phase_type::idle) return;
    if (level_life.reloading && !level_life.exit_seen && host.phase == retail_host::phase_type::counting_down) {
        logger_->Info("Physics session %u: the level's exit is the reload's; the session goes on", host.session);
        return;
    }
    const uint32_t session = host.session, room = host.room;
    const uint32_t tick = host.core ? host.core->current_tick() : 0;
    core_host_end_local("left the level");
    if (!room) {
        logger_->Warn("Physics session %u: left the level at tick %u; no room known, no leave sent", session, tick);
        return;
    }
    // The server acks the leave (RequestAccepted): queued like the console's
    // "/mmo room leave" (send_room_request), so the player reads "Left room
    // #N." and an earlier request still unanswered does not take this ack.
    send_room_request(bmmo::room::action::Leave, room);
    logger_->Info("Physics session %u: left the level at tick %u, before its exit chain; left room %u", session, tick,
                  room);
}

// The restart (the legacy physics_session_countdown's last step), with the
// restart deadline counted from here.  Step SPL: at the SessionStart (or when
// its reload is ready); the 3-2-1 and "Go!" come after the placement
// (core_host_countdown).
void BallanceMMOClient::core_host_go() {
    const uint32_t session = host.session;
    host.phase = retail_host::phase_type::restarting;
    host.saw_ingame_inactive = false;
    host.restart_deadline = bmmo::retail::host_clock().now(bs::clock_site::retail_deadline) + std::chrono::seconds(20);
    logger_->Info("Physics session %u: restart requested", session);
    if (!fixed_tick_.enabled()) fixed_tick_.enable(m_bml);
    restart_current_level();
}

// Step SPL (the user, 2026-11-04): the 3-2-1 after the placement, frozen,
// polled from core_host_prepare_frame - a chat line and the countdown sound at
// each second as before, "Go!" at the end - then core.anchor and the hold
// (core_host_anchor_ready).
void BallanceMMOClient::core_host_countdown() {
    auto& f = anchor_freeze;
    const double elapsed = seconds_since(f.countdown_from, bs::clock_site::retail_prep);
    while (f.countdown_said < PHYSICS_SESSION_COUNTDOWN && elapsed >= f.countdown_said) {
        SendIngameMessage(std::format("Physics session {} - {}", host.session, PHYSICS_SESSION_COUNTDOWN - f.countdown_said),
                          bmmo::ansi::BrightGreen);
        play_countdown_sound(bmmo::countdown_type::Countdown_1);
        ++f.countdown_said;
    }
    if (elapsed < PHYSICS_SESSION_COUNTDOWN) return;
    SendIngameMessage(std::format("Physics session {} - Go!", host.session), bmmo::ansi::BrightGreen);
    play_countdown_sound(bmmo::countdown_type::Go);
    f.prep = anchor_freeze_state::prep_stage::none;
    core_host_anchor_ready();
}

// Every OnProcess, after the legacy session's turn: the 'net' actions armed
// for a tick, the blackouts' ends, then the core session's phase - the frame
// while it runs (which drains the mailbox between post_step and pre_step),
// otherwise the mailbox drained and the countdown's or the restart's checks
// (the legacy process_physics_session).
// Protocol 34: once a frame, before objects_.update.  The players whose
// legacy ball stream we do not need - this session's mirrors while they are
// physicalized here, so the session moves them - go to the server as our mute
// set when it changes, and the server leaves them out of its
// OwnedCompressedBallState (half a member's downstream at 13 players).  Left
// in: a mirror within kAirshipMargin of the airship box, so its stream runs
// again before its body freezes there and the ghost is moved by it from the
// first frame (step BALL).  Our own echo goes too when our own spirit ball is
// hidden.  A player that leaves the set turns back into a legacy ball (a
// death, the airship ghost, the session's end): its ring is seeded with the
// pose it is shown at, so it stays there until the server's catch-up state
// and the stream that follows move it on (game_state::seed).
void BallanceMMOClient::core_host_legacy_mute() {
    constexpr double kAirshipMargin = 4.0;
    double box[6]{};
    const bool airship = core_session_running() && physics_view_.available() && physics_view_.airship_zone_get(box) == 1;
    const auto near_airship = [&](const VxVector& p) {
        return airship && p.x >= box[0] - kAirshipMargin && p.x <= box[3] + kAirshipMargin
            && p.z >= box[2] - kAirshipMargin && p.z <= box[5] + kAirshipMargin;
    };
    std::vector<uint32_t> muted;
    std::map<uint32_t, CK_ID> physicalized;
    for (const auto& b: objects_.shown_balls()) {
        const uint32_t id = static_cast<uint32_t>(b.id);
        if (b.physicalized) {
            physicalized[id] = b.entity;
            if (host.mirror_type.count(id) && !near_airship(b.position)) muted.push_back(id);
            continue;
        }
        const auto was = host.legacy_physicalized.find(id);
        if (was != host.legacy_physicalized.end() && was->second == b.entity
                && std::binary_search(host.legacy_muted.begin(), host.legacy_muted.end(), id))
            db_.seed(b.id, b.type, b.position, b.rotation);
    }
    host.legacy_physicalized = std::move(physicalized);
    if (!muted.empty() && !own_ball_visible_) muted.push_back(static_cast<uint32_t>(db_.get_client_id()));
    std::sort(muted.begin(), muted.end());
    if (muted == host.legacy_muted) return;
    host.legacy_muted = muted;
    bmmo::ball_state_mute_msg msg;
    msg.players = std::move(muted);
    msg.serialize();
    send(msg.raw.str().data(), msg.size(), bmmo::net::kSendReliable);
}

// A new server connection holds no mute set.
void BallanceMMOClient::core_host_legacy_mute_reset() {
    host.legacy_muted.clear();
    host.legacy_physicalized.clear();
}

void BallanceMMOClient::core_host_process() {
    // (Step SP9 also marked the level dirty here while a legacy session ran;
    // FULL step LGM drops that read of the legacy state: a legacy session
    // cannot start since B9DX, and a core anchor marks its own level.)
    if (!host.net_pending.empty()) {
        const uint32_t tick = session_tick_now();
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
    core_host_net_expire();
    // FULL step R6H: 'session hold-event' - a held own event whose time has
    // come goes out (the clock is read only while one is held).
    if (host.core && host.event_hold.pending())
        host.event_hold.release_due(*host.core, []() { return bmmo::retail::host_clock().now(bs::clock_site::hold_event_release); });
    // FULL step AHN (review finding 1): the anchor hold thawed for a
    // SessionStart or this session's SessionEnd it would not handle inside
    // OnRender: drained here first, before a frame of the still-holding core.
    if (anchor_freeze.drain_next_process) {
        anchor_freeze.drain_next_process = false;
        core_host_drain();
        if (core_session_running() && host.core->holding()) {
            // Not reachable: the drain stopped only before an entry that ends
            // this session.  Should the core still hold, the thaw frame has
            // run the world out of the hold; the session cannot start exact.
            logger_->Warn("Physics session %u: the anchor hold was thawed for a session message that did not end "
                          "the session", host.session);
            core_host_end_local("the anchor hold was interrupted");
        } else if (host.phase == retail_host::phase_type::anchoring) {
            core_host_end_local("the level's preparation was interrupted");
        }
    }
    // Step SPL: a failure in the frozen preparation, ended here (not inside the render).
    if (!anchor_freeze.end_next_process.empty()) {
        const std::string why = std::move(anchor_freeze.end_next_process);
        anchor_freeze.end_next_process.clear();
        core_host_end_local(why);
    }
    if (core_session_running()) {
        core_host_frame();
        return;
    }
    // FULL step ESCX: a session that ended with the ESC menu open keeps the
    // ESC block's hold until the menu closes (menu_overlay.hpp, end()).
    if (core_menu_overlay.release(m_bml->GetCKContext(), m_bml->IsIngame() && m_bml->IsPaused()))
        logger_->Info("Physics session: ESC menu overlay: the menu closed after the session's end (or the level was "
                      "left); the ESC hold is released");
    // FULL step ESCK: the key mask likewise, until the menu has closed and
    // every key held across the close has been let go.
    if (core_menu_keys.release(m_bml->GetCKContext()))
        logger_->Info("Physics session: ESC menu keys: the menu closed and its keys were let go after the session's "
                      "end; every key-reading block is handed back");
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
                              level_life.go_waiting ? "; restarting it now" : "");
                if (level_life.go_waiting) {
                    level_life.go_waiting = false;
                    core_host_go();
                }
            } else if (bmmo::retail::host_clock().now(bs::clock_site::retail_deadline) > host.restart_deadline) {
                core_host_end_local(std::format("the level reload did not finish in {} s", kReloadTimeout.count()));
            } else if (!level_request_.error.empty()) {
                core_host_end_local("the level reload failed: " + level_request_.error);
            }
            return;
        }
        // Leaving the level during the lead-in cancels the session instead
        // of restarting something else on "Go!".
        if (!m_bml->IsIngame()) core_host_end_local("the level was left during the countdown");
        else if (bmmo::retail::host_clock().now(bs::clock_site::retail_deadline) > host.restart_deadline) core_host_end_local("the countdown did not finish");
        return;
    case retail_host::phase_type::restarting: {
        const bool active = gameplay_ingame_script_active();
        if (!active) host.saw_ingame_inactive = true;
        else if (host.saw_ingame_inactive) core_host_anchor();
        else if (bmmo::retail::host_clock().now(bs::clock_site::retail_deadline) > host.restart_deadline) core_host_end_local("the level did not restart");
        return;
    }
    case retail_host::phase_type::anchoring:
        // Step SPL: only reachable when the freeze was lost under it.
        core_host_end_local("the level's preparation was interrupted");
        return;
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
// <game>/ModLoader/BMMOCaptures/session_<id>_level<N>_<UTC>_p<own id>.bmic,
// the newest 10 kept; the header names the build and what a replay must set
// as this session found it.
void BallanceMMOClient::core_host_start_capture(const std::string& start_bytes) {
    auto* prop = config_manager_["session_capture"];
    if (!prop || !prop->GetBoolean()) return;
    bmmo::session_start_msg msg{};
    if (!decode_bytes(start_bytes, msg)) return;
    const auto directory = bmmo::retail::journal_files::directory().parent_path() / "BMMOCaptures";
    auto name = bmmo::retail::journal_files::file_name(msg.session, msg.map.level, db_.get_client_id());
    name = name.substr(0, name.rfind('.')) + ".bmic";
    std::string md5;
    for (const auto byte: msg.map.md5) md5 += std::format("{:02x}", static_cast<unsigned>(static_cast<uint8_t>(byte)));
    const std::string header = std::format(
        "bmmo-retail-capture 1 build={} session={} level={} map={} own={} snapshots={} cache={} parked={} trace={} "
        "rollback={} nonblocking={} final_checkpoints={} checkpoint_ticks={} cadence={}",
        physics_view_.build_id(), msg.session, msg.map.level, md5, db_.get_client_id(), snapshots_next.load() ? 1 : 0,
        static_cast<int>(capture_cache_next.load()), own_births_parked_next.load() ? 1 : 0, core_trace ? 1 : 0,
        core_rollback ? 1 : 0, anchor_freeze.nonblocking ? 1 : 0, diag_final_checkpoints() ? 1 : 0,
        journal_checkpoint_ticks(), remote_cadence_next.load())
        + core_host_capture_players() + core_host_capture_requests();
    std::string error;
    auto prune = [directory] { bmmo::retail::session_capture::prune(directory, bmmo::retail::journal_files::kKeepCaptures - 1); };
    if (bmmo::retail::session_capture::instance().start_recording(directory / name, header,
            bs::strand_poster(thread_pool_.get_executor()), std::move(prune), error))
        logger_->Info("Physics session %u: capture %s", msg.session, (directory / name).string().c_str());
    else
        logger_->Warn("Physics session %u: no capture: %s", msg.session, error.c_str());
}

// The journal's LOCAL checkpoint cadence: BMMO_JOURNAL_CHECKPOINT_TICKS (1 ..
// kCheckpointTicks), else the ceiling - or, in a replay, the capture's (K7e:
// the journal it writes is compared with the live one).
uint32_t BallanceMMOClient::journal_checkpoint_ticks() {
    if (replay_checkpoint_ticks != 0) return replay_checkpoint_ticks;
    uint32_t ticks = static_cast<uint32_t>(bmmo::retail::journal_files::kCheckpointTicks);
    if (const char* env = std::getenv("BMMO_JOURNAL_CHECKPOINT_TICKS")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value >= 1 && value <= static_cast<long>(bmmo::retail::journal_files::kCheckpointTicks))
            ticks = static_cast<uint32_t>(value);
    }
    return ticks;
}

// The other players the mod knows (its player list, from the login and the
// PlayerConnected messages before the session, which a capture does not
// hold): " players=<id>:<name in hex>:<cheated>,...", for the replay to make
// them again - their spirit balls are the session's mirrors.
std::string BallanceMMOClient::core_host_capture_players() {
    std::string out;
    db_.for_each([&](const std::pair<const bmmo::net::connection, PlayerState>& pair) {
        if (pair.first == db_.get_client_id()) return true;
        std::string hex;
        for (const auto c: pair.second.name) hex += std::format("{:02x}", static_cast<unsigned>(static_cast<uint8_t>(c)));
        out += std::format("{}{}:{}:{}", out.empty() ? " players=" : ",", pair.first, hex.empty() ? "-" : hex,
                           pair.second.cheated ? 1 : 0);
        return true;
    });
    return out;
}

// The automation requests (789bc114): a gated verb runs when the server's
// verdict for its request id arrives, and the ids count from the game's start
// - a replay, which starts counting at the SessionStart, would match the
// capture's verdicts to other requests.  " requests=<last id>" and the ones
// still waiting " pending=<id>:<line in hex>:<from the console>,...".
std::string BallanceMMOClient::core_host_capture_requests() {
    std::string out = std::format(" requests={}", automation_request_id_);
    for (const auto& [id, request]: automation_pending_) {
        std::string hex;
        for (const auto c: request.first) hex += std::format("{:02x}", static_cast<unsigned>(static_cast<uint8_t>(c)));
        out += std::format("{}{}:{}:{}", out.find(" pending=") == std::string::npos ? " pending=" : ",", id,
                           hex.empty() ? "-" : hex, request.second ? 1 : 0);
    }
    return out;
}

// The session is over (idle again): its capture ends; a replay reports how
// it ended.
void BallanceMMOClient::core_host_capture_idle() {
    auto& capture = bmmo::retail::session_capture::instance();
    if (host.phase != retail_host::phase_type::idle) return;
    if (capture.recording()) {
        capture.stop_recording();
        logger_->Info("Physics session: capture closed%s", capture.write_failed() ? " (a write FAILED: incomplete)" : "");
    } else if (capture.replaying()) {
        capture.end_replay();
        core_host_report_replay();
    }
}

void BallanceMMOClient::core_host_report_replay() {
    auto& capture = bmmo::retail::session_capture::instance();
    const std::string line = capture.divergence().empty()
        ? std::format("Replay: the session ended at frame {}; {}", capture.frames(),
                      capture.replay_complete() ? "the capture's end marker reached: every read matched"
                                                : "the capture holds more (" + capture.where() + ")")
        : "Replay DIVERGED: " + capture.divergence();
    logger_->Info("%s", line.c_str());
    SendIngameMessage(line, capture.divergence().empty() ? bmmo::ansi::BrightGreen : bmmo::ansi::BrightRed);
}

// replay capture <file.bmic> | replay capture status | replay capture stop:
// arm a capture's replay - the level must be the capture's and loaded, and
// no session running; the replay begins at the next drain (the SessionStart
// it holds), with the capture header's switches and own id.
std::string BallanceMMOClient::core_host_capture_verb(const std::string& rest) {
    auto& capture = bmmo::retail::session_capture::instance();
    static const char* const kModes[] = {"off", "recording", "armed", "replaying", "ended"};
    if (rest.empty() || rest == "status")
        return std::format("ok replay capture mode={} frozen={} frames={} path={} divergence={}",
                           kModes[static_cast<int>(capture.state())], replay_stop.frozen ? 1 : 0, capture.frames(),
                           capture.path().empty() ? "-" : capture.path(),
                           capture.divergence().empty() ? "-" : capture.divergence());
    if (rest == "stop") {
        capture.end_replay();
        return "ok replay capture stopped";
    }
    if (rest == "thaw") {
        if (!replay_stop.frozen) return "error the replay is not stopped at a tick";
        core_host_replay_thaw();
        return "ok replay capture thawed: the game runs on, unreplayed";
    }
    if (host.phase != retail_host::phase_type::idle) return "error a session is running";
    // <file> [stop-at <tick>] (the path may hold spaces: the option is read from the end)
    std::string file = rest;
    replay_stop.tick = 0;
    if (const auto at = rest.rfind(" stop-at "); at != std::string::npos) {
        file = rest.substr(0, at);
        replay_stop.tick = static_cast<uint32_t>(std::strtoul(rest.c_str() + at + 9, nullptr, 10));
        if (replay_stop.tick == 0) return "error stop-at needs a tick above 0";
    }
    std::string error;
    if (!capture.arm_replay(file, error, [this](const std::string& why) {
            logger_->Warn("Replay DIVERGED: %s", why.c_str());
            SendIngameMessage("Replay DIVERGED: " + why, bmmo::ansi::BrightRed);
        }))
        return "error " + error;
    std::map<std::string, std::string> h;
    {
        std::istringstream words(capture.header());
        std::string word;
        while (words >> word)
            if (const auto eq = word.find('='); eq != std::string::npos) h[word.substr(0, eq)] = word.substr(eq + 1);
    }
    const auto number = [&](const char* key) { return h.count(key) ? std::strtoll(h[key].c_str(), nullptr, 10) : 0LL; };
    std::string refused;
    if (h["build"] != physics_view_.build_id()) refused = "captured by build " + h["build"] + ", this is " + physics_view_.build_id();
    else if (number("level") != current_map_.level) refused = std::format("captured in level {}, this game is in level {}", h["level"], current_map_.level);
    else if (!m_bml->IsIngame()) refused = "load the capture's level first";
    if (!refused.empty()) {
        capture.end_replay();
        return "error " + refused;
    }
    db_.set_client_id(static_cast<bmmo::net::connection>(number("own")));
    automation_request_id_ = static_cast<uint32_t>(number("requests"));
    automation_pending_.clear();
    {
        std::istringstream pending(h["pending"]);
        std::string request;
        while (std::getline(pending, request, ',')) {
            const auto a = request.find(':'), b = request.rfind(':');
            if (a == std::string::npos || b == a) continue;
            const std::string hex = request.substr(a + 1, b - a - 1);
            std::string line;
            for (size_t i = 0; hex != "-" && i + 1 < hex.size(); i += 2)
                line += static_cast<char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            automation_pending_[static_cast<uint32_t>(std::strtoul(request.substr(0, a).c_str(), nullptr, 10))] =
                {line, request.substr(b + 1) == "1"};
        }
    }
    {
        std::istringstream players(h["players"]);
        std::string player;
        while (std::getline(players, player, ',')) {
            const auto a = player.find(':'), b = player.rfind(':');
            if (a == std::string::npos || b == a) continue;
            const auto id = static_cast<bmmo::net::connection>(std::strtoull(player.substr(0, a).c_str(), nullptr, 10));
            const std::string hex = player.substr(a + 1, b - a - 1);
            std::string name;
            for (size_t i = 0; hex != "-" && i + 1 < hex.size(); i += 2)
                name += static_cast<char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            db_.create(id, name, player.substr(b + 1) == "1");
            objects_.ensure_player(id);
        }
    }
    snapshots_next.store(number("snapshots") != 0);
    capture_cache_next.store(static_cast<uint8_t>(number("cache")));
    own_births_parked_next.store(number("parked") != 0);
    remote_cadence_next.store(static_cast<uint32_t>(std::max(0LL, number("cadence"))));   // older captures: 0
    core_trace = number("trace") != 0;
    core_rollback = number("rollback") != 0;
    anchor_freeze.nonblocking = number("nonblocking") != 0;
    replay_checkpoint_ticks = static_cast<uint32_t>(number("checkpoint_ticks"));
    if (number("final_checkpoints") != (diag_final_checkpoints() ? 1 : 0))
        logger_->Warn("Replay: the capture was taken with final checkpoints %s, this game has them %s: the journals differ there",
                      h["final_checkpoints"].c_str(), diag_final_checkpoints() ? "on" : "off");
    return "ok replay capture armed: " + file + (replay_stop.tick ? std::format(" (stops at tick {})", replay_stop.tick) : std::string{})
         + " (" + capture.header() + ")";
}

void BallanceMMOClient::core_host_drain() {
    // FULL step AHN (review finding 1): inside the frozen anchor hold's
    // OnRender the drain stops before a SessionStart (always a replacement)
    // or a SessionEnd naming the core's session (the one on_session_end ends
    // on); both wait in the mailbox for the next OnProcess.  A SessionEnd of
    // another session goes on to the core, which ignores it.  Where the
    // entry is handled moves, not what the core decides.
    std::deque<mailbox_entry> entries;
    if (anchor_freeze.drain_frozen) {
        const uint32_t own_session = host.core ? host.core->session_id() : 0;
        entries = mailbox.take_until([own_session](const mailbox_entry& e) {
            if (e.code == bmmo::SessionStart) return true;
            if (e.code != bmmo::SessionEnd) return false;
            bmmo::session_end_msg msg{};
            return decode_bytes(e.bytes, msg) && msg.session == own_session;
        }, anchor_freeze.fence_waiting);
    } else {
        entries = mailbox.take();
    }
    // K7d / K7e: the take as the session's capture holds it.  A take with a
    // SessionStart starts a capture (core_host_start_capture) from that entry
    // on - what came before it is not the session's.  A replay begins at its
    // first take, as the capture did, and hands the capture's entries back
    // instead of the mailbox's (no network feeds it).
    {
        auto& capture = bmmo::retail::session_capture::instance();
        if (capture.armed()) capture.begin_replay();
        size_t first = 0;
        bool held = capture.recording() || capture.replaying();
        if (!held) {
            const auto start = std::find_if(entries.begin(), entries.end(),
                                            [](const mailbox_entry& e) { return e.code == bmmo::SessionStart; });
            if (start != entries.end()) {
                first = static_cast<size_t>(start - entries.begin());
                core_host_start_capture(start->bytes);
                held = capture.recording();
            }
        }
        if (held) {
            std::vector<std::string> taken;
            for (size_t i = first; i < entries.size(); ++i) taken.push_back(entries[i].bytes);
            capture.take(bmmo::retail::take_site::mailbox, taken);
            if (capture.replaying()) {
                entries.clear();
                for (auto& bytes: taken) {
                    const int code = bytes.size() >= sizeof(bmmo::opcode)
                        ? static_cast<int>(reinterpret_cast<const bmmo::general_message*>(bytes.data())->code) : -1;
                    entries.push_back({code, std::move(bytes)});
                }
            }
        }
    }
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
        if (entry.code == bmmo::SessionBootStatus) {   // step SPL (protocol 32)
            bmmo::session_boot_status_msg msg{};
            if (decode_bytes(entry.bytes, msg)) core_host_boot_status(msg);
            else if (host.undecodable++ < 8) logger_->Warn("Physics session core: a SessionBootStatus could not be decoded");
            continue;
        }
        if (entry.code == bmmo::SessionEnd && hold_panel_ui.current() == bmmo::retail::hold_panel::stage::server_boot) {
            bmmo::session_end_msg msg{};
            decode_bytes(entry.bytes, msg);
            logger_->Info("Physics session %u: ended before its SessionStart (%s); the boot's panel is taken down",
                          msg.session, msg.reason.c_str());
            hold_panel_ui.hide();
            core_host_start_stage(nullptr);
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
        // Step A5's measure: from the transport's stamp to the handover.
        if (entry.code == bmmo::SessionRemoteInput && entry.received
                && !bmmo::retail::session_capture::instance().replaying())
            host.core->note_inbound_wait((transport().local_timestamp() - entry.received) / 1000.0);
        core_host_dispatch(entry.code, entry.bytes);
    }
    core_host_capture_idle();
    if (replay_stop.tick != 0 && bmmo::retail::session_capture::instance().replaying()
            && session_tick_now() >= replay_stop.tick)
        core_host_replay_freeze();
}

// K7e: the replay stops at its stop tick (after that tick's step) and the world
// freezes there: a debugger, the inspection verbs (the command file, served from
// OnRender while frozen) and the journal see that tick.  'replay capture thaw'
// lets the game run on, unreplayed.
void BallanceMMOClient::core_host_replay_freeze() {
    bmmo::retail::session_capture::instance().end_replay();
    CKTimeManager* tm = m_bml->GetTimeManager();
    replay_stop.saved_limits = tm->GetLimitOptions();
    replay_stop.saved_rate = tm->GetBehavioralRateLimit();
    tm->SetBehavioralRateLimit(0.001f);
    tm->ChangeLimitOptions(CK_RATE_NOP, CK_BEHRATE_LIMIT);
    replay_stop.frozen = true;
    const std::string line = std::format("Replay stopped at tick {} as asked: the world is frozen ('replay capture thaw' "
                                         "lets it run on, unreplayed)", session_tick_now());
    replay_stop.tick = 0;
    logger_->Info("%s", line.c_str());
    SendIngameMessage(line, bmmo::ansi::BrightYellow);
}

void BallanceMMOClient::core_host_replay_thaw() {
    if (!replay_stop.frozen) return;
    CKTimeManager* tm = m_bml->GetTimeManager();
    tm->SetBehavioralRateLimit(replay_stop.saved_rate);
    tm->ChangeLimitOptions(CK_RATE_NOP, static_cast<CK_FRAMERATE_LIMITS>(replay_stop.saved_limits & CK_BEHRATE_MASK));
    replay_stop.frozen = false;
}

// K7d: the connection's state where it steers the session, as a capture
// holds it (a replay has no connection).
bool BallanceMMOClient::core_host_link_up() {
    return bmmo::retail::session_capture::instance().flag(bs::clock_site::retail_connected, connected());
}

// FULL step AHN: IMod::OnRender (BML+'s post-render callback) - while the
// anchor hold is frozen, the frame that polls it.
void BallanceMMOClient::OnRender(CK_RENDER_FLAGS) {
    // The hang watch's beat: every rendered frame, the frozen hold's included.
    char hang_report[384];
    if (NSDumpFile::HangWatchBeat(core_host_session_running(), hang_report, sizeof(hang_report)))
        logger_->Warn("%s", hang_report);
    core_host_hold_frame();
    if (replay_stop.frozen) process_command_file();   // K7e: no OnProcess runs while a stopped replay is frozen
}

// The blackouts of 'net blackout' end at a frame boundary (core_host_process)
// and, while the anchor hold is frozen and no OnProcess runs, in the frame
// that polls it (core_host_hold_frame); the blocking hold still stretches one.
void BallanceMMOClient::core_host_net_expire() {
    if (!host.blackout_up && !host.blackout_down) return;
    const auto now = bmmo::retail::host_clock().now(bs::clock_site::retail_net);
    auto& net = transport();
    if (host.blackout_up && now >= host.blackout_up_until) {
        net.set_fake_loss(bmmo::net::transport::direction::send, 0.0f);
        host.blackout_up = false;
        logger_->Info("Automation: net blackout (up) over at tick %u", session_tick_now());
    }
    if (host.blackout_down && now >= host.blackout_down_until) {
        net.set_fake_loss(bmmo::net::transport::direction::recv, 0.0f);
        host.blackout_down = false;
        logger_->Info("Automation: net blackout (down) over at tick %u", session_tick_now());
    }
}

// The anchor frame's end with `nonblocking_hold` (design 2.3): the world
// frozen and the hold left to core_host_hold_frame.  What runs between here
// and the release: the rest of this OnProcess (the command pipe, the room
// and level requests, the tick record, the local input poll, the legacy
// display's update, the message queue - as the blocking hold ran them after
// its release), then rendered frames only.  No behaviour frame runs, nothing
// is recorded or sent but the mailbox's answers (the core's acknowledgements),
// so the frame after the release is the world's first step.
void BallanceMMOClient::core_host_freeze() {
    auto& f = anchor_freeze;
    if (f.active) return;
    CKTimeManager* tm = m_bml->GetTimeManager();
    f.drain_frozen = f.fence_waiting = f.drain_next_process = false;
    f.prep = anchor_freeze_state::prep_stage::none;
    f.end_next_process.clear();
    f.saved_limits = tm->GetLimitOptions();
    f.saved_beh_rate = tm->GetBehavioralRateLimit();
    tm->SetBehavioralRateLimit(0.001f);   // one behaviour loop per 1000 s
    tm->ChangeLimitOptions(CK_RATE_NOP, CK_BEHRATE_LIMIT);
    f.render_changed = false;
    if (CKRenderContext* rc = m_bml->GetRenderContext()) {
        // The foreground sprites (the HUD and the level's start screens) out
        // of the hold's frames, so the level is seen behind the panel.
        f.saved_render = rc->GetCurrentRenderOptions();
        rc->ChangeCurrentRenderOptions(0, CK_RENDER_FOREGROUNDSPRITES);
        f.render_changed = true;
    }
    if (!f.shot_dir.empty() && !hold_shot.registered)
        if (CKRenderContext* rc = m_bml->GetRenderContext()) {
            hold_shot.pending = hold_shot.written = false;
            rc->AddPostSpriteRenderCallBack(hold_shot_cb, nullptr, FALSE);
            hold_shot.registered = true;
        }
    f.session = host.session;
    f.active = true;
    logger_->Info("Physics session %u: world frozen (the level rendered; limits %08lx -> %08lx, behavioural rate %.3f -> "
                  "0.001; foreground sprites off)", host.session, static_cast<unsigned long>(f.saved_limits),
                  static_cast<unsigned long>(tm->GetLimitOptions()), f.saved_beh_rate);
}

// The hold proper: core.anchor sent SessionReady.  Step SPL: the world is
// frozen already by the preparation (core_host_freeze at the anchor); the
// blocking anchor's caller has not frozen it.
void BallanceMMOClient::core_host_hold_arm() {
    auto& f = anchor_freeze;
    CKContext* ctx = m_bml->GetCKContext();
    CKTimeManager* tm = m_bml->GetTimeManager();
    core_host_freeze();
    f.prep = anchor_freeze_state::prep_stage::none;
    const auto t0 = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    f.at_anchor = hold_capture(ctx, physics_view_, host.script_ids, host.script_hooks, tm);
    f.anchor_capture_ms = std::chrono::duration<double, std::milli>(bmmo::retail::host_clock().now(bs::clock_site::retail_display) - t0).count();
    f.frames = f.polls = 0;
    f.gaps.clear();
    f.next_shot = 0;
    f.driver_released = f.driver_at_instant = false;
    f.armed_at = bmmo::retail::host_clock().now(bs::clock_site::retail_hold_armed);
    ++f.holds;
    hold_panel_ui.armed(host.core ? static_cast<int>(host.core->config.level) : 0,
                        "level synchronized, waiting for the server.", f.armed_at);
    core_host_start_stage("waiting for the server and the other players");
    logger_->Info("Physics session %u: anchor hold armed (non-blocking: the world frozen, the level rendered; limits "
                  "%08lx, behavioural rate %.3f saved); anchor %s (captured in %.2f ms)", host.session,
                  static_cast<unsigned long>(f.saved_limits), f.saved_beh_rate, f.at_anchor.describe().c_str(),
                  f.anchor_capture_ms);
}

// The world thawed: the behaviour loop's limits and the render options back.
// Idempotent; every way out of a frozen hold passes here (the release, the
// session's end, a replacing SessionStart), or the game would stay frozen.
void BallanceMMOClient::core_host_hold_unfreeze(const char* why) {
    auto& f = anchor_freeze;
    if (!f.active) return;
    f.active = false;
    f.prep = anchor_freeze_state::prep_stage::none;
    hold_panel_ui.hide();
    CKTimeManager* tm = m_bml->GetTimeManager();
    tm->SetBehavioralRateLimit(f.saved_beh_rate);
    tm->ChangeLimitOptions(CK_RATE_NOP, static_cast<CK_FRAMERATE_LIMITS>(f.saved_limits & CK_BEHRATE_MASK));
    if (f.render_changed)
        if (CKRenderContext* rc = m_bml->GetRenderContext()) rc->SetCurrentRenderOptions(f.saved_render);
    f.render_changed = false;
    if (hold_shot.registered) {
        if (CKRenderContext* rc = m_bml->GetRenderContext()) rc->RemovePostSpriteRenderCallBack(hold_shot_cb, nullptr);
        hold_shot.registered = hold_shot.pending = false;
    }
    const CKDWORD limits = tm->GetLimitOptions();
    logger_->Info("Physics session %u: anchor hold thawed (%s): limits %08lx, behavioural rate %.3f", f.session, why,
                  static_cast<unsigned long>(limits), tm->GetBehavioralRateLimit());
    if ((limits & CK_BEHRATE_MASK) != (f.saved_limits & CK_BEHRATE_MASK))
        logger_->Warn("Physics session %u: the behaviour loop's limits did not come back (%08lx, were %08lx)", f.session,
                      static_cast<unsigned long>(limits), static_cast<unsigned long>(f.saved_limits));
}

// One rendered frame of the frozen anchor hold: the mailbox polled every
// 1 ms for up to 15 ms (the blocking loop's resolution) with the blocking
// loop's exits - released (the start instant or the tick base), the
// connection lost (core.release_hold), the 10 s timeout - then BML's message
// queue, one ImGui frame (BML's UI and the wait panel) and the armed
// screenshots.  At the release: thawed, the world compared with the
// anchor's, the tick driver released (from the start instant when
// hooks.numbering_restarted gave it) and the first pre_step.
void BallanceMMOClient::core_host_hold_frame() {
    auto& f = anchor_freeze;
    if (!f.active) return;
    if (f.prep != anchor_freeze_state::prep_stage::none) {
        core_host_prepare_frame();
        return;
    }
    const auto frame_start = bmmo::retail::host_clock().now(bs::clock_site::retail_hold_frame);
    if (f.frames > 0) f.gaps.push_back(std::chrono::duration<double, std::milli>(frame_start - f.last_frame).count());
    f.last_frame = frame_start;
    if (f.frames++ == 0) {
        // The freeze's reference: the world at the anchor frame's end.  The
        // rest of the anchor frame's OnProcess (the legacy display's update -
        // the other players' name labels, 2D - among others) ran since the
        // anchor's capture, as it ran after the blocking hold; that part is
        // logged, not judged.
        f.at_first = hold_capture(m_bml->GetCKContext(), physics_view_, host.script_ids, host.script_hooks,
                                  m_bml->GetTimeManager());
        const std::string moved = f.at_first.diff(f.at_anchor);
        if (!moved.empty())
            logger_->Info("Physics session %u: anchor hold: the rest of the anchor frame (after core_host_anchor) "
                          "changed: %s; the freeze is judged from the first hold frame: %s", f.session, moved.c_str(),
                          f.at_first.describe().c_str());
    }
    enum class outcome { held, released, ended, deferred } out = outcome::held;
    while (true) {
        ++f.polls;
        core_host_net_expire();   // no OnProcess runs while frozen
        f.drain_frozen = true;
        core_host_drain();
        f.drain_frozen = false;
        if (f.fence_waiting) {
            out = outcome::deferred;   // a SessionStart / this session's SessionEnd: the next OnProcess's
            break;
        }
        if (!f.active || !host.core || host.phase != retail_host::phase_type::running) {
            out = outcome::ended;   // ended or replaced meanwhile (core_host_end_local thawed it)
            break;
        }
        if (!host.core->holding()) {
            out = outcome::released;
            break;
        }
        if (!core_host_link_up()) {
            host.core->release_hold("the connection was lost");
            out = outcome::released;
            break;
        }
        const auto state = host.core->poll_hold();
        if (state.timed_out) {
            SendIngameMessage("Physics session: the server did not assign a tick base; starting out of phase.",
                              bmmo::ansi::BrightYellow);
            out = outcome::released;
            break;
        }
        if (!state.held) {
            out = outcome::released;
            break;
        }
        if (bmmo::retail::host_clock().now(bs::clock_site::retail_hold_frame) - frame_start >= std::chrono::milliseconds(15)) break;
        bmmo::retail::host_clock().sleep_for(bs::clock_site::retail_hold_frame, std::chrono::milliseconds(1));
    }
    {
        std::lock_guard lk(ingame_msg_mtx_);
        while (!ingame_msg_queue_.empty()) {
            m_bml->SendIngameMessage(ingame_msg_queue_.front().c_str());
            ingame_msg_queue_.pop();
        }
    }
    {
        // One ImGui frame in BML+'s context: BML's own UI (the message area)
        // drawn by its built-in mod, and the wait panel while held; BML's
        // post-sprite render draws it.  At the release one more frame without
        // the panel, so it is gone at the start.
        Bui::ImGuiContextScope scope;
        if (ImGui::GetCurrentContext()) {
            ImGui::NewFrame();
            if (IMod* bml_mod = m_bml->FindMod("BML")) bml_mod->OnProcess();
            if (out == outcome::held) hold_panel_ui.draw(bmmo::retail::host_clock().now(bs::clock_site::retail_display), connected() ? get_ping() : 0);
            ImGui::Render();
        }
    }
    if (hold_shot.written) {
        hold_shot.written = false;
        logger_->Info("Physics session %u: anchor hold screenshot %s written %.3f s into the hold", f.session,
                      hold_shot.path.c_str(), std::chrono::duration<double>(hold_shot.written_at - f.armed_at).count());
    }
    if (out == outcome::held) {
        const double held_s = std::chrono::duration<double>(bmmo::retail::host_clock().now(bs::clock_site::retail_hold_armed) - f.armed_at).count();
        if (hold_shot.registered && !hold_shot.pending && f.next_shot < std::size(kHoldShotSeconds)
                && held_s >= kHoldShotSeconds[f.next_shot]) {
            hold_shot.path = std::format("{}/hold_s{}_{}ms.bmp", f.shot_dir, f.session,
                                         static_cast<int>(kHoldShotSeconds[f.next_shot] * 1000.0 + 0.5));
            hold_shot.pending = true;
            logger_->Info("Physics session %u: anchor hold screenshot %s due at %.3f s into the hold (frame %llu)",
                          f.session, hold_shot.path.c_str(), held_s, static_cast<unsigned long long>(f.frames));
            ++f.next_shot;
        }
        return;
    }
    if (out == outcome::ended) {
        ++f.ended;
        core_host_hold_unfreeze("the session ended during the hold");
        return;
    }
    if (out == outcome::deferred) {
        // The world thawed with the core still holding: the next
        // CKContext::Process runs, and its OnProcess (core_host_process)
        // drains the waiting SessionStart / SessionEnd first, which ends this
        // session (core_host_end_local) outside the render.
        ++f.deferred;
        f.fence_waiting = false;
        f.drain_next_process = true;
        logger_->Info("Physics session %u: anchor hold: a SessionStart or this session's SessionEnd arrived %.3f s into "
                      "the hold; the world thawed and the next OnProcess handles it", f.session,
                      std::chrono::duration<double>(bmmo::retail::host_clock().now(bs::clock_site::retail_display) - f.armed_at).count());
        core_host_hold_unfreeze("a session message waits for the next OnProcess");
        return;
    }
    core_host_hold_unfreeze("released");
    core_host_start_stage(nullptr);
    const auto t0 = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    const hold_digest now = hold_capture(m_bml->GetCKContext(), physics_view_, host.script_ids, host.script_hooks,
                                         m_bml->GetTimeManager());
    const double capture_ms = std::chrono::duration<double, std::milli>(bmmo::retail::host_clock().now(bs::clock_site::retail_display) - t0).count();
    const bool equal = now.same_world(f.at_first);
    ++(equal ? f.equal : f.different);
    std::vector<double> sorted = f.gaps;
    std::sort(sorted.begin(), sorted.end());
    const double p50 = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
    const double mx = sorted.empty() ? 0.0 : sorted.back();
    f.last = std::format("{} after {:.3f} s, {} frame(s) rendered, frame_interval n={} p50={:.2f} max={:.2f} ms, "
                         "{} poll(s)", equal ? "EQUAL" : "DIFFERENT",
                         std::chrono::duration<double>(t0 - f.armed_at).count(), f.frames, sorted.size(), p50, mx,
                         f.polls);
    logger_->Info("Physics session %u: anchor hold released (non-blocking): world at the release %s to the anchor "
                  "frame's end; release capture %.2f ms; tick driver from %s", f.session, f.last.c_str(), capture_ms,
                  f.driver_released ? (f.driver_at_instant ? "the start instant" : "the release") : "the release (set_hold)");
    if (!equal) {
        logger_->Warn("Physics session %u: anchor hold: differs in %s", f.session, now.diff(f.at_first).c_str());
        logger_->Warn("Physics session %u: anchor hold: anchor frame's end %s", f.session, f.at_first.describe().c_str());
        logger_->Warn("Physics session %u: anchor hold: release           %s", f.session, now.describe().c_str());
    } else {
        logger_->Info("Physics session %u: anchor hold: %s", f.session, now.describe().c_str());
    }
    if (!host.core || host.phase != retail_host::phase_type::running) return;
    // The driver's schedule continues from the start instant (released by
    // hooks.numbering_restarted) or from now: the wait is not lag, and
    // `rebases` must not move.
    fixed_tick_.set_hold(false);
    host.last_rebases = fixed_tick_.rebases();
    if (host.core->session_phase() != bs::client_core_state::phase::running) return;
    // The tick base's frame (design 1.3 "the anchor frame"), as the blocking
    // hold's tail: the next CKContext::Process steps it.
    core_manager_trace_due = false;
    host.core->pre_step();
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
    // The pause chain's clock and script edits (FULL step LGM: the core
    // host's own, session/pause_chain.hpp; the legacy members' rule, order and
    // lines).
    const bmmo::retail::pause_chain::log_fn pause_log = [this](int severity, const std::string& line) {
        core_log_line(logger_, severity, line);
    };
    if (!core_pause_chain.clock_resolve(m_bml->GetScriptByName("Event_handler"), pause_log))
        logger_->Warn("Physics session: the pause chain's time factor block was not found");
    else {
        float factor = 0.0f, delta = 0.0f;
        if (physics_view_.get_clock(factor, delta, error)) core_pause_chain.clock_apply(m_bml->GetCKContext(), factor);
    }
    core_pause_chain.scripts_restore(m_bml->GetCKContext());
    core_pause_chain.scripts_resolve(m_bml->GetCKContext(), m_bml->GetScriptByName("Event_handler"), pause_log);
    // FULL step ESCX (RK3E): the rest of the vanilla pause taken out - the
    // ESC re-pause, the tutorial's stop, the points timer's (menu_overlay.hpp).
    for (const std::string& line: core_menu_overlay.resolve(m_bml->GetCKContext())) {
        if (line.rfind("ok ", 0) == 0)
            logger_->Info("Physics session: ESC menu overlay: %s", line.c_str() + 3);
        else
            logger_->Error("Physics session: ESC menu overlay: %s", line.size() > 6 ? line.c_str() + 6 : line.c_str());
    }
    // FULL step ESCK: the menu's keys reach the menu only (menu_keys.hpp);
    // ESCX's ESC block keeps its hold instead.
    {
        IBML* bml = m_bml;
        for (const std::string& line: core_menu_keys.resolve(
                     m_bml->GetCKContext(), [bml]() { return bml->IsIngame() && bml->IsPaused(); },
                     core_menu_overlay.esc_block())) {
            if (line.rfind("ok ", 0) == 0)
                logger_->Info("Physics session: ESC menu %s", line.c_str() + 3);
            else
                logger_->Error("Physics session: ESC menu %s", line.size() > 6 ? line.c_str() + 6 : line.c_str());
        }
    }
    // FULL step RTDF (decision 65): Ball Navigation's shared Physics WakeUp
    // acts on no own ball (own_wake_guard.hpp) - the own replica makes every
    // wake of the own ball, in every world alike.
    for (const std::string& line: core_own_wake.resolve(m_bml->GetCKContext(), [this](CK3dEntity* target) {
             CK3dObject* ball = get_current_ball();
             return host.phase != retail_host::phase_type::idle && ball && target == static_cast<CK3dEntity*>(ball);
         })) {
        if (line.rfind("ok ", 0) == 0)
            logger_->Info("Physics session: own wake: %s", line.c_str() + 3);
        else
            logger_->Error("Physics session: own wake: %s", line.size() > 6 ? line.c_str() + 6 : line.c_str());
    }
    // N1, N2 (FULL step LGM: the shared helpers the headless client uses,
    // game/sector_reset_guard.hpp and game/sector_deactivate_pin.hpp, in
    // file-scope state), N3 and N4 handed back from a previous anchor first,
    // then made again - the legacy order.
    core_reset_restore(m_bml->GetCKContext());
    core_deactivate_restore(m_bml->GetCKContext(), logger_);
    if (!core_checkpoint_hold.restore(m_bml->GetCKContext()))   // FULL step N3K
        logger_->Warn("Physics session: the Script input of activate Sektor could not be handed back to its retail "
                      "source; the session's parameter is left in place");
    if (core_checkpoint_phase.applied() && !core_checkpoint_phase.restore(m_bml->GetCKContext()))
        logger_->Warn("Physics session: a checkpoint phase target could not be handed back to CurrentLevel; the "
                      "session's copy is left in place");
    const bool death_neutralized = core_reset_resolve(m_bml->GetCKContext(), logger_);
    const bool sector_neutralized = core_deactivate_resolve(m_bml->GetCKContext(), logger_);
    if (death_neutralized && sector_neutralized)
        logger_->Info("Physics session: retail world resets neutralized: %d sector reset(s) in Gameplay_Ingame no "
                      "longer run Gameplay_SectorManager (a death and a level reset), and %d chain(s) activate a "
                      "sector without deactivating anything (a checkpoint and a level reset)",
                      static_cast<int>(core_reset_guard.count()), static_cast<int>(core_deactivate_pins.size()));
    // Sector activation as an input (N3; FULL step N3K: the shared helper):
    // the checkpoint's run of the manager is held, so it can be stamped ahead
    // and run at the stamped tick.
    {
        std::string hold_error;
        if (core_checkpoint_hold.apply(m_bml->GetCKContext(), hold_error))
            logger_->Info("Physics session: the checkpoint's sector activation (Gameplay_Events/activate Sektor -> "
                          "Execute Script Gameplay_SectorManager) is held: a crossed checkpoint stamps its Sector event "
                          "%u ticks ahead and the manager runs at that tick",
                          static_cast<unsigned>(core.sector_lead_ticks()));
        else
            logger_->Error("Physics session: the checkpoint's sector activation was NOT taken over (%s): this client "
                           "will activate a sector the moment its ball crosses the line, the server a trip later, and "
                           "the sector's mechanisms run out of phase", hold_error.c_str());
    }
    // N4, the checkpoint chain's phase cell split (game/checkpoint_phase.hpp;
    // FULL step LGM: the core host's own split, the legacy resolve's lines).
    {
        const std::string trouble = core_checkpoint_phase.apply(m_bml->GetCKContext());
        if (trouble.empty())
            logger_->Info("Physics session: the checkpoint chain's phase cell is split: %s",
                          core_checkpoint_phase.describe().c_str());
        else
            logger_->Error("Physics session: the checkpoint chain's phase cell was NOT split (%s): a crossing 3 ticks "
                           "after a sector pass can cancel that pass's module Physicalize on this client",
                           trouble.c_str());
    }
    // Step B2SX: the held chain's [0][1] write to a private cell, which the
    // core's own sector poll reads (platform poll_sector); the manager reads
    // only the union's passes.  Only where the hold is in force.
    core_chain_cell.restore(m_bml->GetCKContext());
    if (core_checkpoint_hold.held()) {
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
    // The level file's counters (HU1 (b), FULL step LGM: the shared restore
    // the server's world and the headless client call, on this host's own
    // record; the Sequencers first, then the proximity countdowns), and the
    // retail census the other hosts do not log: a level proximity born after
    // the record window keeps this client's phase.
    {
        CKContext* context = m_bml->GetCKContext();
        bmmo::game::restore_level_file_counters(context, core_level_sequencers, level_proximity_state,
                                                [this](const std::string& line) {
                                                    logger_->Info("Physics session: %s", line.c_str());
                                                });
        int unrecorded = 0, other_owner = 0;
        std::string other_names;
        level_proximity_state.census(context, unrecorded, other_owner, &other_names);
        logger_->Info("Physics session: proximity countdowns: %d not a level object's, left alone (%s)", other_owner,
                      other_names.c_str());
        if (unrecorded > 0)
            logger_->Warn("Physics session: %d level proximity countdown(s) appeared after the record window and keep "
                          "this client's phase (the engine records on every load tick): this level needs a longer "
                          "window", unrecorded);
    }
    // Step SP10 (sector placement design 2.3, 2.4): after the Sequencer and
    // proximity restores (a copy inherits its source's level-file counters),
    // before the hash, the selection and every pass.  The core decides
    // (placement_active: the world option and the capability); the apply
    // with its hot-source hooks, the option-off release policy and the
    // signature are the shared helper's (placement_start / placement_finish,
    // one copy for every host); a refusal ends the session, as a failed hash
    // does.
    {
        const bmmo::physics::physics_view* view = &physics_view_;
        placement_host.active = core.placement_active();
        ++placement_host.anchors;
        if (!bmmo::game::placement_start(placement_host.placement, m_bml->GetCKContext(), view, placement_host.active, -1,
                                         [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); },
                                         error)) {
            ++placement_host.refused;
            core_host_end_local(error);
            return;
        }
    }
    // Step SPL (the user, 2026-11-04): with the non-blocking hold (the
    // default) the world freezes here, the copies are made a slice per
    // rendered frame behind the panel's progress bar, then the 3-2-1, and the
    // rest of the anchor runs in the frozen frames (core_host_prepare_frame).
    // The blocking hold ('session hold blocking', a control arm) places in
    // this frame and has no countdown.
    if (core.capabilities().nonblocking_hold) {
        auto& f = anchor_freeze;
        core_host_freeze();
        host.phase = retail_host::phase_type::anchoring;
        f.prep = anchor_freeze_state::prep_stage::placing;
        f.prep_from = bmmo::retail::host_clock().now(bs::clock_site::retail_prep);
        f.prep_frames = 0;
        hold_panel_ui.placing(placement_host.placement.copies_done(), placement_host.placement.copies_planned(), f.prep_from);
        core_host_start_stage("placing the mechanisms");
        return;
    }
    std::string refused;
    while (refused.empty() && placement_host.placement.steps_left())
        refused = placement_host.placement.step(m_bml->GetCKContext(), std::chrono::steady_clock::duration::max());
    if (const std::string trouble = core_host_anchor_finish(refused); !trouble.empty()) {
        core_host_end_local(trouble);
        return;
    }
    core_host_anchor_ready();
}

// Step SPL: the anchor after its placement - the placement kept (or its
// refusal), the world hash, the collision filter, the selection and the
// rewires, the report - in the anchor frame (blocking hold) or the frozen
// frame the last copy was made in.  Returns what must end the session, or
// nothing.
std::string BallanceMMOClient::core_host_anchor_finish(const std::string& step_refused) {
    auto& core = *host.core;
    std::string error;
    {
        if (!bmmo::game::placement_finish(placement_host.placement, m_bml->GetCKContext(), placement_host.active, step_refused,
                                          [this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); },
                                          placement_host.signature, error)) {
            ++placement_host.refused;
            return error;
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
    if (!physics_view_.capture(hash, error)) return "world hash failed: " + error;
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
    cells_memo_host.drop();   // step OPB: a new selection, a new memo
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
    // here, as a refused placement does.  Every session runs the union
    // since step UOFF (the core refuses a SessionStart without the bit), so
    // proximity_union_active() is this host's place_probes capability (a
    // previous anchor's union was released at its session's end).
    uint64_t probe_signature = 0;
    proximity_release(m_bml->GetCKContext(), logger_, "a new anchor");
    if (core.proximity_union_active()) {
        const bmmo::game::script_replay::selection sel = host.script_ids.empty()
            ? bmmo::game::session_script_selection(m_bml->GetCKContext(), trafo_host.on) : host.script_ids;
        if (!proximity_rewire(m_bml->GetCKContext(), physics_view_, sel, core.mechanism_slots(), logger_, error)) return error;
        probe_signature = proximity_host.signature;
    }
    host.script_hooks.slots = proximity_host.union_.slots() ? &proximity_host.union_.recorded_slots() : nullptr;
    proximity_host.union_.recorded_slots().select(bmmo::session::bound_member_slots(core.member_lives(), core.next_tick()));
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
    bs::anchor_report& report = anchor_freeze.report;
    report = {};
    report.hash = hash;
    report.physics_sha256 = physics_view_.dll_sha256();
    report.build_id = physics_view_.build_id();
    report.own_name = get_display_nickname();
    report.placement = placement_host.signature;   // step SP10 (caps.placement)
    report.probes = probe_signature;               // step WG4 (0 with the bit clear)
    if (CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter")) parameters->GetElementValue(0, 1, &report.sector_cell);
    return {};
}

// Step SPL: the anchor's SessionReady half - the host running, the anchor's
// bodies logged, core.anchor (SessionReady, the journal, the hold armed) -
// then the hold: the frozen one (the world frozen since the preparation) or
// the blocking loop.
void BallanceMMOClient::core_host_anchor_ready() {
    auto& core = *host.core;
    const bs::anchor_report report = anchor_freeze.report;
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
    // FULL step R6H: 'session hold-event' counts from the anchor, where the
    // headless client restarts its count (the spec stays armed).
    host.event_hold.restart_count();
    core.anchor(report);
    logger_->Info("Physics session %u: anchored (hash %016llx surfaces %016llx), waiting for the tick assignment",
                  core.session_id(), static_cast<unsigned long long>(report.hash.pose),
                  static_cast<unsigned long long>(report.hash.surfaces));
    SendIngameMessage("Physics session: level synchronized, waiting for the server.", bmmo::ansi::BrightGreen);
    // FULL step AHN: the non-blocking hold (the default): the world frozen
    // here and the hold polled from OnRender (core_host_hold_frame) while the
    // level renders and the wait panel shows.
    if (core.capabilities().nonblocking_hold && core.holding()) {
        core_host_hold_arm();
        return;
    }
    core_host_hold_unfreeze("the core holds no anchor");   // step SPL: the preparation's freeze, if any
    // The blocking hold (design 2.3; 'session hold blocking'): no behaviour frame runs, nothing is
    // recorded or sent, so the next frame is the world's first step.
    while (host.core && host.phase == retail_host::phase_type::running && host.core->holding()) {
        core_host_drain();
        if (!host.core || host.phase != retail_host::phase_type::running) return;   // ended or replaced meanwhile
        if (!host.core->holding()) break;
        if (!core_host_link_up()) {
            host.core->release_hold("the connection was lost");
            break;
        }
        if (host.core->poll_hold().timed_out) {
            SendIngameMessage("Physics session: the server did not assign a tick base; starting out of phase.",
                              bmmo::ansi::BrightYellow);
            break;
        }
        bmmo::retail::host_clock().sleep_for(bs::clock_site::retail_blocking_wait, std::chrono::milliseconds(1));
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

// Step SPL: one rendered frame of the anchor's frozen preparation - the
// mailbox polled once (a SessionStart or this session's SessionEnd thaws for
// the next OnProcess, as in the hold), a slice of the placement (at least one
// copy, kPlacementSlice), the rest of the anchor after the last copy, the
// 3-2-1 and at its end core.anchor and the hold; then BML's message queue and
// one ImGui frame with the panel.  A failure ends the session in the next
// OnProcess (finding 1: no teardown inside the render).
void BallanceMMOClient::core_host_prepare_frame() {
    using prep_stage = anchor_freeze_state::prep_stage;
    auto& f = anchor_freeze;
    ++f.prep_frames;
    core_host_net_expire();   // no OnProcess runs while frozen
    f.drain_frozen = true;
    core_host_drain();
    f.drain_frozen = false;
    const bool deferred = f.fence_waiting;
    if (!deferred && (!f.active || !host.core || host.phase != retail_host::phase_type::anchoring)) return;   // ended meanwhile
    std::string trouble;
    if (!deferred && !core_host_link_up()) trouble = "the connection was lost while the level was prepared";
    if (!deferred && trouble.empty() && f.prep == prep_stage::placing) {
        auto& placement = placement_host.placement;
        std::string refused;
        if (placement.steps_left()) refused = placement.step(m_bml->GetCKContext(), kPlacementSlice);
        hold_panel_ui.placing(placement.copies_done(), placement.copies_planned(), bmmo::retail::host_clock().now(bs::clock_site::retail_display));
        if (!refused.empty() || !placement.steps_left()) {
            const auto placed_at = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
            trouble = core_host_anchor_finish(refused);
            if (trouble.empty()) {
                logger_->Info("Physics session %u: placement made in %.3f s over %llu rendered frame(s); the rest of the "
                              "anchor took %.1f ms; the 3-2-1 begins", host.session, seconds_since(f.prep_from),
                              static_cast<unsigned long long>(f.prep_frames), seconds_since(placed_at) * 1000.0);
                f.prep = prep_stage::countdown;
                f.countdown_from = bmmo::retail::host_clock().now(bs::clock_site::retail_prep);
                f.countdown_said = 0;
                hold_panel_ui.countdown(f.countdown_from, f.countdown_from + std::chrono::seconds(PHYSICS_SESSION_COUNTDOWN));
                core_host_start_stage("counting down 3-2-1");
            }
        }
    }
    if (!deferred && trouble.empty() && f.prep == prep_stage::countdown) core_host_countdown();
    {
        std::lock_guard lk(ingame_msg_mtx_);
        while (!ingame_msg_queue_.empty()) {
            m_bml->SendIngameMessage(ingame_msg_queue_.front().c_str());
            ingame_msg_queue_.pop();
        }
    }
    {
        // One ImGui frame in BML+'s context, as the hold's (core_host_hold_frame).
        Bui::ImGuiContextScope scope;
        if (ImGui::GetCurrentContext()) {
            ImGui::NewFrame();
            if (IMod* bml_mod = m_bml->FindMod("BML")) bml_mod->OnProcess();
            if (!deferred && trouble.empty() && hold_panel_ui.visible())
                hold_panel_ui.draw(bmmo::retail::host_clock().now(bs::clock_site::retail_display), connected() ? get_ping() : 0);
            ImGui::Render();
        }
    }
    core_host_stage_shot();
    if (deferred) {
        ++f.deferred;
        f.fence_waiting = false;
        f.drain_next_process = true;
        logger_->Info("Physics session %u: the level's preparation: a SessionStart or this session's SessionEnd arrived "
                      "%.3f s in; the world thawed and the next OnProcess handles it", f.session, seconds_since(f.prep_from));
        core_host_hold_unfreeze("a session message waits for the next OnProcess");
        return;
    }
    if (!trouble.empty()) {
        logger_->Warn("Physics session %u: the level's preparation failed (%s); the session ends in the next OnProcess",
                      f.session, trouble.c_str());
        f.end_next_process = trouble;
        core_host_hold_unfreeze("the preparation failed");
    }
}

// Step SPL (protocol 32): a phase of the server's world boot began; the panel
// shows it with the room's members until the SessionStart.
void BallanceMMOClient::core_host_boot_status(const bmmo::session_boot_status_msg& msg) {
    if (host.phase != retail_host::phase_type::idle) return;   // a session runs here: its panel stays
    std::vector<bmmo::retail::hold_panel::member> members;
    {
        std::lock_guard lk(room_state_mtx_);
        for (const auto& m: last_room_state_.members) {
            bmmo::retail::hold_panel::member member;
            member.own = m.id == db_.get_client_id();
            member.name = member.own ? get_display_nickname() : m.name;
            members.push_back(std::move(member));
        }
    }
    boot_last = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    hold_panel_ui.boot(msg.level, msg.phase, std::move(members), boot_last);
    static constexpr const char* kPhases[] = {"the server boots the game", "the server enters the level",
                                              "the server prepares its world"};
    core_host_start_stage(kPhases[std::min<size_t>(msg.phase, 2)]);
}

// Step SPL: the start's timeline in the log - one line per panel stage, what
// the one before took, and the whole at the end (nullptr: the start is over,
// released or abandoned).
void BallanceMMOClient::core_host_start_stage(const char* name) {
    auto& t = start_timeline;
    const auto now = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    if (!t.stage.empty())
        logger_->Info("Physics session start: '%s' took %.3f s", t.stage.c_str(),
                      std::chrono::duration<double>(now - t.since).count());
    if (!name) {
        if (!t.stage.empty())
            logger_->Info("Physics session start: %.3f s from the first stage to here",
                          std::chrono::duration<double>(now - t.first).count());
        t = {};
        return;
    }
    if (t.stage.empty()) t.first = now;
    logger_->Info("Physics session start: '%s' from +%.3f s", name, std::chrono::duration<double>(now - t.first).count());
    t.stage = name;
    t.since = now;
}

// Step SPL: OnProcess - the start panel in the game's own ImGui frame while
// the world runs (the server's boot, the level's restart or reload); the
// frozen stages draw in core_host_hold_frame.
void BallanceMMOClient::core_host_panel_frame() {
    using stage = bmmo::retail::hold_panel::stage;
    if (anchor_freeze.active) return;
    const auto now = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    bool in_room = false;
    {
        std::lock_guard lk(room_state_mtx_);
        in_room = last_room_state_.own_room != 0;
    }
    if (hold_panel_ui.current() == stage::server_boot && (!connected() || !in_room || now - boot_last > kBootPanelTimeout)) {
        logger_->Info("Physics session: the server's boot panel is taken down (%s)",
                      !connected() ? "disconnected" : !in_room ? "not in a room any more" : "no word from the server in 180 s");
        hold_panel_ui.hide();
        core_host_start_stage(nullptr);
    }
    if (hold_panel_ui.current() == stage::restarting && host.phase != retail_host::phase_type::counting_down
            && host.phase != retail_host::phase_type::restarting)
        hold_panel_ui.hide();
    const bool shown = hold_panel_ui.current() == stage::server_boot || hold_panel_ui.current() == stage::restarting;
    if (shown) {
        Bui::ImGuiContextScope scope;
        if (ImGui::GetCurrentContext()) hold_panel_ui.draw(now, connected() ? get_ping() : 0);
    }
    core_host_stage_shot();
    if (!shown && hold_shot.registered) {
        if (CKRenderContext* rc = m_bml->GetRenderContext()) rc->RemovePostSpriteRenderCallBack(hold_shot_cb, nullptr);
        hold_shot.registered = hold_shot.pending = false;
    }
}

// Step SPL: 'session hold shots DIR' - one back-buffer dump per start stage,
// kStageShotSeconds into it (the hold keeps its own, kHoldShotSeconds).
void BallanceMMOClient::core_host_stage_shot() {
    using stage = bmmo::retail::hold_panel::stage;
    if (hold_shot.written) {
        hold_shot.written = false;
        logger_->Info("Physics session: start screenshot %s written", hold_shot.path.c_str());
    }
    const stage now_stage = hold_panel_ui.current();
    if (anchor_freeze.shot_dir.empty() || now_stage == stage::none || now_stage == stage::holding) return;
    if (static_cast<int>(now_stage) != stage_shot.stage) {
        stage_shot.stage = static_cast<int>(now_stage);
        stage_shot.taken = false;
    }
    if (stage_shot.taken || hold_shot.pending) return;
    const double in_stage = seconds_since(hold_panel_ui.stage_since());
    if (in_stage < kStageShotSeconds) return;
    CKRenderContext* rc = m_bml->GetRenderContext();
    if (!rc) return;
    if (!hold_shot.registered) {
        hold_shot.pending = hold_shot.written = false;
        rc->AddPostSpriteRenderCallBack(hold_shot_cb, nullptr, FALSE);
        hold_shot.registered = true;
    }
    static constexpr const char* kNames[] = {"none", "boot", "restart", "placing", "countdown", "hold"};
    hold_shot.path = std::format("{}/start_{:02}_{}.bmp", anchor_freeze.shot_dir, ++stage_shot.count,
                                 kNames[static_cast<size_t>(now_stage)]);
    hold_shot.pending = true;
    stage_shot.taken = true;
    logger_->Info("Physics session: start screenshot %s due (%.3f s into the stage)", hold_shot.path.c_str(), in_stage);
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
    const auto frame_started = bmmo::retail::host_clock().now(bs::clock_site::retail_display);
    if (frame_times.session != host.session) {   // a host whose session_start did not reset them
        frame_times.frame.reset();
        frame_times.interval.reset();
        frame_times.session = host.session;
        frame_times.have_last = false;
    }
    double interval_ms = -1.0;
    if (frame_times.have_last) {
        interval_ms = std::chrono::duration<double, std::milli>(frame_started - frame_times.last).count();
        frame_times.interval.note(interval_ms);
    }
    frame_times.last = frame_started;
    frame_times.have_last = true;
    struct frame_time_scope {
        std::chrono::steady_clock::time_point started;
        double interval_ms;
        ~frame_time_scope() {
            const double ms =
                    std::chrono::duration<double, std::milli>(bmmo::retail::host_clock().now(bs::clock_site::retail_display) - started).count();
            frame_times.frame.note(ms);
            if (host.core) host.core->note_frame_time(interval_ms, ms);   // step A6: the telemetry window
        }
    } frame_time{frame_started, interval_ms};
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
            if (physics_view_.get_clock(factor, delta, error)) core_pause_chain.clock_apply(m_bml->GetCKContext(), factor);
        }
        core_pause_chain.scripts_apply(m_bml->GetCKContext(), [this](int severity, const std::string& line) {
            core_log_line(logger_, severity, line);
        });
        // FULL step ESCX (RK3E): the overlay kept; while the menu is open
        // (the mute's own test below) the ESC block's Key State is held.
        core_menu_overlay.apply(m_bml->GetCKContext(), m_bml->IsIngame() && m_bml->IsPaused());
        // FULL step ESCK: the key mask's open/close transition and latch,
        // every wrapped block still wrapped.
        core_menu_keys.apply(m_bml->GetCKContext());
        // FULL step RTDF: the own-wake guard's blocks still wrapped.
        core_own_wake.apply(m_bml->GetCKContext());
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
        // FULL step SLG: this ball's sector as the core stamped it, once the
        // core has stepped the stamp's tick.
        core_host_report_own_sector();
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
    // the hold) under the tick driver's hold.  The release keeps the held
    // ticks due, up to kHoldCatchUpFrames, so the pacing catches up the
    // server's schedule without a rebase (a rebase would ask for another
    // resync).
    if (core.poll_resync_hold().held) {
        fixed_tick_.set_hold(true);
        while (core_session_running() && host.core->resync_held()) {
            core_host_drain();
            if (!core_session_running()) break;
            if (!host.core->poll_resync_hold().held) break;
            bmmo::retail::host_clock().sleep_for(bs::clock_site::retail_blocking_wait, std::chrono::milliseconds(1));
        }
        fixed_tick_.release_hold_owing(bs::kHoldCatchUpFrames);
        host.last_rebases = fixed_tick_.rebases();
        if (!core_session_running()) return;
    }
    // Step RT2: a line armed by a frame whose union pass did not read the
    // manager must not be taken by this frame's re-derivation read (inside
    // pre_step, before its after_rewinds hook arms the line again).
    core_manager_trace_due = false;
    host.core->pre_step();
    // FULL step R6B (BEAMF-S (b)): a frame that made an own body through the
    // bridge (a live beam, a rewind's remake, settle_own_due, a resync's
    // remake - the drain and pre_step above, every replay finished) re-arms
    // the own ball's collision-sound blocks once; three frames later the
    // binding is checked.  Never inside a replay: Sound_Manager is outside
    // the script selection.
    if (core_session_running() && !host.sound_blocks.empty()) {
        if (host.own_bodies_made != host.sound_made_seen) {
            host.sound_made_seen = host.own_bodies_made;
            sound_rearm(m_bml->GetCKContext(), physics_view_, logger_, host.core->own_body_entity(),
                        host.core->current_tick());
        } else if (host.sound_rearm_check > 0 && --host.sound_rearm_check == 0) {
            sound_bind_check(m_bml->GetCKContext(), logger_, host.core->own_body_entity(), host.core->current_tick());
        }
    }
}

// The host's session ends (the legacy physics_session_end_local): the script
// edits handed back, the clock guard dropped, the core ended when it still
// runs (journal, body guard, mirrors, status), the own replica gone, the tick
// driver off.  Nothing while no core session runs.
void BallanceMMOClient::core_host_end_local(const std::string& reason) {
    if (host.phase == retail_host::phase_type::idle) return;
    core_host_hold_unfreeze("the session ended");   // FULL step AHN: never left frozen
    // Step SPL: a placement still being made (the session ended between two
    // of its slices) is undone whole; the panel goes.
    if (placement_host.placement.in_progress()) {
        placement_host.placement.abandon(m_bml->GetCKContext(), "the session ended during the placement: " + reason);
        bmmo::game::placement_detail::log_lines([this](const std::string& line) { logger_->Info("Physics session: %s", line.c_str()); },
                                                placement_host.placement.describe());
    }
    anchor_freeze.end_next_process.clear();
    hold_panel_ui.hide();
    core_host_start_stage(nullptr);
    // Step SP9: a reload in progress is the session's no longer (the menus'
    // load goes on; the level it makes is a fresh one either way).
    if (level_life.reloading) {
        level_life.reloading = level_life.go_waiting = false;
        ++level_life.abandoned;
    }
    std::string error;
    // FULL step R6B: the own ball's collision-sound blocks at the end (bound
    // to its body, or off) - the session's last word on BEAMF-S (b).
    if (host.core && !host.sound_blocks.empty()) {
        sound_bind_check(m_bml->GetCKContext(), logger_, host.core->own_body_entity(), host.core->current_tick(), false);
        logger_->Info("Physics session core: sound re-arms %llu (blocks re-armed %llu, off %llu, another target %llu, "
                      "queued %llu, reset failed %llu; checks %llu: bound %llu, unbound %llu)",
                      static_cast<unsigned long long>(host.sound_rearms), static_cast<unsigned long long>(host.sound_rearmed),
                      static_cast<unsigned long long>(host.sound_skipped_off),
                      static_cast<unsigned long long>(host.sound_skipped_target),
                      static_cast<unsigned long long>(host.sound_skipped_queued),
                      static_cast<unsigned long long>(host.sound_reset_failed),
                      static_cast<unsigned long long>(host.sound_checks), static_cast<unsigned long long>(host.sound_bound),
                      static_cast<unsigned long long>(host.sound_unbound));
    }
    core_pause_chain.clock_restore(m_bml->GetCKContext());   // FULL step LGM: the core host's own edits
    core_pause_chain.scripts_restore(m_bml->GetCKContext());
    logger_->Info("Physics session: ESC menu overlay at the end: %s",
                  core_menu_overlay.status(m_bml->GetCKContext()).c_str());
    // FULL step ESCX: handed back; with the menu open the ESC hold stays
    // until it closes (core_host_process), or its closing ESC re-pauses.
    if (core_menu_overlay.end(m_bml->GetCKContext(), m_bml->IsIngame() && m_bml->IsPaused()))
        logger_->Info("Physics session: ESC menu overlay: the session ended with the menu open; the ESC hold stays "
                      "until the menu closes");
    logger_->Info("Physics session: ESC menu %s at the end", core_menu_keys.status().c_str());
    if (core_menu_keys.end(m_bml->GetCKContext()))
        logger_->Info("Physics session: ESC menu keys: the session ended with the menu open or a key held across its "
                      "close; the level's key-reading blocks read no key until both are over");
    // FULL step RTDF (decision 65): Ball Navigation's Physics WakeUp handed back.
    logger_->Info("Physics session: %s at the end", core_own_wake.status().c_str());
    core_own_wake.restore(m_bml->GetCKContext());
    core_own_wake.clear_counts();   // per session, not per anchor (a reset or resync re-anchors)
    core_reset_restore(m_bml->GetCKContext());   // FULL step LGM: N1, N2 and N4 the core host's own
    core_deactivate_restore(m_bml->GetCKContext(), logger_);
    if (!core_checkpoint_hold.restore(m_bml->GetCKContext()))   // FULL step N3K
        logger_->Warn("Physics session: the Script input of activate Sektor could not be handed back to its retail "
                      "source; the session's parameter is left in place");
    if (core_checkpoint_phase.applied() && !core_checkpoint_phase.restore(m_bml->GetCKContext()))
        logger_->Warn("Physics session: a checkpoint phase target could not be handed back to CurrentLevel; the "
                      "session's copy is left in place");
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
    // Step OPB: the written-cell columns' memo (this process, every session).
    logger_->Info("Physics session: %s", cells_memo_host.line().c_str());
    while (cells_memo_notes_logged < cells_memo_host.notes().size())
        logger_->Warn("Physics session: %s", cells_memo_host.notes()[cells_memo_notes_logged++].c_str());
    {   // step OPC
        const auto& reuse = bmmo::game::script_reuse_totals();
        logger_->Info("Physics session: %s", reuse.line(bmmo::game::script_reuse_mode_from_env()).c_str());
        for (const std::string& note: reuse.notes) logger_->Warn("Physics session: %s", note.c_str());
    }
    {   // T3C e3
        const auto& memo = bmmo::game::slot_memo_totals();
        logger_->Info("Physics session: %s", memo.line(bmmo::game::slot_memo_mode_from_env()).c_str());
        for (const std::string& note: memo.notes) logger_->Warn("Physics session: %s", note.c_str());
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
    host.room = 0;
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
    case bmmo::SessionStartAt: {   // FULL step SAXH (protocol 24)
        bmmo::session_start_at_msg msg{};
        if (decode(msg)) {
            // FULL step AHN: a start the core took (it told the player)
            // gives the wait panel its countdown's instant (display only).
            anchor_freeze.notice_seen = false;
            core.on_session_start_at(msg);
            if (anchor_freeze.notice_seen && !(msg.flags & bmmo::session::START_AT_FLAG_CORRECTION) && msg.start_us != 0)
                hold_panel_ui.start_instant(clock_type::time_point{std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::microseconds(msg.start_us))});
        }
        break;
    }
    case bmmo::SessionStartStatus: {
        bmmo::session_start_status_msg msg{};
        if (decode(msg)) {
            // FULL step AHN (display only); step SAXC: the panel follows the
            // core's answer - a status after this member's own start is
            // stale and not shown (the core's notice sets the line, the
            // panel's status the counts and the members' marks).
            if (core.on_session_start_status(msg))
                hold_panel_ui.status(msg.phase, msg.confirmed, msg.total, msg.text);
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
        int flags = bmmo::net::kSendReliable;
        if (mode == bs::send_mode::reliable_no_nagle) flags = bmmo::net::kSendReliableNoNagle;
        else if (mode == bs::send_mode::unreliable_no_delay) flags = bmmo::net::kSendUnreliableNoDelay;
        // K7d / K7e: recorded, or compared with the capture's (a replay sends nothing).
        if (!bmmo::retail::session_capture::instance().send(static_cast<uint8_t>(mode), bytes)) return;
        std::string copy = bytes;   // client::send takes a mutable buffer
        send(copy.data(), copy.size(), flags);
    };
    // Step K1: the core's reads through the one clock entry, by site (the host's own
    // direct reads move there with step K7).
    p.now = [](bs::clock_site site) { return bmmo::retail::host_clock().now(site); };
    p.log = [this](bs::log_level level, const std::string& text) {
        if (level == bs::log_level::warn) logger_->Warn("%s", text.c_str());
        else logger_->Info("%s", text.c_str());
    };
    p.utc_ms = [](bs::clock_site site) { return bmmo::retail::host_clock().utc_ms(site); };

    // ---- reading the world ----
    p.body_state = [this](const std::string& entity, bmmo_physics_body_state& out) {
        report_precision_53 precision;
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
        const bool active = core_manager_active(m_bml->GetCKContext());   // FULL step LGM: the core host's own read
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
        if (core_checkpoint_hold.held()) {
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
    p.world_hash = [this](bmmo::physics::world_hash& out) {   // the TICK record: no surface signature (T3C S1)
        std::string error;
        return physics_view_.capture(out, error, false);
    };
    p.own_ball = [this]() {
        report_precision_53 precision;
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
        // FULL step R6B: the own ball's collision-sound blocks (re-armed on a
        // bridge-made own body, sound_rearm).
        host.sound_blocks = sound_listener_blocks(m_bml->GetCKContext());
        host.sound_made_seen = host.own_bodies_made;
        logger_->Info("Physics session: %zu navigation leaves at tick %u, keys %d/%d/%d/%d; %zu sound listener blocks",
                      graph.leaves.size(), tick, graph.leaves[0].key, graph.leaves[1].key, graph.leaves[2].key,
                      graph.leaves[3].key, host.sound_blocks.size());
        return true;
    };
    // The bridge's event log: created / deleted / script_wakeup to the core
    // in order (an own body made or gone sets the own group again); the
    // diagnostics under trace.  The birth recipes go into the birth log
    // through host support's one feed (FULL step AUD3, the user's (b) of
    // decision 59: session_host_world.hpp birth_feed, the headless host's
    // too; OnPhysicalize no longer feeds it): what this frame's Physicalize
    // made each body with (engine change #19), kept by tick, so a rewind
    // anchored before this tick can make the body again at it.
    p.drain_body_events = [this](uint32_t tick, const std::function<bool(const bs::body_event&)>& each) {
        const std::string events = physics_view_.drain_event_log();
        const bool trace = host.core && host.core->config.trace;
        bs::birth_feed births(host.births, tick, false);
        bs::parse_event_log(events, [&](const std::string& kind, const std::string& name) {
            if (name.empty()) return;
            if (kind == "recipe") {
                if (births.line(name) == bs::birth_feed::result::unparsed && host.recipe_parse_logs++ < 4)
                    logger_->Warn("Physics session: birth recipe at tick %u could not be parsed: %.120s", tick, name.c_str());
                return;
            }
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
            seen.keys = host.navigation.keys_from_state(frame_keys_.data()) | host.navigation.camera_bits_from_state(frame_keys_.data());
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
        // N1 and N2 kept (FULL step LGM: the shared helpers the headless
        // client's guard keeps them with, in the core host's own state).
        core_reset_apply(m_bml->GetCKContext(), logger_, resolved);
        if (const int again = bmmo::game::repin_checkpoint_deactivation(m_bml->GetCKContext(), core_deactivate_pins))
            logger_->Info("Physics session: the chains' sector deactivation: %d input(s) handed back were re-pinned to 0",
                          again);
        // N3 kept (FULL step N3K: the shared helper's keep, the legacy
        // sector_activate_apply's rule and line).
        if (core_checkpoint_hold.keep(m_bml->GetCKContext()))
            logger_->Info("Physics session: the checkpoint's sector activation in activate Sektor was held again");
        if (const int again = core_checkpoint_phase.keep(m_bml->GetCKContext()))   // N4 (FULL step LGM: CH's own split)
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
                                                    proximity_host.active ? &hold : nullptr, host.script_hooks.slots);
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
        if (trafo_host.on) bmmo::game::trafo_frame(m_bml->GetCKContext(), trafo_host.registry, bs::TICK_DELTA_MS);
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
        // The remake, inside the core's mute: the pieces' births go into the
        // birth log at the resync's tick from the recipes the muted log kept
        // (bridge API v33), through the one feed (FULL step AUD3,
        // session_host_world.hpp birth_feed; the headless host's too).
        bs::birth_feed feed(host.births, spec.birth_tick, false,
                            [](const std::string& entity) { return host.core && host.core->is_ball(entity); });
        r.ok = bs::feed_muted_births(counted_muted_recipes{physics_view_, trafo_host.muted_remake_drains}, feed, [&]() {
            return bmmo::game::remake_type_set(context, trafo_host.registry, trafo_host.hooks, spec.player, spec.life,
                                               spec.type, spec.position, spec.seed, bs::TICK_DELTA_MS, r.error);
        }, [](const std::string&) { ++trafo_host.muted_added; });
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
        // FULL step N3K (host-platform audit D3): and a manager run outside
        // every union pass counted, the first 20 logged.
        const uint64_t outside = host.activations.outside_passes();
        host.activations.record_pending(m_bml->GetCKContext(), manager ? manager->GetID() : 0, host.script_activations, tick,
                                        &core_checkpoint_hold);
        if (host.activations.outside_passes() != outside && host.activations.outside_passes() <= 20)
            logger_->Warn("Physics session: %s", host.activations.outside_pass_line().c_str());
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
        host.mirror_type[spec.player] = spec.ball_type;
        host.mirror_player[spec.entity] = spec.player;
        objects_.show_mirror(spec.player, spec.ball_type);
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
    // inside a rewind, and live at an own beam's stamp (FULL step BEAMF),
    // through host support's make_own_body / unmake_own_body - the headless
    // client's code - on the bridge.  The
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
    p.airship_zone_release = [this](const std::string& entity, uint32_t tick, bool undo) {
        physics_view_.airship_zone_release(entity.c_str(), tick, undo);
    };
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
        if (body)
            if (const auto type = host.mirror_type.find(it->second); type != host.mirror_type.end())
                objects_.show_mirror(it->second, type->second);
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
    // FULL step GHF: a leaver's group forgotten at the leave's tick (bridge v32).
    p.spawn_ghost_forget = [this](const std::string& group) { return physics_view_.spawn_ghost_forget(group.c_str()); };
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
        o.delta_ms = bs::TICK_DELTA_MS;
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
            so.bound_slots = [](uint32_t tick) { return bmmo::session::bound_member_slots(host.core->member_lives(), tick); };
            so.scripts_replayed = &host.scripts_replayed;
            so.reset_ticks = &host.script_reset_ticks;
            so.reset_skips = &host.script_reset_skips;
            so.delta_ms = bs::TICK_DELTA_MS;
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
            so.cells_memo = &cells_memo_host;   // step OPB
            so.record_reuse = true;             // step OPC (BMMO_SCRIPT_REUSE)
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
        const auto get_body = world.get_body;
        world.get_body = [get_body](const std::string& entity, bmmo_physics_body_state& out) {
            report_precision_53 precision;
            return get_body(entity, out);
        };
        // Step T4: the births a replayed tick made, in the birth log under
        // that tick: a replay across an explosion physicalizes the set's
        // pieces again (step B2SW, T4 finding c: in every session, the trafo
        // inputs on or off - without them a replay across an original
        // explosion, step P's path, or a sector pass makes its bodies again
        // the same way).  FULL step AUD3: from the recipes the muted log kept
        // (bridge API v33) through host support's one feed, the headless
        // host's rule (session_host_world.hpp birth_feed: only a name the log
        // lacks at that tick, the balls skipped).
        if (world.pre_step) {
            const auto inner = world.pre_step;
            world.pre_step = [this, inner](uint32_t tick) {
                bs::birth_feed feed(host.births, tick, true,
                                    [](const std::string& entity) { return host.core && host.core->is_ball(entity); });
                bs::feed_muted_births(counted_muted_recipes{physics_view_, trafo_host.muted_replay_drains}, feed, [&]() { inner(tick); }, [&](const std::string& entity) {
                    ++trafo_host.muted_added;
                    if (trafo_host.replay_birth_logs++ < 8)
                        logger_->Info("Physics session: birth log: %s made by the replayed tick %u", entity.c_str(), tick);
                });
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
        // K7d: a captured session always has its journal (a replay compares
        // with it); a replay's journal goes beside its capture.
        auto& capture = bmmo::retail::session_capture::instance();
        if (capture.replaying()) return std::filesystem::path(capture.path() + ".replay.bmjc");
        if (!capture.recording() && !config_manager_["session_journal"]->GetBoolean()) return {};
        // Step IOX: no file-system call on the game thread - the directory is
        // created and the old journals pruned by the journal's writer strand,
        // just before it creates this file (journal_prepare below).
        return bmmo::retail::journal_files::directory()
             / bmmo::retail::journal_files::file_name(header.session, header.level, header.own_player);
    };
    p.journal_prepare = [](const std::filesystem::path& directory) {
        bmmo::retail::journal_files::prune(directory, bmmo::retail::journal_files::kKeepFiles - 1);
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
        h.manager_unpassed = static_cast<int64_t>(host.activations.outside_passes());   // FULL step N3K
        h.manager_unpassed_handed_back = host.activations.outside_passes_handed_back();
        h.checkpoint_reholds = core_checkpoint_hold.reholds();
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
    // FULL step SAXH (protocol 24): the start handshake's lines go to the
    // player, in the game's own message area (the user, 2026-10-31: the wait
    // must not look like a hang); the core has logged them already.
    p.hooks.notice = [this](const std::string& text) {
        SendIngameMessage("Physics session: " + text, bmmo::ansi::BrightGreen);
        hold_panel_ui.notice(text, bmmo::retail::host_clock().now(bs::clock_site::retail_display));   // FULL step AHN: the wait panel's state line
        anchor_freeze.notice_seen = true;
    };
    // FULL step AHN: the non-blocking hold's release - the tick driver's
    // schedule starts from the start instant (release_origin, a scheduled
    // start released at its instant) or from now (an unscheduled tick base,
    // the timeout), before a late start's catch_up slews it.  The blocking
    // hold keeps its set_hold(false) at its loop's end.
    p.hooks.numbering_restarted = [this](bs::clock_site site) {
        if (!anchor_freeze.active) return;
        if (site != bs::clock_site::assign_release && site != bs::clock_site::hold_timeout) return;
        const auto at = host.core ? host.core->release_origin() : std::nullopt;
        fixed_tick_.release_hold_at(at ? *at : bmmo::retail::host_clock().now(bs::clock_site::retail_hold_release));
        anchor_freeze.driver_released = true;
        anchor_freeze.driver_at_instant = at.has_value();
    };
    // FULL step SAXH: a late start's whole ticks - the tick driver's origin
    // moves that many ticks earlier and its pacing catches up (two frames
    // per slot through SkipRenderForNextTick; since SRT1 every other one of
    // those frames is rendered), as a SessionLead raise does.
    p.hooks.catch_up = [this](int ticks) {
        fixed_tick_.slew(ticks);
        logger_->Info("Physics session: catching up %d tick(s) of a late start (the tick driver's slew)", ticks);
    };
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
                    bmmo::game::note_script_write();   // step OPC
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
    // FULL step R6H: 'session hold-event' (session/event_hold.hpp, the
    // headless --hold-event's helper) after drop-birth's; what it still
    // holds at the session's end is discarded, said aloud (hooks.ending).
    host.event_hold.set_log([this](const std::string& line) { logger_->Info("%s", line.c_str()); });
    const auto filter_drop = p.hooks.filter_event;
    p.hooks.filter_event = [this, filter_drop](const bmmo::session_event_msg& event) {
        const bs::event_send_verdict verdict = filter_drop(event);
        if (verdict != bs::event_send_verdict::send || !host.core) return verdict;
        return host.event_hold.hold(*host.core, event, []() { return bmmo::retail::host_clock().now(bs::clock_site::hold_event_stamp); });
    };
    p.hooks.ending = [this]() { host.event_hold.discard(); };
    // The own body guard lifted after the journal closed (design 2.2
    // hooks.ended).
    // FULL step AUD3 (audit D9): host support's one order - the sets torn
    // down (step T4), then the guard lifted.
    p.hooks.ended = [this]() {
        bs::end_session_bodies([this]() { trafo_teardown(physics_view_, m_bml->GetCKContext(), logger_); }, [this]() {
            if (!host.body_guard) return;
            std::string error;
            physics_view_.set_body_guard(false, nullptr, error);
            host.body_guard = false;
            host.body_guard_entity.clear();
        });
    };
    return p;
}

// ---------------------------------------------------------------- the verbs

// session core [status], session force-rollback E:D, session cache
// off|on|verify|status (FULL step RCV), session snapshots on|off|status
// (FULL step DEP).  FULL step LGD: 'session core on|off' is no verb any more.
std::string BallanceMMOClient::core_host_session_verb(const std::string& rest) {
    std::istringstream args(rest);
    std::string what, value;
    args >> what >> value;
    // FULL step LGM: 'session trace on|off' and 'session rollback on|off', the
    // core host's own settings (read at each core session's start; not reset
    // per session).
    if ((what == "trace" || what == "rollback") && (value == "on" || value == "off")) {
        (what == "trace" ? core_trace : core_rollback) = value == "on";
        return "ok session " + what + " " + value;
    }
    if (what == "core") {
        // FULL step LGD (R7): the legacy session path and its switch are gone;
        // 'session core on|off' is answered as the dispatcher answers a verb it
        // does not know.  The status line stays (core=on, always).
        if (value == "on" || value == "off") return "error unknown command session core " + value;
        if (!value.empty() && value != "status") return "error usage: session core [status]";
        return std::format("ok core={} latched={} made={} error='{}' sessions={} host_phase={} frames={} refused={} "
                           "births_dropped={} unmatched_resyncs={} mailbox: pushed={} "
                           "dropped={} drained={} starts={} discarded={} undecodable={} beams_made={} nav_source_differs={} "
                           "mute_source_differs={} stray={}",
                           "on", core_latched.load() ? 1 : 0, host.core ? 1 : 0,
                           host.core_error, host.sessions_seen, host_phase_name(host.phase), host.frames, host.refused,
                           host.births_dropped,
                           host.core ? host.core->unmatched_resyncs() : 0,
                           mailbox.pushed(), mailbox.dropped(), host.drained,
                           host.starts, host.discarded, host.undecodable, host.beams_made, host.nav_source_differs,
                           host.mute_source_differs, core_stray.load());
    }
    if (what == "hold") {
        // FULL step AHN: how the next session's anchor hold waits - frozen
        // and polled from OnRender (nonblocking, the default) or the blocking
        // loop inside OnProcess (the A/B control); host-local, read at the
        // SessionStart (caps.nonblocking_hold).  'shots DIR|off': back-buffer
        // dumps at 0.3 s and 1 s into each non-blocking hold (the automation
        // verbs run from OnProcess, which a frozen hold does not call).
        if (value == "blocking" || value == "nonblocking") {
            anchor_freeze.nonblocking = value == "nonblocking";
            return std::format("ok session hold {}: from the next SessionStart{}", value,
                               host.phase != retail_host::phase_type::idle ? " (the running session keeps its hold)" : "");
        }
        if (value == "shots") {
            std::string dir;
            std::getline(args, dir);
            while (!dir.empty() && (dir.front() == ' ' || dir.front() == '\t')) dir.erase(dir.begin());
            if (dir.empty() || dir == "off") {
                anchor_freeze.shot_dir.clear();
                return std::string("ok session hold shots off");
            }
            anchor_freeze.shot_dir = dir;
            return "ok session hold shots " + dir + " (0.3 s and 1 s into each non-blocking hold)";
        }
        if (!value.empty() && value != "status") return "error usage: session hold blocking|nonblocking|status | session hold shots DIR|off";
        return std::format("ok hold={} active={} holds={} equal={} different={} ended={} shots='{}' last='{}'",
                           anchor_freeze.nonblocking ? "nonblocking" : "blocking", anchor_freeze.active ? 1 : 0,
                           anchor_freeze.holds, anchor_freeze.equal, anchor_freeze.different, anchor_freeze.ended,
                           anchor_freeze.shot_dir, anchor_freeze.last);
    }
    if (what == "cache") {
        // FULL step RCV: RC2 (b)'s capture cache (core_config::capture_cache)
        // for the next core session - off, on (the default) or verify (reads
        // anyway and counts every cached entry that differs: the status
        // line's ccache=hits/mismatches v).  A running session keeps the
        // mode it began with.
        using mode = bmmo::session::rollback_engine::capture_cache_mode;
        const auto name = [](uint8_t m) {
            return m == static_cast<uint8_t>(mode::off) ? "off" : m == static_cast<uint8_t>(mode::verify) ? "verify" : "on";
        };
        if (value == "off" || value == "on" || value == "verify") {
            const mode m = value == "off" ? mode::off : value == "verify" ? mode::verify : mode::on;
            capture_cache_next.store(static_cast<uint8_t>(m));
            return std::format("ok session cache {}: the next core session's capture cache{}", value,
                               core_session_running() ? " (the running session keeps its own mode)" : "");
        }
        if (!value.empty() && value != "status") return "error usage: session cache off|on|verify|status";
        return std::format("ok cache next={} running={}", name(capture_cache_next.load()),
                           core_session_running() ? 1 : 0);
    }
    if (what == "snapshots") {
        // FULL step DEP: the snapshot fallback (core_config::snapshots) for
        // the next core session - off (the default: the session runs as
        // snapshots off whatever the server's interval) or on (it follows the
        // server's interval).  A running session keeps what it began with.
        if (value == "on" || value == "off") {
            snapshots_next.store(value == "on");
            return std::format("ok session snapshots {}: the next core session {}{}", value,
                               value == "on" ? "follows the server's snapshot interval"
                                             : "runs without snapshot corrections",
                               core_session_running() ? " (the running session keeps its own)" : "");
        }
        if (!value.empty() && value != "status") return "error usage: session snapshots on|off|status";
        return std::format("ok snapshots next={} running={}", snapshots_next.load() ? "on" : "off",
                           core_session_running() ? 1 : 0);
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
        // after_rewinds hook), armed before or during a core session.
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
        return std::format("ok a forced rewind {} ticks deep every {} ticks", host.force_depth, host.force_every);
    }
    if (what == "stall") {
        // A testing tool (R1 (c)): the game thread stands still for <ms>, so
        // the tick driver falls behind its schedule; past 33 ticks it
        // rebases, which a session answers with a resync (either path).
        const long ms = std::strtol(value.c_str(), nullptr, 10);
        if (ms <= 0 || ms > 10000) return "error usage: session stall <ms> (1..10000)";
        const uint32_t before = session_tick_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        logger_->Info("Automation: the game thread stalled %ld ms at tick %u", ms, before);
        return std::format("ok stalled {} ms at tick {}", ms, before);
    }
    if (what == "hold-event") {
        // FULL step R6H: 'session hold-event TYPE:N:MS|TYPE:N:drop|off' (the
        // headless --hold-event; hooks.filter_event).  'off' stops further
        // holds; an event held already still goes out at its time.
        if (value == "off") {
            host.event_hold.disarm();
            return std::format("ok hold-event off ({} still held, released at their time)", host.event_hold.held());
        }
        bs::event_hold_spec spec;
        if (!bs::parse_event_hold(value, spec))
            return "error usage: session hold-event TYPE:N:MS|TYPE:N:drop|off (TYPE physicalize, unphysicalize, sector, "
                   "finish, bodyrevived, trafo or 0-5; N 1-based)";
        host.event_hold.arm(spec);
        if (spec.drop)
            return std::format("ok own {} #{} of {} dropped (never sent)", bs::event_type_name(spec.type), spec.nth,
                               core_session_running() ? "this session from now" : "the next session");
        return std::format("ok own {} #{} of {} held {} ms", bs::event_type_name(spec.type), spec.nth,
                           core_session_running() ? "this session from now" : "the next session", spec.ms);
    }
    if (what == "force-own-move") {
        // FULL step R6H: 'session force-own-move <n> [after <k>]' (the
        // headless --force-own-move N[:K]; session_client_core::
        // force_own_move): the next own birth moved n ticks late, k ticks
        // after it, through the capability path whatever own_body_control
        // says.  Nothing on the wire: this client parts from the server.
        if (!core_session_running()) return "error session force-own-move: no session core runs";
        std::string after_word, after_value;
        args >> after_word >> after_value;
        char* end = nullptr;
        const unsigned long n = std::strtoul(value.c_str(), &end, 10);
        const bool n_ok = !value.empty() && end && !*end && n >= 1 && n <= 1000;
        unsigned long k = 0;
        bool k_ok = after_word.empty();
        if (after_word == "after" && !after_value.empty()) {
            end = nullptr;
            k = std::strtoul(after_value.c_str(), &end, 10);
            k_ok = end && !*end && k <= 1000;
        }
        if (!n_ok || !k_ok) return "error usage: session force-own-move <n> [after <k>] (n 1-1000, k 0-1000)";
        if (!host.core->force_own_move(static_cast<uint32_t>(n), static_cast<uint32_t>(k)))
            return "error session force-own-move: not armed (the core runs without rollback)";
        return std::format("ok the next own birth is moved {} ticks late, {} ticks after it (at tick {}; {} done, {} "
                           "refused so far)", n, k, host.core->current_tick(), host.core->forced_own_moves(),
                           host.core->forced_own_moves_refused());
    }
    if (what == "own-births") {
        // FULL step R6B: 'session own-births parked|remade' (a test verb,
        // core_config::own_births_parked - the same-build control arm of the
        // in-window own-birth remake): for the next session and, at once, for
        // a running core (the core reads the field at each restore).  The
        // status adds the sound re-arm's counters (BEAMF-S (b)).
        if (value == "parked" || value == "remade") {
            own_births_parked_next.store(value == "parked");
            if (core_session_running()) host.core->config.own_births_parked = value == "parked";
            return std::format("ok session own-births {}: the next core session{}", value,
                               core_session_running() ? " and the running one from its next restore" : "");
        }
        if (!value.empty() && value != "status") return "error usage: session own-births parked|remade|status";
        return std::format("ok own-births next={} running={} own_bodies_made={} unmade={} sound: blocks={} rearms={} "
                           "rearmed={} off={} target={} queued={} reset_failed={} checks={} bound={} unbound={}",
                           own_births_parked_next.load() ? "parked" : "remade",
                           core_session_running() ? (host.core->config.own_births_parked ? "parked" : "remade") : "none",
                           host.own_bodies_made, host.own_bodies_unmade, host.sound_blocks.size(), host.sound_rearms,
                           host.sound_rearmed, host.sound_skipped_off, host.sound_skipped_target,
                           host.sound_skipped_queued, host.sound_reset_failed, host.sound_checks, host.sound_bound,
                           host.sound_unbound);
    }
    if (what == "cadence") {
        // Step A2: 'session cadence N|status' (core_config::
        // remote_cadence_frames, 0-16): for the next session and, at once,
        // for a running core.
        if (!value.empty() && value != "status") {
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end || n < 0 || n > 16) return "error usage: session cadence <0-16>|status";
            remote_cadence_next.store(static_cast<uint32_t>(n));
            if (core_session_running()) host.core->config.remote_cadence_frames = static_cast<uint32_t>(n);
        }
        return std::format("ok cadence next={} running={}", remote_cadence_next.load(),
                           core_session_running() ? std::to_string(host.core->config.remote_cadence_frames) : "none");
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
           "session drop-birth | session hold-event TYPE:N:MS|TYPE:N:drop|off | session force-own-move <n> [after <k>] | "
           "session placement | session probes | session activate <script> at b+<n>|<tick> | "
           "session cache off|on|verify|status | session snapshots on|off|status | "
           "session own-births parked|remade|status | session cadence <0-16>|status | "
           "session hold blocking|nonblocking|status | session hold shots DIR|off";
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
    auto& net = transport();
    if (what == "lag" || what == "blackout") {
        const bool lag = what == "lag";
        if (lag ? ms < 0 || ms > 10000 : ms <= 0 || ms > 60000)
            return "error usage: net lag <ms> [up|down] [at <tick>] (one way; 0: none) | net blackout <ms> [up|down] "
                   "[at <tick>]";
        if (!up && !down) return "error usage: net " + what + " <ms> [up|down] [at <tick>]";
        if (at >= 0) {
            host.net_pending.push_back({lag, ms, up, down, static_cast<uint32_t>(at)});
            return std::format("ok net {} {} ms ({}) armed for tick {} (now {})", what, ms, join_flags(up, down), at,
                               session_tick_now());
        }
    }
    if (what == "lag") {
        const int32_t value = static_cast<int32_t>(ms);
        bool ok = true;
        if (up) ok = net.set_fake_lag(bmmo::net::transport::direction::send, value) && ok;
        if (down) ok = net.set_fake_lag(bmmo::net::transport::direction::recv, value) && ok;
        if (!ok) return "error GNS refused the fake lag";
        if (up) host.lag_up_ms = value;
        if (down) host.lag_down_ms = value;
        const uint32_t tick = session_tick_now();
        logger_->Info("Automation: net lag %d ms (%s) from tick %u", value, join_flags(up, down).c_str(), tick);
        return std::format("ok net lag {} ms ({}) from tick {}: up {} ms, down {} ms", value, join_flags(up, down), tick,
                           host.lag_up_ms, host.lag_down_ms);
    }
    if (what == "blackout") {
        bool ok = true;
        if (up) ok = net.set_fake_loss(bmmo::net::transport::direction::send, 100.0f) && ok;
        if (down) ok = net.set_fake_loss(bmmo::net::transport::direction::recv, 100.0f) && ok;
        if (!ok) return "error GNS refused the fake loss";
        const auto until = bmmo::retail::host_clock().now(bs::clock_site::retail_net) + std::chrono::milliseconds(ms);
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
        const int timeout = net.connection_timeout_ms();
        std::string warning;
        if (timeout > 0 && ms >= timeout)
            warning = std::format(" - at least GNS's connected timeout ({} ms): the connection will drop", timeout);
        const uint32_t tick = session_tick_now();
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
    if (!next_session_tick(tick)) {
        if (r.started && host.phase != retail_host::phase_type::running) {   // FULL step LGM: the core's phase alone
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
    // FULL step AUD3 (the user's (b), decision 59): the birth log is not fed
    // from here any more - the bridge event log's recipe line of the body
    // this Physicalize makes is (drain_body_events; a replayed tick's and a
    // remake's from the muted log's recipes), the headless host's one feed.
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) {
        // A level object's retail Physicalize during the session.
        if (core.config.trace)
            logger_->Info("Physics session: retail physicalize of %s at tick %u (%d convex, %d balls, %d concave)",
                          name.c_str(), tick, convexCnt, ballCnt, concaveCnt);
        return true;
    }
    // FULL step RPA (the user's decision 62): the block is about to keep a
    // body this ball already has (the core's rule, own_physicalize_keeps_body,
    // which logs it): no birth here - no birth cell, no report, no group or
    // spawn impulse - as no body is made.
    if (core.own_physicalize_keeps_body(name, tick)) {
        logger_->Info("Physics session: own ball %s physicalized again at tick %u with its body alive: kept, not reported",
                      name.c_str(), tick);
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
    // (The birth log takes the body's recipe line, read when the body is
    // made, i.e. after the offset: FULL step AUD3.)
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
    if (checkpoint && core.assigned() && core_checkpoint_hold.held()) {
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

// FULL step SLG (RT3's sector lag).  With the chain's [0][1] Set Cell split
// (B2SX) the crossing writes only the private copy, and IngameParameter[0][1]
// holds what the last union pass wrote - a peer's sector, or this ball's
// previous one: the checkpoint hook's read of it reported the crossing one
// checkpoint late (and a peer's higher sector as this ball's).  The core's
// poll already stamps the crossing from the copy (poll_own_sector); so the
// hook reads no cell here, and this ball's sector is reported when the core
// has stepped the tick of its own stamp - the tick the server applies the
// Sector event at, and every world's union counts it from (a MOVED echo
// that arrives before that tick rewrites the stamp first; one that arrives
// after it leaves the sector reported, its timestamp early by the move).  Without the split the chain writes [0][1]
// itself and the hook's read stays right.
bool BallanceMMOClient::core_host_own_sector_from_stamps() const {
    return core_session_running() && core_chain_cell.applied() && core_checkpoint_hold.held();
}

// Once per frame after post_step: the own stamps whose tick the core has
// stepped and that are not yet reported, the latest of them adopted as this
// ball's sector, with what update_current_sector and on_sector_changed do
// for a change (timestamp, max sector, CurrentSector, the highscore flag),
// but no physics_session_on_sector: the core has the crossing already.
void BallanceMMOClient::core_host_report_own_sector() {
    if (!core_host_own_sector_from_stamps()) return;
    const auto& core = *host.core;
    const uint32_t now = core.current_tick();
    // Sectors are small numbers (IngameParameter's sector cell, 1-based; the
    // anchor's sector 1 is never stamped); a stamp lookup each is cheap.
    constexpr int kMaxSector = 64;
    int sector = 0;
    uint32_t stamped = 0;
    for (int candidate = 2; candidate <= kMaxSector; ++candidate) {
        uint32_t tick = 0;
        if (own_sector_reports.reported.count(candidate) || !core.own_sector_stamp(candidate, tick)
                || static_cast<int32_t>(tick - now) > 0)
            continue;
        own_sector_reports.reported.insert(candidate);
        if (!sector || static_cast<int32_t>(tick - stamped) >= 0) {
            sector = candidate;
            stamped = tick;
        }
    }
    if (!sector) return;
    logger_->Info("Physics session: own sector %d reported at tick %u (stamped %u)%s", sector, now, stamped,
                  sector == current_sector_ ? ", already the reported one" : "");
    if (sector == current_sector_) return;
    current_sector_ = sector;
    current_sector_timestamp_ = db_.get_timestamp_ms();
    if (connected()) {
        current_sector_timestamp_ += get_status().ping_ms;
        if (!spectator_mode_) update_sector_timestamp(current_map_, current_sector_, current_sector_timestamp_);
    }
    max_sector_ = std::max(current_sector_.load(), max_sector_.load());
    if (connected()) send_current_sector();
    if (current_level_mode_ == bmmo::level_mode::Highscore) extra_life_received_ = false;
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
    if (!core_checkpoint_hold.held()) return false;   // FULL step N3K
    const uint32_t due = core.current_tick() + 1 + core.sector_lead_ticks();
    core.request_sector(sector, "automation");
    answer = std::format("ok sector {} held for tick {} (Sector event sent ahead; session core)", sector, due);
    return true;
}

// The 'journal' verb: the core's journal (design 1.2).  Every action is
// answered here (FULL step LGM; since LGD there is no legacy black box to fall
// back to).  The verb and the BML property Gameplay/SessionJournal are one
// setting, read once at each session start.
bool BallanceMMOClient::core_host_journal(const std::string& action, const std::string& text, std::string& answer) {
    const bool session = host.phase != retail_host::phase_type::idle && host.core;
    const bool property = config_manager_["session_journal"]->GetBoolean();
    if (!session) {
        // No core session: nothing is open; the setting is the property.
        if (action.empty() || action == "status") {
            answer = std::format("ok journal={} recording=0 (no session journal is open; session core)",
                                 property ? "on" : "off");
            return true;
        }
        if (action == "on" || action == "off") {
            config_manager_["session_journal"]->SetBoolean(action == "on");
            answer = action == "on" ? "ok journal on (SessionJournal=true), recording starts with the next session"
                                    : "ok journal off (SessionJournal=false), nothing was being recorded";
            return true;
        }
        if (action == "mark") {
            answer = text.empty() ? "error usage: journal mark <text>" : "error no session journal is open";
            return true;
        }
        answer = "error usage: journal on|off|status|mark <text>";
        return true;
    }
    auto& journal = host.core->journal();
    if (action.empty() || action == "status") {
        auto& writer = journal.writer();
        if (!writer.is_open())
            answer = std::format("ok journal={} recording=0 (no session journal is open; session core)",
                                 property ? "on" : "off");
        else
            answer = std::format("ok journal={} recording={} capped={} bytes={} path={} (session core)",
                                 property ? "on" : "off", journal.journalling() ? 1 : 0, writer.capped() ? 1 : 0,
                                 writer.bytes(), writer.path().string());
        return true;
    }
    if (action == "on") {
        // The property is the setting: nothing can be opened mid-session (the
        // header needs the anchor, which is long past).
        config_manager_["session_journal"]->SetBoolean(true);
        answer = journal.journalling()
                       ? "ok journal on (SessionJournal=true), already recording to " + journal.writer().path().string()
                       : "ok journal on (SessionJournal=true), recording starts with the next session";
        return true;
    }
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
    answer = "error usage: journal on|off|status|mark <text>";
    return true;
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
    h.manager_unpassed = static_cast<int64_t>(host.activations.outside_passes());   // FULL step N3K
    h.manager_unpassed_handed_back = host.activations.outside_passes_handed_back();
    h.checkpoint_reholds = core_checkpoint_hold.reholds();
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
                       "refused={} unmatched_resyncs={} {} cbq={}/{}/{}/{}/{}/{}/{} cbqx={}/{} "
                       "dorm={}/{}/{}/{}/{}",
                       core.session_id(), host_phase_name(host.phase), core.current_tick(), core.assigned() ? 1 : 0,
                       core.holding() ? 1 : 0, host.navigation_known ? 1 : 0, core.own_physicalized() ? 1 : 0,
                       host.refused, core.unmatched_resyncs(), core.status_text(h),
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
