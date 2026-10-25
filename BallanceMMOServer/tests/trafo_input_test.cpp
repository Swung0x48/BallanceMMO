// Unit tests for the trafo as a lockstep input, the CK-free half
// (docs/trafo-replication-design.md 7.1, step T1): the server's ledger per
// (player, piece type), the per-event seed, the trafo input log and record
// v3's restore plan.  Record v3's bytes need CK and are checked by SimTool
// --trafo-record-check (design 7.1 "Record v3").
#include <gtest/gtest.h>

#include <algorithm>

#include <session/resync_rules.hpp>
#include <session/trafo_input.hpp>

namespace {

using namespace bmmo::session;

TEST(TrafoAcceptance, NormalExplodeAndResetMoveNothing) {
    trafo_acceptance ledger;
    const auto x = ledger.explode(1, 0, 808, 780);
    ASSERT_EQ(1u, x.size());
    EXPECT_EQ(808u, x[0].applied);
    EXPECT_EQ(808u, x[0].origin);
    EXPECT_EQ(TRAFO_EXPLODE, x[0].flags);
    // The owner's Fadeout reset about 1164 ticks later.
    const auto r = ledger.reset(1, 0, 1972, 1950);
    EXPECT_FALSE(r.superseded);
    EXPECT_EQ(1972u, r.applied);
    EXPECT_EQ(TRAFO_RESET, r.flags);
    // The next same-type explosion 156 ticks after the reset: >= 140.
    const auto y = ledger.explode(1, 0, 2128, 2100);
    ASSERT_EQ(1u, y.size());
    EXPECT_EQ(2128u, y[0].applied);
    // Late arrivals are applied at the next tick.
    const auto z = ledger.explode(2, 1, 500, 530);
    EXPECT_EQ(530u, z.back().applied);
}

TEST(TrafoAcceptance, AResetAndTheNextExplosionInOneTickKeepTheSpan) {
    trafo_acceptance ledger;
    ledger.explode(1, 2, 100, 100);
    const auto r = ledger.reset(1, 2, 300, 300);
    EXPECT_EQ(300u, r.applied);
    // An outage delivers the re-explosion stamped right behind the reset.
    const auto x = ledger.explode(1, 2, 305, 300);
    ASSERT_EQ(1u, x.size());
    EXPECT_EQ(300u + kTrafoResetSpan, x[0].applied);
    EXPECT_EQ(305u, x[0].origin);
}

TEST(TrafoAcceptance, AnExplosionOfALiveTypeSetInsertsTheServersReset) {
    trafo_acceptance ledger;
    ledger.explode(4, 0, 1000, 990);
    // The owner's reset was lost; the next explosion of the same type comes.
    const auto x = ledger.explode(4, 0, 1200, 1180);
    ASSERT_EQ(2u, x.size());
    EXPECT_EQ(TRAFO_RESET | TRAFO_SERVER, x[0].flags);
    EXPECT_EQ(1180u, x[0].applied);   // max(N, live.applied + 1)
    EXPECT_EQ(x[0].applied, x[0].origin);
    EXPECT_EQ(TRAFO_EXPLODE, x[1].flags);
    EXPECT_EQ(1180u + kTrafoResetSpan, x[1].applied);
    // live.applied + 1 when the next tick is earlier than that.
    trafo_acceptance l2;
    l2.explode(4, 0, 1000, 990);
    const auto y = l2.explode(4, 0, 995, 995);
    EXPECT_EQ(1001u, y[0].applied);
    EXPECT_EQ(1001u + kTrafoResetSpan, y[1].applied);
}

TEST(TrafoAcceptance, AResetOlderThanTheNewestExplosionIsSuperseded) {
    trafo_acceptance ledger;
    ledger.explode(3, 1, 1000, 1000);
    // The re-explosion arrives before the reset made before it.
    const auto x = ledger.explode(3, 1, 1300, 1300);
    ASSERT_EQ(2u, x.size());   // the server reset the earlier life itself
    const auto r = ledger.reset(3, 1, 1250, 1310);
    EXPECT_TRUE(r.superseded);
    // The live explosion is untouched: a later reset resets it.
    const auto* e = ledger.find(3, 1);
    ASSERT_NE(nullptr, e);
    EXPECT_TRUE(e->live);
    const auto later = ledger.reset(3, 1, 1400, 1400);
    EXPECT_FALSE(later.superseded);
    EXPECT_FALSE(ledger.find(3, 1)->live);
}

TEST(TrafoAcceptance, AResetNeverComesBeforeItsExplosion) {
    trafo_acceptance ledger;
    // The explosion moved to reset + 140; the owner's reset stamped earlier
    // than that applied tick (but after the explosion's origin).
    ledger.explode(1, 0, 100, 100);
    ledger.reset(1, 0, 200, 200);
    const auto x = ledger.explode(1, 0, 210, 210);
    ASSERT_EQ(340u, x.back().applied);
    const auto r = ledger.reset(1, 0, 300, 300);
    EXPECT_FALSE(r.superseded);
    EXPECT_EQ(341u, r.applied);
}

TEST(TrafoAcceptance, TheDeadlineFiresAtLifeEndOnceTheLeadReachesIt) {
    trafo_acceptance ledger;
    ledger.explode(2, 2, 500, 500);
    const uint32_t lead = 35;
    // Not yet: 500 + 1320 > N + L.
    EXPECT_TRUE(ledger.deadline(500 + kTrafoLife - lead - 1, lead).empty());
    const auto d = ledger.deadline(500 + kTrafoLife - lead, lead);
    ASSERT_EQ(1u, d.size());
    EXPECT_EQ(TRAFO_RESET | TRAFO_SERVER, d[0].flags);
    EXPECT_EQ(500u + kTrafoLife, d[0].applied);
    EXPECT_EQ(d[0].applied, d[0].origin);
    EXPECT_EQ(2u, d[0].player);
    EXPECT_EQ(2, d[0].type);
    // Once.
    EXPECT_TRUE(ledger.deadline(500 + kTrafoLife + 10, lead).empty());
    // A late owner reset after the deadline: accepted, a reset of nothing.
    const auto late = ledger.reset(2, 2, 1900, 1900);
    EXPECT_FALSE(late.superseded);
    EXPECT_EQ(1900u, late.applied);
    // The next explosion keeps the span from the latest reset.
    const auto x = ledger.explode(2, 2, 1950, 1950);
    EXPECT_EQ(1900u + kTrafoResetSpan, x.back().applied);
}

TEST(TrafoAcceptance, TheDeadlineDoesNotFireWhenAResetCameFirst) {
    trafo_acceptance ledger;
    ledger.explode(2, 0, 500, 500);
    ledger.reset(2, 0, 1664, 1664);
    EXPECT_TRUE(ledger.deadline(500 + kTrafoLife, 35).empty());
    EXPECT_TRUE(ledger.deadline(5000, 35).empty());
}

TEST(TrafoAcceptance, LeaversGetAFinalResetOfEveryType) {
    // With a ball and a live paper set.
    trafo_acceptance ledger;
    ledger.explode(9, 2, 700, 700);
    auto f = ledger.leaver(9, 900);
    ASSERT_EQ(3u, f.size());
    uint32_t last = 0;
    for (const auto& d: f) {
        EXPECT_EQ(TRAFO_RESET | TRAFO_SERVER | TRAFO_FINAL, d.flags);
        EXPECT_EQ(d.applied, d.origin);
        EXPECT_GE(d.applied, 900u);
        last = std::max(last, d.applied);
        EXPECT_TRUE(ledger.find(9, d.type)->final);
    }
    EXPECT_EQ(900u, last);
    // Without a ball and with an explosion accepted ahead of the removal tick
    // (review 1 S4): the live type set is reset at >= live.applied + 1.
    trafo_acceptance l2;
    const auto x = l2.explode(5, 0, 1200, 1100);
    ASSERT_EQ(1200u, x.back().applied);
    f = l2.leaver(5, 1150);
    ASSERT_EQ(3u, f.size());
    EXPECT_EQ(0, f[0].type);
    EXPECT_EQ(1201u, f[0].applied);
    EXPECT_EQ(1150u, f[1].applied);
    EXPECT_EQ(1150u, f[2].applied);
    // remove_at = 1 + the last.
    uint32_t remove_at = 0;
    for (const auto& d: f) remove_at = std::max(remove_at, d.applied + 1);
    EXPECT_EQ(1202u, remove_at);
}

TEST(TrafoSeed, FixedVectors) {
    // FNV-1a over (session seed, player, applied tick, type), little-endian,
    // folded to a nonzero int32 - the same on x64, x86 and GCC.
    EXPECT_EQ(47238444, trafo_seed(1, 1, 808, 0));
    EXPECT_EQ(1421187190, trafo_seed(1, 7, 808, 0));
    EXPECT_EQ(245989215, trafo_seed(1, 7, 809, 0));
    EXPECT_EQ(878516820, trafo_seed(1, 7, 808, 2));
    EXPECT_EQ(651574269, trafo_seed(3920120238u, 3920120238u, 4000000000u, 1));
    EXPECT_EQ(1768495365, trafo_seed(0, 0, 0, 0));
    EXPECT_NE(0, trafo_seed(0, 0, 0, 0));
}

TEST(TrafoInputLog, OrdersByTickAndAnswersTheNewestExplosion) {
    trafo_input_log log;
    trafo_input a;
    a.player = 2; a.type = 0; a.flags = TRAFO_EXPLODE; a.origin = a.applied = 800;
    trafo_input b = a;
    b.player = 1;
    trafo_input c = a;
    c.flags = TRAFO_RESET; c.origin = c.applied = 1900;
    trafo_input d = a;
    d.origin = d.applied = 2100;
    log.add(c);
    log.add(a);
    log.add(b);
    log.add(d);
    const auto at800 = log.at(800);
    ASSERT_EQ(2u, at800.size());
    EXPECT_EQ(1u, at800[0].player);   // by player inside one tick
    EXPECT_EQ(2u, at800[1].player);
    EXPECT_EQ(nullptr, log.newest_explosion(2, 0, 799));
    ASSERT_NE(nullptr, log.newest_explosion(2, 0, 800));
    EXPECT_EQ(800u, log.newest_explosion(2, 0, 2099)->applied);
    EXPECT_EQ(2100u, log.newest_explosion(2, 0, 5000)->applied);
    EXPECT_EQ(1900u, log.newest(2, 0, true, 2000)->applied);
    // MOVED: re-keyed; REJECTED: dropped.
    EXPECT_TRUE(log.move(2, 0, TRAFO_EXPLODE, 2100, 2240, 77));
    EXPECT_TRUE(log.at(2100).empty());
    ASSERT_EQ(1u, log.at(2240).size());
    EXPECT_EQ(77, log.at(2240)[0].seed);
    EXPECT_EQ(2100u, log.at(2240)[0].origin);
    EXPECT_TRUE(log.drop(1, 0, TRAFO_EXPLODE, 800));
    EXPECT_FALSE(log.drop(1, 0, TRAFO_EXPLODE, 800));
    EXPECT_EQ(3u, log.size());
}

TEST(TrafoInputLog, PruningKeepsEachTypeSetsNewestExplosionAndReset) {
    trafo_input_log log;
    for (uint32_t t: {100u, 300u, 500u}) {
        trafo_input x;
        x.player = 1; x.type = 2; x.origin = x.applied = t;
        log.add(x);
        trafo_input r = x;
        r.flags = TRAFO_RESET; r.origin = r.applied = t + 50;
        log.add(r);
    }
    trafo_input other;
    other.player = 2; other.type = 0; other.origin = other.applied = 120;
    log.add(other);
    // Everything before 1000 goes except (1,2)'s 500/550 and (2,0)'s 120.
    EXPECT_EQ(4u, log.prune(1000));
    EXPECT_EQ(3u, log.size());
    EXPECT_EQ(500u, log.newest_explosion(1, 2, 999)->applied);
    EXPECT_EQ(550u, log.newest(1, 2, true, 999)->applied);
    EXPECT_EQ(120u, log.newest_explosion(2, 0, 999)->applied);
}

TEST(TrafoInputLog, PruningKeepsTheLiveExplosionWhenALaterOneIsPending) {
    // p1 wood explodes at 0 and resets at 200 (bodies until 332); the next
    // explosion is logged for 340 already (the lead).  prune(234) keeps the
    // explosion at 0: it is still the newest at or before every t < 340.
    trafo_input_log log;
    trafo_input e1;
    e1.player = 1; e1.type = 0; e1.origin = e1.applied = 0;
    trafo_input r1 = e1;
    r1.flags = TRAFO_RESET; r1.origin = r1.applied = 200;
    trafo_input e2 = e1;
    e2.origin = e2.applied = 340;
    log.add(e1);
    log.add(r1);
    log.add(e2);
    EXPECT_EQ(0u, log.prune(234));
    EXPECT_EQ(3u, log.size());
    ASSERT_NE(nullptr, log.newest_explosion(1, 0, 300));
    EXPECT_EQ(0u, log.newest_explosion(1, 0, 300)->applied);
    EXPECT_EQ(340u, log.newest_explosion(1, 0, 340)->applied);
    // Once the history passes 340, the explosion at 0 goes.
    EXPECT_EQ(1u, log.prune(400));
    EXPECT_EQ(340u, log.newest_explosion(1, 0, 400)->applied);
    EXPECT_EQ(200u, log.newest(1, 0, true, 400)->applied);
}

TEST(TrafoInputLog, AnExplodeAndAResetAtOneOriginAreTwoInputs) {
    trafo_input_log log;
    trafo_input x;
    x.player = 1; x.type = 1; x.origin = x.applied = 400;
    trafo_input r = x;
    r.flags = TRAFO_RESET;
    trafo_input s = r;
    s.flags = TRAFO_RESET | TRAFO_SERVER;
    log.add(x);
    log.add(r);
    log.add(s);
    EXPECT_EQ(3u, log.size());
    log.add(x);   // the same input again replaces it
    EXPECT_EQ(3u, log.size());
}

TEST(TrafoNames, TheCopiesAreNoOriginalTrafoScripts) {
    // A set's copies are selected through the registry, never by name
    // (design 4.3): the originals' name rule leaves "_Trafo<p>" out.
    EXPECT_TRUE(is_trafo_script_name("Ball_Explosion_Wood"));
    EXPECT_TRUE(is_trafo_script_name("Ball_ResetPieces_Paper"));
    EXPECT_TRUE(is_trafo_script_name("Ball_PaperWind"));
    EXPECT_FALSE(is_trafo_script_name("Ball_Explosion_Wood_Trafo7"));
    EXPECT_FALSE(is_trafo_script_name("Ball_ResetPieces_Stone_Trafo3920120238"));
    EXPECT_FALSE(is_trafo_script_name("Ball_PaperWind_Trafo7"));
    EXPECT_FALSE(keep_local_script("BMMO_TrafoOwner_7", "Ball_Explosion_Paper_Trafo7"));
    EXPECT_TRUE(keep_local_script("All_Balls", "Ball_Explosion_Paper"));
}

TEST(TrafoRecordPlan, BuiltForNewScriptsFailForVanishedOnes) {
    const std::vector<uint32_t> recorded = {11, 12, 13};
    const std::vector<uint32_t> selected = {11, 12, 13, 21, 22};
    auto plan = plan_trafo_record_restore(recorded, selected, [](uint32_t) { return true; });
    EXPECT_TRUE(plan.ok());
    EXPECT_EQ(recorded, plan.from_record);
    EXPECT_EQ((std::vector<uint32_t>{21, 22}), plan.to_built);
    // A recorded set that was deselected (not selected now, still alive)
    // is restored from the record.
    plan = plan_trafo_record_restore(recorded, {21}, [](uint32_t) { return true; });
    EXPECT_EQ(recorded, plan.from_record);
    EXPECT_EQ((std::vector<uint32_t>{21}), plan.to_built);
    // A vanished id fails the restore.
    plan = plan_trafo_record_restore(recorded, selected, [](uint32_t id) { return id != 12; });
    EXPECT_FALSE(plan.ok());
    EXPECT_EQ((std::vector<uint32_t>{12}), plan.vanished);
}

}
