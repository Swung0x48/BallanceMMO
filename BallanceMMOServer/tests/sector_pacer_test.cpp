// Unit tests for the server's sector activation pacing (session/sector_pacer.hpp):
// one pass per idle manager, and the idle count runs every tick, so a change
// that arrives after a long idle fires at its own tick instead of two later.
#include <gtest/gtest.h>

#include <session/sector_pacer.hpp>

using bmmo::session::sector_pacer;

TEST(SectorPacer, IdleSaturatesWhileNothingIsPending) {
    sector_pacer pacer;
    for (int t = 0; t < 10; ++t) pacer.tick(false);
    EXPECT_EQ(pacer.idle_ticks(), sector_pacer::kIdleTicks);
    EXPECT_TRUE(pacer.tick(false));
}

TEST(SectorPacer, AChangeAfterALongIdleFiresAtItsOwnTick) {
    // The server's world: the manager finished the level's own sector-1 walk
    // long ago, nothing has been pending since.  A Sector event stamped for
    // tick T is applied before T's step, and the pass must start AT T - the
    // old rule counted only while a change was pending and needed three
    // passes, so the activation came at T + 2 every time.
    sector_pacer pacer;
    for (int t = 0; t < 40; ++t) EXPECT_EQ(pacer.tick(false), t >= sector_pacer::kIdleTicks - 1);
    EXPECT_TRUE(pacer.tick(false));   // tick T: the pass starts now
}

TEST(SectorPacer, TheLevelsOwnWalkAtTheAnchorBlocksUntilItIsOverPlusTwo) {
    // Fresh world: Init Ingame runs the manager for sector 1 during the first
    // frames.  A pass may not start while it walks, nor in the two ticks after.
    sector_pacer pacer;
    for (int t = 0; t < 7; ++t) EXPECT_FALSE(pacer.tick(true));
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_TRUE(pacer.tick(false));
}

TEST(SectorPacer, BackToBackChangesTheSecondWaitsForTheWalk) {
    sector_pacer pacer;
    for (int t = 0; t < 5; ++t) pacer.tick(false);
    ASSERT_TRUE(pacer.tick(false));
    pacer.started();                                  // first change handed over
    EXPECT_EQ(pacer.idle_ticks(), 0);
    // The manager is seen active from the next tick on, for its walk.
    for (int t = 0; t < 7; ++t) EXPECT_FALSE(pacer.tick(true));
    // Then two idle ticks of grace, and the second change goes on the third.
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_TRUE(pacer.tick(false));
}

TEST(SectorPacer, StartedWithoutAWalkStillCostsTheIdleTicks) {
    // A pass whose manager never shows as active (nothing to walk) is still
    // followed by the grace ticks: the rule is about the scripts' first frame,
    // not only about the walk.
    sector_pacer pacer;
    for (int t = 0; t < 5; ++t) pacer.tick(false);
    pacer.started();
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_FALSE(pacer.tick(false));
    EXPECT_TRUE(pacer.tick(false));
}
