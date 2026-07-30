#ifndef BALLANCEMMOSERVER_MESSAGE_OWNED_BALL_STATE_V2_MSG_HPP
#define BALLANCEMMOSERVER_MESSAGE_OWNED_BALL_STATE_V2_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct owned_ball_state_v2_msg: public serializable_message {
        std::vector<owned_ball_state> balls;

        owned_ball_state_v2_msg(): serializable_message(bmmo::OwnedBallStateV2) {}

        bool serialize() override {
            serializable_message::serialize();

            uint32_t size = balls.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (auto& i: balls) {
                raw.write(reinterpret_cast<const char*>(&i), sizeof(i));
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            uint32_t size = 0;
            if (!message_utils::read_variable(raw, &size))
                return false;
            // refuse counts the payload cannot possibly contain, so a forged
            // length can't make us allocate an arbitrary amount of memory
            if (static_cast<size_t>(size) * sizeof(owned_ball_state)
                    > static_cast<size_t>(message_utils::bytes_remaining(raw)))
                return false;
            balls.resize(size);
            for (uint32_t i = 0; i < size; ++i) {
                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&balls[i]), sizeof(owned_ball_state));
                if (!raw.good() || raw.gcount() != sizeof(owned_ball_state))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_MESSAGE_OWNED_BALL_STATE_V2_MSG_HPP