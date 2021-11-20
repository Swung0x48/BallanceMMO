#ifndef BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct login_request_msg: public serializable_message {
        login_request_msg(): serializable_message(bmmo::LoginRequest) {}

        std::string nickname;

        void serialize() override {
            serializable_message::serialize();

            message_utils::write_string(nickname, raw);
            assert(raw.good());
        }

        void deserialize() override {
            serializable_message::deserialize();

            message_utils::read_string(raw, nickname);
            assert(raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_LOGIN_REQUEST_MSG_HPP
