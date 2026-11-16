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
        EXPECT_TRUE(back.body_rows.empty());
        EXPECT_TRUE(back.folds.empty());
        // A trailing byte, or another schema, is refused.
        bmmo::session_world_report_msg longer;
        EXPECT_FALSE(round(longer, report.raw.str() + "x"));
        std::string other = report.raw.str();
        other[sizeof(bmmo::opcode)] = 2;
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
