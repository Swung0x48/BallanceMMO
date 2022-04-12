#ifndef BALLANCEMMOSERVER_LOGIN_REQUEST_V2_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_REQUEST_V2_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/version.hpp"

namespace bmmo {
    struct login_request_v2_msg: public serializable_message {
        login_request_v2_msg(): serializable_message(bmmo::LoginRequestV2) {}

        std::string nickname;
        bmmo::version_t version;
        uint8_t cheated;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            raw.write(reinterpret_cast<const char*>(&version), sizeof(version));
            message_utils::write_string(nickname, raw);
            
            raw.write(reinterpret_cast<const char*>(&cheated), sizeof(cheated));
            return (raw.good());
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            raw.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (!message_utils::read_string(raw, nickname))
                return false;
            raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
            return (raw.good());
        }
    };
};

#endif