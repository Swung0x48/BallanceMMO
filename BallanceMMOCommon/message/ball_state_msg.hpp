#ifndef BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct ball_state {
        vec3 position;
        quaternion rotation;
    };

    typedef struct message<ball_state, BallState> ball_state_msg;
}

#endif //BALLANCEMMOSERVER_BALL_STATE_MSG_HPP
