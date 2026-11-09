// sim/stall_policy.hpp: a stalled member's keys are released after 3 s, and
// the released record goes to every world inline.
#include <gtest/gtest.h>

#include "../sim/stall_policy.hpp"

namespace {
    using bmmo::session::input_frame;
    using bmmo::sim::kStallReleaseTicks;
    using bmmo::sim::stall_frame;

    input_frame held_keys() {
        input_frame f;
        f.keys = 0x09;   // up + left
        f.cam_dir[2] = 1.0f;
        f.ball_type = 2;
        f.flags = bmmo::session::INPUT_FLAG_PHYSICALIZED | bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        return f;
    }
}

TEST(StallPolicy, ThreeSecondsAtTheTickRate) {
    EXPECT_EQ(kStallReleaseTicks, 3u * bmmo::session::TICK_RATE);
    EXPECT_EQ(bmmo::sim::kStallRemoveTimeout, std::chrono::seconds(20));
}

TEST(StallPolicy, KeysHeldUpToThreeSecondsThenReleased) {
    const input_frame last = held_keys();
    const uint32_t source = 1000;
    bool released = true;
    const input_frame at_limit = stall_frame(last, source + kStallReleaseTicks, source, released);
    EXPECT_FALSE(released);
    EXPECT_EQ(at_limit.keys, last.keys);
    const input_frame past = stall_frame(last, source + kStallReleaseTicks + 1, source, released);
    EXPECT_TRUE(released);
    EXPECT_EQ(past.keys, 0);
    // Only the keys: the ball keeps its type, its state flags and its camera.
    EXPECT_EQ(past.ball_type, last.ball_type);
    EXPECT_EQ(past.flags, last.flags);
    EXPECT_EQ(past.cam_dir[2], last.cam_dir[2]);
}

TEST(StallPolicy, TheDefaultFrameIsNotARelease) {
    bool released = true;
    const input_frame none{};
    const input_frame f = stall_frame(none, 5000, bmmo::session::INPUT_SOURCE_DEFAULT, released);
    EXPECT_FALSE(released);
    EXPECT_EQ(f.keys, 0);
}

TEST(StallPolicy, AReleasedRecordGoesInline) {
    // The owner cannot resolve a released frame by reference (it never sent
    // it): every released tick is past the inline distance.
    const uint32_t source = 500;
    for (uint32_t tick = source + kStallReleaseTicks + 1; tick < source + kStallReleaseTicks + 40; ++tick) {
        bool released = false;
        stall_frame(held_keys(), tick, source, released);
        ASSERT_TRUE(released);
        EXPECT_TRUE(bmmo::session::held_inline(tick, source, 0)) << tick;
    }
}
