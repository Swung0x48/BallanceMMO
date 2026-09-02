#include "fixed_tick.hpp"

#include "bml_includes.h"

#include <thread>

namespace bmmo::session {
    namespace {
        constexpr double kTickSeconds = 1.0 / kTickRate;
    }

    void fixed_tick_driver::enable(IBML* bml) {
        auto* time = bml->GetTimeManager();
        time->SetTimeScaleFactor(1.0f);
        time->SetMinimumDeltaTime(kFixedDeltaMs);
        time->SetMaximumDeltaTime(kFixedDeltaMs);
        // The Player's loop honours CKTimeManager limits before calling
        // Process; a free-running loop lets the pacing below decide.
        time->ChangeLimitOptions(CK_FRAMERATE_FREE, CK_RATE_NOP);
        enabled_ = true;
        ticks_ = 0;
        skipped_renders_ = 0;
        waited_frames_ = 0;
        origin_ = std::chrono::steady_clock::now();
    }

    void fixed_tick_driver::disable(IBML* bml) {
        if (!enabled_) return;
        enabled_ = false;
        auto* time = bml->GetTimeManager();
        // Retail defaults (CKTimeManager::OnCKReset): 1 ms .. 200 ms.
        time->SetMinimumDeltaTime(1.0f);
        time->SetMaximumDeltaTime(200.0f);
        time->ChangeLimitOptions(CK_FRAMERATE_SYNC, CK_RATE_NOP);
    }

    uint64_t fixed_tick_driver::on_process(IBML* bml) {
        if (!enabled_) return 0;
        auto* time = bml->GetTimeManager();
        last_delta_ms_ = time->GetLastDeltaTime();
        // Re-assert every frame: retail "Time Settings" blocks may touch the
        // limits when scripts start a level.
        time->SetTimeScaleFactor(1.0f);
        time->SetMinimumDeltaTime(kFixedDeltaMs);
        time->SetMaximumDeltaTime(kFixedDeltaMs);

        const uint64_t tick = ticks_++;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - origin_).count();
        const double expected = elapsed * kTickRate;
        if (static_cast<double>(ticks_) + kMaxCatchUpTicks < expected) {
            // Far behind (level load, debugger): fast-forwarding would render
            // nothing for seconds, so restart the schedule from here.
            origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(ticks_) * kTickSeconds));
            ++rebases_;
        } else if (static_cast<double>(ticks_) + 1.0 < expected) {
            // Behind by more than one tick: skip the next render so the loop
            // reaches Process again as soon as possible.
            bml->SkipRenderForNextTick();
            ++skipped_renders_;
        } else if (static_cast<double>(ticks_) > expected) {
            // Ahead: wait until this tick's wall-clock slot begins.
            const double due = static_cast<double>(ticks_) * kTickSeconds;
            const auto deadline = origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(due));
            if (deadline > now) {
                std::this_thread::sleep_until(deadline);
                ++waited_frames_;
            }
        }
        return tick;
    }
}
