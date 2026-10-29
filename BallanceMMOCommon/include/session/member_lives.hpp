#ifndef BALLANCEMMO_SESSION_MEMBER_LIVES_HPP
#define BALLANCEMMO_SESSION_MEMBER_LIVES_HPP
// Member lives (docs/world-agreement-gaps-design.md 1.2.4, 2.7, 2.8; step
// M1, protocol 18).  CK-free: the rules the server and the client core
// share, and what their unit tests drive.
//
// A LIFE of a player is the server's interval [J, R) of that player in its
// world, from its add to its removal.  One id can have several lives in a
// session (a rejoin on the same connection), and they never overlap.
//
//  J1  every world holds a life's player_entry before any event of it is
//      applied (SessionStart lists the current lives; SessionMember adds
//      the later ones, and a client parks an event of a life it does not
//      know yet);
//  J2  a join order is never held by two lives in the server's world at
//      once: free again only after its holder was removed;
//  J3  every event of a life is applied at a tick in [J, R);
//  J4  a late joiner's base is at or after the R of every life the server
//      still held when the joiner was added.

#include <cstdint>
#include <set>
#include <vector>

#include "../entity/session.hpp"

namespace bmmo::session {

    // A client's knowledge of one life (design 2.8).
    struct member_life {
        player_entry entry;   // id, join order, spawn, life
        uint32_t from = 0;    // J; 0 for a life listed in SessionStart (known from the start)
        uint32_t until = 0;   // R; 0 while open
        // The client journal's bookkeeping: its PLAYER(+) and PLAYER(-)
        // records are written (client_core journal_member_lives).
        bool journaled_add = false, journaled_remove = false;
    };

    inline member_life* find_member_life(std::vector<member_life>& lives, uint32_t player, uint16_t life) {
        for (auto& l: lives)
            if (l.entry.id == player && l.entry.life == life) return &l;
        return nullptr;
    }
    inline const member_life* find_member_life(const std::vector<member_life>& lives, uint32_t player, uint16_t life) {
        for (const auto& l: lives)
            if (l.entry.id == player && l.entry.life == life) return &l;
        return nullptr;
    }

    // `tick` inside [from, until) (from 0: since the start; until 0: open).
    inline bool member_life_covers(const member_life& l, uint32_t tick) {
        if (l.from != 0 && static_cast<int32_t>(tick - l.from) < 0) return false;
        if (l.until != 0 && static_cast<int32_t>(tick - l.until) >= 0) return false;
        return true;
    }

    // The life of `player` a body change applied at `tick` belongs to.  Two
    // lives of one id never overlap (J3), so the applied tick names the life
    // - which is how the core's lookups that carry no life (a pending death
    // of a resync, a recorded mirror's remake, the reconcile's queue) find
    // it.  A life whose removal has not been heard of yet is still open, so
    // where an old and a new life both cover the tick, the newer (the later
    // J) is the one: the old one ended at the new one's J.  When no life
    // covers the tick, the newest one that began at or before it (a replay
    // past an R), else the player's first: a defensive answer only - J3
    // makes it unreachable for an event.  nullptr: no life of the player.
    inline const member_life* member_life_at(const std::vector<member_life>& lives, uint32_t player, uint32_t tick) {
        const member_life* best = nullptr;
        const auto later = [](const member_life& a, const member_life* b) {
            if (!b) return true;
            if (a.from != b->from) return static_cast<int32_t>(a.from - b->from) > 0;
            return a.entry.life > b->entry.life;
        };
        for (const auto& l: lives)
            if (l.entry.id == player && member_life_covers(l, tick) && later(l, best)) best = &l;
        if (best) return best;
        for (const auto& l: lives)
            if (l.entry.id == player && (l.from == 0 || static_cast<int32_t>(tick - l.from) >= 0) && later(l, best))
                best = &l;
        if (best) return best;
        for (const auto& l: lives)
            if (l.entry.id == player) return &l;
        return nullptr;
    }

    // ---------------------------------------------------------- the server
    // J2: the lowest join order neither held by a member nor retiring (held
    // by a life the runner has not removed yet).  `limit` orders exist (256:
    // the uint8_t range, as before step M1; the cap below K is step Q7's).
    inline uint8_t lowest_free_join_order(const std::set<uint8_t>& held, const std::set<uint8_t>& retiring,
                                          uint32_t limit = 256) {
        uint32_t order = 0;
        while (order + 1 < limit && (held.count(static_cast<uint8_t>(order)) || retiring.count(static_cast<uint8_t>(order))))
            ++order;
        return static_cast<uint8_t>(order);
    }

    // Step Q7 (§2.10 "The cap"): a joiner's order under the session's K
    // mechanism slots - the lowest order below `limit` that no life holds or
    // retires; when every order below K is held or retiring and at least one
    // of them is retiring, the joiner waits for its release; when every one
    // is held by a live life, it is refused the physics session.
    enum class join_order_verdict : uint8_t { order, wait, refuse };
    struct join_order_choice {
        join_order_verdict verdict = join_order_verdict::refuse;
        uint8_t order = 0;
    };
    inline join_order_choice capped_join_order(const std::set<uint8_t>& held, const std::set<uint8_t>& retiring,
                                               uint32_t limit) {
        join_order_choice c;
        bool any_retiring = false;
        for (uint32_t order = 0; order < limit && order < 256; ++order) {
            const auto o = static_cast<uint8_t>(order);
            if (!held.count(o) && !retiring.count(o)) {
                c.verdict = join_order_verdict::order;
                c.order = o;
                return c;
            }
            any_retiring = any_retiring || (retiring.count(o) && !held.count(o));
        }
        c.verdict = any_retiring ? join_order_verdict::wait : join_order_verdict::refuse;
        return c;
    }

    // J4: a late joiner's base is not before the removal of any life it
    // was not told about (`horizon`: the largest removal tick of the
    // runner's leaving lives, 0 when none).
    inline uint32_t late_join_base(uint32_t base, uint32_t horizon) {
        return horizon != 0 && static_cast<int32_t>(horizon - base) > 0 ? horizon : base;
    }

    // J-e: a leaver stays in the server's world through the runner's queue
    // when it has a ball (its Unphysicalize is applied at `next` in its rank,
    // as every client applies it) or events queued; one with neither is
    // removed at once - nothing of it is any world's business.
    inline bool leaver_stays(bool has_ball, bool queued) { return has_ball || queued; }
}

#endif // BALLANCEMMO_SESSION_MEMBER_LIVES_HPP
