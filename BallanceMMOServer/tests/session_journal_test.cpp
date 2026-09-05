// Unit tests for the session black box (session/journal.hpp): a synthetic
// journal with every record type must survive a round trip byte for byte, and
// the reader must survive everything a crashed writer can leave behind - a
// record cut in half, a tag from a newer version, a file with nothing but a
// header, a file the size cap stopped.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <session/journal.hpp>

namespace {
    using namespace bmmo::session;

    // Every test works in its own directory under the system temp path and
    // takes it away again.
    struct temp_dir {
        std::filesystem::path path;

        explicit temp_dir(const std::string& name)
            : path(std::filesystem::temp_directory_path() / ("bmmo_journal_" + name)) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path, ec);
        }
        ~temp_dir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        temp_dir(const temp_dir&) = delete;
        temp_dir& operator=(const temp_dir&) = delete;

        std::filesystem::path file(const std::string& name) const { return path / name; }
    };

    journal_header make_header() {
        journal_header h;
        h.kind = journal_kind::client;
        h.session = 7;
        h.level = 3;
        h.seed = 12345;
        h.spawn_impulse = 1.25f;
        h.input_delay = 4;
        h.checkpoint_ticks = 660;
        h.first_tick = 100;
        h.anchor_hash = 0xdeadbeefcafef00dULL;
        h.anchor_surfaces = 0x0102030405060708ULL;
        h.build_id = "ballanced-0123456789ab+bridge-7";
        h.utc_ms = 1725400000000ULL;
        h.own_player = 42;
        h.own_join_order = 1;
        return h;
    }

    input_frame make_frame(uint8_t keys, float base, uint8_t ball_type, uint8_t flags) {
        input_frame f;
        f.keys = keys;
        for (int k = 0; k < 3; ++k) {
            f.cam_right[k] = base + static_cast<float>(k);
            f.cam_up[k] = base + 10.0f + static_cast<float>(k);
            f.cam_dir[k] = base + 20.0f + static_cast<float>(k);
        }
        f.ball_type = ball_type;
        f.flags = flags;
        return f;
    }

    void expect_same_frame(const input_frame& expected, const input_frame& actual) {
        EXPECT_EQ(expected.keys, actual.keys);
        EXPECT_EQ(expected.ball_type, actual.ball_type);
        EXPECT_EQ(expected.flags, actual.flags);
        for (int k = 0; k < 3; ++k) {
            EXPECT_EQ(expected.cam_right[k], actual.cam_right[k]) << "cam_right " << k;
            EXPECT_EQ(expected.cam_up[k], actual.cam_up[k]) << "cam_up " << k;
            EXPECT_EQ(expected.cam_dir[k], actual.cam_dir[k]) << "cam_dir " << k;
        }
    }

    // A recipe with every field filled: 3 convex meshes, 2 spheres, 1 concave.
    bmmo_physics_ball_recipe make_recipe() {
        bmmo_physics_ball_recipe r{};
        r.fixed = false;
        r.start_frozen = true;
        r.enable_collision = true;
        r.calc_mass_center = false;
        r.friction = 0.7f;
        r.elasticity = 0.5f;
        r.mass = 2.0f;
        r.linear_damp = 0.125f;
        r.rot_damp = 0.25f;
        for (int k = 0; k < 3; ++k) r.mass_center[k] = 0.01f * static_cast<float>(k + 1);
        std::snprintf(r.collision_surface, sizeof(r.collision_surface), "Stone");
        r.convex_count = 3;
        for (int i = 0; i < r.convex_count; ++i)
            std::snprintf(r.convex[i], sizeof(r.convex[i]), "Convex_Mesh_%d", i);
        r.ball_count = 2;
        for (int i = 0; i < r.ball_count; ++i) {
            for (int k = 0; k < 3; ++k) r.ball_center[i][k] = static_cast<float>(i) + 0.5f * static_cast<float>(k);
            r.ball_radius[i] = 0.75f + static_cast<float>(i);
        }
        r.concave_count = 1;
        std::snprintf(r.concave[0], sizeof(r.concave[0]), "Concave_Mesh_0");
        return r;
    }

    void expect_same_recipe(const bmmo_physics_ball_recipe& expected, const bmmo_physics_ball_recipe& actual) {
        EXPECT_EQ(expected.fixed, actual.fixed);
        EXPECT_EQ(expected.start_frozen, actual.start_frozen);
        EXPECT_EQ(expected.enable_collision, actual.enable_collision);
        EXPECT_EQ(expected.calc_mass_center, actual.calc_mass_center);
        EXPECT_EQ(expected.friction, actual.friction);
        EXPECT_EQ(expected.elasticity, actual.elasticity);
        EXPECT_EQ(expected.mass, actual.mass);
        EXPECT_EQ(expected.linear_damp, actual.linear_damp);
        EXPECT_EQ(expected.rot_damp, actual.rot_damp);
        for (int k = 0; k < 3; ++k) EXPECT_EQ(expected.mass_center[k], actual.mass_center[k]) << "mass_center " << k;
        EXPECT_STREQ(expected.collision_surface, actual.collision_surface);
        ASSERT_EQ(expected.convex_count, actual.convex_count);
        for (int i = 0; i < expected.convex_count; ++i) EXPECT_STREQ(expected.convex[i], actual.convex[i]);
        ASSERT_EQ(expected.ball_count, actual.ball_count);
        for (int i = 0; i < expected.ball_count; ++i) {
            for (int k = 0; k < 3; ++k) EXPECT_EQ(expected.ball_center[i][k], actual.ball_center[i][k]);
            EXPECT_EQ(expected.ball_radius[i], actual.ball_radius[i]);
        }
        ASSERT_EQ(expected.concave_count, actual.concave_count);
        for (int i = 0; i < expected.concave_count; ++i) EXPECT_STREQ(expected.concave[i], actual.concave[i]);
    }

    bmmo::physics::world_hash make_hash(uint32_t tick) {
        bmmo::physics::world_hash h;
        h.hash = 0x100000000ULL + tick;
        h.pose = 0x200000000ULL + tick;
        h.cores = 12;
        std::snprintf(h.probe_name, sizeof(h.probe_name), "Ball_Paper_42");
        for (int k = 0; k < 3; ++k) {
            h.probe_position[k] = static_cast<double>(tick) + 0.25 * k;
            h.probe_speed[k] = static_cast<float>(tick) * 0.5f + static_cast<float>(k);
        }
        return h;
    }

    body_state make_body(body_kind kind, uint32_t owner, const std::string& name, double base) {
        body_state b;
        b.kind = kind;
        b.owner = owner;
        b.name = name;
        for (int k = 0; k < 3; ++k) {
            b.position[k] = base + k;
            b.linear[k] = static_cast<float>(base) + 0.5f * static_cast<float>(k);
            b.angular[k] = static_cast<float>(base) - 0.5f * static_cast<float>(k);
        }
        for (int k = 0; k < 4; ++k) b.rotation[k] = base * 0.125 + k;
        b.flags = BODY_FLAG_SIMULATED | BODY_FLAG_COLLISION_ENABLED;
        return b;
    }

    std::vector<body_state> make_bodies() {
        return {make_body(body_kind::Ball, 42, "Ball_Paper_42", 1.0),
                make_body(body_kind::Ball, 43, "Ball_Stone_43", 2.0),
                make_body(body_kind::Mechanism, 3, "Trafo_Piece_3", 3.0)};
    }

    journal_correction make_correction(uint32_t tick, uint32_t local_tick, uint8_t kind, const std::string& entity) {
        journal_correction c;
        c.tick = tick;
        c.local_tick = local_tick;
        c.kind = kind;
        c.entity = entity;
        c.error_m = 0.375f;
        c.velocity_error = 1.5f;
        for (int k = 0; k < 3; ++k) {
            c.local_position[k] = 10.0 + k;
            c.server_position[k] = 20.0 + k;
        }
        return c;
    }

    const input_frame kFrameA = make_frame(3, 1.0f, 1, INPUT_FLAG_PHYSICALIZED | INPUT_FLAG_NAV_ACTIVE);
    const input_frame kFrameB = make_frame(5, 2.0f, 1, INPUT_FLAG_PHYSICALIZED);
    const input_frame kFrameC = make_frame(1, 3.0f, 2, INPUT_FLAG_NAV_ACTIVE);
    const input_frame kFrameD = make_frame(0, 4.0f, 2, 0);

    // The synthetic session every round-trip test uses: 23 records over the
    // ticks 100..103, with repeated and fresh inputs for two players, both
    // event shapes, a mixed checkpoint, notes and corrections.  Returns the
    // offset the closing NOTE starts at, so a test can cut the file inside it.
    uint64_t write_synthetic(const std::filesystem::path& path) {
        journal_writer writer;
        std::string error;
        EXPECT_TRUE(writer.open(path, make_header(), 0, error)) << error;

        writer.note(100, "start: room 1 \"test room\"");
        writer.player(100, 42, 1, true, "alice");
        writer.player(100, 43, 2, true, "bob");
        writer.input(100, 42, kFrameA, JOURNAL_INPUT_FRESH);
        writer.input(100, 43, kFrameC, JOURNAL_INPUT_FRESH | JOURNAL_INPUT_RELAYED);
        writer.tick(100, make_hash(100), 15);
        // a member learned about after the first tick: same tick, but not one
        // of the initial members
        writer.player(100, 44, 3, true, "carol");

        writer.input(101, 42, kFrameA, JOURNAL_INPUT_FRESH);   // repeat
        writer.input(101, 43, kFrameD, JOURNAL_INPUT_RELAYED);
        journal_event physicalize;
        physicalize.tick = 101;
        physicalize.event_tick = 101;   // applied at the tick it was stamped for
        physicalize.id = 42;
        physicalize.type = event_type::Physicalize;
        physicalize.ball_type = 1;
        physicalize.flags = PHYSICALIZE_FLAG_SPAWN;
        for (int k = 0; k < 3; ++k) physicalize.position[k] = 100.0f + static_cast<float>(k);
        for (int i = 0; i < 9; ++i) physicalize.rotation[i] = 0.5f * static_cast<float>(i);
        physicalize.sector = 2;
        physicalize.name = "Ball_Paper_42";
        physicalize.recipe = make_recipe();
        writer.event(physicalize);
        writer.correction(make_correction(101, 104, 0, "Ball_Stone_43"));
        writer.tick(101, make_hash(101), 30);

        writer.input(102, 42, kFrameB, JOURNAL_INPUT_FRESH);
        writer.input(102, 43, kFrameD, JOURNAL_INPUT_RELAYED);   // repeat
        journal_event revived;
        revived.tick = 102;
        revived.event_tick = 102;
        revived.id = 43;
        revived.type = event_type::BodyRevived;
        revived.name = "Trafo_Piece_3";
        writer.event(revived);
        writer.tick(102, make_hash(102), 45);
        writer.checkpoint(102, JOURNAL_CHECKPOINT_FULL, make_bodies());
        writer.correction(make_correction(102, 105, 1, "Ball_Paper_42"));

        writer.input(103, 42, kFrameB, JOURNAL_INPUT_FRESH);   // repeat
        writer.tick(103, make_hash(103), 60);
        writer.player(103, 43, 2, false, "bob");

        const uint64_t last_record = writer.bytes();
        writer.note(103, "end: session over");
        writer.close();
        return last_record;
    }

    std::string read_file(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    void write_file(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

TEST(SessionJournal, RoundTripOfEveryRecordType) {
    const temp_dir dir("round_trip");
    const auto path = dir.file("session.bmjr");
    write_synthetic(path);

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    EXPECT_TRUE(out.warning.empty()) << out.warning;
    EXPECT_EQ(0u, out.bytes_dropped);
    EXPECT_EQ(0u, out.unknown_records);
    EXPECT_EQ(23u, out.records);
    EXPECT_EQ(std::filesystem::file_size(path), out.bytes_read);

    const journal_header expected = make_header();
    EXPECT_EQ(expected.kind, out.header.kind);
    EXPECT_EQ(expected.session, out.header.session);
    EXPECT_EQ(expected.level, out.header.level);
    EXPECT_EQ(expected.seed, out.header.seed);
    EXPECT_EQ(expected.spawn_impulse, out.header.spawn_impulse);
    EXPECT_EQ(expected.input_delay, out.header.input_delay);
    EXPECT_EQ(expected.checkpoint_ticks, out.header.checkpoint_ticks);
    EXPECT_EQ(expected.first_tick, out.header.first_tick);
    EXPECT_EQ(expected.anchor_hash, out.header.anchor_hash);
    EXPECT_EQ(expected.anchor_surfaces, out.header.anchor_surfaces);
    EXPECT_EQ(expected.build_id, out.header.build_id);
    EXPECT_EQ(expected.utc_ms, out.header.utc_ms);
    EXPECT_EQ(expected.own_player, out.header.own_player);
    EXPECT_EQ(expected.own_join_order, out.header.own_join_order);

    // the founding members: the ones at first_tick before the first TICK
    ASSERT_EQ(2u, out.initial_players.size());
    EXPECT_EQ(42u, out.initial_players[0].id);
    EXPECT_EQ("alice", out.initial_players[0].name);
    EXPECT_EQ(1, out.initial_players[0].join_order);
    EXPECT_TRUE(out.initial_players[0].added);
    EXPECT_EQ(43u, out.initial_players[1].id);
    EXPECT_EQ("bob", out.initial_players[1].name);

    ASSERT_EQ(4u, out.ticks.size());
    for (size_t i = 0; i < out.ticks.size(); ++i) {
        EXPECT_EQ(100u + i, out.ticks[i].tick);
        EXPECT_TRUE(out.ticks[i].has_tick);
        const auto& record = out.ticks[i].record;
        const auto hash = make_hash(static_cast<uint32_t>(100 + i));
        EXPECT_EQ(hash.hash, record.hash);
        EXPECT_EQ(hash.pose, record.pose);
        EXPECT_EQ(hash.cores, record.cores);
        EXPECT_EQ("Ball_Paper_42", record.probe_name);
        for (int k = 0; k < 3; ++k) {
            EXPECT_EQ(hash.probe_position[k], record.probe_position[k]);
            EXPECT_EQ(hash.probe_speed[k], record.probe_speed[k]);
        }
    }
    EXPECT_EQ(15u, out.ticks[0].record.ms);
    EXPECT_EQ(60u, out.ticks[3].record.ms);

    // tick 100: three player records (carol is not an initial member), two
    // fresh inputs, the start note
    const auto& first = out.ticks[0];
    ASSERT_EQ(3u, first.players.size());
    EXPECT_EQ(44u, first.players[2].id);
    EXPECT_EQ("carol", first.players[2].name);
    ASSERT_EQ(2u, first.inputs.size());
    EXPECT_EQ(42u, first.inputs[0].id);
    EXPECT_FALSE(first.inputs[0].repeat);
    EXPECT_EQ(JOURNAL_INPUT_FRESH, first.inputs[0].flags);
    expect_same_frame(kFrameA, first.inputs[0].frame);
    EXPECT_EQ(JOURNAL_INPUT_FRESH | JOURNAL_INPUT_RELAYED, first.inputs[1].flags);
    expect_same_frame(kFrameC, first.inputs[1].frame);
    ASSERT_EQ(1u, first.notes.size());
    EXPECT_EQ("start: room 1 \"test room\"", first.notes[0].text);

    // tick 101: player 42 repeats its frame, player 43 sends a new one
    const auto& second = out.ticks[1];
    ASSERT_EQ(2u, second.inputs.size());
    EXPECT_TRUE(second.inputs[0].repeat);
    expect_same_frame(kFrameA, second.inputs[0].frame);
    EXPECT_FALSE(second.inputs[1].repeat);
    expect_same_frame(kFrameD, second.inputs[1].frame);
    ASSERT_EQ(1u, second.events.size());
    const auto& physicalize = second.events[0];
    EXPECT_EQ(101u, physicalize.tick);
    EXPECT_EQ(42u, physicalize.id);
    EXPECT_EQ(event_type::Physicalize, physicalize.type);
    EXPECT_EQ(1, physicalize.ball_type);
    EXPECT_EQ(PHYSICALIZE_FLAG_SPAWN, physicalize.flags);
    EXPECT_EQ(2, physicalize.sector);
    // stamped for the tick it was applied at, like an event that arrived in time
    EXPECT_EQ(101u, physicalize.event_tick);
    EXPECT_EQ("Ball_Paper_42", physicalize.name);
    for (int k = 0; k < 3; ++k) EXPECT_EQ(100.0f + static_cast<float>(k), physicalize.position[k]);
    for (int i = 0; i < 9; ++i) EXPECT_EQ(0.5f * static_cast<float>(i), physicalize.rotation[i]);
    expect_same_recipe(make_recipe(), physicalize.recipe);
    ASSERT_EQ(1u, second.corrections.size());
    EXPECT_EQ(0, second.corrections[0].kind);
    EXPECT_EQ(104u, second.corrections[0].local_tick);
    EXPECT_EQ("Ball_Stone_43", second.corrections[0].entity);
    EXPECT_EQ(0.375f, second.corrections[0].error_m);
    EXPECT_EQ(1.5f, second.corrections[0].velocity_error);
    for (int k = 0; k < 3; ++k) {
        EXPECT_EQ(10.0 + k, second.corrections[0].local_position[k]);
        EXPECT_EQ(20.0 + k, second.corrections[0].server_position[k]);
    }

    // tick 102: player 43 repeats, a BodyRevived event with a zeroed recipe,
    // the checkpoint and the rollback correction
    const auto& third = out.ticks[2];
    ASSERT_EQ(2u, third.inputs.size());
    EXPECT_FALSE(third.inputs[0].repeat);
    expect_same_frame(kFrameB, third.inputs[0].frame);
    EXPECT_TRUE(third.inputs[1].repeat);
    expect_same_frame(kFrameD, third.inputs[1].frame);
    ASSERT_EQ(1u, third.events.size());
    EXPECT_EQ(event_type::BodyRevived, third.events[0].type);
    EXPECT_EQ("Trafo_Piece_3", third.events[0].name);
    expect_same_recipe(bmmo_physics_ball_recipe{}, third.events[0].recipe);
    ASSERT_EQ(1u, third.checkpoints.size());
    EXPECT_EQ(JOURNAL_CHECKPOINT_FULL, third.checkpoints[0].flags);
    const auto expected_bodies = make_bodies();
    ASSERT_EQ(expected_bodies.size(), third.checkpoints[0].bodies.size());
    for (size_t i = 0; i < expected_bodies.size(); ++i) {
        const auto& a = expected_bodies[i];
        const auto& b = third.checkpoints[0].bodies[i];
        EXPECT_EQ(a.kind, b.kind);
        EXPECT_EQ(a.owner, b.owner);
        EXPECT_EQ(a.name, b.name);
        EXPECT_EQ(a.flags, b.flags);
        for (int k = 0; k < 3; ++k) {
            EXPECT_EQ(a.position[k], b.position[k]) << "body " << i << " position " << k;
            EXPECT_EQ(a.linear[k], b.linear[k]);
            EXPECT_EQ(a.angular[k], b.angular[k]);
        }
        for (int k = 0; k < 4; ++k) EXPECT_EQ(a.rotation[k], b.rotation[k]);
    }
    ASSERT_EQ(1u, third.corrections.size());
    EXPECT_EQ(1, third.corrections[0].kind);

    // tick 103: the last repeated input, the leave, the end note
    const auto& fourth = out.ticks[3];
    ASSERT_EQ(1u, fourth.inputs.size());
    EXPECT_TRUE(fourth.inputs[0].repeat);
    expect_same_frame(kFrameB, fourth.inputs[0].frame);
    ASSERT_EQ(1u, fourth.players.size());
    EXPECT_FALSE(fourth.players[0].added);
    EXPECT_EQ(43u, fourth.players[0].id);

    // every NOTE in file order
    ASSERT_EQ(2u, out.notes.size());
    EXPECT_EQ(100u, out.notes[0].tick);
    EXPECT_EQ("start: room 1 \"test room\"", out.notes[0].text);
    EXPECT_EQ(103u, out.notes[1].tick);
    EXPECT_EQ("end: session over", out.notes[1].text);
}

TEST(SessionJournal, TruncatedTailKeepsTheTicksBeforeIt) {
    const temp_dir dir("truncated");
    const auto path = dir.file("session.bmjr");
    const uint64_t last_record = write_synthetic(path);
    const std::string bytes = read_file(path);
    ASSERT_GT(bytes.size(), last_record);

    // (a) cut in the middle of the last record's payload
    const auto middle = dir.file("middle.bmjr");
    write_file(middle, bytes.substr(0, bytes.size() - 8));
    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(middle, out, error)) << error;
    EXPECT_EQ(4u, out.ticks.size());
    EXPECT_EQ(22u, out.records);           // everything but the closing note
    EXPECT_GT(out.bytes_dropped, 0u);
    EXPECT_EQ(bytes.size() - 8 - last_record, out.bytes_dropped);
    ASSERT_EQ(1u, out.notes.size());       // the "end:" note is gone
    EXPECT_EQ("start: room 1 \"test room\"", out.notes[0].text);
    EXPECT_NE(std::string::npos, out.warning.find("truncated")) << out.warning;
    EXPECT_EQ(last_record, out.bytes_read);

    // (b) cut in the middle of the last record's size field
    const auto head = dir.file("head.bmjr");
    write_file(head, bytes.substr(0, static_cast<size_t>(last_record) + 3));
    journal partial;
    ASSERT_TRUE(read_journal(head, partial, error)) << error;
    EXPECT_EQ(4u, partial.ticks.size());
    EXPECT_EQ(22u, partial.records);
    EXPECT_EQ(3u, partial.bytes_dropped);
    EXPECT_EQ(1u, partial.notes.size());

    // (c) a file that ends exactly after a complete record is not truncated
    const auto exact = dir.file("exact.bmjr");
    write_file(exact, bytes.substr(0, static_cast<size_t>(last_record)));
    journal complete;
    ASSERT_TRUE(read_journal(exact, complete, error)) << error;
    EXPECT_EQ(0u, complete.bytes_dropped);
    EXPECT_TRUE(complete.warning.empty()) << complete.warning;
    EXPECT_EQ(22u, complete.records);
}

TEST(SessionJournal, UnknownTagIsSkippedAndCounted) {
    const temp_dir dir("unknown_tag");
    const auto path = dir.file("session.bmjr");
    const uint64_t last_record = write_synthetic(path);
    const std::string bytes = read_file(path);

    // a record of a tag this version does not know, spliced in before the
    // closing note
    std::string unknown;
    journal_detail::write_u8(unknown, 200);
    journal_detail::write_u32(unknown, 6);
    journal_detail::write_u32(unknown, 103);       // a payload shaped like a future record
    journal_detail::write_u16(unknown, 0);
    const auto spliced = dir.file("spliced.bmjr");
    write_file(spliced, bytes.substr(0, static_cast<size_t>(last_record)) + unknown
                            + bytes.substr(static_cast<size_t>(last_record)));

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(spliced, out, error)) << error;
    EXPECT_EQ(0u, out.bytes_dropped);
    EXPECT_EQ(1u, out.unknown_records);
    EXPECT_EQ(24u, out.records);
    EXPECT_EQ(4u, out.ticks.size());
    ASSERT_EQ(2u, out.notes.size());
    EXPECT_EQ("end: session over", out.notes[1].text);
    EXPECT_NE(std::string::npos, out.warning.find("unknown")) << out.warning;
}

TEST(SessionJournal, HeaderOnlyFileReadsWithNoTicks) {
    const temp_dir dir("header_only");
    const auto path = dir.file("failed.bmjr");
    {
        // the boot-failure case: the header is all the box ever gets
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        writer.close();
    }
    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    EXPECT_EQ(1u, out.records);
    EXPECT_EQ(0u, out.ticks.size());
    EXPECT_EQ(0u, out.initial_players.size());
    EXPECT_EQ(0u, out.notes.size());
    EXPECT_EQ(0u, out.bytes_dropped);
    EXPECT_TRUE(out.warning.empty()) << out.warning;
    EXPECT_EQ(make_header().build_id, out.header.build_id);
    EXPECT_EQ(std::filesystem::file_size(path), out.bytes_read);
}

TEST(SessionJournal, SizeCapStopsRecordingWithANote) {
    const temp_dir dir("size_cap");
    const auto path = dir.file("capped.bmjr");
    journal_writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(path, make_header(), 200, error)) << error;
    EXPECT_FALSE(writer.capped());
    int written = 0;
    for (int i = 0; i < 20 && !writer.capped(); ++i) {
        writer.note(static_cast<uint32_t>(100 + i), "note " + std::to_string(i));
        if (!writer.capped()) ++written;
    }
    EXPECT_TRUE(writer.capped());
    EXPECT_GT(written, 0);

    // everything after the cap is a no-op, the file does not grow
    const uint64_t bytes = writer.bytes();
    writer.note(200, "after the cap");
    writer.tick(200, make_hash(200), 1);
    writer.checkpoint(200, JOURNAL_CHECKPOINT_FULL, make_bodies());
    writer.input(200, 42, kFrameA, JOURNAL_INPUT_FRESH);
    EXPECT_EQ(bytes, writer.bytes());
    writer.close();
    EXPECT_EQ(bytes, std::filesystem::file_size(path));

    journal out;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    ASSERT_EQ(static_cast<size_t>(written) + 1, out.notes.size());
    for (int i = 0; i < written; ++i) EXPECT_EQ("note " + std::to_string(i), out.notes[static_cast<size_t>(i)].text);
    EXPECT_EQ("cap: journal_max_bytes reached, recording stopped", out.notes.back().text);
    // the cap note carries the tick of the record that no longer fit
    EXPECT_EQ(static_cast<uint32_t>(100 + written), out.notes.back().tick);
    EXPECT_EQ(0u, out.bytes_dropped);
    // one tick group per note, and nothing from after the cap
    EXPECT_EQ(static_cast<size_t>(written) + 1, out.ticks.size());
}

TEST(SessionJournal, BadMagicAndBadVersionAreRejected) {
    const temp_dir dir("bad_files");
    journal out;
    std::string error;

    const auto missing = dir.file("missing.bmjr");
    EXPECT_FALSE(read_journal(missing, out, error));
    EXPECT_FALSE(error.empty());

    const auto garbage = dir.file("garbage.bmjr");
    write_file(garbage, std::string("NOTAJOURNALFILE...."));
    error.clear();
    EXPECT_FALSE(read_journal(garbage, out, error));
    EXPECT_NE(std::string::npos, error.find("not a BMMO session journal")) << error;

    const auto version = dir.file("version.bmjr");
    std::string bytes(kJournalMagic, sizeof(kJournalMagic));
    journal_detail::write_u32(bytes, kJournalVersion + 1);
    write_file(version, bytes);
    error.clear();
    EXPECT_FALSE(read_journal(version, out, error));
    EXPECT_NE(std::string::npos, error.find("version")) << error;

    // magic and version but no header record
    const auto headerless = dir.file("headerless.bmjr");
    std::string only_magic(kJournalMagic, sizeof(kJournalMagic));
    journal_detail::write_u32(only_magic, kJournalVersion);
    write_file(headerless, only_magic);
    error.clear();
    EXPECT_FALSE(read_journal(headerless, out, error));
    EXPECT_NE(std::string::npos, error.find("header")) << error;
}

TEST(SessionJournal, WriterWithoutAFileIsSilent) {
    const temp_dir dir("no_file");
    journal_writer writer;
    std::string error;
    // a directory that does not exist: the writer stays closed and every call
    // is a no-op
    EXPECT_FALSE(writer.open(dir.file("nope") / "session.bmjr", make_header(), 0, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(writer.is_open());
    writer.note(1, "ignored");
    writer.player(1, 1, 0, true, "alice");
    writer.input(1, 1, kFrameA, 0);
    writer.event(journal_event{});
    writer.tick(1, make_hash(1), 0);
    writer.checkpoint(1, JOURNAL_CHECKPOINT_FULL, make_bodies());
    writer.correction(make_correction(1, 1, 0, "Ball"));
    writer.flush();
    writer.close();
    EXPECT_EQ(0u, writer.bytes());
    EXPECT_FALSE(writer.capped());
}

// A forged count must never index past the recipe's fixed arrays.
TEST(SessionJournal, RecipeCountsBeyondTheLimitsAreRejected) {
    const temp_dir dir("bad_recipe");
    const auto path = dir.file("session.bmjr");
    {
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        journal_event e;
        e.tick = 100;
        e.id = 42;
        e.recipe = make_recipe();
        writer.event(e);
        writer.tick(100, make_hash(100), 1);
        writer.close();
    }
    std::string bytes = read_file(path);
    // the convex count sits right after the collision surface str of the
    // recipe; find it by its value (3) written as an i32 next to "Stone"
    const size_t surface = bytes.find("Stone");
    ASSERT_NE(std::string::npos, surface);
    const size_t convex_count = surface + 5;
    ASSERT_EQ(3, static_cast<int>(journal_detail::load_u32(bytes.data() + convex_count)));
    std::string forged = bytes;
    std::string count;
    journal_detail::write_i32(count, BMMO_PHYSICS_MAX_CONVEX + 1);
    forged.replace(convex_count, 4, count);
    const auto path_forged = dir.file("forged.bmjr");
    write_file(path_forged, forged);

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path_forged, out, error)) << error;
    // the bad EVENT ends the read; the header survives, the rest is dropped
    EXPECT_EQ(1u, out.records);
    EXPECT_EQ(0u, out.ticks.size());
    EXPECT_GT(out.bytes_dropped, 0u);
    EXPECT_NE(std::string::npos, out.warning.find("does not parse")) << out.warning;
}

// A client renumbers its ticks at a resync, so a client journal can hold two
// fingerprints for the same tick.  The group keeps one - the last - and the
// dropped one must not disappear without a word.
TEST(SessionJournal, DuplicateTickRecordsKeepTheLastAndWarn) {
    const temp_dir dir("duplicate_ticks");
    const auto path = dir.file("resync.bmjr");
    {
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        writer.input(200, 42, kFrameA, JOURNAL_INPUT_FRESH);
        writer.tick(200, make_hash(200), 100);
        writer.tick(201, make_hash(201), 115);
        writer.note(201, "resync: reassigned, tick base 200");
        writer.input(200, 42, kFrameB, JOURNAL_INPUT_FRESH);
        writer.tick(200, make_hash(500), 200);   // the world that went on running
        writer.close();
    }

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    EXPECT_EQ(7u, out.records);
    ASSERT_EQ(2u, out.ticks.size());
    EXPECT_EQ(make_hash(500).hash, out.ticks[0].record.hash);
    EXPECT_EQ(200u, out.ticks[0].record.ms);
    EXPECT_EQ(2u, out.ticks[0].inputs.size());   // both inputs of tick 200 survive
    EXPECT_NE(std::string::npos, out.warning.find("duplicate TICK")) << out.warning;
    EXPECT_NE(std::string::npos, out.warning.find("tick 200")) << out.warning;
}

// Somebody quits while the room is still loading: the removal carries
// first_tick and comes before the first TICK record, but it is not a member.
TEST(SessionJournal, InitialMembersLeaveOutARemovalBeforeTheFirstTick) {
    const temp_dir dir("initial_members");
    const auto path = dir.file("session.bmjr");
    {
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        writer.player(100, 42, 1, true, "alice");
        writer.player(100, 43, 2, false, "ghost");
        writer.tick(100, make_hash(100), 15);
        writer.close();
    }

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    ASSERT_EQ(1u, out.initial_players.size());
    EXPECT_EQ(42u, out.initial_players[0].id);
    // the removal is not lost, it is just not a founding member
    ASSERT_EQ(1u, out.ticks.size());
    ASSERT_EQ(2u, out.ticks[0].players.size());
    EXPECT_FALSE(out.ticks[0].players[1].added);
    EXPECT_TRUE(out.warning.empty()) << out.warning;
}

// The warning names the bad record's position in the file so a hex dump lands
// on it: the records a reader skipped count towards that position too.
TEST(SessionJournal, ParseErrorNamesThePositionInTheFile) {
    const temp_dir dir("parse_error_index");
    const auto path = dir.file("good.bmjr");
    {
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        writer.note(100, "start: room 1 \"test room\"");
        writer.note(101, "mark: here");
        writer.close();
    }
    const std::string good = read_file(path);

    // a NOTE too short for its own tick field, and a record from a newer
    // version, which a reader skips and counts
    std::string bad;
    journal_detail::write_u8(bad, static_cast<uint8_t>(journal_tag::note));
    journal_detail::write_u32(bad, 2);
    bad.append(2, '\0');
    std::string unknown;
    journal_detail::write_u8(unknown, 200);
    journal_detail::write_u32(unknown, 4);
    journal_detail::write_u32(unknown, 101);

    const auto plain = dir.file("plain.bmjr");
    write_file(plain, good + bad);
    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(plain, out, error)) << error;
    EXPECT_NE(std::string::npos, out.warning.find("record 4 (tag 6) does not parse")) << out.warning;

    // the same bad record, now the fifth in the file
    const auto spliced = dir.file("spliced.bmjr");
    write_file(spliced, good + unknown + bad);
    journal skipped;
    ASSERT_TRUE(read_journal(spliced, skipped, error)) << error;
    EXPECT_EQ(1u, skipped.unknown_records);
    EXPECT_NE(std::string::npos, skipped.warning.find("record 5 (tag 6) does not parse")) << skipped.warning;
}

// scan_journal is what the SimTool's --list and the replay use on big files.
TEST(SessionJournal, ScanReportsTagsInFileOrder) {
    const temp_dir dir("scan");
    const auto path = dir.file("session.bmjr");
    write_synthetic(path);

    journal_header header;
    std::vector<journal_tag> tags;
    std::string error;
    uint64_t dropped = 0, unknown = 0;
    ASSERT_TRUE(scan_journal(path, header, [&](journal_tag tag, const std::string&) {
        tags.push_back(tag);
        return true;
    }, error, &dropped, &unknown));
    EXPECT_EQ(0u, dropped);
    EXPECT_EQ(0u, unknown);
    EXPECT_EQ(22u, tags.size());            // every record but the header
    EXPECT_EQ(journal_tag::note, tags.front());
    EXPECT_EQ(journal_tag::note, tags.back());
    EXPECT_EQ(7u, header.session);

    // stopping the scan early reports the rest of the file as dropped
    tags.clear();
    ASSERT_TRUE(scan_journal(path, header, [&](journal_tag tag, const std::string&) {
        tags.push_back(tag);
        return tags.size() < 3;
    }, error, &dropped, &unknown));
    EXPECT_EQ(3u, tags.size());
    EXPECT_GT(dropped, 0u);
}

// An event carries two ticks: the one the world consumed it at (what the
// reader groups by, and what a replay has to feed it back at) and the one it
// was stamped for (what the spawn impulse direction comes from).  They differ
// whenever an event reached the server after the tick it asked for.
TEST(SessionJournal, EventKeepsTheAppliedAndTheStampedTickApart) {
    const temp_dir dir("event_tick");
    const auto path = dir.file("session.bmjr");
    {
        journal_writer writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, make_header(), 0, error)) << error;
        journal_event late;
        late.tick = 120;         // the tick the world consumed it at
        late.event_tick = 113;   // the tick the client stamped it for
        late.id = 42;
        late.type = event_type::Physicalize;
        late.ball_type = 1;
        late.flags = PHYSICALIZE_FLAG_SPAWN;
        late.recipe = make_recipe();
        late.name = "Ball_Paper_42";
        writer.event(late);
        journal_event punctual;   // stamped for the tick it was applied at
        punctual.tick = 121;
        punctual.event_tick = 121;
        punctual.id = 43;
        punctual.type = event_type::BodyRevived;
        punctual.name = "Trafo_Piece_3";
        writer.event(punctual);
        // A client at its anchor frame stamps tick 0, and the server can dequeue
        // that event much later: 0 is a stamp like any other, never "absent".
        journal_event anchored;
        anchored.tick = 122;
        anchored.event_tick = 0;
        anchored.id = 44;
        anchored.type = event_type::Physicalize;
        anchored.ball_type = 2;
        anchored.name = "Ball_Stone_44";
        writer.event(anchored);
        writer.tick(122, make_hash(122), 8);
        writer.close();
    }

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(path, out, error)) << error;
    EXPECT_TRUE(out.warning.empty()) << out.warning;
    ASSERT_EQ(3u, out.ticks.size());
    // grouped under the tick it was applied at, not the one it was stamped for
    EXPECT_EQ(120u, out.ticks[0].tick);
    ASSERT_EQ(1u, out.ticks[0].events.size());
    EXPECT_EQ(120u, out.ticks[0].events[0].tick);
    EXPECT_EQ(113u, out.ticks[0].events[0].event_tick);
    expect_same_recipe(make_recipe(), out.ticks[0].events[0].recipe);
    ASSERT_EQ(1u, out.ticks[1].events.size());
    EXPECT_EQ(121u, out.ticks[1].events[0].tick);
    EXPECT_EQ(121u, out.ticks[1].events[0].event_tick);
    // the anchor stamp survives as itself: a 0 written verbatim, not swallowed
    ASSERT_EQ(1u, out.ticks[2].events.size());
    EXPECT_EQ(122u, out.ticks[2].events[0].tick);
    EXPECT_EQ(0u, out.ticks[2].events[0].event_tick);
}

// A file from a writer that predates the stamped tick: the EVENT payload ends
// after the recipe, and the record must still parse - with the stamp equal to
// the applied tick, and with every record after it intact.
TEST(SessionJournal, EventWithoutTheStampedTickReadsItAsTheAppliedTick) {
    const temp_dir dir("event_tick_old");
    const auto path = dir.file("session.bmjr");
    write_synthetic(path);
    const std::string bytes = read_file(path);

    std::string trimmed = bytes.substr(0, 12);   // magic + version
    size_t at = 12;
    size_t stripped = 0;
    while (at + 5 <= bytes.size()) {
        const uint8_t tag = static_cast<uint8_t>(bytes[at]);
        const uint32_t size = journal_detail::load_u32(bytes.data() + at + 1);
        ASSERT_LE(at + 5 + size, bytes.size());
        std::string payload = bytes.substr(at + 5, size);
        if (tag == static_cast<uint8_t>(journal_tag::event)) {
            ASSERT_GE(payload.size(), 4u);
            payload.resize(payload.size() - 4);
            ++stripped;
        }
        trimmed.push_back(static_cast<char>(tag));
        journal_detail::write_u32(trimmed, static_cast<uint32_t>(payload.size()));
        trimmed += payload;
        at += 5 + size;
    }
    ASSERT_EQ(2u, stripped);
    const auto old_style = dir.file("old.bmjr");
    write_file(old_style, trimmed);

    journal out;
    std::string error;
    ASSERT_TRUE(read_journal(old_style, out, error)) << error;
    EXPECT_TRUE(out.warning.empty()) << out.warning;
    EXPECT_EQ(23u, out.records);       // nothing was lost, only shortened
    ASSERT_EQ(4u, out.ticks.size());
    ASSERT_EQ(1u, out.ticks[1].events.size());
    EXPECT_EQ(101u, out.ticks[1].events[0].tick);
    EXPECT_EQ(101u, out.ticks[1].events[0].event_tick);
    expect_same_recipe(make_recipe(), out.ticks[1].events[0].recipe);
    ASSERT_EQ(1u, out.ticks[2].events.size());
    EXPECT_EQ(102u, out.ticks[2].events[0].event_tick);
    EXPECT_EQ("Trafo_Piece_3", out.ticks[2].events[0].name);
    ASSERT_EQ(2u, out.notes.size());   // the records after the shortened ones
    EXPECT_EQ("end: session over", out.notes[1].text);
}
