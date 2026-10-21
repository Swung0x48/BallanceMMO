#ifndef BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"
#include "../session/input_relay.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bmmo {
    // server -> client, one per simulated tick, UNRELIABLE no-delay: for every
    // *other* member present at that tick, the input frames the server
    // actually applied for it (fresh or repeated) at the last `window` ticks
    // ending at `tick` - a redundancy window, like the uplink's, so a lost
    // datagram costs nothing unless `window` in a row are lost, and nothing
    // ever waits behind a loss (session/input_relay.hpp; protocol 5).  A
    // client drives its mirrored remote balls through the same navigation code
    // as the server (design 9.1) and lands every frame by its tick.
    //
    // Layout: session u32, tick u32 (the newest), count u8; per entry: player
    // u32, nframes u8 (1..MAX_RELAY_FRAMES, the ticks tick-nframes+1..tick,
    // oldest first); per frame: header u8, keys u8, ball_type u8, flags u8 and,
    // when the header has FRAME_CAMERA, cam_right/cam_up/cam_dir f32x3.  The
    // first frame of an entry always carries the camera; a later one omits it
    // when its three vectors equal the previous frame's bit for bit, and the
    // reader copies them.  So every datagram stands alone, and a frame whose
    // camera did not move is 4 bytes, not 40.
    struct session_remote_input_msg : public serializable_message {
        struct entry {
            uint32_t player = 0;
            std::vector<session::input_frame> frames;   // <= MAX_RELAY_FRAMES, oldest first, ending at tick
        };
        static constexpr uint8_t FRAME_CAMERA = 1;
        static_assert(sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1 == session::kRelayDatagramHeader,
                      "the relay's byte budget counts this header");

        uint32_t session = 0;
        uint32_t tick = 0;                 // the newest tick in the datagram
        std::vector<entry> entries;        // <= MAX_PLAYERS_PER_SESSION

        session_remote_input_msg() : serializable_message(bmmo::SessionRemoteInput) {}

        // The tick of frame `index` of `e`.
        uint32_t frame_tick(const entry& e, size_t index) const {
            return tick - static_cast<uint32_t>(e.frames.size()) + 1 + static_cast<uint32_t>(index);
        }

        static bool same_camera(const session::input_frame& a, const session::input_frame& b) {
            return session::same_camera(a, b);
        }

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            // An entry without frames has nothing to say and is not written.
            std::vector<const entry*> written;
            for (const auto& e: entries)
                if (!e.frames.empty() && written.size() < session::MAX_PLAYERS_PER_SESSION) written.push_back(&e);
            const uint8_t count = static_cast<uint8_t>(written.size());
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (const entry* e: written) {
                raw.write(reinterpret_cast<const char*>(&e->player), sizeof(e->player));
                // More frames than the cap: the newest ones are what matter.
                const size_t total = e->frames.size();
                const size_t first = total > session::MAX_RELAY_FRAMES ? total - session::MAX_RELAY_FRAMES : 0;
                const uint8_t nframes = static_cast<uint8_t>(total - first);
                raw.write(reinterpret_cast<const char*>(&nframes), sizeof(nframes));
                for (size_t i = first; i < total; ++i) {
                    const auto& f = e->frames[i];
                    const bool camera = i == first || !same_camera(e->frames[i - 1], f);
                    const uint8_t header = camera ? FRAME_CAMERA : 0;
                    raw.write(reinterpret_cast<const char*>(&header), sizeof(header));
                    raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                    raw.write(reinterpret_cast<const char*>(&f.ball_type), sizeof(f.ball_type));
                    raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
                    if (camera) {
                        raw.write(reinterpret_cast<const char*>(f.cam_right), sizeof(f.cam_right));
                        raw.write(reinterpret_cast<const char*>(f.cam_up), sizeof(f.cam_up));
                        raw.write(reinterpret_cast<const char*>(f.cam_dir), sizeof(f.cam_dir));
                    }
                }
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
                uint8_t nframes = 0;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &nframes)) return false;
                if (nframes == 0 || nframes > session::MAX_RELAY_FRAMES) return false;
                if (static_cast<uint32_t>(nframes) - 1u > tick) return false;   // a frame before tick 0
                e.frames.reserve(nframes);
                for (uint8_t k = 0; k < nframes; ++k) {
                    session::input_frame f;
                    uint8_t header = 0;
                    if (!message_utils::read_variable(raw, &header)) return false;
                    if (header & ~FRAME_CAMERA) return false;              // a bit this reader does not know
                    if (k == 0 && !(header & FRAME_CAMERA)) return false;  // the first frame always carries the camera
                    if (!message_utils::read_variable(raw, &f.keys)) return false;
                    if (!message_utils::read_variable(raw, &f.ball_type)) return false;
                    if (!message_utils::read_variable(raw, &f.flags)) return false;
                    if (header & FRAME_CAMERA) {
                        if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                        if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                        if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
                    } else {
                        const auto& previous = e.frames.back();
                        std::memcpy(f.cam_right, previous.cam_right, sizeof(f.cam_right));
                        std::memcpy(f.cam_up, previous.cam_up, sizeof(f.cam_up));
                        std::memcpy(f.cam_dir, previous.cam_dir, sizeof(f.cam_dir));
                    }
                    e.frames.push_back(f);
                }
                entries.push_back(std::move(e));
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
