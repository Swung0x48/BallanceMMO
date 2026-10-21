// Unit tests for session/rollback_binding.hpp over a fake backend: the one
// place the engine's hooks meet a physics world, bound the same way for the
// headless client, the SimTool and the retail mod.  The fake has the methods
// of physics_view / physics_direct under their names and records every call,
// so the tests can check what the binding asked of the world and what it
// reported back.  Nothing here touches a CK type: the core binding compiles
// without the engine, which is what lets the mod and the tools share it.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <session/rollback_binding.hpp>

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

TEST(RollbackBinding, NavPollAlwaysReachesTheBackendAndResumesOnlyWhenSwitchedOn) {
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

    // On while the pause menu mutes the input: stays off, nothing resumes.
    muted = true;
    EXPECT_TRUE(world.nav_poll("ball", true));
    EXPECT_FALSE(backend.last_poll_on);
    EXPECT_EQ(resumed.size(), 1u);
    EXPECT_EQ(count_calls(backend, "poll_"), 3) << "every call reaches the backend";
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
