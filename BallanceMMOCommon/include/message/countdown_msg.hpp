#ifndef BALLANCEMMOSERVER_COUNTDOWN_HPP
#define BALLANCEMMOSERVER_COUNTDOWN_HPP
#include "message.hpp"
#include "../entity/map.hpp"
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
        Countdown_6 = 6, // easter egg
        Unknown = std::numeric_limits<std::underlying_type_t<countdown_type>>::max(),
    };

    struct countdown {
        countdown_type type = countdown_type::Unknown;
        level_mode mode = level_mode::Speedrun;
        HSteamNetConnection sender = k_HSteamNetConnection_Invalid;
        struct map map{};
        uint8_t restart_level = 0;
        uint8_t force_restart = 0;

        inline auto get_level_mode_label() const {
            return std::string{force_restart ? "*" : ""} + bmmo::get_level_mode_label(mode);
        }

        std::string get_type_label() const {
            using ct = countdown_type;
            switch (type) {
                case ct::Go: return "Go!";
                case ct::Ready: return "Get ready";
                case ct::ConfirmReady: return "Please use \"/mmo ready\" to confirm if you are ready";
                case ct::Unknown: return "N/A";
                default: return std::to_string(static_cast<std::underlying_type_t<countdown_type>>(type));
            }
        }
    };

    typedef struct message<countdown, Countdown> countdown_msg;
}

#endif //BALLANCEMMOSERVER_COUNTDOWN_HPP
