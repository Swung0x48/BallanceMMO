#ifndef BALLANCEMMOSERVER_OP_STATE_MSG_HPP
#define BALLANCEMMOSERVER_OP_STATE_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    struct op_state {
        uint8_t op = 0;
    };

    typedef struct message<op_state, OpState> op_state_msg;
}

#endif //BALLANCEMMOSERVER_OP_STATE_MSG_HPP