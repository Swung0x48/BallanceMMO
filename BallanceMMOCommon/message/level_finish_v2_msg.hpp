#ifndef BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#define BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    struct level_finish_v2_msg: public serializable_message {
        level_finish_v2_msg(): serializable_message(bmmo::LevelFinishV2) {}

        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        int points = 0;
        int lifes = 0;
        int lifeBonus = 0;
        int levelBonus = 0;
        float timeElapsed = 0.0f;

        int startPoints = 0;
        int currentLevel = 0;
        bool cheated = false;

        std::string map_name = "";
        map_type type = UnknownType;
        uint8_t md5[16];

        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            raw.write(reinterpret_cast<const char*>(&player_id), sizeof(player_id));
            raw.write(reinterpret_cast<const char*>(&points), sizeof(points));
            raw.write(reinterpret_cast<const char*>(&lifes), sizeof(lifes));
            raw.write(reinterpret_cast<const char*>(&lifeBonus), sizeof(lifeBonus));
            raw.write(reinterpret_cast<const char*>(&levelBonus), sizeof(levelBonus));
            raw.write(reinterpret_cast<const char*>(&timeElapsed), sizeof(timeElapsed));
            raw.write(reinterpret_cast<const char*>(&startPoints), sizeof(startPoints));
            raw.write(reinterpret_cast<const char*>(&currentLevel), sizeof(currentLevel));
            raw.write(reinterpret_cast<const char*>(&cheated), sizeof(cheated));
            message_utils::write_string(map_name, raw);
            raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
            raw.write(reinterpret_cast<const char*>(md5), sizeof(uint8_t) * 16);
            return (raw.good());
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            raw.read(reinterpret_cast<char*>(&player_id), sizeof(player_id));
            if (!raw.good() || raw.gcount() != sizeof(player_id)) return false;

            raw.read(reinterpret_cast<char*>(&points), sizeof(points));
            if (!raw.good() || raw.gcount() != sizeof(points)) return false;

            raw.read(reinterpret_cast<char*>(&lifes), sizeof(lifes));
            if (!raw.good() || raw.gcount() != sizeof(lifes)) return false;

            raw.read(reinterpret_cast<char*>(&lifeBonus), sizeof(lifeBonus));
            if (!raw.good() || raw.gcount() != sizeof(lifeBonus)) return false;

            raw.read(reinterpret_cast<char*>(&levelBonus), sizeof(levelBonus));
            if (!raw.good() || raw.gcount() != sizeof(levelBonus)) return false;

            raw.read(reinterpret_cast<char*>(&timeElapsed), sizeof(timeElapsed));
            if (!raw.good() || raw.gcount() != sizeof(timeElapsed)) return false;

            raw.read(reinterpret_cast<char*>(&startPoints), sizeof(startPoints));
            if (!raw.good() || raw.gcount() != sizeof(startPoints)) return false;

            raw.read(reinterpret_cast<char*>(&currentLevel), sizeof(currentLevel));
            if (!raw.good() || raw.gcount() != sizeof(currentLevel)) return false;

            raw.read(reinterpret_cast<char*>(&cheated), sizeof(cheated));
            if (!raw.good() || raw.gcount() != sizeof(cheated)) return false;

            if (!message_utils::read_string(raw, map_name))
                return false;

            raw.read(reinterpret_cast<char*>(&type), sizeof(type));
            if (!raw.good() || raw.gcount () != sizeof(type)) return false;

            raw.read(reinterpret_cast<char*>(md5), sizeof(uint8_t) * 16);
            if (!raw.good() || raw.gcount() != sizeof(uint8_t) * 16) return false;

            return (raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_LEVEL_FINISH_V2_MSG_HPP
