#ifndef BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // client -> server, unreliable no-delay. Up to MAX_INPUT_FRAMES ticks of
    // input starting at first_tick, most recent last.
    // docs/rooms-and-sessions-protocol.md 2.2.
    struct session_input_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        std::vector<session::input_frame> frames;    // <= MAX_INPUT_FRAMES

        session_input_msg() : serializable_message(bmmo::SessionInput) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));

            const uint8_t count = static_cast<uint8_t>(std::min<size_t>(frames.size(), session::MAX_INPUT_FRAMES));
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (uint8_t i = 0; i < count; ++i) {
                const auto& f = frames[i];
                raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                raw.write(reinterpret_cast<const char*>(f.cam_right), sizeof(f.cam_right));
                raw.write(reinterpret_cast<const char*>(f.cam_up), sizeof(f.cam_up));
                raw.write(reinterpret_cast<const char*>(f.cam_dir), sizeof(f.cam_dir));
                raw.write(reinterpret_cast<const char*>(&f.ball_type), sizeof(f.ball_type));
                raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &first_tick)) return false;

            uint8_t count = 0;
            if (!message_utils::read_variable(raw, &count)) return false;
            if (count > session::MAX_INPUT_FRAMES) return false;
            frames.clear();
            frames.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                session::input_frame f;
                if (!message_utils::read_variable(raw, &f.keys)) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
                if (!message_utils::read_variable(raw, &f.ball_type)) return false;
                if (!message_utils::read_variable(raw, &f.flags)) return false;
                frames.push_back(f);
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
