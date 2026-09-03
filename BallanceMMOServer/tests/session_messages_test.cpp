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
    msg.first_tick = 132;
    msg.seed = -42;
    msg.spawn_impulse = 2.5f;

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
    EXPECT_EQ(132u, parsed.first_tick);
    EXPECT_EQ(-42, parsed.seed);
    EXPECT_FLOAT_EQ(2.5f, parsed.spawn_impulse);
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

bmmo::session_event_msg make_physicalize_event() {
    bmmo::session_event_msg msg{};
    msg.session = 5;
    msg.player = 20;
    msg.tick = 300;
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
    // trailing bytes are convex_count(0), ball_count(0), concave_count(0)
    payload.resize(payload.size() - 3 * sizeof(uint8_t));
    const uint8_t forged_convex_count = static_cast<uint8_t>(bmmo::session::MAX_CONVEX + 1);
    const uint8_t zero = 0;
    payload.append(reinterpret_cast<const char*>(&forged_convex_count), sizeof(forged_convex_count));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));

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
    payload.resize(payload.size() - 3 * sizeof(uint8_t));
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
    // trailing bytes: convex_count(0), ball_count(0), concave_count(0); keep
    // convex_count(0), forge ball_count
    payload.resize(payload.size() - 2 * sizeof(uint8_t));
    const uint8_t forged_ball_count = static_cast<uint8_t>(bmmo::session::MAX_CONVEX + 1);
    const uint8_t zero = 0;
    payload.append(reinterpret_cast<const char*>(&forged_ball_count), sizeof(forged_ball_count));
    payload.append(reinterpret_cast<const char*>(&zero), sizeof(zero));

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
// session_remote_input_msg (design 9.1)
// ---------------------------------------------------------------------

TEST(SessionRemoteInputMsg, SerializeDeserializeRoundTrip) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 4;
    msg.tick = 1234;
    bmmo::session_remote_input_msg::entry e;
    e.player = 77;
    e.frame.keys = 5;
    e.frame.cam_right[0] = 0.5f;
    e.frame.cam_up[1] = 1.0f;
    e.frame.cam_dir[2] = -1.0f;
    e.frame.ball_type = 2;
    e.frame.flags = 5;
    msg.entries.push_back(e);
    e.player = 78;
    e.frame.keys = 0;
    msg.entries.push_back(e);
    auto parsed = round_trip(msg);
    EXPECT_EQ(parsed.session, 4u);
    EXPECT_EQ(parsed.tick, 1234u);
    ASSERT_EQ(parsed.entries.size(), 2u);
    EXPECT_EQ(parsed.entries[0].player, 77u);
    EXPECT_EQ(parsed.entries[0].frame.keys, 5);
    EXPECT_FLOAT_EQ(parsed.entries[0].frame.cam_right[0], 0.5f);
    EXPECT_FLOAT_EQ(parsed.entries[0].frame.cam_dir[2], -1.0f);
    EXPECT_EQ(parsed.entries[0].frame.ball_type, 2);
    EXPECT_EQ(parsed.entries[0].frame.flags, 5);
    EXPECT_EQ(parsed.entries[1].player, 78u);
    EXPECT_EQ(parsed.entries[1].frame.keys, 0);
}

TEST(SessionRemoteInputMsg, RejectsTruncationAndOversizedCount) {
    bmmo::session_remote_input_msg msg{};
    msg.session = 1;
    msg.tick = 2;
    bmmo::session_remote_input_msg::entry e;
    e.player = 9;
    msg.entries.push_back(e);
    expect_rejects_every_truncation(msg);
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    // count byte follows opcode + session + tick
    const size_t count_offset = sizeof(bmmo::opcode) + sizeof(uint32_t) * 2;
    payload[count_offset] = static_cast<char>(bmmo::session::MAX_PLAYERS_PER_SESSION + 1);
    bmmo::session_remote_input_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}
