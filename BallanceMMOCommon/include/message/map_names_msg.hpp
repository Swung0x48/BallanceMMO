#ifndef BALLANCEMMOSERVER_MAP_NAMES_MSG_HPP
#define BALLANCEMMOSERVER_MAP_NAMES_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include <unordered_map>

namespace bmmo {
    struct map_names_msg: public serializable_message {
        // <md5_bytes, map_name>
        std::unordered_map<std::string, std::string> maps;

        constexpr static auto HASH_SIZE = sizeof(bmmo::map::md5);

        map_names_msg(): serializable_message(bmmo::MapNames) {};

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = maps.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));

            for (auto& i: maps) {
                raw.write(i.first.c_str(), HASH_SIZE);
                message_utils::write_string(i.second, raw);
            };

            return raw.good();
        };

        bool deserialize() {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            maps.reserve(size);

            for (uint32_t i = 0; i < size; i++) {
                if (!raw.good())
                    return false;

                std::string md5_bytes(HASH_SIZE, '\0');
                std::string name;
                raw.read(md5_bytes.data(), HASH_SIZE);
                if (!raw.good() || raw.gcount() != HASH_SIZE)
                    return false;
                if (!message_utils::read_string(raw, name)) // check if read string successfully
                    return false;

                maps[md5_bytes] = name;
            };

            return raw.good();
        };
    };
};

#endif // BALLANCEMMOSERVER_MAP_NAMES_MSG_HPP