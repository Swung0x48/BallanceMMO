#ifndef BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#include "message.hpp"
#include "../session/resync_state_codec.hpp"

namespace bmmo {
    // client -> server, reliable. Asks the server to reply with a full
    // snapshot. docs/rooms-and-sessions-protocol.md 2.2.  Protocol 34: and
    // names the last resync state the client decoded (`held`, its tick and
    // frame check), which the server's answer may be coded against
    // (session/resync_state_codec.hpp).
    struct session_resync_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t last_full_tick = 0;
        session::resync_ref_id held;

        session_resync_msg() : serializable_message(bmmo::SessionResync) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&last_full_tick), sizeof(last_full_tick));
            const uint8_t has = held.held ? 1 : 0;
            raw.write(reinterpret_cast<const char*>(&has), sizeof(has));
            raw.write(reinterpret_cast<const char*>(&held.tick), sizeof(held.tick));
            raw.write(reinterpret_cast<const char*>(&held.check), sizeof(held.check));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &last_full_tick)) return false;
            uint8_t has = 0;
            if (!message_utils::read_variable(raw, &has) || has > 1) return false;
            if (!message_utils::read_variable(raw, &held.tick)) return false;
            if (!message_utils::read_variable(raw, &held.check)) return false;
            held.held = has != 0;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
