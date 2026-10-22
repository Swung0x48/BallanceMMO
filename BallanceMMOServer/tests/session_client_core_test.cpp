// Unit tests for the shared session-client core (session/client_core.hpp,
// docs/session-client-core-design.md 5.1) over a fake platform that records
// every call.  Step E1 builds the skeleton only - the state moved out of the
// headless client, the platform with send / now / log, the capabilities and
// the empty legacy_calls - so these pin what E1 has: the moved state's
// initializers, the platform check at construction, and that the core hands
// the host's operations back unchanged (step E3 gives legacy_calls seven
// entries, all empty in a fresh core).  The flow tests of design 5.1 (frame
// order, numbering, the hold, events, echoes, beam, sectors, status text,
// SessionLead) arrive with the steps that move that flow (E3-E10).  Step E2
// adds the journal's (core::client_journal over a temporary file); step E3
// numbering and the hold (design 5.1), begin, SessionReady, the resync
// limiter, the event sends and end; step E4 the snapshot path (the count,
// the stale rule, the resync rebuild, the rollback path's guard order, the
// legacy ladder).

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
// After client_core.hpp: the message headers are not self-contained.
#include <message/session_ready_msg.hpp>
#include <message/session_resync_msg.hpp>

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
        std::string driven;              // what own_driven_entity answers
        bool write_ok = true;            // what write_body answers (false: "no body")

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
            // The snapshot path's (E4).
            p.body_state = [this](const std::string& entity, bmmo_physics_body_state&) {
                calls.push_back("body_state:" + entity);
                return false;
            };
            p.write_body = [this](const std::string& entity, const double position[3], const double*, const float*,
                                  const float*, bool wake, std::string& error) {
                calls.push_back("write_body:" + entity + ":" + std::to_string(position[0]) + (wake ? ":wake" : ":freeze"));
                error.clear();
                if (!write_ok) error = "no body";
                return write_ok;
            };
            p.own_driven_entity = [this]() {
                calls.push_back("own_driven_entity");
                return driven;
            };
            p.bind_world = [this]() {
                calls.push_back("bind_world");
                return bmmo::session::rollback_world{};
            };
            p.keep_live_cells = [this](const session_client_core& core) {
                calls.push_back("keep_live_cells:enter:" + std::to_string(core.current_tick()));
                const uint64_t rollbacks_before = core.rollback_stats().rollbacks;
                return bmmo::session::scope_guard([this, &core, rollbacks_before]() {
                    calls.push_back(core.rollback_stats().rollbacks != rollbacks_before ? "keep_live_cells:exit:rolled back"
                                                                                      : "keep_live_cells:exit");
                });
            };
            return p;
        }
    };

    TEST(SessionClientCore, ConstructsWithTheRequiredOperations) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        EXPECT_TRUE(fake.calls.empty()) << "construction calls nothing on the platform";
    }

    TEST(SessionClientCore, RefusesAPlatformWithoutAnOperation) {
        for (int missing = 0; missing < 8; ++missing) {
            fake_platform fake;
            session_platform p = fake.make();
            if (missing == 0) p.send = nullptr;
            if (missing == 1) p.now = nullptr;
            if (missing == 2) p.log = nullptr;
            if (missing == 3) p.body_state = nullptr;
            if (missing == 4) p.write_body = nullptr;
            if (missing == 5) p.own_driven_entity = nullptr;
            if (missing == 6) p.bind_world = nullptr;
            if (missing == 7) p.keep_live_cells = nullptr;
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

    // The back-calls (design 6.1) are the host's to fill: a core starts with
    // none.  E3 adds the seven its moved code calls, E4 four more.
    TEST(SessionClientCore, LegacyCallsStartEmpty) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        const auto& l = core.legacy;
        EXPECT_FALSE(l.rebase_own_lives);
        EXPECT_FALSE(l.set_aside_unanswered);
        EXPECT_FALSE(l.reset_held_epoch);
        EXPECT_FALSE(l.drop_pending_rewind);
        EXPECT_FALSE(l.finish_lead_slew);
        EXPECT_FALSE(l.end_session_world);
        EXPECT_FALSE(l.report);
        EXPECT_FALSE(l.recorded_input_at);
        EXPECT_FALSE(l.own_ball);
        EXPECT_FALSE(l.describe_movable_objects);
        EXPECT_FALSE(l.drop_carried_own_replay);
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
        EXPECT_TRUE(core.config.rollback);
        EXPECT_TRUE(core.config.correct);
        EXPECT_FALSE(core.config.trace);
        EXPECT_EQ(s.rollback_diag_, 0);
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
        // Without journal_path the host never journals, and needs neither
        // utc_ms nor list_bodies (body_state every host needs: E4).
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


    // ------------------------------------------------- numbering and the hold
    // Step E3: begin, anchor's SessionReady half, poll_hold, SessionAssign,
    // the resync request and its limiter, the event sends and end, over a
    // fake platform whose clock the test moves, with the host hooks and the
    // legacy back-calls recorded in the same call list.

    struct flow_fake : journal_fake {
        std::vector<std::pair<std::string, send_mode>> sent;   // every send, in order, with its bytes

        session_platform make_flow(bool journalling = false) {
            session_platform p = journalling ? make_journal() : make();
            p.send = [this](const std::string& bytes, send_mode mode) {
                calls.push_back("send:" + std::to_string(static_cast<int>(mode)));
                sent.emplace_back(bytes, mode);
            };
            p.hooks.numbering_restarted = [this](clock_site site) {
                calls.push_back(std::string("hook:numbering_restarted:") + bmmo::session::clock_site_name(site));
            };
            p.hooks.ending = [this]() { calls.push_back("hook:ending"); };
            return p;
        }

        void fill_legacy(session_client_core& core) {
            auto& l = core.legacy;
            l.rebase_own_lives = [this]() { calls.push_back("legacy:rebase_own_lives"); };
            l.set_aside_unanswered = [this](client_core_state::own_life::aside why) {
                calls.push_back("legacy:set_aside_unanswered:" + std::to_string(static_cast<int>(why)));
            };
            l.reset_held_epoch = [this](const char* why) { calls.push_back(std::string("legacy:reset_held_epoch:") + why); };
            l.drop_pending_rewind = [this]() { calls.push_back("legacy:drop_pending_rewind"); };
            l.finish_lead_slew = [this](const std::string& why, bool reanchored) {
                calls.push_back("legacy:finish_lead_slew:" + why + (reanchored ? ":reanchored" : ""));
            };
            l.end_session_world = [this]() { calls.push_back("legacy:end_session_world"); };
            l.report = [this]() { calls.push_back("legacy:report"); };
            l.recorded_input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame&) {
                calls.push_back("legacy:recorded_input_at:" + entity + ":" + std::to_string(tick));
                return false;
            };
            l.own_ball = [this]() {
                calls.push_back("legacy:own_ball");
                return own_ball_name;
            };
            l.describe_movable_objects = [this]() {
                calls.push_back("legacy:describe_movable_objects");
                return std::string("-");
            };
            l.drop_carried_own_replay = [this](uint32_t anchor, bool server_rows) {
                calls.push_back("legacy:drop_carried_own_replay:" + std::to_string(anchor) + (server_rows ? ":rows" : ""));
            };
        }
        std::string own_ball_name = "Ball_Wood";

        void advance(std::chrono::milliseconds ms) { clock += ms; }
    };

    bmmo::session_start_msg start_message(uint32_t session, uint32_t own_id) {
        bmmo::session_start_msg msg;
        msg.session = session;
        msg.input_delay = 4;
        msg.sector_lead = 7;
        msg.seed = 11;
        msg.spawn_impulse = 3.0f;
        msg.spawn_spacing = 4.2f;
        msg.map.level = 13;
        bmmo::session::player_entry own, other;
        own.id = own_id; own.join_order = 2;
        other.id = own_id + 1; other.join_order = 0;
        msg.players = {other, own};
        return msg;
    }

    // begin, level_requested and anchor with a session of id 9 for own id 3.
    void run_to_anchor(session_client_core& core, flow_fake& fake) {
        core.s.own_id_ = 3;
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.hash.pose = 0x51;
        report.hash.surfaces = 0x52;
        report.physics_sha256 = "headless-64";
        report.build_id = "build-x";
        report.own_name = "Me";
        report.load_ticks = 12;
        core.anchor(report);
    }

    bmmo::session_assign_msg assign_message(uint32_t session, uint32_t first_tick) {
        bmmo::session_assign_msg msg;
        msg.session = session;
        msg.first_tick = first_tick;
        return msg;
    }

    template <class Msg>
    Msg decode(const std::string& bytes) {
        Msg msg;
        msg.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        EXPECT_TRUE(msg.deserialize());
        return msg;
    }

    std::vector<std::string> since(const std::vector<std::string>& calls, size_t from) {
        return std::vector<std::string>(calls.begin() + static_cast<std::ptrdiff_t>(from), calls.end());
    }

    TEST(SessionClientCoreFlow, BeginRefusesAnotherProtocol) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        auto msg = start_message(9, 3);
        msg.protocol = bmmo::session::PHYSICS_PROTOCOL_VERSION + 1;
        EXPECT_TRUE(core.begin(msg, bmmo::session::core_config{}).refused);
        ASSERT_EQ(fake.calls.size(), 1u);
        EXPECT_EQ(fake.calls[0].rfind("log:0:session 9 refused: the server runs physics protocol", 0), 0u) << fake.calls[0];
        EXPECT_EQ(core.s.session_, 0u);
        EXPECT_EQ(core.s.phase_, client_core_state::phase::idle);
    }

    TEST(SessionClientCoreFlow, BeginTakesTheSessionAndConfig) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        core.s.own_id_ = 3;
        core.s.assigned_ = true;
        core.s.tick_base_ = 77;
        core.s.held_ = true;
        core.s.hold_polls_ = 5;
        core.s.remotes_[4].entity = "x";
        bmmo::session::core_config config;
        config.journal_checkpoint_ticks = 2;
        config.level = 5;
        auto msg = start_message(9, 3);
        EXPECT_FALSE(core.begin(msg, config).refused);
        EXPECT_EQ(core.config.journal_checkpoint_ticks, 2u);
        EXPECT_EQ(core.s.session_, 9u);
        EXPECT_EQ(core.s.level_, 13) << "the session's level, not the fallback";
        EXPECT_EQ(core.s.input_delay_, 4u);
        EXPECT_EQ(core.s.sector_lead_, 7u);
        EXPECT_EQ(core.s.seed_, 11);
        EXPECT_EQ(core.s.own_join_order_, 2);
        EXPECT_EQ(core.s.own_group_, "P#2");
        EXPECT_FALSE(core.s.assigned_);
        EXPECT_FALSE(core.s.held_);
        EXPECT_EQ(core.s.hold_polls_, 0);
        EXPECT_EQ(core.s.tick_base_, 0u);
        EXPECT_EQ(core.s.frames_since_anchor_, -1);
        EXPECT_TRUE(core.s.remotes_.empty());
        EXPECT_EQ(core.s.phase_, client_core_state::phase::idle) << "loading only once the host requested the level";
        const std::vector<std::string> expected = {
            "log:0:session 9 starting: level 13, 2 players, join order 2, seed 11, input delay 4, sector lead 7",
            "legacy:reset_held_epoch:a new session"};
        EXPECT_EQ(fake.calls, expected);
        core.level_requested();
        EXPECT_EQ(core.s.phase_, client_core_state::phase::loading);

        // A SessionStart without a level loads the host's; without our id the
        // join order is unknown and the group is the fallback's.
        msg.map.level = 0;
        msg.players.clear();
        core.begin(msg, config);
        EXPECT_EQ(core.s.level_, 5);
        EXPECT_EQ(core.s.own_join_order_, -1);
        EXPECT_EQ(core.s.own_group_, "P#63");
    }

    // SessionReady carries the anchor hash, the host's strings and CAP_LEAD
    // iff the host can slew (design 2.3); the hold is armed 10 s ahead.
    TEST(SessionClientCoreFlow, AnchorSendsReadyAndArmsTheHold) {
        for (bool slew: {false, true}) {
            flow_fake fake;
            session_capabilities caps;
            caps.slew = slew;
            session_client_core core(fake.make_flow(), caps);
            fake.fill_legacy(core);
            core.s.resync_in_flight_ = true;
            core.s.resync_owed_ = true;
            core.s.window_index_ = 3;
            run_to_anchor(core, fake);
            const std::vector<std::string> expected = {
                "log:0:session 9 starting: level 13, 2 players, join order 2, seed 11, input delay 4, sector lead 7",
                "legacy:reset_held_epoch:a new session",
                "hook:numbering_restarted:anchor_origin",
                "legacy:drop_pending_rewind",
                "now:anchor_hold",
                "send:0",
                "log:0:anchored after 12 load ticks: pose 0000000000000051 surfaces 0000000000000052 cores=0 factor=0.000000"};
            EXPECT_EQ(fake.calls, expected);
            EXPECT_EQ(core.s.phase_, client_core_state::phase::running);
            EXPECT_EQ(core.s.frames_since_anchor_, 0);
            EXPECT_TRUE(core.holding());
            EXPECT_EQ(core.s.hold_deadline_, fake.clock + bmmo::session::kAnchorHoldTimeout);
            EXPECT_FALSE(core.s.resync_in_flight_);
            EXPECT_FALSE(core.s.resync_owed_);
            EXPECT_EQ(core.s.window_index_, 0xFFFFFFFFu);
            ASSERT_EQ(fake.sent.size(), 1u);
            const auto ready = decode<bmmo::session_ready_msg>(fake.sent[0].first);
            EXPECT_EQ(ready.session, 9u);
            EXPECT_EQ(ready.first_tick, 0u);
            EXPECT_EQ(ready.anchor_hash, 0x51u);
            EXPECT_EQ(ready.anchor_surfaces, 0x52u);
            EXPECT_EQ(ready.physics_sha256, "headless-64");
            EXPECT_EQ(ready.build_id, "build-x");
            EXPECT_EQ(ready.caps, slew ? bmmo::session::CAP_LEAD : 0u);
        }
    }

    // Design 5.1 "The hold": released on assign; held while the deadline is
    // ahead, polls counted.
    TEST(SessionClientCoreFlow, HoldReleasesOnAssign) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        for (int i = 0; i < 3; ++i) {
            fake.advance(std::chrono::milliseconds(1000));
            const auto state = core.poll_hold();
            EXPECT_TRUE(state.held);
            EXPECT_FALSE(state.timed_out);
        }
        EXPECT_EQ(core.s.hold_polls_, 3);
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 120));
        EXPECT_TRUE(core.assigned());
        EXPECT_FALSE(core.holding());
        EXPECT_EQ(core.s.tick_base_, 120u);
        EXPECT_EQ(core.s.held_floor_, 120u);
        EXPECT_EQ(core.s.frames_since_anchor_, 0);
        EXPECT_EQ(core.current_tick(), 120u);
        const std::vector<std::string> expected = {
            "legacy:set_aside_unanswered:2",
            "hook:numbering_restarted:assign_renumber",
            "legacy:reset_held_epoch:the new base",
            "hook:numbering_restarted:assign_release",
            "log:0:anchor hold released after 3 polls: the first frame is tick 120",
            "log:0:tick base 120 assigned (0 frames since anchor)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        const auto after = core.poll_hold();
        EXPECT_FALSE(after.held);
        EXPECT_FALSE(after.timed_out);
        EXPECT_EQ(core.s.hold_polls_, 3) << "no poll counted once released";
    }

    // Design 5.1 "The hold": the timeout at 10 s on the fake clock, with the
    // journal note.
    TEST(SessionClientCoreFlow, HoldTimesOutAfterTenSeconds) {
        journal_dir dir("hold");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_flow(true), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        fake.advance(std::chrono::milliseconds(9999));
        EXPECT_TRUE(core.poll_hold().held);
        fake.advance(std::chrono::milliseconds(1));
        const size_t from = fake.calls.size();
        const auto state = core.poll_hold();
        EXPECT_FALSE(state.held);
        EXPECT_TRUE(state.timed_out);
        EXPECT_FALSE(core.holding());
        EXPECT_EQ(core.s.hold_polls_, 2);
        const std::vector<std::string> expected = {
            "now:hold_poll", "hook:numbering_restarted:hold_timeout",
            "log:0:anchor hold: no tick base after 10.0 s; running anyway (the world will be out of phase with the server's)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_FALSE(core.poll_hold().held);
        core.journal().close("done");
        bmmo::session::journal j;
        std::string error;
        ASSERT_TRUE(bmmo::session::read_journal(fake.path, j, error)) << error;
        bool noted = false;
        for (const auto& n: j.notes) noted = noted || n.text == "anchor hold timed out; running without a tick base";
        EXPECT_TRUE(noted);
    }

    // Design 5.1 "Numbering": a first assignment renumbers; a duplicate or an
    // older one changes nothing; base 0 leaves the numbering alone.
    TEST(SessionClientCoreFlow, DuplicateOrOlderAssignmentChangesNothing) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(8, 120));   // another session
        core.s.phase_ = client_core_state::phase::loading;
        core.on_session_assign(assign_message(9, 120));   // not running yet
        core.s.phase_ = client_core_state::phase::running;
        EXPECT_FALSE(core.assigned());
        core.on_session_assign(assign_message(9, 120));
        core.s.frames_since_anchor_ = 40;
        core.s.input_history_.push_back({159, bmmo::session::input_frame{}});
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 120));
        core.on_session_assign(assign_message(9, 100));
        EXPECT_EQ(core.s.duplicate_assigns_, 2u);
        EXPECT_EQ(core.s.tick_base_, 120u);
        EXPECT_EQ(core.s.frames_since_anchor_, 40);
        EXPECT_EQ(core.s.input_history_.size(), 1u);
        EXPECT_TRUE(since(fake.calls, from).empty());

        // Base 0: the numbering stays the anchor's, frames and inputs kept.
        flow_fake zero;
        session_client_core other(zero.make_flow(), session_capabilities{});
        zero.fill_legacy(other);
        run_to_anchor(other, zero);
        other.s.frames_since_anchor_ = 2;
        const size_t zfrom = zero.calls.size();
        other.on_session_assign(assign_message(9, 0));
        EXPECT_TRUE(other.assigned());
        EXPECT_EQ(other.s.frames_since_anchor_, 2);
        const std::vector<std::string> expected = {
            "legacy:reset_held_epoch:the new base", "hook:numbering_restarted:assign_release",
            "log:0:anchor hold released after 0 polls: the first frame is tick 0",
            "log:0:tick base 0 assigned (2 frames since anchor)"};
        EXPECT_EQ(since(zero.calls, zfrom), expected);
    }

    // Design 5.1 "Numbering": a re-assignment resyncs - the history goes,
    // rebase_own_lives, held_floor_ at the new base, a snapshot awaited.
    TEST(SessionClientCoreFlow, ReassignmentResyncs) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.s.frames_since_anchor_ = 300;
        core.s.input_history_.push_back({419, bmmo::session::input_frame{}});
        core.s.own_inputs_[419] = bmmo::session::input_frame{};
        core.s.have_snapshot_ = true;
        core.s.resync_in_flight_ = true;
        core.s.resync_owed_ = true;
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 2000));
        EXPECT_EQ(core.s.tick_base_, 2000u);
        EXPECT_EQ(core.s.held_floor_, 2000u);
        EXPECT_EQ(core.s.last_assign_first_tick_, 2000u);
        EXPECT_EQ(core.s.frames_since_anchor_, 0);
        EXPECT_TRUE(core.s.input_history_.empty());
        EXPECT_TRUE(core.s.own_inputs_.empty());
        EXPECT_TRUE(core.resync_pending());
        EXPECT_FALSE(core.s.have_snapshot_);
        EXPECT_FALSE(core.s.resync_in_flight_);
        EXPECT_FALSE(core.s.resync_owed_);
        const std::vector<std::string> expected = {
            "legacy:reset_held_epoch:the new base", "legacy:rebase_own_lives",
            "hook:numbering_restarted:assign_resync", "log:0:resynced: tick base 2000"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_TRUE(fake.sent.size() == 1u) << "a resync sends nothing (the SessionReady only)";
    }

    // The limiter: nothing before the assignment; one request in flight for
    // up to 10 s; 2 s between requests, an owed one asked again by
    // retry_owed_resync; a slew in flight ended, acknowledged in full.
    TEST(SessionClientCoreFlow, ResyncLimiter) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.request_resync("before the base");
        EXPECT_EQ(core.s.resyncs_sent_, 0u);
        core.on_session_assign(assign_message(9, 120));
        core.s.last_snapshot_tick_ = 115;
        size_t from = fake.calls.size();
        core.request_resync("first");
        EXPECT_EQ(core.s.resyncs_sent_, 1u);
        EXPECT_TRUE(core.s.resync_in_flight_);
        {
            const std::vector<std::string> expected = {"now:resync_limiter", "send:0",
                                                       "log:0:resync requested (first) at tick 120"};
            EXPECT_EQ(since(fake.calls, from), expected);
            const auto msg = decode<bmmo::session_resync_msg>(fake.sent.back().first);
            EXPECT_EQ(msg.session, 9u);
            EXPECT_EQ(msg.last_full_tick, 115u);
        }
        // In flight: nothing for 10 s, not even an owed note.
        fake.advance(std::chrono::milliseconds(5000));
        from = fake.calls.size();
        core.request_resync("in flight", true);
        EXPECT_FALSE(core.s.resync_owed_);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"now:resync_limiter"});
        // Answered (a newer base), then asked again within 2 s: owed.
        core.on_session_assign(assign_message(9, 900));
        core.s.resync_pending_ = false;
        fake.advance(std::chrono::milliseconds(-4000));   // 1 s after the first request
        from = fake.calls.size();
        core.request_resync("not owed");
        EXPECT_FALSE(core.s.resync_owed_);
        core.request_resync("owed", true);
        EXPECT_TRUE(core.s.resync_owed_);
        EXPECT_EQ(core.s.resync_owed_reason_, "owed");
        {
            const std::vector<std::string> expected = {"now:resync_limiter", "now:resync_limiter",
                                                       "log:0:resync owed (owed) at tick 900: the limiter holds it back"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        core.retry_owed_resync();   // still within 2 s: kept
        EXPECT_TRUE(core.s.resync_owed_);
        EXPECT_EQ(core.s.resyncs_sent_, 1u);
        fake.advance(std::chrono::milliseconds(1000));
        core.s.lead_slew_.start(1, 3);
        from = fake.calls.size();
        core.retry_owed_resync();
        EXPECT_FALSE(core.s.resync_owed_);
        EXPECT_EQ(core.s.resyncs_sent_, 2u);
        {
            const std::vector<std::string> expected = {
                "now:resync_limiter", "legacy:finish_lead_slew:ended by a resync request (owed (owed)):reanchored",
                "send:0", "log:0:resync requested (owed (owed)) at tick 900"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        // A resync waiting for its snapshot is the answer to an owed one.
        core.s.resync_owed_ = true;
        core.s.resync_pending_ = true;
        from = fake.calls.size();
        core.retry_owed_resync();
        EXPECT_FALSE(core.s.resync_owed_);
        EXPECT_TRUE(since(fake.calls, from).empty());
    }

    // send_event: stamped with the session, origin = stamp, counted; the
    // host's filter may keep it; on the wire it goes reliable no-Nagle and is
    // repeated unreliably by flush_inputs once assigned, beside the inputs
    // (one datagram per MAX_INPUT_FRAMES), each remembered in the sent ring.
    TEST(SessionClientCoreFlow, EventsAndInputsGoOut) {
        flow_fake fake;
        session_platform p = fake.make_flow();
        bool hold = true;
        p.hooks.filter_event = [&](const bmmo::session_event_msg&) {
            fake.calls.push_back("hook:filter_event");
            return hold ? bmmo::session::event_send_verdict::hold : bmmo::session::event_send_verdict::send;
        };
        session_client_core core(std::move(p), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        bmmo::session_event_msg event;
        event.tick = 130;
        event.type = bmmo::session::event_type::Physicalize;
        size_t from = fake.calls.size();
        const size_t sent_before = fake.sent.size();
        core.send_event(event);
        EXPECT_EQ(event.session, 9u);
        EXPECT_EQ(event.origin_tick, 130u);
        EXPECT_EQ(core.s.events_sent_, 1u);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"hook:filter_event"});
        EXPECT_EQ(fake.sent.size(), sent_before);
        core.flush_inputs();   // not assigned: nothing
        EXPECT_EQ(fake.sent.size(), sent_before);
        const std::string held_bytes = event.raw.str();
        core.release_event(held_bytes);
        hold = false;
        core.send_event(event);
        ASSERT_EQ(fake.sent.size(), sent_before + 2);
        EXPECT_EQ(fake.sent[sent_before].second, send_mode::reliable_no_nagle);
        EXPECT_EQ(fake.sent[sent_before].first, held_bytes);
        EXPECT_EQ(fake.sent[sent_before + 1].second, send_mode::reliable_no_nagle);

        core.on_session_assign(assign_message(9, 120));   // flushes: two repeats each, no input yet
        const size_t after_assign = fake.sent.size();
        EXPECT_EQ(after_assign, sent_before + 4);
        for (size_t i = sent_before + 2; i < after_assign; ++i)
            EXPECT_EQ(fake.sent[i].second, send_mode::unreliable_no_delay);
        for (uint32_t t = 120; t < 120 + bmmo::session::MAX_INPUT_FRAMES + 3; ++t) {
            bmmo::session::input_frame frame;
            frame.keys = static_cast<uint8_t>(t);
            core.s.input_history_.push_back({t, frame});
        }
        const uint64_t inputs_before = core.s.inputs_sent_;
        core.flush_inputs();
        EXPECT_EQ(core.s.inputs_sent_, inputs_before + 2);
        const auto first = decode<bmmo::session_input_msg>(fake.sent[fake.sent.size() - 2].first);
        const auto second = decode<bmmo::session_input_msg>(fake.sent.back().first);
        EXPECT_EQ(first.first_tick, 120u);
        EXPECT_EQ(first.frames.size(), static_cast<size_t>(bmmo::session::MAX_INPUT_FRAMES));
        EXPECT_EQ(second.first_tick, 120u + bmmo::session::MAX_INPUT_FRAMES);
        EXPECT_EQ(second.frames.size(), 3u);
        EXPECT_EQ(core.s.sent_inputs_.size(), static_cast<size_t>(bmmo::session::MAX_INPUT_FRAMES) + 3);
        EXPECT_EQ(core.s.sent_inputs_.at(121).keys, 121);
    }

    // SessionEnd: another session's is only logged; ours goes idle, in
    // order: the host's discards, the journal closed, the host's world, the
    // state, the held epoch, the status line, then idle.
    TEST(SessionClientCoreFlow, EndGoesIdle) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.s.remotes_[4].entity = "Ball_Wood_BMMO_4";
        bmmo::session_end_msg other;
        other.session = 8;
        other.reason = "elsewhere";
        size_t from = fake.calls.size();
        core.on_session_end(other);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"log:0:session 8 ended: elsewhere"});
        EXPECT_EQ(core.s.phase_, client_core_state::phase::running);
        bmmo::session_end_msg ours;
        ours.session = 9;
        from = fake.calls.size();
        core.on_session_end(ours);
        const std::vector<std::string> expected = {
            "log:0:session 9 ended: ", "hook:ending", "legacy:end_session_world",
            "legacy:reset_held_epoch:the session's end", "legacy:report"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_EQ(core.s.phase_, client_core_state::phase::idle);
        EXPECT_EQ(core.s.session_, 0u);
        EXPECT_FALSE(core.assigned());
        EXPECT_TRUE(core.s.remotes_.empty());
        EXPECT_EQ(core.s.sessions_ended_, 1u);
    }

    // ---------------------------------------------------- the snapshot path
    // Step E4: SessionSnapshot's count and apply_snapshot - the resync
    // rebuild, the snapshot rollback and the legacy ladder - over the
    // platform's write_body, own_driven_entity, bind_world and
    // keep_live_cells, with the legacy back-calls recorded.

    bmmo::session::body_state ball_row(uint32_t owner, double x, bool simulated) {
        bmmo::session::body_state b;
        b.kind = bmmo::session::body_kind::Ball;
        b.owner = owner;
        b.position[0] = x;
        b.rotation[3] = 1.0;
        b.flags = simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0;
        return b;
    }

    bmmo::session_snapshot_msg snapshot_message(uint32_t session, uint32_t tick, bool full,
                                                std::vector<bmmo::session::body_state> bodies) {
        bmmo::session_snapshot_msg msg;
        msg.session = session;
        msg.tick = tick;
        msg.full = full;
        msg.bodies = std::move(bodies);
        return msg;
    }

    // Counted always; applied only when it is this running session's; a
    // delta at or before the last applied tick is stale (a full one is not).
    // The rollback path: the live cells kept around the bound world, an
    // unmatched tick rolls nothing back.
    TEST(SessionClientCoreSnapshot, CountedAppliedAndStale) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        core.on_session_snapshot(snapshot_message(9, 100, true, {}));
        EXPECT_EQ(core.s.snapshots_received_, 1u);
        EXPECT_TRUE(fake.calls.empty()) << "idle: counted only";
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        size_t from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(8, 125, true, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(core.s.snapshots_received_, 2u);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "another session's: counted only";
        EXPECT_FALSE(core.s.have_snapshot_);

        core.on_session_snapshot(snapshot_message(9, 125, false, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(core.s.snapshots_received_, 3u);
        EXPECT_EQ(core.s.snapshots_applied_, 1u);
        EXPECT_TRUE(core.s.have_snapshot_);
        EXPECT_EQ(core.s.last_snapshot_tick_, 125u);
        EXPECT_EQ(core.rollback_stats().unmatched, 1u) << "no record of tick 125";
        EXPECT_EQ(core.s.snapshot_rollbacks_, 0u);
        {
            // The own row names no entity while the own ball has no body.
            const std::vector<std::string> expected = {"legacy:own_ball", "keep_live_cells:enter:120", "bind_world",
                                                       "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 125, false, {}));
        core.on_session_snapshot(snapshot_message(9, 110, false, {}));
        EXPECT_EQ(core.s.snapshots_stale_, 2u);
        EXPECT_EQ(core.s.snapshots_applied_, 1u);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "stale: nothing read";
        core.on_session_snapshot(snapshot_message(9, 110, true, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(core.s.snapshots_applied_, 2u) << "a full one is never stale";
        EXPECT_EQ(core.rollback_stats().unmatched, 2u);
        {
            const std::vector<std::string> expected = {"legacy:own_ball", "keep_live_cells:enter:120", "bind_world",
                                                       "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
    }

    // The resync rebuild: only a full snapshot answers it; the history and a
    // pending replay go, every ball with a body here is written (the own one
    // awake, the others as the row says), the held floor moves past the
    // current tick.
    TEST(SessionClientCoreSnapshot, ResyncRebuildsFromTheFullSnapshot) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.on_session_assign(assign_message(9, 2000));
        ASSERT_TRUE(core.resync_pending());
        core.s.frames_since_anchor_ = 10;   // current tick 2009
        core.s.own_physicalized_ = true;
        core.s.remotes_[4].entity = "Ball_Wood_BMMO_4";
        core.s.remotes_[4].physicalized = true;
        core.s.remotes_[5].entity = "Ball_Wood_BMMO_5";
        const std::vector<bmmo::session::body_state> rows = {ball_row(3, 1.0, false), ball_row(4, 2.0, false),
                                                             ball_row(5, 3.0, true), ball_row(6, 4.0, true)};
        size_t from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 1995, false, rows));
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"legacy:own_ball"});
        EXPECT_TRUE(core.resync_pending());
        EXPECT_EQ(core.s.snapshots_applied_, 0u);
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 1995, true, rows));
        const std::vector<std::string> expected = {
            "legacy:own_ball", "legacy:drop_pending_rewind", "write_body:Ball_Wood:1.000000:wake",
            "write_body:Ball_Wood_BMMO_4:2.000000:freeze",
            "log:0:resync applied from the full snapshot of tick 1995 (4 bodies)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_FALSE(core.resync_pending());
        EXPECT_TRUE(core.s.have_snapshot_);
        EXPECT_EQ(core.s.last_snapshot_tick_, 1995u);
        EXPECT_EQ(core.s.snapshots_applied_, 1u);
        EXPECT_EQ(core.s.resyncs_done_, 1u);
        EXPECT_EQ(core.s.held_floor_, 2010u);
    }

    // The legacy ladder (rollback off): the own ball compared with its
    // recorded pose - a hard correction written only with `correct`, the
    // third in a row asks for a resync; a remote without navigation written
    // as the row says, a failed write logged.
    TEST(SessionClientCoreSnapshot, LegacyLadder) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        fake.fill_legacy(core);
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.config.rollback = false;
        core.s.own_physicalized_ = true;
        auto hard_at = [&](uint32_t tick) {
            bmmo::session::ball_pose local;
            local.tick = tick;
            core.s.corrector_.record(local);
            core.on_session_snapshot(snapshot_message(9, tick, false, {ball_row(3, 5.0, true)}));
        };
        size_t from = fake.calls.size();
        hard_at(125);
        {
            const std::vector<std::string> expected = {
                "legacy:own_ball", "log:0:hard correction of own ball for tick 125 (error 5.0000 m, local tick 120)",
                "write_body:Ball_Wood:5.000000:wake"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.s.consecutive_hard_, 1);
        core.config.correct = false;
        from = fake.calls.size();
        hard_at(126);
        {
            const std::vector<std::string> expected = {
                "legacy:own_ball", "log:0:hard correction of own ball for tick 126 (error 5.0000 m, local tick 120)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        from = fake.calls.size();
        hard_at(127);
        {
            const std::vector<std::string> expected = {
                "legacy:own_ball", "now:resync_limiter", "send:0",
                "log:0:resync requested (3 hard corrections in a row) at tick 120",
                "log:0:hard correction of own ball for tick 127 (error 5.0000 m, local tick 120)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.s.resyncs_sent_, 1u);
        EXPECT_EQ(core.s.consecutive_hard_, 0) << "reset by the request";

        core.s.remotes_[4].entity = "Ball_Wood_BMMO_4";
        core.s.remotes_[4].physicalized = true;
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 128, false, {ball_row(4, 2.0, false)}));
        fake.write_ok = false;
        core.on_session_snapshot(snapshot_message(9, 129, false, {ball_row(4, 3.0, true)}));
        {
            const std::vector<std::string> expected = {
                "legacy:own_ball", "write_body:Ball_Wood_BMMO_4:2.000000:freeze",
                "legacy:own_ball", "write_body:Ball_Wood_BMMO_4:3.000000:wake", "log:0:remote 4: no body"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.s.remote_writes_, 1u);
    }
}
