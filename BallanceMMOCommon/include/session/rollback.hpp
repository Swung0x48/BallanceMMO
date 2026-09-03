#pragma once

// Client-side rollback (design 9.6): the client runs ahead of the server's
// confirmed progress and predicts; when the authoritative snapshot of tick
// T disagrees with what the client recorded at T, every tracked body and
// navigation replica is restored to T and the ticks T+1 .. now are
// re-simulated (physics + navigation only, no scripts) from the recorded
// inputs.  Pure logic over a small world adapter, shared by the retail mod
// (through the physics bridge) and the headless session client.

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
        std::function<void(const std::string&)> log;
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
        double position = 0.001;   // metres
        double velocity = 0.01;    // m/s
        size_t history_ticks = 64;
        uint32_t max_resim_ticks = 40;   // give up (hard set only) beyond this lag
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

        void clear() {
            history_.clear();
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
            if (!history_.empty() && history_.back().tick >= tick) {
                // re-recording (resim) or a numbering restart: drop from here
                while (!history_.empty() && history_.back().tick >= tick) history_.pop_back();
            }
            history_.push_back(std::move(state));
            while (history_.size() > thresholds_.history_ticks) history_.pop_front();
        }

        // Authoritative snapshot of `tick`.  `entity_of` maps a snapshot body
        // to the local entity name (empty = not tracked here); `input_at`
        // supplies the input a navigated ball had at a tick during the
        // re-simulation (the recorded own input, the relayed remote input).
        // Returns true when a rollback happened.
        bool on_snapshot(const rollback_world& world, const session_snapshot_msg& snapshot, uint32_t current_tick,
                         const std::function<std::string(const body_state&)>& entity_of,
                         const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at) {
            ++stats_.snapshots;
            tick_state* at = find(snapshot.tick);
            if (!at) {
                ++stats_.unmatched;
                return false;
            }
            // 1. compare
            bool mismatch = false;
            double worst = 0.0;
            std::string worst_entity, detail;
            std::vector<std::pair<std::string, const body_state*>> authoritative;
            for (const auto& body: snapshot.bodies) {
                const std::string entity = entity_of(body);
                if (entity.empty()) continue;
                auto it = at->bodies.find(entity);
                // Not tracked at that tick: the body did not exist here yet
                // (just physicalized), or it is the entity a trafo has since
                // replaced - the row belongs to the ball of tick T, not to
                // this one.  Restoring it would teleport the new ball to the
                // old one's pose; the next snapshot has a tick we recorded.
                if (it == at->bodies.end()) continue;
                authoritative.emplace_back(entity, &body);
                double dp = 0.0, dv = 0.0;
                for (int k = 0; k < 3; ++k) {
                    dp += (body.position[k] - it->second.position[k]) * (body.position[k] - it->second.position[k]);
                    dv += (static_cast<double>(body.linear[k]) - it->second.linear[k])
                        * (static_cast<double>(body.linear[k]) - it->second.linear[k]);
                }
                dp = std::sqrt(dp);
                dv = std::sqrt(dv);
                if (dp > worst) { worst = dp; worst_entity = entity; }
                if (dp > thresholds_.position || dv > thresholds_.velocity) {
                    mismatch = true;
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
            ++stats_.mismatched;
            stats_.last_mismatch = worst_entity;
            const bool frozen = world.simulating && !world.simulating();
            if (!detail.empty() && world.log && !frozen && detailed_logs_++ < 40)
                world.log("mismatch at tick " + std::to_string(snapshot.tick) + " (local " + std::to_string(current_tick) + "):"
                          + detail);

            // 2. restore tick T: snapshot bodies from the server, the other
            //    tracked bodies and every navigation replica from the history
            const rollback_tracked tracked = at->tracked;
            for (const auto& [entity, body]: authoritative) {
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
                for (const auto& [name, body]: authoritative) from_server = from_server || name == entity;
                if (!from_server) world.set_body(entity, state, state.simulated);
            }
            for (const auto& [entity, nav]: at->navs) world.set_nav(entity, nav);

            // 3. re-simulate T+1 .. current with the recorded inputs
            const uint32_t lag = current_tick > snapshot.tick ? current_tick - snapshot.tick : 0;
            if (frozen) {
                // the scripts stopped the local clock: the authoritative
                // states stand until the clock runs again
                ++stats_.frozen;
                truncate_after(snapshot.tick);
                return true;
            }
            ++stats_.rollbacks;
            if (lag > thresholds_.max_resim_ticks) {
                ++stats_.too_far;
                if (world.log) world.log("rollback: lag " + std::to_string(lag) + " ticks, bodies set without re-simulation");
                truncate_after(snapshot.tick);
                return true;
            }
            // The window can span a trafo: the own ball is a different entity
            // after it, and every recorded tick carries the entity it was
            // simulated with.  Take polling off all of them (a replayed tick
            // must use its recorded input, not the live keyboard) and drive
            // each tick's own set below.
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
                auto feed = [&](const std::string& entity) {
                    input_frame frame{};
                    bool have = false;
                    if (recorded) {
                        auto it = recorded->inputs.find(entity);
                        if (it != recorded->inputs.end()) { frame = it->second; have = true; }
                    }
                    if (!have) have = input_at(entity, t, frame);
                    if (have) {
                        world.nav_input(entity, frame);
                        inputs[entity] = frame;
                    }
                };
                if (!step_tracked.own_entity.empty()) feed(step_tracked.own_entity);
                for (const auto& remote: step_tracked.remote_entities) feed(remote);
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
            };
            grab(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab(remote);
            for (const auto& mechanism: state.tracked.mechanisms) grab(mechanism);
            auto grab_nav = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_nav_state nav{};
                if (world.get_nav(entity, nav)) state.navs[entity] = nav;
            };
            grab_nav(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab_nav(remote);
        }

        tick_state* find(uint32_t tick) {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                if (it->tick == tick) return &*it;
            return nullptr;
        }

        void truncate_after(uint32_t tick) {
            while (!history_.empty() && history_.back().tick > tick) history_.pop_back();
        }

        rollback_thresholds thresholds_;
        std::deque<tick_state> history_;
        rollback_stats stats_;
        uint32_t detailed_logs_ = 0;   // mismatch detail lines logged (capped)
        uint32_t resim_traces_ = 0;    // re-simulation trace lines logged (capped)
        bool verbose_ = false;
    };
}
