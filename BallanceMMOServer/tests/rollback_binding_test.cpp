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
#include <string>
#include <vector>

#include <session/rollback_binding.hpp>
#include <session/session_host_world.hpp>

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
// one the host calls revivable (none without the predicate).
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
    w.bind_scripts = [&](rollback_world& world) {
        order.push_back("scripts bound");
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
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "physicalize P_Box", "unmute", "mute",
                                                       "physicalize Ball_Stone_piece01", "unmute"}));
    EXPECT_EQ(order, std::vector<std::string>({"put back P_Box 1", "put back Ball_Stone_piece01 4"}));
    backend.calls.clear();
    backend.refuse_writes = true;
    EXPECT_FALSE(world.revive_body("P_Plank", 130)) << "the make refused";
    EXPECT_EQ(backend.calls, std::vector<std::string>({"mute", "physicalize P_Plank", "unmute"}));
    backend.refuse_writes = false;
    order.clear();
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

    // --park-births: no destroy; without --insert-births: no insert.
    bmmo::session::session_world_options parked;
    parked.park_births = true;
    rollback_world plain = bmmo::session::bind_session_world(backend, hooks, parked);
    EXPECT_FALSE(plain.destroy_body);
    EXPECT_FALSE(plain.revive_body);
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
