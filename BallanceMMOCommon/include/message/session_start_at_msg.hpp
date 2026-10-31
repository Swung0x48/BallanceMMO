#ifndef BALLANCEMMOSERVER_SESSION_START_AT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_START_AT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, ReliableNoNagle plus unreliable copies one per
    // main-loop pass (protocol 24, FULL step SAXH; session/start_handshake.hpp).
    // Phase 2 of the start handshake: every member present at the start
    // acknowledged its scheduled assignment, so the server picked one start
    // instant X on its own clock and tells each member when, on the member's
    // own steady clock, its frame `first_tick` is due (`start_us`: X plus the
    // member's measured clock offset and half its round trip, so each member
    // starts where its assignment would have arrived on a lossless link).  A
    // higher `seq` replaces a lower one (a postponed start).  The client
    // starts only on a message that arrived before its instant and confirms
    // it with a SessionClockAck (CLOCK_ACK_START, seq).
    // START_AT_FLAG_CORRECTION: the instant a late joiner's frame
    // `first_tick` was due; the joiner already runs and catches up the whole
    // ticks it started late.
    struct session_start_at_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        uint32_t seq = 0;
        uint8_t flags = 0;
        int64_t server_us = 0;        // this copy's send instant (server clock): also a clock sample
        int64_t start_server_us = 0;  // X on the server's clock
        int64_t start_us = 0;         // the member's frame first_tick, on its own clock

        session_start_at_msg() : serializable_message(bmmo::SessionStartAt) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
            raw.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            raw.write(reinterpret_cast<const char*>(&server_us), sizeof(server_us));
            raw.write(reinterpret_cast<const char*>(&start_server_us), sizeof(start_server_us));
            raw.write(reinterpret_cast<const char*>(&start_us), sizeof(start_us));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            if (!message_utils::read_variable(raw, &seq)) return false;
            if (!message_utils::read_variable(raw, &flags)) return false;
            if (!message_utils::read_variable(raw, &server_us)) return false;
            if (!message_utils::read_variable(raw, &start_server_us)) return false;
            if (!message_utils::read_variable(raw, &start_us)) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_START_AT_MSG_HPP
