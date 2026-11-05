#ifndef BALLANCEMMOSERVER_OWNED_CHEAT_STATE_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_CHEAT_STATE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct owned_cheat_state {
        cheat_state state{};
        bmmo::net::connection player_id = bmmo::net::kInvalidConnection;
    };

    typedef struct message<owned_cheat_state, OwnedCheatState> owned_cheat_state_msg;
}

#endif //BALLANCEMMOSERVER_OWNED_CHEAT_STATE_MSG_HPP