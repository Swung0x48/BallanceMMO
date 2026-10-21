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

    // How far ahead a member stamps the Sector event of a sector it enters
    // (session/sector_union.hpp), in ticks, for a session whose input delay is
    // `input_delay` and whose worst member round trip is `worst_rtt_ms`.
    //
    // An input only has to reach the server, which simulates a tick
    // input_delay tick lengths after its nominal time; a Sector event has to
    // reach every OTHER member before they simulate the stamped tick, and they
    // run their own clocks from the same anchor.  The hop that binds is client
    // -> server -> client: the input delay covers the first leg (it is the
    // one-way budget for the worst member, jitter margin included), a second
    // one-way budget sized from the same round trip covers the relay leg, and
    // one tick on top because a client can only run the manager before its
    // NEXT step.  A lead that is too short puts that sector's mechanisms out
    // of phase on the members the relay reached late, for the rest of the
    // session, which a rollback cannot repair (it cannot replay a sector's
    // setup); a lead that is too long only starts the sector's mechanisms a
    // little later after the crossing.  So, unlike the late-join lead
    // (sim/late_tick.hpp), the round-trip term is not capped short: both
    // terms carry input_delay_for_ping's own cap, which keeps the sum in a
    // byte.
    inline uint32_t sector_lead_for_ping(uint32_t input_delay, int worst_rtt_ms) {
        return input_delay + 1 + input_delay_for_ping(worst_rtt_ms, 0);
    }

    class input_buffer {
    public:
        // Inputs a client may send ahead of the server (ticks further in the
        // future are dropped as garbage).
        static constexpr uint32_t kMaxLookahead = 660;

        // Stores `count` consecutive frames starting at first_tick; frames
        // behind the consumption point are counted stale and are never
        // applied at their own tick.  Returns how many were new.
        //
        // A stale frame newer than the current substitute becomes the
        // substitute (late-player design 1.2): under persistent lateness
        // every frame arrives stale, and a substitute only a fresh take
        // updated would drive the ball for seconds with the last frame from
        // before the shift - every correction the late client gets would then
        // be the whole lateness instead of the delay.
        template <class Frames>
        int submit(uint32_t first_tick, const Frames& frames) {
            int stored = 0;
            uint32_t tick = first_tick;
            for (const auto& frame: frames) {
                ++received_;
                if (tick >= next_tick_ && tick < next_tick_ + kMaxLookahead) {
                    stored += frames_.emplace(tick, frame).second ? 1 : 0;
                } else if (tick < next_tick_) {
                    ++stale_;   // simulated, or re-anchored past, before this frame arrived
                    // Only for a tick the world has reached: one between the
                    // world's tick and a re-anchor's base (reset) precedes the
                    // client's numbering and is no substitute for anything.
                    if (have_taken_ && tick <= taken_ && (!have_sub_ || tick > sub_tick_)) {
                        last_ = frame;
                        sub_tick_ = tick;
                        have_sub_ = true;
                    }
                }
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
        // Nothing more will come for `tick` that the step could use: its frame
        // is here, or the buffer was re-anchored past it (a late joiner's or a
        // resync's base, later a lead reset's range), so waiting for it would
        // only put the session on deadlines (late-player design 2.3).  The
        // patience step test reads this instead of has().
        bool settled(uint32_t tick) const { return has(tick) || tick < next_tick_; }
        const input_frame* peek(uint32_t tick) const {
            auto it = frames_.find(tick);
            return it == frames_.end() ? nullptr : &it->second;
        }

        // The input to apply for `tick`: the client's frame when it arrived,
        // else the substitute - the newest frame that has arrived for a tick
        // at or below the cursor, fresh or stale (or a default frame before
        // any arrived).  `source` is the tick the returned frame was sent
        // for, INPUT_SOURCE_DEFAULT for the default frame; the server
        // relays it back to the owner with every substituted tick
        // (late-player design 1.2), so the owner's client can re-run its
        // ball with the bytes it sent for `source`.
        //
        // Consumption only ever moves forward, one call at a time: frames up
        // to and including `tick` are discarded and the cursor moves to the
        // first tick after it.  A missing frame is not waited for and does not
        // hold the cursor back - the world has already simulated those ticks
        // with the reused frame - so a frame that arrives after its tick was
        // consumed is counted stale by submit() and never applied at its own
        // tick (only, when it is the newest arrival, as a later tick's
        // substitute).  A
        // cursor held ahead by reset() stays ahead: take() cannot pull it back
        // to a tick the buffer was re-anchored past.
        const input_frame& take(uint32_t tick, bool& fresh, uint32_t& source) {
            auto it = frames_.find(tick);
            fresh = it != frames_.end();
            if (fresh) {
                last_ = it->second;
                last_tick_ = tick;
                sub_tick_ = tick;
                have_sub_ = true;
                any_ = true;
            }
            frames_.erase(frames_.begin(), frames_.upper_bound(tick));
            if (tick >= next_tick_)
                next_tick_ = tick + 1;
            if (!have_taken_ || tick > taken_) taken_ = tick;
            have_taken_ = true;
            source = have_sub_ ? sub_tick_ : INPUT_SOURCE_DEFAULT;
            return last_;
        }
        const input_frame& take(uint32_t tick, bool& fresh) {
            uint32_t source = 0;
            return take(tick, fresh, source);
        }

        // The substitute (the frame a missing tick gets) and its tick.
        const input_frame& last() const { return last_; }
        uint32_t substitute_tick() const { return have_sub_ ? sub_tick_ : INPUT_SOURCE_DEFAULT; }
        uint32_t last_fresh_tick() const { return last_tick_; }
        bool received_any() const { return any_; }
        size_t pending() const { return frames_.size(); }
        uint32_t next_tick() const { return next_tick_; }

        // Re-anchors the buffer at `first_tick`: the first tick the caller
        // expects to consume next (a late joiner's, or a resync's, assigned
        // base).  The cursor never moves backwards - a base behind the
        // consumption point names ticks the world has already simulated, so it
        // is ignored - and frames below the new anchor are dropped and counted
        // stale instead of being kept to confuse the cursor.  Frames at or
        // after the anchor are kept: they are the ones the caller is waiting
        // for, so a re-anchor cannot lose input that already arrived.  The last
        // applied frame stays the fallback for the ticks in between, exactly as
        // it is for a frame lost in flight.
        void reset(uint32_t first_tick) {
            if (first_tick < next_tick_) first_tick = next_tick_;
            const auto keep = frames_.lower_bound(first_tick);
            for (auto it = frames_.begin(); it != keep; ++it) ++stale_;
            frames_.erase(frames_.begin(), keep);
            next_tick_ = first_tick;
        }

    private:
        std::map<uint32_t, input_frame> frames_;
        input_frame last_{};          // the substitute
        uint32_t last_tick_ = 0;      // the last tick taken fresh
        uint32_t sub_tick_ = 0;       // the substitute's tick, when have_sub_
        uint32_t next_tick_ = 0;
        uint32_t taken_ = 0;          // the newest tick take() was called for, when have_taken_
        bool have_taken_ = false;
        bool have_sub_ = false;       // a frame has arrived (fresh or stale)
        bool any_ = false;            // a tick was taken fresh
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
        // The first tick deadline() is defined for: a tick below it would wrap.
        uint32_t first_tick() const { return first_tick_; }

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
