#ifndef BALLANCEMMOSERVER_SESSION_START_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_START_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"
#include "../entity/room.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    // server -> client, reliable. Announces a new physics session (or, for a
    // late joiner, re-announces the running one with just that player) and
    // the player roster. docs/rooms-and-sessions-protocol.md 2.2.
    struct session_start_msg : public serializable_message {
        uint32_t room = 0;
        uint32_t session = 0;              // increments on every Start
        room::mode mode = room::mode::Physics;
        bmmo::map map{};
        uint8_t tick_rate = 66;
        uint8_t snapshot_interval = 0;
        uint8_t input_delay = 0;
        uint32_t first_tick = 0;           // recipient's anchor tick number
        int32_t seed = 0;
        // Kick speed (m/s) applied to every spawn Physicalize of the session;
        // 0 = none (design 9.10).  The same value goes to every member,
        // late joiners included.
        float spawn_impulse = 0.0f;
        std::vector<session::player_entry> players;   // <= MAX_PLAYERS_PER_SESSION; spawn_* = the retail resetpoint

        session_start_msg() : serializable_message(bmmo::SessionStart) {}

        bool serialize() override {
            serializable_message::serialize();
            const auto m = static_cast<uint8_t>(mode);
            raw.write(reinterpret_cast<const char*>(&room), sizeof(room));
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&m), sizeof(m));

            const auto type = static_cast<uint8_t>(map.type);
            raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
            raw.write(reinterpret_cast<const char*>(&map.level), sizeof(map.level));
            raw.write(reinterpret_cast<const char*>(map.md5), sizeof(map.md5));

            raw.write(reinterpret_cast<const char*>(&tick_rate), sizeof(tick_rate));
            raw.write(reinterpret_cast<const char*>(&snapshot_interval), sizeof(snapshot_interval));
            raw.write(reinterpret_cast<const char*>(&input_delay), sizeof(input_delay));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&seed), sizeof(seed));
            raw.write(reinterpret_cast<const char*>(&spawn_impulse), sizeof(spawn_impulse));

            const uint8_t player_count = static_cast<uint8_t>(std::min<size_t>(players.size(), session::MAX_PLAYERS_PER_SESSION));
            raw.write(reinterpret_cast<const char*>(&player_count), sizeof(player_count));
            for (uint8_t i = 0; i < player_count; ++i) {
                const auto& p = players[i];
                raw.write(reinterpret_cast<const char*>(&p.id), sizeof(p.id));
                raw.write(reinterpret_cast<const char*>(&p.join_order), sizeof(p.join_order));
                raw.write(reinterpret_cast<const char*>(&p.ball_type), sizeof(p.ball_type));
                raw.write(reinterpret_cast<const char*>(p.spawn_position), sizeof(p.spawn_position));
                raw.write(reinterpret_cast<const char*>(p.spawn_rotation), sizeof(p.spawn_rotation));
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            uint8_t m = 0;
            if (!message_utils::read_variable(raw, &room)) return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &m)) return false;
            if (m > static_cast<uint8_t>(room::mode::Physics)) return false;
            mode = static_cast<room::mode>(m);

            uint8_t type = 0;
            if (!message_utils::read_variable(raw, &type)) return false;
            if (!message_utils::read_variable(raw, &map.level)) return false;
            if (!raw.read(reinterpret_cast<char*>(map.md5), sizeof(map.md5))) return false;
            map.type = static_cast<map_type>(type);

            if (!message_utils::read_variable(raw, &tick_rate)) return false;
            if (!message_utils::read_variable(raw, &snapshot_interval)) return false;
            if (!message_utils::read_variable(raw, &input_delay)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;
            if (!message_utils::read_variable(raw, &seed)) return false;
            if (!message_utils::read_variable(raw, &spawn_impulse)) return false;

            uint8_t player_count = 0;
            if (!message_utils::read_variable(raw, &player_count)) return false;
            if (player_count > session::MAX_PLAYERS_PER_SESSION) return false;
            players.clear();
            players.reserve(player_count);
            for (uint8_t i = 0; i < player_count; ++i) {
                session::player_entry p;
                if (!message_utils::read_variable(raw, &p.id)) return false;
                if (!message_utils::read_variable(raw, &p.join_order)) return false;
                if (!message_utils::read_variable(raw, &p.ball_type)) return false;
                if (!raw.read(reinterpret_cast<char*>(p.spawn_position), sizeof(p.spawn_position))) return false;
                if (!raw.read(reinterpret_cast<char*>(p.spawn_rotation), sizeof(p.spawn_rotation))) return false;
                players.push_back(p);
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_START_MSG_HPP
