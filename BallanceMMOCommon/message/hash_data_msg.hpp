#ifndef BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP
#define BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include <cstdint>

namespace bmmo {
    struct hash_data_msg: public serializable_message {
        std::string data_name;
        uint8_t md5[16];

        hash_data_msg(): serializable_message(bmmo::HashData) {};

        bool is_same_data(std::string hash_string) {
            uint8_t md5[16];
            if (hash_string.size() != 32)
                return false;
            hex_chars_from_string(md5, hash_string);
            // for (int i = 0; i < 16; i++) {
            //     md5[i] = (uint8_t)hash_string[i * 2];
            //     md5[i] += (uint8_t)hash_string[i * 2 + 1] << 4;
            // }
            return memcmp(md5, this->md5, 16) == 0;
        };

        bool serialize() {
            serializable_message::serialize();

            message_utils::write_string(data_name, raw);
            raw.write(reinterpret_cast<const char*>(md5), 16);

            return raw.good();
        };

        bool deserialize() {
            serializable_message::deserialize();

            if (!message_utils::read_string(raw, data_name))
                return false;
            raw.read(reinterpret_cast<char*>(md5), sizeof(uint8_t) * 16);
            if (!raw.good() || raw.gcount() != sizeof(uint8_t) * 16)
                return false;

            return raw.good();
        };
    };
};

#endif // BALLANCEMMO_SERVER_HASH_DATA_MSG_HPP