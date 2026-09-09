// Unit tests for the two lifecycle judgements of a physics session
// (session/lifecycle.hpp): the server's barrier horizon, which decides whether
// a Physicalize is already queued within reach, and the intake-side repeat
// guard, which drops the client's per-frame BodyRevived re-reports without
// ever dropping a late one.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <session/lifecycle.hpp>

using namespace bmmo::session;

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
