#ifndef BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_connected_msg: public serializable_message {
        std::string name;

        player_connected_msg(): serializable_message(bmmo::PlayerConnected) {}

        void serialize() override {
            serializable_message::serialize();

            message_utils::write_string(name, raw);
            assert(raw.good());
        }

        void deserialize() override {
            serializable_message::deserialize();

            message_utils::read_string(raw, name);
            assert(raw.good());
        }

    };
}

#endif //BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
