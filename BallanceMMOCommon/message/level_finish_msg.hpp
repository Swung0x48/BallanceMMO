#ifndef BALLANCEMMOSERVER_LEVEL_FINISH_MSG_HPP
#define BALLANCEMMOSERVER_LEVEL_FINISH_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct level_finish {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        int points = 0;
        int lifes = 0;
        int lifeBouns = 0;
        int levelBouns = 0;
        float timeElapsed = 0.0f;

        int startPoints = 0;
        int currentLevel = 0;
        bool cheated = false;
    };

    typedef struct message<level_finish, LevelFinish> level_finish_msg;
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_MSG_HPP
