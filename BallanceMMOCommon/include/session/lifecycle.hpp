#pragma once

// The two lifecycle judgements the physics session makes about events, kept as
// pure functions so the unit tests can exercise them without linking the
// simulation (the test target has no physics).
//
//   physicalize_pending    - the server's lifecycle barrier: is a Physicalize
//                            for this player already queued within reach?
//   lifecycle_acceptance   - protocol 9, late-player design 3.1: the tick the
//                            server applies an event at, max(stamp, N), and
//                            whether a birth was overtaken by a newer event;
//                            the order and the guard every receiver uses on
//                            the origin; the leaver's Unphysicalize.
//   repeat_event_duplicate - the intake guard: has this exact repeat event
//                            (BodyRevived) been accepted recently?
//   event_seen / event_repeats - protocol 6: every lifecycle event is sent
//                            reliably and then repeated unreliably on the next
//                            ticks; the first arrival counts.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../entity/session.hpp"

namespace bmmo::session {
    // True when `events` holds a Physicalize for `player` applied no later
    // than `horizon`.  The range may be any container whose elements expose
    // .player, .applied and .event.type (session_runner::pending_event does).
    // Since protocol 9 an event is queued at its applied tick, never before
    // its stamp, and a late one at the session's next tick, which is inside
    // any horizon the barrier asks for.
    //
    // The barrier used to ask for `tick`, which is too strict: the client
    // stamps a lifecycle event one tick past the frame that reports it
    // (physics_session_client.cpp:1135/1230 use current_tick() + 1) and the
    // server runs `input_delay` ticks behind the client frame it consumes, so
    // an event that belongs to the frame at `tick` is stamped at most
    // tick + input_delay + 1.  Asking for that horizon does not apply anything
    // earlier - the consumer still takes e.tick <= tick - it only stops the
    // barrier from burning a full second of wall clock on an event that is
    // already queued.
    template <typename EventRange>
    inline bool physicalize_pending(const EventRange& events, uint32_t player, uint32_t horizon) {
        for (const auto& e: events)
            if (e.player == player && e.applied <= horizon && e.event.type == event_type::Physicalize) return true;
        return false;
    }

    // The tick before whose step the runner takes a leaver out of its world
    // (late-player design 3.2, review of step 16): its events accepted and
    // relayed but not applied yet - a birth stamped ahead, a Sector stamped
    // sector_lead ahead, the leaver's own Unphysicalize - are applied by every
    // receiver at their applied ticks, so the server keeps the player until
    // the tick after the last of them and applies them too.  `events` holds
    // the session's queue (.player, .applied) with the leaver's Unphysicalize
    // already in it.  `next_tick` when nothing of the player is queued: out
    // at once, as before.
    template <class EventRange>
    inline uint32_t lifecycle_leaver_removal_tick(const EventRange& events, uint32_t player, uint32_t next_tick) {
        uint32_t at = next_tick;
        for (const auto& e: events)
            if (e.player == player && static_cast<int32_t>(e.applied + 1 - at) > 0) at = e.applied + 1;
        return at;
    }

    // One accepted repeat event, keyed by what repeats: the body, not the
    // stamp.  Measured on the production journals: one burst of the same body
    // carries stamps up to 18 ticks apart, so a key containing the stamp would
    // never match.
    struct repeat_event_key {
        uint32_t tick = 0;
        uint8_t ball_type = 0;
        std::string name;
    };

    // ~1.3 s at 66 ms/tick: comfortably wider than the 18-tick span measured
    // inside one repeat burst, narrow enough not to swallow a real re-report.
    inline constexpr int32_t kRepeatEventWindow = 90;
    // Per-connection ring of accepted repeat events.
    inline constexpr size_t kRepeatEventRing = 64;

    // True when `recent` already holds the same (ball_type, name) within
    // `window` ticks of `tick`.  The age is compared as a signed difference:
    // a late event's stamp is *older* than the entries around it, so the ring
    // cannot be assumed newest-last in stamp order.
    inline bool repeat_event_duplicate(const std::vector<repeat_event_key>& recent, uint8_t ball_type,
                                       const std::string& name, uint32_t tick,
                                       int32_t window = kRepeatEventWindow) {
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            if (it->ball_type != ball_type || it->name != name) continue;
            if (std::abs(static_cast<int32_t>(tick - it->tick)) <= window) return true;
        }
        return false;
    }

    // Append an accepted repeat event to the ring, evicting the oldest.
    inline void record_repeat_event(std::vector<repeat_event_key>& recent, uint8_t ball_type,
                                    const std::string& name, uint32_t tick) {
        recent.push_back({tick, ball_type, name});
        if (recent.size() > kRepeatEventRing) recent.erase(recent.begin());
    }

    // ------------------------------------------------------------------
    // A lifecycle event on the wire more than once (protocol 6).
    //
    // The events (Physicalize, Unphysicalize, Sector, BodyRevived) travelled
    // reliable and ordered.  Under 10 % loss that made a lost event ~400 ms
    // late by retransmit - 20-40 ticks, past the rollback's 48-tick reach -
    // so a respawn or a beam applied that late left the mirror 10-30 m from
    // where the server has the ball until the next forced snapshot.  So, as
    // with the input relay, the reliable copy is backed by unreliable ones:
    // the sender repeats the same bytes on each of the next
    // kLifecycleEventRepeats ticks (one per tick, each in its own packet),
    // and every receiver takes the first arrival of a (player, stamp, type,
    // ball type, sector, name) and ignores the rest.  Nothing needs the
    // copies in order: the server applies events by stamp in join order,
    // the headless client queues them by stamp and rewinds for a late one,
    // and the retail mod applies a batch in stamp order and drops a
    // lifecycle event older than the one it already applied for that player.
    //
    // Protocol 9 (late-player design 3.1): `tick` is the event's ORIGIN, the
    // tick its sender stamped it for, and `applied` the tick the server
    // applies it at.  Keyed on the applied tick alone, two same-type births
    // re-stamped into one tick by an outage (a trafo wood -> stone -> wood)
    // would be one key and the second dropped as a copy; the origin tells them
    // apart, and every copy of one event carries the same applied tick.
    struct lifecycle_event_key {
        uint32_t tick = 0;      // the origin
        uint32_t applied = 0;
        uint32_t player = 0;
        uint8_t type = 0;
        uint8_t ball_type = 0;
        int32_t sector = 0;
        std::string name;

        bool operator<(const lifecycle_event_key& o) const {
            if (tick != o.tick) return tick < o.tick;
            if (applied != o.applied) return applied < o.applied;
            if (player != o.player) return player < o.player;
            if (type != o.type) return type < o.type;
            if (ball_type != o.ball_type) return ball_type < o.ball_type;
            if (sector != o.sector) return sector < o.sector;
            return name < o.name;
        }
        bool operator==(const lifecycle_event_key& o) const {
            return tick == o.tick && applied == o.applied && player == o.player && type == o.type
                && ball_type == o.ball_type && sector == o.sector && name == o.name;
        }
    };

    // The key of a session_event_msg (or anything with .tick, .origin_tick,
    // .type, .ball_type, .sector, .name) as sent by `player` (0 = the
    // connection on the uplink, where the server keys by the connection
    // instead).  `.tick` is the applied tick and `.origin_tick` the origin; a
    // client sends the two equal.
    template <class Msg>
    inline lifecycle_event_key lifecycle_key_of(const Msg& m, uint32_t player) {
        lifecycle_event_key k;
        k.tick = m.origin_tick;
        k.applied = m.tick;
        k.player = player;
        k.type = static_cast<uint8_t>(m.type);
        k.ball_type = m.ball_type;
        k.sector = m.sector;
        k.name = m.name;
        return k;
    }

    // Copies of an event sent after the reliable one, and how many ticks
    // apart: six copies two ticks apart span twelve ticks (180 ms), longer
    // than a burst of a dozen datagrams (measured: six copies one tick apart
    // were all inside such a burst, and the reliable copy carried the event
    // late again).
    inline constexpr int kLifecycleEventRepeats = 6;
    inline constexpr int kLifecycleEventRepeatSpacing = 2;

    // The order two events of one player with one stamp are applied in: the
    // Unphysicalize before the Physicalize (a beam sends both at one tick), the
    // rest after.  The ordered stream used to guarantee the sender's order;
    // with copies that can overtake each other, every receiver sorts a stamp's
    // events by this rank (the server's runner, the headless client's drain,
    // the retail mod's batch) - and ignores an Unphysicalize stamped at or
    // before the player's last applied Physicalize, which a late copy of a
    // death would otherwise take out the respawn with.
    inline int lifecycle_event_rank(event_type t) {
        if (t == event_type::Unphysicalize) return 0;
        if (t == event_type::Physicalize) return 1;
        return 2;
    }

    // ------------------------------------------------------------------
    // Protocol 9: the re-stamp at acceptance (late-player design 3.1).
    //
    // The server takes an event on its simulation thread, where the
    // session's next tick N is exact, and fixes applied = max(stamp, N): an
    // event is never applied before its stamp, and one that arrived after the
    // session passed its stamp is applied at the first tick still to come,
    // the same tick at the server and at every receiver.  The stamp travels
    // beside it as the ORIGIN, and everything that decides between two events
    // of one player - the order inside a tick, the guard against an older
    // death, the dedupe key - uses the origin: two events of one uplink
    // outage re-stamped into one tick keep the order their owner made them in.

    // The tick an event stamped `origin` is applied at when the session's next
    // tick is `next_tick`.  Signed, as every tick comparison here: a stamp far
    // in the past must not read as one far in the future.
    inline uint32_t lifecycle_applied_tick(uint32_t origin, uint32_t next_tick) {
        return static_cast<int32_t>(origin - next_tick) >= 0 ? origin : next_tick;
    }

    // The order of one tick's events, at the server and at every receiver:
    // join rank, then origin, then lifecycle_event_rank (the Unphysicalize of
    // a beam pair before its Physicalize).  True when (a) goes first.
    inline bool lifecycle_apply_before(uint32_t join_a, uint32_t origin_a, event_type type_a,
                                       uint32_t join_b, uint32_t origin_b, event_type type_b) {
        if (join_a != join_b) return join_a < join_b;
        if (origin_a != origin_b) return static_cast<int32_t>(origin_a - origin_b) < 0;
        return lifecycle_event_rank(type_a) < lifecycle_event_rank(type_b);
    }

    // The guard every side keeps per player: an Unphysicalize whose origin is
    // at or before the origin of the player's last applied Physicalize is a
    // copy that lost its place (protocol 6) or a death the respawn already
    // replaced, and would take the newer ball out.  On origins, never on the
    // applied tick: a death stamped after a re-stamped birth is applied in the
    // same tick as that birth or after it, and judged on the applied ticks it
    // could read as no newer than the birth, leaving the ball alive.
    inline bool lifecycle_unphysicalize_superseded(uint32_t origin, bool have_physicalize,
                                                   uint32_t last_physicalize_origin) {
        return have_physicalize && static_cast<int32_t>(origin - last_physicalize_origin) <= 0;
    }

    // How many of a player's accepted Physicalize and Unphysicalize events the
    // acceptance remembers for the superseded-birth rule.  Design 3.1 says
    // the last 128 ticks, measured at acceptance; bounded by count instead
    // (review of step 16): a birth whose every copy was lost and whose
    // reliable retransmit lands seconds after the newer death would, past an
    // age bound, be applied at N and bring a ghost ball back at the server
    // and the headless remotes (the retail mod's older-birth guard drops it,
    // so they would disagree).  The rule is right at any age; the count only
    // bounds the memory.
    inline constexpr size_t kLifecycleAcceptanceEvents = 16;

    // The server's acceptance of lifecycle events, per player: the re-stamp
    // of every type, and for Physicalize and Unphysicalize the
    // superseded-birth rule and what the leaver's Unphysicalize needs.
    class lifecycle_acceptance {
    public:
        struct verdict {
            uint32_t applied = 0;
            // A birth overtaken by a newer accepted P or U: never queued,
            // never relayed to the others; its owner is told (SUPERSEDED).
            bool superseded = false;
            // The newer event it lost to, for the log.
            uint32_t by_origin = 0, by_applied = 0;
            event_type by_type = event_type::Physicalize;
        };
        struct leaver_event {
            uint32_t origin = 0, applied = 0;
        };

        // Takes one event of `player` stamped `origin` while the session's
        // next tick is `next_tick`.  A Physicalize whose origin is older than
        // an accepted P or U of the same player, but whose applied tick would
        // fall after that event's applied tick, is superseded: applied, it
        // would bring back a ball the newer event replaced or took out, at the
        // server and at every remote, with nothing to take it out again (a
        // birth whose every copy was lost and whose retransmit landed after an
        // on-time death).  One that still lands no later than the newer event
        // (in the same tick it goes first, by origin) is the ordinary
        // out-of-order arrival of protocol 6 and is applied.  An Unphysicalize
        // overtaken by a newer Physicalize is left to the guard
        // (lifecycle_unphysicalize_superseded), which every receiver runs too.
        verdict accept(uint32_t player, event_type type, uint32_t origin, uint32_t next_tick) {
            verdict v;
            v.applied = lifecycle_applied_tick(origin, next_tick);
            if (type != event_type::Physicalize && type != event_type::Unphysicalize) return v;
            auto& p = players_[player];
            if (type == event_type::Physicalize) {
                for (const auto& r: p.recent) {
                    if (static_cast<int32_t>(r.origin - origin) <= 0) continue;          // not newer
                    if (static_cast<int32_t>(v.applied - r.applied) <= 0) continue;      // still lands first
                    v.superseded = true;
                    v.by_origin = r.origin;
                    v.by_applied = r.applied;
                    v.by_type = r.type;
                    return v;
                }
            }
            p.recent.push_back(record{type, origin, v.applied, next_tick});
            if (p.recent.size() > kLifecycleAcceptanceEvents) p.recent.erase(p.recent.begin());
            if (!p.have_newest_applied || static_cast<int32_t>(v.applied - p.newest_applied) > 0)
                p.newest_applied = v.applied;
            p.have_newest_applied = true;
            const bool birth = type == event_type::Physicalize;
            uint32_t& newest = birth ? p.newest_physicalize : p.newest_unphysicalize;
            bool& have = birth ? p.have_physicalize : p.have_unphysicalize;
            if (!have || static_cast<int32_t>(origin - newest) > 0) newest = origin;
            have = true;
            return v;
        }

        // True when the events accepted for `player` leave it a ball at every
        // receiver: a Physicalize, and no Unphysicalize newer by origin (an
        // older one is superseded by the guard everywhere).
        bool has_ball(uint32_t player) const {
            const auto it = players_.find(player);
            if (it == players_.end() || !it->second.have_physicalize) return false;
            const auto& p = it->second;
            return !(p.have_unphysicalize && static_cast<int32_t>(p.newest_unphysicalize - p.newest_physicalize) > 0);
        }

        // Late-player design 3.2: the Unphysicalize the server relays for a
        // player who left with a ball, produced on the simulation thread at
        // the runner's own tick rather than stamped from the network thread's
        // view.  Applied at the session's next tick, or after the player's
        // newest accepted event if that one is still to come (a birth accepted
        // and relayed but not applied yet: the receivers make it at its tick,
        // so the death must not come before it there), and newer by origin
        // than the newest birth, or the guard would take it for a stale copy.
        // Its origin is its applied tick: the receivers refuse an origin after
        // the applied tick, and nobody stamped this one.  False when the
        // player has no ball.
        bool leaver_unphysicalize(uint32_t player, uint32_t next_tick, leaver_event& out) const {
            if (!has_ball(player)) return false;
            const auto& p = players_.at(player);
            uint32_t applied = next_tick;
            if (p.have_newest_applied && static_cast<int32_t>(p.newest_applied - applied) > 0) applied = p.newest_applied;
            if (static_cast<int32_t>(p.newest_physicalize + 1 - applied) > 0) applied = p.newest_physicalize + 1;
            out.origin = applied;
            out.applied = applied;
            return true;
        }

        void forget(uint32_t player) { players_.erase(player); }
        void clear() { players_.clear(); }
        // Accepted P/U events still inside the window (for tests and describe).
        size_t remembered(uint32_t player) const {
            const auto it = players_.find(player);
            return it == players_.end() ? 0 : it->second.recent.size();
        }

    private:
        struct record {
            event_type type = event_type::Physicalize;
            uint32_t origin = 0, applied = 0, accepted_at = 0;
        };
        struct player_state {
            std::vector<record> recent;
            bool have_physicalize = false, have_unphysicalize = false, have_newest_applied = false;
            uint32_t newest_physicalize = 0, newest_unphysicalize = 0, newest_applied = 0;
        };
        std::map<uint32_t, player_state> players_;
    };

    // Which events a receiver has already taken: the first arrival counts.
    // Bounded per player by stamp; a copy arriving later than the bound
    // (seconds, at a few copies over ~100 ms) would be taken again.
    class event_seen {
    public:
        explicit event_seen(size_t cap = 256) : cap_(cap) {}
        bool first(const lifecycle_event_key& key) {
            auto& keys = seen_[key.player];
            if (!keys.insert(key).second) return false;
            while (keys.size() > cap_) keys.erase(keys.begin());
            return true;
        }
        void clear() { seen_.clear(); }
    private:
        std::map<uint32_t, std::set<lifecycle_event_key>> seen_;
        size_t cap_;
    };

    // The copies still to send: the serialized message, its recipients (the
    // uplink has one, named 0), how many are left and how many calls apart
    // they go.  send_due() is called once per tick; an entry sends a copy
    // every `spacing` calls, each in its own packet, the first on the call
    // after the push.
    class event_repeats {
    public:
        struct entry {
            std::string bytes;
            std::vector<uint32_t> to;
            int remaining = 0;
            int spacing = 1;
            int wait = 0;
        };
        void push(std::string bytes, std::vector<uint32_t> to, int copies = kLifecycleEventRepeats,
                  int spacing = kLifecycleEventRepeatSpacing) {
            if (copies <= 0 || to.empty()) return;
            entries_.push_back(entry{std::move(bytes), std::move(to), copies, std::max(spacing, 1), 0});
        }
        template <class Send>
        void send_due(Send send) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->wait > 0) { --it->wait; ++it; continue; }
                for (uint32_t to: it->to) send(to, it->bytes);
                it->wait = it->spacing - 1;
                if (--it->remaining <= 0) it = entries_.erase(it);
                else ++it;
            }
        }
        // A recipient that left gets no more copies.
        void forget(uint32_t to) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                it->to.erase(std::remove(it->to.begin(), it->to.end(), to), it->to.end());
                if (it->to.empty()) it = entries_.erase(it);
                else ++it;
            }
        }
        size_t pending() const { return entries_.size(); }
        void clear() { entries_.clear(); }
    private:
        std::vector<entry> entries_;
    };
}
