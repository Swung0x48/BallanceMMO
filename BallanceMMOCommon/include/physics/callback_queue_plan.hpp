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
}
