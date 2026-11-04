#ifndef BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
#include "message.hpp"
#include "session_event_msg.hpp"
#include "../entity/session.hpp"
#include "../session/camera_heading.hpp"

#include <optional>

namespace bmmo {
    // client -> server, unreliable no-delay. Up to MAX_INPUT_FRAMES ticks of
    // input starting at first_tick, most recent last.
    // docs/rooms-and-sessions-protocol.md 2.2.
    //
    // Layout (protocol 30): session u32, first_tick u32, relay_ack u32, count
    // u8 (bit 7: a base follows), the base when attached (since u32 and
    // cam_right/cam_up/cam_dir f32x3), per frame: keys u8, ball_type u8 with
    // the frame's heading code in bits 5-7, flags u8, and the three rows when
    // the code is kHeadingInline.  A code is a quarter turn of the sender's
    // camera base in force at the frame's tick, the zero rows, or inline:
    // then the rows are the base from that tick on (session/camera_heading.hpp,
    // docs/relay-protocol-design.md 4.7).  The base rides every datagram
    // whose frames need it until the relay header's `acked` (the newest of
    // our frames the server applied) reaches its tick: the first datagram
    // the server got with a frame that late carried it.  relay_ack is the
    // newest tick for which the client holds every other member's relayed
    // frame of every tick up to it: what the server's relay to it starts
    // after (docs/relay-protocol-design.md 4.1).
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
        // Per frame on the wire; empty: every camera goes inline (the zero
        // rows as kHeadingNone).  A decoded message's turned frames carry no
        // rows until resolved_frames.
        std::vector<uint8_t> headings;
        std::optional<session::camera_bases::base> base;
        std::vector<std::string> events;             // <= MAX_INPUT_EVENTS serialized session_event_msg

        static constexpr uint8_t COUNT_BASE = 0x80;

        // The serialized size of the message, without serializing it: what
        // the sender's byte budget is judged by
        // (session/event_piggyback.hpp kInputDatagramBudget).
        size_t wire_size(size_t event_bytes, size_t nevents) const {
            size_t bytes = sizeof(opcode) + 3 * sizeof(uint32_t) + sizeof(uint8_t)
                         + (base ? session::kCameraBaseBytes : 0);
            const size_t count = std::min<size_t>(frames.size(), session::MAX_INPUT_FRAMES);
            for (size_t i = 0; i < count; ++i)
                bytes += 3 + (heading_at(i) == session::kHeadingInline ? sizeof(session::camera_rows) : 0);
            return bytes + sizeof(uint8_t) + nevents * sizeof(uint16_t) + event_bytes;
        }

        // The server's half: the attached base and the inline frames into the
        // sender's `bases`, and the frames with every camera filled.  Empty
        // when a frame is a turn of a base the server does not hold (the
        // datagram is then not used).
        std::optional<std::vector<session::input_frame>> resolved_frames(session::camera_bases& bases) const {
            if (base) bases.set(base->since, base->rows);
            std::vector<session::input_frame> out = frames;
            for (size_t i = 0; i < out.size(); ++i) {
                const uint32_t tick = first_tick + static_cast<uint32_t>(i);
                const uint8_t code = heading_at(i);
                if (code == session::kHeadingInline) {
                    bases.set(tick, session::camera_of(out[i]));
                    continue;
                }
                session::camera_rows rows;
                if (!bases.decode(tick, code, rows)) return std::nullopt;
                session::set_camera(out[i], rows);
            }
            return out;
        }

        session_input_msg() : serializable_message(bmmo::SessionInput) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&first_tick), sizeof(first_tick));
            raw.write(reinterpret_cast<const char*>(&relay_ack), sizeof(relay_ack));

            const uint8_t count = static_cast<uint8_t>(std::min<size_t>(frames.size(), session::MAX_INPUT_FRAMES));
            const uint8_t count_byte = static_cast<uint8_t>(count | (base ? COUNT_BASE : 0));
            raw.write(reinterpret_cast<const char*>(&count_byte), sizeof(count_byte));
            if (base) {
                raw.write(reinterpret_cast<const char*>(&base->since), sizeof(base->since));
                raw.write(reinterpret_cast<const char*>(base->rows.data()), sizeof(base->rows));
            }
            for (uint8_t i = 0; i < count; ++i) {
                const auto& f = frames[i];
                const uint8_t code = heading_at(i);
                const uint8_t type = static_cast<uint8_t>((f.ball_type & session::kBallTypeMask) | (code << session::kHeadingShift));
                raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
                raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
                if (code != session::kHeadingInline) continue;
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

            uint8_t count_byte = 0;
            if (!message_utils::read_variable(raw, &count_byte)) return false;
            const uint8_t count = static_cast<uint8_t>(count_byte & ~COUNT_BASE);
            if (count > session::MAX_INPUT_FRAMES) return false;
            base.reset();
            if (count_byte & COUNT_BASE) {
                session::camera_bases::base b;
                if (!message_utils::read_variable(raw, &b.since)) return false;
                if (!raw.read(reinterpret_cast<char*>(b.rows.data()), sizeof(b.rows))) return false;
                base = b;
            }
            frames.clear();
            frames.reserve(count);
            headings.clear();
            headings.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                session::input_frame f;
                uint8_t type = 0;
                if (!message_utils::read_variable(raw, &f.keys)) return false;
                if (!message_utils::read_variable(raw, &type)) return false;
                if (!message_utils::read_variable(raw, &f.flags)) return false;
                const uint8_t code = static_cast<uint8_t>(type >> session::kHeadingShift);
                if (!session::valid_heading(code)) return false;
                f.ball_type = static_cast<uint8_t>(type & session::kBallTypeMask);
                if (code == session::kHeadingInline) {
                    if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                    if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                    if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
                }
                frames.push_back(f);
                headings.push_back(code);
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
        uint8_t heading_at(size_t i) const {
            if (i < headings.size()) return headings[i];
            return session::zero_camera(session::camera_of(frames[i])) ? session::kHeadingNone : session::kHeadingInline;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_INPUT_MSG_HPP
