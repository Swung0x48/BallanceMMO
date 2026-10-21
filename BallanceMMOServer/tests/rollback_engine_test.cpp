// Unit tests for the client-side rollback engine (design 9.6, restore rules of
// 9.25) over a fake world: bodies move by their velocity each step, a navigated
// ball gains +1 m/s on x per step while key 0 is held, and every adapter call is
// recorded so the tests can check what the engine did to the world.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <session/correction.hpp>
#include <session/rollback.hpp>
#include <session/timeline.hpp>

namespace {
    using bmmo::session::ball_pose;
    using bmmo::session::body_corrector;
    using bmmo::session::body_kind;
    using bmmo::session::body_state;
    using bmmo::session::correction_step;
    using bmmo::session::input_frame;
    using bmmo::session::rollback_engine;
    using bmmo::session::rollback_tracked;
    using bmmo::session::rollback_world;
    using bmmo::session::wake_mode;

    constexpr double kDt = 1.0 / 66.0;

    struct fake_body {
        double position[3] = {};
        float linear[3] = {};
        bool simulated = true;
        // The integrator state a body state does not carry (bridge API v11):
        // the queue IVP would take into the next PSI, and one component of the
        // last-PSI rotation, which is enough to tell a restored quaternion from
        // a re-derived one.
        float speed_change[3] = {};
        double q_last_w = 1.0;
        // The collision bookkeeping neither a body state nor its extra carries
        // - what only the IVP savestate puts back (docs/ivp-savestate-design.md).
        // Stepped, and fed into the motion, only in a world with
        // `savestates` on; zeroed by every pose write, as the beam zeroes
        // current_speed / abs_omega.
        double contact_energy = 0.0;
        uint32_t pair_age = 0;
    };

    const char* mode_name(wake_mode mode) {
        return mode == wake_mode::wake ? "wake" : mode == wake_mode::freeze ? "freeze" : "keep";
    }

    struct fake_world {
        std::map<std::string, fake_body> bodies;
        std::map<std::string, bmmo_physics_nav_state> navs;
        std::map<std::string, uint8_t> pending_keys;   // applied at the next step
        std::vector<std::string> calls;
        bool clock_running = true;
        int steps = 0;
        double clock = 0.0;   // the environment's time, advanced by each step
        // Opt-in (the other tests never set these): the hidden per-body state
        // above runs, get_savestate / set_savestate carry the whole world
        // (bodies, hidden fields, clock) as bytes, and get_contacts /
        // set_contacts are installed so a test can see the per-body contact
        // restore run or not.
        bool savestates = false;
        bool empty_blobs = false;        // get_savestate succeeds with no bytes
        bool refuse_savestate = false;   // set_savestate logs, then returns false
        std::vector<std::string> drop_contacts_of;   // set_savestate reports these bodies' contacts as not carried
        std::vector<uint8_t> last_set_blob;               // the last blob set_savestate was given
        std::vector<uint8_t> last_get_blob;               // the last blob get_savestate returned
        std::vector<std::vector<uint8_t>> got_blobs;      // every blob get_savestate returned

        // The whole world as bytes: every body in name order with every field
        // (copied, not converted), then the clock.
        std::vector<uint8_t> serialize() const {
            std::vector<uint8_t> out;
            const auto put = [&out](const void* data, size_t size) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                out.insert(out.end(), bytes, bytes + size);
            };
            const uint32_t body_count = static_cast<uint32_t>(bodies.size());
            put(&body_count, sizeof body_count);
            for (const auto& [name, body]: bodies) {
                const uint32_t length = static_cast<uint32_t>(name.size());
                put(&length, sizeof length);
                put(name.data(), length);
                put(body.position, sizeof body.position);
                put(body.linear, sizeof body.linear);
                const uint8_t simulated = body.simulated ? 1 : 0;
                put(&simulated, sizeof simulated);
                put(body.speed_change, sizeof body.speed_change);
                put(&body.q_last_w, sizeof body.q_last_w);
                put(&body.contact_energy, sizeof body.contact_energy);
                put(&body.pair_age, sizeof body.pair_age);
            }
            put(&clock, sizeof clock);
            return out;
        }

        // The inverse, leniently like the rollback's restore: a body in the
        // blob that is gone now is skipped, a body alive now that the blob
        // does not know keeps its live state.  false (and nothing changed) on
        // a malformed blob.
        bool apply(const std::vector<uint8_t>& blob) {
            size_t at = 0;
            const auto get = [&](void* data, size_t size) {
                if (blob.size() - at < size) return false;
                if (size) std::memcpy(data, blob.data() + at, size);
                at += size;
                return true;
            };
            uint32_t body_count = 0;
            if (!get(&body_count, sizeof body_count)) return false;
            std::map<std::string, fake_body> stored;
            for (uint32_t i = 0; i < body_count; ++i) {
                uint32_t length = 0;
                if (!get(&length, sizeof length) || blob.size() - at < length) return false;
                std::string name(reinterpret_cast<const char*>(blob.data()) + at, length);
                at += length;
                fake_body body;
                uint8_t simulated = 0;
                if (!get(body.position, sizeof body.position) || !get(body.linear, sizeof body.linear)
                        || !get(&simulated, sizeof simulated) || !get(body.speed_change, sizeof body.speed_change)
                        || !get(&body.q_last_w, sizeof body.q_last_w)
                        || !get(&body.contact_energy, sizeof body.contact_energy)
                        || !get(&body.pair_age, sizeof body.pair_age))
                    return false;
                body.simulated = simulated != 0;
                stored[name] = body;
            }
            double stored_clock = 0.0;
            if (!get(&stored_clock, sizeof stored_clock) || at != blob.size()) return false;
            for (const auto& [name, body]: stored) {
                auto it = bodies.find(name);
                if (it != bodies.end()) it->second = body;
            }
            clock = stored_clock;
            return true;
        }

        rollback_world adapter() {
            rollback_world w;
            w.get_body = [this](const std::string& entity, bmmo_physics_body_state& out) {
                auto it = bodies.find(entity);
                if (it == bodies.end()) return false;
                out = {};
                for (int k = 0; k < 3; ++k) {
                    out.position[k] = it->second.position[k];
                    out.linear[k] = it->second.linear[k];
                }
                out.rotation[3] = 1.0;
                out.simulated = it->second.simulated;
                return true;
            };
            // The call log carries the mode and the recheck request, in that
            // order, so a count() prefix can ask for either one or both.
            w.set_body = [this](const std::string& entity, const bmmo_physics_body_state& state,
                                wake_mode mode, bool recheck) {
                calls.push_back("set_body " + entity + " " + mode_name(mode) + (recheck ? " recheck" : ""));
                auto& body = bodies[entity];
                for (int k = 0; k < 3; ++k) {
                    body.position[k] = state.position[k];
                    body.linear[k] = state.linear[k];
                }
                if (mode == wake_mode::wake) body.simulated = true;
                else if (mode == wake_mode::freeze) body.simulated = false;
                // Design 8.4, which is the behaviour the v11 restore works
                // around: a pose write drops whatever was queued, and
                // re-derives the rotation rather than restoring it.
                for (int k = 0; k < 3; ++k) body.speed_change[k] = 0.0f;
                body.q_last_w = -1.0;   // "whatever the derivation produced"
                // ...and the collision bookkeeping, as the real beam zeroes
                // current_speed / abs_omega: only a savestate brings it back.
                body.contact_energy = 0.0;
                body.pair_age = 0;
                return true;
            };
            w.get_body_extra = [this](const std::string& entity, bmmo_physics_core_extra& out) {
                auto it = bodies.find(entity);
                if (it == bodies.end()) return false;
                out = {};
                // Both bits, as the bridge's get_core_extra reports them: the
                // engine clears what the caller does not want.
                out.fields = BMMO_PHYSICS_EXTRA_IMPULSES | BMMO_PHYSICS_EXTRA_QUATS;
                for (int k = 0; k < 3; ++k) out.speed_change[k] = it->second.speed_change[k];
                out.q_last_psi[3] = it->second.q_last_w;
                return true;
            };
            // The call log carries the mask, so a test can pin which halves of
            // the integrator state a restore asked for.
            w.set_body_extra = [this](const std::string& entity, const bmmo_physics_core_extra& state) {
                calls.push_back("set_body_extra " + entity + " fields=" + std::to_string(state.fields));
                auto& body = bodies[entity];
                if (state.fields & BMMO_PHYSICS_EXTRA_IMPULSES)
                    for (int k = 0; k < 3; ++k) body.speed_change[k] = state.speed_change[k];
                if (state.fields & BMMO_PHYSICS_EXTRA_QUATS) body.q_last_w = state.q_last_psi[3];
                return true;
            };
            w.get_clock = [this](bmmo_physics_clock_state& out) {
                out = {};
                out.current_time = clock;
                out.time_of_last_psi = clock;
                out.time_of_next_psi = clock + kDt;
                return true;
            };
            w.set_clock = [this](const bmmo_physics_clock_state& state) {
                calls.push_back("set_clock");
                clock = state.current_time;
                return true;
            };
            w.get_nav = [this](const std::string& entity, bmmo_physics_nav_state& out) {
                auto it = navs.find(entity);
                if (it == navs.end()) return false;
                out = it->second;
                return true;
            };
            w.set_nav = [this](const std::string& entity, const bmmo_physics_nav_state& state) {
                calls.push_back("set_nav " + entity);
                navs[entity] = state;
                return true;
            };
            w.nav_input = [this](const std::string& entity, const input_frame& frame) {
                calls.push_back("nav_input " + entity + " keys=" + std::to_string(frame.keys));
                pending_keys[entity] = frame.keys;
                return true;
            };
            w.nav_poll = [this](const std::string& entity, bool enable) {
                calls.push_back(std::string("nav_poll ") + entity + (enable ? " on" : " off"));
                return true;
            };
            w.step = [this]() {
                calls.push_back("step");
                ++steps;
                clock += kDt;
                for (auto& [name, body]: bodies) {
                    if (!body.simulated) continue;
                    auto keys = pending_keys.find(name);
                    if (keys != pending_keys.end() && (keys->second & 1u)) body.linear[0] += 1.0f;
                    if (savestates) {
                        // The hidden state ages, accumulates from the motion
                        // and drives one velocity component, so a restore that
                        // loses it moves the body from then on.
                        ++body.pair_age;
                        body.contact_energy = body.contact_energy * 0.75 + static_cast<double>(body.linear[0]) * 0.125
                                            + static_cast<double>(body.pair_age % 5u) * 0.0625;
                        body.linear[1] = static_cast<float>(body.contact_energy * 0.5);
                    }
                    for (int k = 0; k < 3; ++k) body.position[k] += body.linear[k] * kDt;
                }
                pending_keys.clear();
                return true;
            };
            w.simulating = [this]() { return clock_running; };
            w.log = [this](const std::string& text) { calls.push_back("log " + text); };
            if (savestates) {
                w.get_savestate = [this](std::vector<uint8_t>& out) {
                    calls.push_back("get_savestate");
                    out = empty_blobs ? std::vector<uint8_t>() : serialize();
                    last_get_blob = out;
                    got_blobs.push_back(out);
                    return true;
                };
                w.set_savestate = [this](const std::vector<uint8_t>& blob, bmmo::session::savestate_outcome& outcome) {
                    calls.push_back("set_savestate");
                    last_set_blob = blob;
                    if (refuse_savestate) return false;
                    outcome.contacts_dropped = drop_contacts_of;
                    return apply(blob);
                };
                // One resting contact per body, so the per-body path has
                // something to re-create.
                w.get_contacts = [this](const std::string& entity, std::vector<bmmo_physics_contact>& out) -> int {
                    if (bodies.find(entity) == bodies.end()) return -1;
                    out.assign(1, bmmo_physics_contact{});
                    std::snprintf(out[0].partner, sizeof out[0].partner, "%s", "Floor");
                    return 1;
                };
                w.set_contacts = [this](const std::string& entity, const std::vector<bmmo_physics_contact>&,
                                        uint32_t) -> int {
                    calls.push_back("set_contacts " + entity);
                    return bodies.find(entity) == bodies.end() ? -1 : 0;
                };
            }
            return w;
        }

        // Index of the first call with this prefix; calls.size() when none.
        size_t first(const std::string& prefix) const {
            for (size_t i = 0; i < calls.size(); ++i)
                if (calls[i].rfind(prefix, 0) == 0) return i;
            return calls.size();
        }

        int count(const std::string& prefix) const {
            int n = 0;
            for (const auto& call: calls) n += call.rfind(prefix, 0) == 0 ? 1 : 0;
            return n;
        }
    };

    body_state ball_body(uint32_t owner, const fake_body& body) {
        body_state out;
        out.kind = body_kind::Ball;
        out.owner = owner;
        for (int k = 0; k < 3; ++k) {
            out.position[k] = body.position[k];
            out.linear[k] = body.linear[k];
        }
        out.rotation[3] = 1.0;
        out.flags = body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0;
        return out;
    }

    // A shared-mechanism row that names its own entity, the way the client's
    // registry resolves one.
    body_state mechanism_row(const std::string& name, double x, float v, bool simulated) {
        body_state out;
        out.kind = body_kind::Mechanism;
        out.owner = 7;
        out.name = name;
        out.position[0] = x;
        out.linear[0] = v;
        out.rotation[3] = 1.0;
        out.flags = simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0;
        return out;
    }

    bmmo::session_snapshot_msg snapshot_of(uint32_t tick, const std::vector<body_state>& bodies) {
        bmmo::session_snapshot_msg msg;
        msg.tick = tick;
        msg.full = 1;
        msg.bodies = bodies;
        return msg;
    }

    // The own ball is player 1 / entity "Own", the remote ball player 2 /
    // entity "Remote".
    std::string entity_of(const body_state& body) {
        if (body.kind != body_kind::Ball) return {};
        return body.owner == 1 ? "Own" : body.owner == 2 ? "Remote" : "";
    }

    // The same, plus mechanism rows that carry their entity name.
    std::string entity_of_named(const body_state& body) {
        if (body.kind == body_kind::Mechanism) return body.name;
        return entity_of(body);
    }

    bool no_input(const std::string&, uint32_t, input_frame&) { return false; }

    // Records ticks 1..last: the own ball holds key 0 from `key_from` on.
    void run_ticks(fake_world& world, rollback_engine& engine, uint32_t last, uint32_t key_from,
                   std::map<uint32_t, input_frame>& own_inputs) {
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.own_polls = true;
        tracked.remote_entities = {"Remote"};
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= last; ++tick) {
            input_frame own{};
            own.keys = tick >= key_from ? 1 : 0;
            own.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            world.pending_keys["Own"] = own.keys;
            w.step();
            own_inputs[tick] = own;
            std::map<std::string, input_frame> applied{{"Own", own}, {"Remote", input_frame{}}};
            engine.record(w, tick, tracked, applied);
        }
        world.calls.clear();
        world.steps = 0;
    }

    // Steps and records ticks first..last with one tracked set, no inputs.
    void record_ticks(fake_world& world, rollback_engine& engine, const rollback_tracked& tracked,
                      uint32_t first, uint32_t last) {
        auto w = world.adapter();
        for (uint32_t tick = first; tick <= last; ++tick) {
            w.step();
            engine.record(w, tick, tracked, {});
        }
    }

    // The savestate tests' world: the hidden state on, the own ball and a
    // remote ball under navigation, and a mechanism holding a queued push.
    void seed_savestate_world(fake_world& world) {
        world.savestates = true;
        world.bodies["Own"];
        world.bodies["Remote"].position[0] = 5.0;
        world.bodies["Remote"].linear[0] = 0.5f;
        world.bodies["Plate"].position[0] = 40.0;
        world.bodies["Plate"].speed_change[0] = 1.212f;
        world.navs["Own"] = {};
        world.navs["Remote"] = {};
    }

    rollback_tracked savestate_tracked() {
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.own_polls = true;
        tracked.remote_entities = {"Remote"};
        tracked.mechanisms = {"Plate"};
        return tracked;
    }

    // Records ticks 1..last (the own ball holds key 0 from tick 3) and returns
    // the blob each record captured, by tick.  Clears the call log after.
    std::map<uint32_t, std::vector<uint8_t>> record_with_blobs(fake_world& world, rollback_engine& engine,
                                                               uint32_t last) {
        std::map<uint32_t, std::vector<uint8_t>> blobs;
        const rollback_tracked tracked = savestate_tracked();
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= last; ++tick) {
            input_frame own{};
            own.keys = tick >= 3 ? 1 : 0;
            own.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            world.pending_keys["Own"] = own.keys;
            w.step();
            world.last_get_blob.clear();
            engine.record(w, tick, tracked, {{"Own", own}, {"Remote", input_frame{}}});
            blobs[tick] = world.last_get_blob;
        }
        world.calls.clear();
        world.got_blobs.clear();
        world.steps = 0;
        return blobs;
    }

    // The first field in which two worlds differ by bit pattern, then their
    // blobs; "" when they are the same.
    std::string bit_difference(const fake_world& a, const fake_world& b) {
        if (std::memcmp(&a.clock, &b.clock, sizeof a.clock) != 0) return "clock";
        if (a.bodies.size() != b.bodies.size()) return "body count";
        for (const auto& [name, x]: a.bodies) {
            const auto it = b.bodies.find(name);
            if (it == b.bodies.end()) return name + " missing";
            const fake_body& y = it->second;
            if (std::memcmp(x.position, y.position, sizeof x.position) != 0) return name + ".position";
            if (std::memcmp(x.linear, y.linear, sizeof x.linear) != 0) return name + ".linear";
            if (std::memcmp(x.speed_change, y.speed_change, sizeof x.speed_change) != 0) return name + ".speed_change";
            if (std::memcmp(&x.q_last_w, &y.q_last_w, sizeof x.q_last_w) != 0) return name + ".q_last_w";
            if (std::memcmp(&x.contact_energy, &y.contact_energy, sizeof x.contact_energy) != 0)
                return name + ".contact_energy";
            if (x.pair_age != y.pair_age) return name + ".pair_age";
            if (x.simulated != y.simulated) return name + ".simulated";
        }
        if (a.serialize() != b.serialize()) return "blob";
        return "";
    }

    struct lockstep_outcome {
        bool exact = true;
        std::string first_difference;   // the field, and when it first differed
        uint64_t rewinds = 0;           // rewinds the engine ran
        uint64_t refused = 0;           // rewinds the engine refused (anchor not recorded)
        uint64_t savestates_restored = 0;
        uint64_t savestate_failures = 0;
    };

    // Two identical savestate worlds fed identical inputs: the control is
    // stepped straight through, the arm is recorded and rewound `depth` ticks
    // every `every` ticks (the window replayed from the records), and the two
    // are compared by bit pattern after every rewind and at the end.
    lockstep_outcome run_lockstep(uint32_t depth, uint32_t every, bool exact_restore) {
        constexpr uint32_t kTicks = 200;
        fake_world control, arm;
        seed_savestate_world(control);
        seed_savestate_world(arm);
        rollback_engine engine;
        engine.set_exact_restore(exact_restore);
        const rollback_tracked tracked = savestate_tracked();
        auto straight = control.adapter();
        auto rewound = arm.adapter();
        lockstep_outcome outcome;
        const auto compare = [&](uint32_t tick, const char* when) {
            if (!outcome.exact) return;
            const std::string where = bit_difference(control, arm);
            if (where.empty()) return;
            outcome.exact = false;
            outcome.first_difference = where + " " + when + " " + std::to_string(tick);
        };
        for (uint32_t tick = 1; tick <= kTicks; ++tick) {
            input_frame own{}, remote{};
            own.keys = (tick / 9) % 2 == 1 ? 1 : 0;
            own.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            remote.keys = (tick / 13) % 3 == 1 ? 1 : 0;
            remote.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            for (fake_world* world: {&control, &arm}) {
                world->pending_keys["Own"] = own.keys;
                world->pending_keys["Remote"] = remote.keys;
            }
            straight.step();
            rewound.step();
            engine.record(rewound, tick, tracked, {{"Own", own}, {"Remote", remote}});
            if (tick % every != 0 || tick <= depth) continue;
            if (engine.rewind_to(rewound, tick - depth, tick, no_input, "lockstep")) ++outcome.rewinds;
            else ++outcome.refused;
            compare(tick, "after the rewind at tick");
        }
        compare(kTicks, "at the end, tick");
        outcome.savestates_restored = engine.stats().savestates_restored;
        outcome.savestate_failures = engine.stats().savestate_failures;
        return outcome;
    }
}

TEST(RollbackEngine, MatchingSnapshotIsNotARollback) {
    fake_world world;
    world.bodies["Own"].linear[0] = 1.0f;
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    // the server agrees with what was recorded at tick 2
    fake_body own_at_2 = world.bodies["Own"];
    own_at_2.position[0] = 2.0 * kDt;
    const auto snapshot = snapshot_of(2, {ball_body(1, own_at_2), ball_body(2, world.bodies["Remote"])});
    const bool rolled = engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input);
    EXPECT_FALSE(rolled);
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
}

TEST(RollbackEngine, UnmatchedTickIsCounted) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 3, 100, own_inputs);
    const auto snapshot = snapshot_of(9, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 3, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(world.steps, 0);
}

TEST(RollbackEngine, MismatchRestoresAndResimulatesWithRecordedInputs) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    // key 0 held from tick 3: the own ball accelerates at ticks 3 and 4
    run_ticks(world, engine, 4, 3, own_inputs);
    ASSERT_NEAR(world.bodies["Own"].linear[0], 2.0f, 1e-6f);

    // the server had the own ball 0.5 m further at tick 2, still at rest
    fake_body server_own;
    server_own.position[0] = 0.5;
    fake_body server_remote = world.bodies["Remote"];
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    auto w = world.adapter();
    const bool rolled = engine.on_snapshot(w, snapshot, 4, entity_of,
                                           [&](const std::string& entity, uint32_t tick, input_frame& out) {
                                               if (entity != "Own") return false;
                                               auto it = own_inputs.find(tick);
                                               if (it == own_inputs.end()) return false;
                                               out = it->second;
                                               return true;
                                           });
    EXPECT_TRUE(rolled);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);
    EXPECT_EQ(world.steps, 2);
    EXPECT_EQ(world.count("set_body Own wake"), 1);
    EXPECT_EQ(world.count("set_body Remote wake"), 1);
    EXPECT_EQ(world.count("set_nav"), 2);
    // polling paused for the re-simulation, resumed afterwards, in that order
    EXPECT_EQ(world.count("nav_poll Own off"), 1);
    EXPECT_EQ(world.count("nav_poll Own on"), 1);
    // the recorded inputs of ticks 3 and 4 were replayed (key 0 held)
    EXPECT_EQ(world.count("nav_input Own keys=1"), 2);
    // own ball: restored to 0.5 m at rest, then two accelerating steps
    // (v = 1 then 2 m/s) from the recorded inputs
    const double expected = 0.5 + 1.0 * kDt + 2.0 * kDt;
    EXPECT_NEAR(world.bodies["Own"].position[0], expected, 1e-9);
    EXPECT_NEAR(world.bodies["Own"].linear[0], 2.0f, 1e-6f);
    // the history now holds the re-simulated state of tick 4: a matching
    // snapshot of tick 4 is not a mismatch
    fake_body own_at_4 = world.bodies["Own"];
    const auto later = snapshot_of(4, {ball_body(1, own_at_4), ball_body(2, world.bodies["Remote"])});
    EXPECT_FALSE(engine.on_snapshot(w, later, 4, entity_of, no_input));
    EXPECT_EQ(engine.stats().matched, 1u);
}

TEST(RollbackEngine, RelayedRemoteInputsReplacePredictionsAndConverge) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    // Live frames predicted that the remote stayed idle. Its real press,
    // release, and second press arrive with the delayed snapshot.
    run_ticks(world, engine, 5, 3, own_inputs);
    fake_body server_remote;
    server_remote.position[0] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    int own_queries = 0;
    auto input_at = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        out = {};
        if (entity == "Own") {
            ++own_queries;
            return true;   // this idle fallback must not replace recorded own inputs
        }
        if (entity != "Remote") return false;
        out.keys = tick == 4 ? 0 : 1;
        return true;
    };
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 5, entity_of, input_at));
    EXPECT_EQ(own_queries, 0);
    EXPECT_NEAR(world.bodies["Own"].linear[0], 3.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Own"].position[0], 6.0 * kDt, 1e-9);
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 1.0 + 4.0 * kDt, 1e-9);

    // The next snapshot confirms an intermediate replayed tick. The old
    // prediction must also have been replaced in history, or this rolls back
    // again even though the authoritative inputs were already available.
    server_remote.linear[0] = 1.0f;
    server_remote.position[0] = 1.0 + 2.0 * kDt;
    const auto later = snapshot_of(4, {ball_body(2, server_remote)});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), later, 5, entity_of, input_at));
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.stats().matched, 1u);

    // If that relay is no longer available on another correction, retain the
    // inputs captured during the first replay instead of reverting to idle.
    server_remote.position[0] = 2.0;
    server_remote.linear[0] = 0.0f;
    const auto revised = snapshot_of(2, {ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), revised, 5, entity_of, no_input));
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 2.0 + 4.0 * kDt, 1e-9);
}

TEST(RollbackEngine, MissingRelayKeepsRecordedRemotePrediction) {
    fake_world world;
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    tracked.remote_entities = {"Remote"};
    input_frame held{};
    held.keys = 1;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        world.pending_keys["Remote"] = held.keys;
        w.step();
        engine.record(w, tick, tracked, {{"Remote", held}});
    }
    fake_body server_remote;
    server_remote.position[0] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of, no_input));
    EXPECT_NEAR(world.bodies["Remote"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 1.0 + 3.0 * kDt, 1e-9);
}

// What the session journal records: one correction per decision, the mismatch
// (kind 0) before the completed rollback (kind 1), and an unmatched snapshot
// (kind 7) with no entity.
TEST(RollbackEngine, CorrectionCallbackReportsEveryDecision) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 3, own_inputs);

    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };

    // the server had the own ball 0.5 m further at tick 2
    fake_body server_own;
    server_own.position[0] = 0.5;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of, no_input));
    ASSERT_EQ(corrections.size(), 2u);
    EXPECT_EQ(corrections[0].kind, 0);
    EXPECT_EQ(corrections[0].tick, 2u);
    EXPECT_EQ(corrections[0].local_tick, 4u);
    EXPECT_EQ(corrections[0].entity, "Own");
    EXPECT_NEAR(corrections[0].error_m, 0.5, 1e-9);
    EXPECT_NEAR(corrections[0].velocity_error, 0.0, 1e-9);
    EXPECT_NEAR(corrections[0].local_position[0], 0.0, 1e-9);
    EXPECT_NEAR(corrections[0].server_position[0], 0.5, 1e-9);
    // the rollback carries the same worst offender
    EXPECT_EQ(corrections[1].kind, 1);
    EXPECT_EQ(corrections[1].entity, "Own");
    EXPECT_NEAR(corrections[1].error_m, 0.5, 1e-9);

    const auto missing = snapshot_of(99, {ball_body(1, server_own)});
    EXPECT_FALSE(engine.on_snapshot(w, missing, 4, entity_of, no_input));
    ASSERT_EQ(corrections.size(), 3u);
    EXPECT_EQ(corrections[2].kind, 7);
    EXPECT_EQ(corrections[2].tick, 99u);
    EXPECT_TRUE(corrections[2].entity.empty());
}

// A mismatch can be a velocity one - 0.5 m/s trips well before 5 cm of drift
// does, which is what the onset of a divergence looks like - and then the
// record must name that body, not the one that drifted furthest inside the
// tolerance.  The log lines keep naming the largest position error.
TEST(RollbackEngine, VelocityOnlyMismatchNamesTheOffendingBody) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };

    // the remote ball is where the client had it but moving at 1 m/s, while
    // the own ball is 0.5 mm off: inside the position threshold
    fake_body server_own = world.bodies["Own"];
    server_own.position[0] += 0.0005;
    fake_body server_remote = world.bodies["Remote"];
    server_remote.linear[0] = 1.0f;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 4, entity_of, no_input));
    ASSERT_EQ(corrections.size(), 2u);
    for (const auto& c: corrections) {
        EXPECT_EQ(c.entity, "Remote");
        EXPECT_NEAR(c.velocity_error, 1.0, 1e-9);
        EXPECT_NEAR(c.error_m, 0.0, 1e-9);
        EXPECT_NEAR(c.local_position[0], 5.0, 1e-9);
        EXPECT_NEAR(c.server_position[0], 5.0, 1e-9);
    }
    EXPECT_EQ(engine.stats().last_mismatch, "Own");
}

// Every body except the own ball is predicted WITHOUT the input that drove it,
// and the deeper the window the longer that guess has had to drift.  So the
// pair those bodies are judged by is widened in proportion to the window, and
// a velocity-only disagreement that is news over two ticks is not news over
// twenty-two: re-simulating the window on its account would cost the whole
// window's physics to fix a velocity the next snapshot will carry anyway.
TEST(RollbackEngine, ABlindRemoteBallIsJudgedByAPairWidenedForTheWindow) {
    fake_world deep;
    deep.bodies["Own"];
    deep.bodies["Remote"].position[0] = 5.0;
    rollback_engine deep_engine;
    std::map<uint32_t, input_frame> deep_inputs;
    run_ticks(deep, deep_engine, 24, 100, deep_inputs);
    const int deep_steps = deep.steps;

    // the remote ball is exactly where the client has it, moving 1 m/s faster
    fake_body fast_remote = deep.bodies["Remote"];
    fast_remote.linear[0] = 1.0f;
    const auto deep_snapshot = snapshot_of(2, {ball_body(1, deep.bodies["Own"]), ball_body(2, fast_remote)});
    // window 22 ticks: 22/7 widens 0.5 m/s past 1 m/s, so this is not news
    EXPECT_FALSE(deep_engine.on_snapshot(deep.adapter(), deep_snapshot, 24, entity_of, no_input));
    EXPECT_EQ(deep_engine.stats().matched, 1u);
    EXPECT_EQ(deep_engine.stats().mismatched, 0u);
    EXPECT_EQ(deep.steps, deep_steps);

    // the same disagreement over a two-tick window is still a rollback
    fake_world shallow;
    shallow.bodies["Own"];
    shallow.bodies["Remote"].position[0] = 5.0;
    rollback_engine shallow_engine;
    std::map<uint32_t, input_frame> shallow_inputs;
    run_ticks(shallow, shallow_engine, 4, 100, shallow_inputs);
    fake_body same_remote = shallow.bodies["Remote"];
    same_remote.linear[0] = 1.0f;
    const auto shallow_snapshot = snapshot_of(2, {ball_body(1, shallow.bodies["Own"]), ball_body(2, same_remote)});
    EXPECT_TRUE(shallow_engine.on_snapshot(shallow.adapter(), shallow_snapshot, 4, entity_of, no_input));
    EXPECT_EQ(shallow_engine.stats().mismatched, 1u);
}

// The own ball is the exception: its prediction used the player's own exact
// inputs, so a disagreement there is a real divergence rather than news that
// has not arrived yet, and the window's depth buys it no slack.
TEST(RollbackEngine, TheOwnBallKeepsTheUnwidenedPairHoweverDeepTheWindow) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 24, 100, own_inputs);

    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };
    fake_body fast_own = world.bodies["Own"];
    fast_own.linear[0] = 1.0f;
    const auto snapshot = snapshot_of(2, {ball_body(1, fast_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 24, entity_of, no_input));
    EXPECT_EQ(engine.stats().mismatched, 1u);
    ASSERT_FALSE(corrections.empty());
    EXPECT_EQ(corrections.front().entity, "Own");
}

// The widening is capped, so however deep the window gets there is always a
// disagreement big enough to correct.
TEST(RollbackEngine, TheBlindPairIsCappedSoCorrectionNeverStops) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 42, 100, own_inputs);

    // window 40 ticks is 5.7x at face value but 4x after the cap, so the pair
    // stands at 2 m/s and a remote ball 2.5 m/s off is still news.
    std::vector<bmmo::session::rollback_correction> corrections;
    auto w = world.adapter();
    w.on_correction = [&](const bmmo::session::rollback_correction& c) { corrections.push_back(c); };
    fake_body very_fast = world.bodies["Remote"];
    very_fast.linear[0] = 2.5f;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"]), ball_body(2, very_fast)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 42, entity_of, no_input));
    EXPECT_EQ(engine.stats().mismatched, 1u);
    ASSERT_FALSE(corrections.empty());
    EXPECT_EQ(corrections.front().entity, "Remote");
}

TEST(RollbackEngine, FrozenClockSnapsWithoutResimulation) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    world.clock_running = false;   // the retail scripts stopped the physics clock

    fake_body server_remote;
    server_remote.position[0] = 7.0;
    server_remote.linear[0] = 3.0f;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"]), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(engine.stats().frozen, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 0u);
    EXPECT_EQ(engine.stats().mismatched, 1u);
    EXPECT_EQ(world.steps, 0);
    EXPECT_EQ(world.count("set_body Remote wake"), 1);
    EXPECT_NEAR(world.bodies["Remote"].position[0], 7.0, 1e-9);
}

TEST(RollbackEngine, LagBeyondLimitSetsBodiesWithoutResimulation) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.max_resim_ticks = 3;
    thresholds.history_ticks = 96;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 8, 100, own_inputs);

    fake_body server_own;
    server_own.position[2] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().too_far, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(world.steps, 0);
    EXPECT_NEAR(world.bodies["Own"].position[2], 1.0, 1e-9);
    // no re-simulation recorded the lag, so the whole history went with it and
    // tick 8 is unmatched now
    EXPECT_EQ(engine.history_size(), 0u);
    const auto later = snapshot_of(8, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), later, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 1u);
}

// D1: a lag beyond the limit cannot be re-simulated, so the engine must not
// keep a record for the tick it just snapped to either - the tick counter runs
// on, and a surviving record would answer snapshots for a world state that no
// longer corresponds to that tick's inputs.
TEST(RollbackEngine, LagBeyondLimitLeavesNoHistoryToMatch) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.max_resim_ticks = 3;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 8, 100, own_inputs);
    ASSERT_GT(engine.history_size(), 1u);

    fake_body server_own;
    server_own.position[2] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().too_far, 1u);
    EXPECT_EQ(engine.history_size(), 0u);
    // the applied tick itself is gone too: the caller has to re-anchor
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(engine.stats().matched, 0u);
}

// D2: the live frame records a tick before the corrections of that tick are
// applied.  amend_record re-captures it, so a later rollback restores the
// corrected state instead of undoing the correction on its own tick.
TEST(RollbackEngine, AmendRecordReplacesTheTickStateWithoutTouchingInputs) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    // key 0 held from tick 3: the own ball accelerates at ticks 3 and 4
    run_ticks(world, engine, 4, 3, own_inputs);
    const auto recorded = engine.history_size();

    // the snapshot of tick 2 corrected the own ball and its navigation replica
    // after that tick's step: 0.25 m at rest
    world.bodies["Own"].position[0] = 0.25;
    world.bodies["Own"].linear[0] = 0.0f;
    bmmo_physics_nav_state nav{};
    nav.active = 1;
    nav.key_mask = 2;
    world.navs["Own"] = nav;
    ASSERT_TRUE(engine.amend_record(world.adapter(), 2));
    EXPECT_EQ(engine.history_size(), recorded);
    EXPECT_FALSE(engine.amend_record(world.adapter(), 99));
    EXPECT_EQ(engine.history_size(), recorded);

    // the amended pose is the record now: the server agrees with it
    fake_body server_own;
    server_own.position[0] = 0.25;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);

    // a real disagreement at tick 2 restores the amended navigation replica
    // and re-simulates ticks 3 and 4 with the inputs recorded for them
    server_own.position[0] = 0.5;
    const auto later = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), later, 4, entity_of,
        [&](const std::string& entity, uint32_t tick, input_frame& out) {
            if (entity != "Own") return false;
            auto it = own_inputs.find(tick);
            if (it == own_inputs.end()) return false;
            out = it->second;
            return true;
        }));
    EXPECT_EQ(engine.stats().resim_ticks, 2u);
    EXPECT_EQ(world.count("nav_input Own keys=1"), 2);
    EXPECT_NEAR(world.bodies["Own"].linear[0], 2.0f, 1e-6f);
    EXPECT_NEAR(world.bodies["Own"].position[0], 0.5 + 1.0 * kDt + 2.0 * kDt, 1e-9);
    EXPECT_EQ(world.navs["Own"].active, 1u);
    EXPECT_EQ(world.navs["Own"].key_mask, 2u);
}

// The correction of tick T has to survive a rollback to T.  Unamended, the
// history still holds the pre-correction state, so the server row that the
// correction was fixing looks like a fresh divergence and the rollback undoes
// it - which is the loop the amend closes.
TEST(RollbackEngine, CorrectionOfTheCurrentTickSurvivesARollbackOnlyWhenAmended) {
    for (const bool amend: {false, true}) {
        SCOPED_TRACE(amend ? "amended" : "not amended");
        fake_world world;
        world.bodies["Own"];
        world.bodies["Remote"].position[0] = 5.0;
        world.navs["Own"] = {};
        rollback_engine engine;
        std::map<uint32_t, input_frame> own_inputs;
        run_ticks(world, engine, 4, 100, own_inputs);

        // the snapshot of tick 2 moved the own ball to 0.5 m after that step
        world.bodies["Own"].position[0] = 0.5;
        if (amend) EXPECT_TRUE(engine.amend_record(world.adapter(), 2));

        fake_body server_own;
        server_own.position[0] = 0.5;
        const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
        const bool rolled = engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input);
        EXPECT_EQ(rolled, !amend);
        EXPECT_EQ(engine.stats().mismatched, amend ? 0u : 1u);
        EXPECT_EQ(engine.stats().rollbacks, amend ? 0u : 1u);
    }
}

TEST(RollbackEngine, HistoryIsBounded) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.history_ticks = 5;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 20, 100, own_inputs);
    EXPECT_EQ(engine.history_size(), 5u);
    const auto old = snapshot_of(10, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), old, 20, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 1u);
}

// 9.25 rule 4, the trafo case: the ball of the window's first ticks is gone, so
// nothing restores it (it is skipped, not an excuse to drop the window), and
// the ball the trafo created never existed at T.  It is parked at the pose it
// was born with, held there while the earlier ticks replay, and put back on its
// birth pose right before its own first tick is stepped.
TEST(RollbackEngine, TrafoBallIsHeldAtItsBirthPoseAndReposedAtItsBirthTick) {
    fake_world world;
    world.bodies["OwnOld"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["OwnOld"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    auto w = world.adapter();

    rollback_tracked before;                 // ticks 1..3: the ball we started with
    before.own_entity = "OwnOld";
    before.own_polls = true;
    before.remote_entities = {"Remote"};
    rollback_tracked after = before;          // ticks 4..6: the ball the trafo made
    after.own_entity = "OwnNew";

    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            world.bodies.erase("OwnOld");
            world.navs.erase("OwnOld");
            world.bodies["OwnNew"].position[0] = 10.0;
            world.bodies["OwnNew"].linear[0] = 3.0f;
            world.navs["OwnNew"] = {};
        }
        const std::string own = tick >= 4 ? "OwnNew" : "OwnOld";
        input_frame frame{};
        world.pending_keys[own] = frame.keys;
        w.step();
        engine.record(w, tick, tick >= 4 ? after : before, {{own, frame}, {"Remote", input_frame{}}});
    }
    world.calls.clear();
    world.steps = 0;
    // the birth record of the new ball is the state after its first step
    const double born_at = 10.0 + 3.0 * kDt;
    ASSERT_NEAR(world.bodies["OwnNew"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);

    // The server runs behind: its snapshot of tick 2 disagrees about the remote
    // ball and still carries the player-1 ball the trafo has since replaced.
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    fake_body server_own;
    server_own.position[0] = -99.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    // player 1's ball is the entity we have now, which is what the mod maps to
    auto entity_now = [](const body_state& body) -> std::string {
        if (body.kind != body_kind::Ball) return {};
        return body.owner == 1 ? "OwnNew" : body.owner == 2 ? "Remote" : "";
    };

    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 6, entity_now, no_input));
    // the window survived the lifecycle change: ticks 3..6 were replayed
    EXPECT_EQ(engine.stats().unmatched, 0u);
    EXPECT_EQ(engine.history_size(), 6u);
    EXPECT_EQ(engine.stats().resim_ticks, 4u);
    // the destroyed ball is skipped, the new one is never written to the row of
    // the ball it replaced (-99 m)
    EXPECT_EQ(world.count("set_body OwnOld"), 0);
    EXPECT_FALSE(world.bodies.contains("OwnOld"));
    // twice: parked at T, re-posed right after tick 4's step
    EXPECT_EQ(world.count("set_body OwnNew wake recheck"), 2);
    // held through ticks 3 and 4, put back on its birth record after tick 4's
    // step, then stepped at 5 and 6: exactly the state the live frames left
    EXPECT_NEAR(world.bodies["OwnNew"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);
}

// Plan doc 9: with destroy_body / recreate_body installed and the savestate
// applied, a body born inside the window is not parked and re-posed but
// destroyed at the restore and made again at the start of its birth tick, the
// way the live frame made it; after that tick's step it is expected exactly
// where its birth record has it, and nothing is written.
TEST(RollbackEngine, ABodyBornInsideTheWindowIsReCreatedAtItsBirthTickWhenTheWorldCanMakeIt) {
    fake_world world;
    world.savestates = true;
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Remote"] = {};
    rollback_engine engine;
    auto w = world.adapter();
    // The caller's record of the birth: the state the body was created with,
    // before its first step.
    std::map<std::string, fake_body> births;
    w.destroy_body = [&](const std::string& entity) {
        world.calls.push_back("destroy " + entity);
        if (!births.count(entity)) return false;
        return world.bodies.erase(entity) > 0;
    };
    w.recreate_body = [&](const std::string& entity, uint32_t tick) {
        world.calls.push_back("recreate " + entity + "@" + std::to_string(tick));
        world.bodies[entity] = births.at(entity);
        return true;
    };

    rollback_tracked before;
    before.remote_entities = {"Remote"};
    rollback_tracked after = before;
    after.mechanisms = {"Prop"};
    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            fake_body born;
            born.position[0] = 10.0;
            born.linear[0] = 3.0f;
            births["Prop"] = born;
            world.bodies["Prop"] = born;
        }
        w.step();
        engine.record(w, tick, tick >= 4 ? after : before, {{"Remote", input_frame{}}});
    }
    world.calls.clear();
    const double born_at = 10.0 + 3.0 * kDt;
    ASSERT_NEAR(world.bodies["Prop"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);

    // A snapshot of tick 2 that disagrees about the remote ball: a rewind
    // across the prop's birth at 4.
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    auto entity_now = [](const body_state& body) -> std::string {
        return body.kind == body_kind::Ball && body.owner == 2 ? "Remote" : "";
    };
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 6, entity_now, no_input));
    EXPECT_EQ(engine.stats().resim_ticks, 4u);
    EXPECT_EQ(engine.stats().savestates_restored, 1u);
    // destroyed at the restore, made again at 4, found on its record: no write
    EXPECT_EQ(world.count("destroy Prop"), 1);
    EXPECT_EQ(world.count("recreate Prop@4"), 1);
    EXPECT_EQ(world.count("set_body Prop"), 0);
    EXPECT_EQ(engine.stats().births_recreated, 1u);
    EXPECT_EQ(engine.stats().births_recreated_exact, 1u);
    EXPECT_EQ(engine.stats().birth_recreate_mismatches, 0u);
    EXPECT_EQ(engine.stats().birth_recreate_failures, 0u);
    EXPECT_TRUE(world.bodies.contains("Prop"));
    EXPECT_NEAR(world.bodies["Prop"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);
}

// The default path: insert_birth puts the parked body where its Physicalize
// made it before its birth tick's step; after the step it is on its record and
// nothing is written for it.
TEST(RollbackEngine, ABodyBornInsideTheWindowIsInsertedBeforeItsBirthTicksStepWhenTheWorldKnowsItsPhysicalize) {
    fake_world world;
    world.savestates = true;
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Remote"] = {};
    rollback_engine engine;
    auto w = world.adapter();
    std::map<std::string, fake_body> births;
    w.insert_birth = [&](const std::string& entity, uint32_t tick) {
        world.calls.push_back("insert " + entity + "@" + std::to_string(tick));
        const auto it = births.find(entity);
        if (it == births.end()) return false;
        world.bodies[entity] = it->second;   // where and how the Physicalize made it, at rest
        return true;
    };

    rollback_tracked before;
    before.remote_entities = {"Remote"};
    rollback_tracked after = before;
    after.mechanisms = {"Prop"};
    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            fake_body born;
            born.position[0] = 10.0;
            born.linear[0] = 3.0f;
            births["Prop"] = born;
            world.bodies["Prop"] = born;
        }
        w.step();
        engine.record(w, tick, tick >= 4 ? after : before, {{"Remote", input_frame{}}});
    }
    world.calls.clear();
    const double born_at = 10.0 + 3.0 * kDt;
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    auto entity_now = [](const body_state& body) -> std::string {
        return body.kind == body_kind::Ball && body.owner == 2 ? "Remote" : "";
    };
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 6, entity_now, no_input));
    EXPECT_EQ(world.count("insert Prop@4"), 1);
    // parked at the restore (one write), inserted before tick 4's step by the
    // caller, found on its record after it: no re-pose
    EXPECT_EQ(world.count("set_body Prop"), 1);
    EXPECT_EQ(engine.stats().births_inserted, 1u);
    EXPECT_EQ(engine.stats().births_inserted_exact, 1u);
    EXPECT_EQ(engine.stats().birth_insert_mismatches, 0u);
    EXPECT_NEAR(world.bodies["Prop"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);
}

// The same rewind without a recipe for the body: destroy_body says no, and the
// body is parked and re-posed as before.
TEST(RollbackEngine, ABodyBornInsideTheWindowIsParkedWhenTheWorldCannotMakeItAgain) {
    fake_world world;
    world.savestates = true;
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Remote"] = {};
    rollback_engine engine;
    auto w = world.adapter();
    w.destroy_body = [&](const std::string& entity) {
        world.calls.push_back("destroy " + entity);
        return false;
    };
    w.recreate_body = [&](const std::string&, uint32_t) { return true; };

    rollback_tracked before;
    before.remote_entities = {"Remote"};
    rollback_tracked after = before;
    after.mechanisms = {"Prop"};
    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            world.bodies["Prop"].position[0] = 10.0;
            world.bodies["Prop"].linear[0] = 3.0f;
        }
        w.step();
        engine.record(w, tick, tick >= 4 ? after : before, {{"Remote", input_frame{}}});
    }
    world.calls.clear();
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
    auto entity_now = [](const body_state& body) -> std::string {
        return body.kind == body_kind::Ball && body.owner == 2 ? "Remote" : "";
    };
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 6, entity_now, no_input));
    EXPECT_EQ(world.count("destroy Prop"), 1);
    EXPECT_EQ(world.count("recreate Prop"), 0);
    // parked at T, re-posed after tick 4's step: the pre-9 path
    EXPECT_EQ(world.count("set_body Prop"), 2);
    EXPECT_EQ(engine.stats().births_recreated, 0u);
    EXPECT_EQ(engine.stats().birth_recreate_failures, 1u);
}

// 9.25 rule 1: a row for a body we have no record of at T is not a rejection.
// The body exists now, so the row is the best pose anyone has for it at T and
// the restore uses it; the snapshot corrects the rest of the world as usual.
TEST(RollbackEngine, RowForAnUnrecordedBodyRestoresFromTheRowInsteadOfRejectingTheSnapshot) {
    fake_world world;
    world.bodies["Own"].position[0] = 3.0;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    auto w = world.adapter();
    engine.record(w, 218, tracked, {});
    // Like the retail queue: creation happens after record(218), so the remote
    // ball has no record at all and its live state is whatever it was created
    // with - which is exactly what the row is there to replace.
    world.bodies["Remote"].position[0] = 900.0;
    world.bodies["Remote"].linear[0] = 1171.0f;
    fake_body server_own, server_remote;
    server_remote.position[0] = 37.0;
    const auto snapshot = snapshot_of(218, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 234, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 0u);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(world.steps, 16);
    EXPECT_EQ(world.count("set_body Remote wake recheck"), 1);
    EXPECT_DOUBLE_EQ(world.bodies["Own"].position[0], 0.0);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], 37.0);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], 0.0f);
}

// 9.25 rule 5: the tracked set changing from one tick to the next is not a
// reason to throw the window away.  A body that joined it mid-window simply has
// no record at T - it is held at its birth pose and released at its birth tick.
TEST(RollbackEngine, ABodyJoiningTheTrackedSetKeepsTheHistory) {
    fake_world world;
    world.bodies["Own"];
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    auto w = world.adapter();
    engine.record(w, 1, tracked, {});
    world.bodies["Remote"].position[0] = 900.0;
    tracked.remote_entities = {"Remote"};
    engine.record(w, 2, tracked, {});
    EXPECT_EQ(engine.history_size(), 2u);
    world.calls.clear();
    world.steps = 0;

    // The delayed server snapshot of tick 1 does not list the new remote yet
    // and disagrees about the own ball.
    fake_body server_own;
    server_own.position[0] = 1.0;
    EXPECT_TRUE(engine.on_snapshot(w, snapshot_of(1, {ball_body(1, server_own)}), 2, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 0u);
    EXPECT_EQ(engine.history_size(), 2u);
    EXPECT_EQ(world.steps, 1);
    EXPECT_EQ(world.count("set_body Own wake recheck"), 1);
    // the remote is put back on its birth pose instead of being replayed from
    // wherever the replayed tick left it
    EXPECT_EQ(world.count("set_body Remote wake recheck"), 2);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], 900.0);

    // and a body leaving the tracked set does not drop the window either
    world.bodies.erase("Remote");
    tracked.remote_entities.clear();
    engine.record(w, 3, tracked, {});
    EXPECT_EQ(engine.history_size(), 3u);
}

// 9.25 rule 4: a recorded body the world no longer has is skipped - the engine
// cannot recreate it - and the rest of the world is still corrected.  The old
// rule rejected the whole snapshot here, which cost the session every
// correction until the next re-anchor.
TEST(RollbackEngine, VanishedBodyIsSkippedWithoutRejectingTheSnapshot) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    world.bodies.erase("Remote");
    fake_body server_own;
    server_own.position[0] = 1.0;
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 4, entity_of,
                                   no_input));
    EXPECT_EQ(engine.stats().unmatched, 0u);
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    EXPECT_EQ(engine.history_size(), 4u);
    EXPECT_EQ(world.count("set_body Remote"), 0);
    EXPECT_EQ(world.count("set_body Own wake recheck"), 1);
    EXPECT_EQ(world.steps, 2);
    EXPECT_FALSE(world.bodies.contains("Remote"));
}

// 9.25 lifetime generations: a respawn reuses the entity name, so only the
// generation the caller bumps tells the engine that the records before it
// describe another body.  The new ball is never restored from them (nor from a
// server row for the old one): it goes back to its own birth record.
TEST(RollbackEngine, SameNameRespawnWithANewGenerationIsNotRestoredFromTheOldLifetime) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked first;
    first.own_entity = "Own";
    first.remote_entities = {"Remote"};
    first.generations = {{"Own", 1}, {"Remote", 1}};
    record_ticks(world, engine, first, 1, 4);

    // the ball dies and is physicalized again under the same name
    world.bodies.erase("Own");
    world.bodies["Own"].position[0] = 900.0;
    rollback_tracked second = first;
    second.generations["Own"] = 2;
    record_ticks(world, engine, second, 5, 5);
    world.calls.clear();
    world.steps = 0;

    // The server is still behind: its snapshot of tick 2 carries the dead
    // ball's pose and disagrees about the remote one.
    fake_body server_own;
    server_own.position[0] = -50.0;
    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] += 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(w, snapshot, 5, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 0u);
    // neither the old record (0 m) nor the old lifetime's row (-50 m) touched
    // the new ball: it is held at its birth pose and released at tick 5
    EXPECT_EQ(world.count("set_body Own wake recheck"), 2);
    EXPECT_DOUBLE_EQ(world.bodies["Own"].position[0], 900.0);
    EXPECT_NEAR(world.bodies["Remote"].position[0], server_remote.position[0], 1e-9);
}

// 9.25 rule 1, the other half: a row we cannot compare is not a breach either.
// A snapshot whose only news is about a body born after its own tick leaves the
// session alone - the pre-9.25 engine rejected it and wiped the window.
TEST(RollbackEngine, ARowForABodyBornAfterTheSnapshotTickIsNotABreach) {
    fake_world world;
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked none;
    engine.record(w, 203, none, {});
    // the own ball is physicalized during frame 204 and recorded after its step
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    world.bodies["Own"].linear[0] = 3.0f;
    record_ticks(world, engine, tracked, 204, 204);
    world.calls.clear();
    world.steps = 0;

    fake_body server_own;
    server_own.position[0] = 77.0;
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(203, {ball_body(1, server_own)}), 204, entity_of, no_input));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_EQ(engine.history_size(), 2u);
    const auto current = snapshot_of(204, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(w, current, 204, entity_of, no_input));
    EXPECT_EQ(engine.stats().matched, 2u);
}

TEST(RollbackEngine, ResyncDiscardsBothOldNumberingAndPreOverwriteRecords) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.remote_entities = {"Remote"};
    engine.invalidate_history();    // assignment reuses an old tick number
    world.bodies["Own"].position[0] = 900.0;
    engine.record(w, 2, tracked, {}); // still waiting for the full snapshot
    engine.invalidate_history();    // full snapshot overwrites the world
    world.bodies["Own"].position[0] = 5.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(w, snapshot, 2, entity_of, no_input));
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);
    EXPECT_DOUBLE_EQ(world.bodies["Own"].position[0], 5.0);
    engine.record(w, 3, tracked, {});
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(3, {ball_body(1, world.bodies["Own"])}), 3, entity_of, no_input));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().unmatched, 1u);
}

// 9.25 rule 4, the rope-and-sack case: restoring only the body that breached
// tore a constrained pair apart, because its partner stayed on the pose the
// prediction had left it at.  Every body the snapshot carries a row for goes
// back to its own row - a sub-tolerance partner included - so the pair lands at
// the server's relative pose and the replay starts from one consistent world.
TEST(RollbackEngine, SubTolerancePartnerOfABreachingBodyIsRestoredFromItsOwnRow) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Sack"].position[0] = 20.0;
    world.bodies["Sack"].linear[0] = 0.5f;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Sack"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();
    world.steps = 0;
    const double sack_at_2 = 20.0 + 2.0 * 0.5 * kDt;

    // the own ball breaches; the sack row is 20 mm off, well inside its pair
    fake_body server_own;
    server_own.position[0] = 0.5;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own),
                                          mechanism_row("Sack", sack_at_2 + 0.02, 0.5f, true)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of_named, no_input));
    EXPECT_EQ(engine.stats().rollbacks, 1u);
    // an awake row is written with mode wake, and it moves the body, so the
    // contacts are rechecked
    EXPECT_EQ(world.count("set_body Sack wake recheck"), 1);
    EXPECT_TRUE(world.bodies["Sack"].simulated);
    // from the row, not from the record: the 20 mm are still there after the
    // two replayed steps (the record would have left it at sack_at_2 + 2 steps)
    EXPECT_NEAR(world.bodies["Sack"].position[0], sack_at_2 + 0.02 + 2.0 * 0.5 * kDt, 1e-9);
}

// 9.25 rule 2: a sleeping row inside the tolerance is no news.  Copying it
// would put the server's sleep flag on our body - which re-arms the freeze
// timer of a mechanism on every snapshot - so the body rewinds from its own
// record with mode keep, and its sleep state is left alone.  A sleeping row
// that breaches still restores and freezes.
TEST(RollbackEngine, SleepingRowInsideToleranceLeavesTheLocalSleepStateAlone) {
    for (const bool breaching: {false, true}) {
        SCOPED_TRACE(breaching ? "breaching" : "inside tolerance");
        fake_world world;
        world.bodies["Own"];
        world.bodies["Wippe"].position[0] = 30.0;
        rollback_engine engine;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.mechanisms = {"Wippe"};
        record_ticks(world, engine, tracked, 1, 4);
        world.calls.clear();
        world.steps = 0;

        fake_body server_own;
        server_own.position[0] = 0.5;   // the ball breaches, so a rollback runs
        const auto snapshot = snapshot_of(2, {ball_body(1, server_own),
                                              mechanism_row("Wippe", breaching ? 30.4 : 30.0, 0.0f, false)});
        EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of_named, no_input));
        if (breaching) {
            EXPECT_EQ(world.count("set_body Wippe freeze recheck"), 1);
            EXPECT_FALSE(world.bodies["Wippe"].simulated);
            EXPECT_DOUBLE_EQ(world.bodies["Wippe"].position[0], 30.4);
        } else {
            // restored from its own record, which is where it already is and
            // which has it awake (v13: the record's sleep state, not the live
            // one): no contact recheck, and the row's "sleeping" is not
            // applied because the row is inside tolerance
            EXPECT_EQ(world.count("set_body Wippe wake"), 1);
            EXPECT_EQ(world.count("set_body Wippe wake recheck"), 0);
            EXPECT_EQ(world.count("set_body Wippe keep"), 0);
            EXPECT_EQ(world.count("set_body Wippe freeze"), 0);
            EXPECT_TRUE(world.bodies["Wippe"].simulated);
            EXPECT_DOUBLE_EQ(world.bodies["Wippe"].position[0], 30.0);
        }
    }
}

// 9.25 rule 3: a tracked body the snapshot says nothing about gets no opinion
// from the server.  It rewinds with the group from its own record - the world
// has to be one consistent state at T - in the sleep state the RECORD has
// (v13).  It used to be restored with mode keep, which left the live sleep
// state in place: a body that fell asleep after T then replayed T+1.. asleep
// while the live frames had run it awake, and the replay diverged on exactly
// the bodies whose sleep decision fell inside the window
// (scripts/sleep_divergence.py).
TEST(RollbackEngine, TrackedBodyWithNoRowIsRestoredFromItsRecordInItsRecordedSleepState) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Wippe"].position[0] = 30.0;
    world.bodies["Wippe"].simulated = false;   // asleep at T and now
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Wippe"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();
    world.steps = 0;

    fake_body server_own;
    server_own.position[0] = 0.5;
    // a delta snapshot that carries the ball alone
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 4,
                                   entity_of_named, no_input));
    EXPECT_EQ(world.count("set_body Wippe freeze"), 1);
    EXPECT_EQ(world.count("set_body Wippe freeze recheck"), 0);   // it never moved
    EXPECT_EQ(world.count("set_body Wippe keep"), 0);
    EXPECT_FALSE(world.bodies["Wippe"].simulated);
    EXPECT_DOUBLE_EQ(world.bodies["Wippe"].position[0], 30.0);
}

// The rule itself, both ways: awake at T but asleep now is woken, asleep at T
// but awake now is frozen; and set_sleep_as_recorded(false) is the pre-v13
// restore, for the A/B.
TEST(RollbackEngine, RecordedSleepStateWinsOverTheLiveOneUnlessSwitchedOff) {
    for (const bool as_recorded: {true, false}) {
        SCOPED_TRACE(as_recorded ? "as recorded" : "keep (pre-v13)");
        fake_world world;
        world.bodies["Own"];
        world.bodies["Wippe"].position[0] = 30.0;   // awake at T
        world.bodies["Post"].position[0] = 40.0;
        world.bodies["Post"].simulated = false;     // asleep at T
        rollback_engine engine;
        engine.set_sleep_as_recorded(as_recorded);
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.mechanisms = {"Wippe", "Post"};
        record_ticks(world, engine, tracked, 1, 4);
        // after T the live frames changed both sleep states
        world.bodies["Wippe"].simulated = false;
        world.bodies["Post"].simulated = true;
        world.calls.clear();
        world.steps = 0;

        fake_body server_own;
        server_own.position[0] = 0.5;
        EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 4,
                                       entity_of_named, no_input));
        if (as_recorded) {
            EXPECT_EQ(world.count("set_body Wippe wake"), 1);
            EXPECT_EQ(world.count("set_body Post freeze"), 1);
            EXPECT_TRUE(world.bodies["Wippe"].simulated);
            EXPECT_FALSE(world.bodies["Post"].simulated);
        } else {
            EXPECT_EQ(world.count("set_body Wippe keep"), 1);
            EXPECT_EQ(world.count("set_body Post keep"), 1);
            EXPECT_FALSE(world.bodies["Wippe"].simulated);
            EXPECT_TRUE(world.bodies["Post"].simulated);
        }
    }
}

// The contact recheck is asked for only when the write actually moves the body:
// a restore that puts a body back where it already is cannot touch anything
// new, and the mindist recheck it would trigger is not free.
TEST(RollbackEngine, OnlyAWriteThatMovesTheBodyAsksForAContactRecheck) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Post"].position[0] = 40.0;   // a mechanism at rest, unmoved since T
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Post"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();
    world.steps = 0;

    fake_body server_own;
    server_own.position[0] = 0.5;              // 0.5 m: the ball is moved
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own),
                                          mechanism_row("Post", 40.0, 0.0f, true)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of_named, no_input));
    EXPECT_EQ(world.count("set_body Own wake recheck"), 1);
    EXPECT_EQ(world.count("set_body Post wake"), 1);
    EXPECT_EQ(world.count("set_body Post wake recheck"), 0);

    // the same row 2 mm away does ask for one (the ball has to breach again:
    // the first rollback made the server pose of tick 2 our record)
    world.calls.clear();
    fake_body server_own_again;
    server_own_again.position[0] = 1.5;
    const auto moved = snapshot_of(2, {ball_body(1, server_own_again),
                                       mechanism_row("Post", 40.002, 0.0f, true)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), moved, 4, entity_of_named, no_input));
    EXPECT_EQ(world.count("set_body Post wake recheck"), 1);
}

// A shared mechanism row and a ball row are judged by the same tolerance:
// 5 mm of pose error is prediction noise for either - the ball pair used to be
// 50x tighter, which rolled the session back on that noise - and both trip
// once the error passes the tolerance.
// A replay restore puts back the impulses IVP had queued for the next PSI
// (bridge API v11).  The pose write zeroes them by design 8.4 - right for a
// hard set, and the whole reason a rollback used to lose the world: the impact
// solver parks a delayed push on the heavier core, so a plate struck by a ball
// carries one, and dropping it costs centimetres per rewind.
TEST(RollbackEngine, ARewindRestoresTheQueuedImpulseOfABodyItRecordedItself) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Plate"].position[0] = 40.0;
    world.bodies["Plate"].speed_change[0] = 1.212f;   // a push IVP is holding
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Plate"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();

    // A rewind with no snapshot behind it: every body is restored from our own
    // record, so every body's queue is ours to put back.
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_body_extra Plate fields=3"), 1);
    EXPECT_FLOAT_EQ(world.bodies["Plate"].speed_change[0], 1.212f);
    EXPECT_DOUBLE_EQ(world.bodies["Plate"].q_last_w, 1.0);   // restored, not re-derived
    EXPECT_EQ(engine.stats().impulses_restored, 2u);         // Own and Plate
}

// Each half of the integrator state has its own switch, and they meet in the
// mask rather than in the call - which is what lets one binary measure the
// queued impulses against the quaternions.
TEST(RollbackEngine, EachHalfOfTheIntegratorStateHasItsOwnSwitch) {
    const auto mask_after = [](bool impulses, bool quats) {
        fake_world world;
        world.bodies["Own"];
        world.bodies["Plate"].speed_change[0] = 1.212f;
        rollback_engine engine;
        engine.set_restore_impulses(impulses);
        engine.set_restore_quaternions(quats);
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.mechanisms = {"Plate"};
        record_ticks(world, engine, tracked, 1, 4);
        world.calls.clear();
        engine.rewind_to(world.adapter(), 2, 4, no_input, "test");
        return world.count("set_body_extra Plate fields=3")   * 3
             + world.count("set_body_extra Plate fields=2")   * 2
             + world.count("set_body_extra Plate fields=1");
    };
    EXPECT_EQ(mask_after(true, true), 3);
    EXPECT_EQ(mask_after(false, true), 2);
    EXPECT_EQ(mask_after(true, false), 1);
    EXPECT_EQ(mask_after(false, false), 0);   // nothing is written at all
}

// ...and does NOT put one back on a body an authoritative row describes.  A row
// is a pose the server reported; it carries no queue, and the queue we happen
// to hold locally belongs to the state being replaced, not to the one being
// written.  That write IS the hard set design 8.4 is about.
TEST(RollbackEngine, ARowRestoreLeavesTheQueueZeroedBecauseItIsAHardSet) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Plate"].position[0] = 40.0;
    world.bodies["Plate"].speed_change[0] = 1.212f;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Plate"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();

    fake_body server_own;
    server_own.position[0] = 0.5;                      // the ball breaches
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own),
                                          mechanism_row("Plate", 41.0, 0.0f, true)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of_named, no_input));
    // The plate came from a row, so no queue is written for it and the pose
    // write's zeroing stands.
    EXPECT_EQ(world.count("set_body_extra Plate"), 0);
    EXPECT_FLOAT_EQ(world.bodies["Plate"].speed_change[0], 0.0f);
    EXPECT_DOUBLE_EQ(world.bodies["Plate"].q_last_w, -1.0);   // re-derived, as a hard set should
}

// With both halves off a restore is exactly what it was before v11, so an A/B
// never has to compare two builds - whose plain runs disagree by more than
// either half is worth.
TEST(RollbackEngine, BothSwitchesOffReproducesThePreV11Restore) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Plate"].position[0] = 40.0;
    world.bodies["Plate"].speed_change[0] = 1.212f;
    rollback_engine engine;
    engine.set_restore_impulses(false);
    engine.set_restore_quaternions(false);
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Plate"};
    record_ticks(world, engine, tracked, 1, 4);
    world.calls.clear();

    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_body_extra"), 0);
    EXPECT_FLOAT_EQ(world.bodies["Plate"].speed_change[0], 0.0f);
    EXPECT_EQ(engine.stats().impulses_restored, 0u);
}

// A caller whose world predates v11 provides neither callback, and must get
// exactly the old behaviour rather than a crash.
TEST(RollbackEngine, AWorldWithoutTheV11CallbacksStillRewinds) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Plate"].position[0] = 40.0;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Plate"};
    auto adapter = world.adapter();
    adapter.get_body_extra = nullptr;
    adapter.set_body_extra = nullptr;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        engine.record(adapter, tick, tracked, {});
        adapter.step();
    }
    world.calls.clear();

    EXPECT_TRUE(engine.rewind_to(adapter, 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_body_extra"), 0);
    EXPECT_EQ(engine.stats().impulses_restored, 0u);
    EXPECT_GT(world.count("set_body Plate"), 0);
}

// The environment's clock goes back to the anchor BEFORE any body does, and a
// replay then brings it forward to exactly where the live frames had left it:
// the rewind is invisible to IVP's notion of time.  Before this, the clock was
// never rewound, so every replayed tick ran one tick late and every rollback
// left the clock permanently ahead by its depth.
TEST(RollbackEngine, ARewindPutsTheClockBackBeforeTheBodiesAndTheReplayCatchesUp) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Plate"].position[0] = 40.0;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.mechanisms = {"Plate"};
    auto adapter = world.adapter();
    // Record after the step, as the client does: the record of tick t holds
    // the clock the live frame left at the end of t.
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        adapter.step();
        engine.record(adapter, tick, tracked, {});
    }
    const double live_clock = world.clock;        // 4 * kDt
    world.calls.clear();

    EXPECT_TRUE(engine.rewind_to(adapter, 2, 4, no_input, "test"));
    ASSERT_EQ(world.count("set_clock"), 1);
    // Order: the clock before the first body write.
    size_t clock_at = 0, body_at = 0;
    for (size_t i = 0; i < world.calls.size(); ++i) {
        if (world.calls[i] == "set_clock" && !clock_at) clock_at = i + 1;
        if (world.calls[i].rfind("set_body ", 0) == 0 && !body_at) body_at = i + 1;
    }
    ASSERT_GT(clock_at, 0u);
    ASSERT_GT(body_at, 0u);
    EXPECT_LT(clock_at, body_at);
    // Two replayed ticks from the anchor's clock land exactly on the live one.
    EXPECT_NEAR(world.clock, live_clock, 1e-12);
    EXPECT_EQ(engine.stats().clock_rewinds, 1u);
}

TEST(RollbackEngine, TheClockRestoreHasItsOwnSwitchAndAWorldWithoutAClockStillRewinds) {
    {
        fake_world world;
        world.bodies["Own"];
        rollback_engine engine;
        engine.set_restore_clock(false);
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        record_ticks(world, engine, tracked, 1, 4);
        world.calls.clear();
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
        EXPECT_EQ(world.count("set_clock"), 0);
        EXPECT_EQ(engine.stats().clock_rewinds, 0u);
    }
    {
        fake_world world;
        world.bodies["Own"];
        rollback_engine engine;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        auto adapter = world.adapter();
        adapter.get_clock = nullptr;
        adapter.set_clock = nullptr;
        for (uint32_t tick = 1; tick <= 4; ++tick) {
            engine.record(adapter, tick, tracked, {});
            adapter.step();
        }
        world.calls.clear();
        EXPECT_TRUE(engine.rewind_to(adapter, 2, 4, no_input, "test"));
        EXPECT_EQ(world.count("set_clock"), 0);
        EXPECT_GT(world.count("set_body Own"), 0);
    }
}

TEST(RollbackEngine, MechanismAndBallRowsShareTheSameTolerance) {
    fake_world world;
    world.bodies["Wippe"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    tracked.mechanisms = {"Wippe"};
    for (uint32_t tick = 1; tick <= 3; ++tick) {
        w.step();
        engine.record(w, tick, tracked, {});
    }
    world.calls.clear();
    world.steps = 0;
    auto mechanism_of = [](const body_state& body) -> std::string {
        return body.kind == body_kind::Mechanism ? "Wippe" : std::string();
    };
    body_state row;
    row.kind = body_kind::Mechanism;
    row.flags = bmmo::session::BODY_FLAG_SIMULATED;
    row.position[0] = 0.005;   // 5 mm
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(2, {row}), 3, mechanism_of, no_input));
    EXPECT_EQ(engine.stats().matched, 1u);
    EXPECT_EQ(engine.stats().mismatched, 0u);
    EXPECT_EQ(world.count("set_body"), 0);
    EXPECT_EQ(world.steps, 0);

    // the same 5 mm on a ball row is tolerated too
    fake_world ball_world;
    ball_world.bodies["Own"];
    rollback_engine ball_engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(ball_world, ball_engine, 3, 100, own_inputs);
    fake_body server_own;
    server_own.position[0] = 0.005;
    EXPECT_FALSE(ball_engine.on_snapshot(ball_world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 3,
                                         entity_of, no_input));
    EXPECT_EQ(ball_engine.stats().mismatched, 0u);

    // past the tolerance both rows are mismatches
    body_state beyond = row;
    beyond.position[0] = 0.06;
    EXPECT_TRUE(engine.on_snapshot(w, snapshot_of(2, {beyond}), 3, mechanism_of, no_input));
    EXPECT_EQ(engine.stats().mismatched, 1u);

    server_own.position[0] = 0.06;
    EXPECT_TRUE(ball_engine.on_snapshot(ball_world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 3,
                                        entity_of, no_input));
    EXPECT_EQ(ball_engine.stats().mismatched, 1u);
}

// The identity guard: Level 8 has two same-named P_Modul_30_Wippe instances
// 332 m apart in different sectors, and the protocol identifies a mechanism
// only by (name, owner).  A row that far from our own body for the same tick
// names the other instance and must never be applied to ours.
TEST(BodyCorrector, IdentityGuardIsFalseWithoutAComparison) {
    body_corrector corrector;
    EXPECT_FALSE(corrector.identity_mismatch());
    ball_pose row;
    row.tick = 5;
    row.position[0] = 332.09;
    corrector.compare(row);   // no local record for tick 5: nothing to compare
    EXPECT_FALSE(corrector.identity_mismatch());
}

TEST(BodyCorrector, IdentityGuardIgnoresALegalCorrection) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 0.5;    // within the legal mechanism motion
    const auto step = corrector.compare(row);
    EXPECT_EQ(step.action, correction_step::kind::blend);
    EXPECT_FALSE(corrector.identity_mismatch());
}

TEST(BodyCorrector, IdentityGuardSurvivesTheHistoryWipeOfAHardStep) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 332.09;   // the other sector's same-named instance
    const auto step = corrector.compare(row);
    EXPECT_EQ(step.action, correction_step::kind::hard);
    EXPECT_EQ(corrector.history_size(), 0u);   // compare() wiped it
    EXPECT_TRUE(corrector.identity_mismatch());
    corrector.clear();
    EXPECT_FALSE(corrector.identity_mismatch());
}

// The rollback path applies snapshots without compare(), so it asks the
// question directly: is this row far from our record for its own tick?
TEST(BodyCorrector, IdentityGuardComparesARowWithTheRecordedTick) {
    body_corrector corrector;
    ball_pose local;
    local.tick = 5;
    local.position[0] = 75.88;
    corrector.record(local);
    ball_pose row;
    row.tick = 5;
    row.position[0] = 402.72;   // sector 2's instance
    EXPECT_TRUE(corrector.identity_mismatch(row));
    row.position[0] = 75.9;     // our own instance
    EXPECT_FALSE(corrector.identity_mismatch(row));
    row.tick = 6;               // no record for this tick: cannot tell
    EXPECT_FALSE(corrector.identity_mismatch(row));
}

// The frozen path runs no re-simulation, so it must not keep a partial
// history: the caller's tick counter keeps running while T+1..current are
// never recorded again, and every later snapshot of that range is unmatched.
TEST(RollbackEngine, FrozenClockLeavesNoHistoryAndLaterSnapshotsAreUnmatched) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);
    ASSERT_EQ(engine.history_size(), 4u);
    world.clock_running = false;

    fake_body server_remote = world.bodies["Remote"];
    server_remote.position[0] = 7.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, world.bodies["Own"]), ball_body(2, server_remote)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(engine.stats().frozen, 1u);
    EXPECT_EQ(engine.history_size(), 0u);
    // the applied tick is gone too: the caller has to re-anchor
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 1u);
    EXPECT_EQ(engine.stats().matched, 0u);
}

// A navigation replica the world has dropped must not survive an amendment:
// the record would write it back on the next rollback.
TEST(RollbackEngine, AmendRecordDropsAVanishedNavigationReplica) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    world.navs.erase("Remote");   // the replica is gone from the world
    ASSERT_TRUE(engine.amend_record(world.adapter(), 2));
    EXPECT_EQ(engine.history_size(), 4u);

    fake_body server_own;
    server_own.position[0] = 1.0;   // past the 0.05 m pair: a real rollback
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(world.count("set_nav Remote"), 0);
    EXPECT_EQ(world.count("set_nav Own"), 1);
}

// The same for a body: a tracked name the world has dropped must not survive
// an amendment either, or the next rollback restores a replica that is gone.
TEST(RollbackEngine, AmendRecordDropsAVanishedBodyReplica) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 4, 100, own_inputs);

    world.bodies.erase("Remote");   // the replica is gone from the world
    ASSERT_TRUE(engine.amend_record(world.adapter(), 2));
    EXPECT_EQ(engine.history_size(), 4u);

    fake_body server_own;
    server_own.position[0] = 1.0;   // past the 0.05 m pair: a real rollback
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own)});
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot, 4, entity_of, no_input));
    EXPECT_EQ(world.count("set_body Remote"), 0);
    EXPECT_EQ(world.count("set_body Own"), 1);
}

// amend_record's contract: true only while the tick is still recorded.  The
// live frame ignores the return value, so a false must not be read as applied.
TEST(RollbackEngine, AmendRecordReportsWhetherTheTickIsStillRecorded) {
    fake_world world;
    world.bodies["Own"];
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    auto w = world.adapter();
    engine.record(w, 1, tracked, {});
    EXPECT_TRUE(engine.amend_record(w, 1));
    EXPECT_FALSE(engine.amend_record(w, 2));      // never recorded
    engine.invalidate_history();
    EXPECT_FALSE(engine.amend_record(w, 1));      // no longer recorded
}

// The client limits have to hold the worst lag the server may assign: the base
// lead is the input delay (at most kInputDelayMaxTicks) plus a round-trip
// allowance plus 2, and the history must keep two full resim windows so a
// rollback at the depth limit still finds its anchor record.
TEST(RollbackEngine, DefaultLimitsCoverTheWorstServerLead) {
    const bmmo::session::rollback_thresholds limits;
    EXPECT_EQ(limits.max_resim_ticks, 48u);
    EXPECT_EQ(limits.history_ticks, 96u);
    EXPECT_GE(limits.max_resim_ticks, bmmo::session::kInputDelayMaxTicks + 2);
    EXPECT_GE(limits.history_ticks, 2 * static_cast<size_t>(limits.max_resim_ticks));
}

// The pre_step hook is the caller's chance to re-pose something the engine does
// not track.  It must fire for exactly T+1..current, in ascending order, each
// call before its own world.step() - and only when a re-simulation actually
// runs.  Null on the server and in the other tests.
TEST(RollbackEngine, PreStepHookFiresOnceBeforeEveryResimulatedStep) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 5, 100, own_inputs);   // clears the call log

    auto w = world.adapter();
    std::vector<uint32_t> posed;
    w.pre_step = [&](uint32_t tick) {
        posed.push_back(tick);
        world.calls.push_back("pre_step " + std::to_string(tick));
    };

    // the server had the own ball 0.5 m further at tick 2, while the client is
    // at tick 5: a real rollback over ticks 3, 4 and 5
    fake_body server_own;
    server_own.position[0] = 0.5;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    ASSERT_TRUE(engine.on_snapshot(w, snapshot, 5, entity_of, no_input));
    ASSERT_EQ(engine.stats().resim_ticks, 3u);
    EXPECT_EQ(posed, (std::vector<uint32_t>{3, 4, 5}));   // exactly T+1..current, ascending

    // every re-simulated step was preceded by the hook call for its own tick
    std::vector<std::string> order;
    for (const auto& call: world.calls)
        if (call == "step" || call.rfind("pre_step ", 0) == 0) order.push_back(call);
    EXPECT_EQ(order, (std::vector<std::string>{"pre_step 3", "step", "pre_step 4", "step", "pre_step 5", "step"}));

    // the hook belongs to the re-simulation alone: a matching snapshot runs no
    // re-simulation and must not call it again
    const size_t calls_before = world.calls.size();
    EXPECT_FALSE(engine.on_snapshot(w, snapshot_of(5, {ball_body(1, world.bodies["Own"]),
                                                       ball_body(2, world.bodies["Remote"])}),
                                    5, entity_of, no_input));
    EXPECT_EQ(posed, (std::vector<uint32_t>{3, 4, 5}));
    EXPECT_EQ(world.calls.size(), calls_before);
}

// The IVP savestate (docs/ivp-savestate-design.md): an anchor whose record
// carries a blob is put back by set_savestate, after the navigation replicas
// (set_nav re-creates the controllers the blob orders) and before anything
// else touches a body - and every body restored from our own record then gets
// no write of its own: no beam (which zeroes what the blob restored), no queue,
// no contact, no clock (the blob carries it).
TEST(RollbackEngine, SavestateRestoredBeforeBodyWritesAndSkipsThem) {
    fake_world world;
    seed_savestate_world(world);
    rollback_engine engine;
    const auto blobs = record_with_blobs(world, engine, 4);
    ASSERT_EQ(blobs.size(), 4u);
    ASSERT_FALSE(blobs.at(2).empty());
    ASSERT_NE(blobs.at(2), blobs.at(4));
    const std::vector<uint8_t> live_at_4 = world.serialize();

    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    ASSERT_EQ(world.count("set_savestate"), 1);
    EXPECT_EQ(world.last_set_blob, blobs.at(2));
    const size_t blob_at = world.first("set_savestate");
    EXPECT_LT(world.first("set_nav Own"), blob_at);
    EXPECT_LT(world.first("set_nav Remote"), blob_at);
    EXPECT_LT(blob_at, world.first("set_body "));   // calls.size() when there is none
    EXPECT_LT(blob_at, world.first("step"));
    EXPECT_EQ(world.count("set_body "), 0);
    EXPECT_EQ(world.count("set_body_extra"), 0);
    EXPECT_EQ(world.count("set_contacts"), 0);
    EXPECT_EQ(world.count("set_clock"), 0);
    EXPECT_EQ(world.count("set_nav"), 2);           // once each, before the blob only
    // no row was written on top, so the anchor's blob is not captured again:
    // one capture per replayed tick
    EXPECT_EQ(world.count("get_savestate"), 2);
    EXPECT_EQ(engine.stats().savestates_restored, 1u);
    EXPECT_EQ(engine.stats().savestate_failures, 0u);
    EXPECT_EQ(engine.stats().savestate_bytes, blobs.at(2).size());
    EXPECT_EQ(engine.stats().clock_rewinds, 1u);    // the blob's clock, counted
    EXPECT_EQ(engine.stats().impulses_restored, 0u);
    EXPECT_EQ(engine.stats().contacts_restored, 0u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);
    // ...and the replay lands on the live world bit for bit, the hidden state
    // and the clock included
    EXPECT_EQ(world.serialize(), live_at_4);
}

// A snapshot row is the server's pose, not ours: it is written on top of the
// savestate with the beam (and its recheck), the other bodies stay as the
// blob put them, and the anchor's blob is captured again afterwards so a later
// rewind to the same tick restores the corrected world rather than the
// prediction the row replaced.
TEST(RollbackEngine, SavestateRowsWrittenOnTop) {
    fake_world world;
    seed_savestate_world(world);
    rollback_engine engine;
    const auto blobs = record_with_blobs(world, engine, 4);

    // the server had the own ball 0.5 m further at tick 2; the snapshot
    // carries its row alone
    fake_body server_own;
    server_own.position[0] = 0.5;
    EXPECT_TRUE(engine.on_snapshot(world.adapter(), snapshot_of(2, {ball_body(1, server_own)}), 4, entity_of,
                                   no_input));
    ASSERT_EQ(world.count("set_savestate"), 1);
    EXPECT_EQ(world.last_set_blob, blobs.at(2));
    EXPECT_EQ(world.count("set_body "), 1);
    EXPECT_EQ(world.count("set_body Own wake recheck"), 1);
    EXPECT_LT(world.first("set_savestate"), world.first("set_body Own"));
    // a row is a hard set (no queue), the others are covered
    EXPECT_EQ(world.count("set_body_extra"), 0);
    // the row keeps today's contact restore; the covered bodies get none
    EXPECT_EQ(world.count("set_contacts Own"), 1);
    EXPECT_EQ(world.count("set_contacts Remote"), 0);
    EXPECT_EQ(world.count("set_contacts Plate"), 0);
    EXPECT_EQ(world.count("set_clock"), 0);
    // captured again after the row and before the replay: once there, then
    // once per replayed tick
    EXPECT_EQ(world.count("get_savestate"), 3);
    const size_t recapture = world.first("get_savestate");
    EXPECT_GT(recapture, world.first("set_body Own"));
    EXPECT_LT(recapture, world.first("step"));
    ASSERT_EQ(world.got_blobs.size(), 3u);
    const std::vector<uint8_t> corrected = world.got_blobs.front();
    EXPECT_NE(corrected, blobs.at(2));
    EXPECT_EQ(engine.stats().savestates_restored, 1u);
    EXPECT_EQ(engine.stats().resim_ticks, 2u);

    // a later rewind to tick 2 puts the corrected world back, with no row and
    // so no body write at all
    world.calls.clear();
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_savestate"), 1);
    EXPECT_EQ(world.last_set_blob, corrected);
    EXPECT_EQ(world.count("set_body "), 0);
    EXPECT_EQ(engine.stats().savestates_restored, 2u);
}

// No blob, no savestate path: a world without the hooks, a world whose blobs
// come back empty (which count as none) and a world that can capture but not
// restore all get the per-body restore, exactly as before.
TEST(RollbackEngine, SavestateFallsBackWithoutBlob) {
    {
        SCOPED_TRACE("no hooks");
        fake_world world;
        seed_savestate_world(world);
        world.savestates = false;
        rollback_engine engine;
        record_with_blobs(world, engine, 4);
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_body "), 3);
        EXPECT_EQ(world.count("set_clock"), 1);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
        EXPECT_EQ(engine.stats().savestate_failures, 0u);
    }
    {
        SCOPED_TRACE("empty blobs");
        fake_world world;
        seed_savestate_world(world);
        world.empty_blobs = true;
        rollback_engine engine;
        record_with_blobs(world, engine, 4);
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_body "), 3);
        EXPECT_GT(world.count("set_body_extra"), 0);
        EXPECT_EQ(world.count("set_contacts"), 3);
        EXPECT_EQ(world.count("set_clock"), 1);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
        EXPECT_EQ(engine.stats().savestate_failures, 0u);
    }
    {
        SCOPED_TRACE("capture only");
        fake_world world;
        seed_savestate_world(world);
        rollback_engine engine;
        record_with_blobs(world, engine, 4);
        auto adapter = world.adapter();
        adapter.set_savestate = nullptr;
        EXPECT_TRUE(engine.rewind_to(adapter, 2, 4, no_input, "test"));
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_body "), 3);
        EXPECT_EQ(world.count("set_clock"), 1);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
    }
}

// set_exact_restore(false) is the per-body restore over the same records: the
// blobs are still captured, never applied - the A/B in one binary.
TEST(RollbackEngine, SavestateWithSwitchOff) {
    fake_world world;
    seed_savestate_world(world);
    rollback_engine engine;
    engine.set_exact_restore(false);
    const auto blobs = record_with_blobs(world, engine, 4);
    ASSERT_FALSE(blobs.at(2).empty());
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_savestate"), 0);
    EXPECT_EQ(world.count("set_body "), 3);
    EXPECT_GT(world.count("set_body_extra"), 0);
    EXPECT_EQ(world.count("set_contacts"), 3);
    EXPECT_EQ(world.count("set_clock"), 1);
    EXPECT_EQ(world.count("set_nav"), 2);
    EXPECT_EQ(world.count("get_savestate"), 2);   // the replayed ticks are still captured
    EXPECT_EQ(engine.stats().savestates_restored, 0u);
    EXPECT_EQ(engine.stats().savestate_failures, 0u);
}

// A body whose friction contacts the blob could not carry (its system had a
// body the world lost) is still covered - no beam, no extra - but gets its
// contacts from our own record, like an uncovered body would.
TEST(RollbackEngine, SavestateDroppedContactsAreRewrittenFromTheRecord) {
    fake_world world;
    seed_savestate_world(world);
    world.drop_contacts_of = {"Own"};
    rollback_engine engine;
    record_with_blobs(world, engine, 4);
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    ASSERT_EQ(world.count("set_savestate"), 1);
    EXPECT_EQ(world.count("set_body "), 0);
    EXPECT_EQ(world.count("set_body_extra"), 0);
    EXPECT_EQ(world.count("set_contacts Own"), 1);
    EXPECT_EQ(world.count("set_contacts Remote"), 0);
    EXPECT_EQ(world.count("set_contacts Plate"), 0);
    EXPECT_LT(world.first("set_savestate"), world.first("set_contacts Own"));
    EXPECT_LT(world.first("set_contacts Own"), world.first("step"));
    EXPECT_EQ(engine.stats().savestates_restored, 1u);
    EXPECT_EQ(engine.stats().contact_restore_failures, 0u);   // the fake's set_contacts makes 0 and fails none
}

// A record whose blob the world could not capture is counted: the stat tells a
// persistent capture failure (a body the capture refuses) apart from a world
// that simply has no savestate hook.
TEST(RollbackEngine, SavestateCaptureFailuresAreCounted) {
    fake_world world;
    seed_savestate_world(world);
    world.empty_blobs = true;
    rollback_engine engine;
    record_with_blobs(world, engine, 4);
    EXPECT_EQ(engine.stats().savestate_capture_failures, 4u);
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_savestate"), 0);   // nothing to restore from
    EXPECT_EQ(engine.stats().savestates_restored, 0u);
    EXPECT_EQ(engine.stats().savestate_failures, 0u);   // a missing blob is not a refused one
}

// A blob the world refuses costs the anchor its savestate, not the rollback:
// the per-body path runs for everything (the clock included, still before any
// body), the failure is counted and logged, and the refused blob is dropped so
// the next rewind to that tick does not try it again.
TEST(RollbackEngine, SavestateRefusedFallsBackToThePerBodyPath) {
    fake_world world;
    seed_savestate_world(world);
    world.refuse_savestate = true;
    rollback_engine engine;
    record_with_blobs(world, engine, 4);
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_savestate"), 1);
    EXPECT_EQ(engine.stats().savestate_failures, 1u);
    EXPECT_EQ(engine.stats().savestates_restored, 0u);
    EXPECT_EQ(world.count("log savestate:"), 1);
    EXPECT_EQ(world.count("set_clock"), 1);
    EXPECT_LT(world.first("set_savestate"), world.first("set_clock"));
    EXPECT_LT(world.first("set_clock"), world.first("set_body "));
    EXPECT_EQ(world.count("set_body "), 3);
    EXPECT_GT(world.count("set_body_extra"), 0);
    EXPECT_EQ(world.count("set_contacts"), 3);
    EXPECT_EQ(world.count("set_nav"), 4);   // before the refused blob, and again on the per-body path
    EXPECT_EQ(engine.stats().resim_ticks, 2u);

    world.calls.clear();
    EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
    EXPECT_EQ(world.count("set_savestate"), 0);
    EXPECT_EQ(engine.stats().savestate_failures, 1u);
    EXPECT_EQ(world.count("set_body "), 3);
}

// The savestate belongs to a restore a replay follows, like the clock and the
// scripts: a frozen clock or a window past max_resim_ticks snaps the bodies
// and drops the history, and never applies a blob.
TEST(RollbackEngine, SavestateGates) {
    {
        SCOPED_TRACE("frozen clock");
        fake_world world;
        seed_savestate_world(world);
        rollback_engine engine;
        record_with_blobs(world, engine, 4);
        world.clock_running = false;
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 4, no_input, "test"));
        EXPECT_EQ(engine.stats().frozen, 1u);
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_clock"), 0);
        EXPECT_GT(world.count("set_body "), 0);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
        EXPECT_EQ(world.steps, 0);
    }
    {
        SCOPED_TRACE("lag beyond max_resim_ticks");
        fake_world world;
        seed_savestate_world(world);
        bmmo::session::rollback_thresholds thresholds;
        thresholds.max_resim_ticks = 3;
        rollback_engine engine(thresholds);
        record_with_blobs(world, engine, 8);
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 2, 8, no_input, "test"));
        EXPECT_EQ(engine.stats().too_far, 1u);
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_clock"), 0);
        EXPECT_GT(world.count("set_body "), 0);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
        EXPECT_EQ(world.steps, 0);
    }
}

// The claim itself: with the savestate, the physics after any number of
// rewinds is bit for bit the physics of the straight run.  Two worlds, the
// same inputs; one is stepped straight through, the other is rewound `depth`
// ticks every `every` ticks and replayed from its records, and the two are
// compared field by field (bit patterns) and blob by blob after every rewind
// and at the end.  The per-body restore over the same records must drift -
// the beam zeroes the hidden state - which is what makes the positive half
// mean something.
TEST(RollbackEngine, ManyRewindsStayBitExact) {
    for (const uint32_t depth: {1u, 2u, 5u, 12u, 40u, 48u})
        for (const uint32_t every: {3u, 7u, 20u}) {
            SCOPED_TRACE("depth " + std::to_string(depth) + ", every " + std::to_string(every));
            const lockstep_outcome exact = run_lockstep(depth, every, true);
            EXPECT_GT(exact.rewinds, 0u);
            EXPECT_EQ(exact.refused, 0u);
            EXPECT_TRUE(exact.exact) << exact.first_difference;
            EXPECT_EQ(exact.savestates_restored, exact.rewinds);
            EXPECT_EQ(exact.savestate_failures, 0u);

            const lockstep_outcome beam = run_lockstep(depth, every, false);
            EXPECT_EQ(beam.rewinds, exact.rewinds);
            EXPECT_EQ(beam.savestates_restored, 0u);
            EXPECT_FALSE(beam.exact) << "the per-body restore stayed bit-exact: the hidden state is not exercised";
        }
}
