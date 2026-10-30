#pragma once

// The two lifecycle judgements the physics session makes about events, kept as
// pure functions so the unit tests can exercise them without linking the
// simulation (the test target has no physics).
//
//   bodiless_tracker       - S4 (late-player design 3.2): the runs of ticks a
//                            player's frame claimed a ball the server's world
//                            did not have, which the server steps through
//                            since the lifecycle barrier is gone (plan step 20);
//   own_echo_missing       - the owner's deadline for the echo of its own
//                            Physicalize, on the server's tick the relay
//                            header carries every tick (snapshots or not),
//                            and own_echo_deadline, its gate on the
//                            header's acked (the uplink worked after it);
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
    // An event type's name as the headless client's log lines and journal
    // notes print it (and its --hold-event takes it): moved from the headless
    // client at shared-core step E7, whose own-echo lines it names.
    inline const char* event_type_name(int type) {
        switch (type) {
        case 0: return "physicalize";
        case 1: return "unphysicalize";
        case 2: return "sector";
        case 3: return "finish";
        case 4: return "bodyrevived";
        case 5: return "trafo";
        default: return "?";
        }
    }

    // S4 (late-player design 3.2, plan step 20): the lifecycle barrier is
    // gone.  It held every member up to a second when a present frame was
    // flagged INPUT_FLAG_PHYSICALIZED and the world had no ball for that
    // player and no Physicalize queued - one late or lost event stalled the
    // whole session, and the catch-up put every member's relay past its
    // reach.  A late event is re-stamped at acceptance now (design 3.1) and
    // the world never reads the flag (physics_world::set_input), so such a
    // tick is simply stepped without the ball, as every receiver of the
    // re-stamped event steps it too.  What is left is a diagnostic: the run
    // of ticks a player's frame claimed a ball the world did not have, one
    // run per (player, first tick), told when it ends and once while it is
    // still open after kBodilessOpenReportTicks (a dropped birth never ends).
    //
    // Short runs are ordinary: a birth is stamped up to input_delay + 1
    // ticks past the frame that first reports the ball, and a death's frame
    // can still carry the flag for a tick.  The length is what tells a
    // late or lost Physicalize apart.
    inline constexpr uint32_t kBodilessOpenReportTicks = 66;

    class bodiless_tracker {
    public:
        struct run {
            uint32_t player = 0;
            uint32_t first = 0;   // the first tick stepped without the claimed ball
            uint32_t last = 0;    // the last one so far
            bool told_open = false;   // the still_open line was given
            uint32_t ticks() const { return last - first + 1; }
        };
        // What one call found: the run that ended before `tick` (ended), or
        // the run reaching kBodilessOpenReportTicks at `tick` (still_open),
        // with `out` filled; otherwise none (a run began, went on, or nothing
        // was claimed).
        enum class change : uint8_t { none, ended, still_open };

        // One player's frame at `tick`, after the tick's events were applied:
        // `claimed` its INPUT_FLAG_PHYSICALIZED, `has_ball` the world's view.
        change step(uint32_t player, uint32_t tick, bool claimed, bool has_ball, run& out) {
            const auto it = open_.find(player);
            const bool bodiless = claimed && !has_ball;
            if (!bodiless) {
                if (it == open_.end()) return change::none;
                out = it->second;
                open_.erase(it);
                ++ended_;
                return change::ended;
            }
            if (it == open_.end()) {
                open_.emplace(player, run{player, tick, tick});
                ++runs_;
                return change::none;
            }
            it->second.last = tick;
            // At or past the mark, once: a player missing from a tick's
            // applied inputs (a leaver's last ticks, a member not ready)
            // makes `last` jump, and an equality test would skip the line a
            // dropped birth exists to give.
            if (!it->second.told_open && it->second.ticks() >= kBodilessOpenReportTicks) {
                it->second.told_open = true;
                out = it->second;
                return change::still_open;
            }
            return change::none;
        }
        // A player that left: its open run ends with it, untold.
        void forget(uint32_t player) { open_.erase(player); }
        bool open(uint32_t player) const { return open_.count(player) != 0; }
        size_t open_runs() const { return open_.size(); }
        uint64_t runs() const { return runs_; }
        uint64_t ended() const { return ended_; }
    private:
        std::map<uint32_t, run> open_;
        uint64_t runs_ = 0;
        uint64_t ended_ = 0;
    };

    // The owner's deadline for the echo of its own Physicalize (late-player
    // design 3.2-3.3, plan step 20).  The server echoes every accepted event
    // to its owner from the simulation thread before it steps the event's
    // applied tick, and relays each stepped tick right after it; the relay
    // header names that tick (session_remote_input_msg::tick) on every
    // datagram, so the owner knows the server's tick every tick, with
    // snapshots off too.  An own birth with no echo while the server's tick
    // is more than `deadline_ticks` past its origin never reached the server
    // (or will be applied so late that the move is past the reach anyway):
    // missing.  The comparison is signed, so a tick older than the origin
    // (a datagram from before the birth) is never past it.
    inline bool own_echo_missing(uint32_t server_tick, uint32_t origin, uint32_t deadline_ticks) {
        return static_cast<int32_t>(server_tick - origin) > static_cast<int32_t>(deadline_ticks);
    }

    // The uplink gate on that deadline (step 20 review).  The server's tick
    // moves with the DOWNLINK; an uplink outage (or a hold longer than the
    // deadline) leaves the reliable Physicalize queued at the owner while
    // that tick passes origin + deadline, and giving the birth up then
    // resyncs the owner twice: once for the give-up, once more when the
    // queued copy arrives and is echoed as made.  So the deadline counts
    // only once the server has applied a frame of ours fresh at or past the
    // birth's origin (the relay header's `acked`, the server's own
    // numbering): our uplink worked after the event was sent.  From the
    // server tick that was first seen at, the reliable copy gets
    // `settle_ticks` more for its retransmission.  A server that has none of
    // our frames since the origin has not lost the event.  One per own birth.
    class own_echo_deadline {
    public:
        // One look, with the newest server tick and the header's acked (when
        // known): true when the birth is missing.
        bool missing(uint32_t server_tick, bool acked_known, uint32_t acked, uint32_t origin,
                     uint32_t deadline_ticks, uint32_t settle_ticks) {
            if (!uplink_ && acked_known && static_cast<int32_t>(acked - origin) >= 0) {
                uplink_ = true;
                since_ = server_tick;
            }
            if (!own_echo_missing(server_tick, origin, deadline_ticks)) return false;
            return uplink_ && static_cast<int32_t>(server_tick - since_) > static_cast<int32_t>(settle_ticks);
        }
        bool uplink_seen() const { return uplink_; }
        uint32_t uplink_since() const { return since_; }
    private:
        bool uplink_ = false;
        uint32_t since_ = 0;
    };

    // The newest server tick the owner knows of: the relay header's (every
    // tick, any epoch: its tick is the server's own number) and, when
    // snapshots run, the newest snapshot's.  False when it knows neither.
    inline bool newest_server_tick(bool relay_known, uint32_t relay_tick, bool snapshot_known,
                                   uint32_t snapshot_tick, uint32_t& out) {
        if (!relay_known && !snapshot_known) return false;
        if (!relay_known) out = snapshot_tick;
        else if (!snapshot_known) out = relay_tick;
        else out = static_cast<int32_t>(snapshot_tick - relay_tick) > 0 ? snapshot_tick : relay_tick;
        return true;
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
    // the sender repeats the same bytes kLifecycleEventRepeats times on
    // later ticks (the spacing and the schedule below; each copy is a send
    // of its own, which GNS usually coalesces with that tick's other
    // datagram, the uplink's input or the server's tick - step DX8 measured
    // it on the uplink in the proxy logs),
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
        // Protocol 11 (trafo-replication design 5): a Trafo event's flags -
        // an explode and a reset of one type at one origin are two events,
        // and a server-stamped one (TRAFO_SERVER, origin = applied) never
        // collides with an owner's.  0 for every other type.
        uint8_t flags = 0;

        bool operator<(const lifecycle_event_key& o) const {
            if (tick != o.tick) return tick < o.tick;
            if (applied != o.applied) return applied < o.applied;
            if (player != o.player) return player < o.player;
            if (type != o.type) return type < o.type;
            if (ball_type != o.ball_type) return ball_type < o.ball_type;
            if (sector != o.sector) return sector < o.sector;
            if (flags != o.flags) return flags < o.flags;
            return name < o.name;
        }
        bool operator==(const lifecycle_event_key& o) const {
            return tick == o.tick && applied == o.applied && player == o.player && type == o.type
                && ball_type == o.ball_type && sector == o.sector && flags == o.flags && name == o.name;
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
        if constexpr (requires { m.flags; })
            if (m.type == event_type::Trafo) k.flags = m.flags;
        return k;
    }

    // Copies of an event sent after the reliable one, and how many ticks
    // apart: six copies two ticks apart span twelve ticks (180 ms), longer
    // than a burst of a dozen datagrams (measured: six copies one tick apart
    // were all inside such a burst, and the reliable copy carried the event
    // late again).
    // (The server's relay and the legacy retail path keep exactly this.)
    inline constexpr int kLifecycleEventRepeats = 6;
    inline constexpr int kLifecycleEventRepeatSpacing = 2;

    // A client's own events on the uplink (step DX8, the FULL plan's D4
    // option (b'), late-player design 3.1 "As built (step DX8)"): one more
    // copy, at L + 2 - the copies ride the input datagrams of L + 1, L + 2,
    // L + 3, then every other tick to L + 11 (seven copies).  An own
    // Physicalize / Unphysicalize is stamped L + 1 and the server steps
    // L + 1 as soon as frame L + 1 is in (step F3: margin 0..1 for the
    // pacing member), so the copy must ride whichever datagram first
    // carries frame L + 1: with the reliable copy and the L + 1 datagram
    // both lost, that is the L + 2 one, which at spacing 2 had no copy (late
    // by 2: a resync, or a 2-tick own-death gap).  Every send tick of the
    // old schedule is kept (a superset: no loss pattern makes an event later
    // than before, bursts included); the tail stays at spacing 2 for the
    // burst reason above.  The span is 11 ticks.
    inline constexpr int kOwnEventRepeats = 7;
    inline constexpr int kOwnEventRepeatsDense = 3;   // the first three one tick apart
    inline constexpr int kOwnEventRepeatSpacing = 2;

    // The order two events of one player with one stamp are applied in: the
    // Unphysicalize before the Physicalize (a beam sends both at one tick), the
    // rest after.  The ordered stream used to guarantee the sender's order;
    // with copies that can overtake each other, every receiver sorts a stamp's
    // events by this rank (the server's runner, the headless client's drain,
    // the retail mod's batch) - and ignores an Unphysicalize stamped at or
    // before the player's last applied Physicalize, which a late copy of a
    // death would otherwise take out the respawn with.  A Trafo event ranks
    // 2 with Sector, Finish and BodyRevived (trafo-replication design 2.2:
    // the ledger never puts two events of one type set into one tick, so
    // inside a tick it only orders flag writes on distinct type sets).
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
    // every `spacing` calls, each a send of its own (GNS usually packs it
    // with the tick's input datagram), the first on the call after the
    // push.  `dense` (step DX8): the first `dense` copies go on
    // consecutive calls, the rest `spacing` apart (0 = all spaced).
    class event_repeats {
    public:
        struct entry {
            std::string bytes;
            std::vector<uint32_t> to;
            int remaining = 0;
            int spacing = 1;
            int wait = 0;
            int dense = 0;
        };
        void push(std::string bytes, std::vector<uint32_t> to, int copies = kLifecycleEventRepeats,
                  int spacing = kLifecycleEventRepeatSpacing, int dense = 0) {
            if (copies <= 0 || to.empty()) return;
            entries_.push_back(entry{std::move(bytes), std::move(to), copies, std::max(spacing, 1), 0, std::max(dense, 0)});
        }
        template <class Send>
        void send_due(Send send) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->wait > 0) { --it->wait; ++it; continue; }
                for (uint32_t to: it->to) send(to, it->bytes);
                if (it->dense > 0) --it->dense;
                it->wait = it->dense > 0 ? 0 : it->spacing - 1;
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
