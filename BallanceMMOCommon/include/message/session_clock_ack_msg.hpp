#ifndef BALLANCEMMOSERVER_SESSION_CLOCK_ACK_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_CLOCK_ACK_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // client -> server, UnreliableNoDelay (protocol 24, FULL step SAXH): the
    // answer to every copy of a SessionAssign or a SessionStartAt that
    // carried a server instant.  `server_us` echoes the copy's; `client_us`
    // is the client's steady clock when it handled the copy.  With the
    // server's own receive instant that is one clock sample: round trip
    // t4 - t1, offset client_us - (t1 + t4) / 2 (session/start_handshake.hpp).
    // A start copy's ack (kind CLOCK_ACK_START) also confirms `seq`: the
    // client will start at that message's instant; CLOCK_ACK_STARTED answers
    // a start copy that came after the client had started: `seq` is the
    // start it started on.
    struct session_clock_ack_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;   // the epoch the copy belonged to
        uint8_t kind = session::CLOCK_ACK_ASSIGN;
        uint32_t seq = 0;          // CLOCK_ACK_START: the start message's seq
        int64_t server_us = 0;
        int64_t client_us = 0;

        session_clock_ack_msg() : serializable_message(bmmo::SessionClockAck) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            raw.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
            raw.write(reinterpret_cast<const char*>(&server_us), sizeof(server_us));
            raw.write(reinterpret_cast<const char*>(&client_us), sizeof(client_us));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            if (!message_utils::read_variable(raw, &kind)) return false;
            if (!message_utils::read_variable(raw, &seq)) return false;
            if (!message_utils::read_variable(raw, &server_us)) return false;
            if (!message_utils::read_variable(raw, &client_us)) return false;
            if (kind != session::CLOCK_ACK_ASSIGN && kind != session::CLOCK_ACK_START
                    && kind != session::CLOCK_ACK_STARTED)
                return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_CLOCK_ACK_MSG_HPP
