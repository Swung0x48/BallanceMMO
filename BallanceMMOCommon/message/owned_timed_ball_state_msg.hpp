#ifndef BALLANCEMMOSERVER_MESSAGE_OWNED_TIMED_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_MESSAGE_OWNED_TIMED_BALL_STATE_MSG_HPP
#include "message.hpp"
#include "timed_ball_state_msg.hpp"

namespace bmmo {
    struct owned_timed_ball_state {
        timed_ball_state state{};
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
    };

    struct owned_timed_ball_state_msg: public serializable_message {
        std::vector<owned_timed_ball_state> balls;

        owned_timed_ball_state_msg(): serializable_message(bmmo::OwnedTimedBallState) {}

        bool serialize() override {
            serializable_message::serialize();

            uint32_t size = balls.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& i: balls) {
                raw.write(reinterpret_cast<const char*>(&i), sizeof(i));
            }
            return raw.good();
        }

        bool deserialize() override {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            balls.resize(size);
            for (uint32_t i = 0; i < size; ++i) {
                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&balls[i]), sizeof(owned_timed_ball_state));
                if (!raw.good() || raw.gcount() != sizeof(owned_timed_ball_state))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_MESSAGE_OWNED_TIMED_BALL_STATE_MSG_HPP