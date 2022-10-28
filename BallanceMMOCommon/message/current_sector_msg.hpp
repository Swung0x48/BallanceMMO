#ifndef BALLANCEMMOSERVER_CURRENT_SECTOR_MSG_HPP
#define BALLANCEMMOSERVER_CURRENT_SECTOR_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct current_sector {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        int32_t sector = 0;
    };

    typedef struct message<current_sector, CurrentSector> current_sector_msg;
}

#endif //BALLANCEMMOSERVER_CURRENT_SECTOR_MSG_HPP
