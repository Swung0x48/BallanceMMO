#ifndef BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#include "message.hpp"

namespace bmmo {
    // client -> server, reliable. Asks the server to reply with a full
    // snapshot. docs/rooms-and-sessions-protocol.md 2.2.
    struct session_resync_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t last_full_tick = 0;

        session_resync_msg() : serializable_message(bmmo::SessionResync) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&last_full_tick), sizeof(last_full_tick));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &last_full_tick)) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
