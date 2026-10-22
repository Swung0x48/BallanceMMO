// Unit tests for the collision-overhaul physics session wire messages
// (docs/rooms-and-sessions-protocol.md 2.2): serialize()/deserialize()
// round trips, every truncation of a valid payload, and forged element
// counts a malicious peer could send (both above the hard cap and larger
// than what the remaining bytes could possibly hold).
#include <gtest/gtest.h>

#include <cstring>
#include <string>

// the individual message headers are not self-contained (they rely on the
// include order in message_all.hpp), so pull in the aggregate
#include <message/message_all.hpp>
#include <message/message_utils.hpp>

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
    bytes.resize(bytes.size() - sizeof(uint32_t));   // what a protocol 7 build wrote
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
    payload.resize(payload.size() - sizeof(uint8_t));
    const uint8_t forged_count = static_cast<uint8_t>(bmmo::session::MAX_INPUT_FRAMES + 1);
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

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
    payload.resize(payload.size() - sizeof(uint8_t));
    const uint8_t forged_count = 3; // within the cap, but no frame data follows
    payload.append(reinterpret_cast<const char*>(&forged_count), sizeof(forged_count));

    bmmo::session_input_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
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
// session_remote_input_msg (design 9.1; the window layout, protocol 5)
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

TEST(SessionRemoteInputMsg, SerializeDeserializeRoundTripWithTheWindow) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 4;
    msg.tick = 1234;
    bmmo::session_remote_input_msg::entry e;
    e.player = 77;
    e.frames.push_back(relay_frame(5, -1.0f));
    e.frames.push_back(relay_frame(1, -1.0f));   // the camera did not move: omitted on the wire
    e.frames.push_back(relay_frame(1, -0.5f));   // it did: carried again
    msg.entries.push_back(e);
    bmmo::session_remote_input_msg::entry single;
    single.player = 78;
    single.frames.push_back(relay_frame(0, 0.25f));
    msg.entries.push_back(single);
    bmmo::session_remote_input_msg::entry empty;   // nothing to say: not written
    empty.player = 79;
    msg.entries.push_back(empty);
    auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.session, 4u);
    EXPECT_EQ(parsed.tick, 1234u);
    ASSERT_EQ(parsed.entries.size(), 2u);
    EXPECT_EQ(parsed.entries[0].player, 77u);
    ASSERT_EQ(parsed.entries[0].frames.size(), 3u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[0], 0), 1232u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[0], 2), 1234u);
    EXPECT_EQ(parsed.entries[0].frames[0].keys, 5);
    EXPECT_EQ(parsed.entries[0].frames[1].keys, 1);
    EXPECT_FLOAT_EQ(parsed.entries[0].frames[1].cam_dir[2], -1.0f);   // copied from the frame before
    EXPECT_FLOAT_EQ(parsed.entries[0].frames[1].cam_right[0], 1.0f);
    EXPECT_FLOAT_EQ(parsed.entries[0].frames[2].cam_dir[2], -0.5f);
    EXPECT_EQ(parsed.entries[0].frames[2].ball_type, 2);
    EXPECT_EQ(parsed.entries[0].frames[2].flags, 5);
    EXPECT_EQ(parsed.entries[1].player, 78u);
    ASSERT_EQ(parsed.entries[1].frames.size(), 1u);
    EXPECT_FLOAT_EQ(parsed.entries[1].frames[0].cam_dir[2], 0.25f);
    // The still camera is what makes the window affordable: 4 bytes a frame.
    bmmo::session_remote_input_msg still{};
    still.tick = 10;
    bmmo::session_remote_input_msg::entry se;
    se.player = 1;
    for (int i = 0; i < 8; ++i) se.frames.push_back(relay_frame(static_cast<uint8_t>(i), -1.0f));
    still.entries.push_back(se);
    ASSERT_TRUE(still.serialize());
    // opcode, session, tick, count, acked, held_total, epoch; then the
    // entry's player and nframes; and the empty held section's count.
    const size_t header = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1 + sizeof(uint32_t) * 3 + sizeof(uint32_t) + 1;
    EXPECT_EQ(still.size(), header + 4 + 36 + 7 * 4 + 1);
}

TEST(SessionRemoteInputMsg, RejectsTruncationBadCountsAndUnknownHeaders) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 1;
    msg.tick = 1000;   // far from tick 0, so only the caps below can reject a count
    bmmo::session_remote_input_msg::entry e;
    e.player = 9;
    e.frames.push_back(relay_frame(1, 1.0f));
    e.frames.push_back(relay_frame(2, 1.0f));
    msg.entries.push_back(e);
    expect_rejects_every_truncation(msg);
    // serialize() appends: start the stream again, or the payload is two
    // messages (nothing may follow the held section since protocol 7).
    msg.raw = std::stringstream{};
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);
    const size_t count_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2;
    const size_t nframes_offset = count_offset + 1 + sizeof(uint32_t) * 3 + sizeof(uint32_t);
    const size_t first_header_offset = nframes_offset + 1;
    const size_t second_header_offset = first_header_offset + 4 + 36;
    auto parses = [&](size_t offset, char value) {
        std::string copy = payload;
        copy[offset] = value;
        bmmo::session_remote_input_msg parsed{};
        parsed.raw.write(copy.data(), static_cast<std::streamsize>(copy.size()));
        return parsed.deserialize();
    };
    EXPECT_FALSE(parses(count_offset, static_cast<char>(bmmo::session::MAX_PLAYERS_PER_SESSION + 1)));
    EXPECT_FALSE(parses(nframes_offset, 0));
    EXPECT_FALSE(parses(nframes_offset, static_cast<char>(bmmo::session::MAX_RELAY_FRAMES + 1)));
    EXPECT_FALSE(parses(first_header_offset, 0));      // the first frame must carry the camera
    EXPECT_FALSE(parses(second_header_offset, 2));     // a header bit this reader does not know
    EXPECT_TRUE(parses(second_header_offset, 0));      // the second frame may omit it
    // More frames than ticks before tick 0: rejected rather than wrapped.
    bmmo::session_remote_input_msg early{};
    early.tick = 2;
    bmmo::session_remote_input_msg::entry ee;
    ee.player = 1;
    for (int i = 0; i < 4; ++i) ee.frames.push_back(relay_frame(1, 1.0f));
    early.entries.push_back(ee);
    ASSERT_TRUE(early.serialize());
    bmmo::session_remote_input_msg parsed_early{};
    const std::string early_payload = stream_payload(early.raw);
    parsed_early.raw.write(early_payload.data(), static_cast<std::streamsize>(early_payload.size()));
    EXPECT_FALSE(parsed_early.deserialize());
    bmmo::session_remote_input_msg later{};
    later.tick = 3;
    later.entries.push_back(ee);
    ASSERT_TRUE(later.serialize());
    bmmo::session_remote_input_msg parsed_ok{};
    const std::string ok_payload = stream_payload(later.raw);
    parsed_ok.raw.write(ok_payload.data(), static_cast<std::streamsize>(ok_payload.size()));
    EXPECT_TRUE(parsed_ok.deserialize());
    EXPECT_EQ(parsed_ok.frame_tick(parsed_ok.entries[0], 0), 0u);
}

// More frames than the window's cap: the newest MAX_RELAY_FRAMES go out.
TEST(SessionRemoteInputMsg, ClampsAnEntryToTheNewestFrames) {
    bmmo::session_remote_input_msg msg{};
    msg.tick = 100;
    bmmo::session_remote_input_msg::entry e;
    e.player = 3;
    for (int i = 0; i < 40; ++i) e.frames.push_back(relay_frame(static_cast<uint8_t>(i), 1.0f));
    msg.entries.push_back(e);
    auto parsed = round_trip(msg);
    ASSERT_EQ(parsed.entries.size(), 1u);
    ASSERT_EQ(parsed.entries[0].frames.size(), bmmo::session::MAX_RELAY_FRAMES);
    EXPECT_EQ(parsed.entries[0].frames.front().keys, 40 - static_cast<int>(bmmo::session::MAX_RELAY_FRAMES));
    EXPECT_EQ(parsed.entries[0].frames.back().keys, 39);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[0], 0), 100u - static_cast<uint32_t>(bmmo::session::MAX_RELAY_FRAMES) + 1);
}

// Protocol 7 (late-player design 1.1): the recipient's acked, held_total and
// epoch in the header, and the held section after the entries - by
// reference, inline, and the default frame.
TEST(SessionRemoteInputMsg, V3RoundTripWithTheHeldSection) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 4;
    msg.tick = 1234;
    msg.acked = 1201;
    msg.held_total = 17;
    msg.epoch = 1000;
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
    // 5 B a reference, 41 B inline, 1 B of count.
    const size_t entry_bytes = 4 + 1 + 40 + 4;
    EXPECT_EQ(msg.size(), bmmo::session::kRelayDatagramHeader + entry_bytes + 1 + 5 + 41 + 5);
    msg.raw = std::stringstream{};   // serialize() appends
    expect_rejects_every_truncation(msg);
    // A header-only datagram: nothing relayed, nothing substituted.
    bmmo::session_remote_input_msg header_only{};
    header_only.tick = 50;
    header_only.acked = 50;
    header_only.epoch = 40;
    auto parsed_header = round_trip(header_only);
    EXPECT_EQ(header_only.size(), bmmo::session::kRelayDatagramHeader + 1);
    EXPECT_TRUE(parsed_header.entries.empty());
    EXPECT_TRUE(parsed_header.held.empty());
    EXPECT_EQ(parsed_header.acked, 50u);
    EXPECT_EQ(parsed_header.epoch, 40u);
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
    std::string thirty_three = with(nheld_offset, 33);
    for (int i = 0; i < 32; ++i) thirty_three += payload.substr(back_offset, 5);
    EXPECT_FALSE(parses(thirty_three));
    std::string thirty_two = with(nheld_offset, 32);
    for (int i = 0; i < 31; ++i) thirty_two += payload.substr(back_offset, 5);
    EXPECT_TRUE(parses(thirty_two));
    EXPECT_FALSE(parses(with(back_offset, 3 | 0x20)));
    EXPECT_FALSE(parses(with(back_offset, 3 | 0x40)));
    EXPECT_FALSE(parses(payload + std::string(1, '\0')));   // nothing may follow the section
    bmmo::session_remote_input_msg early{};
    early.tick = 2;
    bmmo::session_remote_input_msg::held_record before_zero;
    before_zero.back = 3;
    early.held.push_back(before_zero);
    ASSERT_TRUE(early.serialize());
    EXPECT_EQ(early.size(), bmmo::session::kRelayDatagramHeader + 1);   // the serializer drops it ...
    std::string early_payload = stream_payload(early.raw);
    early_payload.back() = 1;
    early_payload += std::string(1, 3) + std::string(4, '\0');
    EXPECT_FALSE(parses(early_payload));                                // ... and the reader refuses it
    early_payload[early_payload.size() - 5] = 2;
    EXPECT_TRUE(parses(early_payload));                                 // back 2 at tick 2 is tick 0
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
