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

// ---- step B1 (resync design 2.5, 7.1): the union as rollback state ----

// state / restore round trip (the counters stay the live union's), unstamp,
// and the byte form: equal after a round trip, refused short, long or of
// another version.
TEST(SectorUnionState, RestoreUnstampAndBytesRoundTrip) {
    sector_union u;
    uint32_t tick = 200;
    settle(u, tick);
    u.stamp(7, 3, 205);
    u.stamp(8, 4, 230);
    u.stamp(8, 5, 240);
    for (; tick <= 206; ++tick) u.tick(tick, false);
    ASSERT_EQ(u.active(), std::set<int>({1, 3}));
    const sector_union::state_type saved = u.state();
    EXPECT_EQ(saved.pending.size(), 2u);
    EXPECT_EQ(saved.origin.at(3).player, 7u);
    EXPECT_EQ(saved.last_tick, 206u);
    EXPECT_TRUE(saved.ticked);
    const uint64_t passes = u.passes();
    for (; tick <= 260; ++tick) u.tick(tick, false);
    EXPECT_EQ(u.active(), std::set<int>({1, 3, 4, 5}));
    u.restore(saved);
    EXPECT_EQ(u.state(), saved);
    EXPECT_EQ(u.passes(), passes + 2) << "the counters stay";
    EXPECT_EQ(u.unstamp(8, 4), 1u);
    EXPECT_EQ(u.unstamp(8, 4), 0u);
    EXPECT_EQ(u.stamps_pending(), 1u);
    EXPECT_TRUE(u.covers(3, 205));
    EXPECT_TRUE(u.covers(3, 300));
    EXPECT_FALSE(u.covers(3, 204)) << "requested later than this stamp";
    EXPECT_TRUE(u.covers(1, 0)) << "the level's own sector";
    EXPECT_TRUE(u.covers(5, 240));
    EXPECT_FALSE(u.covers(5, 239));
    EXPECT_FALSE(u.covers(4, 1000));

    const std::vector<uint8_t> bytes = sector_union::serialize(saved);
    // 1 + 4 + 1 + 1 + (1 + 2*4) + (1 + 2*4) + (1 + 12) + (2 + 2*12)
    EXPECT_EQ(bytes.size(), 64u);
    EXPECT_EQ(bytes[0], sector_union::kStateVersion);
    sector_union::state_type back;
    ASSERT_TRUE(sector_union::deserialize(bytes.data(), bytes.size(), back));
    EXPECT_EQ(back, saved);
    sector_union::state_type untouched;
    EXPECT_FALSE(sector_union::deserialize(bytes.data(), bytes.size() - 1, untouched));
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(sector_union::deserialize(longer.data(), longer.size(), untouched));
    std::vector<uint8_t> other = bytes;
    other[0] = 99;
    EXPECT_FALSE(sector_union::deserialize(other.data(), other.size(), untouched));
    EXPECT_EQ(untouched, sector_union::state_type{});
    EXPECT_TRUE(sector_union::deserialize(sector_union::serialize({}).data(), sector_union::serialize({}).size(), back));
    EXPECT_EQ(back, sector_union::state_type{});
}

// A late stamp re-derived from the record of its tick - 1 (the stamps from
// the log, the same manager activity) gives the on-time run's passes at the
// same ticks, and the same state at every tick after; a redundant late
// stamp changes nothing.
TEST(SectorUnionState, ALateStampReTickedFromItsRecordGivesTheOnTimePasses) {
    // The manager's activity: a pass keeps it busy 7 ticks.
    const auto run = [](sector_union& u, uint32_t from, uint32_t to, int& busy,
                        std::map<uint32_t, sector_union::state_type>& records, std::map<uint32_t, int>& passes) {
        for (uint32_t t = from; t <= to; ++t) {
            const auto p = u.tick(t, busy > 0);
            if (busy > 0) --busy;
            if (p) {
                passes[t] = p.sector;
                busy = 7;
            }
            records[t] = u.state();
        }
    };
    const std::vector<sector_union::stamped> log = {{410, 2, 3}, {404, 1, 2}, {450, 1, 4}};
    // On time: every stamp before its tick.
    sector_union on_time;
    std::map<uint32_t, sector_union::state_type> on_records;
    std::map<uint32_t, int> on_passes;
    int on_busy = 0;
    for (const auto& s: log) on_time.stamp(s.player, s.sector, s.tick);
    run(on_time, 400, 480, on_busy, on_records, on_passes);
    EXPECT_EQ(on_passes, (std::map<uint32_t, int>{{404, 2}, {414, 3}, {450, 4}}));
    // Late: the stamp for 404 arrives at 430; the others on time.
    sector_union late;
    std::map<uint32_t, sector_union::state_type> late_records;
    std::map<uint32_t, int> late_passes;
    int late_busy = 0;
    late.stamp(2, 3, 410);
    late.stamp(1, 4, 450);
    run(late, 400, 429, late_busy, late_records, late_passes);
    EXPECT_EQ(late_passes, (std::map<uint32_t, int>{{410, 3}}));
    // The re-derivation from 403 with the log's stamps.
    sector_union again;
    again.restore(sector_union::with_pending(late_records.at(403), log));
    int again_busy = 0;   // the manager as the record of 403 had it: idle
    std::map<uint32_t, int> again_passes;
    std::map<uint32_t, sector_union::state_type> again_records;
    run(again, 404, 480, again_busy, again_records, again_passes);
    EXPECT_EQ(again_passes, on_passes);
    for (uint32_t t = 404; t <= 480; ++t) {
        auto a = again_records.at(t), b = on_records.at(t);
        // The pending order is the log's (tick, sector, player), the live
        // union's is arrival: compare as sets.
        const auto key = [](const sector_union::stamped& s) { return std::make_tuple(s.tick, s.sector, s.player); };
        std::sort(a.pending.begin(), a.pending.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
        std::sort(b.pending.begin(), b.pending.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
        EXPECT_EQ(a, b) << "tick " << t;
    }
    // A redundant late stamp: covered, and stamped into the union it is a
    // no-op.
    sector_union redundant;
    redundant.restore(on_records.at(430));
    EXPECT_TRUE(redundant.covers(3, 420));
    const auto before = redundant.state();
    redundant.stamp(9, 3, 420);
    std::map<uint32_t, int> none;
    std::map<uint32_t, sector_union::state_type> ignored;
    int idle = 0;
    run(redundant, 431, 480, idle, ignored, none);
    EXPECT_EQ(none, (std::map<uint32_t, int>{{450, 4}}));
    EXPECT_EQ(redundant.active(), on_time.active());
    EXPECT_EQ(before.requested, on_records.at(430).requested);
}

// The merge (resync design 2.5): the state's own stamps are replaced by the
// log's whose sector the state has not requested - the stamps on both sides
// of the state's tick, in (tick, sector, player) order.
TEST(SectorUnionState, WithPendingTakesTheLogsStampsTheStateHasNotRequested) {
    sector_union server;
    uint32_t tick = 100;
    settle(server, tick);
    server.request(5, 2, tick);
    ASSERT_TRUE(server.tick(tick++, false));
    sector_union::state_type s = server.state();
    s.pending = {{999, 1, 7}};   // replaced
    const std::vector<sector_union::stamped> log = {
        {103, 5, 2},     // requested at S: dropped
        {140, 6, 4},     // after S
        {101, 7, 3},     // before S, not requested by the server yet (dequeued later)
        {140, 5, 3},     // the same sector again, later
        {120, 6, 0}};    // no sector
    const auto merged = sector_union::with_pending(s, log);
    ASSERT_EQ(merged.pending.size(), 3u);
    EXPECT_EQ(merged.pending[0].tick, 101u);
    EXPECT_EQ(merged.pending[0].sector, 3);
    EXPECT_EQ(merged.pending[1].tick, 140u);
    EXPECT_EQ(merged.pending[1].sector, 3);
    EXPECT_EQ(merged.pending[2].sector, 4);
    EXPECT_EQ(merged.active, s.active);
    EXPECT_EQ(merged.origin.size(), 1u);
    sector_union u;
    u.restore(merged);
    sector_union::pass p;
    for (int i = 0; i < 4 && !p; ++i) p = u.tick(tick++, false);   // the pacer after the server's pass
    ASSERT_TRUE(p);
    EXPECT_EQ(p.sector, 3);
    EXPECT_EQ(p.stamped_tick, 101u);
}

// The births a re-derivation moved (sector_union::birth_moved): attributed
// only to the live pass at M with M + 5 == b (its physicalize frame); moved
// when the re-derivation has no pass of that sector at M.  A body born at any
// other tick belongs to no pass: missing there, it stays a missing birth.
TEST(SectorUnionState, BirthAttributionToAMovedPass) {
    const std::map<uint32_t, int> live = {{120, 3}, {130, 4}};
    EXPECT_FALSE(sector_union::birth_moved(live, {}, 124)) << "before every pass's M + 5";
    EXPECT_TRUE(sector_union::birth_moved(live, {}, 125));
    EXPECT_FALSE(sector_union::birth_moved(live, {{120, 3}}, 125));
    EXPECT_FALSE(sector_union::birth_moved(live, {{120, 3}}, 133)) << "no pass physicalizes at 133";
    EXPECT_TRUE(sector_union::birth_moved(live, {{120, 3}}, 135));
    EXPECT_TRUE(sector_union::birth_moved(live, {{120, 3}, {130, 5}}, 135)) << "another sector there";
    EXPECT_FALSE(sector_union::birth_moved(live, {}, 140)) << "a birth after a moved pass is not its own";
    EXPECT_FALSE(sector_union::birth_moved(live, {}, 126)) << "nor one a tick after its physicalize";
    EXPECT_FALSE(sector_union::birth_moved({}, {}, 140));
}

// The re-derivation rule (sector_rederivation), the one the core's
// rewind_for_sectors / rederive_union_tick and SimTool's --late-sector-check
// both drive: the window's live passes moved out of the pass log, a late
// stamp's pass made at its tick, a live pass the manager is now busy for
// forgotten (its slot emptied) and made again once the pacer lets it, the
// birth of the moved pass attributed, through(now), and abandon putting the
// live passes back.
TEST(SectorUnionState, RederivationMovesForgetsAndAbandons) {
    using bmmo::session::sector_rederivation;
    // The record of tick 99: sector 2 ran at 90, the manager idle since.
    sector_union u;
    u.stamp(7, 2, 90);
    uint32_t last_pass = 0;
    const auto busy = [&last_pass](uint32_t t) { return last_pass && t - last_pass <= 7; };
    for (uint32_t t = 80; t <= 99; ++t)
        if (const auto p = u.tick(t, busy(t))) last_pass = t;
    ASSERT_EQ(last_pass, 90u);
    const sector_union::state_type record = u.state();
    std::map<uint32_t, int> pass_log = {{90, 2}, {103, 4}};
    // The log: 4 stamped on time at 103, 3 late at 100.
    const std::vector<sector_union::stamped> log = {{90, 7, 2}, {103, 8, 4}, {100, 9, 3}};

    sector_rederivation rd;
    rd.start(record, log, pass_log, 100);
    EXPECT_EQ(pass_log, (std::map<uint32_t, int>{{90, 2}}));
    EXPECT_EQ(rd.live_passes(), (std::map<uint32_t, int>{{103, 4}}));
    for (uint32_t t = 100; t <= 112; ++t) {
        const auto step = rd.tick(t, busy(t));
        if (step.made) {
            last_pass = t;
            pass_log[t] = step.made.sector;
        }
        if (t == 100) {
            EXPECT_EQ(step.made.sector, 3);
        } else if (t == 103) {
            EXPECT_FALSE(step.made) << "the manager runs 3's pass";
            EXPECT_TRUE(step.clear_slot);
            EXPECT_EQ(step.forgotten, 4);
        } else if (t == 110) {
            EXPECT_EQ(step.made.sector, 4) << "7 busy ticks, then 3 idle";
        } else {
            EXPECT_FALSE(step.made) << t;
            EXPECT_FALSE(step.clear_slot) << t;
            EXPECT_EQ(step.forgotten, 0) << t;
        }
    }
    EXPECT_TRUE(rd.through(112));
    EXPECT_FALSE(rd.through(113));
    EXPECT_EQ(rd.forgotten(), 1u);
    EXPECT_EQ(rd.passes(), (std::map<uint32_t, int>{{100, 3}, {110, 4}}));
    EXPECT_TRUE(rd.birth_moved(108)) << "the live pass of 4 at 103 physicalized at 108";
    EXPECT_FALSE(rd.birth_moved(105)) << "3's pass at 100 is re-derived where it was";
    EXPECT_FALSE(rd.birth_moved(115));
    EXPECT_EQ(rd.u().active(), (std::set<int>{1, 2, 3, 4}));

    // Refused: the live passes back, the re-derived ones the caller logged stay its own business.
    std::map<uint32_t, int> refused = {{90, 2}, {103, 4}};
    sector_rederivation again;
    again.start(record, log, refused, 100);
    again.abandon(refused);
    EXPECT_EQ(refused, (std::map<uint32_t, int>{{90, 2}, {103, 4}}));
    EXPECT_TRUE(again.live_passes().empty());
}

// A state the byte format cannot hold serializes to nothing (refused on
// reading), never to a truncated state that reads back as another one.
TEST(SectorUnionState, SerializeRefusesWhatTheFormatCannotHold) {
    sector_union::state_type big;
    for (int sector = 1; sector <= 256; ++sector) big.active.insert(sector);
    EXPECT_TRUE(sector_union::serialize(big).empty());
    sector_union::state_type stamps;
    stamps.pending.resize(0x10000);
    EXPECT_TRUE(sector_union::serialize(stamps).empty());
    sector_union::state_type back;
    const std::vector<uint8_t> none;
    EXPECT_FALSE(sector_union::deserialize(none.data(), none.size(), back));
    sector_union::state_type fits;
    for (int sector = 1; sector <= 255; ++sector) fits.active.insert(sector);
    const auto bytes = sector_union::serialize(fits);
    ASSERT_FALSE(bytes.empty());
    ASSERT_TRUE(sector_union::deserialize(bytes.data(), bytes.size(), back));
    EXPECT_EQ(back, fits);
}

// The resync's keep-local rule (session/resync_rules.hpp, resync design 2.4,
// step A1): the balloon's scripts always, the trafo scripts until step T2,
// nothing else - a module script, the manager, a name that only contains
// "PE_Balloon" further in.
// Step A3 (resync design 2.3, 2.6.3): the keep-local bodies, the travelling
// blob's renames, a window birth's pass and the reconcile's classes.
TEST(ResyncRules, KeepLocalBodiesRenamesAndTheReconcile) {
    using namespace bmmo::session;
    EXPECT_TRUE(keep_local_body("PE_Balloon_Platte01"));
    EXPECT_TRUE(keep_local_body("PE_Balloon_Ballon02"));
    EXPECT_TRUE(keep_local_body("PE_Box_slide")) << "the airship's slide box, joined to the platform";
    EXPECT_FALSE(keep_local_body("PE_Levelende"));
    EXPECT_FALSE(keep_local_body("P_Modul_08_Schaukel"));
    EXPECT_EQ(keep_local_body_prefixes(), (std::vector<std::string>{"PE_Balloon", "PE_Box_slide"}));
    // Renames: every player's server clone to this world's entity of that
    // player, nothing where the names agree or this world has none.
    const std::vector<std::pair<uint32_t, std::string>> clones = {
        {3, "Ball_Stone_BMMO_3"}, {4, "Ball_Wood_BMMO_4"}, {5, "Ball_Paper_BMMO_5"}};
    const std::map<uint32_t, std::string> local = {{3, "Ball_Stone"}, {4, "Ball_Wood_BMMO_4"}, {5, ""}};
    EXPECT_EQ(resync_renames(clones, local),
              (std::vector<std::pair<std::string, std::string>>{{"Ball_Stone_BMMO_3", "Ball_Stone"}}));
    // A birth's pass: M + 5 == birth first, else the newest before it.
    const std::map<uint32_t, int> passes = {{100, 2}, {130, 3}};
    uint32_t at = 0;
    EXPECT_EQ(attribute_birth(passes, 135, &at), 3);
    EXPECT_EQ(at, 130u);
    EXPECT_EQ(attribute_birth(passes, 120, &at), 2);
    EXPECT_EQ(at, 100u);
    EXPECT_EQ(attribute_birth(passes, 104), 0);
    // The classes, S = 125, the server's union {1, 2}.
    const std::set<int> server = {1, 2};
    EXPECT_EQ(classify_unrowed("PE_Balloon_Platte01", false, 135, 125, passes, server), resync_unrowed::keep_local);
    EXPECT_EQ(classify_unrowed("Ball_Wood_Piece01", true, 135, 125, passes, server), resync_unrowed::local_only);
    int sector = 0;
    EXPECT_EQ(classify_unrowed("P_Modul_03_Floor", false, 135, 125, passes, server, &sector), resync_unrowed::destroy)
        << "born after S by a pass the server's union lacks at S";
    EXPECT_EQ(sector, 3);
    EXPECT_EQ(classify_unrowed("P_Modul_01_Pusher", false, 120, 125, passes, server), resync_unrowed::unexplained)
        << "born before S, no row";
    EXPECT_EQ(classify_unrowed("P_X", false, 0, 125, passes, server), resync_unrowed::unexplained) << "no birth known";
    const std::map<uint32_t, int> early = {{130, 2}};
    EXPECT_EQ(classify_unrowed("P_Y", false, 135, 125, early, server), resync_unrowed::unexplained)
        << "its pass's sector the server has active: it should have a row";
    EXPECT_EQ(classify_unrowed("P_Z", false, 128, 125, std::map<uint32_t, int>{}, server), resync_unrowed::unattributed);
}

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
