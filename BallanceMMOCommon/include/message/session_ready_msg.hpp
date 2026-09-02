#ifndef BALLANCEMMOSERVER_SESSION_READY_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_READY_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // client -> server, reliable. Confirms the client reached its session
    // anchor tick, for the server to compare against its own anchor hash and
    // collision surface signature (late joiners are exempt).
    // docs/rooms-and-sessions-protocol.md 2.2.
    struct session_ready_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        uint64_t anchor_hash = 0;       // anchor world hash
        uint64_t anchor_surfaces = 0;   // collision surface signature
        std::string physics_sha256;     // <= MAX_NAME
        std::string build_id;           // <= MAX_NAME

        session_ready_msg() : serializable_message(bmmo::SessionReady) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&anchor_hash), sizeof(anchor_hash));
            raw.write(reinterpret_cast<const char*>(&anchor_surfaces), sizeof(anchor_surfaces));
            message_utils::write_string<uint16_t>(physics_sha256.substr(0, session::MAX_NAME), raw);
            message_utils::write_string<uint16_t>(build_id.substr(0, session::MAX_NAME), raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            if (!message_utils::read_variable(raw, &anchor_hash)) return false;
            if (!message_utils::read_variable(raw, &anchor_surfaces)) return false;
            if (!message_utils::read_string<uint16_t>(raw, physics_sha256)) return false;
            if (physics_sha256.size() > session::MAX_NAME) return false;
            if (!message_utils::read_string<uint16_t>(raw, build_id)) return false;
            if (build_id.size() > session::MAX_NAME) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_READY_MSG_HPP
