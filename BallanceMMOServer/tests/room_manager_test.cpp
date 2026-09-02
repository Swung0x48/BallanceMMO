// Unit tests for the pure room bookkeeping (no networking):
// membership, ready gating, host migration, capacity, close.
#include <gtest/gtest.h>

#include "../room/room_manager.hpp"

using bmmo::room::error_code;
using bmmo::room::mode;
using bmmo::room::phase;

TEST(RoomManager, CreateAndJoin) {
    bmmo::room_manager rm;
    rm.max_rooms = 4;
    rm.max_members = 3;

    uint32_t id = 0;
    EXPECT_EQ(rm.create(10, "alpha", id), error_code::None);
    EXPECT_EQ(id, 1u);
    EXPECT_EQ(rm.room_of(10), 1u);
    ASSERT_NE(rm.find(1), nullptr);
    EXPECT_EQ(rm.find(1)->host, 10u);

    uint32_t id2 = 0;
    EXPECT_EQ(rm.create(10, "x", id2), error_code::AlreadyInRoom);

    EXPECT_EQ(rm.join(11, 1), error_code::None);
    EXPECT_EQ(rm.join(12, 1), error_code::None);
    EXPECT_EQ(rm.join(13, 1), error_code::Full);          // capacity is 3
    EXPECT_EQ(rm.join(11, 1), error_code::AlreadyInRoom);
    EXPECT_EQ(rm.join(14, 99), error_code::NotFound);
    EXPECT_EQ(rm.find(1)->members.size(), 3u);
}

TEST(RoomManager, ReadyGatingAndStart) {
    bmmo::room_manager rm;
    uint32_t id = 0;
    rm.create(10, "a", id);
    rm.join(11, id);

    EXPECT_EQ(rm.start(10, mode::Shadow), error_code::NotReady);
    rm.set_ready(10, true);
    EXPECT_EQ(rm.start(10, mode::Shadow), error_code::NotReady); // 11 not ready
    rm.set_ready(11, true);
    EXPECT_EQ(rm.start(11, mode::Shadow), error_code::NotHost);
    EXPECT_EQ(rm.start(10, mode::Shadow), error_code::None);
    EXPECT_EQ(rm.find(id)->phase, phase::Running);

    EXPECT_EQ(rm.end_session(11), error_code::NotHost);
    EXPECT_EQ(rm.end_session(10), error_code::None);
    EXPECT_EQ(rm.find(id)->phase, phase::Lobby);
    EXPECT_FALSE(rm.find(id)->all_ready());  // ready cleared on end
}

TEST(RoomManager, KickAndHostMigration) {
    bmmo::room_manager rm;
    uint32_t id = 0;
    rm.create(10, "a", id);
    rm.join(11, id);
    rm.join(12, id);

    bmmo::room_manager::removal_result rr;
    EXPECT_EQ(rm.kick(11, 12, rr), error_code::NotHost);
    EXPECT_EQ(rm.kick(10, 999, rr), error_code::NotFound);
    EXPECT_EQ(rm.kick(10, 12, rr), error_code::None);
    EXPECT_TRUE(rr.was_member);
    EXPECT_FALSE(rr.room_closed);
    EXPECT_EQ(rm.room_of(12), 0u);
    EXPECT_EQ(rm.find(id)->members.size(), 2u);

    // host leaves -> the next member in join order becomes host
    auto lr = rm.leave(10);
    EXPECT_TRUE(lr.was_member);
    EXPECT_FALSE(lr.room_closed);
    EXPECT_EQ(lr.new_host, 11u);
    EXPECT_EQ(rm.find(id)->host, 11u);

    // last member leaves -> room destroyed
    auto lr2 = rm.leave(11);
    EXPECT_TRUE(lr2.room_closed);
    EXPECT_EQ(rm.find(id), nullptr);
}

TEST(RoomManager, CapacityAndClose) {
    bmmo::room_manager rm;
    rm.max_rooms = 4;
    rm.max_members = 8;

    uint32_t ra = 0, rb = 0, rc = 0, rd = 0, re = 0;
    EXPECT_EQ(rm.create(20, "a", ra), error_code::None);
    EXPECT_EQ(rm.create(21, "b", rb), error_code::None);
    EXPECT_EQ(rm.create(22, "c", rc), error_code::None);
    EXPECT_EQ(rm.create(23, "d", rd), error_code::None);
    EXPECT_EQ(rm.create(24, "e", re), error_code::ServerBusy);  // over cap

    EXPECT_EQ(rm.join(25, ra), error_code::None);

    std::vector<HSteamNetConnection> members;
    uint32_t closed_room = 0;
    EXPECT_EQ(rm.close(25, members, closed_room), error_code::NotHost);
    EXPECT_EQ(rm.close(20, members, closed_room), error_code::None);
    EXPECT_EQ(closed_room, ra);
    EXPECT_EQ(members.size(), 2u);
    EXPECT_EQ(rm.find(ra), nullptr);
    EXPECT_EQ(rm.room_of(20), 0u);
    EXPECT_EQ(rm.room_of(25), 0u);

    // a slot is free again
    uint32_t rf = 0;
    EXPECT_EQ(rm.create(24, "f", rf), error_code::None);
}
