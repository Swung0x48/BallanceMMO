#ifndef BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#define BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum simple_action_type: uint8_t {
        UnknownAction,
        LoginDenied,
        CurrentMapQuery,
    };

    struct simple_action {
        simple_action_type action = UnknownAction;
    };

    typedef struct message<simple_action, SimpleAction> simple_action_msg;
}

#endif //BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
