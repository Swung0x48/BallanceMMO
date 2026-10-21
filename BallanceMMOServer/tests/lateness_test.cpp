// Unit tests for the server's lateness meter (session/lateness.hpp, late-player
// design 2.1): synthetic uplink series fed the way the runner feeds it - one
// sample per datagram from its newest frame, in arrival order, and one
// evaluation per stepped tick.  The rule must raise exactly once for a
// persistent +300 ms shift or a 2.5 s one and never for a single burst, a
// one-second spike, a 33-frame stall, the A4 loss pattern (10 % in bursts of
// 8) or a blackout - alone or after a spike or a burst - wherever those fall
// relative to a window boundary.
#include <gtest/gtest.h>

#include <session/lateness.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

using namespace bmmo::session;

namespace {
    constexpr double kTickMs = 1000.0 / 66.0;
    constexpr uint32_t kBase = 100;          // the assignment's first tick
    constexpr uint32_t kInputDelay = 16;     // the session's, as at 150 ms each way
    constexpr double kUplinkMs = 150.0;      // base one-way trip: slack about 6.1 ticks
    constexpr double kJitterTicks = 3.0;     // A0j: +-3 ticks on every datagram

    // Deterministic jitter in [-1, 1] per tick.
    double unit_noise(uint32_t t, uint32_t salt) {
        uint32_t x = t * 2654435761u ^ (salt * 40503u + 0x9E3779B9u);
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return static_cast<double>(x) / 4294967295.0 * 2.0 - 1.0;
    }

    double deadline_ms(uint32_t t) { return (t + kInputDelay) * kTickMs; }

    struct link_model {
        // When the client sends the datagram whose newest frame is t (ms).
        std::function<double(uint32_t)> send_ms = [](uint32_t t) { return t * kTickMs; };
        // Extra one-way delay on top of the base trip and the jitter (ms).
        std::function<double(uint32_t)> extra_ms = [](uint32_t) { return 0.0; };
        std::function<bool(uint32_t)> lost = [](uint32_t) { return false; };
        // Ticks the client's lead was raised by for frame t (an acknowledged
        // adjustment): its frames leave that much earlier.
        std::function<int(uint32_t)> lead_ticks = [](uint32_t) { return 0; };
        double jitter_ticks = kJitterTicks;
        uint32_t salt = 1;
    };

    struct decision_at {
        uint32_t tick;
        lateness_decision decision;
    };

    struct datagram {
        double arrival_ms;
        uint32_t newest;
    };

    // Runs ticks [from, to): the client sends each frame's datagram per
    // `link` (in frame order: a frame whose time came while an older one was
    // still unsent - a stall, or a raised lead - goes out right after it), and
    // the server steps each tick at its deadline (the latest it can), feeding
    // every datagram that arrived by then in arrival order and then
    // evaluating the meter.  The link is read as the frames go out, so an
    // `on_decision` that acknowledges and changes the client's lead acts on
    // the frames sent after it.
    std::vector<decision_at> run(lateness_meter& meter, uint32_t from, uint32_t to, const link_model& link,
                                 const std::function<void(uint32_t, const lateness_decision&)>& on_decision = {}) {
        std::vector<datagram> wire;   // in flight
        std::vector<decision_at> out;
        uint32_t frame = from;
        double last_send = -1e300;
        for (uint32_t n = from; n < to; ++n) {
            const double now = deadline_ms(n);
            while (frame < to) {
                const double send = std::max(last_send, link.send_ms(frame) - link.lead_ticks(frame) * kTickMs);
                if (send > now) break;
                last_send = send;
                if (!link.lost(frame))
                    wire.push_back({send + kUplinkMs + link.extra_ms(frame)
                                    + unit_noise(frame, link.salt) * link.jitter_ticks * kTickMs, frame});
                ++frame;
            }
            std::stable_sort(wire.begin(), wire.end(),
                             [](const datagram& a, const datagram& b) { return a.arrival_ms < b.arrival_ms; });
            size_t delivered = 0;
            while (delivered < wire.size() && wire[delivered].arrival_ms <= now) {
                const auto& d = wire[delivered++];
                meter.sample(d.newest, (deadline_ms(d.newest) - d.arrival_ms) / kTickMs);
            }
            wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(delivered));
            const auto d = meter.on_tick(n);
            if (d.action != lateness_action::none) {
                out.push_back({n, d});
                if (on_decision) on_decision(n, d);
            }
        }
        return out;
    }

    // The client's lead as a list of (from frame, total raise) steps.
    struct lead_steps {
        std::vector<std::pair<uint32_t, int>> steps;
        int at(uint32_t t) const {
            int lead = 0;
            for (const auto& [from, l]: steps) if (t >= from) lead = l;
            return lead;
        }
        int total() const { return steps.empty() ? 0 : steps.back().second; }
    };

    int count(const std::vector<decision_at>& decisions, lateness_action action) {
        return static_cast<int>(std::count_if(decisions.begin(), decisions.end(),
                                              [action](const decision_at& d) { return d.decision.action == action; }));
    }

    // Ticks a meter is given to measure its reference on the steady link.
    // More than 4 * 66: with +-3 ticks of jitter about half the datagrams
    // are overtaken and give no sample.
    constexpr uint32_t kMeasured = 1000;

    // A meter with its reference measured on the steady link, at
    // kBase + kMeasured.
    lateness_meter measured_meter(uint32_t salt = 1, double jitter = kJitterTicks) {
        lateness_meter meter;
        meter.reset(kBase);
        link_model steady;
        steady.salt = salt;
        steady.jitter_ticks = jitter;
        run(meter, kBase, kBase + kMeasured, steady);
        EXPECT_TRUE(meter.reference_measured()) << "salt " << salt;
        return meter;
    }

    constexpr uint32_t kShiftStart = kBase + kMeasured + 500;
    constexpr double kShiftMs = 300.0;
}

TEST(LatenessMeter, QuantileAndMedian) {
    std::vector<double> v;
    for (int i = 0; i < 66; ++i) v.push_back(65 - i);
    EXPECT_EQ(lateness_quantile(v, 0.10), 6.0);    // floor(6.6): the seventh smallest
    EXPECT_EQ(lateness_quantile(v, 0.05), 3.0);
    EXPECT_EQ(lateness_quantile(v, 1.0), 65.0);
    EXPECT_EQ(lateness_quantile({}, 0.5), 0.0);
    EXPECT_EQ(lateness_median({3.0, 1.0, 2.0}), 2.0);
    EXPECT_EQ(lateness_median({4.0, 1.0, 2.0, 3.0}), 2.5);
}

TEST(LatenessMeter, SampleIsTheNewestFrameAndDuplicatesGiveNone) {
    lateness_meter meter;
    meter.reset(kBase);
    EXPECT_FALSE(meter.sample(kBase - 1, 5.0));   // the old numbering
    EXPECT_TRUE(meter.sample(kBase + 3, 5.0));
    EXPECT_FALSE(meter.sample(kBase + 3, 5.0));   // a duplicate
    EXPECT_FALSE(meter.sample(kBase + 2, 5.0));   // overtaken
    EXPECT_TRUE(meter.sample(kBase + 4, 4.0));
    EXPECT_EQ(meter.samples_total(), 2u);
    EXPECT_EQ(meter.last_slack(), 4.0);
}

TEST(LatenessMeter, ReferenceFromFourQualifyingWindows) {
    lateness_meter meter;
    meter.reset(kBase);
    EXPECT_EQ(meter.state(), lateness_state::measuring);
    // 4 windows of 66 samples: 6.0 + i/10 within each window.
    for (uint32_t i = 0; i < 4 * 66; ++i) {
        EXPECT_FALSE(meter.has_reference());
        meter.sample(kBase + i, 6.0 + (i % 66) / 10.0);
    }
    ASSERT_TRUE(meter.has_reference());
    EXPECT_TRUE(meter.reference_measured());
    EXPECT_EQ(meter.windows(), 4);
    EXPECT_EQ(meter.qualifying_windows(), 4);
    // q10 of 264 samples: index 26 = the seventh of each window's ladder.
    EXPECT_NEAR(meter.r10(), 6.6, 1e-9);
    EXPECT_NEAR(meter.r01(), 6.0, 1e-9);
    meter.on_tick(kBase + 300);
    EXPECT_EQ(meter.state(), lateness_state::watching);
}

TEST(LatenessMeter, SteadyJitteredLinkNeverDecides) {
    // A0j: +-3 ticks on every datagram for a whole minute.
    lateness_meter meter;
    meter.reset(kBase);
    link_model link;
    const auto decisions = run(meter, kBase, kBase + 4000, link);
    EXPECT_TRUE(decisions.empty());
    ASSERT_TRUE(meter.reference_measured());
    EXPECT_LT(meter.r01(), meter.r10());
    // Per datagram the slack is 16 - 9.9 +- 3 ticks, q10 about 3.7.  The
    // samples run higher: a datagram the jitter delayed is often overtaken
    // and gives none, so the late tail is under-sampled.
    const double centre = kInputDelay - kUplinkMs / kTickMs;
    EXPECT_GT(meter.r10(), centre - 0.8 * kJitterTicks - 0.5);
    EXPECT_LT(meter.r10(), centre);
    EXPECT_GT(meter.r01(), centre - kJitterTicks - 0.1);
    EXPECT_EQ(meter.misses_total(), 0u);
}

TEST(LatenessMeter, PersistentShiftRaisesExactlyOnce) {
    // A5u: +300 ms on the uplink from kShiftStart on, for good.  Nothing ever
    // acknowledges the decision (observe only), so it is said once.
    for (uint32_t salt = 1; salt <= 5; ++salt) {
        lateness_meter meter = measured_meter(salt);
        link_model link;
        link.salt = salt;
        link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? kShiftMs : 0.0; };
        const auto decisions = run(meter, kBase + kMeasured, kBase + 4000, link);
        ASSERT_EQ(decisions.size(), 1u) << "salt " << salt;
        const auto& d = decisions[0];
        EXPECT_EQ(d.decision.action, lateness_action::raise);
        // K in [ceil(up_ms / 15), +4] (design 5.2, A5u).
        EXPECT_GE(d.decision.k, static_cast<int>(std::ceil(kShiftMs / 15.0)));
        EXPECT_LE(d.decision.k, static_cast<int>(std::ceil(kShiftMs / 15.0)) + 4);
        EXPECT_EQ(d.decision.streak, lateness_meter::kRaiseTicks);
        // The first late datagram reaches the server about 20 ticks after
        // the shift; a majority needs 34 late samples (more ticks than that
        // when the jitter reorders datagrams and the overtaken ones give
        // none), then 132 ticks of it: 2.5 s or a little more from the first
        // late arrival, never less than 131 + 33 ticks.
        const uint32_t first_late = kShiftStart + static_cast<uint32_t>(kShiftMs / kTickMs);
        EXPECT_GE(d.tick, first_late + 33 + 131 - 4);
        EXPECT_LE(d.tick, first_late + 2 * 34 + 132 + 8);
        EXPECT_EQ(meter.state(), lateness_state::pending);
        EXPECT_EQ(meter.pending_k(), d.decision.k);
    }
}

TEST(LatenessMeter, ShiftEndingBeforeTheRuleHoldsDoesNotRaise) {
    // 2 s of +300 ms (132 sends): the majority holds for well under 132 ticks.
    lateness_meter meter = measured_meter();
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart && t < kShiftStart + 132 ? kShiftMs : 0.0; };
    EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 3000, link).empty());
}

TEST(LatenessMeter, TwoAndAHalfSecondShiftRaisesExactlyOnce) {
    // The design's row: 2.5 s of +300 ms (167 sends) is long enough.  The
    // majority holds from the 34th late sample until 33 on-time ones have
    // pushed the misses out again, about 167 ticks: 132 of them decide.
    for (const double jitter: {0.0, kJitterTicks}) {
        for (uint32_t salt = 1; salt <= 5; ++salt) {
            lateness_meter meter = measured_meter(salt, jitter);
            link_model link;
            link.salt = salt;
            link.jitter_ticks = jitter;
            link.extra_ms = [](uint32_t t) { return t >= kShiftStart && t < kShiftStart + 167 ? kShiftMs : 0.0; };
            const auto decisions = run(meter, kBase + kMeasured, kBase + 3000, link);
            ASSERT_EQ(decisions.size(), 1u) << "jitter " << jitter << " salt " << salt;
            EXPECT_EQ(decisions[0].decision.action, lateness_action::raise);
            // K within a tick of A5u's range: the decision lands as the shift
            // ends, from a window whose jitter-delayed late datagrams were
            // partly overtaken (the under-sampled tail), so one seed of five
            // reads 19.
            EXPECT_GE(decisions[0].decision.k, static_cast<int>(std::ceil(kShiftMs / 15.0)) - 1)
                << "jitter " << jitter << " salt " << salt;
            EXPECT_LE(decisions[0].decision.k, static_cast<int>(std::ceil(kShiftMs / 15.0)) + 4);
        }
    }
}

TEST(LatenessMeter, OneSecondSpikeNeverRaises) {
    // A9: +300 ms for one second, late and not lost, at every phase of the
    // window boundary.
    for (uint32_t phase = 0; phase < 66; phase += 5) {
        lateness_meter meter = measured_meter();
        link_model link;
        const uint32_t on = kShiftStart + phase;
        link.extra_ms = [on](uint32_t t) { return t >= on && t < on + 66 ? kShiftMs : 0.0; };
        EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500, link).empty()) << "phase " << phase;
        EXPECT_GT(meter.misses_total(), 0u);
    }
}

TEST(LatenessMeter, SingleHeldBurstNeverRaises) {
    // A path that queues instead of dropping: everything sent for half a
    // second, a second, or two seconds is held and released together, every
    // datagram of it late and each newer than the one before, so each gives
    // a sample.  However many late samples a burst gives, they arrive in one
    // instant and the on-time ones after them end the majority within 33
    // ticks.
    for (uint32_t phase = 0; phase < 66; phase += 7) {
        for (uint32_t held: {33u, 66u, 132u}) {
            lateness_meter meter = measured_meter();
            link_model link;
            const uint32_t on = kShiftStart + phase;
            const double release = (on + held) * kTickMs;
            link.extra_ms = [on, held, release](uint32_t t) {
                return t >= on && t < on + held ? release - t * kTickMs : 0.0;
            };
            link.jitter_ticks = 0.0;
            EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500, link).empty()) << "phase " << phase << " held " << held;
            EXPECT_GE(meter.misses_total(), held - 8) << "phase " << phase << " held " << held;
        }
    }
}

TEST(LatenessMeter, ThirtyThreeFrameStallNeverRaises) {
    // A9c: a client CPU stall of 500 ms - nothing is sent for 33 ticks, then
    // the client catches up and sends the 33 frames back to back, each late.
    for (uint32_t phase = 0; phase < 66; phase += 3) {
        lateness_meter meter = measured_meter();
        link_model link;
        const uint32_t on = kShiftStart + phase;
        link.send_ms = [on](uint32_t t) {
            if (t >= on && t < on + 33) return (on + 33) * kTickMs + (t - on) * 0.3;
            return t * kTickMs;
        };
        const auto decisions = run(meter, kBase + kMeasured, kBase + 2500, link);
        EXPECT_TRUE(decisions.empty()) << "phase " << phase;
        EXPECT_GT(meter.misses_total(), 0u);
        EXPECT_LE(meter.misses_total(), 33u);
    }
}

TEST(LatenessMeter, A4LossPatternNeverRaises) {
    // 10 % loss in bursts of 8 for the whole run: loss gives no sample.
    for (uint32_t phase = 0; phase < 80; phase += 9) {
        lateness_meter meter;
        meter.reset(kBase);
        link_model link;
        link.lost = [phase](uint32_t t) { return ((t + phase) / 8) % 10 == 0; };
        const auto decisions = run(meter, kBase, kBase + 4000, link);
        EXPECT_TRUE(decisions.empty()) << "phase " << phase;
        EXPECT_TRUE(meter.reference_measured());
        EXPECT_EQ(meter.misses_total(), 0u);
    }
}

TEST(LatenessMeter, BlackoutNeverRaises) {
    // A1, A2, A3L and longer: nothing arrives, then the client's schedule
    // resumes on time.
    for (uint32_t ms: {300u, 600u, 1000u, 5000u}) {
        for (uint32_t phase = 0; phase < 66; phase += 11) {
            lateness_meter meter = measured_meter();
            link_model link;
            const uint32_t on = kShiftStart + phase;
            const uint32_t span = static_cast<uint32_t>(ms / kTickMs);
            link.lost = [on, span](uint32_t t) { return t >= on && t < on + span; };
            EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500 + span, link).empty()) << ms << " ms phase " << phase;
            EXPECT_EQ(meter.misses_total(), 0u);
        }
    }
}

TEST(LatenessMeter, SpikeThenBlackoutNeverRaises) {
    // A9 followed by an uplink blackout (a Wi-Fi roam): the trailing window
    // still holds the spike's misses when the samples stop.  The silence must
    // not count as lateness: the streak holds while nothing arrives, and the
    // on-time samples after it end the majority.  The blackout starts with
    // the spike's last frame or 26 frames before it, so it also swallows
    // part of the late tail.
    for (const double jitter: {0.0, kJitterTicks}) {
        for (uint32_t ms: {300u, 1000u, 5000u}) {
            for (uint32_t cut: {40u, 66u}) {
                for (uint32_t phase = 0; phase < 66; phase += 13) {
                    lateness_meter meter = measured_meter(1, jitter);
                    link_model link;
                    link.jitter_ticks = jitter;
                    const uint32_t on = kShiftStart + phase;
                    const uint32_t span = static_cast<uint32_t>(ms / kTickMs);
                    link.extra_ms = [on](uint32_t t) { return t >= on && t < on + 66 ? kShiftMs : 0.0; };
                    link.lost = [on, cut, span](uint32_t t) { return t >= on + cut && t < on + cut + span; };
                    EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500 + span, link).empty())
                        << "jitter " << jitter << " " << ms << " ms cut " << cut << " phase " << phase;
                    EXPECT_GT(meter.misses_total(), 0u);
                }
            }
        }
    }
}

TEST(LatenessMeter, HeldBurstThenBlackoutNeverRaises) {
    // A queued burst released at once, every sample of it a miss, and then
    // silence: one instant of evidence, however long the silence after it.
    for (uint32_t held: {66u, 132u}) {
        for (uint32_t ms: {300u, 1000u, 5000u}) {
            lateness_meter meter = measured_meter();
            link_model link;
            link.jitter_ticks = 0.0;
            const uint32_t on = kShiftStart;
            const uint32_t span = static_cast<uint32_t>(ms / kTickMs);
            const double release = (on + held) * kTickMs;
            link.extra_ms = [on, held, release](uint32_t t) {
                return t >= on && t < on + held ? release - t * kTickMs : 0.0;
            };
            link.lost = [on, held, span](uint32_t t) { return t >= on + held && t < on + held + span; };
            EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500 + span, link).empty()) << "held " << held << " " << ms << " ms";
            EXPECT_GE(meter.misses_total(), held - 8);
        }
    }
}

TEST(LatenessMeter, StreaksHoldWithoutFreshSamples) {
    lateness_meter meter;
    meter.reset(kBase);
    uint32_t t = kBase;
    for (; t < kBase + 4 * 66; ++t) meter.sample(t, 6.0 + ((t - kBase) % 66) / 10.0);
    ASSERT_TRUE(meter.has_reference());
    // 66 misses, one per tick: the streak advances once the majority is there.
    uint32_t tick = 1000;
    for (int i = 0; i < 66; ++i, ++t, ++tick) {
        meter.sample(t, -10.0);
        meter.on_tick(tick);
    }
    const uint32_t streak = meter.raise_streak();
    EXPECT_EQ(streak, 66u - 33u);
    // Silence: kFreshTicks more ticks still count, then the streak holds.
    for (uint32_t i = 0; i < 500; ++i, ++tick) EXPECT_EQ(meter.on_tick(tick).action, lateness_action::none);
    EXPECT_EQ(meter.raise_streak(), streak + lateness_meter::kFreshTicks);
    EXPECT_FALSE(meter.fresh(tick - 1));
    // One on-time sample: fresh again, still a majority, the streak resumes.
    meter.sample(t++, 6.0);
    meter.on_tick(tick++);
    EXPECT_EQ(meter.raise_streak(), streak + lateness_meter::kFreshTicks + 1);
}

TEST(LatenessMeter, AckOfAnythingButThePendingDecisionIsIgnored) {
    lateness_meter meter = measured_meter();
    EXPECT_FALSE(meter.acknowledge(1, 5, kBase + 2000));   // nothing pending
    EXPECT_EQ(meter.offset(), 0);
    EXPECT_EQ(meter.cooldown_until(), 0u);
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? kShiftMs : 0.0; };
    const auto decisions = run(meter, kBase + kMeasured, kBase + 3000, link);
    ASSERT_EQ(decisions.size(), 1u);
    const auto& d = decisions[0].decision;
    EXPECT_EQ(meter.pending_id(), d.id);
    EXPECT_FALSE(meter.acknowledge(d.id + 1, d.k, kBase + 3000));   // another decision's
    EXPECT_EQ(meter.state(), lateness_state::pending);
    // A resync crossed the SessionLead in flight: the ack lands on the fresh
    // meter and changes nothing there.
    meter.reset(kBase + 3100);
    EXPECT_FALSE(meter.acknowledge(d.id, d.k, kBase + 3200));
    EXPECT_EQ(meter.offset(), 0);
    EXPECT_EQ(meter.cooldown_until(), 0u);
    EXPECT_EQ(meter.state(), lateness_state::measuring);
}

TEST(LatenessMeter, CooldownIsTheDecidedKs) {
    // A raise capped by the client (applied < k) still cools down for the
    // decided k (design 2.1: E + |k|*2 + 132); the offset moves by what was
    // applied.
    lateness_meter meter = measured_meter();
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? kShiftMs : 0.0; };
    const auto decisions = run(meter, kBase + kMeasured, kBase + 3000, link);
    ASSERT_EQ(decisions.size(), 1u);
    const auto& d = decisions[0].decision;
    ASSERT_TRUE(meter.acknowledge(d.id, 5, kBase + 3000));
    EXPECT_EQ(meter.offset(), 5);
    EXPECT_EQ(meter.cooldown_until(), kBase + 3000 + 2 * static_cast<uint32_t>(d.k) + lateness_meter::kCooldownTicks);
}

TEST(LatenessMeter, ReferenceIsFrozenUntilTheNextAssignment) {
    lateness_meter meter = measured_meter();
    const double r10 = meter.r10(), r01 = meter.r01();
    // +50 ms for good (3.3 ticks; the arrivals still qualify as a reference):
    // the arrivals move, the reference does not.
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? 50.0 : 0.0; };
    run(meter, kBase + kMeasured, kBase + 2000, link);
    EXPECT_EQ(meter.r10(), r10);
    EXPECT_EQ(meter.r01(), r01);
    EXPECT_LT(meter.trailing_quantile(0.10), r10 - 2.0);

    // A new assignment measures it anew, from the arrivals as they are now,
    // and keeps the player's total offset.
    meter.set_offset(3);
    ASSERT_EQ(meter.offset(), 3);
    const uint32_t base = kBase + 2100;
    meter.reset(base);
    EXPECT_FALSE(meter.has_reference());
    EXPECT_EQ(meter.offset(), 3);
    run(meter, base, base + kMeasured, link);
    ASSERT_TRUE(meter.reference_measured());
    EXPECT_NEAR(meter.r10(), r10 - 50.0 / kTickMs, 0.8);
}

TEST(LatenessMeter, AcknowledgedRaiseRestoresAndALowerFollowsTheReturn) {
    // A5d: +300 ms from kShiftStart until kReturn.  The raise is acknowledged
    // and applied (the client's frames leave k ticks earlier from E on); the
    // arrivals are back at the reference, so nothing more is decided until the
    // path returns, after which the lead is k too long and is lowered once.
    lateness_meter meter = measured_meter();
    const double r10 = meter.r10();
    constexpr uint32_t kReturn = kShiftStart + 1500;
    lead_steps lead;
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart && t < kReturn ? kShiftMs : 0.0; };
    link.lead_ticks = [&lead](uint32_t t) { return lead.at(t); };
    const auto decisions = run(meter, kBase + kMeasured, kReturn + 2000, link,
        [&](uint32_t tick, const lateness_decision& d) {
            // A raise: E = N + rtt + k + 2 (design 2.3) at a 300 + 150 ms
            // trip.  A lower moves no range: E = N (design 2.7).
            const uint32_t e = d.k > 0 ? tick + 30 + static_cast<uint32_t>(d.k) + 2 : tick;
            lead.steps.emplace_back(e, lead.total() + d.k);
            EXPECT_TRUE(meter.acknowledge(d.id, d.k, e));
        });
    ASSERT_EQ(decisions.size(), 2u);
    EXPECT_EQ(decisions[0].decision.action, lateness_action::raise);
    EXPECT_EQ(decisions[1].decision.action, lateness_action::lower);
    const int k = decisions[0].decision.k;
    EXPECT_GE(k, 20);
    EXPECT_LE(k, 24);
    // The lower is 660 ticks of early arrivals after the return, and takes the
    // lead back to within a tick or two of where it started.
    EXPECT_GE(decisions[1].tick, kReturn + lateness_meter::kLowerTicks);
    EXPECT_LT(decisions[1].decision.k, 0);
    EXPECT_GE(decisions[1].decision.k, -k);
    EXPECT_LE(k + decisions[1].decision.k, 3);
    EXPECT_EQ(meter.offset(), k + decisions[1].decision.k);
    EXPECT_EQ(meter.r10(), r10);
}

TEST(LatenessMeter, LowerNeverTakesTheOffsetBelowZero) {
    lateness_meter meter = measured_meter();
    meter.set_offset(5);   // a raise of 5 that was never needed
    link_model link;
    link.lead_ticks = [](uint32_t) { return 20; };   // and arrivals 20 ticks early
    const auto decisions = run(meter, kBase + kMeasured, kBase + 3000, link,
        [&](uint32_t tick, const lateness_decision& d) { EXPECT_TRUE(meter.acknowledge(d.id, d.k, tick)); });
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].decision.action, lateness_action::lower);
    EXPECT_EQ(decisions[0].decision.k, -5);
    EXPECT_EQ(meter.offset(), 0);
}

TEST(LatenessMeter, NoLowerWithoutAnOffset) {
    // Early arrivals alone never lower a lead the meter never raised.
    lateness_meter meter = measured_meter();
    link_model link;
    link.lead_ticks = [](uint32_t) { return 20; };
    EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2000, link).empty());
}

TEST(LatenessMeter, CooldownDiscardsOldSamplesAndTwoUnrestoredRaisesGiveUp) {
    // The client acknowledges every raise but its arrivals never move (the
    // raise was capped, or never really applied): one more raise after the
    // cooldown, then the meter stops deciding for this assignment.
    lateness_meter meter = measured_meter();
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? kShiftMs : 0.0; };
    std::vector<uint32_t> cooldowns;
    std::vector<uint32_t> effectives;
    const auto decisions = run(meter, kBase + kMeasured, kBase + 6000, link,
        [&](uint32_t tick, const lateness_decision& d) {
            if (d.action != lateness_action::raise) return;
            const uint32_t e = tick + 30 + static_cast<uint32_t>(d.k) + 2;
            EXPECT_TRUE(meter.acknowledge(d.id, d.k, e));
            effectives.push_back(e);
            cooldowns.push_back(meter.cooldown_until());
            EXPECT_EQ(meter.cooldown_until(), e + 2 * static_cast<uint32_t>(d.k) + lateness_meter::kCooldownTicks);
            EXPECT_EQ(meter.state(), lateness_state::cooldown);
            EXPECT_EQ(meter.trailing_count(), 0);
            // A frame below E is from the old schedule and gives no sample.
            EXPECT_FALSE(meter.sample(e - 1, -100.0));
        });
    ASSERT_EQ(decisions.size(), 3u);
    EXPECT_EQ(decisions[0].decision.action, lateness_action::raise);
    EXPECT_EQ(decisions[1].decision.action, lateness_action::raise);
    EXPECT_EQ(decisions[2].decision.action, lateness_action::give_up);
    EXPECT_GE(decisions[1].tick, cooldowns[0]);
    EXPECT_GE(decisions[2].tick, cooldowns[1]);
    // No decision before E + 33 + 132: the window restarts at E.
    EXPECT_GE(decisions[1].tick, effectives[0] + 33 + 131);
    EXPECT_EQ(meter.state(), lateness_state::given_up);
    EXPECT_EQ(meter.offset(), decisions[0].decision.k + decisions[1].decision.k);
}

TEST(LatenessMeter, ARestoredRaiseResetsTheCount) {
    // Two separate shifts, each fully compensated by its raise: neither raise
    // counts against the other, so the second is decided like the first.
    lateness_meter meter = measured_meter();
    constexpr uint32_t kSecond = kShiftStart + 1500;
    lead_steps lead;
    link_model link;
    link.extra_ms = [](uint32_t t) {
        return (t >= kShiftStart ? kShiftMs : 0.0) + (t >= kSecond ? kShiftMs : 0.0);
    };
    link.lead_ticks = [&lead](uint32_t t) { return lead.at(t); };
    const auto decisions = run(meter, kBase + kMeasured, kSecond + 1500, link,
        [&](uint32_t tick, const lateness_decision& d) {
            const uint32_t e = tick + 30 + static_cast<uint32_t>(d.k) + 2;
            lead.steps.emplace_back(e, lead.total() + d.k);
            EXPECT_TRUE(meter.acknowledge(d.id, d.k, e));
        });
    ASSERT_EQ(decisions.size(), 2u);
    EXPECT_EQ(decisions[0].decision.action, lateness_action::raise);
    EXPECT_EQ(decisions[1].decision.action, lateness_action::raise);
    EXPECT_GE(decisions[1].tick, kSecond);
}

TEST(LatenessMeter, StructurallyLateMemberTakesTheOthersMedian) {
    // Late from its first tick: more than 10 % of every window after the
    // deadline, so no window qualifies; after 8 it wants the others' median.
    lateness_meter meter;
    meter.reset(kBase);
    link_model late;
    late.extra_ms = [](uint32_t) { return 120.0; };   // slack about -2 +- 3
    // The overtaken datagrams give no sample, so 8 windows take more ticks
    // than 8 * 66.
    uint32_t tick = kBase;
    std::vector<decision_at> decisions;
    while (!meter.wants_fallback() && tick < kBase + 3000) {
        const auto more = run(meter, tick, tick + 66, late);
        decisions.insert(decisions.end(), more.begin(), more.end());
        tick += 66;
    }
    EXPECT_TRUE(decisions.empty());
    EXPECT_EQ(meter.windows(), lateness_meter::kReferenceMaxWindows);
    EXPECT_EQ(meter.qualifying_windows(), 0);
    EXPECT_FALSE(meter.has_reference());
    EXPECT_TRUE(meter.wants_fallback());
    EXPECT_EQ(meter.state(), lateness_state::wants_fallback);

    // The runner's fallback: the median of the measured references.
    const lateness_meter a = measured_meter(2), b = measured_meter(3), c = measured_meter(4);
    meter.adopt_reference(lateness_median({a.r10(), b.r10(), c.r10()}),
                          lateness_median({a.r01(), b.r01(), c.r01()}));
    ASSERT_TRUE(meter.has_reference());
    EXPECT_FALSE(meter.reference_measured());
    // Against the others' schedule its arrivals are misses for good: the
    // raise rule applies and puts it back with them.
    decisions = run(meter, tick, tick + 2000, late);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].decision.action, lateness_action::raise);
    EXPECT_GE(decisions[0].decision.k, static_cast<int>(std::ceil(120.0 / kTickMs)));
    EXPECT_LE(decisions[0].decision.k, static_cast<int>(std::ceil(120.0 / kTickMs)) + 3);
}

TEST(LatenessMeter, ResetKeepsOnlyTheOffset) {
    lateness_meter meter = measured_meter();
    meter.set_offset(4);
    meter.reset(kBase + 1000);
    EXPECT_EQ(meter.offset(), 4);
    EXPECT_FALSE(meter.has_reference());
    EXPECT_EQ(meter.samples_total(), 0u);
    EXPECT_EQ(meter.state(), lateness_state::measuring);
    EXPECT_EQ(meter.base(), kBase + 1000u);
}

TEST(LatenessMeter, SlackOfTimePoints) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::time_point{} + std::chrono::milliseconds(1000);
    EXPECT_NEAR(lateness_slack_ticks(deadline, deadline - std::chrono::milliseconds(150)), 150.0 / kTickMs, 1e-9);
    EXPECT_NEAR(lateness_slack_ticks(deadline, deadline + std::chrono::microseconds(15152)), -15152.0 / (1e6 / 66), 1e-9);
}
