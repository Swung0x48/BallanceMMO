// Unit tests for the pure physics-session bookkeeping: server input buffers
// and tick scheduling (session/timeline.hpp) and the client's own-ball
// correction planner (session/correction.hpp).
#include <gtest/gtest.h>

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
    std::vector<input_frame> far{frame_with_keys(1)};
    EXPECT_EQ(buffer.submit(input_buffer::kMaxLookahead + 5, far), 0);
    EXPECT_EQ(buffer.pending(), 0u);
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
