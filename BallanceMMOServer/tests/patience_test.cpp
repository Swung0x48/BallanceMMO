// Unit tests for patience (late-player design 2.6, plan step 9): the session
// stops waiting for one player at that player's patience, deadline(T) - (r01 -
// 1) ticks of its frozen reference, instead of the deadline; the streak waiver
// stops even that wait after a substitution until the player's first arrival
// inside its patience; a member without statistics waits as long as the
// latest member with them.
//
// The pure pieces (session/lateness.hpp patience_*, input_buffer::settled) are
// tested directly, and then driven the way the runner drives them by a small
// event simulation: one uplink datagram per tick and player carrying its last
// 8 frames, the runner's step test evaluated after every arrival and at every
// wake-up it asks for, and each player's lateness meter fed the same samples
// the runner feeds it.
#include <gtest/gtest.h>

#include <session/lateness.hpp>
#include <session/timeline.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace bmmo::session;

namespace {
    constexpr double kTickMs = 1000.0 / 66.0;
    constexpr uint32_t kInputDelay = 16;
    constexpr uint32_t kRedundancy = 8;      // frames per uplink datagram
    const double kLost = std::numeric_limits<double>::quiet_NaN();

    double deadline_ms(uint32_t t) { return (t + kInputDelay) * kTickMs; }

    // Deterministic noise in [-1, 1] per tick.
    double unit_noise(uint32_t t, uint32_t salt) {
        uint32_t x = t * 2654435761u ^ (salt * 40503u + 0x9E3779B9u);
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return static_cast<double>(x) / 4294967295.0 * 2.0 - 1.0;
    }

    // The slack (ticks before its deadline) the datagram whose newest frame is
    // t arrives with; NaN = lost.
    using link = std::function<double(uint32_t)>;

    struct sim_options {
        bool patience = true;            // runner_config::patience
        bool waiver = true;              // the streak waiver starts at a substitution
        bool end_waiver_at_fresh = false;   // the rule 2.6 rejects: the waiver ends at a fresh frame
        int late_member = -1;            // this member was assigned after the start (a late joiner)
    };

    struct sim_result {
        std::vector<double> step_ms;                // when each tick was stepped
        std::vector<std::vector<bool>> fresh;       // [player][tick]
        std::vector<double> lead;                   // each player's lead at the end
        int substituted(size_t player, uint32_t from, uint32_t to) const {
            int n = 0;
            for (uint32_t t = from; t < to; ++t) n += fresh[player][t] ? 0 : 1;
            return n;
        }
    };

    sim_result run_sim(const std::vector<link>& links, uint32_t ticks, sim_options o = {}) {
        const size_t n = links.size();
        struct datagram { double at; size_t player; uint32_t newest; };
        std::vector<datagram> datagrams;
        for (size_t p = 0; p < n; ++p)
            for (uint32_t t = 0; t < ticks + 40; ++t) {
                const double slack = links[p](t);
                if (!std::isnan(slack)) datagrams.push_back({deadline_ms(t) - slack * kTickMs, p, t});
            }
        std::stable_sort(datagrams.begin(), datagrams.end(),
                         [](const datagram& a, const datagram& b) { return a.at < b.at; });

        std::vector<input_buffer> buffers(n);
        std::vector<lateness_meter> meters(n);
        std::vector<patience_waiver> waivers(n);
        for (size_t p = 0; p < n; ++p) {
            buffers[p].reset(0);
            meters[p].reset(0);
        }
        sim_result r;
        r.step_ms.assign(ticks, 0.0);
        r.fresh.assign(n, std::vector<bool>(ticks, false));

        // What the runner's patience_members() builds.
        const auto members_at = [&](uint32_t tick) {
            std::vector<patience_member> members;
            for (size_t p = 0; p < n; ++p) {
                members.push_back(patience_member_of(buffers[p].has(tick), buffers[p].settled(tick),
                                                     waivers[p].waived(), &meters[p], 0));
                members[p].late_assignment = static_cast<int>(p) == o.late_member;
            }
            if (o.patience) patience_assign_leads(members);
            return members;
        };

        uint32_t tick = 0;
        double now = 0.0;
        size_t next = 0;
        while (tick < ticks) {
            // The runner: step while the step test allows, then sleep until
            // the deadline or the earliest patience, or the next arrival.
            double wake = std::numeric_limits<double>::infinity();
            while (tick < ticks) {
                const auto members = members_at(tick);
                const auto v = patience_step_test(members, now >= deadline_ms(tick),
                                                  (deadline_ms(tick) - now) / kTickMs, o.patience);
                if (!v.step) {
                    wake = deadline_ms(tick);
                    if (v.wait) wake = std::min(wake, now + v.wait_ticks * kTickMs);
                    break;
                }
                r.step_ms[tick] = now;
                for (size_t p = 0; p < n; ++p) {
                    const bool settled = buffers[p].settled(tick);
                    bool fresh = false;
                    buffers[p].take(tick, fresh);
                    r.fresh[p][tick] = fresh;
                    if (o.patience && o.waiver && !fresh && !settled) waivers[p].substituted();
                    if (o.end_waiver_at_fresh && fresh) waivers[p] = patience_waiver{};
                }
                ++tick;
            }
            if (tick >= ticks) break;
            wake = std::max(wake, now + 1e-9);
            if (next < datagrams.size() && datagrams[next].at <= wake) {
                const auto d = datagrams[next++];
                now = std::max(now, d.at);
                const uint32_t first = d.newest >= kRedundancy - 1 ? d.newest - (kRedundancy - 1) : 0;
                buffers[d.player].submit(first, std::vector<input_frame>(d.newest - first + 1));
                const double slack = (deadline_ms(d.newest) - now) / kTickMs;
                if (meters[d.player].sample(d.newest, slack) && o.patience && !o.end_waiver_at_fresh)
                    waivers[d.player].sample(slack, members_at(tick)[d.player].lead);
            } else {
                now = wake;
            }
        }
        const auto members = members_at(ticks);
        for (const auto& m: members) r.lead.push_back(m.lead);
        return r;
    }

    // Two members whose frames are in about ten ticks before the deadline, and
    // one structurally last: about three ticks before it, every frame inside
    // the deadline (the worst ping, which input_delay was sized for).
    link early_member(uint32_t salt) {
        return [salt](uint32_t t) { return 10.0 + unit_noise(t, salt); };
    }
    double last_member_slack(uint32_t t) { return 3.0 + unit_noise(t, 7); }

    constexpr uint32_t kTicks = 1000;
    constexpr uint32_t kEpisode = 600;   // the references are frozen by then (4 windows of 66)
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

TEST(Patience, LeadIsR01LessTheMarginAndNeverPastTheDeadline) {
    EXPECT_DOUBLE_EQ(kPatienceMarginTicks, 1.0);
    EXPECT_DOUBLE_EQ(lateness_meter::kMissMargin, kPatienceMarginTicks);
    EXPECT_DOUBLE_EQ(patience_lead_ticks(5.0), 4.0);
    EXPECT_DOUBLE_EQ(patience_lead_ticks(1.0), 0.0);
    EXPECT_DOUBLE_EQ(patience_lead_ticks(0.4), 0.0);    // min(deadline, ...)
    EXPECT_DOUBLE_EQ(patience_lead_ticks(-3.0), 0.0);
}

TEST(Patience, StatisticsLessMemberGetsTheLatestPatienceAmongTheOthers) {
    std::vector<patience_member> members(3);
    members[0].has_statistics = true;
    members[0].r01 = 5.0;     // lead 4
    members[1].has_statistics = true;
    members[1].r01 = 9.0;     // lead 8
    members[2].late_assignment = true;   // a late joiner in its first windows
    patience_assign_leads(members);
    EXPECT_DOUBLE_EQ(members[0].lead, 4.0);
    EXPECT_DOUBLE_EQ(members[1].lead, 8.0);
    EXPECT_DOUBLE_EQ(members[2].lead, 4.0);   // the later patience: the others' schedule
    EXPECT_TRUE(members[2].capped);
    EXPECT_FALSE(members[0].capped);

    // A member with statistics is never capped at the others' schedule.
    members[0].r01 = 30.0;
    patience_assign_leads(members);
    EXPECT_DOUBLE_EQ(members[0].lead, 29.0);
    EXPECT_DOUBLE_EQ(members[2].lead, 8.0);

    // A member measuring since the session's start waits for the deadline
    // until its own reference is set, whoever else has one.
    members[2].late_assignment = false;
    patience_assign_leads(members);
    EXPECT_DOUBLE_EQ(members[2].lead, 0.0);
    EXPECT_FALSE(members[2].capped);

    // Nobody has statistics (the session's start): the deadline.
    std::vector<patience_member> none(2);
    none[0].r01 = 7.0;
    none[1].late_assignment = true;
    patience_assign_leads(none);
    EXPECT_DOUBLE_EQ(none[0].lead, 0.0);
    EXPECT_DOUBLE_EQ(none[1].lead, 0.0);
}

TEST(Patience, StepTestWithPatienceOffIsTheDeadlineRule) {
    std::vector<patience_member> members(2);
    members[0].has = members[0].settled = true;
    members[1].settled = true;               // settled by its cursor, but its frame is not here
    members[1].lead = 5.0;
    members[1].waived = true;
    auto v = patience_step_test(members, false, 3.0, false);
    EXPECT_FALSE(v.step);
    EXPECT_FALSE(v.wait);
    v = patience_step_test(members, true, 0.0, false);
    EXPECT_TRUE(v.step);
    EXPECT_FALSE(v.early);
    members[1].has = true;
    v = patience_step_test(members, false, 3.0, false);
    EXPECT_TRUE(v.step);
    EXPECT_FALSE(v.early);
}

TEST(Patience, MemberOfMapsTheMeterLikeTheRunner) {
    // No meter yet: a member added but not ready, a late assignment.
    auto m = patience_member_of(false, true, true, nullptr, 100);
    EXPECT_FALSE(m.has);
    EXPECT_TRUE(m.settled);
    EXPECT_TRUE(m.waived);
    EXPECT_TRUE(m.late_assignment);
    EXPECT_FALSE(m.has_statistics);

    lateness_meter meter;
    meter.reset(100);                        // assigned the start base
    m = patience_member_of(true, true, false, &meter, 100);
    EXPECT_TRUE(m.has);
    EXPECT_FALSE(m.late_assignment);
    EXPECT_FALSE(m.has_statistics);
    meter.reset(160);                        // a late join or a resync: a later base
    m = patience_member_of(false, false, false, &meter, 100);
    EXPECT_TRUE(m.late_assignment);
    meter.adopt_reference(6.0, 4.5);         // a reference, adopted or measured
    m = patience_member_of(false, false, false, &meter, 100);
    EXPECT_TRUE(m.has_statistics);
    EXPECT_DOUBLE_EQ(m.r01, 4.5);
}

TEST(Patience, StepTestWaitsUntilTheEarliestPatience) {
    std::vector<patience_member> members(3);
    members[0].has = members[0].settled = true;
    members[1].lead = 2.0;
    members[2].lead = 4.0;
    auto v = patience_step_test(members, false, 5.0, true);
    EXPECT_FALSE(v.step);
    ASSERT_TRUE(v.wait);
    EXPECT_DOUBLE_EQ(v.wait_ticks, 1.0);     // member 2's patience comes first
    v = patience_step_test(members, false, 4.0, true);
    EXPECT_FALSE(v.step);                    // member 1 is still inside its patience
    EXPECT_DOUBLE_EQ(v.wait_ticks, 2.0);
    v = patience_step_test(members, false, 2.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_TRUE(v.early);
    // A waived member is not waited for at all.
    members[1].waived = true;
    v = patience_step_test(members, false, 4.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_TRUE(v.early);
    // The deadline steps regardless; that is not early.
    members[1].waived = false;
    v = patience_step_test(members, true, 0.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_FALSE(v.early);
    // Everybody settled: a normal step.
    for (auto& m: members) m.settled = true;
    v = patience_step_test(members, false, 9.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_FALSE(v.early);
}

// The pacing floor: the waiver and the cursor rule exempt a member only while
// some member's real frame for T is here.  With none, every member lacking
// its frame is waited for until its own patience.
TEST(Patience, StepTestNeedsARealFrameToSkipAWaivedOrReanchoredMember) {
    // Solo, waived: without the floor this stepped at once, every tick.
    std::vector<patience_member> solo(1);
    solo[0].waived = true;
    solo[0].lead = 3.0;
    auto v = patience_step_test(solo, false, 5.0, true);
    EXPECT_FALSE(v.step);
    ASSERT_TRUE(v.wait);
    EXPECT_DOUBLE_EQ(v.wait_ticks, 2.0);
    v = patience_step_test(solo, false, 3.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_TRUE(v.early);

    // Everybody waived (a shared outage): each waits until its own patience,
    // so the tick steps at the latest of them.
    std::vector<patience_member> all(2);
    all[0].waived = all[1].waived = true;
    all[0].lead = 6.0;
    all[1].lead = 2.0;
    v = patience_step_test(all, false, 7.0, true);
    EXPECT_FALSE(v.step);
    EXPECT_DOUBLE_EQ(v.wait_ticks, 1.0);
    v = patience_step_test(all, false, 6.0, true);
    EXPECT_FALSE(v.step);
    EXPECT_DOUBLE_EQ(v.wait_ticks, 4.0);
    v = patience_step_test(all, false, 2.0, true);
    EXPECT_TRUE(v.step);

    // A waived member beside one just re-anchored (settled by its cursor, no
    // frame): nobody's real frame, so neither is skipped.
    std::vector<patience_member> reanchored(2);
    reanchored[0].waived = true;
    reanchored[0].lead = 4.0;
    reanchored[1].settled = true;
    reanchored[1].lead = 4.0;     // the statistics-less cap
    v = patience_step_test(reanchored, false, 6.0, true);
    EXPECT_FALSE(v.step);
    EXPECT_DOUBLE_EQ(v.wait_ticks, 2.0);
    // A third member's real frame paces it: both are skipped.
    reanchored.emplace_back();
    reanchored[2].has = reanchored[2].settled = true;
    v = patience_step_test(reanchored, false, 6.0, true);
    EXPECT_TRUE(v.step);
    EXPECT_TRUE(v.early);
    // Everybody settled by the cursor alone: waited for until patience too.
    std::vector<patience_member> cursor(1);
    cursor[0].settled = true;
    v = patience_step_test(cursor, false, 1.0, true);
    EXPECT_FALSE(v.step);
    EXPECT_FALSE(v.early);
    v = patience_step_test(cursor, true, 0.0, true);
    EXPECT_TRUE(v.step);
}

TEST(Patience, WaiverStartsAtASubstitutionAndEndsInsidePatience) {
    patience_waiver w;
    EXPECT_FALSE(w.waived());
    EXPECT_FALSE(w.sample(5.0, 2.0));          // nothing to end
    EXPECT_TRUE(w.substituted());
    EXPECT_TRUE(w.waived());
    EXPECT_FALSE(w.substituted());             // the same episode
    EXPECT_EQ(w.episodes(), 1u);
    EXPECT_FALSE(w.sample(1.5, 2.0));          // after its patience: still waived
    EXPECT_TRUE(w.waived());
    EXPECT_TRUE(w.sample(2.0, 2.0));           // inside it (the boundary counts)
    EXPECT_FALSE(w.waived());
    EXPECT_TRUE(w.substituted());
    EXPECT_EQ(w.episodes(), 2u);
}

// Steady links: every frame of every member is in before the deadline, so
// patience changes nothing about who is fresh, and the session steps when the
// structurally last member's frame is in, as before.
TEST(Patience, SteadyLinksAreNeverCutOff) {
    const std::vector<link> links{early_member(1), early_member(2), last_member_slack};
    for (const bool patience: {false, true}) {
        sim_options o;
        o.patience = patience;
        const auto r = run_sim(links, kTicks, o);
        for (size_t p = 0; p < links.size(); ++p)
            EXPECT_EQ(r.substituted(p, 8, kTicks), 0) << "patience " << patience << " player " << p;
        if (patience) {
            EXPECT_GT(r.lead[0], 7.0);
            EXPECT_GT(r.lead[2], 0.5);
            EXPECT_LT(r.lead[2], 2.0);
        }
    }
}

// A structurally last member pushed past its patience for a while is waived,
// and comes back to fresh frames with its first in-patience arrival - not
// with a fresh frame, which it would never get again: while it is waived the
// session steps on the others' schedule, ahead of its arrivals.
TEST(Patience, StructurallyLastMemberIsNeverStuckSubstituted) {
    const auto late_episode = [](uint32_t t) {
        return t >= kEpisode && t <= kEpisode + 30 ? -4.0 : last_member_slack(t);
    };
    const std::vector<link> links{early_member(1), early_member(2), late_episode};

    const auto r = run_sim(links, kTicks);
    EXPECT_GE(r.substituted(2, kEpisode, kEpisode + 31), 31);
    EXPECT_LE(r.substituted(2, kEpisode, kTicks), 31 + 15);
    EXPECT_EQ(r.substituted(2, kEpisode + 50, kTicks), 0);
    EXPECT_EQ(r.substituted(0, 8, kTicks), 0);
    EXPECT_EQ(r.substituted(1, 8, kTicks), 0);

    // The rule 2.6 rejects: a waiver that waits for a fresh frame never ends.
    sim_options at_fresh;
    at_fresh.end_waiver_at_fresh = true;
    const auto stuck = run_sim(links, kTicks, at_fresh);
    EXPECT_EQ(stuck.substituted(2, kEpisode + 50, kTicks), static_cast<int>(kTicks - kEpisode - 50));
}

// A one-second blackout of one member.  The first tick waits for it until its
// patience - the runner wakes there, not at the deadline - and with the
// waiver on the rest of the blackout steps on the others' schedule; with it
// off every tick of the blackout waits for that patience; with patience off
// every tick waits for the deadline.
TEST(Patience, BlackoutWaitsOnceAtPatienceWithTheWaiverAndEveryTickWithout) {
    const uint32_t from = kEpisode, to = kEpisode + 66;
    const auto blackout = [from, to](uint32_t t) { return t >= from && t < to ? kLost : last_member_slack(t); };
    const std::vector<link> links{early_member(1), early_member(2), blackout};

    const auto on = run_sim(links, kTicks);
    const double lead = on.lead[2];
    ASSERT_GT(lead, 0.5);
    EXPECT_NEAR(on.step_ms[from], deadline_ms(from) - lead * kTickMs, 1e-6);   // woke at its patience
    for (uint32_t t = from + 1; t < to - kRedundancy; ++t)
        EXPECT_LT(on.step_ms[t], deadline_ms(t) - (lead + 0.5) * kTickMs) << "tick " << t;
    EXPECT_EQ(on.substituted(2, to + 10, kTicks), 0);   // back once its arrivals are inside patience
    EXPECT_EQ(on.substituted(0, 8, kTicks), 0);

    sim_options no_waiver;
    no_waiver.waiver = false;
    const auto off = run_sim(links, kTicks, no_waiver);
    // The last frames of the blackout come back in the first datagram after
    // it (the redundancy window), so only the ticks before those are judged.
    for (uint32_t t = from; t < to - kRedundancy; ++t)
        EXPECT_NEAR(off.step_ms[t], deadline_ms(t) - lead * kTickMs, 1e-6) << "tick " << t;
    EXPECT_EQ(off.substituted(2, to + 10, kTicks), 0);

    sim_options deadline_only;
    deadline_only.patience = false;
    const auto today = run_sim(links, kTicks, deadline_only);
    for (uint32_t t = from; t < to - kRedundancy; ++t)
        EXPECT_NEAR(today.step_ms[t], deadline_ms(t), 1e-6) << "tick " << t;
    // Outside the blackout the deadline rule steps when the last frame is in,
    // which is where patience steps too.
    for (uint32_t t = 400; t < from; ++t)
        EXPECT_NEAR(today.step_ms[t], on.step_ms[t], 1e-6) << "tick " << t;
}

// At the session's start nobody has statistics, so patience is the deadline:
// a member missing then is waited for until the deadline, as before.
TEST(Patience, WithoutStatisticsTheFirstMissWaitsForTheDeadline) {
    const auto early_gap = [](uint32_t t) { return t >= 100 && t < 110 ? kLost : last_member_slack(t); };
    const std::vector<link> links{early_member(1), early_member(2), early_gap};
    const auto r = run_sim(links, 300);
    EXPECT_NEAR(r.step_ms[100], deadline_ms(100), 1e-6);
}

// A member assigned after the start waits, while it has no statistics, as
// long as the latest member that has them - the others' schedule - and not
// until the deadline: the structurally last member, joining late, is cut off
// (and waived) until its own reference is set, then never again.
TEST(Patience, LateAssignedMemberIsCappedAtTheOthersScheduleUntilItsReference) {
    const std::vector<link> links{early_member(1), early_member(2), last_member_slack};
    sim_options late;
    late.late_member = 2;
    const auto r = run_sim(links, kTicks, late);
    EXPECT_GT(r.substituted(2, 8, 400), 0);
    EXPECT_EQ(r.substituted(2, kEpisode, kTicks), 0);
    EXPECT_EQ(r.substituted(0, 8, kTicks), 0);
    EXPECT_EQ(r.substituted(1, 8, kTicks), 0);
}

// A solo session through a one-second blackout: its only member is waived
// after the first substitution, but nobody else's frame paces the session, so
// every tick of the blackout waits for that member's patience - not one steps
// earlier (without the floor the session ran away, max_catch_up_ticks per
// wake-up) - and the member is fresh again once its link is back.
TEST(Patience, SoloBlackoutNeverStepsBeforePatience) {
    const uint32_t from = kEpisode, to = kEpisode + 66;
    const auto blackout = [from, to](uint32_t t) { return t >= from && t < to ? kLost : last_member_slack(t); };
    const auto r = run_sim({blackout}, kTicks);
    const double lead = r.lead[0];
    ASSERT_GT(lead, 0.5);
    // Never ahead of the member's own frames (slack at most 4 ticks).
    for (uint32_t t = 0; t < kTicks; ++t)
        EXPECT_GE(r.step_ms[t], deadline_ms(t) - 4.0 * kTickMs - 1e-6) << "tick " << t;
    for (uint32_t t = from; t < to - kRedundancy; ++t)
        EXPECT_NEAR(r.step_ms[t], deadline_ms(t) - lead * kTickMs, 1e-6) << "tick " << t;
    EXPECT_EQ(r.substituted(0, to + 10, kTicks), 0);
}

// Every member blacked out together (a hiccup at the server's uplink): all
// are waived, nobody paces the session, so each tick of the outage steps at
// the latest of their patiences - never before - and nobody stays substituted.
TEST(Patience, SharedOutageStepsAtTheLatestPatience) {
    const uint32_t from = kEpisode, to = kEpisode + 66;
    const auto out = [from, to](link l) {
        return [l, from, to](uint32_t t) { return t >= from && t < to ? kLost : l(t); };
    };
    const std::vector<link> links{out(early_member(1)), out(early_member(2)), out(last_member_slack)};
    const auto r = run_sim(links, kTicks);
    const double latest = *std::min_element(r.lead.begin(), r.lead.end());
    ASSERT_GT(latest, 0.5);
    for (uint32_t t = from; t < to - kRedundancy; ++t)
        EXPECT_NEAR(r.step_ms[t], deadline_ms(t) - latest * kTickMs, 1e-6) << "tick " << t;
    // Never ahead of the earliest member's frames (slack at most 11 ticks).
    for (uint32_t t = 0; t < kTicks; ++t)
        EXPECT_GE(r.step_ms[t], deadline_ms(t) - 11.0 * kTickMs - 1e-6) << "tick " << t;
    for (size_t p = 0; p < links.size(); ++p)
        EXPECT_EQ(r.substituted(p, to + 10, kTicks), 0) << "player " << p;
}
