#pragma once

// The platform interface of the shared session-client core
// (docs/session-client-core-design.md 2): what the core needs from the world,
// as a struct of std::function fields each host fills with what it has.  An
// empty field means "this host cannot" (the rollback_world convention,
// session/rollback_binding.hpp); where a decision depends on more than one
// field the host declares it in session_capabilities, and the core's
// constructor checks that every field a declared capability needs is filled.
//
// Step E1 (design 6.1) starts it with the three operations every host has
// and the headless platform fills: send, now and log.  Each later step adds
// the operations its moved code calls (design 2.2), and the capability checks
// with them: E2 the journal's (journal_path, utc_ms, body_state, list_bodies);
// E3 the first host hooks (session_hooks: numbering_restarted, filter_event,
// ending); E4 the snapshot path's (write_body, own_driven_entity, bind_world,
// keep_live_cells); E6 the mirrors' (mirror_entity, make_mirror,
// unmake_mirror, detach_mirror_navigation, attach_mirror_navigation,
// mute_event_log); E7 the own ball's (make_own, unmake_own,
// own_birth_capture, ball_recipe_of) and the own_body_control check; E8 the
// sectors' (sector_manager_active, run_sector, record_activation,
// poll_sector); E9 the frame's (world_hash, own_ball, own_body_upkeep,
// own_replica_name, discover_navigation, drain_body_events,
// reportable_wakeup, observe_input, own_camera_rows, drive, force_own,
// unforce_own, arm_own_capture, movable_mechanisms, note_script_resets,
// script_guards, record_pending_activation, the hooks after_rewinds,
// after_record, after_amend and after_journal) and bind_world with the
// core's hooks (core_world_hooks); E10 the reports (counters, the hooks
// window and status).
//
// CK-free and header-only.

#include "clock_site.hpp"
#include "rollback.hpp"
#include "../entity/session.hpp"
#include "../physics/physics_rt_api.h"
#include "../physics/world_hash.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bmmo {
    struct session_event_msg;   // message/session_event_msg.hpp
}

namespace bmmo::session {

    struct journal_header;   // session/journal.hpp
    class session_client_core;   // session/client_core.hpp

    // How a message goes out; the host maps it to k_nSteamNetworkingSend_*.
    enum class send_mode : uint8_t { reliable, reliable_no_nagle, unreliable_no_delay };

    enum class log_level : uint8_t { info, warn };

    // Where the own ball's Physicalize / Unphysicalize are learned (design
    // 2.3): the event log at post_step (headless), or the host's hooks,
    // queued and committed at post_step (retail).
    enum class own_lifecycle_source : uint8_t { event_log, host_hooks };

    // What the host does with an own lifecycle event the core is about to put
    // on the wire (design 2.2 hooks.filter_event): send it, or keep it - held
    // for a later session_client_core::release_event, or dropped.
    enum class event_send_verdict : uint8_t { send, hold, drop };

    // Design 2.2 "host hooks": optional, H.  An empty hook is a host with
    // nothing to do there.
    struct session_hooks {
        // The tick numbering (re)started at `site`, the pacer's origin with it:
        // the anchor (anchor_origin), the hold's timeout (hold_timeout) and a
        // SessionAssign (assign_resync, assign_renumber, assign_release).
        // Headless: origin_ = wall_now(site); retail: nothing (its fixed tick
        // counts on its own).
        std::function<void(clock_site site)> numbering_restarted;
        // An own event, counted and journaled, before it goes on the wire
        // (headless --hold-event).
        std::function<event_send_verdict(const bmmo::session_event_msg& event)> filter_event;
        // The session is ending (session_client_core::end), before anything
        // else: headless discards what --hold-event still holds.
        std::function<void()> ending;
        // The frame's (design 1.3; E9), each with the tick T the frame steps:
        // after pre_step's rewinds (step 7: the headless --force-rollback),
        // after post_step's rollback record (step 23: --dump-savestate,
        // --compare-scripts), after its amend (step 30a: the traces) and
        // inside its journal block, after the TICK record and checkpoint
        // (step 32: the --trace impulse report).
        std::function<void(uint32_t tick)> after_rewinds;
        std::function<void(uint32_t tick)> after_record;
        std::function<void(uint32_t tick)> after_amend;
        std::function<void(uint32_t tick)> after_journal;
        // The reports (design 2.2 hooks.window / hooks.status; E10): the
        // window: line the frame closes (post_step step 31, after
        // after_amend; only a line the window's edge produced), and the
        // status line at the session's end (end, after the mirrors are
        // gone).  The text is the core's (window_text, status_text); the
        // host prints it (headless: one log line each, as logf wrote it).
        std::function<void(const std::string& text)> window;
        std::function<void(const std::string& text)> status;
        // The session's end (E11; legacy_calls::own_body_guard_off until
        // then): after the journal closed and before every mirror's
        // navigation and body are taken out.  Headless: the own body guard
        // lifted (host state).
        std::function<void()> ended;
        // A --trace diagnostic (E11; legacy_calls::describe_movable_objects
        // until then): the movable objects' description, the text of the
        // `after rollback: movable=` line for each of the first 16 snapshot
        // rollbacks (headless: describe_movable_objects; design 2.4 keeps
        // such diagnostics in the host).  Empty: the line is not written.
        std::function<std::string()> describe_movables;
    };

    // The host's own counters the status and window: lines print at their
    // places (design 1.2 "counters", 1.3; E10): the pacer's rebases, the
    // manager activations host support re-issued in replays, and the
    // scripts a restore kept live for having been reset after its anchor.
    struct host_counters {
        int rebases = 0;
        uint64_t activations_replayed = 0;
        uint64_t script_reset_skips = 0;
    };

    // What the core hands bind_world (design 2.2 bind_world, 2.4
    // bind_session_world; E9): its half of the rollback binding.  The host
    // binds its world through the shared bind_session_world, which puts
    // these where design 2.4 says.
    struct core_world_hooks {
        // Before each replayed tick, first: the births deferred into the
        // replay, made at their tick (drain_pending_births).
        std::function<void(uint32_t tick)> before_replayed_tick;
        // The mirrors a replayed tick drives, with their generation
        // (rollback_world::replay_remotes).
        std::function<void(uint32_t tick, std::map<std::string, uint32_t>& out)> replay_remotes;
        // The mechanism bodies the world has after a replayed step: the
        // list the live records track (rollback_world::replay_mechanisms).
        std::function<std::vector<std::string>()> replay_mechanisms;
        // Every decision the engine takes about a snapshot (the journal).
        std::function<void(const rollback_correction& c)> on_correction;
        // destroy_body's first half: a moved own birth the replay makes
        // again at its applied tick.  True when `entity` at `birth_tick` is
        // one (`destroyed`: taken out); false otherwise.
        std::function<bool(const std::string& entity, uint32_t birth_tick, bool& destroyed)> destroy_own_birth;
        // The entity is the own ball's body, or a mirror's: never a
        // mechanism birth to destroy or insert.
        std::function<bool(const std::string& entity)> own_body;
        std::function<bool(const std::string& entity)> mirror;
    };

    // The own ball as the host sees it after a step (design 2.2 own_ball;
    // E9): its entity ("" when the level has none), its row in the ball
    // table (-1: none), whether it has a body, and the body's state.
    struct own_ball_view {
        std::string entity;
        int ball_type = -1;
        bool physicalized = false;
        bmmo_physics_body_state state{};
    };

    // One entry of the body event log (design 2.2 drain_body_events; E9): a
    // body created or deleted, or a mechanism script's Physics WakeUp.  The
    // host keeps the other kinds (the birth recipes) itself.
    struct body_event {
        enum class kind : uint8_t { created, deleted, wakeup };
        kind what = kind::created;
        std::string name;
    };

    // What the own replica used for the step of T (design 2.2 observe_input;
    // E9): the keys as the navigation polls them (0 before the graph is
    // known), the camera reference's rows (`cam_valid` false without one),
    // and whether the navigation's key events were active.
    struct input_observation {
        uint8_t keys = 0;
        float cam[3][3] = {};
        bool cam_valid = false;
        bool nav_active = false;
    };

    // A mirror's body (design 2.2 make_mirror; E6): the entity the host named
    // for `player`'s ball of `ball_type` (mirror_entity), posed and
    // physicalized with the recipe in the player's collision group, and
    // kicked with the spawn impulse of `kick_index`
    // (session/spawn_impulse.hpp kSpawnDirectionTable) when `kick`.
    struct mirror_spec {
        uint32_t player = 0;
        uint8_t ball_type = 0;
        std::string entity;
        float position[3] = {};
        float rotation[9] = {};
        bmmo_physics_ball_recipe recipe{};
        std::string group;
        bool kick = false;
        uint32_t kick_index = 0;
        float kick_strength = 0.0f;
    };
    // What make_mirror did: `made` (false: `error` says why, and nothing was
    // kicked), and when the spec asked for a kick, whether it took
    // (`kick_error` otherwise).
    struct mirror_made {
        bool made = false;
        std::string error;
        bool kicked = false;
        std::string kick_error;
    };
    // attach_mirror_navigation's answer: the host has no navigation to give
    // yet (nothing done, nothing to say), the mirror is driven now, or the
    // attach failed (`error` says why).
    enum class mirror_navigation : uint8_t { unavailable, attached, failed };

    // The own ball's body made again the way the server makes it (design 2.2
    // make_own; E7): at the pose its Physicalize carried, with that recipe in
    // the own collision group, kicked with the spawn impulse of `kick_index`
    // when `kick`.  `tick` names the change in the host's own log lines.
    struct own_body_spec {
        std::string entity;
        float position[3] = {};
        float rotation[9] = {};
        bmmo_physics_ball_recipe recipe{};
        std::string group;
        bool kick = false;
        uint32_t kick_index = 0;
        float kick_strength = 0.0f;
        uint32_t tick = 0;
    };
    // What make_own did: `found` (false: the host has no such entity, and
    // did nothing); a body still there taken out first, or that failing
    // (`took_out_failed`, `took_out_error`); the body made (`made`, `error`
    // otherwise, and nothing kicked); the kick taken (`kick_error`
    // otherwise).  The core logs them, in this order.
    struct own_body_made {
        bool found = false;
        bool took_out_failed = false;
        std::string took_out_error;
        bool made = false;
        std::string error;
        bool kicked = false;
        std::string kick_error;
    };
    // The own ball's Physicalize as the host saw it (design 2.2
    // own_birth_capture; E7): `captured` before the step moved it (headless:
    // the PreSimulate capture of the live tick), else the entity's pose after
    // the step, which is never a spawn; `spawn` when it was at the level's
    // resetpoint (kicked here, so on every side).
    struct own_birth_pose {
        bool captured = false;
        float position[3] = {};
        float rotation[9] = {};
        bool spawn = false;
    };

    // What the host's own sector poll found after a step (design 2.2
    // poll_sector; E8): the sector cell the level's scripts last wrote
    // (IngameParameter[0][1]; `cell_read` false when the host could not read
    // it, and nothing else counts), whether the own ball's resetpoint moved
    // (a crossing of the own ball alone; a host that does not watch it says
    // false), and whether the level's checkpoint sector activation is held
    // (a crossing is then stamped ahead; otherwise it already ran and is
    // reported after the fact).  A host that reports its crossings itself
    // while the hold is not in force (the retail checkpoint hook) says
    // `cell_read` false then, or the core would report them a second time.
    struct sector_poll {
        bool cell_read = false;
        int cell = 0;
        bool resetpoint_moved = false;
        bool checkpoint_held = false;
    };

    // The host's work around a scope of the core (keep_live_cells): what it
    // did on entry is the host's; `on_exit` runs when the scope ends, once.
    class scope_guard {
    public:
        scope_guard() = default;
        explicit scope_guard(std::function<void()> on_exit) : on_exit_(std::move(on_exit)) {}
        scope_guard(scope_guard&& other) noexcept : on_exit_(std::move(other.on_exit_)) { other.on_exit_ = nullptr; }
        scope_guard(const scope_guard&) = delete;
        scope_guard& operator=(const scope_guard&) = delete;
        scope_guard& operator=(scope_guard&&) = delete;
        ~scope_guard() {
            if (on_exit_) on_exit_();
        }

    private:
        std::function<void()> on_exit_;
    };

    struct session_platform {
        // ---- transport and time (design 2.2) ----
        std::function<void(const std::string& bytes, send_mode mode)> send;
        // The wall clock as the flow reads it, by the read's place in the flow
        // (session/clock_site.hpp): the headless client hands it to the
        // inbound capture or takes the captured value; retail ignores it.
        std::function<std::chrono::steady_clock::time_point(clock_site site)> now;
        // One line of text, written as it is (the harness parses the headless
        // client's lines).
        std::function<void(log_level level, const std::string& text)> log;
        // The system clock in ms since the epoch, by the read's site (the
        // journal header's utc_ms, clock_site::journal_utc).
        std::function<uint64_t(clock_site site)> utc_ms;

        // ---- reading the world (design 2.2) ----
        // One body's state by entity name; false when it has no body.
        std::function<bool(const std::string& entity, bmmo_physics_body_state& out)> body_state;
        // Every physicalized entity, in physics-table order.
        std::function<std::vector<bmmo_physics_body_state>()> list_bodies;

        // Gameplay_SectorManager is running (design 2.2
        // sector_manager_active; E8): the sector union's pacer counts it once
        // per tick, before that tick's step.
        std::function<bool()> sector_manager_active;
        // The own sector poll after the step of `tick` (sector_poll; E8):
        // `tick` is the tick the core decides for, so a line the host writes
        // about the poll names the same tick.
        std::function<sector_poll(uint32_t tick)> poll_sector;
        // The world's fingerprint (the journal's TICK record; E9).
        std::function<bool(bmmo::physics::world_hash& out)> world_hash;
        // The own ball after the step (own_ball_view; E9).  A read only:
        // the core asks it whenever it needs the own ball's name.
        std::function<own_ball_view()> own_ball;
        // What the host keeps up for the own ball once a frame, after
        // own_ball (E9; post_step step 17): the body guard, the own
        // collision group and the own navigation replica's attach (headless:
        // only with rollback, a host policy).
        std::function<void(const own_ball_view& own)> own_body_upkeep;
        // The entity the own navigation replica was last named by, kept
        // after the replica is gone (a failed re-attach); "" when it never
        // had one (E9: recorded_input_at tells the own ball's records by it,
        // which own_driven_entity's gate on a live replica would not).
        std::function<std::string()> own_replica_name;
        // The navigation graph, until it is known (E9; post_step step 16):
        // true exactly when it became known in this call (the host logs its
        // line); the core then attaches every mirror's navigation.
        std::function<bool(uint32_t tick)> discover_navigation;
        // The body event log since the last call (E9; post_step step 19), in
        // order: the host keeps the birth recipes (its local birth log) and
        // hands every other entry to `each`, which answers true when it was
        // the own ball's body created or deleted (the host forgets its own
        // collision group then).
        std::function<void(uint32_t tick, const std::function<bool(const body_event& event)>& each)> drain_body_events;
        // A Physics WakeUp worth a BodyRevived event (not a ball, not a
        // mirror; retail: not the own ball either).
        std::function<bool(const std::string& name, const std::string& own_ball)> reportable_wakeup;
        // What the own replica used for the step just taken (E9; step 21).
        std::function<input_observation()> observe_input;
        // The own replica's camera rows for the next step (`basis`: the
        // frame's camera reference), and the retail forces zeroed (E9; step
        // 21).  Nothing without an own replica.
        std::function<void(const float (*basis)[3])> own_camera_rows;
        // The movable bodies a record tracks as mechanisms (every movable
        // body but the balls; E9; step 23).
        std::function<std::vector<std::string>()> movable_mechanisms;
        // The scripts told to reset in the frame of `tick` (E9; step 23: a
        // rewind anchored earlier keeps them live).
        std::function<void(uint32_t tick)> note_script_resets;
        // The retail sector resets kept neutralized before the frame's
        // rewinds (E9; pre_step step 4).  `resolved`: the client resolved a
        // mechanism of its own (the sector is up).
        std::function<void(uint32_t tick, bool resolved)> script_guards;

        // ---- writing the world (design 2.2; E4) ----
        // Beam a body to the pose and overwrite its speeds; `wake` wakes it, a
        // simulated body is frozen otherwise (physics_state.hpp set_body_state,
        // the bool overload).  `error` is the failure's text: the host clears
        // it first, as write_body_state does - the core keeps one string
        // across its writes and logs it only after a failed one.
        std::function<bool(const std::string& entity, const double position[3], const double rotation[4],
                           const float linear[3], const float angular[3], bool wake, std::string& error)> write_body;

        // One pass of the sector union (design 2.2 run_sector; E8):
        // deactivate nothing, write `sector` into the sector cell and start
        // Gameplay_SectorManager from its beginning, as the server's
        // update_sectors() does.
        std::function<void(int sector)> run_sector;
        // A manager activation this frame consumed, by the tick of that
        // frame, for the replay to re-issue (design 2.2 record_activation;
        // E8: the union's passes, `reset` true).
        std::function<void(uint32_t tick, bool reset)> record_activation;
        // An activation the frame's own scripts asked for, pending for the
        // frame of `tick` (E9; post_step step 15): recorded like a pass.
        std::function<void(uint32_t tick)> record_pending_activation;

        // ---- mirrors (design 2.2; E6) ----
        // The entity that mirrors `player` with a ball of `ball_type`, made
        // when it does not exist yet; "" with `error` when there is none.
        std::function<std::string(uint32_t player, uint8_t ball_type, std::string& error)> mirror_entity;
        // Pose, physicalize and kick a mirror's body (mirror_spec).  The core
        // asks mirror_entity for `spec.entity` first, in the same frame, but
        // the operation stands alone: the spec names the player and the ball
        // type, so a host that keeps nothing from mirror_entity finds the
        // entity again (headless: the pointer mirror_entity found when it
        // names spec.entity, else the same lookup).
        std::function<mirror_made(const mirror_spec& spec)> make_mirror;
        // A mirror's body goes (unphysicalized).  Only the body: the core
        // takes the navigation out first (detach_mirror_navigation) and mutes
        // the event log around it where its flow does (mute_event_log).
        std::function<void(const std::string& entity)> unmake_mirror;
        // A mirror's navigation replica goes.
        std::function<void(const std::string& entity)> detach_mirror_navigation;
        // A physicalized mirror without navigation is driven by the shared
        // navigation from now on (headless: the leaf directions of the
        // level's graph, the ball row's force, CamRef_BMMO_<player>).
        std::function<mirror_navigation(uint32_t player, const std::string& entity, uint8_t ball_type,
                                        std::string& error)> attach_mirror_navigation;
        // The bridge's event log muted or not; the core keeps the nesting
        // (session_client_core::event_log_mute).
        std::function<void(bool muted)> mute_event_log;

        // ---- driving (design 2.2; E4) ----
        // The entity the own navigation replica drives, "" when there is none.
        std::function<std::string()> own_driven_entity;
        // A mirror driven with `frame` for the next step (E9; pre_step step
        // 10, post_step step 28); false and `error` when the host refused.
        std::function<bool(const std::string& entity, const bmmo::session::input_frame& frame, std::string& error)> drive;
        // A tick a SessionLead forced (E9; pre_step step 11): the own
        // replica's polling off and `frame` fed instead.  Answers the entity
        // forced ("" when there is no own replica: nothing done), `ok`
        // false and `error` when a call failed; unforce_own (post_step step
        // 13) switches the polling back on for that entity.
        std::function<std::string(const bmmo::session::input_frame& frame, bool& ok, std::string& error)> force_own;
        std::function<bool(const std::string& entity, std::string& error)> unforce_own;
        // Optional (E9; pre_step step 12): arm the capture of an own
        // Physicalize inside the live step of `tick` (headless: the
        // PreSimulate capture).
        std::function<void(uint32_t tick)> arm_own_capture;

        // ---- own ball (design 2.2; E7) ----
        // The level's ball table row of `entity` (Physicalize_GameBall): its
        // type, -1 when the table names no such ball, and the retail
        // Physicalize recipe of that type (cleared, and empty for -1).  The
        // beam's and the own birth's recipe.
        std::function<int(const std::string& entity, bmmo::session::ball_recipe& recipe)> ball_recipe_of;
        // The own birth of `entity` the event log reported for `tick`
        // (own_lifecycle_source::event_log): the pose and spawn flag the host
        // captured before the step, consumed by this call.  Required with
        // event_log.
        std::function<own_birth_pose(const std::string& entity, uint32_t tick)> own_birth_capture;
        // With own_body_control only (design 2.3), and called inside the
        // core's mute of the event log (mute_event_log): the own body made
        // again (own_body_spec), a body still there taken out first; and the
        // own body taken out, the entity put at the pose its Physicalize
        // carried (false and `error`: not taken out).  The host shuts down
        // what drives the body first (headless: the navigation replica).
        std::function<own_body_made(const own_body_spec& spec)> make_own;
        std::function<bool(const std::string& entity, const float position[3], const float rotation[9], uint32_t tick,
                           std::string& error)> unmake_own;

        // ---- rollback (design 2.2; E4) ----
        // The rollback engine's view of the host's world, bound anew on every
        // use, never cached (the options take the own key count by value).
        // E9: with the core's half of the binding (core_world_hooks), which
        // the host composes through the shared bind_session_world
        // (session/session_host_world.hpp).
        std::function<rollback_world(const core_world_hooks& hooks)> bind_world;
        // Around every rewind_to / on_snapshot: the host keeps the live cells
        // the replay rewinds (headless live_cells_guard) and re-issues what a
        // rollback lost when the returned guard goes out of scope.  The guard
        // reads the core's rollback_stats() and current_tick() (headless:
        // host support's live_cells_guard).
        std::function<scope_guard(const session_client_core& core)> keep_live_cells;

        // ---- journal (design 1.2) ----
        // The whole path of the session's journal for this header (the
        // session, level and own player are filled; utc_ms is not yet); an
        // empty path means no journal.  An empty field means this host never
        // journals.  Headless: --journal verbatim; retail (R1): its journal
        // directory and file name, after pruning old files.
        std::function<std::filesystem::path(const journal_header& header)> journal_path;

        // ---- reports (E10), optional ----
        // The host's counters for the text the core reports (host_counters):
        // read at step 31's window: line and at end's status line.  Empty:
        // a host without them, which prints zeros there.
        std::function<host_counters()> counters;

        // ---- host hooks (design 2.2), all optional ----
        session_hooks hooks;
    };

    // Design 2.3.  Every flag defaults to "cannot"; a host declares what it
    // can.  Each comes with the operations it needs (own_body_control:
    // make_own / unmake_own, checked from E7; whole_frame with bind_world at
    // E9).  From
    // E3 the core reads `slew` (SessionReady's caps: CAP_LEAD iff slew); the
    // pacer operations it will need arrive at E5/E9.  `nonblocking_hold`
    // changes nothing in the core (one hold state machine, poll_hold); it
    // says how the host waits.
    struct session_capabilities {
        bool own_body_control = false;   // make_own / unmake_own filled
        bool slew = false;               // the host's pacer applies slew_step (2x raise, 0.9x lower)
        bool nonblocking_hold = false;   // the anchor hold returns to the host loop
        bool whole_frame = false;        // a replayed tick may run a whole engine frame (bind_world fills whole_frame)
        own_lifecycle_source own_lifecycle = own_lifecycle_source::event_log;   // or host_hooks
    };

    // (legacy_calls, design 6.1 "Linking while the core is incomplete", was
    // in session/client_core.hpp from E3 to E10; E11 deleted it, its last
    // entries being hooks.ended and hooks.describe_movables.)

    // The first operation `platform` lacks that every host must fill, or that
    // a capability `caps` declares needs; nullptr when nothing is missing.
    inline const char* missing_platform_operation(const session_platform& platform, const session_capabilities& caps) {
        if (!platform.send) return "send";
        if (!platform.now) return "now";
        if (!platform.log) return "log";
        // The snapshot path (E4): every host applies snapshots.
        if (!platform.body_state) return "body_state";
        if (!platform.write_body) return "write_body";
        if (!platform.own_driven_entity) return "own_driven_entity";
        if (!platform.bind_world) return "bind_world";
        if (!platform.keep_live_cells) return "keep_live_cells";
        // The remote lifecycle (E6): every host mirrors the other players.
        if (!platform.mirror_entity) return "mirror_entity";
        if (!platform.make_mirror) return "make_mirror";
        if (!platform.unmake_mirror) return "unmake_mirror";
        if (!platform.detach_mirror_navigation) return "detach_mirror_navigation";
        if (!platform.attach_mirror_navigation) return "attach_mirror_navigation";
        if (!platform.mute_event_log) return "mute_event_log";
        // The own lives (E7): every host beams and reports its own births;
        // the event log's births need the capture, the own body control the
        // two body operations.
        if (!platform.ball_recipe_of) return "ball_recipe_of";
        // The sectors (E8): every host runs the sector union and reports the
        // sectors its own ball enters.
        if (!platform.sector_manager_active) return "sector_manager_active";
        if (!platform.run_sector) return "run_sector";
        if (!platform.record_activation) return "record_activation";
        if (!platform.poll_sector) return "poll_sector";
        // The frame (E9): every host steps, records, drives and amends.
        // arm_own_capture is optional (retail: empty).
        if (!platform.record_pending_activation) return "record_pending_activation";
        if (!platform.script_guards) return "script_guards";
        if (!platform.drive) return "drive";
        if (!platform.force_own) return "force_own";
        if (!platform.unforce_own) return "unforce_own";
        if (!platform.discover_navigation) return "discover_navigation";
        if (!platform.own_ball) return "own_ball";
        if (!platform.own_body_upkeep) return "own_body_upkeep";
        if (!platform.own_replica_name) return "own_replica_name";
        if (!platform.drain_body_events) return "drain_body_events";
        if (!platform.reportable_wakeup) return "reportable_wakeup";
        if (!platform.observe_input) return "observe_input";
        if (!platform.own_camera_rows) return "own_camera_rows";
        if (!platform.movable_mechanisms) return "movable_mechanisms";
        if (!platform.note_script_resets) return "note_script_resets";
        if (caps.own_lifecycle == own_lifecycle_source::event_log && !platform.own_birth_capture)
            return "own_birth_capture";
        if (caps.own_body_control) {
            if (!platform.make_own) return "make_own";
            if (!platform.unmake_own) return "unmake_own";
        }
        // A host that journals (session/client_journal.hpp) stamps the header,
        // and writes bodies into LOCAL checkpoints (body_state, which body
        // corrections read too, is required above).
        if (platform.journal_path) {
            if (!platform.utc_ms) return "utc_ms";
            if (!platform.list_bodies) return "list_bodies";
            if (!platform.world_hash) return "world_hash";   // the TICK record (E9)
        }
        return nullptr;
    }

}
