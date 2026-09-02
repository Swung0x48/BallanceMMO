#ifndef BALLANCEMMOSERVER_SESSION_END_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_END_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, reliable. Ends the physics session (host Close/End,
    // hash mismatch, room teardown, ...). docs/rooms-and-sessions-protocol.md 2.2.
    struct session_end_msg : public serializable_message {
        uint32_t session = 0;
        std::string reason;    // <= MAX_REASON

        session_end_msg() : serializable_message(bmmo::SessionEnd) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            message_utils::write_string<uint16_t>(reason.substr(0, session::MAX_REASON), raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_string<uint16_t>(raw, reason)) return false;
            if (reason.size() > session::MAX_REASON) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_END_MSG_HPP
