#include <game/script_slots.hpp>
#include <session/member_lives.hpp>

#include <gtest/gtest.h>

TEST(ScriptSlots, BoundSetIncludesInitialAndBegunLivesWithoutDroppingLeavers) {
    std::vector<bmmo::session::member_life> lives(4);
    lives[0].entry.join_order = 0;
    lives[0].until = 15;
    lives[1].entry.join_order = 3;
    lives[1].from = 20;
    lives[1].until = 30;
    lives[2].entry.join_order = 3;
    lives[2].from = 40;
    lives[3].entry.join_order = 63;
    lives[3].from = 50;
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 0), 1u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 19), 1u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 20), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 35), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 40), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 50), (uint64_t{1} << 63) | 9u);
}

TEST(ScriptSlots, InvalidJoinOrderDoesNotShiftPastTheMask) {
    std::vector<bmmo::session::member_life> lives(1);
    lives[0].entry.join_order = 64;
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 10), 0u);
}

// R1: a copy runs while its slot is unbound (the module's script activates it
// with the original), so a restore of a record from before the binding puts
// back the image the last capture without the slot took, not the copy step's.
TEST(ScriptSlots, CaptureRestoreAcrossBindingUsesUnboundImageBeforeAndRecordAfter) {
    bmmo::game::script_slots slots;
    const std::vector<uint8_t> copied{1, 2, 3}, running{7, 8, 9}, changed{4, 5, 6};
    slots.add(123, 3, copied);
    std::vector<bmmo::session::member_life> lives(1);
    lives[0].entry.join_order = 3;
    lives[0].from = 20;
    const auto capture = [&](uint32_t tick, const std::vector<uint8_t>& live) {   // as script_replay::capture_behavior
        slots.select(bmmo::session::bound_member_slots(lives, tick));
        if (!slots.omit(123)) return live;
        EXPECT_TRUE(slots.unbound(123));
        slots.refresh(123, live);
        return std::vector<uint8_t>{};
    };
    const auto before = capture(19, running);
    const uint64_t before_mask = slots.mask();
    const auto after = capture(20, changed);
    const uint64_t after_mask = slots.mask();
    EXPECT_TRUE(before.empty());
    EXPECT_EQ(after, changed);
    EXPECT_FALSE(slots.unbound(123));
    std::vector<uint8_t> live = changed;
    const auto restore = [&](uint64_t mask, const std::vector<uint8_t>& record) {
        slots.select_record(mask);
        if (slots.omit(123))
            return slots.restore_anchor(123, [&](const std::vector<uint8_t>& bytes) { live = bytes; return true; });
        slots.recorded(123);
        live = record;
        return true;
    };
    ASSERT_TRUE(restore(before_mask, before));
    EXPECT_EQ(live, running);
    ASSERT_TRUE(restore(after_mask, after));
    EXPECT_EQ(live, changed);
    ASSERT_TRUE(restore(before_mask, before));
    EXPECT_EQ(live, running);
    EXPECT_EQ(capture(19, live), before);
    EXPECT_EQ(capture(20, changed), after);
}

TEST(ScriptSlots, UnchangedAnchorsAreNotRewrittenAndFailedWritesAreRetried) {
    bmmo::game::script_slots slots;
    slots.add(123, 2, {7});
    int writes = 0;
    const auto write = [&](const std::vector<uint8_t>&) { ++writes; return true; };
    ASSERT_TRUE(slots.restore_anchor(123, write));
    EXPECT_EQ(writes, 0);
    slots.recorded(123);
    EXPECT_FALSE(slots.restore_anchor(123, [](const std::vector<uint8_t>&) { return false; }));
    ASSERT_TRUE(slots.restore_anchor(123, write));
    ASSERT_TRUE(slots.restore_anchor(123, write));
    EXPECT_EQ(writes, 1);
    EXPECT_FALSE(slots.restore_anchor(999, write));
}

TEST(ScriptSlots, PortableRecordRetainsHistoricallyBoundSlotsForLateJoiners) {
    bmmo::game::script_slots slots;
    slots.add(123, 5, {1});
    slots.add(124, 6, {2});
    slots.learn_mask(uint64_t{1} << 5);
    slots.select(1);
    EXPECT_FALSE(slots.omit(123));
    EXPECT_TRUE(slots.omit(124));
    EXPECT_FALSE(slots.omit(999));
    EXPECT_EQ(slots.mask(), (uint64_t{1} << 5) | 1u);
    slots.select_record(0);
    EXPECT_TRUE(slots.omit(123));
    EXPECT_TRUE(slots.omit(124));
    slots.select(1);
    EXPECT_FALSE(slots.omit(123));
    slots.clear();
    slots.add(123, 5, {1});
    slots.select(1);
    EXPECT_TRUE(slots.omit(123));
}
