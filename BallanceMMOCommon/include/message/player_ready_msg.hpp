#ifndef BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_ready_data {
        bmmo::net::connection player_id = bmmo::net::kInvalidConnection;
        uint32_t count = 0;
        uint8_t ready = false;
    };

    typedef struct message<player_ready_data, PlayerReady> player_ready_msg;
}

#endif //BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP