#ifndef BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include "current_map_msg.hpp"

namespace bmmo {
    struct player_status_v3: current_map_state {
        std::string name;
        uint8_t cheated;
    };

    struct login_accepted_v3_msg: public serializable_message {
        std::vector<player_status_v3> online_players;

        login_accepted_v3_msg(): serializable_message(LoginAcceptedV3) {}

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = online_players.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& i: online_players) {
                raw.write(reinterpret_cast<const char*>(&i), sizeof(current_map_state));
                message_utils::write_string(i.name, raw);
                raw.write(reinterpret_cast<const char*>(&i.cheated), sizeof(i.cheated));
            }

            return raw.good();
        }

        bool deserialize() {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            online_players.resize(size);
            for (uint32_t i = 0; i < size; ++i) {
                auto& player_data = online_players[i];
                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&player_data), sizeof(current_map_state));
                if (!raw.good() || raw.gcount() != sizeof(current_map_state))
                    return false;
                if (!message_utils::read_string(raw, player_data.name))
                    return false;
                raw.read(reinterpret_cast<char*>(&player_data.cheated), sizeof(player_data.cheated));
                if (!raw.good() || raw.gcount() != sizeof(player_data.cheated))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_LOGIN_ACCEPTED_V3_MSG_HPP
