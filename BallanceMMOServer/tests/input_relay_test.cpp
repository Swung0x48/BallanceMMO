// session/input_relay.hpp: the server's relay ring (protocol 29: every tick
// after the recipient's acknowledgement; protocol 30: camera headings) and
// the client's reassembly rules; session/camera_heading.hpp.
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <optional>
#include <set>

#include <message/message_all.hpp>
#include <session/input_relay.hpp>

using bmmo::session::input_frame;
using bmmo::session::pending_relay;
using bmmo::session::relay_ring;
using bmmo::session::relay_seen;
using bmmo::session::relay_store;
using bmmo::session::relay_store_result;

namespace {
    input_frame frame(uint8_t keys) {
        input_frame f{};
        f.keys = keys;
        return f;
    }
    input_frame turning(uint8_t keys, float angle) {
        input_frame f{};
        f.keys = keys;
        f.cam_dir[0] = angle;
        f.cam_dir[2] = 1.0f - angle;
        return f;
    }
    using applied_list = std::vector<std::pair<uint32_t, input_frame>>;

    // A datagram as server.cpp on_session_inputs writes it.
    using bases_t = std::map<uint32_t, bmmo::session::camera_bases>;
    bmmo::session_remote_input_msg to_message(relay_ring::datagram d, uint32_t tick, uint32_t epoch = 0) {
        bmmo::session_remote_input_msg msg;
        msg.session = 1;
        msg.tick = tick;
        msg.epoch = epoch;
        msg.from = d.from;
        for (auto& e: d.entries) msg.entries.push_back({e.player, std::move(e.frames), std::move(e.headings), e.base, e.back});
        for (const auto& h: d.held) {
            bmmo::session_remote_input_msg::held_record r;
            r.back = static_cast<uint8_t>(tick - h.tick);
            if (bmmo::session::held_inline(h.tick, h.source, epoch)) {
                r.has_frame = true;
                r.frame = h.frame;
            } else {
                r.source = h.source;
            }
            msg.held.push_back(r);
        }
        return msg;
    }
    bmmo::session_remote_input_msg parse(bmmo::session_remote_input_msg& msg) {
        msg.raw = std::stringstream{};
        EXPECT_TRUE(msg.serialize());
        bmmo::session_remote_input_msg parsed;
        parsed.raw.write(msg.raw.str().data(), static_cast<std::streamsize>(msg.size()));
        EXPECT_TRUE(parsed.deserialize());
        return parsed;
    }
    size_t datagram_bytes(const relay_ring::datagram& d, uint32_t epoch) {
        size_t bytes = bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes;
        for (const auto& e: d.entries) bytes += relay_ring::entry_bytes(e);
        for (const auto& h: d.held) bytes += relay_ring::held_bytes(h, epoch);
        return bytes;
    }
}

// Every other member's frames for every tick after the recipient's
// acknowledgement: however many datagrams were lost, the next one covers
// the whole span; a member that joined inside it starts at its join, a gap
// in the ticks ends a run (the run before it still goes, ending early), and
// a leaver's last frames keep coming until acknowledged.  The recipient
// itself is left out.
TEST(RelayRing, CoversEverythingAfterTheAcknowledgement) {
    relay_ring ring;
    ring.reanchor(1, 9);
    for (uint32_t t = 10; t <= 13; ++t)
        ring.push(t, applied_list{{1, frame(1)}, {2, frame(static_cast<uint8_t>(t))}});
    auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams.size(), 1u);
    EXPECT_EQ(datagrams[0].from, 9u);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].player, 2u);
    ASSERT_EQ(datagrams[0].entries[0].frames.size(), 4u);   // 10..13: nothing acknowledged yet
    // Acknowledged through 12: only 13 is left; never backwards, never past the newest.
    ring.acknowledge(1, 12);
    ring.acknowledge(1, 11);
    EXPECT_EQ(ring.acked(1), 12u);
    ring.acknowledge(1, 99);
    EXPECT_EQ(ring.acked(1), 13u);
    ring.reanchor(1, 12);
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams[0].entries[0].frames.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].frames[0].keys, 13);
    // A tick never stepped (14 missing), a joiner at 16, and 3 and 4 gone at 18.
    ring.push(15, applied_list{{1, frame(1)}, {2, frame(15)}});
    ring.push(16, applied_list{{1, frame(1)}, {2, frame(16)}, {3, frame(160)}});
    ring.push(17, applied_list{{1, frame(1)}, {2, frame(17)}, {3, frame(170)}, {4, frame(4)}});
    ring.push(18, applied_list{{1, frame(1)}, {2, frame(18)}});
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    const auto& e = datagrams[0].entries;
    ASSERT_EQ(e.size(), 4u);
    EXPECT_TRUE(e[0].player == 2 && e[0].frames.size() == 1 && e[0].back == 5);   // 13, before the gap
    EXPECT_TRUE(e[1].player == 2 && e[1].frames.size() == 4 && e[1].back == 0);   // 15..18
    EXPECT_TRUE(e[2].player == 3 && e[2].frames.size() == 2 && e[2].back == 1);   // 16, 17: from its join to its leave
    EXPECT_TRUE(e[3].player == 4 && e[3].frames.size() == 1 && e[3].back == 1);   // 17
    auto msg = to_message(datagrams[0], 18);
    const auto parsed = parse(msg);
    ASSERT_EQ(parsed.entries.size(), 4u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[2], 0), 16u);
    EXPECT_EQ(parsed.frame_tick(parsed.entries[0], 0), 13u);
    EXPECT_EQ(msg.size(), datagram_bytes(datagrams[0], 0));
    // Acknowledged past the leavers' last tick: they are done.
    ring.acknowledge(1, 17);
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].frames.size(), 1u);
    // A push at an older tick replaces the ring from there.
    ring.push(18, applied_list{{1, frame(1)}, {2, frame(77)}});
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].frames.back().keys, 77);
    EXPECT_EQ(ring.newest_tick(), 18u);
}

// An acknowledgement older than the history: header-only notices whose
// `from` is past what the recipient holds - its cue to resync - at gaps of
// 1, 2, 4 .. 64 ticks, nothing in between; an acknowledgement inside the
// history brings the whole span back.
TEST(RelayRing, PastTheHistoryOnlyHeaderNoticesWithBackoff) {
    relay_ring ring;
    ring.reanchor(1, 0);
    uint32_t t = 50;
    for (; t <= 50 + bmmo::session::kRelayHistory + 20; ++t) ring.push(t, applied_list{{1, frame(1)}, {2, frame(2)}});
    std::vector<uint32_t> sent;
    for (int i = 0; i < 300; ++i, ++t) {
        ring.push(t, applied_list{{1, frame(1)}, {2, frame(2)}});
        const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
        if (datagrams.empty()) continue;
        ASSERT_EQ(datagrams.size(), 1u);
        EXPECT_TRUE(datagrams[0].entries.empty());
        EXPECT_EQ(datagrams[0].from, t - bmmo::session::kRelayHistory);
        sent.push_back(t);
    }
    std::vector<uint32_t> gaps;
    for (size_t i = 1; i < sent.size(); ++i) gaps.push_back(sent[i] - sent[i - 1]);
    ASSERT_GE(gaps.size(), 8u);
    EXPECT_EQ(std::vector<uint32_t>(gaps.begin(), gaps.begin() + 8), (std::vector<uint32_t>{1, 2, 4, 8, 16, 32, 64, 64}));
    // Its uplink still arriving (an acknowledgement that moves nothing): a
    // notice every tick, the next one at once.
    for (int i = 0; i < 3; ++i, ++t) {
        ring.push(t, applied_list{{1, frame(1)}, {2, frame(2)}});
        ring.acknowledge(1, 0);
        const auto notice = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
        ASSERT_EQ(notice.size(), 1u) << i;
        EXPECT_TRUE(notice[0].entries.empty());
    }
    ring.acknowledge(1, t - 10);
    ring.push(t, applied_list{{1, frame(1)}, {2, frame(2)}});
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams.size(), 1u);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].frames.size(), 10u);
}

// The owner's snap: the retail turns' rounding (here 1e-5) lands on the
// exact quarter turns of the first camera, four of which give back its bits;
// another orientation becomes the base; no camera stays none.
TEST(CameraHeading, SnapsToTheQuarterTurnsOfTheBase) {
    using namespace bmmo::session;
    const camera_rows base{0.99999994f, 0.0f, -3.1e-7f, 0.0f, 1.0f, 0.0f, 3.1e-7f, 0.0f, 0.99999994f};
    EXPECT_TRUE(same_rows(quarter_turns(base, 4), base));
    heading_snap snap;
    EXPECT_TRUE(zero_camera(snap.apply(camera_rows{})));
    EXPECT_TRUE(same_rows(snap.apply(base), base));
    camera_rows noisy = quarter_turns(base, 3);
    for (float& x: noisy) x += 1e-5f;
    EXPECT_TRUE(same_rows(snap.apply(noisy), quarter_turns(base, 3)));
    const camera_rows diagonal{0.7071068f, 0.0f, -0.7071068f, 0.0f, 1.0f, 0.0f, 0.7071068f, 0.0f, 0.7071068f};
    EXPECT_TRUE(same_rows(snap.apply(diagonal), diagonal));
    EXPECT_TRUE(same_rows(snap.apply(quarter_turns(diagonal, 1)), quarter_turns(diagonal, 1)));
}

// Headings on the relay: the first camera inline, then quarter turns of it;
// the base attached to an entry that starts with a turn until the recipient
// acknowledges the datagram that first carried it; frames come back bit for
// bit; a recipient numbered later gets the base again.
TEST(RelayRing, HeadingsCarryTheBaseUntilAcknowledgedAndRoundTripExactly) {
    using bmmo::session::kHeadingInline;
    relay_ring ring;
    ring.reanchor(1, 99);
    const input_frame north = turning(1, 0.0f);
    input_frame east = north;
    bmmo::session::set_camera(east, bmmo::session::quarter_turns(bmmo::session::camera_of(north), 1));
    east.keys = 2;
    for (uint32_t t = 100; t <= 103; ++t) ring.push(t, applied_list{{1, north}, {2, t < 102 ? north : east}});
    auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    const auto& entry = datagrams[0].entries[0];
    EXPECT_EQ(entry.headings, (std::vector<uint8_t>{kHeadingInline, 0, 1, 1}));
    EXPECT_FALSE(entry.base);
    EXPECT_EQ(relay_ring::entry_bytes(entry), 6u + (4 + 36) + 4 + 4);   // runs: inline, north, east
    bases_t bases;
    auto msg = to_message(datagrams[0], 103);
    auto entries = parse(msg).resolved_entries(bases);
    ASSERT_TRUE(entries);
    for (size_t i = 0; i < 4; ++i) {
        const input_frame& want = i < 2 ? north : east;
        EXPECT_TRUE(want.keys == (*entries)[0].frames[i].keys && bmmo::session::same_camera(want, (*entries)[0].frames[i])) << i;
    }
    // Acknowledged short of that datagram: the next span starts with a turn
    // and carries the base; once acknowledged, it does not.
    ring.acknowledge(1, 101);
    ring.push(104, applied_list{{1, north}, {2, east}});
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_TRUE(datagrams[0].entries[0].base);
    EXPECT_EQ(datagrams[0].entries[0].base->since, 100u);
    ring.acknowledge(1, 103);
    ring.push(105, applied_list{{1, north}, {2, east}});
    datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    EXPECT_FALSE(datagrams[0].entries[0].base);
    msg = to_message(datagrams[0], 105);
    bases_t none;
    EXPECT_FALSE(parse(msg).resolved_entries(none));   // a client without the base cannot use it
    entries = parse(msg).resolved_entries(bases);
    ASSERT_TRUE(entries);
    EXPECT_TRUE(bmmo::session::same_camera((*entries)[0].frames.back(), east));
    // Re-numbered (a resync): the base again.
    ring.reanchor(1, 104);
    EXPECT_TRUE(ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget)[0].entries[0].base);
}

// Whole entries per datagram of one packet: a room of 64 with every frame
// different (keys every tick) splits into several, none over the budget,
// every other member in exactly one; the budget counts what goes on the wire.
TEST(RelayRing, PacksDatagramsToTheBudget) {
    relay_ring ring;
    ring.reanchor(1, 99);
    for (uint32_t t = 100; t < 120; ++t) {
        applied_list applied;
        for (uint32_t p = 1; p <= 64; ++p) applied.emplace_back(p, turning(static_cast<uint8_t>(t + p), 0.5f));
        ring.push(t, applied);
    }
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    EXPECT_GT(datagrams.size(), 1u);
    std::set<uint32_t> players;
    for (const auto& d: datagrams) {
        for (const auto& e: d.entries) EXPECT_TRUE(players.insert(e.player).second);
        auto msg = to_message(d, 119);
        EXPECT_EQ(parse(msg).entries.size(), d.entries.size());
        EXPECT_EQ(msg.size(), datagram_bytes(d, 0));
        EXPECT_LE(msg.size(), bmmo::session::kRelayDatagramBudget);
    }
    EXPECT_EQ(players.size(), 63u);
}

TEST(RelayStore, FirstDeliveryWinsDuplicatesAndStaleFramesDoNothing) {
    std::map<uint32_t, input_frame> ring;
    EXPECT_EQ(relay_store(ring, 100, frame(1), 4), relay_store_result::stored);
    EXPECT_EQ(relay_store(ring, 100, frame(2), 4), relay_store_result::duplicate);
    EXPECT_EQ(ring.at(100).keys, 1);
    EXPECT_EQ(relay_store(ring, 101, frame(3), 4), relay_store_result::stored);
    EXPECT_EQ(relay_store(ring, 102, frame(4), 4), relay_store_result::stored);
    EXPECT_EQ(relay_store(ring, 103, frame(5), 4), relay_store_result::stored);
    // A late frame still inside what the ring keeps is stored and evicts the oldest.
    EXPECT_EQ(relay_store(ring, 104, frame(6), 4), relay_store_result::stored);
    EXPECT_EQ(ring.size(), 4u);
    EXPECT_EQ(ring.count(100), 0u);
    // Older than the ring would hold: stale.
    EXPECT_EQ(relay_store(ring, 100, frame(7), 4), relay_store_result::stale);
    EXPECT_EQ(relay_store(ring, 99, frame(7), 4), relay_store_result::stale);
    EXPECT_EQ(ring.size(), 4u);
}

TEST(RelaySeen, OncePerPlayerAndTick) {
    relay_seen seen(3);
    EXPECT_TRUE(seen.first(1, 10));
    EXPECT_FALSE(seen.first(1, 10));
    EXPECT_TRUE(seen.first(2, 10));    // another player, the same tick
    EXPECT_TRUE(seen.first(1, 11));
    EXPECT_TRUE(seen.first(1, 12));
    EXPECT_TRUE(seen.first(1, 13));    // 10 falls out of the bound
    EXPECT_TRUE(seen.first(1, 10));
    EXPECT_FALSE(seen.first(1, 13));
}

// The frame a replay drives a mirror with: the relayed one, the nearest
// earlier one for a hole between relayed frames, none for the tail.
TEST(RelayFrameOrEarlier, HolesTakeTheEarlierFrameTheTailTakesNothing) {
    std::map<uint32_t, input_frame> ring;
    ring.emplace(10, frame(1));
    ring.emplace(11, frame(2));
    ring.emplace(14, frame(5));
    ASSERT_NE(bmmo::session::relay_frame_or_earlier(ring, 11), nullptr);
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 11)->keys, 2);
    ASSERT_NE(bmmo::session::relay_frame_or_earlier(ring, 12), nullptr);   // a hole: 11's frame
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 12)->keys, 2);
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 13)->keys, 2);
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 15), nullptr);   // the tail
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 9), nullptr);    // before anything
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier({}, 1), nullptr);
}

// Step RC1: the core's replay rule - relay_frame_or_earlier's frame, and for
// the tail past the newest relayed frame that newest frame (hold-last, the
// live predictor's); still nothing before the first relayed frame.
TEST(RelayFrameOrNewest, TheTailTakesTheNewestFrameHolesAndTheStartAsBefore) {
    std::map<uint32_t, input_frame> ring;
    ring.emplace(10, frame(1));
    ring.emplace(11, frame(2));
    ring.emplace(14, frame(5));
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 11)->keys, 2);
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 12)->keys, 2);   // a hole: 11's frame
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 13)->keys, 2);
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 14)->keys, 5);
    ASSERT_NE(bmmo::session::relay_frame_or_newest(ring, 15), nullptr);   // the tail: 14's frame
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 15)->keys, 5);
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 40)->keys, 5);
    EXPECT_EQ(bmmo::session::relay_frame_or_newest(ring, 9), nullptr);    // before anything
    EXPECT_EQ(bmmo::session::relay_frame_or_newest({}, 1), nullptr);
    // The legacy rule is unchanged.
    EXPECT_EQ(bmmo::session::relay_frame_or_earlier(ring, 15), nullptr);
}

TEST(PendingRelay, ParksFramesAndMovesThemIntoTheMirrorsRing) {
    pending_relay pending(8);
    pending.store(7, 50, frame(1));
    pending.store(7, 52, frame(3));
    pending.store(7, 51, frame(2));
    pending.store(7, 52, frame(9));     // a duplicate: the first stays
    pending.store(8, 50, frame(4));
    EXPECT_EQ(pending.players(), 2u);
    std::map<uint32_t, input_frame> ring;
    ring.emplace(51, frame(20));         // the ring's own frame wins
    input_frame newest{};
    EXPECT_EQ(pending.take(7, ring, 128, &newest), 52u);
    EXPECT_EQ(newest.keys, 3);
    ASSERT_EQ(ring.size(), 3u);
    EXPECT_EQ(ring.at(50).keys, 1);
    EXPECT_EQ(ring.at(51).keys, 20);
    EXPECT_EQ(ring.at(52).keys, 3);
    EXPECT_EQ(pending.players(), 1u);
    EXPECT_EQ(pending.take(7, ring, 128, &newest), 0u);   // nothing left for 7
    EXPECT_EQ(pending.take(9, ring, 128, &newest), 0u);   // never parked
}

// ---------------------------------------------------------------------
// Protocol 7: the held section (late-player design 1.1-1.2)
// ---------------------------------------------------------------------

namespace {
    using bmmo::session::applied_input;
    using bmmo::session::kHeldSourceDefault;

    applied_input fresh_input(uint32_t player, const input_frame& f) {
        return applied_input{player, f, false, kHeldSourceDefault};
    }
    applied_input held_input(uint32_t player, const input_frame& f, uint32_t source) {
        return applied_input{player, f, true, source};
    }
}

// A recipient gets its own held ticks of the last 32, from its epoch on, and
// nobody else's.
TEST(RelayHeld, TheSectionSpansTheNoticeDepthFromTheEpoch) {
    relay_ring ring;
    for (uint32_t t = 100; t <= 140; ++t) {
        // Player 1 is substituted at 100..104 and at 130, player 2 at 135.
        const bool one_held = (t >= 100 && t <= 104) || t == 130;
        std::vector<applied_input> applied{
            one_held ? held_input(1, frame(7), 99) : fresh_input(1, frame(static_cast<uint8_t>(t))),
            t == 135 ? held_input(2, frame(8), 134) : fresh_input(2, frame(static_cast<uint8_t>(t)))};
        ring.push(t, applied);
    }
    ring.reanchor(1, 139);
    ring.reanchor(2, 139);
    // At 140 the last 32 ticks are 109..140: 100..104 have aged out.
    const auto mine = ring.held(1);
    ASSERT_EQ(mine.size(), 1u);
    EXPECT_EQ(mine[0].tick, 130u);
    EXPECT_EQ(mine[0].source, 99u);
    EXPECT_EQ(mine[0].frame.keys, 7);
    EXPECT_TRUE(ring.held(1, 131).empty());   // before the epoch: never noticed
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(120));
    ASSERT_EQ(datagrams.size(), 1u);
    ASSERT_EQ(datagrams[0].held.size(), 1u);              // its own record only, not player 2's
    EXPECT_EQ(datagrams[0].held[0].tick, 130u);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].player, 2u);
    EXPECT_EQ(datagrams[0].entries[0].frames.back().keys, 140);
    // Player 2's datagram carries its own 135 and player 1's frames.
    const auto theirs = ring.build_datagrams(2, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    ASSERT_EQ(theirs.size(), 1u);
    ASSERT_EQ(theirs[0].held.size(), 1u);
    EXPECT_EQ(theirs[0].held[0].tick, 135u);
    // Not assigned yet: no records at all.
    const auto unassigned = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget);
    ASSERT_EQ(unassigned.size(), 1u);
    EXPECT_TRUE(unassigned[0].held.empty());
    // Forty held ticks in a row: the last 32 of them are in the section.
    relay_ring blackout;
    for (uint32_t t = 200; t < 240; ++t)
        blackout.push(t, std::vector<applied_input>{held_input(1, frame(3), 199), fresh_input(2, frame(1))});
    const auto all = blackout.held(1);
    ASSERT_EQ(all.size(), bmmo::session::kHeldNoticeTicks);
    EXPECT_EQ(all.front().tick, 239u - 31u);
    EXPECT_EQ(all.back().tick, 239u);
}

// Where the section goes: into the first datagram with room for all of it,
// else into one of its own; a recipient with nothing to be told still gets
// one header-only datagram.
TEST(RelayHeld, PlacementAndTheHeaderOnlyDatagram) {
    // Solo: no entries, nothing held - one header-only datagram.
    relay_ring solo;
    solo.reanchor(1, 9);
    solo.push(10, std::vector<applied_input>{fresh_input(1, frame(1))});
    const auto quiet = solo.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    ASSERT_EQ(quiet.size(), 1u);
    EXPECT_TRUE(quiet[0].entries.empty());
    EXPECT_TRUE(quiet[0].held.empty());
    auto quiet_msg = to_message(quiet[0], 10);
    parse(quiet_msg);
    EXPECT_EQ(quiet_msg.size(), bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes);
    // Solo and substituted: the section rides the header-only datagram.
    solo.push(11, std::vector<applied_input>{held_input(1, frame(1), 10)});
    const auto told = solo.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    ASSERT_EQ(told.size(), 1u);
    ASSERT_EQ(told[0].held.size(), 1u);
    EXPECT_EQ(datagram_bytes(told[0], 0), bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes
                                          + bmmo::session::kHeldReferenceBytes);
    // No room beside the entries: the section gets a datagram of its own.
    relay_ring packed;
    packed.reanchor(1, 0);
    for (uint32_t t = 1; t <= 32; ++t) {
        std::vector<applied_input> applied{held_input(1, frame(1), kHeldSourceDefault)};
        for (uint32_t p = 2; p <= 4; ++p) applied.push_back(fresh_input(p, frame(static_cast<uint8_t>(t + p))));
        packed.push(t, applied);
    }
    const auto loose = packed.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    ASSERT_EQ(loose.size(), 1u);
    const size_t tight = datagram_bytes(loose[0], 0) - loose[0].held.size() * bmmo::session::kHeldReferenceBytes;
    const auto own = packed.build_datagrams(1, tight, std::optional<uint32_t>(0));
    ASSERT_EQ(own.size(), 2u);
    EXPECT_EQ(own[0].entries.size(), 3u);
    EXPECT_TRUE(own[0].held.empty());
    EXPECT_TRUE(own[1].entries.empty());
    EXPECT_EQ(own[1].held.size(), 32u);
    auto own_msg = to_message(own[1], 32);
    parse(own_msg);
    EXPECT_EQ(own_msg.size(), bmmo::session::kRelayDatagramHeader + 1u + 32 * bmmo::session::kHeldReferenceBytes);
}

// A record is a 5-byte reference to the frame the client itself sent, and
// carries the frame inline only when the client's sent ring cannot have it:
// 96 ticks or more back, or from before the recipient's epoch.  32 inline
// records are past one packet and are split; nothing goes over the budget,
// nothing is dropped, and what is counted is what goes on the wire.
TEST(RelayHeld, InlineOrReferenceAndTheBudget) {
    EXPECT_FALSE(bmmo::session::held_inline(200, kHeldSourceDefault, 0));   // the default frame: by reference
    EXPECT_FALSE(bmmo::session::held_inline(200, 105, 0));                  // 95 back
    EXPECT_TRUE(bmmo::session::held_inline(200, 104, 0));                   // 96 back
    EXPECT_TRUE(bmmo::session::held_inline(200, 150, 160));                 // before the epoch
    EXPECT_FALSE(bmmo::session::held_inline(200, 160, 160));
    // A long blackout of player 1 in a full room: every substitute is the
    // frame of tick 100, and at tick 240 the section's ticks 209..240 are all
    // 96 or more past it.
    relay_ring ring;
    for (uint32_t t = 101; t <= 240; ++t) {
        std::vector<applied_input> applied{held_input(1, turning(9, 0.25f), 100)};
        for (uint32_t p = 2; p <= 64; ++p) applied.push_back(fresh_input(p, frame(static_cast<uint8_t>(t))));
        ring.push(t, applied);
    }
    ring.reanchor(1, 239);
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    size_t inline_records = 0, entries = 0;
    std::set<uint32_t> ticks;
    for (const auto& d: datagrams) {
        entries += d.entries.size();
        auto msg = to_message(d, 240);
        const auto parsed = parse(msg);
        EXPECT_EQ(msg.size(), datagram_bytes(d, 0));
        EXPECT_LE(msg.size(), bmmo::session::kRelayDatagramBudget);
        for (const auto& r: parsed.held) {
            EXPECT_TRUE(ticks.insert(parsed.held_tick(r)).second);
            ASSERT_TRUE(r.has_frame);
            ++inline_records;
            EXPECT_EQ(r.frame.keys, 9);
            EXPECT_FLOAT_EQ(r.frame.cam_dir[0], 0.25f);
        }
    }
    EXPECT_EQ(entries, 63u);
    ASSERT_EQ(ticks.size(), bmmo::session::kHeldNoticeTicks);   // none lost to the budget
    EXPECT_EQ(*ticks.begin(), 209u);
    EXPECT_EQ(inline_records, 32u);
    // The same blackout 40 ticks in: every record by reference, one datagram.
    relay_ring early;
    early.reanchor(1, 139);
    for (uint32_t t = 101; t <= 140; ++t)
        early.push(t, std::vector<applied_input>{held_input(1, frame(9), 100), fresh_input(2, frame(1))});
    const auto refs = early.build_datagrams(1, bmmo::session::kRelayDatagramBudget, std::optional<uint32_t>(0));
    ASSERT_EQ(refs.size(), 1u);
    ASSERT_EQ(refs[0].held.size(), 32u);
    auto ref_msg = to_message(refs[0], 140);
    const auto parsed = parse(ref_msg);
    EXPECT_EQ(ref_msg.size(), datagram_bytes(refs[0], 0));
    ASSERT_EQ(parsed.held.size(), 32u);
    EXPECT_FALSE(parsed.held[0].has_frame);
    EXPECT_EQ(parsed.held[0].source, 100u);
    EXPECT_EQ(parsed.held_tick(parsed.held[0]), 109u);
}
