// Unit tests for the pure physics-session bookkeeping: server input buffers
// and tick scheduling (session/timeline.hpp), the client's own-ball
// correction planner (session/correction.hpp) and the retransmit window of
// the input message (message/session_input_msg.hpp).
#include <gtest/gtest.h>

#include <message/message_all.hpp>

#include <algorithm>
#include <string>

#include <session/correction.hpp>
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
