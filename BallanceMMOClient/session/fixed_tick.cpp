#include "fixed_tick.hpp"

#include "bml_includes.h"
#include "host_clock.hpp"

#include <thread>

namespace bmmo::session {
    namespace {
        // FULL step SRT1 (realtime-design section 5 item 1, f1): whether the
        // previous counted frame skipped its render.  File-scope, not a
        // member: the driver is a data member of BallanceMMOClient, whose
        // layout BML fixes (no member may be added to it, nor grow inside
        // it); there is one driver.
        bool previous_render_skipped = false;
    }

    void fixed_tick_driver::enable(IBML* bml) {
        previous_render_skipped = false;
        auto* time = bml->GetTimeManager();
        time->SetTimeScaleFactor(1.0f);
        time->SetMinimumDeltaTime(TICK_DELTA_MS);
        time->SetMaximumDeltaTime(TICK_DELTA_MS);
        // The Player's loop honours CKTimeManager limits before calling
        // Process; a free-running loop lets the pacing below decide.
        time->ChangeLimitOptions(CK_FRAMERATE_FREE, CK_RATE_NOP);
        enabled_ = true;
        held_ = false;
        ticks_ = 0;
        skipped_renders_ = 0;
        waited_frames_ = 0;
        origin_ = bmmo::retail::host_clock().now(clock_site::retail_pace_origin);
    }

    void fixed_tick_driver::set_hold(bool hold) {
        if (hold == held_) return;
        held_ = hold;
        if (hold) return;
        // Continue the schedule where the hold interrupted it: the origin moves
        // so that the ticks counted so far are exactly due now.  Without this
        // the pacing would see itself `held frames` behind and either burn
        // through them or restart the schedule (a rebase, which the session
        // reads as a broken numbering and answers with a resync).
        origin_ = bmmo::retail::host_clock().now(clock_site::retail_pace_origin) - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(ticks_) * TICK_SECONDS));
    }

    void fixed_tick_driver::release_hold_owing(double max_behind) {
        if (!held_) return;
        held_ = false;
        const auto now = bmmo::retail::host_clock().now(clock_site::retail_pace_origin);
        const double behind = std::chrono::duration<double>(now - origin_).count() * TICK_RATE - static_cast<double>(ticks_);
        if (behind > max_behind)
            origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>((static_cast<double>(ticks_) + max_behind) * TICK_SECONDS));
    }

    void fixed_tick_driver::release_hold_at(std::chrono::steady_clock::time_point at) {
        held_ = false;
        origin_ = at - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(ticks_) * TICK_SECONDS));
    }

    void fixed_tick_driver::disable(IBML* bml) {
        if (!enabled_) return;
        enabled_ = false;
        held_ = false;
        auto* time = bml->GetTimeManager();
        time->SetTimeScaleFactor(1.0f);
        // Retail defaults (CKTimeManager::OnCKReset): 1 ms .. 200 ms.
        time->SetMinimumDeltaTime(1.0f);
        time->SetMaximumDeltaTime(200.0f);
        time->ChangeLimitOptions(CK_FRAMERATE_SYNC, CK_RATE_NOP);
    }

    void fixed_tick_driver::slew(int ticks) {
        if (ticks == 0) return;
        origin_ -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(ticks) * TICK_SECONDS));
    }

    uint64_t fixed_tick_driver::on_process(IBML* bml, pacer_slew* pacer) {
        if (!enabled_) return 0;
        auto* time = bml->GetTimeManager();
        last_delta_ms_ = time->GetLastDeltaTime();
        // Re-assert every frame: retail "Time Settings" blocks may touch the
        // limits when scripts start a level.
        time->SetTimeScaleFactor(1.0f);
        time->SetMinimumDeltaTime(TICK_DELTA_MS);
        time->SetMaximumDeltaTime(TICK_DELTA_MS);
        // Held (design 9.26): the session waits inside the anchor frame, so no
        // frame should get here; one that does is neither counted nor paced.
        if (held_) return ticks_;

        const uint64_t tick = ticks_++;
        // K7b: the schedule is the session clock's, so a capture holds it
        // and a replay runs it unpaced (its waits sleep nothing).
        const auto now = bmmo::retail::host_clock().now(clock_site::pace_expected);
        double max_catch_up = kMaxCatchUpTicks;
        if (pacer) {
            // A SessionLead's slew (step R5) moves the origin before the
            // schedule is read, as the headless pacer does: `ticks_` is the
            // frame about to run, the one `expected` is compared with below.
            const double behind = std::chrono::duration<double>(now - origin_).count() * TICK_RATE
                                - static_cast<double>(ticks_);
            slew(pacer->step(now, behind));
            max_catch_up = pacer->rebase_threshold();
        }
        const double elapsed = std::chrono::duration<double>(now - origin_).count();
        const double expected = elapsed * TICK_RATE;
        const bool skipped_before = previous_render_skipped;
        previous_render_skipped = false;
        if (static_cast<double>(ticks_) + max_catch_up < expected) {
            // Far behind (level load, debugger): fast-forwarding would render
            // nothing for seconds, so restart the schedule from here.
            origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(ticks_) * TICK_SECONDS));
            ++rebases_;
        } else if (static_cast<double>(ticks_) + 1.0 < expected) {
            // Behind by more than one tick: skip the next render so the loop
            // reaches Process again as soon as possible.  In a core session
            // (FULL step SRT1, f1) never two in a row: a frame after a
            // skipped one renders, without waiting, so a long catch-up shows
            // every other tick instead of holding one image for its whole
            // length (srt_ctl: 167 frames, 2.5 s).  Presentation only: the
            // ticks and their order are unchanged; each kept render (~0.6 ms
            // on the E-cores) slows the catch-up by that.  A raise of the
            // SessionLead slew gains only while less than a frame is due, so
            // it is not affected beyond that.  Without a pacer (the legacy
            // path) every such frame skips, as before.
            if (pacer && skipped_before) {
                // render this frame; the next one may skip again
            } else {
                bml->SkipRenderForNextTick();
                ++skipped_renders_;
                previous_render_skipped = true;
            }
        } else if (static_cast<double>(ticks_) > expected) {
            // Ahead: wait until this tick's wall-clock slot begins.
            const double due = static_cast<double>(ticks_) * TICK_SECONDS;
            const auto deadline = origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(due));
            if (deadline > now) {
                bmmo::retail::host_clock().sleep_for(clock_site::pace_due, deadline - now);
                ++waited_frames_;
            }
        }
        return tick;
    }
}
