#pragma once

// Client-side rollback (design 9.6, restore rules rewritten for 9.25): the
// client runs ahead of the server's confirmed progress and predicts; when the
// authoritative snapshot of tick T disagrees with what the client recorded at
// T, the whole shared world is rewound to T - every body the snapshot carries
// a row for goes to its row, every other recorded body and navigation replica
// to its own record - and the ticks T+1 .. now are re-simulated (physics +
// navigation only, no scripts) from the recorded inputs.  Pure logic over a
// small world adapter, shared by the retail mod (through the physics bridge)
// and the headless session client.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../entity/session.hpp"
#include "../message/message_utils.hpp"
#include "../message/session_snapshot_msg.hpp"
#include "../physics/physics_rt_api.h"

namespace bmmo::session {
    // What the engine did about an authoritative snapshot, for the session
    // journal (session/journal.hpp CORRECTION records) and any other observer.
    // `kind` uses the journal's numbering; this engine emits 0 mismatch,
    // 1 rollback, 5 too_far, 6 frozen and 7 unmatched.  `entity` is the body
    // furthest past its own threshold (empty when there is none) - a mismatch
    // can be triggered by velocity alone, and then the largest position error
    // names the wrong body - and the positions are that body's local and
    // server pose at `tick`.
    struct rollback_correction {
        uint32_t tick = 0;         // the snapshot's tick
        uint32_t local_tick = 0;   // where the client was when it arrived
        uint8_t kind = 0;
        std::string entity;
        double error_m = 0.0;
        double velocity_error = 0.0;
        double local_position[3] = {};
        double server_position[3] = {};
    };

    // What a restore does to the body's sleep state.  Mirrors the values of
    // bmmo::physics::wake_mode (physics/physics_state.hpp), which is what the
    // adapters forward to the bridge: `keep` never touches the sleep state,
    // `wake` revives a body that is not simulated (and leaves an awake body's
    // freeze timers alone), `freeze` disables simulation for a simulated body.
    enum class wake_mode : uint8_t { keep = 0, wake = 1, freeze = 2 };

    // A restore that barely moves a body cannot create a new contact, and the
    // mindist recheck it would trigger is not free; past these two the write
    // may drop the body into something, so the adapter is asked to rebuild the
    // contact pairs.  1 mm is under the pose noise the tolerances already
    // accept, 1 cm/s under a tick of gravity (0.15 m/s).
    constexpr double kRecheckPositionM = 0.001;
    constexpr double kRecheckVelocityMs = 0.01;

    struct rollback_world {
        std::function<bool(const std::string& entity, bmmo_physics_body_state& out)> get_body;
        // mode: keep = leave the sleep state alone; wake/freeze as above.
        // recheck: the write moves the body into a possibly touching pose, so
        // rebuild its contact pairs (the plain beam keeps the old ones).
        std::function<bool(const std::string& entity, const bmmo_physics_body_state& state,
                           wake_mode mode, bool recheck)> set_body;
        // The core's queued impulses (bridge API v11).  Optional: a caller
        // that does not provide them gets exactly the old behaviour, which is
        // a restore that drops whatever IVP had queued for the next PSI.  With
        // them, a replay restore puts the queue back - worth 1.6-3.6 cm per
        // rewind on a body in contact, see bmmo_physics_core_extra.
        //
        // set_body_extra runs AFTER set_body for the same entity, because
        // set_body zeroes the queue by design 8.4 and nothing in it touches the
        // queue after that point.
        std::function<bool(const std::string& entity, bmmo_physics_core_extra& out)> get_body_extra;
        std::function<bool(const std::string& entity, const bmmo_physics_core_extra& state)> set_body_extra;
        // The environment's clock (bmmo_physics_clock_state).  Optional.  A
        // restore puts it back BEFORE the bodies, because the beam stamps each
        // core with the environment's time_of_last_psi; without it every
        // replayed tick runs one tick later in IVP time than the tick it
        // replaces, which the freeze logic and the event queue both read.
        std::function<bool(bmmo_physics_clock_state& out)> get_clock;
        std::function<bool(const bmmo_physics_clock_state& state)> set_clock;
        std::function<bool(const std::string& entity, bmmo_physics_nav_state& out)> get_nav;
        std::function<bool(const std::string& entity, const bmmo_physics_nav_state& state)> set_nav;
        // A body's friction contacts (bmmo_physics_contact, bridge v15).
        // Optional.  get fills the vector and returns the count (-1: unknown
        // body); set re-creates the recorded contacts the body lacks and
        // returns how many it made (-1: unknown body).
        std::function<int(const std::string& entity, std::vector<bmmo_physics_contact>& out)> get_contacts;
        std::function<int(const std::string& entity, const std::vector<bmmo_physics_contact>& in, uint32_t flags)> set_contacts;
        // explicit input for the next physics step of a navigated ball
        std::function<bool(const std::string& entity, const input_frame& frame)> nav_input;
        // polling mode on/off (the own ball reads the keyboard itself in live frames)
        std::function<bool(const std::string& entity, bool enable)> nav_poll;
        std::function<bool()> step;   // one physics tick
        // false while the local physics clock is stopped by the retail scripts
        // (Level 1 tutorial, pause menu): bodies are snapped, nothing is
        // re-simulated.  Optional; missing means "simulating".
        std::function<bool()> simulating;
        // Called before each re-simulated tick, with the tick about to be
        // stepped: the script replay runs the mechanism scripts' frame here
        // (game/script_replay.hpp), where the live frame runs them - before
        // the physics step.  Null on the server/tests.
        std::function<void(uint32_t tick)> pre_step;
        // The mechanism scripts' runtime state as bytes (game/script_replay.hpp):
        // captured with every record, put back at the anchor so the replayed
        // frames start the scripts where the live frames had them.  Optional;
        // without them the replay is physics only, as it was.
        std::function<bool(std::vector<uint8_t>& out)> capture_scripts;
        // `anchor` is the tick the bytes were captured at: the adapter keeps
        // live every script the game reset after it (script_replay::restore).
        std::function<bool(const std::vector<uint8_t>& bytes, uint32_t anchor)> restore_scripts;
        // Optional: names how two script snapshots differ (the adapter has
        // the graph).  Returns "" when they are equal in every logical field
        // - a float parameter that follows the physics by an ulp is reported
        // in the text but is not a logical difference.  The engine counts a
        // mismatch only when this says the difference is logical (or when it
        // is absent and the bytes differ).
        std::function<std::string(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool& logical)> diff_scripts;
        std::function<void(const std::string&)> log;
        // Optional: every decision the engine takes about a snapshot, so the
        // session journal can record it without parsing the log lines.
        std::function<void(const rollback_correction&)> on_correction;
    };

    // What the caller tracks at a tick: the bodies (entity names) and, for
    // the navigated balls, whether they poll their keys in live frames.
    struct rollback_tracked {
        std::string own_entity;                    // empty when the own ball has no body
        bool own_polls = false;
        std::vector<std::string> remote_entities;  // navigated remote balls
        std::vector<std::string> mechanisms;
        // Lifetime generation of every tracked entity (own, remotes,
        // mechanisms): the caller bumps it when the body behind that name is
        // created or destroyed (Physicalize/Unphysicalize/trafo/respawn), so a
        // record made for an earlier body under the same name is never used to
        // restore the current one.  A name missing from the map is generation 0,
        // which is what a caller that does not track lifetimes gets.
        std::map<std::string, uint32_t> generations;
    };

    struct rollback_thresholds {
        // One pair for every tracked body.  The old 1 mm / 0.01 m/s pair was
        // tighter than the client's own moving-prediction noise (median
        // 9 mm / 0.084 m/s, P95 21 mm; build/live-rollback-retest-20260909/
        // findings/P1-evidence.md sections 4 and 8), so about half of all
        // snapshots rolled the session back on noise alone.  The pair below
        // covers the position noise at its P95: the worst measured phase is
        // 33.8 mm, inside 0.05 m.  It does NOT cover the velocity noise at
        // that percentile -- the measured ball velocity P95 is 0.8869 m/s
        // (own ball, L11_move) and 0.6364 m/s (peer ball, L8_move), both
        // above 0.5 m/s -- so velocity coverage stops in the median-to-P95
        // band and a velocity-only breach above ~0.5 m/s still rolls the
        // session back.  The pair keeps position / velocity = 0.1 s -- "an
        // error equivalent to a tenth of a second of motion" -- so a body
        // 0.5 m/s off reaches the tolerance inside 0.1 s.
        double position = 0.05;   // metres
        double velocity = 0.5;    // m/s
        // A shared mechanism is a script-driven body: its pose is no cleaner
        // than a ball's, so it gets the same pair.  Legal mechanism motion
        // measures <= 0.81 m/tick (Level 8 windows,
        // build/online-symptoms-20260909/mech-jitter), so 0.05 m is about 6%
        // of one such tick: a genuine divergence is still caught inside a
        // tick, while sub-tick pose noise (float velocity mirrored into a
        // double pose, one substep of phase) no longer rolls the session back.
        double mechanism_position = 0.05;   // metres
        double mechanism_velocity = 0.5;    // m/s
        // The pairs above are sized for a prediction a tenth of a second old.
        // A body the client predicted WITHOUT the input that drove it - every
        // remote ball, every mechanism - is predicted blind for the whole
        // window between the snapshot's tick and the live one, and that window
        // is input_delay plus a trip: 20 ticks (0.3 s) at a 300 ms round trip,
        // three times the horizon the pair was chosen for.  Judging such a body
        // by the 0.1 s pair asks for accuracy the client has no information
        // for, and every trip costs a full re-simulation of the window.
        //
        // Measured over a 4-player session at a 300 ms round trip
        // (maps/arena-test, --latency-ms 150): 242 of 417 mismatches were
        // remote balls whose position agreed to within 5 cm and which breached
        // on velocity alone, and they carried 58% of all the re-simulation
        // work.  That is the component that grows with the player count, so it
        // is the one a 30-player room cannot afford.
        //
        // So the pair is widened by how old the prediction is, measured in
        // those same tenths of a second (7 ticks at 66 Hz): never below 1, so a
        // short window keeps exactly today's behaviour, and never above the
        // cap, so a pathological window cannot switch correction off.  The own
        // ball is never widened - its prediction used the player's own exact
        // inputs, so an error there is a real divergence rather than news that
        // has not arrived yet.
        uint32_t blind_reference_ticks = 7;   // the pair's own horizon, in ticks
        double blind_scale_max = 4.0;
        // The server's base lead is the input delay (at most 40 ticks,
        // timeline.hpp kInputDelayMaxTicks) plus the round-trip allowance plus
        // a 2-tick margin, so the client's lag can exceed the old 40-tick resim
        // window.  48 covers the clamped worst case (~40 ticks, see
        // late_tick_base in BallanceMMOServer/server.cpp); the history holds
        // two windows so the anchor tick of a full resim is still recorded
        // when its snapshot arrives.
        size_t history_ticks = 96;
        uint32_t max_resim_ticks = 48;   // give up (hard set only) beyond this lag
    };

    struct rollback_stats {
        uint64_t snapshots = 0, matched = 0, mismatched = 0, rollbacks = 0, resim_ticks = 0;
        uint64_t rewinds = 0;          // re-anchors the caller asked for (rewind_to), not snapshot disagreements
        uint64_t impulses_restored = 0;  // body writes that put a recorded queued impulse back (v11)
        uint64_t calm_restored = 0;      // body writes that put the recorded calm references back (v13)
        uint64_t units_restored = 0;     // body writes that put the recorded unit countdown back (v13)
        uint64_t contacts_restored = 0;         // friction contacts re-created at an anchor (v15)
        uint64_t contact_restore_failures = 0;  // set_contacts calls that failed outright
        // The script replay (game/script_replay.hpp): restores of the scripts'
        // state at an anchor, and replays after which the re-executed scripts'
        // state differed from the live record of the same tick - zero on a
        // solo probe means the snapshot carries everything the scripts keep.
        uint64_t script_restores = 0;
        uint64_t script_restore_failures = 0;
        uint64_t script_mismatches = 0;        // logical: flags, IOs, controllers, counters
        uint64_t script_float_mismatches = 0;  // float parameters only (sensors, timers)
        uint64_t clock_rewinds = 0;      // restores that put the environment's clock back first
        uint64_t unmatched = 0;        // snapshot tick not in the history
        uint64_t too_far = 0;          // lag beyond max_resim_ticks: bodies set, no re-simulation
        uint64_t frozen = 0;           // local physics clock stopped: bodies snapped, no re-simulation
        double last_error = 0.0, max_error = 0.0;
        std::string last_mismatch;     // entity of the last mismatch
    };

    class rollback_engine {
    public:
        // One snapshot row that maps to a local entity, and what the compare
        // pass concluded about it.
        struct row_ref {
            const body_state* row = nullptr;
            bool simulated = false;      // BODY_FLAG_SIMULATED
            bool breached = false;       // past the tolerance of its kind
            bool authoritative = false;  // the row is the restore source for this body
        };

        explicit rollback_engine(rollback_thresholds thresholds = {}) : thresholds_(thresholds) {}

        // Drops every record.  Lifecycle events no longer need this - a body
        // created or destroyed mid-window is handled by its generation and by
        // the birth record (see on_snapshot) - but a re-anchoring caller
        // (session assignment, resync, a full snapshot overwriting the world)
        // still has to say that the recorded ticks no longer describe
        // anything.  Keeps session diagnostics across the boundary.
        void invalidate_history() { history_.clear(); }

        void clear() {
            invalidate_history();
            stats_ = {};
            detailed_logs_ = 0;
            resim_traces_ = 0;
        }
        // Per-step traces of the re-simulation (diagnostics).
        void set_verbose(bool verbose) { verbose_ = verbose; }
        const rollback_stats& stats() const { return stats_; }
        size_t history_size() const { return history_.size(); }

        // After the physics step of `tick`: remember every tracked body and
        // navigation state, plus the inputs that produced this tick (own and
        // remote), for a later re-simulation.  A body appearing in or leaving
        // the tracked set is NOT a reason to drop the history: the restore
        // rules below judge every name on its own (row, record of the same
        // lifetime, birth record, or nothing at all), so a window may span
        // several live sets.
        void record(const rollback_world& world, uint32_t tick, const rollback_tracked& tracked,
                    const std::map<std::string, input_frame>& inputs_applied) {
            tick_state state;
            state.tick = tick;
            state.tracked = tracked;
            state.inputs = inputs_applied;
            capture(world, state);
            if (!history_.empty() && history_.back().tick >= tick) {
                // re-recording (resim) or a numbering restart: drop from here
                while (!history_.empty() && history_.back().tick >= tick) history_.pop_back();
            }
            history_.push_back(std::move(state));
            while (history_.size() > thresholds_.history_ticks) history_.pop_front();
        }

        // The corrections a live frame applies after its physics step (snapshot
        // rollback, blend, mechanism) change the state the next step starts
        // from, so the record for that tick must reflect them: a later rollback
        // restores this record and re-simulates from it exactly as the live
        // frame did.  The entry was captured before those corrections, and the
        // re-simulation writes its result back into the same entry, so an
        // unamended record re-bakes the loss of its own correction into the
        // history.  Keeps the tick's inputs and tracked set.  Returns false
        // when the tick is no longer recorded (evicted, invalidated or never
        // recorded), so the caller must not treat the amendment as applied.
        bool amend_record(const rollback_world& world, uint32_t tick) {
            tick_state* at = find(tick);
            if (!at) return false;
            capture(world, *at);
            return true;
        }

        // Authoritative snapshot of `tick`.  `entity_of` maps a snapshot body
        // to the local entity name (empty = not tracked here); `input_at`
        // supplies the input a navigated ball had at a tick during the
        // re-simulation. Relayed remote inputs supersede recorded predictions;
        // the own ball keeps the input recorded for that tick's entity.
        // Returns true when a rollback happened.
        bool on_snapshot(const rollback_world& world, const session_snapshot_msg& snapshot, uint32_t current_tick,
                         const std::function<std::string(const body_state&)>& entity_of,
                         const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at) {
            ++stats_.snapshots;
            rollback_correction correction;
            correction.tick = snapshot.tick;
            correction.local_tick = current_tick;
            auto report = [&](uint8_t kind) {
                if (!world.on_correction) return;
                correction.kind = kind;
                world.on_correction(correction);
            };
            tick_state* at = find(snapshot.tick);
            if (!at) {
                ++stats_.unmatched;
                report(7);
                return false;
            }
            // The lifetime a name carries right now is the newest one the
            // caller has told us about (a name that left the tracked set keeps
            // the generation it had, so its records stay usable while the body
            // itself is still there).  A record of an older generation belongs
            // to a body that no longer exists and may not be restored onto the
            // current one.
            const auto generation_now = [&](const std::string& entity) -> uint32_t {
                for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                    auto found = it->tracked.generations.find(entity);
                    if (found != it->tracked.generations.end()) return found->second;
                }
                return 0;
            };
            // The record of T that may be used for `entity`: same name and
            // same lifetime.  Null means "no opinion of our own about this
            // body at T", which is not by itself a disagreement.
            const auto record_at = [&](const std::string& entity) -> const bmmo_physics_body_state* {
                auto it = at->bodies.find(entity);
                if (it == at->bodies.end()) return nullptr;
                if (generation_in(*at, entity) != generation_now(entity)) return nullptr;
                return &it->second;
            };

            // How deep the window is.  Needed before the compare pass, which
            // judges a blind-predicted body by a tolerance widened for it, and
            // again below to decide whether the window may be re-simulated.
            const uint32_t window = current_tick > snapshot.tick ? current_tick - snapshot.tick : 0;
            const double blind_window_scale = blind_scale(window);

            // 1. compare every row we can compare
            bool mismatch = false;
            double worst = 0.0;        // largest position error: the log lines and the stats
            double worst_breach = 0.0; // furthest past a threshold: the journal's subject
            std::string worst_entity, detail;
            std::map<std::string, row_ref> rows;
            for (const auto& body: snapshot.bodies) {
                const std::string entity = entity_of(body);
                if (entity.empty()) continue;
                row_ref ref;
                ref.row = &body;
                ref.simulated = (body.flags & BODY_FLAG_SIMULATED) != 0;
                const bmmo_physics_body_state* recorded = record_at(entity);
                if (!recorded) {
                    // No record of this lifetime at T: nothing to disagree
                    // with.  The row is still the best pose we have for the
                    // body, so it is a restore source if the body exists now
                    // (unless a birth record supersedes it, below).
                    ref.authoritative = true;
                    rows[entity] = ref;
                    continue;
                }
                // Each row is judged by the tolerance of its own kind - a
                // mechanism row at the same error as a ball is not a
                // divergence - widened by how long we have been predicting it
                // blind.  The own ball is the one body whose prediction used
                // the exact inputs that drove it, so it keeps the unwidened
                // pair (rollback_thresholds, blind_reference_ticks).
                const bool blind = entity != at->tracked.own_entity;
                const tolerance tol = tolerance_for(body.kind, blind ? blind_window_scale : 1.0);
                double dp = 0.0, dv = 0.0;
                for (int k = 0; k < 3; ++k) {
                    dp += (body.position[k] - recorded->position[k]) * (body.position[k] - recorded->position[k]);
                    dv += (static_cast<double>(body.linear[k]) - recorded->linear[k])
                        * (static_cast<double>(body.linear[k]) - recorded->linear[k]);
                }
                dp = std::sqrt(dp);
                dv = std::sqrt(dv);
                if (dp > worst) {
                    worst = dp;
                    worst_entity = entity;
                }
                // The mismatch can be a velocity one (for a ball, 0.5 m/s
                // trips before 5 cm of drift does), so the record names the
                // body that is furthest past its own threshold, not the one
                // that moved most: otherwise the journal blames an
                // in-tolerance body and reports dv = 0 for exactly the event
                // it was written for.
                const double breach = std::max(tol.position > 0.0 ? dp / tol.position : dp,
                                               tol.velocity > 0.0 ? dv / tol.velocity : dv);
                if (breach > worst_breach) {
                    worst_breach = breach;
                    correction.entity = entity;
                    correction.error_m = dp;
                    correction.velocity_error = dv;
                    for (int k = 0; k < 3; ++k) {
                        correction.local_position[k] = recorded->position[k];
                        correction.server_position[k] = body.position[k];
                    }
                }
                ref.breached = dp > tol.position || dv > tol.velocity;
                // A sleeping row inside the tolerance is no news at all: the
                // body is where we have it and the server says it stopped
                // there.  Copying that row would put the server's sleep flag
                // on our body - which re-arms the freeze timer of a mechanism
                // on every snapshot - so the row is not a restore source and
                // the body rewinds with the group from its own record instead.
                // Only a breaching sleeping row restores and freezes.
                ref.authoritative = ref.breached || ref.simulated;
                if (ref.breached) {
                    mismatch = true;
                    if (detailed_logs_ < 40) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), " %s dp=%.4f dv=%.3f local=(%.3f,%.3f,%.3f)%s server=(%.3f,%.3f,%.3f)%s",
                                      entity.c_str(), dp, dv, recorded->position[0], recorded->position[1],
                                      recorded->position[2], recorded->simulated ? "" : "z", body.position[0],
                                      body.position[1], body.position[2], ref.simulated ? "" : "z");
                        detail += buf;
                    }
                }
                rows[entity] = ref;
            }
            stats_.last_error = worst;
            if (worst > stats_.max_error) stats_.max_error = worst;
            if (!mismatch) {
                ++stats_.matched;
                return false;
            }
            ++stats_.mismatched;
            stats_.last_mismatch = worst_entity;
            report(0);
            const bool frozen = world.simulating && !world.simulating();
            if (!detail.empty() && world.log && !frozen && detailed_logs_++ < 40)
                world.log("mismatch at tick " + std::to_string(snapshot.tick) + " (local " + std::to_string(current_tick) + "):"
                          + detail);

            return restore_and_replay(world, snapshot.tick, current_tick, rows, input_at, report,
                                      worst_entity + " off by " + std::to_string(worst) + " m");
        }

    private:
        // Put every recorded body back to `anchor` and re-simulate
        // anchor+1 .. current_tick from the recorded inputs.  Steps 2 and 3 of
        // on_snapshot, which is the only caller that has `rows` to restore
        // from; rewind_to() calls it with none.  Returns true when the caller
        // must treat the history as re-anchored (it always does today: either
        // the window was replayed, or it was dropped and a resync is due).
        bool restore_and_replay(const rollback_world& world, uint32_t anchor, uint32_t current_tick,
                                const std::map<std::string, row_ref>& rows,
                                const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at,
                                const std::function<void(uint8_t)>& report, const std::string& reason) {
            tick_state* at = find(anchor);
            if (!at) return false;
            const bool frozen = world.simulating && !world.simulating();
            const auto generation_now = [&](const std::string& entity) -> uint32_t {
                for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                    auto found = it->tracked.generations.find(entity);
                    if (found != it->tracked.generations.end()) return found->second;
                }
                return 0;
            };
            const auto record_at = [&](const std::string& entity) -> const bmmo_physics_body_state* {
                auto it = at->bodies.find(entity);
                if (it == at->bodies.end()) return nullptr;
                if (generation_in(*at, entity) != generation_now(entity)) return nullptr;
                return &it->second;
            };
            // 2. restore tick T.  The world is one interacting system: a body
            //    left where it is while the others rewind meets the replayed
            //    ball at the wrong pose and pushes it out (the rope-and-sack
            //    case, where restoring only the breaching half tore the pair
            //    apart).  So every body we have an opinion about goes back to
            //    tick T together - from its row when the snapshot carries one,
            //    otherwise from its own record - and a body we have no opinion
            //    about is left alone rather than guessed at.
            const rollback_tracked tracked = at->tracked;
            const auto extra_at = [&](const std::string& entity) -> const bmmo_physics_core_extra* {
                const auto it = at->extras.find(entity);
                return it == at->extras.end() ? nullptr : &it->second;
            };
            std::map<std::string, restore> plan;
            for (const auto& entity: restore_candidates(anchor, rows)) {
                const auto row = rows.find(entity);
                const bmmo_physics_body_state* recorded = record_at(entity);
                // A body born after T outranks its row: the row describes the
                // body this one replaced (or one the server does not have yet).
                const tick_state* birth = recorded ? nullptr
                                                   : birth_record(entity, anchor, generation_now(entity));
                restore item;
                if (birth) {
                    // It did not exist at T: it was created during the window
                    // we are about to replay.  Nothing can put it back where it
                    // was at T, so it is parked at the pose it was born with,
                    // kept there through T+1..birth-1 (below) and released into
                    // the replay at its birth tick.
                    item.state = birth->bodies.find(entity)->second;
                    // Parked, and re-posed at its birth tick, in the sleep
                    // state its birth record has.  It used to be parked and
                    // re-posed AWAKE whatever the record said, and most sector
                    // bodies are born frozen (start_frozen, or calm from their
                    // first check): a see-saw or a bridge parked awake tilts
                    // under gravity during the replayed window and is then
                    // released awake - measured as the 0.3-0.6 m a rewind
                    // across a sector's births cost (scripts/sleep_divergence.py:
                    // the control had them asleep, the arm awake, from the
                    // first checkpoint after the rewind).  The birth record's
                    // freeze bookkeeping goes with the re-pose, so the first
                    // movement check after it sees the history the live run
                    // had.
                    item.mode = sleep_as_recorded_
                        ? (item.state.simulated ? wake_mode::wake : wake_mode::freeze) : wake_mode::wake;
                    item.repose_tick = birth->tick;
                    item.force_recheck = true;
                    if (const auto ex = birth->extras.find(entity); ex != birth->extras.end()) {
                        item.extra = ex->second;
                        item.have_extra = true;
                    }
                } else if (row != rows.end() && row->second.authoritative) {
                    item.state = state_of(*row->second.row);
                    item.mode = row->second.simulated ? wake_mode::wake : wake_mode::freeze;
                    item.from_row = true;
                } else if (recorded) {
                    // Our own record of T, with the sleep state we had THEN -
                    // the record's, not the live one.  The sleep state used to
                    // be kept as it is now, on the reasoning that the snapshot
                    // said nothing about it; but a body that fell asleep after
                    // T then replayed T+1.. asleep while the live frames ran it
                    // awake (and the other way round), and the replay diverged
                    // from the live run on exactly the bodies whose sleep
                    // decision fell inside the window.
                    item.state = *recorded;
                    item.mode = sleep_as_recorded_
                        ? (recorded->simulated ? wake_mode::wake : wake_mode::freeze) : wake_mode::keep;
                    // ...and with the impulses IVP had queued at T.  Only on
                    // this branch: a row is an authoritative pose from the
                    // server, which carries no queue, and the queue we happen
                    // to hold locally does not belong to the state being
                    // written (the zeroing is right for that case - it IS the
                    // hard set design 8.4 is about).  A body born inside the
                    // window is parked rather than restored, so its queue is
                    // not meaningful either.
                    if (const bmmo_physics_core_extra* extra = extra_at(entity)) {
                        item.extra = *extra;
                        item.have_extra = true;
                    }
                } else {
                    continue;   // no row, no record: no opinion
                }
                plan[entity] = item;
            }
            if (trace_plan_ && world.log && plan_traces_ < 16) {
                ++plan_traces_;
                int rows_used = 0, records = 0, births = 0;
                std::string born;
                for (const auto& [entity, item]: plan) {
                    if (item.repose_tick) {
                        ++births;
                        if (births <= 12) born += " " + entity + "@" + std::to_string(item.repose_tick);
                    } else if (item.from_row) {
                        ++rows_used;
                    } else {
                        ++records;
                    }
                }
                world.log("restore plan at tick " + std::to_string(anchor) + " (now " + std::to_string(current_tick)
                          + ", " + reason + "): " + std::to_string(records) + " from our records, "
                          + std::to_string(rows_used) + " from rows, " + std::to_string(births)
                          + " born in the window" + (born.empty() ? "" : ":" + born));
            }
            // The clock first: the beam below stamps every restored core with
            // the environment's time_of_last_psi, so the environment has to be
            // at T before any body is.  Only when a replay will follow - a
            // frozen clock or a too-far window sets the bodies and drops the
            // history, and a clock left behind with nothing to advance it
            // would stay behind.
            const uint32_t window = current_tick > anchor ? current_tick - anchor : 0;
            // The clock offset the restored calm references have to carry
            // when the clock itself stays live (the rewind is opt-in: IVP
            // keeps other time-anchored state a rewound clock turns into
            // negative deltas).  The references are absolute IVP times; with
            // the live clock `offset` seconds ahead of the anchor's, a
            // reference shifted by the same amount is as old relative to
            // every replayed step as it was in the live run.
            double calm_shift = 0.0;
            {
                bmmo_physics_clock_state now_clock{};
                if (at->have_clock && world.get_clock && world.get_clock(now_clock))
                    calm_shift = now_clock.current_time - at->clock.current_time;
            }
            if (restore_clock_ && at->have_clock && world.set_clock && !frozen
                    && window <= thresholds_.max_resim_ticks) {
                world.set_clock(at->clock);
                ++stats_.clock_rewinds;
                calm_shift = 0.0;
            }
            // The mechanism scripts' state at T, before the bodies: a force
            // block restored here re-creates or deletes its IVP controller,
            // and the replayed frames then toggle it from the right phase.
            // Only when a replay will follow - a frozen clock or a too-far
            // window sets the bodies and drops the history.
            const bool scripts_replayed = replay_scripts_ && at->have_scripts && world.restore_scripts && !frozen
                                        && window <= thresholds_.max_resim_ticks;
            // The live record of `current_tick`, kept aside before the replay
            // overwrites it: what the re-executed scripts must arrive at.
            std::vector<uint8_t> live_scripts;
            bool have_live_scripts = false;
            if (scripts_replayed) {
                if (const tick_state* live = find(current_tick); live && live->have_scripts) {
                    live_scripts = live->scripts;
                    have_live_scripts = true;
                }
                if (world.restore_scripts(at->scripts, anchor)) ++stats_.script_restores;
                else ++stats_.script_restore_failures;
            }
            // What a record's extra may write back, by switch and by sleep
            // state: a body being frozen gets no queue (a frozen body's queue
            // is meaningless), the unit countdown is written in a pass of its
            // own below, after every body is back and IVP has regrouped them.
            const auto extra_mask = [&](const restore& item, bool unit_pass) -> uint32_t {
                uint32_t fields = item.extra.fields;
                if (!restore_impulses_ || item.mode == wake_mode::freeze) fields &= ~BMMO_PHYSICS_EXTRA_IMPULSES;
                if (!restore_quaternions_) fields &= ~BMMO_PHYSICS_EXTRA_QUATS;
                if (!restore_calm_) fields &= ~BMMO_PHYSICS_EXTRA_CALM;
                if (!restore_unit_) fields &= ~BMMO_PHYSICS_EXTRA_UNIT;
                if (unit_pass) fields &= BMMO_PHYSICS_EXTRA_UNIT;
                else fields &= ~BMMO_PHYSICS_EXTRA_UNIT;
                return fields;
            };
            const auto write_extra = [&](const std::string& entity, const restore& item, bool unit_pass) {
                if (!item.have_extra || !world.set_body_extra) return;
                bmmo_physics_core_extra extra = item.extra;
                extra.fields = extra_mask(item, unit_pass);
                if (extra.fields == 0) return;
                if (extra.fields & BMMO_PHYSICS_EXTRA_CALM)
                    for (int k = 0; k < 2; ++k) extra.calm_time[k] += calm_shift;
                if (world.set_body_extra(entity, extra)) {
                    if (extra.fields & BMMO_PHYSICS_EXTRA_IMPULSES) ++stats_.impulses_restored;
                    if (extra.fields & BMMO_PHYSICS_EXTRA_CALM) ++stats_.calm_restored;
                    if (extra.fields & BMMO_PHYSICS_EXTRA_UNIT) ++stats_.units_restored;
                }
            };
            for (auto& [entity, item]: plan) {
                bmmo_physics_body_state live{};
                // A body destroyed after its last record simply has nothing to
                // restore: skip it and correct the rest of the world.  (The
                // engine cannot recreate it, and the caller's next record will
                // stop carrying it.)
                if (!world.get_body(entity, live)) continue;
                // A write that moves a body normally asks IVP to recheck its
                // contacts, because the optimized beam generates no collision
                // when a body lands inside another one.  But the recheck
                // rebuilds the body's mindist/hull bookkeeping, which is state
                // the rollback does not record - so it is also a candidate
                // CAUSE of a replay's infidelity, and this flag is how that is
                // measured rather than argued about.
                const bool recheck = recheck_on_restore_
                                   && (item.force_recheck || write_moves_body(live, item.state));
                world.set_body(entity, item.state, item.mode, recheck);
                // After the pose write, which zeroed the queue.  A body born in
                // the window is parked here and gets its record's extra at the
                // re-pose instead.
                if (!item.repose_tick) write_extra(entity, item, false);
                // What we wrote from a row is the authoritative state of T now:
                // without this the next snapshot of T compares against the
                // prediction we just discarded and rolls back again.
                if (!item.from_row) continue;
                auto existing = at->bodies.find(entity);
                if (existing != at->bodies.end()) existing->second = item.state;
            }
            // The unit countdowns last: a unit is several cores, grouped by
            // IVP after the rechecks above, so the write lands on the units as
            // they are now (last writer wins within a unit).
            for (auto& [entity, item]: plan)
                if (!item.repose_tick) write_extra(entity, item, true);
            // The friction contacts the anchor's record had and the live world
            // lacks - a contact that ended inside the window.  IVP creates a
            // contact only on an impact, and a body put back on the floor it
            // has since rolled off has none coming: it rests on nothing until
            // it leaves (measured: the own ball 0.76 m low after a rewind
            // whose anchor still had the floor, exact when the contact lived
            // on).  After every body is back, so the partners stand where they
            // did; a body born in the window has no record of its own here.
            if (restore_contacts_ && world.set_contacts)
                for (auto& [entity, item]: plan) {
                    if (item.repose_tick) continue;
                    const auto it = at->contacts.find(entity);
                    if (it == at->contacts.end()) continue;
                    const int made = world.set_contacts(entity, it->second, contact_flags_);
                    if (made < 0) ++stats_.contact_restore_failures;
                    else stats_.contacts_restored += static_cast<uint64_t>(made);
                }
            for (const auto& [entity, nav]: at->navs) world.set_nav(entity, nav);

            // 3. re-simulate T+1 .. current with the recorded inputs
            const uint32_t lag = current_tick > anchor ? current_tick - anchor : 0;
            if (frozen) {
                // the scripts stopped the local clock: the authoritative
                // states stand until the clock runs again.  No re-simulation
                // runs, so no record can carry this tick, and the caller's tick
                // counter keeps running: truncating would keep T but drop
                // T+1..current, and every later snapshot of that range would be
                // unmatched.  Drop the history; the caller sees frozen grow and
                // must re-anchor (resync) before recording again.
                ++stats_.frozen;
                report(6);
                invalidate_history();
                return true;
            }
            ++stats_.rollbacks;
            if (lag > thresholds_.max_resim_ticks) {
                ++stats_.too_far;
                if (world.log) world.log("rollback: lag " + std::to_string(lag) + " ticks, bodies set without re-simulation");
                report(5);
                // No re-simulation runs, so no record can carry this tick and
                // the caller's tick counter keeps running: keeping the records
                // at or before T would leave a hole T+1..current that makes
                // every later snapshot of that range unmatched and feeds later
                // inputs time-shifted.  Drop the history; the caller sees
                // too_far grow and must re-anchor (resync) before recording
                // again.
                invalidate_history();
                return true;
            }
            // A replayed tick must use recorded input, not the live keyboard.
            std::vector<std::string> polled;
            auto stop_polling = [&](const rollback_tracked& t) {
                if (t.own_entity.empty() || !t.own_polls) return;
                for (const auto& name: polled) if (name == t.own_entity) return;
                polled.push_back(t.own_entity);
                world.nav_poll(t.own_entity, false);
            };
            stop_polling(tracked);
            for (uint32_t t = anchor + 1; t <= current_tick; ++t)
                if (tick_state* recorded = find(t)) stop_polling(recorded->tracked);
            for (uint32_t t = anchor + 1; t <= current_tick; ++t) {
                tick_state* recorded = find(t);
                const rollback_tracked& step_tracked = recorded ? recorded->tracked : tracked;
                std::map<std::string, input_frame> inputs;
                auto feed = [&](const std::string& entity, bool remote) {
                    input_frame frame{};
                    // A live remote step records our prediction, which may
                    // predate a key edge the server has since relayed. Reusing
                    // that prediction would reproduce the same divergence on
                    // every rollback. Own inputs are already exact and must
                    // stay attached to their historical entity across a trafo.
                    bool have = remote && input_at(entity, t, frame);
                    if (!have && recorded) {
                        auto it = recorded->inputs.find(entity);
                        if (it != recorded->inputs.end()) { frame = it->second; have = true; }
                    }
                    if (!have && !remote) have = input_at(entity, t, frame);
                    if (have) {
                        world.nav_input(entity, frame);
                        inputs[entity] = frame;
                    }
                };
                if (!step_tracked.own_entity.empty()) feed(step_tracked.own_entity, false);
                for (const auto& remote: step_tracked.remote_entities) feed(remote, true);
                std::string trace;
                if (verbose_ && world.log && resim_traces_ < 60) {
                    for (const auto& remote: step_tracked.remote_entities) {
                        bmmo_physics_body_state before{};
                        if (!world.get_body(remote, before)) continue;
                        char buf[200];
                        std::snprintf(buf, sizeof(buf), " %s before=(%.3f,%.3f,%.3f)%s v=(%.3f,%.3f,%.3f)", remote.c_str(),
                                      before.position[0], before.position[1], before.position[2], before.simulated ? "" : "z",
                                      before.linear[0], before.linear[1], before.linear[2]);
                        trace += buf;
                    }
                }
                if (world.pre_step) world.pre_step(t);
                if (!world.step()) break;
                ++stats_.resim_ticks;
                // A body born inside the replayed window has been sitting at
                // its birth pose since the restore; the ticks up to and
                // including its birth tick may have dragged it (gravity, a
                // contact), so after that tick's step it is put back exactly
                // where its birth record has it - that record IS the state the
                // live frame left at the end of the birth tick.  From the next
                // tick on it is an ordinary replayed body.
                for (const auto& [entity, item]: plan) {
                    if (item.repose_tick != t) continue;
                    bmmo_physics_body_state live{};
                    if (!world.get_body(entity, live)) continue;
                    world.set_body(entity, item.state, item.mode, true);
                    write_extra(entity, item, false);
                    write_extra(entity, item, true);
                }
                if (!trace.empty()) {
                    for (const auto& remote: step_tracked.remote_entities) {
                        bmmo_physics_body_state after{};
                        if (!world.get_body(remote, after)) continue;
                        char buf[200];
                        std::snprintf(buf, sizeof(buf), " %s after=(%.3f,%.3f,%.3f)%s v=(%.3f,%.3f,%.3f)", remote.c_str(),
                                      after.position[0], after.position[1], after.position[2], after.simulated ? "" : "z",
                                      after.linear[0], after.linear[1], after.linear[2]);
                        trace += buf;
                    }
                    ++resim_traces_;
                    world.log("resim tick " + std::to_string(t) + ":" + trace);
                }
                tick_state state;
                state.tick = t;
                state.tracked = recorded ? recorded->tracked : tracked;
                state.inputs = std::move(inputs);
                capture(world, state);
                // The last replayed tick: do the re-executed scripts stand where
                // the live frames left them?  A difference is state the
                // snapshot does not carry (solo), or a sensor that decided
                // differently on the corrected world (networked, by design).
                if (t == current_tick && scripts_replayed && have_live_scripts && state.have_scripts
                        && state.scripts != live_scripts) {
                    bool logical = true;
                    std::string where;
                    if (world.diff_scripts) where = world.diff_scripts(live_scripts, state.scripts, logical);
                    if (logical) ++stats_.script_mismatches; else ++stats_.script_float_mismatches;
                    if (world.log && (logical ? script_mismatch_logs_ : script_float_logs_)++ < 8)
                        world.log(std::string("script replay: the scripts re-executed to tick ") + std::to_string(t)
                                  + (logical ? " differ from the live record: " : " differ in float parameters only: ")
                                  + (where.empty() ? std::to_string(state.scripts.size()) + " vs "
                                                       + std::to_string(live_scripts.size()) + " bytes" : where));
                }
                if (recorded) *recorded = std::move(state);
                else history_.push_back(std::move(state));
            }
            for (const auto& entity: polled) world.nav_poll(entity, true);
            if (world.log)
                world.log("rollback to tick " + std::to_string(anchor) + " (" + reason + "), re-simulated "
                          + std::to_string(lag) + " ticks");
            report(1);
            return true;
        }

    public:
        // The input this engine recorded as APPLIED for `entity` at `tick`.
        //
        // The caller needs it to decide whether a relayed remote input is news:
        // a remote ball is predicted with its last known frame, and the frame
        // the server actually applied arrives input_delay + a trip later.  If
        // it equals what the prediction used, the prediction was right and
        // nothing has to be replayed; if it differs, every tick from there on
        // was simulated from the wrong input and has to be re-simulated.  That
        // is the rollback trigger lockstep runs on - the authoritative snapshot
        // is only the fallback for a correction that arrives too late to
        // replay.
        // Whether `tick` is still in the history at all, i.e. whether there is
        // anything to replay from it.  Distinguishing this from "that body had
        // no input recorded at that tick" matters: the second case is news, not
        // a reason to do nothing - we simulated the tick with no input for a
        // body that in fact had one.
        bool knows_tick(uint32_t tick) const { return find_const(tick) != nullptr; }

        // Diagnostic only (see the recheck comment in restore_and_replay).
        void set_recheck_on_restore(bool enable) { recheck_on_restore_ = enable; }
        // Off reproduces the pre-v11 restore, for an A/B in one binary - which
        // is the only honest way to measure this, because two builds' plain
        // runs disagree by more than the effect does.
        void set_restore_impulses(bool enable) { restore_impulses_ = enable; }
        void set_restore_quaternions(bool enable) { restore_quaternions_ = enable; }
        void set_restore_clock(bool enable) { restore_clock_ = enable; }
        // v13: IVP's freeze bookkeeping.  Off reproduces the restore that
        // left the calm references, the unit countdown and the live sleep
        // state in place - each half an A/B in one binary.
        void set_restore_calm(bool enable) { restore_calm_ = enable; }
        void set_restore_unit(bool enable) { restore_unit_ = enable; }
        void set_sleep_as_recorded(bool enable) { sleep_as_recorded_ = enable; }
        // v15: the friction contacts a record had and the world lacks are
        // re-created at the anchor.  Off reproduces the restore that left a
        // restored body resting on nothing.
        void set_restore_contacts(bool enable) { restore_contacts_ = enable; }
        // Which recorded fields a re-created contact gets, and whether a body
        // with some contact left is left alone (BMMO_PHYSICS_CONTACT_*).
        void set_contact_flags(uint32_t flags) { contact_flags_ = flags; }
        // The script replay (game/script_replay.hpp).  Off: the records carry
        // no script state and a replay is physics only, as before.
        void set_replay_scripts(bool enable) { replay_scripts_ = enable; }
        // Log which branch every body of a restore took (capped at 16).
        void set_trace_plan(bool enable) { trace_plan_ = enable; }

        bool recorded_input(const std::string& entity, uint32_t tick, input_frame& out) const {
            const tick_state* at = find_const(tick);
            if (!at) return false;
            const auto it = at->inputs.find(entity);
            if (it == at->inputs.end()) return false;
            out = it->second;
            return true;
        }

        // Re-anchor on a tick in the past because the WORLD changed there, not
        // because an authoritative snapshot disagreed: the caller has created a
        // body for a lifecycle event stamped at `anchor` that arrived late (a
        // remote ball is relayed a round trip after it was born).  The new body
        // is in no record at `anchor`, so the restore leaves it exactly where
        // the caller put it and the replay steps it from there, which is what
        // makes every client's body set agree tick for tick without waiting for
        // a snapshot to notice.
        //
        // `anchor` must still be in the history; too late and there is nothing
        // to rewind to, which is the point at which a snapshot or a resync has
        // to take over.  Returns false then, and the caller must re-anchor.
        bool rewind_to(const rollback_world& world, uint32_t anchor, uint32_t current_tick,
                       const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at,
                       const std::string& reason) {
            if (!find(anchor)) {
                ++stats_.unmatched;
                return false;
            }
            // NOT stats_.mismatched: that counts authoritative snapshots this
            // engine disagreed with, and it is read as the measure of how far
            // the client is from the server.  A rewind the caller asked for is
            // not a disagreement with a snapshot and must not pollute it.
            ++stats_.rewinds;
            const std::map<std::string, row_ref> no_rows;
            auto no_report = [](uint8_t) {};
            return restore_and_replay(world, anchor, current_tick, no_rows, input_at, no_report, reason);
        }

    private:
        struct tick_state {
            uint32_t tick = 0;
            rollback_tracked tracked;
            std::map<std::string, bmmo_physics_body_state> bodies;
            // Parallel to `bodies`, and deliberately a separate map: a body we
            // recorded OURSELVES has a queued-impulse state, a body an
            // authoritative snapshot row describes does not, and the absence
            // is the information that tells the restore which it is holding.
            std::map<std::string, bmmo_physics_core_extra> extras;
            // The environment's clock at the end of this tick, when the world
            // provides it.
            bmmo_physics_clock_state clock{};
            bool have_clock = false;
            // The mechanism scripts' state at the end of this tick (game/script_replay.hpp).
            std::vector<uint8_t> scripts;
            bool have_scripts = false;
            std::map<std::string, bmmo_physics_nav_state> navs;
            std::map<std::string, input_frame> inputs;   // applied at this tick, per navigated ball
            // The friction contacts each recorded body had at the end of this
            // tick (bodies with none are absent).
            std::map<std::string, std::vector<bmmo_physics_contact>> contacts;
        };


        // One body's part in the restore of tick T.
        struct restore {
            bmmo_physics_body_state state{};
            wake_mode mode = wake_mode::keep;
            bool from_row = false;
            bool force_recheck = false;
            uint32_t repose_tick = 0;    // 0: none - re-pose at this replayed tick
            // Only set when the state came from our OWN record (the recorded
            // branch of the planning loop) or from the birth record of a body
            // born inside the window (written at its re-pose, not at the
            // parking write).
            bmmo_physics_core_extra extra{};
            bool have_extra = false;
        };

        static uint32_t generation_in(const tick_state& state, const std::string& entity) {
            auto it = state.tracked.generations.find(entity);
            return it == state.tracked.generations.end() ? 0u : it->second;
        }

        static bmmo_physics_body_state state_of(const body_state& row) {
            bmmo_physics_body_state state{};
            for (int k = 0; k < 3; ++k) {
                state.position[k] = row.position[k];
                state.linear[k] = row.linear[k];
                state.angular[k] = row.angular[k];
            }
            for (int k = 0; k < 4; ++k) state.rotation[k] = row.rotation[k];
            state.simulated = (row.flags & BODY_FLAG_SIMULATED) != 0;
            return state;
        }

        // Does writing `target` actually move the body?  A write that does not
        // cannot touch anything new, so it skips the mindist recheck.
        static bool write_moves_body(const bmmo_physics_body_state& live, const bmmo_physics_body_state& target) {
            double dp = 0.0, dv = 0.0;
            for (int k = 0; k < 3; ++k) {
                dp += (target.position[k] - live.position[k]) * (target.position[k] - live.position[k]);
                dv += (static_cast<double>(target.linear[k]) - live.linear[k])
                    * (static_cast<double>(target.linear[k]) - live.linear[k]);
            }
            return std::sqrt(dp) > kRecheckPositionM || std::sqrt(dv) > kRecheckVelocityMs;
        }

        // The oldest record that carries `entity` in its current lifetime: the
        // tick the body we have now was born at, as far as the history knows.
        const tick_state* find_const(uint32_t tick) const {
            for (const auto& state: history_) if (state.tick == tick) return &state;
            return nullptr;
        }

        const tick_state* birth_record(const std::string& entity, uint32_t after, uint32_t generation) const {
            for (const auto& state: history_) {
                if (state.bodies.find(entity) == state.bodies.end()) continue;
                if (generation_in(state, entity) != generation) continue;
                return state.tick > after ? &state : nullptr;
            }
            return nullptr;
        }

        // Every name the restore may have an opinion about: recorded at T,
        // carried by a row, or recorded somewhere in the window we are about to
        // replay (a body born mid-window still has to be held at its birth pose
        // while the earlier ticks are stepped).
        std::vector<std::string> restore_candidates(uint32_t tick, const std::map<std::string, row_ref>& rows) const {
            std::vector<std::string> names;
            for (const auto& state: history_) {
                if (state.tick < tick) continue;
                for (const auto& [entity, body]: state.bodies) names.push_back(entity);
            }
            for (const auto& [entity, row]: rows) names.push_back(entity);
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        void capture(const rollback_world& world, tick_state& state) const {
            auto grab = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_body_state body{};
                if (world.get_body(entity, body)) state.bodies[entity] = body;
                // A tracked name the world no longer knows must not survive an
                // amendment: a later rollback would write the body back.
                else { state.bodies.erase(entity); state.extras.erase(entity); state.contacts.erase(entity); return; }
                bmmo_physics_core_extra extra{};
                if (world.get_body_extra && world.get_body_extra(entity, extra)) state.extras[entity] = extra;
                else state.extras.erase(entity);
                std::vector<bmmo_physics_contact> contacts;
                if (world.get_contacts && world.get_contacts(entity, contacts) > 0 && !contacts.empty()) state.contacts[entity] = std::move(contacts);
                else state.contacts.erase(entity);
            };
            grab(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab(remote);
            for (const auto& mechanism: state.tracked.mechanisms) grab(mechanism);
            auto grab_nav = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_nav_state nav{};
                if (world.get_nav(entity, nav)) state.navs[entity] = nav;
                else state.navs.erase(entity);
            };
            grab_nav(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab_nav(remote);
            bmmo_physics_clock_state clock{};
            state.have_clock = world.get_clock && world.get_clock(clock);
            if (state.have_clock) state.clock = clock;
            state.have_scripts = replay_scripts_ && world.capture_scripts && world.capture_scripts(state.scripts);
            if (!state.have_scripts) state.scripts.clear();
        }

        tick_state* find(uint32_t tick) {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                if (it->tick == tick) return &*it;
            return nullptr;
        }

        struct tolerance {
            double position = 0.0;
            double velocity = 0.0;
        };

        tolerance tolerance_for(body_kind kind, double scale) const {
            const tolerance base = kind == body_kind::Mechanism
                ? tolerance{thresholds_.mechanism_position, thresholds_.mechanism_velocity}
                : tolerance{thresholds_.position, thresholds_.velocity};
            return {base.position * scale, base.velocity * scale};
        }

        // How far to widen the pair for a body predicted without the input that
        // drove it, over a window `lag` ticks deep (rollback_thresholds).
        double blind_scale(uint32_t lag) const {
            if (thresholds_.blind_reference_ticks == 0) return 1.0;
            const double s = static_cast<double>(lag) / thresholds_.blind_reference_ticks;
            if (s < 1.0) return 1.0;
            return s > thresholds_.blind_scale_max ? thresholds_.blind_scale_max : s;
        }

        rollback_thresholds thresholds_;
        std::deque<tick_state> history_;
        rollback_stats stats_;
        uint32_t detailed_logs_ = 0;   // mismatch detail lines logged (capped)
        uint32_t resim_traces_ = 0;    // re-simulation trace lines logged (capped)
        bool verbose_ = false;
        bool recheck_on_restore_ = true;
        bool restore_impulses_ = true;
        bool restore_quaternions_ = true;
        bool restore_clock_ = true;
        bool restore_calm_ = true;
        bool restore_contacts_ = true;
        uint32_t contact_flags_ = BMMO_PHYSICS_CONTACT_DEFAULT;
        bool restore_unit_ = true;
        bool sleep_as_recorded_ = true;
        bool replay_scripts_ = true;
        uint32_t script_mismatch_logs_ = 0;
        uint32_t script_float_logs_ = 0;
        bool trace_plan_ = false;
        uint32_t plan_traces_ = 0;
    };
}
