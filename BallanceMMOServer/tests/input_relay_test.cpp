// session/input_relay.hpp: the server's relay ring and window, and the
// client's reassembly rules (protocol 5).
#include <gtest/gtest.h>

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
    size_t total_frames(const std::vector<std::vector<relay_ring::window_entry>>& datagrams) {
        size_t n = 0;
        for (const auto& d: datagrams)
            for (const auto& e: d) n += e.frames.size();
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
        const auto datagrams = ring.build_datagrams(1, bmmo::session::kRelayDatagramBudget, keep, &trimmed);
        EXPECT_EQ(trimmed, 0u) << members;
        std::set<uint32_t> players;
        for (const auto& d: datagrams) {
            size_t bytes = bmmo::session::kRelayDatagramHeader;
            bmmo::session_remote_input_msg msg;
            msg.tick = 107;
            for (const auto& e: d) {
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
    const auto datagrams = wide.build_datagrams(1, bmmo::session::kRelayDatagramBudget, [](uint32_t) { return true; }, &trimmed);
    ASSERT_EQ(datagrams.size(), 1u);
    ASSERT_EQ(datagrams[0].size(), 1u);
    EXPECT_GT(trimmed, 0u);
    EXPECT_EQ(datagrams[0][0].frames.size() + trimmed, 32u);
    EXPECT_EQ(datagrams[0][0].frames.back().keys, 31);   // the newest kept
    EXPECT_LE(relay_ring::entry_bytes(datagrams[0][0]) + bmmo::session::kRelayDatagramHeader, bmmo::session::kRelayDatagramBudget);
    // A still camera: 8 members, window 8, one datagram of about half a packet.
    relay_ring still(8);
    for (uint32_t t = 0; t < 8; ++t) {
        applied_list applied;
        for (uint32_t p = 1; p <= 8; ++p) applied.emplace_back(p, frame(static_cast<uint8_t>(t)));
        still.push(t, applied);
    }
    const auto one = still.build_datagrams(1, bmmo::session::kRelayDatagramBudget, [](uint32_t) { return true; });
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].size(), 7u);
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
