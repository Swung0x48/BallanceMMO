#ifndef BALLANCEMMOSERVER_LOGIN_ACCEPTED_V2_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_ACCEPTED_V2_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_status {
        std::string name;
        uint8_t cheated;
    };

    struct login_accepted_v2_msg: public serializable_message {
        std::unordered_map<HSteamNetConnection, player_status> online_players;

        login_accepted_v2_msg(): serializable_message(bmmo::LoginAcceptedV2) {}

        bool serialize() override {
            serializable_message::serialize();

            uint32_t size = online_players.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (auto& i: online_players) {
                raw.write(reinterpret_cast<const char*>(&i.first), sizeof(i.first)); // HSteamNetConnection - uint32_t
                message_utils::write_string(i.second.name, raw);    // name - string
                raw.write(reinterpret_cast<const char*>(&i.second.cheated), sizeof(i.second.cheated)); // cheated - uint8_t
            }
            return raw.good();
        }

        bool deserialize() override {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            for (uint32_t i = 0; i < size; ++i) {
                std::string name;
                HSteamNetConnection conn;
                uint8_t cheated;

                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&conn), sizeof(conn));
                if (!raw.good() || raw.gcount() != sizeof(conn))
                    return false;
                if (!message_utils::read_string(raw, name)) // check if read string successfully
                   return false;
                raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
                if (!raw.good() || raw.gcount() != sizeof(cheated))
                    return false;

                online_players[conn] = { name, cheated };
            }

            return raw.good();
        }

    };
}

#endif //BALLANCEMMOSERVER_LOGIN_ACCEPTED_V2_MSG_HPP
