// Unit tests for the collision-overhaul physics session wire messages
// (docs/rooms-and-sessions-protocol.md 2.2): serialize()/deserialize()
// round trips, every truncation of a valid payload, and forged element
// counts a malicious peer could send (both above the hard cap and larger
// than what the remaining bytes could possibly hold).
#include <message/session_clock_ack_msg.hpp>
#include <message/session_start_at_msg.hpp>
#include <message/session_start_status_msg.hpp>
#include <message/session_assign_msg.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

// the individual message headers are not self-contained (they rely on the
// include order in message_all.hpp), so pull in the aggregate
#include <message/message_all.hpp>
#include <message/message_utils.hpp>
#include <session/lifecycle.hpp>
#include <session/event_piggyback.hpp>

#include <cstdio>

namespace {

std::string stream_payload(std::stringstream& stream) {
    return stream.str();
}

// serializable_message holds a std::stringstream, which is not
// copy-constructible, so these helpers take the message by (mutating)
// reference rather than by value; pass a named local, not a temporary.

template <typename Msg>
Msg round_trip(Msg& msg) {
    EXPECT_TRUE(msg.serialize());
    Msg parsed{};
    parsed.raw.write(stream_payload(msg.raw).data(), static_cast<std::streamsize>(msg.size()));
    EXPECT_TRUE(parsed.deserialize());
    return parsed;
}

template <typename Msg>
void expect_rejects_every_truncation(Msg& msg) {
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);
    for (size_t len = 0; len < payload.size(); ++len) {
        Msg parsed{};
        parsed.raw.write(payload.data(), static_cast<std::streamsize>(len));
        EXPECT_FALSE(parsed.deserialize()) << "accepted a payload truncated to " << len << " bytes";
    }
}

} // namespace

TEST(LoginAcceptedV4Msg, SerializeDeserializeRoundTrip) {
    bmmo::login_accepted_v4_msg msg;
    bmmo::map player_map{};
    player_map.type = bmmo::map_type::OriginalLevel;
    player_map.level = 8;
    for (size_t index = 0; index < sizeof(player_map.md5); ++index)
        player_map.md5[index] = static_cast<uint8_t>(index + 1);
    msg.online_players.emplace(10, bmmo::player_status_v3{"Alice", 0, player_map, 3});
    msg.online_players.emplace(42, bmmo::player_status_v3{"Bob", 1, {}, 0});
    msg.server_info = {"4.0.0-alpha4", false, "physics disabled in the config", 27, true, 8, 1, true, false};

    auto parsed = round_trip(msg);

    EXPECT_EQ(bmmo::LoginAcceptedV4, parsed.code);
    ASSERT_EQ(msg.online_players.size(), parsed.online_players.size());
    for (const auto& [id, player]: msg.online_players) {
        ASSERT_TRUE(parsed.online_players.contains(id));
        const auto& actual = parsed.online_players.at(id);
        EXPECT_EQ(player.name, actual.name);
        EXPECT_EQ(player.cheated, actual.cheated);
        EXPECT_EQ(player.map.type, actual.map.type);
        EXPECT_EQ(player.map.level, actual.map.level);
        EXPECT_EQ(0, std::memcmp(player.map.md5, actual.map.md5, sizeof(player.map.md5)));
        EXPECT_EQ(player.sector, actual.sector);
    }
    EXPECT_EQ(msg.server_info.version, parsed.server_info.version);
    EXPECT_EQ(msg.server_info.physics_available, parsed.server_info.physics_available);
    EXPECT_EQ(msg.server_info.physics_unavailable_reason, parsed.server_info.physics_unavailable_reason);
    EXPECT_EQ(msg.server_info.physics_protocol, parsed.server_info.physics_protocol);
    EXPECT_EQ(msg.server_info.rooms_enabled, parsed.server_info.rooms_enabled);
    EXPECT_EQ(msg.server_info.maximum_members, parsed.server_info.maximum_members);
    EXPECT_EQ(msg.server_info.maximum_physics_rooms, parsed.server_info.maximum_physics_rooms);
    EXPECT_EQ(msg.server_info.trafo_inputs, parsed.server_info.trafo_inputs);
    EXPECT_EQ(msg.server_info.placement_clones, parsed.server_info.placement_clones);
}

// ---------------------------------------------------------------------
// session_start_msg
// ---------------------------------------------------------------------

TEST(SessionStartMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_start_msg msg{};
    msg.room = 7;
    msg.session = 3;
    msg.mode = bmmo::room::mode::Physics;
    msg.map.type = bmmo::map_type::OriginalLevel;
    msg.map.level = 1;
    for (int i = 0; i < 16; ++i) msg.map.md5[i] = static_cast<uint8_t>(i + 1);
    msg.tick_rate = 66;
    msg.snapshot_interval = 2;
    msg.input_delay = 6;
    msg.sector_lead = 15;
    msg.first_tick = 132;
    msg.seed = -42;
    msg.spawn_impulse = 2.5f;
    msg.spawn_spacing = 4.2f;

    bmmo::session::player_entry p0{};
    p0.id = 10; p0.join_order = 0; p0.ball_type = 1;
    p0.spawn_position[0] = 1.f; p0.spawn_position[1] = 2.f; p0.spawn_position[2] = 3.f;
    p0.spawn_rotation[0] = 0.f; p0.spawn_rotation[1] = 0.f; p0.spawn_rotation[2] = 0.f; p0.spawn_rotation[3] = 1.f;
    bmmo::session::player_entry p1{};
    p1.id = 11; p1.join_order = 1; p1.ball_type = 2;
    p1.spawn_position[0] = -1.f; p1.spawn_position[1] = -2.f; p1.spawn_position[2] = -3.f;
    p1.spawn_rotation[0] = 0.1f; p1.spawn_rotation[1] = 0.2f; p1.spawn_rotation[2] = 0.3f; p1.spawn_rotation[3] = 0.9f;
    msg.players = {p0, p1};

    auto parsed = round_trip(msg);

    EXPECT_EQ(7u, parsed.room);
    EXPECT_EQ(3u, parsed.session);
    EXPECT_EQ(bmmo::room::mode::Physics, parsed.mode);
    EXPECT_EQ(bmmo::map_type::OriginalLevel, parsed.map.type);
    EXPECT_EQ(1, parsed.map.level);
    EXPECT_EQ(0, std::memcmp(msg.map.md5, parsed.map.md5, sizeof(msg.map.md5)));
    EXPECT_EQ(66, parsed.tick_rate);
    EXPECT_EQ(2, parsed.snapshot_interval);
    EXPECT_EQ(6, parsed.input_delay);
    EXPECT_EQ(15, parsed.sector_lead);
    EXPECT_EQ(132u, parsed.first_tick);
    EXPECT_EQ(-42, parsed.seed);
    EXPECT_FLOAT_EQ(2.5f, parsed.spawn_impulse);
    EXPECT_FLOAT_EQ(4.2f, parsed.spawn_spacing);
    EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, parsed.protocol);
    EXPECT_EQ(0u, parsed.world_options);
    ASSERT_EQ(2u, parsed.players.size());
    EXPECT_EQ(10u, parsed.players[0].id);
    EXPECT_EQ(0, parsed.players[0].join_order);
    EXPECT_EQ(1, parsed.players[0].ball_type);
    EXPECT_FLOAT_EQ(1.f, parsed.players[0].spawn_position[0]);
    EXPECT_FLOAT_EQ(3.f, parsed.players[0].spawn_position[2]);
    EXPECT_FLOAT_EQ(1.f, parsed.players[0].spawn_rotation[3]);
    EXPECT_EQ(11u, parsed.players[1].id);
    EXPECT_EQ(1, parsed.players[1].join_order);
    EXPECT_FLOAT_EQ(-2.f, parsed.players[1].spawn_position[1]);
    EXPECT_FLOAT_EQ(0.9f, parsed.players[1].spawn_rotation[3]);
}

// Protocol 19 (world agreement step Q7): the mechanism slot count after the
// world options; under WORLD_PLAYER_MECHANISMS a count of 0 or above
// MAX_PLAYERS_PER_SESSION does not parse.
TEST(SessionStartMsg, MechanismSlots) {
    const auto fill = [](bmmo::session_start_msg& msg, uint32_t options, uint8_t slots) {
        msg.mode = bmmo::room::mode::Physics;
        msg.world_options = options;
        msg.mechanism_slots = slots;
        bmmo::session::player_entry p{};
        p.id = 5; p.join_order = 3; p.life = 2;
        msg.players = {p};
    };
    const uint32_t on = bmmo::session::WORLD_PROXIMITY_UNION | bmmo::session::WORLD_PLAYER_MECHANISMS;
    bmmo::session_start_msg msg{};
    fill(msg, on, 8);
    auto parsed = round_trip(msg);
    EXPECT_EQ(8u, parsed.mechanism_slots);
    ASSERT_EQ(1u, parsed.players.size());
    EXPECT_EQ(3, parsed.players[0].join_order);
    EXPECT_EQ(2u, parsed.players[0].life);
    for (const uint8_t bad: {uint8_t(0), uint8_t(65)}) {
        bmmo::session_start_msg b{};
        fill(b, on, bad);
        ASSERT_TRUE(b.serialize());
        bmmo::session_start_msg out{};
        out.raw.write(stream_payload(b.raw).data(), static_cast<std::streamsize>(b.size()));
        EXPECT_FALSE(out.deserialize()) << static_cast<unsigned>(bad);
    }
    bmmo::session_start_msg off{};
    fill(off, bmmo::session::WORLD_PROXIMITY_UNION, 0);
    EXPECT_EQ(0u, round_trip(off).mechanism_slots) << "without the bit a 0 parses";
}

// U7 (record-format-gaps-design.md 7.1; step RK3, protocol 20): the world
// option WORLD_SESSION_CLOCK (bit 3) round-trips with the others.
TEST(SessionStartMsg, SessionClockBitRoundTrips) {
    EXPECT_EQ(1u << 3, bmmo::session::WORLD_SESSION_CLOCK);
    EXPECT_EQ(37u, bmmo::session::PHYSICS_PROTOCOL_VERSION);
    bmmo::session_start_msg msg{};
    msg.mode = bmmo::room::mode::Physics;
    msg.world_options = bmmo::session::WORLD_SESSION_CLOCK | bmmo::session::WORLD_PLACEMENT_CLONES;
    bmmo::session::player_entry p{};
    p.id = 5; p.join_order = 0; p.life = 1;
    msg.players = {p};
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::WORLD_SESSION_CLOCK | bmmo::session::WORLD_PLACEMENT_CLONES, parsed.world_options);
}

TEST(SessionStartMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_start_msg msg{};
    msg.room = 1; msg.session = 1;
    msg.mode = bmmo::room::mode::Physics;
    msg.map.type = bmmo::map_type::CustomMap;
    for (int i = 0; i < 16; ++i) msg.map.md5[i] = static_cast<uint8_t>(i);
    msg.tick_rate = 66; msg.snapshot_interval = 2; msg.input_delay = 6;
    msg.first_tick = 0; msg.seed = 1;
    bmmo::session::player_entry p{};
    p.id = 1;
    msg.players = {p};
    expect_rejects_every_truncation(msg);
}

TEST(SessionStartMsg, DeserializeRejectsPlayerCountAboveCap) {
    bmmo::session_start_msg msg{};
    msg.players.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - sizeof(uint8_t)); // drop the count(0) byte
    const uint8_t forged_count = static_cast<uint8_t>(bmmo::session::MAX_PLAYERS_PER_SESSION + 1);
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

    bmmo::session_start_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
    EXPECT_TRUE(parsed.players.empty());
}

TEST(SessionStartMsg, DeserializeRejectsPlayerCountLargerThanRemaining) {
    bmmo::session_start_msg msg{};
    msg.players.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - sizeof(uint8_t));
    const uint8_t forged_count = 5; // within the cap, but no player data follows
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

    bmmo::session_start_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

TEST(SessionStartMsg, DeserializeRejectsInvalidMode) {
    bmmo::session_start_msg msg{};
    msg.mode = bmmo::room::mode::Shadow;
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    const size_t mode_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2; // room, session
    payload[mode_offset] = static_cast<char>(2); // > Physics(1)

    bmmo::session_start_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

// ---------------------------------------------------------------------
// session_end_msg
// ---------------------------------------------------------------------

TEST(SessionEndMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_end_msg msg{};
    msg.session = 42;
    msg.reason = "anchor hash mismatch";

    auto parsed = round_trip(msg);
    EXPECT_EQ(42u, parsed.session);
    EXPECT_EQ("anchor hash mismatch", parsed.reason);
}

TEST(SessionEndMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_end_msg msg{};
    msg.session = 1;
    msg.reason = "host closed the room";
    expect_rejects_every_truncation(msg);
}

// ---------------------------------------------------------------------
// session_ready_msg
// ---------------------------------------------------------------------

TEST(SessionReadyMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.first_tick = 0;
    msg.anchor_hash = 0x0123456789abcdefULL;
    msg.anchor_surfaces = 0xfedcba9876543210ULL;
    msg.physics_sha256 = "deadbeef";
    msg.build_id = "collision-overhaul-m3";

    auto parsed = round_trip(msg);
    EXPECT_EQ(3u, parsed.session);
    EXPECT_EQ(0u, parsed.first_tick);
    EXPECT_EQ(0x0123456789abcdefULL, parsed.anchor_hash);
    EXPECT_EQ(0xfedcba9876543210ULL, parsed.anchor_surfaces);
    EXPECT_EQ("deadbeef", parsed.physics_sha256);
    EXPECT_EQ("collision-overhaul-m3", parsed.build_id);
}

TEST(SessionReadyMsg, CarriesTheCapsFromProtocol8) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.caps = bmmo::session::CAP_LEAD;
    msg.build_id = "b";
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, parsed.protocol);
    EXPECT_EQ(bmmo::session::CAP_LEAD, parsed.caps);
}

// A protocol 7 client sends no caps: its message still parses (caps 0), so
// the server can say which protocol it runs instead of dropping it.
TEST(SessionReadyMsg, AnOlderProtocolParsesWithoutCaps) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.protocol = 7;
    msg.caps = bmmo::session::CAP_LEAD;
    msg.build_id = "b";
    ASSERT_TRUE(msg.serialize());
    std::string bytes = stream_payload(msg.raw);
    bytes.resize(bytes.size() - sizeof(uint32_t) - sizeof(uint64_t));   // what a protocol 7 build wrote
    bmmo::session_ready_msg parsed{};
    parsed.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(7u, parsed.protocol);
    EXPECT_EQ(0u, parsed.caps);
}

TEST(SessionReadyMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.anchor_hash = 1;
    msg.anchor_surfaces = 2;
    msg.physics_sha256 = "abc123";
    msg.build_id = "beta18";
    expect_rejects_every_truncation(msg);
}

// ---------------------------------------------------------------------
// session_lead_msg / session_lead_ack_msg (late-player design 2.2)
// ---------------------------------------------------------------------

namespace {
    bmmo::session_lead_msg make_lead() {
        bmmo::session_lead_msg msg{};
        msg.session = 4;
        msg.player = 77;
        msg.seq = 3;
        msg.delta = 12;
        msg.held_from = 1000;
        msg.effective = 1046;
        msg.held.keys = bmmo::session::KEY_LEAF_2;
        msg.held.cam_right[0] = 0.5f;
        msg.held.cam_up[1] = -1.f;
        msg.held.cam_dir[2] = 0.25f;
        msg.held.ball_type = 2;
        msg.held.flags = bmmo::session::INPUT_FLAG_PHYSICALIZED;
        msg.held_source = 994;
        msg.epoch = 640;
        return msg;
    }
}

TEST(SessionLeadMsg, SerializeDeserializeRoundTrip) {
    auto msg = make_lead();
    auto parsed = round_trip(msg);
    EXPECT_EQ(4u, parsed.session);
    EXPECT_EQ(77u, parsed.player);
    EXPECT_EQ(3u, parsed.seq);
    EXPECT_EQ(12, parsed.delta);
    EXPECT_EQ(1000u, parsed.held_from);
    EXPECT_EQ(1046u, parsed.effective);
    EXPECT_EQ(bmmo::session::KEY_LEAF_2, parsed.held.keys);
    EXPECT_EQ(0.5f, parsed.held.cam_right[0]);
    EXPECT_EQ(-1.f, parsed.held.cam_up[1]);
    EXPECT_EQ(0.25f, parsed.held.cam_dir[2]);
    EXPECT_EQ(2, parsed.held.ball_type);
    EXPECT_EQ(bmmo::session::INPUT_FLAG_PHYSICALIZED, parsed.held.flags);
    EXPECT_EQ(994u, parsed.held_source);
    EXPECT_EQ(640u, parsed.epoch);
}

TEST(SessionLeadMsg, ALowerCarriesNoRange) {
    bmmo::session_lead_msg msg{};
    msg.session = 4;
    msg.player = 1;
    msg.seq = 9;
    msg.delta = -5;
    msg.effective = 2000;
    auto parsed = round_trip(msg);
    EXPECT_EQ(-5, parsed.delta);
    EXPECT_EQ(0u, parsed.held_from);
    EXPECT_EQ(2000u, parsed.effective);
    EXPECT_EQ(bmmo::session::INPUT_SOURCE_DEFAULT, parsed.held_source);
}

TEST(SessionLeadMsg, DeserializeRejectsEveryTruncationAndBadFields) {
    auto msg = make_lead();
    expect_rejects_every_truncation(msg);
    const auto parses = [](bmmo::session_lead_msg& m) {
        EXPECT_TRUE(m.serialize());
        bmmo::session_lead_msg parsed{};
        parsed.raw.write(stream_payload(m.raw).data(), static_cast<std::streamsize>(m.size()));
        return parsed.deserialize();
    };
    auto reversed = make_lead();
    reversed.effective = reversed.held_from;   // an empty range
    EXPECT_FALSE(parses(reversed));
    auto nothing = make_lead();
    nothing.delta = 0;
    EXPECT_FALSE(parses(nothing));
    auto unnamed = make_lead();
    unnamed.seq = 0;
    EXPECT_FALSE(parses(unnamed));
}

TEST(SessionLeadAckMsg, RoundTripAndTruncation) {
    bmmo::session_lead_ack_msg msg{};
    msg.session = 4;
    msg.seq = 3;
    msg.applied = -7;
    auto parsed = round_trip(msg);
    EXPECT_EQ(4u, parsed.session);
    EXPECT_EQ(3u, parsed.seq);
    EXPECT_EQ(-7, parsed.applied);
    bmmo::session_lead_ack_msg again{};
    again.session = 4;
    again.seq = 3;
    again.applied = 12;
    expect_rejects_every_truncation(again);
}

// ---------------------------------------------------------------------
// session_input_msg
// ---------------------------------------------------------------------

TEST(SessionInputMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_input_msg msg{};
    msg.session = 3;
    msg.first_tick = 100;

    bmmo::session::input_frame f0{};
    f0.keys = static_cast<uint8_t>(bmmo::session::KEY_LEAF_0 | bmmo::session::KEY_SHIFT);
    f0.cam_right[0] = 1.f; f0.cam_right[1] = 0.f; f0.cam_right[2] = 0.f;
    f0.cam_up[0] = 0.f; f0.cam_up[1] = 1.f; f0.cam_up[2] = 0.f;
    f0.cam_dir[0] = 0.f; f0.cam_dir[1] = 0.f; f0.cam_dir[2] = 1.f;
    f0.ball_type = 1;
    f0.flags = bmmo::session::INPUT_FLAG_PHYSICALIZED;

    bmmo::session::input_frame f1{};
    f1.keys = bmmo::session::KEY_LEAF_3;
    f1.cam_right[0] = -1.f;
    f1.ball_type = 2;
    f1.flags = bmmo::session::INPUT_FLAG_PAUSED;

    msg.frames = {f0, f1};

    auto parsed = round_trip(msg);
    EXPECT_EQ(3u, parsed.session);
    EXPECT_EQ(100u, parsed.first_tick);
    ASSERT_EQ(2u, parsed.frames.size());
    EXPECT_EQ(f0.keys, parsed.frames[0].keys);
    EXPECT_FLOAT_EQ(1.f, parsed.frames[0].cam_right[0]);
    EXPECT_FLOAT_EQ(1.f, parsed.frames[0].cam_up[1]);
    EXPECT_FLOAT_EQ(1.f, parsed.frames[0].cam_dir[2]);
    EXPECT_EQ(1, parsed.frames[0].ball_type);
    EXPECT_EQ(bmmo::session::INPUT_FLAG_PHYSICALIZED, parsed.frames[0].flags);
    EXPECT_EQ(f1.keys, parsed.frames[1].keys);
    EXPECT_FLOAT_EQ(-1.f, parsed.frames[1].cam_right[0]);
    EXPECT_EQ(2, parsed.frames[1].ball_type);
    EXPECT_EQ(bmmo::session::INPUT_FLAG_PAUSED, parsed.frames[1].flags);
}

// Protocol 30: cameras as heading codes against the sender's base, the base
// attached; the server's half fills them in, and cannot without the base.
TEST(SessionInputMsg, CamerasAsHeadingsWithTheBase) {
    bmmo::session_input_msg msg{};
    msg.first_tick = 200;
    const bmmo::session::camera_rows base{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    for (int k: {0, 1, 1, 3}) {
        bmmo::session::input_frame f{};
        bmmo::session::set_camera(f, bmmo::session::quarter_turns(base, k));
        f.ball_type = 2;
        msg.frames.push_back(f);
        msg.headings.push_back(static_cast<uint8_t>(k));
    }
    msg.base = bmmo::session::camera_bases::base{150, base};
    auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.size(), msg.wire_size(0, 0));
    bmmo::session::camera_bases bases;
    const auto frames = parsed.resolved_frames(bases);
    ASSERT_TRUE(frames);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(bmmo::session::same_camera((*frames)[i], msg.frames[i])) << i;
        EXPECT_EQ((*frames)[i].ball_type, 2);
    }
    parsed.base.reset();
    bmmo::session::camera_bases empty;
    EXPECT_FALSE(parsed.resolved_frames(empty));
}

TEST(SessionInputMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_input_msg msg{};
    msg.session = 1;
    msg.first_tick = 5;
    bmmo::session::input_frame f{};
    f.keys = 0x0f;
    msg.frames = {f, f};
    expect_rejects_every_truncation(msg);
}

TEST(SessionInputMsg, DeserializeRejectsFrameCountAboveCap) {
    bmmo::session_input_msg msg{};
    msg.frames.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - 2 * sizeof(uint8_t));   // count and (protocol 25) nevents
    const uint8_t forged_count = static_cast<uint8_t>(bmmo::session::MAX_INPUT_FRAMES + 1);
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));
    payload.push_back(0);   // nevents 0

    bmmo::session_input_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
    EXPECT_TRUE(parsed.frames.empty());
}

TEST(SessionInputMsg, DeserializeRejectsFrameCountLargerThanRemaining) {
    bmmo::session_input_msg msg{};
    msg.frames.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - 2 * sizeof(uint8_t));   // count and (protocol 25) nevents
    const uint8_t forged_count = 3; // within the cap, but no frame data follows (the nevents byte is read as frame data)
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));
    payload.push_back(0);

    bmmo::session_input_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

// ---------------------------------------------------------------------
// session_input_msg's piggybacked lifecycle events (protocol 25, FULL step
// EVI; late-player-design.md §3.4 option (b))
// ---------------------------------------------------------------------

namespace {

std::string event_bytes(bmmo::session::event_type type, uint32_t stamp, int32_t sector = 0) {
    bmmo::session_event_msg e{};
    e.session = 3;
    e.tick = stamp;
    e.origin_tick = stamp;
    e.type = type;
    e.sector = sector;
    if (type == bmmo::session::event_type::Physicalize) {
        // The retail ball's recipe shape: one sphere, its surface, no meshes.
        e.ball_type = 1;
        e.recipe.mass = 1.f;
        e.recipe.collision_surface = "Ball_Wood";
        e.recipe.balls = {{{0.f, 0.f, 0.f}, 2.f}};
    }
    if (type == bmmo::session::event_type::Trafo) {
        e.ball_type = 2;
        e.position[0] = 10.f;
    }
    e.serialize();
    return e.raw.str();
}

bmmo::session_input_msg input_with_frames(size_t n, uint32_t first_tick = 100) {
    bmmo::session_input_msg msg{};
    msg.session = 3;
    msg.first_tick = first_tick;
    for (size_t i = 0; i < n; ++i) {
        bmmo::session::input_frame f{};
        f.keys = static_cast<uint8_t>(i + 1);
        msg.frames.push_back(f);
    }
    return msg;
}

} // namespace

// Round trip with 0..n events: the frames as before, each event's bytes
// exactly as its reliable copy, in order; wire_size is the serialized size.
TEST(SessionInputMsg, PiggybackedEventsRoundTrip) {
    using bmmo::session::event_type;
    const std::vector<std::string> all = {
        event_bytes(event_type::Unphysicalize, 120), event_bytes(event_type::Physicalize, 120),
        event_bytes(event_type::Sector, 155, 3), event_bytes(event_type::Trafo, 156)};
    for (size_t n = 0; n <= all.size(); ++n) {
        auto msg = input_with_frames(bmmo::session::MAX_INPUT_FRAMES);
        msg.events.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
        size_t bytes = 0;
        for (const auto& e: msg.events) bytes += e.size();
        auto parsed = round_trip(msg);
        EXPECT_EQ(msg.size(), msg.wire_size(bytes, n)) << n;
        EXPECT_EQ(parsed.first_tick, 100u);
        ASSERT_EQ(parsed.frames.size(), bmmo::session::MAX_INPUT_FRAMES);
        EXPECT_EQ(parsed.frames.back().keys, static_cast<uint8_t>(bmmo::session::MAX_INPUT_FRAMES));
        ASSERT_EQ(parsed.events.size(), n);
        for (size_t i = 0; i < n; ++i) EXPECT_EQ(parsed.events[i], all[i]);
        // Every piggybacked event parses as the session_event_msg it was.
        for (const auto& b: parsed.events) {
            auto e = bmmo::message_utils::deserialize<bmmo::session_event_msg>(const_cast<char*>(b.data()),
                                                                               static_cast<int>(b.size()));
            EXPECT_EQ(e.session, 3u);
        }
    }
    // Every truncation of a datagram with events is refused.
    auto msg = input_with_frames(2);
    msg.events = {all[0], all[1]};
    expect_rejects_every_truncation(msg);
}

// The measured sizes the budget is judged against (the "As built" note
// quotes them): an 8-frame datagram with no event, and each own event type
// as the client core sends it (a retail-shaped one-sphere Physicalize).  A
// beam pair plus a Sector and a Trafo stays far under the 1100-byte budget,
// so the Trafo rides too and the lead-stamped types start at the send.
TEST(SessionInputMsg, PiggybackedEventSizesFitTheBudget) {
    using bmmo::session::event_type;
    const size_t base = input_with_frames(bmmo::session::MAX_INPUT_FRAMES).wire_size(0, 0);
    EXPECT_EQ(base, 42u) << "opcode 4 + session 4 + first_tick 4 + relay_ack 4 + count 1 + 8 x 3 + nevents 1";
    EXPECT_EQ(event_bytes(event_type::Unphysicalize, 1).size(), 24u);
    EXPECT_EQ(event_bytes(event_type::Sector, 1, 2).size(), 28u);
    EXPECT_EQ(event_bytes(event_type::Trafo, 1).size(), 38u);
    const size_t p = event_bytes(event_type::Physicalize, 1).size();
    EXPECT_GE(p, 100u) << p;
    EXPECT_LE(p, 160u) << p;
    const size_t all = base + 4 * sizeof(uint16_t) + 24 + 28 + 38 + p;
    EXPECT_LT(all, bmmo::session::kInputDatagramBudget) << all;
    std::printf("[ sizes    ] input datagram, 8 frames, no event: %zu B; U 24, Sector 28, Trafo 38, P %zu; "
                "8 frames + U + P + Sector + Trafo: %zu B (budget %zu)\n",
                base, p, all, bmmo::session::kInputDatagramBudget);
}

// "The previous protocol refused": a protocol-24 datagram ends after the
// frames (no nevents byte) and does not parse; nor does a count over
// MAX_INPUT_EVENTS, nor an event whose bytes are not a session_event_msg.
TEST(SessionInputMsg, PreviousProtocolAndForgedEventsRefused) {
    auto msg = input_with_frames(3);
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    {
        std::string old = payload.substr(0, payload.size() - 1);   // what a protocol-24 build wrote
        bmmo::session_input_msg parsed{};
        parsed.raw.write(old.data(), static_cast<std::streamsize>(old.size()));
        EXPECT_FALSE(parsed.deserialize());
    }
    {
        std::string forged = payload.substr(0, payload.size() - 1);
        forged.push_back(static_cast<char>(bmmo::session::MAX_INPUT_EVENTS + 1));
        bmmo::session_input_msg parsed{};
        parsed.raw.write(forged.data(), static_cast<std::streamsize>(forged.size()));
        EXPECT_FALSE(parsed.deserialize());
    }
    {
        auto bad = input_with_frames(1);
        bad.events = {std::string(4, static_cast<char>(1))};   // not a SessionEvent
        ASSERT_TRUE(bad.serialize());
        const std::string bytes = stream_payload(bad.raw);
        bmmo::session_input_msg parsed{};
        parsed.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        EXPECT_FALSE(parsed.deserialize());
    }
}

// The client core's piggyback list (session/event_piggyback.hpp): an event
// rides every datagram until the server's header acknowledges a frame at or
// past max(stamp, the newest frame of the first datagram of its carry)
// (protocol 33); over the budget the OLDEST event is left out of that datagram
// only, counted, and its carry starts again; unacknowledged it goes once its
// stamp is kPiggybackReachTicks behind.
TEST(EventPiggyback, UntilAcknowledgedAndBudgetDrop) {
    using bmmo::session::event_type;
    bmmo::session::event_piggyback list;
    EXPECT_TRUE(bmmo::session::event_rides_input(event_type::Physicalize));
    EXPECT_TRUE(bmmo::session::event_rides_input(event_type::Unphysicalize));
    EXPECT_TRUE(bmmo::session::event_rides_input(event_type::Sector));
    EXPECT_TRUE(bmmo::session::event_rides_input(event_type::Trafo));
    EXPECT_FALSE(bmmo::session::event_rides_input(event_type::BodyRevived));
    EXPECT_FALSE(bmmo::session::event_rides_input(event_type::Finish));

    const std::string u = event_bytes(event_type::Unphysicalize, 121);
    const std::string p = event_bytes(event_type::Physicalize, 121);
    list.push(u, 121, event_type::Unphysicalize);
    list.push(p, 121, event_type::Physicalize);
    // The server's acknowledgement short of the stamp: carried, far past
    // stamp + 7 (LBM5 swing seed 2: every datagram with the frame lost).
    for (uint32_t newest = 120; newest <= 140; ++newest) {
        list.prune(newest, 120u);
        auto msg = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, newest - 7);
        EXPECT_EQ(list.attach(msg), 0u);
        ASSERT_EQ(msg.events.size(), 2u) << newest;
        EXPECT_EQ(msg.events[0], u);
        EXPECT_EQ(msg.events[1], p);
    }
    list.prune(141, 121u);   // the stamp's frame applied: the events were taken with it
    EXPECT_EQ(list.pending(), 0u);
    EXPECT_EQ(list.counters().taken, 2u);
    EXPECT_EQ(list.counters().carried, 42u);
    // An event sent after the server passed its stamp (a held one: stamped
    // 100, sent when the newest frame is 140) rides until the
    // acknowledgement reaches the newest frame of its first datagram.
    list.push(u, 100, event_type::Unphysicalize);
    for (uint32_t newest = 140; newest <= 141; ++newest) {
        list.prune(newest, newest - 2);
        auto late = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, newest - 7);
        list.attach(late);
        EXPECT_EQ(late.events.size(), 1u) << newest;
    }
    list.prune(142, 140u);
    EXPECT_EQ(list.pending(), 0u);
    // Never acknowledged: gone once the stamp is the reach behind.
    list.push(u, 300, event_type::Unphysicalize);
    list.prune(300 + bmmo::session::kPiggybackReachTicks);
    EXPECT_EQ(list.pending(), 1u);
    list.prune(301 + bmmo::session::kPiggybackReachTicks);
    EXPECT_EQ(list.pending(), 0u);
    EXPECT_EQ(list.counters().expired, 1u);

    // The budget: two events that do not both fit; the older one is left out
    // of this datagram (it stays in the list) and counted.
    bmmo::session::event_piggyback tight;
    tight.push(u, 200, event_type::Unphysicalize);
    tight.push(p, 200, event_type::Physicalize);
    auto msg = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, 193);
    const size_t budget = msg.wire_size(p.size(), 1);
    EXPECT_EQ(tight.attach(msg, budget), 1u);
    ASSERT_EQ(msg.events.size(), 1u);
    EXPECT_EQ(msg.events[0], p);
    EXPECT_EQ(tight.counters().dropped, 1u);
    EXPECT_EQ(tight.pending(), 2u);
    EXPECT_LE(tight.counters().max_bytes, budget);
    ASSERT_TRUE(msg.serialize());
    EXPECT_LE(msg.size(), budget);
    auto roomy = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, 194);
    EXPECT_EQ(tight.attach(roomy), 0u);
    EXPECT_EQ(roomy.events.size(), 2u);
    // The one left out of the first datagram started its carry at the
    // second (newest 201): frame 200 acknowledged shows only the other taken.
    tight.prune(202, 200u);
    EXPECT_EQ(tight.pending(), 1u);
    tight.prune(202, 201u);
    EXPECT_EQ(tight.pending(), 0u);
}

// ---------------------------------------------------------------------
// session_event_msg
// ---------------------------------------------------------------------

namespace {

// The trailer every session_event_msg ends with since protocol 9:
// origin_tick u32 and state u8.
constexpr size_t kEventTrailer = sizeof(uint32_t) + sizeof(uint8_t);

bmmo::session_event_msg make_physicalize_event() {
    bmmo::session_event_msg msg{};
    msg.session = 5;
    msg.player = 20;
    msg.tick = 300;
    msg.origin_tick = 300;
    msg.type = bmmo::session::event_type::Physicalize;
    msg.ball_type = 1;
    msg.position[0] = 1.f; msg.position[1] = 2.f; msg.position[2] = 3.f;
    msg.rotation[0] = 0.f; msg.rotation[1] = 0.f; msg.rotation[2] = 0.f; msg.rotation[3] = 1.f;

    auto& r = msg.recipe;
    r.fixed = false;
    r.friction = 0.5f;
    r.elasticity = 0.1f;
    r.mass = 10.f;
    r.start_frozen = true;
    r.enable_collision = true;
    r.calc_mass_center = false;
    r.linear_damp = 0.3f;
    r.rot_damp = 0.1f;
    r.mass_center[0] = 0.f; r.mass_center[1] = 0.f; r.mass_center[2] = 0.f;
    r.collision_surface = "Ball_Stone_Mesh";
    r.convex_meshes = {"Convex_A", "Convex_B"};
    r.balls = {{{0.f, 0.f, 0.f}, 2.f}, {{1.f, 1.f, 1.f}, 0.5f}};
    r.concave_meshes = {"Concave_A"};
    return msg;
}

} // namespace

TEST(SessionEventMsg, PhysicalizeSerializeDeserializeRoundTrip) {
    auto msg = make_physicalize_event();
    auto parsed = round_trip(msg);

    EXPECT_EQ(5u, parsed.session);
    EXPECT_EQ(20u, parsed.player);
    EXPECT_EQ(300u, parsed.tick);
    EXPECT_EQ(bmmo::session::event_type::Physicalize, parsed.type);
    EXPECT_EQ(1, parsed.ball_type);
    EXPECT_FLOAT_EQ(2.f, parsed.position[1]);
    EXPECT_FLOAT_EQ(1.f, parsed.rotation[3]);

    const auto& r = parsed.recipe;
    EXPECT_FALSE(r.fixed);
    EXPECT_FLOAT_EQ(0.5f, r.friction);
    EXPECT_FLOAT_EQ(0.1f, r.elasticity);
    EXPECT_FLOAT_EQ(10.f, r.mass);
    EXPECT_TRUE(r.start_frozen);
    EXPECT_TRUE(r.enable_collision);
    EXPECT_FALSE(r.calc_mass_center);
    EXPECT_FLOAT_EQ(0.3f, r.linear_damp);
    EXPECT_FLOAT_EQ(0.1f, r.rot_damp);
    EXPECT_EQ("Ball_Stone_Mesh", r.collision_surface);
    ASSERT_EQ(2u, r.convex_meshes.size());
    EXPECT_EQ("Convex_A", r.convex_meshes[0]);
    EXPECT_EQ("Convex_B", r.convex_meshes[1]);
    ASSERT_EQ(2u, r.balls.size());
    EXPECT_FLOAT_EQ(2.f, r.balls[0].radius);
    EXPECT_FLOAT_EQ(1.f, r.balls[1].center[0]);
    EXPECT_FLOAT_EQ(0.5f, r.balls[1].radius);
    ASSERT_EQ(1u, r.concave_meshes.size());
    EXPECT_EQ("Concave_A", r.concave_meshes[0]);
}

TEST(SessionEventMsg, UnphysicalizeSerializeDeserializeRoundTrip) {
    bmmo::session_event_msg msg{};
    msg.session = 5; msg.player = 20; msg.tick = 301;
    msg.type = bmmo::session::event_type::Unphysicalize;

    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::event_type::Unphysicalize, parsed.type);
    EXPECT_EQ(301u, parsed.tick);
}

TEST(SessionEventMsg, SectorSerializeDeserializeRoundTrip) {
    bmmo::session_event_msg msg{};
    msg.session = 5; msg.player = 20; msg.tick = 302;
    msg.type = bmmo::session::event_type::Sector;
    msg.sector = 4;

    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::event_type::Sector, parsed.type);
    EXPECT_EQ(4, parsed.sector);
}

TEST(SessionEventMsg, FinishSerializeDeserializeRoundTrip) {
    bmmo::session_event_msg msg{};
    msg.session = 5; msg.player = 20; msg.tick = 303;
    msg.type = bmmo::session::event_type::Finish;

    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::event_type::Finish, parsed.type);
    EXPECT_EQ(303u, parsed.tick);
}

TEST(SessionEventMsg, BodyRevivedSerializeDeserializeRoundTrip) {
    bmmo::session_event_msg msg{};
    msg.session = 5; msg.player = 0; msg.tick = 304;
    msg.type = bmmo::session::event_type::BodyRevived;
    msg.name = "P_Modul_01_Rinne";

    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::event_type::BodyRevived, parsed.type);
    EXPECT_EQ("P_Modul_01_Rinne", parsed.name);
}

TEST(SessionEventMsg, DeserializeRejectsEveryTruncationForPhysicalize) {
    auto msg = make_physicalize_event();
    expect_rejects_every_truncation(msg);
}

TEST(SessionEventMsg, DeserializeRejectsEveryTruncationForBodyRevived) {
    bmmo::session_event_msg msg{};
    msg.session = 5; msg.tick = 1;
    msg.type = bmmo::session::event_type::BodyRevived;
    msg.name = "some_mechanism";
    expect_rejects_every_truncation(msg);
}

// Protocol 9: the applied tick, the origin and the state bits.
TEST(SessionEventMsg, CarriesTheOriginAndTheStateFromProtocol9) {
    auto msg = make_physicalize_event();
    msg.tick = 330;            // applied: the server re-stamped it
    msg.origin_tick = 300;     // stamped by its sender
    msg.state = bmmo::session::EVENT_STATE_MOVED;
    auto parsed = round_trip(msg);
    EXPECT_EQ(330u, parsed.tick);
    EXPECT_EQ(300u, parsed.origin_tick);
    EXPECT_EQ(bmmo::session::EVENT_STATE_MOVED, parsed.state);
    EXPECT_EQ("Ball_Stone_Mesh", parsed.recipe.collision_surface);

    bmmo::session_event_msg superseded{};
    superseded.session = 5; superseded.player = 20;
    superseded.tick = 128; superseded.origin_tick = 100;
    superseded.type = bmmo::session::event_type::Unphysicalize;
    superseded.state = bmmo::session::EVENT_STATE_MOVED | bmmo::session::EVENT_STATE_SUPERSEDED;
    auto parsed_superseded = round_trip(superseded);
    EXPECT_EQ(100u, parsed_superseded.origin_tick);
    EXPECT_EQ(bmmo::session::EVENT_STATE_MOVED | bmmo::session::EVENT_STATE_SUPERSEDED, parsed_superseded.state);

    bmmo::session_event_msg sector{};
    sector.tick = 77; sector.origin_tick = 77; sector.type = bmmo::session::event_type::Sector; sector.sector = 3;
    auto parsed_sector = round_trip(sector);
    EXPECT_EQ(77u, parsed_sector.origin_tick);
    EXPECT_EQ(3, parsed_sector.sector);
    EXPECT_EQ(0u, parsed_sector.state);
}

// A protocol 8 message is a protocol 9 one without the trailer: refused for
// every type, as is an origin after the applied tick (the re-stamp only ever
// moves an event later) and a state bit nobody defined.
TEST(SessionEventMsg, RejectsAProtocol8MessageAnOriginAfterTheTickAndUnknownState) {
    auto parses = [](const std::string& bytes) {
        bmmo::session_event_msg parsed{};
        parsed.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return parsed.deserialize();
    };
    for (const auto type: {bmmo::session::event_type::Physicalize, bmmo::session::event_type::Unphysicalize,
                           bmmo::session::event_type::Sector, bmmo::session::event_type::Finish,
                           bmmo::session::event_type::BodyRevived}) {
        auto msg = make_physicalize_event();   // serialize() writes what the type needs only
        msg.type = type;
        msg.tick = 300;
        msg.origin_tick = 300;
        msg.name = "P_Modul_01_Rinne";
        ASSERT_TRUE(msg.serialize());
        const std::string payload = stream_payload(msg.raw);
        ASSERT_TRUE(parses(payload)) << static_cast<int>(type);
        EXPECT_FALSE(parses(payload.substr(0, payload.size() - kEventTrailer))) << "v8 shape of type " << static_cast<int>(type);
    }
    auto later = make_physicalize_event();
    later.tick = 300;
    later.origin_tick = 301;
    ASSERT_TRUE(later.serialize());
    EXPECT_FALSE(parses(stream_payload(later.raw)));
    auto unknown = make_physicalize_event();
    unknown.state = 0x08;
    ASSERT_TRUE(unknown.serialize());
    EXPECT_FALSE(parses(stream_payload(unknown.raw)));
}

TEST(SessionEventMsg, DeserializeRejectsInvalidType) {
    bmmo::session_event_msg msg{};
    msg.type = bmmo::session::event_type::Finish;
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    const size_t type_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 3; // session, player, tick
    payload[type_offset] = static_cast<char>(5); // > BodyRevived(4)

    bmmo::session_event_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

TEST(SessionEventMsg, DeserializeRejectsConvexCountAboveCap) {
    bmmo::session_event_msg msg{};
    msg.type = bmmo::session::event_type::Physicalize;
    msg.recipe.convex_meshes.clear();
    msg.recipe.balls.clear();
    msg.recipe.concave_meshes.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    // the payload ends with origin_tick u32 and state u8 (protocol 9); before
    // them: convex_count(0), ball_count(0), concave_count(0)
    const std::string trailer = payload.substr(payload.size() - kEventTrailer);
    payload.resize(payload.size() - kEventTrailer - 3 * sizeof(uint8_t));
    const uint8_t forged_convex_count = static_cast<uint8_t>(bmmo::session::MAX_CONVEX + 1);
    const uint8_t zero = 0;
    payload.append(reinterpret_cast<const char*>(&forged_convex_count), sizeof(forged_convex_count));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));
    payload += trailer;

    bmmo::session_event_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
    EXPECT_TRUE(parsed.recipe.convex_meshes.empty());
}

TEST(SessionEventMsg, DeserializeRejectsConvexCountLargerThanRemaining) {
    bmmo::session_event_msg msg{};
    msg.type = bmmo::session::event_type::Physicalize;
    msg.recipe.convex_meshes.clear();
    msg.recipe.balls.clear();
    msg.recipe.concave_meshes.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - kEventTrailer - 3 * sizeof(uint8_t));
    const uint8_t forged_convex_count = 3; // within the cap, but no mesh names follow
    payload.append(reinterpret_cast<const char*>(&forged_convex_count), sizeof(forged_convex_count));

    bmmo::session_event_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

TEST(SessionEventMsg, DeserializeRejectsBallCountAboveCap) {
    bmmo::session_event_msg msg{};
    msg.type = bmmo::session::event_type::Physicalize;
    msg.recipe.convex_meshes.clear();
    msg.recipe.balls.clear();
    msg.recipe.concave_meshes.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    // before the origin/state trailer: convex_count(0), ball_count(0),
    // concave_count(0); keep convex_count(0), forge ball_count
    const std::string trailer = payload.substr(payload.size() - kEventTrailer);
    payload.resize(payload.size() - kEventTrailer - 2 * sizeof(uint8_t));
    const uint8_t forged_ball_count = static_cast<uint8_t>(bmmo::session::MAX_CONVEX + 1);
    const uint8_t zero = 0;
    payload.append(reinterpret_cast<const char*>(&forged_ball_count), sizeof(forged_ball_count));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));
    payload += trailer;

    bmmo::session_event_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
    EXPECT_TRUE(parsed.recipe.balls.empty());
}

// ---------------------------------------------------------------------
// session_snapshot_msg
// ---------------------------------------------------------------------

// Protocol 10 (resync design 5, step A3): SessionAssign's flags, on every
// copy, and SessionResyncState with its rows, clones and byte sections.
TEST(SessionAssignMsg, FlagsRoundTrip) {
    bmmo::session_assign_msg msg;
    msg.session = 9;
    msg.first_tick = 1572;
    msg.flags = bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS;
    ASSERT_TRUE(msg.serialize());
    bmmo::session_assign_msg parsed;
    parsed.raw.str(msg.raw.str());
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(9u, parsed.session);
    EXPECT_EQ(1572u, parsed.first_tick);
    EXPECT_EQ(bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS, parsed.flags);
    // A protocol 9 assignment (no flags byte) is refused, never read short
    // (protocol 24, FULL step SAXH: the two trailing instants come after the
    // flags; a cut inside them is refused too).
    const std::string bytes = msg.raw.str();
    bmmo::session_assign_msg old;
    old.raw.str(bytes.substr(0, bytes.size() - 17));
    EXPECT_FALSE(old.deserialize());
    bmmo::session_assign_msg cut;
    cut.raw.str(bytes.substr(0, bytes.size() - 1));
    EXPECT_FALSE(cut.deserialize());
}

// Protocol 34: the sections are the state's body (body_bytes / read_body_bytes).
TEST(SessionResyncStateMsg, RoundTripWithEveryTrailingSection) {
    bmmo::session_resync_state_msg msg;
    msg.session = 9;
    msg.tick = 1571;
    msg.base = 1572;
    bmmo::session::body_state ball, mech;
    ball.kind = bmmo::session::body_kind::Ball;
    ball.owner = 3;
    ball.position[0] = 1.5;
    ball.flags = bmmo::session::BODY_FLAG_SIMULATED;
    mech.kind = bmmo::session::body_kind::Mechanism;
    mech.owner = 17;
    mech.name = "P_Modul_08_Schaukel";
    mech.rotation[3] = 1.0;
    mech.linear[1] = -0.25f;
    msg.rows = {ball, mech};
    msg.clones = {{3, "Ball_Stone_BMMO_3"}, {4, "Ball_Wood_BMMO_4"}};
    bmmo_physics_nav_state nav{};
    nav.active = 1;
    nav.key_mask = 0x05;
    nav.controller_mask = 0x04;
    nav.force[2][0] = 0.75f;
    nav.force[2][2] = -0.5f;
    msg.navs = {{3, nav}};
    msg.union_state = {1, 2, 3};
    msg.scripts.assign(4000, 0x5a);
    msg.savestate.assign(150000, 0x11);
    msg.savestate.back() = 0x22;
    const std::string bytes = msg.body_bytes();
    EXPECT_GT(bytes.size(), 154000u);
    bmmo::session_resync_state_msg parsed;
    ASSERT_TRUE(parsed.read_body_bytes(bytes));   // tick and base are the header's
    ASSERT_EQ(2u, parsed.rows.size());
    EXPECT_EQ(bmmo::session::body_kind::Ball, parsed.rows[0].kind);
    EXPECT_EQ(3u, parsed.rows[0].owner);
    EXPECT_EQ(1.5, parsed.rows[0].position[0]);
    EXPECT_EQ("P_Modul_08_Schaukel", parsed.rows[1].name) << "a mechanism row carries its name, as a full snapshot's";
    EXPECT_EQ(-0.25f, parsed.rows[1].linear[1]);
    ASSERT_EQ(2u, parsed.clones.size());
    EXPECT_EQ(4u, parsed.clones[1].first);
    EXPECT_EQ("Ball_Wood_BMMO_4", parsed.clones[1].second);
    ASSERT_EQ(1u, parsed.navs.size());
    EXPECT_EQ(3u, parsed.navs[0].first);
    EXPECT_EQ(1, parsed.navs[0].second.active);
    EXPECT_EQ(0x05, parsed.navs[0].second.key_mask);
    EXPECT_EQ(0x04, parsed.navs[0].second.controller_mask);
    EXPECT_EQ(0.75f, parsed.navs[0].second.force[2][0]);
    EXPECT_EQ(-0.5f, parsed.navs[0].second.force[2][2]);
    EXPECT_EQ(msg.union_state, parsed.union_state);
    EXPECT_EQ(msg.scripts, parsed.scripts);
    EXPECT_EQ(msg.savestate, parsed.savestate);
    // Every truncation is refused, and so is a trailing byte.
    for (size_t cut: {bytes.size() - 1, bytes.size() - 150000, static_cast<size_t>(20)}) {
        bmmo::session_resync_state_msg shorter;
        EXPECT_FALSE(shorter.read_body_bytes(bytes.substr(0, cut))) << cut;
    }
    bmmo::session_resync_state_msg longer;
    EXPECT_FALSE(longer.read_body_bytes(bytes + '\0'));
}

// Protocol 34: the body travels as one zstd frame, plain or against the
// previous body; the header round-trips; a decode is refused unless the
// size, the whole frame, the reference and zstd's checksum all hold.
TEST(SessionResyncStateMsg, TheBodyTravelsAsOneZstdFrame) {
    bmmo::session_resync_state_msg previous;
    previous.scripts.assign(60000, 0x5a);
    previous.savestate.assign(180000, 0x11);
    for (size_t i = 0; i < previous.savestate.size(); i += 7) previous.savestate[i] = static_cast<uint8_t>(i * 131);
    bmmo::session_resync_state_msg next;
    next.scripts = previous.scripts;
    next.savestate = previous.savestate;
    next.savestate[5000] ^= 0xff;
    const std::string ref = previous.body_bytes(), body = next.body_bytes();
    bmmo::session::resync_codec codec;
    std::vector<uint8_t> plain, delta;
    ASSERT_TRUE(codec.encode(body, nullptr, plain));
    ASSERT_TRUE(codec.encode(body, &ref, delta));
    EXPECT_LT(delta.size() * 4, plain.size()) << "the previous body is the reference";
    std::string out;
    ASSERT_TRUE(codec.decode(plain, nullptr, static_cast<uint32_t>(body.size()), out));
    EXPECT_EQ(body, out);
    ASSERT_TRUE(codec.decode(delta, &ref, static_cast<uint32_t>(body.size()), out));
    EXPECT_EQ(body, out) << "bit for bit";
    EXPECT_FALSE(codec.decode(delta, nullptr, static_cast<uint32_t>(body.size()), out)) << "no reference";
    const std::string other(ref.size(), 'x');
    EXPECT_FALSE(codec.decode(delta, &other, static_cast<uint32_t>(body.size()), out)) << "another reference";
    EXPECT_FALSE(codec.decode(plain, nullptr, static_cast<uint32_t>(body.size()) + 1, out)) << "another size";
    auto flipped = plain;
    flipped[flipped.size() / 2] ^= 0x01;
    EXPECT_FALSE(codec.decode(flipped, nullptr, static_cast<uint32_t>(body.size()), out)) << "the checksum";
    auto longer = plain;
    longer.push_back(0);
    EXPECT_FALSE(codec.decode(longer, nullptr, static_cast<uint32_t>(body.size()), out)) << "one frame, nothing after";
    // The header carries the frame and its reference.
    next.session = 9;
    next.tick = 1571;
    next.base = 1572;
    next.ref_kind = bmmo::session::RESYNC_REF_PREVIOUS;
    next.ref_tick = 1440;
    next.ref_check = bmmo::session::resync_frame_check(delta);
    next.raw_size = static_cast<uint32_t>(body.size());
    next.frame = delta;
    ASSERT_TRUE(next.serialize());
    const std::string wire = next.raw.str();
    EXPECT_LT(wire.size(), 200u + delta.size());
    bmmo::session_resync_state_msg parsed;
    parsed.raw.write(wire.data(), static_cast<std::streamsize>(wire.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(1572u, parsed.base);
    EXPECT_EQ(bmmo::session::RESYNC_REF_PREVIOUS, parsed.ref_kind);
    EXPECT_EQ(1440u, parsed.ref_tick);
    EXPECT_EQ(next.ref_check, parsed.ref_check);
    EXPECT_EQ(delta, parsed.frame);
    EXPECT_TRUE(parsed.scripts.empty()) << "the sections are the frame's";
    std::string kind2 = wire;
    kind2[sizeof(bmmo::opcode) + 12] = 2;
    bmmo::session_resync_state_msg unknown;
    unknown.raw.write(kind2.data(), static_cast<std::streamsize>(kind2.size()));
    EXPECT_FALSE(unknown.deserialize()) << "an unknown reference kind";
    // The request names the state the client holds.
    bmmo::session_resync_msg request;
    request.session = 9;
    request.held = {1440, 0xdeadbeef, true};
    EXPECT_EQ(round_trip(request).held, request.held);
}

TEST(SessionSnapshotMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_snapshot_msg msg{};
    msg.session = 5;
    msg.tick = 400;
    msg.full = 0;
    msg.acked_input_tick = 398;

    bmmo::session::body_state ball{};
    ball.kind = bmmo::session::body_kind::Ball;
    ball.owner = 20;
    ball.position[0] = 1.5; ball.position[1] = -2.5; ball.position[2] = 3.5;
    ball.rotation[0] = 0.0; ball.rotation[1] = 0.0; ball.rotation[2] = 0.0; ball.rotation[3] = 1.0;
    ball.linear[0] = 0.1f; ball.linear[1] = 0.2f; ball.linear[2] = 0.3f;
    ball.angular[0] = 1.f; ball.angular[1] = 2.f; ball.angular[2] = 3.f;
    ball.flags = static_cast<uint8_t>(bmmo::session::BODY_FLAG_SIMULATED | bmmo::session::BODY_FLAG_COLLISION_ENABLED);

    msg.bodies = {ball};

    auto parsed = round_trip(msg);
    EXPECT_EQ(5u, parsed.session);
    EXPECT_EQ(400u, parsed.tick);
    EXPECT_EQ(0, parsed.full);
    EXPECT_EQ(398u, parsed.acked_input_tick);
    ASSERT_EQ(1u, parsed.bodies.size());
    EXPECT_EQ(bmmo::session::body_kind::Ball, parsed.bodies[0].kind);
    EXPECT_EQ(20u, parsed.bodies[0].owner);
    EXPECT_DOUBLE_EQ(1.5, parsed.bodies[0].position[0]);
    EXPECT_DOUBLE_EQ(-2.5, parsed.bodies[0].position[1]);
    EXPECT_DOUBLE_EQ(1.0, parsed.bodies[0].rotation[3]);
    EXPECT_FLOAT_EQ(0.2f, parsed.bodies[0].linear[1]);
    EXPECT_FLOAT_EQ(3.f, parsed.bodies[0].angular[2]);
    EXPECT_EQ(ball.flags, parsed.bodies[0].flags);
}

TEST(SessionSnapshotMsg, NameOnlyForFullMechanismBodies) {
    bmmo::session::body_state mechanism{};
    mechanism.kind = bmmo::session::body_kind::Mechanism;
    mechanism.owner = 0;
    mechanism.name = "P_Modul_01_Rinne";

    bmmo::session::body_state ball{};
    ball.kind = bmmo::session::body_kind::Ball;
    ball.owner = 7;
    ball.name = "should never be written"; // not eligible: kind != Mechanism

    // delta snapshot: neither row keeps its name, and no length prefix is
    // written for either (not even a zero-length one)
    {
        bmmo::session_snapshot_msg msg{};
        msg.full = 0;
        msg.bodies = {mechanism, ball};
        auto parsed = round_trip(msg);
        ASSERT_EQ(2u, parsed.bodies.size());
        EXPECT_TRUE(parsed.bodies[0].name.empty());
        EXPECT_TRUE(parsed.bodies[1].name.empty());
    }

    // full snapshot: only the mechanism row keeps its name
    {
        bmmo::session_snapshot_msg msg{};
        msg.full = 1;
        msg.bodies = {mechanism, ball};
        auto parsed = round_trip(msg);
        ASSERT_EQ(2u, parsed.bodies.size());
        EXPECT_EQ("P_Modul_01_Rinne", parsed.bodies[0].name);
        EXPECT_TRUE(parsed.bodies[1].name.empty());
    }
}

TEST(SessionSnapshotMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_snapshot_msg msg{};
    msg.full = 1;
    bmmo::session::body_state b{};
    b.kind = bmmo::session::body_kind::Mechanism;
    b.name = "mech";
    msg.bodies = {b};
    expect_rejects_every_truncation(msg);
}

TEST(SessionSnapshotMsg, DeserializeRejectsBodyCountAboveCap) {
    bmmo::session_snapshot_msg msg{};
    msg.bodies.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - sizeof(uint16_t));
    const uint16_t forged_count = static_cast<uint16_t>(bmmo::session::MAX_BODIES_PER_SNAPSHOT + 1);
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

    bmmo::session_snapshot_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
    EXPECT_TRUE(parsed.bodies.empty());
}

TEST(SessionSnapshotMsg, DeserializeRejectsBodyCountLargerThanRemaining) {
    bmmo::session_snapshot_msg msg{};
    msg.bodies.clear();
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    payload.resize(payload.size() - sizeof(uint16_t));
    const uint16_t forged_count = 10; // within the cap, but no body data follows
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

    bmmo::session_snapshot_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

TEST(SessionSnapshotMsg, DeserializeRejectsInvalidBodyKind) {
    bmmo::session_snapshot_msg msg{};
    bmmo::session::body_state b{};
    b.kind = bmmo::session::body_kind::Ball;
    msg.bodies = {b};
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    const size_t kind_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + sizeof(uint8_t)
        + sizeof(uint32_t) + sizeof(uint16_t); // session, tick, full, acked_input_tick, body_count
    payload[kind_offset] = static_cast<char>(2); // > Mechanism(1)

    bmmo::session_snapshot_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

// ---------------------------------------------------------------------
// session_resync_msg
// ---------------------------------------------------------------------

TEST(SessionResyncMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_resync_msg msg{};
    msg.session = 9;
    msg.last_full_tick = 660;

    auto parsed = round_trip(msg);
    EXPECT_EQ(9u, parsed.session);
    EXPECT_EQ(660u, parsed.last_full_tick);
}

TEST(SessionResyncMsg, DeserializeRejectsEveryTruncation) {
    bmmo::session_resync_msg msg{};
    msg.session = 9;
    msg.last_full_tick = 660;
    expect_rejects_every_truncation(msg);
}

// ---------------------------------------------------------------------
// session_remote_input_msg (design 9.1; protocol 29: runs; protocol 30: headings)
// ---------------------------------------------------------------------

namespace {
    bmmo::session::input_frame relay_frame(uint8_t keys, float dir_z) {
        bmmo::session::input_frame f{};
        f.keys = keys;
        f.cam_right[0] = 1.0f;
        f.cam_up[1] = 1.0f;
        f.cam_dir[2] = dir_z;
        f.ball_type = 2;
        f.flags = 5;
        return f;
    }
}

TEST(SessionRemoteInputMsg, SerializeDeserializeRoundTripWithRuns) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 4;
    msg.tick = 1234;
    msg.from = 1200;
    msg.part = 1;
    msg.parts = 2;
    bmmo::session_remote_input_msg::entry e;
    e.player = 77;
    e.frames = {relay_frame(5, -1.0f), relay_frame(1, -1.0f), relay_frame(1, -1.0f), relay_frame(1, -0.5f)};
    e.headings = {0, 0, 0, bmmo::session::kHeadingInline};
    e.base = bmmo::session::camera_bases::base{1200, bmmo::session::camera_of(relay_frame(0, -1.0f))};
    msg.entries.push_back(e);
    bmmo::session_remote_input_msg::entry no_ids;   // built in memory: its cameras go inline
    no_ids.player = 78;
    no_ids.frames.push_back(relay_frame(0, 0.25f));
    msg.entries.push_back(no_ids);
    bmmo::session_remote_input_msg::entry ended;    // protocol 31: ends 3 ticks before the newest
    ended.player = 80;
    ended.frames = {relay_frame(2, 0.5f), relay_frame(2, 0.5f)};
    ended.back = 3;
    msg.entries.push_back(ended);
    bmmo::session_remote_input_msg::entry empty;    // nothing to say: not written
    empty.player = 79;
    msg.entries.push_back(empty);
    auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.session, 4u);
    EXPECT_EQ(parsed.tick, 1234u);
    EXPECT_EQ(parsed.from, 1200u);
    EXPECT_EQ(parsed.part, 1u);
    EXPECT_EQ(parsed.parts, 2u);
    ASSERT_EQ(parsed.entries.size(), 3u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[0], 0), 1231u);
    EXPECT_EQ(parsed.entries[2].back, 3u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[2], 0), 1230u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[2], 1), 1231u);
    std::map<uint32_t, bmmo::session::camera_bases> bases;
    const auto entries = parsed.resolved_entries(bases);
    ASSERT_TRUE(entries);
    ASSERT_EQ((*entries)[0].frames.size(), 4u);
    const auto same = [](const bmmo::session::input_frame& a, const bmmo::session::input_frame& b) {
        return a.keys == b.keys && a.ball_type == b.ball_type && a.flags == b.flags && bmmo::session::same_camera(a, b);
    };
    for (size_t i = 0; i < 4; ++i) EXPECT_TRUE(same((*entries)[0].frames[i], e.frames[i])) << i;
    EXPECT_TRUE(same((*entries)[1].frames[0], no_ids.frames[0]));
    // A run of still frames is one run: 4 bytes for any length.
    bmmo::session_remote_input_msg still{};
    still.tick = 100;
    bmmo::session_remote_input_msg::entry se;
    se.player = 1;
    se.frames.assign(60, relay_frame(1, -1.0f));
    se.headings.assign(60, 0);
    still.entries.push_back(se);
    ASSERT_TRUE(still.serialize());
    EXPECT_EQ(still.size(), bmmo::session::kRelayDatagramHeader + 4 + 1 + 1 + 4 + 1 + bmmo::session::kEventCountBytes);
}

TEST(SessionRemoteInputMsg, RejectsTruncationAndBadCounts) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 1;
    msg.tick = 1000;   // far from tick 0, so only the caps below can reject a count
    msg.from = 990;
    bmmo::session_remote_input_msg::entry e;
    e.player = 9;
    e.frames = {relay_frame(1, 1.0f), relay_frame(2, 1.0f)};
    e.headings = {0, 0};
    msg.entries.push_back(e);
    expect_rejects_every_truncation(msg);
    // serialize() appends: start the stream again, or the payload is two
    // messages (nothing may follow the held section since protocol 7).
    msg.raw = std::stringstream{};
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);
    const size_t count_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2;
    const size_t parts_offset = count_offset + 1 + sizeof(uint32_t) * 3 + 1 + sizeof(uint32_t) + 1;
    const size_t nframes_offset = bmmo::session::kRelayDatagramHeader + 4;
    const size_t second_start_offset = nframes_offset + 2 + 4;
    auto parses = [&](size_t offset, char value) {
        std::string copy = payload;
        copy[offset] = value;
        bmmo::session_remote_input_msg parsed{};
        parsed.raw.write(copy.data(), static_cast<std::streamsize>(copy.size()));
        return parsed.deserialize();
    };
    EXPECT_FALSE(parses(count_offset, static_cast<char>(bmmo::session::MAX_PLAYERS_PER_SESSION + 1)));
    EXPECT_FALSE(parses(parts_offset, 0));                // no parts
    EXPECT_FALSE(parses(parts_offset - 1, 1));           // part 1 of 1
    EXPECT_FALSE(parses(nframes_offset, 0));
    EXPECT_FALSE(parses(nframes_offset, 11));            // tick 990 is not after `from`
    EXPECT_FALSE(parses(nframes_offset + 1, 0));         // no runs
    EXPECT_FALSE(parses(second_start_offset, 0));        // runs must start further on
    EXPECT_FALSE(parses(second_start_offset, 2));        // ... inside the entry
}

// Protocol 7 (late-player design 1.1): the recipient's acked, held_total and
// epoch in the header, and the held section after the entries - by
// reference, inline, and the default frame.
// Protocol 33 (docs/relay-protocol-design.md 4.9): the events section - a
// recipe, one pose for two births, every compact type and a raw message -
// round-trips bit for bit, a compact birth is 13 bytes beside its shared pose,
// the client rebuilds the reliable copy's very bytes from it, and every
// truncation is refused.
TEST(SessionRemoteInputMsg, EventsSectionRoundTripsCompactAndRaw) {
    using bmmo::session::event_type;
    bmmo::session::recipe_numbering numbering;
    const auto event = [](event_type type, uint32_t player, uint32_t tick, uint32_t origin) {
        bmmo::session_event_msg m;
        m.session = 3;
        m.player = player;
        m.type = type;
        m.tick = tick;
        m.origin_tick = origin;
        m.life = 2;
        m.ball_type = 1;
        m.flags = 1;
        m.position[0] = -20.0f;
        m.position[1] = 3.65f;
        m.rotation[0] = m.rotation[4] = m.rotation[8] = 1.0f;
        m.rotation[2] = -0.0f;   // a sign bit only a bitwise copy keeps
        m.recipe.mass = 10.0f;
        m.recipe.friction = 0.5f;
        m.recipe.collision_surface = "Ball_Stone_Mesh";
        m.recipe.balls = {{{0.f, 0.f, 0.f}, 2.f}};
        m.sector = type == event_type::Sector ? 4 : 0;
        if (type == event_type::Physicalize && origin != tick) m.state = bmmo::session::EVENT_STATE_MOVED;
        EXPECT_TRUE(m.serialize());
        return m.raw.str();
    };
    const std::vector<std::string> reliable = {
        event(event_type::Physicalize, 7, 231, 231), event(event_type::Physicalize, 8, 231, 220),
        event(event_type::Unphysicalize, 7, 300, 300), event(event_type::Sector, 7, 250, 250),
        event(event_type::Trafo, 8, 260, 260)};
    bmmo::session_member_msg member;
    member.session = 3;
    member.kind = bmmo::session::MEMBER_ADDED;
    member.tick = 240;
    member.entry.id = 9;
    ASSERT_TRUE(member.serialize());
    bmmo::session_remote_input_msg msg{};
    msg.session = 3;
    msg.tick = 245;
    for (const auto& bytes: reliable) msg.events.push_back(bmmo::session::relay_event_of(bytes, numbering));
    msg.events.push_back(bmmo::session::relay_event_of(member.raw.str(), numbering));
    msg.recipes.emplace_back(0, numbering.recipe(0));
    for (size_t i = 0; i < reliable.size(); ++i) EXPECT_TRUE(msg.events[i].compact()) << i;
    EXPECT_FALSE(msg.events.back().compact());
    auto parsed = round_trip(msg);
    const size_t recipe = 1 + bmmo::session::recipe_bytes(numbering.recipe(0)).size();
    const size_t events = 13 + 14 + 9 + 13 + 23 + 3 + member.raw.str().size();
    EXPECT_EQ(msg.size(), bmmo::session::kRelayDatagramHeader + 1 + bmmo::session::kEventCountBytes + recipe
                              + bmmo::session::kRelayPoseBytes + events);
    ASSERT_EQ(parsed.events.size(), msg.events.size());
    std::map<uint8_t, bmmo::session::ball_recipe> dictionary;
    for (const auto& [id, r]: parsed.recipes) dictionary[id] = r;
    for (size_t i = 0; i < reliable.size(); ++i) {
        EXPECT_TRUE(parsed.events[i] == msg.events[i]) << i;
        bmmo::session_event_msg rebuilt;
        ASSERT_TRUE(bmmo::session::event_msg_of(parsed.events[i], 3, dictionary, rebuilt)) << i;
        ASSERT_TRUE(rebuilt.serialize());
        EXPECT_EQ(rebuilt.raw.str(), reliable[i]) << i;
    }
    EXPECT_EQ(parsed.events.back().raw, member.raw.str());
    // Without its recipe a birth cannot be rebuilt.
    bmmo::session_event_msg orphan;
    EXPECT_FALSE(bmmo::session::event_msg_of(parsed.events[0], 3, {}, orphan));
    msg.raw = std::stringstream{};
    expect_rejects_every_truncation(msg);
}

TEST(SessionRemoteInputMsg, V3RoundTripWithTheHeldSection) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 4;
    msg.tick = 1234;
    msg.acked = 1201;
    msg.held_total = 17;
    msg.epoch = 1000;
    msg.wait = 21;
    bmmo::session_remote_input_msg::entry e;
    e.player = 77;
    e.frames.push_back(relay_frame(5, -1.0f));
    e.frames.push_back(relay_frame(1, -1.0f));
    msg.entries.push_back(e);
    bmmo::session_remote_input_msg::held_record by_reference;
    by_reference.back = 0;
    by_reference.source = 1229;
    msg.held.push_back(by_reference);
    bmmo::session_remote_input_msg::held_record inline_frame;
    inline_frame.back = 5;
    inline_frame.has_frame = true;
    inline_frame.frame = relay_frame(9, 0.5f);
    msg.held.push_back(inline_frame);
    bmmo::session_remote_input_msg::held_record by_default;
    by_default.back = 31;
    msg.held.push_back(by_default);
    auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.session, 4u);
    EXPECT_EQ(parsed.tick, 1234u);
    EXPECT_EQ(parsed.acked, 1201u);
    EXPECT_EQ(parsed.held_total, 17u);
    EXPECT_EQ(parsed.epoch, 1000u);
    EXPECT_EQ(parsed.wait, 21u) << "protocol 13: the recipient's wait";
    ASSERT_EQ(parsed.entries.size(), 1u);
    ASSERT_EQ(parsed.entries[0].frames.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.entries[0].frames[1].cam_dir[2], -1.0f);
    ASSERT_EQ(parsed.held.size(), 3u);
    EXPECT_EQ(parsed.held_tick(parsed.held[0]), 1234u);
    EXPECT_FALSE(parsed.held[0].has_frame);
    EXPECT_EQ(parsed.held[0].source, 1229u);
    EXPECT_EQ(parsed.held_tick(parsed.held[1]), 1229u);
    ASSERT_TRUE(parsed.held[1].has_frame);
    EXPECT_EQ(parsed.held[1].frame.keys, 9);
    EXPECT_EQ(parsed.held[1].frame.ball_type, 2);
    EXPECT_EQ(parsed.held[1].frame.flags, 5);
    EXPECT_FLOAT_EQ(parsed.held[1].frame.cam_dir[2], 0.5f);
    EXPECT_FLOAT_EQ(parsed.held[1].frame.cam_right[0], 1.0f);
    EXPECT_EQ(parsed.held_tick(parsed.held[2]), 1203u);
    EXPECT_FALSE(parsed.held[2].has_frame);
    EXPECT_EQ(parsed.held[2].source, bmmo::session::kHeldSourceDefault);
    // 5 B a reference, 41 B inline, 1 B of count; the entry's two runs carry
    // their cameras inline (no codes in memory).
    const size_t entry_bytes = 4 + 1 + 1 + 2 * (4 + 36);
    EXPECT_EQ(msg.size(), bmmo::session::kRelayDatagramHeader + entry_bytes + 1 + 5 + 41 + 5 + bmmo::session::kEventCountBytes);
    msg.raw = std::stringstream{};   // serialize() appends
    expect_rejects_every_truncation(msg);
    // A header-only datagram: nothing relayed, nothing substituted.
    bmmo::session_remote_input_msg header_only{};
    header_only.tick = 50;
    header_only.acked = 50;
    header_only.epoch = 40;
    header_only.wait = 255;
    auto parsed_header = round_trip(header_only);
    EXPECT_EQ(header_only.size(), bmmo::session::kRelayDatagramHeader + 1 + bmmo::session::kEventCountBytes);
    EXPECT_TRUE(parsed_header.entries.empty());
    EXPECT_TRUE(parsed_header.held.empty());
    EXPECT_EQ(parsed_header.acked, 50u);
    EXPECT_EQ(parsed_header.epoch, 40u);
    EXPECT_EQ(parsed_header.wait, 255u);
    // More than 32 records, or a back the 5 bits cannot name, are not written.
    bmmo::session_remote_input_msg many{};
    many.tick = 100;
    for (uint8_t b = 0; b < 40; ++b) {
        bmmo::session_remote_input_msg::held_record r;
        r.back = b;
        r.source = 50;
        many.held.push_back(r);
    }
    auto parsed_many = round_trip(many);
    EXPECT_EQ(parsed_many.held.size(), bmmo::session::kHeldNoticeTicks);
    EXPECT_EQ(parsed_many.held.back().back, 31);
}

// The reader rejects a count past 32, a back with bits 5-6 set, a record
// before tick 0, trailing bytes, and a protocol 6 datagram (no acked,
// held_total, epoch nor held section) of any shape.
TEST(SessionRemoteInputMsg, V3RejectsBadHeldSectionsAndAV6Datagram) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 1;
    msg.tick = 1000;
    bmmo::session_remote_input_msg::held_record r;
    r.back = 3;
    r.source = 990;
    msg.held.push_back(r);
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);
    const size_t nheld_offset = bmmo::session::kRelayDatagramHeader;   // no entries
    const size_t back_offset = nheld_offset + 1;
    auto parses = [](const std::string& bytes) {
        bmmo::session_remote_input_msg parsed{};
        parsed.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return parsed.deserialize();
    };
    ASSERT_TRUE(parses(payload));
    auto with = [&](size_t offset, char value) {
        std::string copy = payload;
        copy[offset] = value;
        return copy;
    };
    // 33 records, with the bytes for all of them there: the count is refused.
    // (The records go before the events section, the datagram's last.)
    const std::string tail(bmmo::session::kEventCountBytes, '\0');
    const auto records = [&](char count, int extra) {
        std::string bytes = with(nheld_offset, count);
        bytes.resize(bytes.size() - tail.size());
        for (int i = 0; i < extra; ++i) bytes += payload.substr(back_offset, 5);
        return bytes + tail;
    };
    EXPECT_FALSE(parses(records(33, 32)));
    EXPECT_TRUE(parses(records(32, 31)));
    EXPECT_FALSE(parses(with(back_offset, 3 | 0x20)));
    EXPECT_FALSE(parses(with(back_offset, 3 | 0x40)));
    EXPECT_FALSE(parses(payload + std::string(1, '\0')));   // nothing may follow the section
    bmmo::session_remote_input_msg early{};
    early.tick = 2;
    bmmo::session_remote_input_msg::held_record before_zero;
    before_zero.back = 3;
    early.held.push_back(before_zero);
    ASSERT_TRUE(early.serialize());
    EXPECT_EQ(early.size(), bmmo::session::kRelayDatagramHeader + 1 + bmmo::session::kEventCountBytes);   // the serializer drops it ...
    std::string early_payload = stream_payload(early.raw);
    early_payload.resize(early_payload.size() - tail.size());
    early_payload.back() = 1;
    const auto early_record = [](char back) { return std::string(1, back) + std::string(4, '\0'); };
    EXPECT_FALSE(parses(early_payload + early_record(3) + tail));   // ... and the reader refuses it
    EXPECT_TRUE(parses(early_payload + early_record(2) + tail));    // back 2 at tick 2 is tick 0
    // A v6 datagram is a v7 one without the three header fields and the
    // section: with no entries, with one, with several.
    for (const size_t members: {0u, 1u, 3u}) {
        bmmo::session_remote_input_msg v7{};
        v7.session = 2;
        v7.tick = 500;
        for (size_t p = 0; p < members; ++p) {
            bmmo::session_remote_input_msg::entry e;
            e.player = static_cast<uint32_t>(10 + p);
            for (int k = 0; k < 8; ++k) e.frames.push_back(relay_frame(static_cast<uint8_t>(k), -1.0f + 0.1f * k));
            v7.entries.push_back(e);
        }
        ASSERT_TRUE(v7.serialize());
        const std::string bytes = stream_payload(v7.raw);
        ASSERT_TRUE(parses(bytes));
        const size_t fields = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1;
        std::string v6 = bytes.substr(0, fields) + bytes.substr(fields + 12);
        v6.pop_back();   // the empty section's count
        EXPECT_FALSE(parses(v6)) << members << " entries";
    }
}

// ---------------------------------------------------------------------
// Protocol 11 (trafo-replication design 5, step T2): the Trafo event, the
// capability, the world options, the resync state's trafo ledger.
// ---------------------------------------------------------------------

TEST(TrafoWire, TheProtocolIs11AndTheBitsAreDistinct) {
    EXPECT_LE(11u, bmmo::session::PHYSICS_PROTOCOL_VERSION) << "12 since step S5 (PlacementWire)";
    EXPECT_EQ(5, static_cast<int>(bmmo::session::event_type::Trafo));
    EXPECT_EQ(0u, bmmo::session::CAP_TRAFO_INPUTS & (bmmo::session::CAP_LEAD | bmmo::session::CAP_RESYNC_STATE));
    EXPECT_NE(0u, bmmo::session::CAP_TRAFO_INPUTS);
    EXPECT_EQ(1u << 1, bmmo::session::WORLD_TRAFO_INPUTS) << "bit 0 is the sector placement's, 2 and 3 are claimed";
}

// ---------------------------------------------------------------------
// Protocol 12 (sector placement design 5, step S5): the world option, the
// capability and SessionReady's placement signature.
// ---------------------------------------------------------------------

TEST(PlacementWire, TheProtocolIs12AndTheBitsAreDistinct) {
    EXPECT_LE(13u, bmmo::session::PHYSICS_PROTOCOL_VERSION) << "13 since step R12 (the relay header's wait)";
    EXPECT_EQ(1u << 0, bmmo::session::WORLD_PLACEMENT_CLONES) << "the design's bit 0";
    EXPECT_EQ(0u, bmmo::session::WORLD_PLACEMENT_CLONES & bmmo::session::WORLD_TRAFO_INPUTS);
    EXPECT_NE(0u, bmmo::session::CAP_PLACEMENT);
    EXPECT_EQ(0u, bmmo::session::CAP_PLACEMENT
                  & (bmmo::session::CAP_LEAD | bmmo::session::CAP_RESYNC_STATE | bmmo::session::CAP_TRAFO_INPUTS));
}

TEST(PlacementWire, SessionReadyCarriesThePlacementSignature) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.caps = bmmo::session::CAP_PLACEMENT | bmmo::session::CAP_LEAD;
    msg.anchor_hash = 0x1111;
    msg.build_id = "b";
    msg.anchor_placement = 0x91a138e4fd6e7254ULL;
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, parsed.protocol);
    EXPECT_EQ(bmmo::session::CAP_PLACEMENT | bmmo::session::CAP_LEAD, parsed.caps);
    EXPECT_EQ(0x91a138e4fd6e7254ULL, parsed.anchor_placement);
    EXPECT_EQ(0x1111u, parsed.anchor_hash);
    msg.clear();
    expect_rejects_every_truncation(msg);
}

// Step B9DX (the user, 2026-10-28: refuse it outright, no more compatibility):
// the server refuses a member without the session core whatever the world
// options; the legacy retail path's SessionReady (caps 0) is that member.
// A core member (CAP_PLACEMENT, which both core hosts always declare) is
// refused only by a trafo-inputs session it cannot run.
TEST(SessionAdmission, AClientWithoutTheSessionCoreIsRefusedInEverySession) {
    using bmmo::session::ready_refusal;
    using bmmo::session::session_ready_refusal;
    const uint32_t P = bmmo::session::WORLD_PLACEMENT_CLONES, T = bmmo::session::WORLD_TRAFO_INPUTS;
    const uint32_t core = bmmo::session::CAP_PLACEMENT | bmmo::session::CAP_LEAD | bmmo::session::CAP_RESYNC_STATE;
    for (const uint32_t options: {0u, P, T, P | T}) {
        // The legacy retail path (physics_session_client.cpp: ready.caps = 0).
        EXPECT_EQ(ready_refusal::no_session_core, session_ready_refusal(0u, options)) << options;
        // Every capability but the core's mark is still not the core.
        EXPECT_EQ(ready_refusal::no_session_core,
                  session_ready_refusal(bmmo::session::CAP_LEAD | bmmo::session::CAP_RESYNC_STATE
                                        | bmmo::session::CAP_TRAFO_INPUTS, options)) << options;
        // A core member with the trafo inputs is admitted everywhere.
        EXPECT_EQ(ready_refusal::none, session_ready_refusal(core | bmmo::session::CAP_TRAFO_INPUTS, options)) << options;
        // A core member without them only where the trafo is not an input.
        EXPECT_EQ((options & T) ? ready_refusal::no_trafo_inputs : ready_refusal::none,
                  session_ready_refusal(core, options)) << options;
    }
    // The reasons the refused member is shown name the switch.
    const std::string no_core = bmmo::session::ready_refusal_reason(ready_refusal::no_session_core);
    EXPECT_NE(std::string::npos, no_core.find("session core on"));
    EXPECT_LE(no_core.size(), bmmo::session::MAX_REASON);
    const std::string no_trafo = bmmo::session::ready_refusal_reason(ready_refusal::no_trafo_inputs);
    EXPECT_NE(std::string::npos, no_trafo.find("trafo inputs"));
    EXPECT_LE(no_trafo.size(), bmmo::session::MAX_REASON);
    EXPECT_EQ(std::string{}, bmmo::session::ready_refusal_reason(ready_refusal::none));
    // The refusal reaches the member as a SessionEnd whose reason survives
    // the wire whole.
    bmmo::session_end_msg end{};
    end.session = 4;
    end.reason = no_core;
    auto parsed = round_trip(end);
    EXPECT_EQ(no_core, parsed.reason);
}

// Step DX7 (the user's answer 8 of 2026-10-29): a placement signature
// mismatch refuses that member alone, and the reason it is sent names both
// signatures (16 hex digits each) and what to check, within MAX_REASON, and
// reaches it whole as a SessionEnd.
TEST(SessionAdmission, APlacementMismatchReasonNamesBothSignatures) {
    const std::string reason = bmmo::session::placement_mismatch_reason(0x0000000000000001ULL, 0xecfbfd9b63a330b5ULL);
    EXPECT_EQ(0u, reason.find("placement signature mismatch: yours 0000000000000001, the server's ecfbfd9b63a330b5"))
        << reason;
    EXPECT_NE(std::string::npos, reason.find("BallanceMMO build"));
    EXPECT_LE(reason.size(), bmmo::session::MAX_REASON);
    const std::string high = bmmo::session::placement_mismatch_reason(0xffffffffffffffffULL, 0x0123456789abcdefULL);
    EXPECT_NE(std::string::npos, high.find("yours ffffffffffffffff, the server's 0123456789abcdef")) << high;
    EXPECT_EQ(reason.size(), high.size());
    bmmo::session_end_msg end{};
    end.session = 7;
    end.reason = reason;
    auto parsed = round_trip(end);
    EXPECT_EQ(7u, parsed.session);
    EXPECT_EQ(reason, parsed.reason);
}

// Protocol 16 (world-agreement-gaps-design.md 2.3, 5; step G3): SessionReady
// carries the client's probe signature after the placement's; a protocol 15
// client's message parses without it (0).
TEST(ProbeWire, SessionReadyCarriesTheProbeSignature) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.caps = bmmo::session::CAP_PLACEMENT;
    msg.build_id = "b";
    msg.anchor_placement = 0x91a138e4fd6e7254ULL;
    msg.anchor_probes = 0x5e0b1c2d3a4f6789ULL;
    auto parsed = round_trip(msg);
    EXPECT_LE(16u, bmmo::session::PHYSICS_PROTOCOL_VERSION) << "16 since G3; 17 since step BALL";
    EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, parsed.protocol);
    EXPECT_EQ(0x91a138e4fd6e7254ULL, parsed.anchor_placement);
    EXPECT_EQ(0x5e0b1c2d3a4f6789ULL, parsed.anchor_probes);
    msg.clear();
    expect_rejects_every_truncation(msg);
    bmmo::session_ready_msg old{};
    old.session = 3;
    old.protocol = 15;
    old.anchor_placement = 0x1234;
    old.anchor_probes = 0x5678;
    old.build_id = "b";
    ASSERT_TRUE(old.serialize());
    std::string bytes = stream_payload(old.raw);
    bytes.resize(bytes.size() - sizeof(uint64_t));   // what a protocol 15 build wrote
    bmmo::session_ready_msg back{};
    back.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(back.deserialize());
    EXPECT_EQ(0x1234u, back.anchor_placement);
    EXPECT_EQ(0u, back.anchor_probes);
    EXPECT_EQ(0u, bmmo::session::WORLD_PROXIMITY_UNION
                  & (bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS));
}

// A protocol 11 client sends no signature: its message still parses
// (placement 0), so the server can say which protocol it runs.
TEST(PlacementWire, AProtocol11SessionReadyParsesWithoutTheSignature) {
    bmmo::session_ready_msg msg{};
    msg.session = 3;
    msg.protocol = 11;
    msg.caps = bmmo::session::CAP_TRAFO_INPUTS;
    msg.anchor_placement = 0x1234;
    msg.build_id = "b";
    ASSERT_TRUE(msg.serialize());
    std::string bytes = stream_payload(msg.raw);
    bytes.resize(bytes.size() - sizeof(uint64_t));   // what a protocol 11 build wrote
    bmmo::session_ready_msg parsed{};
    parsed.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(11u, parsed.protocol);
    EXPECT_EQ(bmmo::session::CAP_TRAFO_INPUTS, parsed.caps);
    EXPECT_EQ(0u, parsed.anchor_placement);
}

TEST(PlacementWire, SessionStartCarriesThePlacementBit) {
    bmmo::session_start_msg msg{};
    msg.world_options = bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS;
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS, parsed.world_options);
}

TEST(TrafoWire, SessionStartCarriesTheWorldOptions) {
    bmmo::session_start_msg msg{};
    msg.world_options = bmmo::session::WORLD_TRAFO_INPUTS;
    bmmo::session::player_entry p{};
    p.id = 4;
    msg.players = {p};
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::WORLD_TRAFO_INPUTS, parsed.world_options);
    ASSERT_EQ(1u, parsed.players.size());
    EXPECT_EQ(4u, parsed.players[0].id);
    msg.clear();
    expect_rejects_every_truncation(msg);
}

namespace {
bmmo::session_event_msg make_trafo_event(uint8_t type, uint8_t flags) {
    bmmo::session_event_msg msg{};
    msg.session = 3;
    msg.player = 77;
    msg.tick = 860;
    msg.origin_tick = 843;
    msg.state = bmmo::session::EVENT_STATE_MOVED;
    msg.type = bmmo::session::event_type::Trafo;
    msg.ball_type = type;
    msg.flags = flags;
    msg.position[0] = 948.74127f;
    msg.position[1] = 3.7663198f;
    msg.position[2] = -56.514111f;
    return msg;
}
}

TEST(TrafoWire, TheEventRoundTripsAndRefusesEveryTruncation) {
    auto msg = make_trafo_event(1, 0);
    auto parsed = round_trip(msg);
    EXPECT_EQ(bmmo::session::event_type::Trafo, parsed.type);
    EXPECT_EQ(77u, parsed.player);
    EXPECT_EQ(860u, parsed.tick);
    EXPECT_EQ(843u, parsed.origin_tick);
    EXPECT_EQ(bmmo::session::EVENT_STATE_MOVED, parsed.state);
    EXPECT_EQ(1, parsed.ball_type);
    EXPECT_EQ(0, parsed.flags);
    EXPECT_EQ(948.74127f, parsed.position[0]);
    EXPECT_EQ(3.7663198f, parsed.position[1]);
    EXPECT_EQ(-56.514111f, parsed.position[2]);
    // Exactly the three fields and the trailer: type byte, then 1 + 1 + 12.
    msg.clear();
    ASSERT_TRUE(msg.serialize());
    auto sector = make_trafo_event(1, 0);
    sector.type = bmmo::session::event_type::Finish;
    ASSERT_TRUE(sector.serialize());
    EXPECT_EQ(sector.raw.str().size() + 14, msg.raw.str().size());
    auto again = make_trafo_event(2, 3);
    expect_rejects_every_truncation(again);
    // A type past Trafo is refused.
    std::string bytes = msg.raw.str();
    const size_t type_at = sizeof(bmmo::opcode) + 3 * sizeof(uint32_t);
    bytes[type_at] = 6;
    bmmo::session_event_msg unknown{};
    unknown.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    EXPECT_FALSE(unknown.deserialize());
}

TEST(TrafoWire, TheKeySeparatesExplodeResetAndServerStamped) {
    using bmmo::session::lifecycle_key_of;
    auto explode = make_trafo_event(0, 0);
    auto reset = make_trafo_event(0, 1);
    auto server = make_trafo_event(0, 3);
    server.origin_tick = server.tick;
    auto owner_at_applied = make_trafo_event(0, 1);
    owner_at_applied.origin_tick = owner_at_applied.tick;
    EXPECT_FALSE(lifecycle_key_of(explode, 77) == lifecycle_key_of(reset, 77)) << "explode and reset at one origin";
    EXPECT_FALSE(lifecycle_key_of(server, 77) == lifecycle_key_of(owner_at_applied, 77)) << "server-stamped vs the owner's";
    EXPECT_TRUE(lifecycle_key_of(reset, 77) == lifecycle_key_of(make_trafo_event(0, 1), 77)) << "a copy is a copy";
    bmmo::session::event_seen seen;
    EXPECT_TRUE(seen.first(lifecycle_key_of(explode, 77)));
    EXPECT_TRUE(seen.first(lifecycle_key_of(reset, 77)));
    EXPECT_FALSE(seen.first(lifecycle_key_of(explode, 77)));
    // Every other type keys as before: the Physicalize's SPAWN flag is not in it.
    auto birth = make_trafo_event(0, bmmo::session::PHYSICALIZE_FLAG_SPAWN);
    birth.type = bmmo::session::event_type::Physicalize;
    EXPECT_EQ(0, lifecycle_key_of(birth, 77).flags);
    EXPECT_EQ(2, bmmo::session::lifecycle_event_rank(bmmo::session::event_type::Trafo));
    EXPECT_STREQ("trafo", bmmo::session::event_type_name(5));
}

TEST(TrafoWire, TheResyncStateCarriesTheLedger) {
    bmmo::session_resync_state_msg msg;
    msg.session = 2;
    msg.tick = 999;
    msg.base = 1000;
    bmmo::session::trafo_ledger_entry a, b;
    a.player = 3920120238u;
    a.type = 2;
    a.has_explosion = true;
    a.explode_applied = 808;
    a.position[0] = 74.0f;
    a.position[1] = 3.095f;
    a.position[2] = 43.0f;
    a.has_bodies = true;
    b.player = 5;
    b.type = 0;
    b.has_explosion = true;
    b.explode_applied = 700;
    b.has_reset = true;
    b.reset_applied = 900;
    b.final = true;            // a leaver's set between its final resets and its retirement
    b.final_applied = 900;
    a.life = 2;                // protocol 21 (step TRL): the set is (player, life)'s
    msg.trafo_ledger = {a, b};
    const std::string bytes = msg.body_bytes();   // protocol 34: the body's sections
    bmmo::session_resync_state_msg parsed;
    ASSERT_TRUE(parsed.read_body_bytes(bytes));
    ASSERT_EQ(2u, parsed.trafo_ledger.size());
    EXPECT_EQ(3920120238u, parsed.trafo_ledger[0].player);
    EXPECT_EQ(2, parsed.trafo_ledger[0].type);
    EXPECT_TRUE(parsed.trafo_ledger[0].has_explosion);
    EXPECT_TRUE(parsed.trafo_ledger[0].has_bodies);
    EXPECT_FALSE(parsed.trafo_ledger[0].has_reset);
    EXPECT_EQ(808u, parsed.trafo_ledger[0].explode_applied);
    EXPECT_EQ(3.095f, parsed.trafo_ledger[0].position[1]);
    EXPECT_FALSE(parsed.trafo_ledger[1].has_bodies);
    EXPECT_TRUE(parsed.trafo_ledger[1].has_reset);
    EXPECT_EQ(900u, parsed.trafo_ledger[1].reset_applied);
    EXPECT_FALSE(parsed.trafo_ledger[0].final);
    EXPECT_TRUE(parsed.trafo_ledger[1].final);
    EXPECT_EQ(900u, parsed.trafo_ledger[1].final_applied);
    EXPECT_EQ(2u, parsed.trafo_ledger[0].life);
    EXPECT_EQ(1u, parsed.trafo_ledger[1].life);
    // Every truncation is refused (since protocol 34 the lives are always there).
    for (size_t len = 0; len < bytes.size(); ++len) {
        bmmo::session_resync_state_msg cut{};
        EXPECT_FALSE(cut.read_body_bytes(bytes.substr(0, len))) << "accepted a body truncated to " << len << " bytes";
    }
    // A life 0 is refused.
    std::string zero = bytes;
    zero[zero.size() - 4] = 0;
    zero[zero.size() - 3] = 0;
    bmmo::session_resync_state_msg zeroed;
    EXPECT_FALSE(zeroed.read_body_bytes(zero));
    // A forged count past the cap, an unknown piece type and an unknown
    // bit are refused.
    std::string forged = bytes;
    const size_t count_at = bytes.size() - 2 * 2 - 2 * (4 + 1 + 1 + 4 + 12 + 4 + 4) - 2;
    const uint16_t too_many = static_cast<uint16_t>(bmmo::session_resync_state_msg::kMaxTrafoLedger + 1);
    std::memcpy(&forged[count_at], &too_many, sizeof(too_many));
    bmmo::session_resync_state_msg capped;
    EXPECT_FALSE(capped.read_body_bytes(forged));
    std::string bad_type = bytes;
    bad_type[count_at + 2 + 4] = 3;
    bmmo::session_resync_state_msg typed;
    EXPECT_FALSE(typed.read_body_bytes(bad_type));
    std::string bad_bits = bytes;
    bad_bits[count_at + 2 + 4 + 1] = static_cast<char>(16);
    bmmo::session_resync_state_msg bitted;
    EXPECT_FALSE(bitted.read_body_bytes(bad_bits));
}

// ---------------------------------------------------------------------
// Protocols 14 (step BALA), 15 (SPGH2) and 17 (step BALL): the airship and
// spawn ghosts.
// ---------------------------------------------------------------------

TEST(SpawnGhostWire, TheProtocolIs15) {
    // Step SPGH2: the spawn ghost's rule in every world and its state in the
    // travelling savestate (format 3); no message layout changed.
    EXPECT_LE(15u, bmmo::session::PHYSICS_PROTOCOL_VERSION) << "15 since step SPGH2 (the spawn ghost); 16 since G3";
}

TEST(GhostPoseWire, RetiredAtProtocol17) {
    // Step BALL (the user's "switch to the old path", 2026-10-28): SessionGhostPose
    // (protocols 14-16) is gone - an airship ghost is shown at its owner's
    // legacy ball stream.  It was the last opcode, so no other value moved
    // (the message and its header are gone: a use would not compile).  What
    // is checked here is the protocol a client of this tree says at
    // SessionReady, which the server matches exactly.
    EXPECT_LE(17u, bmmo::session::PHYSICS_PROTOCOL_VERSION) << "17 since step BALL (SessionGhostPose retired)";
    bmmo::session_ready_msg ready{};
    ready.session = 7;
    auto parsed = round_trip(ready);
    EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, parsed.protocol);
}

// Step M1 (world-agreement-gaps-design.md 7.1 U4): SessionMember round trip
// and refusals; player_entry.life in SessionStart and session_event_msg.life.
TEST(SessionMemberMsg, RoundTripAndRefusals) {
    EXPECT_EQ(18u, bmmo::session::PHYSICS_PROTOCOL_MEMBER_LIVES);
    EXPECT_LE(bmmo::session::PHYSICS_PROTOCOL_MEMBER_LIVES, bmmo::session::PHYSICS_PROTOCOL_VERSION);
    bmmo::session_member_msg msg{};
    msg.session = 4;
    msg.kind = bmmo::session::MEMBER_ADDED;
    msg.tick = 1844;
    msg.entry.id = 142763540;
    msg.entry.join_order = 3;
    msg.entry.life = 2;
    msg.entry.spawn_position[1] = 5.f;
    msg.entry.spawn_rotation[3] = 1.f;
    auto parsed = round_trip(msg);
    EXPECT_EQ(4u, parsed.session);
    EXPECT_EQ(bmmo::session::MEMBER_ADDED, parsed.kind);
    EXPECT_EQ(1844u, parsed.tick);
    EXPECT_EQ(142763540u, parsed.entry.id);
    EXPECT_EQ(3, parsed.entry.join_order);
    EXPECT_EQ(2, parsed.entry.life);
    EXPECT_FLOAT_EQ(5.f, parsed.entry.spawn_position[1]);
    EXPECT_FLOAT_EQ(1.f, parsed.entry.spawn_rotation[3]);
    bmmo::session_member_msg fresh{};   // serialize() appends: a message not serialized yet
    fresh.kind = bmmo::session::MEMBER_REMOVED;
    fresh.entry.id = 5;
    fresh.entry.life = 1;
    expect_rejects_every_truncation(fresh);
    const auto refused = [](uint8_t kind, uint8_t order, uint32_t id, uint16_t life) {
        bmmo::session_member_msg bad{};
        bad.kind = kind;
        bad.entry.join_order = order;
        bad.entry.id = id;
        bad.entry.life = life;
        EXPECT_TRUE(bad.serialize());
        bmmo::session_member_msg back{};
        const std::string bytes = bad.raw.str();
        back.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return !back.deserialize();
    };
    EXPECT_FALSE(refused(bmmo::session::MEMBER_REMOVED, 1, 9, 1));
    EXPECT_TRUE(refused(0, 1, 9, 1)) << "kind 0";
    EXPECT_TRUE(refused(3, 1, 9, 1)) << "kind 3";
    EXPECT_TRUE(refused(bmmo::session::MEMBER_ADDED, 64, 9, 1)) << "join order 64";
    EXPECT_TRUE(refused(bmmo::session::MEMBER_ADDED, 1, 0, 1)) << "id 0";
    EXPECT_TRUE(refused(bmmo::session::MEMBER_ADDED, 1, 9, 0)) << "life 0";

    bmmo::session_start_msg start{};
    start.mode = bmmo::room::mode::Physics;
    bmmo::session::player_entry p{};
    p.id = 7; p.join_order = 2; p.life = 3;
    start.players = {p};
    auto start_back = round_trip(start);
    ASSERT_EQ(1u, start_back.players.size());
    EXPECT_EQ(3, start_back.players[0].life);

    bmmo::session_event_msg event{};
    event.session = 1; event.player = 7; event.tick = 10; event.origin_tick = 9;
    event.type = bmmo::session::event_type::Unphysicalize;
    event.life = 3;
    auto event_back = round_trip(event);
    EXPECT_EQ(3, event_back.life);
    EXPECT_EQ(9u, event_back.origin_tick);
}

// Protocol 24 (FULL step SAXH): the start handshake's messages, and the
// assignment's trailing instants - absent from a protocol-23 capture's bytes.
TEST(SessionStartHandshakeMsgs, RoundTrip) {
    bmmo::session_assign_msg a;
    a.session = 3;
    a.first_tick = 77;
    a.flags = bmmo::session::ASSIGN_FLAG_SCHEDULED;
    a.server_us = -5;
    a.start_us = 123456789012345LL;
    a.serialize();
    bmmo::session_assign_msg a2;
    { const std::string b = a.raw.str(); a2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    ASSERT_TRUE(a2.deserialize());
    EXPECT_EQ(a2.first_tick, 77u);
    EXPECT_EQ(a2.flags, bmmo::session::ASSIGN_FLAG_SCHEDULED);
    EXPECT_EQ(a2.server_us, -5);
    EXPECT_EQ(a2.start_us, 123456789012345LL);
    // The protocol-23 layout: 9 bytes after the opcode.
    std::string old = a.raw.str();
    old.resize(old.size() - 16);
    bmmo::session_assign_msg a3;
    a3.raw.write(old.data(), static_cast<std::streamsize>(old.size()));
    ASSERT_TRUE(a3.deserialize());
    EXPECT_EQ(a3.first_tick, 77u);
    EXPECT_EQ(a3.server_us, 0);
    EXPECT_EQ(a3.start_us, 0);

    bmmo::session_clock_ack_msg k;
    k.session = 4;
    k.first_tick = 9;
    k.kind = bmmo::session::CLOCK_ACK_START;
    k.seq = 2;
    k.server_us = 11;
    k.client_us = -12;
    k.serialize();
    bmmo::session_clock_ack_msg k2;
    { const std::string b = k.raw.str(); k2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    ASSERT_TRUE(k2.deserialize());
    EXPECT_EQ(k2.kind, bmmo::session::CLOCK_ACK_START);
    EXPECT_EQ(k2.seq, 2u);
    EXPECT_EQ(k2.server_us, 11);
    EXPECT_EQ(k2.client_us, -12);
    {   // the started member's answer (the gate round of SAXH)
        bmmo::session_clock_ack_msg st;
        st.kind = bmmo::session::CLOCK_ACK_STARTED;
        st.seq = 1;
        st.serialize();
        bmmo::session_clock_ack_msg st2;
        { const std::string b = st.raw.str(); st2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
        ASSERT_TRUE(st2.deserialize());
        EXPECT_EQ(st2.kind, bmmo::session::CLOCK_ACK_STARTED);
        EXPECT_EQ(st2.seq, 1u);
    }
    bmmo::session_clock_ack_msg bad;
    bad.kind = 7;
    bad.serialize();
    bmmo::session_clock_ack_msg bad2;
    { const std::string b = bad.raw.str(); bad2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    EXPECT_FALSE(bad2.deserialize()) << "an unknown kind";

    bmmo::session_start_at_msg t;
    t.session = 5;
    t.first_tick = 8;
    t.seq = 3;
    t.flags = bmmo::session::START_AT_FLAG_CORRECTION;
    t.server_us = 1;
    t.start_server_us = 2;
    t.start_us = 3;
    t.serialize();
    bmmo::session_start_at_msg t2;
    { const std::string b = t.raw.str(); t2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    ASSERT_TRUE(t2.deserialize());
    EXPECT_EQ(t2.seq, 3u);
    EXPECT_EQ(t2.flags, bmmo::session::START_AT_FLAG_CORRECTION);
    EXPECT_EQ(t2.start_server_us, 2);
    EXPECT_EQ(t2.start_us, 3);

    bmmo::session_start_status_msg st;
    st.session = 6;
    st.phase = static_cast<uint8_t>(bmmo::session::start_status_phase::postponed);
    st.confirmed = 2;
    st.total = 3;
    st.text = "start postponed";
    st.seq = 4;
    st.serialize();
    bmmo::session_start_status_msg st2;
    { const std::string b = st.raw.str(); st2.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    ASSERT_TRUE(st2.deserialize());
    EXPECT_EQ(st2.text, "start postponed");
    EXPECT_EQ(st2.confirmed, 2u);
    EXPECT_EQ(st2.total, 3u);
    EXPECT_TRUE(st2.has_seq);
    EXPECT_EQ(st2.seq, 4u) << "protocol 26: the start it describes";
    // A protocol 24/25 status (no trailing seq) still reads, without one.
    {
        const std::string b = st.raw.str();
        bmmo::session_start_status_msg old;
        old.raw.write(b.data(), static_cast<std::streamsize>(b.size() - sizeof(uint32_t)));
        ASSERT_TRUE(old.deserialize());
        EXPECT_FALSE(old.has_seq);
        EXPECT_EQ(old.seq, 0u);
        EXPECT_EQ(old.text, "start postponed");
    }
    // Distinct sizes on the wire (the harness proxy anchors a burst on one).
    EXPECT_NE(t.raw.str().size(), a.raw.str().size());
    EXPECT_NE(t.raw.str().size(), k.raw.str().size());
}

// Protocol 32 (step SPL): the server's world boot by phase, before the
// SessionStart; an unknown phase or a cut message is refused.
TEST(SessionMessages, BootStatusRoundTrips) {
    bmmo::session_boot_status_msg msg;
    msg.session = 9;
    msg.level = 11;
    msg.phase = static_cast<uint8_t>(bmmo::session::boot_phase::entering_level);
    const auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.code, bmmo::SessionBootStatus);
    EXPECT_EQ(parsed.session, 9u);
    EXPECT_EQ(parsed.level, 11);
    EXPECT_EQ(parsed.phase, static_cast<uint8_t>(bmmo::session::boot_phase::entering_level));
    bmmo::session_boot_status_msg cut;
    cut.session = 9;
    cut.level = 11;
    cut.phase = 2;
    expect_rejects_every_truncation(cut);
    bmmo::session_boot_status_msg unknown;
    unknown.phase = 3;
    ASSERT_TRUE(unknown.serialize());
    bmmo::session_boot_status_msg read;
    { const std::string b = unknown.raw.str(); read.raw.write(b.data(), static_cast<std::streamsize>(b.size())); }
    EXPECT_FALSE(read.deserialize());
}

// The server's intake of an input datagram with piggybacked events (protocol
// 25, FULL step EVI; server.cpp handle_session_input / handle_session_event,
// which this target cannot link): take_input_datagram hands every event to
// the event intake BEFORE the frames are submitted, so a piggybacked P
// stamped T reaches the runner's queue ahead of frame T; each event goes
// through event_intake_admit, the server's own dedupe-then-rate function
// (handle_session_event calls it: the events_seen dedupe on
// lifecycle_key_of(msg, connection) first - origin = the stamp on the uplink
// - then the per-second rate window), so the reliable duplicate is counted as
// a duplicate and the rate limit sees each event once, as before protocol 25;
// P and U of one origin are two events in either order (HLX: receivers never
// depend on copy order).
namespace {
    struct intake_mirror {
        struct window {
            uint32_t in_window = 0;
            std::chrono::steady_clock::time_point start{};
        };
        bmmo::session::event_seen seen;
        std::map<uint32_t, window> windows;   // per member, as server.cpp s.guards[c]
        uint32_t limit = 0;
        uint64_t duplicates = 0, rate_limited = 0;
        std::vector<std::string> log;   // what reached the runner's queue, in order

        void take_event(bmmo::session_event_msg& msg, uint32_t connection, std::chrono::steady_clock::time_point now) {
            msg.origin_tick = msg.tick;
            msg.state = 0;
            auto& w = windows[connection];
            const auto intake = bmmo::session::event_intake_admit(
                    seen, bmmo::session::lifecycle_key_of(msg, connection),
                    msg.type != bmmo::session::event_type::BodyRevived, w.in_window, w.start, now, limit);
            if (intake == bmmo::session::event_intake::duplicate) {
                ++duplicates;
                return;
            }
            if (intake == bmmo::session::event_intake::rate_limited) {
                ++rate_limited;
                return;
            }
            if (msg.type == bmmo::session::event_type::BodyRevived) return;   // dropped, not submitted
            log.push_back(std::string(bmmo::session::event_type_name(static_cast<int>(msg.type))) + "@"
                          + std::to_string(msg.tick));
        }
        void take_datagram(const bmmo::session_input_msg& msg, uint32_t connection, std::chrono::steady_clock::time_point now) {
            bmmo::session::take_input_datagram(
                    msg, [&](bmmo::session_event_msg& e) { take_event(e, connection, now); },
                    [&] { log.push_back("frames " + std::to_string(msg.first_tick) + ".."
                                        + std::to_string(msg.first_tick + msg.frames.size() - 1)); });
        }
    };
}

TEST(EventPiggyback, ServerIntakeEventsBeforeFramesDedupeAndRateOnce) {
    using bmmo::session::event_type;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    intake_mirror server;
    server.limit = 2;
    // A piggybacked P stamped 121 in the datagram whose newest frame is 121:
    // taken before the frames 114..121.
    auto dgram = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, 114);
    const std::string p = event_bytes(event_type::Physicalize, 121);
    dgram.events = {p};
    ASSERT_TRUE(dgram.serialize());
    const std::string wire = stream_payload(dgram.raw);
    bmmo::session_input_msg parsed{};
    parsed.raw.write(wire.data(), static_cast<std::streamsize>(wire.size()));
    ASSERT_TRUE(parsed.deserialize());
    server.take_datagram(parsed, 7, t0);
    ASSERT_EQ(server.log.size(), 2u);
    EXPECT_EQ(server.log[0], "physicalize@121");
    EXPECT_EQ(server.log[1], "frames 114..121");
    // The next datagram carries it again: a duplicate, not a second event.
    auto next = input_with_frames(bmmo::session::MAX_INPUT_FRAMES, 115);
    next.events = {p};
    server.take_datagram(next, 7, t0);
    // The reliable copy arrives after: counted as a duplicate.
    bmmo::session_event_msg reliable{};
    reliable.raw.write(p.data(), static_cast<std::streamsize>(p.size()));
    ASSERT_TRUE(reliable.deserialize());
    server.take_event(reliable, 7, t0);
    EXPECT_EQ(server.duplicates, 2u);
    EXPECT_EQ(server.windows[7].in_window, 1u) << "the rate limit saw the event once";
    EXPECT_EQ(server.log.size(), 3u);   // the second datagram's frames only
    // Another player's identical bytes are its own event.
    server.take_event(reliable, 8, t0);
    EXPECT_EQ(server.log.back(), "physicalize@121");
    // Over the limit (2 per second): the third distinct event is dropped,
    // its copies stay duplicates of nothing - and do not count again.
    auto u = input_with_frames(1, 122);
    u.events = {event_bytes(event_type::Unphysicalize, 122)};
    server.take_datagram(u, 7, t0);
    EXPECT_EQ(server.windows[7].in_window, 2u);
    auto s3 = input_with_frames(1, 123);
    s3.events = {event_bytes(event_type::Sector, 158, 2)};
    server.take_datagram(s3, 7, t0);
    EXPECT_EQ(server.rate_limited, 1u);
    server.take_datagram(s3, 7, t0);   // its piggybacked copy: a duplicate, not a second excess
    EXPECT_EQ(server.rate_limited, 1u);
    EXPECT_EQ(server.duplicates, 3u);

    // P and U of one origin (a beam): two events in either order, each once.
    for (int order = 0; order < 2; ++order) {
        intake_mirror beam;
        auto d = input_with_frames(2, 199);
        const std::string bu = event_bytes(event_type::Unphysicalize, 200);
        const std::string bp = event_bytes(event_type::Physicalize, 200);
        d.events = order == 0 ? std::vector<std::string>{bu, bp} : std::vector<std::string>{bp, bu};
        beam.take_datagram(d, 7, t0);
        ASSERT_EQ(beam.log.size(), 3u) << order;
        EXPECT_EQ(beam.log[2], "frames 199..200");
        // The reliable copies, in the other order: duplicates both.
        for (const auto& b: order == 0 ? std::vector<std::string>{bp, bu} : std::vector<std::string>{bu, bp}) {
            bmmo::session_event_msg r{};
            r.raw.write(b.data(), static_cast<std::streamsize>(b.size()));
            ASSERT_TRUE(r.deserialize());
            beam.take_event(r, 7, t0);
        }
        EXPECT_EQ(beam.duplicates, 2u) << order;
        EXPECT_EQ(beam.windows[7].in_window, 2u) << order;
    }

    // A BodyRevived passes the dedupe without counting toward the window
    // (the server drops it after the dedupe); its copy is a duplicate.
    intake_mirror revive;
    revive.limit = 1;
    bmmo::session_event_msg br{};
    const std::string brb = event_bytes(event_type::BodyRevived, 300);
    br.raw.write(brb.data(), static_cast<std::streamsize>(brb.size()));
    ASSERT_TRUE(br.deserialize());
    revive.take_event(br, 7, t0);
    revive.take_event(br, 7, t0);
    EXPECT_EQ(revive.windows[7].in_window, 0u);
    EXPECT_EQ(revive.duplicates, 1u);
    EXPECT_EQ(revive.rate_limited, 0u);
    EXPECT_TRUE(revive.log.empty());
}

// Protocol 34: a mute set round-trips sorted and unique, a count past the cap
// is refused, and the server's filter keeps the other players' balls and
// timestamps (nothing at all when every one is muted).
TEST(BallStateMuteMsg, RoundTripCapAndFilter) {
    bmmo::ball_state_mute_msg msg{};
    msg.players = {7, 3, 7, 9};
    EXPECT_EQ(round_trip(msg).players, (std::vector<uint32_t>{3, 7, 9}));
    bmmo::ball_state_mute_msg big{};
    big.players.assign(bmmo::ball_state_mute_msg::kMaxPlayers, 1u);
    EXPECT_EQ(round_trip(big).players.size(), 1u);
    big.serialize();
    std::string bytes = big.raw.str();
    const uint16_t over = static_cast<uint16_t>(bmmo::ball_state_mute_msg::kMaxPlayers + 1);
    std::memcpy(bytes.data() + sizeof(bmmo::opcode), &over, sizeof(over));
    bmmo::ball_state_mute_msg parsed{};
    parsed.raw.str(bytes);
    EXPECT_FALSE(parsed.deserialize());

    bmmo::owned_compressed_ball_state_msg states{};
    for (const uint32_t id: {3u, 5u, 9u}) states.balls.push_back({{}, static_cast<bmmo::net::connection>(id)});
    states.unchanged_balls.push_back({{}, static_cast<bmmo::net::connection>(9)});
    states.unchanged_balls.push_back({{}, static_cast<bmmo::net::connection>(5)});
    bmmo::owned_compressed_ball_state_msg kept{};
    ASSERT_TRUE(bmmo::without_muted(states, {3, 9}, kept));
    ASSERT_EQ(kept.balls.size(), 1u);
    EXPECT_EQ(kept.balls[0].player_id, 5u);
    ASSERT_EQ(kept.unchanged_balls.size(), 1u);
    EXPECT_EQ(kept.unchanged_balls[0].player_id, 5u);
    EXPECT_FALSE(bmmo::without_muted(states, {3, 5, 9}, kept));
}
