// Unit tests for the spawn impulse and the deterministic Random block
// generator (design 9.10): the direction choice is integer-only and distinct
// per join order, the generator is the Microsoft runtime's sequence, and the
// Physicalize event carries its spawn flag on the wire.
#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include <message/message_all.hpp>
#include <physics/deterministic_random.hpp>
#include <session/spawn_impulse.hpp>

namespace {

TEST(SpawnImpulse, DirectionsAreUnitAndHorizontal) {
    using namespace bmmo::session;
    for (uint32_t i = 0; i < kSpawnDirections; ++i) {
        const float* d = kSpawnDirectionTable[i];
        EXPECT_FLOAT_EQ(0.0f, d[1]);
        EXPECT_NEAR(1.0f, std::sqrt(d[0] * d[0] + d[2] * d[2]), 1e-6f);
    }
}

TEST(SpawnImpulse, DirectionsArePairwiseDistinct) {
    using namespace bmmo::session;
    for (uint32_t i = 0; i < kSpawnDirections; ++i)
        for (uint32_t j = i + 1; j < kSpawnDirections; ++j) {
            const float* a = kSpawnDirectionTable[i];
            const float* b = kSpawnDirectionTable[j];
            const float dot = a[0] * b[0] + a[2] * b[2];
            // at least 2.9 degrees apart (golden-angle sequence of 64 points)
            EXPECT_LT(dot, std::cos(2.9f * 3.14159265f / 180.0f)) << i << " vs " << j;
        }
}

TEST(SpawnImpulse, OffsetCellsAreDistinctAndAtLeastASpacingApart) {
    using namespace bmmo::session;
    const float right[3] = {0.0f, 0.0f, 1.0f};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    float cells[kSpawnOffsets][3];
    for (uint32_t j = 0; j < kSpawnOffsets; ++j) spawn_offset(static_cast<uint8_t>(j), 4.2f, right, dir, cells[j]);
    EXPECT_FLOAT_EQ(0.0f, cells[0][0]);   // join order 0 is the resetpoint itself
    EXPECT_FLOAT_EQ(0.0f, cells[0][2]);
    for (uint32_t i = 0; i < kSpawnOffsets; ++i) {
        EXPECT_FLOAT_EQ(0.0f, cells[i][1]);   // in the resetpoint's plane
        for (uint32_t j = i + 1; j < kSpawnOffsets; ++j) {
            const float dx = cells[i][0] - cells[j][0], dz = cells[i][2] - cells[j][2];
            EXPECT_GE(std::sqrt(dx * dx + dz * dz), 4.2f - 1e-5f) << i << " vs " << j;
        }
    }
    // Spacing 0 is the design 9.10 layout: every cell at the resetpoint.
    float none[3];
    spawn_offset(5, 0.0f, right, dir, none);
    EXPECT_FLOAT_EQ(0.0f, none[0]);
    EXPECT_FLOAT_EQ(0.0f, none[2]);
}

TEST(SpawnImpulse, IndexIsDistinctPerJoinOrderAndDeterministic) {
    using namespace bmmo::session;
    for (uint32_t tick: {0u, 1u, 66u, 12345u}) {
        std::set<uint32_t> seen;
        for (unsigned order = 0; order < kSpawnDirections; ++order) {
            const uint32_t index = spawn_direction_index(1, static_cast<uint8_t>(order), tick);
            EXPECT_LT(index, kSpawnDirections);
            EXPECT_EQ(index, spawn_direction_index(1, static_cast<uint8_t>(order), tick));
            seen.insert(index);
        }
        EXPECT_EQ(static_cast<size_t>(kSpawnDirections), seen.size()) << "tick " << tick;
    }
    // Different seeds or ticks rotate the table (not necessarily every time,
    // but over a few samples).
    std::set<uint32_t> rotations;
    for (uint32_t tick = 0; tick < 64; ++tick) rotations.insert(spawn_direction_index(7, 0, tick));
    EXPECT_GT(rotations.size(), 8u);
}

TEST(DeterministicRandom, MatchesTheMicrosoftSequence) {
    bmmo::physics::deterministic_random rng;
    rng.reset(1);
    // rand() after srand(1) on the Microsoft runtime
    EXPECT_EQ(41, rng.next());
    EXPECT_EQ(18467, rng.next());
    EXPECT_EQ(6334, rng.next());
    EXPECT_EQ(26500, rng.next());
    EXPECT_EQ(19169, rng.next());
    rng.reset(1);
    EXPECT_EQ(41, rng.next());
    for (int i = 0; i < 100000; ++i) {
        const int32_t value = rng.next();
        ASSERT_GE(value, 0);
        ASSERT_LE(value, bmmo::physics::deterministic_random::kMax);
    }
}

TEST(SessionEventMsg, PhysicalizeFlagsRoundTrip) {
    bmmo::session_event_msg msg{};
    msg.session = 5;
    msg.player = 9;
    msg.tick = 77;
    msg.type = bmmo::session::event_type::Physicalize;
    msg.ball_type = 1;
    msg.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
    msg.position[0] = 1.f; msg.position[1] = 2.f; msg.position[2] = 3.f;
    for (int k = 0; k < 9; ++k) msg.rotation[k] = static_cast<float>(k);
    msg.recipe.mass = 1.9f;
    ASSERT_TRUE(msg.serialize());
    bmmo::session_event_msg parsed{};
    parsed.raw.write(msg.raw.str().data(), static_cast<std::streamsize>(msg.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(bmmo::session::PHYSICALIZE_FLAG_SPAWN, parsed.flags);
    EXPECT_EQ(1, parsed.ball_type);
    EXPECT_FLOAT_EQ(3.f, parsed.position[2]);
    EXPECT_FLOAT_EQ(8.f, parsed.rotation[8]);
    EXPECT_FLOAT_EQ(1.9f, parsed.recipe.mass);
}

// Step SPX: while the spawn ghost covers every spawn birth a session carries
// no spacing and no kick, whatever the config says; a solo session never does.
TEST(SpawnSeparation, NoneUnderTheSpawnGhost) {
    const auto solo = bmmo::session::session_spawn_separation(1, 3.0f, 4.2f);
    EXPECT_EQ(0.0f, solo.impulse);
    EXPECT_EQ(0.0f, solo.spacing);
    const auto four = bmmo::session::session_spawn_separation(4, 3.0f, 4.2f);
    if (bmmo::session::kSpawnGhostCoversSpawns) {
        EXPECT_EQ(0.0f, four.impulse);
        EXPECT_EQ(0.0f, four.spacing);
    } else {
        EXPECT_EQ(3.0f, four.impulse);
        EXPECT_EQ(4.2f, four.spacing);
    }
}

} // namespace
