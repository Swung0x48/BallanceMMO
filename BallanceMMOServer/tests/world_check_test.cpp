// Fork detection step FD1: the row terms (session/world_check.hpp) and the
// two messages (session_world_report_msg.hpp, session_world_fork_msg.hpp).
// The golden values are scripts/world_check.py's (its --self-test holds the
// same set): equal values mean every target and the mirror hash the same
// bytes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <session/world_check.hpp>
#include <message/session_resync_msg.hpp>
#include <message/session_resync_state_msg.hpp>
#include <message/session_world_fork_msg.hpp>
#include <message/session_world_report_msg.hpp>

#include "../sim/world_watch.hpp"

namespace {
    namespace wc = bmmo::session::world_check;
    using bmmo::session::body_kind;
    using bmmo::session::body_state;

    body_state row(body_kind kind, uint32_t owner, const char* name, double x, uint8_t flags = 3) {
        body_state r;
        r.kind = kind;
        r.owner = owner;
        r.name = name;
        r.position[0] = x; r.position[1] = -2.25; r.position[2] = 3.0;
        r.rotation[2] = 0.5; r.rotation[3] = 0.75;
        r.linear[0] = 0.5f; r.linear[2] = -1.0f;
        r.angular[1] = 0.25f;
        r.flags = flags;
        return r;
    }

    // world_check.py golden_rows.
    std::vector<body_state> golden_rows() {
        return {row(body_kind::Ball, 7, "Ball_Wood", 1.5),
                row(body_kind::Ball, 3, "Ball_Paper_BMMO_3", 4.0),
                row(body_kind::Ball, 3, "Ball_Stone_BMMO_3", 8.0),
                row(body_kind::Ball, 9, "Ball_Wood_BMMO_9", 16.0),
                row(body_kind::Ball, 0, "Ball_Wood_BMMO_x", 32.0),
                row(body_kind::Mechanism, 0, "P_Modul_18_MF002", 64.0, 1),
                row(body_kind::Mechanism, 1, "A01_Floor_01", 128.0),
                row(body_kind::Mechanism, 2, "PE_Balloon_01", 256.0)};
    }

    uint64_t ball_term(const wc::world_terms& t, uint32_t owner) {
        for (const auto& [o, term]: t.balls) if (o == owner) return term;
        return 0;
    }
}

TEST(WorldCheck, GoldenAndRules) {
    const auto rows = golden_rows();
    const auto t = wc::fold_rows(rows, {9});
    // The golden values (world_check.py GOLDEN).
    EXPECT_EQ(wc::row_term(rows[0]), 0x31a434fb392c477eULL);
    EXPECT_EQ(wc::row_term(rows[5]), 0x7b7c2f10981cc541ULL);
    EXPECT_EQ(t.rows, 5u) << "the latched ball, the unkeyed one and the balloon are out";
    EXPECT_EQ(t.unkeyed, 1u);
    EXPECT_EQ(t.dup, 1u) << "owner 3's two rows";
    ASSERT_EQ(t.balls.size(), 2u);
    EXPECT_EQ(t.balls[0].first, 3u);
    EXPECT_EQ(t.balls[0].second, 0xb58918358b633052ULL);
    EXPECT_EQ(t.balls[1].first, 7u);
    EXPECT_EQ(t.balls[1].second, 0x31a434fb392c477eULL);
    EXPECT_EQ(wc::bucket_of("P_Modul_18_MF002"), 3);
    EXPECT_EQ(wc::bucket_of("A01_Floor_01"), 0);
    EXPECT_EQ(wc::terms_fold(t), 0xc613d8425945a175ULL);
    EXPECT_EQ(wc::position_fold(rows), 0x1c69ed6087231e41ULL);

    // Row order does not matter.
    auto reversed = rows;
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_EQ(wc::terms_fold(wc::fold_rows(reversed, {9})), wc::terms_fold(t));
    EXPECT_EQ(wc::position_fold(reversed), wc::position_fold(rows));

    // Each field flips its own class only; the keep-local body none.
    auto flip = [&](auto change) {
        auto changed = rows;
        change(changed);
        return wc::fold_rows(changed, {9});
    };
    const auto lin = flip([](auto& r) { r[0].linear[1] = 1e-30f; });
    EXPECT_NE(ball_term(lin, 7), ball_term(t, 7));
    EXPECT_EQ(ball_term(lin, 3), ball_term(t, 3));
    EXPECT_TRUE(std::equal(std::begin(lin.buckets), std::end(lin.buckets), std::begin(t.buckets)));
    EXPECT_NE(ball_term(flip([](auto& r) { r[0].flags = 1; }), 7), ball_term(t, 7));
    EXPECT_NE(ball_term(flip([](auto& r) { r[0].position[2] = std::nextafter(3.0, 4.0); }), 7), ball_term(t, 7));
    EXPECT_NE(ball_term(flip([](auto& r) { r[0].rotation[0] = -0.0; }), 7), ball_term(t, 7)) << "-0.0 differs, as for the judge";
    const auto mech = flip([](auto& r) { r[5].angular[0] = 2.0f; });
    EXPECT_NE(mech.buckets[3], t.buckets[3]);
    EXPECT_EQ(mech.buckets[0], t.buckets[0]);
    EXPECT_EQ(mech.balls, t.balls);
    EXPECT_EQ(wc::terms_fold(flip([](auto& r) { r[7].position[0] = 1.0; })), wc::terms_fold(t)) << "keep-local";
    EXPECT_EQ(wc::terms_fold(flip([](auto& r) { r[3].position[0] = 1.0; })), wc::terms_fold(t)) << "latched";
    EXPECT_EQ(wc::terms_fold(flip([](auto& r) { r[4].position[0] = 1.0; })), wc::terms_fold(t)) << "unkeyed";

    // The compare: equal; the own ball; another ball; a bucket; a ball one
    // side lacks; the latched sets.
    EXPECT_FALSE(wc::compare_terms(t, t, {9}, {9}, 7).differs());
    auto own = wc::compare_terms(t, lin, {9}, {9}, 7);
    EXPECT_EQ(own.kinds, wc::KIND_OWN);
    EXPECT_EQ(own.owners, std::vector<uint32_t>{7});
    EXPECT_EQ(wc::compare_terms(t, lin, {9}, {9}, 3).kinds, wc::KIND_BALL);
    const auto bucket = wc::compare_terms(t, mech, {9}, {9}, 7);
    EXPECT_EQ(bucket.kinds, wc::KIND_BUCKET);
    EXPECT_EQ(bucket.buckets, 1u << 3);
    auto lacking = rows;
    lacking.erase(lacking.begin());   // no ball of owner 7
    const auto lacks = wc::fold_rows(lacking, {9});
    const auto extra = wc::compare_terms(lacks, t, {9}, {9}, 3);   // the server lacks it
    EXPECT_EQ(extra.kinds, wc::KIND_BALL | wc::KIND_SET);
    EXPECT_EQ(extra.owners, std::vector<uint32_t>{7});
    EXPECT_FALSE(wc::compare_terms(t, lacks, {9}, {9}, 3).differs())
        << "the report lacks it but has an unkeyed row: not compared";
    lacking.erase(lacking.begin() + 3);   // and no unkeyed row either
    EXPECT_EQ(wc::compare_terms(t, wc::fold_rows(lacking, {9}), {9}, {9}, 3).kinds, wc::KIND_BALL | wc::KIND_SET);
    // Latched on the server only: a zone difference, its ball not compared.
    const auto zone = wc::compare_terms(wc::fold_rows(rows, {7, 9}), wc::fold_rows(rows, {9}), {7, 9}, {9}, 3);
    EXPECT_EQ(zone.kinds, wc::KIND_ZONE);
    EXPECT_EQ(zone.owners, std::vector<uint32_t>{7});
}

TEST(SessionWorldMsgs, RoundTrip) {
    bmmo::session_world_report_msg report;
    report.session = 4; report.epoch = 120; report.tick = 330;
    report.flags = bmmo::session_world_report_msg::FLAG_UNCONFIRMED;
    report.why = 2; report.rows = 5; report.unkeyed = 1; report.dup = 1;
    report.balls = {{3, 0x1111}, {7, 0x2222}};
    report.buckets[3] = 0x3333;
    report.latched = {9};
    // Schema 2 (step FD3b): the heal's fields, HEAL_MISSED's two.
    report.flags |= bmmo::session_world_report_msg::FLAG_HEAL_MISSED;
    report.heal_x = 264; report.held = {198, 0xABCD, true}; report.last_full = 66; report.k = 8;
    report.missed_x = 330; report.missed = static_cast<uint8_t>(bmmo::session_world_report_msg::missed_reason::light);
    auto round = [](bmmo::session_world_report_msg& out, const std::string& bytes) {
        out.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return out.deserialize();
    };
    report.serialize();
    {
        bmmo::session_world_report_msg back;
        ASSERT_TRUE(round(back, report.raw.str()));
        EXPECT_EQ(back.tick, 330u);
        EXPECT_EQ(back.balls, report.balls);
        EXPECT_EQ(back.buckets[3], 0x3333u);
        EXPECT_EQ(back.latched, report.latched);
        EXPECT_EQ(back.heal_x, 264u);
        EXPECT_TRUE(back.held.held);
        EXPECT_EQ(back.held.check, 0xABCDu);
        EXPECT_EQ(back.k, 8);
        EXPECT_EQ(back.missed_x, 330u);
        EXPECT_STREQ(bmmo::session_world_report_msg::missed_name(back.missed), "light");
        EXPECT_TRUE(back.body_rows.empty());
        EXPECT_TRUE(back.folds.empty());
        // A trailing byte, or another schema, is refused.
        bmmo::session_world_report_msg longer;
        EXPECT_FALSE(round(longer, report.raw.str() + "x"));
        std::string other = report.raw.str();
        other[sizeof(bmmo::opcode)] = 1;
        bmmo::session_world_report_msg schema;
        EXPECT_FALSE(round(schema, other));
    }
    bmmo::session_world_report_msg detail;
    detail.flags = bmmo::session_world_report_msg::FLAG_DETAIL;
    detail.tick = 396;
    detail.body_rows = golden_rows();
    detail.episode = 2; detail.fold_first = 300;
    detail.folds = {{1, 2}, {3, 4}};
    detail.rewinds = {{390, 400, 6, 10, "Ball_Wood_BMMO_4"}};
    detail.resyncs = 2;
    detail.serialize();
    {
        bmmo::session_world_report_msg back;
        ASSERT_TRUE(round(back, detail.raw.str()));
        EXPECT_NE(back.flags & bmmo::session_world_report_msg::FLAG_ROWS, 0);
        ASSERT_EQ(back.body_rows.size(), golden_rows().size());
        EXPECT_EQ(wc::terms_fold(wc::fold_rows(back.body_rows, {})), wc::terms_fold(wc::fold_rows(golden_rows(), {})));
        EXPECT_EQ(back.body_rows[5].name, "P_Modul_18_MF002");
        EXPECT_EQ(back.episode, 2u);
        EXPECT_EQ(back.fold_first, 300u);
        EXPECT_EQ(back.folds, detail.folds);
        ASSERT_EQ(back.rewinds.size(), 1u);
        EXPECT_EQ(back.rewinds[0].entity, "Ball_Wood_BMMO_4");
        EXPECT_EQ(back.rewinds[0].depth, 10u);
        EXPECT_EQ(back.resyncs, 2u);
        // A count past the bytes left is refused, not allocated.
        std::string cut = detail.raw.str();
        cut.resize(cut.size() - 20);
        bmmo::session_world_report_msg truncated;
        EXPECT_FALSE(round(truncated, cut));
    }

    bmmo::session_world_fork_msg fork;
    fork.session = 4; fork.episode = 2; fork.tick = 396;
    fork.action = static_cast<uint8_t>(wc::fork_action::decided);
    fork.kinds = wc::KIND_BALL; fork.owners = {7}; fork.buckets = 8;
    fork.tier = static_cast<uint8_t>(wc::fork_tier::now);
    fork.rule = static_cast<uint8_t>(wc::fork_rule::size);
    fork.tick2 = 400; fork.differing = 1; fork.worst_m = 3.18f; fork.own_m = 0.0f; fork.worst = "p7";
    fork.serialize();
    bmmo::session_world_fork_msg back;
    back.raw.write(fork.raw.str().data(), static_cast<std::streamsize>(fork.raw.str().size()));
    ASSERT_TRUE(back.deserialize());
    EXPECT_EQ(back.owners, fork.owners);
    EXPECT_EQ(back.tick2, 400u);
    EXPECT_EQ(back.worst, "p7");
    EXPECT_FLOAT_EQ(back.worst_m, 3.18f);
    std::string other = fork.raw.str();
    other[sizeof(bmmo::opcode)] = 9;
    bmmo::session_world_fork_msg schema;
    schema.raw.write(other.data(), static_cast<std::streamsize>(other.size()));
    EXPECT_FALSE(schema.deserialize());

    // Step FD3b: the new actions and outcomes have names; IN_PLACE and the
    // episode on SessionResyncState, the reason byte on SessionResync.
    EXPECT_STREQ(wc::action_name(wc::fork_action::kick), "kick");
    EXPECT_STREQ(wc::outcome_name(wc::fork_outcome::heal_failed), "heal_failed");
    bmmo::session_resync_state_msg state;
    state.session = 4; state.tick = 1056; state.base = 120; state.raw_size = 3;
    state.flags = bmmo::session_resync_state_msg::FLAG_IN_PLACE; state.episode = 2;
    state.frame = {1, 2, 3};
    state.serialize();
    bmmo::session_resync_state_msg state_back;
    state_back.raw.write(state.raw.str().data(), static_cast<std::streamsize>(state.raw.str().size()));
    ASSERT_TRUE(state_back.deserialize());
    EXPECT_EQ(state_back.flags, bmmo::session_resync_state_msg::FLAG_IN_PLACE);
    EXPECT_EQ(state_back.episode, 2u);
    EXPECT_EQ(state_back.frame, state.frame);
    bmmo::session_resync_msg resync;
    resync.session = 4; resync.last_full_tick = 66;
    resync.why = static_cast<uint8_t>(bmmo::session_resync_msg::reason::own_life);
    resync.serialize();
    bmmo::session_resync_msg resync_back;
    resync_back.raw.write(resync.raw.str().data(), static_cast<std::streamsize>(resync.raw.str().size()));
    ASSERT_TRUE(resync_back.deserialize());
    EXPECT_STREQ(bmmo::session_resync_msg::reason_name(resync_back.why), "own_life");
}

// Step FD1, the server's half (sim/world_watch.hpp): scripted samples,
// reports and steps, no clock.  Member 1 reports; player 2 owns a ball.
TEST(WorldWatch, Episodes) {
    using bmmo::sim::world_report_in;
    using bmmo::sim::world_watch;
    using bmmo::session::stat_kind;
    struct rec { stat_kind kind; std::vector<std::string> v; };
    std::vector<rec> records;
    std::vector<world_watch::notice> notices;
    std::vector<uint8_t> row_flags;
    world_watch::sink sink;
    sink.record = [&](stat_kind kind, std::initializer_list<bmmo::session::journal_value> values) {
        rec r{kind, {}};
        for (const auto& x: values)
            r.v.push_back(x.type == 's' ? std::string(x.s) : x.type == 'f' ? std::to_string(x.f) : std::to_string(x.i));
        records.push_back(std::move(r));
    };
    sink.rows = [&](uint32_t, uint8_t flags, const std::vector<body_state>&, uint32_t) { row_flags.push_back(flags); };
    sink.out = [&](const world_watch::notice& n) { notices.push_back(n); };
    sink.calm = [](uint32_t) { return 0; };
    world_watch w(sink);
    const auto count = [&](stat_kind kind, size_t field, const std::string& value) {
        return std::count_if(records.begin(), records.end(), [&](const rec& r) { return r.kind == kind && r.v[field] == value; });
    };
    const std::vector<body_state> server = {row(body_kind::Ball, 1, "Ball_Wood_BMMO_1", 0.0),
                                            row(body_kind::Ball, 2, "Ball_Wood_BMMO_2", 20.0),
                                            row(body_kind::Mechanism, 5, "A01_Floor_01", 200.0)};
    const auto moved = [&](size_t i, double dx) {
        auto rows = server;
        rows[i].position[0] += dx;
        return rows;
    };
    const auto report = [&](uint32_t tick, const std::vector<body_state>& rows, uint32_t now, uint32_t epoch = 100) {
        world_report_in r;
        r.player = 1;
        r.epoch = epoch;
        r.tick = tick;
        r.terms = wc::fold_rows(rows, {});
        w.report(r, now);
    };
    const auto detail = [&](uint32_t tick, const std::vector<body_state>& rows, uint32_t now) {
        world_report_in r;
        r.player = 1;
        r.epoch = 100;
        r.tick = tick;
        r.flags = bmmo::session_world_report_msg::FLAG_DETAIL;
        r.terms = wc::fold_rows(rows, {});
        r.rows = rows;
        r.episode = notices.back().episode;
        w.report(r, now);
    };
    constexpr uint32_t caps = bmmo::session::CAP_WORLD_CHECK | bmmo::session::CAP_RESYNC_STATE;
    w.member(1, caps, "a", 100);
    w.member(3, bmmo::session::CAP_RESYNC_STATE, "c", 100);
    EXPECT_EQ(count(stat_kind::fork_blind, 1, "no_capability"), 1);
    w.ready(1, 100, 100);
    ASSERT_TRUE(w.active());

    // Equal: no episode, nothing sent.
    w.sample(132, server, {}, false, 133);
    report(132, server, 140);
    EXPECT_EQ(count(stat_kind::world_agree, 3, "equal"), 1);
    // A report before its tick is stepped waits for the sample.
    report(198, moved(1, 3.0), 190);
    EXPECT_EQ(count(stat_kind::world_agree, 3, "differs"), 0);
    // A remote ball 3 m off: opened at the sample, decided NOW at the DETAIL (final at once).
    w.sample(198, server, {}, false, 199);
    ASSERT_EQ(notices.back().action, wc::fork_action::opened);
    EXPECT_EQ(row_flags.back(), bmmo::session::JOURNAL_CHECKPOINT_FULL | bmmo::session::JOURNAL_CHECKPOINT_CHECK);
    detail(198, moved(1, 3.0), 210);
    EXPECT_EQ(row_flags.back(), bmmo::session::JOURNAL_CHECKPOINT_LOCAL | bmmo::session::JOURNAL_CHECKPOINT_FINAL
                                | bmmo::session::JOURNAL_CHECKPOINT_CHECK);
    auto decided = std::find_if(notices.rbegin(), notices.rend(), [](const auto& n) { return n.action == wc::fork_action::decided; });
    ASSERT_NE(decided, notices.rend());
    EXPECT_EQ(decided->tier, wc::fork_tier::now);
    EXPECT_EQ(decided->rule, wc::fork_rule::size);
    EXPECT_EQ(decided->worst, "p2");
    EXPECT_NEAR(decided->worst_m, 3.0, 1e-6);
    EXPECT_EQ(count(stat_kind::fork_decision, 7, "1"), 1) << "the NOW would-heal is final at once";
    w.sample(264, server, {}, false, 265);
    report(264, server, 300);
    EXPECT_EQ(notices.back().action, wc::fork_action::closed);
    EXPECT_EQ(notices.back().outcome, wc::fork_outcome::equal_again);

    // The own ball 3 cm off: SOON, held to the fixed gap after the last would-heal (210 + 660).
    w.sample(330, server, {}, false, 331);
    report(330, moved(0, 0.03), 420);
    detail(330, moved(0, 0.03), 425);
    decided = std::find_if(notices.rbegin(), notices.rend(), [](const auto& n) { return n.action == wc::fork_action::decided; });
    EXPECT_EQ(decided->episode, 2u);
    EXPECT_EQ(decided->tier, wc::fork_tier::soon);
    EXPECT_EQ(decided->rule, wc::fork_rule::own);
    EXPECT_EQ(decided->tick2, 870u);
    w.sample(396, server, {}, false, 397);
    report(396, server, 480);
    EXPECT_EQ(notices.back().outcome, wc::fork_outcome::self_healed);

    // A body only the server has, for one sample: a lead, no decision sent.
    const std::vector<body_state> without(server.begin(), server.begin() + 2);
    w.sample(462, server, {}, false, 463);
    report(462, without, 560);
    detail(462, without, 565);
    EXPECT_EQ(count(stat_kind::fork_decision, 4, "first_presence"), 1);
    w.sample(528, server, {}, false, 529);
    report(528, server, 600);
    EXPECT_EQ(notices.back().outcome, wc::fork_outcome::lead);
    EXPECT_NE(notices.back().line.find("lead"), std::string::npos);

    // No DETAIL in 66 ticks: held as the first sample (no_detail).
    w.sample(594, server, {}, false, 595);
    report(594, moved(1, 3.0), 700);
    w.step(766);
    EXPECT_EQ(count(stat_kind::fork_decision, 4, "no_detail"), 1);
    w.sample(660, server, {}, false, 661);
    report(660, server, 800);
    EXPECT_EQ(notices.back().outcome, wc::fork_outcome::lead);

    // An older epoch and a sample that left the terms ring are not compared.
    report(660, server, 801, 50);
    EXPECT_EQ(count(stat_kind::world_agree, 3, "old_epoch"), 1);
    report(132, server, 802);
    EXPECT_EQ(count(stat_kind::world_agree, 3, "stale"), 0) << "still in the ring";
    // Silent for 660 ticks: one blind record and line, then its end.
    w.step(1461);
    EXPECT_EQ(count(stat_kind::fork_blind, 1, "silent"), 0);
    w.step(1462);
    EXPECT_EQ(count(stat_kind::fork_blind, 1, "silent"), 1);
    w.step(1463);
    EXPECT_EQ(count(stat_kind::fork_blind, 1, "silent"), 1) << "once per stretch";
    w.sample(1452, server, {}, false, 1464);
    report(1452, server, 1470);
    EXPECT_EQ(count(stat_kind::fork_blind, 4, "1"), 1);
    report(726, server, 1471);   // never sampled
    EXPECT_EQ(count(stat_kind::world_agree, 3, "stale"), 1);
    // A distant mechanism 1 mm off: LATER, by the detection + 396.
    w.sample(1518, server, {}, false, 1519);
    report(1518, moved(2, 0.001), 1530);
    detail(1518, moved(2, 0.001), 1535);
    EXPECT_EQ(notices.back().tier, wc::fork_tier::later);
    EXPECT_EQ(notices.back().rule, wc::fork_rule::distant);
    EXPECT_EQ(notices.back().tick2, 1530u + 396u);
    w.end(1600);
    EXPECT_EQ(notices[notices.size() - 2].outcome, wc::fork_outcome::ended);
    ASSERT_EQ(records.back().kind, stat_kind::world_check_end);
    EXPECT_EQ(records.back().v[4], "5") << "episodes";
    EXPECT_EQ(records.back().v[5], "2") << "leads";
}

// Step FD3b (15.5): the heal on - one capture a tick at X % k == 0, verify
// (healed resets the count, a heal that did not hold adds one unless another
// member reports the same term: the user's Q9), the retry at the fixed gap,
// the kick instead of a 7th, HEAL_MISSED's fallback re-assign and the
// reopen.  Members 1 and 4 report; the mechanism is 3 m off (NOW).
TEST(WorldWatch, Heals) {
    using bmmo::sim::world_report_in;
    using bmmo::sim::world_watch;
    using bmmo::session::stat_kind;
    struct rec { stat_kind kind; std::vector<std::string> v; };
    std::vector<rec> records;
    std::vector<world_watch::notice> notices;
    world_watch::sink sink;
    sink.record = [&](stat_kind kind, std::initializer_list<bmmo::session::journal_value> values) {
        rec r{kind, {}};
        for (const auto& x: values)
            r.v.push_back(x.type == 's' ? std::string(x.s) : x.type == 'f' ? std::to_string(x.f) : std::to_string(x.i));
        records.push_back(std::move(r));
    };
    sink.out = [&](const world_watch::notice& n) { notices.push_back(n); };
    sink.calm = [](uint32_t) { return 0; };
    uint32_t reassign_base = 0;   // what the server's re-assign gives (0: 7.2's guard holds it back)
    sink.reassign = [&](uint32_t, uint32_t, uint32_t, bool) { return reassign_base; };
    world_watch w(sink);
    w.set_heal(true);
    const auto count = [&](stat_kind kind, size_t field, const std::string& value) {
        return std::count_if(records.begin(), records.end(), [&](const rec& r) { return r.kind == kind && r.v[field] == value; });
    };
    const auto last = [&](stat_kind kind) {
        for (auto it = records.rbegin(); it != records.rend(); ++it) if (it->kind == kind) return it->v;
        return std::vector<std::string>{};
    };
    const std::vector<body_state> server = {row(body_kind::Ball, 1, "Ball_Wood_BMMO_1", 0.0),
                                            row(body_kind::Ball, 4, "Ball_Wood_BMMO_4", 20.0),
                                            row(body_kind::Mechanism, 5, "A01_Floor_01", 2.0)};
    auto forked = server;
    forked[2].position[0] += 3.0;
    uint32_t heal_x = 0;
    const auto report = [&](uint32_t player, uint32_t tick, const std::vector<body_state>& rows, uint32_t now,
                            uint8_t flags = 0, uint32_t missed_x = 0) {
        world_report_in r;
        r.player = player;
        r.epoch = 100;
        r.tick = tick;
        r.flags = flags;
        r.terms = wc::fold_rows(rows, {});
        if (flags & bmmo::session_world_report_msg::FLAG_DETAIL) {
            r.rows = rows;
            r.episode = notices.back().episode;
        }
        r.heal_x = player == 1 ? heal_x : 0;
        r.k = 8;
        r.missed_x = missed_x;
        w.report(r, now);
    };
    const auto none = [](uint32_t) { return false; };
    constexpr uint32_t caps = bmmo::session::CAP_WORLD_CHECK | bmmo::session::CAP_RESYNC_STATE;
    w.member(1, caps, "a", 100);
    w.member(4, caps, "d", 100);
    w.ready(1, 100, 100);
    w.ready(4, 100, 100);
    constexpr uint8_t kDetail = bmmo::session_world_report_msg::FLAG_DETAIL;

    // Opened and decided NOW: queued; captured at the first tick % k == 0.
    w.sample(198, server, {}, false, 199);
    report(4, 198, server, 205);
    report(1, 198, forked, 205);
    report(1, 198, forked, 210, kDetail);
    w.step(211);
    EXPECT_FALSE(w.next_heal(212, false, none)) << "212 % 8 != 0";
    EXPECT_FALSE(w.next_heal(216, true, none)) << "another state went out at 216";
    auto order = w.next_heal(216, false, none);
    ASSERT_TRUE(order);
    EXPECT_EQ(order->player, 1u);
    w.heal_sent(*order, 216, 2048, false, 217);
    EXPECT_FALSE(w.next_heal(224, false, none)) << "one heal, one capture";
    EXPECT_EQ(notices.back().action, wc::fork_action::healing);
    EXPECT_EQ(notices.back().tick2, 216u);
    // Verified: equal after X - healed, closed.
    heal_x = 216;
    w.sample(264, server, {}, false, 265);
    report(4, 264, server, 270);
    report(1, 264, server, 270);
    EXPECT_EQ(count(stat_kind::fork_verify, 4, "healed"), 1);
    EXPECT_EQ(notices.back().outcome, wc::fork_outcome::healed);

    // Differs again with the key in common: reopened (same id), and the
    // next heal waits for the fixed gap (217 + 660).
    w.sample(330, server, {}, false, 331);
    report(4, 330, server, 340);
    report(1, 330, forked, 340);
    report(1, 330, forked, 345, kDetail);
    EXPECT_EQ(count(stat_kind::fork_reopen, 1, "1"), 1);
    w.step(876);
    EXPECT_FALSE(w.heal_queued()) << "the 10 s gap";
    w.step(877);
    ASSERT_TRUE(w.heal_queued());
    // Six heals that did not hold, each 660 ticks after the last, then the
    // kick instead of a 7th.
    uint32_t x = 880, now = 881;
    for (int i = 1; i <= 6; ++i) {
        order = w.next_heal(x, false, none);
        ASSERT_TRUE(order) << i;
        w.heal_sent(*order, x, 2048, true, now);
        heal_x = x;
        const uint32_t t = (x / 66 + 1) * 66;
        w.sample(t, server, {}, false, t + 1);
        report(4, t, server, t + 5);
        report(1, t, forked, t + 5);
        EXPECT_EQ(last(stat_kind::fork_verify)[4], "heal_failed") << i;
        EXPECT_EQ(last(stat_kind::fork_verify)[7], "1") << "counted: member 4 agrees with the server";
        EXPECT_EQ(last(stat_kind::fork_verify)[8], std::to_string(i));
        EXPECT_EQ(notices.back().action, wc::fork_action::verified);
        now += 660;
        x = now - 1;
        while (x % 8) ++x;
        w.step(now);
        if (i < 6) EXPECT_TRUE(w.heal_queued()) << i;
    }
    EXPECT_FALSE(w.heal_queued()) << "no 7th heal";
    EXPECT_EQ(count(stat_kind::fork_kick, 5, "6"), 1);
    EXPECT_EQ(notices.back().action, wc::fork_action::kick);
    EXPECT_TRUE(notices.back().kick);
    w.leave(1, now + 2);
    EXPECT_EQ(last(stat_kind::fork_close)[2], "ended");

    // A rejoin is a fresh member (count 0).  Both members forked the same
    // way: the heal that does not hold is shared, not counted (Q9).
    w.member(1, caps, "a", now + 10);
    w.ready(1, 100, now + 10);
    uint32_t t = (now / 66 + 2) * 66;
    w.sample(t, server, {}, false, t + 1);
    report(4, t, forked, t + 5);
    report(1, t, forked, t + 5);
    report(1, t, forked, t + 6, kDetail);
    w.step(t + 7);
    x = t + 8;
    while (x % 8) ++x;
    order = w.next_heal(x, false, none);
    ASSERT_TRUE(order);
    EXPECT_EQ(order->player, 1u) << "decided first";
    w.heal_sent(*order, x, 2048, false, x + 1);
    heal_x = x;
    t = (x / 66 + 1) * 66;
    w.sample(t, server, {}, false, t + 1);
    report(1, t, forked, t + 5);
    EXPECT_EQ(count(stat_kind::fork_verify, 4, "heal_failed"), 6) << "waits for member 4's report of T";
    report(4, t, forked, t + 6);
    EXPECT_EQ(count(stat_kind::fork_verify, 4, "heal_failed"), 7);
    EXPECT_EQ(last(stat_kind::fork_verify)[7], "0") << "shared";
    EXPECT_EQ(last(stat_kind::fork_verify)[8], "0");

    // HEAL_MISSED: the fallback re-assign, verified from its base.
    w.step(x + 1 + 660);
    x = x + 1 + 660;
    while (x % 8) ++x;
    order = w.next_heal(x, false, none);
    ASSERT_TRUE(order);
    w.heal_sent(*order, x, 2048, false, x + 1);
    const uint32_t s = (x / 66 + 1) * 66;
    w.sample(s, server, {}, false, s + 1);
    report(1, s, forked, s + 5, bmmo::session_world_report_msg::FLAG_HEAL_MISSED, x);
    EXPECT_EQ(count(stat_kind::fork_heal, 2, "reassign"), 0) << "held back by the guard";
    reassign_base = s + 40;
    w.step(s + 6);   // asked again
    EXPECT_EQ(count(stat_kind::fork_heal, 2, "reassign"), 1);
    EXPECT_EQ(last(stat_kind::fork_heal)[3], std::to_string(s + 39)) << "X: the base - 1";
    w.ready(1, s + 40, s + 7);
    EXPECT_NE(last(stat_kind::fork_close)[2], "superseded") << "its own re-assign";

    // A ball whose owner had a lifecycle event in the server's world after X
    // differs again: re_fork, count 0 (definition B), healed again at the gap.
    world_watch rf(sink);
    rf.set_heal(true);
    rf.member(1, caps, "a", 100);
    rf.ready(1, 100, 100);
    auto ball = server;
    ball[1].position[0] += 2.0;   // player 4's ball, 2 m
    const auto rf_report = [&](uint32_t tick, const std::vector<body_state>& rows, uint32_t now, uint8_t flags, uint32_t hx) {
        world_report_in r;
        r.player = 1; r.epoch = 100; r.tick = tick; r.flags = flags; r.k = 8; r.heal_x = hx;
        r.terms = wc::fold_rows(rows, {});
        if (flags & kDetail) { r.rows = rows; r.episode = notices.back().episode; }
        rf.report(r, now);
    };
    rf.sample(198, server, {}, false, 199);
    rf_report(198, ball, 205, 0, 0);
    rf_report(198, ball, 206, kDetail, 0);
    order = rf.next_heal(208, false, none);
    ASSERT_TRUE(order);
    rf.heal_sent(*order, 208, 2048, false, 209);
    rf.life(4, 200);   // before X: not this heal's
    rf.life(4, 230);   // its release (or birth, or zone entry) after X
    rf.life(4, 280);   // after T: not this sample's
    rf.sample(264, server, {}, false, 265);
    rf_report(264, ball, 270, 0, 208);
    EXPECT_EQ(last(stat_kind::fork_verify)[4], "re_fork");
    EXPECT_EQ(last(stat_kind::fork_verify)[8], "0");
    EXPECT_EQ(last(stat_kind::fork_verify)[9], "p4@230");

    // A heal the client applied (its report says heal_x >= X) whose verify
    // waits - the episode's ball latched at the sample - gets no fallback
    // after 660 ticks.  A re-assign of an undecided episode leaves it open
    // (FD1's).  A state the client dropped twice falls back.
    world_watch d(sink);
    d.set_heal(true);
    d.member(1, caps, "a", 100);
    d.ready(1, 100, 100);
    const auto d_report = [&](uint32_t tick, const std::vector<body_state>& rows, uint32_t now, uint8_t flags, uint32_t hx,
                              std::vector<uint32_t> latched = {}, uint8_t missed = 0, uint32_t missed_x = 0) {
        world_report_in r;
        r.player = 1; r.epoch = 100; r.tick = tick; r.flags = flags; r.k = 8; r.heal_x = hx;
        r.latched = latched; r.missed = missed; r.missed_x = missed_x;
        r.terms = wc::fold_rows(rows, r.latched);
        if (flags & kDetail) { r.rows = rows; r.episode = notices.back().episode; }
        d.report(r, now);
    };
    const auto closes = [&] {
        return std::count_if(records.begin(), records.end(), [](const rec& r) { return r.kind == stat_kind::fork_close; });
    };
    d.sample(198, server, {}, false, 199);
    d_report(198, ball, 205, 0, 0);
    const auto closed_before = closes();
    d.ready(1, 100, 206);
    EXPECT_EQ(closes(), closed_before) << "undecided: not superseded";
    d_report(198, ball, 207, kDetail, 0);
    order = d.next_heal(208, false, none);
    ASSERT_TRUE(order);
    d.heal_sent(*order, 208, 2048, false, 209);
    d.sample(264, server, {4}, false, 265);
    d_report(264, server, 270, 0, 208, {4});
    const auto reassigns = count(stat_kind::fork_heal, 2, "reassign");
    reassign_base = 2000;
    d.step(209 + 660);
    EXPECT_EQ(count(stat_kind::fork_heal, 2, "reassign"), reassigns) << "seen: no fallback";
    // Compared and equal: healed.  Then a state dropped twice: the second falls back.
    d.sample(330, server, {}, false, 331);
    d_report(330, server, 340, 0, 208);
    EXPECT_EQ(last(stat_kind::fork_verify)[4], "healed");
    d.sample(396, server, {}, false, 397);
    d_report(396, ball, 400, 0, 208);
    d_report(396, ball, 401, kDetail, 208);
    const auto drop = [&](uint32_t at) {
        d.step(at);
        uint32_t hx = at;
        while (hx % 8) ++hx;
        auto o = d.next_heal(hx, false, none);
        ASSERT_TRUE(o) << at;
        d.heal_sent(*o, hx, 2048, false, hx + 1);
        d_report(396, ball, hx + 2, bmmo::session_world_report_msg::FLAG_HEAL_MISSED, 208, {},
                 static_cast<uint8_t>(bmmo::session_world_report_msg::missed_reason::resync_pending), hx);
    };
    drop(209 + 660);
    EXPECT_EQ(count(stat_kind::fork_heal, 2, "reassign"), reassigns) << "the first drop: healed again at the gap";
    drop(209 + 2 * 660 + 16);
    EXPECT_EQ(count(stat_kind::fork_heal, 2, "reassign"), reassigns + 1) << "the second drop falls back";

    // Two heals that did not hold, then the worlds equal again by themselves
    // (equal_again keeps the count): every kDecayTicks (60 s) of equal samples take one off,
    // down to 0 (the user, 2026-11-16).
    world_watch dc(sink);
    dc.set_heal(true);
    dc.member(1, caps, "a", 100);
    dc.ready(1, 100, 100);
    const auto dc_report = [&](uint32_t tick, const std::vector<body_state>& rows, uint32_t now, uint8_t flags, uint32_t hx) {
        world_report_in r;
        r.player = 1; r.epoch = 100; r.tick = tick; r.flags = flags; r.k = 8; r.heal_x = hx;
        r.terms = wc::fold_rows(rows, {});
        if (flags & kDetail) { r.rows = rows; r.episode = notices.back().episode; }
        dc.report(r, now);
    };
    constexpr uint32_t S = wc::kSampleTicks, D = world_watch::kDecayTicks, G = world_watch::kGapTicks;
    const auto k8 = [](uint32_t t) { return (t + 7) / 8 * 8; };   // the first capture tick at or after t (k = 8)
    const uint32_t t0 = 3 * S;
    dc.sample(t0, server, {}, false, t0 + 1);
    dc_report(t0, forked, t0 + 7, 0, 0);
    dc_report(t0, forked, t0 + 8, kDetail, 0);
    uint32_t hx = k8(t0 + 9), last_x = 0;
    for (int i = 1; i <= 2; ++i) {
        order = dc.next_heal(hx, false, none);
        ASSERT_TRUE(order) << i;
        dc.heal_sent(*order, hx, 2048, false, hx + 1);
        const uint32_t v = (hx / S + 1) * S;
        dc.sample(v, server, {}, false, v + 1);
        dc_report(v, forked, v + 5, 0, hx);
        EXPECT_EQ(last(stat_kind::fork_verify)[8], std::to_string(i));
        last_x = hx;
        hx = k8(hx + 1 + G);   // the fixed gap from heal_sent at hx + 1
        dc.step(hx - 1);
    }
    const auto decays = [&] { return count(stat_kind::fork_verify, 4, "decay"); };
    uint32_t e = (hx / S + 1) * S;   // the first equal sample: equal_again, the run starts
    const uint32_t run_start = e;
    const auto equal_to = [&](uint32_t until) {
        for (; e <= until; e += S) {
            dc.sample(e, server, {}, false, e + 1);
            dc_report(e, server, e + 5, 0, last_x);
        }
    };
    equal_to(run_start + D - S);
    EXPECT_EQ(decays(), 0) << "one sample short: not yet";
    equal_to(run_start + D);
    EXPECT_EQ(decays(), 1);
    EXPECT_EQ(last(stat_kind::fork_verify)[8], "1");
    // A differing sample ends the run: the next kDecayTicks count from its next equal one.
    dc.sample(e, server, {}, false, e + 1);
    dc_report(e, ball, e + 5, 0, last_x);
    dc_report(e, ball, e + 6, kDetail, last_x);
    e += S;
    const uint32_t second = e;   // equal again (self_healed before any heal): the run restarts
    equal_to(second + D - S);
    EXPECT_EQ(decays(), 1) << "the run restarted at " << second;
    equal_to(second + D);
    EXPECT_EQ(decays(), 2);
    EXPECT_EQ(last(stat_kind::fork_verify)[8], "0");
    equal_to(second + 2 * D);
    EXPECT_EQ(decays(), 2) << "never below 0";

    // Off: no queue, FD1's would-heal only.
    world_watch off(sink);
    off.member(1, caps, "a", 100);
    off.ready(1, 100, 100);
    off.sample(198, server, {}, false, 199);
    world_report_in r;
    r.player = 1; r.epoch = 100; r.tick = 198; r.terms = wc::fold_rows(forked, {});
    off.report(r, 205);
    r.flags = kDetail; r.rows = forked; r.episode = notices.back().episode;
    off.report(r, 206);
    off.step(207);
    EXPECT_FALSE(off.heal_queued());
}
