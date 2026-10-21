#ifndef BALLANCEMMOSERVER_SESSION_LEAD_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_LEAD_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, reliable, followed by unreliable copies 40 ms apart
    // until the client acknowledges it (late-player design 2.2): the player's
    // own lead changes by `delta` ticks.  SessionAssign cannot carry this: a
    // newer first_tick means a renumbering, a history wipe and a snapshot
    // resync, while a lead change renumbers nothing.  Sent only to a client
    // whose SessionReady declared session::CAP_LEAD.
    //
    // A raise also names the range [held_from, effective) the server's input
    // buffer for this player was reset over (design 2.3): every tick of it is
    // applied with `held`, the substitute frozen at decision time, so the
    // message carries what the server applies there and the client can amend
    // its own history once instead of being corrected tick by tick.  A lower
    // moves no range (held_from 0, effective = the decision's next tick).
    struct session_lead_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t player = 0;        // the recipient, so a journal can keep the bytes verbatim
        // The decision id (lateness_meter decision ids, unique per player for
        // the session's life): what the ack names, and the client's dedupe of
        // the repeated copies.  32 bits rather than the design's 16, so it is
        // the meter's id itself and never wraps.
        uint32_t seq = 0;
        int16_t delta = 0;          // ticks; positive = run further ahead
        uint32_t held_from = 0;     // N: the first tick the reset substitutes; 0 = no range
        uint32_t effective = 0;     // E: from this tick the server expects the new schedule
        session::input_frame held{};    // the substitute for [held_from, effective)
        uint32_t held_source = session::INPUT_SOURCE_DEFAULT;   // the tick `held` was sent for
        // The base of the assignment this decision belongs to: a client whose
        // base is newer (a resync came in between) drops it.
        uint32_t epoch = 0;

        session_lead_msg() : serializable_message(bmmo::SessionLead) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&player), sizeof(player));
            raw.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
            raw.write(reinterpret_cast<const char*>(&delta), sizeof(delta));
            raw.write(reinterpret_cast<const char*>(&held_from), sizeof(held_from));
            raw.write(reinterpret_cast<const char*>(&effective), sizeof(effective));
            raw.write(reinterpret_cast<const char*>(&held.keys), sizeof(held.keys));
            raw.write(reinterpret_cast<const char*>(held.cam_right), sizeof(held.cam_right));
            raw.write(reinterpret_cast<const char*>(held.cam_up), sizeof(held.cam_up));
            raw.write(reinterpret_cast<const char*>(held.cam_dir), sizeof(held.cam_dir));
            raw.write(reinterpret_cast<const char*>(&held.ball_type), sizeof(held.ball_type));
            raw.write(reinterpret_cast<const char*>(&held.flags), sizeof(held.flags));
            raw.write(reinterpret_cast<const char*>(&held_source), sizeof(held_source));
            raw.write(reinterpret_cast<const char*>(&epoch), sizeof(epoch));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &player)) return false;
            if (!message_utils::read_variable(raw, &seq)) return false;
            if (!message_utils::read_variable(raw, &delta)) return false;
            if (!message_utils::read_variable(raw, &held_from)) return false;
            if (!message_utils::read_variable(raw, &effective)) return false;
            if (!message_utils::read_variable(raw, &held.keys)) return false;
            if (!raw.read(reinterpret_cast<char*>(held.cam_right), sizeof(held.cam_right))) return false;
            if (!raw.read(reinterpret_cast<char*>(held.cam_up), sizeof(held.cam_up))) return false;
            if (!raw.read(reinterpret_cast<char*>(held.cam_dir), sizeof(held.cam_dir))) return false;
            if (!message_utils::read_variable(raw, &held.ball_type)) return false;
            if (!message_utils::read_variable(raw, &held.flags)) return false;
            if (!message_utils::read_variable(raw, &held_source)) return false;
            if (!message_utils::read_variable(raw, &epoch)) return false;
            // A held range is never empty and never reversed; a decision
            // that moves nothing is not sent.
            if (held_from != 0 && held_from >= effective) return false;
            if (delta == 0 || seq == 0) return false;
            return true;
        }
    };

    // client -> server, reliable: the client applied `applied` ticks of the
    // SessionLead `seq` (as far as its own caps allowed; 0 = it applied
    // nothing).  The server keeps lead_offset[player] += applied.
    struct session_lead_ack_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t seq = 0;
        int16_t applied = 0;

        session_lead_ack_msg() : serializable_message(bmmo::SessionLeadAck) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
            raw.write(reinterpret_cast<const char*>(&applied), sizeof(applied));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &seq)) return false;
            if (!message_utils::read_variable(raw, &applied)) return false;
            return seq != 0;
        }
    };
}

#endif // BALLANCEMMOSERVER_SESSION_LEAD_MSG_HPP
