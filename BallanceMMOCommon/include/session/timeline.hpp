#pragma once

// Pure bookkeeping for the physics-session timeline (design section 8.2,
// protocol section 2.1).  No engine or networking dependencies, so the same
// code runs in the server, the headless session client and the unit tests.
//
//   input_buffer   - one per player on the server: inputs indexed by tick,
//                    last-known fallback when a tick is missing.
//   tick_scheduler - decides when the server may simulate the next tick:
//                    every player's input has arrived, or the wall clock is
//                    past start + (tick + input_delay) tick lengths.

#include <chrono>
#include <cstdint>
#include <map>

#include "../entity/session.hpp"

namespace bmmo::session {
    inline constexpr double kTickSeconds = 1.0 / 66.0;
    inline constexpr std::chrono::microseconds kTickDuration{15151};  // 1e6 / 66, truncated

    // Wall-clock time of tick T measured from the session start.
    inline std::chrono::microseconds tick_offset(uint32_t tick) {
        return std::chrono::microseconds(static_cast<int64_t>(tick) * 1000000 / 66);
    }

    // How long a session may make the server wait for a tick's inputs, in
    // ticks, for a link whose worst observed round trip is `ping_ms`.
    //
    // The server simulates tick T at the latest input_delay tick lengths after
    // T's nominal time (tick_scheduler::deadline), and the client runs its own
    // clock from the same anchor, so what has to fit inside that window is one
    // trip from the client, not a round trip.
    //
    // Half again on top, because what decides whether an input is late is the
    // jitter, not the average: a path with a 20 ms median and 200 ms
    // excursions misses far more deadlines than its mean suggests, and a
    // round trip - even the worst one seen - is a smoothed number that does
    // not show those excursions.  Plus one tick, so a link with no measurable
    // latency at all still gets the scheduler's own granularity as slack.
    inline constexpr int kInputDelayJitterPercent = 150;
    inline constexpr int kInputDelayMarginMs = 16;
    // Beyond this the corrections are further back than the rollback history
    // is worth keeping, and the session is not playable anyway.
    inline constexpr uint32_t kInputDelayMaxTicks = 40;

    inline uint32_t input_delay_for_ping(int ping_ms, uint32_t floor_ticks) {
        if (ping_ms < 0) ping_ms = 0;
        const int64_t budget_ms = static_cast<int64_t>(ping_ms) * kInputDelayJitterPercent / (2 * 100)
                                + kInputDelayMarginMs;
        const int64_t ticks = (budget_ms * 66 + 999) / 1000;   // round up to whole ticks
        uint32_t delay = static_cast<uint32_t>(ticks < 0 ? 0 : ticks);
        if (delay < floor_ticks) delay = floor_ticks;
        if (delay > kInputDelayMaxTicks) delay = kInputDelayMaxTicks;
        return delay;
    }

    class input_buffer {
    public:
        // Inputs a client may send ahead of the server (ticks further in the
        // future are dropped as garbage).
        static constexpr uint32_t kMaxLookahead = 660;

        // Stores `count` consecutive frames starting at first_tick; frames for
        // ticks already consumed are ignored.  Returns how many were new.
        template <class Frames>
        int submit(uint32_t first_tick, const Frames& frames) {
            int stored = 0;
            uint32_t tick = first_tick;
            for (const auto& frame: frames) {
                ++received_;
                if (tick >= next_tick_ && tick < next_tick_ + kMaxLookahead)
                    stored += frames_.emplace(tick, frame).second ? 1 : 0;
                else if (tick < next_tick_)
                    ++stale_;   // the tick was simulated before this frame arrived
                ++tick;
            }
            stored_ += static_cast<uint64_t>(stored);
            return stored;
        }

        // Diagnostics: frames offered, kept, and already-simulated on arrival.
        uint64_t received() const { return received_; }
        uint64_t stored() const { return stored_; }
        uint64_t stale() const { return stale_; }

        bool has(uint32_t tick) const { return frames_.count(tick) != 0; }
        const input_frame* peek(uint32_t tick) const {
            auto it = frames_.find(tick);
            return it == frames_.end() ? nullptr : &it->second;
        }

        // The input to apply for `tick`: the client's frame when it arrived,
        // else the last one applied (or a default frame before any arrived).
        // Advances the consumption point; earlier frames are discarded.
        const input_frame& take(uint32_t tick, bool& fresh) {
            auto it = frames_.find(tick);
            fresh = it != frames_.end();
            if (fresh) {
                last_ = it->second;
                last_tick_ = tick;
                any_ = true;
            }
            frames_.erase(frames_.begin(), frames_.upper_bound(tick));
            next_tick_ = tick + 1;
            return last_;
        }

        const input_frame& last() const { return last_; }
        uint32_t last_fresh_tick() const { return last_tick_; }
        bool received_any() const { return any_; }
        size_t pending() const { return frames_.size(); }
        uint32_t next_tick() const { return next_tick_; }

        // Late joiners start at their first tick; nothing before it exists.
        void reset(uint32_t first_tick) {
            frames_.clear();
            next_tick_ = first_tick;
            last_ = {};
            last_tick_ = first_tick;
            any_ = false;
        }

    private:
        std::map<uint32_t, input_frame> frames_;
        input_frame last_{};
        uint32_t last_tick_ = 0;
        uint32_t next_tick_ = 0;
        bool any_ = false;
        uint64_t received_ = 0, stored_ = 0, stale_ = 0;
    };

    class tick_scheduler {
    public:
        using clock = std::chrono::steady_clock;

        void start(clock::time_point now, uint32_t first_tick, uint32_t input_delay) {
            started_ = true;
            start_ = now;
            first_tick_ = first_tick;
            next_tick_ = first_tick;
            input_delay_ = input_delay;
        }
        bool started() const { return started_; }
        uint32_t next_tick() const { return next_tick_; }

        // Wall-clock deadline after which `tick` is simulated even without
        // every player's input.
        clock::time_point deadline(uint32_t tick) const {
            return start_ + tick_offset(tick - first_tick_ + input_delay_);
        }
        bool due(clock::time_point now) const { return started_ && now >= deadline(next_tick_); }
        // How long the runner may sleep before the next tick becomes due.
        clock::duration until_due(clock::time_point now) const {
            const auto d = deadline(next_tick_);
            return d > now ? d - now : clock::duration::zero();
        }
        void advance() { ++next_tick_; }

        // The tick a client anchored `now` at (for late joiners): the tick the
        // server would be simulating at that instant, plus a margin so the
        // joiner's first inputs are not already late.
        uint32_t tick_at(clock::time_point now, uint32_t margin) const {
            if (!started_ || now <= start_) return first_tick_ + margin;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
            return first_tick_ + static_cast<uint32_t>(elapsed * 66 / 1000000) + margin;
        }

    private:
        bool started_ = false;
        clock::time_point start_{};
        uint32_t first_tick_ = 0;
        uint32_t next_tick_ = 0;
        uint32_t input_delay_ = 0;
    };

    // Snapshot cadence: a delta every `interval` ticks, a full one every
    // `full_interval` ticks or when the body set changed.
    struct snapshot_cadence {
        uint32_t interval = 2;
        uint32_t full_interval = 66;
        uint32_t last_full = 0;
        bool have_full = false;

        // Returns 0 = nothing, 1 = delta, 2 = full.
        int decide(uint32_t tick, bool body_set_changed) {
            if (!have_full || body_set_changed || tick - last_full >= full_interval) {
                have_full = true;
                last_full = tick;
                return 2;
            }
            if (interval == 0 || tick % interval == 0) return 1;
            return 0;
        }
        void force_full() { have_full = false; }
    };
}
