#pragma once

// Fixed 66 Hz behaviour ticks for the running game (design section 3.1).
//
// The retail CKTimeManager clamps every frame delta into
// [minimum, maximum]; pinning both to the tick length makes each
// CKContext::Process advance the scripts and physics by exactly one tick
// regardless of the wall clock.  Pacing keeps the tick count aligned with
// real time: when the game falls behind, rendering is skipped so the Player
// loop calls Process again immediately; when it is ahead, the game thread
// waits for the next tick boundary.

#include <chrono>
#include <cstdint>

class IBML;

namespace bmmo::session {
    inline constexpr double kTickRate = 66.0;
    inline constexpr float kFixedDeltaMs = 1000.0f / 66.0f;

    // A SessionLead's slew of the schedule (late-player design 2.4 "Retail";
    // session-client-core design 6.2 row R5).  The session that owns the
    // decision (the core host) implements it; on_process asks it once per
    // counted frame, before the schedule is read.  Without one (the legacy
    // path, no core session) the pacing is exactly what it was.
    class pacer_slew {
    public:
        virtual ~pacer_slew() = default;
        // `now` and how far the schedule is behind (the expected frame less
        // the frame about to run; >= 0: a frame is due).  Returns the ticks
        // the origin moves: +1 one tick earlier (the schedule runs ahead, the
        // pacing catches up by skipping a render: 2x), -1 one tick later (the
        // pacing waits: 0.9x at one in ten), 0 nothing.
        virtual int step(std::chrono::steady_clock::time_point now, double behind) = 0;
        // The rebase threshold: kMaxCatchUpTicks plus what a slew still has
        // to move, so a slew is not read as a stall.
        virtual double rebase_threshold() const = 0;
    };

    class fixed_tick_driver {
    public:
        void enable(IBML* bml);
        void disable(IBML* bml);
        bool enabled() const { return enabled_; }

        // Call once at the start of OnProcess.  Returns the index of the tick
        // that this behaviour frame represents (0 for the first frame after
        // enable).  Applies the delta clamp and the wall-clock pacing; with a
        // `pacer` (a core session, step R5) its step moves the origin first
        // and its threshold replaces kMaxCatchUpTicks.
        uint64_t on_process(IBML* bml, pacer_slew* pacer = nullptr);

        // Moves the schedule's origin by `ticks` (positive: earlier, the
        // schedule runs ahead).  No tick is counted or skipped: the pacing
        // below catches up or waits.
        void slew(int ticks);

        // The anchor hold (design 9.26 phase alignment): the session waits
        // inside the anchor frame, so no frame reaches on_process while held
        // (if one did, it is neither counted nor paced).  Releasing moves the
        // schedule's origin forward by the time the hold took, so the wait is
        // not lag the pacing tries to catch up with (which would count as a
        // rebase and ask the session for a resync).
        void set_hold(bool hold);
        bool held() const { return held_; }

        uint64_t ticks() const { return ticks_; }
        // Diagnostics for the last frame.
        float last_delta_ms() const { return last_delta_ms_; }
        uint64_t skipped_renders() const { return skipped_renders_; }
        uint64_t waited_frames() const { return waited_frames_; }
        uint64_t rebases() const { return rebases_; }
        // Ticks the game may fall behind before pacing gives up catching up
        // (a level load) and restarts the schedule from the current frame.
        static constexpr double kMaxCatchUpTicks = 33.0;

    private:
        bool enabled_ = false;
        bool held_ = false;
        uint64_t ticks_ = 0;
        std::chrono::steady_clock::time_point origin_{};
        float last_delta_ms_ = 0.0f;
        uint64_t skipped_renders_ = 0;
        uint64_t waited_frames_ = 0;
        uint64_t rebases_ = 0;
    };
}
