#ifndef BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_connected_msg: public serializable_message {
        HSteamNetConnection connection_id;
        std::string name;

        player_connected_msg(): serializable_message(bmmo::PlayerConnected) {}

        bool serialize() override {
            serializable_message::serialize();

            raw.write(reinterpret_cast<const char*>(&connection_id), sizeof(connection_id));
            message_utils::write_string(name, raw);
            return raw.good();
        }

        bool deserialize() override {
            serializable_message::deserialize();

            raw.read(reinterpret_cast<char*>(&connection_id), sizeof(connection_id));
            message_utils::read_string(raw, name);
            return raw.good();
        }

    };
}

#endif //BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
