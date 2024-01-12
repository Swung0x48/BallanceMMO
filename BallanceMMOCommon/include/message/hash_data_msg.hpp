#ifndef BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP
#define BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include <cstdint>
#include <unordered_map>
#include <fstream>
#include <array>

namespace bmmo {
    constexpr const char* HASHES_TO_CHECK[][2] = {
        {"3D Entities\\Balls.nmo", "fb29d77e63aad08499ce38d36266ec33"},
        {"BuildingBlocks\\Logics.dll", "ce3c3f6e4bcf8527d9a6207d8e3e9dab"},
        {"Managers\\ParameterOperations.dll", "0219ec8f74215cfca083b5020f8c48e9"},
    };

    struct hash_data_msg: public serializable_message {
        // std::string data_name;
        // uint8_t md5[16];
        std::unordered_map<std::string, std::array<uint8_t, 16>> data;

        hash_data_msg(): serializable_message(bmmo::HashData) {};

        bool is_same_data(std::string name, std::string hash_string) {
            decltype(data)::mapped_type md5;
            if (hash_string.size() != 2 * md5.size())
                return false;
            auto hash_it = data.find(name);
            if (hash_it == data.end())
                return false;
            hex_chars_from_string(md5.data(), hash_string);
            // for (int i = 0; i < 16; i++) {
            //     md5[i] = (uint8_t)hash_string[i * 2];
            //     md5[i] += (uint8_t)hash_string[i * 2 + 1] << 4;
            // }
            return memcmp(md5.data(), hash_it->second.data(), md5.size()) == 0;
        };

        // without the length to preserve compatibility
        bool serialize() {
            serializable_message::serialize();

            for (const auto& [name, md5]: data) {
                message_utils::write_string(name, raw);
                raw.write(reinterpret_cast<const char*>(md5.data()), md5.size());
            }

            return raw.good();
        };

        bool deserialize() {
            serializable_message::deserialize();

            while (raw.good() && raw.peek() != std::ifstream::traits_type::eof()) {
                std::string data_name;
                if (!message_utils::read_string(raw, data_name))
                    return false;
                auto& md5 = data[data_name];
                raw.read(reinterpret_cast<char*>(md5.data()), md5.size());
                if (!raw.good() || raw.gcount() != (std::streamsize) md5.size())
                    return false;
            }

            return raw.good();
        };
    };
};

#endif // BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP