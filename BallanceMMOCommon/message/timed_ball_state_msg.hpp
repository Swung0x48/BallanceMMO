#ifndef BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
#include "message.hpp"
#include "ball_state_msg.hpp"

namespace bmmo {
    struct timed_ball_state: ball_state {
        uint64_t timestamp = 0;
    };

    typedef struct message<timed_ball_state, TimedBallState> timed_ball_state_msg;
}

#endif //BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
