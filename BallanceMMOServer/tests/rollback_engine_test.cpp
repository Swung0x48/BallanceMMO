// Unit tests for the client-side rollback engine (design 9.6) over a fake
// world: bodies move by their velocity each step, a navigated ball gains
// +1 m/s on x per step while key 0 is held, and every adapter call is
// recorded so the tests can check what the engine did to the world.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <session/rollback.hpp>

namespace {
    using bmmo::session::body_kind;
    using bmmo::session::body_state;
    using bmmo::session::input_frame;
    using bmmo::session::rollback_engine;
    using bmmo::session::rollback_tracked;
    using bmmo::session::rollback_world;

    constexpr double kDt = 1.0 / 66.0;

    struct fake_body {
        double position[3] = {};
        float linear[3] = {};
        bool simulated = true;
    };

    struct fake_world {
        std::map<std::string, fake_body> bodies;
        std::map<std::string, bmmo_physics_nav_state> navs;
        std::map<std::string, uint8_t> pending_keys;   // applied at the next step
        std::vector<std::string> calls;
        bool clock_running = true;
        int steps = 0;

        rollback_world adapter() {
            rollback_world w;
            w.get_body = [this](const std::string& entity, bmmo_physics_body_state& out) {
                auto it = bodies.find(entity);
                if (it == bodies.end()) return false;
                out = {};
                for (int k = 0; k < 3; ++k) {
                    out.position[k] = it->second.position[k];
                    out.linear[k] = it->second.linear[k];
                }
                out.rotation[3] = 1.0;
                out.simulated = it->second.simulated;
                return true;
            };
            w.set_body = [this](const std::string& entity, const bmmo_physics_body_state& state, bool wake) {
                calls.push_back("set_body " + entity + (wake ? " wake" : " freeze"));
                auto& body = bodies[entity];
                for (int k = 0; k < 3; ++k) {
                    body.position[k] = state.position[k];
                    body.linear[k] = state.linear[k];
                }
                body.simulated = wake;
                return true;
            };
            w.get_nav = [this](const std::string& entity, bmmo_physics_nav_state& out) {
                auto it = navs.find(entity);
                if (it == navs.end()) return false;
                out = it->second;
                return true;
            };
            w.set_nav = [this](const std::string& entity, const bmmo_physics_nav_state& state) {
                calls.push_back("set_nav " + entity);
                navs[entity] = state;
                return true;
            };
            w.nav_input = [this](const std::string& entity, const input_frame& frame) {
                calls.push_back("nav_input " + entity + " keys=" + std::to_string(frame.keys));
                pending_keys[entity] = frame.keys;
                return true;
            };
            w.nav_poll = [this](const std::string& entity, bool enable) {
                calls.push_back(std::string("nav_poll ") + entity + (enable ? " on" : " off"));
                return true;
            };
            w.step = [this]() {
                calls.push_back("step");
                ++steps;
                for (auto& [name, body]: bodies) {
                    if (!body.simulated) continue;
                    auto keys = pending_keys.find(name);
                    if (keys != pending_keys.end() && (keys->second & 1u)) body.linear[0] += 1.0f;
                    for (int k = 0; k < 3; ++k) body.position[k] += body.linear[k] * kDt;
                }
                pending_keys.clear();
                return true;
            };
            w.simulating = [this]() { return clock_running; };
            w.log = [this](const std::string& text) { calls.push_back("log " + text); };
            return w;
        }

        int count(const std::string& prefix) const {
            int n = 0;
            for (const auto& call: calls) n += call.rfind(prefix, 0) == 0 ? 1 : 0;
            return n;
        }
    };

    body_state ball_body(uint32_t owner, const fake_body& body) {
        body_state out;
        out.kind = body_kind::Ball;
        out.owner = owner;
        for (int k = 0; k < 3; ++k) {
            out.position[k] = body.position[k];
            out.linear[k] = body.linear[k];
        }
        out.rotation[3] = 1.0;
        out.flags = body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0;
        return out;
    }

    bmmo::session_snapshot_msg snapshot_of(uint32_t tick, const std::vector<body_state>& bodies) {
        bmmo::session_snapshot_msg msg;
        msg.tick = tick;
        msg.full = 1;
        msg.bodies = bodies;
        return msg;
    }

    // The own ball is player 1 / entity "Own", the remote ball player 2 /
    // entity "Remote".
    std::string entity_of(const body_state& body) {
        if (body.kind != body_kind::Ball) return {};
        return body.owner == 1 ? "Own" : body.owner == 2 ? "Remote" : "";
    }

    // Records ticks 1..last: the own ball holds key 0 from `key_from` on.
    void run_ticks(fake_world& world, rollback_engine& engine, uint32_t last, uint32_t key_from,
                   std::map<uint32_t, input_frame>& own_inputs) {
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.own_polls = true;
        tracked.remote_entities = {"Remote"};
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= last; ++tick) {
            input_frame own{};
            own.keys = tick >= key_from ? 1 : 0;
            own.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            world.pending_keys["Own"] = own.keys;
            w.step();
            own_inputs[tick] = own;
            std::map<std::string, input_frame> applied{{"Own", own}, {"Remote", input_frame{}}};
            engine.record(w, tick, tracked, applied);
        }
        world.calls.clear();
        world.steps = 0;
    }
}

TEST(RollbackEngine, MatchingSnapshotIsNotARollback) {
    fake_world world;
    world.bodies["Own"].linear[0] = 1.0f;
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    // the server agrees with what was recorded at tick 2
    fake_body own_at_2 = world.bodies["Own"];
    own_at_2.position[0] = 2.0 * kDt;
    const auto snapshot = snapshot_of(2, {ball_body(1, own_at_2), ball_body(2, world.bodies["Remote"])});
    const bool rolled = engine.on_snapshot(world.adapter(), snapshot, 4, entity_of,
                                           [&](const std::string&, uint32_t, input_frame&) { return false; });
    EXPECT_FALSE(rolled);
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
}

TEST(RollbackEngine, UnmatchedTickIsCounted) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 3, 100, own_inputs);
    const auto snapshot = snapshot_of(9, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 3, entity_of,
                                    [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(world.steps, 0);
}

TEST(RollbackEngine, MismatchRestoresAndResimulatesWithRecordedInputs) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    // key 0 held from tick 3: the own ball accelerates at ticks 3 and 4
    run_ticks(world, engine, 4, 3, own_inputs);
    ASSERT_NEAR(world.bodies["Own"].linear[0], 2.0f, 1e-6f);

    // the server had the own ball 0.5 m further at tick 2, still at rest
    fake_body server_own;
    server_own.position[0] = 0.5;
    fake_body server_remote = world.bodies["Remote"];
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    auto w = world.adapter();
    const bool rolled = engine.on_snapshot(w, snapshot, 4, entity_of,
                                           [&](const std::string& entity, uint32_t tick, input_frame& out) {
                                               if (entity != "Own") return false;
                                               auto it = own_inputs.find(tick);
                                               if (it == own_inputs.end()) return false;
                                               out = it->second;
                                               return true;
                                           });
    EXPECT_TRUE(rolled);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);
    EXPECT_EQ(world.steps, 2);
    EXPECT_EQ(world.count("set_body Own wake"), 1);
    EXPECT_EQ(world.count("set_body Remote wake"), 1);
    EXPECT_EQ(world.count("set_nav"), 2);
    // polling paused for the re-simulation, resumed afterwards, in that order
    EXPECT_EQ(world.count("nav_poll Own off"), 1);
    EXPECT_EQ(world.count("nav_poll Own on"), 1);
    // the recorded inputs of ticks 3 and 4 were replayed (key 0 held)
    EXPECT_EQ(world.count("nav_input Own keys=1"), 2);
    // own ball: restored to 0.5 m at rest, then two accelerating steps
    // (v = 1 then 2 m/s) from the recorded inputs
    const double expected = 0.5 + 1.0 * kDt + 2.0 * kDt;
    EXPECT_NEAR(world.bodies["Own"].position[0], expected, 1e-9);
    EXPECT_NEAR(world.bodies["Own"].linear[0], 2.0f, 1e-6f);
    // the history now holds the re-simulated state of tick 4: a matching
    // snapshot of tick 4 is not a mismatch
    fake_body own_at_4 = world.bodies["Own"];
    const auto later = snapshot_of(4, {ball_body(1, own_at_4), ball_body(2, world.bodies["Remote"])});
    EXPECT_FALSE(engine.on_snapshot(w, later, 4, entity_of,
                                    [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().matched, 1u);
}

TEST(RollbackEngine, FrozenClockSnapsWithoutResimulation) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    world.clock_running = false;   // the retail scripts stopped the physics clock

    fake_body server_remote;
    server_remote.position[0] = 7.0;
    server_remote.linear[0] = 3.0f;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"]), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of,
                                   [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().frozen, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 0u);
    EXPECT_EQ(engine.stats().mismatched, 1u);
    EXPECT_EQ(world.steps, 0);
    EXPECT_EQ(world.count("set_body Remote wake"), 1);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 7.0, 1e-9);
}

TEST(RollbackEngine, LagBeyondLimitSetsBodiesWithoutResimulation) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.max_resim_ticks = 3;
    thresholds.history_ticks = 64;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 8, 100, own_inputs);

    fake_body server_own;
    server_own.position[2] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 8, entity_of,
                                   [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().too_far, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(world.steps, 0);
    EXPECT_NEAR(world.bodies["Own"].position[2], 1.0, 1e-9);
    // the history after the snapshot tick was dropped: tick 8 is unmatched now
    const auto later = snapshot_of(8, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), later, 8, entity_of,
                                    [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().unmatched, 1u);
}

TEST(RollbackEngine, HistoryIsBounded) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.history_ticks = 5;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 20, 100, own_inputs);
    EXPECT_EQ(engine.history_size(), 5u);
    const auto old = snapshot_of(10, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), old, 20, entity_of,
                                    [&](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().unmatched, 1u);
}
