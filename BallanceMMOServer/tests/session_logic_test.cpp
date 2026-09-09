// Unit tests for the pure physics-session bookkeeping: server input buffers
// and tick scheduling (session/timeline.hpp), the client's own-ball
// correction planner (session/correction.hpp) and the retransmit window of
// the input message (message/session_input_msg.hpp).
#include <gtest/gtest.h>

#include <message/message_all.hpp>

#include <string>

#include <session/correction.hpp>
#include <session/timeline.hpp>

using namespace bmmo::session;
using namespace std::chrono_literals;

namespace {
    input_frame frame_with_keys(uint8_t keys) {
        input_frame f{};
        f.keys = keys;
        return f;
    }
}

TEST(InputBuffer, TakesFreshFramesAndFallsBackToLast) {
    input_buffer buffer;
    buffer.reset(0);
    EXPECT_FALSE(buffer.received_any());

    std::vector<input_frame> frames{frame_with_keys(1), frame_with_keys(2), frame_with_keys(3)};
    EXPECT_EQ(buffer.submit(0, frames), 3);
    EXPECT_EQ(buffer.pending(), 3u);

    bool fresh = false;
    EXPECT_EQ(buffer.take(0, fresh).keys, 1);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.take(1, fresh).keys, 2);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.take(2, fresh).keys, 3);
    EXPECT_TRUE(fresh);
    // tick 3 never arrived: the last applied frame is reused
    EXPECT_EQ(buffer.take(3, fresh).keys, 3);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(buffer.last_fresh_tick(), 2u);
    EXPECT_EQ(buffer.next_tick(), 4u);
}

TEST(InputBuffer, RedundantResendsAreDeduplicatedAndOldTicksIgnored) {
    input_buffer buffer;
    buffer.reset(10);
    std::vector<input_frame> first{frame_with_keys(4), frame_with_keys(5)};
    EXPECT_EQ(buffer.submit(10, first), 2);
    // the client resends the last 8 ticks: overlapping frames are not new
    std::vector<input_frame> resend{frame_with_keys(4), frame_with_keys(5), frame_with_keys(6)};
    EXPECT_EQ(buffer.submit(10, resend), 1);
    bool fresh = false;
    buffer.take(10, fresh);
    buffer.take(11, fresh);
    // a late packet for a consumed tick is dropped
    std::vector<input_frame> late{frame_with_keys(9)};
    EXPECT_EQ(buffer.submit(11, late), 0);
    EXPECT_EQ(buffer.take(12, fresh).keys, 6);
    EXPECT_TRUE(fresh);
}

TEST(InputBuffer, RejectsGarbageFarInTheFuture) {
    input_buffer buffer;
    buffer.reset(0);
    std::vector<input_frame> garbage{frame_with_keys(1)};
    EXPECT_EQ(buffer.submit(input_buffer::kMaxLookahead + 5, garbage), 0);
    EXPECT_EQ(buffer.pending(), 0u);
}

TEST(InputBuffer, ResetNeverMovesTheCursorBackwards) {
    input_buffer buffer;
    buffer.reset(100);
    EXPECT_EQ(buffer.next_tick(), 100u);
    std::vector<input_frame> frames{frame_with_keys(1), frame_with_keys(2)};
    EXPECT_EQ(buffer.submit(100, frames), 2);
    bool fresh = false;
    EXPECT_EQ(buffer.take(100, fresh).keys, 1);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.next_tick(), 101u);

    // A resync that names a base behind the consumption point cannot rewind:
    // those ticks are already simulated, so the cursor stays where it is.
    buffer.reset(50);
    EXPECT_EQ(buffer.next_tick(), 101u);
    // and the frame already held for tick 101 is still there
    EXPECT_EQ(buffer.pending(), 1u);
    EXPECT_EQ(buffer.last_fresh_tick(), 100u);
    EXPECT_EQ(buffer.take(101, fresh).keys, 2);
    EXPECT_TRUE(fresh);
}

TEST(InputBuffer, ResetDropsFramesBelowTheAnchorAndKeepsTheRest) {
    input_buffer buffer;
    buffer.reset(0);
    std::vector<input_frame> frames{frame_with_keys(1), frame_with_keys(2), frame_with_keys(3),
                                    frame_with_keys(4), frame_with_keys(5)};
    EXPECT_EQ(buffer.submit(0, frames), 5);
    bool fresh = false;
    EXPECT_EQ(buffer.take(0, fresh).keys, 1);
    const uint64_t stale_before = buffer.stale();

    // The resync anchor is ahead of everything the client has sent: the held
    // frames are dropped and counted, not left to confuse the cursor.
    buffer.reset(10);
    EXPECT_EQ(buffer.next_tick(), 10u);
    EXPECT_EQ(buffer.pending(), 0u);
    EXPECT_EQ(buffer.stale(), stale_before + 4u);   // ticks 1..4

    // Frames below the anchor are refused the same way on arrival ...
    std::vector<input_frame> later{frame_with_keys(9), frame_with_keys(10), frame_with_keys(11)};
    EXPECT_EQ(buffer.submit(9, later), 2);
    EXPECT_EQ(buffer.stale(), stale_before + 5u);

    // ... and the world can simulate the ticks in between without the cursor
    // slipping backwards; the frame for the anchor is still delivered fresh.
    EXPECT_EQ(buffer.take(5, fresh).keys, 1);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(buffer.next_tick(), 10u);
    EXPECT_EQ(buffer.take(10, fresh).keys, 10);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.next_tick(), 11u);
    EXPECT_EQ(buffer.last_fresh_tick(), 10u);
}

TEST(InputBuffer, TakeAcrossAGapReusesTheLastFrameAndSkipsForward) {
    input_buffer buffer;
    buffer.reset(0);
    std::vector<input_frame> only0{frame_with_keys(7)};
    EXPECT_EQ(buffer.submit(0, only0), 1);
    std::vector<input_frame> from3{frame_with_keys(8), frame_with_keys(9)};
    EXPECT_EQ(buffer.submit(3, from3), 2);

    bool fresh = false;
    EXPECT_EQ(buffer.take(0, fresh).keys, 7);
    EXPECT_TRUE(fresh);
    // ticks 1 and 2 never arrived: the last applied frame is reused and the
    // cursor keeps moving forward, so a frame that lands after its tick was
    // consumed is counted stale instead of being applied out of order.
    EXPECT_EQ(buffer.take(1, fresh).keys, 7);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(buffer.take(2, fresh).keys, 7);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(buffer.next_tick(), 3u);
    EXPECT_EQ(buffer.last_fresh_tick(), 0u);

    const uint64_t stale_before = buffer.stale();
    std::vector<input_frame> late{frame_with_keys(6)};
    EXPECT_EQ(buffer.submit(1, late), 0);
    EXPECT_EQ(buffer.stale(), stale_before + 1u);

    EXPECT_EQ(buffer.take(3, fresh).keys, 8);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(buffer.take(4, fresh).keys, 9);
    EXPECT_TRUE(fresh);
}

TEST(SessionInputMsg, FullRetransmitWindowRoundTrips) {
    // The client sends its whole input ring in one message, so the ring can
    // only be as wide as the wire cap: a message of exactly MAX_INPUT_FRAMES
    // frames - the widest retransmit window the protocol allows - has to
    // survive the round trip with its order intact.
    bmmo::session_input_msg msg{};
    msg.session = 5;
    msg.first_tick = 1000;
    for (size_t i = 0; i < bmmo::session::MAX_INPUT_FRAMES; ++i) {
        input_frame f{};
        f.keys = static_cast<uint8_t>(i + 1);
        msg.frames.push_back(f);
    }
    ASSERT_TRUE(msg.serialize());
    const std::string payload = msg.raw.str();

    bmmo::session_input_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(parsed.session, 5u);
    EXPECT_EQ(parsed.first_tick, 1000u);
    ASSERT_EQ(parsed.frames.size(), bmmo::session::MAX_INPUT_FRAMES);
    for (size_t i = 0; i < parsed.frames.size(); ++i)
        EXPECT_EQ(parsed.frames[i].keys, static_cast<uint8_t>(i + 1));
}

TEST(TickScheduler, DeadlinesFollowInputDelay) {
    tick_scheduler scheduler;
    const auto start = tick_scheduler::clock::time_point(1000s);
    scheduler.start(start, 0, 6);
    EXPECT_TRUE(scheduler.started());
    EXPECT_EQ(scheduler.next_tick(), 0u);
    // tick 0 is due once 6 ticks of wall time passed
    EXPECT_FALSE(scheduler.due(start));
    EXPECT_FALSE(scheduler.due(start + tick_offset(5)));
    EXPECT_TRUE(scheduler.due(start + tick_offset(6)));
    scheduler.advance();
    EXPECT_EQ(scheduler.next_tick(), 1u);
    EXPECT_FALSE(scheduler.due(start + tick_offset(6)));
    EXPECT_TRUE(scheduler.due(start + tick_offset(7)));
    EXPECT_EQ(scheduler.until_due(start + tick_offset(7)), tick_scheduler::clock::duration::zero());
    EXPECT_GT(scheduler.until_due(start), tick_scheduler::clock::duration::zero());
}

TEST(InputDelayForPing, CoversOneWayPlusJitterAndRespectsTheFloor) {
    // A link with no measurable latency still gets the floor.
    EXPECT_EQ(input_delay_for_ping(0, 6), 6u);
    EXPECT_EQ(input_delay_for_ping(0, 0), 2u);   // margin alone, rounded up
    // 100 ms round trip: 50 ms one way, 75 ms with the jitter allowance, plus
    // the 16 ms margin = 91 ms = 7 ticks - above the floor, so it wins.
    EXPECT_EQ(input_delay_for_ping(100, 6), 7u);
    // Bigger round trips keep scaling, and the floor stops mattering.
    EXPECT_EQ(input_delay_for_ping(200, 6), 11u);
    EXPECT_EQ(input_delay_for_ping(400, 6), 21u);
    EXPECT_GT(input_delay_for_ping(300, 6), input_delay_for_ping(150, 6));
    // Never below the floor, never past the cap, never confused by a negative.
    EXPECT_EQ(input_delay_for_ping(10, 12), 12u);
    EXPECT_EQ(input_delay_for_ping(100000, 6), kInputDelayMaxTicks);
    EXPECT_EQ(input_delay_for_ping(-1, 6), 6u);
}

TEST(TickScheduler, LateJoinerTickIncludesMargin) {
    tick_scheduler scheduler;
    const auto start = tick_scheduler::clock::time_point(0s);
    scheduler.start(start, 100, 2);
    EXPECT_EQ(scheduler.tick_at(start, 66), 166u);
    EXPECT_EQ(scheduler.tick_at(start + 1s, 66), 100u + 66u + 66u);
}

TEST(SnapshotCadence, FullThenDeltas) {
    snapshot_cadence cadence;
    cadence.interval = 2;
    cadence.full_interval = 10;
    EXPECT_EQ(cadence.decide(0, false), 2);   // first snapshot is full
    EXPECT_EQ(cadence.decide(1, false), 0);
    EXPECT_EQ(cadence.decide(2, false), 1);
    EXPECT_EQ(cadence.decide(3, true), 2);    // body set changed
    EXPECT_EQ(cadence.decide(4, false), 1);
    EXPECT_EQ(cadence.decide(13, false), 2);  // full interval elapsed
}

namespace {
    ball_pose pose_at(uint32_t tick, double x, float vx) {
        ball_pose p;
        p.tick = tick;
        p.position[0] = x;
        p.linear[0] = vx;
        return p;
    }
}

TEST(OwnBallCorrector, IgnoresTinyErrors) {
    own_ball_corrector corrector;
    corrector.record(pose_at(5, 1.0, 2.0f));
    auto step = corrector.compare(pose_at(5, 1.001, 2.01f));
    EXPECT_EQ(step.action, correction_step::kind::none);
    EXPECT_EQ(corrector.stats().ignored, 1u);
    EXPECT_FALSE(corrector.blending());
}

TEST(OwnBallCorrector, BlendsMediumErrorsOverKTicks) {
    correction_thresholds thresholds;
    thresholds.blend_ticks = 4;
    own_ball_corrector corrector(thresholds);
    for (uint32_t t = 0; t < 10; ++t) corrector.record(pose_at(t, 1.0, 0.0f));
    auto step = corrector.compare(pose_at(3, 1.4, 0.4f));  // 0.4 m off
    ASSERT_EQ(step.action, correction_step::kind::blend);
    EXPECT_NEAR(step.delta_position[0], 0.1, 1e-12);
    EXPECT_NEAR(step.delta_linear[0], 0.1f, 1e-6f);
    EXPECT_TRUE(corrector.blending());
    int increments = 0;
    while (corrector.next_blend().action == correction_step::kind::blend) ++increments;
    EXPECT_EQ(increments, 4);
    EXPECT_FALSE(corrector.blending());
    // later history was shifted by the full difference so the next snapshot
    // compares against the corrected prediction
    auto again = corrector.compare(pose_at(8, 1.4, 0.4f));
    EXPECT_EQ(again.action, correction_step::kind::none);
}

TEST(OwnBallCorrector, HardSetsLargeErrorsAndDropsHistory) {
    own_ball_corrector corrector;
    corrector.record(pose_at(1, 0.0, 0.0f));
    auto step = corrector.compare(pose_at(1, 5.0, 0.0f));
    ASSERT_EQ(step.action, correction_step::kind::hard);
    EXPECT_DOUBLE_EQ(step.target.position[0], 5.0);
    EXPECT_EQ(corrector.history_size(), 0u);
    EXPECT_EQ(corrector.stats().hard, 1u);
    // a snapshot for a tick with no local state is counted, not applied
    EXPECT_EQ(corrector.compare(pose_at(2, 5.0, 0.0f)).action, correction_step::kind::none);
    EXPECT_EQ(corrector.stats().unmatched, 1u);
}
