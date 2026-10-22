// Unit tests for the pure physics-session bookkeeping: server input buffers
// and tick scheduling (session/timeline.hpp), the client's own-ball
// correction planner (session/correction.hpp) and the retransmit window of
// the input message (message/session_input_msg.hpp).
#include <gtest/gtest.h>

#include <message/message_all.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <session/correction.hpp>
#include <session/lead_slew.hpp>
#include <session/own_correction.hpp>
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

// Late-player design 1.2: the substitute for a missing tick is the newest
// frame that has arrived for a tick at or below the cursor, fresh or STALE,
// and take() names the tick it came from - the source the held notice sends
// back to the owner.
TEST(InputBuffer, TheSubstituteIsTheNewestArrivalStaleIncluded) {
    input_buffer buffer;
    buffer.reset(0);
    bool fresh = false;
    uint32_t source = 0;
    EXPECT_EQ(buffer.submit(0, std::vector<input_frame>{frame_with_keys(1)}), 1);
    EXPECT_EQ(buffer.take(0, fresh, source).keys, 1);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(source, 0u);
    for (uint32_t t = 1; t <= 3; ++t) {
        EXPECT_EQ(buffer.take(t, fresh, source).keys, 1);
        EXPECT_FALSE(fresh);
        EXPECT_EQ(source, 0u);
    }
    // 1 and 2 arrive after they were simulated: stale, never applied at
    // their own ticks, but 2 is now the newest arrival.
    EXPECT_EQ(buffer.submit(1, std::vector<input_frame>{frame_with_keys(2), frame_with_keys(3)}), 0);
    EXPECT_EQ(buffer.stale(), 2u);
    EXPECT_EQ(buffer.substitute_tick(), 2u);
    EXPECT_EQ(buffer.take(4, fresh, source).keys, 3);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(source, 2u);
    // An older straggler changes nothing.
    EXPECT_EQ(buffer.submit(1, std::vector<input_frame>{frame_with_keys(9)}), 0);
    EXPECT_EQ(buffer.take(5, fresh, source).keys, 3);
    EXPECT_EQ(source, 2u);
    // A fresh frame is the newest arrival by construction ...
    EXPECT_EQ(buffer.submit(6, std::vector<input_frame>{frame_with_keys(6)}), 1);
    EXPECT_EQ(buffer.take(6, fresh, source).keys, 6);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(source, 6u);
    // ... and a stale one older than it does not replace it.
    EXPECT_EQ(buffer.submit(5, std::vector<input_frame>{frame_with_keys(5)}), 0);
    EXPECT_EQ(buffer.take(7, fresh, source).keys, 6);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(source, 6u);
    EXPECT_EQ(buffer.last_fresh_tick(), 6u);
    // A frame waiting ahead of the cursor is not a substitute: it has not
    // been reached yet.
    EXPECT_EQ(buffer.submit(9, std::vector<input_frame>{frame_with_keys(10)}), 1);
    EXPECT_EQ(buffer.take(8, fresh, source).keys, 6);
    EXPECT_EQ(source, 6u);
    // A re-anchor ahead of the world (a resync's base): a straggler between
    // the world's tick and the base precedes the client's numbering and is
    // no substitute, not even for the ticks the world still runs up to it.
    buffer.reset(20);
    EXPECT_EQ(buffer.submit(15, std::vector<input_frame>{frame_with_keys(15)}), 0);
    EXPECT_EQ(buffer.take(9, fresh, source).keys, 6);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(source, 6u);
}

TEST(InputBuffer, NothingArrivedYetIsTheDefaultFrameAndPersistentLatenessFollowsTheKeys) {
    input_buffer buffer;
    buffer.reset(10);
    bool fresh = true;
    uint32_t source = 0;
    EXPECT_EQ(buffer.take(10, fresh, source).keys, 0);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(source, INPUT_SOURCE_DEFAULT);
    EXPECT_EQ(buffer.substitute_tick(), INPUT_SOURCE_DEFAULT);
    EXPECT_FALSE(buffer.received_any());
    // Every frame 3 ticks late: none is ever fresh, and each tick applies
    // the frame sent 3 ticks before it, not the default frame for ever.
    for (uint32_t t = 11; t <= 60; ++t) {
        if (t >= 13) EXPECT_EQ(buffer.submit(t - 3, std::vector<input_frame>{frame_with_keys(static_cast<uint8_t>(t - 3))}), 0);
        const input_frame& applied = buffer.take(t, fresh, source);
        EXPECT_FALSE(fresh);
        if (t >= 13) {
            EXPECT_EQ(source, t - 3);
            EXPECT_EQ(applied.keys, static_cast<uint8_t>(t - 3));
        } else {
            EXPECT_EQ(source, INPUT_SOURCE_DEFAULT);
        }
    }
    EXPECT_FALSE(buffer.received_any());   // nothing was ever taken fresh
    EXPECT_EQ(buffer.stale(), 48u);
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

TEST(SectorLeadForPing, CoversTheRelayLegOnTopOfTheInputDelay) {
    using bmmo::session::sector_lead_for_ping;
    using bmmo::session::input_delay_for_ping;
    using bmmo::session::kInputDelayMaxTicks;
    // no measurable latency: the input delay floor, one tick, and the margin
    // term alone for the relay leg (16 ms -> 2 ticks)
    EXPECT_EQ(sector_lead_for_ping(6, 0), 9u);
    // the relay leg is a second one-way budget sized from the same round trip
    EXPECT_EQ(sector_lead_for_ping(7, 100), 7u + 1u + input_delay_for_ping(100, 0));
    EXPECT_EQ(sector_lead_for_ping(input_delay_for_ping(300, 6), 300),
              input_delay_for_ping(300, 6) + 1 + input_delay_for_ping(300, 0));
    EXPECT_GT(sector_lead_for_ping(input_delay_for_ping(300, 6), 300), sector_lead_for_ping(input_delay_for_ping(150, 6), 150));
    // both terms are capped, so the sum fits the byte SessionStart carries
    EXPECT_EQ(sector_lead_for_ping(kInputDelayMaxTicks, 100000), 2 * kInputDelayMaxTicks + 1);
    EXPECT_LE(sector_lead_for_ping(kInputDelayMaxTicks, 100000), 255u);
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

// The owner's own corrections (late-player design 1.3, step 4b): a
// difference the replay cannot see needs no replay, and while the notices
// keep coming one replay per kOwnReplayCoalesceFrames frames carries them,
// never later than the frame after which the earliest would leave the reach.
TEST(OwnCorrection, ReplaySeesOnlyWhatTheNavigationConsumes) {
    input_frame a{};
    a.flags = INPUT_FLAG_NAV_ACTIVE;
    a.cam_dir[2] = 1.0f;
    input_frame b = a;
    EXPECT_FALSE(replay_sees_difference(a, b));
    // a camera turned with no leaf key down, Shift/Space, ball type, flags
    // other than NAV_ACTIVE: nothing a replay feeds
    b.cam_dir[0] = 0.5f;
    b.keys = KEY_SHIFT | KEY_SPACE;
    b.ball_type = 2;
    b.flags |= INPUT_FLAG_PHYSICALIZED | INPUT_FLAG_PAUSED;
    EXPECT_FALSE(replay_sees_difference(a, b));
    // the same leaf down: now the camera steers the force
    a.keys = KEY_LEAF_0;
    b.keys = KEY_LEAF_0 | KEY_SPACE;
    EXPECT_TRUE(replay_sees_difference(a, b));
    b.cam_dir[0] = a.cam_dir[0];
    EXPECT_FALSE(replay_sees_difference(a, b));
    // a different leaf, and NAV_ACTIVE differing
    b.keys = KEY_LEAF_1;
    EXPECT_TRUE(replay_sees_difference(a, b));
    b = a;
    b.flags = 0;
    EXPECT_TRUE(replay_sees_difference(a, b));
    // navigation off in both: neither keys nor camera drive anything
    a.flags = 0;
    b.keys = KEY_LEAF_3;
    b.cam_up[1] = -1.0f;
    EXPECT_FALSE(replay_sees_difference(a, b));
}

TEST(OwnCorrection, PacerCoalescesAStreamInsideTheReach) {
    constexpr uint32_t kReach = 48;
    for (uint32_t delay: {30u, 41u, 42u, 46u, 47u}) {
        own_replay_pacer pacer;
        bool pending = false;
        uint32_t earliest = 0;
        uint32_t replays = 0, amended = 0, carried = 0;
        // one notice per frame for 120 frames, `delay` ticks after its tick
        // (the gate of note_held_section: now - (tick - 1) <= reach), then
        // nothing; `now` is the newest simulated tick at the frame's start
        // (the network is polled before the frame, at the same `now`)
        for (uint32_t now = 1000; now < 1200; ++now) {
            if (now < 1120) {
                const uint32_t tick = now - delay;
                ASSERT_LE(now - (tick - 1), kReach);
                ++amended;
                if (pending) ++carried;
                if (!pending || static_cast<int32_t>(tick - earliest) < 0) earliest = tick;
                pending = true;
            }
            pacer.frame();
            if (pending && pacer.due(now, earliest, kReach, true)) {
                EXPECT_LE(now - (earliest - 1), kReach) << "delay " << delay << " at " << now;
                ++replays;
                pacer.ran();
                pending = false;
            }
        }
        EXPECT_FALSE(pending) << "delay " << delay;
        // every notice amended; one replay per window of frames, the window
        // kOwnReplayCoalesceFrames or what the reach leaves after the delay
        // (a notice 47 deep has to replay on its own frame)
        const uint32_t window = std::min(kOwnReplayCoalesceFrames, kReach - delay);
        EXPECT_EQ(amended, 120u);
        EXPECT_LE(replays, 120u / window + 1) << "delay " << delay;
        EXPECT_GE(replays + 1, 120u / window) << "delay " << delay;
        EXPECT_EQ(amended, replays + carried) << "delay " << delay;
    }
}

TEST(OwnCorrection, PacerRunsTheFirstAtOnceAndAtTheReachDeadline) {
    own_replay_pacer pacer;
    pacer.frame();
    // nothing ran yet: due at once
    EXPECT_TRUE(pacer.due(500, 460, 48, true));
    pacer.ran();
    pacer.frame();
    // one frame later: may wait while the next frame still reaches 459
    EXPECT_FALSE(pacer.due(501, 460, 48, true));   // 502 - 459 = 43
    EXPECT_FALSE(pacer.due(501, 455, 48, true));   // 502 - 454 = 48
    EXPECT_TRUE(pacer.due(501, 454, 48, true));    // 502 - 453 = 49: the last frame it can run
    // a tick not simulated yet is the caller's to drop
    EXPECT_TRUE(pacer.due(501, 502, 48, true));
    // the server has our frames again: nothing will join it, run now
    EXPECT_TRUE(pacer.due(501, 460, 48, false));
    for (uint32_t k = 1; k < kOwnReplayCoalesceFrames; ++k) pacer.frame();
    EXPECT_TRUE(pacer.due(506, 500, 48, true));
    // a new numbering: the previous epoch's replay holds nothing back
    pacer.ran();
    pacer.frame();
    EXPECT_FALSE(pacer.due(507, 505, 48, true));
    pacer.reset();
    EXPECT_TRUE(pacer.due(507, 505, 48, true));
}

// The end of an episode: the last notices arrive, then the server's header
// says it has our frames again.  The waiting replay runs on that frame, not
// at the rate limit's or the reach's edge, so the own ball is right as soon
// as before the coalescing (the A1 criterion "own ball 0.0000 from T_off+48").
TEST(OwnCorrection, PacerRunsAtOnceWhenTheNoticesStop) {
    constexpr uint32_t kReach = 48, kDelay = 42;
    own_replay_pacer pacer;
    bool pending = false;
    uint32_t earliest = 0, last_amended = 0, replays = 0, last_replay = 0;
    for (uint32_t now = 1000; now < 1100; ++now) {
        const bool coming = now < 1030;   // the header turns at 1030
        if (now < 1031) {                 // one late notice still arrives after it
            const uint32_t tick = now - kDelay;
            if (!pending || static_cast<int32_t>(tick - earliest) < 0) earliest = tick;
            pending = true;
            last_amended = now;
        }
        pacer.frame();
        if (pending && pacer.due(now, earliest, kReach, coming)) {
            ASSERT_LE(now - (earliest - 1), kReach);
            ++replays;
            last_replay = now;
            pacer.ran();
            pending = false;
        }
    }
    EXPECT_FALSE(pending);
    EXPECT_EQ(last_replay, last_amended);   // the frame the last amendment arrived on
    EXPECT_LE(replays, 30u / std::min(kOwnReplayCoalesceFrames, kReach - kDelay) + 3);
}

// A lifecycle or forced rewind carries a waiting own replay when it started
// at or before that replay's anchor (the earliest amended tick - 1).
TEST(OwnCorrection, AnEarlierRewindCarriesTheWaitingReplay) {
    EXPECT_TRUE(replay_subsumes(459, 460));
    EXPECT_TRUE(replay_subsumes(440, 460));
    EXPECT_FALSE(replay_subsumes(460, 460));   // tick 460 itself was not replayed
    EXPECT_FALSE(replay_subsumes(470, 460));
    // across the wrap of the tick counter
    EXPECT_TRUE(replay_subsumes(0xFFFFFFF0u, 5u));
    EXPECT_FALSE(replay_subsumes(5u, 0xFFFFFFF0u));
}

// ---------------------------------------------------------------------------
// SessionLead on the client (late-player design 2.4, session/lead_slew.hpp).

namespace {
    // The headless client's pacer in tick units: frame f is due once the
    // wall clock, measured from the origin, reaches f; a slew moves the
    // origin.  Each step is a quarter of a tick period, as the pacer polls
    // the network several times per slot.
    struct pacer_model {
        double wall = 0.0;
        double origin = 0.0;          // in ticks; one tick earlier = the clock one tick ahead
        uint64_t next_frame = 1;
        uint32_t tick_base = 1000;    // frame 1 is tick 1000
        std::vector<uint32_t> ticks;  // the ticks stepped, in order

        double expected() const { return wall - origin; }
        uint32_t current_tick() const { return tick_base + static_cast<uint32_t>(next_frame - 2); }
        // One poll: the slew advances, then every frame due runs.
        void poll(lead_slew& slew, double dt) {
            wall += dt;
            origin -= slew.advance(dt, expected() - static_cast<double>(next_frame));
            while (static_cast<double>(next_frame) <= expected()) {
                ticks.push_back(tick_base + static_cast<uint32_t>(next_frame - 1));
                ++next_frame;
            }
        }
        void run(lead_slew& slew, double periods) {
            for (int i = 0; i < static_cast<int>(periods * 4.0 + 0.5); ++i) poll(slew, 0.25);
        }
    };
}

// [N, E) split at the client's current tick c: the simulated part is amended,
// the rest forced, and the first tick played on the client's own input again
// is E - where the server's buffer, re-anchored at E, takes its frames from.
TEST(LeadSlew, TheHeldRangeSplitsAtTheClientsTick) {
    // c = 1020: amend [1000, 1021), force [1021, 1050)
    auto r = lead_split_range(1000, 1050, 1021);
    EXPECT_TRUE(r.amend());
    EXPECT_EQ(r.amend_from, 1000u);
    EXPECT_EQ(r.amend_to, 1021u);
    EXPECT_TRUE(r.force());
    EXPECT_EQ(r.force_from, 1021u);
    EXPECT_EQ(r.force_to, 1050u);
    // c >= E (the round trip was over-estimated): everything amended, nothing forced
    r = lead_split_range(1000, 1050, 1060);
    EXPECT_EQ(r.amend_from, 1000u);
    EXPECT_EQ(r.amend_to, 1050u);
    EXPECT_FALSE(r.force());
    r = lead_split_range(1000, 1050, 1050);
    EXPECT_EQ(r.amend_to, 1050u);
    EXPECT_FALSE(r.force());
    // c < N (the client runs behind the session's next tick): the whole range
    // is forced, and the ticks before N are not the range's
    r = lead_split_range(1000, 1050, 990);
    EXPECT_FALSE(r.amend());
    EXPECT_EQ(r.force_from, 1000u);
    EXPECT_EQ(r.force_to, 1050u);
    // no range (a lower, or a reset that held nothing): the client only slews
    r = lead_split_range(0, 1050, 1021);
    EXPECT_FALSE(r.amend());
    EXPECT_FALSE(r.force());
    // across the wrap of the tick counter
    r = lead_split_range(0xFFFFFFF0u, 0x10u, 0u);
    EXPECT_EQ(r.amend_from, 0xFFFFFFF0u);
    EXPECT_EQ(r.amend_to, 0u);
    EXPECT_EQ(r.force_from, 0u);
    EXPECT_EQ(r.force_to, 0x10u);
}

// A raise of k moves the origin by exactly k, one tick per tick period: the
// pacer runs two frames per slot for k slots and then one again.  Every tick
// is stepped once, in order - nothing skipped or renumbered.
TEST(LeadSlew, ARaiseRunsTwoFramesPerSlotUntilItHasGainedK) {
    pacer_model pacer;
    lead_slew slew;
    pacer.run(slew, 40.0);
    const size_t before = pacer.ticks.size();
    const double origin_before = pacer.origin;
    constexpr int k = 12;
    slew.start(7, k);
    EXPECT_EQ(slew.outstanding(), k);
    EXPECT_DOUBLE_EQ(lead_rebase_threshold(slew.outstanding()), 33.0 + k);
    pacer.run(slew, k);
    EXPECT_TRUE(slew.done());
    EXPECT_EQ(slew.outstanding(), 0);
    EXPECT_DOUBLE_EQ(origin_before - pacer.origin, static_cast<double>(k));
    const size_t during = pacer.ticks.size() - before;
    EXPECT_GE(during, static_cast<size_t>(2 * k - 1));
    EXPECT_LE(during, static_cast<size_t>(2 * k + 1));
    EXPECT_EQ(slew.finish(), k);   // the ack's applied
    EXPECT_FALSE(slew.active());
    EXPECT_DOUBLE_EQ(lead_rebase_threshold(slew.outstanding()), 33.0);
    // afterwards one frame per slot again, the origin k ticks earlier for good
    const size_t settled = pacer.ticks.size();
    pacer.run(slew, 20.0);
    EXPECT_EQ(pacer.ticks.size() - settled, 20u);
    EXPECT_DOUBLE_EQ(origin_before - pacer.origin, static_cast<double>(k));
    for (size_t i = 1; i < pacer.ticks.size(); ++i) ASSERT_EQ(pacer.ticks[i], pacer.ticks[i - 1] + 1);
}

// A lower moves the origin one tick later every tenth period: 0.9x, never a
// stop, and exactly |k| in the end.
TEST(LeadSlew, ALowerRunsAtNineTenths) {
    pacer_model pacer;
    lead_slew slew;
    pacer.run(slew, 40.0);
    const size_t before = pacer.ticks.size();
    const double origin_before = pacer.origin;
    slew.start(9, -5);
    EXPECT_EQ(slew.outstanding(), -5);
    EXPECT_DOUBLE_EQ(lead_rebase_threshold(slew.outstanding()), 38.0);
    pacer.run(slew, 52.0);
    EXPECT_TRUE(slew.done());
    EXPECT_DOUBLE_EQ(pacer.origin - origin_before, 5.0);
    const size_t during = pacer.ticks.size() - before;
    EXPECT_GE(during, 46u);   // 52 periods less 5
    EXPECT_LE(during, 48u);
    EXPECT_EQ(slew.finish(), -5);
}

// A raise gains nothing while the pacer is behind (a slow CPU, a stall): the
// slew never makes a backlog of its own, so 33 + the outstanding slew still
// tells a stall from a slew.  A burst of wall time moves one tick, not many.
TEST(LeadSlew, ARaiseWaitsWhileThePacerIsBehind) {
    lead_slew slew;
    slew.start(3, 4);
    EXPECT_EQ(slew.advance(1.0, 1.5), 0);
    EXPECT_EQ(slew.advance(1.0, 5.0), 0);
    EXPECT_EQ(slew.applied(), 0);
    EXPECT_EQ(slew.advance(10.0, -0.5), 1);   // ten periods at once: one tick
    EXPECT_EQ(slew.advance(0.5, -0.5), 0);    // the credit was capped at one period
    EXPECT_EQ(slew.advance(0.5, -0.5), 1);
    EXPECT_EQ(slew.applied(), 2);
    // a lower is never held back: being behind is what it reduces
    lead_slew lower;
    lower.start(4, -1);
    EXPECT_EQ(lower.advance(10.0, 8.0), -1);
    EXPECT_TRUE(lower.done());
}

// A rebase does not end a slew: the ticks it moved are stepped already (it
// gains only while the pacer is less than a frame behind), so the restarted
// schedule loses none of them, and the rest is moved on the new schedule.
// The client's resync request that follows a rebase is what ends it - and
// then the ack says the whole delta (finish_reanchored), since the server
// re-anchors the client at L_eff with whatever was acknowledged: an ack of
// the part moved would leave the new clock short of the lead decided.
TEST(LeadSlew, ARebaseKeepsTheSlewAndAResyncAcknowledgesTheWholeDelta) {
    pacer_model pacer;
    lead_slew slew;
    pacer.run(slew, 10.0);
    slew.start(11, 10);
    pacer.run(slew, 4.0);
    EXPECT_EQ(slew.applied(), 4);
    EXPECT_EQ(slew.outstanding(), 6);
    EXPECT_DOUBLE_EQ(lead_rebase_threshold(slew.outstanding()), 39.0);
    // a 50-period stall, then the schedule restarts at the next frame: the
    // slew waits while behind, and carries on from 4 once the pacer is not
    const size_t stepped = pacer.ticks.size();
    pacer.wall += 50.0;
    EXPECT_EQ(slew.advance(50.0, pacer.expected() - static_cast<double>(pacer.next_frame)), 0);
    EXPECT_GT(pacer.expected() - static_cast<double>(pacer.next_frame), lead_rebase_threshold(slew.outstanding()));
    pacer.origin = pacer.wall - static_cast<double>(pacer.next_frame);   // the rebase
    EXPECT_TRUE(slew.active());
    EXPECT_EQ(slew.applied(), 4);
    const double origin = pacer.origin;
    pacer.run(slew, 6.0);
    EXPECT_TRUE(slew.done());
    EXPECT_DOUBLE_EQ(origin - pacer.origin, 6.0);
    EXPECT_EQ(slew.finish(), 10);
    for (size_t i = stepped + 1; i < pacer.ticks.size(); ++i) ASSERT_EQ(pacer.ticks[i], pacer.ticks[i - 1] + 1);
    // a resync request part-way: the whole delta is acknowledged, and the
    // slew moves nothing more (the assignment sets the clock)
    slew.start(12, 10);
    pacer.run(slew, 3.0);
    EXPECT_EQ(slew.applied(), 3);
    EXPECT_EQ(slew.finish_reanchored(), 10);
    EXPECT_FALSE(slew.active());
    EXPECT_EQ(slew.outstanding(), 0);
    const double after = pacer.origin;
    pacer.run(slew, 10.0);
    EXPECT_DOUBLE_EQ(pacer.origin, after);
    // a lower the same way, on its own side; idle, it acknowledges nothing
    slew.start(13, -3);
    EXPECT_EQ(slew.advance(10.0, 0.0), -1);
    EXPECT_EQ(slew.finish_reanchored(), -3);
    EXPECT_EQ(slew.finish_reanchored(), 0);
    // superseded by a newer decision: what was moved, never the rest
    slew.start(14, 5);
    EXPECT_EQ(slew.advance(1.0, 0.0), 1);
    EXPECT_EQ(slew.finish(), 1);
    // a zero delta is no slew
    slew.start(15, 0);
    EXPECT_FALSE(slew.active());
}

// The forced range is capped at the client's ring of own frames: the ticks
// past it are neither forced nor learned, so their notices settle them.
TEST(LeadSlew, TheForcedRangeIsCappedAtTheRing) {
    auto r = lead_cap_forced(lead_split_range(1000, 1500, 1021), 256);
    EXPECT_EQ(r.amend_to, 1021u);
    EXPECT_EQ(r.force_from, 1021u);
    EXPECT_EQ(r.force_to, 1021u + 256u);
    r = lead_cap_forced(lead_split_range(1000, 1050, 1021), 256);   // under the cap: unchanged
    EXPECT_EQ(r.force_to, 1050u);
    r = lead_cap_forced(lead_split_range(1000, 1050, 1060), 256);   // nothing forced: unchanged
    EXPECT_FALSE(r.force());
    EXPECT_EQ(r.amend_to, 1050u);
    r = lead_cap_forced(lead_split_range(0xFFFFFF00u, 0x200u, 0xFFFFFF10u), 256);   // across the wrap
    EXPECT_EQ(r.force_from, 0xFFFFFF10u);
    EXPECT_EQ(r.force_to, 0x10u);
}

// The whole client side of a raise, as the server sized it (lead.hpp: E =
// c_est + 2k + 2): the client at c receives it, amends [N, c], forces (c, E)
// and slews.  The frames it sends are stamped with their own ticks: below E
// the held frame (stale to the server, whose buffer holds that frame anyway),
// from E on its own input, which the server's buffer takes.  The slew is
// over before the client reaches E, so E's frame is the first of the new
// schedule.
TEST(LeadSlew, FramesFromEOnAreTheClientsOwnAndTheSlewIsOverByThen) {
    constexpr uint32_t N = 2000;
    constexpr int k = 15;
    pacer_model pacer;
    pacer.tick_base = 1980;
    lead_slew slew;
    pacer.run(slew, 40.0);            // the client is at c = 2019
    const uint32_t c = pacer.current_tick();
    ASSERT_EQ(c, 2019u);
    const uint32_t E = c + 2 * k + 2;   // the server's estimate of c was exact
    const auto r = lead_split_range(N, E, c + 1);
    EXPECT_EQ(r.amend_from, N);
    EXPECT_EQ(r.amend_to, c + 1);
    EXPECT_EQ(r.force_from, c + 1);
    EXPECT_EQ(r.force_to, E);
    slew.start(21, k);
    std::map<uint32_t, char> sent;      // tick -> 'h' held, 'o' own
    size_t seen = pacer.ticks.size();
    const size_t stop = seen + 200;
    uint32_t tick_at_done = 0;
    while (pacer.ticks.size() < stop) {
        pacer.poll(slew, 0.25);
        if (slew.active() && slew.done()) {
            tick_at_done = pacer.current_tick();
            EXPECT_EQ(slew.finish(), k);
        }
        for (; seen < pacer.ticks.size(); ++seen) {
            const uint32_t t = pacer.ticks[seen];
            const bool forced = static_cast<int32_t>(t - r.force_from) >= 0 && static_cast<int32_t>(t - r.force_to) < 0;
            ASSERT_TRUE(sent.emplace(t, forced ? 'h' : 'o').second) << "tick " << t << " stepped twice";
        }
    }
    ASSERT_NE(tick_at_done, 0u);
    EXPECT_LT(tick_at_done, E);
    // every tick after c stepped once, held up to E - 1, own from E on
    uint32_t expect = c + 1;
    for (const auto& [t, what]: sent) {
        ASSERT_EQ(t, expect++);
        EXPECT_EQ(what, static_cast<int32_t>(t - E) < 0 ? 'h' : 'o') << "tick " << t;
    }
    EXPECT_EQ(sent.begin()->first, c + 1);
    EXPECT_EQ(sent.at(E), 'o');
    EXPECT_EQ(sent.at(E - 1), 'h');
}
