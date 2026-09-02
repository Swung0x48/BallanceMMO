#ifndef BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
#include "message.hpp"

namespace bmmo {
    // server -> client, reliable, in reply to session_ready_msg: the server
    // tick number the client's anchor frame corresponds to (0 for the members
    // present at the start, the current server tick for a late joiner).  The
    // client numbers its inputs from it. docs/rooms-and-sessions-protocol.md 2.2.
    struct session_assign_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;

        session_assign_msg() : serializable_message(bmmo::SessionAssign) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
