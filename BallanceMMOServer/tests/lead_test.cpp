// Unit tests for the server's half of the per-player lead (late-player
// design 2.2, 2.3, 2.5; plan step 10): input_buffer::reset_hold, the
// lead_decide / lead_acknowledge gates of session/lead.hpp, E's arithmetic,
// the caps, the deadline reference for a member nobody can lend a reference
// to, and the late-tick leads (the start base unchanged for 0..1000 ms).
//
// The runner's reset is driven the way the runner drives it, by a small
// event simulation in the style of patience_test.cpp: one uplink datagram per
// tick and player carrying its last 8 frames, the runner's step test after
// every arrival and at every deadline, the decision taken after a step, and
// every applied frame written to a real server journal and read back.
#include <session/start_handshake.hpp>
#include <gtest/gtest.h>

#include <session/journal.hpp>
#include <session/lateness.hpp>
#include <session/lead.hpp>
#include <session/rollback.hpp>
#include <session/timeline.hpp>

#include "../sim/late_tick.hpp"
#include "../sim/assign_copies.hpp"
#include <entity/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

using namespace bmmo::session;

namespace {
    constexpr uint32_t kInputDelay = 16;
    constexpr uint32_t kRedundancy = 8;   // frames per uplink datagram

    double deadline_ms(uint32_t t) { return (t + kInputDelay) * TICK_MS; }

    // Player p's frame for tick t: distinct per player and tick, so a test can
    // tell which tick's frame was applied.
    input_frame frame_of(size_t player, uint32_t t) {
        input_frame f;
        f.keys = static_cast<uint8_t>((t * 7 + player * 3) & 0xFF);
        f.cam_right[0] = static_cast<float>(t);
        f.cam_up[1] = static_cast<float>(player + 1);
        f.cam_dir[2] = static_cast<float>(t) * 0.5f;
        f.ball_type = static_cast<uint8_t>(player);
        return f;
    }

    bool same_frame(const input_frame& a, const input_frame& b) {
        if (a.keys != b.keys || a.ball_type != b.ball_type || a.flags != b.flags) return false;
        for (int k = 0; k < 3; ++k)
            if (a.cam_right[k] != b.cam_right[k] || a.cam_up[k] != b.cam_up[k] || a.cam_dir[k] != b.cam_dir[k])
                return false;
        return true;
    }

    // The slack (ticks before its deadline) the datagram whose newest frame
    // is t arrives with.
    using link = std::function<double(uint32_t)>;

    struct applied_record {
        input_frame frame;
        bool fresh = false;
        uint32_t source = 0;
    };

    struct sim_options {
        bool patience = false;            // runner_config::patience
        uint32_t decide_after = 0;        // the decision is taken after stepping this tick (0 = never)
        size_t late_player = 1;           // whose decision it is
        int k = 5;                        // the raise
        uint32_t rtt_ms = 300;
        std::filesystem::path journal;    // empty = none
    };

    struct sim_result {
        std::vector<double> step_ms;                          // when each tick was stepped
        std::vector<std::vector<applied_record>> applied;     // [player][tick]
        lead_plan plan;
        uint32_t decided_at = 0;   // N
    };

    // Each datagram is (arrival, player, newest tick).
    sim_result run_sim(const std::vector<link>& links, uint32_t ticks, const sim_options& o) {
        const size_t n = links.size();
        struct datagram { double at; size_t player; uint32_t newest; };
        std::vector<datagram> datagrams;
        for (size_t p = 0; p < n; ++p)
            for (uint32_t t = 0; t < ticks + 40; ++t) {
                const double slack = links[p](t);
                if (!std::isnan(slack)) datagrams.push_back({deadline_ms(t) - slack * TICK_MS, p, t});
            }
        std::stable_sort(datagrams.begin(), datagrams.end(),
                         [](const datagram& a, const datagram& b) { return a.at < b.at; });

        std::vector<input_buffer> buffers(n);
        std::vector<relative_meter> meters(n);
        std::vector<arrival_ring> arrivals(n);
        std::vector<lateness_baseline> baselines(n);
        std::vector<lead_player> leads(n);
        for (size_t p = 0; p < n; ++p) {
            buffers[p].reset(0);
            meters[p].reset(0);
            baselines[p].assigned(0, true, 0);
            baselines[p].settled(0, {});
            leads[p].cap_lead = true;
            leads[p].assigned_lead = 20;
            leads[p].rtt_ms = o.rtt_ms;
            leads[p].have_rtt = true;
        }
        journal_writer writer;
        if (!o.journal.empty()) {
            journal_header h;
            h.kind = journal_kind::server;
            h.session = 1;
            h.input_delay = kInputDelay;
            h.first_tick = 0;
            std::string error;
            EXPECT_TRUE(writer.open(o.journal, h, 0, error)) << error;
            for (size_t p = 0; p < n; ++p) writer.player(0, static_cast<uint32_t>(p + 1), static_cast<uint8_t>(p), true, "");
        }

        sim_result r;
        r.step_ms.assign(ticks, 0.0);
        r.applied.assign(n, std::vector<applied_record>(ticks));

        // What the runner's patience_members() builds.
        const auto members_at = [&](uint32_t tick) {
            std::vector<patience_member> members;
            for (size_t p = 0; p < n; ++p) {
                if (!o.patience) {
                    patience_member m;
                    m.has = buffers[p].has(tick);
                    m.held = buffers[p].held(tick) || buffers[p].reanchored(tick);
                    members.push_back(m);
                    continue;
                }
                patience_member member;
                member.has = buffers[p].has(tick);
                member.settled = buffers[p].settled(tick);
                member.held = buffers[p].held(tick) || buffers[p].reanchored(tick);
                member.baseline = baselines[p].value();
                member.counted = baselines[p].frozen() && !member.held && !(member.settled && !member.has);
                if (const auto* arrival = arrivals[p].find(tick)) {
                    const auto deadline = arrival_ring::time_point{} + std::chrono::duration_cast<arrival_ring::time_point::duration>(
                        std::chrono::duration<double, std::milli>(deadline_ms(tick)));
                    member.ns = member.baseline - lateness_slack_ticks(deadline, arrival->seen);
                }
                members.push_back(member);
            }
            return members;
        };

        uint32_t tick = 0;
        double now = 0.0;
        size_t next = 0;
        while (tick < ticks) {
            double wake = std::numeric_limits<double>::infinity();
            while (tick < ticks) {
                const auto members = members_at(tick);
                const auto v = patience_step_test(members, now >= deadline_ms(tick) - 1e-9,
                                                  (deadline_ms(tick) - now) / TICK_MS, o.patience);
                if (!v.step) {
                    wake = deadline_ms(tick);
                    if (v.wait) wake = std::min(wake, now + v.wait_ticks * TICK_MS);
                    break;
                }
                r.step_ms[tick] = now;
                for (size_t p = 0; p < n; ++p) {
                    bool fresh = false;
                    uint32_t source = 0;
                    const auto& frame = buffers[p].take(tick, fresh, source);
                    r.applied[p][tick] = {frame, fresh, source};
                    if (writer.is_open())
                        writer.input(tick, static_cast<uint32_t>(p + 1), frame, fresh ? JOURNAL_INPUT_FRESH : 0);
                }
                ++tick;
                if (o.decide_after != 0 && tick - 1 == o.decide_after) {
                    lateness_decision d;
                    d.action = lateness_action::raise;
                    d.k = o.k;
                    d.id = 1;
                    r.decided_at = tick;
                    r.plan = lead_decide(true, leads[o.late_player], meters[o.late_player], d, tick,
                                         buffers[o.late_player]);
                    if (writer.is_open())
                        writer.note(tick, "lead: p" + std::to_string(o.late_player + 1) + " +"
                                    + std::to_string(r.plan.k) + " at " + std::to_string(tick) + ", effective "
                                    + std::to_string(r.plan.effective) + "; buffer reset dropped "
                                    + std::to_string(r.plan.dropped) + " frames");
                }
            }
            if (tick >= ticks) break;
            if (next < datagrams.size() && datagrams[next].at < wake) {
                const auto& d = datagrams[next++];
                now = std::max(now, d.at);
                const uint32_t first = d.newest >= kRedundancy - 1 ? d.newest - (kRedundancy - 1) : 0;
                std::vector<input_frame> frames;
                for (uint32_t t = first; t <= d.newest; ++t) frames.push_back(frame_of(d.player, t));
                buffers[d.player].submit(first, frames);
                const double slack = (deadline_ms(d.newest) - d.at) / TICK_MS;
                const auto arrival = arrival_ring::time_point{} + std::chrono::duration_cast<arrival_ring::time_point::duration>(
                    std::chrono::duration<double, std::milli>(d.at));
                baselines[d.player].sample(d.newest, slack, arrival);
                for (uint32_t frame_tick = first; frame_tick <= d.newest; ++frame_tick)
                    arrivals[d.player].record(frame_tick, arrival, arrival, frame_tick == d.newest);
                if (meters[d.player].sample_absolute(d.newest, slack, baselines[d.player].value())) leads[d.player].received(d.newest, tick);
            } else {
                now = std::max(now, wake);
            }
        }
        writer.close();
        return r;
    }

    struct temp_file {
        std::filesystem::path path;
        explicit temp_file(const std::string& name)
            : path(std::filesystem::temp_directory_path() / ("bmmo_lead_" + name + ".bmjr")) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        ~temp_file() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };
}

// ---------------------------------------------------------------------------
// input_buffer::reset_hold
// ---------------------------------------------------------------------------

TEST(LeadReset, HoldsOneFrameForTheRangeAndKeepsWhatComesAfter) {
    input_buffer buffer;
    buffer.reset(100);
    const std::vector<input_frame> early{frame_of(0, 100), frame_of(0, 101)};
    buffer.submit(100, early);
    bool fresh = false;
    uint32_t source = 0;
    buffer.take(100, fresh, source);
    buffer.take(101, fresh, source);
    ASSERT_TRUE(fresh);
    // Queued frames below E and one at E.
    buffer.submit(103, std::vector<input_frame>{frame_of(0, 103), frame_of(0, 104)});
    buffer.submit(110, std::vector<input_frame>{frame_of(0, 110)});
    const uint64_t stale_before = buffer.stale();
    EXPECT_EQ(buffer.reset_hold(110), 2);
    EXPECT_EQ(buffer.stale(), stale_before + 2);
    EXPECT_EQ(buffer.hold_from(), 102u);
    EXPECT_EQ(buffer.hold_until(), 110u);
    EXPECT_EQ(buffer.next_tick(), 110u);
    for (uint32_t t = 102; t < 110; ++t) {
        EXPECT_TRUE(buffer.held(t)) << t;
        EXPECT_TRUE(buffer.settled(t)) << t;
        EXPECT_FALSE(buffer.has(t)) << t;
    }
    EXPECT_FALSE(buffer.held(101));
    EXPECT_FALSE(buffer.held(110));
    EXPECT_TRUE(buffer.has(110));

    // Stragglers below E arrive while the range is stepped: stale, and the
    // substitute stays the frame of 101 for every tick of it.
    for (uint32_t t = 102; t < 110; ++t) {
        buffer.submit(t, std::vector<input_frame>{frame_of(0, t)});
        const auto& f = buffer.take(t, fresh, source);
        EXPECT_FALSE(fresh) << t;
        EXPECT_EQ(source, 101u) << t;
        EXPECT_TRUE(same_frame(f, frame_of(0, 101))) << t;
    }
    // E is fresh: the frame kept through the reset.
    const auto& at_e = buffer.take(110, fresh, source);
    EXPECT_TRUE(fresh);
    EXPECT_TRUE(same_frame(at_e, frame_of(0, 110)));
    // A straggler below E arriving after E never becomes a later substitute.
    buffer.submit(109, std::vector<input_frame>{frame_of(0, 109)});
    const auto& next = buffer.take(111, fresh, source);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(source, 110u);
    EXPECT_TRUE(same_frame(next, frame_of(0, 110)));
}

TEST(LeadReset, AnEAtOrBehindTheCursorHoldsNothing) {
    input_buffer buffer;
    buffer.reset(200);   // re-anchored ahead of the session
    EXPECT_EQ(buffer.reset_hold(150), -1);
    EXPECT_EQ(buffer.reset_hold(200), -1);
    EXPECT_EQ(buffer.next_tick(), 200u);
    EXPECT_FALSE(buffer.held(199));
}

// ---------------------------------------------------------------------------
// The runner's reset mid-run: a constant held range, no deadline wait, and a
// journal that replays exactly what was applied.
// ---------------------------------------------------------------------------

namespace {
    // Player 0 is on time (6.5 ticks early), player 1 structurally late (4.5
    // ticks after the deadline): every tick is stepped at its deadline and
    // player 1 is substituted at every one of them.  Half ticks, so no
    // arrival ties with a deadline.
    std::vector<link> late_pair() {
        return {[](uint32_t) { return 6.5; }, [](uint32_t) { return -4.5; }};
    }

    void expect_held_range(const sim_result& r, const sim_options& o, uint32_t ticks) {
        const auto& plan = r.plan;
        ASSERT_EQ(plan.route, lead_route::send);
        const uint32_t n = r.decided_at;
        EXPECT_EQ(plan.held_from, n);
        // c_est = newest + (N - n_arr) + ceil(rtt): player 1's newest frame t
        // arrives at deadline(t) + 4.5 ticks, when the session (on deadlines)
        // has stepped t + 4, so its next tick n_arr is t + 5 and c_est is
        // N - 5 + the round trip.
        EXPECT_TRUE(plan.have_client_tick);
        EXPECT_EQ(plan.effective, static_cast<uint32_t>(plan.client_tick + 2 * o.k + 2));
        EXPECT_EQ(plan.client_tick, static_cast<int64_t>(n) - 5 + lead_rtt_ticks(o.rtt_ms));
        ASSERT_GT(plan.effective, n);
        ASSERT_LT(plan.effective, ticks);
        // The substitute held for the range is the newest frame of player 1
        // that had arrived at the decision, and the message carries it.
        EXPECT_EQ(plan.held_source, r.applied[1][n - 1].source);
        EXPECT_TRUE(same_frame(plan.held, r.applied[1][n - 1].frame));
        for (uint32_t t = n; t < plan.effective; ++t) {
            const auto& a = r.applied[1][t];
            EXPECT_FALSE(a.fresh) << t;
            EXPECT_EQ(a.source, plan.held_source) << t;
            EXPECT_TRUE(same_frame(a.frame, plan.held)) << "tick " << t << ": the held range is one frame";
            // No deadline wait: stepped as soon as player 0's frame was in -
            // at the decision for the frames already queued then, at their
            // arrival after that.
            EXPECT_NEAR(r.step_ms[t], std::max(r.step_ms[n - 1], deadline_ms(t) - 6.5 * TICK_MS), 1e-6) << t;
            EXPECT_LE(r.step_ms[t], deadline_ms(t) - TICK_MS) << t;
        }
        // Before N the late player kept the session on deadlines.
        for (uint32_t t = n - 20; t < n; ++t) EXPECT_NEAR(r.step_ms[t], deadline_ms(t), 1e-6) << t;
        // E itself: player 1 is still late (this client never slewed), so the
        // session is back on its deadline and never ahead of it after.
        EXPECT_NEAR(r.step_ms[plan.effective], deadline_ms(plan.effective), 1e-6);
        for (uint32_t t = plan.effective; t < ticks; ++t) EXPECT_GE(r.step_ms[t], deadline_ms(t) - 1e-6) << t;
        // Player 0 is fresh throughout: the reset is one buffer's.
        for (uint32_t t = 20; t < ticks; ++t) EXPECT_TRUE(r.applied[0][t].fresh) << t;
    }
}

TEST(LeadReset, MidRunRangeIsHeldConstantWithoutWaitingAndTheJournalReplaysIt) {
    temp_file file("midrun");
    sim_options o;
    o.decide_after = 300;
    o.journal = file.path;
    constexpr uint32_t kTicks = 600;
    const auto r = run_sim(late_pair(), kTicks, o);
    expect_held_range(r, o, kTicks);

    // The journal: every input record is the frame the step applied, with
    // the held range journaled flag 0 like any substitute, so a replay that
    // only feeds the records back reproduces the server's inputs - it never
    // needs to know about the reset (the note is all it carries).
    journal j;
    std::string error;
    ASSERT_TRUE(read_journal(file.path, j, error)) << error;
    size_t inputs = 0;
    bool noted = false;
    for (const auto& note: j.notes) noted |= note.text.rfind("lead: p2 +5 at 301, effective ", 0) == 0;
    EXPECT_TRUE(noted);
    for (const auto& group: j.ticks) {
        for (const auto& input: group.inputs) {
            ++inputs;
            const size_t p = input.id - 1;
            ASSERT_LT(p, 2u);
            ASSERT_LT(group.tick, kTicks);
            const auto& a = r.applied[p][group.tick];
            EXPECT_TRUE(same_frame(input.frame, a.frame)) << "tick " << group.tick << " player " << p;
            EXPECT_EQ(input.flags, a.fresh ? JOURNAL_INPUT_FRESH : 0) << group.tick;
            if (p == 1 && group.tick >= r.plan.held_from && group.tick < r.plan.effective) {
                EXPECT_EQ(input.flags, 0);
                EXPECT_TRUE(same_frame(input.frame, r.plan.held));
            }
        }
    }
    EXPECT_EQ(inputs, 2u * kTicks);
}

TEST(LeadReset, MidRunRangeWithPatienceOn) {
    // With patience on the held range is settled by the cursor, which the
    // pacing floor exempts while player 0's frame is there: the same range,
    // the same frame, no wait.  Nobody has statistics this early, so outside
    // the range the session is on deadlines as before.
    sim_options o;
    o.patience = true;
    o.decide_after = 300;
    constexpr uint32_t kTicks = 600;
    const auto r = run_sim(late_pair(), kTicks, o);
    expect_held_range(r, o, kTicks);
}

TEST(LeadReset, SoloHeldRangeStillStepsOnTheDeadline) {
    // The pacing floor: with nobody's real frame there, a held member does
    // not let the session run ahead; it steps at the deadline as before.
    sim_options o;
    o.decide_after = 300;
    o.late_player = 0;
    constexpr uint32_t kTicks = 500;
    const auto r = run_sim({[](uint32_t) { return -4.5; }}, kTicks, o);
    ASSERT_EQ(r.plan.route, lead_route::send);
    for (uint32_t t = r.plan.held_from; t < r.plan.effective; ++t) {
        EXPECT_NEAR(r.step_ms[t], deadline_ms(t), 1e-6) << t;
        EXPECT_TRUE(same_frame(r.applied[0][t].frame, r.plan.held)) << t;
    }
}

TEST(LeadReset, WithoutAResetTheStepTestIsTheDeadlineRule) {
    // The flag-off identity: no reset, no held member, and the step test is
    // has-or-deadline, the late player keeping the session on deadlines.
    sim_options o;
    constexpr uint32_t kTicks = 400;
    const auto r = run_sim(late_pair(), kTicks, o);
    for (uint32_t t = 0; t < kTicks; ++t) {
        EXPECT_NEAR(r.step_ms[t], deadline_ms(t), 1e-6) << t;
        EXPECT_FALSE(r.applied[1][t].fresh) << t;
    }
    std::vector<patience_member> members(2);
    members[0].has = true;
    members[1].settled = true;   // settled alone is no exemption without patience: the runner sets held
    EXPECT_FALSE(patience_step_test(members, false, 3.0, false).step);
    members[1].held = true;
    EXPECT_TRUE(patience_step_test(members, false, 3.0, false).step);
    members[0].has = false;      // the pacing floor
    EXPECT_FALSE(patience_step_test(members, false, 3.0, false).step);
    EXPECT_TRUE(patience_step_test(members, true, 0.0, false).step);
}

// Plan step 20: the ticks a resync's (or a late joiner's) base skips are
// treated like a held range by the step test without patience.  Waiting for
// their deadlines put the whole session 22 ticks behind for the 31 ticks
// between the server's tick and the base in lp19b_a6_1500.
TEST(LeadReset, AReanchorsSkippedTicksDoNotWaitForTheirDeadlines) {
    input_buffer paced, resynced;
    paced.reset(100);
    resynced.reset(100);
    std::vector<input_frame> frames;
    for (uint32_t t = 100; t < 140; ++t) frames.push_back(frame_of(0, t));
    paced.submit(100, frames);
    bool fresh = false;
    uint32_t source = 0;
    resynced.submit(100, std::vector<input_frame>{frame_of(1, 100)});
    resynced.take(100, fresh, source);
    paced.take(100, fresh, source);
    // The resync: the server is at 101, the base 131.
    resynced.reset(131);
    EXPECT_FALSE(resynced.reanchored(100));   // taken
    for (uint32_t t = 101; t < 131; ++t) {
        EXPECT_TRUE(resynced.reanchored(t)) << t;
        EXPECT_FALSE(resynced.held(t)) << t;
        EXPECT_TRUE(resynced.settled(t)) << t;
    }
    EXPECT_FALSE(resynced.reanchored(131));
    const auto members_at = [&](uint32_t t) {
        std::vector<patience_member> members(2);
        members[0].has = paced.has(t);
        members[0].held = paced.held(t) || paced.reanchored(t);
        members[1].has = resynced.has(t);
        members[1].held = resynced.held(t) || resynced.reanchored(t);
        return members;
    };
    for (uint32_t t = 101; t < 131; ++t) {
        EXPECT_TRUE(patience_step_test(members_at(t), false, 20.0, false).step) << t;
        paced.take(t, fresh, source);
        resynced.take(t, fresh, source);
        EXPECT_FALSE(fresh) << t;
        EXPECT_EQ(source, 100u) << t;   // the substitute, as on a deadline
        EXPECT_FALSE(resynced.reanchored(t)) << t;   // taken now
    }
    // From the base the resynced member's own frame is waited for again.
    EXPECT_FALSE(patience_step_test(members_at(131), false, 20.0, false).step);
    EXPECT_TRUE(patience_step_test(members_at(131), true, 0.0, false).step);
    // The pacing floor: with nobody's real frame there, the skipped ticks
    // wait for the deadline like any other.
    input_buffer alone;
    alone.reset(100);
    alone.reset(120);
    std::vector<patience_member> solo(1);
    solo[0].held = alone.held(110) || alone.reanchored(110);
    EXPECT_TRUE(solo[0].held);
    EXPECT_FALSE(patience_step_test(solo, false, 20.0, false).step);
    EXPECT_TRUE(patience_step_test(solo, true, 0.0, false).step);
    // A lead reset's held range is held, not reanchored.
    input_buffer hold;
    hold.reset(100);
    hold.take(100, fresh, source);
    ASSERT_EQ(hold.reset_hold(110), 0);
    EXPECT_TRUE(hold.held(105));
    EXPECT_FALSE(hold.reanchored(105));
}

// ---------------------------------------------------------------------------
// The gates: no SessionLead without CAP_LEAD or with the flag off, never two
// unacknowledged, at most two raises that do not restore, the caps.
// ---------------------------------------------------------------------------

namespace {
    // Arrivals against an on-time peer, evaluated at each stepped tick.
    struct meter_rig {
        relative_meter meter;
        input_buffer buffer;
        lead_player lead;
        lateness_baseline baseline;
        arrival_ring arrivals;
        uint32_t tick = 100;

        meter_rig() {
            meter.reset(100);
            buffer.reset(100);
            lead.cap_lead = true;
            lead.assigned_lead = 20;
            lead.rtt_ms = 150;
            lead.have_rtt = true;
            baseline.assigned(100, true, 100);
            baseline.settled(100, arrival_ring::time_point{});
            feed(0.0, 400);
            EXPECT_TRUE(baseline.frozen());
        }
        // Feeds `ticks` ticks of samples at `slack`, calling `on` for every
        // decision; returns how many there were.
        int feed(double delay, uint32_t ticks,
                 const std::function<void(const lateness_decision&)>& on = {}) {
            int decisions = 0;
            for (uint32_t i = 0; i < ticks; ++i, ++tick) {
                const auto deadline = arrival_ring::time_point{} + std::chrono::microseconds(
                    static_cast<int64_t>(tick * TICK_MICROS));
                const auto at = deadline - std::chrono::microseconds(
                    static_cast<int64_t>((6.0 - delay) * TICK_MICROS));
                arrivals.record(tick, at, at, true);
                const double slack = lateness_slack_ticks(deadline, arrivals.find(tick)->at);
                baseline.sample(tick, slack, at);
                if (baseline.frozen() && meter.sample_absolute(tick, slack, baseline.value())) {
                    const double normalized = baseline.value() - slack;
                    meter.sample_relative(tick, normalized, normalized);
                    lead.received(tick, tick + 3);
                }
                bool fresh = false;
                buffer.submit(tick, std::vector<input_frame>{frame_of(0, tick)});
                buffer.take(tick, fresh);
                const auto d = meter.on_tick(tick, false);
                if (d.action == lateness_action::none) continue;
                ++decisions;
                if (on) on(d);
            }
            return decisions;
        }
    };
}

TEST(LeadDecide, NoCapLeadMeansOneObservedDecisionAndNothingSent) {
    meter_rig rig;
    rig.lead.cap_lead = false;
    const uint32_t cursor_before = rig.buffer.next_tick();
    std::vector<lead_plan> plans;
    const int decisions = rig.feed(9.0, 2000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(decisions, 1);
    EXPECT_EQ(plans[0].route, lead_route::observe);
    EXPECT_EQ(rig.lead.observed, 1u);
    EXPECT_EQ(rig.lead.sent, 0u);
    EXPECT_FALSE(rig.lead.outstanding);
    // Pending for good: the one decision of this assignment.
    EXPECT_EQ(rig.meter.state(), lateness_state::pending);
    EXPECT_EQ(rig.buffer.hold_until(), 0u);
    EXPECT_GT(rig.buffer.next_tick(), cursor_before);
}

TEST(LeadDecide, FlagOffObservesEvenWithCapLead) {
    meter_rig rig;
    std::vector<lead_plan> plans;
    rig.feed(9.0, 2000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(false, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans[0].route, lead_route::observe);
    EXPECT_EQ(rig.lead.sent, 0u);
    EXPECT_EQ(rig.buffer.hold_until(), 0u);
}

TEST(LeadDecide, NoSecondDecisionWhileOneIsUnacknowledged) {
    meter_rig rig;
    std::vector<lead_plan> plans;
    const int decisions = rig.feed(9.0, 3000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(decisions, 1);
    ASSERT_EQ(plans[0].route, lead_route::send);
    EXPECT_TRUE(rig.lead.outstanding);
    EXPECT_EQ(rig.lead.id, rig.meter.pending_id());
    EXPECT_GT(plans[0].k, 0);
    EXPECT_GT(plans[0].effective, plans[0].held_from);
    EXPECT_EQ(plans[0].held_from, rig.buffer.hold_from());
    // An ack of something else changes nothing.
    EXPECT_FALSE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id + 1, plans[0].k));
    EXPECT_TRUE(rig.lead.outstanding);
    // The ack: the offset moves, the cooldown is measured from E.
    EXPECT_TRUE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plans[0].k));
    EXPECT_FALSE(rig.lead.outstanding);
    EXPECT_EQ(rig.meter.offset(), plans[0].k);
    EXPECT_EQ(rig.meter.cooldown_until(),
              plans[0].effective + static_cast<uint32_t>(plans[0].k) * 2 + relative_meter::kCooldownTicks);
    EXPECT_FALSE(lead_acknowledge(rig.lead, rig.meter, plans[0].k, plans[0].k));
}

TEST(LeadDecide, TwoRaisesThatDoNotRestoreThenNoMore) {
    meter_rig rig;
    std::vector<lead_plan> plans;
    std::vector<lateness_action> actions;
    rig.feed(9.0, 8000, [&](const lateness_decision& d) {
        actions.push_back(d.action);
        const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
        plans.push_back(plan);
        // The client acks every raise at once, but its arrivals never
        // improve (it cannot really apply it).
        if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
    });
    ASSERT_EQ(actions.size(), 3u);
    EXPECT_EQ(actions[0], lateness_action::raise);
    EXPECT_EQ(actions[1], lateness_action::raise);
    EXPECT_EQ(actions[2], lateness_action::give_up);
    EXPECT_EQ(rig.lead.sent, 2u);
    EXPECT_EQ(rig.meter.state(), lateness_state::given_up);
}

TEST(LeadDecide, TheTotalCapCutsARaiseAndStopsTheNext) {
    meter_rig rig;
    rig.lead.assigned_lead = kLeadCapTicks - 3;   // room for 3
    std::vector<lead_plan> plans;
    const int decisions = rig.feed(12.0, 4000, [&](const lateness_decision& d) {
        const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
        plans.push_back(plan);
        if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
    });
    ASSERT_EQ(decisions, 1);
    ASSERT_EQ(plans[0].route, lead_route::send);
    EXPECT_GT(plans[0].decided_k, 3);
    EXPECT_EQ(plans[0].k, 3);
    ASSERT_NE(plans[0].cap, nullptr);
    EXPECT_STREQ(plans[0].cap, "total");
    EXPECT_EQ(rig.lead.capped, 1u);
    // E is sized with the capped k: the slew the client will run.
    ASSERT_TRUE(plans[0].have_client_tick);
    EXPECT_EQ(plans[0].effective, static_cast<uint32_t>(plans[0].client_tick + 2 * 3 + 2));
    // At the cap: no further raise, however late it stays.
    EXPECT_TRUE(rig.meter.at_cap());
    EXPECT_NE(rig.meter.state(), lateness_state::given_up);
}

TEST(LeadDecide, ARaiseWithNoRoomIsDeclined) {
    meter_rig rig;
    rig.lead.assigned_lead = kLeadCapTicks;
    std::vector<lead_plan> plans;
    const int decisions = rig.feed(9.0, 4000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(decisions, 1);
    EXPECT_EQ(plans[0].route, lead_route::declined);
    EXPECT_EQ(rig.lead.declined, 1u);
    EXPECT_EQ(rig.lead.sent, 0u);
    EXPECT_EQ(rig.buffer.hold_until(), 0u);   // nothing reset
    EXPECT_TRUE(rig.meter.at_cap());
    EXPECT_EQ(rig.meter.pending_id(), 0u);
}

TEST(LeadDecide, TheSectorMarginCapsARaise) {
    meter_rig rig;
    // The others' Sector events reach this client 4 ticks before their
    // stamp at worst: a raise may take 3 of them.
    rig.lead.sector_margins[2].push(9, rig.tick);
    rig.lead.sector_margins[2].push(4, rig.tick);
    rig.lead.sector_margins[2].push(12, rig.tick);
    std::vector<lead_plan> plans;
    rig.feed(12.0, 4000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_EQ(plans[0].route, lead_route::send);
    EXPECT_EQ(plans[0].k, 3);
    EXPECT_STREQ(plans[0].cap, "sector");
}

// Resync step B2 (late-player plan step 14): a client that rewinds a late
// remote Sector (CAP_RESYNC_STATE, the session core with the rollback on) is
// bounded by its rewind reach, not by the margin alone.  The margin 4 that
// caps the client above at 3 leaves this one the whole raise.
TEST(LeadDecide, ASectorRewindingClientIsNotCappedByTheMargin) {
    meter_rig rig;
    rig.lead.rewinds_sectors = true;
    rig.lead.sector_margins[2].push(9, rig.tick);
    rig.lead.sector_margins[2].push(4, rig.tick);
    rig.lead.sector_margins[2].push(12, rig.tick);
    std::vector<lead_plan> plans;
    rig.feed(12.0, 4000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_EQ(plans[0].route, lead_route::send);
    EXPECT_GT(plans[0].decided_k, 3);
    EXPECT_EQ(plans[0].k, plans[0].decided_k);
    EXPECT_EQ(plans[0].cap, nullptr);
    EXPECT_EQ(rig.lead.capped, 0u);
}

// ... but the reach still binds: a raise that would make another member's
// Sector event reach the client more than the reach late (a rewind to
// stamp - 1 deeper than max_resim_ticks, a resync instead) is cut, not
// latched, and one the reach leaves nothing of is deferred.
TEST(LeadDecide, TheRewindReachBoundsARaise) {
    meter_rig rig;
    rig.lead.rewinds_sectors = true;
    rig.lead.sector_margins[2].push(4, rig.tick);
    lateness_decision decision;
    decision.action = lateness_action::raise;
    decision.k = 55;
    const auto plan = lead_decide(true, rig.lead, rig.meter, decision, rig.tick, rig.buffer);
    EXPECT_EQ(plan.route, lead_route::send);
    EXPECT_EQ(plan.k, 51);
    EXPECT_STREQ(plan.binding, "sector");
    EXPECT_FALSE(rig.meter.at_cap());
}

TEST(LeadSector, TheBoundIsTheMarginPlusTheClientsRewindReach) {
    lead_player player;
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 2), 0);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 1), 20);
    player.sector_margins[2].push(15, 500);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 2), 14);
    player.rewinds_sectors = true;
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 2), 62);
    player.sector_margins[3].push(9, 500);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 3), 56);
    player.rewinds_sectors = false;
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 3), 8);
    player.sector_margins[3].push(0, 500);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 3), 14);
    player.sector_margins[2].push(-60, 500);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 3), 20);
    EXPECT_EQ(lead_sector_bound(player, 20, 500, 3, true), 20);
    EXPECT_EQ(lead_sector_bound(player, 20, 1188, 3), 0);
    EXPECT_EQ(kLeadSectorReachTicks, static_cast<int64_t>(kReachFloorTicks));
    EXPECT_EQ(kLeadSectorReachTicks, static_cast<int64_t>(rollback_thresholds{}.max_resim_ticks));
}

TEST(LeadDecide, ALowerMovesNoRange) {
    relative_meter meter;
    meter.reset(0);
    input_buffer buffer;
    buffer.reset(0);
    lead_player lead;
    lead.cap_lead = true;
    lateness_decision d;
    d.action = lateness_action::lower;
    d.k = -4;
    d.id = 7;
    const auto plan = lead_decide(true, lead, meter, d, 500, buffer);
    EXPECT_EQ(plan.route, lead_route::send);
    EXPECT_EQ(plan.k, -4);
    EXPECT_EQ(plan.held_from, 0u);
    EXPECT_EQ(plan.effective, 500u);
    EXPECT_EQ(buffer.hold_until(), 0u);
}

TEST(LeadDecide, AReassignmentDropsTheOutstandingDecision) {
    meter_rig rig;
    std::vector<lead_plan> plans;
    rig.feed(9.0, 2000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_TRUE(rig.lead.outstanding);
    const uint32_t id = rig.lead.id;
    rig.meter.reset(rig.tick + 40);
    rig.lead.reassigned();
    EXPECT_FALSE(lead_acknowledge(rig.lead, rig.meter, id, plans[0].k));
    EXPECT_EQ(rig.meter.offset(), 0);
}

TEST(LeadArithmetic, ClientTickEstimateAndRoundTrip) {
    EXPECT_EQ(lead_rtt_ticks(0), 0u);
    EXPECT_EQ(lead_rtt_ticks(300), 20u);
    EXPECT_EQ(lead_rtt_ticks(301), 20u);
    EXPECT_EQ(lead_rtt_ticks(304), 21u);
    // newest 1000 arrived when the session was at 990, now at 1010, 300 ms:
    // the client is 20 ahead of what we heard, 20 ticks later, plus 20.
    EXPECT_EQ(lead_client_tick_estimate(1000, 990, 1010, 300), 1000 + 20 + 20);
    lead_sector_margin margin;
    EXPECT_FALSE(margin.worst(0));
    margin.push(3, 0);
    margin.push(15, 65, true);
    for (uint32_t index = 1; index < 10; ++index) margin.push(20, index * 66);
    EXPECT_EQ(margin.worst(659), 3);
    EXPECT_EQ(margin.worst(660), 20);
    EXPECT_FALSE(margin.worst(1254));
}

TEST(RelativeMeter, SoloStructurallyLateMemberUsesItsFlooredBaseline) {
    relative_meter meter;
    lateness_baseline baseline;
    const auto at = arrival_ring::time_point{};
    baseline.assigned(100, false, 100);
    baseline.settled(100, at);
    std::vector<relative_decision> decisions;
    for (uint32_t tick = 100; tick < 2100; ++tick) {
        baseline.sample(tick, -3.0, at);
        if (baseline.frozen()) meter.sample_absolute(tick, -3.0, baseline.value());
        const auto decision = meter.on_tick(tick, false);
        if (decision.action != lateness_action::none) decisions.push_back(decision);
    }
    ASSERT_TRUE(baseline.frozen());
    EXPECT_DOUBLE_EQ(baseline.value(), lateness_baseline::kFloorTicks);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_STREQ(decisions[0].rule, "absolute");
    EXPECT_EQ(decisions[0].k, 6);
}

namespace {
    // The start base as plan step 9 computed it (late_tick.hpp at 53d8ecc),
    // copied here so a change to the production function shows up.
    uint32_t step9_start_base(uint32_t input_delay, uint32_t rtt_ms) {
        const uint32_t rtt_ticks = std::min<uint32_t>((rtt_ms * TICK_RATE + 999) / 1000, 12);
        const uint32_t lead = std::min<uint32_t>(std::max<uint32_t>(1, input_delay) + rtt_ticks + 2, 34);
        const uint32_t room = 44 > input_delay ? 44 - input_delay : 0;
        return std::max<uint32_t>(1, std::min(lead, room));
    }
}

TEST(LateTickLead, StartBaseUnchangedFrom0To1000Ms) {
    for (uint32_t rtt = 0; rtt <= 1000; ++rtt)
        for (uint32_t floor_ticks: {0u, 1u, 6u, 12u}) {
            const uint32_t delay = input_delay_for_ping(static_cast<int>(rtt), floor_ticks);
            ASSERT_EQ(bmmo::sim::session_start_tick_base(delay, rtt), step9_start_base(delay, rtt))
                << rtt << " ms, delay " << delay;
        }
}

TEST(LateTickLead, LateJoinLeadIsUncappedButForTheSanityBound) {
    using bmmo::sim::late_join_lead_ticks;
    EXPECT_EQ(late_join_lead_ticks(6, 0), 8u);
    EXPECT_EQ(late_join_lead_ticks(0, 0), 3u);
    // 300 ms is 20 ticks, all of it paid for (the capped lead pays 12).
    EXPECT_EQ(late_join_lead_ticks(16, 300), 16u + 20u + 2u);
    EXPECT_GT(late_join_lead_ticks(16, 300), bmmo::sim::late_tick_lead_ticks(16, 300));
    // 600 ms at the input delay's own cap: 40 + 40 + 2, past the sanity bound.
    EXPECT_EQ(late_join_lead_ticks(kInputDelayMaxTicks, 600), bmmo::sim::kLeadSanityCapTicks);
    EXPECT_EQ(late_join_lead_ticks(kInputDelayMaxTicks, 0xFFFFFFFFu), bmmo::sim::kLeadSanityCapTicks);
}

TEST(LateTickLead, EffectiveLeadIsTheAssignedLeadPlusTheOffset) {
    using bmmo::sim::effective_lead_ticks;
    EXPECT_EQ(effective_lead_ticks(30, 0), 30u);
    EXPECT_EQ(effective_lead_ticks(30, 12), 42u);
    EXPECT_EQ(effective_lead_ticks(30, -40), 1u);
    EXPECT_EQ(effective_lead_ticks(70, 30), kLeadCapTicks);
}

// ---------------------------------------------------------------------------
// Review fixes: the hold released by a resync, the Sector margin spent by an
// ack and fed only by events on time at the server, the Sector cap that does
// not latch, the ack bounded by the delta sent, the cap latch ended by a
// lower, and the per-player path gated on CAP_LEAD.
// ---------------------------------------------------------------------------

TEST(LeadReset, AResyncBeforeEReleasesTheRestOfTheHold) {
    input_buffer buffer;
    buffer.reset(100);
    buffer.submit(100, std::vector<input_frame>{frame_of(0, 100), frame_of(0, 101)});
    bool fresh = false;
    uint32_t source = 0;
    buffer.take(100, fresh, source);
    buffer.take(101, fresh, source);
    ASSERT_EQ(buffer.reset_hold(130), 0);
    for (uint32_t t = 102; t <= 105; ++t) buffer.take(t, fresh, source);
    // The resync's base 110 lies inside the hold: the cursor comes back to
    // it, and the ticks of the hold below it stay held.
    buffer.reset(110);
    EXPECT_EQ(buffer.hold_from(), 102u);
    EXPECT_EQ(buffer.hold_until(), 110u);
    EXPECT_EQ(buffer.next_tick(), 110u);
    EXPECT_TRUE(buffer.held(109));
    EXPECT_FALSE(buffer.held(110));
    // The client's frames on its new base count: stored, and taken fresh.
    std::vector<input_frame> frames;
    for (uint32_t t = 110; t < 116; ++t) frames.push_back(frame_of(0, t));
    EXPECT_EQ(buffer.submit(110, frames), 6);
    for (uint32_t t = 106; t < 110; ++t) {
        const auto& f = buffer.take(t, fresh, source);
        EXPECT_FALSE(fresh) << t;
        EXPECT_EQ(source, 101u) << t;
        EXPECT_TRUE(same_frame(f, frame_of(0, 101))) << t;
    }
    for (uint32_t t = 110; t < 116; ++t) {
        const auto& f = buffer.take(t, fresh, source);
        EXPECT_TRUE(fresh) << t;
        EXPECT_TRUE(same_frame(f, frame_of(0, t))) << t;
    }
}

TEST(LeadReset, AResyncBehindTheTakenTicksReleasesFromTheNextUntaken) {
    input_buffer buffer;
    buffer.reset(100);
    bool fresh = false;
    uint32_t source = 0;
    buffer.submit(100, std::vector<input_frame>{frame_of(0, 100)});
    buffer.take(100, fresh, source);
    ASSERT_EQ(buffer.reset_hold(130), 0);
    for (uint32_t t = 101; t <= 105; ++t) buffer.take(t, fresh, source);
    buffer.reset(103);   // a base the world has already simulated past
    EXPECT_EQ(buffer.next_tick(), 106u);
    EXPECT_EQ(buffer.hold_until(), 106u);
    // A straggler at or after the release becomes the substitute again.
    buffer.submit(106, std::vector<input_frame>{frame_of(0, 106)});
    EXPECT_TRUE(same_frame(buffer.take(106, fresh, source), frame_of(0, 106)));
    EXPECT_TRUE(fresh);
}

TEST(LeadReset, AResyncAtOrPastEKeepsTheHold) {
    input_buffer buffer;
    buffer.reset(100);
    bool fresh = false;
    buffer.take(100, fresh);
    ASSERT_EQ(buffer.reset_hold(130), 0);
    buffer.reset(140);
    EXPECT_EQ(buffer.hold_from(), 101u);
    EXPECT_EQ(buffer.hold_until(), 130u);
    EXPECT_EQ(buffer.next_tick(), 140u);
    // Nothing held: the old re-anchor, untouched (the flag-off path).
    input_buffer plain;
    plain.reset(100);
    plain.take(100, fresh);
    plain.reset(120);
    plain.reset(110);
    EXPECT_EQ(plain.next_tick(), 120u);
    EXPECT_EQ(plain.hold_until(), 0u);
}

TEST(LeadDecide, AnAckedRaiseClearsThePairsAndTheSectorCapDoesNotLatch) {
    meter_rig rig;
    rig.lead.sector_margins[2].push(10, rig.tick);
    std::vector<lead_plan> plans;
    rig.feed(12.0, 6000, [&](const lateness_decision& decision) {
        const auto plan = lead_decide(true, rig.lead, rig.meter, decision, rig.tick + 1, rig.buffer, 2);
        plans.push_back(plan);
        if (plan.route == lead_route::send) {
            EXPECT_TRUE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k));
            EXPECT_TRUE(rig.lead.sector_margins.empty());
        }
    });
    ASSERT_GE(plans.size(), 2u);
    EXPECT_EQ(plans[0].k, 9);
    EXPECT_STREQ(plans[0].binding, "sector");
    for (size_t index = 1; index < plans.size(); ++index)
        EXPECT_EQ(plans[index].route, lead_route::deferred);
    EXPECT_FALSE(rig.meter.at_cap());
}

TEST(LeadDecide, BothCapsCutTheRaiseAndTheSmallerBinds) {
    {
        meter_rig rig;
        rig.lead.assigned_lead = kLeadCapTicks - 5;   // room for 5
        rig.lead.sector_margins[2].push(8, rig.tick);               // room for 7
        std::vector<lead_plan> plans;
        rig.feed(12.0, 3000, [&](const lateness_decision& d) {
            const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer, 2);
            plans.push_back(plan);
            if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
        });
        ASSERT_EQ(plans.size(), 1u);
        EXPECT_EQ(plans[0].k, 5);
        EXPECT_STREQ(plans[0].cap, "total+sector");
        EXPECT_STREQ(plans[0].binding, "total");
        EXPECT_TRUE(rig.meter.at_cap());
    }
    {
        meter_rig rig;
        rig.lead.assigned_lead = kLeadCapTicks - 9;   // room for 9
        rig.lead.sector_margins[2].push(4, rig.tick);               // room for 3
        std::vector<lead_plan> plans;
        rig.feed(12.0, 400, [&](const lateness_decision& d) {
            const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer, 2);
            plans.push_back(plan);
            if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
        });
        ASSERT_EQ(plans.size(), 2u);
        EXPECT_EQ(plans[0].k, 3);
        EXPECT_STREQ(plans[0].cap, "total+sector");
        EXPECT_STREQ(plans[0].binding, "sector");
        EXPECT_FALSE(rig.meter.at_cap());
        // The ack cleared the pairs: without new samples the next raise is deferred.
        EXPECT_EQ(plans[1].route, lead_route::deferred);
        EXPECT_STREQ(plans[1].binding, "sector");
    }
}

TEST(LeadAck, AppliedIsBoundedByTheDeltaSent) {
    EXPECT_EQ(lateness_bound_applied(32767, 5), 5);
    EXPECT_EQ(lateness_bound_applied(-5, 5), 0);
    EXPECT_EQ(lateness_bound_applied(3, 5), 3);
    EXPECT_EQ(lateness_bound_applied(-32768, -4), -4);
    EXPECT_EQ(lateness_bound_applied(2, -4), 0);
    EXPECT_EQ(lateness_bound_applied(-2, -4), -2);
    for (const int applied: {32767, -7}) {
        meter_rig rig;
        rig.lead.sector_margins[2].push(40, rig.tick);
        std::vector<lead_plan> plans;
        rig.feed(9.0, 2000, [&](const lateness_decision& d) {
            plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
        });
        ASSERT_EQ(plans.size(), 1u);
        ASSERT_EQ(plans[0].route, lead_route::send);
        int counted = -1;
        ASSERT_TRUE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id, applied, &counted));
        const int expected = applied > 0 ? plans[0].k : 0;
        EXPECT_EQ(counted, expected) << applied;
        EXPECT_EQ(rig.meter.offset(), expected) << applied;
        EXPECT_TRUE(rig.lead.sector_margins.empty()) << applied;
    }
}

TEST(LeadAck, AnAckedLowerEndsTheCapLatch) {
    meter_rig rig;
    rig.lead.assigned_lead = kLeadCapTicks - 3;   // room for 3
    std::vector<lead_plan> plans;
    const auto handle = [&](const lateness_decision& d) {
        const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
        plans.push_back(plan);
        if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
    };
    rig.feed(12.0, 800, handle);
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_EQ(plans[0].k, 3);
    ASSERT_TRUE(rig.meter.at_cap());
    ASSERT_EQ(rig.meter.offset(), 3);
    // Now far earlier than the reference: a lower, down to the offset.
    rig.feed(-14.0, 3000, handle);
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans[1].route, lead_route::send);
    EXPECT_EQ(plans[1].k, -3);
    EXPECT_EQ(rig.meter.offset(), 0);
    EXPECT_FALSE(rig.meter.at_cap());
}

TEST(LeadSector, AnEventLateAtTheServerIsNoSample) {
    lead_player player;
    EXPECT_FALSE(lead_sector_sample(player, 2, 600, 500));
    player.rtt_ms = 300;
    player.have_rtt = true;
    EXPECT_FALSE(lead_sector_sample(player, 2, 600, 500));
    player.received(520, 490);
    EXPECT_FALSE(lead_sector_sample(player, 2, 499, 500));
    EXPECT_TRUE(player.sector_margins.empty());
    EXPECT_TRUE(lead_sector_sample(player, 2, 500, 500));
    EXPECT_TRUE(lead_sector_sample(player, 2, 560, 500));
    EXPECT_EQ(player.sector_margins[2].worst(500), 500 - 550);
}

TEST(LeadSector, PathMarginsFollowTheSenderNotTheReceiversUplink) {
    lead_player player;
    player.have_rtt = true;
    player.rtt_ms = 300;
    player.received(990, 900);
    ASSERT_TRUE(lead_sector_path_sample(player, 2, 990, 35, 900));
    EXPECT_EQ(player.sector_margins[2].worst(900), 15);
    player.reassigned();
    player.rtt_ms = 150;
    player.received(1000, 900);
    ASSERT_TRUE(lead_sector_path_sample(player, 2, 990, 35, 900));
    EXPECT_EQ(player.sector_margins[2].worst(900), 15);
    ASSERT_TRUE(lead_sector_path_sample(player, 2, 984, 35, 900));
    EXPECT_EQ(player.sector_margins[2].worst(900), 9);
    EXPECT_EQ(lead_sector_bound(player, 13, 900, 2), 8);
    ASSERT_TRUE(lead_sector_path_sample(player, 3, 970, 35, 900));
    EXPECT_EQ(player.sector_margins[3].worst(900), -5);
    EXPECT_EQ(lead_sector_bound(player, 13, 900, 3), 8);
}

TEST(LeadSector, NoSamplesDeferUntilTheUnionIsComplete) {
    for (bool complete: {false, true}) {
        meter_rig rig;
        lateness_decision decision;
        decision.action = lateness_action::raise;
        decision.k = 13;
        const auto plan = lead_decide(true, rig.lead, rig.meter, decision, rig.tick, rig.buffer, 2, complete);
        EXPECT_EQ(plan.route, complete ? lead_route::send : lead_route::deferred);
        EXPECT_EQ(plan.k, complete ? 13 : 0);
    }
}

TEST(LeadSector, AnAckClearsBothSidesOfThePair) {
    std::map<uint32_t, lead_player> players;
    players[1].sector_margins[2].push(15, 500);
    players[2].sector_margins[1].push(9, 500);
    players[2].sector_margins[3].push(20, 500);
    relative_meter meter;
    players[1].outstanding = true;
    players[1].id = 7;
    players[1].k = 3;
    EXPECT_FALSE(lead_acknowledge(players[1], meter, 6, 3));
    EXPECT_FALSE(players[1].sector_margins.empty());
    ASSERT_TRUE(lead_acknowledge(players[1], meter, 7, 3));
    lead_sector_forget(players, 1);
    EXPECT_TRUE(players[1].sector_margins.empty());
    EXPECT_EQ(players[2].sector_margins.count(1), 0u);
    EXPECT_EQ(players[2].sector_margins[3].worst(500), 20);
}

TEST(LateTickLead, ThePerPlayerPathNeedsTheFlagAndCapLead) {
    using bmmo::sim::uses_player_lead;
    EXPECT_FALSE(uses_player_lead(false, 0));
    EXPECT_FALSE(uses_player_lead(false, CAP_LEAD));
    EXPECT_FALSE(uses_player_lead(true, 0));
    EXPECT_FALSE(uses_player_lead(true, ~CAP_LEAD));
    EXPECT_TRUE(uses_player_lead(true, CAP_LEAD));
}

// Step RL1: the base of a re-anchor (resync, resync at L_eff, late join) is
// counted on the session's fixed deadline schedule (tick_scheduler::tick_at),
// floored at the runner's next tick + 2 (late_tick.hpp reanchor_base_tick).
// Whatever the server's pace at the moment of the resync, the member's base
// gets the start's budget: deadline(base) - now = lead + input delay periods.
namespace {
    // deadline(base) - now in ticks.
    double reanchor_budget_ticks(const tick_scheduler& sched, uint32_t base, tick_scheduler::clock::time_point now) {
        return std::chrono::duration<double>(sched.deadline(base) - now).count() * TICK_RATE;
    }
    // The runner's next tick when it steps `e` ticks before the deadlines
    // (e = deadline(C) - step(C); e = 0: it waits on a member's deadlines).
    uint32_t runner_tick_at(const tick_scheduler& sched, tick_scheduler::clock::time_point now, uint32_t delay, int e) {
        return static_cast<uint32_t>(static_cast<int64_t>(sched.tick_at(now, 0)) - static_cast<int64_t>(delay) + e);
    }
}

TEST(ReanchorBase, AServerWaitingOnTheMemberGivesTheStartsBudget) {
    using namespace std::chrono;
    using bmmo::sim::reanchor_base_tick;
    constexpr uint32_t D = 17, lead = 27, first = 27;
    tick_scheduler sched;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));
    // design 9.26: the first step is due start_base periods after the start
    sched.start(t0 + duration_cast<tick_scheduler::clock::duration>(duration<double>(lead / static_cast<double>(TICK_RATE))), first, D);
    // the start's budget: deadline(start base) - t0 = lead + D periods
    EXPECT_NEAR(reanchor_budget_ticks(sched, first, t0), lead + D, 1e-3);
    const auto now = t0 + seconds(23) + microseconds(4321);
    // e(C) = 0: the server steps at the deadlines, D ticks behind the schedule
    const uint32_t C = runner_tick_at(sched, now, D, 0);
    const uint32_t base = reanchor_base_tick(sched.tick_at(now, lead), C);
    const double budget = reanchor_budget_ticks(sched, base, now);
    EXPECT_GT(budget, lead + D - 1.0);
    EXPECT_LE(budget, lead + D + 1e-6);
    EXPECT_EQ(base, C + lead + D);
    // the rule it replaces (C + lead) left the frames about D ticks short
    EXPECT_LT(reanchor_budget_ticks(sched, C + lead, now), lead + 1.0);
}

TEST(ReanchorBase, AnEarlyServerGivesTheSameBudget) {
    using namespace std::chrono;
    using bmmo::sim::reanchor_base_tick;
    constexpr uint32_t D = 17, lead = 27, first = 27;
    tick_scheduler sched;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));
    sched.start(t0 + duration_cast<tick_scheduler::clock::duration>(duration<double>(lead / static_cast<double>(TICK_RATE))), first, D);
    const auto now = t0 + seconds(13) + microseconds(777);
    // arrival-paced, e = 22: the server steps about 5 ticks ahead of the schedule
    const uint32_t C = runner_tick_at(sched, now, D, 22);
    const uint32_t base = reanchor_base_tick(sched.tick_at(now, lead), C);
    const double budget = reanchor_budget_ticks(sched, base, now);
    EXPECT_GT(budget, lead + D - 1.0);
    EXPECT_LE(budget, lead + D + 1e-6);
    EXPECT_EQ(base + 5, C + lead);   // 5 ticks nearer the server than C + lead
}

TEST(ReanchorBase, NeverInTheRunnersPast) {
    using namespace std::chrono;
    using bmmo::sim::kReanchorBaseMarginTicks;
    using bmmo::sim::reanchor_base_tick;
    for (const uint32_t D: {6u, 17u, 21u})
        for (const uint32_t lead: {1u, 9u, 27u}) {
            tick_scheduler sched;
            const auto t0 = tick_scheduler::clock::time_point(seconds(100));
            sched.start(t0 + duration_cast<tick_scheduler::clock::duration>(duration<double>(lead / static_cast<double>(TICK_RATE))), lead, D);
            for (int e = -40; e <= 80; ++e)
                for (const auto now: {t0, t0 + milliseconds(100), t0 + seconds(7) + microseconds(123)}) {
                    const uint32_t C = runner_tick_at(sched, now, D, e);
                    const uint32_t base = reanchor_base_tick(sched.tick_at(now, lead), C);
                    ASSERT_GE(static_cast<int32_t>(base - C), static_cast<int32_t>(kReanchorBaseMarginTicks))
                        << "D " << D << " lead " << lead << " e " << e;
                    // the floor only ever adds budget
                    ASSERT_GT(reanchor_budget_ticks(sched, base, now), lead + D - 1.0)
                        << "D " << D << " lead " << lead << " e " << e;
                }
        }
    // a server far ahead of its schedule (no path, LAN): the floor binds
    tick_scheduler sched;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));
    sched.start(t0, 9, 6);
    const auto now = t0 + seconds(5);
    const uint32_t C = sched.tick_at(now, 0) + 9;
    EXPECT_EQ(reanchor_base_tick(sched.tick_at(now, 9), C), C + kReanchorBaseMarginTicks);
}

// Step RL1b: session_runner::reanchor_tick is reanchor_tick_on_schedule over
// the schedule the start stored (start_ticking, the one place both start
// paths go through).  From the schedule's start on it is exactly the RL1 rule,
// reanchor_base_tick(tick_at(now, lead), next_tick); before it - the first
// start_base periods after the last SessionReady - the schedule's tick is
// signed instead of clamped to first + lead, so a re-anchor there gets the
// start's budget too.  (No world-less session_runner fixture exists - a start
// needs a booted world - so the runner's own call is not exercised here.)
TEST(ReanchorOnSchedule, IsTheRl1RuleFromTheScheduleStartOn) {
    using namespace std::chrono;
    using bmmo::sim::reanchor_base_tick;
    using bmmo::sim::reanchor_margin_ticks;
    using bmmo::sim::reanchor_tick_on_schedule;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));
    for (const uint32_t D: {6u, 17u})
        for (const uint32_t lead: {3u, 27u})
            for (const bool state: {true, false}) {
                tick_scheduler sched;
                const auto start = t0 + duration_cast<tick_scheduler::clock::duration>(duration<double>(27 / static_cast<double>(TICK_RATE)));
                sched.start(start, 27, D);
                for (int64_t us = 0; us < 5000000; us += 7919)
                    for (const auto extra: {nanoseconds(0), nanoseconds(1), nanoseconds(999)})
                        for (int e = -30; e <= 30; e += 6) {
                            const auto now = start + microseconds(us) + extra;
                            const uint32_t C = static_cast<uint32_t>(
                                    static_cast<int64_t>(sched.tick_at(now, 0)) - static_cast<int64_t>(D) + e);
                            ASSERT_EQ(reanchor_tick_on_schedule(start, 27, now, lead, C, reanchor_margin_ticks(state)),
                                      reanchor_base_tick(sched.tick_at(now, lead), C, reanchor_margin_ticks(state)))
                                << "D " << D << " lead " << lead << " us " << us << " e " << e;
                        }
            }
}

TEST(ReanchorOnSchedule, BeforeTheScheduleStartTheBudgetIsStillTheStarts) {
    using namespace std::chrono;
    using bmmo::sim::kReanchorBaseMarginTicks;
    using bmmo::sim::reanchor_tick_on_schedule;
    constexpr uint32_t D = 17, start_base = 27, first = 27;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));   // the last SessionReady
    tick_scheduler sched;
    const auto start = t0 + duration_cast<tick_scheduler::clock::duration>(duration<double>(start_base / static_cast<double>(TICK_RATE)));
    sched.start(start, first, D);
    for (const uint32_t lead: {9u, 27u, 31u})
        for (auto now = t0; now < start; now += microseconds(997)) {
            // nothing stepped yet: the runner's next tick is the first
            const uint32_t base = reanchor_tick_on_schedule(start, first, now, lead, first, kReanchorBaseMarginTicks);
            const double budget = reanchor_budget_ticks(sched, base, now);
            if (base == first + kReanchorBaseMarginTicks) {
                ASSERT_GT(budget, lead + D - 1.001) << "lead " << lead;   // the floor only adds
            } else {
                ASSERT_GT(budget, lead + D - 1.001) << "lead " << lead;
                ASSERT_LE(budget, lead + D + 1e-6) << "lead " << lead;
            }
        }
    // a joiner 100 ms after the all-ready moment: RL1's tick_at clamp gave it
    // about 20 ticks more than the start members, the signed tick does not
    const auto now = t0 + milliseconds(100);
    EXPECT_GT(reanchor_budget_ticks(sched, sched.tick_at(now, 31), now), 31 + D + 19.0);
    const double budget = reanchor_budget_ticks(sched, reanchor_tick_on_schedule(start, first, now, 31, first, 2), now);
    EXPECT_GT(budget, 31 + D - 1.001);
    EXPECT_LE(budget, 31 + D + 1e-6);
}

TEST(ReanchorOnSchedule, TheScheduleTickIsFlooredAndSigned) {
    using bmmo::sim::schedule_tick_at;
    EXPECT_EQ(schedule_tick_at(0, 100, 0), 100u);
    EXPECT_EQ(schedule_tick_at(15151, 100, 0), 100u);    // 0.99997 ticks
    EXPECT_EQ(schedule_tick_at(15152, 100, 0), 101u);
    EXPECT_EQ(schedule_tick_at(-1, 100, 0), 99u);
    EXPECT_EQ(schedule_tick_at(-15151, 100, 0), 99u);
    EXPECT_EQ(schedule_tick_at(-15152, 100, 0), 98u);
    EXPECT_EQ(schedule_tick_at(-15152, 100, 27), 125u);
    // wraps like every tick: the schedule may start at a tick near 0
    EXPECT_EQ(schedule_tick_at(-1000000, 3, 0), static_cast<uint32_t>(3 - 66));
}

// Step RL1b: a member without CAP_RESYNC_STATE (the legacy retail path) has
// no base-missed re-answer, so its floor keeps the 3-tick margin every lead
// had before RL1; the state path keeps 2.
TEST(ReanchorOnSchedule, AMemberWithoutTheResyncStateKeepsAThreeTickFloor) {
    using namespace std::chrono;
    using bmmo::sim::reanchor_margin_ticks;
    using bmmo::sim::reanchor_tick_on_schedule;
    EXPECT_EQ(reanchor_margin_ticks(true), 2u);
    EXPECT_EQ(reanchor_margin_ticks(false), 3u);
    tick_scheduler sched;
    const auto t0 = tick_scheduler::clock::time_point(seconds(100));
    sched.start(t0, 9, 6);
    const auto now = t0 + seconds(5);
    const uint32_t C = sched.tick_at(now, 0) + 9;   // far ahead of the schedule (LAN)
    EXPECT_EQ(reanchor_tick_on_schedule(t0, 9, now, 9, C, reanchor_margin_ticks(true)), C + 2);
    EXPECT_EQ(reanchor_tick_on_schedule(t0, 9, now, 9, C, reanchor_margin_ticks(false)), C + 3);
    // on schedule the floor does not bind and both get the same base
    const uint32_t on = sched.tick_at(now, 0) - 6;
    EXPECT_EQ(reanchor_tick_on_schedule(t0, 9, now, 9, on, 2), reanchor_tick_on_schedule(t0, 9, now, 9, on, 3));
}

// Step DX9 (H3's harness item): the server's re-anchor log line prints the
// rule's parts (reanchor_on_schedule_parts); its base is the rule's, the
// schedule tick is signed and carries no lead, the floor is current + margin.
TEST(ReanchorOnSchedule, TheLogLinesPartsAreTheRules) {
    using namespace std::chrono;
    using bmmo::sim::reanchor_on_schedule_parts;
    using bmmo::sim::reanchor_tick_on_schedule;
    const auto start = tick_scheduler::clock::time_point(seconds(100));
    for (int64_t us = -1000000; us < 2000000; us += 3331)
        for (const uint32_t lead: {9u, 31u})
            for (const uint32_t margin: {2u, 3u})
                for (const int e: {-40, 0, 40}) {
                    const auto now = start + microseconds(us);
                    const uint32_t C = static_cast<uint32_t>(27 + us * TICK_RATE / 1000000 + e);
                    const auto p = reanchor_on_schedule_parts(start, 27, now, lead, C, margin);
                    ASSERT_EQ(p.base, reanchor_tick_on_schedule(start, 27, now, lead, C, margin)) << us;
                    ASSERT_EQ(p.elapsed_us, us);
                    ASSERT_EQ(static_cast<uint32_t>(p.schedule_tick + lead), p.scheduled) << us;
                    ASSERT_EQ(p.floor, C + margin);
                    ASSERT_EQ(p.floored(), p.base != p.scheduled);
                    ASSERT_EQ(p.schedule_tick < 27, us < 0) << us;   // signed: below the first tick before the start
                }
    // 100 ms before the start: 6.6 periods, floored to 7 below the first tick
    const auto p = reanchor_on_schedule_parts(start, 27, start - milliseconds(100), 31, 27, 2);
    EXPECT_EQ(p.elapsed_us, -100000);
    EXPECT_EQ(p.schedule_tick, 20);
    EXPECT_EQ(p.scheduled, 51u);
    EXPECT_EQ(p.floor, 29u);
    EXPECT_EQ(p.base, 51u);
    EXPECT_FALSE(p.floored());
}

// FULL steps SAX / SAXE: the SessionAssign's unreliable copies outlast a
// burst of lost datagrams on the idle downlink before the session's first
// tick (the harness proxy's count-shaped burst), and a time-shaped outage up
// to the reliable retransmit's ~330 ms (150 ms each way) less a margin.
TEST(AssignCopies, OutlastABurstOfLostDatagrams) {
    using namespace bmmo::sim;
    // The pre-SAX pattern (three copies): a burst of 4 at the reliable copy
    // took every datagram (DFR, swing_snapoff burst 4 seed 0).
    EXPECT_EQ(first_assign_copy_through_burst(3, 0, 4), -1);
    // Every burst up to the harness's longest recipe burst (8), and up to the
    // copy count, wherever it began - on an earlier datagram, at the reliable
    // copy or on a copy - leaves a copy, and after one that began at the
    // reliable copy the first to arrive is the b-th, b passes late.
    for (int burst = 1; burst <= kAssignRepeats; ++burst) {
        for (int start = -(burst - 1); start <= kAssignRepeats; ++start) {
            const int first = first_assign_copy_through_burst(kAssignRepeats, start, burst);
            ASSERT_GE(first, 0) << "burst " << burst << " from datagram " << start;
            EXPECT_LE(first, burst) << "burst " << burst << " from datagram " << start;
        }
        EXPECT_EQ(first_assign_copy_through_burst(kAssignRepeats, 0, burst), burst);
    }
    // The limit, stated: one more lost datagram than there are copies.
    EXPECT_EQ(first_assign_copy_through_burst(kAssignRepeats, 0, kAssignRepeats + 1), -1);
    // One copy per main-loop pass (SERVER_RECEIVE_INTERVAL, ~15.15 ms): the
    // copies span ~242 ms - a time-shaped outage shorter than that leaves one,
    // and the last copy still goes before the ~330 ms reliable retransmit.
    const auto pass = std::chrono::duration_cast<std::chrono::microseconds>(bmmo::SERVER_RECEIVE_INTERVAL);
    EXPECT_GE(kAssignRepeats * pass, std::chrono::milliseconds(240));
    EXPECT_LT(kAssignRepeats * pass, std::chrono::milliseconds(330));
    // The minimum gap keeps a copy out of the previous datagram's packet but
    // never holds a copy past the next pass.
    EXPECT_LT(kAssignCopyMinGap, pass);
}

// FULL step SAXE: the copies' schedule - never in the pass of the datagram
// before them, never sooner than kAssignCopyMinGap after it, at most one per
// pass, exactly kAssignRepeats of them.
TEST(AssignCopies, OneCopyPerMainLoopPass) {
    using namespace bmmo::sim;
    using clock = std::chrono::steady_clock;
    const auto pass_len = std::chrono::duration_cast<clock::duration>(bmmo::SERVER_RECEIVE_INTERVAL);
    const clock::time_point t0{std::chrono::seconds(100)};

    // An assignment made inside pass 7 (its messages, or the barrier), 3 ms
    // into the pass: nothing more in pass 7, then one copy a pass.
    {
        auto s = assign_copy_schedule::after_reliable(7, t0);
        EXPECT_FALSE(s.due(7, t0));
        EXPECT_FALSE(s.due(7, t0 + std::chrono::milliseconds(12)));   // same pass, later in it
        int sent = 0;
        uint64_t pass = 8;
        clock::time_point now = t0 + pass_len;   // the next pass, at the same offset
        std::vector<uint64_t> passes;
        while (!s.done() && pass < 100) {
            if (s.due(pass, now)) {
                s.sent(pass, now);
                passes.push_back(pass);
                ++sent;
                EXPECT_FALSE(s.due(pass, now)) << "a second copy in pass " << pass;
            }
            ++pass;
            now += pass_len;
        }
        EXPECT_EQ(sent, kAssignRepeats);
        ASSERT_EQ(passes.size(), size_t(kAssignRepeats));
        for (size_t i = 0; i < passes.size(); ++i) EXPECT_EQ(passes[i], 8u + i);
        EXPECT_FALSE(s.due(pass, now));   // done: nothing more
    }
    // An assignment the simulation thread makes 1 ms before pass 8 begins:
    // pass 8 is a new pass but too soon (the copy would share the reliable
    // one's packet), so the first copy goes in pass 9.
    {
        const auto at = t0 + pass_len - std::chrono::milliseconds(1);
        auto s = assign_copy_schedule::after_reliable(7, at);
        EXPECT_FALSE(s.due(8, t0 + pass_len));
        EXPECT_TRUE(s.due(9, t0 + 2 * pass_len));
    }
    // A pass that came late (a slow update) still sends one copy, and the
    // pass after it the next one once the gap has passed.
    {
        auto s = assign_copy_schedule::after_reliable(1, t0);
        ASSERT_TRUE(s.due(2, t0 + std::chrono::milliseconds(40)));
        s.sent(2, t0 + std::chrono::milliseconds(40));
        EXPECT_FALSE(s.due(3, t0 + std::chrono::milliseconds(43)));   // a new pass but inside the gap
        EXPECT_TRUE(s.due(3, t0 + std::chrono::milliseconds(46)));
        EXPECT_EQ(s.remaining, kAssignRepeats - 1);
    }
    // The count is a parameter (a SessionLead-like pattern would use 3).
    {
        auto s = assign_copy_schedule::after_reliable(0, t0, 3);
        for (int i = 1; i <= 3; ++i) {
            ASSERT_TRUE(s.due(i, t0 + i * pass_len));
            s.sent(i, t0 + i * pass_len);
        }
        EXPECT_TRUE(s.done());
    }
}

// Step STR: a start message's copies go on until the member answers it -
// an early answer leaves the first kAssignRepeats as they were, a later one
// stops them at once, none stops them after the bound.  A commit is not
// answered by the confirmation of the proposal it replaced.
TEST(AssignCopies, StartCopiesUntilAnswered) {
    using namespace bmmo::sim;
    const std::chrono::steady_clock::time_point t0{std::chrono::seconds(100)};
    const auto copies = [&](int answer_after) {
        auto s = assign_copy_schedule::until_answered(0, t0);
        int sent = 0;
        for (uint64_t pass = 1; !s.done(); ++pass) {
            const auto now = t0 + pass * std::chrono::milliseconds(15);
            if (!s.due(pass, now)) continue;
            s.sent(pass, now);
            if (++sent == answer_after) s.answered();
        }
        return sent;
    };
    EXPECT_EQ(copies(3), kAssignRepeats);
    EXPECT_EQ(copies(kAssignRepeats + 25), kAssignRepeats + 25);
    EXPECT_EQ(copies(0), kStartCopiesUntilAnswered);

    EXPECT_TRUE(start_copy_answered(true, 100, 2, false, 100, 2));
    EXPECT_TRUE(start_copy_answered(true, 100, 2, false, 100, 3));
    EXPECT_FALSE(start_copy_answered(true, 100, 2, false, 100, 1));
    EXPECT_TRUE(start_copy_answered(true, 100, 2, true, 100, 1));    // runs on start 1 already
    EXPECT_FALSE(start_copy_answered(true, 100, 2, false, 90, 2));   // another base
    EXPECT_TRUE(start_copy_answered(false, 100, 1, false, 100, 1));  // the commit's own confirmation
    EXPECT_FALSE(start_copy_answered(false, 100, 1, false, 100, 2)); // the replaced proposal's
    EXPECT_FALSE(start_copy_answered(false, 100, 1, true, 100, 1));
}

// FULL step SAXH (session/start_handshake.hpp): the clock estimate, the
// start schedule, the postponement and the refusals.
TEST(StartHandshake, OffsetEstimateKeepsTheLowestRoundTrip) {
    using namespace bmmo::session;
    // The client's clock is 5 s ahead; 40 ms each way.
    const auto s1 = make_clock_sample(1'000'000, 1'000'000 + 5'000'000 + 40'000, 1'080'000);
    EXPECT_EQ(s1.rtt_us, 80'000);
    EXPECT_EQ(s1.offset_us, 5'000'000);
    // A copy delayed 30 ms on the way back: a longer trip, a skewed offset.
    const auto s2 = make_clock_sample(2'000'000, 2'000'000 + 5'000'000 + 40'000, 2'110'000);
    EXPECT_EQ(s2.rtt_us, 110'000);
    EXPECT_EQ(s2.offset_us, 5'000'000 - 15'000);
    clock_estimate e;
    EXPECT_FALSE(e.known());
    e.add(s2);
    e.add(s1);
    e.add(s2);
    EXPECT_EQ(e.samples, 3u);
    EXPECT_EQ(e.best.rtt_us, 80'000);
    EXPECT_EQ(e.best.offset_us, 5'000'000);
    // Where a message the server sends at 3 s lands on the client's clock.
    EXPECT_EQ(e.arrival_local(3'000'000), 3'000'000 + 5'000'000 + 40'000);
    // A negative trip (clock noise) is no trip.
    EXPECT_EQ(make_clock_sample(10, 0, 5).rtt_us, 0);
}

TEST(StartHandshake, ScheduleCoversTheWorstRoundTrip) {
    using namespace bmmo::session;
    const auto s = plan_start(1'000'000, 300'000);
    EXPECT_EQ(s.deadline_us, 1'000'000 + 300'000 + kStartWindowSlackUs);
    EXPECT_EQ(s.x_us, s.deadline_us + 600'000 + kStartGuardSlackUs);
    EXPECT_GE(s.x_us - s.deadline_us, 600'000) << "two worst RTTs remain after the confirmation deadline";
    EXPECT_EQ(start_lateness_ticks(0, 0, TICK_RATE), 0);
    EXPECT_EQ(start_lateness_ticks(15'000, 0, TICK_RATE), 0) << "under one tick";
    EXPECT_EQ(start_lateness_ticks(16'000, 0, TICK_RATE), 1);
    EXPECT_EQ(start_lateness_ticks(122'000, 0, TICK_RATE), 8);
    EXPECT_EQ(start_lateness_ticks(0, 50'000, TICK_RATE), 0) << "early: nothing";
}

TEST(StartHandshake, ConfirmationAndPostponementWindowsBackOffTogether) {
    using namespace bmmo::session;
    for (const int64_t rtt: {int64_t{0}, int64_t{1}, int64_t{30'000}, int64_t{300'000}, int64_t{1'000'000}}) {
        const auto first = plan_start(0, rtt);
        for (int retry = 0; retry <= kStartMaxPostpones + 1; ++retry) {
            const int64_t sent = 9'000'000;
            const auto next = plan_start(sent, rtt, retry);
            EXPECT_EQ(next.deadline_us - sent, first.deadline_us * (int64_t{1} << retry));
            EXPECT_EQ(next.x_us - next.deadline_us,
                      (first.x_us - first.deadline_us) * (int64_t{1} << retry));
            EXPECT_GE(next.x_us - next.deadline_us, 2 * rtt);
        }
    }
}

TEST(StartHandshake, LostConfirmationsRequireEveryoneToConfirmEachLaterStart) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members{1, 2, 3};
    const int64_t rtt = 300'000;
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, m == 3 ? rtt : 20'000));
    int64_t sent = 10;
    ASSERT_TRUE(h.poll(sent, members).send_start);
    const auto initial = h.schedule();
    for (int retry = 1; retry <= kStartMaxPostpones; ++retry) {
        const auto old = h.schedule();
        const auto old_seq = h.seq();
        h.start_acked(1, old_seq, sent + 20'000);
        h.start_acked(2, old_seq, sent + 20'000);
        // Member 3 received the start, but its confirmation was lost.
        ASSERT_FALSE(h.poll(old.deadline_us - 1, members).send_start);
        const auto r = h.poll(old.deadline_us, members);
        ASSERT_TRUE(r.send_start);
        ASSERT_TRUE(r.postponed);
        EXPECT_TRUE(r.refuse.empty());
        EXPECT_EQ(r.missing, std::vector<uint32_t>{3});
        EXPECT_EQ(h.seq(), old_seq + 1);
        sent = old.deadline_us;
        const auto next = h.schedule();
        EXPECT_GT(next.x_us, old.x_us);
        EXPECT_GE(old.x_us - sent, 2 * rtt) << "a lost postponement copy still has retransmission time";
        EXPECT_EQ(next.deadline_us - sent, (initial.deadline_us - 10) * (int64_t{1} << retry));
        h.start_acked(3, old_seq, sent + 1);
        const auto stale = h.poll(sent + 1, members);
        EXPECT_FALSE(stale.start_now) << "an old confirmation cannot release the new start";
        EXPECT_EQ(stale.confirmed, 0u) << "members 1 and 2 must confirm the new start too";
    }
    for (const auto m: members) h.start_acked(m, h.seq(), h.schedule().deadline_us);
    const auto r = h.poll(h.schedule().deadline_us, members);
    EXPECT_TRUE(r.start_now);
    EXPECT_FALSE(r.commit);
    EXPECT_TRUE(r.refuse.empty());
}

TEST(StartHandshake, AConfirmationPastTheDeadlineCannotSpendTheGuard) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members{1, 2};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 300'000));
    ASSERT_TRUE(h.poll(10, members).send_start);
    const auto first = h.schedule();
    h.start_acked(1, 1, first.deadline_us);
    h.start_acked(2, 1, first.deadline_us + 1);
    const auto r = h.poll(first.deadline_us + 1, members);
    EXPECT_FALSE(r.start_now);
    EXPECT_TRUE(r.postponed);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{2});
    EXPECT_EQ(h.seq(), 2u);
}

TEST(StartHandshake, ABetterRttSampleCannotPullThePostponementEarlier) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members{1, 2};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 1'000'000));
    ASSERT_TRUE(h.poll(10, members).send_start);
    const auto first = h.schedule();
    for (const auto m: members) h.add_sample(m, make_clock_sample(0, 0, 1));
    ASSERT_TRUE(h.poll(first.deadline_us, members).postponed);
    EXPECT_GT(h.schedule().x_us, first.x_us);
    EXPECT_EQ(h.schedule().x_us, plan_start(first.deadline_us, 1'000'000, 1).x_us);
    // A new session starts with a fresh estimate of its scheduling budget.
    h.begin(0);
    ASSERT_TRUE(h.poll(kStartSampleWaitUs, members).send_start);
    EXPECT_EQ(h.schedule().x_us, plan_start(kStartSampleWaitUs, 1).x_us);
}

TEST(StartHandshake, StartsWhenEveryMemberConfirmed) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members = {1, 2};
    h.begin(0);
    EXPECT_EQ(h.current_phase(), start_handshake::phase::assign);
    h.add_sample(1, make_clock_sample(0, 100, 20'000));
    auto r = h.poll(30'000, members);
    EXPECT_FALSE(r.send_start) << "member 2 has no sample";
    EXPECT_EQ(r.confirmed, 1u);
    for (int i = 0; i < 4; ++i) h.add_sample(2, make_clock_sample(i * 15'000, 7, i * 15'000 + 30'000));
    r = h.poll(40'000, members);
    EXPECT_FALSE(r.send_start) << "member 1 has 1 sample of 4 and the wait is not over";
    r = h.poll(kStartSampleWaitUs, members);
    ASSERT_TRUE(r.send_start);
    EXPECT_FALSE(r.postponed);
    EXPECT_EQ(h.seq(), 1u);
    EXPECT_EQ(h.current_phase(), start_handshake::phase::start);
    EXPECT_EQ(h.schedule().x_us, plan_start(kStartSampleWaitUs, 30'000).x_us) << "the worst member's round trip";
    h.start_acked(1, 1);
    h.start_acked(2, 0);   // an older start's: void
    r = h.poll(kStartSampleWaitUs + 1, members);
    EXPECT_FALSE(r.start_now);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{2});
    h.start_acked(2, 1);
    r = h.poll(kStartSampleWaitUs + 2, members);
    EXPECT_TRUE(r.start_now);
    EXPECT_EQ(h.current_phase(), start_handshake::phase::started);
    EXPECT_FALSE(h.poll(kStartSampleWaitUs + 3, members).start_now) << "once";
}

TEST(StartHandshake, MissingConfirmationPostponesForEveryone) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members = {1, 2, 3};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 300'000));
    auto r = h.poll(10, members);
    ASSERT_TRUE(r.send_start);
    const auto first = h.schedule();
    h.start_acked(1, 1);
    h.start_acked(2, 1);
    r = h.poll(first.deadline_us - 1, members);
    EXPECT_FALSE(r.send_start);
    EXPECT_FALSE(r.start_now);
    r = h.poll(first.deadline_us, members);
    ASSERT_TRUE(r.send_start) << "member 3 missed the deadline: a new instant for all";
    EXPECT_TRUE(r.postponed);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{3});
    EXPECT_TRUE(r.refuse.empty());
    EXPECT_EQ(h.seq(), 2u);
    EXPECT_EQ(h.postpones(), 1);
    EXPECT_GT(h.schedule().x_us, first.x_us);
    h.start_acked(3, 1);   // the old start's: void
    h.start_acked(1, 2);
    h.start_acked(2, 2);
    EXPECT_FALSE(h.poll(first.deadline_us + 1, members).start_now) << "member 3 must confirm start 2";
    h.start_acked(3, 2);
    EXPECT_TRUE(h.poll(first.deadline_us + 2, members).start_now);
}

TEST(StartHandshake, RefusesAMemberThatNeverConfirms) {
    using namespace bmmo::session;
    start_handshake h;
    std::vector<uint32_t> members = {1, 2};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 100'000));
    int64_t now = 1;
    auto r = h.poll(now, members);
    ASSERT_TRUE(r.send_start);
    for (int p = 1; p <= kStartMaxPostpones; ++p) {
        h.start_acked(1, h.seq());
        now = h.schedule().deadline_us;
        r = h.poll(now, members);
        ASSERT_TRUE(r.send_start);
        EXPECT_TRUE(r.postponed) << p;
        EXPECT_TRUE(r.refuse.empty()) << p;
    }
    h.start_acked(1, h.seq());
    now = h.schedule().deadline_us;
    r = h.poll(now, members);
    EXPECT_EQ(r.refuse, std::vector<uint32_t>{2}) << "past the bound: refused";
    ASSERT_TRUE(r.send_start) << "the rest get a new instant";
    EXPECT_FALSE(r.postponed);
    members = {1};   // the server took member 2 out
    h.forget(2);
    h.start_acked(1, h.seq());
    EXPECT_TRUE(h.poll(now + 1, members).start_now);
}

TEST(StartHandshake, PhaseOneNoticeThenRefusal) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members = {1, 2};
    h.begin(0);
    h.add_sample(1, make_clock_sample(0, 0, 10'000));
    auto r = h.poll(kStartMissingNoticeUs - 1, members);
    EXPECT_FALSE(r.notice_missing);
    r = h.poll(kStartMissingNoticeUs, members);
    EXPECT_TRUE(r.notice_missing);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{2});
    EXPECT_FALSE(h.poll(kStartMissingNoticeUs + 1, members).notice_missing) << "once per phase";
    r = h.poll(kStartAssignGiveUpUs, members);
    EXPECT_EQ(r.refuse, std::vector<uint32_t>{2});
    EXPECT_FALSE(r.send_start);
    r = h.poll(kStartAssignGiveUpUs + 1, std::vector<uint32_t>{1});
    EXPECT_TRUE(r.send_start) << "the rest go on";
}

// The gate round of SAXH (review finding 2): a member whose postponement
// reached it after the old instant has started on the old start; it says
// so (CLOCK_ACK_STARTED) and that start is committed for everyone instead
// of the member being postponed against and in the end refused.
TEST(StartHandshake, AStartedMemberCommitsItsStart) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members = {1, 2, 3};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 150'000));
    ASSERT_TRUE(h.poll(10, members).send_start);
    const auto first = h.schedule();
    h.start_acked(1, 1);
    h.start_acked(2, 1);
    auto r = h.poll(first.deadline_us, members);
    ASSERT_TRUE(r.postponed) << "member 3 missed start 1";
    EXPECT_EQ(h.seq(), 2u);
    h.started_on(1, 3);   // a start that does not exist: void
    h.started_on(1, 0);
    EXPECT_FALSE(h.poll(first.deadline_us + 1, members).start_now);
    // Start 2 reached member 1 after start 1's instant: it runs on start 1.
    h.started_on(1, 1);
    h.start_acked(2, 2);
    r = h.poll(first.deadline_us + 2, members);
    ASSERT_TRUE(r.start_now);
    EXPECT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 1u);
    EXPECT_EQ(r.committed_by, 1u);
    EXPECT_TRUE(r.resync.empty()) << "FULL step SAXR: nobody runs on another start";
    EXPECT_EQ(h.current_phase(), start_handshake::phase::started);
    EXPECT_EQ(h.schedule().x_us, first.x_us) << "start 1's instant is the server's origin";
    EXPECT_FALSE(h.poll(first.deadline_us + 3, members).start_now) << "once";
    h.started_on(2, 1);   // the same start again: nothing new
    EXPECT_FALSE(h.poll(first.deadline_us + 4, members).commit);
}

// FULL step SAXR: before the commit's poll an even earlier report still
// commits that start instead (the server's schedule has not started); the
// member that reported the later one runs on it and is resynced onto the
// committed start rather than sent its commit.  Once the commit is polled
// the schedule ticks from it and cannot move: a report of any other start
// resyncs the reporter instead of committing again (StartHandshake.
// TheCommitEarlierRaceResyncs).
TEST(StartHandshake, AnEarlierStartReportedLaterIsCommittedInstead) {
    using namespace bmmo::session;
    start_handshake h;
    const std::vector<uint32_t> members = {1, 2};
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, 100'000));
    ASSERT_TRUE(h.poll(10, members).send_start);
    const auto first = h.schedule();
    ASSERT_TRUE(h.poll(first.deadline_us, members).postponed);
    const auto second = h.schedule();
    ASSERT_TRUE(h.poll(second.deadline_us, members).postponed);
    EXPECT_EQ(h.seq(), 3u);
    h.started_on(2, 2);
    h.started_on(1, 1);   // member 1 ran on an even earlier one, reported before the poll
    auto r = h.poll(second.deadline_us + 1, members);
    ASSERT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 1u);
    EXPECT_EQ(r.committed_by, 1u);
    EXPECT_EQ(h.schedule().x_us, first.x_us);
    ASSERT_EQ(r.resync.size(), 1u) << "member 2 runs on start 2: resynced onto start 1";
    EXPECT_EQ(r.resync[0].member, 2u);
    EXPECT_EQ(r.resync[0].ran_on, 2u);
    EXPECT_TRUE(r.resync[0].known);
    EXPECT_EQ(r.resync_onto, 1u);
    h.started_on(2, 2);
    r = h.poll(second.deadline_us + 3, members);
    EXPECT_FALSE(r.commit) << "a later start than the committed one: no commit";
    EXPECT_TRUE(r.resync.empty()) << "member 2 is resynced once";
    // A normal start confirmed by everyone commits its own seq: an earlier
    // started report after it still moves it (the members catch up).
    start_handshake g;
    g.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) g.add_sample(m, make_clock_sample(0, 0, 100'000));
    ASSERT_TRUE(g.poll(10, members).send_start);
    g.start_acked(1, 1);
    g.start_acked(2, 1);
    r = g.poll(20, members);
    EXPECT_TRUE(r.start_now);
    EXPECT_FALSE(r.commit);
    EXPECT_EQ(g.committed_seq(), 1u);
    g.started_on(1, 1);
    EXPECT_FALSE(g.poll(30, members).commit);
}

// FULL step SAXR (the user's decision 63; SAXC's residual): the
// commit-earlier race at a 300 ms round trip.  Member 1 runs on start 1 and
// its answer is lost past the deadline, so start 2 is postponed; an outage
// longer than the guard holds that postponement from member 3,
// which starts on start 2 and says so - start 2 is committed and the
// schedule ticks from it.  Member 1's late report of start 1 no longer
// commits start 1 "instead" (the schedule cannot move): member 1 is resynced
// onto start 2, once, and nobody stays on another start.  Member 1 is also
// resynced at the commit itself when its report is lost altogether: it
// confirmed start 1, start 1's instant has passed and so has its
// start_may_run_until.  A member that confirmed the committed start holds
// it (the commit starts it).  The third case (`w`): a member that confirmed
// a start whose instant has passed while its confirmation of the next one
// may still be in flight is watched, not resynced - a confirmation of a
// later start says it held; silent past its bound it is resynced.
TEST(StartHandshake, TheCommitEarlierRaceResyncs) {
    using namespace bmmo::session;
    constexpr int64_t rtt = 300'000;
    const std::vector<uint32_t> members = {1, 2, 3};
    start_handshake h;
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, rtt));
    ASSERT_TRUE(h.poll(10, members).send_start);
    const auto first = h.schedule();
    h.start_acked(1, 1);
    h.start_acked(3, 1);
    auto r = h.poll(first.deadline_us, members);
    ASSERT_TRUE(r.postponed) << "member 2 missed start 1";
    EXPECT_EQ(r.moved_from_seq, 1u) << "the postponement's status describes start 1";
    const auto second = h.schedule();
    h.start_acked(2, 2);
    h.start_acked(3, 2);
    // Member 1's answer (it runs on start 1) is lost past the bound.
    const int64_t bound = h.answer_in_flight_until(1);
    r = h.poll(bound, members);
    ASSERT_TRUE(r.postponed);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{1});
    EXPECT_EQ(r.moved_from_seq, 2u);
    EXPECT_EQ(h.seq(), 3u);
    EXPECT_EQ(bound, second.deadline_us) << "the in-flight wait cannot consume the guard";
    EXPECT_GE(second.x_us - bound, 2 * rtt);
    // A burst longer than the enlarged guard holds start 3 from member 3: it starts on start 2
    // and reports it once start 3 reaches it.
    h.started_on(3, 2);
    r = h.poll(second.x_us + rtt, members);
    ASSERT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 2u);
    EXPECT_EQ(r.committed_by, 3u);
    ASSERT_EQ(r.resync.size(), 1u) << "member 1 confirmed start 1, past its bound: it may run on it";
    EXPECT_EQ(r.resync[0].member, 1u);
    EXPECT_EQ(r.resync[0].ran_on, 1u);
    EXPECT_FALSE(r.resync[0].known);
    EXPECT_EQ(r.resync_onto, 2u);
    EXPECT_EQ(h.schedule().x_us, second.x_us) << "the schedule ticks from start 2";
    EXPECT_FALSE(h.watched(2)) << "member 2 confirmed the committed start: the commit starts it";
    EXPECT_FALSE(h.watched(1));
    // Member 1's late report: no second commit, no second resync.
    h.started_on(1, 1);
    EXPECT_FALSE(h.resync_pending());
    r = h.poll(second.x_us + rtt + 1, members);
    EXPECT_FALSE(r.commit);
    EXPECT_FALSE(r.start_now);
    EXPECT_TRUE(r.resync.empty());
    EXPECT_EQ(h.committed_seq(), 2u);
    EXPECT_FALSE(h.watching());
    r = h.poll(start_may_run_until(second.x_us, rtt) + 1, members);
    EXPECT_TRUE(r.resync.empty()) << "nobody resynced but the runner";

    // The other order of the same race, the commit by member 3 decided while
    // member 1 had not confirmed start 1 (its confirmation lost too): the
    // commit resyncs nobody, and member 1's late report resyncs it.
    start_handshake g;
    g.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) g.add_sample(m, make_clock_sample(0, 0, rtt));
    ASSERT_TRUE(g.poll(10, members).send_start);
    const auto g1 = g.schedule();
    g.start_acked(3, 1);
    ASSERT_TRUE(g.poll(g1.deadline_us, members).postponed);
    const auto g2 = g.schedule();
    g.start_acked(2, 2);
    g.start_acked(3, 2);
    ASSERT_TRUE(g.poll(g2.deadline_us, members).postponed) << "member 1 never confirmed: missing at the deadline";
    g.started_on(3, 2);
    r = g.poll(g2.x_us + rtt, members);
    ASSERT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 2u);
    EXPECT_TRUE(r.resync.empty()) << "nothing says member 1 runs on another start yet";
    EXPECT_FALSE(g.watched(1)) << "it confirmed nothing";
    EXPECT_FALSE(g.watched(2)) << "it confirmed the committed start";
    g.started_on(1, 1);
    EXPECT_TRUE(g.resync_pending());
    r = g.poll(g2.x_us + rtt + 1, members);
    EXPECT_FALSE(r.commit) << "the schedule ticks from start 2: start 1 is not committed";
    ASSERT_EQ(r.resync.size(), 1u);
    EXPECT_EQ(r.resync[0].member, 1u);
    EXPECT_EQ(r.resync[0].ran_on, 1u);
    EXPECT_TRUE(r.resync[0].known);
    EXPECT_EQ(r.resync_onto, 2u);
    g.started_on(1, 1);   // another copy's answer
    EXPECT_FALSE(g.resync_pending()) << "once";
    g.forget(1);
    EXPECT_EQ(g.started_report(1), 0u);

    // A watched member that says nothing: resynced at its bound.  Member 2
    // confirmed start 2 (instant passed) when member 1's report of start 1
    // commits it; a report of the committed start or the commit's own
    // confirmation do not end the watch (a cut catch-up reports it too).
    start_handshake w;
    w.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) w.add_sample(m, make_clock_sample(0, 0, rtt));
    ASSERT_TRUE(w.poll(10, members).send_start);
    const auto w1 = w.schedule();
    w.start_acked(1, 1);
    w.start_acked(3, 1);
    ASSERT_TRUE(w.poll(w1.deadline_us, members).postponed);
    const auto w2 = w.schedule();
    w.start_acked(2, 2);
    w.start_acked(3, 2);
    ASSERT_TRUE(w.poll(w.answer_in_flight_until(1), members).postponed);
    w.start_acked(3, 3);
    w.started_on(1, 1);
    r = w.poll(w2.x_us + rtt, members);
    ASSERT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 1u);
    EXPECT_TRUE(r.resync.empty()) << "member 2 is watched, member 3 confirmed start 3 (still ahead)";
    EXPECT_TRUE(w.watched(2));
    EXPECT_FALSE(w.watched(3));
    {
        auto held = w;   // the same, but member 2 held start 3: its confirmation of start 3 lands
        held.start_acked(2, 3);
        EXPECT_FALSE(held.watching());
        EXPECT_TRUE(held.poll(start_may_run_until(w2.x_us, rtt), members).resync.empty());
    }
    w.start_acked(2, 1);   // the commit's confirmation: says nothing
    w.started_on(2, 1);    // a report of the committed start: says nothing
    const int64_t until = start_may_run_until(w2.x_us, rtt);
    EXPECT_TRUE(w.poll(until - 1, members).resync.empty());
    r = w.poll(until, members);
    ASSERT_EQ(r.resync.size(), 1u) << "silent past its bound: it may run on start 2";
    EXPECT_EQ(r.resync[0].member, 2u);
    EXPECT_EQ(r.resync[0].ran_on, 2u);
    EXPECT_FALSE(r.resync[0].known);
    EXPECT_EQ(r.resync_onto, 1u);
    EXPECT_FALSE(w.watching());
}

// FULL step SAXC (the user's decision 60, HXP's `commit` cell at 150 ms
// each way): a member that confirmed start 1 and runs on it because start 2
// reached it after start 1's instant answers CLOCK_ACK_STARTED no sooner
// than X1 + rtt, after start 2's deadline above 200 ms of round trip.  It is
// not named missing before the deadline while that answer may be in flight.
// STEX no longer waits past the deadline: the start is postponed, retaining
// its guard. A later report of an actual start still uses the recovery path.
TEST(StartHandshake, AnAnswerInFlightIsNotMissing) {
    using namespace bmmo::session;
    constexpr int64_t rtt = 300'000;
    const std::vector<uint32_t> members = {1, 2, 3};
    const auto setup = [&](start_handshake& h, start_schedule& first, start_schedule& second) {
        h.begin(0);
        for (const auto m: members)
            for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, rtt));
        ASSERT_TRUE(h.poll(10, members).send_start);
        first = h.schedule();
        h.start_acked(1, 1);
        h.start_acked(3, 1);
        auto r = h.poll(first.deadline_us, members);
        ASSERT_TRUE(r.postponed) << "member 2 missed start 1";
        EXPECT_EQ(r.missing, std::vector<uint32_t>{2});
        EXPECT_TRUE(r.in_flight.empty()) << "start 1 has no earlier start";
        second = h.schedule();
        h.start_acked(2, 2);
        h.start_acked(3, 2);
        EXPECT_EQ(h.confirmed_seq(1), 1u);
        EXPECT_EQ(h.confirmed_seq(3), 2u);
    };
    start_handshake h;
    start_schedule first, second;
    setup(h, first, second);
    // The earlier-start answer may arrive after this round's deadline.
    // Preserve the full guard instead of waiting into it.
    const int64_t until = h.answer_in_flight_until(1);
    EXPECT_EQ(until, std::min(first.x_us + rtt + kStartWindowSlackUs, second.deadline_us));
    EXPECT_GT(first.x_us + rtt, second.deadline_us) << "the HXP case: the earliest answer lands after the deadline";
    EXPECT_EQ(until, second.deadline_us);
    EXPECT_EQ(h.answer_in_flight_until(3), 0) << "confirmed the current start";
    auto r = h.poll(second.deadline_us - 1, members);
    EXPECT_FALSE(r.send_start) << "not postponed";
    EXPECT_FALSE(r.postponed);
    EXPECT_FALSE(r.start_now);
    EXPECT_TRUE(r.missing.empty()) << "a running member is not named";
    EXPECT_EQ(r.in_flight, std::vector<uint32_t>{1});
    EXPECT_EQ(r.confirmed, 2u);
    EXPECT_EQ(h.postpones(), 1);
    r = h.poll(second.deadline_us, members);
    ASSERT_TRUE(r.postponed) << "even an in-flight answer cannot spend the guard";
    EXPECT_EQ(r.missing, std::vector<uint32_t>{1});
    // A genuinely started member remains covered by the existing recovery.
    h.started_on(1, 1);
    r = h.poll(first.x_us + rtt + 1, members);
    ASSERT_TRUE(r.start_now);
    EXPECT_TRUE(r.commit);
    EXPECT_EQ(r.commit_seq, 1u);
    EXPECT_EQ(h.postpones(), 2);
    EXPECT_TRUE(r.refuse.empty());
    // FULL step SAXR: HXP's `commit` resyncs nobody - members 2 and 3 hold
    // start 2 (its instant still ahead) and the commit starts them.
    EXPECT_LT(first.x_us + rtt + 1, second.x_us);
    EXPECT_TRUE(r.resync.empty());

    // A confirmation just before the deadline still starts normally.
    start_handshake g;
    setup(g, first, second);
    ASSERT_FALSE(g.poll(second.deadline_us - 1, members).send_start);
    g.start_acked(1, 2, second.deadline_us);
    r = g.poll(second.deadline_us, members);
    EXPECT_TRUE(r.start_now);
    EXPECT_FALSE(r.commit);
    EXPECT_GE(second.x_us - second.deadline_us, 2 * rtt);

    // No answer: at the bound the member is missing and the start is
    // postponed as before, still before start 2's instant.
    start_handshake k;
    setup(k, first, second);
    const int64_t bound = k.answer_in_flight_until(1);
    EXPECT_FALSE(k.poll(bound - 1, members).send_start);
    r = k.poll(bound, members);
    ASSERT_TRUE(r.send_start);
    EXPECT_TRUE(r.postponed);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{1});
    EXPECT_TRUE(r.in_flight.empty());
    EXPECT_EQ(k.postpones(), 2);
    EXPECT_LT(bound + rtt / 2, second.x_us + rtt / 2) << "the postponement reaches member 1 before its instant of start 2";
}

// FULL step SAXC: a truly missing member is postponed against and refused
// as before. With STEX, at the deadline even a member that confirmed an
// earlier start must confirm the current one or be retried with everyone.
TEST(StartHandshake, ATrulyMissingMemberIsStillRefused) {
    using namespace bmmo::session;
    constexpr int64_t rtt = 300'000;
    const std::vector<uint32_t> members = {1, 2, 3};
    start_handshake h;
    h.begin(0);
    for (const auto m: members)
        for (int i = 0; i < 4; ++i) h.add_sample(m, make_clock_sample(0, 0, rtt));
    ASSERT_TRUE(h.poll(10, members).send_start);
    h.start_acked(1, 1);   // member 1: start 1 only, then silent
    h.start_acked(2, 1);   // member 2: every start; member 3: none
    auto r = h.poll(h.schedule().deadline_us, members);
    ASSERT_TRUE(r.postponed);
    EXPECT_EQ(r.missing, std::vector<uint32_t>{3});
    h.start_acked(2, 2);
    r = h.poll(h.schedule().deadline_us, members);
    ASSERT_TRUE(r.postponed) << "member 3 is truly missing: postponed at the deadline as before";
    EXPECT_EQ(r.missing, (std::vector<uint32_t>{1, 3})) << "all missing confirmations are retried at the deadline";
    EXPECT_TRUE(r.in_flight.empty());
    EXPECT_EQ(h.postpones(), 2);
    for (int p = 3; p <= kStartMaxPostpones; ++p) {
        h.start_acked(2, h.seq());
        r = h.poll(h.schedule().deadline_us, members);
        ASSERT_TRUE(r.postponed) << p;
        EXPECT_EQ(r.missing, (std::vector<uint32_t>{1, 3})) << "start 1's answer can no longer be in flight";
        EXPECT_TRUE(r.in_flight.empty());
    }
    h.start_acked(2, h.seq());
    r = h.poll(h.schedule().deadline_us, members);
    EXPECT_EQ(r.refuse, (std::vector<uint32_t>{1, 3})) << "past the bound: refused as before";
    ASSERT_TRUE(r.send_start);
    EXPECT_FALSE(r.postponed);
}

// The gate round of SAXH (review finding 3): the two clocks drift, so a
// sample older than kClockEstimateMaxAgeUs is replaced by any newer one and
// an estimate whose best sample is that old is not fresh.
TEST(StartHandshake, ClockEstimateGoesStale) {
    using namespace bmmo::session;
    clock_estimate e;
    e.add(make_clock_sample(0, 1'000, 20'000), 20'000);
    EXPECT_TRUE(e.fresh(20'000 + kClockEstimateMaxAgeUs));
    EXPECT_FALSE(e.fresh(20'001 + kClockEstimateMaxAgeUs));
    e.add(make_clock_sample(100, 1'000, 50'100), 50'100);
    EXPECT_EQ(e.best.rtt_us, 20'000) << "recent and slower: the best stays";
    const int64_t later = 20'000 + kClockEstimateMaxAgeUs + 1;
    e.add(make_clock_sample(later - 60'000, 9'000, later), later);
    EXPECT_EQ(e.best.rtt_us, 60'000) << "the best is stale: replaced";
    EXPECT_EQ(e.best_at_us, later);
    EXPECT_TRUE(e.fresh(later));
    EXPECT_EQ(e.samples, 3u);
}
