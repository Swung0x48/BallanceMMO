#ifndef BALLANCEMMOSERVER_SOUND_DATA_MSG_HPP
#define BALLANCEMMOSERVER_SOUND_DATA_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    // play beep sound sequences
    struct sound_data_msg: public serializable_message {
        sound_data_msg(): serializable_message(bmmo::SoundData) {}

        std::string caption; // optional

        // <frequency/Hz, duration/ms>; 0 Hz = sleep for the specified duration
        std::vector<std::pair<uint16_t, uint32_t>> sounds;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_string(caption, raw);
            uint32_t size = sounds.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& [frequency, duration] : sounds) {
                raw.write(reinterpret_cast<const char*>(&frequency), sizeof(frequency));
                raw.write(reinterpret_cast<const char*>(&duration), sizeof(duration));
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_string(raw, caption))
                return false;
            uint32_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            sounds.resize(size);
            for (uint32_t i = 0; i < size; ++i) {
                if (!raw.good())
                    return false;
                raw.read(reinterpret_cast<char*>(&sounds[i].first), sizeof(sounds[i].first));
                if (!raw.good() || raw.gcount() != sizeof(sounds[i].first))
                    return false;
                raw.read(reinterpret_cast<char*>(&sounds[i].second), sizeof(sounds[i].second));
                if (!raw.good() || raw.gcount() != sizeof(sounds[i].second))
                    return false;
            }
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_SOUND_DATA_MSG_HPP
