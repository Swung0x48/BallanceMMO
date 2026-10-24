#pragma once

// The sectors a physics session runs, and when each one starts
// (docs/lockstep-without-snapshots.md, "The sector union, as built").
//
// The retail Gameplay_SectorManager knows one ball: it activates the sector
// that ball enters and resets the one it leaves.  A session has many balls in
// one world, so every side - the server and each client - runs the UNION of
// the sectors its members have entered, and activates each sector at one tick:
// a member that crosses a checkpoint stamps a Sector event `sector_lead` ticks
// ahead (session_start_msg::sector_lead), the server applies it at that tick
// and relays it, and every other member applies it at the same tick from the
// relay.  The pass that starts the sector's mechanisms then runs under one
// pacing rule on every side (sector_pacer.hpp: one pass per idle manager), so
// the mechanisms of the sector are born on the same tick everywhere.
//
// Nothing is ever deactivated, on the server either.  The retail game tears a
// sector down when its one ball leaves it; with several balls a sector that
// one member left is one another member may still be in or come back to, and
// a client that tore one down would destroy mechanism bodies under its own
// rollback history.  So the union only grows: the level's own sector 1 from
// the anchor, then every sector any member reports, for the rest of the
// session.  The cost is the scripts of the sectors behind the players, which
// the retail game would have stopped; the gain is one body set on every side.
//
// Pure bookkeeping: it names the sector to activate and never touches the
// engine, so the rule is unit-tested once and shared by the server's world,
// the headless session client and the retail client's session.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "sector_pacer.hpp"

namespace bmmo::session {
    class sector_union {
    public:
        // One stamped Sector event: who entered which sector, for which tick.
        struct stamped {
            uint32_t tick = 0;
            uint32_t player = 0;
            int sector = 0;
        };
        // What tick() decided: the sector to activate before this tick's step
        // (0 = none), and the stamp that first asked for it, for the log.
        struct pass {
            int sector = 0;
            uint32_t player = 0;
            uint32_t stamped_tick = 0;
            explicit operator bool() const { return sector > 0; }
        };

        // A member's Sector event as it was stamped - the own one when it is
        // sent, a relayed one when it arrives.  Applied by tick() at the first
        // tick at or after `tick`.  A stamp for a tick already ticked is late:
        // counted, and applied at the next tick, which the server did not do
        // (its world dequeued the event at the stamp), so the two sides start
        // that sector apart by the lateness.
        void stamp(uint32_t player, int sector, uint32_t tick) {
            if (sector <= 0) return;
            if (ticked_ && static_cast<int32_t>(tick - last_tick_) <= 0) ++late_;
            pending_.push_back({tick, player, sector});
        }

        // The same, applied now: the server's world dequeues its events itself
        // at their tick (or later, for one that arrived late) and hands the
        // sector over at that moment.
        void request(uint32_t player, int sector, uint32_t tick) {
            if (sector <= 0) return;
            if (requested_.insert(sector).second) origin_[sector] = {tick, player, sector};
        }

        // The level-walk gate's state (gated_sector_union, step S4): open
        // (`seen`), or closed with `armed` (the manager seen inactive since
        // the anchor) and `wait` (the ticks ticked while closed).  Once open
        // it is {true, false, 0} for good.
        static constexpr int kLevelWalkWait = 64;
        struct level_walk {
            bool seen = false;
            bool armed = false;
            int wait = 0;
            bool operator==(const level_walk& o) const { return seen == o.seen && armed == o.armed && wait == o.wait; }
            bool operator!=(const level_walk& o) const { return !(*this == o); }
        };
        // Where a union's gate opened (diagnostic, the live union's own like
        // the counters): the tick, whether by the timeout, and whether the
        // tick is one of the current numbering (`known`: false until the
        // gate opens in a tick() of this union, and again after a
        // renumbering - gated_sector_union::forget_walk_tick).
        struct walk_opened {
            uint32_t tick = 0;
            bool timed_out = false;
            bool known = false;
        };

        // Once per tick, before that tick's step and after the tick's events:
        // applies the stamps due, counts the pacer with the manager's state
        // after the previous step, and names the sector a pass may start for -
        // the lowest requested sector that is not active yet, so every side
        // picks the same one from the same sets.  One pass per call, like the
        // server's.  This union has no level-walk gate (gated_sector_union).
        pass tick(uint32_t tick, bool manager_active) { return tick_gated(tick, manager_active, nullptr); }

    protected:
        struct gate {
            level_walk walk;
            walk_opened opened;
            uint64_t timeouts = 0;
        };
        // tick(), with the level-walk gate `g` (null: none).
        pass tick_gated(uint32_t tick, bool manager_active, gate* g) {
            ticked_ = true;
            last_tick_ = tick;
            if (!pending_.empty()) {
                auto keep = pending_.begin();
                for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                    if (static_cast<int32_t>(it->tick - tick) <= 0) request(it->player, it->sector, it->tick);
                    else *keep++ = *it;
                }
                pending_.erase(keep, pending_.end());
            }
            const bool may_start = pacer_.tick(manager_active);
            // The level-walk gate (sector-placement design 2.6, audit #16,
            // step S4): no pass before the level's own walk has been seen -
            // the manager observed inactive at some tick after the anchor and
            // active at a later one (an edge, so a walk already running at
            // the anchor cannot open it).  Without it a late joiner's owed
            // pass started at relative tick 2, in the frame Init Ingame's own
            // sector-1 activation arrives, overwrote the activate cell, and
            // sector 1 was never walked in that world.  Bounded: after
            // kLevelWalkWait ticks without an edge it opens anyway (a walk
            // that started after the anchor has ended by then, and the pacer
            // still wants its idle ticks).
            if (g && !g->walk.seen) {
                level_walk& walk = g->walk;
                bool timed_out = false;
                if (!manager_active) walk.armed = true;
                else if (walk.armed) walk.seen = true;
                if (!walk.seen && ++walk.wait > kLevelWalkWait) walk.seen = timed_out = true;
                if (walk.seen) {
                    // Open for good: the state keeps only that (what tick()
                    // decides from); where and how it opened is diagnostic.
                    walk = level_walk{true, false, 0};
                    g->opened = {tick, timed_out, true};
                    if (timed_out) ++g->timeouts;
                }
                if (!walk.seen || manager_active) return {};
            }
            if (requested_ == active_ || !may_start) return {};
            for (int sector: requested_) {
                if (active_.count(sector)) continue;
                active_.insert(sector);
                pacer_.started();
                ++passes_;
                const auto it = origin_.find(sector);
                return it != origin_.end() ? pass{sector, it->second.player, it->second.tick} : pass{sector, 0, tick};
            }
            return {};
        }

    public:
        // The union as rollback state (resync design 2.5, step B1): a value
        // with everything tick() decides from - the sets, the origins, the
        // stamps not applied yet, the pacer's count, the last tick ticked
        // and the level-walk gate (step S4; a union without one reports it
        // open).  The counters (passes, late, walk timeouts) are not in it:
        // diagnostics, they stay the live union's across a restore.
        struct state_type {
            std::set<int> active{1};
            std::set<int> requested{1};
            std::map<int, stamped> origin;
            std::vector<stamped> pending;
            int idle = 0;
            uint32_t last_tick = 0;
            bool ticked = false;
            level_walk walk;
            bool operator==(const state_type& o) const {
                const auto same = [](const stamped& a, const stamped& b) {
                    return a.tick == b.tick && a.player == b.player && a.sector == b.sector;
                };
                if (active != o.active || requested != o.requested || idle != o.idle || last_tick != o.last_tick
                        || ticked != o.ticked || walk != o.walk || origin.size() != o.origin.size()
                        || pending.size() != o.pending.size())
                    return false;
                for (auto a = origin.begin(), b = o.origin.begin(); a != origin.end(); ++a, ++b)
                    if (a->first != b->first || !same(a->second, b->second)) return false;
                for (size_t i = 0; i < pending.size(); ++i)
                    if (!same(pending[i], o.pending[i])) return false;
                return true;
            }
            bool operator!=(const state_type& o) const { return !(*this == o); }
        };
        state_type state() const {
            state_type out;
            out.active = active_;
            out.requested = requested_;
            out.origin = origin_;
            out.pending = pending_;
            out.idle = pacer_.idle_ticks();
            out.last_tick = last_tick_;
            out.ticked = ticked_;
            out.walk = level_walk{true, false, 0};
            return out;
        }
        // Back to a saved state; the counters stay (and the gate is the
        // gated union's).
        void restore(const state_type& in) {
            active_ = in.active;
            requested_ = in.requested;
            origin_ = in.origin;
            pending_ = in.pending;
            pacer_.restore(in.idle);
            last_tick_ = in.last_tick;
            ticked_ = in.ticked;
        }
        // The stamps of `player` for `sector` not applied yet taken back (a
        // moved own Sector whose pass has not run: resync design 2.7).
        // Returns how many were taken.
        size_t unstamp(uint32_t player, int sector) {
            const size_t before = pending_.size();
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                          [&](const stamped& s) { return s.player == player && s.sector == sector; }),
                           pending_.end());
            return before - pending_.size();
        }
        // Whether a stamp of `sector` for `tick` changes nothing: the sector
        // is requested already with an origin at or before `tick` (the
        // level's own sector 1 always), or a stamp of it not applied yet is
        // at or before `tick` (resync design 2.7 case 2).
        bool covers(int sector, uint32_t tick) const {
            if (requested_.count(sector)) {
                const auto it = origin_.find(sector);
                return it == origin_.end() || static_cast<int32_t>(it->second.tick - tick) <= 0;
            }
            for (const auto& s: pending_)
                if (s.sector == sector && static_cast<int32_t>(s.tick - tick) <= 0) return true;
            return false;
        }

        // A saved state whose stamps not applied yet are replaced by every
        // stamp of `log` whose sector the state has not requested, in (tick,
        // sector, player) order (resync design 2.5 "the merge" and 2.8 "the
        // start"): the stamps are the ones every member received, so the
        // result depends neither on when nor in which order they arrived.
        static state_type with_pending(state_type base, const std::vector<stamped>& log) {
            base.pending.clear();
            for (const auto& s: log)
                if (s.sector > 0 && !base.requested.count(s.sector)) base.pending.push_back(s);
            std::stable_sort(base.pending.begin(), base.pending.end(), [](const stamped& a, const stamped& b) {
                if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
                return std::tie(a.sector, a.player) < std::tie(b.sector, b.player);
            });
            return base;
        }

        // The state as bytes (resync design 5 "sector union bytes"), little
        // endian: u8 version, u32 last_tick, u8 ticked, u8 idle, active and
        // requested (u8 count + i32 each), origin (u8 count + (i32 sector,
        // u32 tick, u32 player)), pending (u16 count + (u32 tick, u32 player,
        // i32 sector)).
        // A state the format cannot hold (more than 255 sectors in a set or
        // origins, more than 65535 stamps, an idle count above 255) gives an
        // empty vector, which deserialize refuses: never a truncated state
        // that reads back as another one.
        // The level-walk gate (step S4) is not in the bytes, which stay the
        // protocol-10 ones: a state whose gate has opened reads back as an
        // open gate, and a state whose gate is still closed is one the
        // format cannot hold.  The server's state of S = base - 1, the one
        // state that travels, is past the server's own walk for a late
        // joiner (it loads the level after the runner has started), but not
        // for an A3 resync whose S lies before the server's walk (a request
        // in the first ticks of the session): the server then sends no union
        // bytes (physics_world resync_state's error) and the client keeps its
        // anchor's own union (client_core_resync.cpp) - the designed answer,
        // since neither union can have started a pass before its walk.
        static constexpr uint8_t kStateVersion = 1;
        static std::vector<uint8_t> serialize(const state_type& in) {
            std::vector<uint8_t> out;
            if (!in.walk.seen || in.active.size() > 255 || in.requested.size() > 255 || in.origin.size() > 255
                    || in.pending.size() > 0xFFFF || in.idle < 0 || in.idle > 255)
                return out;
            const auto u8 = [&out](uint32_t v) { out.push_back(static_cast<uint8_t>(v & 0xFFu)); };
            const auto u16 = [&u8](uint32_t v) { u8(v); u8(v >> 8); };
            const auto u32 = [&u16](uint32_t v) { u16(v); u16(v >> 16); };
            u8(kStateVersion);
            u32(in.last_tick);
            u8(in.ticked ? 1u : 0u);
            u8(static_cast<uint32_t>(in.idle));
            for (const std::set<int>* set: {&in.active, &in.requested}) {
                u8(static_cast<uint32_t>(set->size()));
                for (int sector: *set) u32(static_cast<uint32_t>(sector));
            }
            u8(static_cast<uint32_t>(in.origin.size()));
            for (const auto& [sector, s]: in.origin) {
                u32(static_cast<uint32_t>(sector));
                u32(s.tick);
                u32(s.player);
            }
            const size_t stamps = in.pending.size();
            u16(static_cast<uint32_t>(stamps));
            for (size_t i = 0; i < stamps; ++i) {
                u32(in.pending[i].tick);
                u32(in.pending[i].player);
                u32(static_cast<uint32_t>(in.pending[i].sector));
            }
            return out;
        }
        // False, and `out` untouched, on a short, long or other-version buffer.
        static bool deserialize(const uint8_t* data, size_t size, state_type& out) {
            size_t at = 0;
            bool ok = true;
            const auto u8 = [&]() -> uint32_t {
                if (at >= size) {
                    ok = false;
                    return 0;
                }
                return data[at++];
            };
            const auto u16 = [&u8]() -> uint32_t { const uint32_t lo = u8(); return lo | (u8() << 8); };
            const auto u32 = [&u16]() -> uint32_t { const uint32_t lo = u16(); return lo | (u16() << 16); };
            state_type in;
            in.walk.seen = true;   // only an open gate is serialized
            if (u8() != kStateVersion || !ok) return false;
            in.last_tick = u32();
            in.ticked = u8() != 0;
            in.idle = static_cast<int>(u8());
            for (std::set<int>* set: {&in.active, &in.requested}) {
                set->clear();
                const uint32_t count = u8();
                for (uint32_t i = 0; i < count && ok; ++i) set->insert(static_cast<int>(u32()));
            }
            const uint32_t origins = u8();
            for (uint32_t i = 0; i < origins && ok; ++i) {
                stamped s;
                s.sector = static_cast<int>(u32());
                s.tick = u32();
                s.player = u32();
                in.origin[s.sector] = s;
            }
            const uint32_t stamps = u16();
            for (uint32_t i = 0; i < stamps && ok; ++i) {
                stamped s;
                s.tick = u32();
                s.player = u32();
                s.sector = static_cast<int>(u32());
                in.pending.push_back(s);
            }
            if (!ok || at != size) return false;
            out = std::move(in);
            return true;
        }

        // The live pass a body born at `birth_tick` belongs to (resync design
        // 2.6.3 "attribution": a pass at M physicalizes its modules in frame
        // M+5) among `live` (tick -> sector: the passes the live frames had
        // made inside a replayed window), and whether the passes re-derived
        // so far (`rederived`) have it at the same tick.  A body destroyed at
        // the restore whose pass the re-derivation moved or forgot is not
        // made again at its live birth tick, and is no missing birth (step
        // B1).  Only the pass with M + 5 == birth_tick owns a birth: a body
        // born at any other tick is no pass's physicalize, and one missing
        // there stays a missing birth (births_missing), never hidden by the
        // nearest pass before it.  False when no live pass has that tick.
        static constexpr uint32_t kPassPhysicalizeDelay = 5;
        static bool birth_moved(const std::map<uint32_t, int>& live, const std::map<uint32_t, int>& rederived,
                                uint32_t birth_tick) {
            const auto owner = live.find(birth_tick - kPassPhysicalizeDelay);
            if (owner == live.end()) return false;
            const auto again = rederived.find(owner->first);
            return again == rederived.end() || again->second != owner->second;
        }

        // The sectors whose pass has started, in sector order.  Sector 1 is
        // active from the anchor on every side (the level's own Init runs it).
        const std::set<int>& active() const { return active_; }
        // Active plus the ones a pass is still owed for.
        const std::set<int>& requested() const { return requested_; }
        bool pass_pending() const { return requested_ != active_; }
        size_t stamps_pending() const { return pending_.size(); }
        uint64_t passes() const { return passes_; }
        uint64_t late() const { return late_; }

        const sector_pacer& pacer() const { return pacer_; }

        // A new session: back to the level's own sector.
        void reset() { *this = sector_union(); }

    private:
        sector_pacer pacer_;
        std::set<int> active_{1};
        std::set<int> requested_{1};
        std::map<int, stamped> origin_;
        std::vector<stamped> pending_;
        bool ticked_ = false;
        uint32_t last_tick_ = 0;
        uint64_t passes_ = 0, late_ = 0;
    };

    // The union with the level-walk gate (sector-placement design 2.6, audit
    // #16, step S4): no pass before the level's own walk has been seen - the
    // manager observed inactive at some tick after the anchor and active at
    // a later one (an edge, so a walk already running at the anchor cannot
    // open it).  Without it a late joiner's owed pass started at relative
    // tick 2, in the frame Init Ingame's own sector-1 activation arrives,
    // overwrote the activate cell, and sector 1 was never walked in that
    // world.  Bounded: after kLevelWalkWait ticks without an edge it opens
    // anyway (a walk that started after the anchor has ended by then, and
    // the pacer still wants its idle ticks).  The gate is rollback state.
    //
    // Every world of the session core and the server's runs this one.  The
    // gate-free sector_union stays the legacy retail path's: it is held by
    // value in physics_session_state, which keeps its layout until R7
    // (session-client-core design 3.2), and that path gets no feature.
    class gated_sector_union : public sector_union {
    public:
        pass tick(uint32_t tick, bool manager_active) { return tick_gated(tick, manager_active, &gate_); }
        state_type state() const {
            state_type out = sector_union::state();
            out.walk = gate_.walk;
            return out;
        }
        void restore(const state_type& in) {
            sector_union::restore(in);
            gate_.walk = in.walk;
        }
        // The gate's state, where this union saw it open (meaningful once
        // walk().seen became true in a tick() of this union, not after a
        // restore()), and how many times it opened by the timeout.
        const level_walk& walk() const { return gate_.walk; }
        const walk_opened& walk_open() const { return gate_.opened; }
        // The client renumbered (an assignment's new base): the tick the gate
        // opened at names a tick of the old numbering, so it is no longer
        // known.  The gate's state (open or not) is unchanged.
        void forget_walk_tick() { gate_.opened.known = false; }
        uint64_t walk_timeouts() const { return gate_.timeouts; }
        // A new session: back to the level's own sector, the gate closed.
        void reset() { *this = gated_sector_union(); }

    private:
        gate gate_;
    };

    // The re-derivation of a Sector rewind (resync design 2.5 "the merge",
    // 2.8, step B1): the union re-run inside the replay, one tick before each
    // replayed tick's step.  CK-free and the ONE implementation of the rule:
    // the session-client core (client_core_frame.cpp rewind_for_sectors,
    // rederive_union_tick) and SimTool's --late-sector-check both drive it,
    // so the offline exactness check runs the code the clients run; the
    // caller does only its own side effects with what tick() decides (the
    // host's pass slot, its logs, its records).
    class sector_rederivation {
    public:
        // What one replayed tick decided.
        struct step {
            sector_union::pass made;    // the pass before this tick's step: the tick's pass slot gets it
            bool clear_slot = false;    // no pass, but the live frames had one here: the pass slot emptied
            int forgotten = 0;          // the sector of a live pass of this tick not re-derived here, 0 none
        };

        // The window start (anchor = from - 1): the union as recorded after
        // the anchor's tick with its stamps not applied yet replaced by the
        // stamp log's (sector_union::with_pending), and the live passes of
        // the window moved out of `pass_log` - the re-derived ones replace
        // them (tick() leaves those to the caller's log).
        void start(const sector_union::state_type& record, const std::vector<sector_union::stamped>& log,
                   std::map<uint32_t, int>& pass_log, uint32_t from) {
            u_.reset();
            u_.restore(sector_union::with_pending(record, log));
            live_.clear();
            passes_.clear();
            forgotten_ = 0;
            for (auto it = pass_log.lower_bound(from); it != pass_log.end();) {
                live_.insert(*it);
                it = pass_log.erase(it);
            }
        }
        // A rewind refused or cut short: the live passes back in `pass_log`
        // (the live world stands), nothing re-derived kept.
        void abandon(std::map<uint32_t, int>& pass_log) {
            for (const auto& [tick, sector]: live_) pass_log[tick] = sector;
            live_.clear();
            passes_.clear();
        }
        // Before replayed tick `t`'s step, with the manager's state after the
        // replayed step before (as the live frame's union pass and the
        // server's update_sectors take it).
        step tick(uint32_t t, bool manager_active) {
            step out;
            out.made = u_.tick(t, manager_active);
            const auto live = live_.find(t);
            if (out.made) passes_[t] = out.made.sector;
            else if (live != live_.end()) out.clear_slot = true;
            if (live != live_.end() && (!out.made || out.made.sector != live->second)) {
                out.forgotten = live->second;
                ++forgotten_;
            }
            return out;
        }
        // Whether the replay re-derived the union through `now` (a frozen
        // world rewinds nothing; a replay cut short stops before it).
        bool through(uint32_t now) const { return u_.state().ticked && u_.state().last_tick == now; }
        // A window-born body of a live pass the re-derivation moved or
        // forgot (sector_union::birth_moved).
        bool birth_moved(uint32_t birth_tick) const { return sector_union::birth_moved(live_, passes_, birth_tick); }
        const gated_sector_union& u() const { return u_; }
        const std::map<uint32_t, int>& live_passes() const { return live_; }
        const std::map<uint32_t, int>& passes() const { return passes_; }
        uint32_t forgotten() const { return forgotten_; }

    private:
        gated_sector_union u_;
        std::map<uint32_t, int> live_, passes_;
        uint32_t forgotten_ = 0;
    };
}
