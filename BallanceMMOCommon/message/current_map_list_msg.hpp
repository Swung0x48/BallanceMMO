#ifndef BALLANCEMMOSERVER_CURRENT_MAP_LIST_MSG_HPP
#define BALLANCEMMOSERVER_CURRENT_MAP_LIST_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include "current_map_msg.hpp"

namespace bmmo {
    struct current_map_list_msg: public serializable_message {
        std::vector<current_map_state> states;

        current_map_list_msg(): serializable_message(CurrentMapList) {}

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = states.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& i: states) {
                raw.write(reinterpret_cast<const char*>(&i), sizeof(i));
            }

            return raw.good();
        }

        bool deserialize() {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            states.resize(size);
            for (uint32_t i = 0; i < size; ++i) {
                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&states[i]), sizeof(current_map_state));
                if (!raw.good() || raw.gcount() != sizeof(current_map_state))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_CURRENT_MAP_LIST_MSG_HPP
