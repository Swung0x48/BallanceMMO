#pragma once

// Every place the client session flow reads the wall clock
// (docs/session-client-core-design.md 2.2 `now(clock_site)`, 5.2 (5)).
//
// The headless client's inbound capture (BallanceMMOServer/sim/
// inbound_capture.hpp) writes each read with its site, and a replay hands the
// captured value back only to a read at the same site.  So a read that moves
// into the shared core keeps its site: the core passes it to the platform's
// `now`, the headless platform forwards it to the capture, and the retail
// platform ignores it.
//
// The values are part of the capture's file format: append, never renumber.
// CK-free and header-only.

#include <cstddef>
#include <cstdint>

namespace bmmo::session {

    enum class clock_site : uint8_t {
        run_start = 1,        // run(): the --seconds limit's origin
        run_limit,            // run(): the --seconds limit, every pass
        run_report,           // run(): the 5 s status line, every pass
        report_init,          // last_report_'s initial value (taken at construction)
        room_ready,           // handle_room_state: the once-a-second Ready limiter
        assign_resync,        // SessionAssign, re-assignment: the pacer's origin
        assign_renumber,      // SessionAssign, numbered base: the pacer's origin
        assign_release,       // SessionAssign, the anchor hold released: the pacer's origin
        anchor_origin,        // anchor(): the pacer's origin
        anchor_hold,          // anchor(): the hold deadline
        hold_poll,            // pace_and_frame: the hold deadline check
        hold_timeout,         // pace_and_frame: the hold timed out, the pacer's origin
        pace_expected,        // pace_and_frame: the schedule
        pace_rebase,          // pace_and_frame: the schedule rebased
        pace_due,             // pace_and_frame: the wait for the next frame
        slew_step,            // advance_lead_slew
        slew_start,           // on_session_lead: the slew's clock
        hold_event_stamp,     // --hold-event: the release time of a held event
        hold_event_release,   // release_held_events
        resync_limiter,       // request_resync: the 2 s / 10 s limiter
        journal_ms,           // a TICK record's ms
        journal_start,        // core::client_journal::open: the origin of journal_ms
        journal_utc,          // core::client_journal::open: the header's utc_ms (system clock, in ms)
        own_backstop,         // the retail own_body_backstop: the 2 s re-report limiter (R1)
        count_
    };

    inline const char* clock_site_name(clock_site site) {
        static const char* const names[] = {
            "?", "run_start", "run_limit", "run_report", "report_init", "room_ready", "assign_resync",
            "assign_renumber", "assign_release", "anchor_origin", "anchor_hold", "hold_poll", "hold_timeout",
            "pace_expected", "pace_rebase", "pace_due", "slew_step", "slew_start", "hold_event_stamp",
            "hold_event_release", "resync_limiter", "journal_ms", "journal_start", "journal_utc", "own_backstop"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(clock_site::count_),
                      "a name for every clock site");
        const auto i = static_cast<size_t>(site);
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
    }

}
