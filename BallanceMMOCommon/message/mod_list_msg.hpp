#ifndef BALLANCEMMOSERVER_MOD_LIST_MSG_HPP
#define BALLANCEMMOSERVER_MOD_LIST_MSG_HPP
#include "message.hpp"
#include "map_names_msg.hpp"

namespace bmmo {
    struct mod_list_msg: public serializable_message {
        std::unordered_map<std::string, std::string> mods;

        mod_list_msg(): serializable_message(bmmo::ModList) {};

        bool serialize() {
            serializable_message::serialize();

            uint32_t size = mods.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));

            for (auto& i: mods) {
                message_utils::write_string(i.first, raw);
                message_utils::write_string(i.second, raw);
            };

            return raw.good();
        };

        bool deserialize() {
            serializable_message::deserialize();

            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            mods.reserve(size);

            for (uint32_t i = 0; i < size; i++) {
                if (!raw.good())
                    return false;

                std::string mod_name;
                std::string mod_version;
                if (!message_utils::read_string(raw, mod_name))
                    return false;
                if (!message_utils::read_string(raw, mod_version)) // check if read string successfully
                    return false;

                mods[mod_name] = mod_version;
            };

            return raw.good();
        };
    };
}

#endif //BALLANCEMMOSERVER_MOD_LIST_MSG_HPP