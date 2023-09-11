#ifndef BALLANCEMMOSERVER_COUNTDOWN_HPP
#define BALLANCEMMOSERVER_COUNTDOWN_HPP
#include "message.hpp"
#include <cstdint>
#include <limits>

namespace bmmo {
    enum class countdown_type: uint8_t {
        Go = 0,
        Countdown_1 = 1,
        Countdown_2 = 2,
        Countdown_3 = 3,
        Ready = 4,
        ConfirmReady = 5,
        Unknown = std::numeric_limits<std::underlying_type_t<countdown_type>>::max(),
    };

    enum class level_mode: uint8_t {
        Speedrun = 0,
        Highscore = 1,
    };

    inline const char* get_level_mode_label(level_mode mode) {
        switch (mode) {
            case level_mode::Highscore:
                return " <HS>";
            case level_mode::Speedrun:
            default:
                return "";
        }
    }

    struct countdown {
        countdown_type type = countdown_type::Unknown;
        level_mode mode = level_mode::Speedrun;
        HSteamNetConnection sender = k_HSteamNetConnection_Invalid;
        struct map map{};
        uint8_t restart_level = 0;
        uint8_t force_restart = 0;

        inline auto get_level_mode_label() {
            return bmmo::get_level_mode_label(mode);
        }
    };

    typedef struct message<countdown, Countdown> countdown_msg;
}

#endif //BALLANCEMMOSERVER_COUNTDOWN_HPP
