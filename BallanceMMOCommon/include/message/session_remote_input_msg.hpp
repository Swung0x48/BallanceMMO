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
    // Layout: session u32, tick u32 (the newest), count u8, acked u32,
    // held_total u32, epoch u32, wait u8 (protocol 13); per entry: player u32, nframes u8
    // (1..MAX_RELAY_FRAMES, the ticks tick-nframes+1..tick, oldest first); per
    // frame: header u8, keys u8, ball_type u8, flags u8 and, when the header
    // has FRAME_CAMERA, cam_right/cam_up/cam_dir f32x3.  The first frame of an
    // entry always carries the camera; a later one omits it when its three
    // vectors equal the previous frame's bit for bit, and the reader copies
    // them.  So every datagram stands alone, and a frame whose camera did not
    // move is 4 bytes, not 40.
    //
    // Protocol 7 (late-player design 1.1): the header's three new fields are
    // the RECIPIENT's - the last tick the server applied a fresh frame of
    // its own, how many of its ticks at or after `epoch` the server
    // substituted, and `epoch`, the base of its last SessionAssign - and the
    // held section trails the entries: nheld u8 (0..kHeldNoticeTicks), then
    // per substituted tick T of the recipient: back u8 (bits 0-4 tick - T,
    // bit 7 an inline frame follows, bits 5-6 zero) and either source u32 (the
    // tick whose frame as the client sent it the server applied at T, or
    // kHeldSourceDefault for the default frame) or the frame in the encoding
    // above with the camera always present.  The section is always there,
    // nheld 0 when there is nothing to say, and nothing may follow it: a v6
    // datagram, which ends with its entries, does not parse.
    //
    // Protocol 13 (step R12, late-player design 2.5): the header ends with
    // wait u8, after epoch - the RECIPIENT's wait at the server
    // (lateness.hpp server_wait_meter: the largest of its last 240 ticks'
    // steps, whole ticks), how much later a notice of its substituted tick
    // leaves than the datagrams it samples its header lag from.  Its client
    // sizes its rollback reach with it (rollback.hpp reach_meter).
    struct session_remote_input_msg : public serializable_message {
        struct entry {
            uint32_t player = 0;
            std::vector<session::input_frame> frames;   // <= MAX_RELAY_FRAMES, oldest first, ending at tick
        };
        struct held_record {
            uint8_t back = 0;                               // tick - (the substituted tick), 0..31
            uint32_t source = session::kHeldSourceDefault;  // when !has_frame
            bool has_frame = false;                         // the frame is inline
            session::input_frame frame{};                   // when has_frame
        };
        static constexpr uint8_t FRAME_CAMERA = 1;
        static constexpr uint8_t HELD_BACK_MASK = 0x1F;
        static constexpr uint8_t HELD_INLINE = 0x80;
        static_assert(sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1 + sizeof(uint32_t) * 3 + 1 == session::kRelayDatagramHeader,
                      "the relay's byte budget counts this header");
        static_assert(session::kHeldNoticeTicks - 1 <= HELD_BACK_MASK, "back fits its bits");

        uint32_t session = 0;
        uint32_t tick = 0;                 // the newest tick in the datagram
        uint32_t acked = 0;                // the recipient's last tick applied fresh
        uint32_t held_total = 0;           // the recipient's substituted ticks at or after epoch
        uint32_t epoch = 0;                // the recipient's assigned base
        uint8_t wait = 0;                  // protocol 13: the recipient's wait at the server, ticks
        std::vector<entry> entries;        // <= MAX_PLAYERS_PER_SESSION
        std::vector<held_record> held;     // <= kHeldNoticeTicks, the recipient's own

        session_remote_input_msg() : serializable_message(bmmo::SessionRemoteInput) {}

        // The tick of frame `index` of `e`.
        uint32_t frame_tick(const entry& e, size_t index) const {
            return tick - static_cast<uint32_t>(e.frames.size()) + 1 + static_cast<uint32_t>(index);
        }
        // The tick a held record is for.
        uint32_t held_tick(const held_record& r) const { return tick - r.back; }

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
            raw.write(reinterpret_cast<const char*>(&acked), sizeof(acked));
            raw.write(reinterpret_cast<const char*>(&held_total), sizeof(held_total));
            raw.write(reinterpret_cast<const char*>(&epoch), sizeof(epoch));
            raw.write(reinterpret_cast<const char*>(&wait), sizeof(wait));
            for (const entry* e: written) {
                raw.write(reinterpret_cast<const char*>(&e->player), sizeof(e->player));
                // More frames than the cap: the newest ones are what matter.
                const size_t total = e->frames.size();
                const size_t first = total > session::MAX_RELAY_FRAMES ? total - session::MAX_RELAY_FRAMES : 0;
                const uint8_t nframes = static_cast<uint8_t>(total - first);
                raw.write(reinterpret_cast<const char*>(&nframes), sizeof(nframes));
                for (size_t i = first; i < total; ++i)
                    write_frame(e->frames[i], i == first || !same_camera(e->frames[i - 1], e->frames[i]));
            }
            // A record whose tick the 5 bits cannot name, or one past the
            // count's cap, cannot be said; the builder never makes either.
            std::vector<const held_record*> records;
            for (const auto& r: held)
                if (r.back <= HELD_BACK_MASK && r.back <= tick && records.size() < session::kHeldNoticeTicks)
                    records.push_back(&r);
            const uint8_t nheld = static_cast<uint8_t>(records.size());
            raw.write(reinterpret_cast<const char*>(&nheld), sizeof(nheld));
            for (const held_record* r: records) {
                const uint8_t back = static_cast<uint8_t>(r->back | (r->has_frame ? HELD_INLINE : 0));
                raw.write(reinterpret_cast<const char*>(&back), sizeof(back));
                if (r->has_frame) write_frame(r->frame, true);
                else raw.write(reinterpret_cast<const char*>(&r->source), sizeof(r->source));
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
            if (!message_utils::read_variable(raw, &acked)) return false;
            if (!message_utils::read_variable(raw, &held_total)) return false;
            if (!message_utils::read_variable(raw, &epoch)) return false;
            if (!message_utils::read_variable(raw, &wait)) return false;
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
                    if (!read_frame(f, k == 0 ? nullptr : &e.frames.back())) return false;
                    e.frames.push_back(f);
                }
                entries.push_back(std::move(e));
            }
            uint8_t nheld = 0;
            if (!message_utils::read_variable(raw, &nheld)) return false;
            if (nheld > session::kHeldNoticeTicks) return false;
            held.clear();
            held.reserve(nheld);
            for (uint8_t i = 0; i < nheld; ++i) {
                held_record r;
                uint8_t back = 0;
                if (!message_utils::read_variable(raw, &back)) return false;
                if (back & ~(HELD_BACK_MASK | HELD_INLINE)) return false;   // bits 5-6 must be 0
                r.back = static_cast<uint8_t>(back & HELD_BACK_MASK);
                if (r.back > tick) return false;                            // a tick before 0
                r.has_frame = (back & HELD_INLINE) != 0;
                if (r.has_frame) {
                    if (!read_frame(r.frame, nullptr)) return false;
                } else if (!message_utils::read_variable(raw, &r.source)) {
                    return false;
                }
                held.push_back(r);
            }
            // Nothing follows the held section: what does is another layout.
            return raw.peek() == std::stringstream::traits_type::eof();
        }

    private:
        void write_frame(const session::input_frame& f, bool camera) {
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
        // `previous` is the frame an omitted camera is copied from; without
        // one (an entry's first frame, an inline held frame) the camera must
        // be there.
        bool read_frame(session::input_frame& f, const session::input_frame* previous) {
            uint8_t header = 0;
            if (!message_utils::read_variable(raw, &header)) return false;
            if (header & ~FRAME_CAMERA) return false;                 // a bit this reader does not know
            if (!previous && !(header & FRAME_CAMERA)) return false;  // the first frame always carries the camera
            if (!message_utils::read_variable(raw, &f.keys)) return false;
            if (!message_utils::read_variable(raw, &f.ball_type)) return false;
            if (!message_utils::read_variable(raw, &f.flags)) return false;
            if (header & FRAME_CAMERA) {
                if (!raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))) return false;
                if (!raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir))) return false;
            } else {
                std::memcpy(f.cam_right, previous->cam_right, sizeof(f.cam_right));
                std::memcpy(f.cam_up, previous->cam_up, sizeof(f.cam_up));
                std::memcpy(f.cam_dir, previous->cam_dir, sizeof(f.cam_dir));
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
