#ifndef BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#define BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    std::string get_ordinal_rank(uint32_t rank) {
        if ((rank / 10) % 10 != 1) {
            switch (rank % 10) {
                case 1: return "st";
                case 2: return "nd";
                case 3: return "rd";
            }
        }
        return "th";
    };

    struct level_finish_v2 {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        int points = 0;
        int lives = 0;
        int lifeBonus = 0;
        int levelBonus = 0;
        float timeElapsed = 0.0f;

        int startPoints = 0;
        bool cheated = false;

        struct map map;
        int rank = 0;
    };

    typedef struct message<level_finish_v2, LevelFinishV2> level_finish_v2_msg;
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
