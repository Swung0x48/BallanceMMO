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

namespace {
    std::vector<uint8_t> delta_round_trip(const std::vector<uint8_t>& base, const std::vector<uint8_t>& image,
                                          size_t* bytes = nullptr) {
        std::vector<uint8_t> out{0xee};   // a byte before the delta: positions are relative
        EXPECT_TRUE(bmmo::game::script_slots::put_delta(base, image, out));
        if (bytes) *bytes = out.size() - 1;
        size_t pos = 1, skipped = 1;
        std::vector<uint8_t> back;
        EXPECT_TRUE(bmmo::game::script_slots::get_delta(base, out, pos, back));
        EXPECT_EQ(pos, out.size());
        EXPECT_TRUE(bmmo::game::script_slots::skip_delta(out, skipped));
        EXPECT_EQ(skipped, out.size());
        return back;
    }
}

// R1: an unbound copy's image against its copy-step base.
TEST(ScriptSlots, DeltaRoundTrips) {
    std::vector<uint8_t> base(300);
    for (size_t i = 0; i < base.size(); ++i) base[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> out;
    EXPECT_FALSE(bmmo::game::script_slots::put_delta(base, base, out));
    EXPECT_TRUE(out.empty());
    auto few = base;
    few[40] ^= 1;
    few[43] ^= 2;   // closer than a run header: one run
    few[200] ^= 4;
    size_t bytes = 0;
    EXPECT_EQ(delta_round_trip(base, few, &bytes), few);
    EXPECT_LT(bytes, 30u);
    auto edges = base;
    edges[0] ^= 1;
    edges.back() ^= 1;
    EXPECT_EQ(delta_round_trip(base, edges), edges);
    auto longer = base;
    longer.push_back(9);
    EXPECT_EQ(delta_round_trip(base, longer), longer);
    const std::vector<uint8_t> shorter(base.begin(), base.begin() + 10);
    EXPECT_EQ(delta_round_trip(base, shorter), shorter);
    auto all = base;
    for (auto& b: all) b ^= 0xff;
    EXPECT_EQ(delta_round_trip(base, all), all);
    std::vector<uint8_t> truncated;
    ASSERT_TRUE(bmmo::game::script_slots::put_delta(base, few, truncated));
    truncated.pop_back();
    size_t pos = 0;
    std::vector<uint8_t> back;
    EXPECT_FALSE(bmmo::game::script_slots::get_delta(base, truncated, pos, back));
}

// R1: records captured before a bind, with the copy unchanged (B) and
// changed (I2), and after it (I3) restore to B / I2 / I3 whatever was
// restored before: an unbound copy is written base + delta every time.
TEST(ScriptSlots, RestoreAcrossBindingWritesTheRecordsImage) {
    bmmo::game::script_slots slots;
    const std::vector<uint8_t> B{1, 2, 3, 4}, I2{1, 9, 3, 4}, I3{5, 6, 7, 8, 9};
    slots.add(123, 3, B);
    std::vector<bmmo::session::member_life> lives(1);
    lives[0].entry.join_order = 3;
    lives[0].from = 20;
    struct record { uint64_t mask; std::vector<uint8_t> section, body; };
    const auto capture = [&](uint32_t tick, const std::vector<uint8_t>& live) {   // as capture_behavior
        slots.select(bmmo::session::bound_member_slots(lives, tick));
        record r{slots.mask(), {}, {}};
        if (!slots.omit(123)) r.body = live;
        else bmmo::game::script_slots::put_delta(*slots.base(123), live, r.section);
        return r;
    };
    const record r1 = capture(18, B), r2 = capture(19, I2), r3 = capture(20, I3);
    EXPECT_TRUE(r1.section.empty());
    EXPECT_FALSE(r2.section.empty());
    EXPECT_EQ(r3.body, I3);
    std::vector<uint8_t> live = I3;
    const auto restore = [&](const record& r) {   // as restore_behavior
        slots.select_record(r.mask);
        if (!slots.omit(123)) {
            live = r.body;
            return;
        }
        if (r.section.empty()) {
            live = *slots.base(123);
            return;
        }
        size_t pos = 0;
        ASSERT_TRUE(bmmo::game::script_slots::get_delta(*slots.base(123), r.section, pos, live));
    };
    for (const auto* order: {"123", "321", "213", "1313", "2121", "3232"}) {
        for (const char* c = order; *c; ++c) {
            const record& r = *c == '1' ? r1 : *c == '2' ? r2 : r3;
            restore(r);
            EXPECT_EQ(live, *c == '1' ? B : *c == '2' ? I2 : I3) << order;
        }
    }
    EXPECT_EQ(slots.base_hash(123), bmmo::game::script_slots::hash(B));
    slots.add(123, 3, I2);   // a re-copy: the base is replaced
    EXPECT_EQ(*slots.base(123), I2);
    EXPECT_EQ(slots.base_hash(123), bmmo::game::script_slots::hash(I2));
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
