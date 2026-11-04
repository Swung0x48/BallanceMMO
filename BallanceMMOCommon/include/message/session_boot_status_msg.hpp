#ifndef BALLANCEMMOSERVER_SESSION_BOOT_STATUS_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_BOOT_STATUS_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // server -> client, reliable (protocol 32, step SPL): the phase of the
    // session's world boot that has just begun (session::boot_phase), sent to
    // the room's members from the room's start until their SessionStart, so
    // the client can show what the wait is.  `level` is the level the world
    // boots.  A boot that fails ends with the session's SessionEnd.
    struct session_boot_status_msg : public serializable_message {
        uint32_t session = 0;
        int32_t level = 0;
        uint8_t phase = 0;       // session::boot_phase

        session_boot_status_msg() : serializable_message(bmmo::SessionBootStatus) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&level), sizeof(level));
            raw.write(reinterpret_cast<const char*>(&phase), sizeof(phase));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &level)) return false;
            if (!message_utils::read_variable(raw, &phase)) return false;
            return phase <= static_cast<uint8_t>(session::boot_phase::preparing_world);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_BOOT_STATUS_MSG_HPP
