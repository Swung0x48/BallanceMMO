// Unit tests for the client-side correction tolerances (correction.hpp): the
// pair must sit above the measured moving-prediction noise of the own and
// peer balls, must still breach on a real divergence, must leave legal
// mechanism motion alone, and must keep the pair's 0.1 s ratio that bounds
// how long a genuine divergence can hide.  The rollback engine's own pairs
// (rollback.hpp) are covered at the bottom: the mechanism pair is judged
// separately from the ball pair, and Option A leaves mechanism rows untracked.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include <session/correction.hpp>
#include <session/rollback.hpp>

namespace {
    using bmmo::session::ball_pose;
    using bmmo::session::body_corrector;
    using bmmo::session::correction_step;
    using bmmo::session::correction_thresholds;
    using bmmo::session::rollback_thresholds;

    // Measured maximum legal mechanism speed: 0.81 m in one 66 Hz tick
    // (build/online-symptoms-20260909/mech-jitter).
    constexpr double kMechanismMetresPerTick = 0.81;
    constexpr double kTick = 1.0 / 66.0;

    ball_pose pose_at(uint32_t tick, double x, double y, double z, float vx, float vy, float vz) {
        ball_pose pose;
        pose.tick = tick;
        pose.position[0] = x;
        pose.position[1] = y;
        pose.position[2] = z;
        pose.linear[0] = vx;
        pose.linear[1] = vy;
        pose.linear[2] = vz;
        return pose;
    }

    // One comparison at `tick` of a local record against an authoritative pose
    // displaced by `dp` metres on x and `dv` m/s on x.  A fresh corrector per
    // sample, so an earlier blend never leaves compare() skipping the next one.
    // The local x is 0.0, so an authoritative pose at exactly `dp` reproduces
    // the threshold exactly (1.0 + 0.05 is not 0.05 in double): the strictness
    // of the ignore test is observable instead of hidden by rounding.
    correction_step::kind compare_with_error(uint32_t tick, double dp, double dv,
                                             const correction_thresholds& thresholds = {}) {
        body_corrector corrector(thresholds);
        const ball_pose local = pose_at(tick, 0.0, 2.0, 3.0, 0.0f, 0.0f, 0.0f);
        corrector.record(local);
        ball_pose authoritative = local;
        authoritative.position[0] += dp;
        authoritative.linear[0] = static_cast<float>(dv);
        return corrector.compare(authoritative).action;
    }
}

// The measured (position error, velocity error) pairs of the 2026-09-09
// retest, taken from the client-1 journals: the own and peer balls move with a
// few millimetres and a fraction of a metre per second of prediction noise, so
// every one of these samples must be left alone.
TEST(CorrectionThresholds, MeasuredBallNoiseStaysInTolerance) {
    struct sample {
        double position;
        double velocity;
        const char* where;
    };
    const sample samples[] = {
        {0.0090, 0.0840, "P1-evidence section 8 median"},
        {0.0087, 0.2823, "L8 own ball median"},
        {0.0205, 0.3308, "L8 own ball p95"},
        {0.0211, 0.0234, "L8 peer ball p95"},
        {0.0208, 0.3884, "L11 traceoff own ball median"},
        {0.0135, 0.0513, "L11 reloaded own ball p95"},
        {0.0183, 0.2955, "L11 reloaded peer ball p95"},
    };
    uint32_t tick = 1000;
    for (const sample& s: samples) {
        body_corrector corrector;
        const ball_pose local = pose_at(tick, 1.0, 2.0, 3.0, 0.0f, 0.0f, 0.0f);
        corrector.record(local);
        ball_pose authoritative = local;
        authoritative.position[0] += s.position;
        authoritative.linear[0] = static_cast<float>(s.velocity);
        EXPECT_EQ(corrector.compare(authoritative).action, correction_step::kind::none) << s.where;
        EXPECT_EQ(corrector.stats().ignored, 1u) << s.where;
        EXPECT_EQ(corrector.stats().blended, 0u) << s.where;
        EXPECT_EQ(corrector.stats().hard, 0u) << s.where;
        ++tick;
    }
}

// The pair covers the position noise at its 95th percentile, not the velocity
// noise of every phase: the worst own-ball velocity p95 (0.89 m/s, the ball
// riding the mispredicted rope) is still a blend.  That residual is the
// mechanism divergence, and widening the ball pair to hide it would also hide
// a real ball divergence of the same size.  A 0.2 m/s velocity-only error is
// above the pre-widening 0.01 m / 0.05 m/s pair and inside the current one, so
// it has to be ignored: that is the widening this case pins.
TEST(CorrectionThresholds, VelocityNoiseAboveTheBallPairStillBlends) {
    EXPECT_EQ(compare_with_error(2000, 0.0, 0.2), correction_step::kind::none);
    EXPECT_EQ(compare_with_error(2001, 0.0338, 0.8869), correction_step::kind::blend);
}

// The same 2026-09-09 journals also hold samples whose position error is
// *inside* the 0.05 m pair while the velocity error is above 0.5 m/s: the own
// ball with its prediction lagging a rope it is riding (396 of the 622
// in-position L11 own-ball samples, wp5_coverage.py).  The journals record the
// error magnitudes, so the position error is reproduced on x here; these must
// blend rather than be ignored, or a real velocity divergence of the same size
// would be hidden.  Widening ignore_velocity to 1.0 m/s swallows both and turns
// this case red.
TEST(CorrectionThresholds, MeasuredVelocityOnlyBreachStillBlends) {
    struct sample {
        double position;
        double velocity;
        const char* where;
    };
    const sample samples[] = {
        {0.0242, 0.6359, "tick 6802 (local 6805) err=0.024209 dv=0.635877"},
        {0.0338, 0.8869, "tick 6012 (local 6025) err=0.033826 dv=0.886914"},
    };
    uint32_t tick = 2100;
    for (const sample& s: samples) {
        EXPECT_EQ(compare_with_error(tick, s.position, s.velocity), correction_step::kind::blend)
            << s.where;
        ++tick;
    }
}

// The ignore test is strict, so a divergence exactly at the pair breaches on
// either component and gets a blend (below the 1 m hard position), while one
// just inside it -- 20 mm or 0.2 m/s, both above the pre-widening
// 0.01 m / 0.05 m/s pair -- is left alone.
TEST(CorrectionThresholds, FiftyMillimetreHalfMetrePerSecondDivergenceIsABreach) {
    EXPECT_EQ(compare_with_error(2998, 0.02, 0.0), correction_step::kind::none);
    EXPECT_EQ(compare_with_error(2999, 0.0, 0.2), correction_step::kind::none);
    EXPECT_EQ(compare_with_error(3000, 0.05, 0.0), correction_step::kind::blend);
    EXPECT_EQ(compare_with_error(3001, 0.0, 0.5), correction_step::kind::blend);
    EXPECT_EQ(compare_with_error(3002, 0.05, 0.5), correction_step::kind::blend);
}

// A mechanism pose judged by the BALL pair (correction_thresholds), through
// body_corrector.  A snapshot is compared with the record for its own tick, so
// a mechanism's legal speed creates no error by itself: only the sub-tick
// phase difference between the two sides does, and that stays inside the pair.
// A full tick of divergence at the measured maximum legal speed is a blend,
// not a hard set.  The rollback engine's own mechanism pair
// (rollback_thresholds.mechanism_*) is exercised in
// MechanismRowsUseTheMechanismPair below.
TEST(CorrectionThresholds, LegalMechanismSpeedStaysInsideTheBallPair) {
    const double speed = kMechanismMetresPerTick / kTick;
    body_corrector corrector;
    const ball_pose local = pose_at(4000, 1.0, 2.0, 3.0,
                                    static_cast<float>(speed), 0.0f, 0.0f);
    corrector.record(local);
    EXPECT_EQ(corrector.compare(local).action, correction_step::kind::none);

    // 5% of one tick of phase: the sub-tick pose noise the pair is sized to
    // tolerate.
    ball_pose phase = local;
    phase.position[0] += 0.05 * kMechanismMetresPerTick;
    EXPECT_EQ(corrector.compare(phase).action, correction_step::kind::none);

    // One whole tick at that speed is a genuine divergence.
    ball_pose tick_behind = local;
    tick_behind.position[0] += kMechanismMetresPerTick;
    EXPECT_EQ(corrector.compare(tick_behind).action, correction_step::kind::blend);
}

// The pair keeps the 0.1 s ratio -- "an error equivalent to a tenth of a
// second of motion" -- so a divergence closing at a fifth of the velocity
// tolerance reaches the position tolerance inside 0.5 s.  The ratio itself is
// a documented constant (correction.hpp:44-47), pinned here; the guards below
// drive the production comparator on both sides of it and show it really uses
// the pair it was configured with.
TEST(CorrectionThresholds, ThresholdPairKeepsThePointOneSecondProperty) {
    const correction_thresholds ball;
    EXPECT_DOUBLE_EQ(ball.ignore_position / ball.ignore_velocity, 0.1);

    // 0.2 m/s is the ratio's own speed over 0.5 s and is still ignored; the
    // 0.5 m/s velocity tolerance itself breaches, because the ignore test is
    // strict.
    EXPECT_EQ(compare_with_error(5000, 0.0, 0.2), correction_step::kind::none);
    EXPECT_EQ(compare_with_error(5001, 0.0, 0.5), correction_step::kind::blend);

    // A 20 mm / 0.2 m/s error the default pair ignores breaches the
    // pre-widening 0.001 m / 0.01 m/s pair: compare() consults the thresholds
    // it was built with, so the widening is what keeps this sample quiet.
    correction_thresholds tight;
    tight.ignore_position = 0.001;
    tight.ignore_velocity = 0.01;
    EXPECT_EQ(compare_with_error(5002, 0.02, 0.2, tight), correction_step::kind::blend);
    EXPECT_EQ(compare_with_error(5003, 0.02, 0.2), correction_step::kind::none);
}

namespace {
    using bmmo::session::body_kind;
    using bmmo::session::body_state;
    using bmmo::session::input_frame;
    using bmmo::session::rollback_engine;
    using bmmo::session::rollback_tracked;
    using bmmo::session::rollback_world;

    struct fake_body {
        double position[3] = {};
        float linear[3] = {};
        bool simulated = true;
    };

    struct fake_world {
        std::map<std::string, fake_body> bodies;
        std::vector<std::string> calls;
        int steps = 0;

        rollback_world adapter() {
            rollback_world w;
            w.get_body = [this](const std::string& entity, bmmo_physics_body_state& out) {
                const auto it = bodies.find(entity);
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
            w.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            w.set_nav = [](const std::string&, const bmmo_physics_nav_state&) { return true; };
            w.nav_input = [](const std::string&, const input_frame&) { return true; };
            w.nav_poll = [](const std::string&, bool) { return true; };
            w.step = [this]() { calls.push_back("step"); ++steps; return true; };
            w.simulating = []() { return true; };
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

    // A mechanism row this client does not share a world with: another sector's
    // instance of a name that also exists here.
    body_state far_mechanism(uint32_t owner) {
        body_state out;
        out.kind = body_kind::Mechanism;
        out.owner = owner;
        out.name = "P_Modul_30_Wippe";
        out.position[0] = 332.0;
        out.rotation[3] = 1.0;
        out.flags = bmmo::session::BODY_FLAG_SIMULATED;
        return out;
    }

    bmmo::session_snapshot_msg snapshot_of(uint32_t tick, const std::vector<body_state>& bodies) {
        bmmo::session_snapshot_msg msg;
        msg.tick = tick;
        msg.full = 1;
        msg.bodies = bodies;
        return msg;
    }

    // The client's Option A mapping: only the own ball is tracked, a mechanism
    // row maps to no entity at all.
    std::string option_a_entity_of(const body_state& body) {
        if (body.kind != body_kind::Ball) return {};
        return body.owner == 1 ? "Own" : "";
    }

    bool no_input(const std::string&, uint32_t, input_frame&) { return false; }

    // Records ticks 1..4 for the own ball only, the tracked set Option A leaves
    // behind, and clears the call log so the snapshot can be observed alone.
    // The body has to exist before the first record: capture() skips a tracked
    // name the world does not know, and an empty record makes every later
    // snapshot of that entity unmatched.
    void record_own_ball(fake_world& world, rollback_engine& engine) {
        fake_body& own = world.bodies["Own"];
        own.position[0] = 1.0;
        own.position[1] = 2.0;
        own.position[2] = 3.0;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= 4; ++tick) {
            w.step();
            engine.record(w, tick, tracked, {});
        }
        world.calls.clear();
        world.steps = 0;
    }
}

// Option A: mechanisms are server-authoritative, so a mechanism row is not part
// of the tracked set and on_snapshot must neither compare it nor write it.  The
// row below is 332 m from the local body - the case the old identity guard
// existed for - and the snapshot still matches on the own ball alone.
TEST(OptionA, MechanismRowOutsideTheTrackedSetIsNeverApplied) {
    fake_world world;
    rollback_engine engine;
    record_own_ball(world, engine);

    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"]), far_mechanism(7)});
    const bool rolled = engine.on_snapshot(world.adapter(), snapshot, 4, option_a_entity_of, no_input);

    EXPECT_FALSE(rolled);
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(engine.stats().rollbacks, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
}

// A mechanism row must not smuggle a far body into a ball rollback either: the
// own ball breaches its 0.05 m pair, the session rolls back and re-simulates,
// and the mechanism is never written.
TEST(OptionA, MechanismRowDoesNotAffectABallRollback) {
    fake_world world;
    rollback_engine engine;
    record_own_ball(world, engine);

    fake_body own_at_2 = world.bodies["Own"];
    own_at_2.position[0] += 0.06;   // past the 0.05 m position pair
    const auto snapshot = snapshot_of(2, {ball_body(1, own_at_2), far_mechanism(7)});
    const bool rolled = engine.on_snapshot(world.adapter(), snapshot, 4, option_a_entity_of, no_input);

    EXPECT_TRUE(rolled);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);   // ticks 3 and 4
    EXPECT_EQ(world.steps, 2);
    EXPECT_EQ(world.count("set_body Own"), 1);
    EXPECT_EQ(world.count("set_body P_Modul_30_Wippe"), 0);
}

namespace {
    body_state mechanism_body(const fake_body& body) {
        body_state out;
        out.kind = body_kind::Mechanism;
        out.owner = 0;
        out.name = "P_Modul_30_Wippe";
        for (int k = 0; k < 3; ++k) {
            out.position[k] = body.position[k];
            out.linear[k] = body.linear[k];
        }
        out.rotation[3] = 1.0;
        out.flags = body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0;
        return out;
    }

    // Option B mapping: the own ball plus every shared mechanism this client
    // simulates, which is the only path that reaches tolerance_for() for a
    // mechanism (it is private; on_snapshot is the production caller).
    std::string shared_entity_of(const body_state& body) {
        if (body.kind == body_kind::Mechanism) return body.name;
        return body.owner == 1 ? "Own" : "";
    }

    void record_own_ball_and_mechanism(fake_world& world, rollback_engine& engine) {
        fake_body& own = world.bodies["Own"];
        own.position[0] = 1.0;
        own.position[1] = 2.0;
        own.position[2] = 3.0;
        fake_body& mechanism = world.bodies["P_Modul_30_Wippe"];
        mechanism.position[0] = 10.0;
        mechanism.position[1] = 20.0;
        mechanism.position[2] = 30.0;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.mechanisms = {"P_Modul_30_Wippe"};
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= 4; ++tick) {
            w.step();
            engine.record(w, tick, tracked, {});
        }
        world.calls.clear();
        world.steps = 0;
    }
}

// A tracked mechanism row is judged by rollback_thresholds.mechanism_*, the
// pair the engine keeps for shared script-driven bodies, and by nothing else:
// 20 mm / 0.2 m/s is inside it, 60 mm or 0.6 m/s breaches it.  Narrowing
// mechanism_position or mechanism_velocity below those errors, or making
// tolerance_for() return the ball pair, turns the corresponding line red.
TEST(RollbackThresholds, MechanismRowsUseTheMechanismPair) {
    const auto rolls = [](double dp, double dv) {
        fake_world world;
        rollback_engine engine;
        record_own_ball_and_mechanism(world, engine);

        fake_body mechanism_at_2 = world.bodies["P_Modul_30_Wippe"];
        mechanism_at_2.position[0] += dp;
        mechanism_at_2.linear[0] = static_cast<float>(dv);
        const auto snapshot =
            snapshot_of(2, {ball_body(1, world.bodies["Own"]), mechanism_body(mechanism_at_2)});
        return engine.on_snapshot(world.adapter(), snapshot, 4, shared_entity_of, no_input);
    };

    EXPECT_FALSE(rolls(0.02, 0.2));
    EXPECT_TRUE(rolls(0.06, 0.0));
    EXPECT_TRUE(rolls(0.0, 0.6));
}

// The mechanism pair is not the ball pair: with the ball pair tightened to
// 1 mm / 0.01 m/s, the same 20 mm / 0.2 m/s error rolls the session back when
// it is the own ball and is ignored when it is the mechanism row.  A
// tolerance_for() that ignores body_kind cannot satisfy both halves.
TEST(RollbackThresholds, MechanismRowIsJudgedByItsOwnPairNotTheBallPair) {
    rollback_thresholds tight;
    tight.position = 0.001;
    tight.velocity = 0.01;
    tight.mechanism_position = 0.05;
    tight.mechanism_velocity = 0.5;

    {
        fake_world world;
        rollback_engine engine(tight);
        record_own_ball_and_mechanism(world, engine);

        fake_body own_at_2 = world.bodies["Own"];
        own_at_2.position[0] += 0.02;
        own_at_2.linear[0] = 0.2f;
        const auto snapshot = snapshot_of(
            2, {ball_body(1, own_at_2), mechanism_body(world.bodies["P_Modul_30_Wippe"])});

        EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, shared_entity_of, no_input));
        EXPECT_EQ(engine.stats().rollbacks, 1u);
        EXPECT_EQ(engine.stats().matched, 0u);
    }
    {
        fake_world world;
        rollback_engine engine(tight);
        record_own_ball_and_mechanism(world, engine);

        fake_body mechanism_at_2 = world.bodies["P_Modul_30_Wippe"];
        mechanism_at_2.position[0] += 0.02;
        mechanism_at_2.linear[0] = 0.2f;
        const auto snapshot = snapshot_of(
            2, {ball_body(1, world.bodies["Own"]), mechanism_body(mechanism_at_2)});

        EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 4, shared_entity_of, no_input));
        EXPECT_EQ(engine.stats().matched, 1u);
        EXPECT_EQ(engine.stats().rollbacks, 0u);
    }
}
