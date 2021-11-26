#ifndef BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct disconnected_player {
        HSteamNetConnection connection_id;
    };

    typedef struct message<disconnected_player, PlayerDisconnected> player_disconnected_msg;
}

#endif //BALLANCEMMOSERVER_PLAYER_DISCONNECTED_MSG_HPP
