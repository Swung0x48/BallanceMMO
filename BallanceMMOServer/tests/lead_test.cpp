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
#include <gtest/gtest.h>

#include <session/journal.hpp>
#include <session/lateness.hpp>
#include <session/lead.hpp>
#include <session/timeline.hpp>

#include "../sim/late_tick.hpp"

#include <algorithm>
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
    constexpr double kTickMs = 1000.0 / 66.0;
    constexpr uint32_t kInputDelay = 16;
    constexpr uint32_t kRedundancy = 8;   // frames per uplink datagram

    double deadline_ms(uint32_t t) { return (t + kInputDelay) * kTickMs; }

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
                if (!std::isnan(slack)) datagrams.push_back({deadline_ms(t) - slack * kTickMs, p, t});
            }
        std::stable_sort(datagrams.begin(), datagrams.end(),
                         [](const datagram& a, const datagram& b) { return a.at < b.at; });

        std::vector<input_buffer> buffers(n);
        std::vector<lateness_meter> meters(n);
        std::vector<lead_player> leads(n);
        for (size_t p = 0; p < n; ++p) {
            buffers[p].reset(0);
            meters[p].reset(0);
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
                members.push_back(patience_member_of(buffers[p].has(tick), buffers[p].settled(tick), false,
                                                     &meters[p], 0));
            }
            if (o.patience) patience_assign_leads(members);
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
                                                  (deadline_ms(tick) - now) / kTickMs, o.patience);
                if (!v.step) {
                    wake = deadline_ms(tick);
                    if (v.wait) wake = std::min(wake, now + v.wait_ticks * kTickMs);
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
                const double slack = (deadline_ms(d.newest) - d.at) / kTickMs;
                if (meters[d.player].sample(d.newest, slack)) leads[d.player].received(d.newest, tick);
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
            EXPECT_NEAR(r.step_ms[t], std::max(r.step_ms[n - 1], deadline_ms(t) - 6.5 * kTickMs), 1e-6) << t;
            EXPECT_LE(r.step_ms[t], deadline_ms(t) - kTickMs) << t;
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
    // A meter with a measured reference of about 6 ticks of slack, and then
    // `late` slack for as long as `ticks`; on_tick evaluated every tick.
    struct meter_rig {
        lateness_meter meter;
        input_buffer buffer;
        lead_player lead;
        uint32_t tick = 100;

        meter_rig() {
            meter.reset(100);
            buffer.reset(100);
            lead.cap_lead = true;
            lead.assigned_lead = 20;
            lead.rtt_ms = 150;
            lead.have_rtt = true;
            feed(6.0, 400);
            EXPECT_TRUE(meter.reference_measured());
        }
        // Feeds `ticks` ticks of samples at `slack`, calling `on` for every
        // decision; returns how many there were.
        int feed(double slack, uint32_t ticks,
                 const std::function<void(const lateness_decision&)>& on = {}) {
            int decisions = 0;
            for (uint32_t i = 0; i < ticks; ++i, ++tick) {
                if (meter.sample(tick, slack)) lead.received(tick, tick + 3);
                bool fresh = false;
                buffer.submit(tick, std::vector<input_frame>{frame_of(0, tick)});
                buffer.take(tick, fresh);
                const auto d = meter.on_tick(tick);
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
    const int decisions = rig.feed(-3.0, 2000, [&](const lateness_decision& d) {
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
    rig.feed(-3.0, 2000, [&](const lateness_decision& d) {
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
    const int decisions = rig.feed(-3.0, 3000, [&](const lateness_decision& d) {
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
              plans[0].effective + static_cast<uint32_t>(plans[0].k) * 2 + lateness_meter::kCooldownTicks);
    EXPECT_FALSE(lead_acknowledge(rig.lead, rig.meter, plans[0].k, plans[0].k));
}

TEST(LeadDecide, TwoRaisesThatDoNotRestoreThenNoMore) {
    meter_rig rig;
    std::vector<lead_plan> plans;
    std::vector<lateness_action> actions;
    rig.feed(-3.0, 8000, [&](const lateness_decision& d) {
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
    const int decisions = rig.feed(-6.0, 4000, [&](const lateness_decision& d) {
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
    const int decisions = rig.feed(-3.0, 4000, [&](const lateness_decision& d) {
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
    rig.lead.sector_margin.push(9);
    rig.lead.sector_margin.push(4);
    rig.lead.sector_margin.push(12);
    std::vector<lead_plan> plans;
    rig.feed(-6.0, 4000, [&](const lateness_decision& d) {
        plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
    });
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_EQ(plans[0].route, lead_route::send);
    EXPECT_EQ(plans[0].k, 3);
    EXPECT_STREQ(plans[0].cap, "sector");
}

TEST(LeadDecide, ALowerMovesNoRange) {
    lateness_meter meter;
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
    rig.feed(-3.0, 2000, [&](const lateness_decision& d) {
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
    EXPECT_FALSE(margin.known());
    for (int i = 0; i < 10; ++i) margin.push(20 - i);
    EXPECT_EQ(margin.worst(), 11);   // the last 8: 18 .. 11
}

// ---------------------------------------------------------------------------
// The gap step 7 left: a member late from its start whose peers lack a
// measured reference.
// ---------------------------------------------------------------------------

TEST(LatenessReference, SoloStructurallyLateMemberTakesTheDeadlineReferenceAndIsRaised) {
    lateness_meter meter;
    meter.reset(0);
    uint32_t tick = 0;
    // Every sample 3 ticks after the deadline: no window qualifies.
    const auto feed = [&](double slack, uint32_t ticks) {
        std::vector<lateness_decision> out;
        for (uint32_t i = 0; i < ticks; ++i, ++tick) {
            meter.sample(tick, slack);
            lateness_settle_reference(meter, {}, {});
            const auto d = meter.on_tick(tick);
            if (d.action != lateness_action::none) out.push_back(d);
        }
        return out;
    };
    auto decisions = feed(-3.0, 66 * lateness_meter::kReferenceMaxWindows);
    EXPECT_TRUE(decisions.empty());
    EXPECT_TRUE(meter.wants_fallback());
    EXPECT_FALSE(meter.has_reference());   // no peer: the fallback has nothing yet
    decisions = feed(-3.0, 66 * (lateness_meter::kReferenceSelfWindows - lateness_meter::kReferenceMaxWindows));
    ASSERT_TRUE(meter.has_reference());
    EXPECT_STREQ(meter.reference_source(), "deadline");
    EXPECT_DOUBLE_EQ(meter.r01(), lateness_meter::kDeadlineReferenceR01);
    EXPECT_DOUBLE_EQ(patience_lead_ticks(meter.r01()), 0.0);   // its patience is the deadline
    decisions = feed(-3.0, 1000);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].action, lateness_action::raise);
    // Back to two ticks before the deadline, the deadline reference's q10.
    EXPECT_EQ(decisions[0].k, static_cast<int>(std::ceil(lateness_meter::kDeadlineReferenceR10 + 3.0)));
}

TEST(LatenessReference, AMeasuredPeerStillWinsOverTheDeadline) {
    lateness_meter meter;
    meter.reset(0);
    for (uint32_t t = 0; t < 66 * lateness_meter::kReferenceMaxWindows; ++t) meter.sample(t, -3.0);
    ASSERT_TRUE(meter.wants_fallback());
    EXPECT_STREQ(lateness_settle_reference(meter, {5.0, 7.0}, {2.0, 3.0}), "fallback");
    EXPECT_STREQ(meter.reference_source(), "fallback");
    EXPECT_DOUBLE_EQ(meter.r10(), 6.0);
    EXPECT_DOUBLE_EQ(meter.r01(), 2.5);
    // Settled: nothing more is adopted.
    EXPECT_EQ(lateness_settle_reference(meter, {}, {}), nullptr);
}

TEST(LatenessReference, EveryPeerOnTheFallbackTooTakesTheDeadline) {
    // Two members both late from the start: neither has a measured
    // reference to lend the other, so both settle on the deadline at 12.
    lateness_meter a, b;
    a.reset(0);
    b.reset(0);
    for (uint32_t t = 0; t < 66 * lateness_meter::kReferenceSelfWindows; ++t) {
        a.sample(t, -2.0);
        b.sample(t, -5.0);
        std::vector<double> r10s, r01s;
        if (b.reference_measured()) { r10s.push_back(b.r10()); r01s.push_back(b.r01()); }
        lateness_settle_reference(a, r10s, r01s);
        r10s.clear();
        r01s.clear();
        if (a.reference_measured()) { r10s.push_back(a.r10()); r01s.push_back(a.r01()); }
        lateness_settle_reference(b, r10s, r01s);
    }
    EXPECT_STREQ(a.reference_source(), "deadline");
    EXPECT_STREQ(b.reference_source(), "deadline");
}

// ---------------------------------------------------------------------------
// late_tick.hpp: the per-player leads, and the start base unchanged.
// ---------------------------------------------------------------------------

namespace {
    // The start base as plan step 9 computed it (late_tick.hpp at 53d8ecc),
    // copied here so a change to the production function shows up.
    uint32_t step9_start_base(uint32_t input_delay, uint32_t rtt_ms) {
        const uint32_t rtt_ticks = std::min<uint32_t>((rtt_ms * 66 + 999) / 1000, 12);
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

TEST(LeadDecide, AnAckedRaiseSpendsTheSectorMarginAndTheSectorCapDoesNotLatch) {
    meter_rig rig;
    rig.lead.sector_margin.push(10);
    struct seen { lead_plan plan; uint32_t tick; };
    std::vector<seen> plans;
    rig.feed(-6.0, 6000, [&](const lateness_decision& d) {
        const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
        plans.push_back({plan, rig.tick});
        // The client applies every raise, but its arrivals never improve.
        if (plan.route == lead_route::send) {
            EXPECT_TRUE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k));
            EXPECT_FALSE(rig.meter.at_cap());   // the Sector cap does not latch
        }
    });
    ASSERT_GE(plans.size(), 3u);
    ASSERT_EQ(plans[0].plan.route, lead_route::send);
    EXPECT_GT(plans[0].plan.decided_k, 9);
    EXPECT_EQ(plans[0].plan.k, 9);   // margin 10 - 1
    EXPECT_STREQ(plans[0].plan.cap, "sector");
    EXPECT_STREQ(plans[0].plan.binding, "sector");
    // The ack moved p's clock 9 ticks: the same events now land 1 tick ahead.
    EXPECT_EQ(rig.lead.sector_margin.worst(), 1);
    // So the next raise has nothing left (it would have passed against the
    // stale 10): deferred, not sent, not latched, and retried no sooner than
    // kSectorRetryTicks later.
    for (size_t i = 1; i < plans.size(); ++i) {
        EXPECT_EQ(plans[i].plan.route, lead_route::deferred) << i;
        EXPECT_EQ(plans[i].plan.k, 0) << i;
    }
    EXPECT_GE(plans[2].tick - plans[1].tick, kSectorRetryTicks);
    EXPECT_EQ(rig.lead.sent, 1u);
    EXPECT_FALSE(rig.meter.at_cap());
    EXPECT_NE(rig.meter.state(), lateness_state::given_up);   // a deferred raise counts nothing
    EXPECT_EQ(rig.meter.offset(), 9);
}

TEST(LeadDecide, BothCapsCutTheRaiseAndTheSmallerBinds) {
    {
        meter_rig rig;
        rig.lead.assigned_lead = kLeadCapTicks - 5;   // room for 5
        rig.lead.sector_margin.push(8);               // room for 7
        std::vector<lead_plan> plans;
        rig.feed(-6.0, 3000, [&](const lateness_decision& d) {
            const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
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
        rig.lead.sector_margin.push(4);               // room for 3
        std::vector<lead_plan> plans;
        rig.feed(-6.0, 400, [&](const lateness_decision& d) {
            const auto plan = lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer);
            plans.push_back(plan);
            if (plan.route == lead_route::send) lead_acknowledge(rig.lead, rig.meter, rig.lead.id, plan.k);
        });
        ASSERT_EQ(plans.size(), 2u);
        EXPECT_EQ(plans[0].k, 3);
        EXPECT_STREQ(plans[0].cap, "total+sector");
        EXPECT_STREQ(plans[0].binding, "sector");
        EXPECT_FALSE(rig.meter.at_cap());
        // Not latched, so the arrivals still late give a next raise after
        // the cooldown; the ack spent the margin to 1, so it is deferred.
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
        rig.lead.sector_margin.push(40);
        std::vector<lead_plan> plans;
        rig.feed(-3.0, 2000, [&](const lateness_decision& d) {
            plans.push_back(lead_decide(true, rig.lead, rig.meter, d, rig.tick + 1, rig.buffer));
        });
        ASSERT_EQ(plans.size(), 1u);
        ASSERT_EQ(plans[0].route, lead_route::send);
        int counted = -1;
        ASSERT_TRUE(lead_acknowledge(rig.lead, rig.meter, rig.lead.id, applied, &counted));
        const int expected = applied > 0 ? plans[0].k : 0;
        EXPECT_EQ(counted, expected) << applied;
        EXPECT_EQ(rig.meter.offset(), expected) << applied;
        EXPECT_EQ(rig.lead.sector_margin.worst(), 40 - expected) << applied;
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
    rig.feed(-6.0, 800, handle);
    ASSERT_EQ(plans.size(), 1u);
    ASSERT_EQ(plans[0].k, 3);
    ASSERT_TRUE(rig.meter.at_cap());
    ASSERT_EQ(rig.meter.offset(), 3);
    // Now far earlier than the reference: a lower, down to the offset.
    rig.feed(20.0, 3000, handle);
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans[1].route, lead_route::send);
    EXPECT_EQ(plans[1].k, -3);
    EXPECT_EQ(rig.meter.offset(), 0);
    EXPECT_FALSE(rig.meter.at_cap());
}

TEST(LeadSector, AnEventLateAtTheServerIsNoSample) {
    lead_player p;
    EXPECT_FALSE(lead_sector_sample(p, 600, 500));   // no round trip, no newest tick
    p.rtt_ms = 300;
    p.have_rtt = true;
    EXPECT_FALSE(lead_sector_sample(p, 600, 500));   // no newest tick
    p.received(520, 490);
    // c_est at 500: 520 + 10 + 20 = 550.
    EXPECT_FALSE(lead_sector_sample(p, 499, 500));   // the sender's lateness, not p's
    EXPECT_FALSE(p.sector_margin.known());
    EXPECT_TRUE(lead_sector_sample(p, 500, 500));
    EXPECT_TRUE(lead_sector_sample(p, 560, 500));
    EXPECT_EQ(p.sector_margin.worst(), 500 - 550);
}

TEST(LateTickLead, ThePerPlayerPathNeedsTheFlagAndCapLead) {
    using bmmo::sim::uses_player_lead;
    EXPECT_FALSE(uses_player_lead(false, 0));
    EXPECT_FALSE(uses_player_lead(false, CAP_LEAD));
    EXPECT_FALSE(uses_player_lead(true, 0));
    EXPECT_FALSE(uses_player_lead(true, ~CAP_LEAD));
    EXPECT_TRUE(uses_player_lead(true, CAP_LEAD));
}
