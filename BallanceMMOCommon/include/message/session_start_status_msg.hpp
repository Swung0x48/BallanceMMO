#ifndef BALLANCEMMOSERVER_SESSION_START_STATUS_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_START_STATUS_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, reliable (protocol 24, FULL step SAXH): why the
    // session's start still waits - who has not confirmed, a postponed start
    // and its reason, a refusal - one message per state change of the start
    // handshake (session/start_handshake.hpp).  `confirmed` of `total`
    // members have confirmed the current phase; `text` is the line the
    // client shows the player (English, the server's words).
    // Protocol 26 (FULL step SAXR, the user's decision 64): `seq` (a trailing
    // u32) is the start the status describes - 0 for the assignment phase,
    // the current start for a wait, the start a postponement or a refusal
    // moved away from, the committed start for a commit; a client that
    // follows a newer start drops it as stale.  Bytes without it (a protocol
    // 24/25 capture) read `has_seq` false: no sequence, the old rule.
    struct session_start_status_msg : public serializable_message {
        uint32_t session = 0;
        uint8_t phase = 0;       // session::start_status_phase
        uint8_t confirmed = 0;
        uint8_t total = 0;
        std::string text;        // <= MAX_REASON
        uint32_t seq = 0;        // protocol 26: the start it describes
        bool has_seq = true;     // not on the wire: false when the bytes had no seq

        session_start_status_msg() : serializable_message(bmmo::SessionStartStatus) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&phase), sizeof(phase));
            raw.write(reinterpret_cast<const char*>(&confirmed), sizeof(confirmed));
            raw.write(reinterpret_cast<const char*>(&total), sizeof(total));
            message_utils::write_string<uint16_t>(text.substr(0, session::MAX_REASON), raw);
            raw.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &phase)) return false;
            if (!message_utils::read_variable(raw, &confirmed)) return false;
            if (!message_utils::read_variable(raw, &total)) return false;
            if (!message_utils::read_string<uint16_t>(raw, text)) return false;
            if (text.size() > session::MAX_REASON) return false;
            has_seq = raw.peek() != std::char_traits<char>::eof();
            seq = 0;
            if (has_seq && !message_utils::read_variable(raw, &seq)) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_START_STATUS_MSG_HPP
