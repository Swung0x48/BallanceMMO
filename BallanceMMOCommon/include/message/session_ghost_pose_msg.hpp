#ifndef BALLANCEMMOSERVER_SESSION_GHOST_POSE_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_GHOST_POSE_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

#include <cmath>

namespace bmmo {
    // Protocol 14 (step BALA, the airship rule of 2026-10-28): the pose of a
    // ball that has left the sync in the airship zone.
    //
    // client -> server, unreliable, every kGhostPoseInterval ticks while the
    // sender's own ball is inside its world's airship zone (player = 0); the
    // server stamps `player` with the sender and relays it, unreliable, to
    // every other member of the session.  Display only: a receiver moves the
    // sender's ghost (its mirror, out of the simulation since its own copy
    // entered the zone) to it; nothing simulated reads it, and the server
    // never applies it to its own world.
    struct session_ghost_pose_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t player = 0;     // the ball's owner (set by the server)
        uint32_t tick = 0;       // the owner's tick the pose is of
        float position[3] = {};
        float rotation[4] = {0, 0, 0, 1};   // x, y, z, w

        session_ghost_pose_msg() : serializable_message(bmmo::SessionGhostPose) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&player), sizeof(player));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            raw.write(reinterpret_cast<const char*>(position), sizeof(position));
            raw.write(reinterpret_cast<const char*>(rotation), sizeof(rotation));
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &player)) return false;
            if (!message_utils::read_variable(raw, &tick)) return false;
            if (!raw.read(reinterpret_cast<char*>(position), sizeof(position))) return false;
            if (!raw.read(reinterpret_cast<char*>(rotation), sizeof(rotation))) return false;
            for (float v: position) if (!std::isfinite(v)) return false;
            for (float v: rotation) if (!std::isfinite(v)) return false;
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_SESSION_GHOST_POSE_MSG_HPP
