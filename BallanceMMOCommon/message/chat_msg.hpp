#ifndef BALLANCEMMOSERVER_CHAT_MSG_HPP
#define BALLANCEMMOSERVER_CHAT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct chat_msg: public serializable_message {
        chat_msg(): serializable_message(bmmo::Chat) {}

        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        std::string chat_content;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            raw.write(reinterpret_cast<const char*>(&player_id), sizeof(player_id));
            message_utils::write_string(chat_content, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (sizeof(player_id) + raw.tellg() > size()) return false;
            raw.read(reinterpret_cast<char*>(&player_id), sizeof(player_id));
            if (!message_utils::read_string(raw, chat_content))
                return false;
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_CHAT_MSG_HPP
