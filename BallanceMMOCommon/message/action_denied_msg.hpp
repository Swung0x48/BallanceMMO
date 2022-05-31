#ifndef BALLANCEMMOSERVER_ACTION_DENIED_HPP
#define BALLANCEMMOSERVER_ACTION_DENIED_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum action_denied_reason: uint8_t {
        UnknownReason,
        NoPermission,
        InvalidAction,
        InvalidTarget,
        TargetNotFound
    };

    struct action_denied {
        action_denied_reason reason = UnknownReason;
    };

    typedef struct message<action_denied, ActionDenied> action_denied_msg;
}

#endif //BALLANCEMMOSERVER_ACTION_DENIED_HPP