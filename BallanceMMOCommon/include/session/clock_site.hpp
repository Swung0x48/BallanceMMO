#pragma once

// Every place a session flow reads the wall clock - the client's and, after
// them, the server's (step K1) -
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
        retired_own_backstop, // RETIRED at FULL step R6V (read by nobody): the retail own_body_backstop's 2 s
                              // re-report limiter (R1), deleted with the policy; the value stays reserved so the
                              // later sites keep their numbers (the capture format: never renumber)
        resync_hold,          // poll_resync_hold: the hold engaged, its deadline (step A3)
        resync_release,       // poll_resync_hold: the hold released, its length
        resync_hold_timeout,  // poll_resync_hold: the deadline check
        rewind_start,         // rollback rewind: the start of the restore (step R12, the cap's rewind cost)
        rewind_replay,        // rollback rewind: the start of the replay (step R12)
        rewind_end,           // rollback rewind: the end of a rewind that replayed (step R12)
        handshake_stamp,      // a SessionAssign / SessionStartAt copy with a server instant: the ack's stamp (FULL step SAXH)
        handshake_release,    // an unscheduled assignment of protocol 24 started the numbering: its local instant (SAXH)
        handshake_lateness,   // a resync's assignment with a start instant: how late it is applied (SAXH)
        // The server's sites (docs/canned-tests-plan.md, step K1): one enum for every
        // process, so a capture of any of them is one format.
        server_loop_pass,            // server::run: a pass of the network loop (its sleep's origin)
        server_boot_requested,       // create_session: the world boot was asked for (the boot log's origin)
        server_hold_member,          // send_member (--hold-member-ms): the held SessionMember's release time
        server_boot_log,             // on_boot_phase / on_world_ready: the boot log's elapsed time
        server_start_deadline,       // on_world_ready: the start barrier's deadline
        server_start_deadline_join,  // a late SessionReady: the start barrier's deadline, again
        server_rtt_post,             // post_player_rtts: the once-a-second limiter
        server_lead_repeat,          // on_session_lead: the first repeat's time
        server_assign_sent,          // send_session_assign: the assignment's server instant (server_us)
        server_assign_copy,          // send_session_assign: the copies' schedule origin
        server_handshake_begin,      // the start handshake began
        server_start_at_sent,        // send_start_at: the SessionStartAt's server instant (server_us)
        server_handshake_check,      // check_start_handshakes
        server_input_arrival,        // an input datagram's arrival (before its age is taken off)
        server_assign_repeats,       // send_assign_repeats
        server_start_barrier,        // check_start_barriers
        server_event_arrival,        // a lifecycle event's arrival (before its age is taken off)
        server_event_rate,           // a lifecycle event's rate window
        server_event_relay_delay,    // an event relayed: its delay since the intake (the log's)
        server_time_broadcast,       // the real-world timestamp broadcast every 768 passes (system clock, in us)
        runner_journal_utc,          // the server journal's file name and header utc_ms (system clock, in ms)
        runner_handshake_elapsed,    // the start handshake's elapsed time
        runner_reanchor,             // a re-anchor on the schedule: its nominal now
        runner_schedule_origin,      // a session's schedule origin (unscheduled start)
        runner_journal_start,        // the origin of the server journal's TICK ms
        runner_leaver_event,         // a leaver's death event queued
        runner_leaver_intake,        // a leaver's Unphysicalize relay: its intake
        runner_leaver_trafo,         // a leaver's trafo reset queued
        runner_baseline_settled,     // a lead baseline settled
        runner_start_ahead,          // a scheduled start: how far ahead it is (the log's)
        runner_input_seen,           // submit_input: when the runner thread took the frames
        runner_event_arrival,        // submit_event: the event's arrival on the runner
        runner_cutoff,               // a tick's take: the input cutoff check
        runner_journal_ms,           // a server TICK record's ms
        runner_deadline_trafo,       // a deadline trafo reset queued
        runner_pass,                 // run: a pass of the runner's loop
        runner_skew_start,           // the test clock skew begins
        runner_step_observed,        // a step's lateness sample
        runner_until_due,            // run: the wait until the next tick is due
        count_
    };

    inline const char* clock_site_name(clock_site site) {
        static const char* const names[] = {
            "?", "run_start", "run_limit", "run_report", "report_init", "room_ready", "assign_resync",
            "assign_renumber", "assign_release", "anchor_origin", "anchor_hold", "hold_poll", "hold_timeout",
            "pace_expected", "pace_rebase", "pace_due", "slew_step", "slew_start", "hold_event_stamp",
            "hold_event_release", "resync_limiter", "journal_ms", "journal_start", "journal_utc", "own_backstop",
            "resync_hold", "resync_release", "resync_hold_timeout", "rewind_start", "rewind_replay", "rewind_end",
            "handshake_stamp", "handshake_release", "handshake_lateness",
            "server_loop_pass", "server_boot_requested", "server_hold_member", "server_boot_log", "server_start_deadline", "server_start_deadline_join", "server_rtt_post", "server_lead_repeat", "server_assign_sent", "server_assign_copy", "server_handshake_begin", "server_start_at_sent", "server_handshake_check", "server_input_arrival", "server_assign_repeats", "server_start_barrier", "server_event_arrival", "server_event_rate", "server_event_relay_delay", "server_time_broadcast", "runner_journal_utc", "runner_handshake_elapsed", "runner_reanchor", "runner_schedule_origin", "runner_journal_start", "runner_leaver_event", "runner_leaver_intake", "runner_leaver_trafo", "runner_baseline_settled", "runner_start_ahead", "runner_input_seen", "runner_event_arrival", "runner_cutoff", "runner_journal_ms", "runner_deadline_trafo", "runner_pass", "runner_skew_start", "runner_step_observed", "runner_until_due"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(clock_site::count_),
                      "a name for every clock site");
        const auto i = static_cast<size_t>(site);
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
    }

}
