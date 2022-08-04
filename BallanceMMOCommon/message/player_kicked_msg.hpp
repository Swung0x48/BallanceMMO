#ifndef BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    enum class crash_type : uint8_t {
        None = 0U, // no crash
        Crash = 1U,
        FatalError = 2U,
    };

    struct player_kicked_msg: public serializable_message {
        player_kicked_msg(): serializable_message(bmmo::PlayerKicked) {}
        
        std::string kicked_player_name = "";
        std::string executor_name = "";
        std::string reason = "";
        uint8_t crashed = 0;
        
        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            
            message_utils::write_string(kicked_player_name, raw);
            message_utils::write_string(executor_name, raw);
            message_utils::write_string(reason, raw);
            raw.write(reinterpret_cast<const char*>(&crashed), sizeof(crashed));
            return raw.good();
        }
        
        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            
            if (!message_utils::read_string(raw, kicked_player_name)) return false;
            if (!message_utils::read_string(raw, executor_name)) return false;
            if (!message_utils::read_string(raw, reason)) return false;
            raw.read(reinterpret_cast<char*>(&crashed), sizeof(crashed));
            if (!raw.good() || raw.gcount() != sizeof(crashed)) return false;
            
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_PLAYER_KICKED_MSG_HPP
