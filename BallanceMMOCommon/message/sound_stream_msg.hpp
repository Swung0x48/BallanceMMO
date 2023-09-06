#ifndef BALLANCEMMOSERVER_SOUND_STREAM_MSG_HPP
#define BALLANCEMMOSERVER_SOUND_STREAM_MSG_HPP
#include <sstream>
#include <fstream>
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct sound_stream_msg: public serializable_message {
        sound_stream_msg(): serializable_message(bmmo::SoundStream) {}

        enum class sound_type: uint8_t { Wave, /* Midi (WIP) */ };

        std::string caption; // optional
        sound_type type{};
        bool save_to_pwd = false;
        uint32_t duration_ms = 0; // in milliseconds
        float gain = 1.0f;
        float pitch = 1.0f;

        std::string path;

        static constexpr auto get_max_stream_size() {
            return sizeof(general_message) - sizeof(sound_stream_msg);
        }

        std::string get_extension() {
            using st = sound_type;
            switch (type) {
                // case st::Midi:
                //     return ".mid";
                case st::Wave:
                    return ".wav";
                default:
                    return "";
            }
        }

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_string(caption, raw);
            message_utils::write_variable(&type, raw);
            message_utils::write_variable(&duration_ms, raw);
            gain = std::clamp(0.0f, gain, 1.0f);
            pitch = std::clamp(0.5f, pitch, 2.0f);
            message_utils::write_variable(&gain, raw);
            message_utils::write_variable(&pitch, raw);

            std::ifstream ifile(path, std::ios::binary | std::ios::ate);
            if (!ifile.is_open()) return false;
            auto size = (uint64_t) ifile.tellg();
            if (size > get_max_stream_size())
                return false;

            ifile.seekg(std::ios::beg);
            raw << ifile.rdbuf();

            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_string(raw, caption))
                return false;
            if (!message_utils::read_variable(raw, &type))
                return false;
            if (!message_utils::read_variable(raw, &duration_ms))
                return false;
            if (!message_utils::read_variable(raw, &gain))
                return false;
            if (!message_utils::read_variable(raw, &pitch))
                return false;

            path = "BMMO_" + std::to_string(SteamNetworkingUtils()->GetLocalTimestamp()) + get_extension();
            if (!save_to_pwd) path = "..\\ModLoader\\Cache\\" + path;
            std::ofstream ofile(path, std::ios::binary);
            ofile << raw.rdbuf();

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_SOUND_STREAM_MSG_HPP
