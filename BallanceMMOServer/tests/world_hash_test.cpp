// Unit tests for the world hash's core fold (physics/world_hash.hpp): the
// order the physics manager lists the bodies in must not reach the hash, and
// every field of a core still must.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <physics/world_hash.hpp>
#include <physics/world_hash_memo.hpp>
#include <game/hull_template.hpp>

using bmmo::physics::core_state;
using bmmo::physics::fnv1a64;
using bmmo::physics::fold_term;

namespace {
    core_state make_core(double x, float speed, uint8_t movement) {
        core_state c{};
        c.position[0] = x;
        c.position[1] = 2.0;
        c.position[2] = 43.0;
        c.q_last_psi[3] = 1.0;
        c.q_next_psi[3] = 1.0;
        c.speed[0] = speed;
        c.movement_state = movement;
        c.i_delta_time = 66.0f;
        c.time_of_last_psi = 1.5;
        return c;
    }

    uint64_t fold(const std::vector<core_state>& cores) {
        uint64_t sum = 0;
        for (const auto& c: cores) {
            fnv1a64 one;
            bmmo::physics::feed_core(one, c);
            sum += fold_term(one.value);
        }
        fnv1a64 final;
        final.feed(sum);
        return final.value;
    }
}

TEST(WorldHash, TheListOrderDoesNotReachTheFold) {
    // Four balls recreated in one tick come back in whatever order the
    // manager lists them; the live world and a replay of its journal differed
    // in exactly that and nothing else.
    std::vector<core_state> cores{make_core(351.0, 1.0f, 1), make_core(348.0, 2.0f, 1),
                                  make_core(342.0, 3.0f, 2), make_core(345.0, 4.0f, 1)};
    const uint64_t reference = fold(cores);
    std::sort(cores.begin(), cores.end(), [](const core_state& a, const core_state& b) { return a.position[0] < b.position[0]; });
    EXPECT_EQ(reference, fold(cores));
    std::reverse(cores.begin(), cores.end());
    EXPECT_EQ(reference, fold(cores));
}

TEST(WorldHash, EveryCoreAndEveryFieldStillCounts) {
    std::vector<core_state> cores{make_core(351.0, 1.0f, 1), make_core(348.0, 2.0f, 1)};
    const uint64_t reference = fold(cores);
    auto changed = cores;
    changed[1].speed[0] = 2.5f;
    EXPECT_NE(reference, fold(changed));
    changed = cores;
    changed[0].movement_state = 3;
    EXPECT_NE(reference, fold(changed));
    changed = cores;
    changed.pop_back();
    EXPECT_NE(reference, fold(changed));
    // Two identical cores are two terms, not one.
    changed = cores;
    changed.push_back(cores[0]);
    EXPECT_NE(reference, fold(changed));
}

// FULL step H1X (review fix-up): build_hull_templates counts a hull as held
// only when it holds the template's build, and flags the line of every other
// outcome as failed - the hosts end their session on it.
namespace {
    struct fake_hull_backend {
        bool ok = true;
        bmmo_physics_hull_template result{};
        mutable int calls = 0;
        bool hull_template(const char*, const char*, bmmo_physics_hull_template& out, std::string& error) const {
            ++calls;
            if (!ok) { error = "no mesh"; return false; }
            out = result;
            return true;
        }
    };
    struct hull_run {
        int held = 0;
        int lines = 0;
        bool failed = false;
        std::string line;
    };
    hull_run run_hull_templates(const fake_hull_backend& backend) {
        hull_run run;
        run.held = bmmo::game::build_hull_templates(backend, "the anchor", [&run](const std::string& line, bool failed) {
            ++run.lines;
            run.failed = run.failed || failed;
            run.line = line;
        });
        return run;
    }
}

TEST(HullTemplate, OutcomesCountAndFlag) {
    ASSERT_EQ(bmmo::game::kTemplatedHullCount, 1);
    fake_hull_backend backend;
    backend.result.outcome = BMMO_HULL_TEMPLATE_BUILT;
    backend.result.signature = 0xbbd5631496ea2326ull;
    backend.result.size = 7564;
    hull_run run = run_hull_templates(backend);
    EXPECT_EQ(run.held, bmmo::game::kTemplatedHullCount);
    EXPECT_FALSE(run.failed);
    EXPECT_EQ(run.lines, 1);
    EXPECT_NE(run.line.find("built from the template"), std::string::npos);

    backend.result.outcome = BMMO_HULL_TEMPLATE_REBUILT;
    backend.result.previous = 0x1234ull;
    run = run_hull_templates(backend);
    EXPECT_EQ(run.held, bmmo::game::kTemplatedHullCount);
    EXPECT_FALSE(run.failed);
    EXPECT_NE(run.line.find("forgotten first"), std::string::npos);

    backend.result.outcome = BMMO_HULL_TEMPLATE_KEPT;
    backend.result.previous = backend.result.signature;
    run = run_hull_templates(backend);
    EXPECT_EQ(run.held, bmmo::game::kTemplatedHullCount);
    EXPECT_FALSE(run.failed);
    EXPECT_NE(run.line.find("equal to"), std::string::npos);

    backend.result.previous = 0x1234ull;
    run = run_hull_templates(backend);
    EXPECT_LT(run.held, bmmo::game::kTemplatedHullCount);
    EXPECT_TRUE(run.failed);
    EXPECT_NE(run.line.find("DIFFERS"), std::string::npos);

    backend.ok = false;
    run = run_hull_templates(backend);
    EXPECT_LT(run.held, bmmo::game::kTemplatedHullCount);
    EXPECT_TRUE(run.failed);
    EXPECT_NE(run.line.find("NOT built"), std::string::npos);
    EXPECT_NE(run.line.find("no mesh"), std::string::npos);
    EXPECT_EQ(backend.calls, 5);
}

// ---- step OPA: the surface signature's per-object memo (world_hash_memo.hpp)
namespace {
    using bmmo::physics::surface_memo;
    using bmmo::physics::surface_memo_mode;
    using bmmo::physics::surface_memo_probe;
    using bmmo::physics::surface_memo_stats;

    // A fake physicalized object: its name and "compact surface" bytes.
    struct fake_body {
        std::string name;
        std::vector<unsigned char> surface;
        int object_tag = 0;   // stands in for the IVP_Real_Object (its address is the key)
    };

    // The per-object stream physics_state.cpp's surface_term feeds.
    uint64_t stream(const fake_body& b) {
        fnv1a64 h;
        h.feed(b.name.data(), b.name.size());
        const int type = 0;
        h.feed(type);
        h.feed(b.surface.data(), b.surface.size());
        return h.value;
    }

    struct fake_world {
        std::vector<fake_body> bodies;
        std::vector<surface_memo_probe> probes() const {
            std::vector<surface_memo_probe> out;
            for (size_t i = 0; i < bodies.size(); ++i) {
                surface_memo_probe p;
                p.entry_key = static_cast<uint32_t>(100 + i);
                p.object = &bodies[i].object_tag;
                p.manager = &bodies[i].name;
                p.manager_type = 0;
                p.surface = bodies[i].surface.data();
                p.surface_size = static_cast<uint32_t>(bodies[i].surface.size());
                p.name = bodies[i].name.c_str();
                out.push_back(p);
            }
            return out;
        }
        uint64_t full() const {
            uint64_t t = 0;
            for (const auto& b: bodies) t += fold_term(stream(b));
            return t;
        }
    };

    fake_world make_world() {
        fake_world w;
        w.bodies.reserve(8);
        for (int i = 0; i < 3; ++i)
            w.bodies.push_back({"Body_" + std::to_string(i),
                                std::vector<unsigned char>(64 + 8 * i, static_cast<unsigned char>(i + 1)), i});
        return w;
    }

    uint64_t sign(surface_memo& memo, const fake_world& w, surface_memo_mode mode, surface_memo_stats& stats,
                  uint64_t generation = 0, std::vector<std::string>* notes = nullptr, int* hashed_calls = nullptr) {
        static const int environment = 0;
        const auto probes = w.probes();
        return memo.signature(&environment, generation, probes, mode, [&](size_t i) {
            if (hashed_calls) ++*hashed_calls;
            return stream(w.bodies[i]);
        }, stats, notes);
    }
}

TEST(SurfaceMemo, HitReturnsTheFullSum) {
    fake_world w = make_world();
    surface_memo memo;
    surface_memo_stats stats;
    int hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 3);
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 3);            // the second tick hashed nothing
    EXPECT_EQ(stats.hits, 3u);
    EXPECT_EQ(stats.hashed, 3u);
    // Mode off hashes every object and keeps nothing.
    surface_memo off;
    surface_memo_stats off_stats;
    EXPECT_EQ(sign(off, w, surface_memo_mode::off, off_stats), w.full());
    EXPECT_EQ(sign(off, w, surface_memo_mode::off, off_stats), w.full());
    EXPECT_EQ(off.size(), 0u);
    EXPECT_EQ(off_stats.hits, 0u);
    EXPECT_EQ(off_stats.hashed, 6u);
}

TEST(SurfaceMemo, KeyPartChangeRehashesThatObject) {
    fake_world w = make_world();
    surface_memo memo;
    surface_memo_stats stats;
    sign(memo, w, surface_memo_mode::on, stats);
    // The same surface buffer, new leading bytes (a recompile in place).
    w.bodies[1].surface[0] ^= 0x5a;
    int hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 1);
    // A renamed object.
    w.bodies[2].name = "Body_renamed";
    hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 1);
    EXPECT_EQ(stats.set_drops, 0u);
}

TEST(SurfaceMemo, DropOnAFree) {
    fake_world w = make_world();
    surface_memo memo;
    surface_memo_stats stats;
    sign(memo, w, surface_memo_mode::on, stats);
    // A change the key cannot see (bytes past the head, same address and
    // size): stale until the host drops the memo.
    w.bodies[0].surface[60] ^= 0x11;
    EXPECT_NE(sign(memo, w, surface_memo_mode::on, stats), w.full());
    memo.drop(stats);
    EXPECT_EQ(stats.host_drops, 1u);
    int hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 3);
    // A deleted body (the generation the host passes) drops it too.
    w.bodies[0].surface[61] ^= 0x22;
    hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 1, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 3);
    EXPECT_EQ(stats.host_drops, 2u);
}

TEST(SurfaceMemo, DropOnASetChange) {
    fake_world w = make_world();
    surface_memo memo;
    surface_memo_stats stats;
    sign(memo, w, surface_memo_mode::on, stats);
    // Stale bytes the key cannot see, then a body added: the whole memo goes.
    w.bodies[0].surface[60] ^= 0x11;
    w.bodies.push_back({"Body_new", std::vector<unsigned char>(40, 9), 7});
    int hashed = 0;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::on, stats, 0, nullptr, &hashed), w.full());
    EXPECT_EQ(hashed, 4);
    EXPECT_EQ(stats.set_drops, 1u);
    // Every object at a new address (a copy of the world): dropped again.
    fake_world moved = w;
    hashed = 0;
    EXPECT_EQ(sign(memo, moved, surface_memo_mode::on, stats, 0, nullptr, &hashed), moved.full());
    EXPECT_EQ(hashed, 4);
    EXPECT_EQ(stats.set_drops, 2u);
    // A body removed.
    moved.bodies.pop_back();
    moved.bodies[0].surface[62] ^= 0x33;
    hashed = 0;
    EXPECT_EQ(sign(memo, moved, surface_memo_mode::on, stats, 0, nullptr, &hashed), moved.full());
    EXPECT_EQ(hashed, 3);
    EXPECT_EQ(stats.set_drops, 3u);
}

TEST(SurfaceMemo, VerifyCatchesAForcedStaleEntry) {
    fake_world w = make_world();
    surface_memo memo;
    surface_memo_stats stats;
    std::vector<std::string> notes;
    EXPECT_EQ(sign(memo, w, surface_memo_mode::verify, stats, 0, &notes), w.full());
    EXPECT_EQ(sign(memo, w, surface_memo_mode::verify, stats, 0, &notes), w.full());
    EXPECT_EQ(stats.verified, 3u);
    EXPECT_EQ(stats.mismatches, 0u);
    ASSERT_TRUE(memo.corrupt_for_test(1, 0x1));
    // Verify journals what mode on would (the stale sum) and reports it ...
    const uint64_t stale = sign(memo, w, surface_memo_mode::verify, stats, 0, &notes);
    EXPECT_NE(stale, w.full());
    EXPECT_EQ(stats.mismatches, 1u);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("Body_1"), std::string::npos);
    // ... and the entry is fresh again from the next tick.
    EXPECT_EQ(sign(memo, w, surface_memo_mode::verify, stats, 0, &notes), w.full());
    EXPECT_EQ(stats.mismatches, 1u);
    // Mode on keeps a stale value (what verify exists to catch).
    ASSERT_TRUE(memo.corrupt_for_test(0, 0x2));
    EXPECT_NE(sign(memo, w, surface_memo_mode::on, stats), w.full());
}

TEST(SurfaceMemo, ParseMode) {
    using bmmo::physics::parse_surface_memo_mode;
    EXPECT_EQ(parse_surface_memo_mode(nullptr, surface_memo_mode::on), surface_memo_mode::on);
    EXPECT_EQ(parse_surface_memo_mode("off", surface_memo_mode::on), surface_memo_mode::off);
    EXPECT_EQ(parse_surface_memo_mode("verify", surface_memo_mode::on), surface_memo_mode::verify);
    EXPECT_EQ(parse_surface_memo_mode("bogus", surface_memo_mode::on), surface_memo_mode::on);
}
