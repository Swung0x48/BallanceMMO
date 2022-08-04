#ifndef BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#define BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum class action_type: uint8_t {
        Unknown,
        LoginDenied,
        CurrentMapQuery,
    };

    struct simple_action {
        action_type action = action_type::Unknown;
    };

    typedef struct message<simple_action, SimpleAction> simple_action_msg;
}

#endif //BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
