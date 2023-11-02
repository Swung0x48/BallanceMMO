#ifndef BALLANCEMMOSERVER_LOGIN_REQUEST_V3_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_REQUEST_V3_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/version.hpp"

namespace bmmo {
    struct login_request_v3_msg: public serializable_message {
        login_request_v3_msg(): serializable_message(bmmo::LoginRequestV3) {}

        std::string nickname;
        bmmo::version_t version;
        uint8_t cheated = false;
        uint8_t uuid[16];

        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            raw.write(reinterpret_cast<const char*>(&version), sizeof(version));
            message_utils::write_string(nickname, raw);
            
            raw.write(reinterpret_cast<const char*>(&cheated), sizeof(cheated));
            raw.write(reinterpret_cast<const char*>(uuid), sizeof(uint8_t) * 16);
            return (raw.good());
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            raw.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (!raw.good() || raw.gcount() != sizeof(version)) return false;
            
            if (!message_utils::read_string(raw, nickname))
                return false;

            raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
            if (!raw.good() || raw.gcount() != sizeof(cheated)) return false;

            raw.read(reinterpret_cast<char*>(uuid), sizeof(uint8_t) * 16);
            if (!raw.good() || raw.gcount() != sizeof(uint8_t) * 16) return false;

            return (raw.good());
        }
    };
};

#endif