#ifndef BALLANCEMMOSERVER_ROOM_STATE_MSG_HPP
#define BALLANCEMMOSERVER_ROOM_STATE_MSG_HPP
#include "message.hpp"
#include "../entity/room.hpp"

namespace bmmo {
    // server -> client, reliable. The room list plus, when the recipient is in
    // a room, that room's member roster. docs/rooms-and-sessions-protocol.md 1.3.
    struct room_state_msg : public serializable_message {
        uint32_t own_room = 0;                     // 0 = not in a room
        std::vector<room::room_info> rooms;
        std::vector<room::room_member> members;    // only for own_room

        room_state_msg() : serializable_message(bmmo::RoomState) {}

        static void write_map(const bmmo::map& m, std::stringstream& raw) {
            const auto type = static_cast<uint8_t>(m.type);
            raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
            raw.write(reinterpret_cast<const char*>(&m.level), sizeof(m.level));
            raw.write(reinterpret_cast<const char*>(m.md5), sizeof(m.md5));
        }

        static bool read_map(bmmo::map& m, std::stringstream& raw) {
            uint8_t type = 0;
            if (!message_utils::read_variable(raw, &type)) return false;
            if (!message_utils::read_variable(raw, &m.level)) return false;
            if (!raw.read(reinterpret_cast<char*>(m.md5), sizeof(m.md5))) return false;
            m.type = static_cast<bmmo::map_type>(type);
            return true;
        }

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&own_room), sizeof(own_room));

            uint16_t room_count = static_cast<uint16_t>(std::min<size_t>(rooms.size(), room::MAX_ROOMS_PER_MESSAGE));
            raw.write(reinterpret_cast<const char*>(&room_count), sizeof(room_count));
            for (uint16_t i = 0; i < room_count; ++i) {
                const auto& r = rooms[i];
                raw.write(reinterpret_cast<const char*>(&r.id), sizeof(r.id));
                message_utils::write_string<uint16_t>(r.name.substr(0, room::MAX_ROOM_NAME), raw);
                raw.write(reinterpret_cast<const char*>(&r.host), sizeof(r.host));
                raw.write(reinterpret_cast<const char*>(&r.member_count), sizeof(r.member_count));
                raw.write(reinterpret_cast<const char*>(&r.capacity), sizeof(r.capacity));
                const auto ph = static_cast<uint8_t>(r.room_phase);
                const auto md = static_cast<uint8_t>(r.room_mode);
                raw.write(reinterpret_cast<const char*>(&ph), sizeof(ph));
                raw.write(reinterpret_cast<const char*>(&md), sizeof(md));
            }

            uint16_t member_count = static_cast<uint16_t>(std::min<size_t>(members.size(), room::MAX_MEMBERS_PER_MESSAGE));
            raw.write(reinterpret_cast<const char*>(&member_count), sizeof(member_count));
            for (uint16_t i = 0; i < member_count; ++i) {
                const auto& mem = members[i];
                raw.write(reinterpret_cast<const char*>(&mem.id), sizeof(mem.id));
                message_utils::write_string<uint16_t>(mem.name.substr(0, room::MAX_MEMBER_NAME), raw);
                const uint8_t ready = mem.ready ? 1 : 0;
                const uint8_t is_host = mem.is_host ? 1 : 0;
                raw.write(reinterpret_cast<const char*>(&ready), sizeof(ready));
                raw.write(reinterpret_cast<const char*>(&is_host), sizeof(is_host));
                write_map(mem.map, raw);
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &own_room)) return false;

            uint16_t room_count = 0;
            if (!message_utils::read_variable(raw, &room_count)) return false;
            if (room_count > room::MAX_ROOMS_PER_MESSAGE) return false;
            rooms.clear();
            rooms.reserve(room_count);
            for (uint16_t i = 0; i < room_count; ++i) {
                room::room_info r;
                uint8_t ph = 0, md = 0;
                if (!message_utils::read_variable(raw, &r.id)) return false;
                if (!message_utils::read_string<uint16_t>(raw, r.name)) return false;
                if (r.name.size() > room::MAX_ROOM_NAME) return false;
                if (!message_utils::read_variable(raw, &r.host)) return false;
                if (!message_utils::read_variable(raw, &r.member_count)) return false;
                if (!message_utils::read_variable(raw, &r.capacity)) return false;
                if (!message_utils::read_variable(raw, &ph)) return false;
                if (!message_utils::read_variable(raw, &md)) return false;
                r.room_phase = static_cast<room::phase>(ph);
                r.room_mode = static_cast<room::mode>(md);
                rooms.push_back(std::move(r));
            }

            uint16_t member_count = 0;
            if (!message_utils::read_variable(raw, &member_count)) return false;
            if (member_count > room::MAX_MEMBERS_PER_MESSAGE) return false;
            members.clear();
            members.reserve(member_count);
            for (uint16_t i = 0; i < member_count; ++i) {
                room::room_member mem;
                uint8_t ready = 0, is_host = 0;
                if (!message_utils::read_variable(raw, &mem.id)) return false;
                if (!message_utils::read_string<uint16_t>(raw, mem.name)) return false;
                if (mem.name.size() > room::MAX_MEMBER_NAME) return false;
                if (!message_utils::read_variable(raw, &ready)) return false;
                if (!message_utils::read_variable(raw, &is_host)) return false;
                if (!read_map(mem.map, raw)) return false;
                mem.ready = ready != 0;
                mem.is_host = is_host != 0;
                members.push_back(std::move(mem));
            }
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_ROOM_STATE_MSG_HPP
