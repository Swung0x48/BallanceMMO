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
        TargetNotFound,
        PlayerMuted
    };

    struct action_denied {
        deny_reason reason = deny_reason::Unknown;

        std::string to_string() {
            using dr = deny_reason;
            switch (reason) {
                case dr::NoPermission: return "you don't have the permission to run this action.";
                case dr::InvalidAction: return "invalid action.";
                case dr::InvalidTarget: return "invalid target.";
                case dr::TargetNotFound: return "target not found.";
                case dr::PlayerMuted: return "you are not allowed to post public messages on this server.";
                default: return "unknown reason.";
            };
        }
    };

    typedef struct message<action_denied, ActionDenied> action_denied_msg;
}

#endif //BALLANCEMMOSERVER_ACTION_DENIED_HPP
