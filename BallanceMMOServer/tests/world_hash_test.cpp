// Unit tests for the world hash's core fold (physics/world_hash.hpp): the
// order the physics manager lists the bodies in must not reach the hash, and
// every field of a core still must.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <physics/world_hash.hpp>
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
