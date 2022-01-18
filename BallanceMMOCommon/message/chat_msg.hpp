#ifndef BALLANCEMMOSERVER_CHAT_MSG_HPP
#define BALLANCEMMOSERVER_CHAT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct chat_msg: public serializable_message {
        chat_msg(): serializable_message(bmmo::Chat) {}

        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        std::string chat_content;

        void serialize() override {
            serializable_message::serialize();

            raw.write(reinterpret_cast<const char*>(&player_id), sizeof(player_id));
            message_utils::write_string(chat_content, raw);
            assert(raw.good());
        }

        void deserialize() override {
            serializable_message::deserialize();

            raw.read(reinterpret_cast<char*>(&player_id), sizeof(player_id));
            message_utils::read_string(raw, chat_content);
            assert(raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_CHAT_MSG_HPP
