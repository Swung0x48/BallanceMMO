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

        // Once per tick, before that tick's step and after the tick's events:
        // applies the stamps due, counts the pacer with the manager's state
        // after the previous step, and names the sector a pass may start for -
        // the lowest requested sector that is not active yet, so every side
        // picks the same one from the same sets.  One pass per call, like the
        // server's.
        pass tick(uint32_t tick, bool manager_active) {
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
}
