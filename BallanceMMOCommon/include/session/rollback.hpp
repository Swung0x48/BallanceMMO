#pragma once

// Client-side rollback (design 9.6): the client runs ahead of the server's
// confirmed progress and predicts; when the authoritative snapshot of tick
// T disagrees with what the client recorded at T, the bodies that breached
// their own tolerance are restored to the server pose, every other tracked
// body and navigation replica is restored to its own record for T, and the
// ticks T+1 .. now are re-simulated (physics + navigation only, no scripts)
// from the recorded inputs.  Pure logic over a small world adapter, shared by
// the retail mod (through the physics bridge) and the headless session client.

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

    struct rollback_world {
        std::function<bool(const std::string& entity, bmmo_physics_body_state& out)> get_body;
        // wake: ensure_in_simulation when true, freeze when false
        std::function<bool(const std::string& entity, const bmmo_physics_body_state& state, bool wake)> set_body;
        std::function<bool(const std::string& entity, bmmo_physics_nav_state& out)> get_nav;
        std::function<bool(const std::string& entity, const bmmo_physics_nav_state& state)> set_nav;
        // explicit input for the next physics step of a navigated ball
        std::function<bool(const std::string& entity, const input_frame& frame)> nav_input;
        // polling mode on/off (the own ball reads the keyboard itself in live frames)
        std::function<bool(const std::string& entity, bool enable)> nav_poll;
        std::function<bool()> step;   // one physics tick
        // false while the local physics clock is stopped by the retail scripts
        // (Level 1 tutorial, pause menu): bodies are snapped, nothing is
        // re-simulated.  Optional; missing means "simulating".
        std::function<bool()> simulating;
        // Called before each re-simulated tick, with the tick about to be stepped.
        // The client uses it to re-pose the server-authoritative mechanism bodies,
        // which are not in the tracked set (Option A): without it the resim replays
        // the ball against one frozen mechanism pose.  Null on the server/tests.
        std::function<void(uint32_t tick)> pre_step;
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
        uint64_t unmatched = 0;        // snapshot tick not in the history
        uint64_t too_far = 0;          // lag beyond max_resim_ticks: bodies set, no re-simulation
        uint64_t frozen = 0;           // local physics clock stopped: bodies snapped, no re-simulation
        double last_error = 0.0, max_error = 0.0;
        std::string last_mismatch;     // entity of the last mismatch
    };

    class rollback_engine {
    public:
        explicit rollback_engine(rollback_thresholds thresholds = {}) : thresholds_(thresholds) {}

        // Body creation/destruction is not replayable by this adapter. A
        // lifecycle boundary starts a new history, even when a respawn reuses
        // the same entity name. Keep session diagnostics across that boundary.
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
        // remote), for a later re-simulation.
        void record(const rollback_world& world, uint32_t tick, const rollback_tracked& tracked,
                    const std::map<std::string, input_frame>& inputs_applied) {
            tick_state state;
            state.tick = tick;
            state.tracked = tracked;
            state.inputs = inputs_applied;
            capture(world, state);
            // Also catch callers discovering a new/missing body or navigation
            // replica at record time. No replay may span different live sets.
            const auto same_entities = [](const auto& a, const auto& b) {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                    [](const auto& left, const auto& right) { return left.first == right.first; });
            };
            if (!history_.empty() && (!same_entities(history_.back().bodies, state.bodies)
                    || !same_entities(history_.back().navs, state.navs)))
                invalidate_history();
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
            // 1. compare
            bool mismatch = false;
            double worst = 0.0;        // largest position error: the log lines and the stats
            double worst_breach = 0.0; // furthest past a threshold: the journal's subject
            std::string worst_entity, detail;
            std::vector<std::pair<std::string, const body_state*>> authoritative;
            std::vector<std::string> breached;   // rows snapped to the server pose
            for (const auto& body: snapshot.bodies) {
                const std::string entity = entity_of(body);
                if (entity.empty()) continue;
                auto it = at->bodies.find(entity);
                // A currently mapped body without a state at T cannot be
                // left in the world while the others rewind: world.step()
                // advances it too. Reject the WHOLE snapshot before any write.
                if (it == at->bodies.end()) {
                    invalidate_history();
                    ++stats_.unmatched;
                    report(7);
                    return false;
                }
                authoritative.emplace_back(entity, &body);
                // Each row is judged by the tolerance of its own kind: a
                // mechanism row at the same error as a ball is not a
                // divergence.
                const tolerance tol = tolerance_for(body.kind);
                double dp = 0.0, dv = 0.0;
                for (int k = 0; k < 3; ++k) {
                    dp += (body.position[k] - it->second.position[k]) * (body.position[k] - it->second.position[k]);
                    dv += (static_cast<double>(body.linear[k]) - it->second.linear[k])
                        * (static_cast<double>(body.linear[k]) - it->second.linear[k]);
                }
                dp = std::sqrt(dp);
                dv = std::sqrt(dv);
                if (dp > worst) {
                    worst = dp;
                    worst_entity = entity;
                }
                // The mismatch can be a velocity one (for a ball, 0.01 m/s
                // trips before 1 mm of drift does), so the record names the
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
                        correction.local_position[k] = it->second.position[k];
                        correction.server_position[k] = body.position[k];
                    }
                }
                if (dp > tol.position || dv > tol.velocity) {
                    mismatch = true;
                    breached.push_back(entity);
                    if (detailed_logs_ < 40) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), " %s dp=%.4f dv=%.3f local=(%.3f,%.3f,%.3f)%s server=(%.3f,%.3f,%.3f)%s",
                                      entity.c_str(), dp, dv, it->second.position[0], it->second.position[1],
                                      it->second.position[2], it->second.simulated ? "" : "z", body.position[0],
                                      body.position[1], body.position[2], (body.flags & BODY_FLAG_SIMULATED) ? "" : "z");
                        detail += buf;
                    }
                }
            }
            stats_.last_error = worst;
            if (worst > stats_.max_error) stats_.max_error = worst;
            if (!mismatch) {
                ++stats_.matched;
                return false;
            }
            // A deletion can happen after the last record and before queued
            // snapshots run. Preflight every restore target before changing
            // any body, rather than partly restoring and replaying dead names.
            for (const auto& [entity, state]: at->bodies) {
                bmmo_physics_body_state live{};
                if (!world.get_body(entity, live)) {
                    invalidate_history();
                    ++stats_.unmatched;
                    report(7);
                    return false;
                }
            }
            ++stats_.mismatched;
            stats_.last_mismatch = worst_entity;
            report(0);
            const bool frozen = world.simulating && !world.simulating();
            if (!detail.empty() && world.log && !frozen && detailed_logs_++ < 40)
                world.log("mismatch at tick " + std::to_string(snapshot.tick) + " (local " + std::to_string(current_tick) + "):"
                          + detail);

            // 2. restore tick T: the bodies that breached their tolerance from
            //    the server, every other tracked body and every navigation
            //    replica from the history
            const rollback_tracked tracked = at->tracked;
            for (const auto& [entity, body]: authoritative) {
                // A body that agreed with the server stays on its own record
                // for T (below): snapping it to a sub-tolerance pose would
                // also copy the server's wake flag over its local one, which
                // resets the mechanism freeze timer on every snapshot.
                if (std::find(breached.begin(), breached.end(), entity) == breached.end()) continue;
                bmmo_physics_body_state state{};
                for (int k = 0; k < 3; ++k) {
                    state.position[k] = body->position[k];
                    state.linear[k] = body->linear[k];
                    state.angular[k] = body->angular[k];
                }
                for (int k = 0; k < 4; ++k) state.rotation[k] = body->rotation[k];
                const bool wake = (body->flags & BODY_FLAG_SIMULATED) != 0;
                world.set_body(entity, state, wake);
                // the recorded state of T becomes the authoritative one
                auto& recorded = at->bodies[entity];
                recorded = state;
                recorded.simulated = wake;
            }
            for (const auto& [entity, state]: at->bodies) {
                bool from_server = false;
                for (const auto& name: breached) from_server = from_server || name == entity;
                if (!from_server) world.set_body(entity, state, state.simulated);
            }
            for (const auto& [entity, nav]: at->navs) world.set_nav(entity, nav);

            // 3. re-simulate T+1 .. current with the recorded inputs
            const uint32_t lag = current_tick > snapshot.tick ? current_tick - snapshot.tick : 0;
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
            // Lifecycle changes invalidate history, so this window contains
            // only bodies that can all be restored before the first step.
            std::vector<std::string> polled;
            auto stop_polling = [&](const rollback_tracked& t) {
                if (t.own_entity.empty() || !t.own_polls) return;
                for (const auto& name: polled) if (name == t.own_entity) return;
                polled.push_back(t.own_entity);
                world.nav_poll(t.own_entity, false);
            };
            stop_polling(tracked);
            for (uint32_t t = snapshot.tick + 1; t <= current_tick; ++t)
                if (tick_state* recorded = find(t)) stop_polling(recorded->tracked);
            for (uint32_t t = snapshot.tick + 1; t <= current_tick; ++t) {
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
                if (world.pre_step) world.pre_step(t);   // Option A: re-pose the untracked authoritative bodies
                if (!world.step()) break;
                ++stats_.resim_ticks;
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
                if (recorded) *recorded = std::move(state);
                else history_.push_back(std::move(state));
            }
            for (const auto& entity: polled) world.nav_poll(entity, true);
            if (world.log)
                world.log("rollback to tick " + std::to_string(snapshot.tick) + " (" + worst_entity + " off by "
                          + std::to_string(worst) + " m), re-simulated " + std::to_string(lag) + " ticks");
            report(1);
            return true;
        }

    private:
        struct tick_state {
            uint32_t tick = 0;
            rollback_tracked tracked;
            std::map<std::string, bmmo_physics_body_state> bodies;
            std::map<std::string, bmmo_physics_nav_state> navs;
            std::map<std::string, input_frame> inputs;   // applied at this tick, per navigated ball
        };

        void capture(const rollback_world& world, tick_state& state) const {
            auto grab = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_body_state body{};
                if (world.get_body(entity, body)) state.bodies[entity] = body;
                // A tracked name the world no longer knows must not survive an
                // amendment: a later rollback would write the body back.
                else state.bodies.erase(entity);
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

        tolerance tolerance_for(body_kind kind) const {
            if (kind == body_kind::Mechanism)
                return {thresholds_.mechanism_position, thresholds_.mechanism_velocity};
            return {thresholds_.position, thresholds_.velocity};
        }

        rollback_thresholds thresholds_;
        std::deque<tick_state> history_;
        rollback_stats stats_;
        uint32_t detailed_logs_ = 0;   // mismatch detail lines logged (capped)
        uint32_t resim_traces_ = 0;    // re-simulation trace lines logged (capped)
        bool verbose_ = false;
    };
}
