#pragma once

// The lead a late joiner (or a resync) is anchored at: the session's input
// delay, plus the round trip the reliable SessionAssign spends in flight, plus
// a margin.  Since step RL1 the lead is counted on the session's fixed deadline
// schedule (reanchor_base_tick below, session_runner::reanchor_tick), not over
// the server's current tick; the base it gives is floored at current_tick + 2
// (+ 3 for a member without CAP_RESYNC_STATE).  The clamps below size the lead against
// the deadline budget it buys, lead + input_delay periods.  Both latency terms
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
#include <chrono>
#include <cstdint>

#include <session/lead.hpp>

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

    // Late-player design 2.5: with physics.lead_adjust on, a player's lead
    // is its own, and the caps above stay with the start base only.  A
    // client's reach grows with its measured relay lag instead (plan step
    // 12), so the per-player path is bounded by a sanity cap and nothing
    // else.
    constexpr uint32_t kLeadSanityCapTicks = bmmo::session::kLeadCapTicks;

    // The first lead of a late joiner with lead_adjust on: the input delay,
    // the whole round trip the reliable SessionAssign and the first frame
    // spend in flight, and the margin - uncapped but for the sanity bound,
    // so the joiner's frames arrive on time from its first tick instead of
    // being substituted until the meter raises it.
    inline uint32_t late_join_lead_ticks(uint32_t input_delay, uint32_t rtt_ms) {
        const uint64_t rtt_ticks = (static_cast<uint64_t>(rtt_ms) * 66 + 999) / 1000;
        const uint64_t lead = std::max<uint64_t>(1, input_delay) + rtt_ticks + 2;
        return static_cast<uint32_t>(std::min<uint64_t>(lead, kLeadSanityCapTicks));
    }

    // L_eff (design 2.5, 2.8): the lead of the player's first assignment
    // plus the offset its acknowledged SessionLeads added, which is the lead
    // measured to work, in the frame of reference the offset was measured
    // in.  A resync re-anchors there with lead_adjust on.  At least 1 and at
    // most the sanity cap.
    inline uint32_t effective_lead_ticks(uint32_t assigned_lead, int lead_offset) {
        const int64_t lead = static_cast<int64_t>(assigned_lead) + lead_offset;
        return static_cast<uint32_t>(std::clamp<int64_t>(lead, 1, kLeadSanityCapTicks));
    }

    // Whether a player takes the per-player path above: physics.lead_adjust
    // on AND its client declared CAP_LEAD.  A client that cannot apply a
    // SessionLead also cannot grow its reach with its lead (plan steps 12,
    // 15), so it keeps the capped leads - the 34 cap exists to keep its relay
    // lag inside the fixed reach - and with the flag on it is anchored
    // exactly as with the flag off.
    inline bool uses_player_lead(bool lead_adjust, uint32_t caps) {
        return lead_adjust && (caps & bmmo::session::CAP_LEAD) != 0;
    }

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

    // The lead a resync re-anchors a member at, off the per-player path
    // (late-player plan step 20): a resync is a fresh start for that one
    // member, so it gets the start base's lead, clamped inside the window
    // the same way - not the late joiner's capped lead, which the clamp does
    // not bound.  Measured (lp19b_a6_1500, input delay 17, 150 ms each way):
    // the start members' relay lag was 20 (the round trip) at the start's
    // lead of 27; the owner resynced at the late-tick lead of 31 settled at
    // 27-28 once the session ran normally again.  Its 48-49 right after the
    // resync, at its reach, was that plus the 22 ticks the whole session
    // stalled for while it waited for the deadlines of the ticks the resync
    // skipped - fixed in the step test (input_buffer::reanchored), not
    // here.  This clamp only brings a resynced member's lag back to about
    // where it was before (27 -> about 23 at these numbers).
    inline uint32_t resync_lead_ticks(uint32_t input_delay, uint32_t rtt_ms) {
        return session_start_tick_base(input_delay, rtt_ms);
    }

    // Step RL1: the base of every re-anchor (a resync, a resync at L_eff, a
    // late join) is taken from the session's FIXED deadline schedule, not
    // from the server's current tick.  The deadlines are set once, at the
    // start (tick_scheduler::start; deadline(t) = start + (t - first + D)
    // periods), and the server steps at its members' arrivals capped at the
    // deadline, so the current tick C sits anywhere between D ticks behind
    // the schedule (a server waiting on a member's deadlines - every
    // stall-triggered resync) and a few ticks ahead of it (an arrival-paced
    // server).  A base of C + lead gave the member's frames lead + e(C)
    // ticks before their deadlines (e = deadline(C) - step(C)): about 6
    // instead of the start's 22 after a stall, and the server, waiting on
    // those late frames, relayed every member about 15 ticks later for the
    // rest of the session (found by the user's trial; result1/2.md).
    //
    // `scheduled` is schedule_tick_at(now - start, first, lead) below (from
    // the schedule's start on, tick_scheduler::tick_at(now, lead)): the tick
    // the schedule names for this instant plus `lead`, so deadline(base) -
    // now is lead + D periods (in ((lead + D - 1), lead + D] of them), the
    // start's budget (the start base was handed out start_base periods
    // before its first deadline-less step, design 9.26), whatever the
    // server's pace.  `lead` now means exactly that: the start's budget less
    // the input delay.  The floor keeps the base out of the runner's past:
    // current_tick is the tick it steps next, so a base of current_tick +
    // kReanchorBaseMarginTicks leaves base - 1 (the tick whose state A3
    // answers with) at least one tick ahead of the runner when the network
    // thread read it; a runner that bursts further before the command
    // reaches it is the base-missed path (on_resync_base_missed: re-answered
    // with a new base).  That path exists only for a member that applies
    // the resync state (CAP_RESYNC_STATE, the session core): one without it
    // (the legacy retail path) gets a full snapshot and nothing re-answers
    // a base the runner stepped first, its first frames are counted stale
    // and held.  So its floor keeps at least the margin a lead had before
    // RL1: the smallest lead late_tick_lead_ticks gives (input delay 1 + 2
    // = 3; a pre-RL1 legacy resync usually got more, C + resync_lead_ticks).
    // The floor binds only when the server runs more than lead - margin
    // ticks ahead of its schedule, which needs a path shorter than the
    // margin; it then gives the member more budget than the start, never
    // less.
    constexpr uint32_t kReanchorBaseMarginTicks = 2;         // CAP_RESYNC_STATE
    constexpr uint32_t kReanchorBaseLegacyMarginTicks = 3;   // without it (step RL1b)

    inline uint32_t reanchor_margin_ticks(bool resync_state) {
        return resync_state ? kReanchorBaseMarginTicks : kReanchorBaseLegacyMarginTicks;
    }

    inline uint32_t reanchor_base_tick(uint32_t scheduled, uint32_t current_tick,
                                       uint32_t margin = kReanchorBaseMarginTicks) {
        const uint32_t floor = current_tick + margin;
        return static_cast<int32_t>(scheduled - floor) >= 0 ? scheduled : floor;
    }

    // The tick the fixed deadline schedule names `elapsed_us` microseconds
    // after its start (the start is the first tick's deadline less the input
    // delay, tick_scheduler::start), plus `margin`; floored, and SIGNED (step
    // RL1b): before the start - the first start_base periods after the last
    // SessionReady, design 9.26 - it lies below the first tick, so a
    // re-anchor there gets lead + D periods like any other.
    // tick_scheduler::tick_at clamps every instant before the start to
    // first + margin, which gave such a re-anchor up to start_base periods
    // more (start_base 27: a joiner 100 ms after the all-ready moment got
    // about 20 ticks more than the start members, and its frames landed
    // that much early until a resync).
    // From the start on it equals tick_at.
    inline uint32_t schedule_tick_at(int64_t elapsed_us, uint32_t first_tick, uint32_t margin) {
        const int64_t scaled = elapsed_us * 66;
        int64_t ticks = scaled / 1000000;
        if (scaled % 1000000 < 0) --ticks;   // floor below zero too
        return first_tick + static_cast<uint32_t>(ticks) + margin;
    }

    // session_runner::reanchor_tick's whole rule, for a session whose
    // scheduler started at `schedule_start` from `first_tick`: the base of a
    // re-anchor at `now` with `lead`, the runner's next tick `current_tick`,
    // and the member's floor margin (reanchor_margin_ticks).
    inline uint32_t reanchor_tick_on_schedule(std::chrono::steady_clock::time_point schedule_start, uint32_t first_tick,
                                              std::chrono::steady_clock::time_point now, uint32_t lead,
                                              uint32_t current_tick, uint32_t margin) {
        const int64_t elapsed_us = std::chrono::floor<std::chrono::microseconds>(now - schedule_start).count();
        return reanchor_base_tick(schedule_tick_at(elapsed_us, first_tick, lead), current_tick, margin);
    }

    // Step DX9 (H3's harness item): the parts of that rule, for the server's
    // re-anchor log line - the time from the schedule's start (negative
    // before it: RL1-e's window), the SIGNED schedule tick (first_tick plus
    // the floored periods, without the lead; below first_tick before the
    // start), the floor (current_tick + margin) and the base, which is
    // exactly reanchor_tick_on_schedule's.
    struct reanchor_parts {
        int64_t elapsed_us = 0;
        int64_t schedule_tick = 0;
        uint32_t scheduled = 0;   // schedule_tick + lead (as uint32, schedule_tick_at)
        uint32_t floor = 0;
        uint32_t base = 0;
        bool floored() const { return base != scheduled; }
    };

    inline reanchor_parts reanchor_on_schedule_parts(std::chrono::steady_clock::time_point schedule_start, uint32_t first_tick,
                                                     std::chrono::steady_clock::time_point now, uint32_t lead,
                                                     uint32_t current_tick, uint32_t margin) {
        reanchor_parts p;
        p.elapsed_us = std::chrono::floor<std::chrono::microseconds>(now - schedule_start).count();
        const uint32_t unled = schedule_tick_at(p.elapsed_us, first_tick, 0);
        p.schedule_tick = static_cast<int64_t>(first_tick) + static_cast<int32_t>(unled - first_tick);
        p.scheduled = schedule_tick_at(p.elapsed_us, first_tick, lead);
        p.floor = current_tick + margin;
        p.base = reanchor_base_tick(p.scheduled, current_tick, margin);
        return p;
    }
}
