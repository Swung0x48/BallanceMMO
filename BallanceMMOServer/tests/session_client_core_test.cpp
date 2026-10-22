// Unit tests for the shared session-client core (session/client_core.hpp,
// docs/session-client-core-design.md 5.1) over a fake platform that records
// every call.  Step E1 builds the skeleton only - the state moved out of the
// headless client, the platform with send / now / log, the capabilities and
// the empty legacy_calls - so these pin what E1 has: the moved state's
// initializers, the platform check at construction, and that the core hands
// the host's operations back unchanged.  The flow tests of design 5.1 (frame
// order, numbering, the hold, events, echoes, beam, sectors, status text,
// SessionLead) arrive with the steps that move that flow (E3-E10).  Step E2
// adds the journal's (core::client_journal over a temporary file).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <session/client_core.hpp>

namespace {
    using bmmo::session::clock_site;
    using bmmo::session::client_core_state;
    using bmmo::session::log_level;
    using bmmo::session::send_mode;
    using bmmo::session::session_capabilities;
    using bmmo::session::session_client_core;
    using bmmo::session::session_platform;

    // Records every platform call in order, as "op:detail".
    struct fake_platform {
        std::vector<std::string> calls;
        std::chrono::steady_clock::time_point clock{std::chrono::milliseconds(1000)};

        session_platform make() {
            session_platform p;
            p.send = [this](const std::string& bytes, send_mode mode) {
                calls.push_back("send:" + bytes + ":" + std::to_string(static_cast<int>(mode)));
            };
            p.now = [this](clock_site site) {
                calls.push_back(std::string("now:") + bmmo::session::clock_site_name(site));
                return clock;
            };
            p.log = [this](log_level level, const std::string& text) {
                calls.push_back("log:" + std::to_string(static_cast<int>(level)) + ":" + text);
            };
            return p;
        }
    };

    TEST(SessionClientCore, ConstructsWithTheThreeOperations) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        EXPECT_TRUE(fake.calls.empty()) << "construction calls nothing on the platform";
    }

    TEST(SessionClientCore, RefusesAPlatformWithoutAnOperation) {
        for (int missing = 0; missing < 3; ++missing) {
            fake_platform fake;
            session_platform p = fake.make();
            if (missing == 0) p.send = nullptr;
            if (missing == 1) p.now = nullptr;
            if (missing == 2) p.log = nullptr;
            EXPECT_THROW(session_client_core(p, session_capabilities{}), std::invalid_argument) << missing;
        }
        EXPECT_STREQ(bmmo::session::missing_platform_operation(session_platform{}, session_capabilities{}), "send");
    }

    TEST(SessionClientCore, HandsTheHostOperationsBack) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        core.platform().send("abc", send_mode::unreliable_no_delay);
        const auto t = core.platform().now(clock_site::resync_limiter);
        core.platform().log(log_level::warn, "a line");
        EXPECT_EQ(t, fake.clock);
        const std::vector<std::string> expected = {"send:abc:2", "now:resync_limiter", "log:1:a line"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(SessionClientCore, CapabilitiesDefaultToCannot) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        const auto& caps = core.capabilities();
        EXPECT_FALSE(caps.own_body_control);
        EXPECT_FALSE(caps.slew);
        EXPECT_FALSE(caps.nonblocking_hold);
        EXPECT_FALSE(caps.whole_frame);
        EXPECT_EQ(caps.own_lifecycle, bmmo::session::own_lifecycle_source::event_log);
        session_capabilities declared;
        declared.slew = true;
        declared.own_lifecycle = bmmo::session::own_lifecycle_source::host_hooks;
        session_client_core other(fake.make(), declared);
        EXPECT_TRUE(other.capabilities().slew);
        EXPECT_EQ(other.capabilities().own_lifecycle, bmmo::session::own_lifecycle_source::host_hooks);
    }

    // E1 moves no function, so no back-call is needed yet (design 6.1).
    TEST(SessionClientCore, LegacyCallsStartEmpty) {
        static_assert(std::is_empty_v<bmmo::session::legacy_calls>, "E1 adds no legacy call");
        SUCCEED();
    }

    // The state moved with its initializers unchanged (design 6.1, E1): the
    // headless client's values at construction.
    TEST(SessionClientCore, StateKeepsTheHeadlessInitializers) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        const client_core_state& s = core.s;
        EXPECT_EQ(s.phase_, client_core_state::phase::idle);
        EXPECT_EQ(s.own_id_, 0u);
        EXPECT_EQ(s.session_, 0u);
        EXPECT_EQ(s.input_delay_, 6u);
        EXPECT_EQ(s.seed_, 1);
        EXPECT_EQ(s.level_, 1);
        EXPECT_EQ(s.own_join_order_, -1);
        EXPECT_EQ(s.sector_lead_, 0u);
        EXPECT_EQ(s.sector_cell_, 0);
        EXPECT_FALSE(s.start_sector_pending_);
        EXPECT_FALSE(s.assigned_);
        EXPECT_FALSE(s.held_);
        EXPECT_EQ(s.hold_polls_, 0);
        EXPECT_EQ(s.tick_base_, 0u);
        EXPECT_EQ(s.frames_since_anchor_, -1);
        EXPECT_FALSE(s.previous_cam_valid_);
        for (const auto& row: s.previous_cam_)
            for (float v: row) EXPECT_EQ(v, 0.0f);
        EXPECT_EQ(s.window_index_, 0xFFFFFFFFu);
        EXPECT_EQ(client_core_state::kWindowTicks, 60u);
        EXPECT_EQ(client_core_state::kRelayLagBuckets, 129u);
        EXPECT_EQ(s.relay_lag_hist_.size(), 129u);
        for (uint32_t n: s.relay_lag_hist_) EXPECT_EQ(n, 0u);
        EXPECT_EQ(s.held_floor_, 0u);
        EXPECT_FALSE(s.resync_pending_);
        EXPECT_FALSE(s.resync_in_flight_);
        EXPECT_FALSE(s.resync_owed_);
        EXPECT_EQ(s.consecutive_hard_, 0);
        EXPECT_EQ(s.last_resync_request_, std::chrono::steady_clock::time_point{});
        EXPECT_EQ(s.lead_offset_, 0);
        EXPECT_FALSE(s.have_snapshot_);
        EXPECT_FALSE(s.own_physicalized_);
        EXPECT_TRUE(s.remotes_.empty());
        EXPECT_TRUE(s.own_life_.empty());
        EXPECT_TRUE(s.pending_events_.empty());
        EXPECT_FALSE(core.journal().journalling());
        EXPECT_EQ(core.config.journal_checkpoint_ticks, 0u);
        EXPECT_EQ(s.rollback_.stats().rollbacks, 0u);
    }

    // The moved nested types keep their defaults.
    TEST(SessionClientCore, MovedTypesKeepTheirDefaults) {
        client_core_state::own_life life;
        EXPECT_EQ(life.what, client_core_state::own_life::kind::birth);
        EXPECT_EQ(life.state, client_core_state::own_life::verdict::sent);
        EXPECT_EQ(life.why, client_core_state::own_life::aside::none);
        EXPECT_TRUE(life.made);
        EXPECT_FALSE(life.moved());
        life.applied = 5;
        EXPECT_TRUE(life.moved());

        client_core_state::remote_ball remote;
        EXPECT_FALSE(remote.physicalized);
        EXPECT_EQ(remote.born_tick, 0u);
        EXPECT_EQ(remote.driven_from, 0u);

        bmmo::session_event_msg event;
        event.player = 7;
        event.tick = 120;
        event.origin_tick = 118;
        event.ball_type = 2;
        event.position[1] = 3.5f;
        const auto birth = client_core_state::pending_birth::of(event);
        EXPECT_EQ(birth.player, 7u);
        EXPECT_EQ(birth.tick, 120u);
        EXPECT_EQ(birth.origin, 118u);
        EXPECT_EQ(birth.ball_type, 2u);
        EXPECT_EQ(birth.position[1], 3.5f);
    }

    // ------------------------------------------------------------ the journal
    // Step E2: the headless client's journal helpers as core::client_journal,
    // over the platform's journal_path / utc_ms / now / body_state /
    // list_bodies.

    bmmo_physics_body_state make_body(const char* name, bool movable, double x) {
        bmmo_physics_body_state b{};
        std::snprintf(b.name, sizeof(b.name), "%s", name);
        b.movable = movable;
        b.simulated = true;
        b.collision_enabled = true;
        b.position[0] = x;
        b.rotation[3] = 1.0;
        return b;
    }

    struct journal_fake : fake_platform {
        std::filesystem::path path;   // what journal_path answers (empty: no journal)
        std::vector<bmmo_physics_body_state> bodies;

        session_platform make_journal() {
            session_platform p = make();
            p.utc_ms = [this](clock_site site) {
                calls.push_back(std::string("utc:") + bmmo::session::clock_site_name(site));
                return uint64_t{1234};
            };
            p.body_state = [this](const std::string& entity, bmmo_physics_body_state& out) {
                calls.push_back("body_state:" + entity);
                for (const auto& b: bodies)
                    if (entity == b.name) { out = b; return true; }
                return false;
            };
            p.list_bodies = [this]() {
                calls.push_back("list_bodies");
                return bodies;
            };
            p.journal_path = [this](const bmmo::session::journal_header& h) {
                calls.push_back("journal_path:" + std::to_string(h.session) + ":" + std::to_string(h.level) + ":"
                                + std::to_string(h.own_player));
                return path;
            };
            return p;
        }
    };

    struct journal_dir {
        std::filesystem::path path;
        explicit journal_dir(const std::string& name)
            // A per-run suffix: two test processes on one host must not
            // remove each other's journal.
            : path(std::filesystem::temp_directory_path()
                   / ("bmmo_core_journal_" + name + "_" + std::to_string(std::random_device{}()) + "_"
                      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        ~journal_dir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        journal_dir(const journal_dir&) = delete;
        journal_dir& operator=(const journal_dir&) = delete;
    };

    void start_session(session_client_core& core) {
        core.s.session_ = 9;
        core.s.level_ = 13;
        core.s.seed_ = 5;
        core.s.own_id_ = 3;
        core.s.own_join_order_ = 1;
        bmmo::session::player_entry own, other;
        own.id = 3; own.join_order = 1;
        other.id = 4; other.join_order = 0;
        core.s.players_ = {own, other};
    }

    // A host that journals must be able to stamp the header and read bodies.
    TEST(SessionClientCoreJournal, RefusesAJournalWithoutItsOperations) {
        for (int missing = 0; missing < 3; ++missing) {
            journal_fake fake;
            session_platform p = fake.make_journal();
            if (missing == 0) p.utc_ms = nullptr;
            if (missing == 1) p.body_state = nullptr;
            if (missing == 2) p.list_bodies = nullptr;
            EXPECT_THROW(session_client_core(p, session_capabilities{}), std::invalid_argument) << missing;
        }
        // Without journal_path the host never journals, and needs none of them.
        fake_platform plain;
        EXPECT_NO_THROW(session_client_core(plain.make(), session_capabilities{}));
    }

    // No path, no journal - and no clock read, as the headless client without
    // --journal (a read would be one more record in an inbound capture).
    TEST(SessionClientCoreJournal, NoPathNoJournal) {
        journal_fake fake;
        session_client_core core(fake.make_journal(), session_capabilities{});
        start_session(core);
        core.journal().open(bmmo::physics::world_hash{}, "build-x", "Me");
        const std::vector<std::string> expected = {"journal_path:9:13:3"};
        EXPECT_EQ(fake.calls, expected);
        EXPECT_FALSE(core.journal().journalling());
        core.journal().note("nothing");
        core.journal().close("nothing");
        EXPECT_EQ(fake.calls, expected);

        fake_platform plain;
        session_client_core bare(plain.make(), session_capabilities{});
        bare.journal().open(bmmo::physics::world_hash{}, "build-x", "Me");
        EXPECT_TRUE(plain.calls.empty());
        EXPECT_FALSE(bare.journal().journalling());
    }

    TEST(SessionClientCoreJournal, WritesTheHeadlessRecords) {
        journal_dir dir("records");
        journal_fake fake;
        fake.path = dir.path / "sub" / "client.bmjr";
        session_client_core core(fake.make_journal(), session_capabilities{});
        start_session(core);
        core.config.journal_checkpoint_ticks = 2;
        bmmo::physics::world_hash hash;
        hash.pose = 0x11;
        hash.surfaces = 0x22;
        core.journal().open(hash, "build-x", "Me");
        {
            // The path first, then the header's utc_ms and the ms origin, in
            // the headless client's order; the directory is made.
            const std::vector<std::string> expected = {"journal_path:9:13:3", "utc:journal_utc", "now:journal_start",
                                                       "log:0:journal: " + fake.path.string()};
            EXPECT_EQ(fake.calls, expected);
            EXPECT_TRUE(std::filesystem::exists(fake.path));
            EXPECT_TRUE(core.journal().journalling());
        }

        // Tick base 100, three frames since the anchor: the current tick is 102.
        core.s.tick_base_ = 100;
        core.s.frames_since_anchor_ = 3;
        EXPECT_EQ(core.current_tick(), 102u);
        core.journal().note("hello");

        auto& remote = core.s.remotes_[4];
        remote.entity = "Ball_Wood_BMMO_4";
        remote.physicalized = true;
        fake.bodies = {make_body("Ball_Paper", true, 1.0), make_body("Ball_Wood_BMMO_4", true, 2.0),
                       make_body("Ball_Stone_BMMO_9", true, 3.0), make_body("Mech_A", true, 4.0),
                       make_body("Floor", false, 5.0)};
        EXPECT_TRUE(core.journal().checkpoint_due(102));
        core.journal().local_checkpoint(102, "Ball_Paper");

        bmmo::session_event_msg event;
        event.tick = 102;
        event.origin_tick = 101;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = 1;
        event.recipe.collision_surface = "surface";
        event.recipe.convex_meshes = {"a", "b"};
        core.journal().lifecycle_event(7, event);   // never announced: a late join

        const double server[3] = {9.0, 8.0, 7.0};
        core.journal().body_correction(2, "Ball_Paper", 102, 0.5, server);
        core.journal().leave_player(4);
        core.journal().close("bye");
        EXPECT_FALSE(core.journal().journalling());

        bmmo::session::journal j;
        std::string error;
        ASSERT_TRUE(bmmo::session::read_journal(fake.path, j, error)) << error;
        EXPECT_EQ(j.header.kind, bmmo::session::journal_kind::client);
        EXPECT_EQ(j.header.session, 9u);
        EXPECT_EQ(j.header.level, 13);
        EXPECT_EQ(j.header.seed, 5);
        EXPECT_EQ(j.header.checkpoint_ticks, 2u);
        EXPECT_EQ(j.header.first_tick, 0u);
        EXPECT_EQ(j.header.anchor_hash, 0x11u);
        EXPECT_EQ(j.header.anchor_surfaces, 0x22u);
        EXPECT_EQ(j.header.build_id, "build-x");
        EXPECT_EQ(j.header.utc_ms, 1234u);
        EXPECT_EQ(j.header.own_player, 3u);
        EXPECT_EQ(j.header.own_join_order, 1u);
        ASSERT_EQ(j.initial_players.size(), 2u);
        EXPECT_EQ(j.initial_players[0].name, "Me");
        EXPECT_EQ(j.initial_players[1].name, "4");

        std::vector<std::string> notes;
        for (const auto& n: j.notes) notes.push_back(std::to_string(n.tick) + " " + n.text);
        const std::vector<std::string> expected_notes = {
            "0 start: session 9 level 13 as Me(3, join 1): Me(3, join 1) 4(4, join 0)",
            "102 hello",
            "102 late join: p7 has records but was never announced to this client; join order unknown",
            "102 end: bye"};
        EXPECT_EQ(notes, expected_notes);

        const bmmo::session::journal_tick* t102 = nullptr;
        for (const auto& t: j.ticks) if (t.tick == 102) t102 = &t;
        ASSERT_NE(t102, nullptr);
        ASSERT_EQ(t102->checkpoints.size(), 1u);
        const auto& cp = t102->checkpoints[0];
        EXPECT_EQ(cp.flags, bmmo::session::JOURNAL_CHECKPOINT_LOCAL);
        ASSERT_EQ(cp.bodies.size(), 4u);   // the immovable floor is left out
        EXPECT_EQ(cp.bodies[0].owner, 3u);
        EXPECT_EQ(cp.bodies[1].owner, 4u);
        EXPECT_EQ(cp.bodies[2].owner, 0u);   // a ball no mirror owns
        EXPECT_EQ(cp.bodies[3].kind, bmmo::session::body_kind::Mechanism);
        EXPECT_EQ(cp.bodies[3].position[0], 4.0);
        ASSERT_EQ(t102->events.size(), 1u);
        EXPECT_EQ(t102->events[0].id, 7u);
        EXPECT_EQ(t102->events[0].origin_tick, 101u);
        EXPECT_STREQ(t102->events[0].recipe.collision_surface, "surface");
        EXPECT_EQ(t102->events[0].recipe.convex_count, 2);
        ASSERT_EQ(t102->corrections.size(), 1u);
        EXPECT_EQ(t102->corrections[0].kind, 2u);
        EXPECT_EQ(t102->corrections[0].local_tick, 102u);
        EXPECT_EQ(t102->corrections[0].local_position[0], 1.0);
        EXPECT_EQ(t102->corrections[0].server_position[0], 9.0);
        bool left = false;
        for (const auto& p: t102->players) if (p.id == 4 && !p.added) left = true;
        EXPECT_TRUE(left);
    }

    // The checkpoint rule: `tick % cadence`, before the first one, after the
    // tick went back, and a pending one (a hard correction, a resync, a
    // rollback) no sooner than min(cadence, 33) ticks after the last.
    TEST(SessionClientCoreJournal, CheckpointDue) {
        journal_dir dir("due");
        journal_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_journal(), session_capabilities{});
        start_session(core);
        core.config.journal_checkpoint_ticks = 100;
        core.journal().open(bmmo::physics::world_hash{}, "build-x", "Me");
        EXPECT_TRUE(core.journal().checkpoint_due(7)) << "before the first";
        core.journal().local_checkpoint(100, "");
        EXPECT_FALSE(core.journal().checkpoint_due(120));
        EXPECT_TRUE(core.journal().checkpoint_due(200));
        EXPECT_TRUE(core.journal().checkpoint_due(99)) << "the tick went back";
        core.journal().body_correction(3, "", 110, 0.1, nullptr);   // a blend: nothing pending
        EXPECT_FALSE(core.journal().checkpoint_due(140));
        core.journal().body_correction(2, "", 110, 0.1, nullptr);   // a hard set: pending
        EXPECT_FALSE(core.journal().checkpoint_due(132));
        EXPECT_TRUE(core.journal().checkpoint_due(133));
        bmmo::session::rollback_correction rolled;
        rolled.kind = 1;
        core.journal().local_checkpoint(150, "");
        EXPECT_FALSE(core.journal().checkpoint_due(183));
        core.journal().rollback_correction(rolled);
        EXPECT_TRUE(core.journal().checkpoint_due(183));

        // 0 is the default cadence, kJournalCheckpointTicks.
        core.config.journal_checkpoint_ticks = 0;
        EXPECT_TRUE(core.journal().checkpoint_due(bmmo::session::core::kJournalCheckpointTicks * 3));
        core.journal().close("done");
    }
}
