#ifndef BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#define BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
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
            return bmmo::get_formatted_time(timeElapsed);
        }
    };

    typedef struct message<level_finish_v2, LevelFinishV2> level_finish_v2_msg;
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
