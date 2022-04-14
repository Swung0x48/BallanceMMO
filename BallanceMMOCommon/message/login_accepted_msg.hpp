#ifndef BALLANCEMMOSERVER_LOGIN_ACCEPTED_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_ACCEPTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct login_accepted_msg: public serializable_message {
        std::unordered_map<HSteamNetConnection, std::string> online_players;

        login_accepted_msg(): serializable_message(bmmo::LoginAccepted) {}

        bool serialize() override {
            serializable_message::serialize();

            uint32_t size = online_players.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (auto& i: online_players) {
                message_utils::write_string(i.second, raw);
                raw.write(reinterpret_cast<const char*>(&i.first), sizeof(i.first));
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
                if (!message_utils::read_string(raw, name))
                    return false;
                if (sizeof(conn) > raw.gcount())
                    return false;
                raw.read(reinterpret_cast<char*>(&conn), sizeof(conn));

                online_players[conn] = name;
            }

            return raw.good();
        }

    };
}

#endif //BALLANCEMMOSERVER_LOGIN_ACCEPTED_MSG_HPP
