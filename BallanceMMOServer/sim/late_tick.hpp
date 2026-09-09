#pragma once

// The lead a late joiner (or a resync) is anchored at, in ticks ahead of the
// server's current tick: the session's input delay, plus the round trip the
// reliable SessionAssign spends in flight, plus a margin.  Both latency terms
// are capped, because the client's rollback lag grows with them
// (lag ~= 1.5 * rtt_ticks + (lead - rtt_ticks), against rollback.hpp
// max_resim_ticks): past the client's resim window every snapshot is too_far,
// which wipes the rollback history and asks for a resync, so a member on a slow
// link churns instead of playing.  A link slower than the round-trip cap stamps
// its frames slightly behind the ticks they name; the server's input buffer
// reuses the last frame it has for the gap, so the client falls behind by a
// bounded amount and recovers on its own rather than looping.
//
// The lead cap alone is not a lag bound.  The client's lag term uses the REAL
// round trip, not the capped rtt_ticks the lead pays for, so with lead <= 34
// the lag is 34 + 0.5 * ceil(rtt_ms * 66 / 1000) ticks.  That stays inside
// max_resim_ticks = 48 only up to a 424 ms round trip (28 raw ticks); at 425 ms
// it is 48.5, at 1000 ms 67 and at 65535 ms 2197.  Above the cliff the client
// goes too_far -> invalidate_history() -> unmatched snapshots, and the resync
// guard is what keeps it from drifting; the cap is not re-tuned for that, the
// degradation is accepted and only the bound is stated honestly.

#include <algorithm>
#include <cstdint>

namespace bmmo::sim {
    constexpr uint32_t kLateTickRttCapTicks = 12;    // ~180 ms round trip
    constexpr uint32_t kLateTickLeadCapTicks = 34;

    inline uint32_t late_tick_lead_ticks(uint32_t input_delay, uint32_t rtt_ms) {
        const uint32_t rtt_ticks = std::min<uint32_t>((rtt_ms * 66 + 999) / 1000, kLateTickRttCapTicks);
        const uint32_t lead = std::max<uint32_t>(1, input_delay) + rtt_ticks + 2;
        return std::min<uint32_t>(lead, kLateTickLeadCapTicks);
    }
}
