// Unit tests for the pure room bookkeeping (no networking):
// membership, ready gating, host migration, capacity, close, and the host's
// settings (the room's map and kind) with their wire form.
#include <gtest/gtest.h>

#include <message/room_event_msg.hpp>
#include <message/room_request_msg.hpp>
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

    std::vector<bmmo::net::connection> members;
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

// The room's map and kind are its host's to set, and only while it waits.
TEST(RoomManager, HostSettingsWhileWaiting) {
    bmmo::room_manager rm;
    uint32_t id = 0;
    ASSERT_EQ(rm.create(30, "s", id), error_code::None);
    ASSERT_EQ(rm.join(31, id), error_code::None);
    bmmo::map level3{.type = bmmo::map_type::OriginalLevel, .level = 3};
    bmmo::hex_chars_from_string(level3.md5, bmmo::map::original_map_hashes[3]);

    EXPECT_FALSE(bmmo::room::map_set(rm.find(id)->map));          // none until set
    EXPECT_EQ(rm.set_map(31, level3), error_code::NotHost);
    EXPECT_EQ(rm.set_mode(31, mode::Physics), error_code::NotHost);
    EXPECT_EQ(rm.set_map(32, level3), error_code::NotInRoom);
    EXPECT_EQ(rm.set_map(30, level3), error_code::None);
    EXPECT_EQ(rm.set_mode(30, mode::Physics), error_code::None);
    EXPECT_TRUE(rm.find(id)->map == level3);
    EXPECT_EQ(rm.find(id)->mode, mode::Physics);
    EXPECT_EQ(rm.set_map(30, bmmo::map{}), error_code::None);      // the host is in no level: cleared
    EXPECT_FALSE(bmmo::room::map_set(rm.find(id)->map));

    ASSERT_EQ(rm.set_map(30, level3), error_code::None);
    rm.set_ready(30, true);
    rm.set_ready(31, true);
    ASSERT_EQ(rm.start(30, mode::Physics), error_code::None);
    EXPECT_EQ(rm.set_map(30, bmmo::map{}), error_code::SessionRunning);
    EXPECT_EQ(rm.set_mode(30, mode::Shadow), error_code::SessionRunning);
    EXPECT_TRUE(rm.find(id)->map == level3);                       // a running session keeps its own
    rm.reset_session(id);
    EXPECT_EQ(rm.set_mode(30, mode::Shadow), error_code::None);    // waiting again
    EXPECT_TRUE(rm.find(id)->map == level3);                       // and the setting stays

    // A map a Start took from its host (none was set) is that session's only.
    rm.find(id)->map_taken = true;
    rm.reset_session(id);
    EXPECT_FALSE(bmmo::room::map_set(rm.find(id)->map));
}

// The map rides at the end of both messages, so a peer from before it reads
// the rest unchanged and its own shorter message reads as "no map".
TEST(RoomManager, MapOnTheWire) {
    bmmo::map level3{.type = bmmo::map_type::OriginalLevel, .level = 3};
    bmmo::hex_chars_from_string(level3.md5, bmmo::map::original_map_hashes[3]);

    bmmo::room_request_msg request;
    request.action = bmmo::room::action::SetMap;
    request.map = level3;
    ASSERT_TRUE(request.serialize());
    bmmo::room_request_msg read_request;
    const std::string request_bytes = request.raw.str();
    read_request.raw.write(request_bytes.data(), static_cast<std::streamsize>(request_bytes.size()));
    ASSERT_TRUE(read_request.deserialize());
    EXPECT_EQ(read_request.action, bmmo::room::action::SetMap);
    EXPECT_TRUE(read_request.map == level3);
    EXPECT_EQ(read_request.map.level, 3);

    bmmo::room_state_msg state;
    state.own_room = 7;
    state.own_map = level3;
    ASSERT_TRUE(state.serialize());
    const std::string bytes = state.raw.str();
    bmmo::room_state_msg read_state;
    read_state.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(read_state.deserialize());
    EXPECT_TRUE(bmmo::room::map_set(read_state.own_map));
    EXPECT_TRUE(read_state.own_map == level3);

    // The denial a running session gives is one the event's reader takes.
    bmmo::room_event_msg denied;
    denied.type = bmmo::room::event_type::RequestDenied;
    denied.error = error_code::SessionRunning;
    ASSERT_TRUE(denied.serialize());
    const std::string denied_bytes = denied.raw.str();
    bmmo::room_event_msg read_denied;
    read_denied.raw.write(denied_bytes.data(), static_cast<std::streamsize>(denied_bytes.size()));
    ASSERT_TRUE(read_denied.deserialize());
    EXPECT_EQ(read_denied.error, error_code::SessionRunning);

    // An older server's state ends before the map.
    const size_t map_bytes = sizeof(uint8_t) + sizeof(level3.level) + sizeof(level3.md5);
    bmmo::room_state_msg old_state;
    old_state.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - map_bytes));
    ASSERT_TRUE(old_state.deserialize());
    EXPECT_EQ(old_state.own_room, 7u);
    EXPECT_FALSE(bmmo::room::map_set(old_state.own_map));
}

// A running session is restarted by its host once every member has voted.
TEST(RoomManager, RestartByVote) {
    bmmo::room_manager rm;
    uint32_t id = 0;
    ASSERT_EQ(rm.create(40, "v", id), error_code::None);
    ASSERT_EQ(rm.join(41, id), error_code::None);
    const auto everyone = [](bmmo::net::connection) { return true; };
    EXPECT_EQ(rm.set_vote(40, true), error_code::NoSession);       // nothing runs yet
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::NoSession);
    rm.set_ready(40, true);
    rm.set_ready(41, true);
    ASSERT_EQ(rm.start(40, mode::Physics), error_code::None);

    EXPECT_EQ(rm.can_restart(41, everyone), error_code::NotHost);
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::NotReady);           // no votes yet
    EXPECT_EQ(rm.set_vote(40, true), error_code::None);
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::NotReady);           // one of two
    // A member out of the session (it joined the room late, or finished) is not waited for.
    EXPECT_EQ(rm.can_restart(40, [](bmmo::net::connection c) { return c == 40; }), error_code::None);
    EXPECT_EQ(rm.set_vote(41, true), error_code::None);
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::None);
    EXPECT_EQ(rm.set_vote(41, false), error_code::None);           // withdrawn
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::NotReady);
    EXPECT_EQ(rm.set_vote(41, true), error_code::None);
    EXPECT_EQ(rm.set_vote(42, true), error_code::NotInRoom);

    ASSERT_EQ(rm.start(40, mode::Physics), error_code::None);      // the restart: a start
    EXPECT_EQ(rm.can_restart(40, everyone), error_code::NotReady);           // whose session has no votes yet

    // The vote and the session membership share the roster's flags byte with the ready mark.
    bmmo::room_state_msg state;
    state.members.push_back({.id = 1, .name = "a", .ready = true, .vote = false, .in_session = false});
    state.members.push_back({.id = 2, .name = "b", .ready = false, .vote = true, .in_session = true});
    ASSERT_TRUE(state.serialize());
    const std::string bytes = state.raw.str();
    bmmo::room_state_msg read;
    read.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(read.deserialize());
    ASSERT_EQ(read.members.size(), 2u);
    EXPECT_TRUE(read.members[0].ready);
    EXPECT_FALSE(read.members[0].vote);
    EXPECT_FALSE(read.members[1].ready);
    EXPECT_TRUE(read.members[1].vote);
    EXPECT_FALSE(read.members[0].in_session);
    EXPECT_TRUE(read.members[1].in_session);
}
