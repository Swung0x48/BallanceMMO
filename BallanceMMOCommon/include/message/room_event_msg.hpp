#ifndef BALLANCEMMOSERVER_ROOM_EVENT_MSG_HPP
#define BALLANCEMMOSERVER_ROOM_EVENT_MSG_HPP
#include "message.hpp"
#include "../entity/room.hpp"

namespace bmmo {
    // server -> client, reliable. A single room-system notification or the
    // outcome of a room_request. docs/rooms-and-sessions-protocol.md 1.3.
    struct room_event_msg : public serializable_message {
        room::event_type type = room::event_type::RequestAccepted;
        room::error_code error = room::error_code::None;
        uint32_t room = 0;
        uint32_t actor = 0;     // who caused the event
        uint32_t subject = 0;   // who it happened to (may be 0)
        std::string reason;     // <= MAX_REASON

        room_event_msg() : serializable_message(bmmo::RoomEvent) {}

        bool serialize() override {
            serializable_message::serialize();
            const auto t = static_cast<uint8_t>(type);
            const auto e = static_cast<uint8_t>(error);
            raw.write(reinterpret_cast<const char*>(&t), sizeof(t));
            raw.write(reinterpret_cast<const char*>(&e), sizeof(e));
            raw.write(reinterpret_cast<const char*>(&room), sizeof(room));
            raw.write(reinterpret_cast<const char*>(&actor), sizeof(actor));
            raw.write(reinterpret_cast<const char*>(&subject), sizeof(subject));
            message_utils::write_string<uint16_t>(reason.substr(0, room::MAX_REASON), raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            uint8_t t = 0, e = 0;
            if (!message_utils::read_variable(raw, &t)) return false;
            if (!message_utils::read_variable(raw, &e)) return false;
            if (!message_utils::read_variable(raw, &room)) return false;
            if (!message_utils::read_variable(raw, &actor)) return false;
            if (!message_utils::read_variable(raw, &subject)) return false;
            if (!message_utils::read_string<uint16_t>(raw, reason)) return false;
            if (reason.size() > room::MAX_REASON) return false;
            if (t > static_cast<uint8_t>(room::event_type::SessionEnded)) return false;
            if (e > static_cast<uint8_t>(room::error_code::Unsupported)) return false;
            type = static_cast<room::event_type>(t);
            error = static_cast<room::error_code>(e);
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_ROOM_EVENT_MSG_HPP
