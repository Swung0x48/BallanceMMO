#ifndef BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct owned_cheat_toggle {
        cheat_state state{};
        bmmo::net::connection player_id = bmmo::net::kInvalidConnection;
    };

    typedef struct message<owned_cheat_toggle, OwnedCheatToggle> owned_cheat_toggle_msg;
}

#endif //BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
