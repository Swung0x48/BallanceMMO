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
#include <session/input_relay.hpp>
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

// Late-player design 1.3 (plan step 4): the server substituted the own ball's
// frame at a tick and told its owner, who amends the record of that tick and
// rewinds.  A replay feeds the own ball from the RECORD and falls back on the
// caller's input_at only when the record has none, so the amended record -
// not the caller's own input map, which here still says what the player
// played - has to be what drives the replayed tick.  And the replay
// re-captures every record with what it fed, so a later rewind from before
// that tick feeds the server's frame again.
TEST(RollbackEngine, AnAmendedOwnRecordDrivesTheReplay) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"].position[0] = 5.0;
    world.navs["Own"] = {};
    world.navs["Remote"] = {};
    rollback_engine engine;
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 6, 100, own_inputs);   // the player never holds the key
    ASSERT_FLOAT_EQ(world.bodies["Own"].linear[0], 0.0f);

    EXPECT_EQ(engine.recorded_own_entity(4), "Own");
    EXPECT_EQ(engine.recorded_own_entity(99), "");
    input_frame held{};
    held.keys = 1;
    held.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
    EXPECT_FALSE(engine.amend_recorded_input("Own", 99, held));   // not recorded
    ASSERT_TRUE(engine.amend_recorded_input("Own", 4, held));
    input_frame recorded{};
    ASSERT_TRUE(engine.recorded_input("Own", 4, recorded));
    EXPECT_EQ(recorded.keys, 1u);

    auto w = world.adapter();
    const auto own_input_at = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Own") return false;
        const auto it = own_inputs.find(tick);
        if (it == own_inputs.end()) return false;
        out = it->second;
        return true;
    };
    ASSERT_TRUE(engine.rewind_to(w, 3, 6, own_input_at, "own input substituted at tick 4"));
    // tick 4 ran on the server's frame, ticks 5 and 6 on the player's
    EXPECT_EQ(world.count("nav_input Own keys=1"), 1);
    EXPECT_EQ(world.count("nav_input Own keys=0"), 2);
    EXPECT_EQ(world.count("nav_poll Own off"), 1);
    EXPECT_EQ(world.count("nav_poll Own on"), 1);
    EXPECT_FLOAT_EQ(world.bodies["Own"].linear[0], 1.0f);
    EXPECT_NEAR(world.bodies["Own"].position[0], 3.0 * kDt, 1e-9);
    ASSERT_TRUE(engine.recorded_input("Own", 4, recorded));
    EXPECT_EQ(recorded.keys, 1u);
    ASSERT_TRUE(engine.recorded_input("Own", 5, recorded));
    EXPECT_EQ(recorded.keys, 0u);

    // A later correction anchored before it (a remote's frame of tick 3)
    // replays tick 4 from the re-captured record: the server's frame again.
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 2, 6, own_input_at, "Remote input corrected at tick 3"));
    EXPECT_EQ(world.count("nav_input Own keys=1"), 1);
    EXPECT_EQ(world.count("nav_input Own keys=0"), 3);
    EXPECT_FLOAT_EQ(world.bodies["Own"].linear[0], 1.0f);
    EXPECT_NEAR(world.bodies["Own"].position[0], 3.0 * kDt, 1e-9);
}

// The own entity a record tracked is the name to amend: a trafo renames the
// own ball, and a notice of a tick before it must amend the old name, which
// is the one the replay of that tick feeds.
TEST(RollbackEngine, TheRecordedOwnEntityIsTheNameAtThatTick) {
    fake_world world;
    world.bodies["OwnOld"];
    world.navs["OwnOld"] = {};
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked before;
    before.own_entity = "OwnOld";
    before.own_polls = true;
    rollback_tracked after = before;
    after.own_entity = "OwnNew";
    rollback_tracked dead;
    for (uint32_t tick = 1; tick <= 6; ++tick) {
        if (tick == 4) {
            world.bodies.erase("OwnOld");
            world.navs.erase("OwnOld");
            world.bodies["OwnNew"];
            world.navs["OwnNew"] = {};
        }
        w.step();
        if (tick == 6) engine.record(w, tick, dead, {});
        else engine.record(w, tick, tick >= 4 ? after : before, {{tick >= 4 ? "OwnNew" : "OwnOld", input_frame{}}});
    }
    EXPECT_EQ(engine.recorded_own_entity(2), "OwnOld");
    EXPECT_EQ(engine.recorded_own_entity(5), "OwnNew");
    EXPECT_EQ(engine.recorded_own_entity(6), "");   // no ball at that tick
    input_frame held{};
    held.keys = 1;
    ASSERT_TRUE(engine.amend_recorded_input(engine.recorded_own_entity(2), 2, held));
    input_frame recorded{};
    ASSERT_TRUE(engine.recorded_input("OwnOld", 2, recorded));
    EXPECT_EQ(recorded.keys, 1u);
    EXPECT_FALSE(engine.recorded_input("OwnNew", 2, recorded));
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

// Step RC1 (docs/rewind-cost-design.md §2): the replay's feed over the core's
// rule (recorded_input_at = relay_frame_or_newest over the mirror's ring).
// The live run predicted keys 9 for every tick; the relay has 3 (keys 2), 5
// (keys 4) and 6 (keys 8).  A rewind anchored at 1 feeds tick 2 - before the
// first relayed frame - the recorded prediction (the old behaviour), 3 its
// frame, the hole 4 the nearest earlier (keys 2), 5 and 6 their frames, and
// the tail 7..10 the newest relayed frame (hold-last), not the stale
// prediction; the re-captured records hold what was fed, so a later arrival
// equal to the newest frame finds it recorded (no news).
TEST(RollbackEngine, TheTailPastARemotesNewestRelayedFrameIsFedThatFrameAndRecordedSo) {
    fake_world world;
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked tracked;
    tracked.remote_entities = {"Remote"};
    input_frame predicted{};
    predicted.keys = 9;
    for (uint32_t tick = 1; tick <= 10; ++tick) {
        world.pending_keys["Remote"] = predicted.keys;
        w.step();
        engine.record(w, tick, tracked, {{"Remote", predicted}});
    }
    std::map<uint32_t, input_frame> ring;
    const auto relayed = [&ring](uint32_t tick, uint8_t keys) {
        input_frame f{};
        f.keys = keys;
        f.flags = bmmo::session::INPUT_FLAG_PHYSICALIZED | bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        ring[tick] = f;
    };
    relayed(3, 2);
    relayed(5, 4);
    relayed(6, 8);
    const auto input_at = [&ring](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Remote") return false;
        const input_frame* frame = bmmo::session::relay_frame_or_newest(ring, tick);
        if (!frame) return false;
        out = *frame;
        return true;
    };
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 1, 10, input_at, "Remote input corrected at tick 2"));
    const std::map<uint32_t, uint8_t> fed = {{2, 9}, {3, 2}, {4, 2}, {5, 4}, {6, 8}, {7, 8}, {8, 8}, {9, 8}, {10, 8}};
    for (const auto& [tick, keys]: fed) {
        input_frame recorded{};
        ASSERT_TRUE(engine.recorded_input("Remote", tick, recorded)) << "tick " << tick;
        EXPECT_EQ(recorded.keys, keys) << "tick " << tick;
    }
    EXPECT_EQ(world.count("nav_input Remote keys=8"), 5) << "6 and the tail 7..10";
    EXPECT_EQ(world.count("nav_input Remote keys=9"), 1) << "only tick 2, before the first relayed frame";
    // The next frame arrives and repeats the newest: the record already has it.
    input_frame recorded{};
    ASSERT_TRUE(engine.recorded_input("Remote", 7, recorded));
    EXPECT_EQ(recorded.keys, ring.at(6).keys);
    EXPECT_EQ(recorded.flags, ring.at(6).flags);

    // A remote born inside the replay (not named by the records): fed from its
    // stamp, the tail past its newest relayed frame with that frame.
    rollback_tracked none;
    none.own_entity = "Own";
    fake_world born_world;
    born_world.bodies["Own"];
    rollback_engine born_engine;
    auto bw = born_world.adapter();
    for (uint32_t tick = 1; tick <= 10; ++tick) {
        bw.step();
        born_engine.record(bw, tick, none, {{"Own", input_frame{}}});
    }
    constexpr uint32_t kBorn = 4;
    bool born = false;
    bw.pre_step = [&](uint32_t tick) {
        if (tick == kBorn && !born) {
            born = true;
            born_world.bodies["Newborn"];
        }
    };
    bw.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) {
        if (born && tick >= kBorn) out["Newborn"] = 1;
    };
    std::map<uint32_t, input_frame> newborn_ring;
    input_frame first{};
    first.keys = 3;
    newborn_ring[kBorn] = first;
    input_frame newest{};
    newest.keys = 5;
    newborn_ring[kBorn + 1] = newest;
    const auto newborn_at = [&newborn_ring](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Newborn") return false;
        const input_frame* frame = bmmo::session::relay_frame_or_newest(newborn_ring, tick);
        if (!frame) return false;
        out = *frame;
        return true;
    };
    ASSERT_TRUE(born_engine.rewind_to(bw, kBorn - 1, 10, newborn_at, "ball of 2 born at tick 4"));
    ASSERT_TRUE(born_engine.recorded_input("Newborn", kBorn, recorded));
    EXPECT_EQ(recorded.keys, 3u);
    for (uint32_t tick = kBorn + 1; tick <= 10; ++tick) {
        ASSERT_TRUE(born_engine.recorded_input("Newborn", tick, recorded)) << "tick " << tick;
        EXPECT_EQ(recorded.keys, 5u) << "tick " << tick;
    }
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

// A snapshot deeper than the engine re-simulates is refused: nothing is
// written, the history stays, and the next snapshot of a tick the history
// holds is matched as usual.  (It used to write every body to the rows
// without a replay and drop the history.)
TEST(RollbackEngine, LagBeyondLimitRefusesTheSnapshotAndKeepsTheHistory) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    bmmo::session::rollback_thresholds thresholds;
    thresholds.max_resim_ticks = 3;
    thresholds.history_ticks = 96;
    rollback_engine engine(thresholds);
    std::map<uint32_t, input_frame> own_inputs;
    run_ticks(world, engine, 8, 100, own_inputs);
    const size_t history = engine.history_size();
    ASSERT_GT(history, 1u);
    const double own_before = world.bodies["Own"].position[2];

    fake_body server_own;
    server_own.position[2] = 1.0;
    const auto snapshot = snapshot_of(2, {ball_body(1, server_own), ball_body(2, world.bodies["Remote"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), snapshot, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().too_far, 1u);
    EXPECT_EQ(engine.stats().mismatched, 1u);
    EXPECT_EQ(engine.stats().rollbacks, 0u);
    EXPECT_EQ(world.steps, 0);
    EXPECT_EQ(world.count("set_body "), 0);
    EXPECT_NEAR(world.bodies["Own"].position[2], own_before, 1e-9);
    EXPECT_EQ(engine.history_size(), history);
    // the history is intact: a snapshot of the current tick is compared, not unmatched
    const auto later = snapshot_of(8, {ball_body(1, world.bodies["Own"])});
    EXPECT_FALSE(engine.on_snapshot(world.adapter(), later, 8, entity_of, no_input));
    EXPECT_EQ(engine.stats().unmatched, 0u);
    EXPECT_EQ(engine.stats().matched, 1u);
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
    w.destroy_body = [&](const std::string& entity, uint32_t) {
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

// Step P of the session-core design: with revive_body installed and the
// savestate applied, a mechanism the anchor's record has that the window
// destroyed (a replayed script's De Physicalize: the trafo pieces' reset) is
// made again at the restore - before the blob, which then puts its state of
// T back - so the replayed ticks before its death have it, and the replayed
// script takes it away again where the live one did.  A refused blob takes
// the made body out again.
TEST(RollbackEngine, AMechanismTheWindowDestroyedIsMadeAgainAtTheRestoreForTheReplay) {
    for (const bool refuse: {false, true}) {
        SCOPED_TRACE(refuse ? "blob refused" : "blob applied");
        fake_world world;
        world.savestates = true;
        world.bodies["Remote"].position[0] = 5.0;
        world.navs["Remote"] = {};
        world.bodies["Prop"].position[0] = 10.0;
        world.bodies["Prop"].linear[0] = 3.0f;
        rollback_engine engine;
        auto w = world.adapter();
        // The script that destroys the prop at the start of tick 4, live and
        // replayed alike; what the replay finds of it just before.
        double replayed_before_death = -1.0;
        bool replaying = false;
        w.pre_step = [&](uint32_t tick) {
            if (tick != 4) return;
            if (replaying && world.bodies.contains("Prop")) replayed_before_death = world.bodies["Prop"].position[0];
            world.bodies.erase("Prop");
        };
        w.revive_body = [&](const std::string& entity, uint32_t anchor) {
            world.calls.push_back("revive " + entity + "@" + std::to_string(anchor));
            world.bodies[entity] = fake_body{};   // made from its birth: the blob puts its state back
            return true;
        };
        w.destroy_body = [&](const std::string& entity, uint32_t tick) {
            world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
            return world.bodies.erase(entity) > 0;
        };
        world.refuse_savestate = refuse;

        rollback_tracked alive;
        alive.remote_entities = {"Remote"};
        alive.mechanisms = {"Prop"};
        alive.generations = {{"Prop", 1}};
        rollback_tracked dead = alive;
        dead.mechanisms.clear();
        dead.generations["Prop"] = 2;   // the delete moved the lifetime on
        double live_before_death = 0.0;
        for (uint32_t tick = 1; tick <= 6; ++tick) {
            if (tick == 4) live_before_death = world.bodies["Prop"].position[0];
            w.pre_step(tick);
            w.step();
            engine.record(w, tick, tick >= 4 ? dead : alive, {{"Remote", input_frame{}}});
        }
        world.calls.clear();
        ASSERT_FALSE(world.bodies.contains("Prop"));

        fake_body server_remote = world.bodies["Remote"];
        server_remote.position[0] += 1.0;
        const auto snapshot = snapshot_of(2, {ball_body(2, server_remote)});
        auto entity_now = [](const body_state& body) -> std::string {
            return body.kind == body_kind::Ball && body.owner == 2 ? "Remote" : "";
        };
        replaying = true;
        EXPECT_TRUE(engine.on_snapshot(w, snapshot, 6, entity_now, no_input));
        ASSERT_EQ(world.count("revive Prop@2"), 1);
        EXPECT_LT(world.first("revive Prop"), world.first("set_savestate")) << "made before the blob goes back";
        EXPECT_FALSE(world.bodies.contains("Prop")) << "taken away again by the replayed script (or the refusal)";
        if (!refuse) {
            EXPECT_EQ(world.count("destroy Prop"), 0);
            EXPECT_EQ(replayed_before_death, live_before_death) << "the blob's state of tick 2, replayed to the death";
        } else {
            EXPECT_EQ(world.count("destroy Prop@0"), 1) << "the per-body path has no record of that lifetime";
        }
    }
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

// Plan doc 10: with destroy_body alone, the replayed scripts are expected to
// make the body again by its birth tick.  A fake step that does so leaves the
// body on its record with nothing written; one that does not is counted.
TEST(RollbackEngine, ABodyDestroyedAtTheRestoreIsMadeAgainByTheReplayedStepOrCounted) {
    for (const bool scripts_make_it: {true, false}) {
        fake_world world;
        world.savestates = true;
        world.bodies["Remote"].position[0] = 5.0;
        world.navs["Remote"] = {};
        rollback_engine engine;
        auto w = world.adapter();
        fake_body born;
        born.position[0] = 10.0;
        born.linear[0] = 3.0f;
        w.destroy_body = [&](const std::string& entity, uint32_t tick) {
            world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
            return world.bodies.erase(entity) > 0;
        };
        // The replayed scripts' Physicalize, as the pre_step of the birth tick.
        if (scripts_make_it)
            w.pre_step = [&](uint32_t tick) { if (tick == 4) world.bodies["Prop"] = born; };
        rollback_tracked before;
        before.remote_entities = {"Remote"};
        rollback_tracked after = before;
        after.mechanisms = {"Prop"};
        for (uint32_t tick = 1; tick <= 6; ++tick) {
            if (tick == 4) world.bodies["Prop"] = born;
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
        EXPECT_EQ(world.count("destroy Prop@4"), 1);
        EXPECT_EQ(engine.stats().births_recreated, 1u);
        if (scripts_make_it) {
            EXPECT_EQ(engine.stats().births_recreated_exact, 1u);
            EXPECT_EQ(engine.stats().births_missing, 0u);
            EXPECT_EQ(world.count("set_body Prop"), 0);
            EXPECT_NEAR(world.bodies["Prop"].position[0], born_at + 2.0 * 3.0 * kDt, 1e-9);
        } else {
            EXPECT_EQ(engine.stats().births_missing, 1u);
            EXPECT_FALSE(world.bodies.contains("Prop"));
        }
    }
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
    w.destroy_body = [&](const std::string& entity, uint32_t) {
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

// The own beam of the session client (apply_own_beam): the own ball rebuilt at
// a new pose before the step of its stamp, live and by the pre_step hook in
// every replay that crosses the stamp, with no lifetime change.  A rewind
// anchored before the stamp must restore the pre-beam record, replay to the
// stamp on the old path, beam again there and arrive where the live run did;
// the records from the stamp on are re-captured on the beamed path, so a later
// rewind anchored after the stamp restores that path.  Without the hook the
// same rewind undoes the beam for good (the measured 540 m on three clients):
// the control at the end pins that failure.  Both with and without the
// savestate (the blob of an anchor before the stamp is applied over the
// rebuilt body, as the IVP savestate maps a body by name).
TEST(RollbackEngine, PreStepBeamOfTheOwnBallIsReappliedByEveryReplayThatCrossesIt) {
    for (const bool savestates: {false, true}) {
        SCOPED_TRACE(savestates ? "savestate" : "per-body");
        constexpr uint32_t kStamp = 6, kNow = 10;
        fake_world world;
        world.savestates = savestates;
        world.bodies["Own"].linear[0] = 1.0f;
        world.bodies["Remote"].position[0] = 5.0;
        world.navs["Own"] = {};
        rollback_engine engine;
        rollback_tracked tracked;
        tracked.own_entity = "Own";
        tracked.own_polls = true;
        tracked.remote_entities = {"Remote"};
        // A fresh body at the beam pose: nothing of the old one survives.
        const auto beam = [&world](uint32_t tick) {
            if (tick != kStamp) return;
            fake_body fresh;
            fresh.position[0] = 100.0;
            fresh.linear[0] = 2.0f;
            world.bodies["Own"] = fresh;
        };
        auto w = world.adapter();
        for (uint32_t tick = 1; tick <= kNow; ++tick) {
            beam(tick);   // the live frame of the stamp, before its step
            w.step();
            engine.record(w, tick, tracked, {});
        }
        const fake_body live = world.bodies["Own"];
        ASSERT_NEAR(live.position[0], 100.0 + 5 * 2.0 * kDt, 1e-9);

        w.pre_step = beam;
        ASSERT_TRUE(engine.rewind_to(w, 3, kNow, no_input, "test"));
        EXPECT_EQ(world.bodies["Own"].position[0], live.position[0]);
        EXPECT_EQ(world.bodies["Own"].linear[0], live.linear[0]);
        EXPECT_EQ(engine.stats().resim_ticks, kNow - 3);

        // Anchored after the stamp: the re-captured records are on the beamed
        // path, and nothing re-beams (the hook does nothing at other ticks).
        ASSERT_TRUE(engine.rewind_to(w, 7, kNow, no_input, "test"));
        EXPECT_EQ(world.bodies["Own"].position[0], live.position[0]);

        // The control: the same rewind with no hook re-steps the old path from
        // tick 3 and the beam is gone.
        w.pre_step = nullptr;
        ASSERT_TRUE(engine.rewind_to(w, 3, kNow, no_input, "test"));
        EXPECT_LT(world.bodies["Own"].position[0], 1.0);
    }
}

// A remote ball born late: its Physicalize (stamped tick 5) reaches the client
// at tick 10, and the rewind to tick 4 makes the body inside the replay, at
// its stamp (pre_step).  The records of 5..10 were captured while the player
// was dead and do not name it.  It must still be fed its relayed input from
// its birth tick on (rollback_world::replay_remotes), and the re-captured
// records must carry it - with the generation it has now - so that a later
// rewind anchored inside that window restores it from its own record, and
// one anchored before it re-poses it at its birth tick.  The old replay fed it
// nothing (it coasted to tick 10) and recorded it first at the live tick 11,
// where every later rewind parked it.
TEST(RollbackEngine, ARemoteBornInsideTheReplayIsDrivenAndRecordedFromItsBirthTick) {
    fake_world world;
    world.bodies["Own"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked dead;
    dead.own_entity = "Own";
    dead.generations = {{"Own", 1}, {"Remote", 1}};
    for (uint32_t tick = 1; tick <= 10; ++tick) {
        w.step();
        engine.record(w, tick, dead, {{"Own", input_frame{}}});
    }
    world.calls.clear();

    constexpr uint32_t kBorn = 5;
    bool born = false;
    uint32_t generation = 1;
    std::map<uint32_t, double> remote_x_before;   // the remote's x before each replayed tick's step
    w.pre_step = [&](uint32_t tick) {
        if (tick == kBorn && !born) {
            born = true;
            ++generation;   // the caller's new lifetime for the name
            world.bodies["Remote"].position[0] = 50.0;
        }
        if (const auto it = world.bodies.find("Remote"); it != world.bodies.end())
            remote_x_before[tick] = it->second.position[0];
    };
    w.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) {
        if (born && tick >= kBorn) out["Remote"] = generation;
    };
    input_frame held{};
    held.keys = 1;
    held.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
    // The relay holds the owner's frames from the stamp on: key 0 held.
    const auto relayed = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Remote" || tick < kBorn) return false;
        out = held;
        return true;
    };

    ASSERT_TRUE(engine.rewind_to(w, kBorn - 1, 10, relayed, "ball of 2 born at tick 5"));
    // driven from its birth tick: six steps with the key held
    EXPECT_EQ(world.count("nav_input Remote keys=1"), 6);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], 6.0f);
    // and the re-captured records carry its input from that tick
    input_frame recorded{};
    EXPECT_FALSE(engine.recorded_input("Remote", kBorn - 1, recorded));
    for (uint32_t tick = kBorn; tick <= 10; ++tick) {
        EXPECT_TRUE(engine.recorded_input("Remote", tick, recorded)) << "tick " << tick;
        EXPECT_EQ(recorded.keys, 1u) << "tick " << tick;
        EXPECT_TRUE(engine.recorded_body("Remote", tick)) << "tick " << tick;
    }
    EXPECT_FALSE(engine.recorded_body("Remote", kBorn - 1));
    const double x_after_birth_tick = remote_x_before.at(kBorn + 1);
    // The diagnostic reader (R3): the birth tick's record is the state its
    // replayed step left.
    bmmo_physics_body_state at_birth{};
    ASSERT_TRUE(engine.recorded_body_state("Remote", kBorn, at_birth));
    EXPECT_DOUBLE_EQ(at_birth.position[0], x_after_birth_tick);
    EXPECT_FALSE(engine.recorded_body_state("Remote", kBorn - 1, at_birth));

    // The live tick 11 tracks it, under the generation it has now.
    rollback_tracked alive = dead;
    alive.remote_entities = {"Remote"};
    alive.generations["Remote"] = generation;
    world.pending_keys["Remote"] = 1;
    w.step();
    engine.record(w, 11, alive, {{"Own", input_frame{}}, {"Remote", held}});
    const fake_body live = world.bodies["Remote"];

    // A correction anchored inside the birth window restores the newborn from
    // its own record of that tick: one write, no parking and re-pose.
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 7, 11, relayed, "input corrected at tick 8"));
    EXPECT_EQ(world.count("set_body Remote"), 1);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], live.position[0]);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], live.linear[0]);

    // One anchored before the birth finds its birth record at the stamp: it is
    // parked there and re-posed after that tick's step, not at tick 11.
    world.calls.clear();
    remote_x_before.clear();
    ASSERT_TRUE(engine.rewind_to(w, kBorn - 1, 11, relayed, "input corrected at tick 5"));
    EXPECT_EQ(world.count("set_body Remote"), 2);
    EXPECT_DOUBLE_EQ(remote_x_before.at(kBorn + 1), x_after_birth_tick);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], live.position[0]);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], live.linear[0]);
}

// A remote whose history has its birth later in the window than the caller
// claims (a Physicalize made live at tick 10, first recorded at 11, while
// replay_remotes names it from its stamp 5): parked until its birth record, so
// the replay neither drives nor records it before tick 11.  Recorded, the
// parked pose would be restored by the next rewind as an ordinary body and
// released ticks early.
TEST(RollbackEngine, ARemoteParkedUntilItsBirthRecordIsNeitherDrivenNorRecordedBeforeIt) {
    fake_world world;
    world.bodies["Own"];
    rollback_engine engine;
    auto w = world.adapter();
    rollback_tracked dead;
    dead.own_entity = "Own";
    dead.generations = {{"Own", 1}, {"Remote", 1}};
    for (uint32_t tick = 1; tick <= 10; ++tick) {
        w.step();
        engine.record(w, tick, dead, {{"Own", input_frame{}}});
    }
    input_frame held{};
    held.keys = 1;
    held.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
    // made live before the step of tick 11: its first record is 11
    world.bodies["Remote"].position[0] = 50.0;
    rollback_tracked alive = dead;
    alive.remote_entities = {"Remote"};
    alive.generations["Remote"] = 2;
    world.pending_keys["Remote"] = 1;
    w.step();
    engine.record(w, 11, alive, {{"Own", input_frame{}}, {"Remote", held}});

    w.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) {
        if (tick >= 5) out["Remote"] = 2;   // the stamp, not the first stepped tick
    };
    const auto relayed = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Remote" || tick < 5) return false;
        out = held;
        return true;
    };
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 5, 11, relayed, "input corrected at tick 6"));
    EXPECT_EQ(world.count("nav_input Remote keys=1"), 1);   // tick 11 only
    for (uint32_t tick = 6; tick <= 10; ++tick)
        EXPECT_FALSE(engine.recorded_body("Remote", tick)) << "tick " << tick;
    EXPECT_TRUE(engine.recorded_body("Remote", 11));
}

// A same-name death and birth at one stamp (the beam): the records of the
// window name the remote under the lifetime that ended, so the replay feeds it
// before pre_step - on navigation the new body has not got yet.  The body
// made by pre_step is fed again, and the records from its birth on carry its
// generation: a later rewind inside the window restores it from its own
// record instead of parking it at the live tick.
TEST(RollbackEngine, ARemoteRebornUnderTheSameNameIsFedAfterItsBirthAndRelabelled) {
    fake_world world;
    world.bodies["Own"];
    world.bodies["Remote"];
    rollback_engine engine;
    auto w = world.adapter();
    // the navigation exists only while a body does
    const auto base_input = w.nav_input;
    w.nav_input = [&](const std::string& entity, const input_frame& frame) {
        if (entity == "Remote" && world.bodies.find("Remote") == world.bodies.end()) {
            world.calls.push_back("nav_input " + entity + " refused");
            return false;
        }
        return base_input(entity, frame);
    };
    input_frame held{};
    held.keys = 1;
    held.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
    rollback_tracked old_life;
    old_life.own_entity = "Own";
    old_life.remote_entities = {"Remote"};
    old_life.generations = {{"Own", 1}, {"Remote", 1}};
    for (uint32_t tick = 1; tick <= 10; ++tick) {
        w.step();
        engine.record(w, tick, old_life, {{"Own", input_frame{}}, {"Remote", input_frame{}}});
    }
    // The late Unphysicalize: the old body is gone (its own rewind is not
    // what this test is about).
    world.bodies.erase("Remote");

    constexpr uint32_t kBorn = 5;
    bool born = false;
    w.pre_step = [&](uint32_t tick) {
        if (tick == kBorn && !born) {
            born = true;
            world.bodies["Remote"].position[0] = 50.0;
        }
    };
    w.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) {
        if (born && tick >= kBorn) out["Remote"] = 3;
    };
    const auto relayed = [&](const std::string& entity, uint32_t tick, input_frame& out) {
        if (entity != "Remote" || tick < kBorn) return false;
        out = held;
        return true;
    };
    ASSERT_TRUE(engine.rewind_to(w, kBorn - 1, 10, relayed, "ball of 2 born at tick 5"));
    // driven at its birth tick too: six steps with the key held
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], 6.0f);

    rollback_tracked new_life = old_life;
    new_life.generations["Remote"] = 3;
    world.pending_keys["Remote"] = 1;
    w.step();
    engine.record(w, 11, new_life, {{"Own", input_frame{}}, {"Remote", held}});
    const fake_body live = world.bodies["Remote"];
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 7, 11, relayed, "input corrected at tick 8"));
    EXPECT_EQ(world.count("set_body Remote"), 1);
    EXPECT_DOUBLE_EQ(world.bodies["Remote"].position[0], live.position[0]);
    EXPECT_FLOAT_EQ(world.bodies["Remote"].linear[0], live.linear[0]);
}

// Step LB of the session-core design: a mirror born inside the window that
// the caller takes out at the restore (destroy_body) and makes again at its
// birth tick from the inputs (birth_from_inputs) is fed after it is made -
// its navigation is new, and the feed before pre_step found none - and is
// left as that step made it, whatever its birth record says: the record was
// made with the inputs known then (a late birth's first replay, before the
// relay had reached its stamp).  The control, the same re-creation without
// the flag, keeps the birth tick undriven and its record's state.
TEST(RollbackEngine, AMirrorMadeAgainFromTheInputsIsFedAtItsBirthTickAndKeptAsStepped) {
    for (const bool from_inputs: {true, false}) {
        SCOPED_TRACE(from_inputs ? "birth_from_inputs" : "control");
        fake_world world;
        world.savestates = true;
        world.bodies["Own"];
        rollback_engine engine;
        auto w = world.adapter();
        // the navigation exists only while the body does
        const auto base_input = w.nav_input;
        w.nav_input = [&](const std::string& entity, const input_frame& frame) {
            if (entity == "Mirror" && !world.bodies.contains("Mirror")) {
                world.calls.push_back("nav_input " + entity + " refused");
                return false;
            }
            return base_input(entity, frame);
        };
        input_frame held{};
        held.keys = 1;
        held.flags = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        constexpr uint32_t kBorn = 4;
        fake_body born;
        born.position[0] = 10.0;
        rollback_tracked before;
        before.own_entity = "Own";
        before.generations = {{"Own", 1}};
        rollback_tracked after = before;
        after.remote_entities = {"Mirror"};
        after.generations["Mirror"] = 2;
        // Made at its stamp with no frame of its own yet: idle through 4..6.
        for (uint32_t tick = 1; tick <= 6; ++tick) {
            if (tick == kBorn) world.bodies["Mirror"] = born;
            w.step();
            std::map<std::string, input_frame> inputs{{"Own", input_frame{}}};
            if (tick >= kBorn) inputs["Mirror"] = input_frame{};
            engine.record(w, tick, tick >= kBorn ? after : before, inputs);
        }
        ASSERT_FLOAT_EQ(world.bodies["Mirror"].linear[0], 0.0f);

        w.destroy_body = [&](const std::string& entity, uint32_t tick) {
            world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
            return entity == "Mirror" && world.bodies.erase(entity) > 0;
        };
        if (from_inputs) w.birth_from_inputs = [](const std::string& entity) { return entity == "Mirror"; };
        // The caller's births: made again before its birth tick's step, with
        // a new navigation (nothing pending on it).
        w.pre_step = [&](uint32_t tick) {
            if (tick != kBorn || world.bodies.contains("Mirror")) return;
            world.calls.push_back("made Mirror@" + std::to_string(tick));
            world.bodies["Mirror"] = born;
            world.pending_keys.erase("Mirror");
        };
        w.replay_remotes = [&](uint32_t tick, std::map<std::string, uint32_t>& out) {
            if (tick >= kBorn && world.bodies.contains("Mirror")) out["Mirror"] = 2;
        };
        // The relay has the frames now: the key held from the stamp on.
        const auto relayed = [&](const std::string& entity, uint32_t tick, input_frame& out) {
            if (entity != "Mirror" || tick < kBorn) return false;
            out = held;
            return true;
        };
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, kBorn - 1, 6, relayed, "Mirror input corrected at tick 4"));
        EXPECT_EQ(world.count("destroy Mirror@4"), 1);
        EXPECT_EQ(world.count("made Mirror@4"), 1);
        EXPECT_EQ(world.count("set_body Mirror"), 0) << "found on its record or kept: no write either way";
        EXPECT_EQ(engine.stats().births_recreated, 1u);
        if (from_inputs) {
            EXPECT_EQ(world.count("nav_input Mirror keys=1"), 3) << "fed at its birth tick, after it was made";
            EXPECT_FLOAT_EQ(world.bodies["Mirror"].linear[0], 3.0f) << "driven at 4, 5 and 6";
            EXPECT_NEAR(world.bodies["Mirror"].position[0], 10.0 + 6.0 * kDt, 1e-6);
            EXPECT_EQ(engine.stats().births_recreated_exact, 0u) << "not on the record of the earlier inputs";
            EXPECT_EQ(engine.stats().birth_recreate_mismatches, 0u) << "a difference that is the inputs, not a gap";
            EXPECT_EQ(world.count("log birth: Mirror made again at tick 4 from the inputs it has now"), 1);
        } else {
            EXPECT_EQ(world.count("nav_input Mirror keys=1"), 2) << "the birth tick's frame went nowhere";
            EXPECT_FLOAT_EQ(world.bodies["Mirror"].linear[0], 2.0f);
            EXPECT_EQ(engine.stats().births_recreated_exact, 1u) << "on its record: undriven at 4";
        }
    }
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

// Step RR of the session-core design: a snapshot row written at tick t is the
// server's state after t's step, so a later rewind anchored BEFORE t writes it
// again after replaying t - also once that replay has re-recorded t, for the
// next rewind - and arrives at the corrected world bit for bit.  With
// rollback_world::replay_rows off (the legacy retail path) the replay puts the
// prediction back, as before.
TEST(RollbackEngine, ASnapshotRowIsWrittenAgainByEveryLaterReplayThatCrossesItsTick) {
    for (const bool replay_rows: {true, false}) {
        SCOPED_TRACE(replay_rows ? "replay_rows on" : "replay_rows off");
        fake_world world;
        seed_savestate_world(world);
        rollback_engine engine;
        record_with_blobs(world, engine, 8);
        auto w = world.adapter();
        w.replay_rows = replay_rows;
        // the server had the own ball 0.5 m further at tick 5
        fake_body server_own;
        server_own.position[0] = 0.5;
        ASSERT_TRUE(engine.on_snapshot(w, snapshot_of(5, {ball_body(1, server_own)}), 8, entity_of, no_input));
        const fake_world corrected = world;

        // anchored before the row's tick: 4..8 replayed, the row written
        // again after the step of 5 (and nowhere else)
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 3, 8, no_input, "test"));
        EXPECT_EQ(world.count("set_body Own"), replay_rows ? 1 : 0);
        if (replay_rows) {
            EXPECT_EQ(world.count("set_contacts Own"), 1);
            EXPECT_GT(world.first("set_body Own"), world.first("step"));
            EXPECT_EQ(bit_difference(corrected, world), "");
        } else {
            EXPECT_NE(bit_difference(corrected, world), "");
        }

        // the replay re-recorded 5 and kept its row: a second rewind across it
        // arrives at the same world
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 4, 8, no_input, "test"));
        EXPECT_EQ(world.count("set_body Own"), replay_rows ? 1 : 0);
        if (replay_rows) EXPECT_EQ(bit_difference(corrected, world), "");

        // anchored at the row's tick: the blob carries it, no write
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 5, 8, no_input, "test"));
        EXPECT_EQ(world.count("set_body "), 0);
        if (replay_rows) EXPECT_EQ(bit_difference(corrected, world), "");
    }
}

// Step RR: a row whose body the replayed world no longer has at its tick (a
// late birth replaced the player's mirror inside the window) is written on the
// body rollback_world::row_entity names for the server's identity there - its
// pose, not the contacts the restore re-created for the other body - and on
// none when the hook answers "".
TEST(RollbackEngine, AReplayedRowFollowsTheServerBodyToTheBodyItHasAtTheTick) {
    for (const bool answer: {true, false}) {
        SCOPED_TRACE(answer ? "row_entity names Remote2" : "row_entity answers nothing");
        fake_world world;
        seed_savestate_world(world);
        rollback_engine engine;
        record_with_blobs(world, engine, 8);
        auto w = world.adapter();
        w.replay_rows = true;
        std::vector<uint32_t> asked;
        w.row_entity = [&](body_kind kind, uint32_t owner, uint32_t tick) -> std::string {
            EXPECT_EQ(kind, body_kind::Ball);
            EXPECT_EQ(owner, 2u);
            asked.push_back(tick);
            return answer ? "Remote2" : "";
        };
        fake_body server_remote;
        server_remote.position[0] = 7.0;
        ASSERT_TRUE(engine.on_snapshot(w, snapshot_of(5, {ball_body(2, server_remote)}), 8, entity_of, no_input));
        // the mirror the row went on is gone; the player's new one is Remote2
        world.bodies["Remote2"] = world.bodies["Remote"];
        world.bodies["Remote2"].position[0] = 1.0;
        world.bodies.erase("Remote");
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 3, 8, no_input, "test"));
        EXPECT_EQ(asked, std::vector<uint32_t>{5});
        EXPECT_EQ(world.count("set_body Remote2"), answer ? 1 : 0);
        EXPECT_EQ(world.count("set_contacts Remote2"), 0) << "the contacts were another body's";
        if (answer) {
            EXPECT_GT(world.first("set_body Remote2"), world.first("step"));
            EXPECT_EQ(world.bodies["Remote2"].position[0], 7.0);
        } else {
            EXPECT_EQ(world.bodies["Remote2"].position[0], 1.0);
        }
    }
}

// Step RR: two snapshots of one tick that put the same server body's row on two
// local bodies (a new mirror between them) leave one row: the newer.
TEST(RollbackEngine, TheNewerRowOfAServerBodyAtATickWins) {
    fake_world world;
    world.savestates = true;
    world.bodies["Own"];
    world.bodies["RemoteA"].position[0] = 5.0;
    world.bodies["RemoteB"].position[0] = 10.0;
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.remote_entities = {"RemoteA", "RemoteB"};
    record_ticks(world, engine, tracked, 1, 8);
    auto w = world.adapter();
    w.replay_rows = true;
    fake_body first, second;
    first.position[0] = 7.0;
    second.position[0] = 12.0;
    const auto as_a = [](const body_state& body) { return body.owner == 2 ? std::string("RemoteA") : std::string(); };
    const auto as_b = [](const body_state& body) { return body.owner == 2 ? std::string("RemoteB") : std::string(); };
    ASSERT_TRUE(engine.on_snapshot(w, snapshot_of(5, {ball_body(2, first)}), 8, as_a, no_input));
    ASSERT_TRUE(engine.on_snapshot(w, snapshot_of(5, {ball_body(2, second)}), 8, as_b, no_input));
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 3, 8, no_input, "test"));
    EXPECT_EQ(world.count("set_body RemoteA"), 0) << "the older row is gone";
    EXPECT_EQ(world.count("set_body RemoteB"), 1);
    EXPECT_EQ(world.bodies["RemoteB"].position[0], 12.0);
}

// Step RR: an own birth the server applied later (move_birth) keeps the rows of
// the ticks from the applied one on - the server's state of the body made again
// there - and a replay writes them again.
TEST(RollbackEngine, AMovedBirthKeepsTheRowsFromItsAppliedTick) {
    fake_world world;
    world.savestates = true;
    world.bodies["Own"];
    rollback_engine engine;
    rollback_tracked tracked;
    tracked.own_entity = "Own";
    tracked.generations["Own"] = 1;
    record_ticks(world, engine, tracked, 1, 10);
    auto w = world.adapter();
    w.replay_rows = true;
    fake_body server_own;
    server_own.position[0] = 50.0;
    ASSERT_TRUE(engine.on_snapshot(w, snapshot_of(8, {ball_body(1, server_own)}), 10, entity_of, no_input));
    EXPECT_GT(engine.move_birth("Own", 1, 4, 7, true), 0u);
    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, 3, 10, no_input, "test"));
    EXPECT_EQ(world.count("set_body Own"), 1);
    EXPECT_EQ(world.bodies["Own"].position[0], 50.0);
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
// scripts: a frozen clock snaps the bodies and drops the history and never
// applies a blob; a caller's rewind deeper than max_resim_ticks is refused
// outright (counted as too far, nothing written) - without a snapshot's rows
// a restore that deep is never right, and it used to write every tracked body
// back to a record up to the whole history old.
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
        EXPECT_FALSE(engine.rewind_to(world.adapter(), 2, 8, no_input, "test"));
        EXPECT_EQ(engine.stats().too_far, 1u);
        EXPECT_EQ(engine.stats().rewinds, 0u);
        EXPECT_EQ(world.count("set_savestate"), 0);
        EXPECT_EQ(world.count("set_clock"), 0);
        EXPECT_EQ(world.count("set_body "), 0);
        EXPECT_EQ(engine.stats().savestates_restored, 0u);
        EXPECT_EQ(world.steps, 0);
        // At the bound itself the rewind runs.
        EXPECT_TRUE(engine.rewind_to(world.adapter(), 5, 8, no_input, "test"));
        EXPECT_EQ(engine.stats().too_far, 1u);
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

// Late-player design 3.3: the owner's own birth, made live at tick 5, was
// applied by the server at tick 8 (a MOVED echo).  move_birth puts the
// records where the server's world is: no body from 5 to 7 (no name, no
// input, no pose), and from 8 on the name, its generation and its input but
// no pose - the live run's pose there is motion the server never stepped.
// A body of another lifetime is left alone.
namespace {
    constexpr uint32_t kMoveOrigin = 5, kMoveApplied = 8, kMoveNow = 12;

    fake_body moved_birth_body() {
        fake_body fresh;
        fresh.position[0] = 10.0;
        return fresh;
    }

    // Ticks 1..kMoveNow, the own ball born at kMoveOrigin (lifetime 1) and
    // holding key 0 from then on; with `beam`, it lives from tick 1 and is
    // rebuilt at kMoveOrigin under the same lifetime (the --beam pair).
    void record_moved_birth(fake_world& world, rollback_engine& engine, bool beam) {
        world.savestates = true;
        world.bodies["Remote"].position[0] = 5.0;
        auto w = world.adapter();
        if (beam) world.bodies["Own"].position[0] = -3.0;
        for (uint32_t tick = 1; tick <= kMoveNow; ++tick) {
            if (tick == kMoveOrigin) world.bodies["Own"] = moved_birth_body();
            const bool alive = beam || tick >= kMoveOrigin;
            rollback_tracked tracked;
            tracked.remote_entities = {"Remote"};
            std::map<std::string, input_frame> applied{{"Remote", input_frame{}}};
            if (alive) {
                tracked.own_entity = "Own";
                tracked.own_polls = true;
                tracked.generations["Own"] = 1;
                input_frame own{};
                own.keys = 1;
                applied["Own"] = own;
                world.pending_keys["Own"] = 1;
            }
            w.step();
            engine.record(w, tick, tracked, applied);
        }
        world.calls.clear();
    }

    bool same_fake_body(const fake_body& a, const fake_body& b) {
        return std::memcmp(a.position, b.position, sizeof a.position) == 0
            && std::memcmp(a.linear, b.linear, sizeof a.linear) == 0;
    }
}

TEST(RollbackEngine, AMovedBirthLeavesItsGapWithoutTheBodyAndKeepsItsNameFromTheAppliedTick) {
    fake_world world;
    rollback_engine engine;
    record_moved_birth(world, engine, false);
    EXPECT_EQ(0u, engine.move_birth("Own", 2, kMoveOrigin, kMoveApplied, true));   // another lifetime
    EXPECT_EQ(kMoveNow - kMoveOrigin + 1, engine.move_birth("Own", 1, kMoveOrigin, kMoveApplied, true));
    input_frame frame{};
    for (uint32_t tick = kMoveOrigin; tick <= kMoveNow; ++tick) {
        SCOPED_TRACE(tick);
        EXPECT_FALSE(engine.recorded_body("Own", tick));
        const bool gap = tick < kMoveApplied;
        EXPECT_EQ(gap ? std::string() : std::string("Own"), engine.recorded_own_entity(tick));
        EXPECT_EQ(!gap, engine.recorded_input("Own", tick, frame));
        EXPECT_TRUE(engine.recorded_body("Remote", tick));
    }
    EXPECT_EQ(std::string(), engine.recorded_own_entity(kMoveOrigin - 1));
}

// The first replay after the echo: the caller has taken the live body out,
// and the rewind to T-1 neither feeds nor records a body in the gap; the body
// the caller makes again at T' (its pre_step) is fed from the kept input and
// re-captured under the same lifetime.  A later rewind anchored inside the gap
// - or before the birth - finds the body born at T', has the world destroy it
// at the restore and make it again there, bit for bit, with no re-pose.
TEST(RollbackEngine, AMovedBirthIsMadeAgainAtItsAppliedTickByEveryLaterReplay) {
    fake_world world;
    rollback_engine engine;
    record_moved_birth(world, engine, false);
    engine.move_birth("Own", 1, kMoveOrigin, kMoveApplied, true);
    world.bodies.erase("Own");   // the caller's own destroy, before the rewind
    auto w = world.adapter();
    w.pre_step = [&world](uint32_t tick) {
        if (tick == kMoveApplied) world.bodies["Own"] = moved_birth_body();
    };
    w.destroy_body = [&world](const std::string& entity, uint32_t tick) {
        world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
        return world.bodies.erase(entity) > 0;
    };
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin - 1, kMoveNow, no_input, "own birth moved 5 -> 8"));
    EXPECT_EQ(0, world.count("destroy"));
    EXPECT_EQ(static_cast<int>(kMoveNow - kMoveApplied + 1), world.count("nav_input Own"));
    for (uint32_t tick = kMoveOrigin; tick <= kMoveNow; ++tick)
        EXPECT_EQ(tick >= kMoveApplied, engine.recorded_body("Own", tick)) << tick;
    const fake_body made = world.bodies.at("Own");
    EXPECT_FLOAT_EQ(static_cast<float>(kMoveNow - kMoveApplied + 1), made.linear[0]);

    for (const uint32_t anchor: {kMoveOrigin + 1, kMoveOrigin - 2}) {
        SCOPED_TRACE(anchor);
        world.calls.clear();
        const auto exact_before = engine.stats().births_recreated_exact;
        ASSERT_TRUE(engine.rewind_to(w, anchor, kMoveNow, no_input, "later"));
        EXPECT_EQ(1, world.count("destroy Own@8"));
        EXPECT_EQ(exact_before + 1, engine.stats().births_recreated_exact);
        EXPECT_EQ(0, world.count("set_body Own"));
        EXPECT_TRUE(same_fake_body(made, world.bodies.at("Own")));
    }
}

// The --beam pair keeps the lifetime, so the body has records of the same
// generation before the stamp: the oldest record is no birth record for an
// anchor inside the gap.  The moved birth's own entry makes the applied
// tick's record the birth there, while an anchor before the stamp restores
// the old body from its record and the replay takes it out at the stamp.
TEST(RollbackEngine, AMovedBeamIsBornAtItsAppliedTickForAnAnchorInsideItsGap) {
    fake_world world;
    rollback_engine engine;
    record_moved_birth(world, engine, true);
    engine.move_birth("Own", 1, kMoveOrigin, kMoveApplied, true);
    auto w = world.adapter();
    w.pre_step = [&world](uint32_t tick) {
        if (tick == kMoveOrigin) world.bodies.erase("Own");
        if (tick == kMoveApplied) world.bodies["Own"] = moved_birth_body();
    };
    w.destroy_body = [&world](const std::string& entity, uint32_t tick) {
        world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
        return world.bodies.erase(entity) > 0;
    };
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin - 1, kMoveNow, no_input, "own beam moved 5 -> 8"));
    EXPECT_EQ(0, world.count("destroy"));
    EXPECT_TRUE(engine.recorded_body("Own", kMoveOrigin - 1));
    for (uint32_t tick = kMoveOrigin; tick <= kMoveNow; ++tick)
        EXPECT_EQ(tick >= kMoveApplied, engine.recorded_body("Own", tick)) << tick;
    const fake_body made = world.bodies.at("Own");

    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin + 1, kMoveNow, no_input, "inside the gap"));
    EXPECT_EQ(1, world.count("destroy Own@8"));
    EXPECT_EQ(1u, engine.stats().births_recreated_exact);
    EXPECT_TRUE(same_fake_body(made, world.bodies.at("Own")));

    world.calls.clear();
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin - 2, kMoveNow, no_input, "before the stamp"));
    EXPECT_EQ(0, world.count("destroy"));
    EXPECT_TRUE(same_fake_body(made, world.bodies.at("Own")));
}

// A birth the server never applied (SUPERSEDED, REJECTED): every record from
// its origin on loses it, name and input included, and no replay brings a
// body back.
TEST(RollbackEngine, ABirthTheServerNeverMadeLeavesTheHistoryFromItsOrigin) {
    fake_world world;
    rollback_engine engine;
    record_moved_birth(world, engine, false);
    EXPECT_EQ(kMoveNow - kMoveOrigin + 1, engine.move_birth("Own", 1, kMoveOrigin, kMoveOrigin, false));
    world.bodies.erase("Own");
    input_frame frame{};
    for (uint32_t tick = kMoveOrigin; tick <= kMoveNow; ++tick) {
        EXPECT_EQ(std::string(), engine.recorded_own_entity(tick)) << tick;
        EXPECT_FALSE(engine.recorded_input("Own", tick, frame)) << tick;
    }
    auto w = world.adapter();
    w.destroy_body = [&world](const std::string& entity, uint32_t tick) {
        world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
        return world.bodies.erase(entity) > 0;
    };
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin - 1, kMoveNow, no_input, "own birth superseded"));
    EXPECT_EQ(0, world.count("nav_input Own"));
    EXPECT_EQ(0, world.count("destroy"));
    EXPECT_EQ(0u, world.bodies.count("Own"));
    ASSERT_TRUE(engine.rewind_to(w, kMoveOrigin + 2, kMoveNow, no_input, "later"));
    EXPECT_EQ(0u, world.bodies.count("Own"));
}

// A mechanism the replay makes before the live frames did (a sector
// activation the live frames cancelled, re-issued inside the replay at its
// tick: e8ab2_swing_n1/capture3).  The live records of the window do not name
// it; re-recorded with their sets alone, its first record was the first live
// tick after the replay, and a later rewind anchored between the replayed
// birth and that tick destroyed it as a body born inside its window - while
// the restored scripts kept the joints they had made on it.  With
// replay_mechanisms the replayed ticks name it from the tick that has it, under
// the lifetime it has now: a rewind anchored inside that window restores it
// from its record, one anchored before it destroys it at the replayed birth.
TEST(RollbackEngine, AMechanismTheReplayMadeIsRecordedFromTheReplayedTickThatHasIt) {
    for (const bool hook: {true, false}) {
        SCOPED_TRACE(hook ? "replay_mechanisms" : "no hook (the old records)");
        fake_world world;
        world.savestates = true;
        world.bodies["Own"];
        rollback_engine engine;
        auto w = world.adapter();
        rollback_tracked live;
        live.own_entity = "Own";
        live.generations = {{"Own", 1}, {"Arm", 1}};
        for (uint32_t tick = 1; tick <= 10; ++tick) {
            w.step();
            engine.record(w, tick, live, {{"Own", input_frame{}}});
        }
        constexpr uint32_t kReplayedBirth = 5;
        fake_body made;
        made.position[0] = 10.0;
        w.pre_step = [&](uint32_t tick) {
            if (tick == kReplayedBirth && !world.bodies.contains("Arm")) world.bodies["Arm"] = made;
        };
        w.destroy_body = [&](const std::string& entity, uint32_t tick) {
            world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
            return world.bodies.erase(entity) > 0;
        };
        if (hook) {
            w.replay_mechanisms = [&]() {
                std::vector<std::string> names;
                for (const auto& [name, body]: world.bodies) if (name != "Own") names.push_back(name);
                return names;
            };
        }
        ASSERT_TRUE(engine.rewind_to(w, 2, 10, no_input, "input corrected at tick 3"));
        ASSERT_TRUE(world.bodies.contains("Arm"));
        EXPECT_FALSE(engine.recorded_body("Arm", kReplayedBirth - 1));
        for (uint32_t tick = kReplayedBirth; tick <= 10; ++tick)
            EXPECT_EQ(hook, engine.recorded_body("Arm", tick)) << "tick " << tick;
        EXPECT_EQ(hook ? 10u - kReplayedBirth + 1 : 0u, engine.stats().replay_mechanisms_added);

        // The live tick 11 names it, as every live record from here on does.
        rollback_tracked with_arm = live;
        with_arm.mechanisms = {"Arm"};
        w.step();
        engine.record(w, 11, with_arm, {{"Own", input_frame{}}});

        // Anchored inside the replayed window: restored from its own record,
        // not destroyed.  The old records: destroyed as born at tick 11, and
        // not made again by the replay (its scripts made it at tick 5) - the
        // bodies capture3's client lost for the rest of the session.
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 7, 11, no_input, "input corrected at tick 8"));
        EXPECT_EQ(hook ? 0 : 1, world.count("destroy Arm@11"));
        EXPECT_EQ(0, world.count("destroy Arm@" + std::to_string(kReplayedBirth)));
        EXPECT_EQ(hook, world.bodies.contains("Arm"));
        EXPECT_EQ(hook ? 0u : 1u, engine.stats().births_missing);
        if (!hook) continue;

        // Anchored before the replayed birth: destroyed there and made again
        // by the replayed scripts at that tick.
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 3, 11, no_input, "input corrected at tick 4"));
        EXPECT_EQ(1, world.count("destroy Arm@" + std::to_string(kReplayedBirth)));
        EXPECT_EQ(0, world.count("destroy Arm@11"));
        EXPECT_TRUE(world.bodies.contains("Arm"));
        EXPECT_EQ(0u, engine.stats().births_missing);
    }
}

// The same with a live birth later in the window (review of the above): the
// live frames make Arm at tick 8, a rewind anchored before that destroys it
// for that birth, and the replayed scripts make it again at tick 5.  From the
// replayed tick that has it on, it is the replay's body: recorded there, and
// not beamed to the live birth record at tick 8 - a body held back from the
// records until its live birth tick left the records of 5..7 without it while
// their script records held its joints, and a later rewind anchored inside
// 5..7 destroyed it under those joints.
TEST(RollbackEngine, AMechanismTheReplayMadeBeforeItsLiveBirthIsTheReplaysFromThere) {
    for (const bool hook: {true, false}) {
        SCOPED_TRACE(hook ? "replay_mechanisms" : "no hook (the old records)");
        fake_world world;
        world.savestates = true;
        world.bodies["Own"];
        rollback_engine engine;
        auto w = world.adapter();
        rollback_tracked live;
        live.own_entity = "Own";
        live.generations = {{"Own", 1}, {"Arm", 1}};
        rollback_tracked with_arm = live;
        with_arm.mechanisms = {"Arm"};
        constexpr uint32_t kLiveBirth = 8, kReplayedBirth = 5;
        fake_body live_made, replay_made;
        live_made.position[0] = 20.0;
        live_made.simulated = false;
        replay_made.position[0] = 10.0;
        replay_made.simulated = false;
        for (uint32_t tick = 1; tick <= 10; ++tick) {
            if (tick == kLiveBirth) world.bodies["Arm"] = live_made;
            w.step();
            engine.record(w, tick, tick >= kLiveBirth ? with_arm : live, {{"Own", input_frame{}}});
        }
        w.pre_step = [&](uint32_t tick) {
            if (tick == kReplayedBirth && !world.bodies.contains("Arm")) world.bodies["Arm"] = replay_made;
        };
        w.destroy_body = [&](const std::string& entity, uint32_t tick) {
            world.calls.push_back("destroy " + entity + "@" + std::to_string(tick));
            return world.bodies.erase(entity) > 0;
        };
        if (hook) {
            w.replay_mechanisms = [&]() {
                std::vector<std::string> names;
                for (const auto& [name, body]: world.bodies) if (name != "Own") names.push_back(name);
                return names;
            };
        }
        ASSERT_TRUE(engine.rewind_to(w, 2, 10, no_input, "input corrected at tick 3"));
        EXPECT_EQ(1, world.count("destroy Arm@" + std::to_string(kLiveBirth)));
        ASSERT_TRUE(world.bodies.contains("Arm"));
        for (uint32_t tick = kReplayedBirth; tick < kLiveBirth; ++tick)
            EXPECT_EQ(hook, engine.recorded_body("Arm", tick)) << "tick " << tick;
        // Not beamed to the live birth record at tick 8: the replay's body stays.
        EXPECT_EQ(hook ? 0 : 1, world.count("set_body Arm"));
        EXPECT_EQ(hook ? 10.0 : 20.0, world.bodies["Arm"].position[0]);
        EXPECT_EQ(hook ? kLiveBirth - kReplayedBirth : 0u, engine.stats().replay_mechanisms_added);

        // Anchored between the replayed birth and the live one: restored from
        // its record, not destroyed under the joints its scripts hold.
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 6, 10, no_input, "input corrected at tick 7"));
        EXPECT_EQ(hook ? 0 : 1, world.count("destroy Arm@"));
        EXPECT_EQ(hook, world.bodies.contains("Arm"));
        EXPECT_EQ(hook ? 0u : 1u, engine.stats().births_missing);
        if (!hook) continue;

        // Anchored before the replayed birth: destroyed at it, made again there.
        world.calls.clear();
        ASSERT_TRUE(engine.rewind_to(w, 3, 10, no_input, "input corrected at tick 4"));
        EXPECT_EQ(1, world.count("destroy Arm@" + std::to_string(kReplayedBirth)));
        EXPECT_EQ(1, world.count("destroy Arm@"));
        EXPECT_TRUE(world.bodies.contains("Arm"));
        EXPECT_EQ(0u, world.count("set_body Arm"));
        EXPECT_EQ(0u, engine.stats().births_missing);
    }
}
