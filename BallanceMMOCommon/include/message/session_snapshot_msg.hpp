#ifndef BALLANCEMMOSERVER_SESSION_SNAPSHOT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_SNAPSHOT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client. Delta snapshots (full == 0) are sent unreliable; full
    // snapshots (full != 0, carrying the mechanism name dictionary implicitly
    // via each mechanism row's name) are sent reliable.
    // docs/rooms-and-sessions-protocol.md 2.2.
    struct session_snapshot_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t tick = 0;
        uint8_t full = 0;
        uint32_t acked_input_tick = 0;
        std::vector<session::body_state> bodies;    // <= MAX_BODIES_PER_SNAPSHOT

        session_snapshot_msg() : serializable_message(bmmo::SessionSnapshot) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            raw.write(reinterpret_cast<const char*>(&full), sizeof(full));
            raw.write(reinterpret_cast<const char*>(&acked_input_tick), sizeof(acked_input_tick));

            const uint16_t body_count = static_cast<uint16_t>(std::min<size_t>(bodies.size(), session::MAX_BODIES_PER_SNAPSHOT));
            raw.write(reinterpret_cast<const char*>(&body_count), sizeof(body_count));
            for (uint16_t i = 0; i < body_count; ++i) {
                const auto& b = bodies[i];
                const auto kind = static_cast<uint8_t>(b.kind);
                raw.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
                raw.write(reinterpret_cast<const char*>(&b.owner), sizeof(b.owner));
                // the name is only meaningful for a full snapshot's mechanism
                // rows; every other row carries no length prefix at all, not
                // even a zero-length string
                if (full != 0 && b.kind == session::body_kind::Mechanism)
                    message_utils::write_string<uint16_t>(b.name.substr(0, session::MAX_NAME), raw);
                raw.write(reinterpret_cast<const char*>(b.position), sizeof(b.position));
                raw.write(reinterpret_cast<const char*>(b.rotation), sizeof(b.rotation));
                raw.write(reinterpret_cast<const char*>(b.linear), sizeof(b.linear));
                raw.write(reinterpret_cast<const char*>(b.angular), sizeof(b.angular));
                raw.write(reinterpret_cast<const char*>(&b.flags), sizeof(b.flags));
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &tick)) return false;
            if (!message_utils::read_variable(raw, &full)) return false;
            if (!message_utils::read_variable(raw, &acked_input_tick)) return false;

            uint16_t body_count = 0;
            if (!message_utils::read_variable(raw, &body_count)) return false;
            if (body_count > session::MAX_BODIES_PER_SNAPSHOT) return false;
            bodies.clear();
            bodies.reserve(body_count);
            for (uint16_t i = 0; i < body_count; ++i) {
                session::body_state b;
                uint8_t kind = 0;
                if (!message_utils::read_variable(raw, &kind)) return false;
                if (kind > static_cast<uint8_t>(session::body_kind::Mechanism)) return false;
                b.kind = static_cast<session::body_kind>(kind);
                if (!message_utils::read_variable(raw, &b.owner)) return false;
                if (full != 0 && b.kind == session::body_kind::Mechanism) {
                    if (!message_utils::read_string<uint16_t>(raw, b.name)) return false;
                    if (b.name.size() > session::MAX_NAME) return false;
                }
                if (!raw.read(reinterpret_cast<char*>(b.position), sizeof(b.position))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.rotation), sizeof(b.rotation))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.linear), sizeof(b.linear))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.angular), sizeof(b.angular))) return false;
                if (!message_utils::read_variable(raw, &b.flags)) return false;
                bodies.push_back(std::move(b));
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_SNAPSHOT_MSG_HPP
