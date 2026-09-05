#pragma once

// The session black box: a journal of everything a deterministic physics
// session consumed, so a bug that only happens in a live room can be replayed
// offline bit for bit.  The server writes what its physics_world was fed
// (kind 0); a client - the retail mod or the headless session client - writes
// the same shape plus what it received and how it corrected itself (kind 1).
// The format lives in BallanceMMOCommon because both sides write it and three
// readers parse it (the SimTool's --replay-session, scripts/journal_trace.py,
// the round-trip unit tests).
//
// File format, version 1.  Everything little-endian and written field by
// field, never a struct dump: the writer may be a Linux x64 server and the
// reader a Windows x86 client.
//
//   file   = magic "BMMOJRNL" (8 bytes) , u32 version , record*
//   record = u8 tag , u32 payload_size , payload[payload_size]
//   str    = u16 length , that many bytes (no terminator, <= 4096 bytes)
//
//   tag 0 HEADER      what the world was created with, and who wrote the file
//   tag 1 PLAYER      add_player / remove_player
//   tag 2 INPUT       one player's input_frame for one tick (repeat compressed)
//   tag 3 EVENT       a lifecycle event exactly as the world consumed it
//   tag 4 TICK        the fingerprint after the tick (physics::world_hash)
//   tag 5 CHECKPOINT  body poses (server FULL, client LOCAL / RECEIVED)
//   tag 6 NOTE        free text, and the structured milestones (start:, end:, ...)
//   tag 7 CORRECTION  a client's disagreement with an authoritative snapshot
//
// Every record but the header carries a tick as its first field, so a reader
// groups the file by tick without understanding a single tag.  The length
// prefix makes the format forward compatible in both directions: an old reader
// skips a tag it does not know (and counts it), and ignores trailing fields a
// newer writer appended to a payload it does know.
//
// Truncation is expected - the file left behind by a crash is the whole point
// of a black box.  A record cut off by the end of the file, an implausible
// record size, or a payload that does not parse ends the read: everything
// before it is kept and the rest of the file is reported as bytes_dropped.  A
// file that ends exactly after a complete record is not truncated.
//
// See docs/session-journal-plan.md and design 9.15 for the capture points.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "../entity/session.hpp"
#include "../physics/physics_rt_api.h"
#include "../physics/world_hash.hpp"

namespace bmmo::session {
    enum class journal_tag : uint8_t {
        header = 0, player = 1, input = 2, event = 3, tick = 4,
        checkpoint = 5, note = 6, correction = 7
    };
    enum class journal_kind : uint8_t { server = 0, client = 1 };

    constexpr uint32_t kJournalVersion = 1;
    constexpr char kJournalMagic[8] = {'B', 'M', 'M', 'O', 'J', 'R', 'N', 'L'};
    constexpr uint8_t kJournalMaxTag = static_cast<uint8_t>(journal_tag::correction);
    // A str never exceeds this; a longer one makes the record invalid.
    constexpr size_t kJournalMaxString = 4096;
    // A checkpoint never carries more bodies than this (about 600 KB of
    // payload); a bigger count means a corrupt file, not a bigger world.
    constexpr size_t kJournalMaxCheckpointBodies = 4096;
    // Sanity limit on one record's payload.  The largest record the writer can
    // produce is a full checkpoint, well under 1 MB; anything past this is
    // corruption, so the read ends there instead of allocating it.
    constexpr uint32_t kJournalMaxRecordBytes = 16u << 20;

    // INPUT `flags`
    constexpr uint8_t JOURNAL_INPUT_FRESH = 1;     // the writer had the player's own frame for this tick
    constexpr uint8_t JOURNAL_INPUT_RELAYED = 2;   // client: the frame came from the server's relay
    // CHECKPOINT `flags`
    constexpr uint8_t JOURNAL_CHECKPOINT_FULL = 1;       // server: every movable body, names present
    constexpr uint8_t JOURNAL_CHECKPOINT_LOCAL = 2;      // client: its own world
    constexpr uint8_t JOURNAL_CHECKPOINT_RECEIVED = 4;   // client: a snapshot as received

    struct journal_header {
        journal_kind kind = journal_kind::server;
        uint32_t session = 0;
        int32_t level = 0;
        int32_t seed = 0;
        float spawn_impulse = 0.0f;
        uint32_t input_delay = 0;
        uint32_t checkpoint_ticks = 0;
        uint32_t first_tick = 0;
        uint64_t anchor_hash = 0;
        uint64_t anchor_surfaces = 0;
        std::string build_id;
        uint64_t utc_ms = 0;         // wall clock when the header was written
        uint32_t own_player = 0;     // 0 in a server journal
        uint8_t own_join_order = 255;   // 255 in a server journal
    };

    struct journal_player {
        uint32_t tick = 0;
        uint32_t id = 0;
        uint8_t join_order = 0;
        bool added = true;
        std::string name;   // display name, may be empty
    };

    struct journal_input {
        uint32_t tick = 0;
        uint32_t id = 0;
        uint8_t flags = 0;      // JOURNAL_INPUT_*
        input_frame frame;
        bool repeat = false;    // as written: the frame equals the player's previous one
    };

    struct journal_event {
        // The tick the world consumed the event at: what the reader groups by,
        // and what a replay has to apply it at.
        uint32_t tick = 0;
        // The tick the event was stamped for by whoever sent it.  The server
        // applies an event that arrives late at the tick it has reached, not at
        // the one the client asked for, and the spawn impulse direction is
        // derived from the stamp - so a record that carried only one of the two
        // either applies the event at the wrong tick or kicks the ball the
        // wrong way.  Written verbatim, 0 included: a client at its anchor
        // frame really does stamp tick 0, and only an old file (no trailing
        // u32 at all) means "the applied tick".
        uint32_t event_tick = 0;
        uint32_t id = 0;
        event_type type = event_type::Physicalize;
        uint8_t ball_type = 0;
        uint8_t flags = 0;
        float position[3] = {};
        float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // world matrix rows
        int32_t sector = 0;
        std::string name;
        bmmo_physics_ball_recipe recipe{};   // zeroed for non-Physicalize events
    };

    struct journal_tick_record {
        uint32_t tick = 0;
        uint64_t hash = 0;
        uint64_t pose = 0;
        int32_t cores = 0;
        uint32_t ms = 0;        // milliseconds since the header's utc_ms
        std::string probe_name;
        double probe_position[3] = {};
        float probe_speed[3] = {};
    };

    struct journal_checkpoint {
        uint32_t tick = 0;
        uint8_t flags = 0;      // JOURNAL_CHECKPOINT_*
        std::vector<body_state> bodies;
    };

    struct journal_note {
        uint32_t tick = 0;
        std::string text;
    };

    // kind: 0 mismatch, 1 rollback, 2 hard, 3 blend, 4 resync, 5 too_far,
    // 6 frozen, 7 unmatched (see rollback.hpp's rollback_correction).
    struct journal_correction {
        uint32_t tick = 0;         // the snapshot's tick
        uint32_t local_tick = 0;   // where the client was when it learned
        uint8_t kind = 0;
        std::string entity;        // the worst body, may be empty
        float error_m = 0.0f;
        float velocity_error = 0.0f;
        double local_position[3] = {};
        double server_position[3] = {};
    };

    // Serialization helpers.  Explicit shifts and masks rather than a memcpy of
    // the whole value: this code only ever runs on little-endian x86/x64/ARM64,
    // but the file must not depend on that.
    namespace journal_detail {
        inline void write_u8(std::string& out, uint8_t v) { out.push_back(static_cast<char>(v)); }

        inline void write_u16(std::string& out, uint16_t v) {
            out.push_back(static_cast<char>(v & 0xffu));
            out.push_back(static_cast<char>((v >> 8) & 0xffu));
        }

        inline void write_u32(std::string& out, uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
        }

        inline void write_u64(std::string& out, uint64_t v) {
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
        }

        inline void write_i32(std::string& out, int32_t v) { write_u32(out, static_cast<uint32_t>(v)); }

        inline void write_f32(std::string& out, float v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            write_u32(out, bits);
        }

        inline void write_f64(std::string& out, double v) {
            uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            write_u64(out, bits);
        }

        inline void write_str(std::string& out, const std::string& v) {
            const size_t size = std::min(v.size(), kJournalMaxString);
            write_u16(out, static_cast<uint16_t>(size));
            out.append(v, 0, size);
        }

        // A fixed-size char array of the bridge structs: written as the str up
        // to the first NUL.
        inline void write_chars(std::string& out, const char* v, size_t capacity) {
            size_t size = 0;
            while (size < capacity && v[size] != '\0') ++size;
            write_str(out, std::string(v, size));
        }

        inline void write_f32_array(std::string& out, const float* v, size_t count) {
            for (size_t i = 0; i < count; ++i) write_f32(out, v[i]);
        }

        inline void write_f64_array(std::string& out, const double* v, size_t count) {
            for (size_t i = 0; i < count; ++i) write_f64(out, v[i]);
        }

        inline uint32_t load_u32(const char* data) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (8 * i);
            return v;
        }

        // Bounds-checked cursor over one record's payload.  Every read checks,
        // and the first failure latches `ok` so a parser can read a whole
        // record and test once at the end.
        struct cursor {
            const char* data = nullptr;
            size_t size = 0;
            size_t pos = 0;
            bool ok = true;

            explicit cursor(const std::string& payload) : data(payload.data()), size(payload.size()) {}

            bool take(size_t n) {
                if (!ok || n > size - pos) {
                    ok = false;
                    return false;
                }
                pos += n;
                return true;
            }

            uint8_t u8() {
                const size_t at = pos;
                if (!take(1)) return 0;
                return static_cast<uint8_t>(data[at]);
            }

            uint16_t u16() {
                const size_t at = pos;
                if (!take(2)) return 0;
                return static_cast<uint16_t>(static_cast<uint8_t>(data[at])
                                             | (static_cast<uint16_t>(static_cast<uint8_t>(data[at + 1])) << 8));
            }

            uint32_t u32() {
                const size_t at = pos;
                if (!take(4)) return 0;
                return load_u32(data + at);
            }

            uint64_t u64() {
                const size_t at = pos;
                if (!take(8)) return 0;
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                    v |= static_cast<uint64_t>(static_cast<uint8_t>(data[at + i])) << (8 * i);
                return v;
            }

            int32_t i32() { return static_cast<int32_t>(u32()); }

            float f32() {
                const uint32_t bits = u32();
                float v = 0.0f;
                std::memcpy(&v, &bits, sizeof(v));
                return v;
            }

            double f64() {
                const uint64_t bits = u64();
                double v = 0.0;
                std::memcpy(&v, &bits, sizeof(v));
                return v;
            }

            std::string str() {
                const uint16_t length = u16();
                if (!ok) return {};
                if (length > kJournalMaxString) {
                    ok = false;
                    return {};
                }
                const size_t at = pos;
                if (!take(length)) return {};
                return std::string(data + at, length);
            }

            // A str back into a fixed-size char array: truncated like snprintf
            // and always NUL terminated.
            void chars(char* out, size_t capacity) {
                const std::string value = str();
                if (!ok || capacity == 0) return;
                std::snprintf(out, capacity, "%s", value.c_str());
            }

            void f32_array(float* out, size_t count) {
                for (size_t i = 0; i < count; ++i) out[i] = f32();
            }

            void f64_array(double* out, size_t count) {
                for (size_t i = 0; i < count; ++i) out[i] = f64();
            }
        };

        // The bridge recipe in declaration order.  Counts are clamped on write
        // and validated on read: a forged count must never index past the
        // fixed arrays.
        inline void write_recipe(std::string& out, const bmmo_physics_ball_recipe& r) {
            write_u8(out, r.fixed ? 1 : 0);
            write_u8(out, r.start_frozen ? 1 : 0);
            write_u8(out, r.enable_collision ? 1 : 0);
            write_u8(out, r.calc_mass_center ? 1 : 0);
            write_f32(out, r.friction);
            write_f32(out, r.elasticity);
            write_f32(out, r.mass);
            write_f32(out, r.linear_damp);
            write_f32(out, r.rot_damp);
            write_f32_array(out, r.mass_center, 3);
            write_chars(out, r.collision_surface, BMMO_PHYSICS_NAME_SIZE);
            const int32_t convex = std::clamp(r.convex_count, 0, static_cast<int32_t>(BMMO_PHYSICS_MAX_CONVEX));
            write_i32(out, convex);
            for (int32_t i = 0; i < convex; ++i) write_chars(out, r.convex[i], BMMO_PHYSICS_NAME_SIZE);
            const int32_t balls = std::clamp(r.ball_count, 0, static_cast<int32_t>(BMMO_PHYSICS_MAX_BALLS));
            write_i32(out, balls);
            for (int32_t i = 0; i < balls; ++i) {
                write_f32_array(out, r.ball_center[i], 3);
                write_f32(out, r.ball_radius[i]);
            }
            const int32_t concave = std::clamp(r.concave_count, 0, static_cast<int32_t>(BMMO_PHYSICS_MAX_CONCAVE));
            write_i32(out, concave);
            for (int32_t i = 0; i < concave; ++i) write_chars(out, r.concave[i], BMMO_PHYSICS_NAME_SIZE);
        }

        inline bool read_recipe(cursor& in, bmmo_physics_ball_recipe& r) {
            r = {};
            r.fixed = in.u8() != 0;
            r.start_frozen = in.u8() != 0;
            r.enable_collision = in.u8() != 0;
            r.calc_mass_center = in.u8() != 0;
            r.friction = in.f32();
            r.elasticity = in.f32();
            r.mass = in.f32();
            r.linear_damp = in.f32();
            r.rot_damp = in.f32();
            in.f32_array(r.mass_center, 3);
            in.chars(r.collision_surface, BMMO_PHYSICS_NAME_SIZE);
            r.convex_count = in.i32();
            if (!in.ok || r.convex_count < 0 || r.convex_count > BMMO_PHYSICS_MAX_CONVEX) return false;
            for (int32_t i = 0; i < r.convex_count; ++i) in.chars(r.convex[i], BMMO_PHYSICS_NAME_SIZE);
            r.ball_count = in.i32();
            if (!in.ok || r.ball_count < 0 || r.ball_count > BMMO_PHYSICS_MAX_BALLS) return false;
            for (int32_t i = 0; i < r.ball_count; ++i) {
                in.f32_array(r.ball_center[i], 3);
                r.ball_radius[i] = in.f32();
            }
            r.concave_count = in.i32();
            if (!in.ok || r.concave_count < 0 || r.concave_count > BMMO_PHYSICS_MAX_CONCAVE) return false;
            for (int32_t i = 0; i < r.concave_count; ++i) in.chars(r.concave[i], BMMO_PHYSICS_NAME_SIZE);
            return in.ok;
        }

        inline void write_body(std::string& out, const body_state& b) {
            write_u8(out, static_cast<uint8_t>(b.kind));
            write_u32(out, b.owner);
            write_str(out, b.name);
            write_f64_array(out, b.position, 3);
            write_f64_array(out, b.rotation, 4);
            write_f32_array(out, b.linear, 3);
            write_f32_array(out, b.angular, 3);
            write_u8(out, b.flags);
        }

        inline bool read_body(cursor& in, body_state& b) {
            const uint8_t kind = in.u8();
            if (!in.ok || kind > static_cast<uint8_t>(body_kind::Mechanism)) return false;
            b.kind = static_cast<body_kind>(kind);
            b.owner = in.u32();
            b.name = in.str();
            in.f64_array(b.position, 3);
            in.f64_array(b.rotation, 4);
            in.f32_array(b.linear, 3);
            in.f32_array(b.angular, 3);
            b.flags = in.u8();
            return in.ok;
        }

        // "byte for byte identical" for the repeat flag, without comparing the
        // struct's padding.
        inline bool same_input_frame(const input_frame& a, const input_frame& b) {
            return a.keys == b.keys && a.ball_type == b.ball_type && a.flags == b.flags
                && std::memcmp(a.cam_right, b.cam_right, sizeof(a.cam_right)) == 0
                && std::memcmp(a.cam_up, b.cam_up, sizeof(a.cam_up)) == 0
                && std::memcmp(a.cam_dir, b.cam_dir, sizeof(a.cam_dir)) == 0;
        }

        inline void write_header_payload(std::string& out, const journal_header& h) {
            write_u8(out, static_cast<uint8_t>(h.kind));
            write_u32(out, h.session);
            write_i32(out, h.level);
            write_i32(out, h.seed);
            write_f32(out, h.spawn_impulse);
            write_u32(out, h.input_delay);
            write_u32(out, h.checkpoint_ticks);
            write_u32(out, h.first_tick);
            write_u64(out, h.anchor_hash);
            write_u64(out, h.anchor_surfaces);
            write_str(out, h.build_id);
            write_u64(out, h.utc_ms);
            write_u32(out, h.own_player);
            write_u8(out, h.own_join_order);
        }

        inline bool read_header_payload(const std::string& payload, journal_header& h) {
            cursor in(payload);
            const uint8_t kind = in.u8();
            if (!in.ok || kind > static_cast<uint8_t>(journal_kind::client)) return false;
            h.kind = static_cast<journal_kind>(kind);
            h.session = in.u32();
            h.level = in.i32();
            h.seed = in.i32();
            h.spawn_impulse = in.f32();
            h.input_delay = in.u32();
            h.checkpoint_ticks = in.u32();
            h.first_tick = in.u32();
            h.anchor_hash = in.u64();
            h.anchor_surfaces = in.u64();
            h.build_id = in.str();
            h.utc_ms = in.u64();
            h.own_player = in.u32();
            h.own_join_order = in.u8();
            return in.ok;
        }
    }

    // Writes the file as the session runs.  Every method is a no-op when the
    // file could not be opened or the size cap was reached, so a caller never
    // has to check: capture code must never be able to break a session.
    class journal_writer {
    public:
        journal_writer() = default;
        ~journal_writer() { close(); }
        journal_writer(const journal_writer&) = delete;
        journal_writer& operator=(const journal_writer&) = delete;

        // `max_bytes` == 0 means no limit.  The directory must exist.
        bool open(const std::filesystem::path& path, const journal_header& header, uint64_t max_bytes,
                  std::string& error) {
            close();
            last_input_.clear();
            capped_ = false;
            bytes_ = 0;
            last_tick_ = header.first_tick;
            max_bytes_ = max_bytes;
            path_ = path;
            stream_.open(path, std::ios::binary | std::ios::trunc);
            if (!stream_) {
                error = "cannot create " + path.string();
                path_.clear();
                return false;
            }
            stream_.write(kJournalMagic, sizeof(kJournalMagic));
            std::string version;
            journal_detail::write_u32(version, kJournalVersion);
            stream_.write(version.data(), static_cast<std::streamsize>(version.size()));
            bytes_ += sizeof(kJournalMagic) + version.size();
            std::string payload;
            journal_detail::write_header_payload(payload, header);
            write_record(journal_tag::header, payload);
            stream_.flush();
            if (!stream_) {
                error = "cannot write " + path.string();
                close();
                path_.clear();
                return false;
            }
            return true;
        }

        bool is_open() const { return stream_.is_open(); }
        bool capped() const { return capped_; }
        uint64_t bytes() const { return bytes_; }
        const std::filesystem::path& path() const { return path_; }

        void player(uint32_t tick, uint32_t id, uint8_t join_order, bool added, const std::string& name) {
            if (!writable()) return;
            last_tick_ = tick;
            std::string payload;
            journal_detail::write_u32(payload, tick);
            journal_detail::write_u32(payload, id);
            journal_detail::write_u8(payload, join_order);
            journal_detail::write_u8(payload, added ? 1 : 0);
            journal_detail::write_str(payload, name);
            record(journal_tag::player, payload);
        }

        // Writes the frame only when it differs from this player's previous
        // one; a repeat costs 10 bytes of payload.
        void input(uint32_t tick, uint32_t id, const input_frame& frame, uint8_t flags) {
            if (!writable()) return;
            last_tick_ = tick;
            auto previous = last_input_.find(id);
            const bool repeat = previous != last_input_.end()
                && journal_detail::same_input_frame(previous->second, frame);
            std::string payload;
            journal_detail::write_u32(payload, tick);
            journal_detail::write_u32(payload, id);
            journal_detail::write_u8(payload, repeat ? 1 : 0);
            journal_detail::write_u8(payload, flags);
            if (!repeat) {
                journal_detail::write_u8(payload, frame.keys);
                journal_detail::write_f32_array(payload, frame.cam_right, 3);
                journal_detail::write_f32_array(payload, frame.cam_up, 3);
                journal_detail::write_f32_array(payload, frame.cam_dir, 3);
                journal_detail::write_u8(payload, frame.ball_type);
                journal_detail::write_u8(payload, frame.flags);
                last_input_[id] = frame;
            }
            record(journal_tag::input, payload);
        }

        void event(const journal_event& e) {
            if (!writable()) return;
            last_tick_ = e.tick;
            std::string payload;
            journal_detail::write_u32(payload, e.tick);
            journal_detail::write_u32(payload, e.id);
            journal_detail::write_u8(payload, static_cast<uint8_t>(e.type));
            journal_detail::write_u8(payload, e.ball_type);
            journal_detail::write_u8(payload, e.flags);
            journal_detail::write_f32_array(payload, e.position, 3);
            journal_detail::write_f32_array(payload, e.rotation, 9);
            journal_detail::write_i32(payload, e.sector);
            journal_detail::write_str(payload, e.name);
            journal_detail::write_recipe(payload, e.recipe);
            // Appended after the recipe so a reader that predates the field
            // still parses the record and reads the stamp as the applied tick.
            // Verbatim: a stamp of 0 is a real stamp (a client's anchor frame),
            // and guessing here would kick that ball the wrong way on replay.
            journal_detail::write_u32(payload, e.event_tick);
            record(journal_tag::event, payload);
        }

        void tick(uint32_t tick, const bmmo::physics::world_hash& hash, uint32_t ms) {
            if (!writable()) return;
            last_tick_ = tick;
            std::string payload;
            journal_detail::write_u32(payload, tick);
            journal_detail::write_u64(payload, hash.hash);
            journal_detail::write_u64(payload, hash.pose);
            journal_detail::write_i32(payload, hash.cores);
            journal_detail::write_u32(payload, ms);
            journal_detail::write_chars(payload, hash.probe_name, sizeof(hash.probe_name));
            journal_detail::write_f64_array(payload, hash.probe_position, 3);
            journal_detail::write_f32_array(payload, hash.probe_speed, 3);
            record(journal_tag::tick, payload);
        }

        void checkpoint(uint32_t tick, uint8_t flags, const std::vector<body_state>& bodies) {
            if (!writable()) return;
            last_tick_ = tick;
            const size_t count = std::min(bodies.size(), kJournalMaxCheckpointBodies);
            std::string payload;
            journal_detail::write_u32(payload, tick);
            journal_detail::write_u8(payload, flags);
            journal_detail::write_u32(payload, static_cast<uint32_t>(count));
            for (size_t i = 0; i < count; ++i) journal_detail::write_body(payload, bodies[i]);
            record(journal_tag::checkpoint, payload);
        }

        void note(uint32_t tick, const std::string& text) {
            if (!writable()) return;
            last_tick_ = tick;
            record(journal_tag::note, note_payload(tick, text));
        }

        void correction(const journal_correction& c) {
            if (!writable()) return;
            last_tick_ = c.tick;
            std::string payload;
            journal_detail::write_u32(payload, c.tick);
            journal_detail::write_u32(payload, c.local_tick);
            journal_detail::write_u8(payload, c.kind);
            journal_detail::write_str(payload, c.entity);
            journal_detail::write_f32(payload, c.error_m);
            journal_detail::write_f32(payload, c.velocity_error);
            journal_detail::write_f64_array(payload, c.local_position, 3);
            journal_detail::write_f64_array(payload, c.server_position, 3);
            record(journal_tag::correction, payload);
        }

        void flush() {
            if (stream_.is_open()) stream_.flush();
        }

        void close() {
            if (!stream_.is_open()) return;
            stream_.flush();
            stream_.close();
        }

    private:
        bool writable() const { return stream_.is_open() && !capped_; }

        static std::string note_payload(uint32_t tick, const std::string& text) {
            std::string payload;
            journal_detail::write_u32(payload, tick);
            journal_detail::write_str(payload, text);
            return payload;
        }

        void write_record(journal_tag tag, const std::string& payload) {
            std::string head;
            journal_detail::write_u8(head, static_cast<uint8_t>(tag));
            journal_detail::write_u32(head, static_cast<uint32_t>(payload.size()));
            stream_.write(head.data(), static_cast<std::streamsize>(head.size()));
            stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            bytes_ += head.size() + payload.size();
        }

        void record(journal_tag tag, const std::string& payload) {
            const uint64_t size = 5 + payload.size();
            if (max_bytes_ != 0 && bytes_ + size > max_bytes_) {
                // The last thing in the file says why it stops; that note is
                // itself written past the cap so the file is never silently
                // truncated in the middle of a session.
                capped_ = true;
                write_record(journal_tag::note,
                             note_payload(last_tick_, "cap: journal_max_bytes reached, recording stopped"));
                stream_.flush();
                return;
            }
            write_record(tag, payload);
            // A crash must not cost more than the ticks since the last thing
            // that happened; TICK and INPUT ride along with the next flush.
            switch (tag) {
                case journal_tag::player:
                case journal_tag::event:
                case journal_tag::checkpoint:
                case journal_tag::note:
                case journal_tag::correction:
                    stream_.flush();
                    break;
                default:
                    break;
            }
        }

        std::ofstream stream_;
        std::filesystem::path path_;
        std::map<uint32_t, input_frame> last_input_;   // per player, for the repeat flag
        uint64_t max_bytes_ = 0;
        uint64_t bytes_ = 0;
        uint32_t last_tick_ = 0;
        bool capped_ = false;
    };

    // Records grouped by tick number (every record but the header carries
    // one).  A group exists for every tick with at least one record; `ticks`
    // is sorted ascending.
    struct journal_tick {
        uint32_t tick = 0;
        std::vector<journal_player> players;
        std::vector<journal_input> inputs;
        std::vector<journal_event> events;
        bool has_tick = false;
        // the tick's fingerprint; the last one when the file carries several
        // for this tick (a client renumbers its ticks at a resync)
        journal_tick_record record;
        std::vector<journal_checkpoint> checkpoints;
        std::vector<journal_note> notes;
        std::vector<journal_correction> corrections;
    };

    struct journal {
        journal_header header;
        // PLAYER adds with tick == header.first_tick written before the first
        // TICK record, in file order: the session's founding members.  They
        // are also in their tick group, together with any removal.
        std::vector<journal_player> initial_players;
        std::vector<journal_tick> ticks;
        std::vector<journal_note> notes;   // every NOTE, file order
        uint64_t records = 0;              // records read, header and unknown ones included
        uint64_t unknown_records = 0;
        uint64_t bytes_read = 0;
        uint64_t bytes_dropped = 0;
        std::string warning;               // truncation, unknown tags, a bad payload, duplicate TICKs
    };

    // Streams the file: the header is parsed here, every other record is handed
    // to `on_record` as raw payload bytes (return false to stop the scan).
    // Records with a tag this version does not know are counted, not delivered.
    // Returns false only when the magic, the version or the header record
    // cannot be read; a truncated tail is not an error, it is `bytes_dropped`
    // (which also counts everything from a record the callback stopped at).
    inline bool scan_journal(const std::filesystem::path& path, journal_header& header,
                             const std::function<bool(journal_tag, const std::string& payload)>& on_record,
                             std::string& error, uint64_t* bytes_dropped = nullptr, uint64_t* unknown = nullptr) {
        if (bytes_dropped) *bytes_dropped = 0;
        if (unknown) *unknown = 0;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "cannot open " + path.string();
            return false;
        }
        stream.seekg(0, std::ios::end);
        const std::streamoff end = stream.tellg();
        stream.seekg(0, std::ios::beg);
        if (end < 0 || !stream) {
            error = "cannot read " + path.string();
            return false;
        }
        const uint64_t file_size = static_cast<uint64_t>(end);

        char magic[sizeof(kJournalMagic)] = {};
        char version_bytes[4] = {};
        if (!stream.read(magic, sizeof(magic)) || std::memcmp(magic, kJournalMagic, sizeof(magic)) != 0) {
            error = "not a BMMO session journal: " + path.string();
            return false;
        }
        if (!stream.read(version_bytes, sizeof(version_bytes))) {
            error = "truncated journal header: " + path.string();
            return false;
        }
        const uint32_t file_version = journal_detail::load_u32(version_bytes);
        if (file_version != kJournalVersion) {
            error = "journal version " + std::to_string(file_version) + " (expected "
                  + std::to_string(kJournalVersion) + "): " + path.string();
            return false;
        }
        uint64_t consumed = sizeof(kJournalMagic) + 4;

        // tag + size + payload, or false when the file ends inside the record
        std::string payload;
        uint8_t tag = 0;
        auto next_record = [&]() {
            char head[5] = {};
            if (!stream.read(head, sizeof(head))) return false;
            const uint32_t size = journal_detail::load_u32(head + 1);
            if (size > kJournalMaxRecordBytes || size > file_size - consumed - sizeof(head)) return false;
            payload.assign(size, '\0');
            if (size != 0 && !stream.read(payload.data(), static_cast<std::streamsize>(size))) return false;
            tag = static_cast<uint8_t>(head[0]);
            return true;
        };

        if (!next_record() || tag != static_cast<uint8_t>(journal_tag::header)
                || !journal_detail::read_header_payload(payload, header)) {
            error = "unreadable journal header record: " + path.string();
            return false;
        }
        consumed += 5 + payload.size();

        while (true) {
            const uint64_t at = consumed;
            if (!next_record()) {
                if (bytes_dropped && file_size > at) *bytes_dropped = file_size - at;
                break;
            }
            consumed = at + 5 + payload.size();
            if (tag > kJournalMaxTag) {
                if (unknown) ++*unknown;
                continue;
            }
            if (!on_record(static_cast<journal_tag>(tag), payload)) {
                if (bytes_dropped && file_size > at) *bytes_dropped = file_size - at;
                break;
            }
        }
        return true;
    }

    // Reads a whole journal into memory, grouped by tick.  Returns false only
    // when the magic, the version or the header cannot be read; anything else
    // (a truncated tail, an unknown tag, a payload that does not parse) keeps
    // what was read and shows up in `warning` / `bytes_dropped`.
    inline bool read_journal(const std::filesystem::path& path, journal& out, std::string& error) {
        out = journal{};
        std::map<uint32_t, journal_tick> groups;
        std::map<uint32_t, input_frame> last_input;   // per player, to expand repeats
        bool seen_tick_record = false;
        uint64_t delivered = 0;
        uint64_t duplicate_ticks = 0;
        uint32_t first_duplicate_tick = 0;
        uint64_t parse_error_record = 0;   // 1-based position in the file, 0 = none
        int parse_error_tag = 0;

        uint64_t seen = 1;   // the header record
        auto on_record = [&](journal_tag tag, const std::string& payload) {
            ++seen;
            journal_detail::cursor in(payload);
            switch (tag) {
                case journal_tag::player: {
                    journal_player p;
                    p.tick = in.u32();
                    p.id = in.u32();
                    p.join_order = in.u8();
                    p.added = in.u8() != 0;
                    p.name = in.str();
                    if (!in.ok) break;
                    // founding members only: a removal before the first TICK
                    // (somebody quit while the room was still loading) is in
                    // its tick group, not in the member list
                    if (!seen_tick_record && p.added && p.tick == out.header.first_tick)
                        out.initial_players.push_back(p);
                    auto& group = groups[p.tick];
                    group.tick = p.tick;
                    group.players.push_back(std::move(p));
                    ++delivered;
                    return true;
                }
                case journal_tag::input: {
                    journal_input i;
                    i.tick = in.u32();
                    i.id = in.u32();
                    i.repeat = in.u8() != 0;
                    i.flags = in.u8();
                    if (i.repeat) {
                        auto previous = last_input.find(i.id);
                        // a repeat without a previous frame means a lost
                        // record: keep the (zeroed) frame rather than guessing
                        if (previous != last_input.end()) i.frame = previous->second;
                    } else {
                        i.frame.keys = in.u8();
                        in.f32_array(i.frame.cam_right, 3);
                        in.f32_array(i.frame.cam_up, 3);
                        in.f32_array(i.frame.cam_dir, 3);
                        i.frame.ball_type = in.u8();
                        i.frame.flags = in.u8();
                        if (!in.ok) break;
                        last_input[i.id] = i.frame;
                    }
                    if (!in.ok) break;
                    auto& group = groups[i.tick];
                    group.tick = i.tick;
                    group.inputs.push_back(std::move(i));
                    ++delivered;
                    return true;
                }
                case journal_tag::event: {
                    journal_event e;
                    e.tick = in.u32();
                    e.id = in.u32();
                    e.type = static_cast<event_type>(in.u8());
                    e.ball_type = in.u8();
                    e.flags = in.u8();
                    in.f32_array(e.position, 3);
                    in.f32_array(e.rotation, 9);
                    e.sector = in.i32();
                    e.name = in.str();
                    if (!in.ok || !journal_detail::read_recipe(in, e.recipe)) break;
                    // A file written before the stamp was split off the applied
                    // tick has the two the same way round for everything but a
                    // late event, which it cannot express.
                    e.event_tick = in.size - in.pos >= 4 ? in.u32() : e.tick;
                    if (!in.ok) break;
                    auto& group = groups[e.tick];
                    group.tick = e.tick;
                    group.events.push_back(std::move(e));
                    ++delivered;
                    return true;
                }
                case journal_tag::tick: {
                    journal_tick_record t;
                    t.tick = in.u32();
                    t.hash = in.u64();
                    t.pose = in.u64();
                    t.cores = in.i32();
                    t.ms = in.u32();
                    t.probe_name = in.str();
                    in.f64_array(t.probe_position, 3);
                    in.f32_array(t.probe_speed, 3);
                    if (!in.ok) break;
                    auto& group = groups[t.tick];
                    group.tick = t.tick;
                    // A client journal renumbers its ticks at a resync, so the
                    // same tick can carry two fingerprints.  The group holds
                    // one: keep the last (the world that went on running) and
                    // say in `warning` that an earlier one was dropped.
                    if (group.has_tick) {
                        if (duplicate_ticks++ == 0) first_duplicate_tick = t.tick;
                    }
                    group.has_tick = true;
                    group.record = std::move(t);
                    seen_tick_record = true;
                    ++delivered;
                    return true;
                }
                case journal_tag::checkpoint: {
                    journal_checkpoint c;
                    c.tick = in.u32();
                    c.flags = in.u8();
                    const uint32_t count = in.u32();
                    if (!in.ok || count > kJournalMaxCheckpointBodies) break;
                    c.bodies.resize(count);
                    bool ok = true;
                    for (uint32_t i = 0; i < count && ok; ++i) ok = journal_detail::read_body(in, c.bodies[i]);
                    if (!ok) break;
                    auto& group = groups[c.tick];
                    group.tick = c.tick;
                    group.checkpoints.push_back(std::move(c));
                    ++delivered;
                    return true;
                }
                case journal_tag::note: {
                    journal_note n;
                    n.tick = in.u32();
                    n.text = in.str();
                    if (!in.ok) break;
                    auto& group = groups[n.tick];
                    group.tick = n.tick;
                    group.notes.push_back(n);
                    out.notes.push_back(std::move(n));
                    ++delivered;
                    return true;
                }
                case journal_tag::correction: {
                    journal_correction c;
                    c.tick = in.u32();
                    c.local_tick = in.u32();
                    c.kind = in.u8();
                    c.entity = in.str();
                    c.error_m = in.f32();
                    c.velocity_error = in.f32();
                    in.f64_array(c.local_position, 3);
                    in.f64_array(c.server_position, 3);
                    if (!in.ok) break;
                    auto& group = groups[c.tick];
                    group.tick = c.tick;
                    group.corrections.push_back(std::move(c));
                    ++delivered;
                    return true;
                }
                case journal_tag::header:
                default:
                    break;
            }
            // `seen` skips the unknown records scan_journal never delivers:
            // the file position is only known once the scan is over.
            parse_error_record = seen;
            parse_error_tag = static_cast<int>(tag);
            return false;
        };

        uint64_t unknown = 0;
        if (!scan_journal(path, out.header, on_record, error, &out.bytes_dropped, &unknown)) return false;
        out.unknown_records = unknown;
        out.records = 1 + delivered + unknown;
        out.ticks.reserve(groups.size());
        for (auto& entry: groups) out.ticks.push_back(std::move(entry.second));

        std::error_code ec;
        const uintmax_t file_size = std::filesystem::file_size(path, ec);
        if (!ec && file_size >= out.bytes_dropped) out.bytes_read = file_size - out.bytes_dropped;
        auto warn = [&](const std::string& text) {
            if (!out.warning.empty()) out.warning += "; ";
            out.warning += text;
        };
        if (parse_error_record != 0)
            warn("record " + std::to_string(parse_error_record + unknown) + " (tag "
               + std::to_string(parse_error_tag) + ") does not parse");
        if (out.bytes_dropped != 0) warn("truncated: " + std::to_string(out.bytes_dropped) + " bytes dropped");
        if (out.unknown_records != 0) warn(std::to_string(out.unknown_records) + " unknown records skipped");
        if (duplicate_ticks != 0)
            warn(std::to_string(duplicate_ticks) + " duplicate TICK records (first at tick "
               + std::to_string(first_duplicate_tick) + ", kept the last of each)");
        return true;
    }
}
