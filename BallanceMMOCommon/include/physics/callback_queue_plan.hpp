#pragma once
// Gap Q (rollback-engine-gaps design 2.2.2-2.2.4, step B7EG): the decisions
// the PreSimulate queue functions of physics_savestate_common.cpp make, as
// pure functions over block ids, so the unit tests reach them without CK or a
// physics manager.  The bridge functions (callbacks_hold, callbacks_unhold,
// restorer::check_callbacks) walk the real queue and call these.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bmmo::physics::queue_plan {

    // One entry of one slot of the queue, as the hold sees it.
    struct entry {
        uint32_t block = 0;   // the entry's m_BehaviorID
        bool hold = false;    // an entry of a script the replay does not execute
    };

    // Where a held entry was.
    struct place {
        int index = 0;               // its index in the slot before the hold
        uint32_t predecessor = 0;    // the block of the nearest older entry left in the queue, 0: none
        int occurrence = 0;          // that entry is the occurrence-th (1-based) entry of `predecessor` left in the slot
        bool after_held = false;     // the entry right before it was held too
    };

    // The held entries of one slot, oldest first.  The predecessor is named
    // by block and occurrence: entries of one block are interchangeable
    // (design 1.1), but "after the first entry of b" would put an entry that
    // followed two entries of b between them.
    inline std::vector<place> plan_hold(const std::vector<entry>& slot) {
        std::vector<place> out;
        std::map<uint32_t, int> seen;
        uint32_t predecessor = 0;
        int occurrence = 0;
        bool previous_held = false;
        for (int i = 0; i < static_cast<int>(slot.size()); ++i) {
            const entry& e = slot[static_cast<size_t>(i)];
            if (!e.hold) {
                predecessor = e.block;
                occurrence = ++seen[e.block];
                previous_held = false;
                continue;
            }
            place p;
            p.index = i;
            p.predecessor = predecessor;
            p.occurrence = predecessor ? occurrence : 0;
            p.after_held = previous_held;
            previous_held = true;
            out.push_back(p);
        }
        return out;
    }

    // Where a held entry goes back.  `now` is the slot as it is (block ids),
    // `reinserted[i]` true for an entry this unhold already put back;
    // `cursor` is one past the previous entry put back into this slot (0
    // before the first).  The held entries of a slot go back oldest first.
    // `clamped` is set when the predecessor is not where the hold found it:
    // the replay did not end with the selected entries of the live run.
    inline int unhold_position(const std::vector<uint32_t>& now, const std::vector<bool>& reinserted, const place& h,
                               int cursor, bool& clamped) {
        clamped = false;
        const int len = static_cast<int>(now.size());
        int at = -1;
        if (h.after_held || h.predecessor == 0) {
            at = cursor;
        } else {
            int seen = 0;
            for (int i = 0; i < len; ++i) {
                if (i < static_cast<int>(reinserted.size()) && reinserted[static_cast<size_t>(i)]) continue;
                if (now[static_cast<size_t>(i)] != h.predecessor) continue;
                if (++seen == h.occurrence) {
                    at = i + 1;
                    break;
                }
            }
            if (at < 0) {
                at = std::max(cursor, std::min(h.index, len));
                clamped = true;
            } else if (at < cursor) {
                // The older held entries already went back after it: the
                // selected entries came back in another order.
                at = cursor;
                clamped = true;
            }
        }
        return std::min(at, len);
    }

    // The whole put-back of one slot, for the tests: `now` gains `held_blocks`
    // (one per place, oldest first); returns the clamps.
    inline int unhold_slot(std::vector<uint32_t>& now, const std::vector<place>& held, const std::vector<uint32_t>& held_blocks) {
        std::vector<bool> reinserted(now.size(), false);
        int cursor = 0, clamped = 0;
        for (size_t k = 0; k < held.size() && k < held_blocks.size(); ++k) {
            bool c = false;
            const int at = unhold_position(now, reinserted, held[k], cursor, c);
            if (c) ++clamped;
            now.insert(now.begin() + at, held_blocks[k]);
            reinserted.insert(reinserted.begin() + at, true);
            cursor = at + 1;
        }
        return clamped;
    }

    // restorer::check_callbacks: whether an entry takes part in the
    // comparison.  No scope: every entry (mode 0, as before v23).  A block
    // that is gone: a recorded entry is compared (fail-closed), a live one
    // cannot name its script and is not.
    inline bool compared(const std::set<uint32_t>* scope, bool block_exists, uint32_t owner_script, bool live_side) {
        if (!scope) return true;
        if (!block_exists) return !live_side;
        return scope->count(owner_script) != 0;
    }

    // ------------------------------------------------------------------
    // The record's queue section (design 2.2.3, Q3) and the bridge's
    // rebuild of it (close-out step S3: the decisions as templates over a
    // queue model, so Q-U1 / Q-U2 run them over a fake queue backend; the
    // record, game/script_replay.hpp, and the bridge, callbacks_rebuild in
    // physics_savestate_common.cpp, call these).  An entry type E has
    // `behavior`, `owner_script`, `slot` and `bmmo` (queued_callback,
    // bmmo_physics_callback).
    // ------------------------------------------------------------------

    // script_replay::capture: the entries the record keeps, in queue order -
    // never the bridge's own, never one whose block names no script, and only
    // those of the recorded scripts (`scripts`: the selection's roots and the
    // trafo sets' scripts, in any order).
    template<class E>
    std::vector<const E*> recorded_entries(const std::vector<E>& all, std::vector<uint32_t> scripts) {
        std::vector<const E*> kept;
        if (all.empty()) return kept;
        std::sort(scripts.begin(), scripts.end());
        for (const E& e: all)
            if (!e.bmmo && e.owner_script
                    && std::binary_search(scripts.begin(), scripts.end(), static_cast<uint32_t>(e.owner_script)))
                kept.push_back(&e);
        return kept;
    }

    // script_replay::restore: the scripts whose entries the record's are -
    // every written root, every selected script absent from the record, every
    // trafo script that exists - sorted, each once.
    inline std::vector<uint32_t> rebuild_scope(std::vector<uint32_t> ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    // script_replay::restore: the recorded entries the bridge rebuilds, in
    // the record's order, each with its block's owner as it is now.
    // `owner_of(block, owner)` returns false for a block that is gone: the
    // restore then refuses (false, `gone` = that block) and nothing changes.
    template<class E, class OwnerOf>
    bool rebuild_target(const std::vector<E>& recorded, const std::vector<uint32_t>& scope, OwnerOf owner_of,
                        std::vector<E>& target, uint32_t& gone) {
        target.clear();
        for (E e: recorded) {
            uint32_t owner = 0;
            if (!owner_of(static_cast<uint32_t>(e.behavior), owner)) {
                gone = static_cast<uint32_t>(e.behavior);
                target.clear();
                return false;
            }
            e.owner_script = owner;
            if (!std::binary_search(scope.begin(), scope.end(), owner)) continue;
            target.push_back(e);
        }
        return true;
    }

    // callbacks_rebuild: every entry of `in_scope`'s scripts out of the queue
    // (the bridge's own and empty places stay), then `target`'s in its order,
    // each at the end of its slot.  Every target is made first: a refusal (a
    // block gone, outside the scope, a bad slot, no factory) frees what was
    // made and changes nothing.  Returns the entries removed, -1 with `error`.
    // The queue backend Q has:
    //   bool exists(uint32_t block); uint32_t owner(uint32_t block);
    //   std::string text(uint32_t block);
    //   typename Q::made make(uint32_t block)   (a falsy value: no factory);
    //   void release(typename Q::made);
    //   int len(int slot); bool kept_at(int slot, int i)   (empty or the bridge's own);
    //   uint32_t owner_at(int slot, int i); void remove(int slot, int i);
    //   void append(int slot, typename Q::made).
    template<class Q, class E>
    int rebuild(Q& q, const std::set<uint32_t>& in_scope, const std::vector<E>& target, std::string& error) {
        error.clear();
        std::vector<std::pair<int, typename Q::made>> made;
        made.reserve(target.size());
        const auto undo = [&q, &made]() {
            for (auto& m: made) q.release(m.second);
            made.clear();
        };
        for (const E& t: target) {
            const uint32_t block = static_cast<uint32_t>(t.behavior);
            if (!q.exists(block)) {
                undo();
                error = "the queued block " + std::to_string(block) + " is gone";
                return -1;
            }
            if (!in_scope.count(q.owner(block))) {
                undo();
                error = "the queued block " + q.text(block) + " is outside the rebuilt scripts";
                return -1;
            }
            if (t.slot > 2) {
                undo();
                error = "the queued block " + q.text(block) + " names slot " + std::to_string(t.slot);
                return -1;
            }
            typename Q::made cb = q.make(block);
            if (!cb) {
                undo();
                error = "no callback factory for the queued block " + q.text(block);
                return -1;
            }
            made.emplace_back(static_cast<int>(t.slot), cb);
        }
        int removed = 0;
        for (int slot = 0; slot < 3; ++slot) {
            for (int i = q.len(slot) - 1; i >= 0; --i) {
                if (q.kept_at(slot, i)) continue;
                if (!in_scope.count(q.owner_at(slot, i))) continue;
                q.remove(slot, i);
                ++removed;
            }
        }
        for (auto& m: made) q.append(m.first, m.second);
        made.clear();
        return removed;
    }
}
