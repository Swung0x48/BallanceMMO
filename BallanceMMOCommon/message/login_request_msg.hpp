#ifndef BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct login_request_msg: public serializable_message {
        login_request_msg(): serializable_message(bmmo::LoginRequest) {}

        std::string nickname;

        bool serialize() override {
            if (serializable_message::serialize()) return false;

            message_utils::write_string(nickname, raw);
            return (raw.good());
        }

        bool deserialize() override {
            if (serializable_message::deserialize())
                return false;

            if (!message_utils::read_string(raw, nickname))
                return false;
            
            return (raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
