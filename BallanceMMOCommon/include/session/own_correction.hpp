#pragma once

// The owner's correction of its own substituted ticks (late-player design
// 1.3): which differences a replay can see, and how often an own replay may
// run while the server's notices keep arriving.  Pure bookkeeping, CK-free,
// so the rule is unit-tested and the retail mod can take it over with its
// input rewind.

#include <cstdint>

#include "../entity/session.hpp"
#include "input_relay.hpp"

namespace bmmo::session {
    // The key bits the navigation consumes: the four SetPhysicsForce leaves
    // (ball_navigation.cpp applies keys & 0x0F; Shift and Space are recorded
    // but drive nothing).
    constexpr uint8_t kNavigationKeys = KEY_LEAF_0 | KEY_LEAF_1 | KEY_LEAF_2 | KEY_LEAF_3;

    // Whether a replay fed `a` instead of `b` for the own ball steps a
    // different world.  A replay drives the own ball only through nav_input
    // (rollback.hpp feed), which takes the leaf keys, NAV_ACTIVE and the
    // camera rows; the camera rows reach the physics only through a leaf's
    // force, so they matter while a leaf key is down.  With navigation off in
    // both frames the keys do not matter either (apply shuts every leaf down
    // and returns).  Ball type and the other flags are lifecycle bookkeeping
    // the replay never reads.  Conservative: NAV_ACTIVE differing is always a
    // difference, although an active frame with no key down ends the tick
    // with the same forces as an inactive one.
    inline bool replay_sees_difference(const input_frame& a, const input_frame& b) {
        const bool active_a = (a.flags & INPUT_FLAG_NAV_ACTIVE) != 0;
        const bool active_b = (b.flags & INPUT_FLAG_NAV_ACTIVE) != 0;
        if (active_a != active_b) return true;
        if (!active_a) return false;
        const uint8_t keys_a = a.keys & kNavigationKeys, keys_b = b.keys & kNavigationKeys;
        if (keys_a != keys_b) return true;
        if (keys_a == 0) return false;
        return !same_camera(a, b);
    }

    // At most one own replay per kOwnReplayCoalesceFrames frames.  While a
    // client runs late the server substitutes one of its ticks per server
    // tick, each notice amends one record, and a replay per notice is one
    // replay of about 42 ticks per frame for as long as the lateness lasts.
    // Measured on the lane (lp4_a0/a1): those replays cross the sector
    // activation, which makes them expensive enough to stall the client
    // further, which keeps its frames late, until the schedule rebases into
    // a resync.  The records are still amended at once; only the replay
    // waits, and it carries every tick amended meanwhile (it starts at the
    // earliest).  6 frames: the notices arrive about 42 ticks after their tick
    // and the reach is 48, so the deadline below usually binds at about the
    // same time; a remote correction, which is never held back, takes any
    // waiting own correction along.  Only while more notices are coming (the
    // server's newest header still says it lacks our frame for the tick it
    // simulated): once it has our frames again the episode is over and the
    // waiting replay runs at once, so the own ball converges as soon as it
    // did before the coalescing, not at the reach's edge.
    constexpr uint32_t kOwnReplayCoalesceFrames = 6;

    // Whether a rewind that replayed from `anchor` feeding the records (a
    // lifecycle or forced rewind) already carried a waiting own replay whose
    // earliest amended tick is `earliest`: it started at or before that
    // replay's anchor, earliest - 1, and the records it fed hold the
    // server's frames since the amendment.  A waiting replay run after it
    // would only step the same ticks again.  (A snapshot rollback carries it
    // from any anchor: its rows at the anchor are the server's world, which
    // ran on the substituted frames, and the ticks after it are fed from the
    // amended records - and a replay from before its anchor would restore
    // our own prediction over those rows.)
    inline bool replay_subsumes(uint32_t anchor, uint32_t earliest) {
        return static_cast<int32_t>(anchor - (earliest - 1)) <= 0;
    }

    class own_replay_pacer {
    public:
        // Once per frame, before due().
        void frame() { ++frames_; }

        // Whether an own replay whose earliest amended tick is `earliest`
        // must run now, at `now` (the newest simulated tick).  It may wait
        // while one ran fewer than kOwnReplayCoalesceFrames frames ago, but
        // never past the frame after which its anchor (earliest - 1) would
        // leave the reach: the next frame replays from now + 1, so it waits
        // only while now + 1 - anchor <= reach.  A tick not simulated yet is
        // the caller's to drop: due.  `notices_coming`: whether the server is
        // still substituting our ticks (its newest header of our epoch has
        // acked < tick); false runs the replay now - nothing will join it.
        bool due(uint32_t now, uint32_t earliest, uint32_t reach, bool notices_coming) const {
            if (!notices_coming) return true;
            if (!ran_any_ || frames_ - last_ >= kOwnReplayCoalesceFrames) return true;
            const int64_t depth_next = static_cast<int64_t>(static_cast<int32_t>(now - earliest)) + 2;   // now + 1 - (earliest - 1)
            if (depth_next <= 1) return true;
            return depth_next > static_cast<int64_t>(reach);
        }

        // An own replay ran (alone or with a remote's), this frame - or
        // another rewind made it redundant (replay_subsumes).
        void ran() {
            ran_any_ = true;
            last_ = frames_;
        }

        // A new numbering (a session start or a resync's assignment): the
        // previous epoch's replay does not hold back the first of this one.
        void reset() { ran_any_ = false; }

    private:
        uint64_t frames_ = 0;
        uint64_t last_ = 0;
        bool ran_any_ = false;
    };
}
