#ifndef BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
#include "message.hpp"

namespace bmmo {
    // server -> client, reliable, in reply to session_ready_msg: the server
    // tick number the client's anchor frame corresponds to (0 for the members
    // present at the start, the current server tick for a late joiner).  The
    // client numbers its inputs from it. docs/rooms-and-sessions-protocol.md 2.2.
    // Protocol 10: `flags` (session::ASSIGN_FLAG_*), on the reliable copy and
    // on every unreliable repeat, so whichever copy arrives first says it.
    // Protocol 24 (FULL step SAXH, the start handshake): two trailing i64s.
    // `server_us` is THIS copy's send instant on the server's steady clock
    // (microseconds; every copy is a clock sample the client acknowledges
    // with a SessionClockAck); `start_us` the instant, on the member's own
    // steady clock, at which its frame `first_tick` is due (0 = the server
    // has no clock estimate of the member yet: start on arrival).  A reader
    // of bytes without them (a capture of protocol <= 23) takes both as 0,
    // the pre-handshake assignment.
    struct session_assign_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        uint8_t flags = 0;
        int64_t server_us = 0;
        int64_t start_us = 0;

        session_assign_msg() : serializable_message(bmmo::SessionAssign) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            raw.write(reinterpret_cast<const char*>(&server_us), sizeof(server_us));
            raw.write(reinterpret_cast<const char*>(&start_us), sizeof(start_us));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            if (!message_utils::read_variable(raw, &flags)) return false;
            // Protocol 24's trailing fields: absent from older bytes (the
            // end of the message right after the flags), else both whole.
            if (raw.peek() == std::char_traits<char>::eof()) return true;
            if (!message_utils::read_variable(raw, &server_us)) return false;
            if (!message_utils::read_variable(raw, &start_us)) return false;
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_SESSION_ASSIGN_MSG_HPP
