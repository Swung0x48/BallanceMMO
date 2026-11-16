#pragma once

// Fork detection, step FD1 (the fork detection design, 1.1,
// 6.3a, 14.1): the terms a client reports of its retired rows of a sample
// tick (every T % kSampleTicks == 0) and the server compares with its own
// rows of T.  Pure: no globals, no CK, no clock - the client core, the server
// and the tests compile the same functions, and scripts/world_check.py
// mirrors them (its --self-test holds the golden values of the unit test).
//
// A row's term is FNV-1a 64 fed field by field (never a struct, so padding
// and the compiler do not reach it; every target is little-endian), then
// world_hash's fold_term: kind u8 (0 ball, 1 mechanism), the key (a ball's
// owner u32; a mechanism's name length u16 and bytes), position 3 x f64,
// rotation 4 x f64, linear 3 x f32, angular 3 x f32 bit patterns, and the
// flags u8 (SIMULATED | COLLISION_ENABLED).  The bytes are equal exactly when
// journal_judge's packed <7d6fB> row is (-0.0 differs from +0.0 there too).
// A ball is keyed by its owner (the server's clone, the own ball and a retail
// mirror have different names); a mechanism by its level name.  Terms are
// summed (mod 2^64), so the order of the rows never matters.

#include "../entity/session.hpp"
#include "../physics/world_hash.hpp"
#include "resync_rules.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bmmo::session::world_check {
    constexpr uint32_t kSampleTicks = TICK_RATE;   // a sample every second, the FINAL / FULL rows' cadence
    constexpr int kBuckets = 8;                     // mechanism buckets: fnv1a64(name) & 7
    // A report held back while a resync is pending at most this long, then
    // sent with PENDING; the same span bounds the server's expected wait (2.3).
    constexpr uint32_t kPendingHoldTicks = 10 * TICK_RATE;
    // The reports of an open episode that carry rows, until it is decided
    // (LATER's 396 ticks: 6 samples; 14.0).
    constexpr uint32_t kRowReports = 6;

    // world_diff::kinds and the notices' kinds.
    constexpr uint8_t KIND_OWN = 1;        // the reporting member's own ball
    constexpr uint8_t KIND_BALL = 2;       // another player's ball
    constexpr uint8_t KIND_BUCKET = 4;     // a mechanism bucket
    constexpr uint8_t KIND_SET = 8;        // a ball only one side has, or the dup counts differ
    constexpr uint8_t KIND_ZONE = 16;      // the latched owner sets differ
    constexpr uint8_t KIND_PRESENCE = 32;  // set by the server from the rows: a body only one side has

    // The report's `why` (the retire observer's verdict, client_core.cpp):
    // 0 confirmed; 1-4 the normal reasons; 5-6 the expected own ones; 7 a
    // resync asked for (the fork detection design, 2.3).
    enum class why : uint8_t {
        none = 0,
        no_player,
        not_relayed,
        relayed_differs,
        inherited,
        own_notices,
        own_echo,
        resync_pending,
    };
    inline const char* why_text(why w) {
        switch (w) {
        case why::none: return "";
        case why::no_player: return "no player";
        case why::not_relayed: return "not relayed";
        case why::relayed_differs: return "differs from the relayed frame";
        case why::inherited: return "inherits an unconfirmed retired tick";
        case why::own_notices: return "pending: own substitution notices outstanding";
        case why::own_echo: return "pending: own lifecycle echo outstanding";
        case why::resync_pending: return "pending: a resync was asked for";
        }
        return "?";
    }

    // SessionWorldFork's fields (14.2); the names go into both journals and
    // the log lines.
    enum class fork_action : uint8_t { opened = 0, decided = 1, closed = 2 };
    enum class fork_tier : uint8_t { unsettled = 0, now = 1, soon = 2, later = 3 };
    // (`nearby`, not `near`: windows.h defines near and far as macros.)
    enum class fork_rule : uint8_t {
        none = 0, size, own, flags, breadth, calm_absent, calm_finished, nearby, distant, no_detail,
        first_presence, first_zone, latched, expected,
        classes,   // decided from the report's classes, the DETAIL answered (a report without rows)
    };
    enum class fork_outcome : uint8_t { none = 0, lead, self_healed, equal_again, ended };

    inline const char* action_name(fork_action a) {
        switch (a) {
        case fork_action::opened: return "opened";
        case fork_action::decided: return "decided";
        case fork_action::closed: return "closed";
        }
        return "?";
    }
    inline const char* tier_name(fork_tier t) {
        switch (t) {
        case fork_tier::unsettled: return "unsettled";
        case fork_tier::now: return "now";
        case fork_tier::soon: return "soon";
        case fork_tier::later: return "later";
        }
        return "?";
    }
    inline const char* rule_name(fork_rule r) {
        static constexpr const char* names[] = {"none", "size", "own", "flags", "breadth", "calm_absent",
                                                "calm_finished", "near", "distant", "no_detail", "first_presence",
                                                "first_zone", "latched", "expected", "classes"};
        const auto i = static_cast<size_t>(r);
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
    }
    inline const char* outcome_name(fork_outcome o) {
        switch (o) {
        case fork_outcome::none: return "none";
        case fork_outcome::lead: return "lead";
        case fork_outcome::self_healed: return "self_healed";
        case fork_outcome::equal_again: return "equal_again";
        case fork_outcome::ended: return "ended";
        }
        return "?";
    }

    inline uint64_t name_hash(std::string_view name) {
        bmmo::physics::fnv1a64 h;
        h.feed(name.data(), name.size());
        return h.value;
    }
    inline int bucket_of(std::string_view name) { return static_cast<int>(name_hash(name) & (kBuckets - 1)); }

    namespace detail {
        inline void feed_key(bmmo::physics::fnv1a64& h, body_kind kind, uint32_t owner, std::string_view name) {
            const uint8_t k = kind == body_kind::Ball ? 0 : 1;
            h.feed(k);
            if (kind == body_kind::Ball) {
                h.feed(owner);
            } else {
                const auto size = static_cast<uint16_t>(std::min<size_t>(name.size(), 0xFFFF));
                h.feed(size);
                h.feed(name.data(), size);
            }
        }
    }

    // The 1.1 term of one row.
    inline uint64_t row_term(const body_state& row) {
        bmmo::physics::fnv1a64 h;
        detail::feed_key(h, row.kind, row.owner, row.name);
        for (const double& v: row.position) h.feed(v);
        for (const double& v: row.rotation) h.feed(v);
        for (const float& v: row.linear) h.feed(v);
        for (const float& v: row.angular) h.feed(v);
        const uint8_t flags = row.flags & (BODY_FLAG_SIMULATED | BODY_FLAG_COLLISION_ENABLED);
        h.feed(flags);
        return bmmo::physics::fold_term(h.value);
    }

    struct world_terms {
        uint16_t rows = 0;     // rows hashed
        uint8_t unkeyed = 0;   // ball rows with owner 0, left out
        uint8_t dup = 0;       // owners with more than one row
        std::vector<std::pair<uint32_t, uint64_t>> balls;   // (owner, sum of its rows' terms), by owner
        uint64_t buckets[kBuckets] = {};
    };

    // The 1.2 rules: keep-local bodies out (resync_rules.hpp, the predicate
    // the resync and the judge use), the balls of `latched` owners out, a
    // ball of owner 0 counted unkeyed; two rows of one owner (a type switch's
    // old and new mirror) summed and counted in `dup`.  `skip` leaves out
    // more rows by name (the client's local-only bodies).
    template <class Skip>
    world_terms fold_rows(const std::vector<body_state>& rows, const std::vector<uint32_t>& latched, Skip skip) {
        world_terms out;
        std::vector<std::pair<uint32_t, uint32_t>> counts;   // (owner, rows), parallel to out.balls
        uint32_t hashed = 0, unkeyed = 0;
        for (const auto& row: rows) {
            if (keep_local_body(row.name) || skip(row.name)) continue;
            const uint64_t term = row_term(row);
            if (row.kind == body_kind::Mechanism) {
                out.buckets[bucket_of(row.name)] += term;
                ++hashed;
                continue;
            }
            if (row.owner == 0) { ++unkeyed; continue; }
            if (std::find(latched.begin(), latched.end(), row.owner) != latched.end()) continue;
            ++hashed;
            const auto it = std::lower_bound(out.balls.begin(), out.balls.end(), row.owner,
                                             [](const auto& entry, uint32_t owner) { return entry.first < owner; });
            const auto at = it - out.balls.begin();
            if (it != out.balls.end() && it->first == row.owner) {
                it->second += term;
                ++counts[at].second;
            } else {
                out.balls.insert(it, {row.owner, term});
                counts.insert(counts.begin() + at, {row.owner, 1u});
            }
        }
        out.rows = static_cast<uint16_t>(std::min<uint32_t>(hashed, 0xFFFF));
        out.unkeyed = static_cast<uint8_t>(std::min<uint32_t>(unkeyed, 0xFF));
        uint32_t dup = 0;
        for (const auto& [owner, n]: counts) if (n > 1) ++dup;
        out.dup = static_cast<uint8_t>(std::min<uint32_t>(dup, 0xFF));
        return out;
    }
    inline world_terms fold_rows(const std::vector<body_state>& rows, const std::vector<uint32_t>& latched) {
        return fold_rows(rows, latched, [](const std::string&) { return false; });
    }

    // One 64-bit fold of a report's terms (the journal's `fold`): equal
    // folds, equal reports.
    inline uint64_t terms_fold(const world_terms& t) {
        bmmo::physics::fnv1a64 h;
        h.feed(t.rows); h.feed(t.unkeyed); h.feed(t.dup);
        for (const auto& [owner, term]: t.balls) { h.feed(owner); h.feed(term); }
        for (uint64_t b: t.buckets) h.feed(b);
        return h.value;
    }

    // The per-tick fold (6.3a) of one row: its key and position only.
    inline uint64_t position_term(body_kind kind, uint32_t owner, std::string_view name, const double position[3]) {
        bmmo::physics::fnv1a64 h;
        detail::feed_key(h, kind, owner, name);
        for (int k = 0; k < 3; ++k) h.feed(position[k]);
        return bmmo::physics::fold_term(h.value);
    }
    // Sums of position_term per class: `mechanisms` and `balls` (keyed balls
    // only; keep-local bodies out).  The ring keeps the upper 32 bits of each
    // (position_fold).  Not filtered by the airship zone: the status is read
    // at sample ticks only, so an analyzer ignores the ball half where a ball
    // may be latched (6.3a's holes).
    struct position_sums {
        uint64_t mechanisms = 0, balls = 0;
        void add(body_kind kind, uint32_t owner, std::string_view name, const double position[3]) {
            if (keep_local_body(name)) return;
            if (kind == body_kind::Ball) {
                if (owner != 0) balls += position_term(kind, owner, name, position);
            } else {
                mechanisms += position_term(kind, owner, name, position);
            }
        }
        // mechanisms half << 32 | balls half.
        uint64_t fold() const { return (mechanisms & 0xFFFFFFFF00000000ULL) | (balls >> 32); }
    };
    inline uint64_t position_fold(const std::vector<body_state>& rows) {
        position_sums sums;
        for (const auto& row: rows) sums.add(row.kind, row.owner, row.name, row.position);
        return sums.fold();
    }

    struct world_diff {
        uint8_t kinds = 0;               // KIND_*
        std::vector<uint32_t> owners;    // the differing balls (sorted), the zone's included
        uint8_t buckets = 0;             // the differing bucket mask
        bool differs() const { return kinds != 0; }
    };

    // The server's terms of T against a member's report of T.  A ball latched
    // on one side only is a zone difference and its term is not compared; a
    // ball latched on both is out of both folds.  When the report has unkeyed
    // rows, a ball only the server has is not compared (it may be the
    // unkeyed one).
    inline world_diff compare_terms(const world_terms& server, const world_terms& client,
                                    const std::vector<uint32_t>& server_latched,
                                    const std::vector<uint32_t>& client_latched, uint32_t member) {
        world_diff d;
        auto contains = [](const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); };
        std::vector<uint32_t> zone;
        for (uint32_t o: server_latched) if (!contains(client_latched, o)) zone.push_back(o);
        for (uint32_t o: client_latched) if (!contains(server_latched, o) && !contains(zone, o)) zone.push_back(o);
        if (!zone.empty()) d.kinds |= KIND_ZONE;
        // A differing ball marks KIND_OWN (the member's own) or KIND_BALL;
        // one only one side has marks KIND_SET as well.
        auto mark = [&](uint32_t owner, bool missing) {
            if (contains(zone, owner)) return;
            d.kinds |= owner == member ? KIND_OWN : KIND_BALL;
            if (missing) d.kinds |= KIND_SET;
            d.owners.push_back(owner);
        };
        auto s = server.balls.begin();
        auto c = client.balls.begin();
        while (s != server.balls.end() || c != client.balls.end()) {
            if (c == client.balls.end() || (s != server.balls.end() && s->first < c->first)) {
                if (client.unkeyed == 0) mark(s->first, true);
                ++s;
            } else if (s == server.balls.end() || c->first < s->first) {
                mark(c->first, true);
                ++c;
            } else {
                if (s->second != c->second) mark(s->first, false);
                ++s;
                ++c;
            }
        }
        if (server.dup != client.dup) d.kinds |= KIND_SET;
        for (int b = 0; b < kBuckets; ++b)
            if (server.buckets[b] != client.buckets[b]) d.buckets |= static_cast<uint8_t>(1u << b);
        if (d.buckets) d.kinds |= KIND_BUCKET;
        for (uint32_t o: zone) d.owners.push_back(o);
        std::sort(d.owners.begin(), d.owners.end());
        d.owners.erase(std::unique(d.owners.begin(), d.owners.end()), d.owners.end());
        return d;
    }

    // The records' and the log lines' texts, both sides.
    inline std::string owner_list(const std::vector<uint32_t>& owners) {
        std::string out;
        for (uint32_t o: owners) out += (out.empty() ? "" : " ") + std::to_string(o);
        return out;
    }
    inline std::string hex64(uint64_t v) {
        char text[20];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
        return text;
    }
    inline std::string ball_key(uint32_t owner) { return "p" + std::to_string(owner); }
    inline std::string kinds_text(uint8_t kinds) {
        static constexpr std::pair<uint8_t, const char*> names[] = {
            {KIND_OWN, "own ball"}, {KIND_BALL, "balls"}, {KIND_BUCKET, "mechanisms"},
            {KIND_SET, "ball set"}, {KIND_ZONE, "airship zone"}, {KIND_PRESENCE, "presence"}};
        std::string out;
        for (const auto& [bit, name]: names)
            if (kinds & bit) out += std::string(out.empty() ? "" : ", ") + name;
        return out.empty() ? "nothing" : out;
    }
    // A body's name for a default-level line: printable ASCII only (a
    // client's row may carry any bytes), at most 64 characters.
    inline std::string printable(std::string_view name) {
        std::string out;
        for (char c: name.substr(0, 64)) out += c >= 0x20 && c < 0x7F ? c : '?';
        return out;
    }
    // "the ball of player 12" for "p12" (the own ball for the member's own),
    // a mechanism's name as it is.
    inline std::string body_text(const std::string& key, uint32_t own = 0) {
        if (key.size() > 1 && key[0] == 'p'
            && std::all_of(key.begin() + 1, key.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return own != 0 && key == ball_key(own) ? "the own ball" : "the ball of player " + key.substr(1);
        return printable(key);
    }
}
