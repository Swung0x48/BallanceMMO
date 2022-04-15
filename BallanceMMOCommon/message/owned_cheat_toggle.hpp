#ifndef BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct owned_cheat_toggle {
        cheat_state state{};
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        uint8_t notify = 1;
    };

    typedef struct message<owned_cheat_toggle, OwnedCheatToggle> owned_cheat_toggle_msg;
}

#endif //BALLANCEMMOSERVER_OWNED_CHEAT_TOGGLE_MSG_HPP
