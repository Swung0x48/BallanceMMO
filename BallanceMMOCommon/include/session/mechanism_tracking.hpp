#pragma once

// Which local body, if any, a snapshot row about a shared mechanism names
// (design 9.25).  Since 9.25 the client simulates the script-driven bodies
// itself again and the rollback engine corrects them, so every mechanism row
// has to be mapped onto one of THIS client's bodies before it can be compared -
// and onto the right one:
//
//  * the same dictionary name can be carried by several server owners (Level 8
//    has two P_Modul_30_Wippe instances 332 m apart in different sectors), so
//    the owners sharing a name are narrowed to the one whose newest row is
//    nearest our body of that name, and the pick is dropped outright beyond
//    identity_guard_m (correction.hpp, 20 m) - the pre-9.17 identity guard,
//    without its per-tick pose history;
//  * a name this client has no body for is another sector's instance: it
//    resolves to nothing and its rows are ignored, because a tracked name with
//    no body would be dropped from the tracked set on every record.
//
// The probe that answers "do we have this body" is a bridge call, so it is not
// made per row (11 148 snapshots x 15 owners in the 9.24 session) but only when
// the dictionary grew, when the local body set changed (the caller marks the
// registry dirty on a created/deleted entry) or every kResolveInterval ticks.
//
// Shared by the retail mod and the headless session client so the two cannot
// drift apart again (findings/A5 section 3.2).

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../entity/session.hpp"
#include "../physics/physics_rt_api.h"

namespace bmmo::session {
    class mechanism_registry {
    public:
        // The local body of an entity; false when this client has no such body.
        using body_probe = std::function<bool(const char* entity, bmmo_physics_body_state& out)>;

        // Beyond this the row names another instance of the same name, not ours
        // (correction_thresholds::identity_guard_m).
        double identity_guard_m = 20.0;
        // A re-resolve costs one probe per distinct name; the world changes far
        // more slowly than that, and every real change marks the registry dirty
        // anyway.  This is the safety net for the ones nobody reported.
        static constexpr uint32_t kResolveInterval = 66;   // ticks

        // The dictionary, from full snapshots (a delta row carries no name).
        void note_name(uint32_t owner, const std::string& name) {
            if (name.empty()) return;
            entry& e = entries_[owner];
            if (e.name == name) return;
            e.name = name;
            e.entity.clear();
            dirty_ = true;
        }

        // The newest authoritative pose of one owner, for the identity guard
        // only: nothing renders from it since 9.25.
        void note_row(uint32_t tick, const body_state& row) {
            entry& e = entries_[row.owner];
            if (e.have_row && e.tick > tick) return;   // a queued older snapshot
            e.have_row = true;
            e.tick = tick;
            for (int k = 0; k < 3; ++k) e.position[k] = row.position[k];
        }

        // The local body set changed (a bridge created/deleted entry, a sector
        // event, a failed write): the next resolve pass asks the world again.
        void mark_dirty() { dirty_ = true; }

        bool due(uint32_t tick) const {
            // Unsigned arithmetic on purpose: a rebase that renumbers us
            // backwards leaves a huge difference and resolves at once.
            return dirty_ || !resolved_once_ || (tick - last_resolve_tick_) >= kResolveInterval;
        }

        // One probe per distinct name, the owners of a name narrowed to the
        // instance nearest to it.
        void resolve(uint32_t tick, const body_probe& probe) {
            dirty_ = false;
            resolved_once_ = true;
            last_resolve_tick_ = tick;
            tracked_.clear();
            std::map<std::string, std::vector<uint32_t>> owners_by_name;
            for (auto& [owner, e]: entries_) {
                e.entity.clear();
                if (!e.name.empty()) owners_by_name[e.name].push_back(owner);
            }
            for (const auto& [name, owners]: owners_by_name) {
                bmmo_physics_body_state local{};
                if (!probe(name.c_str(), local)) continue;   // another sector's instance
                uint32_t chosen = 0;
                bool have_chosen = false;
                double best = 0.0;
                for (uint32_t owner: owners) {
                    const entry& e = entries_[owner];
                    if (!e.have_row) continue;
                    double distance = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        const double d = e.position[k] - local.position[k];
                        distance += d * d;
                    }
                    if (!have_chosen || distance < best) {
                        chosen = owner;
                        best = distance;
                        have_chosen = true;
                    }
                }
                if (!have_chosen) {
                    // No row for any of them yet (the name arrived with a full
                    // snapshot whose rows we have not stored, or every row is
                    // older than the dictionary).  One owner is unambiguous;
                    // several are not, and guessing would drive the wrong body.
                    if (owners.size() != 1) continue;
                    chosen = owners.front();
                } else if (std::sqrt(best) > identity_guard_m) {
                    continue;   // the nearest instance is not ours
                }
                entries_[chosen].entity = name;
                tracked_.push_back(name);
            }
        }

        // The local entity a row's owner names, or "" when this client has no
        // body for it (another sector, or the identity guard refused the pick).
        std::string entity_of(uint32_t owner) const {
            const auto it = entries_.find(owner);
            return it == entries_.end() ? std::string() : it->second.entity;
        }

        // The names to track: exactly the ones with a local body, in dictionary
        // order (rollback_tracked::mechanisms).
        const std::vector<std::string>& tracked_entities() const { return tracked_; }

        size_t known() const { return entries_.size(); }
        size_t resolved() const { return tracked_.size(); }

        void clear() {
            entries_.clear();
            tracked_.clear();
            dirty_ = true;
            resolved_once_ = false;
            last_resolve_tick_ = 0;
        }

    private:
        struct entry {
            std::string name;      // dictionary name, from full snapshots
            std::string entity;    // resolved local entity ("" = not ours)
            bool have_row = false;
            uint32_t tick = 0;
            double position[3] = {};
        };
        std::map<uint32_t, entry> entries_;   // by the server's owner index
        std::vector<std::string> tracked_;
        bool dirty_ = true;
        bool resolved_once_ = false;
        uint32_t last_resolve_tick_ = 0;
    };
}
