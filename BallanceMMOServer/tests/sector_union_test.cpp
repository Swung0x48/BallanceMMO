// Unit tests for the sector union every side of a session runs
// (session/sector_union.hpp): stamped Sector events applied at their tick, one
// pass per idle manager, the lowest owed sector first, nothing ever
// deactivated.
#include <gtest/gtest.h>

#include <session/resync_rules.hpp>
#include <session/sector_union.hpp>

using bmmo::session::sector_pacer;
using bmmo::session::sector_union;

namespace {
    // Ticks the union with an idle manager until the pacer is saturated.
    void settle(sector_union& u, uint32_t& tick) {
        for (int i = 0; i < sector_pacer::kIdleTicks; ++i) EXPECT_FALSE(u.tick(tick++, false));
    }
}

TEST(SectorUnion, StartsWithTheLevelsOwnSectorAndNothingOwed) {
    sector_union u;
    EXPECT_EQ(u.active(), std::set<int>{1});
    EXPECT_FALSE(u.pass_pending());
    uint32_t tick = 100;
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(u.tick(tick++, false));
    EXPECT_EQ(u.passes(), 0u);
}

TEST(SectorUnion, AStampIsAppliedAtItsTickAndThePassStartsThere) {
    sector_union u;
    uint32_t tick = 500;
    settle(u, tick);
    u.stamp(7, 2, 520);
    for (; tick < 520; ++tick) {
        EXPECT_FALSE(u.tick(tick, false)) << "tick " << tick;
        EXPECT_FALSE(u.pass_pending());
    }
    const auto p = u.tick(520, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 2);
    EXPECT_EQ(p.player, 7u);
    EXPECT_EQ(p.stamped_tick, 520u);
    EXPECT_EQ(u.active(), (std::set<int>{1, 2}));
    EXPECT_FALSE(u.pass_pending());
    EXPECT_EQ(u.late(), 0u);
}

TEST(SectorUnion, TwoSectorsOwedOnePassPerIdleManagerLowestFirst) {
    // Two members cross into different sectors, stamped for one tick.  The
    // first pass starts at the stamp; the second waits for the manager's walk
    // and the idle ticks after it, exactly as the server's pacer does.
    sector_union u;
    uint32_t tick = 10;
    settle(u, tick);
    u.stamp(3, 4, 30);
    u.stamp(9, 3, 30);
    while (tick < 30) EXPECT_FALSE(u.tick(tick++, false));
    auto p = u.tick(30, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 3);
    EXPECT_EQ(p.player, 9u);
    EXPECT_TRUE(u.pass_pending());
    // the manager walks for a while
    for (int i = 0; i < 7; ++i) EXPECT_FALSE(u.tick(31 + i, true));
    EXPECT_FALSE(u.tick(38, false));
    EXPECT_FALSE(u.tick(39, false));
    p = u.tick(40, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 4);
    EXPECT_EQ(p.player, 3u);
    EXPECT_EQ(u.active(), (std::set<int>{1, 3, 4}));
    EXPECT_FALSE(u.pass_pending());
    EXPECT_EQ(u.passes(), 2u);
}

TEST(SectorUnion, ASectorAlreadyActiveIsNoPassAndNothingIsEverDeactivated) {
    sector_union u;
    uint32_t tick = 0;
    settle(u, tick);
    u.stamp(1, 2, 5);
    while (tick < 5) u.tick(tick++, false);
    ASSERT_TRUE(u.tick(5, false));
    // a second member enters sector 2 later; a third reports sector 1 again
    // (its ball is still there): neither starts a pass, nothing goes away
    u.stamp(2, 2, 40);
    u.stamp(3, 1, 40);
    for (tick = 6; tick <= 60; ++tick) EXPECT_FALSE(u.tick(tick, false)) << "tick " << tick;
    EXPECT_EQ(u.active(), (std::set<int>{1, 2}));
    EXPECT_EQ(u.passes(), 1u);
}

TEST(SectorUnion, ALateStampIsCountedAndAppliedAtTheNextTick) {
    sector_union u;
    uint32_t tick = 0;
    settle(u, tick);
    for (; tick < 100; ++tick) u.tick(tick, false);
    // stamped for 90, arrives after tick 99 was simulated
    u.stamp(5, 2, 90);
    EXPECT_EQ(u.late(), 1u);
    const auto p = u.tick(100, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 2);
    EXPECT_EQ(p.stamped_tick, 90u);
}

TEST(SectorUnion, RequestNowIsTheServersPathAndIgnoresNonSectors) {
    sector_union u;
    uint32_t tick = 0;
    settle(u, tick);
    u.request(4, 0, tick);
    u.request(4, -1, tick);
    u.stamp(4, 0, tick + 1);
    EXPECT_FALSE(u.pass_pending());
    u.request(4, 3, tick);
    EXPECT_TRUE(u.pass_pending());
    const auto p = u.tick(tick, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 3);
    EXPECT_EQ(p.player, 4u);
}

TEST(SectorUnion, ThePassWaitsWhileTheLevelsOwnWalkRuns) {
    // Fresh world: Init Ingame runs the manager for sector 1 during the first
    // frames.  A sector stamped for the anchor waits for it, plus the idle ticks.
    sector_union u;
    u.stamp(1, 2, 0);
    for (uint32_t t = 0; t < 7; ++t) EXPECT_FALSE(u.tick(t, true));
    EXPECT_FALSE(u.tick(7, false));
    EXPECT_FALSE(u.tick(8, false));
    const auto p = u.tick(9, false);
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 2);
    EXPECT_EQ(p.stamped_tick, 0u);
}

TEST(SectorUnion, ResetGoesBackToTheLevelsOwnSector) {
    sector_union u;
    uint32_t tick = 0;
    settle(u, tick);
    u.request(1, 3, tick);
    ASSERT_TRUE(u.tick(tick, false));
    u.stamp(1, 4, tick + 50);
    u.reset();
    EXPECT_EQ(u.active(), std::set<int>{1});
    EXPECT_EQ(u.stamps_pending(), 0u);
    EXPECT_EQ(u.passes(), 0u);
    EXPECT_FALSE(u.pass_pending());
}

// The resync's keep-local rule (session/resync_rules.hpp, resync design 2.4,
// step A1): the balloon's scripts always, the trafo scripts until step T2,
// nothing else - a module script, the manager, a name that only contains
// "PE_Balloon" further in.
TEST(ResyncRules, KeepLocalScripts) {
    using bmmo::session::keep_local_script;
    EXPECT_TRUE(keep_local_script("PE_Balloon_01", "PE_Balloon Script"));
    EXPECT_TRUE(keep_local_script("PE_Balloon", "anything"));
    EXPECT_TRUE(keep_local_script("", "Ball_Explosion_Wood"));
    EXPECT_TRUE(keep_local_script("Balls", "Ball_ResetPieces_Paper"));
    EXPECT_TRUE(keep_local_script("Balls", "Ball_PaperWind"));
    EXPECT_FALSE(keep_local_script("Balls", "Ball_PaperWind2"));
    EXPECT_FALSE(keep_local_script("Balls", "Ball_CollSound_Pieces"));
    EXPECT_FALSE(keep_local_script("P_Modul_01_01", "P_Modul_01_MF Script"));
    EXPECT_FALSE(keep_local_script("Gameplay", "Gameplay_SectorManager"));
    EXPECT_FALSE(keep_local_script("P_PE_Balloon", "x"));
    EXPECT_FALSE(keep_local_script("PE_Ballo", "x"));
}
