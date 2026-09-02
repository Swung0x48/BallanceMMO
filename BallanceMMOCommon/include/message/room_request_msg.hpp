#ifndef BALLANCEMMOSERVER_ROOM_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_ROOM_REQUEST_MSG_HPP
#include "message.hpp"
#include "../entity/room.hpp"

namespace bmmo {
    // client -> server, reliable. One request against the room system.
    // docs/rooms-and-sessions-protocol.md 1.3.
    struct room_request_msg : public serializable_message {
        room::action action = room::action::List;
        uint32_t room = 0;                 // target room; 0 for Create / List
        std::string name;                  // Create only, <= MAX_ROOM_NAME
        uint32_t target = 0;               // Kick only: player to remove
        room::mode mode = room::mode::Shadow;  // Start only

        room_request_msg() : serializable_message(bmmo::RoomRequest) {}

        bool serialize() override {
            serializable_message::serialize();
            const auto a = static_cast<uint8_t>(action);
            const auto m = static_cast<uint8_t>(mode);
            raw.write(reinterpret_cast<const char*>(&a), sizeof(a));
            raw.write(reinterpret_cast<const char*>(&room), sizeof(room));
            message_utils::write_string<uint16_t>(name.substr(0, room::MAX_ROOM_NAME), raw);
            raw.write(reinterpret_cast<const char*>(&target), sizeof(target));
            raw.write(reinterpret_cast<const char*>(&m), sizeof(m));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            uint8_t a = 0, m = 0;
            if (!message_utils::read_variable(raw, &a)) return false;
            if (!message_utils::read_variable(raw, &room)) return false;
            if (!message_utils::read_string<uint16_t>(raw, name)) return false;
            if (name.size() > room::MAX_ROOM_NAME) return false;
            if (!message_utils::read_variable(raw, &target)) return false;
            if (!message_utils::read_variable(raw, &m)) return false;
            if (a > static_cast<uint8_t>(room::action::Close)) return false;
            if (m > static_cast<uint8_t>(room::mode::Physics)) return false;
            action = static_cast<room::action>(a);
            mode = static_cast<room::mode>(m);
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_ROOM_REQUEST_MSG_HPP
