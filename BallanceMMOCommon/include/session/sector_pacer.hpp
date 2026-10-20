#pragma once

// Pacing of the server's sector activations (physics_world::update_sectors).
// Pure bookkeeping, so the rule is unit-tested and the headless harness can
// mirror it.
//
// The retail Gameplay_SectorManager knows one ball and is run from the level's
// own scripts the moment that ball crosses a sector line.  The server has many
// balls and hands the manager the changes to its sector union one at a time:
// its PH-table walk spans about seven frames (it waits for each Activate
// Script), and starting a new run in the middle of one both resets that walk
// and flips CurrentLevel[Activation Phase?] under the mechanism scripts it has
// just started - they read that cell in their first frame and stay dead if it
// is false.  So: one pass per idle manager, and "idle" is two ticks more than
// "not active", so the scripts the last pass started get their own first frame
// before the flag moves again.
//
// The count runs EVERY tick, pending change or not.  It used to run only while
// the desired set differed from the active one, which made every activation
// land exactly two ticks after the tick its event was stamped for: the counter
// never accumulated while the sets agreed and always needed three passes once
// they differed.  A client that activates at the stamp was then two ticks
// ahead of the server on every mechanism of the sector, for the rest of the
// session (docs/lockstep-without-snapshots.md, "the 2-tick residual").

#include <algorithm>

namespace bmmo::session {
    class sector_pacer {
    public:
        // Ticks the manager must have been idle before a pass may start.
        static constexpr int kIdleTicks = 3;

        // Once per tick, before the pass decision, with whether the manager
        // script is active in the engine.  Returns true if a pass may start
        // this tick.  Saturates: a long idle is "idle", not a bigger number.
        bool tick(bool manager_active) {
            idle_ = manager_active ? 0 : std::min(idle_ + 1, kIdleTicks);
            return idle_ >= kIdleTicks;
        }

        // A pass was started this tick: the manager runs from this tick's
        // frame on, and the next pass waits for the walk and the idle ticks.
        void started() { idle_ = 0; }

        int idle_ticks() const { return idle_; }

    private:
        int idle_ = 0;
    };
}
