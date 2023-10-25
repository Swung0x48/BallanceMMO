#ifndef BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#define BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    std::string get_ordinal_suffix(uint32_t n) {
        if ((n / 10) % 10 != 1) {
            switch (n % 10) {
                case 1: return "st";
                case 2: return "nd";
                case 3: return "rd";
            }
        }
        return "th";
    };

    struct level_finish_v2 {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        int32_t points = 0;
        int32_t lives = 0;
        int32_t lifeBonus = 0;
        int32_t levelBonus = 0;
        float timeElapsed = 0.0f;

        int32_t startPoints = 0;
        bool cheated = false;
        level_mode mode = level_mode::Speedrun;

        struct map map;
        int32_t rank = 0;

        std::string get_formatted_score() {
            int score = points + lives * lifeBonus;
            if (map.is_original_level()) {
                score += levelBonus;
                return std::to_string(score);
            }
            std::string text(64, 0);
            text.resize(std::snprintf(text.data(), text.size(), "%d [%d]", score, lives));
            return text;
        }

        std::string get_formatted_time() {
            int total = int(timeElapsed);
            int minutes = total / 60;
            int seconds = total % 60;
            int hours = minutes / 60;
            minutes = minutes % 60;
            int ms = int((timeElapsed - total) * 1000);
            std::string text(64, 0);
            text.resize(std::snprintf(text.data(), text.size(),
                "%02d:%02d:%02d.%03d", hours, minutes, seconds, ms));
            return text;
        }
    };

    typedef struct message<level_finish_v2, LevelFinishV2> level_finish_v2_msg;
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
