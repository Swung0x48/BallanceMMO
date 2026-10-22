#pragma once

// The client's half of the per-player lead (late-player design 2.4): what a
// SessionLead asks of the own ticks, and the slew that moves the tick clock
// by its delta.  Pure arithmetic, CK-free, so the rules are unit-tested and
// the retail mod's fixed-tick driver can take the slew over (plan step 15).
//
// A SessionLead of delta k names the range [N, E) the server's input buffer
// for this player was reset over, and the frame it applies at every tick of
// it.  At the client's current tick c (the newest tick it simulated):
//   - the ticks of [N, E) at or before c were simulated with the client's own
//     input: their records are amended to the held frame and replayed from
//     the earliest (the own-correction path of design 1.3);
//   - the ticks of (c, E) are not simulated yet: their live frames are forced
//     to the held frame, so the client plays what the server applies there
//     and no correction follows when the server's notices of them arrive;
//   - the first tick the client plays with its own input again is E, exactly
//     the tick from which the server's buffer, re-anchored at E, takes the
//     client's frames.  No tick is renumbered: every tick is still stepped,
//     recorded and sent in a datagram of its own, stamped with its number.
// Then the clock moves by k.  A raise moves the pacer's origin one tick
// earlier per tick period, so the pacer runs two frames per slot until it has
// gained k; a lower moves it one tick later every tenth tick period, 0.9x (a
// lowering is never urgent, and 0.5x would show as slow motion).

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace bmmo::session {
    // The own ticks a SessionLead touches, from the client's point of view.
    // Half-open ranges; an empty one has from == to.
    struct lead_ranges {
        uint32_t amend_from = 0, amend_to = 0;   // simulated ticks: amend the record, replay
        uint32_t force_from = 0, force_to = 0;   // ticks still ahead: force the live frame
        bool amend() const { return static_cast<int32_t>(amend_to - amend_from) > 0; }
        bool force() const { return static_cast<int32_t>(force_to - force_from) > 0; }
    };

    // [held_from, effective) split at the client's next unsimulated tick
    // `next_unsimulated` (c + 1).  held_from 0 is a SessionLead without a
    // range (a lower, or a raise whose reset found the cursor past E): both
    // ranges are empty and the client only slews.  A client already at or
    // past E (the server over-estimated the round trip) forces nothing; one
    // still before N (it runs behind the session's next tick) amends nothing
    // and forces the whole range - the ticks before N are not the range's.
    inline lead_ranges lead_split_range(uint32_t held_from, uint32_t effective, uint32_t next_unsimulated) {
        lead_ranges r;
        if (held_from == 0 || static_cast<int32_t>(effective - held_from) <= 0) {
            r.amend_from = r.amend_to = r.force_from = r.force_to = effective;
            return r;
        }
        const auto before = [](uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; };
        const uint32_t split = before(next_unsimulated, held_from) ? held_from
                             : (before(effective, next_unsimulated) ? effective : next_unsimulated);
        r.amend_from = held_from;
        r.amend_to = split;
        r.force_from = split;
        r.force_to = effective;
        return r;
    }

    // At most `max_forced` ticks of the forced range (the client's ring of
    // own frames): the ticks past it are left out of the SessionLead - not
    // forced and not marked as learned - so the server's notices of them
    // settle them as any substituted tick (an own correction), instead of
    // being dropped as re-deliveries of a tick nothing forced.  Only a round
    // trip of over a second at a large k comes near it.
    inline lead_ranges lead_cap_forced(lead_ranges r, uint32_t max_forced) {
        if (r.force() && r.force_to - r.force_from > max_forced) r.force_to = r.force_from + max_forced;
        return r;
    }

    // The pacer's rebase threshold (design 2.4, 2.8): a schedule more than 33
    // frames behind means a stall and restarts; while a slew is outstanding
    // it may be that much further behind without being one.
    inline constexpr double kPacerRebaseFrames = 33.0;
    inline double lead_rebase_threshold(int outstanding) {
        return kPacerRebaseFrames + static_cast<double>(std::abs(outstanding));
    }

    // One SessionLead's slew of the tick clock.  The caller asks advance()
    // with the tick periods of wall time since the previous call and how far
    // its pacer is behind its schedule, and moves its origin by what it
    // returns (+1: one tick earlier, the clock runs ahead; -1: one tick
    // later).  At most one tick per call, so a burst of wall time (a long
    // frame) never moves the origin by more than one tick at once.
    class lead_slew {
    public:
        // A lower moves the origin one tick per this many tick periods.
        static constexpr int kLowerPeriods = 10;
        // A raise gains a tick only while the pacer has at most this many
        // frames due: one more frame per slot, never a backlog.  A client
        // that cannot run at 2x (a slow CPU, a stall) slews more slowly
        // instead of falling behind by the slew and reading as a stall.
        static constexpr double kRaiseMaxBehind = 1.0;

        // A new adjustment.  The server takes no decision while one is
        // unacknowledged, and this one is acknowledged only when it ends, so
        // a second never overlaps a first; the caller ends any that does
        // (finish) before starting the next.
        void start(uint32_t seq, int delta) {
            active_ = delta != 0;
            seq_ = seq;
            delta_ = delta;
            applied_ = 0;
            credit_ = 0.0;
        }
        bool active() const { return active_; }
        uint32_t seq() const { return seq_; }
        int delta() const { return delta_; }
        int applied() const { return applied_; }
        // Ticks still to move (signed, delta's side); 0 when idle.
        int outstanding() const { return active_ ? delta_ - applied_ : 0; }
        // The whole delta moved: the caller acknowledges it (finish).
        bool done() const { return active_ && applied_ == delta_; }

        // `wall_ticks`: tick periods since the previous call; `behind`: the
        // pacer's expected frame less its next frame (>= 0: a frame is due).
        int advance(double wall_ticks, double behind) {
            if (!active_ || applied_ == delta_ || wall_ticks <= 0.0) return 0;
            if (delta_ > 0) {
                if (behind >= kRaiseMaxBehind) return 0;
                credit_ = std::min(credit_ + wall_ticks, 1.0);
            } else {
                credit_ = std::min(credit_ + wall_ticks / kLowerPeriods, 1.0);
            }
            if (credit_ < 1.0) return 0;
            credit_ -= 1.0;
            const int step = delta_ > 0 ? 1 : -1;
            applied_ += step;
            return step;
        }

        // The adjustment ends - all of it moved, or cut short (a newer
        // SessionLead superseded it; a new assignment voided it).  Returns
        // what was moved: the acknowledgement's `applied`, between 0 and
        // delta on delta's side.  The rest is dropped, never moved later.
        // A rebase of the pacer does not end it: the ticks it moved are
        // stepped already (a raise gains only while the pacer is less than a
        // frame behind), and it goes on moving the new schedule.
        int finish() {
            const int applied = applied_;
            active_ = false;
            delta_ = 0;
            applied_ = 0;
            credit_ = 0.0;
            return applied;
        }

        // The adjustment ends because this client asks for a resync (design
        // 2.8): the server re-anchors it at L_eff, the first lead plus the
        // acknowledged offset, and the clock the assignment sets has the
        // whole delta in it however much of it the slew had moved.  So the
        // ack, sent before the request on the same reliable channel, says
        // all of it - one that said only the part moved would re-anchor the
        // client short of the lead the server decided, and late again from
        // its first frame.
        int finish_reanchored() {
            const int whole = active_ ? delta_ : 0;
            finish();
            return whole;
        }

    private:
        bool active_ = false;
        uint32_t seq_ = 0;
        int delta_ = 0;
        int applied_ = 0;
        double credit_ = 0.0;
    };
}
