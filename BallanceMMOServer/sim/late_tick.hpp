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
// The members present at the start of a session are anchored at the same lead
// (session_start_tick_base below): the lead is measured from tick 0 because the
// world has not ticked when their SessionAssign goes out, and the client
// renumbers itself from it exactly as a late joiner does, so every member of a
// fresh session starts numbered from one bounded, common base instead of from
// its own level-load anchor.  That base is additionally clamped so the member's
// start lag - the base plus the input delay the world adds on top of it - stays
// inside its rollback window.
//
// The lead cap alone is not a lag bound.  The client's lag term uses the REAL
// round trip, not the capped rtt_ticks the lead pays for, so with lead <= 34
// the lag is 34 + 0.5 * ceil(rtt_ms * 66 / 1000) ticks.  That stays inside
// max_resim_ticks = 48 only up to a 424 ms round trip (28 raw ticks); at 425 ms
// it is 48.5, at 1000 ms 67 and at 65535 ms 2197.  Above the cliff the client
// goes too_far -> invalidate_history() -> unmatched snapshots, and the resync
// guard is what keeps it from drifting; the cap is not re-tuned for that, the
// degradation is accepted and only the bound is stated honestly.  A session
// start does not spend that acceptance: its base is clamped against the window
// below, so a fresh session never begins at the cliff even though the late join
// path's lead would.

#include <algorithm>
#include <cstdint>

namespace bmmo::sim {
    constexpr uint32_t kLateTickRttCapTicks = 12;    // ~180 ms round trip
    constexpr uint32_t kLateTickLeadCapTicks = 34;
    // The client's rollback window (rollback.hpp rollback_thresholds:
    // max_resim_ticks = 48) less a margin.  A start member's lag is the base
    // plus the input delay the world adds on top of it, so the base has to fit
    // under 48 minus that delay; the margin keeps the window's last few ticks
    // for the ordinary corrections the member makes on top of the start.
    constexpr uint32_t kStartBaseLagMarginTicks = 4;
    constexpr uint32_t kStartBaseLagBudgetTicks = 48 - kStartBaseLagMarginTicks;   // 44

    inline uint32_t late_tick_lead_ticks(uint32_t input_delay, uint32_t rtt_ms) {
        const uint32_t rtt_ticks = std::min<uint32_t>((rtt_ms * 66 + 999) / 1000, kLateTickRttCapTicks);
        const uint32_t lead = std::max<uint32_t>(1, input_delay) + rtt_ticks + 2;
        return std::min<uint32_t>(lead, kLateTickLeadCapTicks);
    }

    // The tick base the members present at the start of a session are numbered
    // from: the same lead, with the late join's "current tick" term zero,
    // because the world has not ticked yet when their SessionAssign goes out.
    // `worst_rtt_ms` is the worst-member round trip the session's input delay
    // was sized from, so the two budgets come from one measurement.
    //
    // Assigning 0 instead (up to build 9.20) left every start member numbered
    // from its own level-load anchor, which is what the client-side
    // rebase-on-nonzero-first_tick exists to undo: the client's lag over the
    // snapshot it is correcting is its numbering offset plus the input delay
    // plus the one-way trip, and with the base at 0 that offset was the spread
    // of the anchors - the level-load time, seconds for a slow member, which
    // nothing bounded.  Past a max_resim_ticks spread the session's first
    // seconds were too_far -> invalidate_history() -> unmatched snapshots ->
    // resync churn, which is exactly what the caps above are meant to prevent
    // for a late joiner.
    //
    // The lead alone does not bound that lag either: the offset a start member
    // works with is the base PLUS the input delay, and the lead already carries
    // the input delay inside it, so the lead cap of 34 (reached at an input
    // delay of 20 ticks, a ~365 ms worst round trip) puts the member at
    // 34 + delay against rollback.hpp's max_resim_ticks = 48 - past the window
    // before the session has played a tick.  So the base is clamped to
    // 44 - input_delay (max_resim_ticks less a 4-tick margin less the input
    // delay), the largest base whose lag still fits the window, and floored at
    // 1: a base of 0 would silently disable the client's
    // rebase-on-nonzero-first_tick and put that member back on its own anchor
    // numbering.  The clamp spends re-simulation headroom to stay inside the
    // window, and the input-arrival slack survives it - the slack is
    // base + input_delay - raw_rtt_ticks, which at the clamp is
    // 44 - raw_rtt_ticks: at a 400 ms round trip (27 raw ticks, input delay 21)
    // the base is 23 and the slack is still 17 ticks.  Only a link past ~660 ms
    // (44 raw ticks) goes negative there, and the input buffer already covers
    // that case by reusing the last frame it has (see above).
    inline uint32_t session_start_tick_base(uint32_t input_delay, uint32_t worst_rtt_ms) {
        const uint32_t lead = late_tick_lead_ticks(input_delay, worst_rtt_ms);
        const uint32_t room = kStartBaseLagBudgetTicks > input_delay ? kStartBaseLagBudgetTicks - input_delay : 0;
        return std::max<uint32_t>(1, std::min(lead, room));
    }
}
