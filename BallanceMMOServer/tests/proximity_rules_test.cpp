// U1 (docs/world-agreement-gaps-design.md §7.1, step G1): the proximity
// union's sensor rules, session/proximity_rules.hpp.  The float results are
// pinned as hex constants; the same vectors give the same constants on the
// x86 MSVC and GCC builds (.local-tools/work/WG1/u1_gen.cpp prints them).

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <session/proximity_rules.hpp>

namespace {
    using bmmo::session::probe_ball;
    using bmmo::session::probe_metric;
    using bmmo::session::probe_nearest;

    uint32_t bits(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    probe_ball ball(uint32_t player, float x, float y, float z) {
        probe_ball b;
        b.player = player;
        b.position[0] = x;
        b.position[1] = y;
        b.position[2] = z;
        return b;
    }

    const float kRef[3] = {12.345f, -3.21f, 7.89f};
    const float kA[3] = {52.3456f, -63.2109f, 7.8899f};
    const float kB[3] = {-42.6543f, -3.2101f, 7.8901f};
    const float kC[3] = {0.1f, 0.2f, 0.3f};
}

TEST(ProximityRules, MetricPinnedBits) {
    struct row { const float* p; uint8_t axes; uint32_t expected; };
    const row rows[] = {
        {kA, 1, 0x44c80189u}, {kA, 2, 0x456101bbu}, {kA, 4, 0x322aa100u}, {kA, 5, 0x44c80189u}, {kA, 7, 0x45a28140u},
        {kB, 1, 0x453d0ec5u}, {kB, 2, 0x322b7240u}, {kB, 4, 0x322c4400u}, {kB, 5, 0x453d0ec5u}, {kB, 7, 0x453d0ec5u},
        {kC, 1, 0x4315f0a5u}, {kC, 2, 0x413a0cb3u}, {kC, 4, 0x42666eb0u}, {kC, 5, 0x434f8c51u}, {kC, 7, 0x435b2d1cu},
    };
    for (const row& r: rows) {
        EXPECT_EQ(bits(probe_metric(r.p, kRef, r.axes)), r.expected) << "axes " << int(r.axes);
        // The square makes the operands' order irrelevant to the bits.
        EXPECT_EQ(bits(probe_metric(kRef, r.p, r.axes)), r.expected) << "axes " << int(r.axes);
    }
    EXPECT_EQ(probe_metric(kA, kRef, 0), 0.0f);
}

TEST(ProximityRules, NearestUsesTheBlocksAxes) {
    // G-c5: A at XZ 40 m and 60 m below a block measuring X and Z; B at XZ
    // 55 m at the block's height.  Axes 5 picks A (in range of a 50 m block),
    // axes 7 picks B (3D 55 m against 72 m).
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const std::vector<probe_ball> balls{ball(1, 40.0f, -60.0f, 0.0f), ball(2, 55.0f, 0.0f, 0.0f)};
    EXPECT_EQ(probe_nearest(balls, origin, 5), 0);
    EXPECT_EQ(probe_nearest(balls, origin, 7), 1);
    const std::vector<probe_ball> pinned{ball(7, kA[0], kA[1], kA[2]), ball(9, kB[0], kB[1], kB[2])};
    EXPECT_EQ(probe_nearest(pinned, kRef, 5), 0);
    EXPECT_EQ(probe_nearest(pinned, kRef, 7), 1);
}

TEST(ProximityRules, TieTakesTheFirstAndEmptyIsMinusOne) {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    // Equal metrics (25 each): the first, i.e. the lower player id.
    const std::vector<probe_ball> tie{ball(3, 5.0f, 0.0f, 0.0f), ball(4, 0.0f, 0.0f, -5.0f)};
    EXPECT_EQ(probe_nearest(tie, origin, 5), 0);
    EXPECT_EQ(probe_nearest(tie, origin, 7), 0);
    EXPECT_EQ(probe_nearest({}, origin, 7), -1);
    // A strictly nearer later ball wins.
    const std::vector<probe_ball> later{ball(3, 5.0f, 0.0f, 0.0f), ball(4, 0.0f, 0.0f, -4.999f)};
    EXPECT_EQ(probe_nearest(later, origin, 5), 1);
}
