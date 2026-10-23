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
// legacy ladder); step E5 the relay and late-player parts one and two
// (design 5.1 "SessionLead": with the slew capability the range amended and
// forced and the ack at the slew's end, without it nothing applied or
// acknowledged; the relay intake; the forced own frames; the input replay and
// the forced rewind's call order).  E5 moved the targets of four back-calls
// into the core, so the call lists below no longer show them.  Step E6 adds
// the remote lifecycle (design 5.1 "Events": the intake, the drain's order,
// the held body changes, the supersede guards, the births made inside a
// replay, the mirrors' platform calls and the event log's mute).  Step E7
// adds the own lives (design 5.1 "Own echoes", "Own reports from hooks",
// "Beam": each echo verdict with and without own_body_control and with the
// own_echoes switch off; the reports queued and committed after the drain's
// bump; the beam stamped T+1, live and in a replay) and takes the five
// back-calls whose targets moved out of the call lists.  Step E8 adds the
// sectors (design 5.1 "Sector request before the assignment": pending until
// the first assigned frame, then stamped T + lead; the union pass's cell
// write raising no Sector event in the own poll; the own crossings stamped
// ahead or reported after the fact).  Step E9 adds the frame (design 5.1
// "Frame order": one pre_step / step / post_step cycle and the next
// pre_step make design 1.3's calls in order; the own ball's body events; the
// input_rewind and own_corrections switches off: no replay, no own amend;
// the pacer's rebase threshold and slew step) and takes the two back-calls
// whose targets moved (recorded_input_at, own_ball) out of the call lists.
// Step E10 adds the reports (design 5.1 "Status text": the host's counters
// at their places in the status line, the window: line at its edges and its
// phase, the window hook at post_step step 31 and the status hook at the
// session's end) and takes the back-call `report` out of the call lists.
// Step E11 closes the core: its state and the frame's steps are private, so
// the tests reach them through core_test_access (defined here, the core's one
// friend for them: state_of(core), steps{core}); legacy_calls is gone, its last
// two entries are the optional hooks.ended and hooks.describe_movables, which
// the flow fake records as "hook:ended" and "hook:describe_movables".

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <session/client_core.hpp>
#include <session/spawn_impulse.hpp>
// After client_core.hpp: the message headers are not self-contained.
#include <message/session_lead_msg.hpp>
#include <message/session_ready_msg.hpp>
#include <message/session_remote_input_msg.hpp>
#include <message/session_resync_msg.hpp>

// The core's state and its frame's steps are private (step E11); the tests
// reach them through this friend.  steps{core}.<step>(...) calls a private
// step; state_of(core) is the state.
namespace bmmo::session {
    struct core_test_access {
        session_client_core& c;
        client_core_state& s() { return c.s; }
#define BMMO_CORE_STEP(name) \
        template <class... A> decltype(auto) name(A&&... a) { return c.name(std::forward<A>(a)...); }
        BMMO_CORE_STEP(drain_due_events)
        BMMO_CORE_STEP(apply_held_lifecycle)
        BMMO_CORE_STEP(drain_pending_births)
        BMMO_CORE_STEP(attach_remote_navigation)
        BMMO_CORE_STEP(process_own_echoes)
        BMMO_CORE_STEP(report_physicalize)
        BMMO_CORE_STEP(report_unphysicalize)
        BMMO_CORE_STEP(commit_own_reports)
        BMMO_CORE_STEP(apply_requested_beam)
        BMMO_CORE_STEP(destroy_moved_own_birth)
        BMMO_CORE_STEP(hold_pending_sector)
        BMMO_CORE_STEP(sector_union_pass)
        BMMO_CORE_STEP(poll_own_sector)
        BMMO_CORE_STEP(replay_corrected_inputs)
        BMMO_CORE_STEP(retry_owed_resync)
        BMMO_CORE_STEP(take_forced_own)
        BMMO_CORE_STEP(play_forced_own)
        BMMO_CORE_STEP(take_held_ahead)
        BMMO_CORE_STEP(finish_lead_slew)
        BMMO_CORE_STEP(drop_carried_own_replay)
        BMMO_CORE_STEP(held_header_current)
        BMMO_CORE_STEP(flush_inputs)
#undef BMMO_CORE_STEP
        static bmmo::session::input_frame live_drive(const client_core_state::remote_ball& remote, uint32_t tick) {
            return session_client_core::live_drive(remote, tick);
        }
    };
}

namespace {
    using bmmo::session::clock_site;
    using bmmo::session::client_core_state;
    using bmmo::session::log_level;
    using bmmo::session::send_mode;
    using bmmo::session::session_capabilities;
    using bmmo::session::session_client_core;
    using bmmo::session::session_platform;
    using steps = bmmo::session::core_test_access;

    client_core_state& state_of(session_client_core& core) { return steps{core}.s(); }

    // Records every platform call in order, as "op:detail".
    struct fake_platform {
        std::vector<std::string> calls;
        std::chrono::steady_clock::time_point clock{std::chrono::milliseconds(1000)};
        std::string driven;              // what own_driven_entity answers
        bool write_ok = true;            // what write_body answers (false: "no body")
        bool mirror_ok = true;           // what mirror_entity answers (false: "" and "no ball")
        bmmo::session::mirror_navigation attach_answer = bmmo::session::mirror_navigation::attached;
        std::vector<bmmo::session::mirror_spec> specs;   // every make_mirror's spec, in order
        std::vector<std::string> bodies_there;           // the entities body_state answers true for (E7)
        int ball_type = 1;                               // what ball_recipe_of answers
        bmmo::session::own_birth_pose capture;           // what own_birth_capture answers
        std::vector<bmmo::session::own_body_spec> own_specs;   // every make_own's spec, in order
        bool manager_active = false;                     // what sector_manager_active answers (E8)
        bmmo::session::sector_poll poll;                 // what poll_sector answers (E8)
        // The frame's (E9): the own ball own_ball answers (`own_physical`:
        // with a body), the replica's name, the navigation graph's turn,
        // the body events drain_body_events hands over (each answer kept),
        // the observation, the movable mechanisms, and a forced own tick's
        // entity ("" none).
        std::string own_ball_name = "Ball_Wood";
        bool own_physical = false;
        std::string replica_name;
        bool navigation_turns = false;
        std::vector<bmmo::session::body_event> body_events;
        std::vector<bool> body_event_answers;
        bmmo::session::input_observation observation;
        std::vector<std::string> mechanisms;
        std::string forced_entity;

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
                return std::find(bodies_there.begin(), bodies_there.end(), entity) != bodies_there.end();
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
            p.bind_world = [this](const bmmo::session::core_world_hooks&) {
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
            // The mirrors' (E6): an entity named Ball_<type>_BMMO_<player>.
            p.mirror_entity = [this](uint32_t player, uint8_t ball_type, std::string& error) {
                calls.push_back("mirror_entity:" + std::to_string(player) + ":" + std::to_string(ball_type));
                if (!mirror_ok) {
                    error = "no ball";
                    return std::string();
                }
                return "Ball_" + std::to_string(ball_type) + "_BMMO_" + std::to_string(player);
            };
            p.make_mirror = [this](const bmmo::session::mirror_spec& spec) {
                calls.push_back("make_mirror:" + spec.entity + ":" + spec.group
                                + (spec.kick ? ":kick" + std::to_string(spec.kick_index) : std::string()));
                specs.push_back(spec);
                bmmo::session::mirror_made made;
                made.made = true;
                made.kicked = spec.kick;
                return made;
            };
            p.unmake_mirror = [this](const std::string& entity) { calls.push_back("unmake_mirror:" + entity); };
            p.detach_mirror_navigation = [this](const std::string& entity) {
                calls.push_back("detach_mirror_navigation:" + entity);
            };
            p.attach_mirror_navigation = [this](uint32_t player, const std::string& entity, uint8_t, std::string& error) {
                calls.push_back("attach_mirror_navigation:" + std::to_string(player) + ":" + entity);
                if (attach_answer == bmmo::session::mirror_navigation::failed) error = "no graph";
                return attach_answer;
            };
            p.mute_event_log = [this](bool muted) { calls.push_back(muted ? "mute_event_log:on" : "mute_event_log:off"); };
            // The own ball's (E7).
            p.ball_recipe_of = [this](const std::string& entity, bmmo::session::ball_recipe& recipe) {
                calls.push_back("ball_recipe_of:" + entity);
                recipe = {};
                recipe.mass = 1.5f;
                return ball_type;
            };
            p.own_birth_capture = [this](const std::string& entity, uint32_t tick) {
                calls.push_back("own_birth_capture:" + entity + ":" + std::to_string(tick));
                return capture;
            };
            p.make_own = [this](const bmmo::session::own_body_spec& spec) {
                calls.push_back("make_own:" + spec.entity + ":" + spec.group + ":" + std::to_string(spec.tick)
                                + (spec.kick ? ":kick" + std::to_string(spec.kick_index) : std::string()));
                own_specs.push_back(spec);
                bmmo::session::own_body_made made;
                made.found = true;
                made.made = true;
                made.kicked = spec.kick;
                return made;
            };
            p.unmake_own = [this](const std::string& entity, const float*, const float*, uint32_t tick, std::string&) {
                calls.push_back("unmake_own:" + entity + ":" + std::to_string(tick));
                return true;
            };
            // The sectors' (E8).
            p.sector_manager_active = [this]() {
                calls.push_back(manager_active ? "sector_manager_active:on" : "sector_manager_active:off");
                return manager_active;
            };
            p.run_sector = [this](int sector) { calls.push_back("run_sector:" + std::to_string(sector)); };
            p.record_activation = [this](uint32_t tick, bool reset) {
                calls.push_back("record_activation:" + std::to_string(tick) + (reset ? ":reset" : ""));
            };
            p.poll_sector = [this](uint32_t tick) {
                calls.push_back("poll_sector:" + std::to_string(tick));
                return poll;
            };
            // The frame's (E9).
            p.record_pending_activation = [this](uint32_t tick) {
                calls.push_back("record_pending_activation:" + std::to_string(tick));
            };
            p.script_guards = [this](uint32_t tick, bool resolved) {
                calls.push_back("script_guards:" + std::to_string(tick) + (resolved ? ":resolved" : ""));
            };
            p.drive = [this](const std::string& entity, const bmmo::session::input_frame& frame, std::string&) {
                calls.push_back("drive:" + entity + ":" + std::to_string(frame.keys));
                return true;
            };
            p.force_own = [this](const bmmo::session::input_frame& frame, bool& ok, std::string&) {
                calls.push_back("force_own:" + std::to_string(frame.keys));
                ok = true;
                return forced_entity;
            };
            p.unforce_own = [this](const std::string& entity, std::string&) {
                calls.push_back("unforce_own:" + entity);
                return true;
            };
            p.arm_own_capture = [this](uint32_t tick) { calls.push_back("arm_own_capture:" + std::to_string(tick)); };
            p.discover_navigation = [this](uint32_t tick) {
                calls.push_back("discover_navigation:" + std::to_string(tick));
                const bool turned = navigation_turns;
                navigation_turns = false;
                return turned;
            };
            p.own_ball = [this]() {
                calls.push_back("own_ball");
                bmmo::session::own_ball_view own;
                own.entity = own_ball_name;
                own.ball_type = own_ball_name.empty() ? -1 : ball_type;
                own.physicalized = own_physical && !own_ball_name.empty();
                return own;
            };
            p.own_body_upkeep = [this](const bmmo::session::own_ball_view& own) {
                calls.push_back("own_body_upkeep:" + own.entity + (own.physicalized ? ":body" : ""));
            };
            p.own_replica_name = [this]() {
                calls.push_back("own_replica_name");
                return replica_name;
            };
            p.drain_body_events = [this](uint32_t tick, const std::function<bool(const bmmo::session::body_event&)>& each) {
                calls.push_back("drain_body_events:" + std::to_string(tick));
                for (const auto& event: body_events) body_event_answers.push_back(each(event));
                body_events.clear();
            };
            p.reportable_wakeup = [this](const std::string& name, const std::string&) {
                calls.push_back("reportable_wakeup:" + name);
                return name.rfind("Ball_", 0) != 0;
            };
            p.observe_input = [this]() {
                calls.push_back("observe_input");
                return observation;
            };
            p.own_camera_rows = [this](const float (*basis)[3]) {
                calls.push_back("own_camera_rows:" + std::to_string(basis[2][2]));
            };
            p.movable_mechanisms = [this]() {
                calls.push_back("movable_mechanisms");
                return mechanisms;
            };
            p.note_script_resets = [this](uint32_t tick) { calls.push_back("note_script_resets:" + std::to_string(tick)); };
            return p;
        }
    };

    TEST(SessionClientCore, ConstructsWithTheRequiredOperations) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        EXPECT_TRUE(fake.calls.empty()) << "construction calls nothing on the platform";
    }

    TEST(SessionClientCore, RefusesAPlatformWithoutAnOperation) {
        for (int missing = 0; missing < 35; ++missing) {
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
            if (missing == 8) p.mirror_entity = nullptr;
            if (missing == 9) p.make_mirror = nullptr;
            if (missing == 10) p.unmake_mirror = nullptr;
            if (missing == 11) p.detach_mirror_navigation = nullptr;
            if (missing == 12) p.attach_mirror_navigation = nullptr;
            if (missing == 13) p.mute_event_log = nullptr;
            if (missing == 14) p.ball_recipe_of = nullptr;
            if (missing == 15) p.own_birth_capture = nullptr;
            if (missing == 16) p.sector_manager_active = nullptr;
            if (missing == 17) p.run_sector = nullptr;
            if (missing == 18) p.record_activation = nullptr;
            if (missing == 19) p.poll_sector = nullptr;
            if (missing == 20) p.record_pending_activation = nullptr;
            if (missing == 21) p.script_guards = nullptr;
            if (missing == 22) p.drive = nullptr;
            if (missing == 23) p.force_own = nullptr;
            if (missing == 24) p.unforce_own = nullptr;
            if (missing == 25) p.discover_navigation = nullptr;
            if (missing == 26) p.own_ball = nullptr;
            if (missing == 27) p.own_body_upkeep = nullptr;
            if (missing == 28) p.own_replica_name = nullptr;
            if (missing == 29) p.drain_body_events = nullptr;
            if (missing == 30) p.reportable_wakeup = nullptr;
            if (missing == 31) p.observe_input = nullptr;
            if (missing == 32) p.own_camera_rows = nullptr;
            if (missing == 33) p.movable_mechanisms = nullptr;
            if (missing == 34) p.note_script_resets = nullptr;
            EXPECT_THROW(session_client_core(p, session_capabilities{}), std::invalid_argument) << missing;
        }
        // E7: the capture only with the event log's own births; make_own and
        // unmake_own only with own_body_control.
        {
            fake_platform fake;
            session_platform p = fake.make();
            p.own_birth_capture = nullptr;
            p.make_own = nullptr;
            p.unmake_own = nullptr;
            session_capabilities hooks;
            hooks.own_lifecycle = bmmo::session::own_lifecycle_source::host_hooks;
            EXPECT_NO_THROW(session_client_core(p, hooks));
            session_capabilities control = hooks;
            control.own_body_control = true;
            EXPECT_THROW(session_client_core(p, control), std::invalid_argument);
            EXPECT_STREQ(bmmo::session::missing_platform_operation(p, control), "make_own");
            p.make_own = fake.make().make_own;
            EXPECT_STREQ(bmmo::session::missing_platform_operation(p, control), "unmake_own");
        }
        EXPECT_STREQ(bmmo::session::missing_platform_operation(session_platform{}, session_capabilities{}), "send");
        // E9: arm_own_capture is optional (retail has no capture to arm).
        {
            fake_platform fake;
            session_platform p = fake.make();
            p.arm_own_capture = nullptr;
            EXPECT_NO_THROW(session_client_core(p, session_capabilities{}));
        }
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

    // The back-calls (design 6.1) were the host's to fill: E3 added seven, E4
    // four, E5-E10 moved the targets of all but two into the core.  E11
    // deleted legacy_calls: its last two entries, own_body_guard_off and
    // describe_movable_objects, are the optional hooks.ended and
    // hooks.describe_movables.  Neither is required, and a session ends
    // without them.
    TEST(SessionClientCore, BackCallsBecameOptionalHooks) {
        fake_platform fake;
        session_platform p = fake.make();
        EXPECT_FALSE(p.hooks.ended);
        EXPECT_FALSE(p.hooks.describe_movables);
        EXPECT_EQ(bmmo::session::missing_platform_operation(p, session_capabilities{}), nullptr);
        session_client_core core(std::move(p), session_capabilities{});
        core.end("no session");
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::idle);
    }

    // The state moved with its initializers unchanged (design 6.1, E1): the
    // headless client's values at construction.
    TEST(SessionClientCore, StateKeepsTheHeadlessInitializers) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        const client_core_state& s = state_of(core);
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
        EXPECT_TRUE(s.start_sectors_.empty());
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
        EXPECT_FALSE(s.event_log_muted_);
        EXPECT_FALSE(core.journal().journalling());
        EXPECT_EQ(core.config.journal_checkpoint_ticks, 0u);
        EXPECT_TRUE(core.config.rollback);
        EXPECT_TRUE(core.config.correct);
        EXPECT_FALSE(core.config.trace);
        EXPECT_TRUE(core.config.own_echoes);
        EXPECT_TRUE(core.config.input_rewind);
        EXPECT_TRUE(core.config.own_corrections);
        EXPECT_EQ(s.last_keys_, 0u);
        EXPECT_FALSE(s.last_nav_active_);
        EXPECT_EQ(core.previous_cam(), nullptr);
        EXPECT_EQ(core.next_tick(), 0u);
        EXPECT_FALSE(core.slewing());
        EXPECT_EQ(s.snapshot_interval_, 2u);
        EXPECT_FALSE(s.beam_requested_);
        EXPECT_TRUE(s.own_reports_.empty());
        EXPECT_EQ(client_core_state::kOwnEchoDeadlineTicks, 192u);
        EXPECT_EQ(client_core_state::kOwnEchoUplinkSettleTicks, 66u);
        EXPECT_EQ(client_core_state::kOwnRecordKeepTicks, 512u);
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
            p.world_hash = [this](bmmo::physics::world_hash& hash) {
                calls.push_back("world_hash");
                hash = bmmo::physics::world_hash{};
                hash.pose = 0x1234;
                return true;
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
        state_of(core).session_ = 9;
        state_of(core).level_ = 13;
        state_of(core).seed_ = 5;
        state_of(core).own_id_ = 3;
        state_of(core).own_join_order_ = 1;
        bmmo::session::player_entry own, other;
        own.id = 3; own.join_order = 1;
        other.id = 4; other.join_order = 0;
        state_of(core).players_ = {own, other};
    }

    // A host that journals must be able to stamp the header and read bodies.
    TEST(SessionClientCoreJournal, RefusesAJournalWithoutItsOperations) {
        for (int missing = 0; missing < 4; ++missing) {
            journal_fake fake;
            session_platform p = fake.make_journal();
            if (missing == 0) p.utc_ms = nullptr;
            if (missing == 1) p.body_state = nullptr;
            if (missing == 2) p.list_bodies = nullptr;
            if (missing == 3) p.world_hash = nullptr;   // the TICK record (E9)
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
        state_of(core).tick_base_ = 100;
        state_of(core).frames_since_anchor_ = 3;
        EXPECT_EQ(core.current_tick(), 102u);
        core.journal().note("hello");

        auto& remote = state_of(core).remotes_[4];
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
    // fake platform whose clock the test moves, with the host hooks (and,
    // until E11, the legacy back-calls) recorded in the same call list.

    struct flow_fake : journal_fake {
        std::vector<std::pair<std::string, send_mode>> sent;   // every send, in order, with its bytes
        std::vector<std::string> statuses, windows;            // the reports' texts (hooks.status, hooks.window)

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
            p.hooks.ended = [this]() { calls.push_back("hook:ended"); };
            p.hooks.describe_movables = [this]() {
                calls.push_back("hook:describe_movables");
                return std::string("-");
            };
            p.hooks.status = [this](const std::string& text) {
                calls.push_back("hook:status");
                statuses.push_back(text);
            };
            return p;
        }

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
        state_of(core).own_id_ = 3;
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
        auto msg = start_message(9, 3);
        msg.protocol = bmmo::session::PHYSICS_PROTOCOL_VERSION + 1;
        EXPECT_TRUE(core.begin(msg, bmmo::session::core_config{}).refused);
        ASSERT_EQ(fake.calls.size(), 1u);
        EXPECT_EQ(fake.calls[0].rfind("log:0:session 9 refused: the server runs physics protocol", 0), 0u) << fake.calls[0];
        EXPECT_EQ(state_of(core).session_, 0u);
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::idle);
    }

    TEST(SessionClientCoreFlow, BeginTakesTheSessionAndConfig) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        state_of(core).own_id_ = 3;
        state_of(core).assigned_ = true;
        state_of(core).tick_base_ = 77;
        state_of(core).held_ = true;
        state_of(core).hold_polls_ = 5;
        state_of(core).remotes_[4].entity = "x";
        bmmo::session::core_config config;
        config.journal_checkpoint_ticks = 2;
        config.level = 5;
        auto msg = start_message(9, 3);
        EXPECT_FALSE(core.begin(msg, config).refused);
        EXPECT_EQ(core.config.journal_checkpoint_ticks, 2u);
        EXPECT_EQ(state_of(core).session_, 9u);
        EXPECT_EQ(state_of(core).level_, 13) << "the session's level, not the fallback";
        EXPECT_EQ(state_of(core).input_delay_, 4u);
        EXPECT_EQ(state_of(core).sector_lead_, 7u);
        EXPECT_EQ(state_of(core).seed_, 11);
        EXPECT_EQ(state_of(core).own_join_order_, 2);
        EXPECT_EQ(state_of(core).own_group_, "P#2");
        EXPECT_FALSE(state_of(core).assigned_);
        EXPECT_FALSE(state_of(core).held_);
        EXPECT_EQ(state_of(core).hold_polls_, 0);
        EXPECT_EQ(state_of(core).tick_base_, 0u);
        EXPECT_EQ(state_of(core).frames_since_anchor_, -1);
        EXPECT_TRUE(state_of(core).remotes_.empty());
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::idle) << "loading only once the host requested the level";
        const std::vector<std::string> expected = {
            "log:0:session 9 starting: level 13, 2 players, join order 2, seed 11, input delay 4, sector lead 7"};
        EXPECT_EQ(fake.calls, expected);
        core.level_requested();
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::loading);

        // A SessionStart without a level loads the host's; without our id the
        // join order is unknown and the group is the fallback's.
        msg.map.level = 0;
        msg.players.clear();
        core.begin(msg, config);
        EXPECT_EQ(state_of(core).level_, 5);
        EXPECT_EQ(state_of(core).own_join_order_, -1);
        EXPECT_EQ(state_of(core).own_group_, "P#63");
    }

    // SessionReady carries the anchor hash, the host's strings and CAP_LEAD
    // iff the host can slew (design 2.3); the hold is armed 10 s ahead.
    TEST(SessionClientCoreFlow, AnchorSendsReadyAndArmsTheHold) {
        for (bool slew: {false, true}) {
            flow_fake fake;
            session_capabilities caps;
            caps.slew = slew;
            session_client_core core(fake.make_flow(), caps);
            state_of(core).resync_in_flight_ = true;
            state_of(core).resync_owed_ = true;
            state_of(core).window_index_ = 3;
            run_to_anchor(core, fake);
            const std::vector<std::string> expected = {
                "log:0:session 9 starting: level 13, 2 players, join order 2, seed 11, input delay 4, sector lead 7",
                "hook:numbering_restarted:anchor_origin",
                "now:anchor_hold",
                "send:0",
                "log:0:anchored after 12 load ticks: pose 0000000000000051 surfaces 0000000000000052 cores=0 factor=0.000000"};
            EXPECT_EQ(fake.calls, expected);
            EXPECT_EQ(state_of(core).phase_, client_core_state::phase::running);
            EXPECT_EQ(state_of(core).frames_since_anchor_, 0);
            EXPECT_TRUE(core.holding());
            EXPECT_EQ(state_of(core).hold_deadline_, fake.clock + bmmo::session::kAnchorHoldTimeout);
            EXPECT_FALSE(state_of(core).resync_in_flight_);
            EXPECT_FALSE(state_of(core).resync_owed_);
            EXPECT_EQ(state_of(core).window_index_, 0xFFFFFFFFu);
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
        run_to_anchor(core, fake);
        for (int i = 0; i < 3; ++i) {
            fake.advance(std::chrono::milliseconds(1000));
            const auto state = core.poll_hold();
            EXPECT_TRUE(state.held);
            EXPECT_FALSE(state.timed_out);
        }
        EXPECT_EQ(state_of(core).hold_polls_, 3);
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 120));
        EXPECT_TRUE(core.assigned());
        EXPECT_FALSE(core.holding());
        EXPECT_EQ(state_of(core).tick_base_, 120u);
        EXPECT_EQ(state_of(core).held_floor_, 120u);
        EXPECT_EQ(state_of(core).frames_since_anchor_, 0);
        EXPECT_EQ(core.current_tick(), 120u);
        const std::vector<std::string> expected = {
            "hook:numbering_restarted:assign_renumber",
            "hook:numbering_restarted:assign_release",
            "log:0:anchor hold released after 3 polls: the first frame is tick 120",
            "log:0:tick base 120 assigned (0 frames since anchor)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        const auto after = core.poll_hold();
        EXPECT_FALSE(after.held);
        EXPECT_FALSE(after.timed_out);
        EXPECT_EQ(state_of(core).hold_polls_, 3) << "no poll counted once released";
    }

    // Design 5.1 "The hold": the timeout at 10 s on the fake clock, with the
    // journal note.
    TEST(SessionClientCoreFlow, HoldTimesOutAfterTenSeconds) {
        journal_dir dir("hold");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_flow(true), session_capabilities{});
        run_to_anchor(core, fake);
        fake.advance(std::chrono::milliseconds(9999));
        EXPECT_TRUE(core.poll_hold().held);
        fake.advance(std::chrono::milliseconds(1));
        const size_t from = fake.calls.size();
        const auto state = core.poll_hold();
        EXPECT_FALSE(state.held);
        EXPECT_TRUE(state.timed_out);
        EXPECT_FALSE(core.holding());
        EXPECT_EQ(state_of(core).hold_polls_, 2);
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
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(8, 120));   // another session
        state_of(core).phase_ = client_core_state::phase::loading;
        core.on_session_assign(assign_message(9, 120));   // not running yet
        state_of(core).phase_ = client_core_state::phase::running;
        EXPECT_FALSE(core.assigned());
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 40;
        state_of(core).input_history_.push_back({159, bmmo::session::input_frame{}});
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 120));
        core.on_session_assign(assign_message(9, 100));
        EXPECT_EQ(state_of(core).duplicate_assigns_, 2u);
        EXPECT_EQ(state_of(core).tick_base_, 120u);
        EXPECT_EQ(state_of(core).frames_since_anchor_, 40);
        EXPECT_EQ(state_of(core).input_history_.size(), 1u);
        EXPECT_TRUE(since(fake.calls, from).empty());

        // Base 0: the numbering stays the anchor's, frames and inputs kept.
        flow_fake zero;
        session_client_core other(zero.make_flow(), session_capabilities{});
        run_to_anchor(other, zero);
        state_of(other).frames_since_anchor_ = 2;
        const size_t zfrom = zero.calls.size();
        other.on_session_assign(assign_message(9, 0));
        EXPECT_TRUE(other.assigned());
        EXPECT_EQ(state_of(other).frames_since_anchor_, 2);
        const std::vector<std::string> expected = {
            "hook:numbering_restarted:assign_release",
            "log:0:anchor hold released after 0 polls: the first frame is tick 0",
            "log:0:tick base 0 assigned (2 frames since anchor)"};
        EXPECT_EQ(since(zero.calls, zfrom), expected);
    }

    // Design 5.1 "Numbering": a re-assignment resyncs - the history goes,
    // rebase_own_lives, held_floor_ at the new base, a snapshot awaited.
    TEST(SessionClientCoreFlow, ReassignmentResyncs) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 300;
        state_of(core).input_history_.push_back({419, bmmo::session::input_frame{}});
        state_of(core).own_inputs_[419] = bmmo::session::input_frame{};
        state_of(core).have_snapshot_ = true;
        state_of(core).resync_in_flight_ = true;
        state_of(core).resync_owed_ = true;
        // rebase_own_lives (E7, the core's): a birth still unanswered is set
        // aside, an answered one goes; a beam the new numbering has not
        // reached is live once more, an older one goes.
        client_core_state::own_life sent, acked, ahead, behind;
        sent.origin = sent.applied = 410;
        acked.origin = acked.applied = 412;
        acked.state = client_core_state::own_life::verdict::acked;
        ahead.what = behind.what = client_core_state::own_life::kind::beam;
        ahead.origin = ahead.applied = 2004;
        behind.origin = behind.applied = 415;
        ahead.origin_live = ahead.applied_live = true;
        state_of(core).own_life_ = {sent, acked, ahead, behind};
        const size_t from = fake.calls.size();
        core.on_session_assign(assign_message(9, 2000));
        EXPECT_EQ(state_of(core).tick_base_, 2000u);
        EXPECT_EQ(state_of(core).held_floor_, 2000u);
        EXPECT_EQ(state_of(core).last_assign_first_tick_, 2000u);
        EXPECT_EQ(state_of(core).frames_since_anchor_, 0);
        EXPECT_TRUE(state_of(core).input_history_.empty());
        EXPECT_TRUE(state_of(core).own_inputs_.empty());
        EXPECT_TRUE(core.resync_pending());
        EXPECT_FALSE(state_of(core).have_snapshot_);
        EXPECT_FALSE(state_of(core).resync_in_flight_);
        EXPECT_FALSE(state_of(core).resync_owed_);
        const std::vector<std::string> expected = {
            "hook:numbering_restarted:assign_resync", "log:0:resynced: tick base 2000"};
        EXPECT_EQ(since(fake.calls, from), expected);
        ASSERT_EQ(state_of(core).own_unanswered_.size(), 1u);
        EXPECT_EQ(state_of(core).own_unanswered_[0].origin, 410u);
        EXPECT_EQ(state_of(core).own_unanswered_[0].why, client_core_state::own_life::aside::resync);
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        EXPECT_EQ(state_of(core).own_life_[0].origin, 2004u);
        EXPECT_FALSE(state_of(core).own_life_[0].origin_live);
        EXPECT_FALSE(state_of(core).own_life_[0].applied_live);
        EXPECT_TRUE(fake.sent.size() == 1u) << "a resync sends nothing (the SessionReady only)";
    }

    // The limiter: nothing before the assignment; one request in flight for
    // up to 10 s; 2 s between requests, an owed one asked again by
    // retry_owed_resync; a slew in flight ended, acknowledged in full.
    TEST(SessionClientCoreFlow, ResyncLimiter) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.request_resync("before the base");
        EXPECT_EQ(state_of(core).resyncs_sent_, 0u);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).last_snapshot_tick_ = 115;
        size_t from = fake.calls.size();
        core.request_resync("first");
        EXPECT_EQ(state_of(core).resyncs_sent_, 1u);
        EXPECT_TRUE(state_of(core).resync_in_flight_);
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
        EXPECT_FALSE(state_of(core).resync_owed_);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"now:resync_limiter"});
        // Answered (a newer base), then asked again within 2 s: owed.
        core.on_session_assign(assign_message(9, 900));
        state_of(core).resync_pending_ = false;
        fake.advance(std::chrono::milliseconds(-4000));   // 1 s after the first request
        from = fake.calls.size();
        core.request_resync("not owed");
        EXPECT_FALSE(state_of(core).resync_owed_);
        core.request_resync("owed", true);
        EXPECT_TRUE(state_of(core).resync_owed_);
        EXPECT_EQ(state_of(core).resync_owed_reason_, "owed");
        {
            const std::vector<std::string> expected = {"now:resync_limiter", "now:resync_limiter",
                                                       "log:0:resync owed (owed) at tick 900: the limiter holds it back"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        steps{core}.retry_owed_resync();   // still within 2 s: kept
        EXPECT_TRUE(state_of(core).resync_owed_);
        EXPECT_EQ(state_of(core).resyncs_sent_, 1u);
        fake.advance(std::chrono::milliseconds(1000));
        state_of(core).lead_slew_.start(1, 3);
        from = fake.calls.size();
        steps{core}.retry_owed_resync();
        EXPECT_FALSE(state_of(core).resync_owed_);
        EXPECT_EQ(state_of(core).resyncs_sent_, 2u);
        {
            const std::vector<std::string> expected = {
                "now:resync_limiter", "send:0",
                "log:0:lead: SessionLead 1 acknowledged at tick 900: applied +3 of +3, the slew moved +0 (ended by a resync "
                "request (owed (owed))); lead offset +3",
                "send:0", "log:0:resync requested (owed (owed)) at tick 900"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        // A resync waiting for its snapshot is the answer to an owed one.
        state_of(core).resync_owed_ = true;
        state_of(core).resync_pending_ = true;
        from = fake.calls.size();
        steps{core}.retry_owed_resync();
        EXPECT_FALSE(state_of(core).resync_owed_);
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
        run_to_anchor(core, fake);
        bmmo::session_event_msg event;
        event.tick = 130;
        event.type = bmmo::session::event_type::Physicalize;
        size_t from = fake.calls.size();
        const size_t sent_before = fake.sent.size();
        core.send_event(event);
        EXPECT_EQ(event.session, 9u);
        EXPECT_EQ(event.origin_tick, 130u);
        EXPECT_EQ(state_of(core).events_sent_, 1u);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"hook:filter_event"});
        EXPECT_EQ(fake.sent.size(), sent_before);
        steps{core}.flush_inputs();   // not assigned: nothing
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
            state_of(core).input_history_.push_back({t, frame});
        }
        const uint64_t inputs_before = state_of(core).inputs_sent_;
        steps{core}.flush_inputs();
        EXPECT_EQ(state_of(core).inputs_sent_, inputs_before + 2);
        const auto first = decode<bmmo::session_input_msg>(fake.sent[fake.sent.size() - 2].first);
        const auto second = decode<bmmo::session_input_msg>(fake.sent.back().first);
        EXPECT_EQ(first.first_tick, 120u);
        EXPECT_EQ(first.frames.size(), static_cast<size_t>(bmmo::session::MAX_INPUT_FRAMES));
        EXPECT_EQ(second.first_tick, 120u + bmmo::session::MAX_INPUT_FRAMES);
        EXPECT_EQ(second.frames.size(), 3u);
        EXPECT_EQ(state_of(core).sent_inputs_.size(), static_cast<size_t>(bmmo::session::MAX_INPUT_FRAMES) + 3);
        EXPECT_EQ(state_of(core).sent_inputs_.at(121).keys, 121);
    }

    // SessionEnd: another session's is only logged; ours goes idle, in
    // order: the host's discards, the journal closed, the host's world, the
    // state, the held epoch, the status line, then idle.
    TEST(SessionClientCoreFlow, EndGoesIdle) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).remotes_[4].entity = "Ball_Wood_BMMO_4";
        bmmo::session_end_msg other;
        other.session = 8;
        other.reason = "elsewhere";
        size_t from = fake.calls.size();
        core.on_session_end(other);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"log:0:session 8 ended: elsewhere"});
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::running);
        bmmo::session_end_msg ours;
        ours.session = 9;
        from = fake.calls.size();
        core.on_session_end(ours);
        const std::vector<std::string> expected = {
            "log:0:session 9 ended: ", "hook:ending", "hook:ended",
            "hook:status"};
        EXPECT_EQ(since(fake.calls, from), expected);
        ASSERT_EQ(fake.statuses.size(), 1u);
        EXPECT_EQ(fake.statuses[0].rfind("status: phase=2 session=9 tick=120 assigned=1 ", 0), 0u)
            << "printed before the core goes idle: " << fake.statuses[0];
        EXPECT_NE(fake.statuses[0].find(" remotes=0 "), std::string::npos) << "after the mirrors are gone";
        EXPECT_NE(fake.statuses[0].find(" rebases=0 resyncs="), std::string::npos) << "no counters: zeros";
        EXPECT_EQ(state_of(core).phase_, client_core_state::phase::idle);
        EXPECT_EQ(state_of(core).session_, 0u);
        EXPECT_FALSE(core.assigned());
        EXPECT_TRUE(state_of(core).remotes_.empty());
        EXPECT_EQ(state_of(core).sessions_ended_, 1u);
    }

    // ---------------------------------------------------- the snapshot path
    // Step E4: SessionSnapshot's count and apply_snapshot - the resync
    // rebuild, the snapshot rollback and the legacy ladder - over the
    // platform's write_body, own_driven_entity, bind_world and
    // keep_live_cells, with the host's hooks recorded.

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
        core.on_session_snapshot(snapshot_message(9, 100, true, {}));
        EXPECT_EQ(state_of(core).snapshots_received_, 1u);
        EXPECT_TRUE(fake.calls.empty()) << "idle: counted only";
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        size_t from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(8, 125, true, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(state_of(core).snapshots_received_, 2u);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "another session's: counted only";
        EXPECT_FALSE(state_of(core).have_snapshot_);

        core.on_session_snapshot(snapshot_message(9, 125, false, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(state_of(core).snapshots_received_, 3u);
        EXPECT_EQ(state_of(core).snapshots_applied_, 1u);
        EXPECT_TRUE(state_of(core).have_snapshot_);
        EXPECT_EQ(state_of(core).last_snapshot_tick_, 125u);
        EXPECT_EQ(core.rollback_stats().unmatched, 1u) << "no record of tick 125";
        EXPECT_EQ(state_of(core).snapshot_rollbacks_, 0u);
        {
            // The own row names no entity while the own ball has no body.
            const std::vector<std::string> expected = {"own_ball", "keep_live_cells:enter:120", "bind_world",
                                                       "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 125, false, {}));
        core.on_session_snapshot(snapshot_message(9, 110, false, {}));
        EXPECT_EQ(state_of(core).snapshots_stale_, 2u);
        EXPECT_EQ(state_of(core).snapshots_applied_, 1u);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "stale: nothing read";
        core.on_session_snapshot(snapshot_message(9, 110, true, {ball_row(3, 1.0, true)}));
        EXPECT_EQ(state_of(core).snapshots_applied_, 2u) << "a full one is never stale";
        EXPECT_EQ(core.rollback_stats().unmatched, 2u);
        {
            const std::vector<std::string> expected = {"own_ball", "keep_live_cells:enter:120", "bind_world",
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
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.on_session_assign(assign_message(9, 2000));
        ASSERT_TRUE(core.resync_pending());
        state_of(core).frames_since_anchor_ = 10;   // current tick 2009
        state_of(core).own_physicalized_ = true;
        state_of(core).remotes_[4].entity = "Ball_Wood_BMMO_4";
        state_of(core).remotes_[4].physicalized = true;
        state_of(core).remotes_[5].entity = "Ball_Wood_BMMO_5";
        const std::vector<bmmo::session::body_state> rows = {ball_row(3, 1.0, false), ball_row(4, 2.0, false),
                                                             ball_row(5, 3.0, true), ball_row(6, 4.0, true)};
        size_t from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 1995, false, rows));
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"own_ball"});
        EXPECT_TRUE(core.resync_pending());
        EXPECT_EQ(state_of(core).snapshots_applied_, 0u);
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 1995, true, rows));
        const std::vector<std::string> expected = {
            "own_ball", "write_body:Ball_Wood:1.000000:wake",
            "write_body:Ball_Wood_BMMO_4:2.000000:freeze",
            "log:0:resync applied from the full snapshot of tick 1995 (4 bodies)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_FALSE(core.resync_pending());
        EXPECT_TRUE(state_of(core).have_snapshot_);
        EXPECT_EQ(state_of(core).last_snapshot_tick_, 1995u);
        EXPECT_EQ(state_of(core).snapshots_applied_, 1u);
        EXPECT_EQ(state_of(core).resyncs_done_, 1u);
        EXPECT_EQ(state_of(core).held_floor_, 2010u);
    }

    // The legacy ladder (rollback off): the own ball compared with its
    // recorded pose - a hard correction written only with `correct`, the
    // third in a row asks for a resync; a remote without navigation written
    // as the row says, a failed write logged.
    TEST(SessionClientCoreSnapshot, LegacyLadder) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.config.rollback = false;
        state_of(core).own_physicalized_ = true;
        auto hard_at = [&](uint32_t tick) {
            bmmo::session::ball_pose local;
            local.tick = tick;
            state_of(core).corrector_.record(local);
            core.on_session_snapshot(snapshot_message(9, tick, false, {ball_row(3, 5.0, true)}));
        };
        size_t from = fake.calls.size();
        hard_at(125);
        {
            const std::vector<std::string> expected = {
                "own_ball", "log:0:hard correction of own ball for tick 125 (error 5.0000 m, local tick 120)",
                "write_body:Ball_Wood:5.000000:wake"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).consecutive_hard_, 1);
        core.config.correct = false;
        from = fake.calls.size();
        hard_at(126);
        {
            const std::vector<std::string> expected = {
                "own_ball", "log:0:hard correction of own ball for tick 126 (error 5.0000 m, local tick 120)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        from = fake.calls.size();
        hard_at(127);
        {
            const std::vector<std::string> expected = {
                "own_ball", "now:resync_limiter", "send:0",
                "log:0:resync requested (3 hard corrections in a row) at tick 120",
                "log:0:hard correction of own ball for tick 127 (error 5.0000 m, local tick 120)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).resyncs_sent_, 1u);
        EXPECT_EQ(state_of(core).consecutive_hard_, 0) << "reset by the request";

        state_of(core).remotes_[4].entity = "Ball_Wood_BMMO_4";
        state_of(core).remotes_[4].physicalized = true;
        from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 128, false, {ball_row(4, 2.0, false)}));
        fake.write_ok = false;
        core.on_session_snapshot(snapshot_message(9, 129, false, {ball_row(4, 3.0, true)}));
        {
            const std::vector<std::string> expected = {
                "own_ball", "write_body:Ball_Wood_BMMO_4:2.000000:freeze",
                "own_ball", "write_body:Ball_Wood_BMMO_4:3.000000:wake", "log:0:remote 4: no body"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).remote_writes_, 1u);
    }

    // ------------------------------- the relay and late-player parts one and two
    // Step E5: SessionRemoteInput's intake and note_input_correction, the held
    // section, SessionLead with its forced own frames, replay_corrected_inputs
    // and force_rewind, over the same fake platform.

    bmmo::session::input_frame keys_frame(uint8_t keys) {
        bmmo::session::input_frame f;
        f.keys = keys;
        f.cam_dir[2] = 1.0f;
        return f;
    }

    bmmo::session_lead_msg lead_message(uint32_t seq, int16_t delta, uint32_t held_from, uint32_t effective,
                                        uint32_t epoch) {
        bmmo::session_lead_msg msg;
        msg.session = 9;
        msg.player = 3;
        msg.seq = seq;
        msg.delta = delta;
        msg.held_from = held_from;
        msg.effective = effective;
        msg.held = keys_frame(0x21);
        msg.epoch = epoch;
        return msg;
    }

    // An assigned session at base 120 whose current tick is 125 (six frames
    // stepped), with a resync's full snapshot up to 125 behind it (the held
    // floor): the simulated ticks a SessionLead names are base ticks, stored
    // without a replay.
    void run_to_tick_125(session_client_core& core, flow_fake& fake) {
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        state_of(core).held_floor_ = 125;
        ASSERT_EQ(core.current_tick(), 125u);
    }

    // Design 5.1 "SessionLead", with the slew capability: the range's ticks
    // at or before c amended (stored as the server's frame), those of (c, E)
    // forced, the slew started on the slew_start clock, and the ack sent only
    // when the slew ends, with what it moved.  Copies and another base's
    // decisions are dropped.
    TEST(SessionClientCoreLead, WithSlewAmendsForcesAndAcksAtTheSlewsEnd) {
        flow_fake fake;
        session_capabilities caps;
        caps.slew = true;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        const size_t sent_before = fake.sent.size();
        size_t from = fake.calls.size();
        core.on_session_lead(lead_message(1, 4, 121, 130, 120));
        {
            const std::vector<std::string> expected = {
                "now:slew_start",
                "log:0:lead: SessionLead 1 applied: k=+4 N=121 E=130 at tick 125 (base 120): amended 5 [121,126) "
                "forced 4 [126,130) already noticed 0, held-source=default; slewing at 2x"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(fake.sent.size(), sent_before) << "no ack while the slew runs";
        EXPECT_TRUE(state_of(core).lead_slew_.active());
        EXPECT_EQ(state_of(core).lead_adjustments_, 1u);
        EXPECT_EQ(state_of(core).subst_base_, 5u);
        for (uint32_t t = 121; t <= 125; ++t) {
            ASSERT_TRUE(state_of(core).own_inputs_.count(t)) << t;
            EXPECT_EQ(state_of(core).own_inputs_.at(t).keys, 0x21) << t;
        }
        ASSERT_EQ(state_of(core).forced_own_.size(), 4u);
        EXPECT_EQ(state_of(core).forced_own_.begin()->first, 126u);
        EXPECT_EQ(state_of(core).held_learned_total_, 9u) << "every tick of the range learned";

        // The forced frames: taken by tick, older ones dropped; played as the
        // server's frame and counted as a received notice the first time.
        client_core_state::forced_own forced;
        EXPECT_FALSE(steps{core}.take_forced_own(125, forced));
        ASSERT_TRUE(steps{core}.take_forced_own(127, forced));
        EXPECT_EQ(state_of(core).forced_own_.size(), 2u) << "126 dropped, 127 taken";
        bmmo::session::input_frame input = keys_frame(0x01);
        steps{core}.play_forced_own(127, forced, input);
        EXPECT_EQ(input.keys, 0x21);
        EXPECT_EQ(state_of(core).lead_forced_played_, 1u);
        EXPECT_EQ(state_of(core).subst_recv_, 1u);
        EXPECT_EQ(state_of(core).subst_same_, 1u);

        // A copy, and a decision of another base: dropped, nothing sent.
        from = fake.calls.size();
        core.on_session_lead(lead_message(1, 4, 121, 130, 120));
        EXPECT_EQ(state_of(core).lead_dups_, 1u);
        core.on_session_lead(lead_message(2, 4, 121, 130, 100));
        EXPECT_EQ(state_of(core).lead_stale_, 1u);
        EXPECT_EQ(since(fake.calls, from),
                  std::vector<std::string>{"log:0:lead: SessionLead 2 of base 100 dropped (our base 120)"});
        EXPECT_EQ(fake.sent.size(), sent_before);

        // The slew moves its four ticks (the host's pacer, E9), then ends:
        // acknowledged, reliable, with what it moved.
        for (int i = 0; i < 4; ++i) EXPECT_EQ(state_of(core).lead_slew_.advance(1.0, 0.0), 1);
        ASSERT_TRUE(state_of(core).lead_slew_.done());
        from = fake.calls.size();
        steps{core}.finish_lead_slew("slew complete");
        {
            const std::vector<std::string> expected = {
                "send:0",
                "log:0:lead: SessionLead 1 acknowledged at tick 125: applied +4 of +4, the slew moved +4 (slew complete); "
                "lead offset +4"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(fake.sent.size(), sent_before + 1);
        EXPECT_EQ(fake.sent.back().second, send_mode::reliable);
        const auto ack = decode<bmmo::session_lead_ack_msg>(fake.sent.back().first);
        EXPECT_EQ(ack.session, 9u);
        EXPECT_EQ(ack.seq, 1u);
        EXPECT_EQ(ack.applied, 4);
        EXPECT_EQ(state_of(core).lead_offset_, 4);
        EXPECT_FALSE(state_of(core).lead_slew_.active());

        // A new base voids what is left: the forced ticks go.
        core.on_session_assign(assign_message(9, 2000));
        EXPECT_TRUE(state_of(core).forced_own_.empty());
    }

    // Design 5.1 "SessionLead", without it (design 2.3): SessionReady's caps
    // are 0, and a SessionLead that comes anyway is counted and journaled,
    // never applied (nothing amended, forced or slewed) and never
    // acknowledged.
    TEST(SessionClientCoreLead, WithoutSlewNothingAppliedNothingAcked) {
        journal_dir dir("lead");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_flow(true), session_capabilities{});
        run_to_tick_125(core, fake);
        ASSERT_FALSE(fake.sent.empty());
        EXPECT_EQ(decode<bmmo::session_ready_msg>(fake.sent[0].first).caps, 0u);
        const size_t sent_before = fake.sent.size();
        const size_t from = fake.calls.size();
        core.on_session_lead(lead_message(1, 4, 121, 130, 120));
        EXPECT_EQ(state_of(core).lead_refused_, 1u);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "no clock read, no log line, no send";
        EXPECT_EQ(fake.sent.size(), sent_before);
        EXPECT_FALSE(state_of(core).lead_slew_.active());
        EXPECT_TRUE(state_of(core).forced_own_.empty());
        EXPECT_EQ(state_of(core).lead_seen_.count(1u), 1u) << "refused once per decision";
        EXPECT_EQ(state_of(core).lead_adjustments_, 0u);
        EXPECT_EQ(state_of(core).subst_base_, 0u);
        EXPECT_TRUE(state_of(core).own_inputs_.empty());
        EXPECT_EQ(state_of(core).held_learned_total_, 0u);
        // The server's copy of the same decision: a copy, not refused again.
        const size_t copy_from = fake.calls.size();
        core.on_session_lead(lead_message(1, 4, 121, 130, 120));
        EXPECT_EQ(state_of(core).lead_refused_, 1u);
        EXPECT_EQ(state_of(core).lead_dups_, 1u);
        EXPECT_TRUE(since(fake.calls, copy_from).empty());
        // Another session's, or before the assignment: not even counted.
        bmmo::session_lead_msg other = lead_message(2, 4, 121, 130, 120);
        other.session = 8;
        core.on_session_lead(other);
        EXPECT_EQ(state_of(core).lead_refused_, 1u);
        core.journal().close("done");
        bmmo::session::journal j;
        std::string error;
        ASSERT_TRUE(bmmo::session::read_journal(fake.path, j, error)) << error;
        int noted = 0;
        for (const auto& n: j.notes) noted += n.text == "lead: SessionLead 1 refused: this host cannot slew (no CAP_LEAD)";
        EXPECT_EQ(noted, 1);
    }

    bmmo::session_remote_input_msg relay_message(uint32_t tick, uint32_t epoch) {
        bmmo::session_remote_input_msg msg;
        msg.session = 9;
        msg.tick = tick;
        msg.acked = tick;
        msg.epoch = epoch;
        return msg;
    }

    // The relay's intake: counted before the session filter; each first
    // arrival stored (a mirror's ring) or parked (no mirror yet), its lag
    // counted, a re-delivery only counted; a frame for a simulated tick past
    // the history is late; the newest frame drives the live prediction.  A
    // held section of another base is only counted, once per tick.
    TEST(SessionClientCoreRelay, IntakeStoresParksAndCounts) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        auto& remote = state_of(core).remotes_[4];
        remote.entity = "Ball_Wood_BMMO_4";
        remote.physicalized = true;
        remote.navigation = true;

        bmmo::session_remote_input_msg other = relay_message(124, 120);
        other.session = 8;
        core.on_session_remote_input(other);
        EXPECT_EQ(state_of(core).remote_inputs_received_, 1u);
        EXPECT_EQ(state_of(core).relay_new_, 0u) << "another session's: counted only";

        bmmo::session_remote_input_msg msg = relay_message(124, 120);
        bmmo::session_remote_input_msg::entry mirrored, unmirrored;
        mirrored.player = 4;
        mirrored.frames = {keys_frame(1), keys_frame(2), keys_frame(3)};   // ticks 122-124
        unmirrored.player = 5;
        unmirrored.frames = {keys_frame(7)};                              // tick 124
        msg.entries = {mirrored, unmirrored};
        const size_t from = fake.calls.size();
        core.on_session_remote_input(msg);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "nothing logged or sent";
        EXPECT_EQ(state_of(core).remote_inputs_received_, 2u);
        EXPECT_EQ(state_of(core).relay_new_, 4u);
        EXPECT_EQ(state_of(core).relay_parked_, 1u);
        EXPECT_EQ(state_of(core).relay_late_, 3u) << "simulated ticks with no history to replay from";
        EXPECT_EQ(state_of(core).relay_lag_max_, 3u);
        EXPECT_EQ(state_of(core).relay_lag_hist_[1], 2u);   // tick 124, both players
        EXPECT_EQ(state_of(core).window_lag_[4][3], 1u);
        ASSERT_EQ(remote.inputs.size(), 3u);
        EXPECT_TRUE(remote.have_input);
        EXPECT_EQ(remote.input_tick, 124u);
        EXPECT_EQ(remote.input.keys, 3);
        EXPECT_FALSE(state_of(core).input_rewind_pending_);
        EXPECT_EQ(state_of(core).input_reports_, 1u);

        core.on_session_remote_input(msg);   // the window's re-delivery
        EXPECT_EQ(state_of(core).relay_dup_, 4u);
        EXPECT_EQ(state_of(core).relay_new_, 4u);

        bmmo::session_remote_input_msg stale = relay_message(126, 100);
        bmmo::session_remote_input_msg::held_record record;
        record.back = 2;
        stale.held = {record};
        core.on_session_remote_input(stale);
        core.on_session_remote_input(stale);
        EXPECT_EQ(state_of(core).subst_stale_epoch_, 1u);
        EXPECT_TRUE(steps{core}.held_header_current()) << "the header of our own base stays the newest";
        EXPECT_EQ(state_of(core).held_header_tick_, 124u);
    }

    // replay_corrected_inputs: one replay for the frame's corrections, inside
    // the live-cells scope; a refused one is counted and logged (a remote's)
    // and resyncs when it carried an own correction.  force_rewind keeps the
    // live cells for as long as its result lives.
    TEST(SessionClientCoreRelay, ReplayAndForcedRewindKeepTheLiveCells) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        steps{core}.replay_corrected_inputs();   // nothing pending: nothing
        size_t from = fake.calls.size();
        EXPECT_TRUE(since(fake.calls, from).empty());

        state_of(core).input_rewind_pending_ = true;
        state_of(core).input_rewind_tick_ = 118;
        state_of(core).input_rewind_entity_ = "Ball_Wood_BMMO_4";
        state_of(core).input_rewind_entities_ = {"Ball_Wood_BMMO_4"};
        steps{core}.replay_corrected_inputs();
        {
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:125", "bind_world",
                "log:0:input correction for Ball_Wood_BMMO_4 at tick 118 is outside the rollback history at local tick "
                "125: a snapshot has to re-anchor it",
                "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).input_rewinds_missed_, 1u);
        EXPECT_EQ(state_of(core).subst_rewinds_missed_, 0u);
        EXPECT_FALSE(state_of(core).input_rewind_pending_);
        EXPECT_TRUE(state_of(core).input_rewind_entities_.empty());

        // An own correction among them: the refusal asks for a resync.
        state_of(core).input_rewind_pending_ = true;
        state_of(core).input_rewind_tick_ = 119;
        state_of(core).input_rewind_entity_ = "Ball_Wood";
        state_of(core).input_rewind_own_ = true;
        state_of(core).input_rewind_has_own_ = true;
        state_of(core).input_rewind_entities_ = {"Ball_Wood_BMMO_4"};
        from = fake.calls.size();
        steps{core}.replay_corrected_inputs();
        {
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:125", "bind_world",
                "log:0:input correction for Ball_Wood at tick 119 is outside the rollback history at local tick 125: "
                "a snapshot has to re-anchor it",
                "now:resync_limiter", "send:0",
                "log:0:resync requested (a substituted own frame could not be replayed) at tick 125",
                "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).input_rewinds_missed_, 2u);
        EXPECT_EQ(state_of(core).subst_rewinds_missed_, 1u);

        from = fake.calls.size();
        {
            const auto forced = core.force_rewind(12);
            EXPECT_FALSE(forced.rolled);
            EXPECT_EQ(state_of(core).forced_rollbacks_missed_, 1u);
            const std::vector<std::string> inside = {"keep_live_cells:enter:125", "bind_world"};
            EXPECT_EQ(since(fake.calls, from), inside) << "the cells stay kept while the result lives";
        }
        EXPECT_EQ(fake.calls.back(), "keep_live_cells:exit");

        // Deeper than the ticks played (the anchor would wrap): nothing at
        // all, not even counted as missed.
        from = fake.calls.size();
        {
            const auto forced = core.force_rewind(124);
            EXPECT_FALSE(forced.rolled);
            EXPECT_EQ(state_of(core).forced_rollbacks_missed_, 1u);
        }
        EXPECT_TRUE(since(fake.calls, from).empty());
    }

    // ------------------------------------------------- the remote lifecycle
    // Step E6: SessionEvent's intake, the drain at the due tick in the
    // server's order (join, applied, origin, rank), the body changes held
    // past the frame's rewinds with our own lives at our place, the
    // supersede guards, the births made inside a replay with the event log
    // muted, live_drive and the mirrors taken out at the end, over the
    // platform's mirror operations (design 5.1 "Events").

    bmmo::session_event_msg event_message(uint32_t player, bmmo::session::event_type type, uint32_t tick, uint32_t origin) {
        bmmo::session_event_msg msg;
        msg.session = 9;
        msg.player = player;
        msg.type = type;
        msg.tick = tick;
        msg.origin_tick = origin;
        return msg;
    }

    bmmo::session_event_msg sector_message(uint32_t player, int sector, uint32_t tick, uint32_t origin) {
        auto msg = event_message(player, bmmo::session::event_type::Sector, tick, origin);
        msg.sector = sector;
        return msg;
    }

    // run_to_tick_125 with two more members: 5 (join order 1) and 6 (join
    // order 3); the own player 3 is join order 2, player 4 join order 0.
    void run_to_tick_125_four(session_client_core& core, flow_fake& fake) {
        run_to_tick_125(core, fake);
        bmmo::session::player_entry five, six;
        five.id = 5; five.join_order = 1;
        six.id = 6; six.join_order = 3;
        state_of(core).players_.push_back(five);
        state_of(core).players_.push_back(six);
    }

    // Counted always; this running session's only; a copy is counted and
    // dropped; our own is an echo for the frame, another member's waits for
    // its due tick.  Nothing reaches the platform.
    TEST(SessionClientCoreLifecycle, IntakeCountsQueuesAndEchoes) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        const size_t from = fake.calls.size();
        auto other = event_message(4, bmmo::session::event_type::Physicalize, 126, 126);
        other.session = 8;
        core.on_session_event(std::move(other));
        EXPECT_EQ(state_of(core).events_received_, 1u);
        EXPECT_TRUE(state_of(core).pending_events_.empty());
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 126, 126));
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 126, 126));
        core.on_session_event(event_message(3, bmmo::session::event_type::Physicalize, 127, 126));
        EXPECT_EQ(state_of(core).events_received_, 4u);
        EXPECT_EQ(state_of(core).event_dups_, 1u);
        ASSERT_EQ(state_of(core).pending_events_.size(), 1u);
        EXPECT_EQ(state_of(core).pending_events_[0].player, 4u);
        ASSERT_EQ(state_of(core).own_echoes_.size(), 1u);
        EXPECT_EQ(state_of(core).own_echoes_[0].tick, 127u);
        EXPECT_TRUE(since(fake.calls, from).empty());
        // Not running: dropped before the copy check.
        state_of(core).phase_ = client_core_state::phase::loading;
        core.on_session_event(event_message(4, bmmo::session::event_type::Sector, 126, 126));
        EXPECT_EQ(state_of(core).events_received_, 5u);
        EXPECT_EQ(state_of(core).pending_events_.size(), 1u);
        EXPECT_EQ(state_of(core).event_dups_, 1u);
    }

    // Design 5.1 "Events": the drain takes every event due at the tick, in
    // (join, applied, origin, rank); a Physicalize or Unphysicalize stamped
    // for the tick itself is held until apply_held_lifecycle, which makes
    // them in the same order with our own lives at our join rank (between
    // player 4's and player 6's), and once more at the end.
    TEST(SessionClientCoreLifecycle, DrainsInTheServersOrderAndHoldsTheTicksBodyChanges) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        auto spawn = event_message(6, bmmo::session::event_type::Physicalize, 126, 126);
        spawn.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        spawn.ball_type = 1;
        // Arrival order scrambled.
        core.on_session_event(sector_message(6, 2, 124, 124));
        core.on_session_event(sector_message(4, 3, 124, 123));
        core.on_session_event(std::move(spawn));
        core.on_session_event(sector_message(4, 5, 127, 127));
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 126, 126));
        core.on_session_event(sector_message(4, 4, 123, 123));
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        {
            const std::vector<std::string> expected = {
                "log:0:remote player 4 entered sector 4, stamped for tick 123 (LATE: the server started it already)",
                "log:0:remote player 4 entered sector 3, stamped for tick 124 (LATE: the server started it already)",
                "log:0:remote player 6 entered sector 2, stamped for tick 124 (LATE: the server started it already)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(state_of(core).pending_events_.size(), 1u);
        EXPECT_EQ(state_of(core).pending_events_[0].tick, 127u);
        ASSERT_EQ(state_of(core).held_lifecycle_.size(), 2u);
        EXPECT_EQ(state_of(core).held_lifecycle_[0].player, 4u);
        EXPECT_EQ(state_of(core).held_lifecycle_[1].player, 6u);
        EXPECT_TRUE(state_of(core).remotes_.empty()) << "held, not made";

        from = fake.calls.size();
        steps{core}.apply_held_lifecycle(126);
        const uint32_t kick = bmmo::session::spawn_direction_index(11, 3, 126);
        {
            const std::vector<std::string> expected = {
                "mirror_entity:4:0", "make_mirror:Ball_0_BMMO_4:P#0",
                "attach_mirror_navigation:4:Ball_0_BMMO_4", "log:0:remote Ball_0_BMMO_4 now predicted",
                "log:0:remote player 4 physicalized (Ball_0_BMMO_4) at tick 126, predicted",
                "mirror_entity:6:1", "make_mirror:Ball_1_BMMO_6:P#3:kick" + std::to_string(kick),
                "log:0:remote spawn impulse index=" + std::to_string(kick) + " for player 6 at tick 126",
                "attach_mirror_navigation:6:Ball_1_BMMO_6", "log:0:remote Ball_1_BMMO_6 now predicted",
                "log:0:remote player 6 physicalized (Ball_1_BMMO_6) at tick 126, predicted"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).held_lifecycle_.empty());
        // The spec names the player and the ball type: make_mirror stands
        // alone (a host finds the entity again from them).
        ASSERT_EQ(fake.specs.size(), 2u);
        EXPECT_EQ(fake.specs[0].player, 4u);
        EXPECT_EQ(fake.specs[0].ball_type, 0u);
        EXPECT_EQ(fake.specs[1].player, 6u);
        EXPECT_EQ(fake.specs[1].ball_type, 1u);
        EXPECT_EQ(fake.specs[1].entity, "Ball_1_BMMO_6");
        ASSERT_EQ(state_of(core).remotes_.count(6), 1u);
        const auto& six = state_of(core).remotes_.at(6);
        EXPECT_TRUE(six.physicalized);
        EXPECT_TRUE(six.navigation);
        EXPECT_EQ(six.born_tick, 126u);
        EXPECT_EQ(six.driven_from, 126u);
        EXPECT_EQ(state_of(core).generations_.at("Ball_1_BMMO_6"), 1u);
        EXPECT_EQ(state_of(core).remote_last_physicalize_.at(6), 126u);
        EXPECT_EQ(state_of(core).life_rewinds_ + state_of(core).life_rewinds_missed_, 0u) << "on time: no rewind";

        // Nothing held and no own life: nothing.
        from = fake.calls.size();
        steps{core}.apply_held_lifecycle(127);
        EXPECT_TRUE(since(fake.calls, from).empty());
    }

    // The supersede guards: a death no newer (by origin) than the player's
    // last applied birth is a stale copy (counted, nothing done); a birth
    // older than an applied death is dropped in the death's tick, and made
    // and the death applied again when it lived before it.  A late birth
    // outside the history is made live, and its rewind refused and logged.
    TEST(SessionClientCoreLifecycle, SupersedeGuards) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        // A death of player 4, applied while it has no mirror: remembered.
        core.on_session_event(event_message(4, bmmo::session::event_type::Unphysicalize, 124, 124));
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "no mirror: nothing to take out, no rewind";
        EXPECT_EQ(state_of(core).remote_last_unphysicalize_.at(4).origin, 124u);

        // A birth stamped before it, applied in its tick: no ball.
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 124, 122));
        from = fake.calls.size();
        steps{core}.drain_due_events(126);
        EXPECT_EQ(state_of(core).births_overtaken_, 1u);
        {
            const std::vector<std::string> expected = {
                "log:0:remote physicalize of 4 stamped 122 (applied 124) is older than its death stamped 124, applied at "
                "124 already: no ball"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).remotes_.empty());

        // ...applied before it: made (late, outside the history: live), and
        // the death applied again with its own rewind.
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 123, 121));
        from = fake.calls.size();
        steps{core}.drain_due_events(126);
        EXPECT_EQ(state_of(core).births_overtaken_, 2u);
        {
            const std::vector<std::string> expected = {
                "log:0:remote physicalize of 4 stamped 121 (applied 123) is older than its death stamped 124, applied at "
                "124 already: made, and the death applied again",
                "mirror_entity:4:0", "make_mirror:Ball_0_BMMO_4:P#0",
                "attach_mirror_navigation:4:Ball_0_BMMO_4", "log:0:remote Ball_0_BMMO_4 now predicted",
                "log:0:remote player 4 physicalized (Ball_0_BMMO_4) at tick 123, predicted",
                "keep_live_cells:enter:125", "bind_world",
                "log:0:lifecycle event of 4 stamped tick 123 is outside the rollback history at local tick 125: the "
                "body set disagrees until a snapshot re-anchors it",
                "keep_live_cells:exit",
                "detach_mirror_navigation:Ball_0_BMMO_4",
                "mute_event_log:on", "unmake_mirror:Ball_0_BMMO_4", "mute_event_log:off",
                "log:0:remote player 4 unphysicalized at tick 124",
                "keep_live_cells:enter:125", "bind_world",
                "log:0:lifecycle event of 4 stamped tick 124 is outside the rollback history at local tick 125: the "
                "body set disagrees until a snapshot re-anchors it",
                "keep_live_cells:exit"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).life_rewinds_missed_, 2u);
        const auto& four = state_of(core).remotes_.at(4);
        EXPECT_FALSE(four.physicalized);
        EXPECT_FALSE(four.navigation);
        EXPECT_EQ(state_of(core).generations_.at("Ball_0_BMMO_4"), 2u) << "born, then gone";
        EXPECT_EQ(four.born_tick, 126u) << "made live: the next tick";

        // A newer birth, then a death stamped no later than it: a stale copy.
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 125, 125));
        steps{core}.drain_due_events(126);
        ASSERT_TRUE(state_of(core).remotes_.at(4).physicalized);
        core.on_session_event(event_message(4, bmmo::session::event_type::Unphysicalize, 125, 125));
        from = fake.calls.size();
        steps{core}.drain_due_events(126);
        EXPECT_EQ(state_of(core).events_superseded_, 1u);
        EXPECT_TRUE(since(fake.calls, from).empty());
        EXPECT_TRUE(state_of(core).remotes_.at(4).physicalized);
    }

    // The births deferred into a replay, made at their tick before its step:
    // in join order with our own lives at our rank, each with the event log
    // muted (a trafo's take-out nests inside the birth's mute and leaves it
    // on); a birth of another tick waits.
    TEST(SessionClientCoreLifecycle, PendingBirthsMadeInTheReplayMuted) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        // An own beam stamped 124 whose live frame has run: due in the
        // replay of 124, at our rank (written in place: no own body control).
        client_core_state::own_life beam;
        beam.what = client_core_state::own_life::kind::beam;
        beam.origin = beam.applied = 124;
        beam.entity = "Ball_Wood";
        beam.position[0] = 711.0f;
        beam.origin_live = beam.applied_live = true;
        state_of(core).own_life_.push_back(beam);
        // Player 4 has a mirror of another ball type: the birth is a trafo.
        auto& four = state_of(core).remotes_[4];
        four.entity = "Ball_1_BMMO_4";
        four.physicalized = true;
        four.navigation = true;
        auto birth = [](uint32_t player, uint32_t tick, uint32_t origin) {
            client_core_state::pending_birth b;
            b.player = player; b.tick = tick; b.origin = origin;
            return b;
        };
        state_of(core).pending_births_ = {birth(6, 124, 124), birth(5, 125, 125), birth(4, 124, 123)};
        size_t from = fake.calls.size();
        steps{core}.drain_pending_births(124);
        {
            const std::vector<std::string> expected = {
                "mute_event_log:on",
                "mirror_entity:4:0", "detach_mirror_navigation:Ball_1_BMMO_4",
                "mute_event_log:on", "unmake_mirror:Ball_1_BMMO_4", "mute_event_log:on",
                "make_mirror:Ball_0_BMMO_4:P#0",
                "attach_mirror_navigation:4:Ball_0_BMMO_4", "log:0:remote Ball_0_BMMO_4 now predicted",
                "log:0:remote player 4 physicalized (Ball_0_BMMO_4) at tick 124, predicted",
                "mute_event_log:off",
                "write_body:Ball_Wood:711.000000:wake",
                "mute_event_log:on",
                "mirror_entity:6:0", "make_mirror:Ball_0_BMMO_6:P#3",
                "attach_mirror_navigation:6:Ball_0_BMMO_6", "log:0:remote Ball_0_BMMO_6 now predicted",
                "log:0:remote player 6 physicalized (Ball_0_BMMO_6) at tick 124, predicted",
                "mute_event_log:off"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(state_of(core).pending_births_.size(), 1u);
        EXPECT_EQ(state_of(core).pending_births_[0].player, 5u);
        EXPECT_FALSE(state_of(core).event_log_muted_);
        EXPECT_EQ(state_of(core).generations_.at("Ball_1_BMMO_4"), 1u) << "the trafo ends the old mirror's lifetime";
        EXPECT_EQ(state_of(core).remotes_.at(4).born_tick, 124u) << "made inside the replay: its stamp";

        // No birth due and no own life: nothing at all past the check.
        from = fake.calls.size();
        state_of(core).pending_births_.clear();
        steps{core}.drain_pending_births(130);
        EXPECT_TRUE(since(fake.calls, from).empty());
    }

    // A late birth inside the history is deferred into the replay of its
    // rewind; when that replay does not make it (it never ran the tick's
    // pre_step), it is made live after the rewind, with the next live tick
    // as its first, and nothing is left pending.
    TEST(SessionClientCoreLifecycle, BirthTheReplayDidNotReachIsMadeLive) {
        flow_fake fake;
        auto platform = fake.make_flow();
        // A world the engine can rewind and replay: no tracked body, every
        // step taken, and no pre_step (the births deferred into the replay
        // are never drained).
        platform.bind_world = [&fake](const bmmo::session::core_world_hooks&) {
            fake.calls.push_back("bind_world");
            bmmo::session::rollback_world world;
            world.get_body = [](const std::string&, bmmo_physics_body_state&) { return false; };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            world.step = [&fake]() {
                fake.calls.push_back("step");
                return true;
            };
            return world;
        };
        session_client_core core(std::move(platform), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        for (uint32_t tick = 118; tick <= 125; ++tick)
            state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, bmmo::session::rollback_tracked{}, {});
        ASSERT_TRUE(state_of(core).rollback_.knows_tick(122));
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 123, 123));
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        {
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:125", "bind_world", "step", "step", "step",
                "keep_live_cells:exit:rolled back",
                "log:0:remote physicalize of 4: the replay did not reach tick 123, made live",
                "mirror_entity:4:0", "make_mirror:Ball_0_BMMO_4:P#0",
                "attach_mirror_navigation:4:Ball_0_BMMO_4", "log:0:remote Ball_0_BMMO_4 now predicted",
                "log:0:remote player 4 physicalized (Ball_0_BMMO_4) at tick 123, predicted"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).pending_births_.empty());
        EXPECT_EQ(state_of(core).life_rewinds_, 1u);
        EXPECT_EQ(state_of(core).life_rewinds_missed_, 0u);
        EXPECT_EQ(state_of(core).life_rewind_depth_, 3u);
        const auto& four = state_of(core).remotes_.at(4);
        EXPECT_TRUE(four.physicalized);
        EXPECT_EQ(four.born_tick, 126u) << "made live: the next tick";
        EXPECT_EQ(four.driven_from, 126u);
        EXPECT_EQ(state_of(core).generations_.at("Ball_0_BMMO_4"), 1u) << "made once";
    }

    // The --trace diagnostic after a snapshot rollback (E11: the optional
    // hooks.describe_movables, legacy_calls::describe_movable_objects until
    // then): the first 16 rolled snapshots count rollback_diag_ whether or
    // not the host fills the hook; the "after rollback: movable=" line is
    // written only when it does, and never past the 16th.
    TEST(SessionClientCoreSnapshot, TracedRollbackDescribesTheMovablesWhenTheHostCan) {
        struct arm {
            bool hook;
            int diag_before;
            bool line;
        };
        for (const arm a: {arm{true, 0, true}, arm{false, 0, false}, arm{true, 16, false}}) {
            SCOPED_TRACE(std::string(a.hook ? "with" : "without") + " the hook, " + std::to_string(a.diag_before)
                         + " traced before");
            flow_fake fake;
            fake.driven = "Ball_Wood";
            auto platform = fake.make_flow();
            if (!a.hook) platform.hooks.describe_movables = nullptr;
            // A world the engine can rewind and replay: the own ball at the
            // origin, at rest, every step taken.
            auto own = std::make_shared<bmmo_physics_body_state>();
            own->movable = true;
            own->simulated = true;
            own->rotation[3] = 1.0;
            platform.bind_world = [&fake, own](const bmmo::session::core_world_hooks&) {
                fake.calls.push_back("bind_world");
                bmmo::session::rollback_world world;
                world.get_body = [own](const std::string& entity, bmmo_physics_body_state& out) {
                    if (entity != "Ball_Wood") return false;
                    out = *own;
                    return true;
                };
                world.set_body = [own](const std::string& entity, const bmmo_physics_body_state& state,
                                       bmmo::session::wake_mode, bool) {
                    if (entity != "Ball_Wood") return false;
                    *own = state;
                    return true;
                };
                world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
                world.step = []() { return true; };
                return world;
            };
            session_client_core core(std::move(platform), session_capabilities{});
            run_to_tick_125(core, fake);
            core.config.rollback = true;
            core.config.trace = true;
            state_of(core).own_physicalized_ = true;
            state_of(core).rollback_diag_ = a.diag_before;
            bmmo::session::rollback_tracked tracked;
            tracked.own_entity = "Ball_Wood";
            for (uint32_t tick = 118; tick <= 125; ++tick)
                state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, tracked, {});
            const size_t from = fake.calls.size();
            // The server had the own ball 5 m away at tick 122.
            core.on_session_snapshot(snapshot_message(9, 122, false, {ball_row(3, 5.0, true)}));
            EXPECT_EQ(state_of(core).snapshot_rollbacks_, 1u);
            EXPECT_EQ(state_of(core).rollback_diag_, a.diag_before + 1) << "counted with or without the hook";
            const auto calls = since(fake.calls, from);
            const auto has = [&calls](const std::string& call) {
                return std::find(calls.begin(), calls.end(), call) != calls.end();
            };
            EXPECT_TRUE(has("keep_live_cells:exit:rolled back"));
            EXPECT_EQ(has("hook:describe_movables"), a.line);
            EXPECT_EQ(has("log:0:after rollback: movable=-"), a.line);
            if (a.line) {
                // Asked for after the rollback, and written at once.
                const auto asked = std::find(calls.begin(), calls.end(), "hook:describe_movables");
                ASSERT_NE(asked + 1, calls.end());
                EXPECT_EQ(*(asked + 1), "log:0:after rollback: movable=-");
            }
        }
    }

    // A navigation the host cannot give yet leaves the mirror undriven
    // without a word; a failed attach is logged.
    TEST(SessionClientCoreLifecycle, MirrorNavigationAnswers) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        fake.attach_answer = bmmo::session::mirror_navigation::unavailable;
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 126, 126));
        steps{core}.drain_due_events(126);
        size_t from = fake.calls.size();
        steps{core}.apply_held_lifecycle(126);
        {
            const std::vector<std::string> expected = {
                "mirror_entity:4:0", "make_mirror:Ball_0_BMMO_4:P#0", "attach_mirror_navigation:4:Ball_0_BMMO_4",
                "log:0:remote player 4 physicalized (Ball_0_BMMO_4) at tick 126, mirrored"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        fake.attach_answer = bmmo::session::mirror_navigation::failed;
        from = fake.calls.size();
        steps{core}.attach_remote_navigation(4, state_of(core).remotes_.at(4));
        {
            const std::vector<std::string> expected = {
                "attach_mirror_navigation:4:Ball_0_BMMO_4", "log:0:remote navigation for Ball_0_BMMO_4: no graph"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_FALSE(state_of(core).remotes_.at(4).navigation);
        // A refused mirror entity or body: logged, nothing made.
        fake.mirror_ok = false;
        core.on_session_event(event_message(5, bmmo::session::event_type::Physicalize, 127, 127));
        steps{core}.drain_due_events(127);
        from = fake.calls.size();
        steps{core}.apply_held_lifecycle(127);
        {
            const std::vector<std::string> expected = {
                "mirror_entity:5:0", "log:0:remote physicalize: no ball"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).remotes_.count(5), 0u);
    }

    // live_drive: the relayed frame of the tick, else the newest (the
    // predictor), else no keys and the navigation off.
    TEST(SessionClientCoreLifecycle, LiveDrive) {
        client_core_state::remote_ball remote;
        auto none = steps::live_drive(remote, 130);
        EXPECT_EQ(none.keys, 0u);
        EXPECT_EQ(none.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE, 0);
        remote.have_input = true;
        remote.input = keys_frame(0x05);
        remote.input.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        auto predicted = steps::live_drive(remote, 130);
        EXPECT_EQ(predicted.keys, 0x05u);
        EXPECT_NE(predicted.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE, 0);
        remote.inputs[130] = keys_frame(0x09);
        auto exact = steps::live_drive(remote, 130);
        EXPECT_EQ(exact.keys, 0x09u);
        EXPECT_EQ(exact.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE, 0) << "the relayed frame's navigation was off";
    }

    // The session's end takes every mirror's navigation and body out after
    // the own body guard, with the event log as it is (not muted).
    TEST(SessionClientCoreLifecycle, EndTakesTheMirrorsOut) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        auto& four = state_of(core).remotes_[4];
        four.entity = "Ball_0_BMMO_4";
        four.physicalized = true;
        four.navigation = true;
        auto& five = state_of(core).remotes_[5];
        five.entity = "Ball_0_BMMO_5";
        bmmo::session_end_msg ours;
        ours.session = 9;
        const size_t from = fake.calls.size();
        core.on_session_end(ours);
        const std::vector<std::string> expected = {
            "log:0:session 9 ended: ", "hook:ending", "hook:ended",
            "detach_mirror_navigation:Ball_0_BMMO_4", "unmake_mirror:Ball_0_BMMO_4", "hook:status"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_TRUE(state_of(core).remotes_.empty());
    }

    // ------------------------------------------------------ the own lives
    // Step E7: the own births reported (from the event log, or queued from
    // a host's hooks and committed after the drain's bump), the beam, and
    // the echoes of our own events - each verdict with and without
    // own_body_control, and with the own_echoes switch off (design 5.1 "Own
    // echoes", "Own reports from hooks", "Beam").

    std::string hex_pos(float x, float y, float z) {
        char text[160];
        std::snprintf(text, sizeof(text), "%a,%a,%a", static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
        return text;
    }

    // A platform whose world the engine records and rewinds (no tracked
    // body, every step taken), with the core's pending births and own lives
    // drained before each replayed tick, as the host's binding does
    // (before_replayed_tick).
    session_platform replay_platform(flow_fake& fake, session_client_core*& core) {
        session_platform platform = fake.make_flow();
        platform.bind_world = [&fake, &core](const bmmo::session::core_world_hooks&) {
            fake.calls.push_back("bind_world");
            bmmo::session::rollback_world world;
            world.get_body = [](const std::string&, bmmo_physics_body_state&) { return false; };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            world.step = [&fake]() {
                fake.calls.push_back("step");
                return true;
            };
            world.pre_step = [&fake, &core](uint32_t tick) {
                fake.calls.push_back("pre_step:" + std::to_string(tick));
                if (core) steps{*core}.drain_pending_births(tick);
            };
            return world;
        };
        return platform;
    }

    // Records ticks `from`..`to` of the bound world.
    void record_ticks(session_client_core& core, uint32_t from, uint32_t to) {
        for (uint32_t tick = from; tick <= to; ++tick)
            state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, bmmo::session::rollback_tracked{}, {});
    }

    // An own birth of Ball_Wood stamped `origin`, as record_own_birth keeps
    // it (the scripts made it live).
    client_core_state::own_life own_birth(uint32_t origin, uint32_t generation = 0) {
        client_core_state::own_life life;
        life.origin = life.applied = origin;
        life.entity = "Ball_Wood";
        life.generation = generation;
        life.position[0] = 5.0f;
        life.rotation[0] = life.rotation[4] = life.rotation[8] = 1.0f;
        life.recipe.mass = 1.5f;
        life.group = "P#2";
        life.origin_live = life.applied_live = true;
        return life;
    }

    bmmo::session_event_msg own_echo(uint32_t applied, uint32_t origin, uint8_t state) {
        auto msg = event_message(3, bmmo::session::event_type::Physicalize, applied, origin);
        msg.state = state;
        return msg;
    }

    // From the event log: the host's capture and recipe, sent, recorded with
    // the generation in force, and logged; without a capture the pose is the
    // entity's and never a spawn.  The Unphysicalize is sent and logged.
    // The same with and without own body control (design 5.1: the reports
    // have no branch on it).
    void reports_from_the_event_log(bool control) {
        SCOPED_TRACE(control ? "own body control" : "no own body control");
        flow_fake fake;
        session_capabilities caps;
        caps.own_body_control = control;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        state_of(core).generations_["Ball_Wood"] = 4;
        fake.capture.captured = true;
        fake.capture.spawn = true;
        fake.capture.position[0] = 1.5f;
        fake.capture.position[1] = 2.0f;
        fake.capture.position[2] = -3.25f;
        size_t from = fake.calls.size();
        size_t sent_from = fake.sent.size();
        steps{core}.report_physicalize("Ball_Wood", 126);
        {
            const std::vector<std::string> expected = {
                "own_birth_capture:Ball_Wood:126", "ball_recipe_of:Ball_Wood", "send:1",
                "log:0:own ball Ball_Wood physicalized at tick 126 (type 1, spawn) pos=" + hex_pos(1.5f, 2.0f, -3.25f)};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(fake.sent.size(), sent_from + 1);
        const auto event = decode<bmmo::session_event_msg>(fake.sent.back().first);
        EXPECT_EQ(event.type, bmmo::session::event_type::Physicalize);
        EXPECT_EQ(event.tick, 126u);
        EXPECT_EQ(event.origin_tick, 126u);
        EXPECT_EQ(event.ball_type, 1u);
        EXPECT_EQ(event.flags, bmmo::session::PHYSICALIZE_FLAG_SPAWN);
        EXPECT_EQ(event.recipe.mass, 1.5f);
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        const auto& life = state_of(core).own_life_[0];
        EXPECT_EQ(life.what, client_core_state::own_life::kind::birth);
        EXPECT_EQ(life.origin, 126u);
        EXPECT_EQ(life.entity, "Ball_Wood");
        EXPECT_EQ(life.generation, 4u);
        EXPECT_EQ(life.group, "P#2");
        EXPECT_TRUE(life.spawn);
        EXPECT_TRUE(life.origin_live && life.applied_live);

        // No capture: the warning, the pose as the host has it, no spawn.
        fake.capture.captured = false;
        fake.ball_type = -1;
        from = fake.calls.size();
        steps{core}.report_physicalize("Ball_Wood", 130);
        {
            const std::vector<std::string> expected = {
                "own_birth_capture:Ball_Wood:130", "ball_recipe_of:Ball_Wood",
                "log:0:warning: physicalize pose taken from the entity after the tick (no capture before the step)",
                "send:1",
                "log:0:own ball Ball_Wood physicalized at tick 130 (type -1) pos=" + hex_pos(1.5f, 2.0f, -3.25f)};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        const auto unflagged = decode<bmmo::session_event_msg>(fake.sent.back().first);
        EXPECT_EQ(unflagged.flags, 0u);
        EXPECT_EQ(unflagged.ball_type, 0u);

        from = fake.calls.size();
        steps{core}.report_unphysicalize(131);
        {
            const std::vector<std::string> expected = {"send:1", "log:0:own ball unphysicalized at tick 131"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(decode<bmmo::session_event_msg>(fake.sent.back().first).type, bmmo::session::event_type::Unphysicalize);

        // Without rollback nothing is recorded.
        core.config.rollback = false;
        steps{core}.report_physicalize("Ball_Wood", 132);
        EXPECT_EQ(state_of(core).own_life_.size(), 2u);
    }

    TEST(SessionClientCoreOwnLives, ReportsFromTheEventLog) {
        reports_from_the_event_log(false);
        reports_from_the_event_log(true);
    }

    // Design 5.1 "Own reports from hooks": queued in the behaviour pass
    // (nothing sent), committed in order at step 19 with the generation
    // the drain bumped in between; with and without own body control.
    void reports_from_hooks(bool control) {
        SCOPED_TRACE(control ? "own body control" : "no own body control");
        flow_fake fake;
        session_capabilities caps;
        caps.own_lifecycle = bmmo::session::own_lifecycle_source::host_hooks;
        caps.own_body_control = control;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        bmmo::session::own_birth_report report;
        report.entity = "Ball_Wood";
        report.ball_type = 2;
        report.tick = 126;
        report.pose.captured = true;
        report.pose.position[1] = 7.0f;
        report.recipe.mass = 2.5f;
        size_t from = fake.calls.size();
        core.own_physicalized(report);
        core.own_unphysicalized(127);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "queued, not sent";
        EXPECT_EQ(state_of(core).own_reports_.size(), 2u);
        EXPECT_TRUE(state_of(core).own_life_.empty());
        // The drain of the frame that made the body bumps its generation.
        ++state_of(core).generations_["Ball_Wood"];
        steps{core}.commit_own_reports();
        {
            const std::vector<std::string> expected = {
                "send:1", "log:0:own ball Ball_Wood physicalized at tick 126 (type 2) pos=" + hex_pos(0.0f, 7.0f, 0.0f),
                "send:1", "log:0:own ball unphysicalized at tick 127"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).own_reports_.empty());
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        EXPECT_EQ(state_of(core).own_life_[0].generation, 1u) << "the generation after the drain's bump";
        EXPECT_EQ(state_of(core).own_life_[0].recipe.mass, 2.5f);
        EXPECT_EQ(state_of(core).own_life_[0].ball_type, 2u);
    }

    TEST(SessionClientCoreOwnLives, ReportsFromHooksCommitAfterTheDrainsBump) {
        reports_from_hooks(false);
        reports_from_hooks(true);
    }

    // Design 5.1 "Beam", with own body control: requested before the frame
    // that stepped 125 ends, stamped 126, the pair sent, the body rebuilt
    // live before the step of 126 (muted).
    TEST(SessionClientCoreOwnLives, BeamRebuiltAtItsStampWithOwnBodyControl) {
        flow_fake fake;
        session_capabilities caps;
        caps.own_body_control = true;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        state_of(core).own_physicalized_ = true;
        fake.driven = "Ball_Wood";
        const double position[3] = {711.0, 4.0, 43.0};
        size_t from = fake.calls.size();
        core.request_beam(position);
        EXPECT_TRUE(since(fake.calls, from).empty());
        steps{core}.apply_requested_beam(125);
        {
            const std::vector<std::string> expected = {
                "own_ball", "own_driven_entity", "ball_recipe_of:Ball_Wood", "send:1", "send:1",
                "log:0:--beam: Ball_Wood to (711.000,4.000,43.000), reported to the server for tick 126 and rebuilt here "
                "at that tick"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        EXPECT_EQ(state_of(core).own_life_[0].origin, 126u);
        EXPECT_EQ(state_of(core).own_life_[0].generation, 0u) << "the same lifetime on every side";
        EXPECT_EQ(state_of(core).generations_.count("Ball_Wood"), 0u);
        from = fake.calls.size();
        steps{core}.apply_requested_beam(126);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "made once";
        steps{core}.apply_held_lifecycle(126);
        {
            const std::vector<std::string> expected = {
                "mute_event_log:on", "make_own:Ball_Wood:P#2:126", "mute_event_log:off",
                "log:0:own body: Ball_Wood made at (711.000,4.000,43.000) before tick 126 (live, beam)",
                "body_state:Ball_Wood"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(fake.own_specs.size(), 1u);
        EXPECT_EQ(fake.own_specs[0].position[0], 711.0f);
        EXPECT_EQ(fake.own_specs[0].recipe.mass, 1.5f);
        EXPECT_FALSE(fake.own_specs[0].kick);

        // No own body: said, nothing sent.
        state_of(core).own_physicalized_ = false;
        core.request_beam(position);
        from = fake.calls.size();
        steps{core}.apply_requested_beam(127);
        {
            const std::vector<std::string> expected = {"own_ball", "log:0:--beam at tick 127: no own body to beam"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }

        // No navigation replica (headless --beam --no-rollback): moved in
        // place at once with the control too (write_beam), a new lifetime,
        // nothing kept; a failed write says why and sends nothing.
        state_of(core).own_physicalized_ = true;
        fake.driven.clear();
        const size_t kept = state_of(core).own_life_.size();
        core.request_beam(position);
        from = fake.calls.size();
        steps{core}.apply_requested_beam(127);
        {
            const std::vector<std::string> expected = {
                "own_ball", "own_driven_entity", "write_body:Ball_Wood:711.000000:wake",
                "ball_recipe_of:Ball_Wood", "send:1", "send:1",
                "log:0:--beam: Ball_Wood to (711.000,4.000,43.000), reported to the server for tick 128 and moved here "
                "now (no navigation replica)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).generations_.at("Ball_Wood"), 1u);
        EXPECT_EQ(state_of(core).own_life_.size(), kept) << "nothing kept";
        fake.write_ok = false;
        core.request_beam(position);
        from = fake.calls.size();
        steps{core}.apply_requested_beam(128);
        {
            const std::vector<std::string> expected = {
                "own_ball", "own_driven_entity", "write_body:Ball_Wood:711.000000:wake",
                "log:0:--beam at tick 128: no body"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).generations_.at("Ball_Wood"), 1u) << "not bumped";
    }

    // Design 5.1 "Beam", without own body control: the generation bumped at
    // the request, the record kept, and its stamp one write_body live at
    // T+1 and one in a forced replay that crosses T+1.  Without a
    // navigation replica the ball is moved at once and nothing is kept.
    TEST(SessionClientCoreOwnLives, BeamWrittenInPlaceWithoutOwnBodyControl) {
        flow_fake fake;
        session_client_core* bound = nullptr;
        session_client_core core(replay_platform(fake, bound), session_capabilities{});
        bound = &core;
        run_to_tick_125(core, fake);
        record_ticks(core, 118, 125);
        state_of(core).own_physicalized_ = true;
        fake.driven = "Ball_Wood";
        const double position[3] = {711.0, 4.0, 43.0};
        core.request_beam(position);
        size_t from = fake.calls.size();
        steps{core}.apply_requested_beam(125);
        {
            const std::vector<std::string> expected = {
                "own_ball", "own_driven_entity", "ball_recipe_of:Ball_Wood", "send:1", "send:1",
                "log:0:--beam: Ball_Wood to (711.000,4.000,43.000), reported to the server for tick 126 and moved here "
                "at that tick (no own body control)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).generations_.at("Ball_Wood"), 1u) << "bumped at the request";
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        EXPECT_EQ(state_of(core).own_life_[0].generation, 1u);
        from = fake.calls.size();
        steps{core}.apply_held_lifecycle(126);
        {
            const std::vector<std::string> expected = {"write_body:Ball_Wood:711.000000:wake", "body_state:Ball_Wood"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(state_of(core).own_life_.size(), 1u) << "kept while a rewind can cross it";
        state_of(core).frames_since_anchor_ = 7;   // the frame of 126 ran
        record_ticks(core, 126, 126);
        from = fake.calls.size();
        {
            const auto forced = core.force_rewind(3);
            EXPECT_TRUE(forced.rolled);
        }
        {
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:126", "bind_world",
                "pre_step:124", "step", "pre_step:125", "step",
                "pre_step:126", "write_body:Ball_Wood:711.000000:wake", "step",
                "keep_live_cells:exit:rolled back"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }

        // No navigation replica: moved at once, nothing kept.
        state_of(core).own_life_.clear();
        fake.driven.clear();
        core.request_beam(position);
        from = fake.calls.size();
        steps{core}.apply_requested_beam(126);
        {
            const std::vector<std::string> expected = {
                "own_ball", "own_driven_entity", "write_body:Ball_Wood:711.000000:wake",
                "ball_recipe_of:Ball_Wood", "send:1", "send:1",
                "log:0:--beam: Ball_Wood to (711.000,4.000,43.000), reported to the server for tick 127 and moved here "
                "now (no navigation replica)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).generations_.at("Ball_Wood"), 2u);
        EXPECT_TRUE(state_of(core).own_life_.empty());
    }

    // Design 5.1 "Own echoes": a MOVED birth, with own body control, is
    // taken out, moved in the records and rewound for, and made again at
    // its applied tick inside the replay; the restore's destroy_body takes
    // a moved birth out (the live cells kept until the move's last line, as
    // the headless client kept them).  Without the control: nothing when snapshots run,
    // a resync when they do not, and the restore's branch is inert.
    TEST(SessionClientCoreOwnLives, MovedEcho) {
        {
            flow_fake fake;
            session_client_core* bound = nullptr;
            session_capabilities caps;
            caps.own_body_control = true;
            session_client_core core(replay_platform(fake, bound), caps);
            bound = &core;
            run_to_tick_125(core, fake);
            record_ticks(core, 118, 125);
            state_of(core).own_life_ = {own_birth(122)};
            fake.bodies_there = {"Ball_Wood"};
            core.on_session_event(own_echo(124, 122, bmmo::session::EVENT_STATE_MOVED));
            const size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            const std::vector<std::string> expected = {
                "body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125", "mute_event_log:off",
                "keep_live_cells:enter:125", "bind_world",
                "pre_step:122", "step", "pre_step:123", "step",
                "pre_step:124", "mute_event_log:on", "make_own:Ball_Wood:P#2:124", "mute_event_log:off",
                "log:0:own body: Ball_Wood made at (5.000,0.000,0.000) before tick 124 (replay, moved birth)", "step",
                "pre_step:125", "step",
                "log:0:own physicalize stamped 122 moved to 124 by the server at local tick 125: rewound to 121 (0 records "
                "changed, made again in the replay)",
                "keep_live_cells:exit:rolled back"};
            EXPECT_EQ(since(fake.calls, from), expected);
            EXPECT_EQ(state_of(core).own_echo_moved_, 1u);
            EXPECT_EQ(state_of(core).own_birth_echo_moved_, 1u);
            EXPECT_EQ(state_of(core).own_births_moved_, 1u);
            EXPECT_EQ(state_of(core).own_remakes_, 1u);
            EXPECT_EQ(state_of(core).life_rewinds_, 1u);
            ASSERT_EQ(state_of(core).own_life_.size(), 1u);
            EXPECT_EQ(state_of(core).own_life_[0].state, client_core_state::own_life::verdict::moved);
            EXPECT_EQ(state_of(core).own_life_[0].applied, 124u);
            // The restore of a later rewind across 124 takes the body out.
            bool destroyed = false;
            const size_t restore = fake.calls.size();
            EXPECT_TRUE(steps{core}.destroy_moved_own_birth("Ball_Wood", 124, destroyed));
            EXPECT_TRUE(destroyed);
            const std::vector<std::string> out = {"mute_event_log:on", "unmake_own:Ball_Wood:125", "mute_event_log:off"};
            EXPECT_EQ(since(fake.calls, restore), out);
            EXPECT_FALSE(steps{core}.destroy_moved_own_birth("Ball_Wood", 123, destroyed));
        }
        for (const uint8_t interval: {uint8_t{2}, uint8_t{255}}) {
            flow_fake fake;
            session_client_core core(fake.make_flow(), session_capabilities{});
            run_to_tick_125(core, fake);
            state_of(core).snapshot_interval_ = interval;
            state_of(core).own_life_ = {own_birth(122)};
            fake.bodies_there = {"Ball_Wood"};
            core.on_session_event(own_echo(124, 122, bmmo::session::EVENT_STATE_MOVED));
            const size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            if (interval == 2) {
                EXPECT_TRUE(since(fake.calls, from).empty()) << "snapshots run: left to them";
                EXPECT_FALSE(state_of(core).own_life_[0].retired);
            } else {
                const std::vector<std::string> expected = {
                    "log:0:own birth moved 122 -> 124 at local tick 125: no own body control, no snapshots, resync",
                    "now:resync_limiter", "send:0", "log:0:resync requested (own birth: resync) at tick 125"};
                EXPECT_EQ(since(fake.calls, from), expected);
                EXPECT_EQ(state_of(core).own_moves_resynced_, 1u);
            }
            EXPECT_EQ(state_of(core).own_echo_moved_, 1u);
            EXPECT_EQ(state_of(core).own_life_[0].state, client_core_state::own_life::verdict::moved);
            bool destroyed = false;
            state_of(core).own_life_[0].applied = 124;   // as if moved: still inert
            EXPECT_FALSE(steps{core}.destroy_moved_own_birth("Ball_Wood", 124, destroyed));
        }
    }

    // Design 5.1 "Own echoes": a REJECTED birth.  With own body control it
    // is removed from the history (the body out, a rewind); without, it is
    // re-reported at the next tick without the spawn flag while its life is
    // current, and the client resyncs; a life that ended since gets the
    // resync only.
    TEST(SessionClientCoreOwnLives, RejectedEcho) {
        {
            flow_fake fake;
            session_client_core* bound = nullptr;
            session_capabilities caps;
            caps.own_body_control = true;
            session_client_core core(replay_platform(fake, bound), caps);
            bound = &core;
            run_to_tick_125(core, fake);
            record_ticks(core, 118, 125);
            state_of(core).own_life_ = {own_birth(122)};
            state_of(core).own_physicalized_ = true;
            fake.bodies_there = {"Ball_Wood"};
            core.on_session_event(own_echo(122, 122, bmmo::session::EVENT_STATE_REJECTED));
            const size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            const std::vector<std::string> expected = {
                "body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125", "mute_event_log:off",
                "keep_live_cells:enter:125", "bind_world",
                "pre_step:122", "step", "pre_step:123", "step", "pre_step:124", "step", "pre_step:125", "step",
                "log:0:own physicalize stamped 122 never made by the server at local tick 125: rewound to 121 (0 records "
                "changed, removed from the history)",
                "keep_live_cells:exit:rolled back"};
            EXPECT_EQ(since(fake.calls, from), expected);
            EXPECT_EQ(state_of(core).own_echo_rejected_, 1u);
            EXPECT_EQ(state_of(core).own_births_removed_, 1u);
            EXPECT_FALSE(state_of(core).own_physicalized_);
            EXPECT_TRUE(state_of(core).own_life_.empty() || state_of(core).own_life_[0].retired);
        }
        for (const bool current: {true, false}) {
            flow_fake fake;
            session_client_core core(fake.make_flow(), session_capabilities{});
            run_to_tick_125(core, fake);
            state_of(core).own_life_ = {own_birth(122)};
            state_of(core).own_life_[0].spawn = true;
            state_of(core).own_physicalized_ = true;
            if (!current) state_of(core).generations_["Ball_Wood"] = 1;   // a death since
            fake.bodies_there = {"Ball_Wood"};
            core.on_session_event(own_echo(122, 122, bmmo::session::EVENT_STATE_REJECTED));
            const size_t from = fake.calls.size();
            const size_t sent_from = fake.sent.size();
            steps{core}.process_own_echoes();
            std::vector<std::string> expected;
            if (current) expected.push_back("send:1");
            for (const char* line: {"log:0:own birth moved 122 -> never at local tick 125: no own body control, resync",
                                    "now:resync_limiter", "send:0",
                                    "log:0:resync requested (own birth: resync) at tick 125"})
                expected.emplace_back(line);
            EXPECT_EQ(since(fake.calls, from), expected) << current;
            if (current) {
                const auto again = decode<bmmo::session_event_msg>(fake.sent[sent_from].first);
                EXPECT_EQ(again.type, bmmo::session::event_type::Physicalize);
                EXPECT_EQ(again.tick, 126u);
                EXPECT_EQ(again.flags, 0u) << "no spawn flag";
                EXPECT_EQ(again.position[0], 5.0f);
            }
            EXPECT_TRUE(state_of(core).own_physicalized_) << "the body is never touched";
            EXPECT_TRUE(state_of(core).own_life_[0].retired);
        }
    }

    // Design 5.1 "Own echoes": a SUPERSEDED birth.  With own body control
    // it is removed from the history as a rejected one is (the body out,
    // a rewind); a pair whose Unphysicalize the server moved resyncs
    // instead (the body out first: the server never made it).  Without
    // the control it gets nothing - no re-report, no resync: a newer
    // accepted event of ours overtook it (design 2.3) - the pair included.
    TEST(SessionClientCoreOwnLives, SupersededEcho) {
        for (const bool control: {true, false}) {
            for (const bool pair: {false, true}) {
                SCOPED_TRACE(std::string(control ? "control" : "no control") + (pair ? ", pair" : ""));
                flow_fake fake;
                session_client_core* bound = nullptr;
                session_capabilities caps;
                caps.own_body_control = control;
                session_client_core core(replay_platform(fake, bound), caps);
                bound = &core;
                run_to_tick_125(core, fake);
                record_ticks(core, 118, 125);
                state_of(core).own_life_ = {own_birth(122)};
                state_of(core).own_physicalized_ = true;
                fake.bodies_there = {"Ball_Wood"};
                if (pair) state_of(core).own_moved_unphysicalize_ = {122};
                core.on_session_event(own_echo(122, 122, bmmo::session::EVENT_STATE_SUPERSEDED));
                const size_t from = fake.calls.size();
                const size_t sent_from = fake.sent.size();
                steps{core}.process_own_echoes();
                std::vector<std::string> expected;
                if (control && !pair) {
                    expected = {
                        "body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125", "mute_event_log:off",
                        "keep_live_cells:enter:125", "bind_world",
                        "pre_step:122", "step", "pre_step:123", "step", "pre_step:124", "step", "pre_step:125", "step",
                        "log:0:own physicalize stamped 122 never made by the server at local tick 125: rewound to 121 (0 "
                        "records changed, removed from the history)",
                        "keep_live_cells:exit:rolled back"};
                } else if (control) {
                    expected = {
                        "body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125", "mute_event_log:off",
                        "log:0:own birth moved 122 -> never at local tick 125: its Unphysicalize was moved too (an own "
                        "pair, plan step 22), resync",
                        "now:resync_limiter", "send:0", "log:0:resync requested (own birth: resync) at tick 125"};
                }
                EXPECT_EQ(since(fake.calls, from), expected);
                EXPECT_EQ(state_of(core).own_echo_superseded_, 1u);
                ASSERT_EQ(state_of(core).own_life_.size(), 1u);
                EXPECT_EQ(state_of(core).own_life_[0].state, client_core_state::own_life::verdict::superseded);
                if (control) {
                    EXPECT_FALSE(state_of(core).own_physicalized_);
                    EXPECT_TRUE(state_of(core).own_life_[0].retired);
                    EXPECT_EQ(state_of(core).own_births_removed_, pair ? 0u : 1u);
                    EXPECT_EQ(state_of(core).own_moves_resynced_, pair ? 1u : 0u);
                } else {
                    EXPECT_EQ(fake.sent.size(), sent_from) << "no re-report, no resync";
                    EXPECT_TRUE(state_of(core).own_physicalized_) << "the body is never touched";
                    EXPECT_FALSE(state_of(core).own_life_[0].retired);
                    EXPECT_EQ(state_of(core).own_moves_resynced_, 0u);
                }
            }
        }
    }

    // Design 5.1 "Own echoes": an own pair the server applied late as a
    // whole, with own body control: the birth's MOVED echo after its
    // Unphysicalize's, and the Unphysicalize's after a birth already moved
    // inside the history.  Either way a resync (the server's old body lived
    // on until T'), the body left as it is (the server made it too).
    TEST(SessionClientCoreOwnLives, PairResync) {
        for (const bool birth_first: {false, true}) {
            SCOPED_TRACE(birth_first ? "the birth moved first" : "the Unphysicalize first");
            flow_fake fake;
            session_capabilities caps;
            caps.own_body_control = true;
            session_client_core core(fake.make_flow(), caps);
            run_to_tick_125(core, fake);
            state_of(core).own_life_ = {own_birth(122)};
            state_of(core).own_physicalized_ = true;
            fake.bodies_there = {"Ball_Wood"};
            auto unphysicalize = event_message(3, bmmo::session::event_type::Unphysicalize, 124, 122);
            unphysicalize.state = bmmo::session::EVENT_STATE_MOVED;
            if (birth_first) {
                state_of(core).own_life_[0].state = client_core_state::own_life::verdict::moved;
                state_of(core).own_life_[0].applied = 124;
                core.on_session_event(std::move(unphysicalize));
            } else {
                core.on_session_event(std::move(unphysicalize));
                core.on_session_event(own_echo(124, 122, bmmo::session::EVENT_STATE_MOVED));
            }
            const size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            const std::vector<std::string> expected = {
                "log:0:own unphysicalize stamped tick 122 echoed moved by the server (applied 124, state 01) at local "
                "tick 125; kept where we stamped it (only an own Physicalize is moved here)",
                "body_state:Ball_Wood",
                "log:0:own birth moved 122 -> 124 at local tick 125: its Unphysicalize was moved too (an own pair, "
                "plan step 22), resync",
                "now:resync_limiter", "send:0", "log:0:resync requested (own birth: resync) at tick 125"};
            EXPECT_EQ(since(fake.calls, from), expected);
            EXPECT_EQ(state_of(core).own_moves_resynced_, 1u);
            EXPECT_EQ(state_of(core).own_moves_other_, 1u);
            EXPECT_TRUE(state_of(core).own_life_[0].retired);
            EXPECT_TRUE(state_of(core).own_physicalized_);
            EXPECT_EQ(state_of(core).own_moved_unphysicalize_, std::vector<uint32_t>{122});
        }
    }

    // Design 5.1 "Own echoes": a moved birth the history cannot carry, with
    // own body control - no record of the tick before it (past the reach),
    // a life that ended since (plan step 22), and an echo with no record at
    // all (past the history): a resync each, nothing rewound.
    TEST(SessionClientCoreOwnLives, MoveRefusedResyncs) {
        for (int which = 0; which < 3; ++which) {
            SCOPED_TRACE(which);
            flow_fake fake;
            session_client_core* bound = nullptr;
            session_capabilities caps;
            caps.own_body_control = true;
            session_client_core core(replay_platform(fake, bound), caps);
            bound = &core;
            run_to_tick_125(core, fake);
            if (which != 0) record_ticks(core, 118, 125);
            if (which != 2) state_of(core).own_life_ = {own_birth(122)};
            if (which == 1) state_of(core).generations_["Ball_Wood"] = 1;   // a death since
            state_of(core).own_physicalized_ = true;
            fake.bodies_there = {"Ball_Wood"};
            core.on_session_event(own_echo(124, 122, bmmo::session::EVENT_STATE_MOVED));
            const size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            std::vector<std::string> expected;
            if (which == 0) {
                expected = {"body_state:Ball_Wood",
                            "log:0:own birth moved 122 -> 124 at local tick 125: past the reach (depth 4), resync",
                            "now:resync_limiter", "send:0", "log:0:resync requested (own birth: resync) at tick 125"};
            } else if (which == 1) {
                expected = {"log:0:own birth moved 122 -> 124 at local tick 125: its life has ended since (plan step "
                            "22), resync",
                            "now:resync_limiter", "send:0", "log:0:resync requested (own birth: resync) at tick 125"};
            } else {
                expected = {"log:0:own birth moved 122 -> 124 at local tick 125: no record of it (past the history), "
                            "resync",
                            "now:resync_limiter", "send:0",
                            "log:0:resync requested (own birth moved past the history) at tick 125"};
            }
            EXPECT_EQ(since(fake.calls, from), expected);
            EXPECT_EQ(state_of(core).own_moves_resynced_, 1u);
            EXPECT_EQ(state_of(core).life_rewinds_, 0u);
            EXPECT_EQ(state_of(core).own_births_moved_, 0u);
            EXPECT_TRUE(state_of(core).own_physicalized_);
            if (which != 2) EXPECT_TRUE(state_of(core).own_life_[0].retired);
        }
    }

    // Design 5.1 "Own echoes": the answer to a birth set aside, with and
    // without own body control, through the echoes queued before the last
    // SessionAssign (stale: no record of the new numbering is looked up).
    // Given up as missing and made after all: made again (with the
    // control) and a resync.  Sent before a resync and never made: the
    // body out (with the control; re-reported without it) and a resync.
    // The other two need nothing, and a stale echo with nothing set aside
    // answers nothing.
    TEST(SessionClientCoreOwnLives, SetAsideAnswered) {
        using own_life = client_core_state::own_life;
        for (const bool control: {true, false}) {
            for (int which = 0; which < 5; ++which) {
                SCOPED_TRACE(std::string(control ? "control " : "no control ") + std::to_string(which));
                flow_fake fake;
                session_capabilities caps;
                caps.own_body_control = control;
                session_client_core core(fake.make_flow(), caps);
                run_to_tick_125(core, fake);
                // A record of the new numbering with the same origin: never
                // answered by a stale echo.
                state_of(core).own_life_ = {own_birth(122)};
                state_of(core).own_physicalized_ = true;
                const bool missing = which == 0 || which == 1;
                const bool made = which == 0 || which == 2;
                if (which < 4) {
                    own_life aside = own_birth(122);
                    aside.why = missing ? own_life::aside::missing : own_life::aside::resync;
                    aside.state = missing ? own_life::verdict::missing : own_life::verdict::sent;
                    state_of(core).own_unanswered_ = {aside};
                }
                fake.bodies_there = (which == 0) ? std::vector<std::string>{} : std::vector<std::string>{"Ball_Wood"};
                state_of(core).own_echoes_stale_.push_back(
                    own_echo(made || which == 4 ? 124 : 122, 122,
                             made || which == 4 ? bmmo::session::EVENT_STATE_MOVED : bmmo::session::EVENT_STATE_REJECTED));
                const size_t from = fake.calls.size();
                const size_t sent_from = fake.sent.size();
                steps{core}.process_own_echoes();
                std::vector<std::string> expected;
                if (which == 0) {
                    if (control)
                        expected = {"body_state:Ball_Wood", "mute_event_log:on", "make_own:Ball_Wood:P#2:125",
                                    "mute_event_log:off",
                                    "log:0:own body: Ball_Wood made at (5.000,0.000,0.000) before tick 125 (resync, moved "
                                    "birth)"};
                    for (const char* line: {"log:0:own birth moved 122 -> 124 at local tick 125: made after its echo was "
                                            "given up as missing, resync",
                                            "now:resync_limiter", "send:0",
                                            "log:0:resync requested (own birth: resync) at tick 125"})
                        expected.emplace_back(line);
                } else if (which == 1) {
                    expected = {"log:0:own physicalize stamped 122 echoed rejected by the server (applied 122) at local "
                                "tick 125: taken out already as missing, nothing to do"};
                } else if (which == 2) {
                    expected = {"log:0:own physicalize stamped 122 echoed moved by the server (applied 124) at local "
                                "tick 125: sent before the last resync, nothing to do"};
                } else if (which == 3) {
                    if (control)
                        expected = {"body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125",
                                    "mute_event_log:off"};
                    else
                        expected = {"send:1"};
                    for (const char* line: {"log:0:own birth moved 122 -> never at local tick 125: sent before the last "
                                            "resync and never made, resync",
                                            "now:resync_limiter", "send:0",
                                            "log:0:resync requested (own birth: resync) at tick 125"})
                        expected.emplace_back(line);
                } else {
                    expected = {"log:0:own physicalize stamped 122 echoed moved by the server (applied 124) at local "
                                "tick 125: no record of it, sent before the last resync: nothing to do"};
                }
                EXPECT_EQ(since(fake.calls, from), expected);
                EXPECT_TRUE(state_of(core).own_unanswered_.empty());
                EXPECT_TRUE(state_of(core).own_echoes_stale_.empty());
                EXPECT_EQ(state_of(core).own_life_[0].state, own_life::verdict::sent) << "the new numbering's record untouched";
                EXPECT_FALSE(state_of(core).own_life_[0].retired);
                EXPECT_EQ(state_of(core).own_remakes_, (control && which == 0) ? 1u : 0u);
                EXPECT_EQ(state_of(core).own_physicalized_, !(control && which == 3));
                if (!control && which == 3) {
                    const auto again = decode<bmmo::session_event_msg>(fake.sent[sent_from].first);
                    EXPECT_EQ(again.tick, 126u);
                    EXPECT_EQ(again.flags, 0u);
                }
            }
        }
    }

    // The live frame's own lives (live_own_lives), with own body control:
    // a moved birth whose ball has had another life since retires at its
    // applied tick without a remake; one whose applied tick passed with no
    // live frame (a resync renumbered past it) is dropped.
    TEST(SessionClientCoreOwnLives, LiveFrameRetiresAndDrops) {
        flow_fake fake;
        session_capabilities caps;
        caps.own_body_control = true;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        auto ended = own_birth(122);
        ended.state = client_core_state::own_life::verdict::moved;
        ended.applied = 126;
        ended.applied_live = false;
        auto skipped = own_birth(118);
        skipped.entity = "Ball_Stone";
        skipped.state = client_core_state::own_life::verdict::moved;
        skipped.applied = 124;
        skipped.applied_live = false;
        state_of(core).own_life_ = {ended, skipped};
        state_of(core).generations_["Ball_Wood"] = 1;   // a death since
        const size_t from = fake.calls.size();
        steps{core}.apply_held_lifecycle(126);
        const std::vector<std::string> expected = {
            "log:0:own birth stamped 118 (applied 124) never met its live frame (now 126): dropped",
            "log:0:own birth stamped 122 retired at tick 126 (live): Ball_Wood has had another life since (generation "
            "0 -> 1)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_TRUE(fake.own_specs.empty()) << "nothing made";
        EXPECT_EQ(state_of(core).own_remakes_, 0u);
        EXPECT_TRUE(state_of(core).own_life_.empty()) << "both retired";
    }

    // Design 5.1 "Own echoes": a MISSING birth (no echo past the deadline,
    // the uplink gate passed).  With own body control the body goes and the
    // client resyncs; without it the birth is re-reported and the client
    // resyncs.  Either way it is set aside for a late echo.
    TEST(SessionClientCoreOwnLives, MissingEcho) {
        for (const bool control: {true, false}) {
            flow_fake fake;
            session_capabilities caps;
            caps.own_body_control = control;
            session_client_core core(fake.make_flow(), caps);
            run_to_tick_125(core, fake);
            state_of(core).own_life_ = {own_birth(122)};
            state_of(core).own_physicalized_ = true;
            fake.bodies_there = {"Ball_Wood"};
            state_of(core).acked_header_known_ = true;
            state_of(core).header_acked_ = 122;
            state_of(core).acked_header_tick_ = 123;
            size_t from = fake.calls.size();
            steps{core}.process_own_echoes();
            EXPECT_TRUE(since(fake.calls, from).empty()) << "the uplink seen, the deadline ahead";
            state_of(core).acked_header_tick_ = 422;
            from = fake.calls.size();
            const size_t sent_from = fake.sent.size();
            steps{core}.process_own_echoes();
            std::vector<std::string> expected;
            if (control) {
                for (const char* line: {"body_state:Ball_Wood", "mute_event_log:on", "unmake_own:Ball_Wood:125",
                                        "mute_event_log:off"})
                    expected.emplace_back(line);
            } else {
                expected.emplace_back("send:1");
            }
            for (const char* line: {"log:0:own birth moved 122 -> never at local tick 125: no echo by the server's tick "
                                    "422, resync",
                                    "now:resync_limiter", "send:0",
                                    "log:0:resync requested (own birth: resync) at tick 125"})
                expected.emplace_back(line);
            EXPECT_EQ(since(fake.calls, from), expected) << control;
            EXPECT_EQ(state_of(core).own_echo_missing_, 1u);
            EXPECT_EQ(state_of(core).own_physicalized_, !control);
            ASSERT_EQ(state_of(core).own_unanswered_.size(), 1u);
            EXPECT_EQ(state_of(core).own_unanswered_[0].why, client_core_state::own_life::aside::missing);
            if (!control) EXPECT_EQ(decode<bmmo::session_event_msg>(fake.sent[sent_from].first).tick, 126u);
        }
    }

    // The own_echoes switch off (design 1.2): an echo is counted and logged
    // as kept where we stamped it, no record is touched, no birth expires;
    // with and without own body control (retail R1's setting is the second).
    void own_echoes_off_counts_only(bool control) {
        SCOPED_TRACE(control ? "own body control" : "no own body control");
        flow_fake fake;
        session_capabilities caps;
        caps.own_body_control = control;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        core.config.own_echoes = false;
        state_of(core).own_life_ = {own_birth(122), own_birth(110)};
        fake.bodies_there = {"Ball_Wood"};
        state_of(core).acked_header_known_ = true;
        state_of(core).header_acked_ = 122;
        state_of(core).acked_header_tick_ = 900;
        auto unphysicalize = event_message(3, bmmo::session::event_type::Unphysicalize, 124, 122);
        unphysicalize.state = bmmo::session::EVENT_STATE_MOVED;
        core.on_session_event(own_echo(124, 122, bmmo::session::EVENT_STATE_MOVED));
        core.on_session_event(std::move(unphysicalize));
        const size_t from = fake.calls.size();
        steps{core}.process_own_echoes();
        const std::vector<std::string> expected = {
            "log:0:own physicalize stamped 122 echoed moved by the server (applied 124) at local tick 125: no record of "
            "it, own echoes off: kept where we stamped it",
            "log:0:own unphysicalize stamped tick 122 echoed moved by the server (applied 124, state 01) at local tick "
            "125; kept where we stamped it (only an own Physicalize is moved here)"};
        EXPECT_EQ(since(fake.calls, from), expected);
        EXPECT_EQ(state_of(core).own_echo_moved_, 2u);
        EXPECT_TRUE(state_of(core).own_moved_unphysicalize_.empty());
        EXPECT_EQ(state_of(core).own_life_[0].state, client_core_state::own_life::verdict::sent);
        EXPECT_EQ(state_of(core).own_echo_missing_, 0u);
        EXPECT_EQ(state_of(core).own_moves_resynced_, 0u);
    }

    TEST(SessionClientCoreOwnLives, OwnEchoesOffCountsOnly) {
        own_echoes_off_counts_only(true);
        own_echoes_off_counts_only(false);
    }

    // ------------------------------------------------------------- sectors (E8)
    bmmo::session::sector_poll cell_poll(int cell, bool held, bool moved = false, bool read = true) {
        bmmo::session::sector_poll poll;
        poll.cell_read = read;
        poll.cell = cell;
        poll.checkpoint_held = held;
        poll.resetpoint_moved = moved;
        return poll;
    }

    // Design 5.1 "Sector request before the assignment": the anchor makes
    // sector 1 ours and takes the report's cell; a request before the
    // assignment only waits (nothing sent, nothing stamped), and the first
    // assigned frame stamps it T + lead with the request's reason.  A
    // request after the assignment is stamped current + 1 + lead at once;
    // each sector once.
    TEST(SessionClientCoreSectors, RequestBeforeTheAssignmentWaitsForTheFirstAssignedFrame) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        state_of(core).own_id_ = 3;
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        state_of(core).sector_cell_ = 9;   // begin's reset is the anchor's to overwrite
        core.level_requested();
        // A request at the anchor (the headless --start-sector) comes before
        // the anchor's SessionReady: not assigned, so pending.
        size_t from = fake.calls.size();
        core.request_sector(3, "--start-sector");
        EXPECT_TRUE(since(fake.calls, from).empty());
        EXPECT_TRUE(state_of(core).start_sector_pending_);
        {
            const std::vector<std::pair<int, std::string>> expected = {{3, "--start-sector"}};
            EXPECT_EQ(state_of(core).start_sectors_, expected);
        }
        // A second request before the assignment (a retail checkpoint, say)
        // waits too, after the first; a repeat is dropped when it is stamped.
        core.request_sector(2, "checkpoint");
        core.request_sector(3, "sector verb");
        EXPECT_TRUE(since(fake.calls, from).empty());
        ASSERT_EQ(state_of(core).start_sectors_.size(), 3u);
        EXPECT_EQ(state_of(core).start_sectors_[1], std::make_pair(2, std::string("checkpoint")));
        bmmo::session::anchor_report report;
        report.build_id = "build-x";
        report.sector_cell = 1;
        core.anchor(report);
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1}));
        EXPECT_EQ(state_of(core).sector_cell_, 1);
        EXPECT_TRUE(state_of(core).start_sector_pending_) << "the anchor keeps the request";
        // Not assigned: the frame (a hold that timed out) stamps nothing.
        from = fake.calls.size();
        steps{core}.hold_pending_sector(5);
        EXPECT_TRUE(since(fake.calls, from).empty());
        EXPECT_TRUE(state_of(core).start_sector_pending_);
        EXPECT_EQ(state_of(core).sectors_.stamps_pending(), 0u);

        core.on_session_assign(assign_message(9, 120));
        from = fake.calls.size();
        const size_t sent_before = fake.sent.size();
        steps{core}.hold_pending_sector(120);   // the first assigned frame steps 120
        {
            const std::vector<std::string> expected = {
                "send:1", "log:0:sector 3 stamped for tick 127 (--start-sector, lead 7, input_delay 4), event sent ahead",
                "send:1", "log:0:sector 2 stamped for tick 127 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(fake.sent.size(), sent_before + 2);
        {
            const auto event = decode<bmmo::session_event_msg>(fake.sent[sent_before].first);
            EXPECT_EQ(event.session, 9u);
            EXPECT_EQ(event.type, bmmo::session::event_type::Sector);
            EXPECT_EQ(event.sector, 3);
            EXPECT_EQ(event.tick, 127u);
            EXPECT_EQ(event.origin_tick, 127u);
            EXPECT_EQ(decode<bmmo::session_event_msg>(fake.sent.back().first).sector, 2);
        }
        EXPECT_FALSE(state_of(core).start_sector_pending_);
        EXPECT_TRUE(state_of(core).start_sectors_.empty());
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1, 2, 3}));
        EXPECT_EQ(state_of(core).sectors_.stamps_pending(), 2u);
        EXPECT_EQ(state_of(core).events_sent_, 2u);
        // Once: the next frame and a repeated request do nothing.
        from = fake.calls.size();
        steps{core}.hold_pending_sector(121);
        core.request_sector(3, "sector verb");
        EXPECT_TRUE(since(fake.calls, from).empty());
        // After the assignment: current (120, nothing stepped yet) + 1 + lead.
        ASSERT_EQ(core.current_tick(), 120u);
        core.request_sector(4, "sector verb");
        {
            const std::vector<std::string> expected = {
                "send:1", "log:0:sector 4 stamped for tick 128 (sector verb, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(decode<bmmo::session_event_msg>(fake.sent.back().first).tick, 128u);
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1, 2, 3, 4}));
        // A new session forgets the request.
        core.end("over");
        core.request_sector(5, "x");
        EXPECT_TRUE(state_of(core).start_sector_pending_);
        ASSERT_FALSE(core.begin(start_message(10, 3), bmmo::session::core_config{}).refused);
        EXPECT_FALSE(state_of(core).start_sector_pending_);
        EXPECT_TRUE(state_of(core).start_sectors_.empty());
        EXPECT_TRUE(state_of(core).sectors_requested_.empty());
    }

    // The union's pass before the step (design 1.3 step 9): the manager's
    // state counted every tick, a pass started for the lowest sector owed
    // once the manager has been idle three ticks, with the cell written,
    // the manager run, the activation recorded and the line logged.  The
    // own poll after the step (step 26) then raises no Sector event for the
    // pass's own write; a crossing is stamped ahead with the chain held,
    // reported after the fact without it, and a moved resetpoint is our own
    // crossing of a sector a peer started.
    TEST(SessionClientCoreSectors, UnionPassAndTheOwnPoll) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        EXPECT_EQ(state_of(core).sector_cell_, 0);
        core.request_sector(3, "sector verb");   // stamped 128
        size_t from = fake.calls.size();
        for (uint32_t tick = 120; tick <= 127; ++tick) steps{core}.sector_union_pass(tick);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>(8, "sector_manager_active:off"));
        from = fake.calls.size();
        steps{core}.sector_union_pass(128);
        {
            const std::vector<std::string> expected = {
                "sector_manager_active:off", "run_sector:3", "record_activation:128:reset",
                "log:0:sector +3 at tick 128 (player 3, stamped 128), running: 1 3 "};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sector_cell_, 3);
        // The pass's own write: no crossing.
        fake.poll = cell_poll(3, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(128);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:128"});

        // A peer's sector, relayed for 129: the pacer holds it while the
        // manager runs and for the idle ticks after.
        core.on_session_event(sector_message(4, 5, 129, 129));
        steps{core}.drain_due_events(129);
        fake.manager_active = true;
        steps{core}.sector_union_pass(129);
        fake.manager_active = false;
        for (uint32_t tick = 130; tick <= 131; ++tick) steps{core}.sector_union_pass(tick);
        from = fake.calls.size();
        steps{core}.sector_union_pass(132);
        {
            const std::vector<std::string> expected = {
                "sector_manager_active:off", "run_sector:5", "record_activation:132:reset",
                "log:0:sector +5 at tick 132 (player 4, stamped 129, held by the pacer), running: 1 3 5 "};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sector_cell_, 5);
        fake.poll = cell_poll(5, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(132);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:132"}) << "a peer's pass is no crossing of ours";
        // ...but our own ball crossing into it moves the resetpoint: ours too.
        fake.poll = cell_poll(5, true, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(134);
        {
            const std::vector<std::string> expected = {
                "poll_sector:134", "send:1",
                "log:0:sector 5 stamped for tick 142 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1, 3, 5}));
        // Moved again into a sector already ours: nothing.
        from = fake.calls.size();
        steps{core}.poll_own_sector(135);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:135"});
        // A crossing with the chain held: stamped tick + 1 + lead.
        fake.poll = cell_poll(6, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(136);
        {
            const std::vector<std::string> expected = {
                "poll_sector:136", "send:1",
                "log:0:sector 6 stamped for tick 144 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sector_cell_, 6);
        // Without the hold the chain ran already: reported after the fact,
        // stamped with the tick that ran it, and not handed to the union.
        const size_t stamps = state_of(core).sectors_.stamps_pending();
        fake.poll = cell_poll(8, false);
        from = fake.calls.size();
        steps{core}.poll_own_sector(140);
        {
            const std::vector<std::string> expected = {"poll_sector:140", "send:1", "log:0:sector 8 at tick 140"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        {
            const auto event = decode<bmmo::session_event_msg>(fake.sent.back().first);
            EXPECT_EQ(event.type, bmmo::session::event_type::Sector);
            EXPECT_EQ(event.sector, 8);
            EXPECT_EQ(event.tick, 140u);
        }
        EXPECT_EQ(state_of(core).sectors_.stamps_pending(), stamps);
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1, 3, 5, 6, 8}));
        // An unread cell counts for nothing; a cell of 0 is taken, unreported.
        fake.poll = cell_poll(11, true, true, false);
        from = fake.calls.size();
        steps{core}.poll_own_sector(141);
        fake.poll = cell_poll(0, true);
        steps{core}.poll_own_sector(142);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>({"poll_sector:141", "poll_sector:142"}));
        EXPECT_EQ(state_of(core).sector_cell_, 0);
    }

    // The phase check of the own poll (game/checkpoint_phase.hpp): a pass at
    // M has its modules branch on CurrentLevel[0][4] in frame M+5; a 0 there
    // is a pass whose bodies this world never made - counted and logged,
    // once, for that pass only.  A 0 on any other tick (the pass's own
    // window, M+6 on) and an unread cell count for nothing, and so does a
    // 0 at M+5 when another pass starts at M+5 (its own write).
    TEST(SessionClientCoreSectors, PhaseCellUnderAPass) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        core.request_sector(3, "sector verb");   // stamped 128
        for (uint32_t tick = 120; tick <= 128; ++tick) steps{core}.sector_union_pass(tick);
        const auto phase_poll = [](int phase, bool read = true) {
            bmmo::session::sector_poll poll = cell_poll(3, true);
            poll.phase_read = read;
            poll.phase = phase;
            return poll;
        };
        size_t from = fake.calls.size();
        for (uint32_t tick = 128; tick <= 132; ++tick) {
            fake.poll = phase_poll(tick < 132 ? 0 : 1);
            steps{core}.poll_own_sector(tick);
        }
        fake.poll = phase_poll(0, false);
        steps{core}.poll_own_sector(133);
        EXPECT_EQ(core.phase_misses(), 0u);
        EXPECT_EQ(since(fake.calls, from).size(), 6u) << "polls only";
        // A second pass, the cell 0 at its M+5 this time.
        core.request_sector(4, "sector verb");
        uint32_t m = 0;
        for (uint32_t tick = 134; tick <= 160 && !m; ++tick) {
            steps{core}.sector_union_pass(tick);
            if (state_of(core).recent_passes_[0].second == 4) m = tick;
        }
        ASSERT_NE(m, 0u);
        fake.poll = phase_poll(1);
        steps{core}.poll_own_sector(m + 4);
        from = fake.calls.size();
        fake.poll = phase_poll(0);
        steps{core}.poll_own_sector(m + 5);
        {
            const std::vector<std::string> expected = {
                "poll_sector:" + std::to_string(m + 5),
                "log:0:sector +4 at tick " + std::to_string(m) + ": its modules' first frame (tick " + std::to_string(m + 5)
                    + ") found CurrentLevel[0][4] = 0, so they took their teardown branch here: this world lacks that "
                      "pass's bodies (1 so far)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.phase_misses(), 1u);
        steps{core}.poll_own_sector(m + 6);
        EXPECT_EQ(core.phase_misses(), 1u) << "M+6 is not the modules' frame";
        // A pass at M+5 writes its own 0 there: not counted.
        state_of(core).recent_passes_[1] = {200, 7};
        state_of(core).recent_passes_[0] = {205, 8};
        steps{core}.poll_own_sector(205);
        EXPECT_EQ(core.phase_misses(), 1u);
        // A new session starts with no passes and no misses: the old pass's
        // M+5 with the anchor's 0 in the cell is not one of its passes.
        ASSERT_FALSE(core.begin(start_message(10, 3), bmmo::session::core_config{}).refused);
        EXPECT_EQ(core.phase_misses(), 0u);
        EXPECT_EQ(state_of(core).recent_passes_[0].first, 0u);
        EXPECT_EQ(state_of(core).recent_passes_[1].first, 0u);
        fake.poll = phase_poll(0);
        steps{core}.poll_own_sector(210);
        EXPECT_EQ(core.phase_misses(), 0u);
    }

    // ------------------------------------------------------------ the frame
    // Step E9: pre_step and post_step (design 1.3's tables) over the
    // platform's frame operations and hooks, recorded in the same call list.

    // A world the engine records and rewinds: every body there (the state
    // named, at rest), no navigation state, every step taken and recorded.
    session_platform frame_platform(flow_fake& fake, bool journalling) {
        session_platform platform = fake.make_flow(journalling);
        platform.bind_world = [&fake](const bmmo::session::core_world_hooks&) {
            fake.calls.push_back("bind_world");
            bmmo::session::rollback_world world;
            world.get_body = [](const std::string& entity, bmmo_physics_body_state& out) {
                out = bmmo_physics_body_state{};
                std::snprintf(out.name, sizeof(out.name), "%s", entity.c_str());
                out.movable = true;
                out.simulated = true;
                out.rotation[3] = 1.0;
                return true;
            };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            world.set_body = [](const std::string&, const bmmo_physics_body_state&, bmmo::session::wake_mode, bool) { return true; };
            world.recheck_body = [](const std::string&) { return true; };
            world.get_body_extra = [](const std::string&, bmmo_physics_core_extra& out) {
                out = bmmo_physics_core_extra{};
                return true;
            };
            world.set_body_extra = [](const std::string&, const bmmo_physics_core_extra&) { return true; };
            world.get_clock = [](bmmo_physics_clock_state& out) {
                out = bmmo_physics_clock_state{};
                return true;
            };
            world.get_contacts = [](const std::string&, std::vector<bmmo_physics_contact>& out) {
                out.clear();
                return 0;
            };
            world.set_contacts = [](const std::string&, const std::vector<bmmo_physics_contact>&, uint32_t) { return 0; };
            world.set_nav = [](const std::string&, const bmmo_physics_nav_state&) { return true; };
            world.nav_input = [](const std::string&, const bmmo::session::input_frame&) { return true; };
            world.nav_poll = [](const std::string&, bool) { return true; };
            world.simulating = []() { return true; };
            world.step = [&fake]() {
                fake.calls.push_back("step");
                return true;
            };
            return world;
        };
        platform.hooks.after_rewinds = [&fake](uint32_t tick) { fake.calls.push_back("hook:after_rewinds:" + std::to_string(tick)); };
        platform.hooks.after_record = [&fake](uint32_t tick) { fake.calls.push_back("hook:after_record:" + std::to_string(tick)); };
        platform.hooks.after_amend = [&fake](uint32_t tick) { fake.calls.push_back("hook:after_amend:" + std::to_string(tick)); };
        platform.hooks.after_journal = [&fake](uint32_t tick) { fake.calls.push_back("hook:after_journal:" + std::to_string(tick)); };
        platform.counters = [&fake]() {
            fake.calls.push_back("counters");
            bmmo::session::host_counters h;
            h.rebases = 3;
            h.activations_replayed = 4;
            h.script_reset_skips = 5;
            return h;
        };
        platform.hooks.window = [&fake](const std::string& text) {
            fake.calls.push_back("hook:window");
            fake.windows.push_back(text);
        };
        return platform;
    }

    // Design 5.1 "Frame order": one pre_step, the host's step, one
    // post_step, a message between frames and the next pre_step make the
    // platform calls of design 1.3's tables, in their order: steps 4 (the
    // script guards), 7 (after the rewinds), 9 (the union's manager state),
    // 10 (the mirror's drive of T), 11 (a forced own tick), 12 (the
    // capture armed); then 13 (the forced polling back), 15, 16, 17 (the own
    // ball and its upkeep), 18 (the mirror's corrector), 19 (the event log:
    // a mechanism's wakeup becomes a BodyRevived, sent), 21 (the input and
    // the camera rows), 23 (the record and its hook), 25 (the inputs and
    // the event's repeat), 26, 28 (the drive of T + 1), 30 (the amend), 30a,
    // 31 (the window: line the tick closes, with the host's counters), 32
    // (the TICK hash, a checkpoint, its hook); the next frame's events are
    // drained in its pre_step, not on arrival.
    TEST(SessionClientCoreFrame, OneCycleMakesTheTablesCallsInOrder) {
        journal_dir dir("frame");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_capabilities caps;
        caps.own_body_control = true;
        session_client_core core(frame_platform(fake, true), caps);
        run_to_tick_125_four(core, fake);
        core.config.journal_checkpoint_ticks = 2;
        auto& four = state_of(core).remotes_[4];
        four.entity = "Ball_0_BMMO_4";
        four.physicalized = true;
        four.navigation = true;
        four.applied.keys = 3;
        four.have_input = true;
        four.input.keys = 6;
        fake.own_physical = true;
        fake.driven = "Ball_Wood";
        fake.replica_name = "Ball_Wood";
        fake.forced_entity = "Ball_Wood";
        fake.mechanisms = {"P_Box"};
        fake.observation.keys = 5;
        fake.observation.cam_valid = true;
        fake.observation.cam[2][2] = 1.0f;
        fake.observation.nav_active = true;
        client_core_state::forced_own forced;
        forced.frame.keys = 9;
        state_of(core).forced_own_[126] = forced;
        bmmo::session::body_event box_wakeup;
        box_wakeup.what = bmmo::session::body_event::kind::wakeup;
        box_wakeup.name = "P_Box";
        bmmo::session::body_event box_made;
        box_made.what = bmmo::session::body_event::kind::created;
        box_made.name = "P_Box";
        fake.body_events = {box_made, box_wakeup, box_wakeup};
        // A window open since before 120 (the index of 60-119): 126 closes it.
        state_of(core).window_index_ = 2;

        ASSERT_EQ(core.next_tick(), 126u);
        size_t from = fake.calls.size();
        core.pre_step();
        {
            const std::vector<std::string> expected = {
                "script_guards:126", "hook:after_rewinds:126", "sector_manager_active:off", "drive:Ball_0_BMMO_4:3",
                "force_own:9", "arm_own_capture:126"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.current_tick(), 125u) << "T is current only after the step";
        from = fake.calls.size();
        ASSERT_TRUE(core.post_step(true));
        {
            const std::vector<std::string> expected = {
                "unforce_own:Ball_Wood", "record_pending_activation:127", "discover_navigation:126", "own_ball",
                "own_body_upkeep:Ball_Wood:body", "body_state:Ball_0_BMMO_4", "drain_body_events:126",
                "reportable_wakeup:P_Box", "send:1", "reportable_wakeup:P_Box", "observe_input", "own_camera_rows:1.000000", "own_driven_entity",
                "movable_mechanisms", "note_script_resets:126", "bind_world", "hook:after_record:126", "send:2",
                "send:2", "poll_sector:126", "drive:Ball_0_BMMO_4:6", "bind_world", "hook:after_amend:126",
                "counters", "hook:window", "world_hash", "now:journal_ms", "own_ball", "list_bodies",
                "hook:after_journal:126"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        ASSERT_EQ(fake.windows.size(), 1u);
        EXPECT_EQ(fake.windows[0].rfind("window: tick=126 inrew=0/0 ", 0), 0u) << fake.windows[0];
        EXPECT_NE(fake.windows[0].find(" rebases=3 relay="), std::string::npos) << "the host's, from a zero base";
        EXPECT_EQ(state_of(core).window_index_, 3u);
        EXPECT_EQ(core.current_tick(), 126u);
        EXPECT_EQ(core.next_tick(), 127u);
        EXPECT_EQ(fake.body_event_answers, std::vector<bool>({false, false, false})) << "none was the own ball's";
        EXPECT_EQ(state_of(core).generations_["P_Box"], 1u);
        EXPECT_EQ(state_of(core).events_sent_, 1u) << "one BodyRevived per name and frame";
        ASSERT_EQ(state_of(core).input_history_.size(), 1u);
        const auto& input = state_of(core).input_history_.back().second;
        EXPECT_EQ(input.keys, 9) << "the forced tick plays the server's frame, whole";
        EXPECT_EQ(input.flags, 0u);
        EXPECT_EQ(input.cam_dir[2], 0.0f);
        ASSERT_NE(core.previous_cam(), nullptr);
        EXPECT_EQ(core.previous_cam()[2][2], 1.0f);
        EXPECT_TRUE(state_of(core).own_physicalized_);
        EXPECT_EQ(state_of(core).own_body_entity_, "Ball_Wood");
        EXPECT_TRUE(state_of(core).rollback_.knows_tick(126));
        EXPECT_EQ(state_of(core).own_inputs_.at(126).keys, 9);
        EXPECT_EQ(four.applied.keys, 6) << "the drive of 127 is the newest relayed frame";

        // Between frames: an event due at 127 waits for its frame's pre_step,
        // where the drain (step 2) comes first; the union's pass waits for
        // the manager's pacer (E8).
        core.on_session_event(sector_message(4, 2, 127, 127));
        from = fake.calls.size();
        EXPECT_TRUE(since(fake.calls, from).empty());
        core.pre_step();
        {
            const std::vector<std::string> expected = {
                "log:0:remote player 4 entered sector 2, stamped for tick 127", "script_guards:127",
                "hook:after_rewinds:127", "sector_manager_active:off", "drive:Ball_0_BMMO_4:6", "arm_own_capture:127"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        // A failed step: only the forced polling is restored (none here),
        // nothing is current, and the host ends the session.
        from = fake.calls.size();
        EXPECT_FALSE(core.post_step(false));
        EXPECT_TRUE(since(fake.calls, from).empty());
        EXPECT_EQ(core.current_tick(), 126u);
    }

    // An own ball made or gone in the event log is reported (the event log's
    // own births) and answered true to the host; the drive and the forced
    // polling of a host without an own replica do nothing.
    TEST(SessionClientCoreFrame, TheOwnBallsBodyEventsAreReportedAndAnswered) {
        flow_fake fake;
        session_client_core core(frame_platform(fake, false), session_capabilities{});
        run_to_tick_125(core, fake);
        fake.own_physical = true;
        fake.capture.captured = true;
        bmmo::session::body_event made;
        made.what = bmmo::session::body_event::kind::created;
        made.name = "Ball_Wood";
        bmmo::session::body_event gone = made;
        gone.what = bmmo::session::body_event::kind::deleted;
        bmmo::session::body_event ball_wakeup = made;
        ball_wakeup.what = bmmo::session::body_event::kind::wakeup;
        fake.body_events = {gone, made, ball_wakeup};
        state_of(core).forced_own_[126] = client_core_state::forced_own{};
        core.pre_step();
        ASSERT_TRUE(core.post_step(true));
        EXPECT_EQ(fake.body_event_answers, std::vector<bool>({true, true, false}));
        EXPECT_EQ(state_of(core).generations_["Ball_Wood"], 2u);
        int unforced = 0, reported = 0;
        for (const auto& call: fake.calls) {
            unforced += call.rfind("unforce_own:", 0) == 0;
            reported += call == "own_birth_capture:Ball_Wood:126";
        }
        EXPECT_EQ(unforced, 0) << "no own replica was forced";
        EXPECT_EQ(reported, 1) << "the own birth reported from the capture";
        EXPECT_EQ(state_of(core).events_sent_, 2u) << "the Unphysicalize and the Physicalize; a ball's wakeup is no BodyRevived";
    }

    // The input_rewind and own_corrections switches (design 1.2, 5.1; E9).
    // On (the headless setting): a relayed frame that differs from the
    // prediction and a held notice of our own substituted tick both ask for
    // the one replay, and the own record takes the server's frame.  Off:
    // neither asks for a replay, and the own record keeps what we played.
    TEST(SessionClientCoreFrame, BothSwitchesOffNoReplayNoOwnAmend) {
        for (const bool on: {true, false}) {
            SCOPED_TRACE(on ? "switches on" : "switches off");
            flow_fake fake;
            session_client_core core(frame_platform(fake, false), session_capabilities{});
            run_to_tick_125(core, fake);
            core.config.input_rewind = on;
            core.config.own_corrections = on;
            state_of(core).held_floor_ = 119;
            auto& four = state_of(core).remotes_[4];
            four.entity = "Ball_Wood_BMMO_4";
            four.physicalized = true;
            four.navigation = true;
            for (uint32_t tick = 118; tick <= 125; ++tick) {
                bmmo::session::rollback_tracked tracked;
                tracked.own_entity = "Ball_Wood";
                tracked.own_polls = true;
                tracked.remote_entities = {"Ball_Wood_BMMO_4"};
                std::map<std::string, bmmo::session::input_frame> applied;
                applied["Ball_Wood"] = keys_frame(1);
                applied["Ball_Wood"].flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
                applied["Ball_Wood_BMMO_4"] = keys_frame(0);
                state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, tracked, applied);
            }
            // The relay: player 4 played keys 7 at 124, where we predicted 0.
            bmmo::session_remote_input_msg msg = relay_message(124, 120);
            bmmo::session_remote_input_msg::entry entry;
            entry.player = 4;
            entry.frames = {keys_frame(7)};
            msg.entries = {entry};
            core.on_session_remote_input(msg);
            // Our own tick 124: the server applied keys 9 where we played 1.
            bmmo::session::input_frame served = keys_frame(9);
            served.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;   // a difference a replay sees
            state_of(core).held_ahead_[124] = {served, true};
            steps{core}.take_held_ahead(124);
            bmmo::session::input_frame recorded{};
            ASSERT_TRUE(state_of(core).rollback_.recorded_input("Ball_Wood", 124, recorded));
            EXPECT_EQ(recorded.keys, on ? 9 : 1) << "the own record amended only with own_corrections";
            EXPECT_EQ(state_of(core).subst_diff_, on ? 1u : 0u);
            EXPECT_EQ(state_of(core).input_rewind_pending_, on);
            EXPECT_EQ(state_of(core).input_rewind_entities_.count("Ball_Wood_BMMO_4"), on ? 1u : 0u);
            EXPECT_EQ(state_of(core).own_inputs_.at(124).keys, 9) << "the server's frame is stored either way";
            const size_t from = fake.calls.size();
            steps{core}.replay_corrected_inputs();
            int stepped = 0;
            for (const auto& call: since(fake.calls, from)) stepped += call == "step";
            EXPECT_EQ(stepped, on ? 2 : 0) << "one replay from 123 to 125, or none";
            EXPECT_EQ(state_of(core).input_rewinds_, on ? 1u : 0u);
            EXPECT_EQ(state_of(core).subst_rewinds_, on ? 1u : 0u);
        }
    }

    // The pacer's half (design 1.3 "pacing"; E9): the rebase threshold is
    // 33 frames plus the slew still outstanding, a rebase asks for a resync
    // through the limiter, and a slew step moves the clock by what
    // lead_slew::advance returns and acknowledges the slew when it is done.
    TEST(SessionClientCoreFrame, PacingRebaseAndSlew) {
        flow_fake fake;
        session_capabilities caps;
        caps.slew = true;
        session_client_core core(fake.make_flow(), caps);
        run_to_tick_125(core, fake);
        EXPECT_EQ(core.rebase_threshold(), 33.0);
        EXPECT_FALSE(core.slewing());
        size_t from = fake.calls.size();
        core.schedule_rebased("tick schedule rebased");
        EXPECT_EQ(state_of(core).resyncs_sent_, 1u);
        {
            const std::vector<std::string> expected = {"now:resync_limiter", "send:0",
                                                       "log:0:resync requested (tick schedule rebased) at tick 125"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        // A slew of +2 (a raise): the clock moves earlier one tick per period.
        state_of(core).lead_slew_.start(7, 2);
        state_of(core).lead_slew_clock_ = fake.clock;
        ASSERT_TRUE(core.slewing());
        EXPECT_EQ(core.rebase_threshold(), 35.0);
        int moved = 0;
        for (int step = 0; step < 16 && core.slewing(); ++step) {
            fake.advance(std::chrono::milliseconds(16));
            moved += core.slew_step(fake.clock, 66.0, 0.0);
        }
        EXPECT_EQ(moved, 2);
        EXPECT_FALSE(core.slewing());
        EXPECT_EQ(state_of(core).lead_offset_, 2);
        EXPECT_EQ(state_of(core).lead_slew_clock_, fake.clock);
    }


    // ------------------------------------------------------------ reports
    // Step E10 (design 5.1 "Status text"): the status line is one format with
    // the host's counters at the places the headless client printed them
    // (rebases=, activations=, scr='s last field), whole however long it is.
    TEST(SessionClientCoreReport, StatusTextCarriesTheHostCounters) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        bmmo::session::host_counters h;
        h.rebases = 7;
        h.activations_replayed = 8;
        h.script_reset_skips = 9;
        state_of(core).relay_lag_max_ = 40;
        state_of(core).relay_lag_hist_[5] = 98;
        state_of(core).relay_lag_hist_[30] = 2;
        state_of(core).input_reports_ = 4;
        state_of(core).input_starved_ = 1;
        const size_t from = fake.calls.size();
        const std::string text = core.status_text(h);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "a text, nothing printed";
        EXPECT_EQ(text.rfind("status: phase=0 session=0 tick=0 assigned=0 held=0 frames=", 0), 0u) << text;
        EXPECT_NE(text.find(" lag=40/30 "), std::string::npos) << "the 99th percentile: " << text;
        EXPECT_NE(text.find(" amend_failures=0 rebases=7 resyncs=0/0 "), std::string::npos) << text;
        EXPECT_NE(text.find(" deferred=0 activations=8 scr=0/0/0/0/9 t_rest="), std::string::npos) << text;
        const std::string tail_text = "input: reports=4 starved=1 (25%) worst=0 lead=0.0";
        ASSERT_GE(text.size(), tail_text.size());
        EXPECT_EQ(text.substr(text.size() - tail_text.size()), tail_text) << "the last field, not cut off";
        EXPECT_GT(text.size(), 256u) << "past the first buffer, whole";
        EXPECT_EQ(text.find('\n'), std::string::npos);
    }

    // The window: line (design 1.3 step 31): nothing on the first edge
    // (the base is set), one line on each new window index with this
    // window's deltas (the host's rebases among them), the lag and the
    // input-rewind sources appended and cleared, nothing inside a window;
    // --window-origin's phase moves the edges.
    TEST(SessionClientCoreReport, WindowTextAtTheEdges) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        bmmo::session::host_counters h;
        state_of(core).input_rewinds_ = 4;
        state_of(core).input_rewind_depth_ = 10;
        EXPECT_TRUE(core.window_due(59)) << "the first edge is due";
        EXPECT_EQ(core.window_text(59, h), "") << "the first edge only sets the base";
        EXPECT_FALSE(core.window_due(59)) << "the frame reads no counters inside a window";
        EXPECT_EQ(core.window_text(59, h), "") << "the same window";
        EXPECT_TRUE(core.window_due(60));
        state_of(core).input_rewinds_ = 6;
        state_of(core).input_rewind_depth_ = 15;
        state_of(core).subst_recv_ = 3;
        state_of(core).lead_offset_ = -2;
        state_of(core).window_lag_[3][5] = 2;
        state_of(core).window_lag_[3][7] = 1;
        state_of(core).window_lag_[6][1] = 4;
        state_of(core).window_inrew_src_[3] = 2;
        h.rebases = 1;
        EXPECT_EQ(core.window_text(60, h),
                  "window: tick=60 inrew=2/5 life=0/0 forced=0/0 snaprb=0/0 snaps=0 resyncs=0/0 rebases=1 relay=0 "
                  "late=0 subst=3/0/0/0/0/0 substrew=0/0 substco=0/0/0/0 lead_adj=0 lead=-2 slew=0 moved=0 "
                  "echo=0/0/0/0/0 ownlife=0/0/0 lag[3]=5*2,7*1 lag[6]=1*4 inrew[3]=2");
        EXPECT_TRUE(state_of(core).window_lag_.empty());
        EXPECT_TRUE(state_of(core).window_inrew_src_.empty());
        EXPECT_FALSE(core.window_due(119));
        EXPECT_EQ(core.window_text(119, h), "");
        EXPECT_EQ(core.window_text(120, h).rfind("window: tick=120 inrew=0/0 ", 0), 0u);
        EXPECT_NE(state_of(core).window_base_.rebases, 0) << "the base keeps the host's count";

        session_client_core shifted(fake.make_flow(), session_capabilities{});
        shifted.config.window_shift = 25;
        EXPECT_EQ(shifted.window_text(84, h), "");
        EXPECT_FALSE(shifted.window_due(84));
        EXPECT_TRUE(shifted.window_due(85));
        EXPECT_EQ(shifted.window_text(85, h).rfind("window: tick=85 ", 0), 0u) << "(85 - 25) % 60 == 0";
        EXPECT_EQ(shifted.window_text(144, h), "");
        EXPECT_EQ(shifted.window_text(145, h).rfind("window: tick=145 ", 0), 0u);
    }

    // ---------------------------------------------------------------- R1
    // Step R1 (design 6.2): what the retail host needs of the core - the
    // frame refused without its pre_step, the hold given up, the lifecycle
    // switch off with the mirror started from a row, the own body backstop
    // and the snapshots' acked input.

    // post_step refuses a frame no pre_step prepared (logged, nothing taken),
    // takes one frame per pre_step, and begin clears a prepared frame.
    TEST(SessionClientCoreR1, PostStepRefusesAFrameNoPreStepPrepared) {
        flow_fake fake;
        session_client_core core(frame_platform(fake, false), session_capabilities{});
        run_to_tick_125(core, fake);
        EXPECT_FALSE(core.post_step(true));
        EXPECT_EQ(core.current_tick(), 125u) << "nothing taken";
        EXPECT_EQ(fake.calls.back(), "log:0:post_step refused at tick 125: no pre_step prepared this frame");
        core.pre_step();
        EXPECT_TRUE(core.post_step(true));
        EXPECT_EQ(core.current_tick(), 126u);
        EXPECT_FALSE(core.post_step(true)) << "one post_step per pre_step";
        EXPECT_EQ(core.current_tick(), 126u);
        core.pre_step();
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        EXPECT_FALSE(core.post_step(true)) << "begin clears a prepared frame";
    }

    // release_hold: the blocking hold given up (the retail host's lost
    // connection); nothing while not holding.
    TEST(SessionClientCoreR1, ReleaseHoldEndsTheHold) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        ASSERT_TRUE(core.holding());
        size_t from = fake.calls.size();
        core.release_hold("the connection was lost");
        EXPECT_FALSE(core.holding());
        const auto calls = since(fake.calls, from);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "log:0:anchor hold released after 0 polls: the connection was lost"),
                  calls.end());
        from = fake.calls.size();
        core.release_hold("again");
        EXPECT_TRUE(since(fake.calls, from).empty()) << "not holding: nothing";
        EXPECT_FALSE(core.poll_hold().held);
    }

    // lifecycle_rewind off (design 1.2; R1): late births and a late death are
    // made at the drain, at the live tick, with no rewind; with
    // mirror_start_from_row a birth whose player has a newer snapshot row
    // starts from it (its pose, no kick, then the row written), one without
    // starts from the event (kicked).
    TEST(SessionClientCoreR1, LifecycleRewindOffMakesLateChangesLive) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.lifecycle_rewind = false;
        core.config.mirror_start_from_row = true;
        state_of(core).spawn_impulse_ = 3.0f;
        core.on_session_snapshot(snapshot_message(9, 124, false, {ball_row(4, 7.0, true)}));
        ASSERT_EQ(state_of(core).ball_rows_.count(4), 1u);
        EXPECT_EQ(state_of(core).ball_rows_.at(4).tick, 124u);
        auto four = event_message(4, bmmo::session::event_type::Physicalize, 121, 121);
        four.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        four.position[0] = 1.0f;
        auto five = event_message(5, bmmo::session::event_type::Physicalize, 122, 122);
        five.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        five.position[0] = 2.0f;
        core.on_session_event(std::move(five));
        core.on_session_event(std::move(four));
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        auto calls = since(fake.calls, from);
        for (const auto& call: calls) {
            EXPECT_NE(call, "bind_world") << "no rewind";
            EXPECT_NE(call.rfind("keep_live_cells", 0), 0u) << "no rewind";
        }
        ASSERT_EQ(fake.specs.size(), 2u);
        EXPECT_EQ(fake.specs[0].player, 4u) << "join order 0 first";
        EXPECT_FALSE(fake.specs[0].kick) << "the row carries the server's kick";
        EXPECT_FLOAT_EQ(fake.specs[0].position[0], 7.0f) << "the row's pose";
        EXPECT_FLOAT_EQ(fake.specs[0].rotation[0], 1.0f);
        EXPECT_EQ(fake.specs[1].player, 5u);
        EXPECT_TRUE(fake.specs[1].kick) << "no row: the event's spawn kick";
        EXPECT_FLOAT_EQ(fake.specs[1].position[0], 2.0f);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "write_body:Ball_0_BMMO_4:7.000000:wake"), calls.end());
        EXPECT_EQ(std::count_if(calls.begin(), calls.end(), [](const std::string& c) { return c.rfind("write_body:", 0) == 0; }), 1)
            << "only the row's mirror is written";
        EXPECT_EQ(state_of(core).remotes_.at(4).born_tick, 126u) << "made live: the next tick";
        EXPECT_EQ(state_of(core).remotes_.at(5).born_tick, 126u);
        EXPECT_TRUE(state_of(core).pending_births_.empty());
        EXPECT_EQ(state_of(core).life_rewinds_ + state_of(core).life_rewinds_missed_, 0u);
        // A late death: taken out at the drain, no rewind.
        core.on_session_event(event_message(4, bmmo::session::event_type::Unphysicalize, 124, 124));
        from = fake.calls.size();
        steps{core}.drain_due_events(126);
        calls = since(fake.calls, from);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "unmake_mirror:Ball_0_BMMO_4"), calls.end());
        for (const auto& call: calls) EXPECT_NE(call, "bind_world") << "no rewind";
        EXPECT_FALSE(state_of(core).remotes_.at(4).physicalized);
        // The policy off: no rows kept.
        session_client_core plain(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(plain, fake);
        plain.on_session_snapshot(snapshot_message(9, 124, false, {ball_row(4, 7.0, true)}));
        EXPECT_TRUE(state_of(plain).ball_rows_.empty());
    }

    // own_body_backstop (design 4.1; R1): 30 snapshots in a row without a
    // body of ours, while ours is physicalized, re-report the last own
    // Physicalize without the spawn flag; at most every 2 s; a snapshot with
    // our row, or no birth to report, counts for nothing.
    TEST(SessionClientCoreR1, OwnBodyBackstopReReportsAfter30Snapshots) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        core.config.rollback = false;   // the ladder: no rollback needed to see the counts
        core.config.own_body_backstop = true;
        state_of(core).own_physicalized_ = true;
        fake.own_physical = true;
        state_of(core).own_last_birth_.valid = true;
        state_of(core).own_last_birth_.ball_type = 1;
        state_of(core).own_last_birth_.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        uint32_t tick = 100;
        const auto physicalizes = [&fake]() {
            int n = 0;
            for (const auto& [bytes, mode]: fake.sent) {
                if (mode != send_mode::reliable_no_nagle) continue;
                bmmo::session_event_msg msg;
                msg.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (msg.deserialize() && msg.type == bmmo::session::event_type::Physicalize) {
                    EXPECT_EQ(msg.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN, 0) << "no spawn flag";
                    EXPECT_EQ(msg.tick, 126u);
                    EXPECT_EQ(msg.ball_type, 1u);
                    ++n;
                }
            }
            return n;
        };
        for (int i = 0; i < 29; ++i) core.on_session_snapshot(snapshot_message(9, ++tick, false, {ball_row(4, 1.0, true)}));
        EXPECT_EQ(physicalizes(), 0);
        core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        EXPECT_EQ(physicalizes(), 1);
        EXPECT_EQ(core.own_body_backstops(), 1u);
        for (int i = 0; i < 31; ++i) core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        EXPECT_EQ(core.own_body_backstops(), 1u) << "the 2 s limiter";
        fake.advance(std::chrono::milliseconds(2000));
        core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        EXPECT_EQ(core.own_body_backstops(), 2u);
        // Our row comes back: the count starts over.
        for (int i = 0; i < 29; ++i) core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        core.on_session_snapshot(snapshot_message(9, ++tick, false, {ball_row(3, 1.0, true)}));
        fake.advance(std::chrono::milliseconds(2000));
        core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        EXPECT_EQ(core.own_body_backstops(), 2u);
        // The ball unphysicalized: nothing to report.
        steps{core}.report_unphysicalize(126);
        for (int i = 0; i < 40; ++i) core.on_session_snapshot(snapshot_message(9, ++tick, false, {}));
        EXPECT_EQ(core.own_body_backstops(), 2u);
        EXPECT_EQ(physicalizes(), 2);
        // A core reused for another session counts, and limits, from scratch.
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        EXPECT_EQ(core.own_body_backstops(), 0u);
        EXPECT_TRUE(state_of(core).last_backstop_ == std::chrono::steady_clock::time_point{});
    }

    // acked_input (R1): the snapshots' acked_input_tick under the current
    // numbering - its first value the baseline, the frame it last moved at,
    // the frame of the newest snapshot - and a renumbering forgets it.
    TEST(SessionClientCoreR1, AckedInputFollowsTheSnapshots) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        core.config.rollback = false;
        EXPECT_FALSE(core.acked_input().known);
        auto snapshot = snapshot_message(9, 120, false, {});
        snapshot.acked_input_tick = 118;
        core.on_session_snapshot(snapshot);
        auto acked = core.acked_input();
        EXPECT_TRUE(acked.known);
        EXPECT_EQ(acked.tick, 118u);
        EXPECT_EQ(acked.frame, 6);
        EXPECT_EQ(acked.snapshot_frame, 6);
        state_of(core).frames_since_anchor_ = 9;
        snapshot = snapshot_message(9, 121, false, {});
        snapshot.acked_input_tick = 118;
        core.on_session_snapshot(snapshot);
        acked = core.acked_input();
        EXPECT_EQ(acked.frame, 6) << "the cursor did not move";
        EXPECT_EQ(acked.snapshot_frame, 9);
        core.on_session_assign(assign_message(9, 300));
        EXPECT_FALSE(core.acked_input().known) << "a renumbering";
    }

    // drive_from_newest_relay (R1): with the policy, the step of T drives a
    // predicted mirror with the frame live_drive chooses at pre_step(T) from
    // the relay as the messages left it; without (the headless setting), with
    // the one post_step(T - 1) chose.
    TEST(SessionClientCoreR1, DriveFromTheNewestRelayAtPreStep) {
        for (const bool on: {false, true}) {
            SCOPED_TRACE(on ? "policy on" : "policy off");
            flow_fake fake;
            session_client_core core(frame_platform(fake, false), session_capabilities{});
            run_to_tick_125_four(core, fake);
            core.config.drive_from_newest_relay = on;
            auto& four = state_of(core).remotes_[4];
            four.entity = "Ball_Wood_BMMO_4";
            four.physicalized = true;
            four.navigation = true;
            four.applied = keys_frame(1);   // what post_step(125) chose
            four.inputs[126] = keys_frame(5);   // relayed since
            const size_t from = fake.calls.size();
            core.pre_step();
            const auto calls = since(fake.calls, from);
            EXPECT_NE(std::find(calls.begin(), calls.end(), on ? "drive:Ball_Wood_BMMO_4:5" : "drive:Ball_Wood_BMMO_4:1"),
                      calls.end());
            EXPECT_EQ(state_of(core).remotes_.at(4).applied.keys, on ? 5 : 1) << "the record of 126 stores what drove it";
        }
    }

    // frozen -> resync (design 4.1, 7 Q1; R1, both hosts): a snapshot that
    // needs a rollback while the local physics clock stands still sets the
    // bodies and drops the history with no replay, so the core asks for a
    // resync; with the clock running the same snapshot rolls back and asks
    // for nothing.
    TEST(SessionClientCoreR1, FrozenClockAsksForAResync) {
        for (const bool simulating: {false, true}) {
            SCOPED_TRACE(simulating ? "clock running" : "clock frozen");
            flow_fake fake;
            fake.driven = "Ball_Wood";
            auto platform = fake.make_flow();
            auto own = std::make_shared<bmmo_physics_body_state>();
            own->movable = true;
            own->simulated = true;
            own->rotation[3] = 1.0;
            platform.bind_world = [&fake, own, simulating](const bmmo::session::core_world_hooks&) {
                fake.calls.push_back("bind_world");
                bmmo::session::rollback_world world;
                world.get_body = [own](const std::string& entity, bmmo_physics_body_state& out) {
                    if (entity != "Ball_Wood") return false;
                    out = *own;
                    return true;
                };
                world.set_body = [own](const std::string& entity, const bmmo_physics_body_state& state,
                                       bmmo::session::wake_mode, bool) {
                    if (entity != "Ball_Wood") return false;
                    *own = state;
                    return true;
                };
                world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
                world.simulating = [simulating]() { return simulating; };
                world.step = []() { return true; };
                return world;
            };
            session_client_core core(std::move(platform), session_capabilities{});
            run_to_tick_125(core, fake);
            core.config.rollback = true;
            state_of(core).own_physicalized_ = true;
            bmmo::session::rollback_tracked tracked;
            tracked.own_entity = "Ball_Wood";
            for (uint32_t tick = 118; tick <= 125; ++tick)
                state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, tracked, {});
            const uint64_t resyncs_before = core.resyncs_sent();
            const size_t from = fake.calls.size();
            // The server had the own ball 5 m away at tick 122.
            core.on_session_snapshot(snapshot_message(9, 122, false, {ball_row(3, 5.0, true)}));
            const auto calls = since(fake.calls, from);
            const bool asked = std::find(calls.begin(), calls.end(),
                                         "log:0:resync requested (local physics clock frozen) at tick 125") != calls.end();
            EXPECT_EQ(core.rollback_stats().frozen, simulating ? 0u : 1u);
            EXPECT_EQ(core.resyncs_sent() - resyncs_before, simulating ? 0u : 1u);
            EXPECT_EQ(asked, !simulating);
            EXPECT_EQ(state_of(core).resync_in_flight_, !simulating) << "the request waits for its assignment";
        }
    }

}
