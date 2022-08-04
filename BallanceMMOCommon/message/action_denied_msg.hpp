#ifndef BALLANCEMMOSERVER_ACTION_DENIED_HPP
#define BALLANCEMMOSERVER_ACTION_DENIED_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum class deny_reason: uint8_t {
        Unknown,
        NoPermission,
        InvalidAction,
        InvalidTarget,
        TargetNotFound
    };

    struct action_denied {
        deny_reason reason = deny_reason::Unknown;
    };

    typedef struct message<action_denied, ActionDenied> action_denied_msg;
}

#endif //BALLANCEMMOSERVER_ACTION_DENIED_HPP
