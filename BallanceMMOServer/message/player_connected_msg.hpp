#ifndef BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_connected_msg: public serializable_message {
        std::string name;

        player_connected_msg(): serializable_message(bmmo::PlayerConnected) {}


        void serialize() override {
            serializable_message::serialize();
        }

        void deserialize() override {
            serializable_message::deserialize();
        }

    };
}

#endif //BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
