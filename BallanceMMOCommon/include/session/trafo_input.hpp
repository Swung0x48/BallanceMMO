#pragma once

// The trafo as a lockstep input (docs/trafo-replication-design.md 2.2, 2.5,
// 2.6, 4.5; step T1): CK-free, so the server, both session-client cores,
// SimTool and the unit tests share one implementation.
//
//   * the event's flags and the ledger's constants;
//   * trafo_seed: the Random seed of one explosion, an integer-only mix of
//     (session seed, player, applied tick, piece type), the same bits on
//     every platform;
//   * trafo_acceptance: the server's ledger per (player, piece type) - the
//     applied tick of every accepted explosion and reset, the server's own
//     resets (before a re-explosion, at the 20 s deadline, for a leaver),
//     superseded resets;
//   * trafo_input_log: every applied or pending trafo input by applied tick,
//     own, remote and server-stamped (live application, the replayed ticks'
//     re-application, "the newest explosion of (p, T) at or before t");
//   * plan_trafo_record_restore: what record v3's restore does with the
//     trafo sets' scripts (game/script_replay.hpp), decided without CK.
//
// No wire in T1: nothing here is serialized.  Step T2 adds event_type::Trafo
// and carries these fields (ball_type = T, flags, position) in the existing
// session_event_msg fields.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace bmmo::session {

    // ---- the event's flags (design 2.2)
    constexpr uint8_t TRAFO_EXPLODE = 0;       // bit 0 clear
    constexpr uint8_t TRAFO_RESET = 1u << 0;   // bit 0 set
    constexpr uint8_t TRAFO_SERVER = 1u << 1;  // stamped by the server (deadline, re-explosion, leaver)
    constexpr uint8_t TRAFO_FINAL = 1u << 2;   // a leaver's last input for this type set

    // Piece types, as `ball_type` (0 wood, 1 stone, 2 paper).
    constexpr uint8_t kTrafoTypes = 3;
    inline const char* trafo_type_name(uint8_t type) {
        static const char* const kNames[kTrafoTypes] = {"Wood", "Stone", "Paper"};
        return type < kTrafoTypes ? kNames[type] : "?";
    }

    // A reset applied at R removes its bodies in R+132 and restores the IC in
    // R+133 (Bezier Progression, 2000 ms at 1000/66 ms per frame, design
    // 1.5); 140 leaves 7 ticks of margin.
    constexpr uint32_t kTrafoResetSpan = 140;
    constexpr uint32_t kTrafoResetDephysicalize = 132;   // R + 132: the De Physicalize's frame
    constexpr uint32_t kTrafoResetRestoreIC = 133;       // R + 133: Restore IC
    // 20 s at 66 ticks/s: the piece life (decision 9.6).
    constexpr uint32_t kTrafoLife = 1320;

    inline bool trafo_is_reset(uint8_t flags) { return (flags & TRAFO_RESET) != 0; }

    // ---- the seed (design 2.6)
    //
    // FNV-1a over the four values' little-endian bytes, folded to a nonzero
    // int32.  The applied tick goes in, not the origin (the server and every
    // receiver know only the applied tick; an owner whose own event was moved
    // re-runs the explosion at the applied tick).
    inline int32_t trafo_seed(uint32_t session_seed, uint32_t player, uint32_t applied_tick, uint8_t type) {
        uint32_t h = 2166136261u;
        const auto mix = [&h](uint32_t v) {
            for (int k = 0; k < 4; ++k) {
                h ^= (v >> (8 * k)) & 0xffu;
                h *= 16777619u;
            }
        };
        mix(session_seed);
        mix(player);
        mix(applied_tick);
        mix(type);
        const int32_t folded = static_cast<int32_t>(h & 0x7fffffffu);
        return folded == 0 ? 1 : folded;
    }

    // Tick arithmetic that survives a wrap (the session's ticks are u32).
    inline uint32_t trafo_tick_max(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0 ? a : b; }
    inline bool trafo_tick_before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

    // One decision of the ledger: an event to queue (and relay) at `applied`.
    struct trafo_decision {
        uint32_t player = 0;
        uint8_t type = 0;
        uint8_t flags = 0;         // TRAFO_RESET / TRAFO_SERVER / TRAFO_FINAL
        uint32_t origin = 0;       // the stamp; a server-stamped event has origin = applied
        uint32_t applied = 0;
        bool superseded = false;   // a reset older than a newer explosion: never queued or relayed
    };

    // ---- the server's ledger (design 2.2), the twin of lifecycle_acceptance.
    //
    // Each event is taken on the simulation thread with the session's next
    // tick N.  What the rules guarantee in every world (every world applies
    // the applied ticks the ledger fixed): an explosion and a reset of one
    // type set are never in one tick and a reset never comes before the
    // explosion it resets; an explosion never lands while that type set's
    // previous bodies exist (it lands >= 140 ticks after the reset); no type
    // set has bodies for longer than kTrafoLife + 133 ticks after its
    // explosion.
    class trafo_acceptance {
    public:
        struct entry {
            bool live = false;                       // an accepted explosion no accepted reset follows
            uint32_t live_origin = 0, live_applied = 0;
            bool have_reset = false;
            uint32_t last_reset_applied = 0;
            bool have_explosion = false;
            uint32_t newest_explode_origin = 0;
            bool final = false;                      // a TRAFO_FINAL reset was queued (a leaver)
        };

        // An explosion of (player, type) stamped `origin`, taken at the next
        // tick `next`.  Returns the decisions in queue order: the server's
        // reset of a type set still live (rule 1), then the explosion.
        std::vector<trafo_decision> explode(uint32_t player, uint8_t type, uint32_t origin, uint32_t next) {
            std::vector<trafo_decision> out;
            entry& e = at(player, type);
            if (e.live) {
                trafo_decision r;
                r.player = player;
                r.type = type;
                r.flags = TRAFO_RESET | TRAFO_SERVER;
                r.applied = trafo_tick_max(next, e.live_applied + 1);
                r.origin = r.applied;
                e.live = false;
                e.have_reset = true;
                e.last_reset_applied = r.applied;
                out.push_back(r);
            }
            trafo_decision x;
            x.player = player;
            x.type = type;
            x.flags = TRAFO_EXPLODE;
            x.origin = origin;
            x.applied = trafo_tick_max(origin, next);
            if (e.have_reset) x.applied = trafo_tick_max(x.applied, e.last_reset_applied + kTrafoResetSpan);
            e.live = true;
            e.live_origin = origin;
            e.live_applied = x.applied;
            e.newest_explode_origin = e.have_explosion ? trafo_tick_max(e.newest_explode_origin, origin) : origin;
            e.have_explosion = true;
            out.push_back(x);
            return out;
        }

        // A reset of (player, type) stamped `origin`, taken at `next`.  A reset
        // older than the newest explosion's origin is superseded (the
        // explosion's rule 1 reset the earlier life already): returned with
        // `superseded` set and nothing changed.
        trafo_decision reset(uint32_t player, uint8_t type, uint32_t origin, uint32_t next, uint8_t extra_flags = 0) {
            entry& e = at(player, type);
            trafo_decision r;
            r.player = player;
            r.type = type;
            r.flags = static_cast<uint8_t>(TRAFO_RESET | extra_flags);
            r.origin = origin;
            if (e.have_explosion && trafo_tick_before(origin, e.newest_explode_origin)) {
                r.superseded = true;
                r.applied = origin;
                return r;
            }
            r.applied = trafo_tick_max(origin, next);
            if (e.live) r.applied = trafo_tick_max(r.applied, e.live_applied + 1);
            e.live = false;
            e.have_reset = true;
            e.last_reset_applied = r.applied;
            return r;
        }

        // After each server tick: every live type set whose life ends within
        // the lead (live.applied + kTrafoLife <= next + lead) gets the server's
        // reset at max(next, live.applied + kTrafoLife).
        std::vector<trafo_decision> deadline(uint32_t next, uint32_t lead) {
            std::vector<trafo_decision> out;
            for (auto& [key, e]: entries_) {
                if (!e.live) continue;
                const uint32_t due = e.live_applied + kTrafoLife;
                if (static_cast<int32_t>(due - (next + lead)) > 0) continue;
                trafo_decision r;
                r.player = key.first;
                r.type = key.second;
                r.flags = TRAFO_RESET | TRAFO_SERVER;
                r.applied = trafo_tick_max(next, due);
                r.origin = r.applied;
                e.live = false;
                e.have_reset = true;
                e.last_reset_applied = r.applied;
                out.push_back(r);
            }
            return out;
        }

        // A leaver (design 2.2 "Leavers"): `removal` is
        // lifecycle_leaver_removal_tick after the leaver's Unphysicalize was
        // queued.  Every live type set goes through the reset rule at N =
        // removal (applied = max(removal, live.applied + 1)); a type set with
        // nothing live gets a final reset at `removal`.  All three carry
        // TRAFO_RESET | TRAFO_SERVER | TRAFO_FINAL, origin = applied.  The
        // caller sets the leaver's remove_at to 1 + the last applied tick.
        std::vector<trafo_decision> leaver(uint32_t player, uint32_t removal) {
            std::vector<trafo_decision> out;
            for (uint8_t type = 0; type < kTrafoTypes; ++type) {
                entry& e = at(player, type);
                trafo_decision r;
                r.player = player;
                r.type = type;
                r.flags = TRAFO_RESET | TRAFO_SERVER | TRAFO_FINAL;
                r.applied = removal;
                if (e.live) r.applied = trafo_tick_max(removal, e.live_applied + 1);
                r.origin = r.applied;
                e.live = false;
                e.have_reset = true;
                e.last_reset_applied = r.applied;
                e.final = true;
                out.push_back(r);
            }
            return out;
        }

        const entry* find(uint32_t player, uint8_t type) const {
            const auto it = entries_.find({player, type});
            return it == entries_.end() ? nullptr : &it->second;
        }
        void forget(uint32_t player) {
            for (auto it = entries_.begin(); it != entries_.end();)
                it = it->first.first == player ? entries_.erase(it) : std::next(it);
        }
        void clear() { entries_.clear(); }
        size_t size() const { return entries_.size(); }

    private:
        entry& at(uint32_t player, uint8_t type) { return entries_[{player, type}]; }
        std::map<std::pair<uint32_t, uint8_t>, entry> entries_;
    };

    // ---- the trafo input log (design 2.5)
    struct trafo_input {
        uint32_t player = 0;
        uint8_t type = 0;
        uint8_t flags = 0;
        uint32_t origin = 0;
        uint32_t applied = 0;
        float position[3] = {};   // explode: the referential's translation; reset: zeros
        int32_t seed = 0;         // explode: trafo_seed(session seed, player, applied, type)
        bool own = false;         // the owner's own input (stamped here)
    };

    // The key that tells two inputs apart: (player, type, kind, origin) -
    // an explode and a reset of one type at one origin are two inputs, and a
    // server-stamped one (origin = applied) never collides with an owner's.
    inline bool trafo_same_input(const trafo_input& a, uint32_t player, uint8_t type, uint8_t flags, uint32_t origin) {
        return a.player == player && a.type == type && trafo_is_reset(a.flags) == trafo_is_reset(flags) && a.origin == origin
            && (a.flags & TRAFO_SERVER) == (flags & TRAFO_SERVER);
    }

    class trafo_input_log {
    public:
        // Inserted by applied tick; inside one tick in (player, type, kind)
        // order - inputs only set flags on distinct type sets (the ledger
        // never puts two inputs of one type set into one tick), so the order
        // only has to be the same everywhere.  An input already logged (same
        // key) is replaced.
        void add(const trafo_input& in) {
            drop(in.player, in.type, in.flags, in.origin);
            auto& tick = by_tick_[in.applied];
            const auto pos = std::find_if(tick.begin(), tick.end(), [&in](const trafo_input& x) {
                return std::make_tuple(x.player, x.type, trafo_is_reset(x.flags), x.origin)
                     > std::make_tuple(in.player, in.type, trafo_is_reset(in.flags), in.origin);
            });
            tick.insert(pos, in);
        }
        // The inputs applied at `tick`, in their order.
        std::vector<trafo_input> at(uint32_t tick) const {
            const auto it = by_tick_.find(tick);
            return it == by_tick_.end() ? std::vector<trafo_input>{} : it->second;
        }
        // The newest explosion (or reset) of (player, type) applied at or
        // before `tick`; null when none.
        const trafo_input* newest(uint32_t player, uint8_t type, bool reset, uint32_t tick) const {
            for (auto it = by_tick_.upper_bound(tick); it != by_tick_.begin();) {
                --it;
                for (auto e = it->second.rbegin(); e != it->second.rend(); ++e)
                    if (e->player == player && e->type == type && trafo_is_reset(e->flags) == reset) return &*e;
            }
            return nullptr;
        }
        const trafo_input* newest_explosion(uint32_t player, uint8_t type, uint32_t tick) const {
            return newest(player, type, false, tick);
        }
        // An input re-keyed to another applied tick (an own input the server
        // MOVED).  False when it is not logged.
        bool move(uint32_t player, uint8_t type, uint8_t flags, uint32_t origin, uint32_t applied, int32_t seed) {
            trafo_input found;
            if (!take(player, type, flags, origin, found)) return false;
            found.applied = applied;
            found.seed = seed;
            add(found);
            return true;
        }
        // An input dropped (REJECTED, SUPERSEDED).  False when it is not logged.
        bool drop(uint32_t player, uint8_t type, uint8_t flags, uint32_t origin) {
            trafo_input ignored;
            return take(player, type, flags, origin, ignored);
        }
        // Entries applied before `tick` dropped, except each type set's newest
        // explosion and newest reset among them (design 2.5: "the newest
        // explosion of (p, T) at or before t" outlives the history).  The
        // newest is taken among the entries applied before `tick` only: an
        // input already logged for `tick` or later (a lead's pending one)
        // must not displace the one that is still the newest at or before
        // every t in [tick, its applied tick).  Returns how many.
        size_t prune(uint32_t tick) {
            std::map<std::tuple<uint32_t, uint8_t, bool>, std::pair<uint32_t, uint32_t>> newest;   // key -> (applied, origin)
            for (const auto& [t, inputs]: by_tick_) {
                if (!trafo_tick_before(t, tick)) break;
                for (const auto& e: inputs) newest[{e.player, e.type, trafo_is_reset(e.flags)}] = {e.applied, e.origin};
            }
            size_t n = 0;
            for (auto it = by_tick_.begin(); it != by_tick_.end() && trafo_tick_before(it->first, tick);) {
                auto& inputs = it->second;
                const size_t before = inputs.size();
                inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [&](const trafo_input& e) {
                    const auto k = newest.find({e.player, e.type, trafo_is_reset(e.flags)});
                    return k == newest.end() || k->second != std::make_pair(e.applied, e.origin);
                }), inputs.end());
                n += before - inputs.size();
                it = inputs.empty() ? by_tick_.erase(it) : std::next(it);
            }
            return n;
        }
        size_t size() const {
            size_t n = 0;
            for (const auto& [t, inputs]: by_tick_) n += inputs.size();
            return n;
        }
        void clear() { by_tick_.clear(); }
        template <class F> void each(F&& f) const {
            for (const auto& [t, inputs]: by_tick_)
                for (const auto& e: inputs) f(e);
        }

    private:
        bool take(uint32_t player, uint8_t type, uint8_t flags, uint32_t origin, trafo_input& out) {
            for (auto it = by_tick_.begin(); it != by_tick_.end(); ++it) {
                auto& inputs = it->second;
                const auto e = std::find_if(inputs.begin(), inputs.end(), [&](const trafo_input& x) {
                    return trafo_same_input(x, player, type, flags, origin);
                });
                if (e == inputs.end()) continue;
                out = *e;
                inputs.erase(e);
                if (inputs.empty()) by_tick_.erase(it);
                return true;
            }
            return false;
        }
        std::map<uint32_t, std::vector<trafo_input>> by_tick_;
    };

    // ---- record v3's restore, decided (design 4.5; game/script_replay.hpp
    // carries the bytes).  `recorded`: the trafo scripts a record lists, by
    // id; `selected`: the trafo list now; `exists`: whether an id still names
    // a script.  A recorded script is restored from the record (a deselected
    // set's too: inert by the retirement rule, so a no-op); a selected script
    // the record does not list (its set was made after the record) is put to
    // its set's built state; a recorded id that no longer exists fails the
    // restore (retirement destroys a set only after every record listing it
    // has left the history, so a correct session never meets it).
    struct trafo_record_plan {
        std::vector<uint32_t> from_record;   // in the record's order
        std::vector<uint32_t> to_built;      // in the trafo list's order
        std::vector<uint32_t> vanished;      // recorded ids that name nothing: the restore fails
        bool ok() const { return vanished.empty(); }
    };
    inline trafo_record_plan plan_trafo_record_restore(const std::vector<uint32_t>& recorded,
                                                       const std::vector<uint32_t>& selected,
                                                       const std::function<bool(uint32_t)>& exists) {
        trafo_record_plan plan;
        for (uint32_t id: recorded) {
            if (exists && !exists(id)) plan.vanished.push_back(id);
            else plan.from_record.push_back(id);
        }
        for (uint32_t id: selected)
            if (std::find(recorded.begin(), recorded.end(), id) == recorded.end()) plan.to_built.push_back(id);
        return plan;
    }
}
