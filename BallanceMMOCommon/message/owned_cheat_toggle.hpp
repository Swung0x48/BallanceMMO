#ifndef BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct owned_cheat_toggle {
        cheat_state state{};
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
    };

    typedef struct message<owned_cheat_state, OwnedCheatToggle> owned_cheat_state_msg;
}

#endif //BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
