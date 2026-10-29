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
#include <array>
#include <tuple>
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
#include <message/session_resync_state_msg.hpp>

// The core's state and its frame's steps are private (step E11); the tests
// reach them through this friend.  steps{core}.<step>(...) calls a private
// step; state_of(core) is the state.
namespace bmmo::session {
    struct core_test_access {
        session_client_core& c;
        client_core_state& s() { return c.s; }
        const core_world_hooks& world_hooks() { return c.world_hooks_; }
#define BMMO_CORE_STEP(name) \
        template <class... A> decltype(auto) name(A&&... a) { return c.name(std::forward<A>(a)...); }
        BMMO_CORE_STEP(drain_due_events)
        BMMO_CORE_STEP(apply_held_lifecycle)
        BMMO_CORE_STEP(drain_pending_lives)
        BMMO_CORE_STEP(settle_unreplayed_lives)
        BMMO_CORE_STEP(log_life)
        BMMO_CORE_STEP(remake_mirror_birth)
        BMMO_CORE_STEP(make_unmade_remakes)
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
        BMMO_CORE_STEP(movable_mechanisms)
        BMMO_CORE_STEP(rewind_for_sectors)
        BMMO_CORE_STEP(try_apply_resync)
        BMMO_CORE_STEP(rewind_for_lifecycle)
        BMMO_CORE_STEP(window_rewind_reach)
        BMMO_CORE_STEP(drain_pending_deaths)
        BMMO_CORE_STEP(probe_candidates)
        BMMO_CORE_STEP(settle_held_tick)
        BMMO_CORE_STEP(check_own_body)
        BMMO_CORE_STEP(poll_own_trafo)
        BMMO_CORE_STEP(hold_pending_trafo)
        BMMO_CORE_STEP(apply_trafo_inputs)
        BMMO_CORE_STEP(retire_trafo)
        BMMO_CORE_STEP(reconcile_trafo)
        BMMO_CORE_STEP(note_own_death)
        BMMO_CORE_STEP(keep_live_cells)
        BMMO_CORE_STEP(join_rank)
#undef BMMO_CORE_STEP
        static bmmo::session::input_frame live_drive(const client_core_state::remote_ball& remote, uint32_t tick) {
            return session_client_core::live_drive(remote, tick);
        }
        bool recorded_input_at(const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return c.recorded_input_at(entity, tick, out);
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

    // The level-walk gate (sector_union.hpp, step S4) opened, as after the
    // level's own walk: for a union, or for the core's live union.
    void open_gate(bmmo::session::gated_sector_union& u) {
        auto st = u.state();
        st.walk = bmmo::session::sector_union::level_walk{true, false, 0};
        u.restore(st);
    }
    void open_gate(session_client_core& core) { open_gate(state_of(core).sectors_); }

    // Records every platform call in order, as "op:detail".
    struct fake_platform {
        std::vector<std::string> calls;
        std::chrono::steady_clock::time_point clock{std::chrono::milliseconds(1000)};
        std::string driven;              // what own_driven_entity answers
        bool write_ok = true;            // what write_body answers (false: "no body")
        bool mirror_ok = true;           // what mirror_entity answers (false: "" and "no ball")
        bool make_mirror_ok = true;      // what make_mirror answers (false: not made, "no body")
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
                made.made = make_mirror_ok;
                made.kicked = make_mirror_ok && spec.kick;
                if (!make_mirror_ok) made.error = "no body";
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
            p.unmake_own = [this](const std::string& entity, const float*, const float*, uint32_t tick, bool death, std::string&) {
                calls.push_back("unmake_own:" + entity + ":" + std::to_string(tick) + (death ? ":death" : ""));
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
            p.movable_bodies = [this]() {
                calls.push_back("movable_bodies");
                return mechanisms;
            };
            p.note_script_resets = [this](uint32_t tick) { calls.push_back("note_script_resets:" + std::to_string(tick)); };
            // Step B1's.
            p.set_union_pass = [this](uint32_t tick, int sector) {
                calls.push_back("set_union_pass:" + std::to_string(tick) + ":" + std::to_string(sector));
            };
            p.sector_cell_adopted = [this](int& cell) {
                calls.push_back("sector_cell_adopted");
                cell = adopted_cell;
                return adopted_read;
            };
            // Step A3's.
            p.localize_scripts = [this](const std::vector<uint8_t>& portable, const std::vector<uint8_t>* anchor,
                                        std::vector<uint8_t>& out, std::string& error,
                                        bmmo::session::resync_script_report& report) {
                calls.push_back("localize_scripts:" + std::to_string(portable.size()) + ":"
                                + (anchor ? std::to_string(anchor->size()) : std::string("none")));
                report.unportable = localize_unportable;
                if (!localize_ok) {
                    error = "no such script";
                    return false;
                }
                out = portable;
                return true;
            };
            // The state's packed sections: carried unpacked in these tests.
            p.unpack_bytes = [](const std::vector<uint8_t>& packed, uint32_t size, std::vector<uint8_t>& out) {
                out = packed;
                return out.size() == size;
            };
            p.forget_activations_from = [this](uint32_t tick) {
                calls.push_back("forget_activations_from:" + std::to_string(tick));
            };
            p.forget_births_after = [this](uint32_t tick, const std::vector<std::string>& entities) {
                calls.push_back("forget_births_after:" + std::to_string(tick) + ":" + std::to_string(entities.size()));
            };
            // Step A5's.
            p.forget_pending_activations = [this](uint32_t from, uint32_t to) {
                calls.push_back("forget_pending_activations:" + std::to_string(from) + ":" + std::to_string(to));
                return size_t{1};
            };
            p.birth_tick_of = [this](const std::string& entity) {
                const auto it = birth_ticks.find(entity);
                return it == birth_ticks.end() ? 0u : it->second;
            };
            p.local_only_body = [this](const std::string& entity) { return local_only.count(entity) != 0; };
            return p;
        }
        int adopted_cell = 0;        // what sector_cell_adopted answers (B1)
        bool adopted_read = true;
        bool localize_ok = true;     // what localize_scripts answers (A3)
        uint32_t localize_unportable = 0;
        std::map<std::string, uint32_t> birth_ticks;   // what birth_tick_of answers (A3)
        std::set<std::string> local_only;               // what local_only_body answers (A3)
    };

    TEST(SessionClientCore, ConstructsWithTheRequiredOperations) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        EXPECT_TRUE(fake.calls.empty()) << "construction calls nothing on the platform";
    }

    TEST(SessionClientCore, RefusesAPlatformWithoutAnOperation) {
        for (int missing = 0; missing < 42; ++missing) {
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
            if (missing == 33) p.movable_bodies = nullptr;
            if (missing == 34) p.note_script_resets = nullptr;
            if (missing == 35) p.set_union_pass = nullptr;
            if (missing == 36) p.sector_cell_adopted = nullptr;
            if (missing == 37) p.localize_scripts = nullptr;
            if (missing == 38) p.forget_activations_from = nullptr;
            if (missing == 39) p.forget_births_after = nullptr;
            if (missing == 40) p.unpack_bytes = nullptr;
            if (missing == 41) p.forget_pending_activations = nullptr;
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

    // Step RR: the body a snapshot row of the server's (kind, owner) belongs
    // to at a replayed tick - a remote's mirror once it is driven there (the
    // replay_remotes rule), never the own ball (the engine's), an unresolved
    // mechanism none.
    TEST(SessionClientCore, RowEntityIsTheBodyTheServerIdentityHasAtTheTick) {
        fake_platform fake;
        session_client_core core(fake.make(), session_capabilities{});
        state_of(core).own_id_ = 3;
        auto& remote = state_of(core).remotes_[4];
        remote.entity = "Ball_Wood_BMMO_4";
        remote.physicalized = true;
        remote.navigation = true;
        remote.driven_from = 120;
        const auto& row_entity = steps{core}.world_hooks().row_entity;
        ASSERT_TRUE(row_entity);
        using bmmo::session::body_kind;
        EXPECT_EQ(row_entity(body_kind::Ball, 4, 119), "") << "not driven yet at 119";
        EXPECT_EQ(row_entity(body_kind::Ball, 4, 120), "Ball_Wood_BMMO_4");
        EXPECT_EQ(row_entity(body_kind::Ball, 4, 130), "Ball_Wood_BMMO_4");
        EXPECT_EQ(row_entity(body_kind::Ball, 3, 130), "") << "the own ball is the engine's";
        EXPECT_EQ(row_entity(body_kind::Ball, 5, 130), "") << "no such player";
        EXPECT_EQ(row_entity(body_kind::Mechanism, 7, 130), "") << "no mechanism resolved";
        remote.navigation = false;
        EXPECT_EQ(row_entity(body_kind::Ball, 4, 130), "") << "not driven by the navigation";
        remote.navigation = true;
        remote.physicalized = false;
        EXPECT_EQ(row_entity(body_kind::Ball, 4, 130), "") << "no body";
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
        own.id = 3; own.join_order = 1; own.life = 1;
        other.id = 4; other.join_order = 0; other.life = 1;
        state_of(core).lives_.clear();
        for (const auto& entry: {own, other}) {
            bmmo::session::member_life life;
            life.entry = entry;
            state_of(core).lives_.push_back(life);
        }
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
                       make_body("Floor", false, 5.0), make_body("Ball_Stone_piece01", true, 6.0)};
        EXPECT_TRUE(core.journal().checkpoint_due(102));
        core.journal().local_checkpoint(102, "Ball_Paper");

        bmmo::session_event_msg event;
        event.tick = 102;
        event.origin_tick = 101;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = 1;
        event.recipe.collision_surface = "surface";
        event.recipe.convex_meshes = {"a", "b"};
        core.journal().lifecycle_event(7, event);   // no life names it: a note, no PLAYER record (step M1)

        const double server[3] = {9.0, 8.0, 7.0};
        core.journal().body_correction(2, "Ball_Paper", 102, 0.5, server);
        core.journal().leave_player(4, 102);   // step M1: at the server's R
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
            "102 p7 has records before its member message: its PLAYER record follows from that message",
            "102 end: bye"};
        EXPECT_EQ(notes, expected_notes);

        const bmmo::session::journal_tick* t102 = nullptr;
        for (const auto& t: j.ticks) if (t.tick == 102) t102 = &t;
        ASSERT_NE(t102, nullptr);
        ASSERT_EQ(t102->checkpoints.size(), 1u);
        const auto& cp = t102->checkpoints[0];
        EXPECT_EQ(cp.flags, bmmo::session::JOURNAL_CHECKPOINT_LOCAL);
        ASSERT_EQ(cp.bodies.size(), 5u);   // the immovable floor is left out
        EXPECT_EQ(cp.bodies[0].owner, 3u);
        EXPECT_EQ(cp.bodies[0].kind, bmmo::session::body_kind::Ball);
        EXPECT_EQ(cp.bodies[1].owner, 4u);
        EXPECT_EQ(cp.bodies[1].kind, bmmo::session::body_kind::Ball);
        // Step P: a body is a ball by what it is (the own ball, a mirror),
        // not by its "Ball_" name: one no player owns is a mechanism, by name.
        EXPECT_EQ(cp.bodies[2].owner, 0u);
        EXPECT_EQ(cp.bodies[2].kind, bmmo::session::body_kind::Mechanism);
        EXPECT_EQ(cp.bodies[2].name, "Ball_Stone_BMMO_9");
        EXPECT_EQ(cp.bodies[3].kind, bmmo::session::body_kind::Mechanism);
        EXPECT_EQ(cp.bodies[3].position[0], 4.0);
        EXPECT_EQ(cp.bodies[4].kind, bmmo::session::body_kind::Mechanism) << "a trafo explosion piece";
        EXPECT_EQ(cp.bodies[4].owner, 0u);
        EXPECT_EQ(cp.bodies[4].name, "Ball_Stone_piece01");
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
        own.id = own_id; own.join_order = 2; own.life = 1;
        other.id = own_id + 1; other.join_order = 0; other.life = 1;
        msg.players = {other, own};
        return msg;
    }

    // begin, level_requested and anchor with a session of id 9 for own id 3.
    // Step M1: a member life (life 1, from the start) for `id` with `order`,
    // replacing one the fixture gave it.  The fixtures give every other id
    // the tests send events for (5-10) join order 63, the placeholder an
    // unknown player had before the lives - its rank, group and kick are
    // what these tests were written against.
    void set_test_life(session_client_core& core, uint32_t id, uint8_t order) {
        auto& lives = state_of(core).lives_;
        lives.erase(std::remove_if(lives.begin(), lives.end(),
                                   [id](const bmmo::session::member_life& l) { return l.entry.id == id; }),
                    lives.end());
        bmmo::session::member_life life;
        life.entry.id = id;
        life.entry.join_order = order;
        life.entry.life = 1;
        lives.push_back(life);
    }

    // Step M1: the member lives' account end() logs, for a fixture session
    // (two lives from SessionStart and the six test lives, nothing journalled).
    const char* const kEndLivesLine = "log:0:member lives: 8 lives, 0 member messages (0 copies), events parked 0 (applied 0, "
                                      "dropped 0), outside their life 0, mirrors outside the lives 0, own echoes of another life "
                                      "0, catch-up stamps of untold lives 0, PLAYER records not written 0";

    void run_to_anchor(session_client_core& core, flow_fake& fake) {
        state_of(core).own_id_ = 3;
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        for (uint32_t id = 5; id <= 10; ++id) set_test_life(core, id, 63);
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
            "log:0:session 9 starting: level 13, 2 players, join order 2, life 1, seed 11, input delay 4, sector lead 7"};
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
                "log:0:session 9 starting: level 13, 2 players, join order 2, life 1, seed 11, input delay 4, sector lead 7",
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
            EXPECT_EQ(ready.caps, (slew ? bmmo::session::CAP_LEAD : 0u) | bmmo::session::CAP_RESYNC_STATE);
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
            "log:0:session 9 ended: ", "hook:ending", kEndLivesLine, "hook:ended",
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

    // Session-core design 7.1 decision 1 (step B2SW, the retail host's
    // transitional trigger until then): 30 authoritative snapshots in a row
    // with no record of their tick ask for a resync, from the core, for both
    // hosts.  Counted per snapshot: a stale one neither counts nor ends the
    // run; the request resets it; with the rollback off nothing is counted.
    TEST(SessionClientCoreSnapshot, ThirtyUnmatchedSnapshotsInARowAskForAResync) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        ASSERT_TRUE(core.config.rollback);
        for (uint32_t t = 0; t < 29; ++t) core.on_session_snapshot(snapshot_message(9, 130 + t, false, {}));
        EXPECT_EQ(core.rollback_stats().unmatched, 29u) << "no record of any of them";
        EXPECT_EQ(core.unmatched_run(), 29);
        EXPECT_EQ(state_of(core).resyncs_sent_, 0u);
        core.on_session_snapshot(snapshot_message(9, 150, false, {}));
        EXPECT_EQ(state_of(core).snapshots_stale_, 1u);
        EXPECT_EQ(core.unmatched_run(), 29) << "a stale snapshot is not one of the run";
        size_t from = fake.calls.size();
        core.on_session_snapshot(snapshot_message(9, 159, false, {}));
        {
            const auto calls = since(fake.calls, from);
            const std::vector<std::string> expected = {
                "own_ball", "keep_live_cells:enter:120", "bind_world",
                "log:0:30 snapshots in a row with no record of their tick (the last 159, local tick 120)",
                "now:resync_limiter", "send:0",
                "log:0:resync requested (30 snapshots with no record of their tick) at tick 120",
                "keep_live_cells:exit"};
            EXPECT_EQ(calls, expected);
        }
        EXPECT_EQ(state_of(core).resyncs_sent_, 1u);
        EXPECT_EQ(core.unmatched_resyncs(), 1u);
        EXPECT_EQ(core.unmatched_run(), 0) << "reset by the request";
        // A run under way ends with any resync request that goes out.
        for (uint32_t t = 0; t < 5; ++t) core.on_session_snapshot(snapshot_message(9, 160 + t, false, {}));
        EXPECT_EQ(core.unmatched_run(), 5);
        fake.advance(std::chrono::seconds(11));   // past the limiter and the in-flight give-up
        core.request_resync("test");
        EXPECT_EQ(state_of(core).resyncs_sent_, 2u);
        EXPECT_EQ(core.unmatched_run(), 0);
        // The rollback off (the legacy ladder): not counted.
        core.config.rollback = false;
        for (uint32_t t = 0; t < 40; ++t) core.on_session_snapshot(snapshot_message(9, 170 + t, false, {}));
        EXPECT_EQ(core.unmatched_run(), 0);
        EXPECT_EQ(core.unmatched_resyncs(), 1u);
        // A run the limiter holds back is not counted as a resync asked for.
        core.config.rollback = true;
        for (uint32_t t = 0; t < 30; ++t) core.on_session_snapshot(snapshot_message(9, 220 + t, false, {}));
        EXPECT_EQ(state_of(core).resyncs_sent_, 2u) << "within 10 s of the last request: held back";
        EXPECT_EQ(core.unmatched_resyncs(), 1u);
        // A run under way does not carry into the next session.
        for (uint32_t t = 0; t < 25; ++t) core.on_session_snapshot(snapshot_message(9, 260 + t, false, {}));
        EXPECT_EQ(core.unmatched_run(), 25);
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        EXPECT_EQ(core.unmatched_run(), 0);
        EXPECT_EQ(core.unmatched_resyncs(), 0u);
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
    // have no CAP_LEAD (CAP_RESYNC_STATE only, protocol 10), and a SessionLead
    // that comes anyway is counted and journaled,
    // never applied (nothing amended, forced or slewed) and never
    // acknowledged.
    TEST(SessionClientCoreLead, WithoutSlewNothingAppliedNothingAcked) {
        journal_dir dir("lead");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_flow(true), session_capabilities{});
        run_to_tick_125(core, fake);
        ASSERT_FALSE(fake.sent.empty());
        EXPECT_EQ(decode<bmmo::session_ready_msg>(fake.sent[0].first).caps, bmmo::session::CAP_RESYNC_STATE);
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

    // Late-player plan step 12 (design 2.5): every first arrival of a server
    // tick's header is a relay lag sample (current - header tick); the reach
    // is clamp(p99 of the last 4 windows + 8, 48, 120), re-derived at each
    // sample, and the history grows to 128 with it.  A pinned reach
    // (config.reach_ticks) ignores the samples; a new session starts over.
    TEST(SessionClientCoreRelay, PerClientReachFollowsTheRelayLag) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        auto& st = state_of(core);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u);
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 96u);
        // Step R12: a measured rewind cost whose cap does not bind (120).
        st.rollback_.rewind_cost().sample(1.0, 0.1 * 48, 48);
        ASSERT_EQ(st.rollback_.rewind_cost().cap(), 120u);
        // A copy or an older header is no sample.
        const auto deliver = [&](int frames, uint32_t lag) {
            st.frames_since_anchor_ = frames;
            core.on_session_remote_input(relay_message(core.current_tick() - lag, 120));
        };
        for (int i = 0; i < 60; ++i) deliver(100 + i, 21);
        core.on_session_remote_input(relay_message(core.current_tick() - 21, 120));   // the same tick again
        EXPECT_EQ(st.reach_meter_.samples(), 60u);
        EXPECT_EQ(st.reach_meter_.p99(), 21u);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u) << "21 + 8 is under the floor";
        EXPECT_EQ(st.reach_changes_, 0u);
        const size_t from = fake.calls.size();
        for (int i = 60; i < 120; ++i) deliver(100 + i, 52);   // the lag raised by 31 ticks
        EXPECT_EQ(st.reach_meter_.p99(), 52u);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 60u);
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 128u);
        EXPECT_EQ(st.rollback_.resync_reach(), 127u);
        EXPECT_EQ(st.reach_max_, 60u);
        EXPECT_GE(st.reach_changes_, 1u);
        int grown = 0;
        for (const auto& call: since(fake.calls, from))
            grown += call.find("reach: ") != std::string::npos && call.find("history 128, grown") != std::string::npos;
        EXPECT_EQ(grown, 1);
        EXPECT_NE(core.status_text({}).find(" reach=60/60/128 cap=120/1.00/0.100/1 wait=0"), std::string::npos);
        // The raised windows age out: 4 windows of 21 bring the floor back,
        // and the history stays.
        for (int i = 120; i < 400; ++i) deliver(100 + i, 21);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u);
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 128u);
        EXPECT_EQ(st.reach_max_, 60u);

        // Pinned: the samples change nothing; a new session starts over.
        bmmo::session::core_config pinned;
        pinned.reach_ticks = 120;
        ASSERT_FALSE(core.begin(start_message(9, 3), pinned).refused);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 120u);
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 128u);
        EXPECT_EQ(st.reach_meter_.samples(), 0u);
        pinned.reach_ticks = 30;
        ASSERT_FALSE(core.begin(start_message(9, 3), pinned).refused);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u) << "clamped to the floor";
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 96u) << "a new session's history";
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u);
        EXPECT_EQ(st.reach_max_, 48u);
    }

    // Step R12 (protocol 13, the user's decisions of 2026-10-27): the newest
    // header's wait widens the reach (p99 + 8 + w), and this client's own
    // rewind cost caps it - a slow machine's lower.  R5's run-end case: a
    // header lag of 43 and a wait of 21 give 72, which a 64-deep notice is
    // inside; at step 12's 51 it resynced.
    TEST(SessionClientCoreRelay, TheServerWaitWidensTheReachUpToTheCap) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        auto& st = state_of(core);
        int frames = 100;
        const auto deliver = [&](uint32_t lag, uint8_t wait) {
            st.frames_since_anchor_ = frames++;
            auto msg = relay_message(core.current_tick() - lag, 120);
            msg.wait = wait;
            core.on_session_remote_input(msg);
        };
        for (int i = 0; i < 60; ++i) deliver(43, 21);
        EXPECT_EQ(st.server_wait_, 21u);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 48u) << "no rewind measured yet: the cap is the floor";
        // x64's measured cost (3 ms, 0.46 ms a tick): cap 92.
        st.rollback_.rewind_cost().sample(3.0, 0.46 * 48, 48);
        deliver(43, 21);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 72u);
        EXPECT_EQ(st.rollback_.thresholds().history_ticks, 128u);
        EXPECT_NE(core.status_text({}).find(" reach=72/72/128 cap=92/3.00/0.460/1 wait=21"), std::string::npos)
            << core.status_text({});
        deliver(43, 60);                                 // a larger wait: the cap binds
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, 92u);
        // A slow machine: 20 rewinds at 0.8 ms a tick and 6 ms of restore
        // bring the cap down to floor(39.45 / 0.8...) and the reach with it.
        for (int i = 0; i < 40; ++i) st.rollback_.rewind_cost().sample(6.0, 0.8 * 48, 48);
        const uint32_t slow_cap = st.rollback_.rewind_cost().cap();
        EXPECT_LT(slow_cap, 92u);
        EXPECT_GE(slow_cap, 48u);
        deliver(43, 60);
        EXPECT_EQ(st.rollback_.thresholds().max_resim_ticks, slow_cap);
        // A new session: the wait and the cost start over.
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        EXPECT_EQ(st.server_wait_, 0u);
        EXPECT_FALSE(st.server_wait_known_);
        EXPECT_EQ(st.rollback_.rewind_cost().samples(), 0u);
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
        msg.life = 1;   // step M1: the server fills it on relay
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
        set_test_life(core, 5, 1);
        set_test_life(core, 6, 3);
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
            // Step B1: the late stamps no record covers (none is recorded
            // here) are stamped live as before and ask for a resync - sent
            // once, the request in flight covering the others.
            const std::vector<std::string> expected = {
                "log:0:remote player 4 entered sector 4, stamped for tick 123 (LATE: the server started it already)",
                "log:0:late sector stamp for tick 123 is outside the rollback history at local tick 125: stamped live, resync",
                "now:resync_limiter", "send:0",
                "log:0:resync requested (a late Sector event beyond the reach) at tick 125",
                "log:0:remote player 4 entered sector 3, stamped for tick 124 (LATE: the server started it already)",
                "log:0:late sector stamp for tick 124 is outside the rollback history at local tick 125: stamped live, resync",
                "now:resync_limiter",
                "log:0:remote player 6 entered sector 2, stamped for tick 124 (LATE: the server started it already)",
                "log:0:late sector stamp for tick 124 is outside the rollback history at local tick 125: stamped live, resync",
                "now:resync_limiter"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sector_rewinds_missed_, 3u);
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
        steps{core}.drain_pending_lives(124);
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
        steps{core}.drain_pending_lives(130);
        EXPECT_TRUE(since(fake.calls, from).empty());
    }

    // Step LB: a mirror made at its stamp (here inside a replay) is taken out
    // by a later restore anchored before it (the binding's destroy_body ->
    // remake_mirror_birth) and made again at that tick by the core's births:
    // the same entity (no mirror_entity), its lifetime kept, no log line, the
    // live predictor and the frame the next live step drives it with left
    // alone.  Not for another tick, a body started elsewhere (made live after
    // its stamp), one already taken out, or without the lifecycle rewind.  A
    // remake no replay made is made live by make_unmade_remakes.
    TEST(SessionClientCoreLifecycle, AMirrorBornInTheWindowIsMadeAgainAtItsBirthTick) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        client_core_state::pending_birth birth;
        birth.player = 4; birth.tick = 124; birth.origin = 124;
        birth.position[0] = 705.0f;
        state_of(core).pending_births_ = {birth};
        steps{core}.drain_pending_lives(124);
        auto& four = state_of(core).remotes_.at(4);
        ASSERT_TRUE(four.physicalized);
        ASSERT_TRUE(four.navigation);
        EXPECT_TRUE(four.birth_known) << "made at its stamp";
        EXPECT_EQ(four.born_tick, 124u);
        EXPECT_EQ(state_of(core).generations_.at("Ball_0_BMMO_4"), 1u);
        four.applied.keys = 9;   // what the end of the last live frame chose
        const auto& remake = steps{core}.world_hooks().remake_mirror_birth;
        ASSERT_TRUE(remake) << "the core's half of the binding";

        EXPECT_FALSE(remake("Ball_0_BMMO_4", 123)) << "another tick";
        EXPECT_FALSE(remake("Ball_0_BMMO_5", 124)) << "no such mirror";
        size_t from = fake.calls.size();
        EXPECT_TRUE(remake("Ball_0_BMMO_4", 124));
        {
            const std::vector<std::string> expected = {
                "detach_mirror_navigation:Ball_0_BMMO_4", "mute_event_log:on", "unmake_mirror:Ball_0_BMMO_4",
                "mute_event_log:off"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_FALSE(four.physicalized);
        EXPECT_FALSE(four.navigation);
        ASSERT_EQ(state_of(core).pending_births_.size(), 1u);
        EXPECT_TRUE(state_of(core).pending_births_[0].remake);
        EXPECT_FALSE(remake("Ball_0_BMMO_4", 124)) << "taken out already";

        from = fake.calls.size();
        steps{core}.drain_pending_lives(124);
        {
            const std::vector<std::string> expected = {
                "mute_event_log:on", "make_mirror:Ball_0_BMMO_4:P#0", "attach_mirror_navigation:4:Ball_0_BMMO_4",
                "mute_event_log:off"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).pending_births_.empty());
        EXPECT_TRUE(four.physicalized);
        EXPECT_TRUE(four.navigation);
        EXPECT_EQ(four.born_tick, 124u);
        EXPECT_EQ(four.driven_from, 124u);
        EXPECT_EQ(four.applied.keys, 9u) << "the next live step's frame stays";
        EXPECT_EQ(state_of(core).generations_.at("Ball_0_BMMO_4"), 1u) << "the same lifetime";

        // Taken out, and no replay made it again: made live.
        EXPECT_TRUE(remake("Ball_0_BMMO_4", 124));
        from = fake.calls.size();
        steps{core}.make_unmade_remakes();
        {
            const std::vector<std::string> expected = {
                "log:0:remote physicalize of 4: the replay did not make it again at tick 124, made live",
                "mute_event_log:on", "make_mirror:Ball_0_BMMO_4:P#0", "attach_mirror_navigation:4:Ball_0_BMMO_4",
                "mute_event_log:off"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(four.physicalized);
        EXPECT_TRUE(state_of(core).pending_births_.empty());
        EXPECT_EQ(state_of(core).mirror_remakes_lost_, 0u);
        EXPECT_EQ(state_of(core).mirror_remakes_, 2u) << "the replay's and the live one";

        // A remake whose make fails in the replay is queued again and tried
        // once more live; failing there too, it is counted (remake=.../lost),
        // left bodiless and never remade again.
        EXPECT_TRUE(remake("Ball_0_BMMO_4", 124));
        fake.make_mirror_ok = false;
        steps{core}.drain_pending_lives(124);
        ASSERT_EQ(state_of(core).pending_births_.size(), 1u) << "queued again";
        EXPECT_TRUE(state_of(core).pending_births_[0].remake);
        EXPECT_FALSE(four.physicalized);
        steps{core}.make_unmade_remakes();
        EXPECT_TRUE(state_of(core).pending_births_.empty());
        EXPECT_FALSE(four.physicalized);
        EXPECT_FALSE(four.birth_known);
        EXPECT_EQ(state_of(core).mirror_remakes_lost_, 1u);
        EXPECT_FALSE(remake("Ball_0_BMMO_4", 124)) << "no body to take out";
        // Made again by an ordinary make (here: the queued remake once more,
        // the make working again).
        fake.make_mirror_ok = true;
        client_core_state::pending_birth again = state_of(core).remotes_.at(4).birth;
        again.remake = true;
        state_of(core).pending_births_ = {again};
        steps{core}.make_unmade_remakes();
        EXPECT_TRUE(four.physicalized);
        EXPECT_EQ(state_of(core).mirror_remakes_lost_, 1u);
        EXPECT_EQ(state_of(core).mirror_remakes_, 3u);

        // Without the lifecycle rewind: the parking path.
        core.config.lifecycle_rewind = false;
        EXPECT_FALSE(remake("Ball_0_BMMO_4", 124));
        core.config.lifecycle_rewind = true;
        // A body made live after its stamp (a late birth the history could
        // not replay): started elsewhere, no remake.
        state_of(core).pending_births_.clear();
        core.on_session_event(event_message(5, bmmo::session::event_type::Physicalize, 100, 100));
        steps{core}.drain_due_events(126);
        const auto& five = state_of(core).remotes_.at(5);
        ASSERT_TRUE(five.physicalized);
        EXPECT_FALSE(five.birth_known);
        EXPECT_FALSE(remake(five.entity, five.born_tick));
        EXPECT_TRUE(five.physicalized);
    }

    // ------------------------------------------------- gap L, L2 (step EL7)
    // Rollback-engine gaps design 2.3.3, 7.1 L-U3..5, L-U8..10: the mirror
    // lives that end inside a rewind's window - the life log, the
    // reconcile (the core's half of restore_lives), the queued window events
    // drained in the server's order, the generations, recorded_input_at of
    // an old name, the late births kept, the settle of what no replay made,
    // and the trim.

    // A history whose records carry the given body poses per tick and the
    // given tracked sets (no physics behind it).
    struct life_history {
        std::map<uint32_t, std::map<std::string, double>> bodies;   // tick -> entity -> x
        std::map<uint32_t, bmmo::session::rollback_tracked> tracked;
        void record(session_client_core& core) {
            for (const auto& [tick, set]: tracked) {
                bmmo::session::rollback_world world;
                const auto here = bodies[tick];
                world.get_body = [here](const std::string& entity, bmmo_physics_body_state& out) {
                    const auto it = here.find(entity);
                    if (it == here.end()) return false;
                    out = bmmo_physics_body_state{};
                    out.movable = out.simulated = true;
                    out.position[0] = it->second;
                    out.rotation[3] = 1.0;
                    return true;
                };
                world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
                state_of(core).rollback_.record(world, tick, set, {});
            }
        }
        void span(uint32_t from, uint32_t to, const std::vector<std::pair<std::string, double>>& alive,
                  const std::map<std::string, uint32_t>& generations) {
            for (uint32_t t = from; t <= to; ++t) {
                auto& tr = tracked[t];
                tr.generations = generations;
                for (const auto& [entity, x]: alive) {
                    tr.remote_entities.push_back(entity);
                    bodies[t][entity] = x;
                }
            }
        }
    };

    client_core_state::life_entry life_entry_of(uint32_t player, bmmo::session::event_type type, uint32_t stamp,
                                                const std::string& entity, uint32_t generation, uint8_t ball_type = 0) {
        client_core_state::life_entry e;
        e.applied = e.made_at = e.origin = stamp;
        e.player = player;
        e.type = type;
        e.entity = entity;
        e.generation = generation;
        if (type == bmmo::session::event_type::Physicalize) {
            e.birth.player = player;
            e.birth.tick = e.birth.origin = stamp;
            e.birth.ball_type = ball_type;
            e.birth.recipe.mass = 2.5f;
            e.birth.entity = entity;
        }
        return e;
    }

    // The live end state of a player whose mirror `entity` is current
    // (made at its stamp `born`, navigated).
    void live_mirror(session_client_core& core, uint32_t player, const std::string& entity, uint32_t born, bool body) {
        auto& r = state_of(core).remotes_[player];
        r.entity = entity;
        r.physicalized = body;
        r.navigation = body;
        r.born_tick = r.driven_from = born;
        r.birth.player = player;
        r.birth.tick = r.birth.origin = born;
        r.birth_known = true;
        state_of(core).mirror_entities_.insert(entity);
    }

    size_t count_prefix(const std::vector<std::string>& calls, const std::string& prefix) {
        return static_cast<size_t>(std::count_if(calls.begin(), calls.end(),
                                                 [&prefix](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }

    // L-U3, H naming (one clone per life; the fake's mirror_entity mints a
    // new name on every call, make_mirror takes any exact name): the mirror
    // alive at A = 110 (Ball_0_BMMO_4#1, born 100) dies at D = 115 and the
    // player's next life (#2) is born at E = 118; now 125.  The reconcile
    // takes #2 out, makes #1 again at the A record's pose with no kick,
    // queues the death at 115 and the birth at 118 with #2's name; the
    // replayed records over (A, D) carry #1's old generation, the drains
    // make no mirror entity, and the final generations equal the live ones.
    // recorded_input_at of the old name reads the player's ring.
    TEST(SessionClientCoreLives, AMirrorDeadInTheWindowIsRevivedUnderItsOwnName) {
        flow_fake fake;
        session_platform platform = fake.make_flow();
        int minted = 2;
        platform.mirror_entity = [&fake, &minted](uint32_t player, uint8_t, std::string&) {
            fake.calls.push_back("mirror_entity:" + std::to_string(player));
            return "Ball_0_BMMO_4#" + std::to_string(++minted);
        };
        session_client_core core(std::move(platform), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        const std::string one = "Ball_0_BMMO_4#1", two = "Ball_0_BMMO_4#2";
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, one, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 115, one, 2),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 118, two, 1)};
        live_mirror(core, 4, two, 118, true);
        s.mirror_entities_.insert(one);
        s.generations_ = {{one, 2}, {two, 1}};
        bmmo::session::input_frame frame{};
        frame.keys = 5;
        s.remotes_.at(4).inputs[112] = frame;
        life_history h;
        h.span(110, 114, {{one, 7.0}}, {{one, 1}});
        h.span(115, 117, {}, {{one, 2}});
        h.span(118, 125, {{two, 9.0}}, {{one, 2}, {two, 1}});
        h.record(core);

        const auto& lives = steps{core}.world_hooks().restore_lives;
        ASSERT_TRUE(lives) << "the core's half is set (G7)";
        std::map<std::string, bmmo::session::life_outcome> out;
        size_t from = fake.calls.size();
        lives(110, 125, {{one, 1, 115, 0, bmmo::session::life_kind::mirror}}, true, out);
        {
            const auto calls = since(fake.calls, from);
            const std::vector<std::string> head = {
                "detach_mirror_navigation:" + two, "mute_event_log:on", "unmake_mirror:" + two, "mute_event_log:off",
                "mute_event_log:on", "make_mirror:" + one + ":P#0", "mute_event_log:off",
                "attach_mirror_navigation:4:" + one};
            ASSERT_GE(calls.size(), head.size());
            EXPECT_EQ(std::vector<std::string>(calls.begin(), calls.begin() + static_cast<std::ptrdiff_t>(head.size())), head);
            EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        }
        ASSERT_EQ(fake.specs.size(), 1u);
        EXPECT_EQ(fake.specs[0].position[0], 7.0f) << "the A record's pose";
        EXPECT_FALSE(fake.specs[0].kick) << "no kick at a revive";
        EXPECT_EQ(fake.specs[0].recipe.mass, 2.5f) << "its Physicalize's recipe";
        EXPECT_EQ(out[one].revived_until, 115u);
        EXPECT_EQ(out[one].remade_at, 0u);
        EXPECT_EQ(out[two].remade_at, 118u);
        EXPECT_EQ(s.mirror_revives_, 1u);
        EXPECT_EQ(s.remotes_.at(4).entity, one);
        EXPECT_TRUE(s.remotes_.at(4).physicalized);
        EXPECT_TRUE(s.remotes_.at(4).navigation);
        EXPECT_EQ(s.generations_.at(one), 1u) << "A's";
        EXPECT_EQ(s.generations_.count(two), 0u) << "absent at A";
        ASSERT_EQ(s.pending_deaths_.size(), 1u);
        EXPECT_EQ(s.pending_deaths_[0].tick, 115u);
        EXPECT_TRUE(s.pending_deaths_[0].reconciled);
        ASSERT_EQ(s.pending_births_.size(), 1u);
        EXPECT_EQ(s.pending_births_[0].tick, 118u);
        EXPECT_EQ(s.pending_births_[0].entity, two);
        // The replay: (A, D) drives #1 under its old generation.
        std::map<std::string, uint32_t> driven;
        steps{core}.world_hooks().replay_remotes(111, driven);
        EXPECT_EQ(driven, (std::map<std::string, uint32_t>{{one, 1u}}));
        EXPECT_TRUE(steps{core}.world_hooks().mirror(two)) << "an old life's entity is a mirror's (birth_from_inputs)";
        from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u) << "no entity minted inside a replay";
            EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + one), 1u);
            EXPECT_EQ(count_prefix(calls, "make_mirror:" + two), 1u);
        }
        EXPECT_EQ(s.generations_.at(one), 2u) << "the live value";
        EXPECT_EQ(s.generations_.at(two), 1u) << "the live value";
        EXPECT_EQ(s.remotes_.at(4).entity, two);
        EXPECT_TRUE(s.pending_births_.empty());
        EXPECT_TRUE(s.pending_deaths_.empty());
        // The old name's frames: the player's ring.
        bmmo::session::input_frame got{};
        EXPECT_TRUE(steps{core}.recorded_input_at(one, 112, got));
        EXPECT_EQ(got.keys, 5u);
        EXPECT_FALSE(steps{core}.recorded_input_at("Ball_0_BMMO_9#1", 112, got));
    }

    // L-U4, RC naming (one name per player and type): a beam pair of player
    // 4 at stamp 115 (Unphysicalize + Physicalize of the same spirit ball)
    // inside the window of A = 110.  The live body (the new life) is taken
    // out, the old life revived until 115, and at 115 the death comes before
    // the birth (the server's order); the name carries both halves and its
    // final generation equals the live one.
    TEST(SessionClientCoreLives, ARetailBeamPairInTheWindowIsRevivedThenRemade) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        const std::string ball = "Ball_0_BMMO_4";
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, ball, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 115, ball, 2),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 115, ball, 3)};
        live_mirror(core, 4, ball, 115, true);
        s.generations_ = {{ball, 3}};
        life_history h;
        h.span(110, 114, {{ball, 7.0}}, {{ball, 1}});
        h.span(115, 125, {{ball, 9.0}}, {{ball, 3}});
        h.record(core);
        std::map<std::string, bmmo::session::life_outcome> out;
        steps{core}.world_hooks().restore_lives(110, 125, {{ball, 1, 115, 115, bmmo::session::life_kind::mirror}}, true,
                                                out);
        EXPECT_EQ(out[ball].revived_until, 115u);
        EXPECT_EQ(out[ball].remade_at, 115u);
        EXPECT_EQ(s.generations_.at(ball), 1u);
        const size_t from = fake.calls.size();
        steps{core}.drain_pending_lives(115);
        {
            const auto calls = since(fake.calls, from);
            const auto death = std::find(calls.begin(), calls.end(), "unmake_mirror:" + ball);
            const auto birth = std::find(calls.begin(), calls.end(), "make_mirror:" + ball + ":P#0");
            ASSERT_NE(death, calls.end());
            ASSERT_NE(birth, calls.end());
            EXPECT_LT(death, birth) << "the beam pair's death first";
            EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        }
        EXPECT_EQ(s.generations_.at(ball), 3u) << "the live value";
        EXPECT_TRUE(s.remotes_.at(4).physicalized);

        // The parked arm (L-H6's control): a later life in the world stays,
        // so nothing is revived (left to the engine's unrevivable count).
        core.config.remote_births_parked = true;
        out.clear();
        s.pending_births_.clear();
        s.pending_deaths_.clear();
        const size_t from2 = fake.calls.size();
        steps{core}.world_hooks().restore_lives(110, 125, {{ball, 1, 115, 115, bmmo::session::life_kind::mirror}}, true,
                                                out);
        EXPECT_TRUE(out.empty());
        EXPECT_EQ(count_prefix(since(fake.calls, from2), "unmake_mirror:"), 0u);
        EXPECT_EQ(s.mirror_revive_failures_, 1u);
        EXPECT_FALSE(steps{core}.remake_mirror_birth(ball, 115)) << "no LB remake in the parked arm";
    }

    // L-U5, the RC stamp rule (lifecycle_rewind off): a death this world
    // applied live at 120 (> its stamp 115) is replayed at its stamp; a
    // birth stamped at or before A but made live after it keeps the parking
    // path, counted late_births_kept.
    TEST(SessionClientCoreLives, TheRetailStampRuleReplaysAtTheStamp) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = false;
        auto& s = state_of(core);
        const std::string ball = "Ball_0_BMMO_4";
        auto death = life_entry_of(4, bmmo::session::event_type::Unphysicalize, 115, ball, 2);
        death.made_at = 120;
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, ball, 1), death};
        live_mirror(core, 4, ball, 100, false);
        s.generations_ = {{ball, 2}};
        life_history h;
        h.span(110, 119, {{ball, 7.0}}, {{ball, 1}});
        h.span(120, 125, {}, {{ball, 2}});
        h.record(core);
        std::map<std::string, bmmo::session::life_outcome> out;
        steps{core}.world_hooks().restore_lives(110, 125, {{ball, 1, 120, 0, bmmo::session::life_kind::mirror}}, true, out);
        EXPECT_EQ(out[ball].revived_until, 115u) << "the stamp, not the tick it was applied at";
        ASSERT_EQ(s.pending_deaths_.size(), 1u);
        EXPECT_EQ(s.pending_deaths_[0].tick, 115u);

        // Player 5: stamped 105, made live at 121.
        auto& five = s.remotes_[5];
        five.entity = "Ball_0_BMMO_5";
        five.physicalized = five.navigation = true;
        five.born_tick = five.driven_from = 121;
        five.birth.tick = 105;
        five.birth_known = false;
        EXPECT_FALSE(steps{core}.remake_mirror_birth("Ball_0_BMMO_5", 121));
        EXPECT_EQ(s.late_births_kept_, 1u);
        EXPECT_FALSE(steps{core}.remake_mirror_birth("Ball_0_BMMO_5", 121));
        EXPECT_EQ(s.late_births_kept_, 1u) << "once per birth (review finding 4)";
    }

    // L-U8: a late birth at its own rewind is made at E from the queue; the
    // reconcile's queued events no replay made are made live once after the
    // rewind (settle_unreplayed_lives), a failed make in the replay queued
    // again for that; the pending births of other paths are not touched.
    TEST(SessionClientCoreLives, WindowEventsNoReplayMadeAreMadeLiveOnce) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        client_core_state::pending_birth birth;
        birth.player = 5;
        birth.tick = birth.origin = 120;
        birth.entity = "Ball_0_BMMO_5";
        birth.reconciled = true;
        client_core_state::pending_birth other = birth;
        other.player = 6;
        other.entity.clear();
        other.reconciled = false;   // a late birth's own (make_remote_birth)
        s.pending_births_ = {birth, other};
        client_core_state::pending_death death;
        death.player = 4;
        death.tick = 119;
        death.reconciled = true;
        live_mirror(core, 4, "Ball_0_BMMO_4", 100, true);
        s.pending_deaths_ = {death};
        // A failed make inside the replay: queued again.
        fake.make_mirror_ok = false;
        steps{core}.drain_pending_lives(120);
        fake.make_mirror_ok = true;
        ASSERT_EQ(s.pending_births_.size(), 1u) << "the reconciled one queued again, the other one dropped as before";
        EXPECT_TRUE(s.pending_births_[0].reconciled);
        size_t from = fake.calls.size();
        steps{core}.settle_unreplayed_lives(125);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(count_prefix(calls, "unmake_mirror:Ball_0_BMMO_4"), 1u);
        EXPECT_EQ(count_prefix(calls, "make_mirror:Ball_0_BMMO_5"), 1u);
        EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        EXPECT_LT(std::find(calls.begin(), calls.end(), "unmake_mirror:Ball_0_BMMO_4"),
                  std::find(calls.begin(), calls.end(), "make_mirror:Ball_0_BMMO_5:P#1")) << "stamp order";
        EXPECT_EQ(s.lives_settled_live_, 2u);
        EXPECT_TRUE(s.pending_births_.empty());
        EXPECT_TRUE(s.pending_deaths_.empty());
        EXPECT_TRUE(s.remotes_.at(5).physicalized);
        EXPECT_EQ(s.remotes_.at(5).born_tick, 126u) << "made live";
        from = fake.calls.size();
        steps{core}.settle_unreplayed_lives(125);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "once";
    }

    // L-U9: a mirror life born at E = 114 and dead at D' = 117, both inside
    // the window of A = 110, after the A-life's death at 112: no body now.
    // Made at 114, removed at 117; the A-life revived until 112.
    TEST(SessionClientCoreLives, ALifeInsideTheWindowIsMadeAndRemoved) {
        flow_fake fake;
        session_platform platform = fake.make_flow();
        platform.mirror_entity = [&fake](uint32_t player, uint8_t, std::string&) {
            fake.calls.push_back("mirror_entity:" + std::to_string(player));
            return std::string("Ball_0_BMMO_4#9");
        };
        session_client_core core(std::move(platform), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        const std::string one = "Ball_0_BMMO_4#1", two = "Ball_0_BMMO_4#2";
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, one, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 112, one, 2),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 114, two, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 117, two, 2)};
        live_mirror(core, 4, two, 114, false);
        s.mirror_entities_.insert(one);
        s.generations_ = {{one, 2}, {two, 2}};
        life_history h;
        h.span(110, 111, {{one, 7.0}}, {{one, 1}});
        h.span(112, 113, {}, {{one, 2}});
        h.span(114, 116, {{two, 3.0}}, {{one, 2}, {two, 1}});
        h.span(117, 125, {}, {{one, 2}, {two, 2}});
        h.record(core);
        std::map<std::string, bmmo::session::life_outcome> out;
        steps{core}.world_hooks().restore_lives(110, 125, {{one, 1, 112, 0, bmmo::session::life_kind::mirror}}, true, out);
        EXPECT_EQ(out[one].revived_until, 112u);
        EXPECT_EQ(out[two].remade_at, 114u);
        const size_t from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + one), 1u);
        EXPECT_EQ(count_prefix(calls, "make_mirror:" + two), 1u);
        EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + two), 1u);
        EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        EXPECT_EQ(s.generations_.at(one), 2u);
        EXPECT_EQ(s.generations_.at(two), 2u);
        EXPECT_FALSE(s.remotes_.at(4).physicalized);
        EXPECT_EQ(s.remotes_.at(4).entity, two);
    }

    // L-U9 with no A-life (review finding 1): player 4's mirror died at 105,
    // so nothing of it is alive at A = 110 and no life ends there; its next
    // life (#2) is born at E = 114 and dies at D' = 117, both inside the
    // window; no body now.  The engine asks with no ended life; the
    // reconcile queues the birth (with #2's name) and the death at their
    // stamps, so the replay makes #2 at 114 and removes it at 117, and the
    // generations end at the live values.
    TEST(SessionClientCoreLives, ALifeInsideTheWindowWithNoLifeAtTheAnchorIsMadeAndRemoved) {
        flow_fake fake;
        session_platform platform = fake.make_flow();
        platform.mirror_entity = [&fake](uint32_t player, uint8_t, std::string&) {
            fake.calls.push_back("mirror_entity:" + std::to_string(player));
            return std::string("Ball_0_BMMO_4#9");
        };
        session_client_core core(std::move(platform), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        const std::string one = "Ball_0_BMMO_4#1", two = "Ball_0_BMMO_4#2";
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, one, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 105, one, 2),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 114, two, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 117, two, 2)};
        live_mirror(core, 4, two, 114, false);
        s.mirror_entities_.insert(one);
        s.generations_ = {{one, 2}, {two, 2}};
        life_history h;
        h.span(110, 113, {}, {{one, 2}});
        h.span(114, 116, {{two, 3.0}}, {{one, 2}, {two, 1}});
        h.span(117, 125, {}, {{one, 2}, {two, 2}});
        h.record(core);
        std::map<std::string, bmmo::session::life_outcome> out;
        steps{core}.world_hooks().restore_lives(110, 125, {}, true, out);
        EXPECT_EQ(out.count(one), 0u) << "no life of #1 in the window";
        ASSERT_EQ(out.count(two), 1u);
        EXPECT_EQ(out[two].remade_at, 114u);
        EXPECT_EQ(out[two].revived_until, 0u);
        EXPECT_EQ(s.window_lives_, 1u);
        EXPECT_EQ(s.mirror_revive_failures_, 0u);
        EXPECT_EQ(s.generations_.count(two), 0u) << "absent at A";
        EXPECT_EQ(s.generations_.at(one), 2u);
        const size_t from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(count_prefix(calls, "make_mirror:" + two), 1u);
        EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + two), 1u);
        EXPECT_LT(std::find(calls.begin(), calls.end(), "make_mirror:" + two + ":P#0"),
                  std::find(calls.begin(), calls.end(), "unmake_mirror:" + two));
        EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        EXPECT_EQ(s.generations_.at(two), 2u) << "the live value";
        EXPECT_FALSE(s.remotes_.at(4).physicalized);
        EXPECT_TRUE(s.pending_births_.empty());
        EXPECT_TRUE(s.pending_deaths_.empty());
    }

    // The same on RC naming, with a body now (review findings 1 and 3):
    // player 4 (dead at A = 110) lives [114, 117) and again from 120 under
    // the one name; the body now is taken out and the name answered remade
    // at its LAST window birth (120, the life its restore item is about).
    // Player 5's body was made live at 121 for a birth stamped 115 (a
    // refused lifecycle rewind): replayed at its stamp (remade_at 115), not
    // parked and not counted kept.  Player 6's only window event is the
    // birth of its body now, made at its stamp: left to LB (no answer, the
    // body stays for destroy_body).
    TEST(SessionClientCoreLives, WindowLivesWithABodyNowAreTakenOutAndReplayedAtTheirStamps) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.rollback = true;
        core.config.lifecycle_rewind = true;
        auto& s = state_of(core);
        const std::string ball = "Ball_0_BMMO_4", five = "Ball_0_BMMO_5", six = "Ball_0_BMMO_6";
        auto late = life_entry_of(5, bmmo::session::event_type::Physicalize, 115, five, 1);
        late.made_at = 121;
        s.life_log_ = {life_entry_of(4, bmmo::session::event_type::Physicalize, 100, ball, 1),
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 105, ball, 2),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 114, ball, 3),
                       late,
                       life_entry_of(4, bmmo::session::event_type::Unphysicalize, 117, ball, 4),
                       life_entry_of(6, bmmo::session::event_type::Physicalize, 118, six, 1),
                       life_entry_of(4, bmmo::session::event_type::Physicalize, 120, ball, 5)};
        live_mirror(core, 4, ball, 120, true);
        live_mirror(core, 5, five, 121, true);
        s.remotes_.at(5).birth.tick = 115;
        s.remotes_.at(5).birth_known = false;
        live_mirror(core, 6, six, 118, true);
        s.generations_ = {{ball, 5}, {five, 1}, {six, 1}};
        life_history h;
        h.span(110, 113, {}, {{ball, 2}});
        h.span(114, 116, {{ball, 3.0}}, {{ball, 3}});
        h.span(117, 117, {}, {{ball, 4}});
        h.span(118, 119, {{six, 4.0}}, {{ball, 4}, {six, 1}});
        h.span(120, 120, {{ball, 9.0}, {six, 4.0}}, {{ball, 5}, {six, 1}});
        h.span(121, 125, {{ball, 9.0}, {six, 4.0}, {five, 6.0}}, {{ball, 5}, {six, 1}, {five, 1}});
        h.record(core);
        std::map<std::string, bmmo::session::life_outcome> out;
        size_t from = fake.calls.size();
        steps{core}.world_hooks().restore_lives(110, 125, {}, true, out);
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + ball), 1u);
            EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + five), 1u);
            EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + six), 0u) << "LB's";
        }
        EXPECT_EQ(out.count(six), 0u);
        ASSERT_EQ(out.count(ball), 1u);
        EXPECT_EQ(out[ball].remade_at, 120u) << "its last window birth";
        EXPECT_EQ(out[five].remade_at, 115u) << "the stamp, not the tick it was made at";
        EXPECT_EQ(s.window_lives_, 2u);
        EXPECT_EQ(s.generations_.at(ball), 2u) << "A's";
        EXPECT_EQ(s.generations_.count(five), 0u);
        EXPECT_TRUE(s.remotes_.at(6).physicalized);
        EXPECT_FALSE(steps{core}.remake_mirror_birth(five, 121)) << "taken out already";
        EXPECT_EQ(s.late_births_kept_, 0u) << "stamped inside the window: not the documented class";
        from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(count_prefix(calls, "make_mirror:" + ball), 2u);
            EXPECT_EQ(count_prefix(calls, "unmake_mirror:" + ball), 1u);
            EXPECT_EQ(count_prefix(calls, "make_mirror:" + five), 1u);
            EXPECT_EQ(count_prefix(calls, "mirror_entity:"), 0u);
        }
        EXPECT_EQ(s.generations_.at(ball), 5u) << "the live value";
        EXPECT_EQ(s.generations_.at(five), 1u) << "the live value";
        EXPECT_TRUE(s.remotes_.at(4).physicalized);
        EXPECT_TRUE(s.remotes_.at(5).physicalized);
        EXPECT_EQ(s.remotes_.at(5).born_tick, 115u) << "made at its stamp";
        EXPECT_TRUE(s.remotes_.at(5).birth_known);
    }

    // L-U10: the trim keeps the last history_ticks + 128 ticks and, of the
    // older entries, each player's newest Physicalize - so a player whose
    // A-life began before the horizon is still revived from it - and each
    // player's newest older entry of any kind (review finding 2: player 5's
    // death after its kept birth stays, so the reconcile's consistency test
    // still refuses that life).
    TEST(SessionClientCoreLives, TheTrimKeepsEachPlayersNewestOldBirth) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        auto& s = state_of(core);
        const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks) + 128u;
        const uint32_t late = 1000;
        const uint32_t old_tick = late - keep - 50;
        s.life_log_.clear();
        for (const auto& [player, type, tick]: std::vector<std::tuple<uint32_t, bmmo::session::event_type, uint32_t>>{
                     {4, bmmo::session::event_type::Physicalize, old_tick - 20},
                     {4, bmmo::session::event_type::Unphysicalize, old_tick - 10},
                     {4, bmmo::session::event_type::Physicalize, old_tick},
                     {5, bmmo::session::event_type::Physicalize, old_tick + 5},
                     {5, bmmo::session::event_type::Unphysicalize, old_tick + 6}}) {
            steps{core}.log_life(event_message(player, type, tick, tick));
        }
        steps{core}.log_life(event_message(6, bmmo::session::event_type::Physicalize, late, late));
        std::vector<std::pair<uint32_t, uint32_t>> left;
        for (const auto& e: s.life_log_) left.emplace_back(e.player, e.applied);
        EXPECT_EQ(left, (std::vector<std::pair<uint32_t, uint32_t>>{{4, old_tick}, {5, old_tick + 5}, {5, old_tick + 6}, {6, late}}));
        EXPECT_EQ(s.life_log_[0].birth.tick, old_tick) << "the Physicalize as it came";
    }

    // ---- gap L, L3: the own ball (rollback-engine gaps design 2.3.4, 9.7;
    // step EL8) ----

    // The matrices the fake's entity_matrix / set_entity_matrix read and
    // write, by entity, with the calls logged.
    struct own_matrices {
        std::map<std::string, std::array<float, 16>> m;
        void install(session_platform& p, std::vector<std::string>& calls) {
            p.entity_matrix = [this, &calls](const std::string& entity, float out[16]) {
                calls.push_back("entity_matrix:" + entity);
                const auto it = m.find(entity);
                if (it == m.end()) return false;
                std::copy(it->second.begin(), it->second.end(), out);
                return true;
            };
            p.set_entity_matrix = [this, &calls](const std::string& entity, const float in[16]) {
                calls.push_back("set_entity_matrix:" + entity + ":" + std::to_string(in[12]));
                std::copy(in, in + 16, m[entity].begin());
                return true;
            };
        }
        static std::array<float, 16> at(float x) {
            std::array<float, 16> a{};
            a[0] = a[5] = a[10] = a[15] = 1.0f;
            a[12] = x;
            return a;
        }
    };

    // Records of the own ball `entity` over [from, to]: tracked as the own
    // entity with a body at x, or (alive = false) bodiless; `generation`.
    void own_span(session_client_core& core, uint32_t from, uint32_t to, const std::string& entity, bool alive, double x,
                  uint32_t generation) {
        for (uint32_t t = from; t <= to; ++t) {
            bmmo::session::rollback_tracked tracked;
            if (alive) tracked.own_entity = entity;
            tracked.generations[entity] = generation;
            bmmo::session::rollback_world world;
            world.get_body = [entity, alive, x](const std::string& name, bmmo_physics_body_state& out) {
                if (!alive || name != entity) return false;
                out = bmmo_physics_body_state{};
                out.movable = out.simulated = true;
                out.position[0] = x;
                out.rotation[3] = 1.0;
                return true;
            };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            state_of(core).rollback_.record(world, t, tracked, {});
        }
    }

    session_capabilities own_replay_caps() {
        session_capabilities caps;
        caps.own_body_in_replay = true;
        return caps;
    }

    // L-U6: Ball_Wood alive at A = 110 (generation 1) dies at D = 115 (the
    // drain logged it with its matrix); now 125, no body.  The own half
    // revives it at the A record's pose through make_own (the own group, no
    // kick, with the replica), answers revived until 115 and removes it in
    // the replay's drain of 115 with the death's matrix (a replayed death).
    // The core's scope puts the bodiless entity's pre-rewind matrix back.
    // Without own_body_in_replay nothing is made and nothing answered (the
    // engine counts the life unrevivable).
    TEST(SessionClientCoreLives, AnOwnDeathInTheWindowIsRevivedAndRemovedAtItsTick) {
        flow_fake fake;
        own_matrices matrices;
        session_platform platform = fake.make_flow();
        matrices.install(platform, fake.calls);
        std::vector<float> unmade_x;
        platform.unmake_own = [&fake, &unmade_x](const std::string& entity, const float* position, const float*, uint32_t tick,
                                                 bool death, std::string&) {
            fake.calls.push_back("unmake_own:" + entity + ":" + std::to_string(tick) + (death ? ":death" : ""));
            unmade_x.push_back(position[0]);
            return true;
        };
        session_client_core core(std::move(platform), own_replay_caps());
        run_to_tick_125(core, fake);
        core.config.rollback = true;
        auto& s = state_of(core);
        const std::string wood = "Ball_Wood";
        s.own_ball_entities_ = {wood};
        s.own_body_entity_.clear();
        s.generations_ = {{wood, 2}};
        own_span(core, 110, 114, wood, true, 7.0, 1);
        own_span(core, 115, 125, wood, false, 0.0, 2);
        matrices.m[wood] = own_matrices::at(31.0f);   // after frame 115: the death sequence's
        steps{core}.note_own_death(wood, 115, 1);
        ASSERT_EQ(s.own_deaths_.size(), 1u);
        EXPECT_TRUE(s.own_deaths_[0].has_matrix);
        EXPECT_EQ(s.own_deaths_[0].matrix[12], 31.0f) << "the matrix after frame D";
        EXPECT_EQ(s.own_deaths_[0].generation, 1u);
        matrices.m[wood] = own_matrices::at(-20.0f);   // live, by now: the resetpoint

        bmmo::session::scope_guard scope = steps{core}.keep_live_cells();
        std::map<std::string, bmmo::session::life_outcome> out;
        size_t from = fake.calls.size();
        steps{core}.world_hooks().restore_lives(110, 125, {{wood, 1, 115, 0, bmmo::session::life_kind::own}}, true, out);
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(count_prefix(calls, "make_own:" + wood + ":" + s.own_group_ + ":110"), 1u);
            EXPECT_EQ(count_prefix(calls, "unmake_own:"), 0u) << "no later life to take out";
        }
        ASSERT_EQ(fake.own_specs.size(), 1u);
        EXPECT_EQ(fake.own_specs[0].position[0], 7.0f) << "the A record's pose";
        EXPECT_FALSE(fake.own_specs[0].kick) << "no kick at a revive";
        EXPECT_TRUE(fake.own_specs[0].with_replica);
        EXPECT_EQ(out[wood].revived_until, 115u);
        EXPECT_EQ(out[wood].remade_at, 0u);
        EXPECT_EQ(s.own_revives_, 1u);
        EXPECT_EQ(s.generations_.at(wood), 2u) << "untouched: the records carry the generations";
        from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        {
            const auto calls = since(fake.calls, from);
            const std::vector<std::string> death = {"mute_event_log:on", "unmake_own:" + wood + ":115:death",
                                                    "mute_event_log:off"};
            const auto at = std::search(calls.begin(), calls.end(), death.begin(), death.end());
            EXPECT_NE(at, calls.end()) << "the death replayed at 115, muted";
            EXPECT_EQ(count_prefix(calls, "unmake_own:"), 1u);
        }
        ASSERT_EQ(unmade_x.size(), 1u);
        EXPECT_EQ(unmade_x[0], 31.0f) << "at the death's matrix";
        EXPECT_TRUE(s.own_due_.empty());
        // The replay moved the bodiless entity; the scope puts it back.
        matrices.m[wood] = own_matrices::at(5.0f);
        from = fake.calls.size();
        { bmmo::session::scope_guard done = std::move(scope); }
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(count_prefix(calls, "set_entity_matrix:" + wood), 1u);
        }
        EXPECT_EQ(matrices.m[wood][12], -20.0f) << "the pre-rewind matrix";

        // Without the capability: nothing made, nothing answered.
        flow_fake plain;
        session_platform p2 = plain.make_flow();
        session_client_core core2(std::move(p2), session_capabilities{});
        run_to_tick_125(core2, plain);
        core2.config.rollback = true;
        state_of(core2).own_ball_entities_ = {wood};
        own_span(core2, 110, 114, wood, true, 7.0, 1);
        own_span(core2, 115, 125, wood, false, 0.0, 2);
        steps{core2}.note_own_death(wood, 115, 1);
        std::map<std::string, bmmo::session::life_outcome> out2;
        from = plain.calls.size();
        steps{core2}.world_hooks().restore_lives(110, 125, {{wood, 1, 115, 0, bmmo::session::life_kind::own}}, true, out2);
        EXPECT_EQ(count_prefix(since(plain.calls, from), "make_own:"), 0u);
        EXPECT_TRUE(out2.empty()) << "unanswered: the engine counts it unrevivable";
    }

    // L-U7 (H): the same name born again in the window - Ball_Wood alive at
    // A = 110 dies at 115 and is born again at E = 118 (alive now).  The
    // later life is taken out at the restore (not a replayed death), the
    // A-life revived until 115, and the later life remade at 118 by
    // remake_own_body (its Physicalize's pose and recipe, the spawn kick of
    // 118), after the death of 115.
    TEST(SessionClientCoreLives, AnOwnSameNameRebirthIsTakenOutAndRemadeAtItsStamp) {
        flow_fake fake;
        own_matrices matrices;
        session_platform platform = fake.make_flow();
        matrices.install(platform, fake.calls);
        session_client_core core(std::move(platform), own_replay_caps());
        run_to_tick_125(core, fake);
        core.config.rollback = true;
        auto& s = state_of(core);
        const std::string wood = "Ball_Wood";
        s.own_ball_entities_ = {wood};
        s.own_body_entity_ = wood;
        s.generations_ = {{wood, 3}};
        fake.bodies_there = {wood};
        own_span(core, 110, 114, wood, true, 7.0, 1);
        own_span(core, 115, 117, wood, false, 0.0, 2);
        own_span(core, 118, 125, wood, true, 40.0, 3);
        steps{core}.note_own_death(wood, 115, 1);
        client_core_state::own_life birth;
        birth.what = client_core_state::own_life::kind::birth;
        birth.origin = birth.applied = 118;
        birth.entity = wood;
        birth.generation = 3;
        birth.position[0] = 40.0f;
        birth.recipe.mass = 2.0f;
        birth.group = s.own_group_;
        birth.spawn = true;
        birth.origin_live = birth.applied_live = true;
        s.own_life_.push_back(birth);

        std::map<std::string, bmmo::session::life_outcome> out;
        size_t from = fake.calls.size();
        steps{core}.world_hooks().restore_lives(110, 125, {{wood, 1, 115, 118, bmmo::session::life_kind::own}}, true, out);
        {
            const auto calls = since(fake.calls, from);
            const auto out_at = std::find(calls.begin(), calls.end(), "unmake_own:" + wood + ":110");
            const auto made_at = std::find_if(calls.begin(), calls.end(),
                                              [](const std::string& c) { return c.rfind("make_own:", 0) == 0; });
            ASSERT_NE(out_at, calls.end()) << "the later life taken out at the restore (no replayed death)";
            ASSERT_NE(made_at, calls.end());
            EXPECT_LT(out_at - calls.begin(), made_at - calls.begin()) << "taken out before the revive";
        }
        EXPECT_EQ(out[wood].revived_until, 115u);
        EXPECT_EQ(out[wood].remade_at, 118u);
        EXPECT_EQ(s.own_window_remakes_, 1u);
        ASSERT_EQ(s.own_due_.size(), 2u);
        EXPECT_TRUE(s.own_due_[0].death);
        EXPECT_EQ(s.own_due_[0].tick, 115u);
        EXPECT_FALSE(s.own_due_[1].death);
        EXPECT_EQ(s.own_due_[1].tick, 118u);
        from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        {
            const auto calls = since(fake.calls, from);
            const auto death = std::find(calls.begin(), calls.end(), "unmake_own:" + wood + ":115:death");
            const auto remake = std::find_if(calls.begin(), calls.end(),
                                             [&wood](const std::string& c) { return c.rfind("make_own:" + wood, 0) == 0; });
            ASSERT_NE(death, calls.end());
            ASSERT_NE(remake, calls.end());
            EXPECT_LT(death - calls.begin(), remake - calls.begin());
        }
        ASSERT_EQ(fake.own_specs.size(), 2u);
        EXPECT_EQ(fake.own_specs[1].tick, 118u);
        EXPECT_EQ(fake.own_specs[1].position[0], 40.0f) << "its Physicalize's pose";
        EXPECT_TRUE(fake.own_specs[1].kick) << "the spawn kick of its stamp";
        EXPECT_FALSE(fake.own_specs[1].with_replica);
        EXPECT_TRUE(s.own_due_.empty());
    }

    // L-U9 (own half): an own life born at 117 and dead at 121, both inside
    // the window of A = 110 (no record of it at A, no body now): remade at
    // 117 and removed at 121 in the replay, answered remade_at = 117.
    TEST(SessionClientCoreLives, AnOwnLifeInsideTheWindowIsMadeAndRemoved) {
        flow_fake fake;
        own_matrices matrices;
        session_platform platform = fake.make_flow();
        matrices.install(platform, fake.calls);
        session_client_core core(std::move(platform), own_replay_caps());
        run_to_tick_125(core, fake);
        core.config.rollback = true;
        auto& s = state_of(core);
        const std::string paper = "Ball_Paper";
        s.own_ball_entities_ = {paper};
        own_span(core, 110, 116, paper, false, 0.0, 0);
        own_span(core, 117, 120, paper, true, 3.0, 1);
        own_span(core, 121, 125, paper, false, 0.0, 2);
        client_core_state::own_life birth;
        birth.what = client_core_state::own_life::kind::birth;
        birth.origin = birth.applied = 117;
        birth.entity = paper;
        birth.generation = 1;
        birth.origin_live = birth.applied_live = true;
        s.own_life_.push_back(birth);
        steps{core}.note_own_death(paper, 121, 1);
        std::map<std::string, bmmo::session::life_outcome> out;
        steps{core}.world_hooks().restore_lives(110, 125, {}, true, out);
        EXPECT_EQ(out[paper].remade_at, 117u);
        EXPECT_EQ(out[paper].revived_until, 0u);
        size_t from = fake.calls.size();
        for (uint32_t t = 111; t <= 125; ++t) steps{core}.drain_pending_lives(t);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(count_prefix(calls, "make_own:" + paper), 1u);
        EXPECT_EQ(count_prefix(calls, "unmake_own:" + paper + ":121:death"), 1u);
        EXPECT_TRUE(s.own_due_.empty());
    }

    // L-U11: a beam stamped T = 115 inside a life that dies at 117, the
    // window over both (the live generation is the dead life's).  A replay
    // applies the beam at 115 (the record of 114 is its life's) and never
    // retires it; a replay in which the record of 114 names another life
    // skips it, still without retiring; only a live frame retires.
    TEST(SessionClientCoreLives, ABeamInsideALifeThatDiesLaterIsAppliedInTheReplay) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        core.config.rollback = true;
        auto& s = state_of(core);
        const std::string wood = "Ball_Wood";
        s.generations_ = {{wood, 2}};   // the death at 117 bumped it
        own_span(core, 110, 116, wood, true, 7.0, 1);
        own_span(core, 117, 125, wood, false, 0.0, 2);
        client_core_state::own_life beam;
        beam.what = client_core_state::own_life::kind::beam;
        beam.origin = beam.applied = 115;
        beam.entity = wood;
        beam.generation = 1;
        beam.position[0] = 12.0f;
        beam.origin_live = beam.applied_live = true;
        s.own_life_.push_back(beam);
        size_t from = fake.calls.size();
        steps{core}.drain_pending_lives(115);
        EXPECT_EQ(count_prefix(since(fake.calls, from), "write_body:" + wood + ":12"), 1u) << "applied in the replay";
        ASSERT_EQ(s.own_life_.size(), 1u);
        EXPECT_FALSE(s.own_life_[0].retired) << "a replay never retires";
        // A life the record of 114 does not have: not applied, not retired.
        s.own_life_[0].generation = 5;
        from = fake.calls.size();
        steps{core}.drain_pending_lives(115);
        EXPECT_EQ(count_prefix(since(fake.calls, from), "write_body:"), 0u);
        EXPECT_FALSE(s.own_life_[0].retired);
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
                "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay", "step", "step", "step", "now:rewind_end",
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

    // Step RC3 (rewind-cost design 3): with the session's snapshots off
    // (SessionStart's interval byte 255), the full snapshot of a body-set
    // change is no correction - nothing compared, nothing restored, its
    // mechanism names kept for the registry, the first few logged - while
    // the same snapshot with snapshots on, or with the pre-RC3 switch
    // (--body-set-corrections, the control arm), rewinds as before.  The
    // default is RC3's (step B5RX: work as if snapshots did not exist).
    TEST(SessionClientCoreSnapshot, ASnapshotsOffSessionTakesABodySetSnapshotForItsNamesOnly) {
        EXPECT_FALSE(bmmo::session::core_config{}.body_set_corrections) << "snapshot-free by default";
        struct arm {
            uint8_t interval;
            bool corrections;
            bool rolls;
        };
        for (const arm a: {arm{255, false, false}, arm{255, true, true}, arm{2, false, true}}) {
            SCOPED_TRACE("interval " + std::to_string(a.interval) + (a.corrections ? ", --body-set-corrections" : ""));
            flow_fake fake;
            fake.driven = "Ball_Wood";
            auto platform = fake.make_flow();
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
            core.config.body_set_corrections = a.corrections;
            state_of(core).snapshot_interval_ = a.interval;
            state_of(core).own_physicalized_ = true;
            bmmo::session::rollback_tracked tracked;
            tracked.own_entity = "Ball_Wood";
            for (uint32_t tick = 118; tick <= 125; ++tick)
                state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, tracked, {});
            const size_t from = fake.calls.size();
            // The server had the own ball 5 m away at tick 122: a breach.
            core.on_session_snapshot(snapshot_message(9, 122, true, {ball_row(3, 5.0, true)}));
            EXPECT_EQ(state_of(core).snapshots_applied_, 1u);
            EXPECT_EQ(state_of(core).last_snapshot_tick_, 122u);
            EXPECT_EQ(state_of(core).snapshot_rollbacks_, a.rolls ? 1u : 0u);
            EXPECT_EQ(state_of(core).rollback_.stats().snapshots, a.rolls ? 1u : 0u) << "compared at all";
            EXPECT_EQ(state_of(core).snapshots_uncorrected_, a.rolls ? 0u : 1u);
            const auto calls = since(fake.calls, from);
            const bool logged = std::find(calls.begin(), calls.end(),
                                          "log:0:snapshot of tick 122 (full, 1 rows) taken for its names only: the session's "
                                          "snapshots are off (step RC3)") != calls.end();
            EXPECT_EQ(logged, !a.rolls);
            EXPECT_EQ(std::find(calls.begin(), calls.end(), "keep_live_cells:exit:rolled back") != calls.end(), a.rolls);
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
            "log:0:session 9 ended: ", "hook:ending", kEndLivesLine, "hook:ended",
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
                if (core) steps{*core}.drain_pending_lives(tick);
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

    // Design 5.1 "Beam", without own body control: the record kept, and its
    // stamp one write_body live at T+1 and one in a forced replay that
    // crosses T+1.  The generation is not bumped (rollback-engine gaps
    // design 2.4, B-U1, step B8EG item EGB): a beam is a move within one
    // life.  Without a navigation replica the ball is moved at once,
    // nothing is kept and the generation is bumped (the server's new
    // lifetime, which no replay redoes).
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
        EXPECT_EQ(state_of(core).generations_.count("Ball_Wood"), 0u) << "not bumped at the request: one life";
        ASSERT_EQ(state_of(core).own_life_.size(), 1u);
        EXPECT_EQ(state_of(core).own_life_[0].generation, 0u);
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
                "keep_live_cells:enter:126", "bind_world", "now:rewind_start", "now:rewind_replay",
                "pre_step:124", "step", "pre_step:125", "step",
                "pre_step:126", "write_body:Ball_Wood:711.000000:wake", "step",
                "now:rewind_end", "keep_live_cells:exit:rolled back"};
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
        EXPECT_EQ(state_of(core).generations_.at("Ball_Wood"), 1u) << "the in-place move: a new lifetime";
        EXPECT_TRUE(state_of(core).own_life_.empty());
    }

    // Late-player plan step 12: a held notice of our tick 130 that arrives at
    // 180 needs a rewind 51 deep.  At the fixed reach of 48 it is late and
    // resyncs; once the relay lag measured 52 (reach 60) it is a notice like
    // any other, and nothing resyncs.
    TEST(SessionClientCoreRelay, HeldNoticeInsideThePerClientReachIsNoResync) {
        for (const bool raised: {false, true}) {
            SCOPED_TRACE(raised ? "reach 60" : "reach 48");
            flow_fake fake;
            session_client_core* bound = nullptr;
            session_client_core core(replay_platform(fake, bound), session_capabilities{});
            bound = &core;
            run_to_tick_125(core, fake);
            auto& st = state_of(core);
            st.rollback_.rewind_cost().sample(1.0, 0.1 * 48, 48);   // step R12: a cap of 120
            st.held_floor_ = 120;
            st.frames_since_anchor_ = 61;
            ASSERT_EQ(core.current_tick(), 180u);
            record_ticks(core, 100, 180);
            if (raised) core.on_session_remote_input(relay_message(128, 120));   // one sample: lag 52
            ASSERT_EQ(st.rollback_.thresholds().max_resim_ticks, raised ? 60u : 48u);
            const uint64_t resyncs_before = st.resyncs_sent_;
            steps{core}.settle_held_tick(130, bmmo::session::input_frame{});
            EXPECT_EQ(st.subst_late_, raised ? 0u : 1u);
            EXPECT_EQ(st.subst_recv_, raised ? 1u : 0u);
            EXPECT_EQ(st.resyncs_sent_ - resyncs_before, raised ? 0u : 1u);
        }
    }

    // Resync design 9 Q2 (step A4): a forced rewind deeper than
    // max_resim_ticks (48) re-steps, up to the resync reach (history_ticks -
    // 1 = 95), as an exact resync does; one whose anchor has left the
    // history is a counted miss that steps nothing.
    TEST(SessionClientCoreForcedRewind, DeeperThanMaxResimReStepsUpToTheResyncReach) {
        flow_fake fake;
        session_client_core* bound = nullptr;
        session_client_core core(replay_platform(fake, bound), session_capabilities{});
        bound = &core;
        run_to_tick_125(core, fake);
        ASSERT_EQ(state_of(core).rollback_.thresholds().max_resim_ticks, 48u);
        ASSERT_EQ(state_of(core).rollback_.resync_reach(), 95u);
        record_ticks(core, 30, 125);   // the whole history: 96 records

        size_t from = fake.calls.size();
        {
            const auto forced = core.force_rewind(80);
            EXPECT_TRUE(forced.rolled);
        }
        {
            const auto calls = since(fake.calls, from);
            const auto steps_taken = std::count(calls.begin(), calls.end(), std::string("step"));
            EXPECT_EQ(steps_taken, 80) << "anchor 45, ticks 46..125 re-simulated";
            ASSERT_GE(calls.size(), 6u);
            EXPECT_EQ(calls[2], "now:rewind_start");   // step R12: the cap's clock reads
            EXPECT_EQ(calls[3], "now:rewind_replay");
            EXPECT_EQ(calls[4], "pre_step:46");
            EXPECT_EQ(calls.back(), "keep_live_cells:exit:rolled back");
        }
        EXPECT_EQ(state_of(core).forced_rollbacks_, 1u);
        EXPECT_EQ(state_of(core).forced_rollback_depth_, 80u);
        EXPECT_EQ(state_of(core).forced_rollbacks_missed_, 0u);
        EXPECT_EQ(state_of(core).rollback_.stats().too_far, 0u);

        // Past the reach: the anchor (29) is no longer recorded, so the
        // engine refuses it before any depth check; nothing is stepped.
        const uint64_t unmatched = state_of(core).rollback_.stats().unmatched;
        from = fake.calls.size();
        {
            const auto forced = core.force_rewind(96);
            EXPECT_FALSE(forced.rolled);
        }
        {
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(std::count(calls.begin(), calls.end(), std::string("step")), 0);
        }
        EXPECT_EQ(state_of(core).forced_rollbacks_, 1u);
        EXPECT_EQ(state_of(core).forced_rollbacks_missed_, 1u);
        EXPECT_EQ(state_of(core).rollback_.stats().unmatched, unmatched + 1);
        EXPECT_EQ(state_of(core).rollback_.stats().too_far, 0u);
    }

    // Design 5.1 "Own echoes": a MOVED birth, with own body control, is
    // taken out, moved in the records and rewound for, and made again at
    // its applied tick inside the replay; the restore's destroy_body takes
    // a moved birth out (the live cells kept until the move's last line, as
    // the headless client kept them).  Without the control: nothing when snapshots run,
    // a resync when they do not, and the restore's branch is inert.
    // Rollback-engine gaps design 2.4 (B-U2's core half, step B8EG item
    // EGB): the core hooks bind_session_world builds keeps_parked from
    // (RollbackBinding.KeepsParkedIsDestroyBodysTestsWithoutItsSideEffects)
    // answer what destroy_body refuses by design - the own ball's birth and
    // any other own ball entity are balls, a mirror is a mirror - and
    // own_birth_destroyable answers exactly when destroy_own_birth acts: a
    // moved own birth at its applied tick, with own body control only.
    TEST(SessionClientCoreOwnLives, OwnBirthDestroyableIsDestroyOwnBirthsTest) {
        for (const bool control: {false, true}) {
            flow_fake fake;
            session_client_core* bound = nullptr;
            session_capabilities caps;
            caps.own_body_control = control;
            session_client_core core(replay_platform(fake, bound), caps);
            bound = &core;
            run_to_tick_125(core, fake);
            const auto& hooks = steps{core}.world_hooks();
            ASSERT_TRUE(hooks.own_birth_destroyable);
            ASSERT_TRUE(hooks.ball);
            ASSERT_TRUE(hooks.own_body);
            ASSERT_TRUE(hooks.mirror);
            // bind_session_world's keeps_parked, over the core's hooks.
            const auto keeps_parked = [&hooks](const std::string& entity, uint32_t tick) {
                if (hooks.own_birth_destroyable(entity, tick)) return false;
                if (hooks.mirror(entity)) return false;
                return hooks.ball(entity) || hooks.own_body(entity);
            };
            state_of(core).own_body_entity_ = "Ball_Wood";
            state_of(core).own_ball_entities_ = {"Ball_Wood", "Ball_Stone"};
            state_of(core).remotes_[4].entity = "Ball_0_BMMO_4";
            EXPECT_TRUE(keeps_parked("Ball_Wood", 122)) << "the own ball's birth the scripts made";
            EXPECT_TRUE(keeps_parked("Ball_Stone", 122)) << "an entity the own ball has been";
            EXPECT_FALSE(keeps_parked("Ball_0_BMMO_4", 122)) << "a mirror: the core makes it again";
            EXPECT_FALSE(keeps_parked("P_Box", 122)) << "a mechanism";
            state_of(core).own_life_ = {own_birth(120)};
            state_of(core).own_life_[0].applied = 122;   // moved: made again at 122 in the replay
            EXPECT_EQ(keeps_parked("Ball_Wood", 122), !control) << "a moved own birth is destroyed with own body control";
            EXPECT_TRUE(keeps_parked("Ball_Wood", 121)) << "another birth tick";
            EXPECT_EQ(hooks.own_birth_destroyable("Ball_Wood", 122), control);
            EXPECT_FALSE(hooks.own_birth_destroyable("Ball_Wood", 121));
            bool destroyed = false;
            EXPECT_FALSE(steps{core}.destroy_moved_own_birth("Ball_Wood", 121, destroyed)) << "the same answer at 121";
            if (!control) EXPECT_FALSE(steps{core}.destroy_moved_own_birth("Ball_Wood", 122, destroyed));
        }
    }

    // Step OWNB (SFD (A)): an own birth the scripts made at its stamp (not
    // moved) inside a rewind's window is taken out at the restore and made
    // again at its birth tick by the replay's drain (own_due), the way the
    // server makes its clone - with own body control and without
    // own_births_parked; otherwise it keeps the parking path.  A birth of
    // another tick, a retired or a later-generation life, is not one.
    TEST(SessionClientCoreOwnLives, ScriptedOwnBirthInsideTheWindowIsRemade) {
        for (const bool control: {false, true})
            for (const bool parked: {false, true}) {
                SCOPED_TRACE(std::string(control ? "control" : "no control") + (parked ? ", own_births_parked" : ""));
                flow_fake fake;
                session_client_core* bound = nullptr;
                session_capabilities caps;
                caps.own_body_control = control;
                session_client_core core(replay_platform(fake, bound), caps);
                bound = &core;
                run_to_tick_125(core, fake);
                core.config.own_births_parked = parked;   // after begin(), which takes the session's
                const auto& hooks = steps{core}.world_hooks();
                ASSERT_TRUE(hooks.own_birth_destroyable);
                state_of(core).own_body_entity_ = "Ball_Wood";
                state_of(core).own_ball_entities_ = {"Ball_Wood"};
                state_of(core).own_life_ = {own_birth(122)};
                const bool remade = control && !parked;
                EXPECT_EQ(hooks.own_birth_destroyable("Ball_Wood", 122), remade);
                EXPECT_FALSE(hooks.own_birth_destroyable("Ball_Wood", 121)) << "another birth tick";
                EXPECT_FALSE(hooks.own_birth_destroyable("Ball_Stone", 122)) << "another entity";
                state_of(core).own_due_.clear();
                const size_t from = fake.calls.size();
                bool destroyed = false;
                EXPECT_EQ(steps{core}.destroy_moved_own_birth("Ball_Wood", 122, destroyed), remade);
                if (remade) {
                    EXPECT_TRUE(destroyed);
                    const auto calls = since(fake.calls, from);
                    EXPECT_NE(std::find(calls.begin(), calls.end(), std::string("unmake_own:Ball_Wood:125")), calls.end());
                    ASSERT_EQ(state_of(core).own_due_.size(), 1u) << "queued for the replay's drain";
                    EXPECT_EQ(state_of(core).own_due_[0].tick, 122u);
                    EXPECT_FALSE(state_of(core).own_due_[0].death);
                    EXPECT_EQ(state_of(core).own_due_[0].life.entity, "Ball_Wood");
                    EXPECT_EQ(state_of(core).own_births_remade_, 1u);
                    EXPECT_TRUE(state_of(core).own_due_[0].scripted);
                    ASSERT_TRUE(hooks.own_birth_remade);
                    EXPECT_TRUE(hooks.own_birth_remade("Ball_Wood")) << "kept as the replay steps it (birth_from_inputs)";
                    EXPECT_FALSE(hooks.own_birth_remade("Ball_Stone"));
                    // The replay's drain makes it again at its birth tick, not before.
                    const size_t drain = fake.calls.size();
                    steps{core}.drain_pending_lives(121);
                    EXPECT_EQ(count_prefix(since(fake.calls, drain), "make_own:"), 0u) << "not before its birth tick";
                    EXPECT_EQ(state_of(core).own_due_.size(), 1u);
                    steps{core}.drain_pending_lives(122);
                    const auto made = since(fake.calls, drain);
                    EXPECT_EQ(count_prefix(made, "make_own:Ball_Wood:P#2:122"), 1u) << "made once, at 122";
                    EXPECT_EQ(count_prefix(made, "make_own:"), 1u);
                    EXPECT_TRUE(state_of(core).own_due_.empty()) << "drained";
                    EXPECT_FALSE(hooks.own_birth_remade("Ball_Wood")) << "nothing queued any more";
                } else {
                    EXPECT_TRUE(state_of(core).own_due_.empty());
                    if (hooks.own_birth_remade) EXPECT_FALSE(hooks.own_birth_remade("Ball_Wood"));
                }
                // A retired life, and a life of an earlier generation, are not.
                state_of(core).own_life_[0].retired = true;
                EXPECT_FALSE(hooks.own_birth_destroyable("Ball_Wood", 122));
                state_of(core).own_life_[0].retired = false;
                state_of(core).generations_["Ball_Wood"] = 3;
                EXPECT_FALSE(hooks.own_birth_destroyable("Ball_Wood", 122)) << "a death or a trafo since";
            }
    }

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
                "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay",
                "pre_step:122", "step", "pre_step:123", "step",
                "pre_step:124", "mute_event_log:on", "make_own:Ball_Wood:P#2:124", "mute_event_log:off",
                "log:0:own body: Ball_Wood made at (5.000,0.000,0.000) before tick 124 (replay, moved birth)", "step",
                "pre_step:125", "step", "now:rewind_end",
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
                "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay",
                "pre_step:122", "step", "pre_step:123", "step", "pre_step:124", "step", "pre_step:125", "step", "now:rewind_end",
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
                        "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay",
                        "pre_step:122", "step", "pre_step:123", "step", "pre_step:124", "step", "pre_step:125", "step", "now:rewind_end",
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
        open_gate(core);   // after the level's own walk (step S4)
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

    // Step B2SX (game/sector_cell_split.hpp): a host whose checkpoint chain
    // writes a private [0][1] (sector_poll::cell_private) - no pass writes
    // that cell, so the poll compares it with what the polls saw, never with
    // a pass's sector: the own crossing of 2 in the frame of the pass of 3
    // (the [0][1] cell's race) is reported at its own poll, a pass changes
    // nothing for the poll, and an own crossing into a sector a peer started
    // is seen without a moved resetpoint.
    TEST(SessionClientCoreSectors, PrivateChainCellIsComparedWithThePollsNotThePasses) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        open_gate(core);
        const auto private_poll = [](int cell) {
            bmmo::session::sector_poll poll = cell_poll(cell, true);
            poll.cell_private = true;
            return poll;
        };
        EXPECT_EQ(state_of(core).chain_cell_, 0);
        core.request_sector(3, "sector verb");   // stamped 128
        for (uint32_t tick = 120; tick <= 128; ++tick) steps{core}.sector_union_pass(tick);
        EXPECT_EQ(state_of(core).sector_cell_, 3);
        EXPECT_EQ(state_of(core).chain_cell_, 0) << "a pass never writes the chain's cell";
        // The pass's frame, the chain's cell untouched: nothing.
        fake.poll = private_poll(0);
        size_t from = fake.calls.size();
        steps{core}.poll_own_sector(128);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:128"});
        // The own crossing of 2 in the frame of that pass: the chain wrote its
        // own cell, the manager read the pass's 3; the crossing is ours.
        fake.poll = private_poll(2);
        from = fake.calls.size();
        steps{core}.poll_own_sector(129);
        {
            const std::vector<std::string> expected = {
                "poll_sector:129", "send:1",
                "log:0:sector 2 stamped for tick 137 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).chain_cell_, 2);
        EXPECT_EQ(state_of(core).sector_cell_, 3);
        // A peer's pass of 5, and our 2's own pass at its stamp: the poll
        // still sees 2, which it has seen.
        core.on_session_event(sector_message(4, 5, 130, 130));
        steps{core}.drain_due_events(130);
        for (uint32_t tick = 130; tick <= 140; ++tick) steps{core}.sector_union_pass(tick);
        EXPECT_EQ(state_of(core).sectors_.active(), std::set<int>({1, 2, 3, 5}));
        EXPECT_EQ(state_of(core).chain_cell_, 2);
        from = fake.calls.size();
        steps{core}.poll_own_sector(140);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:140"});
        // Our own crossing into 5: the chain writes 5 over its 2 - seen.
        fake.poll = private_poll(5);
        from = fake.calls.size();
        steps{core}.poll_own_sector(141);
        {
            const std::vector<std::string> expected = {
                "poll_sector:141", "send:1",
                "log:0:sector 5 stamped for tick 149 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sectors_requested_, std::set<int>({1, 2, 3, 5}));
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
        open_gate(core);   // after the level's own walk (step S4)
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
                "movable_bodies", "note_script_resets:126", "bind_world", "hook:after_record:126", "send:2",
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

    // Step A5: a late joiner's wakeups before its apply are its catch-up
    // passes', no event of the shared world: not sent (withheld, counted,
    // logged once).  Any other frame's are BodyRevived as before.
    TEST(SessionClientCoreFrame, ALateJoinersWakeupsBeforeTheApplyAreWithheld) {
        for (const bool joining: {true, false}) {
            journal_dir dir(joining ? "join_wake_on" : "join_wake_off");
            flow_fake fake;
            fake.path = dir.path / "client.bmjr";
            session_capabilities caps;
            caps.own_body_control = true;
            session_client_core core(frame_platform(fake, true), caps);
            run_to_tick_125_four(core, fake);
            state_of(core).resync_join_ = joining;
            state_of(core).resync_pending_ = joining;
            bmmo::session::body_event wakeup;
            wakeup.what = bmmo::session::body_event::kind::wakeup;
            wakeup.name = "P_Modul_29_Platte06";
            fake.body_events = {wakeup};
            const uint64_t sent = state_of(core).events_sent_;
            core.pre_step();
            const size_t from = fake.calls.size();
            ASSERT_TRUE(core.post_step(true));
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(state_of(core).events_sent_ - sent, joining ? 0u : 1u) << joining;
            EXPECT_EQ(state_of(core).join_wakes_withheld_, joining ? 1u : 0u) << joining;
            EXPECT_EQ(std::find(calls.begin(), calls.end(),
                                "log:0:late join: wakeups before the apply are not reported (P_Modul_29_Platte06 at tick 126 first)")
                          != calls.end(),
                      joining);
        }
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

    // Step RC1 (docs/rewind-cost-design.md §2): the replay re-predicts a
    // remote's tail with its newest relayed frame, the live predictor's.  The
    // live run predicted keys 0 for player 4 at 118-125; the relay brings keys
    // 7 at 121: a correction, and the replay from 120 re-records 122-125 with
    // keys 7 (hold-last), so the next arrival repeating it (122) is no news
    // and asks for no rewind, while a different one (123, keys 3) does.  The
    // live predictor and the replay are one rule: past the newest relayed
    // frame recorded_input_at returns live_drive's frame; a hole keeps the
    // nearest earlier frame; before the first relayed frame there is none.
    TEST(SessionClientCoreFrame, TheReplayHoldsTheNewestRelayedFrameSoARepeatIsNoNews) {
        flow_fake fake;
        session_client_core core(frame_platform(fake, false), session_capabilities{});
        run_to_tick_125(core, fake);
        core.config.input_rewind = true;
        state_of(core).held_floor_ = 117;
        auto& four = state_of(core).remotes_[4];
        four.entity = "Ball_Wood_BMMO_4";
        four.physicalized = true;
        four.navigation = true;
        for (uint32_t tick = 118; tick <= 125; ++tick) {
            bmmo::session::rollback_tracked tracked;
            tracked.remote_entities = {"Ball_Wood_BMMO_4"};
            state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick, tracked,
                                            {{"Ball_Wood_BMMO_4", keys_frame(0)}});
        }
        bmmo::session::input_frame out{};
        EXPECT_FALSE(steps{core}.recorded_input_at("Ball_Wood_BMMO_4", 125, out)) << "no relayed frame yet";
        const auto relay = [&core](uint32_t tick, uint8_t keys) {
            bmmo::session_remote_input_msg msg = relay_message(tick, 120);
            bmmo::session_remote_input_msg::entry entry;
            entry.player = 4;
            entry.frames = {keys_frame(keys)};
            msg.entries = {entry};
            core.on_session_remote_input(msg);
        };
        relay(121, 7);
        EXPECT_TRUE(state_of(core).input_rewind_pending_) << "keys 7 against the predicted 0";
        EXPECT_EQ(state_of(core).input_rewind_tick_, 121u);
        // One rule: the tail is the live predictor's frame.
        for (uint32_t tick = 122; tick <= 126; ++tick) {
            ASSERT_TRUE(steps{core}.recorded_input_at("Ball_Wood_BMMO_4", tick, out)) << tick;
            const auto live = steps::live_drive(four, tick);
            EXPECT_EQ(out.keys, live.keys) << tick;
            EXPECT_EQ(out.flags, live.flags) << tick;
            EXPECT_EQ(out.keys, 7) << tick;
        }
        EXPECT_FALSE(steps{core}.recorded_input_at("Ball_Wood_BMMO_4", 120, out)) << "before the first relayed frame";
        const size_t from = fake.calls.size();
        steps{core}.replay_corrected_inputs();
        int stepped = 0;
        for (const auto& call: since(fake.calls, from)) stepped += call == "step";
        EXPECT_EQ(stepped, 5) << "one replay from 120 to 125";
        EXPECT_FALSE(state_of(core).input_rewind_pending_);
        for (uint32_t tick = 121; tick <= 125; ++tick) {
            bmmo::session::input_frame recorded{};
            ASSERT_TRUE(state_of(core).rollback_.recorded_input("Ball_Wood_BMMO_4", tick, recorded)) << tick;
            EXPECT_EQ(recorded.keys, 7) << "re-captured with the newest relayed frame, tick " << tick;
        }
        relay(122, 7);
        EXPECT_FALSE(state_of(core).input_rewind_pending_) << "a repeat of the hold-last frame is no correction";
        relay(124, 3);
        EXPECT_TRUE(state_of(core).input_rewind_pending_) << "a different frame is";
        EXPECT_EQ(state_of(core).input_rewind_tick_, 124u);
        // 123 is now a hole between 122 and 124: the nearest earlier frame.
        ASSERT_TRUE(steps{core}.recorded_input_at("Ball_Wood_BMMO_4", 123, out));
        EXPECT_EQ(out.keys, 7);
        ASSERT_TRUE(steps{core}.recorded_input_at("Ball_Wood_BMMO_4", 125, out));
        EXPECT_EQ(out.keys, 3);
        EXPECT_EQ(out.keys, steps::live_drive(four, 125).keys);
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
        const std::string tail_text = "input: reports=4 starved=1 (25%) worst=0 lead=0.0 reach=48/48/96 cap=48/0.00/0.000/0 wait=0";
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
                  "late=0 covered=0 subst=3/0/0/0/0/0 substrew=0/0 substco=0/0/0/0 lead_adj=0 lead=-2 slew=0 moved=0 "
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
    // made at the drain, at the live tick, with no rewind; each birth from
    // its event (kicked).  (R3 deleted mirror_start_from_row, the retail
    // policy that started such a birth from its player's newer snapshot row:
    // a newer row changes nothing now.)
    TEST(SessionClientCoreR1, LifecycleRewindOffMakesLateChangesLive) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125_four(core, fake);
        core.config.lifecycle_rewind = false;
        state_of(core).spawn_impulse_ = 3.0f;
        core.on_session_snapshot(snapshot_message(9, 124, false, {ball_row(4, 7.0, true)}));
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
        EXPECT_TRUE(fake.specs[0].kick) << "the event's spawn kick, a newer row or not";
        EXPECT_FLOAT_EQ(fake.specs[0].position[0], 1.0f) << "the event's pose, not the row's";
        EXPECT_EQ(fake.specs[1].player, 5u);
        EXPECT_TRUE(fake.specs[1].kick) << "the event's spawn kick";
        EXPECT_FLOAT_EQ(fake.specs[1].position[0], 2.0f);
        EXPECT_EQ(std::count_if(calls.begin(), calls.end(), [](const std::string& c) { return c.rfind("write_body:", 0) == 0; }), 0)
            << "no mirror is written from a row";
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

    // R3: own_body_backstop beside the acted-on own echoes.  While an own
    // birth still waits for its echo (verdict sent) the backstop does not
    // count: the echo or its deadline answers it (expire_own_echoes), and a
    // backstop re-report beside it would be a second Physicalize at the
    // server.  Once the birth is answered the backstop counts from scratch;
    // with the own echoes off (R1, R2) a pending birth gates nothing.
    TEST(SessionClientCoreR1, OwnBodyBackstopWaitsForAPendingOwnEcho) {
        for (const bool echoes: {true, false}) {
            SCOPED_TRACE(echoes ? "own echoes on" : "own echoes off");
            flow_fake fake;
            session_client_core core(fake.make_flow(), session_capabilities{});
            run_to_tick_125(core, fake);
            core.config.own_body_backstop = true;
            core.config.own_echoes = echoes;
            state_of(core).own_physicalized_ = true;
            fake.own_physical = true;
            state_of(core).own_last_birth_.valid = true;
            state_of(core).own_last_birth_.ball_type = 1;
            state_of(core).own_life_ = {own_birth(122)};
            uint32_t tick = 100;
            for (int i = 0; i < 40; ++i) steps{core}.check_own_body(snapshot_message(9, ++tick, false, {}));
            EXPECT_EQ(core.own_body_backstops(), echoes ? 0u : 1u);
            EXPECT_EQ(state_of(core).snapshots_without_own_, echoes ? 0u : 10u);
            if (!echoes) continue;
            // A retired record, or one already answered, waits for nothing.
            state_of(core).own_life_[0].state = client_core_state::own_life::verdict::missing;
            for (int i = 0; i < 29; ++i) steps{core}.check_own_body(snapshot_message(9, ++tick, false, {}));
            EXPECT_EQ(core.own_body_backstops(), 0u) << "counted from scratch";
            steps{core}.check_own_body(snapshot_message(9, ++tick, false, {}));
            EXPECT_EQ(core.own_body_backstops(), 1u);
            // A beam's record (no deadline of its own) gates nothing either.
            auto beam = own_birth(130);
            beam.what = client_core_state::own_life::kind::beam;
            state_of(core).own_life_ = {beam};
            fake.advance(std::chrono::milliseconds(2000));
            for (int i = 0; i < 30; ++i) steps{core}.check_own_body(snapshot_message(9, ++tick, false, {}));
            EXPECT_EQ(core.own_body_backstops(), 2u);
        }
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

    // ---------------------------------------------------------------- R2
    // observe_input's `paused` (R2, design 2.2): the mute in force during the
    // step is recorded, journaled and sent as INPUT_FLAG_PAUSED with what the
    // host observed (keys 0, the navigation off); without it the flag stays
    // clear, as on the headless client, which never sets it.
    TEST(SessionClientCoreR2, PausedObservationSetsThePausedFlag) {
        for (const bool paused: {false, true}) {
            SCOPED_TRACE(paused ? "paused" : "live");
            flow_fake fake;
            session_client_core core(frame_platform(fake, false), session_capabilities{});
            run_to_tick_125(core, fake);
            fake.own_physical = true;
            fake.own_ball_name = "Ball_Wood";
            fake.observation.keys = paused ? 0 : 5;
            fake.observation.nav_active = !paused;
            fake.observation.paused = paused;
            core.pre_step();
            ASSERT_TRUE(core.post_step(true));
            ASSERT_FALSE(state_of(core).input_history_.empty());
            const auto& input = state_of(core).input_history_.back().second;
            EXPECT_EQ(state_of(core).input_history_.back().first, 126u);
            EXPECT_EQ(input.keys, paused ? 0 : 5);
            EXPECT_EQ((input.flags & bmmo::session::INPUT_FLAG_PAUSED) != 0, paused);
            EXPECT_EQ((input.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0, !paused);
            EXPECT_NE(input.flags & bmmo::session::INPUT_FLAG_PHYSICALIZED, 0);
        }
    }

    // Step P: the balls are known by what they are - every entity the own
    // ball has been this session (a trafo makes another one current; the old
    // one is still a ball) and the mirrors - not by the "Ball_" prefix the
    // trafo explosion pieces share.  The mechanisms a record tracks (and a
    // replayed tick's, world_hooks replay_mechanisms) are every movable body
    // the platform lists but those, in its order; begin forgets the own
    // ball's entities.  Every live frame records the current own entity
    // whether or not it has a body yet, before the frame's record: a body a
    // replay makes for the own ball (a moved own birth, a replayed own
    // lifecycle) is of an entity some earlier live frame already had as its
    // current ball, so the identity never waits for a body.
    TEST(SessionClientCoreP, MechanismsAreEveryMovableBodyButTheBalls) {
        flow_fake fake;
        session_client_core core(frame_platform(fake, false), session_capabilities{});
        run_to_tick_125(core, fake);
        state_of(core).remotes_[4].entity = "Ball_Wood_BMMO_4";
        fake.own_physical = true;
        fake.own_ball_name = "Ball_Stone";
        core.pre_step();
        ASSERT_TRUE(core.post_step(true));
        fake.own_ball_name = "Ball_Wood";   // a trafo
        core.pre_step();
        ASSERT_TRUE(core.post_step(true));
        EXPECT_TRUE(core.is_ball("Ball_Wood"));
        EXPECT_TRUE(core.is_ball("Ball_Stone")) << "an entity the own ball has been";
        EXPECT_TRUE(core.is_ball("Ball_Wood_BMMO_4")) << "a mirror";
        EXPECT_FALSE(core.is_ball("Ball_Paper")) << "never the own ball";
        EXPECT_FALSE(core.is_ball("Ball_Stone_piece01")) << "a trafo explosion piece";
        EXPECT_FALSE(core.is_ball("P_Box"));
        EXPECT_FALSE(core.is_ball(""));
        fake.mechanisms = {"P_Box", "Ball_Wood", "Ball_Stone_piece01", "Ball_Wood_BMMO_4", "Ball_Stone", "Ball_Stone_piece02"};
        EXPECT_EQ(steps{core}.movable_mechanisms(),
                  std::vector<std::string>({"P_Box", "Ball_Stone_piece01", "Ball_Stone_piece02"}));
        // A new session: the own ball's entities are forgotten.
        run_to_tick_125(core, fake);
        EXPECT_FALSE(core.is_ball("Ball_Stone"));
        // A frame without a body (before the own Physicalize): the current
        // own entity is a ball from that frame on.
        fake.own_physical = false;
        fake.own_ball_name = "Ball_Paper";
        core.pre_step();
        ASSERT_TRUE(core.post_step(true));
        EXPECT_TRUE(core.is_ball("Ball_Paper")) << "the current own ball, bodiless";
        fake.mechanisms = {"Ball_Paper", "P_Box"};
        EXPECT_EQ(steps{core}.movable_mechanisms(), std::vector<std::string>({"P_Box"}));
    }

    // ------------------------------------------------ step B1: the union
    // Resync design 2.5, 2.7, 2.8 and 7.1: the union as rollback state, a
    // late remote Sector stamp or a moved own one re-derived inside one
    // rewind to the lowest stamp - 1, and the cell re-read after it.

    // A world the engine records, rewinds and replays: no tracked body,
    // every step taken, each replayed tick named in the call list and run
    // through the core's before_replayed_tick (the re-derivation), the
    // core's birth_moved handed on; `frozen` stops its clock (a restore
    // then replays nothing).
    session_platform b1_platform(flow_fake& fake, std::shared_ptr<bool> frozen) {
        session_platform platform = fake.make_flow();
        platform.bind_world = [&fake, frozen](const bmmo::session::core_world_hooks& hooks) {
            fake.calls.push_back("bind_world");
            bmmo::session::rollback_world world;
            world.get_body = [](const std::string&, bmmo_physics_body_state&) { return false; };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            world.step = []() { return true; };
            world.simulating = [frozen]() { return !*frozen; };
            const auto before = hooks.before_replayed_tick;
            world.pre_step = [&fake, before](uint32_t tick) {
                fake.calls.push_back("replayed:" + std::to_string(tick));
                if (before) before(tick);
            };
            world.birth_moved = hooks.birth_moved;
            return world;
        };
        return platform;
    }

    // The live frames of ticks [from, to] as pre_step and post_step make
    // them for the sectors: `before` (the messages before the frame), the
    // drain, the own echoes, the Sector rewind, the union's pass, then the
    // rollback record and the union record of the tick.
    void b1_frames(session_client_core& core, uint32_t from, uint32_t to,
                   const std::function<void(uint32_t tick)>& before = nullptr) {
        auto& s = state_of(core);
        for (uint32_t t = from; t <= to; ++t) {
            s.frames_since_anchor_ = static_cast<int64_t>(t - s.tick_base_);   // current = t - 1
            if (before) before(t);
            steps{core}.drain_due_events(t);
            steps{core}.process_own_echoes();
            steps{core}.rewind_for_sectors();
            steps{core}.sector_union_pass(t);
            s.frames_since_anchor_ = static_cast<int64_t>(t - s.tick_base_ + 1);   // current = t
            s.rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), t,
                               bmmo::session::rollback_tracked{}, {});
            s.union_records_[t] = s.sectors_.state();
        }
    }

    // Anchored, assigned at 120 (the own id 3, lead 7), frames 120..to.
    void b1_session(session_client_core& core, flow_fake& fake, uint32_t to) {
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        open_gate(core);   // the level's own walk is not what these tests are about
        b1_frames(core, 120, to);
        ASSERT_EQ(core.current_tick(), to);
    }

    size_t b1_count(const std::vector<std::string>& calls, const std::string& prefix) {
        return static_cast<size_t>(std::count_if(calls.begin(), calls.end(),
                                                 [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }

    // A late remote Sector inside the reach: nothing at the drain but the
    // late set; the frame's rewind goes to stamp - 1 once, the replayed
    // stamp tick records the re-derived pass before that tick's re-issue,
    // the cells are adopted and the cell re-read, the line logged; no live
    // pass at the arrival tick, the replayed pass's cell is no crossing of
    // ours, and a real crossing afterwards is.
    TEST(SessionClientCoreSectorRewind, ALateRemoteStampRewindsOnceAndReDerivesThePass) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_event(sector_message(4, 3, 122, 122));
        fake.adopted_cell = 3;
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        EXPECT_TRUE(since(fake.calls, from).empty()) << "the late set only";
        ASSERT_EQ(state_of(core).sector_late_.size(), 1u);
        EXPECT_EQ(state_of(core).sectors_.stamps_pending(), 0u) << "not stamped live";
        steps{core}.rewind_for_sectors();
        {
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay",
                "replayed:122", "sector_manager_active:off", "set_union_pass:122:3",
                "log:0:sector +3 at tick 122 (player 4, stamped 122), running: 1 3 (re-derived in a rewind at live tick 125)",
                "replayed:123", "sector_manager_active:off",
                "replayed:124", "sector_manager_active:off",
                "replayed:125", "sector_manager_active:off", "now:rewind_end",
                "keep_live_cells:exit:rolled back", "sector_cell_adopted",
                "log:0:remote player 4 entered sector 3, stamped for tick 122 (late by 4: rewound to 121)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(core.sector_rewinds(), 1u);
        EXPECT_EQ(core.rollback_stats().rewinds, 1u);
        EXPECT_FALSE(core.last_rewind_rederived()) << "only while the rewind's guard ends";
        EXPECT_EQ(state_of(core).sectors_.active(), std::set<int>({1, 3}));
        EXPECT_EQ(state_of(core).sector_cell_, 3);
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{122, 3}}));
        EXPECT_EQ(state_of(core).union_records_.at(121).active, std::set<int>({1}));
        EXPECT_EQ(state_of(core).union_records_.at(122).active, std::set<int>({1, 3}));
        EXPECT_EQ(state_of(core).union_records_.at(125), state_of(core).sectors_.state());
        EXPECT_EQ(state_of(core).sector_stamps_.at({4u, 3}).tick, 122u);
        // No live pass at the arrival tick.
        from = fake.calls.size();
        steps{core}.sector_union_pass(126);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"sector_manager_active:off"});
        // The replayed pass's cell: no crossing of ours.
        fake.poll = cell_poll(3, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(126);
        EXPECT_EQ(since(fake.calls, from), std::vector<std::string>{"poll_sector:126"});
        EXPECT_FALSE(state_of(core).sectors_requested_.count(3));
        // A real crossing afterwards: stamped ahead.
        fake.poll = cell_poll(5, true);
        from = fake.calls.size();
        steps{core}.poll_own_sector(127);
        {
            const std::vector<std::string> expected = {
                "poll_sector:127", "send:1",
                "log:0:sector 5 stamped for tick 135 (checkpoint, lead 7, input_delay 4), event sent ahead"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_EQ(state_of(core).sector_stamps_.at({3u, 5}).tick, 135u);
    }

    // Without the cell re-read the replayed pass's write would be taken for
    // an own crossing (the a5u_lead clients' phantom sector 4): the re-read
    // is what keeps it out.  An unread cell keeps the pass's own write.
    TEST(SessionClientCoreSectorRewind, AnUnreadCellKeepsTheReDerivedPassWrite) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_event(sector_message(4, 3, 122, 122));
        fake.adopted_read = false;
        b1_frames(core, 126, 126);
        EXPECT_EQ(core.sector_rewinds(), 1u);
        EXPECT_EQ(state_of(core).sector_cell_, 3) << "the re-derived pass's write";
        fake.poll = cell_poll(3, true);
        const size_t sent = fake.sent.size();
        steps{core}.poll_own_sector(126);
        EXPECT_EQ(fake.sent.size(), sent);
    }

    // The reviews' scenario (resync design 10, R1.1): stamps that reached
    // the live union after the anchor's record survive the re-derivation -
    // an own stamp for a tick inside the window and an on-time remote stamp
    // drained inside it pass at their ticks, the live passes are re-set
    // there and nothing is forgotten.
    TEST(SessionClientCoreSectorRewind, StampsInsideTheWindowSurvive) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        open_gate(core);   // after the level's own walk (step S4)
        b1_frames(core, 120, 140, [&](uint32_t t) {
            if (t == 125) core.request_sector(4, "checkpoint");                    // current 124: stamped 132
            if (t == 130) core.on_session_event(sector_message(5, 5, 135, 135));   // on time
        });
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{132, 4}, {135, 5}}));
        core.on_session_event(sector_message(4, 3, 122, 122));   // arrives at 141
        size_t from = fake.calls.size();
        b1_frames(core, 141, 141);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(b1_count(calls, "keep_live_cells:enter"), 1u);
        EXPECT_EQ(b1_count(calls, "replayed:"), 19u) << "122..140";
        EXPECT_EQ(b1_count(calls, "set_union_pass:"), 3u);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:122:3"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:132:4"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:135:5"), calls.end());
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{122, 3}, {132, 4}, {135, 5}}));
        EXPECT_EQ(state_of(core).sector_passes_forgotten_, 0u);
        EXPECT_EQ(state_of(core).sectors_.active(), std::set<int>({1, 3, 4, 5}));
        EXPECT_EQ(b1_count(calls, "run_sector:"), 0u) << "no live pass at 141";
    }

    // A late stamp that moves a later live pass (the manager is busy with
    // the earlier sector): the live pass is forgotten at its tick and made
    // at the re-derived one; its bodies' birth tick is no missing birth.
    TEST(SessionClientCoreSectorRewind, ALivePassTheReDerivationMovesIsForgotten) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        open_gate(core);   // after the level's own walk (step S4)
        b1_frames(core, 120, 135, [&](uint32_t t) {
            if (t == 121) core.on_session_event(sector_message(5, 4, 124, 124));
        });
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{124, 4}}));
        core.on_session_event(sector_message(4, 3, 123, 123));
        size_t from = fake.calls.size();
        b1_frames(core, 136, 136);
        const auto calls = since(fake.calls, from);
        // 3 at 123; the pacer idles 124, 125, 126: 4 at 126.
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:123:3"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:124:0"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:126:4"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:sector +4 at tick 124 of the live frames is not re-derived there: forgotten"),
                  calls.end());
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{123, 3}, {126, 4}}));
        EXPECT_EQ(state_of(core).sector_passes_forgotten_, 1u);
        // The birth attribution the engine asks during such a replay.
        EXPECT_TRUE(bmmo::session::sector_union::birth_moved({{124, 4}}, {{123, 3}}, 129));
        EXPECT_FALSE(bmmo::session::sector_union::birth_moved({{124, 4}}, {{124, 4}}, 129));
    }

    // Two late stamps in one drain, the later one first: one rewind, to the
    // lower stamp - 1, both re-derived and both logged.
    TEST(SessionClientCoreSectorRewind, TwoLateStampsInOneDrainMakeOneRewindToTheLowerOne) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_event(sector_message(4, 3, 124, 124));   // join order 0: drained first
        core.on_session_event(sector_message(5, 2, 122, 122));
        set_test_life(core, 5, 1);
        size_t from = fake.calls.size();
        b1_frames(core, 126, 126);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(b1_count(calls, "keep_live_cells:enter"), 1u);
        EXPECT_EQ(core.rollback_stats().rewinds, 1u);
        EXPECT_EQ(b1_count(calls, "replayed:"), 4u) << "122..125";
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{122, 2}, {125, 3}}));
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:remote player 4 entered sector 3, stamped for tick 124 (late by 2: rewound to 121)"),
                  calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:remote player 5 entered sector 2, stamped for tick 122 (late by 4: rewound to 121)"),
                  calls.end());
    }

    // A late stamp that changes nothing (requested at or before its tick
    // already) is stamped as before, with no rewind.
    TEST(SessionClientCoreSectorRewind, ARedundantLateStampIsStampedAsBefore) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        b1_frames(core, 120, 125, [&](uint32_t t) {
            if (t == 121) core.on_session_event(sector_message(5, 3, 122, 122));
        });
        core.on_session_event(sector_message(4, 3, 124, 124));
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        {
            const std::vector<std::string> expected = {
                "log:0:remote player 4 entered sector 3, stamped for tick 124 (LATE: the server started it already)"};
            EXPECT_EQ(since(fake.calls, from), expected);
        }
        EXPECT_TRUE(state_of(core).sector_late_.empty());
    }

    // Step B5RX (the review of RC3): a frozen clock's re-anchor no longer
    // waits for a snapshot.  Every rewind the core asks outside one (inputs,
    // lifecycle events, sector stamps, the forced one) goes through
    // rewind_world, which asks for the resync when the engine found the
    // clock stopped; with the clock running it asks for nothing.
    TEST(SessionClientCoreSectorRewind, AFrozenRewindOutsideASnapshotAsksForAResync) {
        for (const bool stopped: {true, false}) {
            SCOPED_TRACE(stopped ? "clock frozen" : "clock running");
            flow_fake fake;
            auto frozen = std::make_shared<bool>(false);
            session_client_core core(b1_platform(fake, frozen), session_capabilities{});
            b1_session(core, fake, 125);
            core.config.body_set_corrections = false;   // RC3's default: no snapshot would ask
            *frozen = stopped;
            const uint64_t resyncs_before = core.resyncs_sent();
            const size_t from = fake.calls.size();
            {
                const auto forced = core.force_rewind(3);
                EXPECT_TRUE(forced.rolled) << "a frozen rewind still returns true (rollback.hpp)";
            }
            const auto calls = since(fake.calls, from);
            const bool asked = std::find(calls.begin(), calls.end(),
                                         "log:0:resync requested (local physics clock frozen) at tick 125") != calls.end();
            EXPECT_EQ(core.rollback_stats().frozen, stopped ? 1u : 0u);
            EXPECT_EQ(core.resyncs_sent() - resyncs_before, stopped ? 1u : 0u);
            EXPECT_EQ(asked, stopped);
            EXPECT_EQ(state_of(core).resync_in_flight_, stopped);
        }
    }

    // The rewind refused (a frozen world replays nothing): every stamp of
    // the set live, as before B1, and one resync.  Beyond the reach: the
    // same at the drain; while a resync is pending, nothing is requested.
    TEST(SessionClientCoreSectorRewind, RefusedOrBeyondTheReachStampsLiveAndResyncs) {
        {
            flow_fake fake;
            auto frozen = std::make_shared<bool>(false);
            session_client_core core(b1_platform(fake, frozen), session_capabilities{});
            b1_session(core, fake, 125);
            core.on_session_event(sector_message(4, 3, 122, 122));
            *frozen = true;
            size_t from = fake.calls.size();
            steps{core}.drain_due_events(126);
            steps{core}.rewind_for_sectors();
            // Step B5RX: the frozen rewind itself asks for the re-anchor
            // (session_client_core::rewind_world); the refusal's request
            // that follows finds it in flight.
            const std::vector<std::string> expected = {
                "keep_live_cells:enter:125", "bind_world", "now:rewind_start", "now:rewind_replay", "now:rewind_end",
                "now:resync_limiter", "send:0",
                "log:0:resync requested (local physics clock frozen) at tick 125",
                "keep_live_cells:exit",
                "log:0:remote player 4 entered sector 3, stamped for tick 122 (LATE: the server started it already)",
                "log:0:late sector stamp for tick 122 is outside the rollback history at local tick 125: stamped live, resync",
                "now:resync_limiter"};
            EXPECT_EQ(since(fake.calls, from), expected);
            EXPECT_EQ(state_of(core).sector_rewinds_missed_, 1u);
            EXPECT_EQ(state_of(core).sectors_.stamps_pending(), 1u);
            EXPECT_EQ(state_of(core).pass_log_.size(), 0u);
        }
        {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            b1_session(core, fake, 125);
            state_of(core).rollback_.thresholds().max_resim_ticks = 2;
            core.on_session_event(sector_message(4, 3, 122, 122));
            size_t from = fake.calls.size();
            steps{core}.drain_due_events(126);
            {
                const std::vector<std::string> expected = {
                    "log:0:remote player 4 entered sector 3, stamped for tick 122 (LATE: the server started it already)",
                    "log:0:late sector stamp for tick 122 is outside the rollback history at local tick 125: stamped live, resync",
                    "now:resync_limiter", "send:0",
                    "log:0:resync requested (a late Sector event beyond the reach) at tick 125"};
                EXPECT_EQ(since(fake.calls, from), expected);
            }
            EXPECT_TRUE(state_of(core).sector_late_.empty());
            EXPECT_EQ(state_of(core).sector_rewinds_missed_, 1u);
            // While a resync is pending: live, nothing else.
            state_of(core).resync_pending_ = true;
            core.on_session_event(sector_message(4, 2, 123, 123));
            from = fake.calls.size();
            steps{core}.drain_due_events(126);
            {
                const std::vector<std::string> expected = {
                    "log:0:remote player 4 entered sector 2, stamped for tick 123 (LATE: the server started it already)"};
                EXPECT_EQ(since(fake.calls, from), expected);
            }
            EXPECT_EQ(state_of(core).sector_rewinds_missed_, 1u);
            // Before this numbering's first tick (a late joiner's catch-up):
            // live, nothing else.
            state_of(core).resync_pending_ = false;
            core.on_session_event(sector_message(4, 4, 100, 100));
            from = fake.calls.size();
            steps{core}.drain_due_events(126);
            {
                const std::vector<std::string> expected = {
                    "log:0:remote player 4 entered sector 4, stamped for tick 100 (LATE: the server started it already)"};
                EXPECT_EQ(since(fake.calls, from), expected);
            }
        }
    }

    // A MOVED own Sector echo (plan step 23): the own pass whose tick has
    // run moves from T to T' in one rewind to T - 1 (the live pass at T
    // forgotten); one whose tick is ahead is stamped again at T' live.
    TEST(SessionClientCoreSectorRewind, AMovedOwnSectorMovesTheOwnPass) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        open_gate(core);   // after the level's own walk (step S4)
        b1_frames(core, 120, 135, [&](uint32_t t) {
            if (t == 122) core.request_sector(4, "checkpoint");   // current 121: stamped 129
        });
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{129, 4}}));
        bmmo::session_event_msg echo = sector_message(3, 4, 133, 129);
        core.on_session_event(std::move(echo));
        size_t from = fake.calls.size();
        b1_frames(core, 136, 136);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(calls.front(),
                  "log:0:own sector stamped tick 129 echoed moved by the server (applied 133, state 00) at local tick 135; "
                  "the own pass moves to the server's tick");
        EXPECT_EQ(b1_count(calls, "keep_live_cells:enter"), 1u);
        EXPECT_EQ(b1_count(calls, "replayed:"), 7u) << "129..135";
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:129:0"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:133:4"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:own sector 4 moved 129 -> 133 by the server: the own pass moved (rewound to 128)"),
                  calls.end());
        EXPECT_EQ(state_of(core).pass_log_, (std::map<uint32_t, int>{{133, 4}}));
        EXPECT_EQ(state_of(core).sector_passes_forgotten_, 1u);
        EXPECT_EQ(state_of(core).sector_stamps_.at({3u, 4}).tick, 133u);
        EXPECT_EQ(state_of(core).own_moves_other_, 0u) << "moved, not kept";
        // The own pass still ahead: stamped again live, no rewind.
        b1_frames(core, 137, 137, [&](uint32_t) { core.request_sector(5, "checkpoint"); });   // current 136: 144
        core.on_session_event(sector_message(3, 5, 147, 144));
        from = fake.calls.size();
        b1_frames(core, 138, 138);
        const auto ahead = since(fake.calls, from);
        EXPECT_EQ(b1_count(ahead, "keep_live_cells:enter"), 0u);
        EXPECT_NE(std::find(ahead.begin(), ahead.end(),
                            "log:0:own sector 5 moved 144 -> 147 by the server before its pass: stamped again at 147"),
                  ahead.end());
        EXPECT_EQ(state_of(core).sectors_.stamps_pending(), 1u);
        EXPECT_FALSE(state_of(core).sectors_.covers(5, 146));
        EXPECT_TRUE(state_of(core).sectors_.covers(5, 147));
    }


    // ------------------------------------------------ step A3: the exact resync
    // Resync design 2.6 and 7.1: a re-assign with STATE_FOLLOWS anchors the
    // history at base - 1; snapshots meanwhile are dropped; the state is
    // applied as that tick and re-stepped (D = 0, 20); it waits for its
    // bodies; a stale one is dropped; the resync hold holds and times out;
    // a late change before the anchor is the state's.

    bmmo::session_assign_msg exact_assign(uint32_t session, uint32_t first_tick) {
        bmmo::session_assign_msg msg = assign_message(session, first_tick);
        msg.flags = bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS;
        return msg;
    }

    bmmo::session_resync_state_msg resync_state(uint32_t base, const std::set<int>& active) {
        bmmo::session_resync_state_msg msg;
        msg.session = 9;
        msg.tick = base - 1;
        msg.base = base;
        bmmo::session::gated_sector_union u;
        open_gate(u);
        for (int sector: active) if (sector != 1) u.request(4, sector, base - 30);
        while (u.pass_pending()) u.tick(base - 20, false);
        msg.union_state = bmmo::session::sector_union::serialize(u.state());
        msg.scripts = {1, 2, 3, 4};
        return msg;
    }

    // The state as a host delivers it: received in the drain, applied by the
    // resync poll every host runs before pre_step (A3 fix-up: after the whole
    // drain, so the notices queued behind it amend the re-step's records).
    void deliver(session_client_core& core, bmmo::session_resync_state_msg msg) {
        core.on_session_resync_state(std::move(msg));
        core.poll_resync_hold();
    }

    TEST(SessionClientCoreResync, AReassignWithTheStateFollowingAnchorsAtBaseMinusOne) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        const size_t from = fake.calls.size();
        core.on_session_assign(exact_assign(9, 140));
        const auto calls = since(fake.calls, from);
        auto& s = state_of(core);
        EXPECT_TRUE(s.resync_pending_);
        EXPECT_TRUE(s.resync_exact_);
        EXPECT_EQ(s.resync_base_, 140u);
        EXPECT_EQ(s.resync_old_tick_, 125u);
        EXPECT_EQ(s.resync_delta_, -14);
        EXPECT_EQ(s.held_floor_, 140u) << "notices for base.. amend records that exist";
        EXPECT_TRUE(s.rollback_.knows_tick(139)) << "the synthetic anchor";
        EXPECT_EQ(s.rollback_.history_size(), 1u);
        EXPECT_EQ(s.union_records_.count(139), 1u);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "note_script_resets:139"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "forget_activations_from:140"), calls.end());
        EXPECT_EQ(b1_count(calls, "forget_births_after"), 0u) << "the birth log is kept";
        EXPECT_EQ(b1_count(calls, "hook:numbering_restarted:assign_resync"), 1u);
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:resynced: tick base 140 (old tick 125, delta -14): anchored at 139, the server's state of it follows"),
                  calls.end());
        // A snapshot meanwhile is dropped and counted.
        bmmo::session_snapshot_msg snapshot;
        snapshot.session = 9;
        snapshot.tick = 139;
        snapshot.full = 1;
        core.on_session_snapshot(snapshot);
        EXPECT_EQ(s.snapshots_while_resync_, 1u);
        EXPECT_TRUE(s.resync_pending_);
        // A state for another base is stale.
        core.on_session_resync_state(resync_state(150, {1}));
        EXPECT_EQ(s.resync_states_stale_, 1u);
        EXPECT_FALSE(s.have_resync_state_);
    }

    TEST(SessionClientCoreResync, TheStateAppliedBeforeAnyFrameAndAfterTwenty) {
        for (const uint32_t depth: {0u, 20u}) {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            b1_session(core, fake, 125);
            core.on_session_assign(exact_assign(9, 140));
            if (depth) b1_frames(core, 140, 139 + depth);
            auto& s = state_of(core);
            const size_t from = fake.calls.size();
            core.on_session_resync_state(resync_state(140, {1, 2}));
            EXPECT_TRUE(s.resync_pending_) << "kept until the drain is over";
            EXPECT_TRUE(s.have_resync_state_);
            EXPECT_FALSE(core.poll_resync_hold().held);
            const auto calls = since(fake.calls, from);
            EXPECT_FALSE(s.resync_pending_) << depth;
            EXPECT_FALSE(s.have_resync_state_);
            EXPECT_EQ(s.resyncs_restepped_, 1u);
            EXPECT_EQ(core.rollback_stats().resyncs, 1u);
            EXPECT_EQ(b1_count(calls, "replayed:"), depth) << "one replayed tick per tick of the window";
            EXPECT_NE(std::find(calls.begin(), calls.end(), "localize_scripts:4:none"), calls.end());
            EXPECT_EQ(b1_count(calls, "forget_births_after:139"), 1u);
            EXPECT_EQ(b1_count(calls, "forget_pending_activations:"), 0u) << "no level walk in a running client's window";
            EXPECT_EQ(s.sectors_.active(), std::set<int>({1, 2})) << "the server's union";
            EXPECT_EQ(s.union_records_.at(139).active, std::set<int>({1, 2}));
            const auto line = std::find_if(calls.begin(), calls.end(), [](const std::string& c) {
                return c.rfind("log:0:resync: re-stepped from the server's tick 139 to ", 0) == 0;
            });
            ASSERT_NE(line, calls.end()) << depth;
            EXPECT_NE(line->find("(" + std::to_string(depth) + " ticks, delta -14"), std::string::npos) << *line;
            // The re-step window (139, 139 + depth]: an own correction of a
            // tick in it reaches as deep as the re-step; outside it, 48.
            EXPECT_TRUE(s.resync_window_);
            EXPECT_EQ(s.held_floor_, 139u) << "the history reaches S: a notice for base itself is replayed";
            EXPECT_EQ(steps{core}.window_rewind_reach(140u), depth ? 95u : 48u) << "S + 1";
            EXPECT_EQ(steps{core}.window_rewind_reach(139u + depth), depth ? 95u : 48u) << "the apply's tick";
            EXPECT_EQ(steps{core}.window_rewind_reach(140u + depth), 48u) << "after the apply";
            EXPECT_EQ(steps{core}.window_rewind_reach(139u), 48u) << "its anchor would be before S";
            core.on_session_assign(exact_assign(9, 200));
            EXPECT_FALSE(s.resync_window_) << "a new numbering ends it";
        }
    }

    // Step RLT: a relayed frame for a tick at or before the anchor S of the
    // exact state this numbering stands on is the state's - counted covered,
    // never late - while the state is pending and after it is applied; a
    // frame of a mirror that does not exist yet is parked as before; a frame
    // after S past the reach is still late; a re-assign without the state
    // (a full snapshot's) ends the rule.
    TEST(SessionClientCoreResync, RelayedFramesTheStateCoversAreNotLate) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        auto& s = state_of(core);
        s.remotes_[4].entity = "Ball_Wood_BMMO_4";   // a mirror's name; no body, not navigated
        const auto relay = [&](uint32_t player, uint32_t newest, std::vector<uint8_t> keys, uint32_t epoch) {
            bmmo::session_remote_input_msg msg = relay_message(newest, epoch);
            bmmo::session_remote_input_msg::entry e;
            e.player = player;
            for (const uint8_t k: keys) e.frames.push_back(keys_frame(k));
            msg.entries = {e};
            core.on_session_remote_input(msg);
        };
        EXPECT_FALSE(s.state_anchored_) << "an ordinary base";
        core.on_session_assign(exact_assign(9, 140));   // S = 139
        EXPECT_TRUE(s.state_anchored_);
        b1_frames(core, 140, 141);   // the base's first frames ran: 139's frames arrive after them
        ASSERT_EQ(core.current_tick(), 141u);
        // Pending: ticks 138 and 139 (S itself) are the state's.
        relay(4, 139, {1, 2}, 140);
        EXPECT_EQ(s.relay_new_, 2u);
        EXPECT_EQ(s.relay_covered_, 2u);
        EXPECT_EQ(s.relay_late_, 0u) << "the server's state of 139 holds them";
        EXPECT_FALSE(s.input_rewind_pending_);
        EXPECT_EQ(s.remotes_[4].inputs.count(139), 1u) << "stored in the ring as every frame";
        // The base's own tick is not the state's: its anchor S is recorded.
        relay(4, 140, {3}, 140);
        EXPECT_EQ(s.relay_covered_, 2u);
        EXPECT_EQ(s.relay_late_, 0u);
        // No mirror yet: parked as before, neither counter.
        const uint64_t parked = s.relay_parked_;
        relay(5, 139, {4}, 140);
        EXPECT_EQ(s.relay_parked_, parked + 1);
        EXPECT_EQ(s.relay_covered_, 2u);
        EXPECT_EQ(s.relay_late_, 0u);
        // Applied: still the state's.
        deliver(core, resync_state(140, {1, 2}));
        ASSERT_FALSE(s.resync_pending_);
        EXPECT_TRUE(s.state_anchored_) << "the world stands on the state of 139";
        relay(4, 137, {5}, 140);
        EXPECT_EQ(s.relay_covered_, 3u);
        EXPECT_EQ(s.relay_late_, 0u);
        // After S and past the reach: a lost correction, late as before.
        // (The reach adapts to the relay lag it sees, so the frame is put
        // past the history too.)
        b1_frames(core, 142, 300);
        ASSERT_FALSE(s.rollback_.knows_tick(144));
        relay(4, 145, {6}, 140);
        EXPECT_EQ(s.relay_covered_, 3u);
        EXPECT_EQ(s.relay_late_, 1u);
        // A re-assign without the state: its full snapshot is the world, and
        // a frame before its base has no state to be covered by.
        core.on_session_assign(assign_message(9, 260));
        EXPECT_FALSE(s.state_anchored_);
        relay(4, 250, {7}, 260);
        EXPECT_EQ(s.relay_covered_, 3u);
        EXPECT_EQ(s.relay_late_, 2u);
        // The status and window lines carry it.
        const std::string status = core.status_text(bmmo::session::host_counters{});
        EXPECT_NE(status.find(" relay=7/0/1/2/3 "), std::string::npos) << status;
    }

    // Step A5 (resync design 2.9): a late joiner's first assignment with the
    // flag is taken as a resync's - the world as it stands (the anchored
    // level start) recorded as base - 1, the exact resync pending.  The
    // level's own walk then opens the union's gate (step S4) inside the
    // window, and the apply drops its manager activations from the base
    // through the walk's tick, which the server's state has run long before.
    // A first assignment without the flag, or with rollback off, is the
    // ordinary one.
    TEST(SessionClientCoreResync, ALateJoinersFirstAssignmentTakesTheStatePath) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        size_t from = fake.calls.size();
        core.on_session_assign(exact_assign(9, 140));
        auto calls = since(fake.calls, from);
        auto& s = state_of(core);
        EXPECT_TRUE(s.assigned_);
        EXPECT_EQ(core.current_tick(), 140u);
        EXPECT_TRUE(s.resync_pending_);
        EXPECT_TRUE(s.resync_exact_);
        EXPECT_EQ(s.resync_base_, 140u);
        EXPECT_TRUE(s.resync_join_) << "its wakeups are withheld until the apply";
        EXPECT_TRUE(s.rollback_.knows_tick(139)) << "the synthetic anchor: the level start";
        EXPECT_EQ(s.union_records_.count(139), 1u);
        EXPECT_FALSE(s.union_records_.at(139).walk.seen) << "the joiner's own walk has not run";
        EXPECT_NE(std::find(calls.begin(), calls.end(), "note_script_resets:139"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "forget_activations_from:140"), calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(), "log:0:late join: the server's state of tick 139 follows (protocol 10)"),
                  calls.end());
        // The level's own walk: the manager idle 140-142, active 143-146.
        b1_frames(core, 140, 152, [&fake](uint32_t t) { fake.manager_active = t >= 143 && t <= 146; });
        fake.manager_active = false;
        EXPECT_TRUE(s.sectors_.walk().seen);
        EXPECT_EQ(s.sectors_.walk_open().tick, 143u);
        EXPECT_NE(std::find(fake.calls.begin(), fake.calls.end(), "log:0:level walk seen at tick 143"), fake.calls.end());
        from = fake.calls.size();
        deliver(core, resync_state(140, {1}));
        calls = since(fake.calls, from);
        EXPECT_FALSE(s.resync_pending_);
        EXPECT_FALSE(s.resync_join_);
        EXPECT_EQ(s.resyncs_restepped_, 1u);
        EXPECT_EQ(b1_count(calls, "forget_pending_activations:"), 1u);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "forget_pending_activations:140:143"), calls.end());
        EXPECT_LT(std::find(calls.begin(), calls.end(), "forget_pending_activations:140:143"),
                  std::find(calls.begin(), calls.end(), "replayed:140"))
            << "dropped before the re-step re-issues them";
        EXPECT_EQ(b1_count(calls, "replayed:"), 13u);
        EXPECT_TRUE(s.sectors_.walk().seen) << "the server's union: past its own walk";

        // Without the flag, or with rollback off: the ordinary first assignment.
        for (const bool rollback: {true, false}) {
            flow_fake plain;
            session_client_core other(b1_platform(plain, std::make_shared<bool>(false)), session_capabilities{});
            state_of(other).own_id_ = 3;
            bmmo::session::core_config config;
            config.rollback = rollback;
            ASSERT_FALSE(other.begin(start_message(9, 3), config).refused);
            other.level_requested();
            bmmo::session::anchor_report report;
            report.hash.pose = 0x51;
            report.hash.surfaces = 0x52;
            report.physics_sha256 = "headless-64";
            report.build_id = "build-x";
            report.own_name = "Me";
            report.load_ticks = 12;
            other.anchor(report);
            other.on_session_assign(rollback ? assign_message(9, 140) : exact_assign(9, 140));
            EXPECT_FALSE(state_of(other).resync_pending_) << rollback;
            EXPECT_FALSE(state_of(other).rollback_.knows_tick(139)) << rollback;
        }
    }

    // Step A5 review fix-up: the level-walk drop is the late joiner's alone,
    // and only for a walk seen in the current numbering.  (a) A running
    // member's exact resync whose window holds its own walk (a request in
    // the session's first ticks) drops nothing: the server's state at S has
    // not run the walk.  (b) A join superseded by an exact re-assign (the
    // base missed) is still a join, but the walk it saw is a tick of the old
    // numbering: nothing dropped.  (c) A re-assign without the flag ends the
    // join (the full snapshot answers it).
    TEST(SessionClientCoreResync, TheWalkDropIsTheLateJoinersAlone) {
        {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            run_to_anchor(core, fake);
            core.on_session_assign(assign_message(9, 120));   // the gate still closed
            b1_frames(core, 120, 122);
            core.on_session_assign(exact_assign(9, 124));
            auto& s = state_of(core);
            ASSERT_TRUE(s.resync_pending_);
            EXPECT_FALSE(s.resync_join_);
            b1_frames(core, 124, 132, [&fake](uint32_t t) { fake.manager_active = t >= 126 && t <= 128; });
            fake.manager_active = false;
            ASSERT_TRUE(s.sectors_.walk().seen);
            EXPECT_EQ(s.sectors_.walk_open().tick, 126u);
            EXPECT_TRUE(s.sectors_.walk_open().known);
            const size_t from = fake.calls.size();
            deliver(core, resync_state(124, {1}));
            const auto calls = since(fake.calls, from);
            EXPECT_FALSE(s.resync_pending_);
            EXPECT_EQ(s.resyncs_restepped_, 1u);
            EXPECT_EQ(b1_count(calls, "forget_pending_activations:"), 0u) << "a start member's walk is the server's too";
        }
        {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            run_to_anchor(core, fake);
            core.on_session_assign(exact_assign(9, 140));
            b1_frames(core, 140, 146, [&fake](uint32_t t) { fake.manager_active = t >= 143; });
            fake.manager_active = false;
            auto& s = state_of(core);
            ASSERT_TRUE(s.sectors_.walk_open().known);
            EXPECT_EQ(s.sectors_.walk_open().tick, 143u);
            core.on_session_assign(exact_assign(9, 141));   // the base missed: answered again
            EXPECT_TRUE(s.resync_pending_);
            EXPECT_TRUE(s.resync_join_) << "still the late joiner's resync";
            EXPECT_FALSE(s.sectors_.walk_open().known) << "143 names the old numbering";
            b1_frames(core, 141, 145);
            const size_t from = fake.calls.size();
            deliver(core, resync_state(141, {1}));
            const auto calls = since(fake.calls, from);
            EXPECT_FALSE(s.resync_pending_);
            EXPECT_FALSE(s.resync_join_);
            EXPECT_EQ(s.resyncs_restepped_, 1u);
            EXPECT_EQ(b1_count(calls, "forget_pending_activations:"), 0u) << "an old numbering's walk tick";
        }
        {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            run_to_anchor(core, fake);
            core.on_session_assign(exact_assign(9, 140));
            auto& s = state_of(core);
            ASSERT_TRUE(s.resync_join_);
            b1_frames(core, 140, 142);
            core.on_session_assign(assign_message(9, 150));
            EXPECT_TRUE(s.resync_pending_);
            EXPECT_FALSE(s.resync_exact_);
            EXPECT_FALSE(s.resync_join_) << "the full snapshot answers it";
        }
    }

    TEST(SessionClientCoreResync, TheStateWaitsForItsBodies) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_assign(exact_assign(9, 140));
        b1_frames(core, 140, 145);
        auto msg = resync_state(140, {1});
        bmmo::session::body_state row;
        row.kind = bmmo::session::body_kind::Mechanism;
        row.owner = 7;
        row.name = "P_Modul_03_Floor";
        row.position[1] = 100.0;   // far from this world's body (the registry's guard): the name decides
        msg.rows.push_back(row);
        deliver(core, std::move(msg));
        auto& s = state_of(core);
        EXPECT_TRUE(s.resync_pending_) << "its body is not here yet";
        EXPECT_TRUE(s.have_resync_state_);
        EXPECT_EQ(s.resync_deferred_, 1u);
        EXPECT_FALSE(steps{core}.try_apply_resync("retry"));
        EXPECT_EQ(s.resync_deferred_, 1u) << "one wait, counted once";
        EXPECT_EQ(s.resync_deferred_ticks_, 2u);
        fake.bodies_there.push_back("P_Modul_03_Floor");
        EXPECT_TRUE(steps{core}.try_apply_resync("retry"));
        EXPECT_FALSE(s.resync_pending_);
        EXPECT_EQ(s.resyncs_restepped_, 1u);
    }

    TEST(SessionClientCoreResync, TheHoldHoldsAtTheReachAndTimesOut) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_assign(exact_assign(9, 140));
        b1_frames(core, 140, 233);   // D = 94
        EXPECT_FALSE(core.poll_resync_hold().held);
        b1_frames(core, 234, 234);   // D = 95: the next step would leave the anchor outside the history
        auto& s = state_of(core);
        s.resync_in_flight_ = true;
        EXPECT_TRUE(core.poll_resync_hold().held);
        EXPECT_TRUE(core.resync_held());
        fake.advance(std::chrono::milliseconds(1500));
        EXPECT_TRUE(core.poll_resync_hold().held);
        fake.advance(std::chrono::milliseconds(600));
        const uint64_t sent = s.resyncs_sent_;
        const auto out = core.poll_resync_hold();
        EXPECT_FALSE(out.held);
        EXPECT_TRUE(out.timed_out);
        EXPECT_EQ(s.resyncs_sent_, sent + 1) << "asked again at once: every other trigger waits while one is pending";
        EXPECT_TRUE(s.resync_in_flight_);
        EXPECT_TRUE(s.resync_pending_);
        EXPECT_EQ(s.resync_holds_, 1u);
        EXPECT_EQ(s.resync_hold_timeouts_, 1u);
        EXPECT_FALSE(core.poll_resync_hold().held) << "a timed-out hold is spent: the world runs on";
        EXPECT_EQ(s.resync_holds_, 1u);
        // The state of an anchor that left the history is refused, and the
        // client asks again: held back by the 2 s limiter, the request is
        // owed, and the owed request survives the pending resync (nothing
        // else would ask for it) until the limiter lets it go.
        b1_frames(core, 235, 236);
        deliver(core, resync_state(140, {1}));
        EXPECT_EQ(s.resync_states_refused_, 1u);
        EXPECT_EQ(s.resyncs_restepped_, 0u);
        EXPECT_TRUE(s.resync_owed_);
        EXPECT_FALSE(s.resync_in_flight_);
        steps{core}.retry_owed_resync();
        EXPECT_TRUE(s.resync_owed_) << "kept, not dropped for the pending resync";
        EXPECT_EQ(s.resyncs_sent_, sent + 1);
        fake.advance(std::chrono::milliseconds(2100));
        steps{core}.retry_owed_resync();
        EXPECT_FALSE(s.resync_owed_);
        EXPECT_EQ(s.resyncs_sent_, sent + 2);
        EXPECT_TRUE(s.resync_in_flight_);
    }

    // A mirror dead after S (resync design 2.6.3, Delta > 0): made from its
    // row before the renames and the navigation are built (so its server
    // clone is renamed to it), and its death applied again at its tick -
    // live when the tick lies past the apply's.
    TEST(SessionClientCoreResync, AMirrorDeadAfterSIsMadeAndDiesAgainAtItsTick) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 110));
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 112, 112));
        core.on_session_event(event_message(4, bmmo::session::event_type::Unphysicalize, 124, 124));
        // An on-time event is held for apply_held_lifecycle (after the
        // frame's rewinds): applied here before the next frame.
        b1_frames(core, 110, 125, [&](uint32_t t) { steps{core}.apply_held_lifecycle(t - 1); });
        auto& s = state_of(core);
        ASSERT_EQ(s.remotes_.count(4), 1u);
        ASSERT_FALSE(s.remotes_.at(4).physicalized) << "born at 112, dead at 124";
        core.on_session_assign(exact_assign(9, 121));
        EXPECT_EQ(s.resync_delta_, 5) << "R 125, S 120";
        b1_frames(core, 121, 122);
        auto msg = resync_state(121, {1});
        bmmo::session::body_state ball;
        ball.kind = bmmo::session::body_kind::Ball;
        ball.owner = 4;
        msg.rows.push_back(ball);
        msg.clones = {{4u, std::string("Ball_Stone_BMMO_4")}};
        const size_t from = fake.calls.size();
        deliver(core, std::move(msg));
        const auto calls = since(fake.calls, from);
        EXPECT_FALSE(s.resync_pending_);
        EXPECT_TRUE(s.remotes_.at(4).physicalized) << "made from its row";
        ASSERT_EQ(s.pending_deaths_.size(), 1u) << "its death past the apply's tick 122 is kept for the live frame";
        EXPECT_EQ(s.pending_deaths_[0].tick, 124u);
        const auto line = std::find_if(calls.begin(), calls.end(), [](const std::string& c) {
            return c.rfind("log:0:resync: re-stepped from the server's tick 120 to 122", 0) == 0;
        });
        ASSERT_NE(line, calls.end());
        EXPECT_NE(line->find("Ball_Stone_BMMO_4->" + s.remotes_.at(4).entity), std::string::npos) << *line;
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:resync: mirror of 4 made from its row at S 120, its death at 124 applied again live"),
                  calls.end());
        steps{core}.drain_pending_deaths(123);
        EXPECT_TRUE(s.remotes_.at(4).physicalized);
        steps{core}.drain_pending_deaths(124);
        EXPECT_FALSE(s.remotes_.at(4).physicalized) << "dead again at its tick";
        EXPECT_TRUE(s.pending_deaths_.empty());
    }

    // Step F2 (DRJ): a late joiner's mirror born before the anchor is made
    // live at base (its birth not known at its stamp); its owner leaving
    // after S, before the state arrives, left the state waiting for a body
    // that never came (trl_live_B client 1) until the hold timed out.  Its
    // recorded birth makes it from the row, and its death is applied again.
    TEST(SessionClientCoreResync, AJoinersMirrorBornBeforeTheAnchorAndDeadAfterSIsMadeFromItsRow) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(exact_assign(9, 140));
        auto& s = state_of(core);
        ASSERT_TRUE(s.resync_join_);
        core.on_session_event(event_message(4, bmmo::session::event_type::Physicalize, 112, 112));
        b1_frames(core, 140, 142, [&](uint32_t t) { steps{core}.apply_held_lifecycle(t - 1); });
        ASSERT_EQ(s.remotes_.count(4), 1u);
        ASSERT_TRUE(s.remotes_.at(4).physicalized) << "made live at base";
        ASSERT_FALSE(s.remotes_.at(4).birth_known) << "not at its stamp 112";
        core.on_session_event(event_message(4, bmmo::session::event_type::Unphysicalize, 142, 142));
        b1_frames(core, 143, 146, [&](uint32_t t) { steps{core}.apply_held_lifecycle(t - 1); });
        ASSERT_FALSE(s.remotes_.at(4).physicalized) << "its owner left at 142";
        auto msg = resync_state(140, {1});
        bmmo::session::body_state ball;
        ball.kind = bmmo::session::body_kind::Ball;
        ball.owner = 4;
        msg.rows.push_back(ball);
        msg.clones = {{4u, std::string("Ball_Stone_BMMO_4")}};
        const size_t from = fake.calls.size();
        deliver(core, std::move(msg));
        const auto calls = since(fake.calls, from);
        EXPECT_FALSE(s.resync_pending_) << "applied at its arrival, no wait";
        EXPECT_EQ(s.resync_deferred_, 0u);
        EXPECT_EQ(s.resyncs_restepped_, 1u);
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:resync: mirror of 4 made from its row at S 139, its death at 142 applied again in the replay"),
                  calls.end());
        EXPECT_FALSE(s.remotes_.at(4).physicalized) << "dead again in the replay";
        EXPECT_TRUE(s.pending_deaths_.empty());
    }

    // A held notice for a tick of a pending exact resync's epoch amends the
    // own record the apply's re-step feeds the ball from (resync design 2.2),
    // without a rewind of its own; one for a tick not simulated yet is
    // amended when its live frame is recorded.  After the apply the floor is
    // S: a notice for base itself is replayed like any later one.
    TEST(SessionClientCoreResync, NoticesWhilePendingAmendTheRecordsTheReStepReplays) {
        flow_fake fake;
        // The b1 world, able to drive a navigated own ball in the replay.
        session_platform platform = b1_platform(fake, std::make_shared<bool>(false));
        const auto bind = platform.bind_world;
        platform.bind_world = [bind](const bmmo::session::core_world_hooks& hooks) {
            bmmo::session::rollback_world world = bind(hooks);
            world.nav_input = [](const std::string&, const bmmo::session::input_frame&) { return true; };
            world.nav_poll = [](const std::string&, bool) { return true; };
            world.set_nav = [](const std::string&, const bmmo_physics_nav_state&) { return true; };
            return world;
        };
        session_client_core core(platform, session_capabilities{});
        b1_session(core, fake, 125);
        core.config.own_corrections = true;
        core.on_session_assign(exact_assign(9, 140));
        auto& s = state_of(core);
        bmmo::session::input_frame played{}, held{};
        played.keys = 0;
        held.keys = 5;
        bmmo::session::rollback_tracked tracked;
        tracked.own_entity = "Ball_Me";
        const auto world = core.platform().bind_world(bmmo::session::core_world_hooks{});
        for (uint32_t t = 140; t <= 145; ++t) {
            s.frames_since_anchor_ = static_cast<int64_t>(t - s.tick_base_ + 1);   // current = t
            s.rollback_.record(world, t, tracked, {{"Ball_Me", played}});
        }
        ASSERT_EQ(core.current_tick(), 145u);
        steps{core}.settle_held_tick(140u, held);
        steps{core}.settle_held_tick(143u, held);
        steps{core}.settle_held_tick(147u, held);   // not simulated yet
        bmmo::session::input_frame got{};
        ASSERT_TRUE(s.rollback_.recorded_input("Ball_Me", 140, got));
        EXPECT_EQ(got.keys, 5u) << "base itself";
        ASSERT_TRUE(s.rollback_.recorded_input("Ball_Me", 143, got));
        EXPECT_EQ(got.keys, 5u);
        ASSERT_TRUE(s.rollback_.recorded_input("Ball_Me", 141, got));
        EXPECT_EQ(got.keys, 0u) << "no notice: kept";
        EXPECT_FALSE(s.input_rewind_pending_) << "the apply's re-step is the replay";
        EXPECT_EQ(s.resync_amends_, 2u);
        // The tick ahead: amended once its live frame is recorded.
        for (uint32_t t = 146; t <= 147; ++t) {
            s.frames_since_anchor_ = static_cast<int64_t>(t - s.tick_base_ + 1);
            s.rollback_.record(world, t, tracked, {{"Ball_Me", played}});
        }
        steps{core}.take_held_ahead(147u);
        ASSERT_TRUE(s.rollback_.recorded_input("Ball_Me", 147, got));
        EXPECT_EQ(got.keys, 5u);
        EXPECT_EQ(s.resync_amends_, 3u);
        EXPECT_FALSE(s.input_rewind_pending_);
        // Applied: the floor is S, so a notice for base that comes now is
        // replayed (a rewind from S), not stored as base.
        deliver(core, resync_state(140, {1}));
        ASSERT_FALSE(s.resync_pending_);
        EXPECT_EQ(s.held_floor_, 139u);
        ASSERT_TRUE(s.rollback_.recorded_input("Ball_Me", 140, got));
        EXPECT_EQ(got.keys, 5u) << "the re-step re-records what it fed";
    }

    // Delta > 0: the frames the old numbering sent for base .. R are the ones
    // the server applies (its buffer keeps what it has at or after the base
    // and takes the first copy of a tick); they are played again at those
    // ticks as forced own frames.  Delta <= 0 sent nothing at or after the
    // base, and the legacy re-assign forces nothing.
    TEST(SessionClientCoreResync, ADeltaAboveZeroPlaysTheOldNumberingsSentFramesAgain) {
        for (const bool exact: {true, false}) {
            flow_fake fake;
            session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
            b1_session(core, fake, 125);
            auto& s = state_of(core);
            for (uint32_t t = 116; t <= 125; ++t) {
                bmmo::session::input_frame sent{};
                sent.keys = static_cast<uint8_t>(t - 110);
                s.sent_inputs_[t] = sent;
            }
            core.on_session_assign(exact ? exact_assign(9, 121) : assign_message(9, 121));
            if (!exact) {
                EXPECT_TRUE(s.forced_own_.empty());
                continue;
            }
            EXPECT_EQ(s.resync_delta_, 5) << "R 125, S 120";
            ASSERT_EQ(s.forced_own_.size(), 5u) << "121..125";
            EXPECT_EQ(s.forced_own_.begin()->first, 121u);
            EXPECT_EQ(s.forced_own_.rbegin()->first, 125u);
            EXPECT_EQ(s.forced_own_.at(123).frame.keys, 13u);
            EXPECT_FALSE(s.forced_own_.at(123).count);
            EXPECT_EQ(s.resync_resent_, 5u);
            EXPECT_TRUE(s.sent_inputs_.empty()) << "the epoch's ring starts over";
        }
    }

    // A core with rollback off cannot re-step: it does not declare
    // CAP_RESYNC_STATE, and the server answers it with a full snapshot.
    TEST(SessionClientCoreResync, NoCapabilityWithoutRollback) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        state_of(core).own_id_ = 3;
        bmmo::session::core_config config;
        config.rollback = false;
        ASSERT_FALSE(core.begin(start_message(9, 3), config).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.own_name = "Me";
        core.anchor(report);
        ASSERT_FALSE(fake.sent.empty());
        EXPECT_EQ(decode<bmmo::session_ready_msg>(fake.sent[0].first).caps & bmmo::session::CAP_RESYNC_STATE, 0u);
    }

    // The reconcile under delta > 0 (resync design 2.6.3): the anchor world
    // ran a pass at 122 after S = 120 that the server's union does not have
    // at S; its body (born at 127 = 122 + 5) is destroyed at the restore, and
    // the replay re-derives the pass at its stamp from the server's union.
    TEST(SessionClientCoreResync, ADeltaAboveZeroPassIsUndoneAndReDerived) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        run_to_anchor(core, fake);
        core.on_session_assign(assign_message(9, 110));
        open_gate(core);   // after the level's own walk (step S4)
        core.on_session_event(sector_message(4, 3, 122, 122));
        b1_frames(core, 110, 125);
        auto& s = state_of(core);
        ASSERT_EQ(s.pass_log_, (std::map<uint32_t, int>{{122, 3}})) << "the live pass at the stamp";
        fake.mechanisms = {"P_Modul_03_Floor", "PE_Balloon_Platte01", "Ball_Wood_Piece01"};
        fake.birth_ticks["P_Modul_03_Floor"] = 127;
        fake.local_only.insert("Ball_Wood_Piece01");
        core.on_session_assign(exact_assign(9, 121));
        EXPECT_EQ(s.resync_delta_, 5) << "R 125, S 120";
        b1_frames(core, 121, 128);
        auto msg = resync_state(121, {1});
        {
            bmmo::session::gated_sector_union u;   // the server's union at S: sector 1, the pacer idle
            open_gate(u);
            for (uint32_t t = 118; t <= 120; ++t) u.tick(t, false);
            msg.union_state = bmmo::session::sector_union::serialize(u.state());
        }
        const size_t from = fake.calls.size();
        deliver(core, std::move(msg));
        const auto calls = since(fake.calls, from);
        EXPECT_FALSE(s.resync_pending_);
        EXPECT_EQ(s.resync_destroyed_, 1u) << "the pass's body";
        EXPECT_EQ(s.resync_unexplained_, 0u) << "the balloon is keep-local, the piece local-only";
        EXPECT_NE(std::find(calls.begin(), calls.end(), "set_union_pass:122:3"), calls.end())
            << "the pass re-derived at its stamp";
        EXPECT_EQ(s.sector_passes_forgotten_, 0u);
        EXPECT_EQ(s.sectors_.active(), std::set<int>({1, 3}));
        const auto line = std::find_if(calls.begin(), calls.end(), [](const std::string& c) {
            return c.rfind("log:0:resync: re-stepped from the server's tick 120 to 128 (8 ticks, delta +5", 0) == 0;
        });
        ASSERT_NE(line, calls.end());
        EXPECT_NE(line->find("keep-local 1, local-only 1"), std::string::npos) << *line;
    }

    TEST(SessionClientCoreResync, ALateChangeBeforeTheAnchorIsTheStates) {
        flow_fake fake;
        session_client_core core(b1_platform(fake, std::make_shared<bool>(false)), session_capabilities{});
        b1_session(core, fake, 125);
        core.on_session_assign(exact_assign(9, 140));
        b1_frames(core, 140, 142);
        const uint64_t missed = state_of(core).life_rewinds_missed_;
        const size_t from = fake.calls.size();
        steps{core}.rewind_for_sectors();
        steps{core}.rewind_for_lifecycle(4u, 135u);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(state_of(core).life_rewinds_missed_, missed) << "no miss: the server's state covers it";
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:lifecycle event of 4 stamped tick 135 precedes the resync's anchor 139: the server's state covers it"),
                  calls.end());
    }


    // ------------------------------------------------- the trafo as an input
    // Step T3 (docs/trafo-replication-design.md 7.1 "Core with the fake
    // platform"): the capability and its operations, sets at the anchor and on
    // first sight (never inside a replay), the hold's stamps and its deferral
    // before the assignment, the inputs applied live and in a replay, a late
    // input joining the input rewind (the retail switches included), an input
    // past the history asking for a resync, the own echoes, and the
    // reconcile's decision table with every platform call.

    struct trafo_fake {
        std::vector<std::string>* calls = nullptr;
        bmmo::session::trafo_poll next_poll;                       // what poll_trafo answers (once)
        std::map<std::pair<uint32_t, uint8_t>, int> bodies;        // (player, type) -> bodies trafo_type_set reports
        bool reconcile_ok = true;
        std::vector<bmmo::session::trafo_key> retire_answer;      // what retire_trafo_sets answers (once)
        // A set's label in the calls: "<player>" for life 1, "<player>L<life>" later (step TRL).
        static std::string label(uint32_t player, uint16_t life) { return bmmo::session::trafo_life_label(player, life); }

        void install(session_platform& p) {
            p.poll_trafo = [this](uint32_t tick, bool option_e) {
                calls->push_back("poll_trafo:" + std::to_string(tick) + (option_e ? ":e" : ""));
                bmmo::session::trafo_poll out;
                std::swap(out, next_poll);
                return out;
            };
            p.make_trafo_set = [this](uint32_t player, uint16_t life, std::string&) {
                calls->push_back("make_trafo_set:" + label(player, life));
                return true;
            };
            p.run_trafo = [this](const bmmo::session::trafo_input& in, std::string&) {
                calls->push_back("run_trafo:" + label(in.player, in.life) + ":" + std::to_string(in.type) + ":"
                                 + std::to_string(in.flags) + ":" + std::to_string(in.applied) + ":" + std::to_string(in.seed));
                return true;
            };
            p.trafo_final = [this](uint32_t player, uint16_t life, uint8_t type, uint32_t tick) {
                calls->push_back("trafo_final:" + label(player, life) + ":" + std::to_string(type) + ":" + std::to_string(tick));
            };
            p.trafo_frame = [this](uint32_t tick) { calls->push_back("trafo_frame:" + std::to_string(tick)); };
            p.trafo_type_set = [this](uint32_t player, uint16_t life, uint8_t type) {
                calls->push_back("trafo_type_set:" + label(player, life) + ":" + std::to_string(type));
                bmmo::session::trafo_type_view view;
                view.exists = true;
                for (int k = 1; k <= 3; ++k)
                    view.pieces.push_back("piece" + std::to_string(k) + "_" + std::to_string(type) + "_" + label(player, life));
                const auto it = bodies.find({player, type});
                view.bodies = it == bodies.end() ? 0 : it->second;
                return view;
            };
            p.trafo_reconcile = [this](const bmmo::session::trafo_reconcile_spec& spec) {
                const bool remake = spec.action == bmmo::session::trafo_reconcile_action::remake;
                if (spec.action == bmmo::session::trafo_reconcile_action::retire) {
                    calls->push_back("trafo_reconcile:retire:" + label(spec.player, spec.life) + ":" + std::to_string(spec.birth_tick));
                    bmmo::session::trafo_reconcile_result r;
                    r.ok = reconcile_ok;
                    return r;
                }
                calls->push_back(std::string("trafo_reconcile:") + (remake ? "remake" : "unmake") + ":" + label(spec.player, spec.life)
                                 + ":" + std::to_string(spec.type) + (remake ? ":" + std::to_string(spec.seed) + ":"
                                                                                 + std::to_string(spec.birth_tick) : std::string()));
                bmmo::session::trafo_reconcile_result r;
                r.ok = reconcile_ok;
                if (!reconcile_ok) r.error = "refused";
                return r;
            };
            p.retire_trafo_sets = [this](uint32_t now, uint32_t history) {
                calls->push_back("retire_trafo_sets:" + std::to_string(now) + ":" + std::to_string(history));
                std::vector<bmmo::session::trafo_key> out;
                std::swap(out, retire_answer);
                return out;
            };
        }
    };

    session_capabilities trafo_caps() {
        session_capabilities caps;
        caps.trafo_inputs = true;
        return caps;
    }

    // run_to_anchor with the world option on: own 3, member 4.
    void trafo_anchor(session_client_core& core, flow_fake& fake, bool option = true) {
        state_of(core).own_id_ = 3;
        auto start = start_message(9, 3);
        start.world_options = option ? bmmo::session::WORLD_TRAFO_INPUTS : 0u;
        ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.build_id = "build-x";
        report.own_name = "Me";
        core.anchor(report);
    }

    // The anchor, then assigned at base 120 and stepped to 125.
    void trafo_tick_125(session_client_core& core, flow_fake& fake) {
        trafo_anchor(core, fake);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        state_of(core).held_floor_ = 125;
        ASSERT_EQ(core.current_tick(), 125u);
    }

    bmmo::session_event_msg trafo_message(uint32_t player, uint8_t type, uint8_t flags, uint32_t tick, uint32_t origin) {
        auto msg = event_message(player, bmmo::session::event_type::Trafo, tick, origin);
        msg.ball_type = type;
        msg.flags = flags;
        if (!bmmo::session::trafo_is_reset(flags)) {
            msg.position[0] = 1.5f;
            msg.position[1] = 2.5f;
            msg.position[2] = -3.5f;
        }
        return msg;
    }

    std::vector<std::string> only(const std::vector<std::string>& calls, const std::string& prefix) {
        std::vector<std::string> out;
        for (const auto& c: calls)
            if (c.rfind(prefix, 0) == 0) out.push_back(c);
        return out;
    }

    TEST(SessionClientCoreTrafo, TheCapabilityNeedsItsOperationsAndIsDeclared) {
        for (int missing = 0; missing < 8; ++missing) {
            flow_fake fake;
            trafo_fake t;
            t.calls = &fake.calls;
            session_platform p = fake.make_flow();
            t.install(p);
            if (missing == 0) p.poll_trafo = nullptr;
            if (missing == 1) p.make_trafo_set = nullptr;
            if (missing == 2) p.run_trafo = nullptr;
            if (missing == 3) p.trafo_final = nullptr;
            if (missing == 4) p.trafo_frame = nullptr;
            if (missing == 5) p.trafo_type_set = nullptr;
            if (missing == 6) p.trafo_reconcile = nullptr;
            if (missing == 7) p.retire_trafo_sets = nullptr;
            EXPECT_THROW({ session_client_core c(p, trafo_caps()); }, std::invalid_argument) << missing;
            EXPECT_NO_THROW({ session_client_core c(p, session_capabilities{}); }) << "not needed without the capability";
        }
        flow_fake fake;
        trafo_fake t;
        t.calls = &fake.calls;
        session_platform p = fake.make_flow();
        t.install(p);
        session_client_core core(std::move(p), trafo_caps());
        trafo_anchor(core, fake);
        const auto ready = decode<bmmo::session_ready_msg>(fake.sent.at(0).first);
        EXPECT_NE(ready.caps & bmmo::session::CAP_TRAFO_INPUTS, 0u);
        EXPECT_TRUE(core.trafo_active());
        // Without the world option the capability is declared and nothing runs.
        flow_fake off;
        trafo_fake t2;
        t2.calls = &off.calls;
        session_platform p2 = off.make_flow();
        t2.install(p2);
        session_client_core other(std::move(p2), trafo_caps());
        trafo_anchor(other, off, false);
        EXPECT_FALSE(other.trafo_active());
        EXPECT_TRUE(only(off.calls, "make_trafo_set").empty());
        EXPECT_NE(decode<bmmo::session_ready_msg>(off.sent.at(0).first).caps & bmmo::session::CAP_TRAFO_INPUTS, 0u);
    }

    // Step S5 (sector placement design 2.4): a host with the placement
    // capability sends CAP_PLACEMENT and its anchor's signature in
    // SessionReady; without it the signature is 0 and no cap is declared,
    // whatever the host put in its report.  The core keeps the world options.
    TEST(SessionClientCorePlacement, SessionReadyCarriesTheSignatureOnlyWithTheCapability) {
        for (const bool capable: {true, false}) {
            flow_fake fake;
            session_capabilities caps;
            caps.placement = capable;
            session_client_core core(fake.make_flow(), caps);
            state_of(core).own_id_ = 3;
            auto start = start_message(9, 3);
            start.world_options = bmmo::session::WORLD_PLACEMENT_CLONES;
            ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
            EXPECT_EQ(bmmo::session::WORLD_PLACEMENT_CLONES, core.world_options());
            core.level_requested();
            bmmo::session::anchor_report report;
            report.build_id = "build-x";
            report.own_name = "Me";
            report.placement = 0x5d22d3b5a61c38cbULL;
            core.anchor(report);
            const auto ready = decode<bmmo::session_ready_msg>(fake.sent.at(0).first);
            EXPECT_EQ(capable, (ready.caps & bmmo::session::CAP_PLACEMENT) != 0) << capable;
            EXPECT_EQ(capable ? 0x5d22d3b5a61c38cbULL : 0u, ready.anchor_placement) << capable;
        }
    }

    // Journal v6: the client's header carries the session's world options and
    // the signature SessionReady carried.
    TEST(SessionClientCorePlacement, TheJournalHeaderCarriesOptionsAndSignature) {
        journal_dir dir("placement_header");
        journal_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_journal(), session_capabilities{});
        start_session(core);
        state_of(core).world_options_ = bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS;
        core.journal().open(bmmo::physics::world_hash{}, "build-x", "Me", 0xabcdef0123456789ULL);
        core.journal().close("bye");
        bmmo::session::journal j;
        std::string error;
        ASSERT_TRUE(bmmo::session::read_journal(fake.path, j, error)) << error;
        EXPECT_EQ(bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS, j.header.world_options);
        EXPECT_EQ(0xabcdef0123456789ULL, j.header.anchor_placement);
    }

    TEST(SessionClientCoreTrafo, SetsAtTheAnchorAndOnFirstSightNeverInAReplay) {
        flow_fake fake;
        trafo_fake t;
        t.calls = &fake.calls;
        session_platform p = fake.make_flow();
        t.install(p);
        session_client_core core(std::move(p), trafo_caps());
        trafo_anchor(core, fake);
        EXPECT_EQ(only(fake.calls, "make_trafo_set"), (std::vector<std::string>{"make_trafo_set:3", "make_trafo_set:4"}))
            << "every member and the own player, by id";
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        size_t from = fake.calls.size();
        // Step TRL: an event of a life not announced yet makes no set (it is
        // parked; a guessed life could be one the server never held) - its
        // member message does.
        core.on_session_event(event_message(7, bmmo::session::event_type::Sector, 130, 130));
        EXPECT_TRUE(only(since(fake.calls, from), "make_trafo_set").empty()) << "an unannounced life: none";
        set_test_life(core, 7, 5);
        core.on_session_event(event_message(7, bmmo::session::event_type::Physicalize, 131, 131));
        core.on_session_event(event_message(7, bmmo::session::event_type::Sector, 132, 132));
        EXPECT_EQ(only(since(fake.calls, from), "make_trafo_set"), std::vector<std::string>{"make_trafo_set:7"})
            << "once, at the player's first message of a known life";
        bmmo::session_remote_input_msg relay;
        relay.session = 9;
        relay.tick = 124;
        relay.epoch = 120;
        bmmo::session_remote_input_msg::entry entry;
        entry.player = 8;
        entry.frames.resize(1);
        relay.entries.push_back(entry);
        from = fake.calls.size();
        core.on_session_remote_input(relay);
        EXPECT_TRUE(only(since(fake.calls, from), "make_trafo_set").empty()) << "step TRL: no life to name, no set";
        EXPECT_EQ(state_of(core).trafo_unnamed_lives_, 1u);
        set_test_life(core, 8, 6);
        core.on_session_remote_input(relay);
        EXPECT_EQ(only(since(fake.calls, from), "make_trafo_set"), std::vector<std::string>{"make_trafo_set:8"});
        // A replayed tick applies inputs and never builds a set.
        state_of(core).trafo_log_.add(bmmo::session::trafo_input{9, 0, 0, 122, 122, {}, 5, false});
        from = fake.calls.size();
        steps{core}.world_hooks().before_replayed_tick(122);
        const auto calls = since(fake.calls, from);
        EXPECT_TRUE(only(calls, "make_trafo_set").empty());
        EXPECT_EQ(only(calls, "run_trafo"), std::vector<std::string>{"run_trafo:9:0:0:122:5"});
    }

    TEST(SessionClientCoreTrafo, TheHoldStampsAndWaitsForTheAssignment) {
        flow_fake fake;
        trafo_fake t;
        t.calls = &fake.calls;
        session_platform p = fake.make_flow();
        t.install(p);
        session_client_core core(std::move(p), trafo_caps());
        trafo_anchor(core, fake);
        auto& s = state_of(core);
        // Before the assignment: the report waits, nothing is sent.
        bmmo::session::trafo_report wood;
        wood.flags = bmmo::session::TRAFO_EXPLODE;
        wood.type = 0;
        wood.position[0] = 10.0f;
        wood.source = "activation";
        t.next_poll.reports.push_back(wood);
        size_t sent = fake.sent.size();
        steps{core}.poll_own_trafo(0);
        EXPECT_EQ(fake.sent.size(), sent);
        EXPECT_EQ(s.trafo_pending_.size(), 1u);
        EXPECT_EQ(s.trafo_log_.size(), 0u);
        // The first assigned frame (tick 120): stamped 120 + lead (7).
        core.on_session_assign(assign_message(9, 120));
        steps{core}.hold_pending_trafo(120);
        ASSERT_EQ(fake.sent.size(), sent + 1);
        auto event = decode<bmmo::session_event_msg>(fake.sent.back().first);
        EXPECT_EQ(event.type, bmmo::session::event_type::Trafo);
        EXPECT_EQ(event.tick, 127u);
        EXPECT_EQ(event.ball_type, 0);
        EXPECT_EQ(event.flags, 0);
        EXPECT_EQ(event.position[0], 10.0f);
        ASSERT_EQ(s.trafo_log_.at(127).size(), 1u);
        EXPECT_TRUE(s.trafo_log_.at(127)[0].own);
        EXPECT_EQ(s.trafo_log_.at(127)[0].seed, bmmo::session::trafo_seed(11u, 3u, 127u, 0));
        EXPECT_TRUE(s.trafo_pending_.empty());
        // Assigned: tick + 1 + lead; a reset carries no position.
        s.frames_since_anchor_ = 6;   // current tick 125
        bmmo::session::trafo_report reset;
        reset.flags = bmmo::session::TRAFO_RESET;
        reset.type = 2;
        reset.position[0] = 99.0f;
        t.next_poll.reports.push_back(reset);
        steps{core}.poll_own_trafo(125);
        event = decode<bmmo::session_event_msg>(fake.sent.back().first);
        EXPECT_EQ(event.tick, 133u);
        EXPECT_EQ(event.flags, bmmo::session::TRAFO_RESET);
        EXPECT_EQ(event.position[0], 0.0f);
        EXPECT_EQ(s.trafo_log_.at(133).at(0).seed, 0);
        // Option E (lead 7 <= 66, asked with ":e"): the Delayer's prediction
        // is the stamp when it is far enough ahead, never earlier than the hold's.
        bmmo::session::trafo_report predicted = wood;
        predicted.predicted_tick = 192;
        predicted.type = 1;
        t.next_poll.reports.push_back(predicted);
        bmmo::session::trafo_report early = wood;
        early.predicted_tick = 128;
        early.type = 2;
        t.next_poll.reports.push_back(early);
        const size_t from = fake.calls.size();
        steps{core}.poll_own_trafo(125);
        EXPECT_EQ(only(since(fake.calls, from), "poll_trafo"), std::vector<std::string>{"poll_trafo:125:e"});
        EXPECT_EQ(s.trafo_log_.at(192).size(), 1u);
        EXPECT_EQ(s.trafo_log_.at(133).size(), 2u) << "the early prediction is stamped tick + 1 + lead";
        // The effect checks' leaks are counted.
        t.next_poll.leaks = 2;
        t.next_poll.first_leak = "body Ball_Wood_piece01";
        steps{core}.poll_own_trafo(126);
        EXPECT_EQ(core.trafo_original_leaks(), 2u);
    }

    // A world the engine can rewind: no tracked body, every step taken, and
    // the core's before_replayed_tick called before each replayed step.
    session_platform trafo_rewind_platform(flow_fake& fake, trafo_fake& t) {
        session_platform p = fake.make_flow();
        t.calls = &fake.calls;
        t.install(p);
        p.bind_world = [&fake](const bmmo::session::core_world_hooks& hooks) {
            fake.calls.push_back("bind_world");
            bmmo::session::rollback_world world;
            world.get_body = [](const std::string&, bmmo_physics_body_state&) { return false; };
            world.get_nav = [](const std::string&, bmmo_physics_nav_state&) { return false; };
            world.pre_step = hooks.before_replayed_tick;
            world.step = [&fake]() {
                fake.calls.push_back("step");
                return true;
            };
            return world;
        };
        return p;
    }

    void trafo_record_ticks(session_client_core& core, uint32_t from, uint32_t to) {
        for (uint32_t tick = from; tick <= to; ++tick)
            state_of(core).rollback_.record(core.platform().bind_world(bmmo::session::core_world_hooks{}), tick,
                                            bmmo::session::rollback_tracked{}, {});
    }

    TEST(SessionClientCoreTrafo, InputsAppliedLiveAndInTheReplayOfALateOne) {
        for (const bool retail: {false, true}) {
            flow_fake fake;
            trafo_fake t;
            session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
            trafo_tick_125(core, fake);
            core.config.rollback = true;
            if (retail) {   // the retail host's switches before R3: the trafo rewind does not need them
                core.config.lifecycle_rewind = false;
                core.config.own_echoes = false;
            }
            trafo_record_ticks(core, 110, 125);
            // A remote explosion on time: logged, applied at its tick live.
            core.on_session_event(trafo_message(4, 1, 0, 126, 124));
            EXPECT_FALSE(state_of(core).input_rewind_pending_);
            size_t from = fake.calls.size();
            steps{core}.apply_trafo_inputs(126, false);
            EXPECT_EQ(only(since(fake.calls, from), "run_trafo"),
                      std::vector<std::string>{"run_trafo:4:1:0:126:" + std::to_string(bmmo::session::trafo_seed(11u, 4u, 126u, 1))});
            // A late one (a server-stamped final reset of player 4, applied
            // at 122): never applied live, the input rewind from it.
            core.on_session_event(trafo_message(4, 2, bmmo::session::TRAFO_RESET | bmmo::session::TRAFO_SERVER
                                                          | bmmo::session::TRAFO_FINAL, 122, 122));
            auto& s = state_of(core);
            ASSERT_TRUE(s.input_rewind_pending_);
            EXPECT_EQ(s.input_rewind_tick_, 122u);
            EXPECT_TRUE(s.input_rewind_trafo_);
            EXPECT_EQ(s.trafo_late_, 1u);
            from = fake.calls.size();
            steps{core}.replay_corrected_inputs();
            const auto calls = since(fake.calls, from);
            EXPECT_EQ(only(calls, "run_trafo"), std::vector<std::string>{"run_trafo:4:2:7:122:0"})
                << "re-applied once, in the replayed tick 122";
            EXPECT_EQ(only(calls, "trafo_final"), std::vector<std::string>{"trafo_final:4:2:122"});
            EXPECT_EQ(only(calls, "step").size(), 4u) << "121 -> 125";
            EXPECT_EQ(s.input_rewinds_, 1u);
            EXPECT_FALSE(s.input_rewind_pending_);
            EXPECT_EQ(s.trafo_missed_, 0u);
        }
    }

    TEST(SessionClientCoreTrafo, AnInputPastTheHistoryAsksForAResync) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        core.config.rollback = true;
        trafo_record_ticks(core, 122, 125);
        const size_t from = fake.calls.size();
        core.on_session_event(trafo_message(4, 0, 0, 121, 121));   // its anchor (120) is not recorded
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(state_of(core).trafo_missed_, 1u);
        EXPECT_FALSE(state_of(core).input_rewind_pending_);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "log:0:resync requested (a trafo input could not be replayed) at tick 125"),
                  calls.end());
        // Without the input rewind (rollback off): counted, no resync.
        flow_fake off;
        trafo_fake t2;
        session_client_core other(trafo_rewind_platform(off, t2), trafo_caps());
        trafo_tick_125(other, off);
        other.config.rollback = false;
        other.on_session_event(trafo_message(4, 0, 0, 124, 124));
        EXPECT_EQ(state_of(other).trafo_missed_, 1u);
        EXPECT_EQ(state_of(other).resyncs_sent_, 0u);
    }

    TEST(SessionClientCoreTrafo, OwnEchoesMoveOrDropTheInputAndRewind) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        core.config.rollback = true;
        trafo_record_ticks(core, 110, 125);
        auto& s = state_of(core);
        const auto own = [&](uint8_t type, uint8_t flags, uint32_t stamp) {
            bmmo::session::trafo_input in;
            in.player = 3;
            in.type = type;
            in.flags = flags;
            in.origin = in.applied = stamp;
            in.own = true;
            s.trafo_log_.add(in);
        };
        own(0, 0, 123);
        own(1, 0, 130);
        own(2, bmmo::session::TRAFO_RESET, 121);
        own(1, bmmo::session::TRAFO_RESET, 124);
        // Acked: nothing.
        auto acked = trafo_message(3, 0, 0, 123, 123);
        core.on_session_event(std::move(acked));
        steps{core}.process_own_echoes();
        EXPECT_FALSE(s.input_rewind_pending_);
        EXPECT_EQ(s.own_echo_acked_, 1u);
        // MOVED 123 -> 125: re-keyed with the applied tick's seed, rewound from 123.
        auto moved = trafo_message(3, 0, 0, 125, 123);
        moved.state = bmmo::session::EVENT_STATE_MOVED;
        core.on_session_event(std::move(moved));
        steps{core}.process_own_echoes();
        EXPECT_TRUE(s.trafo_log_.at(123).empty());
        ASSERT_EQ(s.trafo_log_.at(125).size(), 1u);
        EXPECT_EQ(s.trafo_log_.at(125)[0].seed, bmmo::session::trafo_seed(11u, 3u, 125u, 0));
        EXPECT_TRUE(s.input_rewind_pending_);
        EXPECT_EQ(s.input_rewind_tick_, 123u);
        steps{core}.replay_corrected_inputs();
        // MOVED ahead of both ticks (130 -> 140): re-keyed, no rewind.
        auto ahead = trafo_message(3, 1, 0, 140, 130);
        ahead.state = bmmo::session::EVENT_STATE_MOVED;
        core.on_session_event(std::move(ahead));
        steps{core}.process_own_echoes();
        EXPECT_EQ(s.trafo_log_.at(140).size(), 1u);
        EXPECT_FALSE(s.input_rewind_pending_);
        // REJECTED: dropped, rewound from its origin.
        auto rejected = trafo_message(3, 2, bmmo::session::TRAFO_RESET, 121, 121);
        rejected.state = bmmo::session::EVENT_STATE_REJECTED;
        core.on_session_event(std::move(rejected));
        steps{core}.process_own_echoes();
        EXPECT_TRUE(s.trafo_log_.at(121).empty());
        EXPECT_TRUE(s.input_rewind_pending_);
        EXPECT_EQ(s.input_rewind_tick_, 121u);
        steps{core}.replay_corrected_inputs();
        // SUPERSEDED: dropped too.
        auto superseded = trafo_message(3, 1, bmmo::session::TRAFO_RESET, 124, 124);
        superseded.state = bmmo::session::EVENT_STATE_SUPERSEDED;
        core.on_session_event(std::move(superseded));
        steps{core}.process_own_echoes();
        EXPECT_TRUE(s.trafo_log_.at(124).empty());
        EXPECT_EQ(s.trafo_echo_moved_, 2u);
        EXPECT_EQ(s.trafo_echo_dropped_, 2u);
        // A server-stamped reset of our own set is an input, not an echo.
        core.on_session_event(trafo_message(3, 0, bmmo::session::TRAFO_RESET | bmmo::session::TRAFO_SERVER, 150, 150));
        EXPECT_EQ(s.trafo_log_.at(150).size(), 1u);
        EXPECT_TRUE(s.own_echoes_.empty());
    }

    TEST(SessionClientCoreTrafo, TheReconcilesDecisionTable) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        auto& s = state_of(core);
        // This world: player 3 wood live from 110 (equal at the server),
        // player 3 stone live from 112 (the server has none: extra here),
        // player 4 wood from 100 (the server's is from 115: different), and
        // player 4 paper has nothing here (the server's has bodies: missing).
        const auto explosion = [&](uint32_t player, uint8_t type, uint32_t tick) {
            bmmo::session::trafo_input in;
            in.player = player;
            in.type = type;
            in.origin = in.applied = tick;
            s.trafo_log_.add(in);
        };
        explosion(3, 0, 110);
        explosion(3, 1, 112);
        explosion(4, 0, 100);
        t.bodies[{3, 0}] = 3;
        t.bodies[{3, 1}] = 3;
        t.bodies[{4, 0}] = 3;
        bmmo::session_resync_state_msg st;
        const auto entry = [&](uint32_t player, uint8_t type, bool bodies, uint32_t applied, bool final = false) {
            bmmo::session::trafo_ledger_entry e;
            e.player = player;
            e.type = type;
            e.has_explosion = applied != 0;
            e.explode_applied = applied;
            e.position[0] = 7.0f;
            e.has_bodies = bodies;
            e.final = final;
            e.final_applied = final ? 118 : 0;
            st.trafo_ledger.push_back(e);
        };
        entry(3, 0, true, 110);
        entry(3, 1, false, 0);
        entry(3, 2, false, 0);
        entry(4, 0, true, 115);
        entry(4, 1, false, 0, true);
        entry(4, 2, true, 117);
        entry(5, 0, false, 0);   // a set this world has not seen: built
        std::set<std::string> gone;
        std::vector<bmmo::session::journal_trafo_entry> journal;
        const size_t from = fake.calls.size();
        steps{core}.reconcile_trafo(st, 119u, gone, journal);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(only(calls, "make_trafo_set"), std::vector<std::string>{"make_trafo_set:5"});
        EXPECT_EQ(only(calls, "trafo_final"), std::vector<std::string>{"trafo_final:4:1:118"});
        const std::vector<std::string> expected = {
            "trafo_reconcile:unmake:3:1",
            "trafo_reconcile:unmake:4:0",
            "trafo_reconcile:remake:4:0:" + std::to_string(bmmo::session::trafo_seed(11u, 4u, 115u, 0)) + ":119",
            "trafo_reconcile:remake:4:2:" + std::to_string(bmmo::session::trafo_seed(11u, 4u, 117u, 2)) + ":119"};
        EXPECT_EQ(only(calls, "trafo_reconcile"), expected);
        EXPECT_EQ(only(calls, "mute_event_log"), (std::vector<std::string>{"mute_event_log:on", "mute_event_log:off"}));
        // Every piece of a type set the server has no bodies for is kept out.
        EXPECT_EQ(gone.count("piece1_1_3"), 1u);
        EXPECT_EQ(gone.count("piece2_1_4"), 1u);
        EXPECT_EQ(gone.count("piece1_0_5"), 1u);
        EXPECT_EQ(gone.count("piece1_0_3"), 0u) << "kept: the server has it";
        EXPECT_EQ(gone.count("piece1_2_4"), 0u) << "remade";
        EXPECT_EQ(s.trafo_unmade_, 2u);
        EXPECT_EQ(s.trafo_remade_, 2u);
        // The log learned the server's explosions.
        ASSERT_NE(s.trafo_log_.newest_explosion(4, 1, 0, 119), nullptr);
        EXPECT_EQ(s.trafo_log_.newest_explosion(4, 1, 0, 119)->applied, 115u);
        ASSERT_NE(s.trafo_log_.newest_explosion(4, 1, 2, 119), nullptr);
        EXPECT_EQ(s.trafo_log_.newest_explosion(4, 1, 2, 119)->applied, 117u);
        // The journal's section: one entry per type set, in key order.
        ASSERT_EQ(journal.size(), 9u) << "players 3, 4, 5 x three types";
        const auto find = [&](uint32_t player, uint8_t type) {
            for (const auto& j: journal) if (j.player == player && j.type == type) return j;
            return bmmo::session::journal_trafo_entry{};
        };
        EXPECT_EQ(find(3, 0).action, bmmo::session::JOURNAL_TRAFO_KEPT);
        EXPECT_EQ(find(3, 1).action, bmmo::session::JOURNAL_TRAFO_UNMADE);
        EXPECT_EQ(find(4, 0).action, bmmo::session::JOURNAL_TRAFO_UNMADE | bmmo::session::JOURNAL_TRAFO_REMADE);
        EXPECT_EQ(find(4, 2).action, bmmo::session::JOURNAL_TRAFO_REMADE);
        EXPECT_EQ(find(4, 1).bits & 8, 8) << "final";
    }

    TEST(SessionClientCoreTrafo, RetirementForgetsTheSetAndPrunesTheLog) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        auto& s = state_of(core);
        bmmo::session::trafo_input old;
        old.player = 4;
        old.origin = old.applied = 10;
        s.trafo_log_.add(old);
        old.origin = old.applied = 20;
        s.trafo_log_.add(old);
        // The sets' pieces are known from the make on (a snapshot row of
        // one restores it only on a breach): 3 per type set here.
        EXPECT_EQ(s.trafo_pieces_.count("piece2_1_4"), 1u);
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_3"), 1u);
        const size_t pieces = s.trafo_pieces_.size();
        t.retire_answer = {bmmo::session::trafo_key{4, 1}};
        steps{core}.retire_trafo(300);
        EXPECT_EQ(only(fake.calls, "retire_trafo_sets"), std::vector<std::string>{"retire_trafo_sets:300:96"});
        EXPECT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 1}), 0u);
        EXPECT_EQ(s.trafo_pieces_.count("piece2_1_4"), 0u) << "the retired set's pieces are forgotten";
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_3"), 1u) << "the others' stay";
        EXPECT_EQ(s.trafo_pieces_.size(), pieces - 9u);
        EXPECT_EQ(s.trafo_log_.size(), 1u) << "the newest explosion stays";
        // Step TRL ('Key by life, no resume'): a retired set is gone for
        // good - the next sight of the life makes nothing.
        const size_t from = fake.calls.size();
        core.on_session_event(event_message(4, bmmo::session::event_type::Sector, 301, 301));
        EXPECT_TRUE(only(since(fake.calls, from), "make_trafo_set").empty());
        EXPECT_EQ(s.trafo_retired_sights_, 1u);
        EXPECT_EQ(s.trafo_retired_sets_.count(bmmo::session::trafo_key{4, 1}), 1u);
    }

    // A frame runs the trafo inputs before the union's pass and the trafo
    // frame after it, the hold and the retirement after the record.
    TEST(SessionClientCoreTrafo, TheFrameOrder) {
        flow_fake fake;
        trafo_fake t;
        session_platform p = fake.make_flow();
        t.calls = &fake.calls;
        t.install(p);
        session_client_core core(std::move(p), trafo_caps());
        trafo_tick_125(core, fake);
        state_of(core).trafo_log_.add(bmmo::session::trafo_input{4, 1, 0, 126, 126, {}, 9, false});
        const size_t from = fake.calls.size();
        core.pre_step();
        EXPECT_TRUE(core.post_step(true));
        const auto calls = since(fake.calls, from);
        const auto at = [&](const std::string& prefix) {
            for (size_t i = 0; i < calls.size(); ++i)
                if (calls[i].rfind(prefix, 0) == 0) return static_cast<long>(i);
            return -1L;
        };
        ASSERT_GE(at("run_trafo:4:1:0:126:9"), 0);
        ASSERT_GE(at("sector_manager_active"), 0);
        ASSERT_GE(at("trafo_frame:126"), 0);
        EXPECT_LT(at("run_trafo:4:1:0:126:9"), at("sector_manager_active")) << "inputs before the union's pass";
        EXPECT_LT(at("sector_manager_active"), at("trafo_frame:126")) << "the trafo frame after it";
        ASSERT_GE(at("poll_sector:126"), 0);
        EXPECT_LT(at("poll_sector:126"), at("poll_trafo:126"));
        EXPECT_LT(at("poll_trafo:126"), at("retire_trafo_sets:126"));
    }

    // Step G3 (world-agreement-gaps-design.md 2.2, 2.5, U2): the candidates
    // of the place point are the balls with a body in this world, one per
    // player, by player id - the own ball's entity with a body, every mirror
    // entity with a body whether or not it is driven, nothing a replay holds
    // out, nothing without a body; two bodies of one player are counted and
    // the first by name taken.
    TEST(SessionClientCoreProbes, CandidatesAreTheBallsWithABodyByPlayerId) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        auto& s = state_of(core);
        s.own_id_ = 3;
        s.own_ball_entities_ = {"Ball_Paper", "Ball_Wood"};       // the paper ball has no body now
        s.mirror_owners_["Ball_Wood_Peer_9"] = {9, 1};            // no body: not a candidate
        s.mirror_owners_["Ball_Stone_Peer_4"] = {4, 2};
        s.mirror_owners_["Ball_Wood_Peer_4"] = {4, 1};            // a second body of 4: the invariant guard
        s.mirror_owners_["Ball_Paper_Peer_2"] = {2, 0};           // undriven: still a candidate
        s.remotes_[2].navigation = false;
        fake.bodies_there = {"Ball_Wood", "Ball_Stone_Peer_4", "Ball_Wood_Peer_4", "Ball_Paper_Peer_2"};
        const auto all = steps{core}.probe_candidates(std::function<bool(const std::string&)>{});
        ASSERT_EQ(all.size(), 3u);
        EXPECT_EQ(all[0].player, 2u);
        EXPECT_EQ(all[0].entity, "Ball_Paper_Peer_2");
        EXPECT_EQ(all[1].player, 3u);
        EXPECT_EQ(all[1].entity, "Ball_Wood");
        EXPECT_EQ(all[2].player, 4u);
        EXPECT_EQ(all[2].entity, "Ball_Stone_Peer_4") << "the first by name";
        EXPECT_EQ(core.probe_two_bodies(), 1u);
        // A replay holds the newborn paper ball out: no candidate of player 2.
        const auto held = steps{core}.probe_candidates([](const std::string& e) { return e == "Ball_Paper_Peer_2"; });
        ASSERT_EQ(held.size(), 2u);
        EXPECT_EQ(held[0].player, 3u);
        EXPECT_EQ(held[1].player, 4u);
        // The own ball dead (no body): the own player has none.
        fake.bodies_there = {"Ball_Stone_Peer_4"};
        const auto dead = steps{core}.probe_candidates(std::function<bool(const std::string&)>{});
        ASSERT_EQ(dead.size(), 1u);
        EXPECT_EQ(dead[0].player, 4u);
    }

    // Step G3 (design 2.4, 5): under WORLD_PROXIMITY_UNION a host that places
    // sends its rewire's signature in SessionReady; without the world option,
    // or without place_probes, it sends 0 whatever its report says.
    TEST(SessionClientCoreProbes, SessionReadyCarriesTheProbeSignatureUnderTheBit) {
        for (const int arm: {0, 1, 2}) {   // 0: bit and op, 1: bit without op, 2: op without bit
            flow_fake fake;
            session_platform p = fake.make_flow();
            if (arm != 1) p.place_probes = [](const std::vector<bmmo::session::probe_candidate>&) {};
            session_client_core core(std::move(p), session_capabilities{});
            state_of(core).own_id_ = 3;
            auto start = start_message(9, 3);
            start.world_options = arm == 2 ? 0u : bmmo::session::WORLD_PROXIMITY_UNION;
            ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
            EXPECT_EQ(arm == 0, core.proximity_union_active()) << arm;
            core.level_requested();
            bmmo::session::anchor_report report;
            report.build_id = "build-x";
            report.own_name = "Me";
            report.probes = 0x2c6aa1f5d1e0b7c3ULL;
            core.anchor(report);
            const auto ready = decode<bmmo::session_ready_msg>(fake.sent.at(0).first);
            EXPECT_EQ(arm == 0 ? 0x2c6aa1f5d1e0b7c3ULL : 0u, ready.anchor_probes) << arm;
            EXPECT_EQ(bmmo::session::PHYSICS_PROTOCOL_VERSION, ready.protocol);
        }
    }

    // Steps G3 and G5 in the frame (design 2.5 step 7b, 2.6): under the bit
    // the place point comes right after the rewinds' hook and before the held
    // body changes, with the world's balls; and a mechanism's script wake is
    // no BodyRevived any more - nothing is sent, the count is kept.
    TEST(SessionClientCoreProbes, Step7bPlacesAfterTheRewindsAndNoBodyRevivedIsSent) {
        journal_dir dir("probes_frame");
        flow_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_capabilities caps;
        caps.own_body_control = true;
        session_platform p = frame_platform(fake, true);
        std::vector<std::vector<bmmo::session::probe_candidate>> placed;
        p.place_probes = [&fake, &placed](const std::vector<bmmo::session::probe_candidate>& c) {
            fake.calls.push_back("place_probes:" + std::to_string(c.size()));
            placed.push_back(c);
        };
        session_client_core core(std::move(p), caps);
        run_to_tick_125_four(core, fake);
        auto& s = state_of(core);
        s.world_options_ = bmmo::session::WORLD_PROXIMITY_UNION;
        s.probes_active_ = true;
        auto& four = s.remotes_[4];
        four.entity = "Ball_0_BMMO_4";
        four.physicalized = true;
        four.navigation = true;
        s.mirror_owners_["Ball_0_BMMO_4"] = {4, 0};
        s.own_ball_entities_ = {"Ball_Wood"};
        for (const char* name: {"Ball_Wood", "Ball_0_BMMO_4"}) {   // the journal fake's body_state answers from these
            bmmo_physics_body_state body{};
            std::snprintf(body.name, sizeof(body.name), "%s", name);
            body.rotation[3] = 1.0;
            fake.bodies.push_back(body);
        }
        fake.own_physical = true;
        fake.driven = "Ball_Wood";
        fake.replica_name = "Ball_Wood";
        fake.mechanisms = {"P_Box"};
        bmmo::session::body_event box_wakeup;
        box_wakeup.what = bmmo::session::body_event::kind::wakeup;
        box_wakeup.name = "P_Box";
        fake.body_events = {box_wakeup};
        const uint64_t sent_before = s.events_sent_;
        ASSERT_EQ(core.next_tick(), 126u);
        size_t from = fake.calls.size();
        core.pre_step();
        const auto pre = since(fake.calls, from);
        const auto at = [&pre](const std::string& call) {
            return static_cast<size_t>(std::find(pre.begin(), pre.end(), call) - pre.begin());
        };
        std::string listed;
        for (const std::string& call: pre) listed += call + " | ";
        ASSERT_LT(at("place_probes:2"), pre.size()) << listed;
        ASSERT_LT(at("hook:after_rewinds:126"), at("place_probes:2"));
        for (size_t i = at("hook:after_rewinds:126") + 1; i < at("place_probes:2"); ++i)
            EXPECT_EQ(pre[i].rfind("body_state:", 0), 0u) << "only the candidates' body tests between: " << listed;
        EXPECT_EQ(at("place_probes:2") + 1, at("sector_manager_active:off")) << listed;
        ASSERT_EQ(placed.size(), 1u);
        EXPECT_EQ(placed[0][0].player, s.own_id_ < 4 ? s.own_id_ : 4u);
        ASSERT_TRUE(core.post_step(true));
        EXPECT_EQ(s.events_sent_, sent_before) << "no BodyRevived under the proximity union";
        EXPECT_EQ(core.revived_local(), 1u);
    }


    // ------------------------------------------------- member lives (step M1)
    // world-agreement-gaps-design.md 2.7, 2.8, 7.1 U5, U6.

    bmmo::session_member_msg member_message(uint8_t kind, uint32_t id, uint16_t life, uint8_t order, uint32_t tick) {
        bmmo::session_member_msg msg;
        msg.session = 9;
        msg.kind = kind;
        msg.tick = tick;
        msg.entry.id = id;
        msg.entry.life = life;
        msg.entry.join_order = order;
        return msg;
    }

    // U5 (a), (b), (d), (j): an event of a life not announced yet is parked,
    // then released by its SessionMember and drained at its tick with the
    // life's join order: group P#k, the kick index from k, never 63.
    TEST(SessionClientCoreMemberLives, ParkedUntilTheMemberThenMadeWithItsJoinOrder) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        auto spawn = event_message(11, bmmo::session::event_type::Physicalize, 126, 126);
        spawn.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        core.on_session_event(std::move(spawn));
        EXPECT_EQ(core.events_parked(), 1u);
        EXPECT_TRUE(state_of(core).pending_events_.empty()) << "parked, not queued";
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 5, 120));
        ASSERT_EQ(state_of(core).pending_events_.size(), 1u) << "released into the drain";
        EXPECT_TRUE(state_of(core).parked_events_.empty());
        // A copy changes nothing.
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 5, 120));
        EXPECT_EQ(state_of(core).member_copies_, 1u);
        size_t from = fake.calls.size();
        steps{core}.drain_due_events(126);
        steps{core}.apply_held_lifecycle(126);
        const uint32_t kick = bmmo::session::spawn_direction_index(11, 5, 126);
        const auto calls = since(fake.calls, from);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "make_mirror:Ball_0_BMMO_11:P#5:kick" + std::to_string(kick)),
                  calls.end());
        EXPECT_EQ(core.mirrors_outside_lives(), 0u);
        EXPECT_EQ(core.member_life_outside(), 0u);
        EXPECT_EQ(steps{core}.join_rank(11, 126), 5u);
        // Our own id in a SessionMember is ignored.
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 3, 2, 9, 130));
        EXPECT_EQ(bmmo::session::find_member_life(core.member_lives(), 3, 2), nullptr);
    }

    // U5 (b)/(c): the member message comes after the event's tick has
    // passed - released, the event takes the late path at the next drain:
    // made at its stamp with the life's group and kick (P#5, never 63), and
    // its rewind asked for (outside this fixture's history: refused and
    // logged, as any late birth).
    TEST(SessionClientCoreMemberLives, AParkedEventPastItsTickTakesTheLatePath) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        auto spawn = event_message(11, bmmo::session::event_type::Physicalize, 122, 122);
        spawn.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        core.on_session_event(std::move(spawn));
        EXPECT_EQ(core.events_parked(), 1u);
        steps{core}.drain_due_events(126);
        EXPECT_TRUE(state_of(core).remotes_.empty()) << "parked: nothing made";
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 5, 110));
        const size_t from = fake.calls.size();
        steps{core}.drain_due_events(127);
        const auto calls = since(fake.calls, from);
        const uint32_t kick = bmmo::session::spawn_direction_index(11, 5, 122);
        EXPECT_NE(std::find(calls.begin(), calls.end(), "make_mirror:Ball_0_BMMO_11:P#5:kick" + std::to_string(kick)),
                  calls.end());
        EXPECT_NE(std::find(calls.begin(), calls.end(),
                            "log:0:lifecycle event of 11 stamped tick 122 is outside the rollback history at local tick 125: "
                            "the body set disagrees until a snapshot re-anchors it"),
                  calls.end());
        EXPECT_EQ(core.mirrors_outside_lives(), 0u);
        EXPECT_EQ(core.member_life_outside(), 0u);
        ASSERT_EQ(state_of(core).remotes_.count(11), 1u);
        EXPECT_TRUE(state_of(core).remotes_.at(11).physicalized);
    }

    // U5 (h): a rejoin on the same id - removed(p, 1, R) and added(p, 2, R)
    // in reverse arrival order: the old life ranks with its order up to R,
    // the new one with its own from R; no session end.  A removal before its
    // addition is kept until the addition comes.
    TEST(SessionClientCoreMemberLives, ARejoinKeepsEachLifesOrder) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 12, 1, 4, 110));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 12, 2, 7, 150));
        EXPECT_EQ(steps{core}.join_rank(12, 149), 4u) << "the old life, its removal not heard yet";
        EXPECT_EQ(steps{core}.join_rank(12, 150), 7u);
        core.on_session_member(member_message(bmmo::session::MEMBER_REMOVED, 12, 1, 4, 150));
        EXPECT_EQ(steps{core}.join_rank(12, 149), 4u);
        EXPECT_EQ(steps{core}.join_rank(12, 150), 7u);
        EXPECT_EQ(bmmo::session::find_member_life(core.member_lives(), 12, 1)->until, 150u);
        // removed before added
        core.on_session_member(member_message(bmmo::session::MEMBER_REMOVED, 13, 1, 2, 160));
        EXPECT_EQ(bmmo::session::find_member_life(core.member_lives(), 13, 1), nullptr);
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 13, 1, 2, 140));
        ASSERT_NE(bmmo::session::find_member_life(core.member_lives(), 13, 1), nullptr);
        EXPECT_EQ(bmmo::session::find_member_life(core.member_lives(), 13, 1)->until, 160u);
        EXPECT_EQ(core.session_phase(), client_core_state::phase::running);
        // An echo of our own previous life is no echo of ours.
        auto old = event_message(3, bmmo::session::event_type::Unphysicalize, 124, 124);
        old.life = 7;
        core.on_session_event(std::move(old));
        EXPECT_TRUE(state_of(core).own_echoes_.empty());
        EXPECT_EQ(state_of(core).own_other_life_echoes_, 1u);
    }

    // U5 (e): a life re-announced with another join order ends the session
    // locally (no mirror group could be trusted).
    TEST(SessionClientCoreMemberLives, AConflictingJoinOrderEndsTheSession) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        run_to_tick_125(core, fake);
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 5, 120));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 6, 120));
        EXPECT_EQ(core.session_phase(), client_core_state::phase::idle);
    }

    // A member message is taken while loading (not dropped like an event),
    // and one of another session is not.
    TEST(SessionClientCoreMemberLives, TakenWhileLoading) {
        flow_fake fake;
        session_client_core core(fake.make_flow(), session_capabilities{});
        state_of(core).own_id_ = 3;
        ASSERT_FALSE(core.begin(start_message(9, 3), bmmo::session::core_config{}).refused);
        core.level_requested();
        ASSERT_EQ(core.session_phase(), client_core_state::phase::loading);
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 11, 1, 5, 120));
        auto other = member_message(bmmo::session::MEMBER_ADDED, 12, 1, 6, 120);
        other.session = 8;
        core.on_session_member(other);
        EXPECT_NE(bmmo::session::find_member_life(core.member_lives(), 11, 1), nullptr);
        EXPECT_EQ(bmmo::session::find_member_life(core.member_lives(), 12, 1), nullptr);
    }

    // U7's writer half: a rejoin's PLAYER(-) precedes its PLAYER(+) at one
    // tick in file order whatever order the messages came in, at the
    // server's ticks, with the life's join order (no 255).
    TEST(SessionClientCoreMemberLives, JournalWritesEachLifeAtItsTicks) {
        journal_dir dir("lives");
        journal_fake fake;
        fake.path = dir.path / "client.bmjr";
        session_client_core core(fake.make_journal(), session_capabilities{});
        start_session(core);
        state_of(core).phase_ = client_core_state::phase::running;
        core.journal().open(bmmo::physics::world_hash{}, "build-x", "Me");
        state_of(core).assigned_ = true;
        state_of(core).tick_base_ = 100;
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 12, 1, 4, 110));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 12, 2, 7, 150));
        core.on_session_member(member_message(bmmo::session::MEMBER_REMOVED, 12, 1, 4, 150));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 14, 1, 5, 90));   // before the base
        core.journal().close("bye");
        bmmo::session::journal j;
        std::string error;
        ASSERT_TRUE(bmmo::session::read_journal(fake.path, j, error)) << error;
        std::vector<std::string> records;
        for (const auto& t: j.ticks)
            for (const auto& p: t.players)
                records.push_back(std::to_string(p.tick) + (p.added ? " +" : " -") + std::to_string(p.id) + " join "
                                  + std::to_string(p.join_order));
        const std::vector<std::string> expected = {"0 +3 join 1", "0 +4 join 0", "100 +14 join 5", "110 +12 join 4",
                                                   "150 -12 join 0", "150 +12 join 7"};
        EXPECT_EQ(records, expected);
    }

    // Step RK3 (record-format-gaps-design.md 8 row K3 = C4, option (c)): the
    // core activates the Level 1 tutorial's exit (platform tutorial_exit)
    // once, right before the world's frame 10 after its anchor - its tick is
    // the base + 9 - and only on Level 1 under WORLD_SESSION_CLOCK; a
    // re-assign before frame 10 neither moves it off the world's frame 10
    // (counted in world steps, never from the assigned tick) nor fires it
    // twice; a new anchor brings it back.
    TEST(SessionClientCoreTutorial, TheExitBeforeFrameTenOnLevelOneUnderTheBit) {
        for (const int arm: {0, 1, 2, 3}) {   // 0: L1 + bit, 1: L1 no bit, 2: L13 + bit, 3: L1 + bit, no op
            flow_fake fake;
            session_platform p = frame_platform(fake, false);
            std::vector<uint32_t> exits;
            std::vector<int> exit_frames;   // the 1-based pre_step the hook ran in
            int pre_steps = 0;
            if (arm != 3)
                p.tutorial_exit = [&exits, &exit_frames, &pre_steps](uint32_t tick, std::string&) {
                    exits.push_back(tick);
                    exit_frames.push_back(pre_steps);
                    return true;
                };
            session_client_core core(std::move(p), session_capabilities{});
            state_of(core).own_id_ = 3;
            auto start = start_message(9, 3);
            start.map.level = arm == 2 ? 13 : 1;
            start.world_options = arm == 1 ? 0u : bmmo::session::WORLD_SESSION_CLOCK;
            ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
            EXPECT_EQ(arm == 0 || arm == 3, core.tutorial_off()) << arm;
            for (uint32_t id = 5; id <= 10; ++id) set_test_life(core, id, 63);
            core.level_requested();
            bmmo::session::anchor_report report;
            report.build_id = "build-x";
            report.own_name = "Me";
            core.anchor(report);
            core.on_session_assign(assign_message(9, 120));
            for (int frame = 1; frame <= 14; ++frame) {
                if (frame == 5) core.on_session_assign(assign_message(9, 300));   // a re-assign: the world goes on
                pre_steps = frame;
                core.pre_step();
                ASSERT_TRUE(core.post_step(true)) << arm << " frame " << frame;
            }
            if (arm == 0)
                EXPECT_EQ(exit_frames, std::vector<int>({10})) << "once, in the world's 10th frame, the re-assign at 5 notwithstanding";
            else
                EXPECT_TRUE(exits.empty()) << arm;
            if (arm == 0) {
                // A new anchor (a level reload of the same session): due once more.
                core.level_requested();
                core.anchor(report);
                core.on_session_assign(assign_message(9, 500));
                for (int frame = 1; frame <= 10; ++frame) {
                    pre_steps = frame;
                    core.pre_step();
                    ASSERT_TRUE(core.post_step(true));
                }
                EXPECT_EQ(exit_frames, std::vector<int>({10, 10}));
                ASSERT_EQ(exits.size(), 2u);
                EXPECT_EQ(exits[1], 509u) << "before frame 10, tick base + 9";
            }
        }
    }

    // The one timing rule both the client core and physics_world call: due
    // when the world has stepped exit_frame - 1 times since its anchor.
    TEST(SessionClientCoreTutorial, ExitDueRule) {
        EXPECT_FALSE(bmmo::session::tutorial_exit_due(-1));
        EXPECT_FALSE(bmmo::session::tutorial_exit_due(0));
        EXPECT_FALSE(bmmo::session::tutorial_exit_due(8));
        EXPECT_TRUE(bmmo::session::tutorial_exit_due(9));
        EXPECT_FALSE(bmmo::session::tutorial_exit_due(10));
        EXPECT_TRUE(bmmo::session::tutorial_exit_due(15, 16));
        EXPECT_TRUE(bmmo::session::tutorial_exit_due(0, 1));
        static_assert(bmmo::session::tutorial_exit_due(bmmo::session::SESSION_CLOCK_TUTORIAL_EXIT_FRAME - 1));
    }

    // Step RK3H (the user, 2026-10-29: 'Hold frame-9 write'): the one
    // statement of the decision every world takes - the server's
    // physics_world from its option, the client core's tutorial_off() from
    // SessionStart - to switch the tutorial off (the exit) and hold its
    // frame-9 write (tutorial_factor_hold, made at the anchor, so before the
    // frame of the write and of the exit); the core's answer is the rule's
    // on every level with and without the bit; and the protocol that carries
    // the hold.
    TEST(SessionClientCoreTutorial, TheFrameNineHoldRule) {
        using bmmo::session::session_clock_tutorial_off;
        EXPECT_TRUE(session_clock_tutorial_off(true, 1));
        EXPECT_FALSE(session_clock_tutorial_off(false, 1));
        for (int level = 2; level <= 13; ++level) EXPECT_FALSE(session_clock_tutorial_off(true, level)) << level;
        static_assert(bmmo::session::SESSION_CLOCK_TUTORIAL_FREEZE_FRAME == 9, "K0's --globals-trace: tick 8, frame 9");
        static_assert(bmmo::session::SESSION_CLOCK_TUTORIAL_FREEZE_FRAME < bmmo::session::SESSION_CLOCK_TUTORIAL_EXIT_FRAME);
        // The hold is made at the anchor: no frame before the write's is a
        // frame the exit is due in, so the exit never runs before it.
        for (int64_t stepped = 0; stepped + 1 < bmmo::session::SESSION_CLOCK_TUTORIAL_FREEZE_FRAME; ++stepped)
            EXPECT_FALSE(bmmo::session::tutorial_exit_due(stepped)) << stepped;
        static_assert(bmmo::session::PHYSICS_PROTOCOL_VERSION >= 22, "protocol 22 carries the hold");
        for (const int level: {1, 2, 13})
            for (const bool bit: {false, true}) {
                flow_fake fake;
                session_platform p = frame_platform(fake, false);
                p.tutorial_exit = [](uint32_t, std::string&) { return true; };
                session_client_core core(std::move(p), session_capabilities{});
                state_of(core).own_id_ = 3;
                auto start = start_message(9, 3);
                start.map.level = level;
                start.world_options = bit ? bmmo::session::WORLD_SESSION_CLOCK : 0u;
                ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
                EXPECT_EQ(core.tutorial_off(), session_clock_tutorial_off(bit, level)) << level << " " << bit;
            }
    }

    // U6 at the rules' level (session/member_lives.hpp): the server's join
    // order skips a retiring order; the late base is not before the leaving
    // horizon; a leave with a ball always stays through the queue.
    TEST(MemberLives, ServerRules) {
        EXPECT_EQ(bmmo::session::lowest_free_join_order({0, 2}, {1}), 3);
        EXPECT_EQ(bmmo::session::lowest_free_join_order({0, 2}, {}), 1);
        EXPECT_EQ(bmmo::session::lowest_free_join_order({}, {0}), 1);
        EXPECT_EQ(bmmo::session::late_join_base(1600, 0), 1600u);
        EXPECT_EQ(bmmo::session::late_join_base(1600, 1590), 1600u);
        EXPECT_EQ(bmmo::session::late_join_base(1600, 1603), 1603u);
        EXPECT_TRUE(bmmo::session::leaver_stays(true, false)) << "J-e: a ball leave is always queued";
        EXPECT_TRUE(bmmo::session::leaver_stays(false, true));
        EXPECT_FALSE(bmmo::session::leaver_stays(false, false));
        std::vector<bmmo::session::member_life> lives(2);
        lives[0].entry.id = 5; lives[0].entry.life = 1; lives[0].until = 150;
        lives[1].entry.id = 5; lives[1].entry.life = 2; lives[1].from = 150;
        EXPECT_EQ(bmmo::session::member_life_at(lives, 5, 149)->entry.life, 1);
        EXPECT_EQ(bmmo::session::member_life_at(lives, 5, 150)->entry.life, 2);
        EXPECT_EQ(bmmo::session::member_life_at(lives, 6, 150), nullptr);
    }

    // ------------------------------------------- step TRL: sets per life
    // The user, 2026-10-29: 'Key by life, no resume'.  A rejoin on the same
    // id is a new life with its own set; the old life's set fades and
    // retires beside it; nothing of one life serves another.

    // Two lives' sets of one id coexist: the anchor's (4, life 1), then the
    // rejoin's member message (4, life 2) builds 4L2 while 4 is held; the
    // old life's final resets run on its own set, the new life's input on
    // its own.
    TEST(SessionClientCoreTrafoLives, TwoLivesOfOneIdHoldTwoSets) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        auto& s = state_of(core);
        ASSERT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 1}), 1u);
        size_t from = fake.calls.size();
        // The old life's final resets (server-stamped, life 1) and its removal.
        for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
            auto reset = trafo_message(4, type, bmmo::session::TRAFO_RESET | bmmo::session::TRAFO_SERVER
                                                    | bmmo::session::TRAFO_FINAL, 130, 130);
            reset.life = 1;
            core.on_session_event(std::move(reset));
        }
        core.on_session_member(member_message(bmmo::session::MEMBER_REMOVED, 4, 1, 0, 131));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 4, 2, 0, 131));
        EXPECT_EQ(only(since(fake.calls, from), "make_trafo_set"), std::vector<std::string>{"make_trafo_set:4L2"})
            << "the new life's own set; the old one is not reused";
        EXPECT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 1}), 1u) << "the old life's set still fades";
        EXPECT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 2}), 1u);
        EXPECT_EQ(core.trafo_set_list(), "3,4,4L2");
        // The new life's explosion, at the same type and a later tick.
        auto boom = trafo_message(4, 0, bmmo::session::TRAFO_EXPLODE, 140, 140);
        boom.life = 2;
        core.on_session_event(std::move(boom));
        from = fake.calls.size();
        steps{core}.apply_trafo_inputs(130, false);
        steps{core}.apply_trafo_inputs(140, false);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(only(calls, "trafo_final"),
                  (std::vector<std::string>{"trafo_final:4:0:130", "trafo_final:4:1:130", "trafo_final:4:2:130"}))
            << "the finals mark the old life's set only";
        const auto runs = only(calls, "run_trafo");
        ASSERT_EQ(runs.size(), 4u);
        EXPECT_EQ(runs[3].rfind("run_trafo:4L2:0:0:140:", 0), 0u) << runs[3];
        // The log keeps the two lives apart.
        EXPECT_EQ(s.trafo_log_.newest_explosion(4, 1, 0, 200), nullptr);
        ASSERT_NE(s.trafo_log_.newest_explosion(4, 2, 0, 200), nullptr);
        EXPECT_EQ(s.trafo_log_.newest_explosion(4, 2, 0, 200)->applied, 140u);
    }

    // Per-life retirement: the old life's set retires, the new life's stays;
    // no sight makes the retired one again, the live one is kept.
    TEST(SessionClientCoreTrafoLives, RetirementIsPerLifeAndFinal) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        trafo_tick_125(core, fake);
        auto& s = state_of(core);
        core.on_session_member(member_message(bmmo::session::MEMBER_REMOVED, 4, 1, 0, 131));
        core.on_session_member(member_message(bmmo::session::MEMBER_ADDED, 4, 2, 0, 131));
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_4"), 1u);
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_4L2"), 1u);
        t.retire_answer = {bmmo::session::trafo_key{4, 1}};
        steps{core}.retire_trafo(400);
        EXPECT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 1}), 0u);
        EXPECT_EQ(s.trafo_sets_.count(bmmo::session::trafo_key{4, 2}), 1u);
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_4"), 0u);
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_4L2"), 1u) << "the live life's pieces stay";
        const size_t from = fake.calls.size();
        auto old = event_message(4, bmmo::session::event_type::Sector, 401, 401);
        old.life = 1;
        core.on_session_event(std::move(old));
        auto now = event_message(4, bmmo::session::event_type::Sector, 402, 402);
        now.life = 2;
        core.on_session_event(std::move(now));
        auto relay_tick = trafo_message(4, 0, bmmo::session::TRAFO_RESET | bmmo::session::TRAFO_SERVER, 403, 403);
        relay_tick.life = 1;
        core.on_session_event(std::move(relay_tick));
        EXPECT_TRUE(only(since(fake.calls, from), "make_trafo_set").empty())
            << "the retired life is not made again, the live one is held";
        EXPECT_EQ(s.trafo_retired_sights_, 2u);
        EXPECT_EQ(s.trafo_log_.at(403).size(), 0u) << "an input of a retired set is not logged";
        EXPECT_EQ(core.trafo_set_list(), "3,4L2");
    }

    // A late joiner's state while an old life's set still fades (J4: the
    // joiner was never told of that life, its SessionStart lists the new
    // one): the anchor builds the open lives, the ledger names the old
    // life's set too - built from the ledger with its final marks, so the
    // joiner holds exactly the server's set list and the portable record's
    // trafo list (keyed by the per-life names) matches.  A ledger naming a
    // set this world retired brings it back (the server's state at S holds
    // it): the one path that may.
    TEST(SessionClientCoreTrafoLives, ALateJoinLedgerWithAFadingOldLife) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        state_of(core).own_id_ = 3;
        auto start = start_message(9, 3);
        start.world_options = bmmo::session::WORLD_TRAFO_INPUTS;
        start.players[0].life = 2;   // member 4 is in its second life
        ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.build_id = "build-x";
        report.own_name = "Me";
        core.anchor(report);
        EXPECT_EQ(only(fake.calls, "make_trafo_set"), (std::vector<std::string>{"make_trafo_set:3", "make_trafo_set:4L2"}));
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        state_of(core).held_floor_ = 125;
        auto& s = state_of(core);
        bmmo::session_resync_state_msg st;
        for (uint16_t life: {uint16_t{1}, uint16_t{2}})
            for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
                bmmo::session::trafo_ledger_entry e;
                e.player = 4;
                e.life = life;
                e.type = type;
                e.final = life == 1;
                e.final_applied = life == 1 ? 110 : 0;
                st.trafo_ledger.push_back(e);
            }
        for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
            bmmo::session::trafo_ledger_entry e;
            e.player = 3;
            e.type = type;
            st.trafo_ledger.push_back(e);
        }
        std::set<std::string> gone;
        std::vector<bmmo::session::journal_trafo_entry> journal;
        size_t from = fake.calls.size();
        steps{core}.reconcile_trafo(st, 119u, gone, journal);
        auto calls = since(fake.calls, from);
        EXPECT_EQ(only(calls, "make_trafo_set"), std::vector<std::string>{"make_trafo_set:4"}) << "the fading set, from the ledger";
        EXPECT_EQ(only(calls, "trafo_final"),
                  (std::vector<std::string>{"trafo_final:4:0:110", "trafo_final:4:1:110", "trafo_final:4:2:110"}));
        EXPECT_EQ(core.trafo_set_list(), "3,4,4L2") << "the server's list";
        ASSERT_EQ(journal.size(), 9u);
        EXPECT_EQ(journal[3].player, 4u);
        EXPECT_EQ(journal[3].life, 1u);
        EXPECT_EQ(journal[6].life, 2u);
        // Retired here, named again by a later ledger: made again (reselected
        // by the host while not destroyed).
        t.retire_answer = {bmmo::session::trafo_key{4, 1}};
        steps{core}.retire_trafo(300);
        EXPECT_EQ(core.trafo_set_list(), "3,4L2");
        from = fake.calls.size();
        steps{core}.reconcile_trafo(st, 119u, gone, journal);
        calls = since(fake.calls, from);
        EXPECT_EQ(only(calls, "make_trafo_set"), std::vector<std::string>{"make_trafo_set:4"});
        EXPECT_EQ(core.trafo_set_list(), "3,4,4L2");
        EXPECT_EQ(s.trafo_retired_sets_.count(bmmo::session::trafo_key{4, 1}), 0u);
    }

    // The own id's earlier life: a rejoiner's core sees its old life's
    // relayed explosion as another life's input, not as an own echo.
    TEST(SessionClientCoreTrafoLives, AnOwnEarlierLifeIsNoEcho) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        state_of(core).own_id_ = 3;
        auto start = start_message(9, 3);
        start.world_options = bmmo::session::WORLD_TRAFO_INPUTS;
        start.players[1].life = 2;   // our own entry: the second life
        ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.build_id = "build-x";
        report.own_name = "Me";
        core.anchor(report);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        state_of(core).held_floor_ = 125;
        EXPECT_EQ(core.trafo_set_list(), "3L2,4");
        auto old = trafo_message(3, 1, bmmo::session::TRAFO_EXPLODE, 130, 130);
        old.life = 1;
        core.on_session_event(std::move(old));
        auto& s = state_of(core);
        EXPECT_TRUE(s.own_echoes_.empty());
        ASSERT_EQ(s.trafo_log_.at(130).size(), 1u);
        EXPECT_EQ(s.trafo_log_.at(130)[0].life, 1u);
        EXPECT_FALSE(s.trafo_log_.at(130)[0].own);
        EXPECT_EQ(core.trafo_set_list(), "3,3L2,4");
    }

    // Run B of step TRL (a rejoin on the same connection 1 s after the
    // leave): the rejoiner's first state (S 1296) named its old life's
    // fading set, which it built from that ledger; the state was never
    // applied (a wait, then a timeout), and the server retired the set at
    // 1431; the next state (S 1529) does not name it.  Its reconcile retires
    // the set of the ended life at once, so the local list is the ledger's
    // and the portable record is taken; a set of an open life the ledger
    // does not name is left alone.
    TEST(SessionClientCoreTrafoLives, AReconcileRetiresAnEndedLifesSetTheLedgerNoLongerNames) {
        flow_fake fake;
        trafo_fake t;
        session_client_core core(trafo_rewind_platform(fake, t), trafo_caps());
        state_of(core).own_id_ = 3;
        auto start = start_message(9, 3);
        start.world_options = bmmo::session::WORLD_TRAFO_INPUTS;
        start.players[1].life = 2;   // our own entry: the second life
        ASSERT_FALSE(core.begin(start, bmmo::session::core_config{}).refused);
        core.level_requested();
        bmmo::session::anchor_report report;
        report.build_id = "build-x";
        report.own_name = "Me";
        core.anchor(report);
        core.on_session_assign(assign_message(9, 120));
        state_of(core).frames_since_anchor_ = 6;
        state_of(core).held_floor_ = 125;
        auto& s = state_of(core);
        // The first state's ledger: our old life's set, final.
        bmmo::session_resync_state_msg first;
        for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
            bmmo::session::trafo_ledger_entry e;
            e.player = 3;
            e.life = 1;
            e.type = type;
            e.final = true;
            e.final_applied = 101;
            first.trafo_ledger.push_back(e);
        }
        std::set<std::string> gone;
        std::vector<bmmo::session::journal_trafo_entry> journal;
        steps{core}.reconcile_trafo(first, 119u, gone, journal);
        ASSERT_EQ(core.trafo_set_list(), "3,3L2,4") << "made from the ledger";
        // The next state names only the open lives' sets (and not even 4's).
        bmmo::session_resync_state_msg next;
        for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
            bmmo::session::trafo_ledger_entry e;
            e.player = 3;
            e.life = 2;
            e.type = type;
            next.trafo_ledger.push_back(e);
        }
        const size_t from = fake.calls.size();
        gone.clear();
        journal.clear();
        steps{core}.reconcile_trafo(next, 124u, gone, journal);
        const auto calls = since(fake.calls, from);
        EXPECT_EQ(only(calls, "trafo_reconcile:retire"), std::vector<std::string>{"trafo_reconcile:retire:3:125"});
        EXPECT_EQ(core.trafo_set_list(), "3L2,4") << "the ended life's set retired; member 4's open life kept";
        EXPECT_EQ(s.trafo_retired_sets_.count(bmmo::session::trafo_key{3, 1}), 1u);
        EXPECT_EQ(s.trafo_pieces_.count("piece1_0_3"), 0u);
        EXPECT_EQ(gone.count("piece1_0_3"), 1u) << "its pieces are kept out of the restore";
        // Review 6: the retirement is in the journal (each of its type
        // entries, not in the ledger), and nothing else is marked.
        size_t retired_entries = 0;
        for (const auto& j: journal) {
            const bool ended = j.player == 3 && j.life == 1;
            EXPECT_EQ((j.action & bmmo::session::JOURNAL_TRAFO_RETIRED) != 0, ended) << j.player << "L" << j.life;
            if (ended) {
                EXPECT_EQ(j.bits & 16, 16) << "not in the ledger";
                ++retired_entries;
            }
        }
        EXPECT_EQ(retired_entries, static_cast<size_t>(bmmo::session::kTrafoTypes));
    }
}
