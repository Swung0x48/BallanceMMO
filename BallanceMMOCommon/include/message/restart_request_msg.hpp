#ifndef BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct restart_request {
        bmmo::net::connection requester = bmmo::net::kInvalidConnection;
        bmmo::net::connection victim = bmmo::net::kInvalidConnection;
    };

    typedef struct message<restart_request, RestartRequest> restart_request_msg;
}

#endif //BALLANCEMMOSERVER_RESTART_REQUEST_MSG_HPP
