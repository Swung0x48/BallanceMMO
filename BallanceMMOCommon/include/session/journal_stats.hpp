#pragma once

// Step JNL: the journal's typed records of what the analyses read - the
// data the logs used to be the only home of (input delay, members, resyncs,
// lead, rewinds, the window and status counters, the server's step time and
// the members' telemetry).  Logs are for people; every number a report,
// gate or criterion reads is in the journal (docs/realtime-design.md
// 'Built (the journal carries what the analyses read, step JNL)').
//
// One table: each kind has a name, a class and its fields.  A journal
// describes itself - its SCHEMA record (tag 10, written with the HEADER)
// lists the kinds this writer records, with their ids, classes and field
// names - so a reader keeps no copy of this table, and a kind absent from a
// file's SCHEMA means "not recorded", never 0.
//
//   SCHEMA (tag 10) = u32 first_tick , u16 n ,
//                     n x ( u16 id , str name , u8 class , u8 nfields , nfields x ( str name , u8 type ) )
//   STAT   (tag 11) = u32 tick , u16 kind , the values in the kind's field order
//   CLOCK  (tag 12) = the same payload, for a kind of class 1 or 2
//
// type 'i' = a signed integer as zigzag LEB128, 'f' = f64, 's' = a str of up
// to kJournalMaxText (65 535) bytes - one of exactly that size was cut.
// Class 0 (logical) values follow from the ticks alone: STAT, compared byte
// for byte between two runs.  Class 1 is a clock read at a recorded clock
// site (session/clock_site.hpp: exact under the virtual clock and a capture
// replay), class 2 a raw clock read or a link's status (neither): both are
// CLOCK, which a run-to-run compare skips (scripts/journal_compare.py).
// Nothing here reads a clock: a record carries a value its site already had.

#include <cstddef>
#include <cstdint>

namespace bmmo::session {
    enum class stat_class : uint8_t { logical = 0, clock_site = 1, raw = 2 };

    // The writer a kind belongs to (a SCHEMA lists its own side's kinds).
    constexpr uint8_t STAT_SERVER = 1;
    constexpr uint8_t STAT_CLIENT = 2;

    struct stat_field {
        const char* name;
        char type;   // 'i', 'f' or 's'
    };

    struct stat_kind_info {
        const char* name;
        stat_class klass;
        uint8_t side;
        const stat_field* fields;
        uint8_t field_count;
    };

    // The ids are the table's order and never change meaning: a new kind
    // goes at the end.
    enum class stat_kind : uint16_t {
        // server
        server_config = 0,   // at the open: the switches the criteria read
        ready,               // the members were ready, the first step
        resync_state,        // a resync state emitted (the note's numbers and the request's)
        resync_cost,         // its capture and coding time
        resync_wire,         // what the server put on the wire for it
        lead,                // a SessionLead decided and sent (the plan, never recomputed)
        lateness,            // a raise or lower and what became of it
        s1,                  // an S1 line of the lateness meter, as key=value text
        step,                // the step time over a window of ticks
        link,                // a member's round trip at the start (it sized the input delay)
        telemetry,           // a member's SessionTelemetry as received
        s5,                  // a lifecycle event's intake-to-send delay
        // client
        rewind,              // one rewind of the rollback engine: what was asked (its base and depth: rewind_base)
        window,              // the 60-tick window counters (the window: line)
        status,              // the session's counters at its end (the status: line's logical part)
        status_clock,        // the status line's clock values and the whole line (each headless report's too)
        stall,               // a stall the headless client was told to make (--stall-ms)
        resync_request,      // server: a resync request as the runner took it (the log's "P resynced at tick B")
        ghost,               // server: a ball's spawn ghost made or gone solid (the world's spawn ghost lines)
        own_death,           // client: what became of an own death the server applied late (its line)
        rewind_base,         // client: the base a rewind restored and the ticks it re-simulated (after its rewind)
        s1_quorum,           // server: an S1 quorum summary or arrival line (clock-derived quantiles)
        count
    };

    namespace stat_fields {
        constexpr stat_field server_config[] = {{"lead_adjust", 'i'}, {"patience", 'i'}};
        constexpr stat_field ready[] = {{"players", 'i'}, {"first_tick", 'i'}};
        constexpr stat_field resync_state[] = {
            {"player", 'i'}, {"tick", 'i'}, {"base", 'i'}, {"rows", 'i'}, {"script_bytes", 'i'},
            {"savestate_bytes", 'i'}, {"raw_bytes", 'i'}, {"frame_bytes", 'i'}, {"ref_tick", 'i'},
            {"last_full", 'i'}, {"counted_through", 'i'}, {"error", 's'}};
        constexpr stat_field resync_cost[] = {{"player", 'i'}, {"tick", 'i'}, {"capture_us", 'i'}, {"code_us", 'i'}};
        constexpr stat_field resync_wire[] = {{"player", 'i'}, {"tick", 'i'}, {"frame_bytes", 'i'}, {"wire_bytes", 'i'},
                                              {"sent", 'i'}};
        constexpr stat_field lead[] = {
            {"player", 'i'}, {"id", 'i'}, {"k", 'i'}, {"decided", 'i'}, {"cap", 's'}, {"binding", 's'}, {"at", 'i'},
            {"held_from", 'i'}, {"effective", 'i'}, {"dropped", 'i'}, {"epoch", 'i'}, {"sector_bound", 'i'}};
        constexpr stat_field lateness[] = {{"player", 'i'}, {"tick", 'i'}, {"raise", 'i'}, {"k", 'i'}, {"rule", 's'},
                                           {"streak", 'i'}, {"offset", 'i'}, {"route", 's'}};
        constexpr stat_field s1[] = {{"kind", 's'}, {"text", 's'}};
        constexpr stat_field s1_quorum[] = {{"kind", 's'}, {"text", 's'}};
        constexpr stat_field step[] = {{"first", 'i'}, {"last", 'i'}, {"p50_us", 'i'}, {"p99_us", 'i'}, {"max_us", 'i'}};
        constexpr stat_field link[] = {{"player", 'i'}, {"ping_ms", 'i'}, {"worst_ms", 'i'}, {"input_delay", 'i'}};
        constexpr stat_field telemetry[] = {
            {"player", 'i'}, {"host", 'i'}, {"tick", 'i'}, {"ticks", 'i'}, {"frames", 'i'}, {"interval_p50", 'i'},
            {"interval_p99", 'i'}, {"interval_p999", 'i'}, {"interval_max", 'i'}, {"over_33", 'i'}, {"over_50", 'i'},
            {"over_100", 'i'}, {"work_p99", 'i'}, {"work_max", 'i'}, {"rewinds", 'i'}, {"replayed", 'i'},
            {"resyncs", 'i'}, {"holds", 'i'}, {"starved", 'i'}, {"behind_max", 'i'}, {"inbound_p50", 'i'},
            {"inbound_p99", 'i'}, {"inbound_max", 'i'}, {"ping_ms", 'i'}};
        constexpr stat_field s5[] = {{"player", 'i'}, {"type", 's'}, {"origin", 'i'}, {"applied", 'i'},
                                     {"delay_us", 'i'}, {"to", 'i'}, {"flag", 's'}};
        constexpr stat_field rewind[] = {{"anchor", 'i'}, {"at", 'i'}, {"cause", 'i'}, {"body", 's'}};
        constexpr stat_field rewind_base[] = {{"at", 'i'}, {"base", 'i'}, {"depth", 'i'}};
        constexpr stat_field window[] = {
            {"tick", 'i'}, {"inrew", 'i'}, {"inrew_depth", 'i'}, {"life", 'i'}, {"life_depth", 'i'},
            {"forced", 'i'}, {"forced_depth", 'i'}, {"snaprb", 'i'}, {"snaprb_depth", 'i'}, {"snaps", 'i'},
            {"resyncs_sent", 'i'}, {"resyncs_done", 'i'}, {"rebases", 'i'}, {"relay", 'i'}, {"late", 'i'},
            {"covered", 'i'}, {"subst_recv", 'i'}, {"subst_diff", 'i'}, {"subst_same", 'i'},
            {"subst_bodiless", 'i'}, {"subst_late", 'i'}, {"substrew", 'i'}, {"substrew_depth", 'i'},
            {"subst_deferred", 'i'}, {"subst_unseen", 'i'}, {"subst_subsumed", 'i'}, {"subst_saved", 'i'},
            {"lead_adj", 'i'}, {"lead", 'i'}, {"slew", 'i'}, {"moved", 'i'}, {"echo_acked", 'i'},
            {"echo_moved", 'i'}, {"echo_superseded", 'i'}, {"echo_rejected", 'i'}, {"echo_missing", 'i'},
            {"own_moved", 'i'}, {"own_removed", 'i'}, {"own_resynced", 'i'}, {"superseded", 'i'},
            {"replaced", 'i'}, {"lag", 's'}, {"inrew_src", 's'}, {"sector_margin", 's'}};
        constexpr stat_field status[] = {
            {"tick", 'i'}, {"frames", 'i'}, {"mism", 'i'}, {"rb", 'i'}, {"unmatched", 'i'},
            {"hard", 'i'}, {"mech_hard", 'i'}, {"max_err", 'f'}, {"mech_max_err", 'f'}, {"starved", 'i'},
            {"worst", 'i'}, {"rebases", 'i'}, {"resyncs_sent", 'i'}, {"resyncs_done", 'i'},
            {"snapshots_received", 'i'}, {"snapshots_applied", 'i'}, {"snapshots_stale", 'i'},
            {"relay_new", 'i'}, {"relay_dup", 'i'}, {"relay_parked", 'i'}, {"relay_late", 'i'},
            {"relay_covered", 'i'}, {"lag_max", 'i'}, {"lag_p99", 'i'}, {"mechanisms_known", 'i'},
            {"mechanisms_resolved", 'i'}, {"subst_recv", 'i'}, {"subst_diff", 'i'}, {"subst_same", 'i'},
            {"subst_bodiless", 'i'}, {"subst_rew", 'i'}, {"subst_late", 'i'}, {"lead_adj", 'i'},
            {"echo_acked", 'i'}, {"echo_moved", 'i'}, {"echo_superseded", 'i'}, {"echo_rejected", 'i'},
            {"echo_missing", 'i'}, {"own_moved", 'i'}, {"own_removed", 'i'}, {"own_resynced", 'i'},
            {"own_remakes", 'i'}, {"own_other", 'i'}, {"scr_restores", 'i'}, {"scr_failures", 'i'},
            {"scr_mismatches", 'i'}, {"scr_float", 'i'}, {"scr_reset_skips", 'i'}, {"holds", 'i'},
            {"input_rewinds", 'i'}, {"life_rewinds", 'i'}, {"forced_rollbacks", 'i'}, {"sdiff_on", 'i'},
            {"sdiff_queued", 'i'}, {"sdiff_classified", 'i'}, {"sdiff_forced", 'i'}, {"sdiff_unclassified", 'i'},
            {"sdiff_max", 'i'}, {"sdiff_rewound", 'i'}};
        constexpr stat_field status_clock[] = {{"resim", 'i'}, {"t_rest", 'f'}, {"t_rep", 'f'}, {"t_max", 'f'}, {"t_cap", 'f'},
                                               {"lead", 'f'}, {"hold_max", 'f'}, {"text", 's'}};
        constexpr stat_field stall[] = {{"ms", 'i'}, {"tick", 'i'}};
        // `why`: "resync" (the member's SessionResync), "start" (the start
        // handshake's), "again" (a base the runner stepped first, re-answered)
        // or "join" (a late joiner's state); `state`: 1 when the server's state
        // follows to the member alone, 0 for the legacy full snapshot.
        constexpr stat_field resync_request[] = {{"player", 'i'}, {"base", 'i'}, {"last_full", 'i'},
                                                 {"counted_through", 'i'}, {"why", 's'}, {"state", 'i'}};
        constexpr stat_field ghost[] = {{"player", 'i'}, {"tick", 'i'}, {"solid", 'i'}};
        constexpr stat_field own_death[] = {{"origin", 'i'}, {"applied", 'i'}, {"at", 'i'}, {"line", 's'}};
    }

#define BMMO_STAT_KIND(name, klass, side) \
    {#name, stat_class::klass, side, stat_fields::name, static_cast<uint8_t>(sizeof(stat_fields::name) / sizeof(stat_field))}
    constexpr stat_kind_info kStatKinds[] = {
        BMMO_STAT_KIND(server_config, logical, STAT_SERVER),
        BMMO_STAT_KIND(ready, logical, STAT_SERVER),
        BMMO_STAT_KIND(resync_state, logical, STAT_SERVER),
        BMMO_STAT_KIND(resync_cost, raw, STAT_SERVER),
        BMMO_STAT_KIND(resync_wire, raw, STAT_SERVER),
        BMMO_STAT_KIND(lead, logical, STAT_SERVER),
        BMMO_STAT_KIND(lateness, logical, STAT_SERVER),
        // The meter's decisions, resets, lead acks, patience and clock-skew
        // lines: their values follow the runner's clock sites (a capture
        // replay gives them again); the quorum summary is s1_quorum.
        BMMO_STAT_KIND(s1, clock_site, STAT_SERVER),
        BMMO_STAT_KIND(step, raw, STAT_SERVER),
        BMMO_STAT_KIND(link, raw, STAT_SERVER),
        BMMO_STAT_KIND(telemetry, raw, STAT_SERVER),
        // Written on the runner's thread as the event is relayed
        // (on_event_accepted): its delay is a clock site's.
        BMMO_STAT_KIND(s5, clock_site, STAT_SERVER),
        // What a rewind was asked for and every counter of the window and the
        // status are logical; the base a rewind restored and so its depth and
        // the status's resim= follow a raw wall-clock gate (step LVR: a heavy
        // frame - steady_clock, no clock site - records its live tick light,
        // and a later rewind rebases to the whole record before it): those
        // are rewind_base and status_clock, which a run-to-run compare skips.
        BMMO_STAT_KIND(rewind, logical, STAT_CLIENT),
        BMMO_STAT_KIND(window, logical, STAT_CLIENT),
        BMMO_STAT_KIND(status, logical, STAT_CLIENT),
        // Also at each headless 5 s report, whose instant is the wall clock's.
        BMMO_STAT_KIND(status_clock, raw, STAT_CLIENT),
        BMMO_STAT_KIND(stall, logical, STAT_CLIENT),
        // At the tick the runner takes the command, which thread timing sets: raw.
        BMMO_STAT_KIND(resync_request, raw, STAT_SERVER),
        BMMO_STAT_KIND(ghost, logical, STAT_SERVER),
        BMMO_STAT_KIND(own_death, logical, STAT_CLIENT),
        BMMO_STAT_KIND(rewind_base, raw, STAT_CLIENT),
        BMMO_STAT_KIND(s1_quorum, raw, STAT_SERVER),
    };
#undef BMMO_STAT_KIND
    static_assert(sizeof(kStatKinds) / sizeof(kStatKinds[0]) == static_cast<size_t>(stat_kind::count),
                  "one table row per stat_kind, in its order");

    inline const stat_kind_info& stat_info(stat_kind kind) { return kStatKinds[static_cast<size_t>(kind)]; }

    // rewind `cause` (the record's code; the log line's reason text is built
    // only while a host's log is open, step DCL).
    enum class rewind_cause : uint8_t {
        input = 0,          // a remote's relayed frame corrected (body: the first corrected ball)
        own_input = 1,      // our own frame the server substituted
        calibration = 2,
        forced = 3,         // the headless --force-rollback
        sector = 4,
        spawn_forget = 5,
        lifecycle = 6,      // a late lifecycle event (one, or a batch)
        own_move = 7,       // our own event the server applied elsewhere
        resync = 8,         // an exact resync's re-step from the server's state
    };
}
