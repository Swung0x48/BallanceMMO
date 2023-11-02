#ifndef BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct player_ready_data {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        uint32_t count = 0;
        uint8_t ready = false;
    };

    typedef struct message<player_ready_data, PlayerReady> player_ready_msg;
}

#endif //BALLANCEMMOSERVER_PLAYER_READY_MSG_HPP