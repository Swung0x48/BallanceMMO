// Step OPC (rewind-cost-design R.4 row C): an inactive script's record reused.
// The engine-free half - the rule on a root's activation words, the mode
// parser, the host-write generation and the counters.  The rollback engine's
// choice of the record a capture may reuse from is in rollback_engine_test.cpp
// (RollbackEngine.ScriptReuse*); the CK half (the copy inside
// script_replay::capture and its verify) runs in the replays.

#include <game/script_record_reuse.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
    using bmmo::game::script_reuse_mode;
    using bmmo::game::script_root_words;

    constexpr uint32_t kActive = 1u << 0, kActivateNext = 1u << 1, kDeactivateNext = 1u << 2, kResetNext = 1u << 3,
                       kLaunchedOnce = 1u << 4;
    constexpr uint32_t kFrame = kActive | kActivateNext | kDeactivateNext | kResetNext;

    std::vector<uint8_t> block_of(const script_root_words& w, size_t tail = 8) {
        std::vector<uint8_t> b(16 + tail, 0xAB);
        std::memcpy(b.data(), &w.id, 4);
        std::memcpy(b.data() + 4, &w.flags, 4);
        std::memcpy(b.data() + 8, &w.in_mask, 4);
        std::memcpy(b.data() + 12, &w.out_mask, 4);
        return b;
    }
}

TEST(ScriptRecordReuse, AnInactiveRootWithItsRecordedWordsIsReused) {
    script_root_words recorded{42, kLaunchedOnce, 0, 0};
    const auto block = block_of(recorded);
    script_root_words read;
    ASSERT_TRUE(bmmo::game::read_script_root_words(block.data(), block.size(), read));
    EXPECT_EQ(read.id, 42u);
    EXPECT_EQ(read.flags, kLaunchedOnce);
    EXPECT_FALSE(bmmo::game::read_script_root_words(block.data(), 15, read)) << "a block shorter than its four words";
    EXPECT_TRUE(bmmo::game::reusable_root(read, script_root_words{42, kLaunchedOnce, 0, 0}, kFrame));
}

TEST(ScriptRecordReuse, AFrameFlagNowOrAtTheRecordOrAnyChangedWordWalks) {
    const script_root_words idle{7, 0, 0, 0};
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, kActive, 0, 0}, kFrame)) << "active now";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, kActivateNext, 0, 0}, kFrame)) << "activation pending";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, kDeactivateNext, 0, 0}, kFrame));
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, kResetNext, 0, 0}, kFrame));
    EXPECT_FALSE(bmmo::game::reusable_root(script_root_words{7, kActive, 0, 0}, idle, kFrame))
        << "active at t - 1, finished since: walked";
    EXPECT_FALSE(bmmo::game::reusable_root(script_root_words{7, kActivateNext, 0, 0}, idle, kFrame))
        << "a pending activation at t - 1 (ran and finished inside the frame): walked";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, kLaunchedOnce, 0, 0}, kFrame))
        << "launched once since: it ran";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, 0, 1, 0}, kFrame)) << "an input switched on";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{7, 0, 0, 4}, kFrame)) << "an output switched on";
    EXPECT_FALSE(bmmo::game::reusable_root(idle, script_root_words{8, 0, 0, 0}, kFrame)) << "another root";
    EXPECT_TRUE(bmmo::game::reusable_root(idle, idle, kFrame));
}

TEST(ScriptRecordReuse, ModeParserAndHostWriteGeneration) {
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode("off", script_reuse_mode::on), script_reuse_mode::off);
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode("0", script_reuse_mode::on), script_reuse_mode::off);
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode("on", script_reuse_mode::off), script_reuse_mode::on);
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode("verify", script_reuse_mode::on), script_reuse_mode::verify);
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode(nullptr, script_reuse_mode::on), script_reuse_mode::on);
    EXPECT_EQ(bmmo::game::parse_script_reuse_mode("junk", script_reuse_mode::on), script_reuse_mode::on);
    EXPECT_STREQ(bmmo::game::script_reuse_mode_name(script_reuse_mode::verify), "verify");
    const uint64_t before = bmmo::game::script_write_generation();
    bmmo::game::note_script_write();
    EXPECT_EQ(bmmo::game::script_write_generation(), before + 1);
}

TEST(ScriptRecordReuse, TheCountersNoteTheFirstMismatchesAndTheLineSaysTheRate) {
    bmmo::game::script_reuse_stats s;
    s.reused = 3;
    s.walked = 1;
    s.walked_structure = 1;
    s.tails = 7;
    for (int i = 0; i < 20; ++i) s.note("m" + std::to_string(i));
    EXPECT_EQ(s.mismatches, 20u);
    EXPECT_EQ(s.notes.size(), bmmo::game::script_reuse_stats::kNotes);
    const std::string line = s.line(script_reuse_mode::verify);
    EXPECT_NE(line.find("mode=verify"), std::string::npos);
    EXPECT_NE(line.find("reused=3 (60.0%)"), std::string::npos) << line;
    EXPECT_NE(line.find("structure=1 "), std::string::npos) << line;
    EXPECT_NE(line.find("(60.0%) tails=7 "), std::string::npos) << line;
    EXPECT_NE(line.find("mismatches=20"), std::string::npos);
    const uint8_t a[] = {1, 2, 3, 4}, b[] = {1, 2, 9, 4}, c[] = {1, 2};
    EXPECT_EQ(bmmo::game::first_difference(a, 4, b, 4), 2u);
    EXPECT_EQ(bmmo::game::first_difference(a, 4, c, 2), 2u);
    EXPECT_EQ(bmmo::game::first_difference(a, 4, a, 4), 4u);
}
