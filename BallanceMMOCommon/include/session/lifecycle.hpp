#pragma once

// The two lifecycle judgements the physics session makes about events, kept as
// pure functions so the unit tests can exercise them without linking the
// simulation (the test target has no physics).
//
//   physicalize_pending    - the server's lifecycle barrier: is a Physicalize
//                            for this player already queued within reach?
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
    // True when `events` holds a Physicalize for `player` stamped no later than
    // `horizon`.  The range may be any container whose elements expose
    // .player, .tick and .event.type (session_runner::pending_event does).
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
            if (e.player == player && e.tick <= horizon && e.event.type == event_type::Physicalize) return true;
        return false;
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
    struct lifecycle_event_key {
        uint32_t tick = 0;
        uint32_t player = 0;
        uint8_t type = 0;
        uint8_t ball_type = 0;
        int32_t sector = 0;
        std::string name;

        bool operator<(const lifecycle_event_key& o) const {
            if (tick != o.tick) return tick < o.tick;
            if (player != o.player) return player < o.player;
            if (type != o.type) return type < o.type;
            if (ball_type != o.ball_type) return ball_type < o.ball_type;
            if (sector != o.sector) return sector < o.sector;
            return name < o.name;
        }
        bool operator==(const lifecycle_event_key& o) const {
            return tick == o.tick && player == o.player && type == o.type && ball_type == o.ball_type
                && sector == o.sector && name == o.name;
        }
    };

    // The key of a session_event_msg (or anything with .tick, .type,
    // .ball_type, .sector, .name) as sent by `player` (0 = the connection on
    // the uplink, where the server keys by the connection instead).
    template <class Msg>
    inline lifecycle_event_key lifecycle_key_of(const Msg& m, uint32_t player) {
        lifecycle_event_key k;
        k.tick = m.tick;
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
