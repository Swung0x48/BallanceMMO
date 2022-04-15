#ifndef BALLANCEMMOSERVER_PLAYER_CONNECTED_V2_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_CONNECTED_V2_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_connected_v2_msg: public serializable_message {
        HSteamNetConnection connection_id = k_HSteamNetConnection_Invalid;
        std::string name;
        uint8_t cheated = false;

        player_connected_v2_msg(): serializable_message(bmmo::PlayerConnectedV2) {}

        bool serialize() override {
            serializable_message::serialize();

            raw.write(reinterpret_cast<const char*>(&connection_id), sizeof(connection_id));
            message_utils::write_string(name, raw);
            raw.write(reinterpret_cast<const char*>(&cheated), sizeof(cheated));
            return raw.good();
        }

        bool deserialize() override {
            serializable_message::deserialize();

            raw.read(reinterpret_cast<char*>(&connection_id), sizeof(connection_id));
            if (!raw.good() || raw.gcount() != sizeof(connection_id)) return false;

            if (!message_utils::read_string(raw, name)) return false;

            raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
            if (!raw.good() || raw.gcount() != sizeof(cheated)) return false;

            return raw.good();
        }

    };
}

#endif //BALLANCEMMOSERVER_PLAYER_CONNECTED_V2_MSG_HPP
