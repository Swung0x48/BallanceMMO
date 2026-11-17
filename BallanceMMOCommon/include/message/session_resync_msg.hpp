#ifndef BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../session/resync_state_codec.hpp"

namespace bmmo {
    // client -> server, reliable. Asks the server to reply with a full
    // snapshot. docs/rooms-and-sessions-protocol.md 2.2.  Protocol 34: and
    // names the last resync state the client decoded (`held`, its tick and
    // frame check), which the server's answer may be coded against
    // (session/resync_state_codec.hpp).
    struct session_resync_msg : public serializable_message {
        // Protocol 43 (fork detection 15.1 row 8): why the client asks, one
        // value per class of request_resync's call sites; the server
        // journals it in resync_request.
        enum class reason : uint8_t {
            other = 0, past_reach, own_life, state_refused, physics_fault, corrections, schedule, unreplayable, starved,
            count
        };
        static const char* reason_name(uint8_t r) {
            static constexpr const char* names[] = {"other", "past_reach", "own_life", "state_refused", "physics_fault",
                                                    "corrections", "schedule", "unreplayable", "starved"};
            return r < static_cast<uint8_t>(reason::count) ? names[r] : "other";
        }

        uint32_t session = 0;
        uint32_t last_full_tick = 0;
        session::resync_ref_id held;
        uint8_t why = 0;   // reason

        session_resync_msg() : serializable_message(bmmo::SessionResync) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&last_full_tick), sizeof(last_full_tick));
            const uint8_t has = held.held ? 1 : 0;
            raw.write(reinterpret_cast<const char*>(&has), sizeof(has));
            raw.write(reinterpret_cast<const char*>(&held.tick), sizeof(held.tick));
            raw.write(reinterpret_cast<const char*>(&held.check), sizeof(held.check));
            raw.write(reinterpret_cast<const char*>(&why), sizeof(why));
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
            if (!message_utils::read_variable(raw, &why)) return false;
            held.held = has != 0;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_RESYNC_MSG_HPP
