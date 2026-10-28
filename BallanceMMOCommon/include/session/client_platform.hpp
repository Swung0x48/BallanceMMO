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
// unforce_own, arm_own_capture, movable_mechanisms (movable_bodies since
// step P), note_script_resets,
// script_guards, record_pending_activation, the hooks after_rewinds,
// after_record, after_amend and after_journal) and bind_world with the
// core's hooks (core_world_hooks); E10 the reports (counters, the hooks
// window and status).
//
// CK-free and header-only.

#include "clock_site.hpp"
#include "rollback.hpp"
#include "trafo_input.hpp"
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
        // Step A3 (resync design 2.6.6): the resync hold ended after `held`
        // of wall time in which the world stepped nothing.  Headless: the
        // pacer's origin moves by it (a hold is no stall to rebase for);
        // retail: nothing (its loop holds the tick driver itself).
        std::function<void(std::chrono::steady_clock::duration held)> resync_hold_ended;
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

    // What a host's localize_scripts met (step A3; game/script_replay.hpp
    // portable_report, the counts the core logs and the gate reads).
    struct resync_script_report {
        uint32_t scripts = 0, kept_local = 0;
        uint32_t objects_kept = 0, objects_resolved = 0, unportable = 0, ambiguous = 0;
        std::string first_unportable;   // where the first unportable reference was ("" none)
    };

    // What the core hands bind_world (design 2.2 bind_world, 2.4
    // bind_session_world; E9): its half of the rollback binding.  The host
    // binds its world through the shared bind_session_world, which puts
    // these where design 2.4 says.
    struct core_world_hooks {
        // Before each replayed tick, first: the births deferred into the
        // replay, made at their tick (drain_pending_lives; with the deaths
        // the reconcile of an ended life queued, step EL7).
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
        // destroy_body for a mirror (step LB): true when the mirror born at
        // `birth_tick` was taken out and the replay makes it again at that
        // tick from its Physicalize (the core's births); false, and the
        // mirror keeps the parking path, otherwise.
        std::function<bool(const std::string& entity, uint32_t birth_tick)> remake_mirror_birth;
        // The entity is the own ball's body, or a mirror's: never a
        // mechanism birth to destroy or insert.
        std::function<bool(const std::string& entity)> own_body;
        std::function<bool(const std::string& entity)> mirror;
        // The entity is a ball (session_client_core::is_ball: an entity the
        // own ball has been this session, or a mirror's), whether or not it
        // has a body now: the replica's controllers are on it, so it keeps
        // the parking path (step P: the identity the "Ball_" prefix stood
        // in for, which the trafo explosion pieces share).
        std::function<bool(const std::string& entity)> ball;
        // destroy_own_birth's test without its side effect (rollback-engine
        // gaps design 2.4, step B8EG item EGB): true when destroy_own_birth
        // would take `entity` at `birth_tick` out (a moved own birth, with
        // own body control).  bind_session_world builds
        // rollback_world::keeps_parked from it and the ball / own_body /
        // mirror hooks, next to destroy_body.
        std::function<bool(const std::string& entity, uint32_t birth_tick)> own_birth_destroyable;
        // Step OWNB: true when the restore just took `entity` out as an own
        // birth the scripts made, queued to be made again at its birth tick
        // the way the server makes its clone.  bind_session_world adds it to
        // rollback_world::birth_from_inputs: like a mirror made again from
        // the inputs, the replay's step is its state - the birth record was
        // stepped with the remotes known then, the replay with those known
        // now - and nothing writes the record over it.
        std::function<bool(const std::string& entity)> own_birth_remade;
        // Step RR: the local entity a snapshot row of the server's body
        // (kind, owner) belongs to at a replayed tick (rollback_world::
        // row_entity): a mirror driven by then, or the resolved mechanism.
        std::function<std::string(body_kind kind, uint32_t owner, uint32_t tick)> row_entity;
        // Step B1 (resync design 2.8): a body destroyed at the restore and
        // not made again at its live birth tick because the union's
        // re-derivation moved or forgot the sector pass that made it live
        // (rollback_world::birth_moved).
        std::function<bool(const std::string& entity, uint32_t birth_tick)> birth_moved;
        // Gap L (rollback-engine gaps design 2.3.1, step EL6): the core's
        // half of rollback_world::restore_lives - the balls (own and
        // mirrors, L2/L3) - asked first by bind_session_world, before the
        // host's mechanism half.  Step EL7 (G7) sets it to the mirrors'
        // reconcile (session_client_core::reconcile_remote_lives); the own
        // ball's half is G8's: until then an ended own life is answered by
        // no half (unrevivable, counted).
        std::function<void(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended, bool savestate,
                           std::map<std::string, life_outcome>& out)> restore_lives;
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
    // and whether the navigation's key events were active.  `paused` (R2):
    // the pause menu's mute was in force during the step (retail; the own
    // input is then keys 0 with the navigation off), sent as
    // INPUT_FLAG_PAUSED; the headless client never pauses.
    struct input_observation {
        uint8_t keys = 0;
        float cam[3][3] = {};
        bool cam_valid = false;
        bool nav_active = false;
        bool paused = false;
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
        // Gap L, L3 (rollback-engine gaps design 2.3.4, step EL8): a life
        // revived at a restore.  The host makes sure a navigation drives the
        // entity (the own replica when it is on it, else one made for the
        // revive and destroyed with the body at its replayed death), so the
        // restore's set_nav and the blob find its controllers and the replay
        // feeds the recorded own frames to it.
        bool with_replica = false;
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
    // `phase` is CurrentLevel[0][4] ("Activation Phase?"), the cell the
    // modules of a union pass branch on in their first frame (`phase_read`
    // false when the host does not read it): the core's check that nothing
    // else wrote it under a pass (game/checkpoint_phase.hpp).
    // `cell_private` (step B2SX): `cell` is the checkpoint chain's own copy
    // of IngameParameter[0][1] (game/sector_cell_split.hpp), which only the
    // chain writes - a union pass never does, so the core compares it with
    // what its polls saw, not with the passes' writes.
    struct sector_poll {
        bool cell_read = false;
        int cell = 0;
        bool cell_private = false;
        bool resetpoint_moved = false;
        bool checkpoint_held = false;
        bool phase_read = false;
        int phase = 0;
    };

    // ---- the trafo as an input (trafo-replication design 2.3-2.5, 4.7; step T3)
    // One activation of an original trafo script the host's hold cancelled
    // after a live frame (game/trafo_sets.hpp trafo_hold_report, CK-free
    // here): the kind (TRAFO_EXPLODE / TRAFO_RESET), the piece type, an
    // explosion's referential and, under option E, the explosion's frame the
    // Trafo Manager's Delayer predicts (0: the cancelled activation itself).
    struct trafo_report {
        uint8_t flags = 0;
        uint8_t type = 0;
        float position[3] = {};
        uint32_t predicted_tick = 0;
        std::string source;
    };
    // What poll_trafo found after a live frame: the reports, in order, and
    // the effect checks (design 2.3: original pieces with a body plus
    // original scripts active; `first_leak` names the first).
    struct trafo_poll {
        std::vector<trafo_report> reports;
        int leaks = 0;
        std::string first_leak;
    };
    // One member's type set as the host's world holds it now (the
    // reconcile, design 4.7): whether the set exists, how many of its
    // pieces have a body, and the pieces' entities.
    struct trafo_type_view {
        bool exists = false;
        int bodies = 0;
        std::vector<std::string> pieces;
    };
    // The reconcile's two halves for one type set (design 4.7; game/
    // trafo_sets.hpp unmake_type_set / remake_type_set), called by the core
    // outside any replay, inside its mute of the event log: unmake puts the
    // type set back to its built state (bodies, wind, scripts); remake runs
    // the explosion of (position, seed) once, and the host's birth log takes
    // the pieces' births at `birth_tick` (the resync's S).
    enum class trafo_reconcile_action : uint8_t { unmake, remake };
    struct trafo_reconcile_spec {
        trafo_reconcile_action action = trafo_reconcile_action::unmake;
        uint32_t player = 0;
        uint8_t type = 0;
        float position[3] = {};
        int32_t seed = 0;
        uint32_t birth_tick = 0;
    };
    struct trafo_reconcile_result {
        bool ok = false;
        std::string error;
        int bodies_removed = 0;
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
        // Every movable body this world has, in the engine's table order (E9;
        // step 23).  The core tracks all of them as mechanisms but the balls,
        // which it knows by what they are (session_client_core::is_ball: the
        // own ball's entities and the mirrors), not by a name prefix: the
        // trafo explosion pieces share the balls' "Ball_" prefix and are
        // rigid bodies like any mechanism (step P).
        std::function<std::vector<std::string>()> movable_bodies;
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
        // Optional (step RO1, session/script_order.hpp): after the live frame
        // of `tick` (post_step, with record_pending_activation), the order
        // that frame ran the replay's selected scripts in, recorded under
        // `tick` - the tick a replay re-runs that frame as - for the binding
        // (script_binding_options::live_order).  Host-local: nothing on the
        // wire, nothing in a record or the journal.  Unset: the replay runs
        // the selection's own order (the fallback).
        std::function<void(uint32_t tick)> record_script_order;
        // Step B1 (resync design 2.8): the union pass a replay re-derived
        // for `tick` - recorded as run_sector's pass would be (cells
        // `sector`, 0, reset) in the tick's pass slot, which the host's
        // replay_activation of that tick re-issues right after; `sector` 0
        // forgets a pass the live frames made at `tick` (the pending slot
        // stays).  Host support's manager_activations::set_pass/forget_pass.
        std::function<void(uint32_t tick, int sector)> set_union_pass;
        // Step B1 (resync design 4.6): after a rewind that re-derived the
        // union, the live cells were not written back (adopt mode); the host
        // reads IngameParameter[0][1] as the replay left it, does the
        // bookkeeping run_sector does for it (retail: the cell is no longer
        // the own ball's sector), and returns it in `cell` (false: unread).
        // The core takes it as the cell its own sector poll compares with.
        std::function<bool(int& cell)> sector_cell_adopted;

        // ---- the exact resync (step A3; resync design 2.6, 2.10) ----
        // The server's portable script record made a local v2 record of
        // this world's selection (game/script_replay.hpp localize), with the
        // core's keep-local rule (session/resync_rules.hpp keep_local_script)
        // taking those scripts from `anchor_scripts` (the synthetic anchor's
        // own record; null: none).  False and `error`: refused, `out` not to
        // be used.
        std::function<bool(const std::vector<uint8_t>& portable, const std::vector<uint8_t>* anchor_scripts,
                           std::vector<uint8_t>& out, std::string& error, resync_script_report& report)> localize_scripts;
        // At a resync's re-assign: the host's manager and script activation
        // records and its script reset ticks at ticks >= `tick` dropped -
        // the old numbering's frames, which the new numbering re-runs or
        // never runs (G-a).  The birth log is kept: its ticks are shared.
        std::function<void(uint32_t tick)> forget_activations_from;
        // After a resync's restore: the host's birth log entries at ticks >
        // `tick` of `entities` dropped - the bodies the server's state has at
        // S, which the live frames of either numbering made after it (their
        // births are no births of this history any more).  Births of the
        // bodies the replay made again at their tick stay.
        std::function<void(uint32_t tick, const std::vector<std::string>& entities)> forget_births_after;
        // Step A5 (resync design 2.9): the manager's pending activations
        // recorded for the ticks from..to dropped, the pass slots kept -
        // the level's own walk (Init Ingame's activation of the manager)
        // that a late joiner's live frames ran inside its resync's window,
        // and that the server's state at S ran long before.  Returns how
        // many were dropped.
        std::function<size_t(uint32_t from, uint32_t to)> forget_pending_activations;
        // Optional: the tick the host's birth log has `entity` born at (its
        // latest birth; 0 unknown) - the reconcile's attribution of an
        // anchor body the server has no row for (resync design 2.6.3).
        // Empty: every such body counts as unexplained.
        std::function<uint32_t(const std::string& entity)> birth_tick_of;
        // Optional: the resync state's packed sections back to `size` bytes
        // (the server packs them with CKPackData: zlib; the host unpacks with
        // its CK's CKUnPackData).  Empty: a packed state is refused.
        std::function<bool(const std::vector<uint8_t>& packed, uint32_t size, std::vector<uint8_t>& out)> unpack_bytes;
        // Optional: a body only this client's world has (the trafo explosion
        // pieces until the trafo replication): the reconcile keeps it from
        // the anchor.  Empty: none.
        std::function<bool(const std::string& entity)> local_only_body;

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
        // `death` (step EL8): an own death replayed at its tick (gap L, L3) -
        // the navigation is left as the live death left it (its controllers
        // lose their core with the body), except a navigation made for the
        // revive (own_body_spec::with_replica), which goes with the body.
        std::function<bool(const std::string& entity, const float position[3], const float rotation[9], uint32_t tick,
                           bool death, std::string& error)> unmake_own;
        // Step EL8 (rollback-engine gaps design 2.3.4, 9.7), required with
        // session_capabilities::own_body_in_replay, optional otherwise: an
        // entity's CK world matrix read (row-major, VxMatrix order; false:
        // no such entity) and written.  The own death's pose, and the own
        // entities' matrices while they have no body (recorded per live
        // tick, written before each replayed tick, put back at the rewind's
        // end).
        std::function<bool(const std::string& entity, float matrix[16])> entity_matrix;
        std::function<bool(const std::string& entity, const float matrix[16])> set_entity_matrix;

        // ---- the airship zone (step BALA; physics_state.hpp airship_zone_set) ----
        // On (at the anchor, with the own ball's nocoll group) or off (at the
        // end): the zone's pair rule and PSI hook in this world; `box` gets
        // min xyz, max xyz; returns 1 active, 0 no airship / off, -1 error.
        // Optional: a host without it runs no zone (and is not protocol 14's).
        std::function<int(bool enable, const std::string& own_group, double box[6], std::string& error)> airship_zone;
        // 1: the entity's body is a player ball inside the zone; 0: not; -1: no body.
        std::function<int(const std::string& entity)> airship_zone_contains;
        // The zone as it stands: 1 with the box once it is taken (the
        // platform has its body), 0 not yet, -1 off.
        std::function<int(double box[6])> airship_zone_box;
        // Optional, display only: a mirror shown as a ghost (semi-transparent)
        // or as itself again; `matrix` (row-major, VxMatrix order) when the
        // ghost is to be moved to its owner's relayed pose, else null.
        std::function<void(const std::string& entity, bool ghost, const float* matrix)> show_ghost;

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

        // ---- the trafo as an input (trafo-replication design 2.3-2.5, 2.11,
        // 4.7; step T3), required with session_capabilities::trafo_inputs ----
        // After the live frame of `tick` (post_step, beside the own sector
        // poll): the originals' activations the frame's scripts asked for,
        // cancelled and reported (the hold, game/trafo_sets.hpp trafo_hold),
        // and the effect checks.  `option_e`: the session's lead allows the
        // Delayer's prediction (decision 9.5).
        std::function<trafo_poll(uint32_t tick, bool option_e)> poll_trafo;
        // `player`'s piece set, by the rule every world runs (game/
        // trafo_sets.hpp ensure_trafo_set): kept when selected, a retired one
        // selected again, otherwise built.  Never inside a replay.  False and
        // `error`: no set (the builder failed closed).
        std::function<bool(uint32_t player, std::string& error)> make_trafo_set;
        // One input applied (game/trafo_sets.hpp run_trafo): an explosion's
        // referential and seed, the explosion's or the reset's next-frame
        // flags.  Live and in every replayed tick.
        std::function<bool(const trafo_input& input, std::string& error)> run_trafo;
        // A TRAFO_FINAL reset of (player, type) applied at `tick`
        // (trafo_set_final): the set's retirement counts from it.
        std::function<void(uint32_t player, uint8_t type, uint32_t tick)> trafo_final;
        // The trafo frame of `tick` (game/trafo_sets.hpp trafo_frame): every
        // selected set's scripts, by player id, with the fixed delta - after
        // the tick's inputs and the union's pass, before the step.  A
        // replayed tick runs the same list inside the script replay's frame.
        std::function<void(uint32_t tick)> trafo_frame;
        // The reconcile's view and halves (design 4.7, above).
        std::function<trafo_type_view(uint32_t player, uint8_t type)> trafo_type_set;
        std::function<trafo_reconcile_result(const trafo_reconcile_spec& spec)> trafo_reconcile;
        // Retirement after the live frame of `now` (design 2.4): the players
        // whose sets this call deselected (the next sight of one gets a set
        // by make_trafo_set again).  `history`: the rollback history.
        std::function<std::vector<uint32_t>(uint32_t now, uint32_t history)> retire_trafo_sets;

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
        // Step EL8 (rollback-engine gaps design 2.3.4): the host fills
        // make_own / unmake_own (and entity_matrix / set_entity_matrix) and
        // the core may call them from restore_lives and the replay's drain
        // only - an own life ending inside a window is revived at the restore
        // and removed again at its replayed death, an own life born in the
        // window is remade at its stamp.  Every body made so is gone again at
        // a tick <= now.  Without it such a life is unrevivable (counted).
        // own_body_control implies nothing here: a host declares both.
        bool own_body_in_replay = false;
        // Step T3 (trafo-replication design 2.14): the host builds the
        // per-player piece sets and runs the trafo as an input (poll_trafo,
        // make_trafo_set, run_trafo, trafo_final, trafo_frame,
        // trafo_type_set, trafo_reconcile, retire_trafo_sets filled):
        // SessionReady declares CAP_TRAFO_INPUTS, and a session whose
        // SessionStart has WORLD_TRAFO_INPUTS runs it.  Without it the server
        // refuses this client in such a session, and a session without the
        // option is the step P path (owner-only pieces).
        bool trafo_inputs = false;
        // Step S5 (sector placement design 2.4, 5): the host computes the
        // placement signature of its world at every anchor
        // (anchor_report::placement) and, when SessionStart's world_options
        // has WORLD_PLACEMENT_CLONES (world_options()), builds the copies
        // there first.  SessionReady then declares CAP_PLACEMENT and carries
        // the signature, which the server compares with its own world's;
        // without it SessionReady carries 0, nothing is compared, and a
        // session with the world option refuses this client.  The headless
        // host computes it from step S6, the retail core host from S10.
        bool placement = false;
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
        if (!platform.movable_bodies) return "movable_bodies";
        if (!platform.note_script_resets) return "note_script_resets";
        // The union inside a replay (step B1): every host re-derives it.
        if (!platform.set_union_pass) return "set_union_pass";
        if (!platform.sector_cell_adopted) return "sector_cell_adopted";
        // The exact resync (step A3): every host applies the resync state.
        if (!platform.localize_scripts) return "localize_scripts";
        if (!platform.forget_activations_from) return "forget_activations_from";
        if (!platform.forget_births_after) return "forget_births_after";
        if (!platform.forget_pending_activations) return "forget_pending_activations";   // step A5
        if (!platform.unpack_bytes) return "unpack_bytes";   // the state's packed sections
        if (caps.own_lifecycle == own_lifecycle_source::event_log && !platform.own_birth_capture)
            return "own_birth_capture";
        if (caps.own_body_control || caps.own_body_in_replay) {
            if (!platform.make_own) return "make_own";
            if (!platform.unmake_own) return "unmake_own";
        }
        if (caps.own_body_in_replay) {   // step EL8
            if (!platform.entity_matrix) return "entity_matrix";
            if (!platform.set_entity_matrix) return "set_entity_matrix";
        }
        if (caps.trafo_inputs) {   // step T3
            if (!platform.poll_trafo) return "poll_trafo";
            if (!platform.make_trafo_set) return "make_trafo_set";
            if (!platform.run_trafo) return "run_trafo";
            if (!platform.trafo_final) return "trafo_final";
            if (!platform.trafo_frame) return "trafo_frame";
            if (!platform.trafo_type_set) return "trafo_type_set";
            if (!platform.trafo_reconcile) return "trafo_reconcile";
            if (!platform.retire_trafo_sets) return "retire_trafo_sets";
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
