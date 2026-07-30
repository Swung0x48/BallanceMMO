#ifndef BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct player_connected_msg: public serializable_message {
        HSteamNetConnection connection_id = k_HSteamNetConnection_Invalid;
        std::string name;

        player_connected_msg(): serializable_message(bmmo::PlayerConnected) {}

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            raw.write(reinterpret_cast<const char*>(&connection_id), sizeof(connection_id));
            message_utils::write_string(name, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_variable(raw, &connection_id)) return false;
            return message_utils::read_string(raw, name);
        }

    };
}

#endif //BALLANCEMMOSERVER_PLAYER_CONNECTED_MSG_HPP
