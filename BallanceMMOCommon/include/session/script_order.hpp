#pragma once

// The order the live frames ran the replay's scripts in (step RO1), the
// CK-free half: host-local, kept beside the rollback history, never on the
// wire and in no record.
//
// The live frame runs the active objects in CKBehaviorManager::m_BeObjects
// order (CK2 CKBehaviorManager.cpp Execute / ManageObjectsActivity): an
// object enters it appended in the next-frame hash table's order and the
// whole array is then sorted by priority alone with an unstable sort
// (XDeterministicQSort over CKBeObject::BeObjectPrioritySort), so among
// owners of one priority the order is neither the CK id order nor stable
// across activations; AddObject, RemoveAllObjects and SequenceDeleted change
// it too.  An object runs its scripts in its own script array's order.  The
// replay used to run the selected scripts in (-priority, CK id) order, so
// two scripts of one priority that both make bodies in one frame (the seven
// scripts of P_Modul_41's pieces) made them in another order than the live
// frame did - IVP's revive list, the physics_RT movable list, the OV tree
// and the sim-unit numbering all follow creation order (V9's tick-266 split,
// sector-placement-design.md "As built (step SP7 = S7)" / "As built (step
// RO1)").
//
// So after every live frame the host records the order its own frame ran
// the selected scripts in (game/script_replay.hpp live_frame_order), keyed
// by the tick the rollback replays that frame as (its pre_step tick), and
// the replayed frame runs them in that order (order_by_live_rank, through
// session/rollback_binding_scripts.hpp).  Stored only when it changes.
//
// What this does NOT cover (measured at step RO2): (A) a script whose first
// execution is inside a replay - an activation the live frame of that tick
// did not run (it ran later, or not at all): it takes the fallback order;
// (B) two hosts whose live frames ran the same scripts in different orders
// (the server and a retail client): each replays its own order, which is
// exact for its own history only.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace bmmo::session {

    // The replayed list in the live order of one frame: a script the live
    // order names takes its rank there; one it does not name (the live frame
    // did not run its owner - an activation first run inside the replay, or
    // no record) falls back to the replay's own order, (-priority, CK id),
    // among the ranked ones of its priority after them.  Stable: the input
    // order (select's (-priority, id)) breaks every remaining tie.  `priority`
    // gives a script's owner priority (higher runs first); the live order is
    // itself sorted by it (CKBehaviorManager::SortObjects), so for ranked
    // scripts the key is their rank.
    template <class Id, class PriorityOf>
    void order_by_live_rank(std::vector<Id>& scripts, const std::vector<Id>& live, PriorityOf&& priority) {
        if (scripts.size() < 2 || live.empty()) return;
        std::map<Id, size_t> rank;
        for (size_t i = 0; i < live.size(); ++i) rank.emplace(live[i], i);
        struct keyed {
            int neg_priority;
            int unranked;
            size_t rank;
            Id id;
        };
        std::vector<keyed> keys;
        keys.reserve(scripts.size());
        for (const Id& id: scripts) {
            const auto r = rank.find(id);
            keys.push_back({-static_cast<int>(priority(id)), r == rank.end() ? 1 : 0, r == rank.end() ? 0 : r->second, id});
        }
        std::stable_sort(keys.begin(), keys.end(), [](const keyed& a, const keyed& b) {
            return std::tie(a.neg_priority, a.unranked, a.rank) < std::tie(b.neg_priority, b.unranked, b.rank);
        });
        for (size_t i = 0; i < keys.size(); ++i) scripts[i] = keys[i].id;
    }

    // The live orders by tick, each kept from the tick it was first seen
    // until the next change (a host records every frame; only a change is
    // stored).  Ids are CK ids as 32-bit values.
    class live_script_order {
    public:
        using order = std::vector<uint32_t>;
        void clear() { by_tick_.clear(); }
        // The order the live frame of `tick` ran.  A record at or before an
        // earlier one (a resync's re-numbered ticks, a tool re-running its
        // straight pass) drops every later entry first: that future is being
        // lived again.  True when it was stored (a change).
        bool record(uint32_t tick, order&& live) {
            by_tick_.erase(by_tick_.lower_bound(tick), by_tick_.end());
            if (!by_tick_.empty() && *std::prev(by_tick_.end())->second == live) return false;
            by_tick_.emplace(tick, std::make_shared<const order>(std::move(live)));
            ++changes_;
            while (by_tick_.size() > 1 && std::next(by_tick_.begin())->first + kKeep < tick) by_tick_.erase(by_tick_.begin());
            return true;
        }
        // The order in force at `tick` (the latest recorded at or before
        // it), or nullptr when none is.
        const order* at(uint32_t tick) const {
            auto it = by_tick_.upper_bound(tick);
            if (it == by_tick_.begin()) return nullptr;
            return std::prev(it)->second.get();
        }
        // Every entry at a tick >= `tick` dropped (a resync's re-assign, as
        // the activation slots).  Returns how many.
        size_t forget_from(uint32_t tick) {
            size_t n = 0;
            for (auto it = by_tick_.lower_bound(tick); it != by_tick_.end(); it = by_tick_.erase(it)) ++n;
            return n;
        }
        size_t entries() const { return by_tick_.size(); }
        uint64_t changes() const { return changes_; }

    private:
        static constexpr uint32_t kKeep = 4096;   // ticks, as the birth log and the activation slots
        std::map<uint32_t, std::shared_ptr<const order>> by_tick_;
        uint64_t changes_ = 0;
    };
}
