#ifndef BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_kicked_msg: public serializable_message {
        player_kicked_msg(): serializable_message(bmmo::PlayerKicked) {}
        
        std::string kicked_player_name = "";
        std::string executor_name = "";
        std::string reason = "";
        
        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            
            message_utils::write_string(kicked_player_name, raw);
            message_utils::write_string(executor_name, raw);
            message_utils::write_string(reason, raw);
            return raw.good();
        }
        
        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            
            if (!message_utils::read_string(raw, kicked_player_name)) return false;
            if (!message_utils::read_string(raw, executor_name)) return false;
            if (!message_utils::read_string(raw, reason)) return false;
            
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP