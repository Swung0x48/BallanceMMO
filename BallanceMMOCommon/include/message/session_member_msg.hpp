#ifndef BALLANCEMMOSERVER_SESSION_MEMBER_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_MEMBER_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, ReliableNoNagle plus the lifecycle events' unreliable
    // repeats (protocol 18; docs/world-agreement-gaps-design.md 2.7, 2.8, step
    // M1).  A life of a member - the server's interval [J, R) of that player
    // in its world - was added (`tick` = J, the tick of the server's PLAYER
    // record) or removed (`tick` = R).  Sent to every member that has its
    // SessionStart, except the player itself.  A receiver keys it by
    // (entry.id, entry.life), so copies and out-of-order arrivals are
    // harmless.  A removed one carries the id, the life and the join order.
    struct session_member_msg : public serializable_message {
        uint32_t session = 0;
        uint8_t kind = 0;                    // session::MEMBER_ADDED / MEMBER_REMOVED
        uint32_t tick = 0;                   // added: J; removed: R
        session::player_entry entry{};

        session_member_msg() : serializable_message(bmmo::SessionMember) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            raw.write(reinterpret_cast<const char*>(&entry.id), sizeof(entry.id));
            raw.write(reinterpret_cast<const char*>(&entry.join_order), sizeof(entry.join_order));
            raw.write(reinterpret_cast<const char*>(&entry.ball_type), sizeof(entry.ball_type));
            raw.write(reinterpret_cast<const char*>(entry.spawn_position), sizeof(entry.spawn_position));
            raw.write(reinterpret_cast<const char*>(entry.spawn_rotation), sizeof(entry.spawn_rotation));
            raw.write(reinterpret_cast<const char*>(&entry.life), sizeof(entry.life));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &kind)) return false;
            if (!message_utils::read_variable(raw, &tick)) return false;
            if (!message_utils::read_variable(raw, &entry.id)) return false;
            if (!message_utils::read_variable(raw, &entry.join_order)) return false;
            if (!message_utils::read_variable(raw, &entry.ball_type)) return false;
            if (!raw.read(reinterpret_cast<char*>(entry.spawn_position), sizeof(entry.spawn_position))) return false;
            if (!raw.read(reinterpret_cast<char*>(entry.spawn_rotation), sizeof(entry.spawn_rotation))) return false;
            if (!message_utils::read_variable(raw, &entry.life)) return false;
            // The reader's refusals (design 2.7).
            if (kind != session::MEMBER_ADDED && kind != session::MEMBER_REMOVED) return false;
            if (entry.join_order >= session::MAX_PLAYERS_PER_SESSION) return false;
            if (entry.id == 0 || entry.life == 0) return false;
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_MEMBER_MSG_HPP
