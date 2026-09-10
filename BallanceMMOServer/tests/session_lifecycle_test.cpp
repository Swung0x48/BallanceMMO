// Unit tests for the two lifecycle judgements of a physics session
// (session/lifecycle.hpp): the server's barrier horizon, which decides whether
// a Physicalize is already queued within reach, and the intake-side repeat
// guard, which drops the client's per-frame BodyRevived re-reports without
// ever dropping a late one.
//
// Also covers the timeline half of the same judgements (session/timeline.hpp):
// what a SessionAssign or a resync may do to a client's input_buffer, and the
// slack the assigned base counts on in the tick_scheduler.
//
// Finally the server-side halves this test target cannot link (server.cpp): the
// caps on the lead a late joiner is anchored at, the base the members present at
// the start are numbered from and the window its clamp keeps the start lag
// inside, all shared through sim/late_tick.hpp, and the rule that only the first
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
using bmmo::sim::session_start_tick_base;

namespace {
    struct stub_event { event_type type = event_type::Physicalize; };
    struct stub_pending {
        uint32_t player = 0;
        uint32_t tick = 0;
        stub_event event;
    };

    stub_pending event_of(uint32_t player, uint32_t tick, event_type type = event_type::Physicalize) {
        return stub_pending{player, tick, stub_event{type}};
    }
}

// The production shape: input_delay 6 (journal header), so the barrier at tick
// T asks for the horizon T + 7.
TEST(LifecycleBarrier, AcceptsThePhysicalizeStampedInputDelayPlusOne) {
    const std::vector<stub_pending> events{event_of(7, 124)};
    EXPECT_TRUE(physicalize_pending(events, 7, 117 + 6 + 1));
}

TEST(LifecycleBarrier, RejectsThePhysicalizeStampedPastTheHorizon) {
    const std::vector<stub_pending> events{event_of(7, 125)};
    EXPECT_FALSE(physicalize_pending(events, 7, 117 + 6 + 1));
}

TEST(LifecycleBarrier, AcceptsEarlierStampsIncludingLateOnes) {
    EXPECT_TRUE(physicalize_pending(std::vector<stub_pending>{event_of(7, 123)}, 7, 124));
    EXPECT_TRUE(physicalize_pending(std::vector<stub_pending>{event_of(7, 117)}, 7, 124));
    EXPECT_TRUE(physicalize_pending(std::vector<stub_pending>{event_of(7, 112)}, 7, 124));
}

TEST(LifecycleBarrier, IgnoresAnotherPlayersPhysicalize) {
    const std::vector<stub_pending> events{event_of(8, 120)};
    EXPECT_FALSE(physicalize_pending(events, 7, 124));
}

TEST(LifecycleBarrier, IgnoresOtherEventTypes) {
    EXPECT_FALSE(physicalize_pending(std::vector<stub_pending>{event_of(7, 120, event_type::Unphysicalize)}, 7, 124));
    EXPECT_FALSE(physicalize_pending(std::vector<stub_pending>{event_of(7, 120, event_type::BodyRevived)}, 7, 124));
    EXPECT_FALSE(physicalize_pending(std::vector<stub_pending>{event_of(7, 120, event_type::Sector)}, 7, 124));
}

TEST(LifecycleBarrier, EmptyQueueIsNeverPending) {
    EXPECT_FALSE(physicalize_pending(std::vector<stub_pending>{}, 7, 124));
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
