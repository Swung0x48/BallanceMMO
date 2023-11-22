#ifndef BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#define BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum class simple_action: int32_t {
        Unknown,
        LoginDenied,
        CurrentMapQuery,
        FatalError, // client encountered a fatal error; end connection from server
        TriggerFatalError, // produce a fatal error just for fun
        BallOff, // we already have map/sector info so it's as simple as this
    };

    // struct simple_action {
    //     action_type action = action_type::Unknown;
    // };

    typedef struct message<simple_action, SimpleAction> simple_action_msg;
}

#endif //BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
