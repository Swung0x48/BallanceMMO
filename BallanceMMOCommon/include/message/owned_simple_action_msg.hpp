#ifndef BALLANCEMMOSERVER_OWNED_SIMPLE_ACTION_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_SIMPLE_ACTION_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum class owned_simple_action_type: int32_t {
        Unknown,
        RestartRequestFailed,
    };

    struct owned_simple_action {
        owned_simple_action_type type = owned_simple_action_type::Unknown;
        bmmo::net::connection player_id = bmmo::net::kInvalidConnection;
    };

    typedef struct message<owned_simple_action, OwnedSimpleAction> owned_simple_action_msg;
}

#endif //BALLANCEMMOSERVER_SIMPLE_ACTION_MSG_HPP
