#ifndef BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#include "message.hpp"
#include "session_event_msg.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // client -> server, unreliable no-delay. Up to MAX_INPUT_FRAMES ticks of
    // input starting at first_tick, most recent last.
    // docs/rooms-and-sessions-protocol.md 2.2.
    //
    // Protocol 25 (FULL step EVI, late-player-design.md §3.4 option (b)):
    // after the frames, `nevents` u8 and each piggybacked lifecycle event as
    // a u16 length and the event's whole serialized session_event_msg (its
    // opcode included), the very bytes its reliable copy carries.  The
    // sender's own Physicalize / Unphysicalize / Sector / Trafo events ride
    // every input datagram from their send until stamp + 7, so a datagram
    // that carries a stamp's frame carries its events too; the server takes
    // them through the event intake before the frames (server.cpp
    // handle_session_input).  `nevents` is required: a protocol-24 datagram
    // (it ends after the frames) is refused, as is a count over
    // MAX_INPUT_EVENTS, a length past the end, or an event that does not
    // parse as a session_event_msg.
    struct session_input_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        std::vector<session::input_frame> frames;    // <= MAX_INPUT_FRAMES
        std::vector<std::string> events;             // <= MAX_INPUT_EVENTS serialized session_event_msg

        // The serialized size of the message, without serializing it: what
        // the sender's byte budget is judged by
        // (session/event_piggyback.hpp kInputDatagramBudget).
        static constexpr size_t kFrameBytes = sizeof(uint8_t) + 9 * sizeof(float) + 2 * sizeof(uint8_t);
        static size_t wire_size(size_t nframes, size_t event_bytes, size_t nevents) {
            return sizeof(opcode) + 2 * sizeof(uint32_t) + sizeof(uint8_t) + nframes * kFrameBytes
                + sizeof(uint8_t) + nevents * sizeof(uint16_t) + event_bytes;
        }

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
            // Protocol 25: the piggybacked events (an oversized one is the
            // sender's bug; it is left out rather than cut).
            uint8_t nevents = 0;
            for (const auto& e: events)
                if (nevents < session::MAX_INPUT_EVENTS && e.size() <= UINT16_MAX) ++nevents;
            raw.write(reinterpret_cast<const char*>(&nevents), sizeof(nevents));
            uint8_t written = 0;
            for (const auto& e: events) {
                if (written >= nevents) break;
                if (e.size() > UINT16_MAX) continue;
                const auto len = static_cast<uint16_t>(e.size());
                raw.write(reinterpret_cast<const char*>(&len), sizeof(len));
                raw.write(e.data(), static_cast<std::streamsize>(e.size()));
                ++written;
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
            // Protocol 25: required (a protocol-24 datagram ends here).
            uint8_t nevents = 0;
            if (!message_utils::read_variable(raw, &nevents)) return false;
            if (nevents > session::MAX_INPUT_EVENTS) return false;
            events.clear();
            events.reserve(nevents);
            for (uint8_t i = 0; i < nevents; ++i) {
                uint16_t len = 0;
                if (!message_utils::read_variable(raw, &len)) return false;
                std::string bytes(len, '\0');
                if (len > 0 && !raw.read(bytes.data(), len)) return false;
                session_event_msg check;
                check.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!check.deserialize()) return false;
                events.push_back(std::move(bytes));
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
