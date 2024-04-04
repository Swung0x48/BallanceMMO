#ifndef BALLANCEMMOSERVER_EXTRA_LIFE_MSG_HPP
#define BALLANCEMMOSERVER_EXTRA_LIFE_MSG_HPP
#include <unordered_map>
#include "../entity/map.hpp"
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct extra_life_msg: public serializable_message {
        std::unordered_map<std::string, int> life_count_goals;

        constexpr static auto HASH_SIZE = sizeof(bmmo::map::md5);

        extra_life_msg(): serializable_message(bmmo::ExtraLife) {}

        bool serialize() override {
            serializable_message::serialize();

            uint32_t size = life_count_goals.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& [hash, goal]: life_count_goals) {
                raw.write(hash.c_str(), HASH_SIZE);
                raw.write(reinterpret_cast<const char*>(&goal), sizeof(goal));
            }

            return raw.good();
        }
        
        bool deserialize() override {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!raw.good()) return false;
            life_count_goals.reserve(size);
            for (uint32_t i = 0; i < size; ++i) {
                std::string hash(HASH_SIZE, 0);
                raw.read(hash.data(), HASH_SIZE);
                if (!raw.good() || raw.gcount() != HASH_SIZE)
                    return false;
                int goal;
                raw.read(reinterpret_cast<char*>(&goal), sizeof(goal));
                if (!raw.good() || raw.gcount() != sizeof(goal))
                    return false;
                life_count_goals.try_emplace(hash, goal);
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_EXTRA_LIFE_MSG_HPP