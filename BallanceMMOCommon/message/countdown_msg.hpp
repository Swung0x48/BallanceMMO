#ifndef BALLANCEMMOSERVER_COUNTDOWN_HPP
#define BALLANCEMMOSERVER_COUNTDOWN_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum countdown_type: uint8_t {
        CountdownType_Go,
        CountdownType_1,
        CountdownType_2,
        CountdownType_3,
        CountdownType_Unknown = 255
    };

    struct countdown {
        countdown_type type = CountdownType_Unknown;
        HSteamNetConnection sender = k_HSteamNetConnection_Invalid;
        struct map map;
        uint8_t restart_level = 0;
        uint8_t force_restart = 0;
    };

    typedef struct message<countdown, Countdown> countdown_msg;
}

#endif //BALLANCEMMOSERVER_COUNTDOWN_HPP