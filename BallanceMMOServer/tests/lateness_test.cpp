// Unit tests for the server's lateness meter (session/lateness.hpp, late-player
// design 2.1): synthetic uplink series fed the way the runner feeds it - one
// sample per datagram from its newest frame, in arrival order, and one
// evaluation per stepped tick.  The rule must raise exactly once for a
// persistent +300 ms shift or a 2.5 s one and never for a single burst, a
// one-second spike, a 33-frame stall, the A4 loss pattern (10 % in bursts of
// 8) or a blackout - alone or after a spike or a burst - wherever those fall
// relative to a window boundary.
#include <gtest/gtest.h>

#include <entity/session.hpp>
#include <session/lateness.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <vector>

using namespace bmmo::session;

namespace relative_tests {
    struct decision_at {
        size_t member;
        uint32_t tick;
        relative_decision decision;
    };
    struct result {
        std::vector<relative_meter> meters;
        std::vector<decision_at> decisions;
    };
    result run(uint32_t ticks, double baseline, const std::function<double(size_t, uint32_t)>& shift,
               const std::vector<int>& offsets) {
        struct event {
            double at;
            uint32_t tick;
            size_t member;
        };
        const size_t count = offsets.size();
        result out{std::vector<relative_meter>(count), {}};
        std::vector<arrival_ring> rings(count);
        std::vector<uint32_t> newest(count);
        std::vector<event> events;
        for (size_t member = 0; member < count; ++member) {
            out.meters[member].set_offset(offsets[member]);
            for (uint32_t tick = 1; tick <= ticks + 66; ++tick)
                events.push_back({tick - baseline + shift(member, tick), tick, member});
        }
        for (uint32_t tick = 1; tick <= ticks; ++tick) events.push_back({double(tick), tick, count});
        std::sort(events.begin(), events.end(), [](const event& left, const event& right) {
            return left.at < right.at || (left.at == right.at && left.member < right.member);
        });
        const auto time = [](double tick) {
            return arrival_ring::time_point{} + std::chrono::duration_cast<arrival_ring::time_point::duration>(
                std::chrono::duration<double, std::micro>(tick * TICK_MICROS));
        };
        uint32_t stepped = 0;
        for (const auto& event: events) {
            if (event.at > ticks) break;
            if (event.member < count) {
                const bool as_newest = event.tick > newest[event.member];
                rings[event.member].record(event.tick, time(event.at), time(event.at), as_newest);
                if (as_newest) {
                    newest[event.member] = event.tick;
                    out.meters[event.member].sample_absolute(event.tick, event.tick - event.at, baseline);
                }
            } else stepped = event.tick;
            if (count > 1 && event.tick <= stepped) {
                std::vector<double> normalized;
                for (const auto& ring: rings) {
                    const auto* frame = ring.find(event.tick);
                    const double slack = lateness_slack_ticks(time(event.tick), frame ? frame->at : time(event.at));
                    normalized.push_back(baseline - std::round(slack * 1e6) / 1e6);
                }
                for (size_t member = 0; member < count; ++member) {
                    const auto* frame = rings[member].find(event.tick);
                    if (!frame || !frame->as_newest) continue;
                    const auto relative = relative_lateness(normalized, member);
                    out.meters[member].sample_relative(event.tick, relative.first, relative.second);
                }
            }
            if (event.member < count) continue;
            const size_t majorities = static_cast<size_t>(std::count_if(out.meters.begin(), out.meters.end(),
                [](const relative_meter& meter) { return meter.absolute_majority(); }));
            const bool shared = count >= 2 && 2 * majorities > count;
            for (size_t member = 0; member < count; ++member) {
                const auto decision = out.meters[member].on_tick(event.tick, shared);
                if (decision.action != lateness_action::none) out.decisions.push_back({member, event.tick, decision});
            }
        }
        return out;
    }
}

TEST(RelativeMeter, OneLateMemberRaisesOnceByItsRelativeP90) {
    const auto out = relative_tests::run(1200, 24, [](size_t member, uint32_t) { return member == 0 ? 20.0 : 0.0; }, {0, 0, 0, 0});
    ASSERT_EQ(out.decisions.size(), 1u);
    EXPECT_EQ(out.decisions[0].member, 0u);
    EXPECT_STREQ(out.decisions[0].decision.rule, "relative");
    EXPECT_EQ(out.decisions[0].decision.k, 20);
    EXPECT_EQ(out.decisions[0].decision.streak, relative_meter::kRaiseTicks);
}

TEST(RelativeMeter, SharedDipsDoNotRaiseIncludingAShortAbsoluteMajority) {
    const auto dip = relative_tests::run(1000, 22, [](size_t, uint32_t tick) { return tick <= 400 ? 15.0 : 0.0; }, {0, 0, 0, 0});
    EXPECT_TRUE(dip.decisions.empty());
    const auto absolute = relative_tests::run(515, 12, [](size_t, uint32_t) { return 20.0; }, {0, 0, 0, 0});
    EXPECT_TRUE(absolute.decisions.empty());
}

TEST(RelativeMeter, LateMajorityNeeds660TicksAndUsesTheMinorityRule) {
    const auto out = relative_tests::run(1000, 12, [](size_t member, uint32_t) { return member < 3 ? 20.0 : 0.0; }, {0, 0, 0, 0});
    ASSERT_EQ(out.decisions.size(), 3u);
    for (const auto& decision: out.decisions) {
        EXPECT_LT(decision.member, 3u);
        EXPECT_STREQ(decision.decision.rule, "minority");
        EXPECT_EQ(decision.decision.k, 20);
        EXPECT_EQ(decision.decision.streak, relative_meter::kMinorityPersistTicks);
        EXPECT_EQ(decision.tick, 701u);
    }
}

TEST(RelativeMeter, SoloRaisesByTheAbsoluteMedianAfter132Ticks) {
    const auto out = relative_tests::run(1000, 12, [](size_t, uint32_t) { return 20.0; }, {0});
    ASSERT_EQ(out.decisions.size(), 1u);
    EXPECT_STREQ(out.decisions[0].decision.rule, "absolute");
    EXPECT_EQ(out.decisions[0].decision.k, 20);
    EXPECT_EQ(out.decisions[0].decision.streak, relative_meter::kRaiseTicks);
    EXPECT_EQ(out.decisions[0].tick, 173u);
}

TEST(RelativeMeter, RecoveryClampsTheReferenceAndLowersWithinOffsetAndSlack) {
    for (const int offset: {20, 4}) {
        auto out = relative_tests::run(1000, 22, [](size_t member, uint32_t) { return member == 0 ? -20.0 : 0.0; }, {offset, 0});
        ASSERT_EQ(out.decisions.size(), 1u);
        const auto& decision = out.decisions[0];
        EXPECT_EQ(decision.member, 0u);
        EXPECT_EQ(decision.decision.action, lateness_action::lower);
        EXPECT_EQ(decision.decision.k, -std::min(offset, 19));
        EXPECT_EQ(decision.decision.streak, relative_meter::kLowerTicks);
        EXPECT_NEAR(out.meters[1].relative_quantile(.5), 0, 1e-9);
        EXPECT_TRUE(out.meters[0].acknowledge(decision.decision.id, -100, decision.tick));
        EXPECT_EQ(out.meters[0].offset(), std::max(0, offset - 19));
        EXPECT_FALSE(out.meters[0].sample_relative(decision.tick - 1, -20, -20));
        EXPECT_FALSE(out.meters[0].sample_absolute(decision.tick - 1, 42, 22));
    }
    const auto slack = relative_tests::run(1000, 6, [](size_t member, uint32_t) { return member == 0 ? 1.0 : 20.0; }, {20, 0, 0});
    const auto lower = std::find_if(slack.decisions.begin(), slack.decisions.end(),
        [](const relative_tests::decision_at& decision) { return decision.member == 0; });
    ASSERT_NE(lower, slack.decisions.end());
    EXPECT_EQ(lower->decision.action, lateness_action::lower);
    EXPECT_EQ(lower->decision.k, -2);
}

TEST(ArrivalRing, FirstArrivalStaleAndWrap) {
    arrival_ring ring;
    const auto at = arrival_ring::time_point{} + std::chrono::seconds(1);
    EXPECT_TRUE(ring.record(100, at, at + std::chrono::milliseconds(2), true));
    EXPECT_FALSE(ring.record(100, at + std::chrono::milliseconds(3), at, false));
    EXPECT_EQ(ring.find(100)->at, at);
    EXPECT_TRUE(ring.find(100)->as_newest);
    EXPECT_TRUE(ring.record(99, at, at - std::chrono::milliseconds(1), false));
    EXPECT_EQ(ring.find(99)->seen, at);
    EXPECT_FALSE(ring.find(99)->as_newest);
    EXPECT_TRUE(ring.record(356, at, at, false));
    EXPECT_EQ(ring.find(100), nullptr);
    EXPECT_FALSE(ring.record(100, at, at, true));
    EXPECT_EQ(ring.find(356)->tick, 356u);
}

TEST(LatenessBaseline, HeldStartupWaitsForAdoptedX) {
    lateness_baseline baseline;
    const auto assigned = arrival_ring::time_point{};
    const auto x = assigned + std::chrono::seconds(40);
    baseline.assigned(20, true, 20);
    for (uint32_t t = 0; t < 1000; ++t) {
        EXPECT_FALSE(baseline.timeout(t, assigned + std::chrono::seconds(t)));
        EXPECT_FALSE(baseline.sample(t, -30, assigned + std::chrono::seconds(t)));
    }
    // Proposals and their acks do not call settled. Only the adopted X does.
    baseline.settled(20, x);
    EXPECT_FALSE(baseline.sample(1000, -30, x - std::chrono::microseconds(1)));
    for (uint32_t t = 0; t < 264; ++t) EXPECT_TRUE(baseline.sample(t, t % 2 ? 12 : 10, x));
    EXPECT_TRUE(baseline.frozen());
    EXPECT_EQ(baseline.value(), 11);
    EXPECT_FALSE(baseline.sample(300, 40, x)); // an acknowledged raise cannot remeasure it
    baseline.assigned(400, false, 400);
    baseline.settled(400, x);
    baseline.rebase(-2);
    EXPECT_EQ(baseline.value(), 9);
    EXPECT_FALSE(baseline.sample(401, 50, x));
}

TEST(LatenessBaseline, JoinCorrectionChecksCurrentEpochAndFloor) {
    lateness_baseline baseline;
    const auto at = arrival_ring::time_point{};
    baseline.assigned(100, false, 100);
    EXPECT_FALSE(baseline.sample(100, -20, at));
    baseline.assigned(200, false, 200);
    baseline.settled(100, at);
    EXPECT_FALSE(baseline.sample(200, -20, at));
    baseline.settled(200, at);
    for (uint32_t t = 200; t < 464; ++t) baseline.sample(t, 12, at);
    EXPECT_EQ(baseline.value(), 12);
    lateness_baseline fallback;
    fallback.assigned(1, false, 1);
    EXPECT_FALSE(fallback.timeout(660, at + std::chrono::seconds(40)));
    EXPECT_TRUE(fallback.timeout(661, at + std::chrono::seconds(10)));
    for (uint32_t t = 0; t < 264; ++t) fallback.sample(t, -20, at + std::chrono::seconds(10));
    EXPECT_EQ(fallback.value(), 3);
}

TEST(QuorumObservation, TimersClampAndInterveningExpiry) {
    std::vector<quorum_member> members(4);
    for (auto& m: members) { m.counted = m.has = true; m.baseline = 20; m.ns = 0; }
    members[3].ns = 8;
    EXPECT_EQ(quorum_step_time(members), -16); // expiry before the last arrival woke the runner
    members[2].ns = 12;
    EXPECT_EQ(quorum_step_time(members), -10); // two missing: M2
    members.resize(2);
    members[0].ns = -10;
    members[1].ns = 10;
    EXPECT_EQ(quorum_step_time(members), -16); // the early peer is clamped at on time
    members[0].counted = false;
    EXPECT_EQ(quorum_step_time(members), -10); // no reference, waits for both real frames
    members[0].has = members[1].has = false;
    members[0].waived = members[1].waived = true;
    EXPECT_EQ(quorum_step_time(members), 0); // pacing floor
}

namespace {
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

    double deadline_ms(uint32_t t) { return (t + kInputDelay) * TICK_MS; }

    struct link_model {
        // When the client sends the datagram whose newest frame is t (ms).
        std::function<double(uint32_t)> send_ms = [](uint32_t t) { return t * TICK_MS; };
        // Extra one-way delay on top of the base trip and the jitter (ms).
        std::function<double(uint32_t)> extra_ms = [](uint32_t) { return 0.0; };
        // A path that queues: the datagram of frame t arrives no earlier than
        // this (ms).  Every datagram of a held burst gets the very same
        // arrival, so their order is the send order whatever the compiler
        // does with release - t * TICK_MS (an FMA there - GCC 15 on the
        // ubuntu-26.04 runner - left three distinct arrivals and the sort
        // shuffled a third of the burst out of its samples).
        std::function<double(uint32_t)> held_until = [](uint32_t) { return -1e300; };
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
    std::vector<decision_at> run(relative_meter& meter, uint32_t from, uint32_t to, const link_model& link,
                                 const std::function<void(uint32_t, const lateness_decision&)>& on_decision = {}) {
        std::vector<datagram> wire;   // in flight
        std::vector<decision_at> out;
        uint32_t frame = from;
        double last_send = -1e300;
        for (uint32_t n = from; n < to; ++n) {
            const double now = deadline_ms(n);
            while (frame < to) {
                const double send = std::max(last_send, link.send_ms(frame) - link.lead_ticks(frame) * TICK_MS);
                if (send > now) break;
                last_send = send;
                if (!link.lost(frame))
                    wire.push_back({std::max(link.held_until(frame), send + kUplinkMs + link.extra_ms(frame)
                                    + unit_noise(frame, link.salt) * link.jitter_ticks * TICK_MS), frame});
                ++frame;
            }
            std::stable_sort(wire.begin(), wire.end(),
                             [](const datagram& a, const datagram& b) { return a.arrival_ms < b.arrival_ms; });
            size_t delivered = 0;
            while (delivered < wire.size() && wire[delivered].arrival_ms <= now) {
                const auto& d = wire[delivered++];
                const double slack = (deadline_ms(d.newest) - d.arrival_ms) / TICK_MS;
                if (meter.sample_absolute(d.newest, slack, kInputDelay - kUplinkMs / TICK_MS)) {
                    const double normalized = kInputDelay - kUplinkMs / TICK_MS - slack;
                    meter.sample_relative(d.newest, normalized, normalized);
                }
            }
            wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(delivered));
            const auto d = meter.on_tick(n, false);
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

    constexpr uint32_t kMeasured = 1000;

    // Warm the trailing windows with arrivals from the steady link.
    relative_meter steady_meter(uint32_t salt = 1, double jitter = kJitterTicks) {
        relative_meter meter;
        meter.reset(kBase);
        link_model steady;
        steady.salt = salt;
        steady.jitter_ticks = jitter;
        run(meter, kBase, kBase + kMeasured, steady);
        EXPECT_GT(meter.samples_total(), 0u);
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
}

TEST(LatenessMeter, SampleIsTheNewestFrameAndDuplicatesGiveNone) {
    relative_meter meter;
    meter.reset(kBase);
    EXPECT_FALSE(meter.sample_absolute(kBase - 1, 5.0, 6.0));   // the old numbering
    EXPECT_TRUE(meter.sample_absolute(kBase + 3, 5.0, 6.0));
    EXPECT_FALSE(meter.sample_absolute(kBase + 3, 5.0, 6.0));   // a duplicate
    EXPECT_FALSE(meter.sample_absolute(kBase + 2, 5.0, 6.0));   // overtaken
    EXPECT_TRUE(meter.sample_absolute(kBase + 4, 4.0, 6.0));
    EXPECT_EQ(meter.samples_total(), 2u);
    EXPECT_EQ(meter.slack_quantile(0.0), 4.0);
}

TEST(LatenessMeter, SteadyJitteredLinkNeverDecides) {
    relative_meter meter;
    meter.reset(kBase);
    EXPECT_TRUE(run(meter, kBase, kBase + 4000, link_model{}).empty());
    EXPECT_EQ(meter.absolute_misses(), 0u);
}

TEST(LatenessMeter, ShiftEndingBeforeTheRuleHoldsDoesNotRaise) {
    // 2 s of +300 ms (132 sends): the majority holds for well under 132 ticks.
    relative_meter meter = steady_meter();
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
            relative_meter meter = steady_meter(salt, jitter);
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
        relative_meter meter = steady_meter();
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
            relative_meter meter = steady_meter();
            link_model link;
            const uint32_t on = kShiftStart + phase;
            const double release = (on + held) * TICK_MS + kUplinkMs;
            link.held_until = [on, held, release](uint32_t t) { return t >= on && t < on + held ? release : -1e300; };
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
        relative_meter meter = steady_meter();
        link_model link;
        const uint32_t on = kShiftStart + phase;
        link.send_ms = [on](uint32_t t) {
            if (t >= on && t < on + 33) return (on + 33) * TICK_MS + (t - on) * 0.3;
            return t * TICK_MS;
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
        relative_meter meter;
        meter.reset(kBase);
        link_model link;
        link.lost = [phase](uint32_t t) { return ((t + phase) / 8) % 10 == 0; };
        const auto decisions = run(meter, kBase, kBase + 4000, link);
        EXPECT_TRUE(decisions.empty()) << "phase " << phase;
        EXPECT_GT(meter.samples_total(), 0u);
        EXPECT_EQ(meter.absolute_misses(), 0u);
    }
}

TEST(LatenessMeter, BlackoutNeverRaises) {
    // A1, A2, A3L and longer: nothing arrives, then the client's schedule
    // resumes on time.
    for (uint32_t ms: {300u, 600u, 1000u, 5000u}) {
        for (uint32_t phase = 0; phase < 66; phase += 11) {
            relative_meter meter = steady_meter();
            link_model link;
            const uint32_t on = kShiftStart + phase;
            const uint32_t span = static_cast<uint32_t>(ms / TICK_MS);
            link.lost = [on, span](uint32_t t) { return t >= on && t < on + span; };
            EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500 + span, link).empty()) << ms << " ms phase " << phase;
            EXPECT_EQ(meter.absolute_misses(), 0u);
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
                    relative_meter meter = steady_meter(1, jitter);
                    link_model link;
                    link.jitter_ticks = jitter;
                    const uint32_t on = kShiftStart + phase;
                    const uint32_t span = static_cast<uint32_t>(ms / TICK_MS);
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
            relative_meter meter = steady_meter();
            link_model link;
            link.jitter_ticks = 0.0;
            const uint32_t on = kShiftStart;
            const uint32_t span = static_cast<uint32_t>(ms / TICK_MS);
            const double release = (on + held) * TICK_MS + kUplinkMs;
            link.held_until = [on, held, release](uint32_t t) { return t >= on && t < on + held ? release : -1e300; };
            link.lost = [on, held, span](uint32_t t) { return t >= on + held && t < on + held + span; };
            EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2500 + span, link).empty()) << "held " << held << " " << ms << " ms";
            EXPECT_GE(meter.misses_total(), held - 8);
        }
    }
}

TEST(LatenessMeter, StreaksHoldWithoutFreshSamples) {
    relative_meter meter;
    meter.reset(kBase);
    uint32_t tick = 1000;
    for (uint32_t frame = kBase; frame < kBase + 66; ++frame, ++tick) {
        meter.sample_absolute(frame, -10.0, 6.0);
        meter.sample_relative(frame, 16.0, 16.0);
        meter.on_tick(tick, false);
    }
    const uint32_t streak = meter.raise_streak();
    EXPECT_EQ(streak, 33u);
    for (uint32_t count = 0; count < 500; ++count, ++tick)
        EXPECT_EQ(meter.on_tick(tick, false).action, lateness_action::none);
    EXPECT_EQ(meter.raise_streak(), streak + relative_meter::kFreshTicks);
    meter.sample_absolute(kBase + 66, 6.0, 6.0);
    meter.sample_relative(kBase + 66, 0.0, 0.0);
    meter.on_tick(tick, false);
    EXPECT_EQ(meter.raise_streak(), streak + relative_meter::kFreshTicks + 1);
}

TEST(LatenessMeter, AckOfAnythingButThePendingDecisionIsIgnored) {
    relative_meter meter = steady_meter();
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
    EXPECT_EQ(meter.state(), lateness_state::baseline);
}

TEST(LatenessMeter, CooldownIsTheDecidedKs) {
    // A raise capped by the client (applied < k) still cools down for the
    // decided k (design 2.1: E + |k|*2 + 132); the offset moves by what was
    // applied.
    relative_meter meter = steady_meter();
    link_model link;
    link.extra_ms = [](uint32_t t) { return t >= kShiftStart ? kShiftMs : 0.0; };
    const auto decisions = run(meter, kBase + kMeasured, kBase + 3000, link);
    ASSERT_EQ(decisions.size(), 1u);
    const auto& d = decisions[0].decision;
    ASSERT_TRUE(meter.acknowledge(d.id, 5, kBase + 3000));
    EXPECT_EQ(meter.offset(), 5);
    EXPECT_EQ(meter.cooldown_until(), kBase + 3000 + 2 * static_cast<uint32_t>(d.k) + relative_meter::kCooldownTicks);
}

TEST(LatenessMeter, AcknowledgedRaiseRestoresAndALowerFollowsTheReturn) {
    // A5d: +300 ms from kShiftStart until kReturn.  The raise is acknowledged
    // and applied (the client's frames leave k ticks earlier from E on); the
    // arrivals are back at the reference, so nothing more is decided until the
    // path returns, after which the lead is k too long and is lowered once.
    relative_meter meter = steady_meter();
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
    EXPECT_GE(decisions[1].tick, kReturn + relative_meter::kLowerTicks);
    EXPECT_LT(decisions[1].decision.k, 0);
    EXPECT_GE(decisions[1].decision.k, -k);
    EXPECT_LE(k + decisions[1].decision.k, 3);
    EXPECT_EQ(meter.offset(), k + decisions[1].decision.k);
}

TEST(LatenessMeter, NoLowerWithoutAnOffset) {
    // Early arrivals alone never lower a lead the meter never raised.
    relative_meter meter = steady_meter();
    link_model link;
    link.lead_ticks = [](uint32_t) { return 20; };
    EXPECT_TRUE(run(meter, kBase + kMeasured, kBase + 2000, link).empty());
}

TEST(LatenessMeter, CooldownDiscardsOldSamplesAndTwoUnrestoredRaisesGiveUp) {
    // The client acknowledges every raise but its arrivals never move (the
    // raise was capped, or never really applied): one more raise after the
    // cooldown, then the meter stops deciding for this assignment.
    relative_meter meter = steady_meter();
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
            EXPECT_EQ(meter.cooldown_until(), e + 2 * static_cast<uint32_t>(d.k) + relative_meter::kCooldownTicks);
            EXPECT_EQ(meter.state(), lateness_state::cooldown);
            EXPECT_EQ(meter.absolute_misses(), 0u);
            // A frame below E is from the old schedule and gives no sample.
            EXPECT_FALSE(meter.sample_absolute(e - 1, -100.0, 6.0));
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
    relative_meter meter = steady_meter();
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

TEST(LatenessMeter, ResetKeepsOnlyTheOffset) {
    relative_meter meter = steady_meter();
    meter.set_offset(4);
    meter.reset(kBase + 1000);
    EXPECT_EQ(meter.offset(), 4);
    EXPECT_EQ(meter.samples_total(), 0u);
    EXPECT_EQ(meter.samples_total(), 0u);
    EXPECT_EQ(meter.state(), lateness_state::baseline);
    EXPECT_EQ(meter.base(), kBase + 1000u);
}

TEST(LatenessMeter, SlackOfTimePoints) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::time_point{} + std::chrono::milliseconds(1000);
    EXPECT_NEAR(lateness_slack_ticks(deadline, deadline - std::chrono::milliseconds(150)), 150.0 / TICK_MS, 1e-9);
    EXPECT_NEAR(lateness_slack_ticks(deadline, deadline + std::chrono::microseconds(15152)), -15152.0 / TICK_MICROS, 1e-9);
}

TEST(LatenessMeter, AbsolutePersistenceRestartsWhenSharednessChanges) {
    for (bool initially_shared: {false, true}) {
        relative_meter meter;
        const uint32_t before = initially_shared ? 200 : 100;
        for (uint32_t tick = 1; tick <= before; ++tick) {
            meter.sample_absolute(tick, -10, 6);
            EXPECT_EQ(meter.on_tick(tick, initially_shared).action, lateness_action::none);
        }
        const uint32_t persistence = initially_shared ? relative_meter::kRaiseTicks : relative_meter::kSharedPersistTicks;
        for (uint32_t elapsed = 1; elapsed <= persistence; ++elapsed) {
            const uint32_t tick = before + elapsed;
            meter.sample_absolute(tick, -10, 6);
            const auto decision = meter.on_tick(tick, !initially_shared);
            EXPECT_EQ(decision.action, elapsed == persistence ? lateness_action::raise : lateness_action::none);
        }
    }
}

TEST(LatenessMeter, SharedReanchorKeepsAbsoluteEvidenceAndMinorityTakesPrecedence) {
    relative_meter shared, minority;
    int shared_raises = 0, minority_raises = 0;
    for (uint32_t tick = 1; tick <= 800; ++tick) {
        if (tick == 400) {
            const auto samples = shared.samples_total();
            const auto streak = shared.raise_streak();
            shared.reset(tick, true);
            EXPECT_EQ(shared.samples_total(), samples);
            EXPECT_EQ(shared.raise_streak(), streak);
            EXPECT_TRUE(shared.absolute_majority());
            EXPECT_FALSE(shared.sample_absolute(tick - 1, -10, 6));
        }
        shared.sample_absolute(tick, -10, 6);
        shared.sample_relative(tick, 0, 0);
        minority.sample_absolute(tick, -10, 6);
        if (tick > 1) minority.sample_relative(tick, 0, 16);
        const auto common = shared.on_tick(tick, true);
        const auto other = minority.on_tick(tick, true);
        if (common.action != lateness_action::none) {
            ++shared_raises;
            EXPECT_STREQ(common.rule, "shared-absolute");
            EXPECT_EQ(tick, 693u);
            EXPECT_EQ(common.k, 16);
        }
        if (other.action != lateness_action::none) {
            ++minority_raises;
            EXPECT_STREQ(other.rule, "minority");
            EXPECT_EQ(tick, 694u);
        }
    }
    EXPECT_EQ(shared_raises, 1);
    EXPECT_EQ(minority_raises, 1);
    shared.reset(900);
    EXPECT_EQ(shared.samples_total(), 0u);
    EXPECT_FALSE(shared.absolute_majority());
}

// The 2026-11-08 server crash (std::out_of_range, map::at): a resync's
// player_ready in the same gap between two steps as another member's leave
// asks shared_absolute, whose view (the last stepped tick's quorum members)
// still names the leaver; remove_player already dropped its meter.  The
// leaver is not counted, and nothing throws: two of the three members left
// hold an absolute majority (counted with the leaver it would be 2 of 4).
TEST(LatenessMeter, SharedAbsoluteSkipsAMemberThatLeftAfterTheStep) {
    struct entry { relative_meter meter; };
    std::map<uint32_t, entry> meters;
    for (uint32_t tick = 1; tick <= 400; ++tick) {
        meters[1].meter.sample_absolute(tick, -10, 6);
        meters[2].meter.sample_absolute(tick, -10, 6);
    }
    meters[3];
    ASSERT_TRUE(meters[1].meter.absolute_majority());
    ASSERT_FALSE(meters[3].meter.absolute_majority());
    std::vector<quorum_member> view(4);
    for (uint32_t player = 1; player <= 4; ++player) {
        view[player - 1].player = player;
        view[player - 1].ready = view[player - 1].has_baseline = true;
    }
    bool shared = false;
    EXPECT_NO_THROW(shared = shared_absolute_majority(view, meters));   // player 4 left: no meter
    EXPECT_TRUE(shared);
    meters.erase(2);
    EXPECT_FALSE(shared_absolute_majority(view, meters));   // 1 of 2
}
