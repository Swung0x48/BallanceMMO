#ifndef BALLANCEMMOSERVER_SESSION_WORLD_REPORT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_WORLD_REPORT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "../entity/session.hpp"
#include "../session/resync_state_codec.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace bmmo {
    // client -> server, reliable (protocol 42, fork detection step FD1,
    // the fork detection design, 14.2): the terms of the
    // member's retired rows of a sample tick (session/world_check.hpp), once
    // per sample at its retirement, and once more at once (DETAIL) answering a
    // SessionWorldFork `opened`.  The server compares them with its own rows
    // of the tick; nothing here writes any world.
    //
    //   schema u8, session u32, epoch u32, tick u32, flags u8, why u8, rows u16,
    //   unkeyed u8, dup u8, u8 n x (owner u32, term u64), 8 x bucket u64,
    //   u8 n x latched owner u32,
    //   (schema 2, protocol 43, fork heal) heal_x u32, held (u8 has, u32 tick,
    //   u32 check), last_full u32, k u8, HEAL_MISSED: missed_x u32, missed u8,
    //   ROWS:   u16 n x row (the journal's row layout: kind u8, owner u32,
    //           name str16, position 3 f64, rotation 4 f64, linear 3 f32,
    //           angular 3 f32, flags u8),
    //   DETAIL: episode u32, fold_first u32, u8 n x (mechanisms u32, balls u32),
    //           u8 n x rewind (anchor u32, at u32, cause u8, depth u16, entity str16),
    //           resyncs u16 (asked in the window ending at T)
    // A DETAIL answer repeats T's terms; one whose sample had left the ring
    // has rows 0, no balls and no ROWS section.
    struct session_world_report_msg : public serializable_message {
        static constexpr uint8_t kSchema = 2;
        static constexpr uint8_t FLAG_UNCONFIRMED = 1;   // the observer's verdict was not CONFIRMED (why != 0)
        static constexpr uint8_t FLAG_PENDING = 2;       // sent while a resync is pending, past the 660-tick hold
        static constexpr uint8_t FLAG_DETAIL = 4;        // the answer to `opened`: the DETAIL section follows
        static constexpr uint8_t FLAG_ROWS = 8;          // the rows section follows
        static constexpr uint8_t FLAG_HEAL_MISSED = 16;  // a fork heal's state could not be applied (missed_x, missed)
        static constexpr uint8_t FLAG_PRE_HEAL = 32;     // T <= heal_x: a record the in-place heal retired, not compared
        // Why a fork heal's state was not applied (HEAL_MISSED).
        enum class missed_reason : uint8_t {
            left_history = 0, light, resync_pending, other_epoch, waiting, refused, undecodable, count
        };
        static const char* missed_name(uint8_t r) {
            static constexpr const char* names[] = {"left_history", "light", "resync_pending", "other_epoch", "waiting", "refused",
                                                    "undecodable"};
            return r < static_cast<uint8_t>(missed_reason::count) ? names[r] : "?";
        }
        static constexpr size_t kMaxRows = 4096;
        static constexpr size_t kMaxName = 256;

        struct rewind_entry {
            uint32_t anchor = 0, at = 0;
            uint8_t cause = 0;     // session::rewind_cause
            uint16_t depth = 0;    // the ticks it re-simulated
            std::string entity;    // the body that asked (may be empty)
        };

        uint32_t session = 0;
        uint32_t epoch = 0;        // the base of the member's current assignment
        uint32_t tick = 0;         // the sample tick T
        uint8_t flags = 0;
        uint8_t why = 0;           // session::world_check::why
        uint16_t rows = 0;
        uint8_t unkeyed = 0, dup = 0;
        std::vector<std::pair<uint32_t, uint64_t>> balls;   // by owner
        uint64_t buckets[8] = {};
        std::vector<uint32_t> latched;                      // the owners latched in the airship zone at T
        uint32_t heal_x = 0;                                // the X of the last in-place heal applied (0 none)
        session::resync_ref_id held;                        // the resync state the member holds (the heal's reference)
        uint32_t last_full = 0;                             // the last full snapshot it had (a fallback's)
        uint8_t k = 0;                                      // its sparse capture: a whole record every k-th tick
        uint32_t missed_x = 0;                              // HEAL_MISSED: the X that could not be applied
        uint8_t missed = 0;                                 // HEAL_MISSED: missed_reason
        std::vector<session::body_state> body_rows;         // ROWS
        uint32_t episode = 0;                               // DETAIL
        uint32_t fold_first = 0;                            // DETAIL: the tick of folds[0]
        std::vector<std::pair<uint32_t, uint32_t>> folds;   // DETAIL: per tick (mechanisms, balls)
        std::vector<rewind_entry> rewinds;                  // DETAIL: the last rewinds, oldest first
        uint16_t resyncs = 0;                               // DETAIL: resyncs asked in the window ending at T

        session_world_report_msg() : serializable_message(bmmo::SessionWorldReport) {}

        bool serialize() override {
            serializable_message::serialize();
            uint8_t f = static_cast<uint8_t>(flags & ~(FLAG_ROWS));
            if (!body_rows.empty()) f |= FLAG_ROWS;
            auto put = [this](const auto& v) { raw.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
            auto count8 = [](size_t n) { return static_cast<uint8_t>(std::min<size_t>(n, 0xFF)); };
            auto str = [this](const std::string& s) {
                message_utils::write_string<uint16_t>(s.size() > kMaxName ? s.substr(0, kMaxName) : s, raw);
            };
            put(kSchema); put(session); put(epoch); put(tick); put(f); put(why); put(rows); put(unkeyed); put(dup);
            const uint8_t nb = count8(balls.size());
            put(nb);
            for (size_t i = 0; i < nb; ++i) { put(balls[i].first); put(balls[i].second); }
            for (uint64_t b: buckets) put(b);
            const uint8_t nl = count8(latched.size());
            put(nl);
            for (size_t i = 0; i < nl; ++i) put(latched[i]);
            const uint8_t has = held.held ? 1 : 0;
            put(heal_x); put(has); put(held.tick); put(held.check); put(last_full); put(k);
            if (f & FLAG_HEAL_MISSED) { put(missed_x); put(missed); }
            if (f & FLAG_ROWS) {
                const auto nr = static_cast<uint16_t>(std::min(body_rows.size(), kMaxRows));
                put(nr);
                for (size_t i = 0; i < nr; ++i) {
                    const auto& r = body_rows[i];
                    const auto kind = static_cast<uint8_t>(r.kind);
                    put(kind); put(r.owner); str(r.name);
                    for (double v: r.position) put(v);
                    for (double v: r.rotation) put(v);
                    for (float v: r.linear) put(v);
                    for (float v: r.angular) put(v);
                    put(r.flags);
                }
            }
            if (f & FLAG_DETAIL) {
                put(episode); put(fold_first);
                const uint8_t nf = count8(folds.size());
                put(nf);
                for (size_t i = 0; i < nf; ++i) { put(folds[i].first); put(folds[i].second); }
                const uint8_t nw = count8(rewinds.size());
                put(nw);
                for (size_t i = 0; i < nw; ++i) {
                    const auto& w = rewinds[i];
                    put(w.anchor); put(w.at); put(w.cause); put(w.depth); str(w.entity);
                }
                put(resyncs);
            }
            return raw.good();
        }

        // Every count is bounded by the bytes left, and a message with bytes
        // after its last section is refused (it comes from a client).
        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            auto get = [this](auto& v) { return message_utils::read_variable(raw, &v); };
            auto fits = [this](size_t n, size_t each) {
                return static_cast<size_t>(message_utils::bytes_remaining(raw)) >= n * each;
            };
            auto str = [this](std::string& s) { return message_utils::read_string<uint16_t>(raw, s) && s.size() <= kMaxName; };
            uint8_t schema = 0;
            if (!get(schema) || schema != kSchema) return false;
            if (!(get(session) && get(epoch) && get(tick) && get(flags) && get(why) && get(rows) && get(unkeyed) && get(dup)))
                return false;
            uint8_t nb = 0;
            if (!get(nb) || !fits(nb, 12)) return false;
            balls.resize(nb);
            for (auto& [owner, term]: balls) if (!get(owner) || !get(term)) return false;
            for (uint64_t& b: buckets) if (!get(b)) return false;
            uint8_t nl = 0;
            if (!get(nl) || !fits(nl, 4)) return false;
            latched.resize(nl);
            for (uint32_t& o: latched) if (!get(o)) return false;
            uint8_t has = 0;
            if (!(get(heal_x) && get(has) && has <= 1 && get(held.tick) && get(held.check) && get(last_full) && get(k)))
                return false;
            held.held = has != 0;
            if ((flags & FLAG_HEAL_MISSED) && !(get(missed_x) && get(missed))) return false;
            body_rows.clear();
            if (flags & FLAG_ROWS) {
                uint16_t nr = 0;
                if (!get(nr) || nr > kMaxRows || !fits(nr, 88)) return false;
                body_rows.resize(nr);
                for (auto& r: body_rows) {
                    uint8_t kind = 0;
                    if (!get(kind) || kind > static_cast<uint8_t>(session::body_kind::Mechanism)) return false;
                    r.kind = static_cast<session::body_kind>(kind);
                    if (!get(r.owner) || !str(r.name)) return false;
                    for (double& v: r.position) if (!get(v)) return false;
                    for (double& v: r.rotation) if (!get(v)) return false;
                    for (float& v: r.linear) if (!get(v)) return false;
                    for (float& v: r.angular) if (!get(v)) return false;
                    if (!get(r.flags)) return false;
                }
            }
            folds.clear();
            rewinds.clear();
            if (flags & FLAG_DETAIL) {
                uint8_t nf = 0;
                if (!get(episode) || !get(fold_first) || !get(nf) || !fits(nf, 8)) return false;
                folds.resize(nf);
                for (auto& [m, b]: folds) if (!get(m) || !get(b)) return false;
                uint8_t nw = 0;
                if (!get(nw) || !fits(nw, 13)) return false;
                rewinds.resize(nw);
                for (auto& w: rewinds)
                    if (!(get(w.anchor) && get(w.at) && get(w.cause) && get(w.depth) && str(w.entity))) return false;
                if (!get(resyncs)) return false;
            }
            return message_utils::bytes_remaining(raw) == 0;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_WORLD_REPORT_MSG_HPP
