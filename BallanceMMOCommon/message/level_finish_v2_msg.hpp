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

    struct level_finish_v2_msg: public serializable_message {
        level_finish_v2_msg(): serializable_message(bmmo::LevelFinishV2) {}

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

        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            raw.write(reinterpret_cast<const char*>(&player_id), sizeof(player_id));
            raw.write(reinterpret_cast<const char*>(&points), sizeof(points));
            raw.write(reinterpret_cast<const char*>(&lives), sizeof(lives));
            raw.write(reinterpret_cast<const char*>(&lifeBonus), sizeof(lifeBonus));
            raw.write(reinterpret_cast<const char*>(&levelBonus), sizeof(levelBonus));
            raw.write(reinterpret_cast<const char*>(&timeElapsed), sizeof(timeElapsed));
            raw.write(reinterpret_cast<const char*>(&startPoints), sizeof(startPoints));
            raw.write(reinterpret_cast<const char*>(&cheated), sizeof(cheated));
            map.serialize(raw);
            raw.write(reinterpret_cast<const char*>(&rank), sizeof(rank));
            return (raw.good());
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            raw.read(reinterpret_cast<char*>(&player_id), sizeof(player_id));
            if (!raw.good() || raw.gcount() != sizeof(player_id)) return false;

            raw.read(reinterpret_cast<char*>(&points), sizeof(points));
            if (!raw.good() || raw.gcount() != sizeof(points)) return false;

            raw.read(reinterpret_cast<char*>(&lives), sizeof(lives));
            if (!raw.good() || raw.gcount() != sizeof(lives)) return false;

            raw.read(reinterpret_cast<char*>(&lifeBonus), sizeof(lifeBonus));
            if (!raw.good() || raw.gcount() != sizeof(lifeBonus)) return false;

            raw.read(reinterpret_cast<char*>(&levelBonus), sizeof(levelBonus));
            if (!raw.good() || raw.gcount() != sizeof(levelBonus)) return false;

            raw.read(reinterpret_cast<char*>(&timeElapsed), sizeof(timeElapsed));
            if (!raw.good() || raw.gcount() != sizeof(timeElapsed)) return false;

            raw.read(reinterpret_cast<char*>(&startPoints), sizeof(startPoints));
            if (!raw.good() || raw.gcount() != sizeof(startPoints)) return false;

            raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
            if (!raw.good() || raw.gcount() != sizeof(cheated)) return false;

            if (!map.deserialize(raw)) return false;

            raw.read(reinterpret_cast<char*>(&rank), sizeof(rank));
            if (!raw.good() || raw.gcount() != sizeof(rank)) return false;

            return (raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
