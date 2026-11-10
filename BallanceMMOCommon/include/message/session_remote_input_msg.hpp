#ifndef BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_REMOTE_INPUT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"
#include "../session/input_relay.hpp"
#include "../session/relay_events.hpp"

#include <algorithm>
#include <array>
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
    // protocols 29 and 30; session/input_relay.hpp).  A client drives its mirrored
    // remote balls through the same navigation code as the server (design
    // 9.1) and lands every frame by its tick.
    //
    // Layout: session u32, tick u32 (the newest), count u8, acked u32,
    // held_total u32, epoch u32, wait u8 (protocol 13), from u32, part u8,
    // parts u8; per entry: player u32, nframes u8 (the ticks
    // end-nframes+1..end, all after `from`, oldest first; end = tick - back),
    // nruns u8 - or 0, the extension: flags u8 (bit 0 a base follows, bit 1
    // back follows), nruns u8, back u8 when flagged (an entry that ends
    // before the newest tick: a leaver's last frames, a run before a gap,
    // protocol 31) - then the base when attached (since u32 and
    // cam_right/cam_up/cam_dir f32x3), per run: start u8 (the index of its
    // first frame; the first run starts at 0), keys u8, ball_type u8 with the
    // heading code in bits 5-7, flags u8, and the three rows when the code is
    // kHeadingInline.  A run's frame stands for every tick up to the next
    // run's start.  `part` of `parts`: the datagrams one tick's relay was
    // split into, each with whole entries; the recipient counts the span as
    // held once it has them all.
    //
    // Protocol 30: a heading code is a quarter turn of the player's camera
    // base in force at the frame's tick, the zero rows, or inline - the rows
    // follow and are the base from that tick on (session/camera_heading.hpp).
    // An entry whose first camera is a turn of a base the recipient may not
    // hold attaches that base, until the recipient's acknowledgement passes
    // the datagram that first carried it (input_relay.hpp relay_ring).  In
    // memory an entry's frames carry their cameras; on the wire the codes
    // replace them, and the client fills them in (resolved_entries).
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
    // section is always there, nheld 0 when there is nothing to say.
    //
    // Protocol 33 (docs/relay-protocol-design.md 4.9): the events section
    // trails the held one, always there and last: nrecipes u8 (id u8 and the
    // recipe as a Physicalize writes it, each), nposes u8 (position f32x3 and
    // rotation f32x9, each), nevents u8, and per event a head u8 (bits 0-2 the
    // type or kRelayEventRaw, bits 3-5 the state, bit 6 an origin byte follows,
    // bit 7 zero) - a raw one: len u16 and the message as sent reliably; a
    // compact one: player u32, life u16, tick - this tick as i16, the origin
    // byte (tick - origin_tick), then by type: Physicalize ball_type u8, flags
    // u8, pose u8 (into the table), recipe u8 (the session's dictionary);
    // Unphysicalize and Finish nothing; Sector i32; Trafo ball_type u8, flags
    // u8, position f32x3.  The recipes are the dictionary's entries this
    // recipient may not hold yet (session/input_relay.hpp relay_ring).
    //
    // Protocol 13 (step R12, late-player design 2.5): wait is the RECIPIENT's
    // wait at the server (lateness.hpp server_wait_meter), which its client
    // sizes its rollback reach with (rollback.hpp reach_meter).
    //
    // Protocol 37 (session/input_relay.hpp, the hints): after the events
    // section, only when there is one to carry: nhints u8 (1..), then per
    // hint player u32, nticks u8 (1..kHintMaxTicks: the ticks tick + 1 ..
    // tick + nticks), nruns u8 and per run len u8, keys u8, flags u8 - what
    // that member's frames queued at the server hold for those ticks.  A
    // datagram without hints ends with its events section, as before.
    struct session_remote_input_msg : public serializable_message {
        struct entry {
            uint32_t player = 0;
            std::vector<session::input_frame> frames;   // <= MAX_RELAY_FRAMES, oldest first, ending at tick
            std::vector<uint8_t> headings;              // per frame on the wire; empty: every camera inline (zero rows as none)
            std::optional<session::camera_bases::base> base;
            uint8_t back = 0;                           // the entry ends at tick - back
        };
        struct held_record {
            uint8_t back = 0;                               // tick - (the substituted tick), 0..31
            uint32_t source = session::kHeldSourceDefault;  // when !has_frame
            bool has_frame = false;                         // the frame is inline
            session::input_frame frame{};                   // when has_frame
        };
        static constexpr uint8_t FRAME_CAMERA = 1;
        static constexpr uint8_t ENTRY_BASE = 1, ENTRY_BACK = 2;
        static constexpr uint8_t HELD_BACK_MASK = 0x1F;
        static constexpr uint8_t HELD_INLINE = 0x80;
        static_assert(sizeof(bmmo::opcode) + sizeof(uint32_t) * 2 + 1 + sizeof(uint32_t) * 3 + 1 + sizeof(uint32_t) + 2
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
        std::vector<entry> entries;        // <= MAX_PLAYERS_PER_SESSION
        std::vector<held_record> held;     // <= kHeldNoticeTicks, the recipient's own
        std::vector<std::pair<uint8_t, session::ball_recipe>> recipes;   // protocol 33: attached
        std::vector<session::relay_event> events;                       // protocol 33, oldest first
        std::vector<session::hint_entry> hints;                         // protocol 37, of the ticks after `tick`

        session_remote_input_msg() : serializable_message(bmmo::SessionRemoteInput) {}

        uint32_t frame_tick(const entry& e, size_t index) const {
            return tick - e.back - static_cast<uint32_t>(e.frames.size()) + 1 + static_cast<uint32_t>(index);
        }
        uint32_t held_tick(const held_record& r) const { return tick - r.back; }

        // The client's half of the headings: the attached bases and the
        // inline frames into the players' `bases` (player -> camera_bases),
        // and the entries with every frame's camera filled.  Empty when a
        // frame is a turn of a base this client does not hold - the datagram
        // is then not usable, and its span is not held.
        template <class Bases>
        std::optional<std::vector<entry>> resolved_entries(Bases& bases) const {
            std::vector<entry> out = entries;
            for (auto& e: out) {
                auto& player = bases[e.player];
                if (e.base) player.set(e.base->since, e.base->rows);
                for (size_t i = 0; i < e.frames.size(); ++i) {
                    const uint32_t t = frame_tick(e, i);
                    const uint8_t code = heading_at(e, i);
                    if (code == session::kHeadingInline) {
                        player.set(t, session::camera_of(e.frames[i]));
                        continue;
                    }
                    session::camera_rows rows;
                    if (!player.decode(t, code, rows)) return std::nullopt;
                    session::set_camera(e.frames[i], rows);
                }
                e.headings.clear();
                e.base.reset();
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
                        && (e.headings.empty() || e.headings.size() == e.frames.size())
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
            for (const entry* e: written) {
                raw.write(reinterpret_cast<const char*>(&e->player), sizeof(e->player));
                const uint8_t nframes = static_cast<uint8_t>(e->frames.size());
                raw.write(reinterpret_cast<const char*>(&nframes), sizeof(nframes));
                std::vector<uint8_t> starts;
                for (size_t i = 0; i < e->frames.size(); ++i)
                    if (i == 0 || !same_run(*e, i - 1, i)) starts.push_back(static_cast<uint8_t>(i));
                const uint8_t nruns = static_cast<uint8_t>(starts.size());
                if (e->base || e->back) {
                    const uint8_t extension = 0;
                    const uint8_t flags = static_cast<uint8_t>((e->base ? ENTRY_BASE : 0) | (e->back ? ENTRY_BACK : 0));
                    raw.write(reinterpret_cast<const char*>(&extension), sizeof(extension));
                    raw.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
                }
                raw.write(reinterpret_cast<const char*>(&nruns), sizeof(nruns));
                if (e->back) raw.write(reinterpret_cast<const char*>(&e->back), sizeof(e->back));
                if (e->base) {
                    raw.write(reinterpret_cast<const char*>(&e->base->since), sizeof(e->base->since));
                    raw.write(reinterpret_cast<const char*>(e->base->rows.data()), sizeof(e->base->rows));
                }
                for (const uint8_t start: starts) {
                    const auto& f = e->frames[start];
                    const uint8_t code = heading_at(*e, start);
                    const uint8_t type = static_cast<uint8_t>((f.ball_type & session::kBallTypeMask) | (code << session::kHeadingShift));
                    raw.write(reinterpret_cast<const char*>(&start), sizeof(start));
                    raw.write(reinterpret_cast<const char*>(&f.keys), sizeof(f.keys));
                    raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
                    raw.write(reinterpret_cast<const char*>(&f.flags), sizeof(f.flags));
                    if (code == session::kHeadingInline) write_camera(f);
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
            write_events();
            write_hints();
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
            entries.clear();
            entries.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                entry e;
                uint8_t nframes = 0, nruns = 0;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &nframes)) return false;
                if (nframes == 0 || nframes > session::MAX_RELAY_FRAMES) return false;
                if (!message_utils::read_variable(raw, &nruns)) return false;
                uint8_t flags = 0;
                if (nruns == 0) {
                    if (!message_utils::read_variable(raw, &flags) || (flags & ~(ENTRY_BASE | ENTRY_BACK)) || !flags) return false;
                    if (!message_utils::read_variable(raw, &nruns)) return false;
                }
                if (flags & ENTRY_BACK) {
                    if (!message_utils::read_variable(raw, &e.back) || e.back == 0) return false;
                }
                if (static_cast<uint32_t>(nframes) - 1u + e.back > tick) return false;           // a frame before tick 0
                if (static_cast<int32_t>(tick - e.back - nframes - from) < 0) return false;    // a frame at or before `from`
                if (flags & ENTRY_BASE) {
                    session::camera_bases::base b;
                    if (!message_utils::read_variable(raw, &b.since)) return false;
                    if (!raw.read(reinterpret_cast<char*>(b.rows.data()), sizeof(b.rows))) return false;
                    e.base = b;
                }
                if (nruns == 0 || nruns > nframes) return false;
                struct run { uint8_t start = 0; session::input_frame frame; uint8_t heading = 0; };
                std::vector<run> runs(nruns);
                for (uint8_t r = 0; r < nruns; ++r) {
                    run& x = runs[r];
                    if (!message_utils::read_variable(raw, &x.start)) return false;
                    if (r == 0 ? x.start != 0 : x.start <= runs[r - 1].start) return false;
                    if (x.start >= nframes) return false;
                    uint8_t type = 0;
                    if (!message_utils::read_variable(raw, &x.frame.keys)) return false;
                    if (!message_utils::read_variable(raw, &type)) return false;
                    if (!message_utils::read_variable(raw, &x.frame.flags)) return false;
                    x.heading = static_cast<uint8_t>(type >> session::kHeadingShift);
                    if (!session::valid_heading(x.heading)) return false;
                    x.frame.ball_type = static_cast<uint8_t>(type & session::kBallTypeMask);
                    if (x.heading == session::kHeadingInline && !read_camera(x.frame)) return false;
                }
                // A run's frame stands for every tick up to the next run's start.
                for (uint8_t r = 0; r < nruns; ++r) {
                    const size_t end = r + 1 < nruns ? runs[r + 1].start : nframes;
                    e.frames.insert(e.frames.end(), end - runs[r].start, runs[r].frame);
                    e.headings.insert(e.headings.end(), end - runs[r].start, runs[r].heading);
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
            if (!read_events()) return false;
            hints.clear();
            if (raw.peek() == std::stringstream::traits_type::eof()) return true;
            // Nothing follows the hints: what does is another layout.
            return read_hints() && raw.peek() == std::stringstream::traits_type::eof();
        }

    private:
        template <class T>
        void put(const T& v) { raw.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
        void write_events() {
            const uint8_t nrecipes = static_cast<uint8_t>(std::min<size_t>(recipes.size(), 255));
            put(nrecipes);
            for (uint8_t i = 0; i < nrecipes; ++i) {
                put(recipes[i].first);
                session_event_msg::write_recipe(recipes[i].second, raw);
            }
            std::vector<std::string> poses;
            std::vector<uint8_t> pose_of(events.size(), 0);
            for (size_t i = 0; i < events.size() && i < 255; ++i) {
                const auto& e = events[i];
                if (!session::relay_compact_at(e, tick) || e.kind != static_cast<uint8_t>(session::event_type::Physicalize))
                    continue;
                const std::string pose = e.pose();
                const auto it = std::find(poses.begin(), poses.end(), pose);
                pose_of[i] = static_cast<uint8_t>(it - poses.begin());
                if (it == poses.end()) poses.push_back(pose);
            }
            const uint8_t nposes = static_cast<uint8_t>(poses.size());
            put(nposes);
            for (const auto& pose: poses) raw.write(pose.data(), static_cast<std::streamsize>(pose.size()));
            const uint8_t nevents = static_cast<uint8_t>(std::min<size_t>(events.size(), 255));
            put(nevents);
            for (uint8_t i = 0; i < nevents; ++i) {
                const auto& e = events[i];
                if (!session::relay_compact_at(e, tick)) {
                    put(session::kRelayEventRaw);
                    put(static_cast<uint16_t>(e.raw.size()));
                    raw.write(e.raw.data(), static_cast<std::streamsize>(e.raw.size()));
                    continue;
                }
                const bool origin = e.origin_tick != e.tick;
                put(static_cast<uint8_t>(e.kind | (e.state << session::kRelayEventStateShift)
                                         | (origin ? session::kRelayEventOrigin : 0)));
                put(e.player);
                put(e.life);
                put(static_cast<int16_t>(static_cast<int32_t>(e.tick - tick)));
                if (origin) put(static_cast<uint8_t>(e.tick - e.origin_tick));
                switch (static_cast<session::event_type>(e.kind)) {
                    case session::event_type::Physicalize:
                        put(e.ball_type);
                        put(e.flags);
                        put(pose_of[i]);
                        put(e.recipe);
                        break;
                    case session::event_type::Sector:
                        put(e.sector);
                        break;
                    case session::event_type::Trafo:
                        put(e.ball_type);
                        put(e.flags);
                        raw.write(reinterpret_cast<const char*>(e.position), sizeof(e.position));
                        break;
                    default:
                        break;
                }
            }
        }
        bool read_events() {
            uint8_t nrecipes = 0;
            if (!message_utils::read_variable(raw, &nrecipes)) return false;
            recipes.clear();
            for (uint8_t i = 0; i < nrecipes; ++i) {
                std::pair<uint8_t, session::ball_recipe> r;
                if (!message_utils::read_variable(raw, &r.first)) return false;
                if (!session_event_msg::read_recipe(r.second, raw)) return false;
                recipes.push_back(std::move(r));
            }
            uint8_t nposes = 0;
            if (!message_utils::read_variable(raw, &nposes)) return false;
            std::vector<std::array<float, 12>> poses(nposes);
            for (auto& pose: poses)
                if (!raw.read(reinterpret_cast<char*>(pose.data()), sizeof(pose))) return false;
            uint8_t nevents = 0;
            if (!message_utils::read_variable(raw, &nevents)) return false;
            events.clear();
            events.reserve(nevents);
            for (uint8_t i = 0; i < nevents; ++i) {
                session::relay_event e;
                uint8_t head = 0;
                if (!message_utils::read_variable(raw, &head) || (head & 0x80)) return false;
                e.kind = static_cast<uint8_t>(head & 0x07);
                if (e.kind == session::kRelayEventRaw) {
                    uint16_t len = 0;
                    if (head != session::kRelayEventRaw || !message_utils::read_variable(raw, &len) || len == 0) return false;
                    e.raw.resize(len);
                    if (!raw.read(e.raw.data(), len)) return false;
                    events.push_back(std::move(e));
                    continue;
                }
                const auto type = static_cast<session::event_type>(e.kind);
                if (type != session::event_type::Physicalize && type != session::event_type::Unphysicalize
                        && type != session::event_type::Sector && type != session::event_type::Finish
                        && type != session::event_type::Trafo)
                    return false;
                e.state = static_cast<uint8_t>((head >> session::kRelayEventStateShift) & 0x07);
                int16_t dtick = 0;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &e.life)) return false;
                if (!message_utils::read_variable(raw, &dtick)) return false;
                e.tick = tick + static_cast<uint32_t>(static_cast<int32_t>(dtick));
                e.origin_tick = e.tick;
                if (head & session::kRelayEventOrigin) {
                    uint8_t back = 0;
                    if (!message_utils::read_variable(raw, &back) || back == 0) return false;
                    e.origin_tick = e.tick - back;
                }
                switch (type) {
                    case session::event_type::Physicalize: {
                        uint8_t pose = 0;
                        if (!message_utils::read_variable(raw, &e.ball_type)) return false;
                        if (!message_utils::read_variable(raw, &e.flags)) return false;
                        if (!message_utils::read_variable(raw, &pose) || pose >= poses.size()) return false;
                        if (!message_utils::read_variable(raw, &e.recipe)) return false;
                        std::memcpy(e.position, poses[pose].data(), sizeof(e.position));
                        std::memcpy(e.rotation, poses[pose].data() + 3, sizeof(e.rotation));
                        break;
                    }
                    case session::event_type::Sector:
                        if (!message_utils::read_variable(raw, &e.sector)) return false;
                        break;
                    case session::event_type::Trafo:
                        if (!message_utils::read_variable(raw, &e.ball_type)) return false;
                        if (!message_utils::read_variable(raw, &e.flags)) return false;
                        if (!raw.read(reinterpret_cast<char*>(e.position), sizeof(e.position))) return false;
                        break;
                    default:
                        break;
                }
                events.push_back(std::move(e));
            }
            return true;
        }
        void write_hints() {
            std::vector<const session::hint_entry*> written;
            for (const auto& h: hints)
                if (!h.ticks.empty() && h.ticks.size() <= session::kHintMaxTicks && written.size() < 255)
                    written.push_back(&h);
            if (written.empty()) return;
            put(static_cast<uint8_t>(written.size()));
            for (const session::hint_entry* h: written) {
                put(h->player);
                put(static_cast<uint8_t>(h->ticks.size()));
                put(static_cast<uint8_t>(session::hint_runs(*h)));
                for (size_t i = 0; i < h->ticks.size();) {
                    size_t end = i + 1;
                    while (end < h->ticks.size() && h->ticks[end] == h->ticks[i]) ++end;
                    put(static_cast<uint8_t>(end - i));
                    put(h->ticks[i].keys);
                    put(h->ticks[i].flags);
                    i = end;
                }
            }
        }
        bool read_hints() {
            uint8_t nhints = 0;
            if (!message_utils::read_variable(raw, &nhints) || nhints == 0) return false;
            hints.reserve(nhints);
            for (uint8_t i = 0; i < nhints; ++i) {
                session::hint_entry h;
                uint8_t nticks = 0, nruns = 0;
                if (!message_utils::read_variable(raw, &h.player)) return false;
                if (!message_utils::read_variable(raw, &nticks) || nticks == 0 || nticks > session::kHintMaxTicks) return false;
                if (!message_utils::read_variable(raw, &nruns) || nruns == 0 || nruns > nticks) return false;
                for (uint8_t r = 0; r < nruns; ++r) {
                    uint8_t len = 0;
                    session::input_hint hint;
                    if (!message_utils::read_variable(raw, &len) || len == 0 || h.ticks.size() + len > nticks) return false;
                    if (!message_utils::read_variable(raw, &hint.keys)) return false;
                    if (!message_utils::read_variable(raw, &hint.flags)) return false;
                    h.ticks.insert(h.ticks.end(), len, hint);
                }
                if (h.ticks.size() != nticks) return false;
                hints.push_back(std::move(h));
            }
            return true;
        }
        // One run: the same keys, ball type, flags and heading (and for an
        // inline one the rows) as the frame before.
        static bool same_run(const entry& e, size_t a, size_t b) {
            const auto& x = e.frames[a];
            const auto& y = e.frames[b];
            return x.keys == y.keys && x.ball_type == y.ball_type && x.flags == y.flags
                && heading_at(e, a) == heading_at(e, b)
                && (heading_at(e, a) != session::kHeadingInline || session::same_camera(x, y));
        }
        // An entry without codes (built in memory) sends its cameras inline.
        static uint8_t heading_at(const entry& e, size_t i) {
            if (!e.headings.empty()) return e.headings[i];
            return session::zero_camera(session::camera_of(e.frames[i])) ? session::kHeadingNone : session::kHeadingInline;
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
