#ifndef BALLANCEMMOSERVER_KICK_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_KICK_REQUEST_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct kick_request_msg: public serializable_message {
        kick_request_msg(): serializable_message(bmmo::KickRequest) {}

        std::string player_name = "";
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        std::string reason = "";

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_string(player_name, raw);
            raw.write(reinterpret_cast<const char*>(&player_id), sizeof(player_id));
            message_utils::write_string(reason, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_string(raw, player_name)) return false;

            raw.read(reinterpret_cast<char*>(&player_id), sizeof(player_id));
            if (!raw.good() || raw.gcount() != sizeof(player_id)) return false;

            if (!message_utils::read_string(raw, reason)) return false;

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_KICK_REQUEST_MSG_HPP