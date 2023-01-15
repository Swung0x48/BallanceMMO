#ifndef BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct ball_state {
        uint32_t type = 0;
        vec3 position;
        quaternion rotation;

        const std::string get_type_name() const {
            switch (type) {
                case 0: return "paper";
                case 1: return "stone";
                case 2: return "wood";
                default: return "unknown (id #" + std::to_string(type) + ")";
            }
        }
    };

    typedef struct message<ball_state, BallState> ball_state_msg;
}

#endif //BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
