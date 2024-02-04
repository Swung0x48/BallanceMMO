#ifndef BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct restart_request {
        HSteamNetConnection requester = k_HSteamNetConnection_Invalid;
        HSteamNetConnection victim = k_HSteamNetConnection_Invalid;
    };

    typedef struct message<restart_request, RestartRequest> restart_request_msg;
}

#endif //BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
