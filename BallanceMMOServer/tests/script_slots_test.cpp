#include <game/script_slots.hpp>
#include <session/member_lives.hpp>

#include <gtest/gtest.h>

TEST(ScriptSlots, BoundSetIncludesInitialAndBegunLivesWithoutDroppingLeavers) {
    std::vector<bmmo::session::member_life> lives(4);
    lives[0].entry.join_order = 0;
    lives[0].until = 15;
    lives[1].entry.join_order = 3;
    lives[1].from = 20;
    lives[1].until = 30;
    lives[2].entry.join_order = 3;
    lives[2].from = 40;
    lives[3].entry.join_order = 63;
    lives[3].from = 50;
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 0), 1u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 19), 1u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 20), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 35), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 40), 9u);
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 50), (uint64_t{1} << 63) | 9u);
}

TEST(ScriptSlots, InvalidJoinOrderDoesNotShiftPastTheMask) {
    std::vector<bmmo::session::member_life> lives(1);
    lives[0].entry.join_order = 64;
    EXPECT_EQ(bmmo::session::bound_member_slots(lives, 10), 0u);
}

namespace {
    std::vector<uint8_t> delta_round_trip(const std::vector<uint8_t>& base, const std::vector<uint8_t>& image,
                                          size_t* bytes = nullptr) {
        std::vector<uint8_t> out{0xee};   // a byte before the delta: positions are relative
        EXPECT_TRUE(bmmo::game::script_slots::put_delta(base, image, out));
        if (bytes) *bytes = out.size() - 1;
        size_t pos = 1, skipped = 1;
        std::vector<uint8_t> back;
        EXPECT_TRUE(bmmo::game::script_slots::get_delta(base, out, pos, back));
        EXPECT_EQ(pos, out.size());
        EXPECT_TRUE(bmmo::game::script_slots::skip_delta(out, skipped));
        EXPECT_EQ(skipped, out.size());
        return back;
    }
}

// R1: an unbound copy's image against its copy-step base.
TEST(ScriptSlots, DeltaRoundTrips) {
    std::vector<uint8_t> base(300);
    for (size_t i = 0; i < base.size(); ++i) base[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> out;
    EXPECT_FALSE(bmmo::game::script_slots::put_delta(base, base, out));
    EXPECT_TRUE(out.empty());
    auto few = base;
    few[40] ^= 1;
    few[43] ^= 2;   // closer than a run header: one run
    few[200] ^= 4;
    size_t bytes = 0;
    EXPECT_EQ(delta_round_trip(base, few, &bytes), few);
    EXPECT_LT(bytes, 30u);
    auto edges = base;
    edges[0] ^= 1;
    edges.back() ^= 1;
    EXPECT_EQ(delta_round_trip(base, edges), edges);
    auto longer = base;
    longer.push_back(9);
    EXPECT_EQ(delta_round_trip(base, longer), longer);
    const std::vector<uint8_t> shorter(base.begin(), base.begin() + 10);
    EXPECT_EQ(delta_round_trip(base, shorter), shorter);
    auto all = base;
    for (auto& b: all) b ^= 0xff;
    EXPECT_EQ(delta_round_trip(base, all), all);
    std::vector<uint8_t> truncated;
    ASSERT_TRUE(bmmo::game::script_slots::put_delta(base, few, truncated));
    truncated.pop_back();
    size_t pos = 0;
    std::vector<uint8_t> back;
    EXPECT_FALSE(bmmo::game::script_slots::get_delta(base, truncated, pos, back));
}

// R1: records captured before a bind, with the copy unchanged (B) and
// changed (I2), and after it (I3) restore to B / I2 / I3 whatever was
// restored before: an unbound copy is written base + delta every time.
TEST(ScriptSlots, RestoreAcrossBindingWritesTheRecordsImage) {
    bmmo::game::script_slots slots;
    const std::vector<uint8_t> B{1, 2, 3, 4}, I2{1, 9, 3, 4}, I3{5, 6, 7, 8, 9};
    slots.add(123, 3, B);
    std::vector<bmmo::session::member_life> lives(1);
    lives[0].entry.join_order = 3;
    lives[0].from = 20;
    struct record { uint64_t mask; std::vector<uint8_t> section, body; };
    const auto capture = [&](uint32_t tick, const std::vector<uint8_t>& live) {   // as capture_behavior
        slots.select(bmmo::session::bound_member_slots(lives, tick));
        record r{slots.mask(), {}, {}};
        if (!slots.omit(123)) r.body = live;
        else bmmo::game::script_slots::put_delta(*slots.base(123), live, r.section);
        return r;
    };
    const record r1 = capture(18, B), r2 = capture(19, I2), r3 = capture(20, I3);
    EXPECT_TRUE(r1.section.empty());
    EXPECT_FALSE(r2.section.empty());
    EXPECT_EQ(r3.body, I3);
    std::vector<uint8_t> live = I3;
    const auto restore = [&](const record& r) {   // as restore_behavior
        slots.select_record(r.mask);
        if (!slots.omit(123)) {
            live = r.body;
            return;
        }
        if (r.section.empty()) {
            live = *slots.base(123);
            return;
        }
        size_t pos = 0;
        ASSERT_TRUE(bmmo::game::script_slots::get_delta(*slots.base(123), r.section, pos, live));
    };
    for (const auto* order: {"123", "321", "213", "1313", "2121", "3232"}) {
        for (const char* c = order; *c; ++c) {
            const record& r = *c == '1' ? r1 : *c == '2' ? r2 : r3;
            restore(r);
            EXPECT_EQ(live, *c == '1' ? B : *c == '2' ? I2 : I3) << order;
        }
    }
    EXPECT_EQ(slots.base_hash(123), bmmo::game::script_slots::hash(B));
    slots.add(123, 3, I2);   // a re-copy: the base is replaced
    EXPECT_EQ(*slots.base(123), I2);
    EXPECT_EQ(slots.base_hash(123), bmmo::game::script_slots::hash(I2));
}

TEST(ScriptSlots, PortableRecordRetainsHistoricallyBoundSlotsForLateJoiners) {
    bmmo::game::script_slots slots;
    slots.add(123, 5, {1});
    slots.add(124, 6, {2});
    slots.learn_mask(uint64_t{1} << 5);
    slots.select(1);
    EXPECT_FALSE(slots.omit(123));
    EXPECT_TRUE(slots.omit(124));
    EXPECT_FALSE(slots.omit(999));
    EXPECT_EQ(slots.mask(), (uint64_t{1} << 5) | 1u);
    slots.select_record(0);
    EXPECT_TRUE(slots.omit(123));
    EXPECT_TRUE(slots.omit(124));
    slots.select(1);
    EXPECT_FALSE(slots.omit(123));
    slots.clear();
    slots.add(123, 5, {1});
    slots.select(1);
    EXPECT_TRUE(slots.omit(123));
}

// T3C e3: the slot memo's quiet-run ledger and the memo's refusals.
TEST(ScriptSlots, MemoServesOnlyWhileItsRootStaysQuiet) {
    using bmmo::game::script_root_words;
    using bmmo::game::script_slots;
    constexpr uint32_t kActive = 1;   // a frame flag
    script_slots slots;
    slots.add(10, 2, {1, 2, 3}, 7);
    script_slots::copy* c = slots.find(10);
    ASSERT_NE(c, nullptr);
    EXPECT_STREQ(slots.memo_refused(*c, 0), "none");
    script_root_words quiet;
    quiet.id = 7;
    const auto observe = [&](uint32_t tick, script_root_words words, bool present = true) {
        slots.begin_observation(tick);
        slots.observe(7, tick, present, words, kActive);
    };
    const auto walk = [&](uint64_t writes) {
        c->m.valid = true;
        c->m.seq = slots.seq();
        c->m.writes = writes;
    };
    observe(100, quiet);
    walk(5);
    EXPECT_EQ(slots.memo_refused(*c, 5), nullptr) << "walked at this observation";
    observe(101, quiet);
    observe(102, quiet);
    EXPECT_EQ(slots.memo_refused(*c, 5), nullptr) << "quiet ticks go on";
    EXPECT_STREQ(slots.memo_refused(*c, 6), "gen") << "a host write";
    script_root_words active = quiet;
    active.flags = kActive;
    observe(103, active);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "the root ran";
    observe(104, quiet);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "from an active tick: no run either";
    walk(5);
    observe(105, quiet);
    EXPECT_EQ(slots.memo_refused(*c, 5), nullptr);
    observe(107, quiet);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "a gap";
    walk(5);
    observe(107, quiet);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "the same tick again (an amend)";
    walk(5);
    observe(90, quiet);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "a backward jump (a rewind)";
    walk(5);
    script_root_words other = quiet;
    other.in_mask = 1;
    observe(91, other);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "an IO switched";
    walk(5);
    observe(92, other, false);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "the root not found";
    walk(5);
    observe(93, active);
    walk(5);
    observe(94, active);
    EXPECT_STREQ(slots.memo_refused(*c, 5), "run") << "active at both ends, words equal: the frame flag alone ends it";
    slots.add(11, 3, {4, 5}, 8);
    script_slots::copy* d = slots.find(11);
    d->m.valid = true;
    d->m.seq = slots.seq();
    d->m.writes = 5;
    EXPECT_STREQ(slots.memo_refused(*d, 5), "run") << "a root never observed";
    slots.add(10, 2, {1, 2, 3}, 7);
    EXPECT_STREQ(slots.memo_refused(*slots.find(10), 5), "none") << "a re-copy drops the memo";
    slots.clear();
    EXPECT_EQ(slots.find(10), nullptr);
    EXPECT_TRUE(slots.roots().empty());
}

// The restore skip: a root counts as quiet at a restore only while its words
// now would go on its run; the armed set is read by root and cleared.
TEST(ScriptSlots, RestoreSkipArmsOnlyRootsStillQuiet) {
    using bmmo::game::script_root_words;
    using bmmo::game::script_slots;
    constexpr uint32_t kActive = 1;
    script_slots slots;
    slots.add(10, 2, {1, 2, 3}, 7);
    script_root_words quiet;
    quiet.id = 7;
    EXPECT_FALSE(slots.quiet_now(7, quiet, kActive)) << "never observed";
    slots.begin_observation(100);
    slots.observe(7, 100, true, quiet, kActive);
    EXPECT_TRUE(slots.quiet_now(7, quiet, kActive));
    script_root_words active = quiet;
    active.flags = kActive;
    EXPECT_FALSE(slots.quiet_now(7, active, kActive)) << "activated since its last observation";
    script_root_words switched = quiet;
    switched.out_mask = 2;
    EXPECT_FALSE(slots.quiet_now(7, switched, kActive)) << "an output switched since";
    EXPECT_FALSE(slots.restore_quiet(7)) << "not armed";
    slots.arm_restore({9, 7}, 4);
    EXPECT_TRUE(slots.restore_armed());
    EXPECT_TRUE(slots.restore_quiet(7));
    EXPECT_FALSE(slots.restore_quiet(8));
    EXPECT_EQ(slots.restore_writes(), 4u);
    slots.disarm_restore();
    EXPECT_FALSE(slots.restore_quiet(7));
    slots.arm_restore({7}, 4);
    slots.clear();
    EXPECT_FALSE(slots.restore_armed()) << "a clear disarms";
}

// The kept source: a record a capture left or a restore wrote serves a later
// capture only under its generation and for a root quiet at every observation
// since - a root that ran on a light tick between is walked.
TEST(ScriptSlots, KeptSourceServesARootQuietAtEveryObservationSince) {
    using bmmo::game::script_root_words;
    using bmmo::game::script_slots;
    constexpr uint32_t kActive = 1;
    script_slots slots;
    slots.track(7);
    slots.track(8);
    slots.track(7);
    EXPECT_EQ(slots.roots().size(), 2u);
    script_root_words quiet7, quiet8;
    quiet7.id = 7;
    quiet8.id = 8;
    script_root_words ran8 = quiet8;
    ran8.flags = kActive;
    const auto observe = [&](uint32_t tick, const script_root_words& w8) {
        slots.begin_observation(tick);
        slots.observe(7, tick, true, quiet7, kActive);
        slots.observe(8, tick, true, w8, kActive);
    };
    const std::vector<uint8_t> record{1, 2, 3};
    EXPECT_EQ(slots.kept_source(5), nullptr) << "none yet";
    observe(100, quiet8);
    slots.keep_source(record.data(), 2, 5);
    ASSERT_NE(slots.kept_source(5), nullptr);
    EXPECT_EQ(*slots.kept_source(5), (std::vector<uint8_t>{1, 2})) << "the script list alone";
    EXPECT_EQ(slots.kept_source(6), nullptr) << "a host write since";
    observe(101, ran8);       // a light tick: 8 runs
    observe(102, quiet8);     // and is quiet again with its old words
    EXPECT_TRUE(slots.quiet_since_kept(7));
    EXPECT_FALSE(slots.quiet_since_kept(8)) << "it ran between";
    // A restore to tick 90 wrote 7 exactly and not 8; it skipped a copy
    // under each.
    slots.add(10, 2, {1}, 7);
    slots.add(11, 2, {1}, 8);
    for (uint32_t id: {10u, 11u}) {
        script_slots::copy* c = slots.find(id);
        c->m.valid = true;
        c->m.writes = 5;
        slots.keep_memo(id);
    }
    slots.restored(90, {quiet7}, record.data(), record.size(), 6);
    EXPECT_TRUE(slots.kept_restored());
    EXPECT_EQ(slots.memo_refused(*slots.find(10), 6), nullptr) << "a copy the restore left keeps its memo";
    EXPECT_STREQ(slots.memo_refused(*slots.find(11), 6), "run") << "not under a root the restore did not write exactly";
    observe(91, quiet8);
    EXPECT_TRUE(slots.quiet_since_kept(7)) << "the replay goes on from the record's tick";
    EXPECT_FALSE(slots.quiet_since_kept(8));
    EXPECT_EQ(slots.memo_refused(*slots.find(10), 6), nullptr);
    observe(93, quiet8);
    EXPECT_FALSE(slots.quiet_since_kept(7)) << "a gap";
    slots.forget();
    EXPECT_EQ(slots.kept_source(6), nullptr) << "a failed restore";
}

// Step RELATCH: a marked root (a host write that left its words as they were)
// has no source - no kept source, copy plan, slot memo or restore skip, and
// the capture walks it from record t - 1 too (marked) - for the rest of the
// tick of an observing capture's walk (walked: its run starts again there, so
// nothing kept from before serves it; a second capture of that tick walks it
// too), or until a restore that wrote it exactly; another root keeps
// everything.
TEST(ScriptSlots, MarkedRootHasNoSourceUntilWalkedOrRestored) {
    using bmmo::game::script_root_words;
    using bmmo::game::script_slots;
    constexpr uint32_t kActive = 1;
    script_slots slots;
    slots.track(7);
    slots.track(8);
    slots.add(10, 2, {1}, 7);
    slots.add(11, 2, {1}, 8);
    script_root_words quiet7, quiet8;
    quiet7.id = 7;
    quiet8.id = 8;
    uint32_t tick = 99;
    const auto observe = [&](bool next = true) {
        if (next) ++tick;
        slots.begin_observation(tick);
        slots.observe(7, tick, true, quiet7, kActive);
        slots.observe(8, tick, true, quiet8, kActive);
    };
    const auto memo_and_plan = [&](uint32_t copy, uint32_t root) {
        script_slots::copy* c = slots.find(copy);
        c->m.valid = true;
        c->m.seq = slots.seq();
        c->m.writes = 5;
        script_slots::plan& p = slots.plan_of(root);
        p = script_slots::plan{};
        p.valid = true;
        p.seq = slots.seq();
        p.writes = 5;
        p.mask = slots.mask();
    };
    const std::vector<uint8_t> record{1, 2, 3};
    observe();
    memo_and_plan(10, 7);
    memo_and_plan(11, 8);
    slots.keep_source(record.data(), record.size(), 5);
    observe();
    const char* refused = nullptr;
    ASSERT_NE(slots.plan_for(7, 5, refused), nullptr);
    ASSERT_TRUE(slots.quiet_since_kept(7));

    slots.mark(7);   // the relatch, between two observations
    const auto no_source = [&](const char* when) {
        EXPECT_TRUE(slots.marked(7)) << when;
        EXPECT_FALSE(slots.quiet_since_kept(7)) << when << ": the kept source";
        EXPECT_EQ(slots.plan_for(7, 5, refused), nullptr) << when << ": the copy plan";
        EXPECT_STREQ(refused, "run") << when;
        EXPECT_STREQ(slots.memo_refused(*slots.find(10), 5), "run") << when << ": the slot memo";
        EXPECT_FALSE(slots.quiet_now(7, quiet7, kActive)) << when << ": the restore skip";
        // The other root keeps its source, plan, memo and skip.
        EXPECT_FALSE(slots.marked(8)) << when;
        EXPECT_TRUE(slots.quiet_since_kept(8)) << when;
        EXPECT_NE(slots.plan_for(8, 5, refused), nullptr) << when;
        EXPECT_EQ(slots.memo_refused(*slots.find(11), 5), nullptr) << when;
        EXPECT_TRUE(slots.quiet_now(8, quiet8, kActive)) << when;
    };
    no_source("marked");
    observe();   // a light record observes and does not walk
    no_source("after a light observation");
    slots.walked(7);   // an observing capture's walk
    no_source("walked");
    memo_and_plan(10, 7);   // what that capture's walk and copy leave
    slots.keep_source(record.data(), record.size(), 5);
    observe(false);   // a second capture of the same tick (an amend: every run starts again)
    EXPECT_TRUE(slots.marked(7)) << "a second capture of the walk's tick walks it too";
    EXPECT_FALSE(slots.quiet_since_kept(7));
    EXPECT_FALSE(slots.quiet_now(7, quiet7, kActive));
    slots.walked(7);
    memo_and_plan(10, 7);
    slots.keep_source(record.data(), record.size(), 5);
    observe();   // the next tick
    EXPECT_FALSE(slots.marked(7));
    EXPECT_TRUE(slots.quiet_now(7, quiet7, kActive)) << "its words are quiet";
    EXPECT_TRUE(slots.quiet_since_kept(7));
    EXPECT_NE(slots.plan_for(7, 5, refused), nullptr);
    EXPECT_EQ(slots.memo_refused(*slots.find(10), 5), nullptr);

    slots.mark(7);
    slots.walked(7);
    slots.mark(7);   // relatched again after the walk, before the next tick
    observe();
    EXPECT_TRUE(slots.marked(7)) << "the second relatch is not walked yet";

    // A restore that wrote 7 exactly takes its mark off; one that did not
    // leaves it.
    slots.mark(7);
    slots.mark(8);
    slots.restored(90, {quiet7}, record.data(), record.size(), 6);
    EXPECT_FALSE(slots.marked(7));
    EXPECT_TRUE(slots.quiet_since_kept(7));
    EXPECT_TRUE(slots.marked(8)) << "not written by the restore";
    EXPECT_FALSE(slots.quiet_since_kept(8));
    slots.forget();
    EXPECT_FALSE(slots.marked(8)) << "a failed restore forgets every mark";
}
