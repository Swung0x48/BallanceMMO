#include <gtest/gtest.h>

#include <session/lateness.hpp>
#include <session/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace bmmo::session;

TEST(ServerWait, TheStepTestsHoldForAMissingFrame) {
    patience_member member;
    constexpr double lead = 5.0;
    EXPECT_DOUBLE_EQ(server_wait_ticks(21.4, member, false, true, lead), 21.4);
    EXPECT_DOUBLE_EQ(server_wait_ticks(-3.0, member, false, true, lead), 0.0);
    EXPECT_DOUBLE_EQ(server_wait_ticks(21.4, member, true, true, lead), 16.4);
    EXPECT_DOUBLE_EQ(server_wait_ticks(4.0, member, true, true, lead), 0.0);
    member.waived = true;
    EXPECT_DOUBLE_EQ(server_wait_ticks(21.4, member, true, true, lead), 0.0);
    EXPECT_DOUBLE_EQ(server_wait_ticks(21.4, member, true, false, lead), 16.4);
    EXPECT_DOUBLE_EQ(server_wait_ticks(21.4, member, false, true, lead), 21.4);
}

TEST(ServerWait, TheHeaderCarriesTheLargestOfFourWindows) {
    server_wait_meter m;
    EXPECT_EQ(m.value(), 0u);
    for (uint32_t t = 600; t < 660; ++t) m.sample(t, t == 630 ? 21.2 : 20.0);
    EXPECT_EQ(m.value(), 22u) << "21.2 rounded up";
    m.sample(660, 0.0);                       // a substituted step: at the deadline
    EXPECT_EQ(m.value(), 22u) << "a deadline step's own 0 says nothing about the wait";
    for (uint32_t t = 661; t < 840; ++t) m.sample(t, 6.7);
    EXPECT_EQ(m.value(), 22u) << "window 10 still counts at 13";
    m.sample(840, 6.7);                       // window 14: 10 is out
    EXPECT_EQ(m.value(), 7u);
    m.sample(700, 30.0);                      // window 11, still inside
    EXPECT_EQ(m.value(), 30u);
    m.sample(500, 90.0);                      // window 8: aged out, dropped
    EXPECT_EQ(m.value(), 30u);
    m.sample(841, 400.0);
    EXPECT_EQ(m.value(), 255u) << "saturated";
}

namespace {
    using slack_link = std::function<double(uint32_t)>;
    constexpr uint32_t kTicks = 1000, kEpisode = 600, kRecovery = 800;
    constexpr double kDeadlineOffset = 32;

    arrival_ring::time_point instant(double ticks) {
        return arrival_ring::time_point{} + std::chrono::duration_cast<arrival_ring::time_point::duration>(
            std::chrono::duration<double, std::micro>(ticks * TICK_MICROS));
    }

    struct sim_result {
        std::vector<double> step_time;
        std::vector<std::vector<bool>> fresh;
        std::vector<uint64_t> episodes;
        int substituted(size_t player, uint32_t from, uint32_t to) const {
            return static_cast<int>(std::count(fresh[player].begin() + from, fresh[player].begin() + to, false));
        }
    };

    sim_result run_sim(const std::vector<slack_link>& links, bool patience = true, double seen_delay = 0) {
        struct datagram { double at; size_t player; uint32_t tick; };
        std::vector<datagram> datagrams;
        for (size_t player = 0; player < links.size(); ++player)
            for (uint32_t tick = 0; tick < kTicks + 40; ++tick) {
                const double slack = links[player](tick);
                if (!std::isnan(slack)) datagrams.push_back({tick + kDeadlineOffset - slack, player, tick});
            }
        std::stable_sort(datagrams.begin(), datagrams.end(),
            [](const auto& first, const auto& second) { return first.at < second.at; });
        std::vector<input_buffer> buffers(links.size());
        std::vector<arrival_ring> arrivals(links.size());
        std::vector<lateness_baseline> baselines(links.size());
        std::vector<patience_waiver> waivers(links.size());
        for (size_t player = 0; player < links.size(); ++player) {
            buffers[player].reset(0);
            baselines[player].assigned(0, true, 0);
            baselines[player].settled(0, instant(0));
        }
        std::vector<std::vector<patience_member>> history(kTicks);
        const auto members_at = [&](uint32_t tick) {
            std::vector<patience_member> members(links.size());
            for (size_t player = 0; player < links.size(); ++player) {
                auto& member = members[player];
                member.has = buffers[player].has(tick);
                member.settled = buffers[player].settled(tick);
                member.held = buffers[player].held(tick) || buffers[player].reanchored(tick);
                member.waived = waivers[player].waived();
                member.counted = baselines[player].frozen() && !member.held && !member.waived
                    && !(member.settled && !member.has);
                member.baseline = baselines[player].value();
                if (const auto* frame = arrivals[player].find(tick))
                    member.ns = member.baseline - lateness_slack_ticks(instant(tick + kDeadlineOffset), frame->seen);
            }
            return members;
        };
        const auto recover = [&](size_t player, uint32_t tick) {
            if (!waivers[player].waived() || history[tick].empty()) return;
            auto members = history[tick];
            members[player].waived = false;
            members[player].counted = baselines[player].frozen();
            double latest = 0;
            for (size_t other = 0; other < links.size(); ++other)
                if (const auto* frame = arrivals[other].find(tick)) {
                    members[other].has = true;
                    members[other].ns = members[other].baseline
                        - lateness_slack_ticks(instant(tick + kDeadlineOffset), frame->seen);
                    latest = std::max(latest, members[other].ns - members[other].baseline);
                }
            if (members[player].has)
                waivers[player].sample(members[player].baseline - members[player].ns,
                    quorum_patience(members, player, latest).lead());
        };
        sim_result result;
        result.step_time.resize(kTicks);
        result.fresh.assign(links.size(), std::vector<bool>(kTicks));
        uint32_t tick = 0;
        size_t next = 0;
        double now = 0;
        while (tick < kTicks) {
            const auto members = members_at(tick);
            const double remaining = tick + kDeadlineOffset - now;
            const auto verdict = patience_step_test(members, remaining <= 1e-9, remaining, patience);
            if (verdict.step) {
                result.step_time[tick] = now;
                history[tick] = members;
                for (size_t player = 0; player < links.size(); ++player) {
                    bool fresh = false;
                    buffers[player].take(tick, fresh);
                    result.fresh[player][tick] = fresh;
                    if (patience && !fresh && !members[player].settled) waivers[player].substituted();
                }
                for (size_t player = 0; player < links.size(); ++player) recover(player, tick);
                ++tick;
                continue;
            }
            const double wake = verdict.wait ? now + verdict.wait_ticks : tick + kDeadlineOffset;
            if (next < datagrams.size() && datagrams[next].at + seen_delay <= wake + 1e-9) {
                const auto datagram = datagrams[next++];
                now = std::max(now, datagram.at + seen_delay);
                const uint32_t first = datagram.tick >= 7 ? datagram.tick - 7 : 0;
                for (uint32_t frame_tick = first; frame_tick <= datagram.tick; ++frame_tick)
                    if (arrivals[datagram.player].record(frame_tick, instant(datagram.at), instant(now), frame_tick == datagram.tick)
                            && frame_tick < tick)
                        recover(datagram.player, frame_tick);
                baselines[datagram.player].sample(datagram.tick,
                    datagram.tick + kDeadlineOffset - datagram.at, instant(datagram.at));
                buffers[datagram.player].submit(first, std::vector<input_frame>(datagram.tick - first + 1));
            } else now = std::max(now, wake);
        }
        for (const auto& waiver: waivers) result.episodes.push_back(waiver.episodes());
        return result;
    }
}

TEST(Patience, SettledIsArrivedOrBelowTheCursor) {
    input_buffer buffer;
    buffer.reset(10);
    EXPECT_FALSE(buffer.settled(10));
    EXPECT_TRUE(buffer.settled(9));          // re-anchored past: nothing will come for it
    buffer.submit(10, std::vector<input_frame>(1));
    EXPECT_TRUE(buffer.settled(10));
    EXPECT_FALSE(buffer.settled(11));
    buffer.reset(20);                        // a late joiner's or a resync's base
    EXPECT_TRUE(buffer.settled(15));
    EXPECT_FALSE(buffer.has(15));
    EXPECT_FALSE(buffer.settled(20));
}

TEST(Patience, StepTestWithPatienceOffIsTheDeadlineRule) {
    std::vector<patience_member> members(2);
    members[0].has = members[0].settled = true;
    members[1].settled = members[1].waived = true;
    members[1].counted = true;
    members[1].baseline = 24;
    EXPECT_FALSE(patience_step_test(members, false, 3, false).step);
    EXPECT_FALSE(patience_step_test(members, false, 3, false).wait);
    EXPECT_TRUE(patience_step_test(members, true, 0, false).step);
    members[1].held = true;
    EXPECT_TRUE(patience_step_test(members, false, 3, false).step);
    members[0].has = false;
    EXPECT_FALSE(patience_step_test(members, false, 3, false).step);
    members[0].has = members[1].has = true;
    EXPECT_TRUE(patience_step_test(members, false, 3, false).step);
}

TEST(Patience, StepTestWaitsUntilTheEarliestPatience) {
    std::vector<patience_member> members(4);
    for (auto& member: members) { member.counted = true; member.baseline = 24; }
    members[0].has = members[1].has = true;
    auto verdict = patience_step_test(members, false, 20, true);
    EXPECT_FALSE(verdict.step);
    EXPECT_DOUBLE_EQ(verdict.wait_ticks, 6);
    EXPECT_TRUE(patience_step_test(members, false, 14, true).early);
    members[2].has = true;
    EXPECT_TRUE(patience_step_test(members, false, 20, true).early);
    EXPECT_FALSE(patience_step_test(members, false, 20, true, 6, 12).step);
    EXPECT_TRUE(patience_step_test(members, false, 18, true, 6, 12).early);
    const auto timer = quorum_patience(members, 3, -20);
    EXPECT_DOUBLE_EQ(server_wait_ticks(24, members[3], true, true, timer.lead()), 4);
    members[3].counted = false;
    EXPECT_FALSE(patience_step_test(members, false, 1, true).step);
    EXPECT_TRUE(patience_step_test(members, true, 0, true).step);
}

TEST(Patience, StepTestNeedsARealFrameToSkipAWaivedOrReanchoredMember) {
    std::vector<patience_member> members(2);
    members[0].waived = true;
    members[1].settled = members[1].held = true;
    EXPECT_FALSE(patience_step_test(members, false, 1, true).step);
    EXPECT_DOUBLE_EQ(patience_step_test(members, false, 1, true).wait_ticks, 1);
    EXPECT_TRUE(patience_step_test(members, true, 0, true).step);
    members.emplace_back();
    members.back().has = true;
    EXPECT_TRUE(patience_step_test(members, false, 20, true).early);
    members.resize(1);
    EXPECT_FALSE(patience_step_test(members, false, 1, true).step);
    EXPECT_TRUE(patience_step_test(members, true, 0, true).step);
}

TEST(Patience, WaiverStartsAtASubstitutionAndEndsInsidePatience) {
    patience_waiver waiver;
    EXPECT_TRUE(waiver.substituted());
    EXPECT_FALSE(waiver.substituted());
    EXPECT_EQ(waiver.episodes(), 1u);
    std::vector<patience_member> members(2);
    for (auto& member: members) { member.has = member.counted = true; member.baseline = 24; }
    members[0].ns = 20;
    members[1].ns = 25;
    EXPECT_FALSE(waiver.sample(-1, quorum_patience(members, 1, 1).lead()));
    members[1].ns = 23;
    EXPECT_TRUE(waiver.sample(1, quorum_patience(members, 1, 1).lead()));
    EXPECT_FALSE(waiver.waived());
    waiver.substituted();
    members[0].counted = false;
    EXPECT_DOUBLE_EQ(quorum_patience(members, 1, 1).lead(), 0);
    EXPECT_FALSE(waiver.sample(-0.1, 0));
    EXPECT_TRUE(waiver.sample(0, quorum_patience(members, 1, 1).lead()));
}

TEST(Patience, SteadyLinksAreNeverCutOff) {
    const std::vector<slack_link> links{
        [](uint32_t tick) { return 24 + std::sin(tick * .7); },
        [](uint32_t tick) { return 15 + std::sin(tick * .3); },
        [](uint32_t tick) { return 3 + std::sin(tick * .9); }};
    for (const bool patience: {false, true}) {
        const auto result = run_sim(links, patience);
        for (size_t player = 0; player < links.size(); ++player)
            EXPECT_EQ(result.substituted(player, 8, kTicks), 0);
    }
}

TEST(Patience, OneMemberTwentyLateIsCutFromTheFirstTick) {
    const slack_link healthy = [](uint32_t) { return 24.0; };
    const slack_link late = [](uint32_t tick) { return tick >= kEpisode && tick < kRecovery ? 4.0 : 24.0; };
    const auto control = run_sim({healthy, healthy, healthy, healthy});
    const auto result = run_sim({healthy, healthy, healthy, late});
    EXPECT_EQ(result.substituted(3, kEpisode, kRecovery), kRecovery - kEpisode);
    EXPECT_NEAR(result.step_time[kEpisode] - control.step_time[kEpisode], kQuorumLastMargin, 1e-6);
    for (uint32_t tick = kEpisode + 1; tick < kRecovery; ++tick)
        EXPECT_NEAR(result.step_time[tick], std::max(control.step_time[tick], result.step_time[kEpisode]), 1e-6) << tick;
    for (size_t player = 0; player < 3; ++player) EXPECT_EQ(result.substituted(player, 8, kTicks), 0);
    EXPECT_EQ(result.episodes[3], 1u);
    EXPECT_EQ(result.substituted(3, kRecovery + 1, kTicks), 0);
}

TEST(Patience, SharedDipCutsNobody) {
    const slack_link dip = [](uint32_t tick) { return tick >= kEpisode && tick < kRecovery ? 9.0 : 24.0; };
    for (const double seen_delay: {0.0, 1.0}) {
        const auto result = run_sim({dip, dip, dip, dip}, true, seen_delay);
        for (size_t player = 0; player < 4; ++player) {
            EXPECT_EQ(result.substituted(player, 8, kTicks), 0);
            EXPECT_EQ(result.episodes[player], 0u);
        }
    }
}

TEST(Patience, StructurallyLastMemberIsNeverStuckSubstituted) {
    const auto result = run_sim({[](uint32_t) { return 24.0; }, [](uint32_t) { return 15.0; },
        [](uint32_t tick) { return tick >= kEpisode && tick < kRecovery ? -17.0 : 3.0; }});
    EXPECT_EQ(result.substituted(2, kEpisode, kRecovery), kRecovery - kEpisode);
    EXPECT_EQ(result.substituted(2, kRecovery + 25, kTicks), 0);
    EXPECT_EQ(result.episodes[2], 1u);
    EXPECT_EQ(result.substituted(0, 8, kTicks), 0);
    EXPECT_EQ(result.substituted(1, 8, kTicks), 0);
}
