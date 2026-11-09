#ifndef BALLANCEMMOSERVER_BALL_STATE_MUTE_MSG_HPP
#define BALLANCEMMOSERVER_BALL_STATE_MUTE_MSG_HPP
#include "message.hpp"
#include "owned_compressed_ball_state_msg.hpp"
#include "../entity/session.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bmmo {
    // client -> server, reliable (protocol 34): the players whose legacy ball
    // stream (OwnedCompressedBallState) this client does not need - their
    // balls are live physics mirrors here, moved by the session, not by the
    // stream.  Each message replaces the previous set; an empty one ends it.
    // The server leaves those players out of what it sends this client and,
    // for a player that leaves the set, sends its current state once at once
    // (the ball turns back into a legacy ball: a death, the airship zone, the
    // session's end).  About half of a session member's downstream was this
    // stream (2026-11-08 public test).
    struct ball_state_mute_msg : public serializable_message {
        std::vector<uint32_t> players;   // sorted, unique, at most kMaxPlayers

        static constexpr size_t kMaxPlayers = session::MAX_PLAYERS_PER_SESSION + 1;

        ball_state_mute_msg() : serializable_message(bmmo::BallStateMute) {}

        bool serialize() override {
            serializable_message::serialize();
            const auto count = static_cast<uint16_t>(std::min(players.size(), kMaxPlayers));
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            raw.write(reinterpret_cast<const char*>(players.data()), static_cast<std::streamsize>(count * sizeof(uint32_t)));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            uint16_t count = 0;
            if (!message_utils::read_variable(raw, &count) || count > kMaxPlayers) return false;
            players.resize(count);
            raw.read(reinterpret_cast<char*>(players.data()), static_cast<std::streamsize>(count * sizeof(uint32_t)));
            if (!raw.good()) return false;
            std::sort(players.begin(), players.end());
            players.erase(std::unique(players.begin(), players.end()), players.end());
            return true;
        }
    };

    // What of `in` a recipient that muted `muted` (sorted) gets: the rest of
    // its balls and timestamps, in their order.  False when nothing is left
    // (then nothing is sent: no packet, no overhead).
    inline bool without_muted(const owned_compressed_ball_state_msg& in, const std::vector<uint32_t>& muted,
                              owned_compressed_ball_state_msg& out) {
        const auto kept = [&](uint32_t id) { return !std::binary_search(muted.begin(), muted.end(), id); };
        out.balls.clear();
        out.unchanged_balls.clear();
        for (const auto& b: in.balls) if (kept(b.player_id)) out.balls.push_back(b);
        for (const auto& u: in.unchanged_balls) if (kept(u.player_id)) out.unchanged_balls.push_back(u);
        return !out.balls.empty() || !out.unchanged_balls.empty();
    }
}

#endif //BALLANCEMMOSERVER_BALL_STATE_MUTE_MSG_HPP
