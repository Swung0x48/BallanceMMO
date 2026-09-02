#ifndef BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

#include <algorithm>
#include <vector>

namespace bmmo {
    // server -> client, one per simulated tick, unreliable: the input frame
    // the server actually applied for every *other* member at that tick
    // (fresh or repeated), so a client can drive its mirrored remote balls
    // through the same navigation code as the server (design 9.1).
    struct session_remote_input_msg : public serializable_message {
        struct entry {
            uint32_t player = 0;
            session::input_frame frame{};
        };
        uint32_t session = 0;
        uint32_t tick = 0;
        std::vector<entry> entries;    // <= MAX_PLAYERS_PER_SESSION

        session_remote_input_msg() : serializable_message(bmmo::SessionRemoteInput) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            const uint8_t count = static_cast<uint8_t>(std::min<size_t>(entries.size(), session::MAX_PLAYERS_PER_SESSION));
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (uint8_t i = 0; i < count; ++i) {
                const auto& e = entries[i];
                const auto& f = e.frame;
                raw.write(reinterpret_cast<const char*>(&e.player), sizeof(e.player));
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
            if (!message_utils::read_variable(raw, &tick)) return false;
            uint8_t count = 0;
            if (!message_utils::read_variable(raw, &count)) return false;
            if (count > session::MAX_PLAYERS_PER_SESSION) return false;
            entries.clear();
            entries.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                entry e;
                auto& f = e.frame;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &f.keys)) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
                if (!message_utils::read_variable(raw, &f.ball_type)) return false;
                if (!message_utils::read_variable(raw, &f.flags)) return false;
                entries.push_back(e);
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
