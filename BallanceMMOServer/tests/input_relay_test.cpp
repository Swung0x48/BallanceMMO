// session/input_relay.hpp: the server's relay ring and window, and the
// client's reassembly rules (protocol 5).
#include <gtest/gtest.h>

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
    using applied_list = std::vector<std::pair<uint32_t, input_frame>>;
}

// Every other member present at the newest tick, with its trailing run of
// consecutive ticks, oldest first; the recipient itself left out.
TEST(RelayRing, BuildsTheWindowPerRecipient) {
    relay_ring ring(4);
    for (uint32_t t = 10; t <= 13; ++t)
        ring.push(t, applied_list{{1, frame(static_cast<uint8_t>(t))}, {2, frame(static_cast<uint8_t>(t + 100))}});
    const auto window = ring.build(1);
    ASSERT_EQ(window.size(), 1u);
    EXPECT_EQ(window[0].player, 2u);
    ASSERT_EQ(window[0].frames.size(), 4u);
    EXPECT_EQ(window[0].frames.front().keys, 110);
    EXPECT_EQ(window[0].frames.back().keys, 113);
    const auto other = ring.build(2);
    ASSERT_EQ(other.size(), 1u);
    EXPECT_EQ(other[0].player, 1u);
    EXPECT_EQ(other[0].frames.back().keys, 13);
    // A recipient that is not a member gets everyone.
    EXPECT_EQ(ring.build(99).size(), 2u);
}

TEST(RelayRing, TrimsToTheWindowAndStopsAtAGapOrAJoin) {
    relay_ring ring(8);
    ring.push(1, applied_list{{1, frame(1)}, {2, frame(1)}});
    ring.push(2, applied_list{{1, frame(2)}, {2, frame(2)}});
    // tick 3 never relayed (one member): a gap
    ring.push(4, applied_list{{1, frame(4)}, {2, frame(4)}});
    ring.push(5, applied_list{{1, frame(5)}, {2, frame(5)}, {3, frame(5)}});   // 3 joins
    ring.push(6, applied_list{{1, frame(6)}, {2, frame(6)}, {3, frame(6)}});
    const auto window = ring.build(1);
    ASSERT_EQ(window.size(), 2u);
    EXPECT_EQ(window[0].player, 2u);
    EXPECT_EQ(window[0].frames.size(), 3u);   // 4, 5, 6: the gap at 3 ends the run
    EXPECT_EQ(window[0].frames.front().keys, 4);
    EXPECT_EQ(window[1].player, 3u);
    EXPECT_EQ(window[1].frames.size(), 2u);   // 5, 6: present from its join
    // A member absent at the newest tick (left) is not relayed at all.
    ring.push(7, applied_list{{1, frame(7)}, {2, frame(7)}});
    const auto after = ring.build(1);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].player, 2u);
    EXPECT_EQ(after[0].frames.size(), 4u);
    // The window bounds the run.
    relay_ring small(2);
    for (uint32_t t = 1; t <= 5; ++t) small.push(t, applied_list{{1, frame(1)}, {2, frame(static_cast<uint8_t>(t))}});
    const auto bounded = small.build(1);
    ASSERT_EQ(bounded.size(), 1u);
    ASSERT_EQ(bounded[0].frames.size(), 2u);
    EXPECT_EQ(bounded[0].frames.front().keys, 4);
    EXPECT_EQ(small.newest_tick(), 5u);
}

TEST(RelayRing, APushAtAnOlderTickReplacesTheRingFromThere) {
    relay_ring ring(8);
    for (uint32_t t = 1; t <= 4; ++t) ring.push(t, applied_list{{1, frame(1)}, {2, frame(static_cast<uint8_t>(t))}});
    ring.push(3, applied_list{{1, frame(1)}, {2, frame(30)}});
    const auto window = ring.build(1);
    ASSERT_EQ(window.size(), 1u);
    ASSERT_EQ(window[0].frames.size(), 3u);   // 1, 2, 3
    EXPECT_EQ(window[0].frames.back().keys, 30);
    EXPECT_EQ(ring.newest_tick(), 3u);
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

namespace {
    input_frame turning(uint8_t keys, float angle) {
        input_frame f{};
        f.keys = keys;
        f.cam_dir[0] = angle;
        f.cam_dir[2] = 1.0f - angle;
        return f;
    }
    size_t total_frames(const std::vector<relay_ring::datagram>& datagrams) {
        size_t n = 0;
        for (const auto& d: datagrams)
            for (const auto& e: d.entries) n += e.frames.size();
        return n;
    }
}

// Whole entries per datagram of one packet; a room of 30 or 64 with the
// cameras turning every tick splits into several, none over the budget, every
// other member in exactly one of them; a leaver is left out.
TEST(RelayRing, PacksDatagramsToTheBudgetAndKeepsOnlyMembers) {
    for (const uint32_t members: {4u, 30u, 64u}) {
        relay_ring ring(8);
        for (uint32_t t = 100; t < 108; ++t) {
            applied_list applied;
            for (uint32_t p = 1; p <= members; ++p) applied.emplace_back(p, turning(static_cast<uint8_t>(t), 0.01f * t + 0.001f * p));
            ring.push(t, applied);
        }
        const auto keep = [](uint32_t player) { return player != 2; };   // 2 left
        size_t trimmed = 0;
        const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, keep, std::nullopt, &trimmed);
        EXPECT_EQ(trimmed, 0u) << members;
        std::set<uint32_t> players;
        for (const auto& d: datagrams) {
            size_t bytes = bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes;
            bmmo::session_remote_input_msg msg;
            msg.tick = 107;
            EXPECT_TRUE(d.held.empty());
            for (const auto& e: d.entries) {
                bytes += relay_ring::entry_bytes(e);
                EXPECT_EQ(e.frames.size(), 8u);
                EXPECT_TRUE(players.insert(e.player).second) << "player " << e.player << " in two datagrams";
                bmmo::session_remote_input_msg::entry me;
                me.player = e.player;
                me.frames = e.frames;
                msg.entries.push_back(me);
            }
            EXPECT_LE(bytes, bmmo::session::kRelayDatagramBudget) << members;
            // The budget counts what goes on the wire.
            ASSERT_TRUE(msg.serialize());
            EXPECT_EQ(msg.size(), bytes) << members;
            EXPECT_LE(msg.size(), bmmo::session::kRelayDatagramBudget) << members;
        }
        EXPECT_EQ(players.size(), members - 2u) << members;   // not the recipient, not the leaver
        EXPECT_EQ(players.count(1), 0u);
        EXPECT_EQ(players.count(2), 0u);
        // the turning camera costs 40 B a frame: 325 B an entry, three to a packet
        if (members == 30) EXPECT_EQ(datagrams.size(), 10u);
        if (members == 4) EXPECT_EQ(datagrams.size(), 1u);
    }
    // An entry that does not fit a packet on its own (window 32, camera
    // turning: 5 + 32 x 40 B) loses its oldest frames until it does.
    relay_ring wide(32);
    for (uint32_t t = 0; t < 32; ++t)
        wide.push(t, applied_list{{1, turning(1, 0.5f)}, {2, turning(static_cast<uint8_t>(t), 0.01f * t)}});
    size_t trimmed = 0;
    const auto datagrams = wide.build_datagrams(1, bmmo::session::kRelayDatagramBudget, [](uint32_t) { return true; },
                                                std::nullopt, &trimmed);
    ASSERT_EQ(datagrams.size(), 1u);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_GT(trimmed, 0u);
    EXPECT_EQ(datagrams[0].entries[0].frames.size() + trimmed, 32u);
    EXPECT_EQ(datagrams[0].entries[0].frames.back().keys, 31);   // the newest kept
    EXPECT_LE(relay_ring::entry_bytes(datagrams[0].entries[0]) + bmmo::session::kRelayDatagramHeader
              + bmmo::session::kHeldCountBytes, bmmo::session::kRelayDatagramBudget);
    // A still camera: 8 members, window 8, one datagram of about half a packet.
    relay_ring still(8);
    for (uint32_t t = 0; t < 8; ++t) {
        applied_list applied;
        for (uint32_t p = 1; p <= 8; ++p) applied.emplace_back(p, frame(static_cast<uint8_t>(t)));
        still.push(t, applied);
    }
    const auto one = still.build_datagrams(1, bmmo::session::kRelayDatagramBudget, [](uint32_t) { return true; });
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].entries.size(), 7u);
    EXPECT_EQ(total_frames(one), 56u);
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
    const auto everyone = [](uint32_t) { return true; };

    // A datagram as server.cpp on_session_inputs writes it, for a recipient
    // numbered from `epoch`.
    bmmo::session_remote_input_msg to_message(const relay_ring::datagram& d, uint32_t tick, uint32_t epoch) {
        bmmo::session_remote_input_msg msg;
        msg.session = 1;
        msg.tick = tick;
        msg.epoch = epoch;
        for (const auto& e: d.entries) {
            bmmo::session_remote_input_msg::entry me;
            me.player = e.player;
            me.frames = e.frames;
            msg.entries.push_back(me);
        }
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
    size_t datagram_bytes(const relay_ring::datagram& d, uint32_t epoch) {
        size_t bytes = bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes;
        for (const auto& e: d.entries) bytes += relay_ring::entry_bytes(e);
        for (const auto& h: d.held) bytes += relay_ring::held_bytes(h, epoch);
        return bytes;
    }
}

// The notice depth is 32 ticks even though the others' window is 8: a
// recipient gets its own held ticks of the last 32, from its epoch on, and
// nobody else's; the others' runs are still cut at the window.
TEST(RelayHeld, TheSectionSpansTheNoticeDepthFromTheEpoch) {
    relay_ring ring(8);
    EXPECT_EQ(ring.depth(), bmmo::session::kHeldNoticeTicks);
    for (uint32_t t = 100; t <= 140; ++t) {
        // Player 1 is substituted at 100..104 and at 130, player 2 at 135.
        const bool one_held = (t >= 100 && t <= 104) || t == 130;
        std::vector<applied_input> applied{
            one_held ? held_input(1, frame(7), 99) : fresh_input(1, frame(static_cast<uint8_t>(t))),
            t == 135 ? held_input(2, frame(8), 134) : fresh_input(2, frame(static_cast<uint8_t>(t)))};
        ring.push(t, applied);
    }
    // At 140 the last 32 ticks are 109..140: 100..104 have aged out.
    const auto mine = ring.held(1);
    ASSERT_EQ(mine.size(), 1u);
    EXPECT_EQ(mine[0].tick, 130u);
    EXPECT_EQ(mine[0].source, 99u);
    EXPECT_EQ(mine[0].frame.keys, 7);
    EXPECT_TRUE(ring.held(1, 131).empty());   // before the epoch: never noticed
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(120));
    ASSERT_EQ(datagrams.size(), 1u);
    ASSERT_EQ(datagrams[0].held.size(), 1u);              // its own record only, not player 2's
    EXPECT_EQ(datagrams[0].held[0].tick, 130u);
    ASSERT_EQ(datagrams[0].entries.size(), 1u);
    EXPECT_EQ(datagrams[0].entries[0].player, 2u);
    EXPECT_EQ(datagrams[0].entries[0].frames.size(), 8u);  // the window, not the depth
    EXPECT_EQ(datagrams[0].entries[0].frames.back().keys, 140);
    // Player 2's datagram carries its own 135 and player 1's frames.
    const auto theirs = ring.build_datagrams(2, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(theirs.size(), 1u);
    ASSERT_EQ(theirs[0].held.size(), 1u);
    EXPECT_EQ(theirs[0].held[0].tick, 135u);
    // Not assigned yet: no records at all.
    const auto unassigned = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone);
    ASSERT_EQ(unassigned.size(), 1u);
    EXPECT_TRUE(unassigned[0].held.empty());
    // Forty held ticks in a row: the last 32 of them are in the section.
    relay_ring blackout(8);
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
    relay_ring solo(8);
    solo.push(10, std::vector<applied_input>{fresh_input(1, frame(1))});
    const auto quiet = solo.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(quiet.size(), 1u);
    EXPECT_TRUE(quiet[0].entries.empty());
    EXPECT_TRUE(quiet[0].held.empty());
    auto quiet_msg = to_message(quiet[0], 10, 0);
    ASSERT_TRUE(quiet_msg.serialize());
    EXPECT_EQ(quiet_msg.size(), bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes);
    // Solo and substituted: the section rides the header-only datagram.
    solo.push(11, std::vector<applied_input>{held_input(1, frame(1), 10)});
    const auto told = solo.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(told.size(), 1u);
    ASSERT_EQ(told[0].held.size(), 1u);
    EXPECT_EQ(datagram_bytes(told[0], 0), bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes
                                          + bmmo::session::kHeldReferenceBytes);
    // 30 members with turning cameras: three entries a packet (26 + 3 x 325
    // B), and the 16 records (80 B) fit beside the first three.
    relay_ring room(8);
    for (uint32_t t = 100; t < 132; ++t) {
        std::vector<applied_input> applied;
        for (uint32_t p = 1; p <= 30; ++p) {
            const input_frame f = turning(static_cast<uint8_t>(t), 0.01f * t + 0.001f * p);
            applied.push_back(p == 1 && t % 2 == 0 ? held_input(p, f, t - 1) : fresh_input(p, f));
        }
        room.push(t, applied);
    }
    const auto datagrams = room.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(datagrams.size(), 10u);   // the section found room: no datagram of its own
    ASSERT_EQ(datagrams[0].held.size(), 16u);   // the even ticks of 100..131
    for (const auto& d: datagrams) EXPECT_LE(datagram_bytes(d, 0), bmmo::session::kRelayDatagramBudget);
    for (size_t i = 1; i < datagrams.size(); ++i) EXPECT_TRUE(datagrams[i].held.empty());
    // No room beside the entries anywhere: the section gets a datagram of its own.
    relay_ring packed(8);
    for (uint32_t t = 0; t < 32; ++t) {
        std::vector<applied_input> applied;
        for (uint32_t p = 1; p <= 4; ++p) {
            const input_frame f = turning(static_cast<uint8_t>(t), 0.01f * t + 0.001f * p);
            applied.push_back(p == 1 ? held_input(p, f, kHeldSourceDefault) : fresh_input(p, f));
        }
        packed.push(t, applied);
    }
    // A budget the three entries fill exactly.
    const size_t tight = bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes + 3 * (5 + 8 * 40);
    const auto own = packed.build_datagrams(1, tight, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(own.size(), 2u);
    EXPECT_EQ(own[0].entries.size(), 3u);
    EXPECT_TRUE(own[0].held.empty());
    EXPECT_TRUE(own[1].entries.empty());
    EXPECT_EQ(own[1].held.size(), 32u);
    EXPECT_EQ(datagram_bytes(own[1], 0), 25u + 161u);   // the header and a full section by reference
    auto own_msg = to_message(own[1], 31, 0);
    ASSERT_TRUE(own_msg.serialize());
    EXPECT_EQ(own_msg.size(), 25u + 161u);
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
    relay_ring ring(8);
    for (uint32_t t = 101; t <= 240; ++t) {
        std::vector<applied_input> applied{held_input(1, turning(9, 0.25f), 100)};
        for (uint32_t p = 2; p <= 64; ++p)
            applied.push_back(fresh_input(p, turning(static_cast<uint8_t>(t), 0.01f * t + 0.001f * p)));
        ring.push(t, applied);
    }
    const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    size_t inline_records = 0, reference_records = 0, entries = 0, with_held = 0;
    std::set<uint32_t> ticks;
    for (const auto& d: datagrams) {
        entries += d.entries.size();
        if (!d.held.empty()) ++with_held;
        auto msg = to_message(d, 240, 0);
        ASSERT_TRUE(msg.serialize());
        EXPECT_EQ(msg.size(), datagram_bytes(d, 0));
        EXPECT_LE(msg.size(), bmmo::session::kRelayDatagramBudget);
        bmmo::session_remote_input_msg parsed;
        parsed.raw.write(msg.raw.str().data(), static_cast<std::streamsize>(msg.size()));
        ASSERT_TRUE(parsed.deserialize());
        for (const auto& r: parsed.held) {
            EXPECT_TRUE(ticks.insert(parsed.held_tick(r)).second);
            if (r.has_frame) {
                ++inline_records;
                EXPECT_EQ(r.frame.keys, 9);
                EXPECT_FLOAT_EQ(r.frame.cam_dir[0], 0.25f);
            } else {
                ++reference_records;
            }
        }
    }
    EXPECT_EQ(entries, 63u);
    EXPECT_EQ(with_held, 2u);   // 26 + 26 x 41 B, then the other 6
    ASSERT_EQ(ticks.size(), bmmo::session::kHeldNoticeTicks);   // none lost to the budget
    EXPECT_EQ(*ticks.begin(), 209u);
    EXPECT_EQ(inline_records, 32u);
    EXPECT_EQ(reference_records, 0u);
    // The same blackout 40 ticks in: every record by reference, one datagram.
    relay_ring early(8);
    for (uint32_t t = 101; t <= 140; ++t)
        early.push(t, std::vector<applied_input>{held_input(1, frame(9), 100), fresh_input(2, frame(1))});
    const auto refs = early.build_datagrams(1, bmmo::session::kRelayDatagramBudget, everyone, std::optional<uint32_t>(0));
    ASSERT_EQ(refs.size(), 1u);
    ASSERT_EQ(refs[0].held.size(), 32u);
    auto ref_msg = to_message(refs[0], 140, 0);
    ASSERT_TRUE(ref_msg.serialize());
    EXPECT_EQ(ref_msg.size(), bmmo::session::kRelayDatagramHeader + bmmo::session::kHeldCountBytes
                              + relay_ring::entry_bytes(refs[0].entries[0]) + 32 * bmmo::session::kHeldReferenceBytes);
    bmmo::session_remote_input_msg parsed;
    parsed.raw.write(ref_msg.raw.str().data(), static_cast<std::streamsize>(ref_msg.size()));
    ASSERT_TRUE(parsed.deserialize());
    ASSERT_EQ(parsed.held.size(), 32u);
    EXPECT_FALSE(parsed.held[0].has_frame);
    EXPECT_EQ(parsed.held[0].source, 100u);
    EXPECT_EQ(parsed.held_tick(parsed.held[0]), 109u);
}
