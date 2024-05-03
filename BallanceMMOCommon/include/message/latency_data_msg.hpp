#ifndef BALLANCEMMOSERVER_LATENCY_DATA_MSG_HPP
#define BALLANCEMMOSERVER_LATENCY_DATA_MSG_HPP
#include <cstdint>
#include <steam/steamnetworkingtypes.h>
#include "message.hpp"

namespace bmmo {
    struct latency_data_msg: public serializable_message {
        // <id, ping_ms>
        std::unordered_map<HSteamNetConnection, uint16_t> data;

        latency_data_msg(): serializable_message(bmmo::LatencyData) {};

        bool serialize() {
            serializable_message::serialize();

            auto size = (uint16_t) data.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));

            for (auto& [id, ping]: data) {
                message_utils::write_variable(&id, raw);
                message_utils::write_variable(&ping, raw);
            };

            return raw.good();
        };

        bool deserialize() {
            serializable_message::deserialize();

            uint16_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            data.reserve(size);

            for (uint16_t i = 0; i < size; i++) {
                if (!raw.good())
                    return false;

                HSteamNetConnection id;
                uint16_t ping;
                if (!message_utils::read_variable(raw, &id))
                    return false;
                if (!message_utils::read_variable(raw, &ping)) // check if read string successfully
                    return false;

                data[id] = ping;
            };

            return raw.good();
        };
    };
}

#endif //BALLANCEMMOSERVER_LATENCY_DATA_MSG_HPP