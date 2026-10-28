// Unit tests for session/rollback_binding.hpp over a fake backend: the one
// place the engine's hooks meet a physics world, bound the same way for the
// headless client, the SimTool and the retail mod.  The fake has the methods
// of physics_view / physics_direct under their names and records every call,
// so the tests can check what the binding asked of the world and what it
// reported back.  Nothing here touches a CK type: the core binding compiles
// without the engine, which is what lets the mod and the tools share it.
// Step E9 adds the shared bind_session_world (session/session_host_world.hpp,
// design 2.4 and 5.1): the core's hooks and the host's options in one order.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <session/rollback_binding.hpp>
#include <session/session_host_world.hpp>
#include <session/script_order.hpp>

namespace {
    using bmmo::session::bind_rollback_world;
    using bmmo::session::input_frame;
    using bmmo::session::rollback_binding_options;
    using bmmo::session::rollback_world;
    using bmmo::session::savestate_outcome;
    using bmmo::session::wake_mode;

    struct fake_backend {
        std::vector<std::string> calls;
        bool refuse_writes = false;
        bool refuse_savestate = false;
        bool clock_readable = true;
        float time_factor = 1.0f, physics_delta = 1000.0f / 66.0f;
        int contact_count = 0;
        std::vector<std::string> dropped, unknown;
        std::vector<uint8_t> blob = {1, 2, 3};

        bmmo::physics::wake_mode last_mode = bmmo::physics::wake_mode::keep;
        bool last_recheck = false;
        uint32_t last_flags = 0;
        std::vector<uint8_t> last_set;
        bool last_poll_on = false;
        int last_poll_count = -1;
        const int* last_poll_codes = nullptr;
        bool last_nav_active = false;
        float last_delta = 0.0f;

        bool get_body_state(const char* entity, bmmo_physics_body_state& out, std::string&) {
            calls.push_back(std::string("get_body ") + entity);
            std::memset(&out, 0, sizeof(out));
            std::strncpy(out.name, entity, sizeof(out.name) - 1);
            return true;
        }
        bool set_body_state(const char* entity, const double[3], const double[4], const float[3], const float[3],
                            bmmo::physics::wake_mode mode, bool recheck, std::string& error) {
            calls.push_back(std::string("set_body ") + entity);
            last_mode = mode;
            last_recheck = recheck;
            if (refuse_writes) {
                error = "no such body";
                return false;
            }
            return true;
        }
        bool body_recheck(const char* entity, std::string& error) {
            calls.push_back(std::string("recheck ") + entity);
            if (refuse_writes) {
                error = "recheck refused";
                return false;
            }
            return true;
        }
        bool get_core_extra(const char* entity, bmmo_physics_core_extra& out, std::string&) {
            calls.push_back(std::string("get_extra ") + entity);
            std::memset(&out, 0, sizeof(out));
            return true;
        }
        bool set_core_extra(const char* entity, const bmmo_physics_core_extra&, std::string&) {
            calls.push_back(std::string("set_extra ") + entity);
            return true;
        }
        bool get_clock_state(bmmo_physics_clock_state& out, std::string&) {
            calls.push_back("get_clock_state");
            std::memset(&out, 0, sizeof(out));
            out.current_time = 12.5;
            return true;
        }
        bool set_clock_state(const bmmo_physics_clock_state&, std::string& error) {
            calls.push_back("set_clock_state");
            if (refuse_writes) {
                error = "clock refused";
                return false;
            }
            return true;
        }
        bool get_clock(float& factor, float& delta, std::string&) {
            calls.push_back("get_clock");
            if (!clock_readable) return false;
            factor = time_factor;
            delta = physics_delta;
            return true;
        }
        int contacts_get(const char* entity, bmmo_physics_contact* out, int max, std::string&) {
            calls.push_back(std::string("contacts_get ") + entity);
            for (int i = 0; i < contact_count && i < max; ++i) {
                std::memset(&out[i], 0, sizeof(out[i]));
                std::snprintf(out[i].partner, sizeof(out[i].partner), "partner%d", i);
            }
            return contact_count;
        }
        int contacts_set(const char* entity, const bmmo_physics_contact*, int count, uint32_t flags, std::string& error) {
            calls.push_back(std::string("contacts_set ") + entity);
            last_flags = flags;
            if (refuse_writes) {
                error = "no contact made";
                return 0;
            }
            return count;
        }
        bool navigation_get_state(const char* entity, bmmo_physics_nav_state& out, std::string&) {
            calls.push_back(std::string("nav_get ") + entity);
            std::memset(&out, 0, sizeof(out));
            return true;
        }
        bool navigation_set_state(const char* entity, const bmmo_physics_nav_state&, std::string&) {
            calls.push_back(std::string("nav_set ") + entity);
            return true;
        }
        bool navigation_input(const char* entity, uint8_t, const float[3], const float[3], const float[3], bool active,
                              std::string&) {
            calls.push_back(std::string("nav_input ") + entity);
            last_nav_active = active;
            return true;
        }
        bool navigation_poll(const char* entity, bool enable, const int* codes, const uint32_t*, int count, std::string&) {
            calls.push_back(std::string(enable ? "poll_on " : "poll_off ") + entity);
            last_poll_on = enable;
            last_poll_codes = codes;
            last_poll_count = count;
            return true;
        }
        bool savestate_get(std::vector<uint8_t>& out, std::string& error) {
            calls.push_back("savestate_get");
            if (refuse_savestate) {
                error = "capture refused";
                return false;
            }
            out = blob;
            return true;
        }
        bool savestate_set(const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                           std::vector<std::string>* contacts_dropped, std::vector<std::string>* unknown_bodies) {
            calls.push_back("savestate_set");
            last_set.assign(data, data + size);
            last_flags = flags;
            if (contacts_dropped) *contacts_dropped = dropped;
            if (unknown_bodies) *unknown_bodies = unknown;
            if (refuse_savestate) {
                error = "restore refused";
                return false;
            }
            return true;
        }
        bool step_physics(float delta_ms, std::string& error) {
            calls.push_back("step");
            last_delta = delta_ms;
            if (refuse_writes) {
                error = "step refused";
                return false;
            }
            return true;
        }
    };

    int count_calls(const fake_backend& b, const char* prefix) {
        int n = 0;
        for (const auto& c: b.calls)
            if (c.rfind(prefix, 0) == 0) ++n;
        return n;
    }
}

TEST(RollbackBinding, EveryWorldHookIsBoundAndTheScriptHooksAreNot) {
    fake_backend backend;
    rollback_world world = bind_rollback_world(backend, rollback_binding_options{});
    EXPECT_TRUE(world.get_body && world.set_body && world.recheck_body);
    EXPECT_TRUE(world.get_body_extra && world.set_body_extra);
    EXPECT_TRUE(world.get_clock);
    EXPECT_FALSE(world.set_clock) << "the clock rewind is opt-in";
    EXPECT_TRUE(world.get_nav && world.set_nav && world.nav_input && world.nav_poll);
    EXPECT_TRUE(world.get_contacts && world.set_contacts);
    EXPECT_TRUE(world.get_savestate && world.set_savestate);
    EXPECT_TRUE(world.step && world.simulating);
    EXPECT_FALSE(world.pre_step) << "no caller work, no hook";
    EXPECT_FALSE(world.capture_scripts || world.restore_scripts || world.diff_scripts)
        << "the scripts are bound by rollback_binding_scripts.hpp, on top";

    bmmo_physics_body_state state{};
    EXPECT_TRUE(world.get_body("ball", state));
    EXPECT_STREQ(state.name, "ball");
    bmmo_physics_core_extra extra{};
    EXPECT_TRUE(world.get_body_extra("ball", extra));
    EXPECT_TRUE(world.set_body_extra("ball", extra));
    bmmo_physics_clock_state clock{};
    EXPECT_TRUE(world.get_clock(clock));
    EXPECT_DOUBLE_EQ(clock.current_time, 12.5);
    bmmo_physics_nav_state nav{};
    EXPECT_TRUE(world.get_nav("ball", nav));
    EXPECT_TRUE(world.set_nav("ball", nav));
    EXPECT_EQ(count_calls(backend, "get_body ball"), 1);
    EXPECT_EQ(count_calls(backend, "nav_set ball"), 1);
}

TEST(RollbackBinding, SetBodyCarriesTheWakeModeAndTheRecheck) {
    fake_backend backend;
    rollback_world world = bind_rollback_world(backend, rollback_binding_options{});
    bmmo_physics_body_state state{};
    EXPECT_TRUE(world.set_body("ball", state, wake_mode::keep, false));
    EXPECT_EQ(backend.last_mode, bmmo::physics::wake_mode::keep);
    EXPECT_FALSE(backend.last_recheck);
    EXPECT_TRUE(world.set_body("ball", state, wake_mode::wake, true));
    EXPECT_EQ(backend.last_mode, bmmo::physics::wake_mode::wake);
    EXPECT_TRUE(backend.last_recheck);
    EXPECT_TRUE(world.set_body("ball", state, wake_mode::freeze, false));
    EXPECT_EQ(backend.last_mode, bmmo::physics::wake_mode::freeze);
    EXPECT_EQ(count_calls(backend, "set_body ball"), 3);
}

TEST(RollbackBinding, ARefusedWriteIsReportedWithTheBackendsError) {
    fake_backend backend;
    backend.refuse_writes = true;
    std::vector<std::string> failures;
    rollback_binding_options o;
    o.restore_clock = true;
    o.on_write_failed = [&](const std::string& error) { failures.push_back(error); };
    rollback_world world = bind_rollback_world(backend, o);

    bmmo_physics_body_state state{};
    EXPECT_FALSE(world.set_body("ball", state, wake_mode::keep, false));
    EXPECT_FALSE(world.recheck_body("ball"));
    bmmo_physics_clock_state clock{};
    EXPECT_FALSE(world.set_clock(clock));
    ASSERT_EQ(failures.size(), 3u);
    EXPECT_EQ(failures[0], "no such body");
    EXPECT_EQ(failures[1], "recheck refused");
    EXPECT_EQ(failures[2], "clock refused");
}

TEST(RollbackBinding, TheClockIsWrittenOnlyWhenTheCallerAsked) {
    fake_backend backend;
    rollback_binding_options o;
    o.restore_clock = true;
    rollback_world world = bind_rollback_world(backend, o);
    ASSERT_TRUE(world.set_clock);
    bmmo_physics_clock_state clock{};
    EXPECT_TRUE(world.set_clock(clock));
    EXPECT_EQ(count_calls(backend, "set_clock_state"), 1);
}

TEST(RollbackBinding, SavestateHooksFollowTheSwitchAndReportTheOutcome) {
    {
        fake_backend backend;
        rollback_binding_options o;
        o.savestate = false;
        rollback_world world = bind_rollback_world(backend, o);
        EXPECT_FALSE(world.get_savestate);
        EXPECT_FALSE(world.set_savestate);
    }
    fake_backend backend;
    backend.dropped = {"paper", "box"};
    backend.unknown = {"late_ball"};
    std::vector<std::string> capture_failures;
    struct restored_t {
        bool ok;
        std::string error;
        savestate_outcome outcome;
    };
    std::vector<restored_t> restored;
    rollback_binding_options o;
    o.savestate_flags = 5;
    o.on_savestate_capture_failed = [&](const std::string& error) { capture_failures.push_back(error); };
    o.on_savestate_restored = [&](bool ok, const std::string& error, const savestate_outcome& outcome) {
        restored.push_back({ok, error, outcome});
    };
    rollback_world world = bind_rollback_world(backend, o);

    std::vector<uint8_t> out;
    EXPECT_TRUE(world.get_savestate(out));
    EXPECT_EQ(out, backend.blob);
    EXPECT_TRUE(capture_failures.empty());

    savestate_outcome outcome;
    EXPECT_TRUE(world.set_savestate({9, 8, 7}, outcome));
    EXPECT_EQ(backend.last_set, (std::vector<uint8_t>{9, 8, 7}));
    EXPECT_EQ(backend.last_flags, 5u);
    EXPECT_EQ(outcome.contacts_dropped, (std::vector<std::string>{"paper", "box"}));
    EXPECT_EQ(outcome.unknown_bodies, (std::vector<std::string>{"late_ball"}));
    ASSERT_EQ(restored.size(), 1u);
    EXPECT_TRUE(restored[0].ok);
    EXPECT_TRUE(restored[0].error.empty());
    EXPECT_EQ(restored[0].outcome.unknown_bodies, outcome.unknown_bodies);

    backend.refuse_savestate = true;
    EXPECT_FALSE(world.get_savestate(out));
    ASSERT_EQ(capture_failures.size(), 1u);
    EXPECT_EQ(capture_failures[0], "capture refused");
    EXPECT_FALSE(world.set_savestate({1}, outcome));
    ASSERT_EQ(restored.size(), 2u);
    EXPECT_FALSE(restored[1].ok);
    EXPECT_EQ(restored[1].error, "restore refused");
}

TEST(RollbackBinding, NavPollAlwaysReachesTheBackendAndRearmsWhenHandedBack) {
    fake_backend backend;
    const int codes[2] = {38, 40};
    const uint32_t blocks[2] = {1, 2};
    bool muted = false;
    std::vector<std::string> resumed;
    rollback_binding_options o;
    o.own_key_codes = codes;
    o.own_key_blocks = blocks;
    o.own_key_count = 2;
    o.input_muted = [&]() { return muted; };
    o.on_poll_resumed = [&](const std::string& entity) { resumed.push_back(entity); };
    rollback_world world = bind_rollback_world(backend, o);

    // Off: the backend is told (the replay drives the replica), nothing resumes.
    EXPECT_TRUE(world.nav_poll("ball", false));
    EXPECT_FALSE(backend.last_poll_on);
    EXPECT_EQ(backend.last_poll_codes, codes) << "the codes go along as the clients always sent them";
    EXPECT_EQ(backend.last_poll_count, 2);
    EXPECT_TRUE(resumed.empty());

    // On: the backend is told, and the caller re-arms its live input.
    EXPECT_TRUE(world.nav_poll("ball", true));
    EXPECT_TRUE(backend.last_poll_on);
    ASSERT_EQ(resumed.size(), 1u);
    EXPECT_EQ(resumed[0], "ball");

    // On while the pause menu mutes the input: the polling stays off, but
    // the live input is re-armed all the same (R2: nothing polls, so the
    // last replayed tick's frame would drive the live step).
    muted = true;
    EXPECT_TRUE(world.nav_poll("ball", true));
    EXPECT_FALSE(backend.last_poll_on);
    ASSERT_EQ(resumed.size(), 2u);
    EXPECT_EQ(resumed[1], "ball");
    // Off while muted: nothing re-armed (the replay is about to drive it).
    EXPECT_TRUE(world.nav_poll("ball", false));
    EXPECT_EQ(resumed.size(), 2u);
    EXPECT_EQ(count_calls(backend, "poll_"), 4) << "every call reaches the backend";
}

TEST(RollbackBinding, NavInputPassesTheActiveFlag) {
    fake_backend backend;
    rollback_world world = bind_rollback_world(backend, rollback_binding_options{});
    input_frame frame{};
    frame.flags = 0;
    EXPECT_TRUE(world.nav_input("ball", frame));
    EXPECT_FALSE(backend.last_nav_active);
    frame.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
    EXPECT_TRUE(world.nav_input("ball", frame));
    EXPECT_TRUE(backend.last_nav_active);
    EXPECT_EQ(count_calls(backend, "nav_input ball"), 2);
}

TEST(RollbackBinding, TheStepIsOnePhysicsTickUnlessTheCallerOwnsTheFrame) {
    fake_backend backend;
    std::vector<std::string> logs;
    rollback_binding_options o;
    o.delta_ms = 7.5f;
    o.log = [&](const std::string& line) { logs.push_back(line); };
    rollback_world world = bind_rollback_world(backend, o);
    EXPECT_TRUE(world.step());
    EXPECT_FLOAT_EQ(backend.last_delta, 7.5f);
    EXPECT_EQ(count_calls(backend, "step"), 1);

    backend.refuse_writes = true;
    EXPECT_FALSE(world.step());
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_EQ(logs[0], "replay step: step refused");

    int frames = 0;
    fake_backend untouched;
    o.whole_frame = [&]() {
        ++frames;
        return true;
    };
    rollback_world framed = bind_rollback_world(untouched, o);
    EXPECT_TRUE(framed.step());
    EXPECT_EQ(frames, 1);
    EXPECT_EQ(count_calls(untouched, "step"), 0) << "the caller's frame replaces the physics step";
}

TEST(RollbackBinding, SimulatingFollowsTheLocalClock) {
    fake_backend backend;
    rollback_world world = bind_rollback_world(backend, rollback_binding_options{});
    EXPECT_TRUE(world.simulating());
    backend.time_factor = 0.0f;
    EXPECT_FALSE(world.simulating()) << "the retail scripts stop the clock with a zero factor";
    backend.time_factor = 1.0f;
    backend.physics_delta = 0.0f;
    EXPECT_FALSE(world.simulating());
    backend.clock_readable = false;
    EXPECT_TRUE(world.simulating()) << "a world without a readable clock is taken as running";
}

TEST(RollbackBinding, ContactsAreCappedAtTheBufferAndTheirRestoreIsReported) {
    fake_backend backend;
    backend.contact_count = 20;
    struct report_t {
        std::string entity;
        int made;
        std::string error;
        size_t recorded;
    };
    std::vector<report_t> reports;
    rollback_binding_options o;
    o.on_contacts_restored = [&](const std::string& entity, int made, const std::string& error,
                                 const std::vector<bmmo_physics_contact>& recorded) {
        reports.push_back({entity, made, error, recorded.size()});
    };
    rollback_world world = bind_rollback_world(backend, o);

    std::vector<bmmo_physics_contact> contacts;
    EXPECT_EQ(world.get_contacts("ball", contacts), 20) << "the backend's count is passed through";
    EXPECT_EQ(contacts.size(), 16u) << "the binding's buffer holds 16";
    EXPECT_STREQ(contacts[15].partner, "partner15");

    std::vector<bmmo_physics_contact> two(2);
    EXPECT_EQ(world.set_contacts("ball", two, 3u), 2);
    EXPECT_EQ(backend.last_flags, 3u);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].entity, "ball");
    EXPECT_EQ(reports[0].made, 2);
    EXPECT_TRUE(reports[0].error.empty());
    EXPECT_EQ(reports[0].recorded, 2u);

    backend.refuse_writes = true;
    EXPECT_EQ(world.set_contacts("ball", two, 3u), 0);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[1].error, "no contact made");
}

TEST(RollbackBinding, PreStepIsTheCallersWorkBeforeAReplayedTick) {
    fake_backend backend;
    std::vector<uint32_t> ticks;
    rollback_binding_options o;
    o.before_replayed_tick = [&](uint32_t tick) { ticks.push_back(tick); };
    rollback_world world = bind_rollback_world(backend, o);
    ASSERT_TRUE(world.pre_step);
    world.pre_step(42);
    ASSERT_EQ(ticks.size(), 1u);
    EXPECT_EQ(ticks[0], 42u);
}

TEST(RollbackBinding, TheOptionsAreCopiedSoTheWorldOutlivesThem) {
    fake_backend backend;
    std::vector<std::string> resumed;
    rollback_world world;
    {
        const int codes[1] = {38};
        rollback_binding_options o;
        o.own_key_codes = codes;   // deliberately dangling after the scope: the binding must not read it here
        o.own_key_count = 0;
        o.on_poll_resumed = [&](const std::string& entity) { resumed.push_back(entity); };
        world = bind_rollback_world(backend, o);
    }
    EXPECT_TRUE(world.nav_poll("ball", true));
    ASSERT_EQ(resumed.size(), 1u);
    EXPECT_EQ(resumed[0], "ball");
}

namespace {
    // The binding's backend plus the methods bind_session_world calls (the
    // mechanism destroy and the birth insert).
    struct session_backend : fake_backend {
        // Gap L (step EL6): bodies the world has none of (get_body fails).
        std::set<std::string> bodiless;
        bool get_body_state(const char* entity, bmmo_physics_body_state& out, std::string& error) {
            if (bodiless.count(entity)) {
                calls.push_back(std::string("get_body ") + entity);
                return false;
            }
            return fake_backend::get_body_state(entity, out, error);
        }
        bool set_event_log_muted(bool muted, std::string&) {
            calls.push_back(muted ? "mute" : "unmute");
            return true;
        }
        bool unphysicalize(const char* entity, std::string& error) {
            calls.push_back(std::string("unphysicalize ") + entity);
            if (refuse_writes) {
                error = "no body";
                return false;
            }
            return true;
        }
        bool set_body_matrix(const char* entity, const float[16], bool simulated, std::string&) {
            calls.push_back(std::string("set_body_matrix ") + entity + (simulated ? " simulated" : ""));
            return true;
        }
        bool physicalize(const char* entity, const bmmo_physics_ball_recipe&, const char*, std::string& error) {
            calls.push_back(std::string("physicalize ") + entity);
            if (refuse_writes) {
                error = "refused";
                return false;
            }
            return true;
        }
    };

    bmmo::session::local_birth birth_of(const char* name, float x) {
        bmmo::session::local_birth b;
        b.name = name;
        b.matrix[12] = x;
        return b;
    }
}

// Step LB: destroy_body hands a mirror's birth to the core
// (remake_mirror_birth) before any ball rule, and takes nothing out itself;
// birth_from_inputs is the core's mirror test.  Without the core's hook a
// mirror keeps the parking path; --park-births installs neither.
TEST(RollbackBinding, SessionWorldHandsAMirrorsBirthToTheCore) {
    session_backend backend;
    bmmo::session::core_world_hooks hooks;
    std::vector<std::string> asked;
    hooks.mirror = [](const std::string& entity) { return entity.rfind("Ball_Wood_BMMO_", 0) == 0; };
    hooks.ball = hooks.mirror;
    bmmo::session::local_birth_log births;
    bmmo::session::session_world_options w;
    w.births = &births;
    {
        rollback_world world = bmmo::session::bind_session_world(backend, hooks, w);
        EXPECT_FALSE(world.destroy_body("Ball_Wood_BMMO_5", 120)) << "no core hook: parked";
    }
    hooks.remake_mirror_birth = [&](const std::string& entity, uint32_t tick) {
        asked.push_back(entity + "@" + std::to_string(tick));
        return entity == "Ball_Wood_BMMO_5";
    };
    rollback_world world = bmmo::session::bind_session_world(backend, hooks, w);
    backend.calls.clear();
    EXPECT_TRUE(world.destroy_body("Ball_Wood_BMMO_5", 120));
    EXPECT_FALSE(world.destroy_body("Ball_Wood_BMMO_6", 121));
    EXPECT_EQ(asked, std::vector<std::string>({"Ball_Wood_BMMO_5@120", "Ball_Wood_BMMO_6@121"}));
    EXPECT_TRUE(backend.calls.empty()) << "the core takes it out, not the binding";
    ASSERT_TRUE(world.birth_from_inputs);
    EXPECT_TRUE(world.birth_from_inputs("Ball_Wood_BMMO_5"));
    EXPECT_FALSE(world.birth_from_inputs("P_Box"));
    w.park_births = true;
    rollback_world parked = bmmo::session::bind_session_world(backend, hooks, w);
    EXPECT_FALSE(parked.destroy_body);
    EXPECT_FALSE(parked.birth_from_inputs);
}

// Design 2.4 / 5.1 (E9): bind_session_world composes the core's hooks with
// the host's options in one order.  Before a replayed tick: the core's
// births, then the manager activation, then the host's traces (and a
// script binding's frame after them).  The replayed mirrors and the
// corrections are the core's.  destroy_body: the core's moved own birth
// first; the balls (the core's is_ball, not a "Ball_" name: step P), the own
// body and the mirrors kept; a mechanism - a trafo explosion piece too (step
// P) - destroyed muted, put back from its birth, unmuted, a failure reported.
// revive_body (step P): a mechanism the window destroyed made again, muted,
// from the birth of the lifetime it had at the anchor, never a ball, only
// one the host calls revivable (none without the predicate), read from the
// finished world (gap L, L4: bind_script_replay widens it).
// insert_birth only when asked, from the tick's own birth; the birth log
// behind birth_tick_of and birth_order; --park-births installs no destroy
// and no revive.  The host's savestate flags pass unchanged (step P: the
// Random cursor is restored with the blob).
TEST(RollbackBinding, SessionWorldComposesTheCoresHooksInTheDesignsOrder) {
    session_backend backend;
    std::vector<std::string> order;
    bmmo::session::core_world_hooks hooks;
    hooks.before_replayed_tick = [&](uint32_t tick) { order.push_back("core births " + std::to_string(tick)); };
    hooks.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) { out["Ball_Wood_BMMO_4"] = tick; };
    hooks.replay_mechanisms = []() { return std::vector<std::string>{"P_Box", "P_Plank"}; };
    hooks.on_correction = [&](const bmmo::session::rollback_correction& c) { order.push_back("correction " + c.entity); };
    hooks.destroy_own_birth = [&](const std::string& entity, uint32_t, bool& destroyed) {
        if (entity != "Own_Moved") return false;
        destroyed = false;
        return true;
    };
    hooks.own_body = [](const std::string& entity) { return entity == "Own_Body"; };
    hooks.mirror = [](const std::string& entity) { return entity == "Mirror_Box"; };
    hooks.ball = [](const std::string& entity) { return entity == "Ball_Wood"; };
    bmmo::session::local_birth_log births;
    births.add(119, birth_of("P_Gate", 6.0f));
    births.add(119, birth_of("P_Selected", 7.0f));
    births.add(120, birth_of("P_Box", 1.0f));
    births.add(120, birth_of("P_Plank", 2.0f));
    births.add(122, birth_of("P_Box", 3.0f));
    births.add(122, birth_of("Ball_Stone_piece01", 4.0f));
    births.add(122, birth_of("Ball_Stray", 5.0f));
    bmmo::session::session_world_options w;
    w.replay_activation = [&](uint32_t tick) { order.push_back("activation " + std::to_string(tick)); };
    w.before_replayed_tick = [&](uint32_t tick) { order.push_back("host traces " + std::to_string(tick)); };
    w.births = &births;
    w.revivable = [](const std::string& entity) { return entity != "P_Gate"; };
    w.insert_births = true;
    w.binding.savestate = true;
    w.binding.savestate_flags = BMMO_PHYSICS_SAVESTATE_STRICT;
    w.put_back = [&](const std::string& entity, const bmmo::session::local_birth& birth) {
        order.push_back("put back " + entity + " " + std::to_string(static_cast<int>(birth.matrix[12])));
    };
    w.on_own_destroy_failed = [&](const std::string& entity) { order.push_back("own destroy failed " + entity); };
    w.on_destroy_failed = [&](const std::string& entity, const std::string& error) {
        order.push_back("destroy failed " + entity + ": " + error);
    };
    std::vector<std::string> wrapped_destroys;
    w.bind_scripts = [&](rollback_world& world) {
        order.push_back("scripts bound");
        // Gap L, L4 (step EL9): what bind_script_replay does - the revivable
        // set widened by the selection's bodies, destroy_body wrapped.
        const auto base = world.revivable;
        world.revivable = [base](const std::string& entity) { return base(entity) || entity == "P_Selected"; };
        const auto destroy = world.destroy_body;
        world.destroy_body = [&wrapped_destroys, destroy](const std::string& entity, uint32_t birth_tick) {
            wrapped_destroys.push_back(entity + " " + std::to_string(birth_tick));
            return destroy(entity, birth_tick);
        };
        const auto before = world.pre_step;
        world.pre_step = [&order, before](uint32_t tick) {
            before(tick);
            order.push_back("scripts frame " + std::to_string(tick));
        };
    };
    rollback_world world = bmmo::session::bind_session_world(backend, hooks, w);
    ASSERT_EQ(order, std::vector<std::string>({"scripts bound"})) << "the scripts' part last, at the bind";
    order.clear();
    world.pre_step(121);
    EXPECT_EQ(order, std::vector<std::string>({"core births 121", "activation 121", "host traces 121", "scripts frame 121"}));
    std::map<std::string, uint32_t> remotes;
    world.replay_remotes(121, remotes);
    EXPECT_EQ(remotes.at("Ball_Wood_BMMO_4"), 121u);
    ASSERT_TRUE(world.replay_mechanisms) << "the core's list of the replayed world's mechanisms";
    EXPECT_EQ(world.replay_mechanisms(), std::vector<std::string>({"P_Box", "P_Plank"}));
    order.clear();
    bmmo::session::rollback_correction c;
    c.entity = "Ball_Wood";
    world.on_correction(c);
    EXPECT_EQ(order, std::vector<std::string>({"correction Ball_Wood"}));

    ASSERT_TRUE(world.set_savestate);
    savestate_outcome restored;
    EXPECT_TRUE(world.set_savestate({1, 2, 3}, restored));
    EXPECT_EQ(backend.last_flags, BMMO_PHYSICS_SAVESTATE_STRICT) << "the host's flags, nothing added";

    EXPECT_EQ(world.birth_tick_of("P_Box"), 122u);
    EXPECT_EQ(world.birth_tick_of("P_None"), 0u);
    EXPECT_EQ(world.birth_order(120), std::vector<std::string>({"P_Box", "P_Plank"}));

    // destroy_body.
    order.clear();
    backend.calls.clear();
    EXPECT_FALSE(world.destroy_body("Own_Moved", 120)) << "the core's branch: not destroyed";
    EXPECT_FALSE(world.destroy_body("Ball_Wood", 120));
    EXPECT_FALSE(world.destroy_body("Own_Body", 120));
    EXPECT_FALSE(world.destroy_body("Mirror_Box", 120));
    EXPECT_TRUE(backend.calls.empty()) << "nothing taken out";
    EXPECT_EQ(order, std::vector<std::string>({"own destroy failed Own_Moved"}));
    order.clear();
    EXPECT_TRUE(world.destroy_body("P_Box", 120));
    EXPECT_TRUE(world.destroy_body("P_Plank", 0)) << "tick 0: the latest birth";
    EXPECT_TRUE(world.destroy_body("P_Box", 121)) << "no birth at 121: the latest";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "unphysicalize P_Box", "unmute", "mute",
                                                       "unphysicalize P_Plank", "unmute", "mute", "unphysicalize P_Box",
                                                       "unmute"}));
    EXPECT_EQ(order, std::vector<std::string>({"put back P_Box 1", "put back P_Plank 2", "put back P_Box 3"}));
    order.clear();
    backend.calls.clear();
    EXPECT_TRUE(world.destroy_body("Ball_Stray", 122)) << "a \"Ball_\" name that is no ball is a mechanism (step P)";
    EXPECT_TRUE(world.destroy_body("Ball_Stone_piece01", 122)) << "a piece: the replayed explosion makes it again";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "unphysicalize Ball_Stray", "unmute", "mute",
                                                       "unphysicalize Ball_Stone_piece01", "unmute"}));
    order.clear();

    // revive_body (step P): the lifetime alive at the anchor, made again.
    backend.calls.clear();
    ASSERT_TRUE(world.revive_body);
    EXPECT_TRUE(world.revive_body("P_Box", 121)) << "alive at 121: the birth of 120";
    EXPECT_TRUE(world.revive_body("Ball_Stone_piece01", 130));
    EXPECT_FALSE(world.revive_body("Ball_Stone_piece01", 121)) << "no lifetime at 121";
    EXPECT_FALSE(world.revive_body("Ball_Wood", 130)) << "a ball's lifetime is the core's";
    EXPECT_FALSE(world.revive_body("Own_Body", 130));
    EXPECT_FALSE(world.revive_body("Mirror_Box", 130));
    EXPECT_FALSE(world.revive_body("P_Gate", 130)) << "alive at 130, but not revivable";
    EXPECT_TRUE(world.revive_body("P_Selected", 130)) << "revivable by the selection (bind_scripts widened it)";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "physicalize P_Box", "unmute", "mute",
                                                       "physicalize Ball_Stone_piece01", "unmute", "mute",
                                                       "physicalize P_Selected", "unmute"}));
    EXPECT_EQ(order, std::vector<std::string>({"put back P_Box 1", "put back Ball_Stone_piece01 4", "put back P_Selected 7"}));
    order.clear();
    backend.calls.clear();
    backend.refuse_writes = true;
    EXPECT_FALSE(world.revive_body("P_Plank", 130)) << "the make refused";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "physicalize P_Plank", "unmute"}));
    backend.refuse_writes = false;

    // restore_lives (gap L, L1; rollback-engine gaps design 2.3.1, step
    // EL6; L4, 2.3.5, step EL9): no core half here; the host's is step P's
    // revive generalized - on the savestate and the per-body path alike, a
    // mechanism with no body now through revive_body (the revivable set as
    // bind_scripts widened it, the A-life's birth), in the order given,
    // revived until the life's end; a name born again inside the window has
    // its later life (a body now) destroyed first through the wrapped
    // destroy_body, before every revive, and goes out revived until its end
    // and remade at its rebirth; a later life is never taken out for a name
    // that cannot be revived (not revivable, no A-life known).
    {
        using bmmo::session::ended_life;
        using bmmo::session::life_kind;
        ASSERT_TRUE(world.restore_lives);
        backend.bodiless = {"Ball_Wood", "Ball_Stone_piece01", "P_Box", "P_Selected"};
        const std::vector<ended_life> ended = {
            {"Ball_Wood", 1, 140, 0, life_kind::own},             // a ball: the core's half
            {"Ball_Stone_piece01", 1, 135, 0, life_kind::mechanism},
            {"P_Plank", 1, 133, 138, life_kind::mechanism},       // born again at 138, that life has a body now
            {"P_Box", 2, 131, 0, life_kind::mechanism},
            {"P_Gate", 1, 132, 137, life_kind::mechanism},        // not revivable: its later life stays
            {"P_Unborn", 1, 132, 136, life_kind::mechanism},      // no A-life known: its later life stays
            {"P_Selected", 1, 134, 0, life_kind::mechanism},      // revivable by the selection
            {"P_Alive", 1, 134, 0, life_kind::mechanism}};        // a body now: nothing to make
        for (const bool savestate: {false, true}) {
            SCOPED_TRACE(savestate ? "savestate" : "per-body");
            std::map<std::string, bmmo::session::life_outcome> out;
            backend.calls.clear();
            order.clear();
            wrapped_destroys.clear();
            world.restore_lives(130, 140, ended, savestate, out);
            ASSERT_EQ(out.size(), 4u);
            EXPECT_EQ(out.at("Ball_Stone_piece01").revived_until, 135u);
            EXPECT_EQ(out.at("Ball_Stone_piece01").remade_at, 0u);
            EXPECT_EQ(out.at("P_Plank").revived_until, 133u);
            EXPECT_EQ(out.at("P_Plank").remade_at, 138u);
            EXPECT_EQ(out.at("P_Box").revived_until, 131u);
            EXPECT_EQ(out.at("P_Box").remade_at, 0u);
            EXPECT_EQ(out.at("P_Selected").revived_until, 134u);
            EXPECT_EQ(wrapped_destroys, std::vector<std::string>({"P_Plank 138"})) << "through the wrapped destroy_body";
            std::vector<std::string> made;
            for (const auto& call: backend.calls)
                if (call.rfind("physicalize ", 0) == 0 || call.rfind("unphysicalize ", 0) == 0) made.push_back(call);
            EXPECT_EQ(made, std::vector<std::string>({"unphysicalize P_Plank", "physicalize Ball_Stone_piece01",
                                                      "physicalize P_Plank", "physicalize P_Box", "physicalize P_Selected"}))
                << "the later life out before every revive";
            EXPECT_EQ(order, std::vector<std::string>({"put back P_Plank 2", "put back Ball_Stone_piece01 4",
                                                       "put back P_Plank 2", "put back P_Box 3", "put back P_Selected 7"}))
                << "P_Box's life at 130 is the birth of 122";
        }
        // A later life that cannot be taken out: the name is not answered
        // (the engine counts it unrevivable) and nothing is made for it.
        {
            std::map<std::string, bmmo::session::life_outcome> out;
            backend.refuse_writes = true;
            backend.calls.clear();
            order.clear();
            world.restore_lives(130, 140, {{"P_Plank", 1, 133, 138, life_kind::mechanism}}, true, out);
            EXPECT_TRUE(out.empty());
            EXPECT_EQ(backend.calls, std::vector<std::string>({"get_body P_Plank", "mute", "unphysicalize P_Plank", "unmute"}));
            backend.refuse_writes = false;
        }
        // A ball or a mirror the revivable set names, with an A-life and a
        // later life with a body now: revive_body would refuse it, so its
        // later life is not taken out (no destroy, wrapped or not) and the
        // name is not answered.
        {
            births.add(119, birth_of("Mirror_Box", 8.0f));
            births.add(119, birth_of("Ball_Wood", 9.0f));
            const auto bodiless = backend.bodiless;
            backend.bodiless.clear();
            std::map<std::string, bmmo::session::life_outcome> out;
            backend.calls.clear();
            wrapped_destroys.clear();
            order.clear();
            world.restore_lives(130, 140,
                                {{"Mirror_Box", 1, 133, 138, life_kind::mechanism},
                                 {"Ball_Wood", 1, 133, 138, life_kind::mechanism}},
                                true, out);
            EXPECT_TRUE(out.empty());
            EXPECT_TRUE(wrapped_destroys.empty()) << "no later life taken out for a name revive_body refuses";
            for (const auto& call: backend.calls) {
                EXPECT_NE(call.rfind("unphysicalize ", 0), 0u) << call;
                EXPECT_NE(call.rfind("physicalize ", 0), 0u) << call;
            }
            EXPECT_TRUE(order.empty());
            backend.bodiless = bodiless;
        }
        backend.bodiless = {"Ball_Wood", "Ball_Stone_piece01", "P_Box", "P_Plank", "P_Gate"};
        order.clear();

        // The core's half first: a name it answered is not asked again.
        bmmo::session::core_world_hooks with_core = hooks;
        std::vector<std::string> asked;
        with_core.restore_lives = [&](uint32_t anchor, uint32_t now, const std::vector<ended_life>& lives, bool savestate,
                                      std::map<std::string, bmmo::session::life_outcome>& answer) {
            asked.push_back(std::to_string(anchor) + ":" + std::to_string(now) + ":" + std::to_string(lives.size())
                            + (savestate ? ":blob" : ":bodies"));
            answer["Ball_Wood"].revived_until = 140;
            answer["P_Box"].remade_at = 139;
        };
        rollback_world cored = bmmo::session::bind_session_world(backend, with_core, w);
        backend.bodiless = {"Ball_Wood", "P_Box"};
        backend.calls.clear();
        std::map<std::string, bmmo::session::life_outcome> answered;
        cored.restore_lives(130, 140, ended, true, answered);
        EXPECT_EQ(asked, std::vector<std::string>({"130:140:8:blob"}));
        EXPECT_EQ(answered.at("Ball_Wood").revived_until, 140u);
        EXPECT_EQ(answered.at("P_Box").revived_until, 0u) << "answered by the core: the host's half leaves it";
        EXPECT_EQ(answered.at("P_Box").remade_at, 139u);
        for (const auto& call: backend.calls) EXPECT_NE(call, "physicalize P_Box");
        backend.bodiless.clear();
        order.clear();
    }

    backend.refuse_writes = true;
    EXPECT_FALSE(world.destroy_body("P_Box", 120));
    EXPECT_EQ(order, std::vector<std::string>({"destroy failed P_Box: no body"}));
    backend.refuse_writes = false;

    // insert_birth: the tick's own birth only; the balls and the own body never.
    backend.calls.clear();
    EXPECT_TRUE(world.insert_birth("P_Box", 122));
    EXPECT_FALSE(world.insert_birth("P_Box", 121));
    EXPECT_FALSE(world.insert_birth("Ball_Wood", 122));
    EXPECT_FALSE(world.insert_birth("Own_Body", 122));
    EXPECT_TRUE(world.insert_birth("Ball_Stone_piece01", 122)) << "a piece is a mechanism birth (step P)";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"set_body_matrix P_Box simulated",
                                                       "set_body_matrix Ball_Stone_piece01 simulated"}));

    // keeps_parked (rollback-engine gaps design 2.4, step B8EG item EGB):
    // none while the core's destroy_own_birth comes without its test.
    EXPECT_FALSE(world.keeps_parked) << "destroy_own_birth without own_birth_destroyable";

    // --park-births: no destroy; without --insert-births: no insert.
    bmmo::session::session_world_options parked;
    parked.park_births = true;
    rollback_world plain = bmmo::session::bind_session_world(backend, hooks, parked);
    EXPECT_FALSE(plain.destroy_body);
    EXPECT_FALSE(plain.revive_body);
    EXPECT_FALSE(plain.restore_lives) << "no half: no restore_lives (the engine skips the detection)";
    EXPECT_FALSE(plain.insert_birth);
    EXPECT_EQ(plain.birth_tick_of("P_Box"), 0u) << "no birth log";
    bmmo::session::session_world_options unrevivable;
    unrevivable.births = &births;
    EXPECT_FALSE(bmmo::session::bind_session_world(backend, hooks, unrevivable).revive_body) << "no revivable: no revive";
    order.clear();
    plain.pre_step(130);
    EXPECT_EQ(order, std::vector<std::string>({"core births 130"}));
}

// The event log's entries in order, the malformed ones skipped, and a
// recipe line parsed into a birth (design 2.4 parse_event_log /
// parse_recipe_event); the birth log keeps 4096 ticks.
TEST(RollbackBinding, EventLogAndRecipeParse) {
    std::vector<std::string> seen;
    bmmo::session::parse_event_log("t=1 created P_Box;bad;t=2 deleted P_Box;t=3 recipe P_Box|m=2;t=4",
                                   [&](const std::string& kind, const std::string& name) { seen.push_back(kind + ":" + name); });
    EXPECT_EQ(seen, std::vector<std::string>({"created:P_Box", "deleted:P_Box", "recipe:P_Box|m=2"}));
    bmmo::session::local_birth birth;
    EXPECT_FALSE(bmmo::session::parse_recipe_event("P_Box|m=2", birth)) << "no matrix";
    EXPECT_TRUE(bmmo::session::parse_recipe_event(
        "P_Box|fixed=0|frozen=1|m=2.5|group=G|parent=1|mtx=1,0,0,0,0,1,0,0,0,0,1,0,4,5,6,1|lmtx=1,0,0,0,0,1,0,0,0,0,1,0,7,8,9,1",
        birth));
    EXPECT_EQ(birth.name, "P_Box");
    EXPECT_TRUE(birth.recipe.start_frozen);
    EXPECT_FLOAT_EQ(birth.recipe.mass, 2.5f);
    EXPECT_EQ(birth.group, "G");
    EXPECT_TRUE(birth.has_parent);
    EXPECT_TRUE(birth.has_local);
    EXPECT_FLOAT_EQ(birth.matrix[13], 5.0f);
    EXPECT_FLOAT_EQ(birth.local_matrix[14], 9.0f);
    bmmo::session::local_birth_log log;
    log.add(10, std::move(birth));
    log.add(5000, birth_of("P_Late", 0.0f));
    EXPECT_EQ(log.find("P_Box", 0), nullptr) << "more than 4096 ticks behind: dropped";
    ASSERT_NE(log.find("P_Late", 5000), nullptr);
}

// Host support's two activation slots per tick (resync design 2.8, step B1,
// session_host_world.hpp activation_slots): a frame's pending activation and
// a union pass at one tick are both kept and re-issued pending first, then
// the pass; forget_pass keeps the pending slot; set_pass overwrites the pass.
// Step A3: a resync's re-assign forgets the old numbering's slots from its
// base on; the restore forgets the births after S of the named bodies.
TEST(ActivationSlots, ForgetFromAndBirthsForgetAfter) {
    bmmo::session::activation_slots slots;
    bmmo::session::manager_activation a;
    a.activate = 2;
    slots.record_pending(100, a);
    slots.set_pass(139, a);
    slots.set_pass(140, a);
    slots.record_pending(150, a);
    EXPECT_EQ(slots.forget_from(140), 2u);
    EXPECT_NE(slots.find(139), nullptr);
    EXPECT_EQ(slots.find(140), nullptr);
    EXPECT_EQ(slots.find(150), nullptr);
    bmmo::session::local_birth_log births;
    bmmo::session::local_birth b;
    b.name = "P_Modul_03_Floor";
    births.add(130, bmmo::session::local_birth(b));
    b.name = "P_Modul_08_Schaukel";
    births.add(145, bmmo::session::local_birth(b));
    b.name = "P_Modul_03_Floor";
    births.add(146, bmmo::session::local_birth(b));
    EXPECT_EQ(births.forget_after(139, {"P_Modul_03_Floor"}), 1u);
    EXPECT_EQ(births.birth_tick_of("P_Modul_03_Floor"), 130u);
    EXPECT_EQ(births.birth_tick_of("P_Modul_08_Schaukel"), 145u) << "not named: kept";
    EXPECT_EQ(births.forget_after(139), 1u);
    EXPECT_EQ(births.birth_tick_of("P_Modul_08_Schaukel"), 0u);
}

// Step A5: forget_pending empties the pending slots of from..to (inclusive),
// keeps their pass slots, drops a tick left with nothing, and leaves the
// ticks outside the range alone.
TEST(ActivationSlots, ForgetPending) {
    bmmo::session::activation_slots slots;
    bmmo::session::manager_activation pending;
    pending.activate = 1;
    bmmo::session::manager_activation pass;
    pass.activate = 2;
    slots.record_pending(139, pending);   // before the range
    slots.record_pending(140, pending);   // pending only
    slots.record_pending(141, pending);   // pending and a pass
    slots.set_pass(141, pass);
    slots.set_pass(142, pass);            // a pass only
    slots.record_pending(143, pending);   // `to` itself
    slots.record_pending(144, pending);   // to + 1
    EXPECT_EQ(slots.forget_pending(140, 143), 3u);
    ASSERT_NE(slots.find(139), nullptr);
    EXPECT_TRUE(slots.find(139)->have_pending);
    EXPECT_EQ(slots.find(140), nullptr) << "nothing left: the tick goes";
    ASSERT_NE(slots.find(141), nullptr);
    EXPECT_FALSE(slots.find(141)->have_pending);
    EXPECT_TRUE(slots.find(141)->have_pass);
    ASSERT_EQ(slots.at(141).size(), 1u);
    EXPECT_EQ(slots.at(141)[0]->activate, 2);
    ASSERT_NE(slots.find(142), nullptr);
    EXPECT_TRUE(slots.find(142)->have_pass);
    EXPECT_EQ(slots.find(143), nullptr) << "the bound is inclusive";
    ASSERT_NE(slots.find(144), nullptr);
    EXPECT_TRUE(slots.find(144)->have_pending);
    EXPECT_EQ(slots.forget_pending(140, 143), 0u);
}

TEST(ActivationSlots, PendingThenPassAndForgetKeepsPending) {
    bmmo::session::activation_slots slots;
    bmmo::session::manager_activation pending;
    pending.reset = false;
    pending.activate = 2;
    pending.deactivate = 1;
    bmmo::session::manager_activation pass;
    pass.reset = true;
    pass.activate = 4;
    slots.record_pending(100, pending);
    slots.set_pass(100, pass);
    auto due = slots.at(100);
    ASSERT_EQ(due.size(), 2u);
    EXPECT_EQ(due[0]->activate, 2);
    EXPECT_FALSE(due[0]->reset);
    EXPECT_EQ(due[1]->activate, 4);
    EXPECT_TRUE(due[1]->reset);
    // A pass alone, re-set: one entry, the newer.
    bmmo::session::manager_activation moved = pass;
    moved.activate = 5;
    slots.set_pass(101, pass);
    slots.set_pass(101, moved);
    due = slots.at(101);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0]->activate, 5);
    // forget_pass keeps the pending slot; a tick with nothing left goes.
    EXPECT_TRUE(slots.forget_pass(100));
    EXPECT_FALSE(slots.forget_pass(100));
    due = slots.at(100);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0]->activate, 2);
    EXPECT_TRUE(slots.forget_pass(101));
    EXPECT_EQ(slots.find(101), nullptr);
    EXPECT_TRUE(slots.at(102).empty());
    EXPECT_FALSE(slots.forget_pass(102));
    // A pending activation alone re-issues as the one slot did.
    slots.record_pending(103, pending);
    ASSERT_EQ(slots.at(103).size(), 1u);
    // The last 4096 ticks.
    slots.set_pass(10000, pass);
    EXPECT_TRUE(slots.at(100).empty());
    EXPECT_EQ(slots.at(10000).size(), 1u);
    slots.clear();
    EXPECT_TRUE(slots.at(10000).empty());
}

// Step RO1 (session/script_order.hpp): a replayed frame runs the selected
// scripts in the order the host's live frame of that tick ran them.  Ranked
// scripts take their live rank (the live order is priority-sorted, so among
// one priority the rank decides); a script the live frame did not run falls
// back to the replay's own (-priority, id) order, after the ranked ones of
// its priority; no live order leaves the list as it is.
TEST(RollbackBinding, TheReplayedScriptsRunInTheLiveFramesOrderWithThePriorityFallback) {
    const std::map<uint32_t, int> priority = {{10, 5}, {11, 0}, {12, 0}, {13, 0}, {14, 0}, {15, -3}};
    const auto priority_of = [&priority](uint32_t id) { return priority.at(id); };
    // select's order: (-priority, id).
    std::vector<uint32_t> scripts = {10, 11, 12, 13, 14, 15};
    // The live frame ran 14, 12, 11 among the priority-0 owners (an unstable
    // sort over a hash-ordered append), 10 first, 15 last; 13 was not run.
    const std::vector<uint32_t> live = {10, 14, 12, 11, 15};
    bmmo::session::order_by_live_rank(scripts, live, priority_of);
    EXPECT_EQ(scripts, std::vector<uint32_t>({10, 14, 12, 11, 13, 15}));
    // Not run live and of a higher priority than every ranked one: first.
    std::vector<uint32_t> late = {10, 11, 12};
    bmmo::session::order_by_live_rank(late, std::vector<uint32_t>{12, 11}, priority_of);
    EXPECT_EQ(late, std::vector<uint32_t>({10, 12, 11}));
    // No live order: unchanged.
    std::vector<uint32_t> none = {12, 11, 10};
    bmmo::session::order_by_live_rank(none, std::vector<uint32_t>{}, priority_of);
    EXPECT_EQ(none, std::vector<uint32_t>({12, 11, 10}));
}

// The live orders by tick: stored only on a change, each in force until the
// next; a record at or before an earlier one drops that later future (a
// resync's re-numbering, a tool re-running its straight pass); forget_from
// drops the old numbering's entries.
TEST(RollbackBinding, TheLiveScriptOrderIsKeptFromEachChangeOn) {
    bmmo::session::live_script_order order;
    EXPECT_EQ(order.at(100), nullptr);
    EXPECT_TRUE(order.record(100, {1, 2, 3}));
    EXPECT_FALSE(order.record(101, {1, 2, 3})) << "no change: not stored";
    EXPECT_TRUE(order.record(105, {2, 1, 3}));
    EXPECT_EQ(order.entries(), 2u);
    EXPECT_EQ(order.at(99), nullptr);
    ASSERT_NE(order.at(104), nullptr);
    EXPECT_EQ(*order.at(104), std::vector<uint32_t>({1, 2, 3}));
    EXPECT_EQ(*order.at(105), std::vector<uint32_t>({2, 1, 3}));
    EXPECT_EQ(*order.at(500), std::vector<uint32_t>({2, 1, 3}));
    // Re-recording 103: the entry of 105 is a future being lived again.
    EXPECT_FALSE(order.record(103, {1, 2, 3}));
    EXPECT_EQ(order.entries(), 1u);
    EXPECT_EQ(*order.at(105), std::vector<uint32_t>({1, 2, 3}));
    EXPECT_TRUE(order.record(110, {3, 2, 1}));
    EXPECT_EQ(order.forget_from(106), 1u);
    EXPECT_EQ(*order.at(200), std::vector<uint32_t>({1, 2, 3}));
    // Trimmed past 4096 ticks, the entry in force at the edge kept.
    EXPECT_TRUE(order.record(5000, {9}));
    EXPECT_TRUE(order.record(9000, {8}));
    EXPECT_EQ(order.entries(), 3u) << "the entry of 100 still covers ticks within 4096 of 9000";
    EXPECT_TRUE(order.record(9200, {7}));
    EXPECT_EQ(order.entries(), 3u) << "100 trimmed: 5000 covers the ticks from 5000 on";
    EXPECT_EQ(*order.at(5000), std::vector<uint32_t>({9}));
    EXPECT_EQ(order.at(100), nullptr) << "older than the kept range";
}

// Step RO1: bind_session_world's half of the guard on destroying an untracked
// window birth at the restore for the replayed scripts to make again - a
// FIXED birth of this world at that tick, with no lifetime at the anchor, no
// ball, no own body, no mirror; none with --park-births.
TEST(RollbackBinding, AFixedWindowBirthWithNoLifetimeAtTheAnchorIsOneTheReplayMayMakeAgain) {
    session_backend backend;
    bmmo::session::core_world_hooks hooks;
    hooks.ball = [](const std::string& entity) { return entity == "Ball_Wood"; };
    hooks.own_body = [](const std::string& entity) { return entity == "Own_Body"; };
    hooks.mirror = [](const std::string& entity) { return entity == "Mirror_Box"; };
    bmmo::session::local_birth_log births;
    const auto fixed = [](const char* name) {
        bmmo::session::local_birth b = birth_of(name, 1.0f);
        b.recipe.fixed = true;
        return b;
    };
    births.add(130, fixed("P_Modul_01_Filler"));
    births.add(130, birth_of("P_Modul_01_Pusher", 2.0f));   // movable: the plan's
    births.add(130, fixed("Ball_Wood"));
    births.add(100, fixed("P_Rail"));
    births.add(131, fixed("P_Rail"));                        // a second life of a body alive at the anchor
    bmmo::session::session_world_options w;
    w.births = &births;
    std::vector<std::string> outcomes;
    w.fixed_birth_remade = [&](const std::string& entity, uint32_t tick, bool back) {
        outcomes.push_back(entity + "@" + std::to_string(tick) + (back ? " back" : " missing"));
    };
    rollback_world world = bmmo::session::bind_session_world(backend, hooks, w);
    ASSERT_TRUE(world.remade_by_replay);
    EXPECT_TRUE(world.remade_by_replay("P_Modul_01_Filler", 130, 120));
    EXPECT_FALSE(world.remade_by_replay("P_Modul_01_Filler", 131, 120)) << "no birth at that tick";
    EXPECT_FALSE(world.remade_by_replay("P_Modul_01_Filler", 130, 130)) << "alive at the anchor: the blob knows it";
    EXPECT_FALSE(world.remade_by_replay("P_Modul_01_Pusher", 130, 120)) << "not fixed";
    EXPECT_FALSE(world.remade_by_replay("Ball_Wood", 130, 120));
    EXPECT_FALSE(world.remade_by_replay("P_Rail", 131, 120)) << "a lifetime at the anchor (from 100): deferred as before";
    ASSERT_TRUE(world.fixed_birth_remade);
    world.fixed_birth_remade("P_Modul_01_Filler", 130, true);
    EXPECT_EQ(outcomes, std::vector<std::string>({"P_Modul_01_Filler@130 back"}));
    bmmo::session::session_world_options parked = w;
    parked.park_births = true;
    EXPECT_FALSE(bmmo::session::bind_session_world(backend, hooks, parked).remade_by_replay);
}

// Rollback-engine gaps design 2.4 (step B8EG, item EGB): keeps_parked is
// destroy_body's tests in its order without its side effects - a moved own
// birth (the core's own_birth_destroyable) and a mirror are taken out, any
// other ball and the own body stay parked, a mechanism is destroyed - and
// asking it never calls the core's destroy_own_birth.  --park-births
// installs neither.
TEST(RollbackBinding, KeepsParkedIsDestroyBodysTestsWithoutItsSideEffects) {
    session_backend backend;
    std::vector<std::string> calls;
    bmmo::session::core_world_hooks hooks;
    hooks.destroy_own_birth = [&](const std::string& entity, uint32_t tick, bool& destroyed) {
        calls.push_back("destroy_own_birth " + entity);
        if (entity != "Own_Moved" || tick != 120) return false;
        destroyed = true;
        return true;
    };
    hooks.own_birth_destroyable = [](const std::string& entity, uint32_t tick) {
        return entity == "Own_Moved" && tick == 120;
    };
    hooks.own_body = [](const std::string& entity) { return entity == "Own_Body"; };
    hooks.mirror = [](const std::string& entity) { return entity == "Mirror_Box"; };
    hooks.ball = [](const std::string& entity) { return entity == "Ball_Wood" || entity == "Own_Moved"; };
    rollback_world world = bmmo::session::bind_session_world(backend, hooks, bmmo::session::session_world_options{});
    ASSERT_TRUE(world.keeps_parked);
    ASSERT_TRUE(world.destroy_body);
    EXPECT_FALSE(world.keeps_parked("Own_Moved", 120)) << "a moved own birth: destroy_body takes it out";
    EXPECT_TRUE(world.keeps_parked("Own_Moved", 121)) << "the same ball at another tick: parked";
    EXPECT_FALSE(world.keeps_parked("Mirror_Box", 120)) << "a mirror: the core makes it again";
    EXPECT_TRUE(world.keeps_parked("Ball_Wood", 120));
    EXPECT_TRUE(world.keeps_parked("Own_Body", 120));
    EXPECT_FALSE(world.keeps_parked("P_Box", 120)) << "a mechanism";
    EXPECT_TRUE(calls.empty()) << "asking never calls destroy_own_birth";
    EXPECT_TRUE(backend.calls.empty());
    // destroy_body agrees wherever keeps_parked says parked: it refuses.
    EXPECT_FALSE(world.destroy_body("Own_Moved", 121));
    EXPECT_FALSE(world.destroy_body("Ball_Wood", 120));
    EXPECT_FALSE(world.destroy_body("Own_Body", 120));
    EXPECT_TRUE(backend.calls.empty()) << "nothing taken out";
    EXPECT_TRUE(world.destroy_body("Own_Moved", 120)) << "and takes the moved own birth out";
    bmmo::session::session_world_options parked;
    parked.park_births = true;
    const rollback_world plain = bmmo::session::bind_session_world(backend, hooks, parked);
    EXPECT_FALSE(plain.keeps_parked);
    EXPECT_FALSE(plain.destroy_body);
}
