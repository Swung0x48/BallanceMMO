// Unit tests for the lifecycle judgements of a physics session
// (session/lifecycle.hpp): the S4 record of the ticks the server steps without
// a ball a frame claims (the lifecycle barrier that used to wait for it is
// gone, plan step 20), the owner's deadline for the echo of its own
// Physicalize on the server's tick from the relay header (gated on the
// header's acked: the uplink worked after the birth), and the intake-side
// repeat guard, which drops the client's per-frame BodyRevived re-reports
// without ever dropping a late one.
//
// Also covers the timeline half of the same judgements (session/timeline.hpp):
// what a SessionAssign or a resync may do to a client's input_buffer, and the
// slack the assigned base counts on in the tick_scheduler.
//
// Finally the server-side halves this test target cannot link (server.cpp): the
// caps on the lead a late joiner is anchored at, the base the members present at
// the start are numbered from and the window its clamp keeps the start lag
// inside, all shared through sim/late_tick.hpp, the lead a resync re-anchors a
// member at (the start base's, plan step 20), and the rule that only the first
// join before the start barrier arms it (that one is still mirrored).  The
// budgets they have to fit in are the production ones.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <session/lifecycle.hpp>
#include <session/rollback.hpp>
#include <session/timeline.hpp>

#include "../sim/late_tick.hpp"

using namespace bmmo::session;
using bmmo::sim::kLateTickLeadCapTicks;
using bmmo::sim::kLateTickRttCapTicks;
using bmmo::sim::late_tick_lead_ticks;
using bmmo::sim::resync_lead_ticks;
using bmmo::sim::session_start_tick_base;

// ---------------------------------------------------------------------------
// S4 (late-player design 3.2, plan step 20).  The barrier that held every
// member up to a second for the Physicalize of a frame that claimed a ball is
// gone: the tick is stepped without the ball, and a run of such ticks is told
// once when it ends, and once while still open at kBodilessOpenReportTicks
// (a dropped birth never ends).
// ---------------------------------------------------------------------------

namespace {
    using bodiless_change = bodiless_tracker::change;
}

TEST(LifecycleBodiless, ARunIsToldWhenTheBallComesWithItsLength) {
    bodiless_tracker t;
    bodiless_tracker::run run;
    // Ordinary ticks: the frame claims a ball the world has.
    EXPECT_EQ(t.step(7, 100, true, true, run), bodiless_change::none);
    EXPECT_EQ(t.runs(), 0u);
    // A birth stamped input_delay + 1 past its first flagged frame: the
    // server steps those ticks without the ball, and waits for nothing.
    for (uint32_t tick = 101; tick <= 107; ++tick)
        EXPECT_EQ(t.step(7, tick, true, false, run), bodiless_change::none) << "tick " << tick;
    EXPECT_TRUE(t.open(7));
    EXPECT_EQ(t.runs(), 1u);
    // The Physicalize applied at 108 made the ball before that step.
    ASSERT_EQ(t.step(7, 108, true, true, run), bodiless_change::ended);
    EXPECT_EQ(run.player, 7u);
    EXPECT_EQ(run.first, 101u);
    EXPECT_EQ(run.last, 107u);
    EXPECT_EQ(run.ticks(), 7u);
    EXPECT_FALSE(t.open(7));
    EXPECT_EQ(t.ended(), 1u);
}

TEST(LifecycleBodiless, AFrameThatStopsClaimingEndsTheRunToo) {
    bodiless_tracker t;
    bodiless_tracker::run run;
    t.step(7, 200, true, false, run);
    // The client's own death: its frames stop claiming the ball.
    ASSERT_EQ(t.step(7, 201, false, false, run), bodiless_change::ended);
    EXPECT_EQ(run.ticks(), 1u);
    // Nothing claimed and nothing open: no run, and the world is not asked.
    EXPECT_EQ(t.step(7, 202, false, false, run), bodiless_change::none);
    EXPECT_EQ(t.runs(), 1u);
}

TEST(LifecycleBodiless, ADroppedBirthIsToldOnceWhileOpenAndNeverStallsAnything) {
    bodiless_tracker t;
    bodiless_tracker::run run;
    int open_reports = 0;
    uint32_t told_at = 0;
    for (uint32_t tick = 2502; tick < 2502 + 1000; ++tick) {
        const auto what = t.step(7, tick, true, false, run);
        ASSERT_NE(what, bodiless_change::ended) << "tick " << tick;
        if (what == bodiless_change::still_open) {
            ++open_reports;
            told_at = tick;
        }
    }
    EXPECT_EQ(open_reports, 1);
    EXPECT_EQ(told_at, 2502u + kBodilessOpenReportTicks - 1);
    EXPECT_EQ(run.first, 2502u);
    EXPECT_EQ(run.ticks(), kBodilessOpenReportTicks);
    EXPECT_EQ(t.runs(), 1u);
    EXPECT_EQ(t.open_runs(), 1u);
}

TEST(LifecycleBodiless, TheOpenLineIsGivenOnceEvenWhenTheRunSkipsTheMark) {
    // A player missing from some ticks' applied inputs: `last` jumps over
    // kBodilessOpenReportTicks, and the line still comes, once.
    bodiless_tracker t;
    bodiless_tracker::run run;
    EXPECT_EQ(t.step(7, 1000, true, false, run), bodiless_change::none);
    EXPECT_EQ(t.step(7, 1060, true, false, run), bodiless_change::none);   // 61 ticks
    ASSERT_EQ(t.step(7, 1070, true, false, run), bodiless_change::still_open);   // 71: past 66
    EXPECT_EQ(run.first, 1000u);
    EXPECT_EQ(run.ticks(), 71u);
    for (uint32_t tick = 1071; tick < 1300; ++tick)
        EXPECT_EQ(t.step(7, tick, true, false, run), bodiless_change::none) << "tick " << tick;
    ASSERT_EQ(t.step(7, 1300, true, true, run), bodiless_change::ended);
    EXPECT_EQ(run.ticks(), 300u);
    EXPECT_TRUE(run.told_open);
}

TEST(LifecycleBodiless, PlayersAreTrackedApartAndALeaverIsForgotten) {
    bodiless_tracker t;
    bodiless_tracker::run run;
    t.step(7, 300, true, false, run);
    t.step(8, 300, true, false, run);
    EXPECT_EQ(t.open_runs(), 2u);
    ASSERT_EQ(t.step(8, 301, true, true, run), bodiless_change::ended);
    EXPECT_EQ(run.player, 8u);
    EXPECT_TRUE(t.open(7));
    t.forget(7);
    EXPECT_FALSE(t.open(7));
    EXPECT_EQ(t.ended(), 1u) << "a leaver's run ends with it, untold";
    // The same player back later starts a new run.
    EXPECT_EQ(t.step(7, 400, true, false, run), bodiless_change::none);
    EXPECT_EQ(t.runs(), 3u);
}

// ---------------------------------------------------------------------------
// The owner's deadline for the echo of its own Physicalize (plan step 20).
// Step 19 read the server's tick from the snapshots only, so with snapshots
// off a birth whose every copy was lost (the A6 drop arm) lived on at its
// owner for good while the server never made it.  The relay header carries
// the server's tick on every datagram, every tick.
// ---------------------------------------------------------------------------

TEST(OwnEchoDeadline, MissingOnlyPastTheDeadlineOnTheServersTick) {
    constexpr uint32_t deadline = 192;
    EXPECT_FALSE(own_echo_missing(2502, 2502, deadline));
    EXPECT_FALSE(own_echo_missing(2502 + deadline, 2502, deadline)) << "at the deadline the echo still has its tick";
    EXPECT_TRUE(own_echo_missing(2502 + deadline + 1, 2502, deadline));
    // A datagram from before the birth (reordered, or an old epoch's) is
    // never past it: the difference is signed.
    EXPECT_FALSE(own_echo_missing(2400, 2502, deadline));
    // Across the wrap of the tick counter.
    EXPECT_TRUE(own_echo_missing(200u, 0xFFFFFFF0u, deadline));
    EXPECT_FALSE(own_echo_missing(150u, 0xFFFFFFF0u, deadline));
}

TEST(OwnEchoDeadline, TheServersTickComesFromTheRelayWithSnapshotsOff) {
    uint32_t tick = 0;
    EXPECT_FALSE(newest_server_tick(false, 0, false, 0, tick)) << "nothing heard yet: no deadline";
    // Snapshots off (the deployment the A6 drop arm runs): the relay header
    // alone sets the deadline.
    ASSERT_TRUE(newest_server_tick(true, 2700, false, 0, tick));
    EXPECT_EQ(tick, 2700u);
    EXPECT_TRUE(own_echo_missing(tick, 2502, 192));
    // Both known: the newer one, signed.
    ASSERT_TRUE(newest_server_tick(true, 2700, true, 2650, tick));
    EXPECT_EQ(tick, 2700u);
    ASSERT_TRUE(newest_server_tick(true, 2700, true, 2710, tick));
    EXPECT_EQ(tick, 2710u);
    ASSERT_TRUE(newest_server_tick(false, 0, true, 2650, tick));
    EXPECT_EQ(tick, 2650u);
    ASSERT_TRUE(newest_server_tick(true, 5, true, 0xFFFFFFF0u, tick));
    EXPECT_EQ(tick, 5u) << "the relay's tick is newer across the wrap";
}

// The uplink gate (step 20 review): the server's tick moves with the
// downlink, so an uplink outage longer than the deadline would give up a
// birth whose reliable copy is still queued at the owner, and the copy's
// later echo would resync it a second time.  The deadline counts only once
// the header's acked shows a frame of ours at or past the origin, and only
// kOwnEchoUplinkSettleTicks (66 in the client) after that.
TEST(OwnEchoDeadline, CountsOnlyOnceTheServerHasOurFramesPastTheOrigin) {
    constexpr uint32_t deadline = 192, settle = 66, origin = 2500;
    // The drop arm: our frames keep arriving, the event never does.  The
    // server applies our frame for the origin at about the origin.
    {
        own_echo_deadline d;
        EXPECT_FALSE(d.missing(2490, true, 2489, origin, deadline, settle));
        EXPECT_FALSE(d.uplink_seen());
        EXPECT_FALSE(d.missing(2501, true, 2500, origin, deadline, settle));
        EXPECT_TRUE(d.uplink_seen());
        EXPECT_EQ(d.uplink_since(), 2501u);
        EXPECT_FALSE(d.missing(origin + deadline, true, origin + deadline, origin, deadline, settle));
        EXPECT_TRUE(d.missing(origin + deadline + 1, true, origin + deadline + 1, origin, deadline, settle))
            << "the deadline binds, as without the gate";
    }
    // An uplink outage from before the birth: the server's tick runs on, our
    // acked stays behind the origin, and nothing is given up.
    {
        own_echo_deadline d;
        for (uint32_t tick = origin; tick < origin + 600; ++tick)
            EXPECT_FALSE(d.missing(tick, true, origin - 3, origin, deadline, settle)) << "tick " << tick;
        // The uplink comes back at origin + 600: frames past the origin are
        // applied fresh again.  The queued reliable copy gets `settle` more.
        const uint32_t back = origin + 600;
        EXPECT_FALSE(d.missing(back, true, back, origin, deadline, settle));
        EXPECT_FALSE(d.missing(back + settle, true, back + settle, origin, deadline, settle));
        EXPECT_TRUE(d.missing(back + settle + 1, true, back + settle + 1, origin, deadline, settle));
    }
    // No header acked known (nothing relayed yet): never missing.
    {
        own_echo_deadline d;
        EXPECT_FALSE(d.missing(origin + 1000, false, 0, origin, deadline, settle));
    }
    // Across the wrap of the tick counter.
    {
        own_echo_deadline d;
        const uint32_t o = 0xFFFFFFF0u;
        EXPECT_FALSE(d.missing(o + 1, true, o, o, deadline, settle));
        EXPECT_TRUE(d.uplink_seen());
        EXPECT_TRUE(d.missing(o + deadline + 1, true, o + deadline, o, deadline, settle));
    }
}

namespace {
    std::vector<repeat_event_key> ring_with(uint32_t tick, uint8_t ball_type, const std::string& name) {
        std::vector<repeat_event_key> recent;
        record_repeat_event(recent, ball_type, name, tick);
        return recent;
    }
}

// One measured burst of the same body: stamps 34437 and 34448, 11 ticks apart.
TEST(RepeatEventGuard, SameBodyInsideTheWindowIsADuplicate) {
    const auto recent = ring_with(34437, 1, "P_Modul_01_Pusher");
    EXPECT_TRUE(repeat_event_duplicate(recent, 1, "P_Modul_01_Pusher", 34448));
}

TEST(RepeatEventGuard, WindowBoundaryIsInclusive) {
    const auto recent = ring_with(34437, 1, "P_Modul_01_Pusher");
    EXPECT_TRUE(repeat_event_duplicate(recent, 1, "P_Modul_01_Pusher", 34437 + kRepeatEventWindow));
    EXPECT_FALSE(repeat_event_duplicate(recent, 1, "P_Modul_01_Pusher", 34437 + kRepeatEventWindow + 1));
}

TEST(RepeatEventGuard, OlderStampStillMatchesTheSignedDifference) {
    // A late event's stamp is behind the entries recorded after it; the guard
    // must compare the age as a signed difference, not assume newest-last.
    std::vector<repeat_event_key> recent;
    record_repeat_event(recent, 1, "P_Ball_Wood_MF", 3064);
    record_repeat_event(recent, 1, "P_Modul_01_Pusher", 3070);
    EXPECT_TRUE(repeat_event_duplicate(recent, 1, "P_Ball_Wood_MF", 3064 - 60));
}

TEST(RepeatEventGuard, DifferentBodyOrBallTypeIsNotADuplicate) {
    const auto recent = ring_with(34437, 1, "P_Modul_01_Pusher");
    EXPECT_FALSE(repeat_event_duplicate(recent, 2, "P_Modul_01_Pusher", 34448));
    EXPECT_FALSE(repeat_event_duplicate(recent, 1, "P_Modul_01_Pusher2", 34448));
}

TEST(RepeatEventGuard, RingEvictsTheOldestAndStaysCapped) {
    std::vector<repeat_event_key> recent;
    for (size_t i = 0; i < kRepeatEventRing + 5; ++i)
        record_repeat_event(recent, 1, "body" + std::to_string(i), 1000);
    EXPECT_EQ(recent.size(), kRepeatEventRing);
    EXPECT_FALSE(repeat_event_duplicate(recent, 1, "body0", 1000));
    EXPECT_TRUE(repeat_event_duplicate(recent, 1, "body" + std::to_string(kRepeatEventRing + 4), 1000));
}

// ---------------------------------------------------------------------------
// Re-anchoring the input buffer (design 9.2).  A resync sends the client a
// fresh base mid-flight, so the buffer is reset() under a stream that is
// already arriving.  What must not happen is losing the first batch the client
// queued after the base, or pulling the consumption cursor back behind ticks
// the world has already simulated.
// ---------------------------------------------------------------------------

TEST(InputReanchor, ResyncKeepsFramesQueuedAtOrAfterTheBase) {
    input_buffer buffer;
    const uint32_t assigned = 140;
    buffer.reset(assigned);
    std::vector<input_frame> batch(4);
    batch[0].keys = KEY_LEAF_0;
    EXPECT_EQ(buffer.submit(assigned, batch), 4);
    EXPECT_TRUE(buffer.has(assigned));
    EXPECT_TRUE(buffer.has(assigned + 3));
    bool fresh = false;
    const input_frame& applied = buffer.take(assigned, fresh);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(applied.keys, KEY_LEAF_0);
    EXPECT_EQ(buffer.last_fresh_tick(), assigned);
    EXPECT_EQ(buffer.stale(), 0u);
}

// The regression itself: SessionAssign is reliable and can land after the
// client already stamped and sent its first batch, so the reset() has to keep
// frames that sit exactly on the new base rather than dropping the batch.
TEST(InputReanchor, ResetKeepsFramesAlreadyQueuedAtTheBase) {
    input_buffer buffer;
    std::vector<input_frame> batch(3);
    batch[1].flags = INPUT_FLAG_PHYSICALIZED;
    ASSERT_EQ(buffer.submit(100, batch), 3);
    buffer.reset(100);   // SessionAssign arrives after the batch
    EXPECT_TRUE(buffer.has(100));
    EXPECT_TRUE(buffer.has(101));
    bool fresh = false;
    EXPECT_EQ(buffer.take(101, fresh).flags, INPUT_FLAG_PHYSICALIZED);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.stale(), 0u);
}

TEST(InputReanchor, ResetNeverPullsTheCursorBack) {
    input_buffer buffer;
    std::vector<input_frame> batch(1);
    ASSERT_EQ(buffer.submit(200, batch), 1);
    bool fresh = false;
    buffer.take(200, fresh);
    ASSERT_TRUE(fresh);
    ASSERT_EQ(buffer.next_tick(), 201u);
    buffer.reset(150);   // behind the consumption point: ignored
    EXPECT_EQ(buffer.next_tick(), 201u);
    ASSERT_EQ(buffer.submit(150, batch), 0);
    EXPECT_EQ(buffer.stale(), 1u);
}

TEST(InputReanchor, LastAppliedFrameStaysTheFallbackAcrossReset) {
    input_buffer buffer;
    std::vector<input_frame> batch(1);
    batch[0].keys = KEY_SPACE;
    ASSERT_EQ(buffer.submit(300, batch), 1);
    bool fresh = false;
    const input_frame& applied = buffer.take(300, fresh);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(applied.keys, KEY_SPACE);
    buffer.reset(340);
    EXPECT_EQ(buffer.take(339, fresh).keys, KEY_SPACE);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(buffer.take(340, fresh).keys, KEY_SPACE);
    EXPECT_FALSE(fresh);
    EXPECT_TRUE(buffer.received_any());
}

// ---------------------------------------------------------------------------
// The slack the assigned tick base counts on.  late_tick_base() hands a late
// joiner or a resync a base ahead of the server's current tick, so the first
// tick that client stamps is not already past the scheduler's deadline.
// ---------------------------------------------------------------------------

TEST(TickSchedulerGrace, TickIsNotDueBeforeItsDelayElapses) {
    tick_scheduler scheduler;
    const auto start = tick_scheduler::clock::now();
    scheduler.start(start, 100, 6);
    EXPECT_FALSE(scheduler.due(start + tick_offset(6) - std::chrono::microseconds(1)));
    EXPECT_TRUE(scheduler.due(start + tick_offset(6)));
}

// ---------------------------------------------------------------------------
// How far ahead of the server that base sits.  The policy lives in
// sim/late_tick.hpp so these cases exercise the production function, not a
// mirror; the budget it has to fit in is the client's own resim window below.
// ---------------------------------------------------------------------------

namespace {
    // The client's rollback lag for an assigned lead (the review's model): the
    // retransmit and snapshot delay grow with the round trip, the rest of the
    // lead is the input delay the server runs at.  The round trip passed in is
    // the real one, not the capped rtt_ticks the lead pays for.
    double client_lag_ticks(uint32_t lead, uint32_t rtt_ticks) {
        return 1.5 * rtt_ticks + static_cast<double>(lead - rtt_ticks);
    }
}

TEST(LateTickLead, RoundTripAllowanceAndLeadAreCapped) {
    // A 300 ms round trip is 20 ticks of latency; only the cap is paid for, so
    // the lead is the floor plus the cap plus the margin, not floor + 20 + 2.
    EXPECT_EQ(late_tick_lead_ticks(6, 300), 6u + kLateTickRttCapTicks + 2u);
    // The worst input delay the server can pick still lands on the lead cap.
    EXPECT_EQ(late_tick_lead_ticks(kInputDelayMaxTicks, 65535), kLateTickLeadCapTicks);
    // A link that has shown no latency yet keeps the floor plus the margin.
    EXPECT_EQ(late_tick_lead_ticks(6, 0), 8u);
    EXPECT_EQ(late_tick_lead_ticks(0, 0), 3u);
}

TEST(LateTickLead, StaysInsideTheClientResimWindow) {
    const uint32_t window = rollback_thresholds{}.max_resim_ticks;
    // The client's lag term uses the real round trip, not the capped rtt_ticks
    // the lead pays for, so the caps alone do not bound the lag: at the cliff
    // the lead is 34 and the lag is 34 + 0.5 * raw_rtt_ticks.  Up to a 424 ms
    // ping that is inside the window.
    for (const uint32_t ping: {0u, 50u, 100u, 150u, 200u, 250u, 278u, 300u, 320u, 400u, 424u}) {
        const uint32_t delay = input_delay_for_ping(static_cast<int>(ping), 6);
        const uint32_t lead = late_tick_lead_ticks(delay, ping);
        const uint32_t raw_rtt_ticks = (ping * 66 + 999) / 1000;
        EXPECT_LE(lead, kLateTickLeadCapTicks) << "ping " << ping;
        EXPECT_LE(client_lag_ticks(lead, raw_rtt_ticks), static_cast<double>(window)) << "ping " << ping;
    }
    // One millisecond past it the same arithmetic leaves the window: 425 ms is
    // 29 raw ticks, so the lag is 34 + 14.5 = 48.5 and the client goes
    // too_far -> invalidate_history() -> unmatched snapshots.
    const uint32_t cliff_ping = 425;
    const uint32_t cliff_delay = input_delay_for_ping(static_cast<int>(cliff_ping), 6);
    const uint32_t cliff_lead = late_tick_lead_ticks(cliff_delay, cliff_ping);
    const uint32_t cliff_raw_rtt_ticks = (cliff_ping * 66 + 999) / 1000;
    EXPECT_EQ(cliff_lead, kLateTickLeadCapTicks);
    EXPECT_GT(client_lag_ticks(cliff_lead, cliff_raw_rtt_ticks), static_cast<double>(window));
}

TEST(LateTickLead, TheUncappedArithmeticIsTheTooFarCliff) {
    // Without the caps the lead would be delay + rtt_ticks + 2 over the raw
    // peak.  At 320 ms that lag passes the client's resim window outright,
    // which is the too_far -> resync loop the caps remove.
    const uint32_t ping = 320;
    const uint32_t delay = input_delay_for_ping(static_cast<int>(ping), 6);
    const uint32_t raw_rtt_ticks = (ping * 66 + 999) / 1000;
    const uint32_t uncapped_lead = delay + raw_rtt_ticks + 2;
    EXPECT_GT(client_lag_ticks(uncapped_lead, raw_rtt_ticks),
              static_cast<double>(rollback_thresholds{}.max_resim_ticks));
}

// ---------------------------------------------------------------------------
// The base the members present at the start of a session are numbered from:
// the same lead with no "current tick" term, because the world has not ticked
// when their SessionAssign goes out.  It exists because a member that kept its
// own anchor numbering ran ahead of the server by however long it waited for
// the slowest level load - a lead nothing bounded, against the same resim
// window.  The price is that the offset is not zero: the lag a start member
// works with is the base plus the input delay (its numbering offset from the
// world plus the delay the world applies a tick at), so the base is clamped to
// the window less that delay instead of following the lead cap off the cliff.
// ---------------------------------------------------------------------------

TEST(SessionStartBase, IsTheLeadMeasuredFromTickZeroBelowTheClamp) {
    EXPECT_EQ(session_start_tick_base(6, 0), 8u);
    EXPECT_EQ(session_start_tick_base(6, 300), 6u + kLateTickRttCapTicks + 2u);
}

TEST(SessionStartBase, ClampKeepsTheStartLagInsideTheClientResimWindow) {
    // lag = base + input_delay against rollback.hpp's max_resim_ticks = 48, with
    // 4 ticks of the window left to the ordinary corrections, so the base is
    // clamped to 44 - input_delay.
    constexpr uint32_t budget = 44;
    // 400 ms is the case the lead cap used to lose: the uncapped lead is 34,
    // the clamp is what the window allows.
    const uint32_t far_ping = 400;
    const uint32_t far_delay = input_delay_for_ping(static_cast<int>(far_ping), 6);
    EXPECT_EQ(session_start_tick_base(far_delay, far_ping), budget - far_delay);
    // The swept range, including the input delay's own cap: the clamp holds for
    // every link the policy can produce, and never reaches 0 - a zero base would
    // silently disable the client's rebase-on-nonzero-first_tick and put that
    // member back on its own anchor numbering.
    for (uint32_t ping = 0; ping <= 1000; ping += 50) {
        const uint32_t delay = input_delay_for_ping(static_cast<int>(ping), 6);
        const uint32_t base = session_start_tick_base(delay, ping);
        EXPECT_LE(base + delay, budget) << "ping " << ping;
        EXPECT_GE(base, 1u) << "ping " << ping;
    }
    EXPECT_EQ(session_start_tick_base(kInputDelayMaxTicks, 65535), budget - kInputDelayMaxTicks);
    EXPECT_GE(session_start_tick_base(kInputDelayMaxTicks, 65535), 1u);
}

// Plan step 20: a resync off the per-player path re-anchors at the start
// base's lead, clamped inside the window, not at the late joiner's capped
// lead.  The measured case (lp19b_a6_1500): input delay 17 at 150 ms each
// way, the late-tick lead 31 against the start's 27; the resynced owner's
// relay lag settled at 27-28 against the start members' 20.  (Its 48-50
// right after the resync was the session's stall on the skipped ticks'
// deadlines on top: input_buffer::reanchored, lead_test.)
TEST(ResyncLead, IsTheStartBasesLeadClampedInsideTheWindow) {
    constexpr uint32_t budget = 44;
    EXPECT_EQ(late_tick_lead_ticks(17, 300), 31u);
    EXPECT_EQ(resync_lead_ticks(17, 300), 27u);
    EXPECT_EQ(resync_lead_ticks(17, 300), session_start_tick_base(17, 300));
    for (uint32_t ping = 0; ping <= 1000; ping += 25) {
        const uint32_t delay = input_delay_for_ping(static_cast<int>(ping), 6);
        const uint32_t lead = resync_lead_ticks(delay, ping);
        EXPECT_LE(lead + delay, budget) << "ping " << ping;
        EXPECT_LE(lead, late_tick_lead_ticks(delay, ping)) << "never longer than the late-tick lead, ping " << ping;
        EXPECT_GE(lead, 1u) << "ping " << ping;
    }
    // Where the late-tick lead fits the window it is unchanged.
    EXPECT_EQ(resync_lead_ticks(6, 300), late_tick_lead_ticks(6, 300));
    EXPECT_EQ(resync_lead_ticks(6, 0), late_tick_lead_ticks(6, 0));
}

// ---------------------------------------------------------------------------
// Phase alignment of the start base (design 9.25 follow-up).  The base is a
// NUMBERING, not a head start: the world's first simulated tick carries it
// (physics_world::set_tick_index at create_session) and every member holds its
// own world at the anchor until SessionAssign arrives, so at a common tick
// number both sides have taken the same number of steps from their anchor.
// With the world left numbering from 0 the server was `base` steps further
// from the anchor at every tick number, which is what made Level 11's sandbag
// Delayer flip that much earlier on the server than on the clients and put the
// rollback engine into a correction every time it flipped.
// ---------------------------------------------------------------------------

namespace {
    // One side of the session: the tick its first step from the anchor is
    // numbered, and how many steps it has taken when the tick numbered `tick`
    // has just been simulated.
    struct phase_model {
        uint32_t first_tick = 0;
        uint32_t steps_at(uint32_t tick) const { return tick - first_tick + 1; }
    };
}

TEST(SessionStartBase, TheWorldAndItsMembersStepFromTheirAnchorsUnderOneNumber) {
    const uint32_t base = session_start_tick_base(6, 0);
    ASSERT_GT(base, 0u);
    const phase_model server{base};    // set_tick_index(base) before the first step
    const phase_model member{base};    // held at the anchor, then numbered from the base
    for (uint32_t tick = base; tick < base + 1000; ++tick)
        EXPECT_EQ(server.steps_at(tick), member.steps_at(tick)) << "tick " << tick;

    // What it was: the world started at 0 and a member that ran k frames
    // between its anchor and the assignment had them renumbered away, so the
    // server was base - k steps ahead of it at every tick number.  Measured on
    // Level 11: 8 ticks with k = 0 (the headless client), 12 with k = 1.
    const phase_model old_server{0};
    for (const uint32_t k: {0u, 1u, 4u}) {
        const uint32_t tick = base + 500;
        EXPECT_EQ(old_server.steps_at(tick) - (member.steps_at(tick) + k), base - k) << "k " << k;
    }
}

// ---------------------------------------------------------------------------
// Start-barrier arming (server.cpp physics_session_member_joined): a member
// joining before the session runs shares the deadline already armed for the
// members that were there, so a trickle of joiners cannot postpone it.
// ---------------------------------------------------------------------------

namespace {
    struct mirrored_start_barrier {
        bool armed = false;
        std::chrono::steady_clock::time_point deadline{};

        void arm_if_idle(std::chrono::steady_clock::time_point now) {
            if (armed) return;
            armed = true;
            deadline = now + std::chrono::seconds(8);   // sim kStartBarrierTimeout
        }
    };
}

TEST(StartBarrierArming, AJoinWhileTheBarrierIsPendingDoesNotExtendIt) {
    mirrored_start_barrier barrier;
    const std::chrono::steady_clock::time_point first(std::chrono::seconds(1000));
    barrier.arm_if_idle(first);
    const auto armed_deadline = barrier.deadline;
    for (int i = 1; i <= 8; ++i) barrier.arm_if_idle(first + std::chrono::seconds(i));
    EXPECT_EQ(barrier.deadline, armed_deadline);
    EXPECT_TRUE(barrier.armed);
}

TEST(StartBarrierArming, ADisarmedBarrierIsArmedAgainForTheNextJoin) {
    mirrored_start_barrier barrier;
    const std::chrono::steady_clock::time_point first(std::chrono::seconds(1000));
    barrier.arm_if_idle(first);
    barrier.armed = false;   // check_start_barriers() dropped the silent members
    const auto later = first + std::chrono::seconds(20);
    barrier.arm_if_idle(later);
    EXPECT_EQ(barrier.deadline, later + std::chrono::seconds(8));
}

// ---------------------------------------------------------------------
// Protocol 6: a lifecycle event on the wire more than once (lifecycle.hpp
// event_seen / event_repeats).
// ---------------------------------------------------------------------

namespace {
    struct fake_event {
        uint32_t tick = 0;          // applied
        uint32_t origin_tick = 0;   // stamped
        bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
        uint8_t ball_type = 0;
        int32_t sector = 0;
        std::string name;
    };
}

TEST(LifecycleEventRedundancy, TheFirstArrivalCountsAndTheCopiesDoNot) {
    bmmo::session::event_seen seen(4);
    fake_event e;
    e.tick = 100;
    e.origin_tick = 100;
    EXPECT_TRUE(seen.first(bmmo::session::lifecycle_key_of(e, 7)));
    EXPECT_FALSE(seen.first(bmmo::session::lifecycle_key_of(e, 7)));   // the reliable copy, or a repeat
    EXPECT_TRUE(seen.first(bmmo::session::lifecycle_key_of(e, 8)));    // another player's, same stamp
    // A trafo: Unphysicalize and Physicalize at one stamp are two events.
    fake_event down = e;
    down.type = bmmo::session::event_type::Unphysicalize;
    EXPECT_TRUE(seen.first(bmmo::session::lifecycle_key_of(down, 7)));
    // Another sector at the same stamp is another event; the same one is not.
    fake_event sector;
    sector.tick = 100;
    sector.origin_tick = 100;
    sector.type = bmmo::session::event_type::Sector;
    sector.sector = 3;
    EXPECT_TRUE(seen.first(bmmo::session::lifecycle_key_of(sector, 7)));
    EXPECT_FALSE(seen.first(bmmo::session::lifecycle_key_of(sector, 7)));
    sector.sector = 4;
    EXPECT_TRUE(seen.first(bmmo::session::lifecycle_key_of(sector, 7)));
    // The bound is per player and by stamp: the oldest goes first.
    bmmo::session::event_seen small(2);
    for (uint32_t t = 10; t < 13; ++t) {
        fake_event f;
        f.tick = t;
        f.origin_tick = t;
        EXPECT_TRUE(small.first(bmmo::session::lifecycle_key_of(f, 1)));
    }
    fake_event oldest;
    oldest.tick = 10;
    oldest.origin_tick = 10;
    EXPECT_TRUE(small.first(bmmo::session::lifecycle_key_of(oldest, 1)));   // evicted, so taken again
    fake_event newest;
    newest.tick = 12;
    newest.origin_tick = 12;
    EXPECT_FALSE(small.first(bmmo::session::lifecycle_key_of(newest, 1)));
}

TEST(LifecycleEventRedundancy, RepeatsGoOutOncePerCallToEveryRecipientAndStop) {
    bmmo::session::event_repeats repeats;
    repeats.push("A", {1, 2, 3}, 2, 1);
    repeats.push("B", {2}, 1, 1);
    repeats.push("C", {}, 5, 1);     // nobody to send to: dropped
    repeats.push("D", {1}, 0, 1);    // no copies: dropped
    EXPECT_EQ(repeats.pending(), 2u);
    std::vector<std::pair<uint32_t, std::string>> sent;
    const auto record = [&sent](uint32_t to, const std::string& bytes) { sent.emplace_back(to, bytes); };
    repeats.send_due(record);
    ASSERT_EQ(sent.size(), 4u);   // A to 1, 2, 3 and B to 2
    EXPECT_EQ(sent[0], (std::pair<uint32_t, std::string>{1, "A"}));
    EXPECT_EQ(sent[3], (std::pair<uint32_t, std::string>{2, "B"}));
    EXPECT_EQ(repeats.pending(), 1u);   // B is done
    repeats.forget(2);                   // 2 left
    sent.clear();
    repeats.send_due(record);
    ASSERT_EQ(sent.size(), 2u);   // A to 1 and 3
    EXPECT_EQ(sent[0].first, 1u);
    EXPECT_EQ(sent[1].first, 3u);
    EXPECT_EQ(repeats.pending(), 0u);
    sent.clear();
    repeats.send_due(record);
    EXPECT_TRUE(sent.empty());
    repeats.push("E", {1}, 3, 1);
    repeats.forget(1);
    EXPECT_EQ(repeats.pending(), 0u);
    // Two ticks apart (the default): a copy on the first call, none on the
    // next, one again - six copies over twelve calls.
    bmmo::session::event_repeats spaced;
    spaced.push("F", {1});
    std::vector<int> calls_with_a_copy;
    for (int call = 0; call < 14; ++call) {
        bool any = false;
        spaced.send_due([&any](uint32_t, const std::string&) { any = true; });
        if (any) calls_with_a_copy.push_back(call);
    }
    EXPECT_EQ(calls_with_a_copy, (std::vector<int>{0, 2, 4, 6, 8, 10}));
    EXPECT_EQ(spaced.pending(), 0u);
}

TEST(LifecycleEventRedundancy, OneStampOrdersTheDeathBeforeTheBirth) {
    using bmmo::session::event_type;
    using bmmo::session::lifecycle_event_rank;
    EXPECT_LT(lifecycle_event_rank(event_type::Unphysicalize), lifecycle_event_rank(event_type::Physicalize));
    EXPECT_LT(lifecycle_event_rank(event_type::Physicalize), lifecycle_event_rank(event_type::Sector));
    EXPECT_EQ(lifecycle_event_rank(event_type::Sector), lifecycle_event_rank(event_type::BodyRevived));
}

// ---------------------------------------------------------------------
// Protocol 9: late events are re-stamped at acceptance (late-player design
// 3.1).  The runner itself cannot be linked here (it owns a physics world),
// so its step is modelled with the production pieces it is made of: the
// acceptance (lifecycle_acceptance::accept), the queue keyed on the applied
// tick, the order inside a tick (lifecycle_apply_before) and the guard
// (lifecycle_unphysicalize_superseded) - exactly what session_runner::
// submit_event and step() call, in the same order.
// ---------------------------------------------------------------------

namespace {
    struct model_event {
        uint32_t player = 0, origin = 0, applied = 0;
        event_type type = event_type::Physicalize;
        uint8_t ball_type = 0;
    };

    // One session as the runner runs it: events accepted between steps at
    // the session's next tick, queued by applied tick, and each tick's due
    // events sorted and guarded.  `alive` and `ball` are what the world ends
    // up with per player; `applied_log` what it consumed, in order.
    struct runner_model {
        lifecycle_acceptance acceptance;
        std::map<uint32_t, uint8_t> join;
        std::vector<model_event> queue;
        std::map<uint32_t, uint32_t> last_physicalize;
        std::map<uint32_t, bool> alive;
        std::map<uint32_t, uint8_t> ball;
        std::vector<model_event> applied_log;
        std::vector<model_event> superseded;
        uint32_t next = 0;

        // session_runner::remove_player: the leaver's Unphysicalize, queued
        // when anything of the player still is, and the tick before whose
        // step the player is taken out (removed[player]).
        std::map<uint32_t, uint32_t> removed;
        bool leave(uint32_t player, lifecycle_acceptance::leaver_event& gone) {
            const bool has_ball = acceptance.leaver_unphysicalize(player, next, gone);
            const bool queued = std::any_of(queue.begin(), queue.end(),
                                            [player](const model_event& e) { return e.player == player; });
            if (has_ball && queued) queue.push_back(model_event{player, gone.origin, gone.applied, event_type::Unphysicalize});
            acceptance.forget(player);
            removed[player] = queued ? lifecycle_leaver_removal_tick(queue, player, next) : next;
            if (!queued) alive[player] = false;   // the world drops the ball with the player
            return has_ball;
        }
        lifecycle_acceptance::verdict accept(uint32_t player, event_type type, uint32_t origin, uint8_t ball_type = 0) {
            const auto v = acceptance.accept(player, type, origin, next);
            model_event e{player, origin, v.applied, type, ball_type};
            if (v.superseded) superseded.push_back(e);
            else queue.push_back(e);
            return v;
        }
        void step_to(uint32_t until) {
            for (; static_cast<int32_t>(next - until) < 0; ++next) {
                for (const auto& [player, at]: removed)
                    if (at == next) {
                        EXPECT_TRUE(std::none_of(queue.begin(), queue.end(),
                                                 [p = player](const model_event& e) { return e.player == p; }))
                            << "every queued event of a leaver is applied before it is taken out";
                        EXPECT_FALSE(alive[player]) << "a leaver is taken out without a ball";
                    }
                std::vector<model_event> due;
                for (auto it = queue.begin(); it != queue.end();) {
                    if (static_cast<int32_t>(it->applied - next) <= 0) { due.push_back(*it); it = queue.erase(it); }
                    else ++it;
                }
                std::stable_sort(due.begin(), due.end(), [this](const model_event& a, const model_event& b) {
                    return lifecycle_apply_before(join[a.player], a.origin, a.type, join[b.player], b.origin, b.type);
                });
                for (const auto& e: due) {
                    EXPECT_EQ(e.applied, next) << "an event is applied at exactly its applied tick";
                    if (e.type == event_type::Unphysicalize) {
                        const auto last = last_physicalize.find(e.player);
                        if (lifecycle_unphysicalize_superseded(e.origin, last != last_physicalize.end(),
                                                               last == last_physicalize.end() ? 0 : last->second))
                            continue;
                        alive[e.player] = false;
                    } else if (e.type == event_type::Physicalize) {
                        last_physicalize[e.player] = e.origin;
                        alive[e.player] = true;
                        ball[e.player] = e.ball_type;
                    }
                    applied_log.push_back(e);
                }
            }
        }
    };
}

// A re-stamp never applies an event before its stamp, and a late one at the
// session's next tick.
TEST(LifecycleRestamp, AppliedIsTheStampOrTheNextTickWhicheverIsLater) {
    EXPECT_EQ(lifecycle_applied_tick(120, 100), 120u);
    EXPECT_EQ(lifecycle_applied_tick(100, 100), 100u);
    EXPECT_EQ(lifecycle_applied_tick(90, 130), 130u);
    // signed: a stamp from far in the past is late, not far in the future
    EXPECT_EQ(lifecycle_applied_tick(0xFFFFFFF0u, 5), 5u);
    EXPECT_EQ(lifecycle_applied_tick(5, 0xFFFFFFF0u), 5u);
}

// The design's first case: one uplink outage covers P (origin 100) and U
// (origin 110), and both are accepted at N = 130, the death first.  Ordered
// by origin the birth goes first and the death takes the ball out, at the
// server as at every receiver.  By applied tick and rank (the old order) the
// death would run first, a no-op, and the ball would live on.
TEST(LifecycleRestamp, BirthAndDeathOfOneOutageApplyAtOneTickInOriginOrder) {
    runner_model m;
    m.join = {{7, 1}};
    m.alive[7] = false;
    m.next = 130;
    const auto u = m.accept(7, event_type::Unphysicalize, 110);   // its copies arrived first
    const auto p = m.accept(7, event_type::Physicalize, 100);
    EXPECT_EQ(u.applied, 130u);
    EXPECT_EQ(p.applied, 130u);
    EXPECT_FALSE(p.superseded) << "it still lands with the death, ordered first";
    m.step_to(131);
    ASSERT_EQ(m.applied_log.size(), 2u);
    EXPECT_EQ(m.applied_log[0].type, event_type::Physicalize);
    EXPECT_EQ(m.applied_log[1].type, event_type::Unphysicalize);
    EXPECT_FALSE(m.alive[7]) << "the ball dies everywhere";
    EXPECT_FALSE(m.acceptance.has_ball(7));

    // The old order (applied tick, then rank): the death first, then the
    // birth - and the ball survives.  What the origin fixes.
    EXPECT_LT(lifecycle_event_rank(event_type::Unphysicalize), lifecycle_event_rank(event_type::Physicalize));
    EXPECT_TRUE(lifecycle_apply_before(1, 100, event_type::Physicalize, 1, 110, event_type::Unphysicalize));
    EXPECT_FALSE(lifecycle_apply_before(1, 110, event_type::Unphysicalize, 1, 100, event_type::Physicalize));
    // Join order still comes first, and a beam pair of one stamp keeps the
    // death before the birth.
    EXPECT_TRUE(lifecycle_apply_before(0, 200, event_type::Physicalize, 1, 100, event_type::Physicalize));
    EXPECT_TRUE(lifecycle_apply_before(1, 100, event_type::Unphysicalize, 1, 100, event_type::Physicalize));
}

// The second case: two same-type trafos in one outage (wood -> stone -> wood)
// land at one applied tick.  Keyed on the applied tick the second wood birth
// would be a copy of the first and dropped by every receiver, the owner's
// echo included; keyed on the origin they are distinct, and the world ends
// on wood as the owner did.
TEST(LifecycleRestamp, SameTypeTrafosOfOneOutageDoNotCollideOnTheDedupeKey) {
    struct wire_event {
        uint32_t tick = 0, origin_tick = 0;
        event_type type = event_type::Physicalize;
        uint8_t ball_type = 0;
        int32_t sector = 0;
        std::string name;
    };
    runner_model m;
    m.join = {{7, 0}};
    m.next = 90;
    m.accept(7, event_type::Physicalize, 90, 0);   // on time: the first wood ball
    m.step_to(95);
    m.next = 130;   // the outage: everything below arrives at N = 130
    const uint32_t applied_stone = m.accept(7, event_type::Unphysicalize, 100).applied;
    m.accept(7, event_type::Physicalize, 100, 1);   // stone
    m.accept(7, event_type::Unphysicalize, 110);
    const auto wood = m.accept(7, event_type::Physicalize, 110, 0);
    EXPECT_EQ(applied_stone, 130u);
    EXPECT_EQ(wood.applied, 130u);
    EXPECT_FALSE(wood.superseded);
    m.step_to(131);
    EXPECT_TRUE(m.alive[7]);
    EXPECT_EQ(m.ball[7], 0u) << "wood, as the owner left it";
    EXPECT_TRUE(m.superseded.empty());

    // The keys of the two wood births as relayed: one applied tick, two origins.
    event_seen seen;
    wire_event a{130, 100, event_type::Physicalize, 1, 0, {}};
    wire_event b{130, 110, event_type::Physicalize, 0, 0, {}};
    wire_event b_wood_again{130, 90, event_type::Physicalize, 0, 0, {}};   // same applied tick as b, other origin
    EXPECT_TRUE(seen.first(lifecycle_key_of(a, 7)));
    EXPECT_TRUE(seen.first(lifecycle_key_of(b, 7)));
    EXPECT_TRUE(seen.first(lifecycle_key_of(b_wood_again, 7)))
        << "same player, type, ball type and applied tick, other origin: another event";
    EXPECT_FALSE(seen.first(lifecycle_key_of(b, 7))) << "a copy of b is still a copy";
    // What the old key (applied tick only) did: both wood births one key.
    lifecycle_event_key old_b = lifecycle_key_of(b, 7), old_again = lifecycle_key_of(b_wood_again, 7);
    old_b.tick = old_b.applied;
    old_again.tick = old_again.applied;
    EXPECT_EQ(old_b, old_again);
}

// A late birth after an on-time death: every copy of the birth (origin 100)
// was lost, its reliable retransmit lands after the death stamped 110 was
// accepted on time.  Applied at 128 it would leave a ghost ball nothing takes
// out; it is superseded at acceptance, never queued or relayed.
TEST(LifecycleRestamp, ALateBirthAfterAnOnTimeDeathIsSuperseded) {
    runner_model m;
    m.join = {{7, 0}};
    m.next = 60;
    m.accept(7, event_type::Physicalize, 60);   // the ball the death ends
    m.step_to(100);
    const auto u = m.accept(7, event_type::Unphysicalize, 110);   // on time
    EXPECT_EQ(u.applied, 110u);
    m.step_to(128);
    EXPECT_FALSE(m.alive[7]);
    const auto p = m.accept(7, event_type::Physicalize, 100);
    EXPECT_TRUE(p.superseded);
    EXPECT_EQ(p.applied, 128u);
    EXPECT_EQ(p.by_origin, 110u);
    EXPECT_EQ(p.by_applied, 110u);
    EXPECT_EQ(p.by_type, event_type::Unphysicalize);
    m.step_to(200);
    EXPECT_FALSE(m.alive[7]) << "no ghost ball";
    ASSERT_EQ(m.superseded.size(), 1u);
    EXPECT_FALSE(m.acceptance.has_ball(7)) << "the superseded birth is not remembered as a ball";
    // A later beam or respawn is not affected: a newer birth is accepted.
    EXPECT_FALSE(m.accept(7, event_type::Physicalize, 205).superseded);
    // Overtaken by a newer Physicalize the same way (a beam accepted first).
    runner_model beam;
    beam.join = {{7, 0}};
    beam.next = 100;
    beam.accept(7, event_type::Unphysicalize, 105);
    beam.accept(7, event_type::Physicalize, 105);
    beam.step_to(120);
    EXPECT_TRUE(beam.accept(7, event_type::Physicalize, 101).superseded);
    // Bounded by count, not by age: a retransmit seconds after the death is
    // still superseded (an age bound would apply it at N - a ghost ball at
    // the server and the headless remotes, none at the retail mod).
    runner_model old;
    old.next = 100;
    old.accept(7, event_type::Unphysicalize, 110);
    old.next = 110 + 500;
    EXPECT_TRUE(old.accept(7, event_type::Physicalize, 100).superseded);
    EXPECT_EQ(old.acceptance.remembered(7), 1u) << "the superseded birth is not remembered";
    // Only the last kLifecycleAcceptanceEvents are kept.
    lifecycle_acceptance many;
    for (uint32_t i = 0; i < kLifecycleAcceptanceEvents + 4; ++i)
        many.accept(7, i % 2 ? event_type::Unphysicalize : event_type::Physicalize, 200 + i, 200 + i);
    EXPECT_EQ(many.remembered(7), kLifecycleAcceptanceEvents);
}

// An older birth that still lands no later than the newer event is the
// ordinary out-of-order arrival of protocol 6: the death stamped 110 accepted
// on time (applied 110), the birth stamped 100 accepted at N = 98, so
// applied 100 - before the death.  Applied, in order.
TEST(LifecycleRestamp, AnOlderBirthThatStillLandsBeforeTheNewerEventIsApplied) {
    runner_model m;
    m.join = {{7, 0}};
    m.next = 95;
    EXPECT_EQ(m.accept(7, event_type::Unphysicalize, 110).applied, 110u);
    m.step_to(98);
    const auto p = m.accept(7, event_type::Physicalize, 100);
    EXPECT_FALSE(p.superseded);
    EXPECT_EQ(p.applied, 100u);
    m.step_to(105);
    EXPECT_TRUE(m.alive[7]);
    m.step_to(111);
    EXPECT_FALSE(m.alive[7]);
    ASSERT_EQ(m.applied_log.size(), 2u);
    EXPECT_EQ(m.applied_log[0].applied, 100u);
    EXPECT_EQ(m.applied_log[1].applied, 110u);
    // The same birth re-stamped INTO the death's tick (accepted at N = 110)
    // lands with it, first by origin: still applied, and the death follows.
    runner_model same;
    same.join = {{7, 0}};
    same.next = 95;
    same.accept(7, event_type::Unphysicalize, 110);
    same.step_to(110);
    EXPECT_FALSE(same.accept(7, event_type::Physicalize, 100).superseded);
    same.step_to(111);
    ASSERT_EQ(same.applied_log.size(), 2u);
    EXPECT_EQ(same.applied_log[0].type, event_type::Physicalize);
    EXPECT_FALSE(same.alive[7]);
}

// An on-time pair that arrives the other way round is still applied as
// stamped: a beam's pair (one stamp, the birth's copy first) and a death and
// respawn (the respawn's copy first).  Neither is re-stamped.
TEST(LifecycleRestamp, AnOnTimeOutOfOrderPairIsStillApplied) {
    runner_model beam;
    beam.join = {{7, 0}};
    beam.next = 50;
    beam.accept(7, event_type::Physicalize, 50);
    beam.step_to(90);
    EXPECT_FALSE(beam.accept(7, event_type::Physicalize, 100, 2).superseded);
    const auto down = beam.accept(7, event_type::Unphysicalize, 100);
    EXPECT_EQ(down.applied, 100u);
    beam.step_to(101);
    ASSERT_EQ(beam.applied_log.size(), 3u);
    EXPECT_EQ(beam.applied_log[1].type, event_type::Unphysicalize);
    EXPECT_EQ(beam.applied_log[2].type, event_type::Physicalize);
    EXPECT_TRUE(beam.alive[7]);
    EXPECT_EQ(beam.ball[7], 2u);

    runner_model respawn;
    respawn.join = {{7, 0}};
    respawn.next = 50;
    respawn.accept(7, event_type::Physicalize, 50);
    respawn.step_to(90);
    EXPECT_FALSE(respawn.accept(7, event_type::Physicalize, 120).superseded);
    EXPECT_EQ(respawn.accept(7, event_type::Unphysicalize, 110).applied, 110u);
    respawn.step_to(115);
    EXPECT_FALSE(respawn.alive[7]);
    respawn.step_to(121);
    EXPECT_TRUE(respawn.alive[7]);
    EXPECT_TRUE(respawn.acceptance.has_ball(7));
    // A death that lost its place (stamped before the respawn it arrives
    // after) is left to the guard on origins - at the server and everywhere.
    respawn.next = 125;
    EXPECT_FALSE(respawn.accept(7, event_type::Unphysicalize, 110).superseded);
    respawn.step_to(130);
    EXPECT_TRUE(respawn.alive[7]) << "the stale death does not take the respawn out";
    EXPECT_TRUE(lifecycle_unphysicalize_superseded(120, true, 120));
    EXPECT_FALSE(lifecycle_unphysicalize_superseded(121, true, 120));
    EXPECT_FALSE(lifecycle_unphysicalize_superseded(100, false, 0));
}

// The leaver's Unphysicalize (design 3.2): produced by the runner at its own
// next tick; after the newest accepted event of the player when that one is
// still to come, and newer by origin than the newest birth.  None when the
// player has no ball.
TEST(LifecycleRestamp, TheLeaversUnphysicalizeIsAtTheRunnersTick) {
    lifecycle_acceptance a;
    lifecycle_acceptance::leaver_event gone;
    EXPECT_FALSE(a.leaver_unphysicalize(7, 150, gone)) << "never had a ball";
    a.accept(7, event_type::Physicalize, 100, 90);
    ASSERT_TRUE(a.leaver_unphysicalize(7, 150, gone));
    EXPECT_EQ(gone.applied, 150u);
    EXPECT_EQ(gone.origin, 150u);
    // A birth accepted late (applied 160 > origin 140) and not applied yet:
    // the receivers make the ball at 160, so the death comes at 160 too,
    // after it by origin.
    lifecycle_acceptance pending;
    pending.accept(7, event_type::Physicalize, 140, 160);
    ASSERT_TRUE(pending.leaver_unphysicalize(7, 150, gone));
    EXPECT_EQ(gone.applied, 160u);
    EXPECT_EQ(gone.origin, 160u);
    EXPECT_FALSE(lifecycle_unphysicalize_superseded(gone.origin, true, 140));
    // An on-time birth stamped ahead of the runner (origin = applied = 155):
    // the death one tick after it, never at or before its origin.
    lifecycle_acceptance ahead;
    ahead.accept(7, event_type::Physicalize, 155, 150);
    ASSERT_TRUE(ahead.leaver_unphysicalize(7, 150, gone));
    EXPECT_EQ(gone.applied, 156u);
    EXPECT_EQ(gone.origin, 156u);
    EXPECT_FALSE(lifecycle_unphysicalize_superseded(gone.origin, true, 155));
    // Dead already: nothing to relay.  A stale death (older than the birth by
    // origin) does not count as one.
    lifecycle_acceptance dead;
    dead.accept(7, event_type::Physicalize, 100, 100);
    dead.accept(7, event_type::Unphysicalize, 120, 120);
    EXPECT_FALSE(dead.leaver_unphysicalize(7, 150, gone));
    lifecycle_acceptance stale;
    stale.accept(7, event_type::Physicalize, 130, 130);
    stale.accept(7, event_type::Unphysicalize, 120, 135);
    EXPECT_TRUE(stale.leaver_unphysicalize(7, 150, gone));
    EXPECT_EQ(gone.applied, 150u);
    // Another player is not this one, and forget() drops the history.
    EXPECT_FALSE(stale.leaver_unphysicalize(8, 150, gone));
    stale.forget(7);
    EXPECT_FALSE(stale.leaver_unphysicalize(7, 150, gone));
}

// Review of step 16: the leaver's events accepted and relayed but not
// applied yet are applied by the server too, at the ticks every receiver
// applies them at, and the player is taken out of the world the tick after
// the last of them.  Before, the runner dropped them with the player: a birth
// stamped ahead was made and stepped by the others and never by the server,
// and a Sector stamped ahead was started everywhere but at the server.
TEST(LifecycleRestamp, TheLeaversQueuedEventsAreAppliedBeforeItIsTakenOut) {
    runner_model m;
    m.join = {{7, 0}, {8, 1}};
    m.next = 100;
    m.accept(7, event_type::Physicalize, 100);
    m.step_to(150);
    // A respawn stamped ahead (155), accepted and relayed at N = 150, and a
    // Sector stamped 160: then the player leaves at N = 150.
    m.accept(7, event_type::Unphysicalize, 152);
    m.accept(7, event_type::Physicalize, 155);
    m.accept(7, event_type::Sector, 160);
    lifecycle_acceptance::leaver_event gone;
    ASSERT_TRUE(m.leave(7, gone));
    EXPECT_EQ(gone.applied, 156u) << "after the respawn's origin";
    EXPECT_EQ(m.removed[7], 161u) << "the tick after its last queued event, the Sector";
    m.step_to(155);
    EXPECT_FALSE(m.alive[7]);
    m.step_to(156);
    EXPECT_TRUE(m.alive[7]) << "the respawn is made at the server too, as at every receiver";
    m.step_to(157);
    EXPECT_FALSE(m.alive[7]);
    m.step_to(170);
    ASSERT_EQ(m.applied_log.size(), 5u);
    EXPECT_EQ(m.applied_log[4].type, event_type::Sector) << "the leaver's Sector is applied at the server too";
    EXPECT_EQ(m.applied_log[4].applied, 160u);

    // Nothing queued (the usual case): out at once, at the session's tick.
    runner_model idle;
    idle.join = {{7, 0}};
    idle.next = 100;
    idle.accept(7, event_type::Physicalize, 100);
    idle.step_to(150);
    ASSERT_TRUE(idle.leave(7, gone));
    EXPECT_EQ(gone.applied, 150u);
    EXPECT_EQ(idle.removed[7], 150u);
    EXPECT_TRUE(idle.queue.empty()) << "the world drops the ball with the player; nothing to queue";

    // lifecycle_leaver_removal_tick alone: signed, other players ignored.
    struct queued { uint32_t player, applied; };
    const std::vector<queued> q{{7, 152}, {8, 400}, {7, 151}};
    EXPECT_EQ(lifecycle_leaver_removal_tick(q, 7, 150), 153u);
    EXPECT_EQ(lifecycle_leaver_removal_tick(q, 9, 150), 150u);
    EXPECT_EQ(lifecycle_leaver_removal_tick(std::vector<queued>{{7, 3}}, 7, 0xFFFFFFF0u), 4u);
}

// The other types are re-stamped and never superseded.
TEST(LifecycleRestamp, SectorAndBodyRevivedAreReStampedOnly) {
    lifecycle_acceptance a;
    a.accept(7, event_type::Unphysicalize, 200, 100);
    const auto sector = a.accept(7, event_type::Sector, 90, 130);
    EXPECT_EQ(sector.applied, 130u);
    EXPECT_FALSE(sector.superseded);
    EXPECT_EQ(a.accept(7, event_type::BodyRevived, 140, 130).applied, 140u);
    EXPECT_EQ(a.remembered(7), 1u) << "only P and U are remembered";
}
