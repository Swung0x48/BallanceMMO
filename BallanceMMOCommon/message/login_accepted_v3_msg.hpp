#ifndef BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    struct player_status_v3 {
        std::string name;
        uint8_t cheated{};
        struct map map{};
        int32_t sector = 0;
    };

    struct login_accepted_v3_msg: public serializable_message {
        std::unordered_map<HSteamNetConnection, player_status_v3> online_players;

        login_accepted_v3_msg(): serializable_message(LoginAcceptedV3) {}

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = online_players.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& [id, data]: online_players) {
                raw.write(reinterpret_cast<const char*>(&id), sizeof(id));
                message_utils::write_string(data.name, raw);
                raw.write(reinterpret_cast<const char*>(&data.cheated), sizeof(data.cheated));
                raw.write(reinterpret_cast<const char*>(&data.map), sizeof(data.map));
                raw.write(reinterpret_cast<const char*>(&data.sector), sizeof(data.sector));
            }

            return raw.good();
        }

        bool deserialize() {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!raw.good())
                return false;
            online_players.reserve(size);
            for (uint32_t i = 0; i < size; ++i) {
                decltype(online_players)::key_type id;
                raw.read(reinterpret_cast<char*>(&id), sizeof(id));
                if (!raw.good() || raw.gcount() != sizeof(id))
                    return false;
                auto& player_data = online_players[id];
                if (!message_utils::read_string(raw, player_data.name))
                    return false;
                raw.read(reinterpret_cast<char*>(&player_data.cheated), sizeof(player_data.cheated));
                if (!raw.good() || raw.gcount() != sizeof(player_data.cheated))
                    return false;
                raw.read(reinterpret_cast<char*>(&player_data.map), sizeof(player_data.map));
                if (!raw.good() || raw.gcount() != sizeof(player_data.map))
                    return false;
                raw.read(reinterpret_cast<char*>(&player_data.sector), sizeof(player_data.sector));
                if (!raw.good() || raw.gcount() != sizeof(player_data.sector))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
