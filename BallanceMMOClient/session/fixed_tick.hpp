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

    class fixed_tick_driver {
    public:
        void enable(IBML* bml);
        void disable(IBML* bml);
        bool enabled() const { return enabled_; }

        // Call once at the start of OnProcess.  Returns the index of the tick
        // that this behaviour frame represents (0 for the first frame after
        // enable).  Applies the delta clamp and the wall-clock pacing.
        uint64_t on_process(IBML* bml);

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
        uint64_t ticks_ = 0;
        std::chrono::steady_clock::time_point origin_{};
        float last_delta_ms_ = 0.0f;
        uint64_t skipped_renders_ = 0;
        uint64_t waited_frames_ = 0;
        uint64_t rebases_ = 0;
    };
}
