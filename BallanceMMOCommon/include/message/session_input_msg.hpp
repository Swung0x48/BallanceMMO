#ifndef BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#include "message.hpp"
#include "session_event_msg.hpp"
#include "../entity/session.hpp"
#include "../session/input_relay.hpp"

namespace bmmo {
    // client -> server, unreliable no-delay. Up to MAX_INPUT_FRAMES ticks of
    // input starting at first_tick, most recent last.
    // docs/rooms-and-sessions-protocol.md 2.2.
    //
    // Layout (protocol 29): session u32, first_tick u32, relay_ack u32, count
    // u8, per frame: header u8, keys u8, ball_type u8, flags u8 and, when the
    // header has FRAME_CAMERA, cam_right/cam_up/cam_dir f32x3.  The first
    // frame always carries the camera; a later one omits it when its rows
    // equal the frame before's bit for bit.  relay_ack is the newest tick for
    // which the client holds every other member's relayed frame of every
    // tick up to it: what the server's relay to it starts after
    // (docs/relay-protocol-design.md 4.1).
    //
    // Protocol 25 (FULL step EVI, late-player-design.md §3.4 option (b)):
    // after the frames, `nevents` u8 and each piggybacked lifecycle event as
    // a u16 length and the event's whole serialized session_event_msg (its
    // opcode included), the very bytes its reliable copy carries.  The
    // sender's own Physicalize / Unphysicalize / Sector / Trafo events ride
    // every input datagram from their send until stamp + 7, so a datagram
    // that carries a stamp's frame carries its events too; the server takes
    // them through the event intake before the frames (server.cpp
    // handle_session_input).  `nevents` is required: a count over
    // MAX_INPUT_EVENTS, a length past the end, or an event that does not
    // parse as a session_event_msg is refused.
    struct session_input_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t first_tick = 0;
        uint32_t relay_ack = 0;
        std::vector<session::input_frame> frames;    // <= MAX_INPUT_FRAMES
        std::vector<std::string> events;             // <= MAX_INPUT_EVENTS serialized session_event_msg

        static constexpr uint8_t FRAME_CAMERA = 1;

        // The serialized size of the message, without serializing it: what
        // the sender's byte budget is judged by
        // (session/event_piggyback.hpp kInputDatagramBudget).
        static size_t wire_size(const std::vector<session::input_frame>& frames, size_t event_bytes, size_t nevents) {
            size_t bytes = sizeof(opcode) + 3 * sizeof(uint32_t) + sizeof(uint8_t);
            const size_t count = std::min<size_t>(frames.size(), session::MAX_INPUT_FRAMES);
            for (size_t i = 0; i < count; ++i) bytes += 4 + (camera_at(frames, i) ? 9 * sizeof(float) : 0);
            return bytes + sizeof(uint8_t) + nevents * sizeof(uint16_t) + event_bytes;
        }

        session_input_msg() : serializable_message(bmmo::SessionInput) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&relay_ack), sizeof(relay_ack));

            const uint8_t count = static_cast<uint8_t>(std::min<size_t>(frames.size(), session::MAX_INPUT_FRAMES));
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (uint8_t i = 0; i < count; ++i) {
                const auto& f = frames[i];
                const bool camera = camera_at(frames, i);
                const uint8_t header = camera ? FRAME_CAMERA : 0;
                raw.write(reinterpret_cast<const char*>(&header), sizeof(header));
                raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                raw.write(reinterpret_cast<const char*>(&f.ball_type), sizeof(f.ball_type));
                raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
                if (!camera) continue;
                raw.write(reinterpret_cast<const char*>(f.cam_right), sizeof(f.cam_right));
                raw.write(reinterpret_cast<const char*>(f.cam_up), sizeof(f.cam_up));
                raw.write(reinterpret_cast<const char*>(f.cam_dir), sizeof(f.cam_dir));
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
            if (!message_utils::read_variable(raw, &relay_ack)) return false;

            uint8_t count = 0;
            if (!message_utils::read_variable(raw, &count)) return false;
            if (count > session::MAX_INPUT_FRAMES) return false;
            frames.clear();
            frames.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                session::input_frame f;
                uint8_t header = 0;
                if (!message_utils::read_variable(raw, &header)) return false;
                if (header & ~FRAME_CAMERA) return false;                 // a bit this reader does not know
                if (i == 0 && !(header & FRAME_CAMERA)) return false;     // the first frame always carries the camera
                if (!message_utils::read_variable(raw, &f.keys)) return false;
                if (!message_utils::read_variable(raw, &f.ball_type)) return false;
                if (!message_utils::read_variable(raw, &f.flags)) return false;
                if (header & FRAME_CAMERA) {
                    if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                    if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                    if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
                } else {
                    session::set_camera(f, session::camera_of(frames.back()));
                }
                frames.push_back(f);
            }
            // Protocol 25: required.
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

    private:
        // Whether frame i carries its camera on the wire.
        static bool camera_at(const std::vector<session::input_frame>& frames, size_t i) {
            return i == 0 || !session::same_camera(frames[i - 1], frames[i]);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
