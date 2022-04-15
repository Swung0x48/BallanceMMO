#ifndef BALLANCEMMOSERVER_CHEAT_STATE_MSG_HPP
#define BALLANCEMMOSERVER_CHEAT_STATE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct cheat_state {
        uint8_t cheated;
        uint8_t notify = 1;
    };

    typedef struct message<cheat_state, CheatState> cheat_state_msg;
}

#endif //BALLANCEMMOSERVER_CHEAT_STATE_MSG_HPP