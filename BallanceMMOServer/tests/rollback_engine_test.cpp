// Unit tests for the client-side rollback engine (design 9.6) over a fake
// world: bodies move by their velocity each step, a navigated ball gains
// +1 m/s on x per step while key 0 is held, and every adapter call is
// recorded so the tests can check what the engine did to the world.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <session/correction.hpp>
#include <session/rollback.hpp>

namespace {
    using bmmo::session::ball_pose;
    using bmmo::session::body_corrector;
    using bmmo::session::body_kind;
    using bmmo::session::body_state;
    using bmmo::session::correction_step;
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

TEST(RollbackEngine, RelayedRemoteInputsReplacePredictionsAndConverge) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    // Live frames predicted that the remote stayed idle. Its real press,
    // release, and second press arrive with the delayed snapshot.
    run_ticks(world, engine, 5, 3, own_inputs);
    fake_body server_remote;
    server_remote.position[0] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    int own_queries = 0;
    auto input_at = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        out = {};
        if (entity == "Own") {
            ++own_queries;
            return true;   // this idle fallback must not replace recorded own inputs
        }
        if (entity != "Remote") return false;
        out.keys = tick == 4 ? 0 : 1;
        return true;
    };
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 5, entity_of, input_at));
    EXPECT_EQ(own_queries, 0);
    EXPECT_NEAR(world.bodies["Own"].linear[0], 3.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Own"].position[0], 6.0 * kDt, 1e-9);
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 1.0 + 4.0 * kDt, 1e-9);

    // The next snapshot confirms an intermediate replayed tick. The old
    // prediction must also have been replaced in history, or this rolls back
    // again even though the authoritative inputs were already available.
    server_remote.linear[0] = 1.0f;
    server_remote.position[0] = 1.0 + 2.0 * kDt;
    const auto later = snapshot_of(4, {ball_body(2, server_remote)});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), later, 5, entity_of, input_at));
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().matched, 1u);

    // If that relay is no longer available on another correction, retain the
    // inputs captured during the first replay instead of reverting to idle.
    server_remote.position[0] = 2.0;
    server_remote.linear[0] = 0.0f;
    const auto revised = snapshot_of(2, {ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), revised, 5, entity_of,
                                   [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 2.0 + 4.0 * kDt, 1e-9);
}

TEST(RollbackEngine, MissingRelayKeepsRecordedRemotePrediction) {
    fake_world world;
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    tracked.remote_entities = {"Remote"};
    input_frame held{};
    held.keys = 1;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        world.pending_keys["Remote"] = held.keys;
        w.step();
        engine.record(w, tick, tracked, {{"Remote", held}});
    }
    fake_body server_remote;
    server_remote.position[0] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of,
                                   [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 1.0 + 3.0 * kDt, 1e-9);
}

// What the session journal records: one correction per decision, the mismatch
// (kind 0) before the completed rollback (kind 1), and an unmatched snapshot
// (kind 7) with no entity.
TEST(RollbackEngine, CorrectionCallbackReportsEveryDecision) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 3, own_inputs);

    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };

    // the server had the own ball 0.5 m further at tick 2
    fake_body server_own;
    server_own.position[0] = 0.5;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of,
                                   [&](const std::string&, uint32_t, input_frame&) { return false; }));
    ASSERT_EQ(corrections.size(), 2u);
    EXPECT_EQ(corrections[0].kind, 0);
    EXPECT_EQ(corrections[0].tick, 2u);
    EXPECT_EQ(corrections[0].local_tick, 4u);
    EXPECT_EQ(corrections[0].entity, "Own");
    EXPECT_NEAR(corrections[0].error_m, 0.5, 1e-9);
    EXPECT_NEAR(corrections[0].velocity_error, 0.0, 1e-9);
    EXPECT_NEAR(corrections[0].local_position[0], 0.0, 1e-9);
    EXPECT_NEAR(corrections[0].server_position[0], 0.5, 1e-9);
    // the rollback carries the same worst offender
    EXPECT_EQ(corrections[1].kind, 1);
    EXPECT_EQ(corrections[1].entity, "Own");
    EXPECT_NEAR(corrections[1].error_m, 0.5, 1e-9);

    const auto missing = snapshot_of(99, {ball_body(1, server_own)});
    EXPECT_FALSE(engine.on_snapshot(w, missing, 4, entity_of,
                                    [&](const std::string&, uint32_t, input_frame&) { return false; }));
    ASSERT_EQ(corrections.size(), 3u);
    EXPECT_EQ(corrections[2].kind, 7);
    EXPECT_EQ(corrections[2].tick, 99u);
    EXPECT_TRUE(corrections[2].entity.empty());
}

// A mismatch can be a velocity one - 0.01 m/s trips well before 1 mm of drift
// does, which is what the onset of a divergence looks like - and then the
// record must name that body, not the one that drifted furthest inside the
// tolerance.  The log lines keep naming the largest position error.
TEST(RollbackEngine, VelocityOnlyMismatchNamesTheOffendingBody) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };

    // the remote ball is where the client had it but moving at 1 m/s, while
    // the own ball is 0.5 mm off: inside the position threshold
    fake_body server_own = world.bodies["Own"];
    server_own.position[0] += 0.0005;
    fake_body server_remote = world.bodies["Remote"];
    server_remote.linear[0] = 1.0f;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of,
                                   [&](const std::string&, uint32_t, input_frame&) { return false; }));
    ASSERT_EQ(corrections.size(), 2u);
    for (const auto& c: corrections) {
        EXPECT_EQ(c.entity, "Remote");
        EXPECT_NEAR(c.velocity_error, 1.0, 1e-9);
        EXPECT_NEAR(c.error_m, 0.0, 1e-9);
        EXPECT_NEAR(c.local_position[0], 5.0, 1e-9);
        EXPECT_NEAR(c.server_position[0], 5.0, 1e-9);
    }
    EXPECT_EQ(engine.stats().last_mismatch, "Own");
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

// This adapter cannot recreate the ball destroyed by a trafo. Reject the old
// window entirely, then resume correction once a new lifetime has history.
TEST(RollbackEngine, TrafoInvalidatesHistoryInsteadOfReplayingDestroyedBall) {
    fake_world world;
    world.bodies["OwnOld"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["OwnOld"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    auto w = world.adapter();

    rollback_tracked before;                 // ticks 1..3: the ball we started with
    before.own_entity = "OwnOld";
    before.own_polls = true;
    before.remote_entities = {"Remote"};
    rollback_tracked after = before;          // ticks 4..6: the ball the trafo made
    after.own_entity = "OwnNew";

    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            world.bodies.erase("OwnOld");
            world.navs.erase("OwnOld");
            world.bodies["OwnNew"].position[0] = 10.0;
            world.navs["OwnNew"] = {};
        }
        const std::string own = tick >= 4 ? "OwnNew" : "OwnOld";
        input_frame frame{};
        frame.keys = 1;
        world.pending_keys[own] = frame.keys;
        w.step();
        engine.record(w, tick, tick >= 4 ? after : before, {{own, frame}, {"Remote", input_frame{}}});
    }
    world.calls.clear();
    world.steps = 0;

    // The server runs behind: its snapshot of tick 2 still carries the ball
    // the trafo replaced, and it disagrees with what we recorded.
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    fake_body server_own;
    server_own.position[0] = -99.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    // player 1's ball is the entity we have now, which is what the mod maps to
    auto entity_now = [](const body_state& body) -> std::string {
        if (body.kind != body_kind::Ball) return {};
        return body.owner == 1 ? "OwnNew" : body.owner == 2 ? "Remote" : "";
    };

    EXPECT_FALSE(engine.on_snapshot(w, snapshot, 6, entity_now,
                                   [&](const std::string& entity, uint32_t, input_frame& out) {
                                       out = {};
                                       return entity != "Remote";   // stale own fallback, never preferred
                                   }));
    EXPECT_EQ(engine.history_size(), 3u);
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.count("set_nav"), 0);
    EXPECT_EQ(world.count("nav_input"), 0);
    EXPECT_EQ(world.count("nav_poll"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_FALSE(world.bodies.contains("OwnOld"));
    EXPECT_GT(world.bodies["OwnNew"].position[0], 0.0);
    const auto current = snapshot_of(5, {ball_body(1, server_own)});
    EXPECT_TRUE(engine.on_snapshot(w, current, 6, entity_now,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(world.count("nav_input OwnNew keys=1"), 1);
    EXPECT_EQ(world.count("nav_input OwnOld"), 0);
    EXPECT_EQ(world.count("nav_poll OwnNew off"), 1);
    EXPECT_EQ(world.count("nav_poll OwnNew on"), 1);
}

TEST(RollbackEngine, LateRemoteCreationRejectsWholeSnapshotBeforeNextRecord) {
    fake_world world;
    world.bodies["Own"].position[0] = 3.0;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    auto w = world.adapter();
    engine.record(w, 218, tracked, {});
    // Like the retail queue: creation happens after record(218). Even if a
    // lifecycle hook were missed, no partial restore may advance this body.
    world.bodies["Remote"].position[0] = 900.0;
    world.bodies["Remote"].linear[0] = 1171.0f;
    fake_body server_own, server_remote;
    server_remote.position[0] = 37.0;
    const auto snapshot = snapshot_of(218, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_FALSE(engine.on_snapshot(w, snapshot, 234, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.history_size(), 0u);
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.count("set_nav"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_DOUBLE_EQ(world.bodies["Own"].position[0], 3.0);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], 900.0);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], 1171.0f);
}

TEST(RollbackEngine, RecordedBodyCreationOrDeletionStartsANewHistory) {
    fake_world world;
    world.bodies["Own"];
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    auto w = world.adapter();
    engine.record(w, 1, tracked, {});
    world.bodies["Remote"].position[0] = 900.0;
    tracked.remote_entities = {"Remote"};
    engine.record(w, 2, tracked, {});
    EXPECT_EQ(engine.history_size(), 1u);
    fake_body server_own;
    server_own.position[0] = 1.0;
    // The delayed server snapshot does not even list the new remote yet.
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(1, {ball_body(1, server_own)}), 2, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    world.bodies.erase("Remote");
    tracked.remote_entities.clear();
    engine.record(w, 3, tracked, {});
    EXPECT_EQ(engine.history_size(), 1u);
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(2, {ball_body(1, server_own)}), 3, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
}

TEST(RollbackEngine, DeletedBodyAfterLastRecordPreventsAnyPartialRestore) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    world.bodies.erase("Remote");
    fake_body server_own;
    server_own.position[0] = 1.0;
    // Even when only Own appears in the snapshot, the history restoration
    // would otherwise try to write the deleted Remote after changing Own.
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 4, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.history_size(), 0u);
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.count("set_nav"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_FALSE(world.bodies.contains("Remote"));
}

TEST(RollbackEngine, SameNameRespawnInvalidatesHistoryAndPreservesSessionStats) {
    for (const std::string entity: {"Own", "Remote"}) {
        SCOPED_TRACE(entity);
        fake_world world;
        world.bodies["Own"];
        world.bodies["Remote"];
        rollback_engine engine;
        auto w = world.adapter();
        std::map<uint32_t, input_frame> own_inputs;
        run_ticks(world, engine, 4, 100, own_inputs);
        fake_body server_own;
        server_own.position[0] = 1.0;
        const auto old = snapshot_of(2, {ball_body(1, server_own)});
        ASSERT_TRUE(engine.on_snapshot(w, old, 4, entity_of,
            [](const std::string&, uint32_t, input_frame&) { return false; }));
        const auto previous = engine.stats();
        // Own hooks run before PreSimulate; remote hooks run after this
        // frame's record. Both need an explicit boundary for reused names.
        engine.invalidate_history();
        world.bodies.erase(entity);
        engine.invalidate_history();
        world.bodies[entity].position[0] = 900.0;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.remote_entities = {"Remote"};
        engine.record(w, 5, tracked, {});
        EXPECT_EQ(engine.stats().snapshots, previous.snapshots);
        EXPECT_EQ(engine.stats().mismatched, previous.mismatched);
        EXPECT_EQ(engine.stats().rollbacks, previous.rollbacks);
        EXPECT_EQ(engine.stats().resim_ticks, previous.resim_ticks);
        EXPECT_EQ(engine.stats().max_error, previous.max_error);
        EXPECT_EQ(engine.stats().last_mismatch, previous.last_mismatch);
        world.calls.clear();
        world.steps = 0;
        EXPECT_FALSE(engine.on_snapshot(w, old, 5, entity_of,
            [](const std::string&, uint32_t, input_frame&) { return false; }));
        EXPECT_EQ(world.count("set_body"), 0);
        EXPECT_EQ(world.count("set_nav"), 0);
        EXPECT_EQ(world.steps, 0);
        EXPECT_DOUBLE_EQ(world.bodies[entity].position[0], 900.0);
    }
}

TEST(RollbackEngine, FirstOwnPhysicalizeRecordsOnlyTheNewLifetime) {
    fake_world world;
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    engine.record(w, 203, tracked, {});
    // BML OnPhysicalize invalidates before the body exists; PreSimulate then
    // creates/moves it, and the normal post-physics record starts the history.
    engine.invalidate_history();
    tracked.own_entity = "Own";
    world.bodies["Own"].linear[0] = 3.0f;
    w.step();
    engine.record(w, 204, tracked, {});
    world.calls.clear();
    world.steps = 0;
    fake_body server_own;
    const auto old = snapshot_of(203, {ball_body(1, server_own)});
    EXPECT_FALSE(engine.on_snapshot(w, old, 204, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_EQ(engine.history_size(), 1u);
    const auto current = snapshot_of(204, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(w, current, 204, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().matched, 1u);
}

TEST(RollbackEngine, ResyncDiscardsBothOldNumberingAndPreOverwriteRecords) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.remote_entities = {"Remote"};
    engine.invalidate_history();    // assignment reuses an old tick number
    world.bodies["Own"].position[0] = 900.0;
    engine.record(w, 2, tracked, {}); // still waiting for the full snapshot
    engine.invalidate_history();    // full snapshot overwrites the world
    world.bodies["Own"].position[0] = 5.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(w, snapshot, 2, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_DOUBLE_EQ(world.bodies["Own"].position[0], 5.0);
    engine.record(w, 3, tracked, {});
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(3, {ball_body(1, world.bodies["Own"])}), 3, entity_of,
        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().unmatched, 1u);
}

// Only a body that breached its own tolerance is snapped to the server pose.
// An in-tolerance body is restored from its own record for T, so neither the
// server's sub-tolerance pose nor its wake flag reaches the world.
TEST(RollbackEngine, RestoreWritesOnlyTheBodyThatBreached) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.bodies["Remote"].linear[0] = 2.0f;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    const double step = 2.0 * kDt;
    const double remote_at_2 = 5.0 + 2.0 * step;

    // the server had the own ball 0.5 m further at tick 2; the remote row is
    // 0.5 mm off, inside the ball tolerance - and the server reports it frozen
    fake_body server_own;
    server_own.position[0] = 0.5;
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] = remote_at_2 + 0.0005;
    server_remote.simulated = false;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of,
                                   [&](const std::string&, uint32_t, input_frame&) { return false; }));
    // the re-simulation still ran over both bodies
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);
    EXPECT_EQ(world.steps, 2);
    // the breaching ball was written from the server pose
    EXPECT_EQ(world.count("set_body Own wake"), 1);
    EXPECT_NEAR(world.bodies["Own"].position[0], 0.5, 1e-9);
    // the remote ball came back from its own record for tick 2 and was
    // re-simulated from there: the server's 0.5 mm and its freeze flag are
    // both absent (a server-pose write would leave 5.0 + 4*step + 0.0005)
    EXPECT_EQ(world.count("set_body Remote wake"), 1);
    EXPECT_TRUE(world.bodies["Remote"].simulated);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 5.0 + 4.0 * step, 1e-9);
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
}

// A shared mechanism row is judged by its own tolerance: 5 mm of pose error
// (5x the ball tolerance, inside the mechanism one) is not a divergence for a
// mechanism, while the same row on a ball still is - and the wider tolerance
// is not unlimited.
TEST(RollbackEngine, MechanismRowsUseTheirOwnWiderTolerance) {
    fake_world world;
    world.bodies["Wippe"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    tracked.mechanisms = {"Wippe"};
    for (uint32_t tick = 1; tick <= 3; ++tick) {
        w.step();
        engine.record(w, tick, tracked, {});
    }
    world.calls.clear();
    world.steps = 0;
    auto mechanism_of = [](const body_state& body) -> std::string {
        return body.kind == body_kind::Mechanism ? "Wippe" : std::string();
    };
    body_state row;
    row.kind = body_kind::Mechanism;
    row.position[0] = 0.005;   // 5 mm
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(2, {row}), 3, mechanism_of,
                                    [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);

    // the same 5 mm on a ball row is a mismatch
    fake_world ball_world;
    ball_world.bodies["Own"];
    rollback_engine ball_engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(ball_world, ball_engine, 3, 100, own_inputs);
    fake_body server_own;
    server_own.position[0] = 0.005;
    EXPECT_TRUE(ball_engine.on_snapshot(ball_world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 3, entity_of,
                                        [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(ball_engine.stats().mismatched, 1u);

    // past the mechanism tolerance the mechanism row is a mismatch too
    body_state beyond = row;
    beyond.position[0] = 0.06;
    EXPECT_TRUE(engine.on_snapshot(w, snapshot_of(2, {beyond}), 3, mechanism_of,
                                   [](const std::string&, uint32_t, input_frame&) { return false; }));
    EXPECT_EQ(engine.stats().mismatched, 1u);
}

// The identity guard: Level 8 has two same-named P_Modul_30_Wippe instances
// 332 m apart in different sectors, and the protocol identifies a mechanism
// only by (name, owner).  A row that far from our own body for the same tick
// names the other instance and must never be applied to ours.
TEST(BodyCorrector, IdentityGuardIsFalseWithoutAComparison) {
    body_corrector corrector;
    EXPECT_FALSE(corrector.identity_mismatch());
    ball_pose row;
    row.tick = 5;
    row.position[0] = 332.09;
    corrector.compare(row);   // no local record for tick 5: nothing to compare
    EXPECT_FALSE(corrector.identity_mismatch());
}

TEST(BodyCorrector, IdentityGuardIgnoresALegalCorrection) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 0.5;    // within the legal mechanism motion
    const auto step = corrector.compare(row);
    EXPECT_EQ(step.action, correction_step::kind::blend);
    EXPECT_FALSE(corrector.identity_mismatch());
}

TEST(BodyCorrector, IdentityGuardSurvivesTheHistoryWipeOfAHardStep) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 332.09;   // the other sector's same-named instance
    const auto step = corrector.compare(row);
    EXPECT_EQ(step.action, correction_step::kind::hard);
    EXPECT_EQ(corrector.history_size(), 0u);   // compare() wiped it
    EXPECT_TRUE(corrector.identity_mismatch());
    corrector.clear();
    EXPECT_FALSE(corrector.identity_mismatch());
}

// The rollback path applies snapshots without compare(), so it asks the
// question directly: is this row far from our record for its own tick?
TEST(BodyCorrector, IdentityGuardComparesARowWithTheRecordedTick) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    local.position[0] = 75.88;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 402.72;   // sector 2's instance
    EXPECT_TRUE(corrector.identity_mismatch(row));
    row.position[0] = 75.9;     // our own instance
    EXPECT_FALSE(corrector.identity_mismatch(row));
    row.tick = 6;               // no record for this tick: cannot tell
    EXPECT_FALSE(corrector.identity_mismatch(row));
}
