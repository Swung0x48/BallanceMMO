#ifndef BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_disconnected_msg: public serializable_message {
        std::string name;

        player_disconnected_msg(): serializable_message(bmmo::PlayerDisconnected) {}

        void serialize() override {
            serializable_message::serialize();
            message_utils::write_string(name, raw);
        }

        void deserialize() override {
            serializable_message::deserialize();
            message_utils::read_string(raw, name);
        }
    };
}

#endif //BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
