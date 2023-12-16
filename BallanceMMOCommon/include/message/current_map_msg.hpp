#ifndef BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#define BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    struct current_map_state {
        enum state_type: uint8_t { None, Announcement, EnteringMap, NameChange };
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        struct map map{};
        int32_t sector = 0;
        state_type type = None;
    };
    
    typedef struct message<current_map_state, CurrentMap> current_map_msg;
}

#endif //BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
