#ifndef BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"
#include "../session/input_relay.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace bmmo {
    // server -> client, one per simulated tick (more when one packet cannot
    // hold it), UNRELIABLE no-delay: for every *other* member, the input
    // frames the server applied for it at every tick after `from` up to
    // `tick` - `from` being the newest tick the recipient acknowledged
    // holding every member's frame for (session_input_msg::relay_ack), so
    // whatever a loss cost, the next datagram that arrives fills it, and
    // nothing ever waits behind a loss (docs/relay-protocol-design.md,
    // protocol 29; session/input_relay.hpp).  A client drives its mirrored
    // remote balls through the same navigation code as the server (design
    // 9.1) and lands every frame by its tick.
    //
    // Layout: session u32, tick u32 (the newest), count u8, acked u32,
    // held_total u32, epoch u32, wait u8 (protocol 13), from u32, part u8,
    // parts u8, ncameras u8; per camera definition: id u8 and
    // cam_right/cam_up/cam_dir f32x3; per entry: player u32, nframes u8 (the
    // ticks tick-nframes+1..tick, all after `from`, oldest first), nruns u8,
    // per run: start u8 (the index of its first frame; the first run starts
    // at 0), keys u8, ball_type u8, flags u8, camera u8 (a defined id, or
    // kCameraInline followed by the three rows).  A run's frame stands for
    // every tick up to the next run's start.  `part` of `parts`: the
    // datagrams one tick's relay was split into, each with whole entries;
    // the recipient counts the span as held once it has them all.
    //
    // Camera ids are the server's for the whole session (input_relay.hpp
    // relay_ring), each naming one exact camera basis; a definition rides
    // every datagram that needs it until the recipient's acknowledgement
    // passes its first send, and a client keeps the dictionary for the
    // session.  In memory an entry's frames carry their cameras; on the wire
    // `camera_ids` (one per frame) replaces them, and a decoded entry has
    // them filled from the definitions by the client (resolve_cameras).
    //
    // Protocol 7 (late-player design 1.1): acked, held_total and epoch are the
    // RECIPIENT's - the last tick the server applied a fresh frame of its own,
    // how many of its ticks at or after `epoch` the server substituted, and
    // `epoch`, the base of its last SessionAssign - and the held section
    // trails the entries: nheld u8 (0..kHeldNoticeTicks), then per substituted
    // tick T of the recipient: back u8 (bits 0-4 tick - T, bit 7 an inline
    // frame follows, bits 5-6 zero) and either source u32 (the tick whose
    // frame as the client sent it the server applied at T, or
    // kHeldSourceDefault for the default frame) or the frame (header u8 with
    // FRAME_CAMERA, keys u8, ball_type u8, flags u8, the three rows).  The
    // section is always there, nheld 0 when there is nothing to say, and
    // nothing may follow it.
    //
    // Protocol 13 (step R12, late-player design 2.5): wait is the RECIPIENT's
    // wait at the server (lateness.hpp server_wait_meter), which its client
    // sizes its rollback reach with (rollback.hpp reach_meter).
    struct session_remote_input_msg : public serializable_message {
        struct entry {
            uint32_t player = 0;
            std::vector<session::input_frame> frames;   // <= MAX_RELAY_FRAMES, oldest first, ending at tick
            std::vector<uint8_t> camera_ids;            // per frame on the wire; empty: the frames carry their cameras (sent inline)
        };
        struct camera_definition {
            uint8_t id = 0;
            session::camera_rows rows{};
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
        static_assert(sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1 + sizeof(uint32_t) * 3 + 1 + sizeof(uint32_t) + 3
                      == session::kRelayDatagramHeader, "the relay's byte budget counts this header");
        static_assert(session::kHeldNoticeTicks - 1 <= HELD_BACK_MASK, "back fits its bits");
        static_assert(session::MAX_RELAY_FRAMES <= 255, "nframes and a run's start are u8");

        uint32_t session = 0;
        uint32_t tick = 0;                 // the newest tick in the datagram
        uint32_t acked = 0;                // the recipient's last tick applied fresh
        uint32_t held_total = 0;           // the recipient's substituted ticks at or after epoch
        uint32_t epoch = 0;                // the recipient's assigned base
        uint8_t wait = 0;                  // protocol 13: the recipient's wait at the server, ticks
        uint32_t from = 0;                 // protocol 29: the frames are of the ticks after it
        uint8_t part = 0, parts = 1;       // protocol 29: this datagram among the tick's
        std::vector<camera_definition> cameras;
        std::vector<entry> entries;        // <= MAX_PLAYERS_PER_SESSION
        std::vector<held_record> held;     // <= kHeldNoticeTicks, the recipient's own

        session_remote_input_msg() : serializable_message(bmmo::SessionRemoteInput) {}

        uint32_t frame_tick(const entry& e, size_t index) const {
            return tick - static_cast<uint32_t>(e.frames.size()) + 1 + static_cast<uint32_t>(index);
        }
        uint32_t held_tick(const held_record& r) const { return tick - r.back; }

        // The client's half of the camera dictionary: takes the definitions
        // into `dictionary` (id -> rows, a slot per id) and gives the entries
        // with every frame's camera filled from it.  Empty when an entry
        // names an id neither this datagram nor an earlier one defined - the
        // datagram is then not usable, and its span is not held.
        template <class Dictionary>
        std::optional<std::vector<entry>> resolved_entries(Dictionary& dictionary) const {
            for (const auto& d: cameras) dictionary[d.id] = d.rows;
            std::vector<entry> out = entries;
            for (auto& e: out) {
                for (size_t i = 0; i < e.camera_ids.size(); ++i) {
                    if (e.camera_ids[i] == session::kCameraInline) continue;
                    const auto& rows = dictionary[e.camera_ids[i]];
                    if (!rows) return std::nullopt;
                    session::set_camera(e.frames[i], *rows);
                }
                e.camera_ids.clear();
            }
            return out;
        }

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            std::vector<const entry*> written;
            for (const auto& e: entries)
                if (!e.frames.empty() && e.frames.size() <= session::MAX_RELAY_FRAMES
                        && (e.camera_ids.empty() || e.camera_ids.size() == e.frames.size())
                        && written.size() < session::MAX_PLAYERS_PER_SESSION)
                    written.push_back(&e);
            const uint8_t count = static_cast<uint8_t>(written.size());
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            raw.write(reinterpret_cast<const char*>(&acked), sizeof(acked));
            raw.write(reinterpret_cast<const char*>(&held_total), sizeof(held_total));
            raw.write(reinterpret_cast<const char*>(&epoch), sizeof(epoch));
            raw.write(reinterpret_cast<const char*>(&wait), sizeof(wait));
            raw.write(reinterpret_cast<const char*>(&from), sizeof(from));
            raw.write(reinterpret_cast<const char*>(&part), sizeof(part));
            raw.write(reinterpret_cast<const char*>(&parts), sizeof(parts));
            const uint8_t ncameras = static_cast<uint8_t>(std::min<size_t>(cameras.size(), session::kCameraInline));
            raw.write(reinterpret_cast<const char*>(&ncameras), sizeof(ncameras));
            for (size_t i = 0; i < ncameras; ++i) {
                raw.write(reinterpret_cast<const char*>(&cameras[i].id), sizeof(cameras[i].id));
                raw.write(reinterpret_cast<const char*>(cameras[i].rows.data()), sizeof(cameras[i].rows));
            }
            for (const entry* e: written) {
                raw.write(reinterpret_cast<const char*>(&e->player), sizeof(e->player));
                const uint8_t nframes = static_cast<uint8_t>(e->frames.size());
                raw.write(reinterpret_cast<const char*>(&nframes), sizeof(nframes));
                std::vector<uint8_t> starts;
                for (size_t i = 0; i < e->frames.size(); ++i)
                    if (i == 0 || !same_run(*e, i - 1, i)) starts.push_back(static_cast<uint8_t>(i));
                const uint8_t nruns = static_cast<uint8_t>(starts.size());
                raw.write(reinterpret_cast<const char*>(&nruns), sizeof(nruns));
                for (const uint8_t start: starts) {
                    const auto& f = e->frames[start];
                    const uint8_t camera = camera_at(*e, start);
                    raw.write(reinterpret_cast<const char*>(&start), sizeof(start));
                    raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                    raw.write(reinterpret_cast<const char*>(&f.ball_type), sizeof(f.ball_type));
                    raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
                    raw.write(reinterpret_cast<const char*>(&camera), sizeof(camera));
                    if (camera == session::kCameraInline) write_camera(f);
                }
            }
            std::vector<const held_record*> records;
            for (const auto& r: held)
                if (r.back <= HELD_BACK_MASK && r.back <= tick && records.size() < session::kHeldNoticeTicks)
                    records.push_back(&r);
            const uint8_t nheld = static_cast<uint8_t>(records.size());
            raw.write(reinterpret_cast<const char*>(&nheld), sizeof(nheld));
            for (const held_record* r: records) {
                const uint8_t back = static_cast<uint8_t>(r->back | (r->has_frame ? HELD_INLINE : 0));
                raw.write(reinterpret_cast<const char*>(&back), sizeof(back));
                if (r->has_frame) write_frame(r->frame);
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
            if (!message_utils::read_variable(raw, &from)) return false;
            if (!message_utils::read_variable(raw, &part)) return false;
            if (!message_utils::read_variable(raw, &parts)) return false;
            if (parts == 0 || part >= parts) return false;
            uint8_t ncameras = 0;
            if (!message_utils::read_variable(raw, &ncameras)) return false;
            if (ncameras >= session::kCameraInline) return false;
            cameras.assign(ncameras, {});
            for (auto& d: cameras) {
                if (!message_utils::read_variable(raw, &d.id) || d.id == session::kCameraInline) return false;
                if (!raw.read(reinterpret_cast<char*>(d.rows.data()), sizeof(d.rows))) return false;
            }
            entries.clear();
            entries.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                entry e;
                uint8_t nframes = 0, nruns = 0;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &nframes)) return false;
                if (nframes == 0 || nframes > session::MAX_RELAY_FRAMES) return false;
                if (static_cast<uint32_t>(nframes) - 1u > tick) return false;          // a frame before tick 0
                if (static_cast<int32_t>(tick - nframes - from) < 0) return false;     // a frame at or before `from`
                if (!message_utils::read_variable(raw, &nruns)) return false;
                if (nruns == 0 || nruns > nframes) return false;
                struct run { uint8_t start = 0; session::input_frame frame; uint8_t camera = 0; };
                std::vector<run> runs(nruns);
                for (uint8_t r = 0; r < nruns; ++r) {
                    run& x = runs[r];
                    if (!message_utils::read_variable(raw, &x.start)) return false;
                    if (r == 0 ? x.start != 0 : x.start <= runs[r - 1].start) return false;
                    if (x.start >= nframes) return false;
                    if (!message_utils::read_variable(raw, &x.frame.keys)) return false;
                    if (!message_utils::read_variable(raw, &x.frame.ball_type)) return false;
                    if (!message_utils::read_variable(raw, &x.frame.flags)) return false;
                    if (!message_utils::read_variable(raw, &x.camera)) return false;
                    if (x.camera == session::kCameraInline && !read_camera(x.frame)) return false;
                }
                // A run's frame stands for every tick up to the next run's start.
                for (uint8_t r = 0; r < nruns; ++r) {
                    const size_t end = r + 1 < nruns ? runs[r + 1].start : nframes;
                    e.frames.insert(e.frames.end(), end - runs[r].start, runs[r].frame);
                    e.camera_ids.insert(e.camera_ids.end(), end - runs[r].start, runs[r].camera);
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
                    if (!read_frame(r.frame)) return false;
                } else if (!message_utils::read_variable(raw, &r.source)) {
                    return false;
                }
                held.push_back(r);
            }
            // Nothing follows the held section: what does is another layout.
            return raw.peek() == std::stringstream::traits_type::eof();
        }

    private:
        // One run: the same keys, ball type, flags and camera (the id, and
        // for an inline one the rows) as the frame before.
        static bool same_run(const entry& e, size_t a, size_t b) {
            const auto& x = e.frames[a];
            const auto& y = e.frames[b];
            return x.keys == y.keys && x.ball_type == y.ball_type && x.flags == y.flags
                && camera_at(e, a) == camera_at(e, b)
                && (camera_at(e, a) != session::kCameraInline || session::same_camera(x, y));
        }
        // An entry without ids (built in memory) sends its cameras inline.
        static uint8_t camera_at(const entry& e, size_t i) {
            return e.camera_ids.empty() ? session::kCameraInline : e.camera_ids[i];
        }
        void write_camera(const session::input_frame& f) {
            raw.write(reinterpret_cast<const char*>(f.cam_right), sizeof(f.cam_right));
            raw.write(reinterpret_cast<const char*>(f.cam_up), sizeof(f.cam_up));
            raw.write(reinterpret_cast<const char*>(f.cam_dir), sizeof(f.cam_dir));
        }
        bool read_camera(session::input_frame& f) {
            return raw.read(reinterpret_cast<char*>(f.cam_right), sizeof(f.cam_right))
                && raw.read(reinterpret_cast<char*>(f.cam_up), sizeof(f.cam_up))
                && raw.read(reinterpret_cast<char*>(f.cam_dir), sizeof(f.cam_dir));
        }
        // A held record's inline frame: always with its camera.
        void write_frame(const session::input_frame& f) {
            const uint8_t header = FRAME_CAMERA;
            raw.write(reinterpret_cast<const char*>(&header), sizeof(header));
            raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
            raw.write(reinterpret_cast<const char*>(&f.ball_type), sizeof(f.ball_type));
            raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
            write_camera(f);
        }
        bool read_frame(session::input_frame& f) {
            uint8_t header = 0;
            if (!message_utils::read_variable(raw, &header) || header != FRAME_CAMERA) return false;
            if (!message_utils::read_variable(raw, &f.keys)) return false;
            if (!message_utils::read_variable(raw, &f.ball_type)) return false;
            if (!message_utils::read_variable(raw, &f.flags)) return false;
            return read_camera(f);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
