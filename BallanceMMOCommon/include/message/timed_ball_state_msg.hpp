#ifndef BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
#include "message.hpp"
#include <cstring>
#include "ball_state_msg.hpp"
#include "timestamp_msg.hpp"

namespace bmmo {
    // We cannot just use a int64_t (taking 8 bits) here;
    // ball_state has the size of 32, but opcode comes before it and has a size of 4.
    // No matter how the order is, we will always get a padding of 4 bytes.
    struct timed_ball_state: ball_state {
        timestamp_t timestamp{};
    };

    typedef struct message<timed_ball_state, TimedBallState> timed_ball_state_msg;
}

#endif //BALLANCEMMOSERVER_TIMED_BALL_STATE_MSG_HPP
