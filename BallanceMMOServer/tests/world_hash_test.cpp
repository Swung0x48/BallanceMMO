// Unit tests for the world hash's core fold (physics/world_hash.hpp): the
// order the physics manager lists the bodies in must not reach the hash, and
// every field of a core still must.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <physics/world_hash.hpp>

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
