#ifndef BALLANCEMMO_SERVER_MAP_NAMES_MSG_HPP
#define BALLANCEMMO_SERVER_MAP_NAMES_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include <unordered_map>

namespace bmmo {
    struct map_names_msg: public serializable_message {
        // <md5_bytes, map_name>
        std::unordered_map<std::string, std::string> maps;

        map_names_msg(): serializable_message(bmmo::MapNames) {};

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = maps.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));

            for (auto& i: maps) {
                raw.write(i.first.c_str(), 16);
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

                std::string md5_bytes;
                std::string name;
                md5_bytes.resize(16);
                raw.read(md5_bytes.data(), 16);
                if (!raw.good() || raw.gcount() != 16)
                    return false;
                if (!message_utils::read_string(raw, name)) // check if read string successfully
                    return false;

                maps[md5_bytes] = name;
            };

            return raw.good();
        };
    };
};

#endif // BALLANCEMMO_SERVER_MAP_NAMES_MSG_HPP