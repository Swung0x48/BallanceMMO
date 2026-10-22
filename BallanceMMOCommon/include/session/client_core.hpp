#pragma once

// The shared session-client core (docs/session-client-core-design.md): one
// implementation of the client session flow, hosted by the headless client
// (BallanceMMOServer/sim/session_client.cpp) and, from step R1 on, by the
// retail mod (BallanceMMOClient/session/core_host.cpp).
//
// Step E1 (design 6.1) is the skeleton: the state the core owns (design 1.2),
// moved verbatim with its initializers out of the headless client, which
// reaches it as `core_.s.<member>` while its functions move here area by area
// (E2-E10); the platform interface (session/client_platform.hpp) with the
// three operations the headless platform starts with; and `legacy_calls`, the
// back-calls into the host that moved code still makes.  E11 makes the state
// private and deletes `legacy_calls`.
//
// Step E2 moves the journal: the core owns one core::client_journal
// (session/client_journal.hpp), the headless client's helpers, reached as
// journal().
//
// Step E3 moves numbering, the hold and the resync request (design 6.1 row
// E3): begin, anchor's SessionReady half, poll_hold, SessionAssign,
// request_resync / retry_owed_resync, flush_inputs, remember_sent,
// send_event, put_event_on_wire and SessionEnd, in client_core.cpp.
//
// Step E4 moves the snapshot path (design 6.1 row E4): SessionSnapshot's
// apply_snapshot - the rollback path, the resync rebuild and the legacy
// ladder - in client_core_messages.cpp, over the platform's write_body,
// own_driven_entity, bind_world and keep_live_cells.
//
// Step E5 moves the relay and late-player parts one and two (design 6.1 row
// E5): SessionRemoteInput's intake and note_input_correction
// (client_core_messages.cpp); the held section, the own correction and
// SessionLead with its forced own frames (client_core_held.cpp); and the
// replays they ask for, replay_corrected_inputs and force_rewind
// (client_core_frame.cpp).
//
// Step E6 moves the remote lifecycle (design 6.1 row E6): SessionEvent's
// intake, the drain at the due tick and the held body changes, apply_event,
// the births made inside a replay (pending births) and the core half of the
// mirrors' birth and death, in client_core_lifecycle.cpp, over the platform's
// mirror_entity, make_mirror, unmake_mirror, detach_mirror_navigation,
// attach_mirror_navigation and mute_event_log.
//
// CK-free: compiled into the headless client, the retail mod and
// BallanceMMOMessageTests (cmake/SessionCore.cmake).

#include "client_journal.hpp"
#include "client_platform.hpp"
#include "correction.hpp"
#include "input_relay.hpp"
#include "journal.hpp"
#include "lead_slew.hpp"
#include "lifecycle.hpp"
#include "mechanism_tracking.hpp"
#include "own_correction.hpp"
#include "rollback.hpp"
#include "sector_union.hpp"
#include "../entity/session.hpp"
#include "../message/session_assign_msg.hpp"
#include "../message/session_end_msg.hpp"
#include "../message/session_event_msg.hpp"
#include "../message/session_input_msg.hpp"
#include "../message/session_lead_msg.hpp"
#include "../message/session_remote_input_msg.hpp"
#include "../message/session_start_msg.hpp"
#include "../physics/world_hash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::session {

    // How long the world waits at the anchor for SessionAssign before it starts
    // stepping anyway (design 9.25 phase alignment).  The assignment is one
    // reliable round trip behind SessionReady, so this is a fault timeout, not
    // a schedule: a server that never answers must not freeze the client.
    constexpr std::chrono::seconds kAnchorHoldTimeout{10};

    // Design 1.2: everything the session flow decides from, as the headless
    // client declared it (session_client.cpp at 9d9cb08), in the same order
    // and with the same initializers and comments.  The nested types moved
    // with it (remote_ball, own_life, pending_birth, forced_own,
    // applied_death, window_base); the host names them through aliases.
    // The dead members of design 1.2 (spawn_position_, spawn_known_,
    // anchor_hash_, snapshot_interval_) stay in the host until E11 deletes
    // them.
    struct client_core_state {
        using clock_type = std::chrono::steady_clock;

        enum class phase { idle, loading, running };

        struct remote_ball {
            std::string entity;
            uint8_t ball_type = 0;
            bool physicalized = false;
            bool navigation = false;        // driven by the shared navigation (design 9.1)
            bool have_input = false;
            uint32_t input_tick = 0;
            bmmo::session::input_frame input{};
            bmmo::session::input_frame applied{};
            // The first tick this world stepped the current body: the stamp of
            // a birth made inside a replay, the live tick after the frame that
            // made it otherwise.  Its record is the body's birth record, which
            // every rewind across it re-poses the body to.
            uint32_t born_tick = 0;
            // The first tick a replay drives it from (rollback_world::
            // replay_remotes): born_tick, or the tick its navigation was
            // attached when that came later.
            uint32_t driven_from = 0;
            std::map<uint32_t, bmmo::session::input_frame> inputs;   // relayed frames by tick
            bmmo::session::body_corrector corrector;
            uint64_t blends = 0, hard_sets = 0;
        };

        // The own ball's body changes the history has to keep (late-player
        // design 3.3): a birth this client made - a spawn, a respawn or a
        // trafo, the retail scripts' Physicalize that report_physicalize sends
        // - and the --beam pair, rebuilt here at its stamp (beam_own_ball).
        // Each carries what its Physicalize event carried, so the body can be
        // made again exactly as the server makes it, and what the server did
        // with it (its echo, protocol 9).  Kept while a rewind can be anchored
        // before it (retire_own_lives): every replay that crosses a change
        // makes it again (apply_own_lives).
        struct own_life {
            enum class kind : uint8_t { birth, beam };
            enum class verdict : uint8_t { sent, acked, moved, rejected, superseded, missing };
            // Why a birth sits in own_unanswered_: its echo was given up as
            // missing (expire_own_echoes), or it was still unanswered when a
            // resync renumbered the history it lived in.
            enum class aside : uint8_t { none, missing, resync };
            kind what = kind::birth;
            verdict state = verdict::sent;
            aside why = aside::none;
            uint32_t aside_at = 0;      // the local tick it was set aside at
            uint32_t origin = 0;        // T: the tick this client made the body at (a beam: its stamp)
            uint32_t applied = 0;       // where the server made it: T until a MOVED echo says T'
            bool made = true;           // false: the server never made it (SUPERSEDED, REJECTED)
            std::string entity;
            uint32_t generation = 0;    // the ball's lifetime it belongs to
            uint8_t ball_type = 0;
            float position[3] = {};     // the pose its Physicalize carried (after the birth cell offset)
            float rotation[9] = {};
            bmmo::session::ball_recipe recipe;
            std::string group;
            bool spawn = false;         // kicked with the spawn impulse of `applied`
            bool origin_live = false;   // the live run has made the origin's change (a birth: the scripts did)
            bool applied_live = false;  // ...and the body at `applied`
            bool retired = false;       // its lifetime ended before a replay could make it again
            // The missing-echo deadline and its uplink gate (expire_own_echoes).
            bmmo::session::own_echo_deadline echo_deadline;
            bool moved() const { return made && applied != origin; }
        };

        struct pending_birth {
            uint32_t player = 0, tick = 0, origin = 0;   // tick: applied (protocol 9)
            uint8_t ball_type = 0, flags = 0;
            float position[3] = {};
            float rotation[9] = {};
            bmmo::session::ball_recipe recipe;
            static pending_birth of(const bmmo::session_event_msg& event) {
                pending_birth b;
                b.player = event.player; b.tick = event.tick; b.origin = event.origin_tick;
                b.ball_type = event.ball_type; b.flags = event.flags;
                std::copy(std::begin(event.position), std::end(event.position), b.position);
                std::copy(std::begin(event.rotation), std::end(event.rotation), b.rotation);
                b.recipe = event.recipe;
                return b;
            }
        };

        uint32_t own_id_ = 0;
        phase phase_ = phase::idle;
        uint32_t session_ = 0;
        uint8_t input_delay_ = 6;
        uint64_t life_rewinds_ = 0, life_rewinds_missed_ = 0;
        // Births stamped in the past, waiting to be made inside a replay at
        // their tick (apply_event / drain_pending_births).
        std::vector<pending_birth> pending_births_;
        uint64_t input_rewinds_ = 0, input_rewinds_missed_ = 0;
        uint64_t forced_rollbacks_ = 0, forced_rollbacks_missed_ = 0;
        bool start_sector_pending_ = false;  // a --start-sector waiting for the first assigned frame
        bmmo::session::sector_union sectors_;   // the sectors this session runs, the rule every side runs
        uint8_t sector_lead_ = 0;            // SessionStart::sector_lead
        std::set<int> sectors_requested_;    // every sector THIS client reported (1 from the anchor)
        int sector_cell_ = 0;                // IngameParameter[0][1] as last seen or written here
        bool input_rewind_pending_ = false;
        uint32_t input_rewind_tick_ = 0;
        std::string input_rewind_entity_;
        bool input_rewind_own_ = false;       // the earliest correction is our own substituted frame
        bool input_rewind_has_own_ = false;   // an own correction is among this frame's, earliest or not
        uint32_t input_rewind_own_amends_ = 0;   // own amendments the pending replay carries (subst_saved_)
        std::set<std::string> input_rewind_entities_;   // every remote ball corrected this frame (window: inrew[P])
        // Lifecycle events waiting for the tick they are due at (drain_due_events).
        std::vector<bmmo::session_event_msg> pending_events_;
        int32_t seed_ = 1;
        int level_ = 1;
        std::vector<bmmo::session::player_entry> players_;
        int own_join_order_ = -1;
        float spawn_impulse_ = 0.0f;             // SessionStart::spawn_impulse (design 9.10)
        float spawn_spacing_ = 0.0f;             // SessionStart::spawn_spacing (session/spawn_impulse.hpp)
        std::string own_body_entity_;            // the own ball entity that had a body after the last frame
        std::string own_group_;
        bool own_physicalized_ = false;
        bool assigned_ = false;
        uint32_t last_assign_first_tick_ = 0;  // the assignment applied last: its redundant copies are ignored
        uint64_t duplicate_assigns_ = 0;
        // Design 9.25 phase alignment: between the anchor and the assignment
        // the world does not step.  The server's world takes its own first step
        // under the start base it hands out, so a client that ran k frames in
        // that window would be k steps further from the anchor at every tick
        // number for the rest of the session - the phase offset that made every
        // script-driven mechanism (Level 11's sandbags) fight the rollback
        // engine.  Bounded: the game must not freeze for ever if the assignment
        // never comes.
        bool held_ = false;
        clock_type::time_point hold_deadline_{};
        int hold_polls_ = 0;
        uint32_t tick_base_ = 0;
        int64_t frames_since_anchor_ = -1;
        std::deque<std::pair<uint32_t, bmmo::session::input_frame>> input_history_;
        float previous_cam_[3][3] = {};
        bool previous_cam_valid_ = false;
        // Input freshness as the server saw it (session_snapshot_msg.acked_input_tick).
        uint64_t input_reports_ = 0, input_starved_ = 0;
        uint32_t worst_starved_ = 0;
        uint64_t lead_sum_ = 0, lead_count_ = 0;
        bmmo::session::body_corrector corrector_;
        std::map<std::string, bmmo::session::body_corrector> mechanism_correctors_;
        // Design 9.25: the mechanisms are tracked bodies of this client's own
        // world again, so a row has to be resolved onto one of its bodies
        // (mechanism_tracking.hpp), and every tracked name carries the
        // generation of the body behind it.
        bmmo::session::mechanism_registry mechanism_tracking_;
        std::map<std::string, uint32_t> generations_;
        uint64_t amend_failures_ = 0;
        std::vector<own_life> own_life_;   // struct own_life, beside beam_own_ball
        // Our own events' echoes, handled in the frame (process_own_echoes).
        std::vector<bmmo::session_event_msg> own_echoes_;
        // ...and those queued before the last SessionAssign: they answer
        // births of the old numbering (own_unanswered_), never a record.
        std::vector<bmmo::session_event_msg> own_echoes_stale_;
        // Own births set aside unanswered (struct own_life::aside).
        std::vector<own_life> own_unanswered_;
        // Origins of own Unphysicalize events the server echoed MOVED: an
        // own birth of the same origin is then a pair the server applied
        // late as a whole (a beam, a trafo, a respawn's pair), which moving
        // the birth alone cannot match (the own death's move is plan step 22).
        std::vector<uint32_t> own_moved_unphysicalize_;
        // Body changes stamped for the frame's own tick, held past the frame's
        // rewinds (drain_due_events -> apply_held_lifecycle, same frame).
        std::vector<bmmo::session_event_msg> held_lifecycle_;
        uint64_t own_lives_applied_ = 0;
        // Own births the server moved (MOVED echoes of a Physicalize) and
        // what this client did with them: moved inside the history, removed
        // from it (SUPERSEDED, REJECTED), resynced (past the reach, a life
        // that ended since, a pair moved as a whole, a missing echo), the
        // re-makes at the applied tick (live and in replays), and MOVED
        // echoes of the other types (kept where stamped).
        uint64_t own_birth_echo_moved_ = 0;
        uint64_t own_births_moved_ = 0, own_births_removed_ = 0, own_moves_resynced_ = 0, own_remakes_ = 0;
        uint64_t own_moves_other_ = 0;
        std::map<uint32_t, remote_ball> remotes_;
        bool have_snapshot_ = false;
        uint32_t last_snapshot_tick_ = 0;
        uint64_t inputs_sent_ = 0, events_sent_ = 0, events_received_ = 0;
        uint64_t snapshots_received_ = 0, snapshots_applied_ = 0, snapshots_stale_ = 0;
        uint64_t remote_writes_ = 0, mechanism_blends_ = 0, mechanism_hard_ = 0;
        uint64_t remote_inputs_received_ = 0;
        // The relayed frames, reassembled (session/input_relay.hpp): first
        // arrivals, re-deliveries of the window, and frames parked for a
        // player whose mirror did not exist yet.
        uint64_t relay_new_ = 0, relay_dup_ = 0, relay_parked_ = 0;
        // ...first arrivals too late for the rollback (deeper than it
        // re-simulates, or past the history), and the arrival lag of the first
        // arrivals in ticks (max, and a histogram for the 99th percentile): the
        // numbers that say whether the relay reaches the rollback in time.
        uint64_t relay_late_ = 0;
        // ...and first arrivals for a tick at which the mirror had no body
        // (dead, or not born here yet): stored, not replayed.
        uint64_t relay_bodiless_ = 0;
        // ...and for a mirror's birth tick, which no replay can change.
        uint64_t relay_birth_frozen_ = 0;
        uint32_t relay_lag_max_ = 0;
        static constexpr uint32_t kRelayLagBuckets = 129;
        std::array<uint32_t, kRelayLagBuckets> relay_lag_hist_{};
        // The window: line (report_window).  The status line's counters are
        // cumulative and five seconds apart, which cannot say whether a
        // rollback or a late frame fell before, during or after a burst at
        // one player; these are reset every kWindowTicks.  The lag histogram
        // is kept per source player because a late player shows up in the
        // others' lag for its frames alone.
        static constexpr uint32_t kWindowTicks = 60;
        uint32_t window_index_ = 0xFFFFFFFFu;
        std::map<uint32_t, std::map<uint32_t, uint32_t>> window_lag_;   // source -> lag -> first arrivals
        std::map<uint32_t, uint32_t> window_inrew_src_;                 // source -> input rewinds naming its ball
        struct window_base {
            uint64_t inrew = 0, inrew_depth = 0, life = 0, life_depth = 0, forced = 0, forced_depth = 0;
            uint64_t snaprb = 0, snaprb_depth = 0, snaps = 0, resyncs_sent = 0, resyncs_done = 0;
            uint64_t relay_new = 0, relay_late = 0;
            uint64_t subst_recv = 0, subst_diff = 0, subst_same = 0, subst_bodiless = 0, subst_late = 0;
            uint64_t substrew = 0, substrew_depth = 0;
            uint64_t subst_deferred = 0, subst_unseen = 0, subst_subsumed = 0, subst_saved = 0;
            uint64_t lead_adj = 0;
            uint64_t echo_acked = 0, echo_moved = 0, echo_superseded = 0, echo_rejected = 0, echo_missing = 0;
            uint64_t birth_moved = 0, own_moved = 0, own_removed = 0, own_resynced = 0;
            int rebases = 0;
        } window_base_;
        uint64_t input_rewind_depth_ = 0, life_rewind_depth_ = 0, forced_rollback_depth_ = 0;
        uint64_t snapshot_rollbacks_ = 0, snapshot_rollback_depth_ = 0;
        // The --trace lines after a snapshot rollback (apply_snapshot): the
        // first 16 (moved from the headless client's state at E4).
        int rollback_diag_ = 0;
        bmmo::session::relay_seen relay_seen_;
        bmmo::session::pending_relay pending_relay_;
        // Protocol 7 (late-player design 1.3): the server's notices of our own
        // substituted ticks, taken once per tick (keyed by our own id) and
        // applied (note_held_section): recv inside the reach, late past it
        // (a resync), base when no rewind could ever have reached it, dup a
        // re-delivery, stale_epoch the records of a datagram numbered from
        // another base (taken once per (epoch, tick) in held_stale_seen_).
        // Of recv: diff (the record amended and replayed), same (we played
        // the server's frame), bodiless (no own ball at the tick); rew the
        // replays our own substitution named, and their depth.
        // `held_floor_`: a record at or below it has no anchor this
        // numbering ever recorded - the base itself, or a tick a resync's
        // full snapshot overwrote.  The header of the newest datagram of our
        // own epoch: the server's ack and held_total.
        bmmo::session::relay_seen held_seen_;
        bmmo::session::relay_seen held_stale_seen_;
        uint64_t subst_recv_ = 0, subst_late_ = 0, subst_base_ = 0, subst_dup_ = 0, subst_stale_epoch_ = 0;
        uint64_t subst_diff_ = 0, subst_same_ = 0, subst_bodiless_ = 0, subst_rewinds_ = 0, subst_rewind_depth_ = 0;
        uint64_t subst_rewinds_missed_ = 0;   // own replays rewind_to refused (a resync each)
        // Frames a waiting own replay was put off (own_replay_pacer_), diffs
        // no replay can see (amended without a replay; in diff too), waiting
        // own replays another rewind carried (drop_carried_own_replay), and
        // the amendments that needed no own replay of their own (rode along
        // with another's, or carried by such a rewind).
        uint64_t subst_deferred_ = 0, subst_unseen_ = 0, subst_subsumed_ = 0, subst_saved_ = 0;
        bmmo::session::own_replay_pacer own_replay_pacer_;
        uint32_t subst_late_logs_ = 0, subst_unresolved_logs_ = 0;
        // The frames each uplink datagram carried (remember_sent), and the
        // notices of ticks not simulated yet with whether to correct them
        // (take_held_ahead); both numbered from our base.
        std::map<uint32_t, bmmo::session::input_frame> sent_inputs_;
        std::map<uint32_t, std::pair<bmmo::session::input_frame, bool>> held_ahead_;
        // SessionLead (late-player design 2.4, on_session_lead): the ticks
        // whose live frame plays the server's held frame, with whether it
        // still counts as a received notice (no notice named it first); the
        // decision ids taken this session; the slew in flight and the clock
        // its steps are measured on; the total acknowledged offset.  Counted
        // for the status and window lines: adjustments applied (lead_adj),
        // forced frames played, copies dropped, and decisions of another base.
        struct forced_own {
            bmmo::session::input_frame frame{};
            bool count = false;
        };
        std::map<uint32_t, forced_own> forced_own_;
        std::set<uint32_t> lead_seen_;
        bmmo::session::lead_slew lead_slew_;
        clock_type::time_point lead_slew_clock_{};
        int lead_offset_ = 0;
        uint64_t lead_adjustments_ = 0, lead_forced_played_ = 0, lead_dups_ = 0, lead_stale_ = 0, lead_void_ = 0;
        uint64_t lead_force_logs_ = 0;
        // SessionLeads a host without the slew capability received (design
        // 2.3: a protocol error - counted and journaled, never applied or
        // acknowledged).  Always 0 on the headless client, which can slew.
        uint64_t lead_refused_ = 0;
        // The completeness check (check_held_complete): the ticks learned
        // since the epoch, the recent ones by tick, the shortfall already
        // counted, and the datagrams whose held_total was ahead (tick ->
        // held_total), judged kHeldGapTicks later.
        uint64_t held_learned_total_ = 0, held_gap_credit_ = 0;
        std::set<uint32_t> held_learned_recent_;
        std::map<uint32_t, uint32_t> held_gap_watch_;
        uint32_t held_floor_ = 0;
        bool held_header_known_ = false;
        bool acked_header_known_ = false;     // the newest server tick whose header counted for acked
        uint32_t acked_header_tick_ = 0;
        uint32_t header_acked_ = 0;            // that header's acked: our newest frame the server applied fresh
        uint32_t held_header_epoch_ = 0, held_header_tick_ = 0, held_header_acked_ = 0, held_header_total_ = 0;
        // Lifecycle events (protocol 6): the first arrival of each counts, and
        // our own are repeated unreliably on the next ticks (flush_inputs).
        bmmo::session::event_seen event_seen_;
        bmmo::session::event_repeats own_event_repeats_;
        uint64_t event_dups_ = 0, events_superseded_ = 0;
        // Protocol 9: echoes of our own events (process_own_echo).
        uint64_t own_echo_acked_ = 0, own_echo_moved_ = 0, own_echo_superseded_ = 0, own_echo_rejected_ = 0;
        uint64_t own_echo_missing_ = 0;   // own births whose echo never came (expire_own_echoes)
        std::map<uint32_t, uint32_t> remote_last_physicalize_;   // origin of each remote's last applied Physicalize
        // Each remote's newest applied Unphysicalize, by origin, with the tick
        // it was applied at: an older birth relayed after it is put back in
        // front of it (apply_event).
        struct applied_death { uint32_t origin = 0, applied = 0; };
        std::map<uint32_t, applied_death> remote_last_unphysicalize_;
        uint64_t births_overtaken_ = 0;   // older births dropped (same tick) or re-ordered (earlier) behind a death
        bool resync_pending_ = false;
        // A resync request sent and not answered yet (no assignment with a
        // newer base): its full snapshot covers every reason found meanwhile.
        bool resync_in_flight_ = false;
        // A resync the own correction needs (a late or lost substituted
        // frame, a refused own replay) that the 2 s limiter held back: asked
        // again from frame() once the limiter allows, unless an assignment
        // with a newer base answers it first.
        bool resync_owed_ = false;
        std::string resync_owed_reason_;
        int consecutive_hard_ = 0;
        clock_type::time_point last_resync_request_{};
        uint64_t resyncs_sent_ = 0, resyncs_done_ = 0, sessions_ended_ = 0;
        bmmo::session::rollback_engine rollback_;
        std::map<uint32_t, bmmo::session::input_frame> own_inputs_;
        // The event log's mute in force (session_client_core::event_log_mute;
        // moved from the headless client at E6).
        bool event_log_muted_ = false;
        // The black box (design 9.15) and its bookkeeping moved into
        // core::client_journal at E2 (session_client_core::journal()).

    };

    // Design 1.2 "Configuration": what the host decides for the session, filled
    // before the session starts (begin takes it from E3 on).  Each step adds
    // the fields its moved code reads.
    struct core_config {
        // The LOCAL checkpoint cadence in ticks (headless --checkpoint-ticks,
        // retail BMMO_JOURNAL_CHECKPOINT_TICKS); 0 = core::kJournalCheckpointTicks.
        uint32_t journal_checkpoint_ticks = 0;
        // The level to load when SessionStart names none (headless --level).
        int level = 1;
        // The rollback engine's switches, applied by begin (E3).  The defaults
        // are the headless client's argument defaults.
        bool verbose = false;               // set_verbose (headless --trace)
        bool restore_recheck = true;        // set_recheck_on_restore
        bool two_pass_recheck = false;      // set_two_pass_recheck
        bool restore_impulses = true;       // set_restore_impulses
        bool restore_quaternions = true;    // set_restore_quaternions
        bool restore_clock = false;         // set_restore_clock
        bool restore_calm = true;           // set_restore_calm
        bool restore_unit = true;           // set_restore_unit
        bool restore_contacts = true;       // set_restore_contacts
        uint32_t contact_flags = BMMO_PHYSICS_CONTACT_DEFAULT;   // set_contact_flags
        bool sleep_as_recorded = true;      // set_sleep_as_recorded
        bool script_replay = true;          // set_replay_scripts
        bool savestate = true;              // set_exact_restore
        uint32_t script_diff_logs = 8;      // set_script_log_cap
        bool trace_plan = false;            // set_trace_plan
        // The flow's switches (E4), the headless argument defaults.
        bool rollback = true;               // the snapshot rollback; off: the legacy blend ladder (--no-rollback)
        bool correct = true;                // the ladder writes its hard corrections (--no-correct: logged only)
        bool trace = false;                 // the core's diagnostic lines (headless --trace)
    };

    // Transitional (design 6.1, "Linking while the core is incomplete"): one
    // std::function per host function that code already moved into the core
    // still calls.  Each E step adds the entries its moved code needs and
    // deletes those whose target it moves; E11 deletes the struct.  The
    // headless client fills every entry; the core calls them unguarded.
    struct legacy_calls {
        // E3: SessionAssign (they move with the own lives, E7).
        std::function<void()> rebase_own_lives;
        std::function<void(client_core_state::own_life::aside why)> set_aside_unanswered;
        // (E3's reset_held_epoch, drop_pending_rewind and finish_lead_slew,
        // and E4's drop_carried_own_replay, were removed at E5: section J and
        // SessionLead moved into the core.)
        // E3: end - the own body guard off (host state).  Until E6 this was
        // end_session_world, which took every mirror's navigation and body
        // out too; end does that now (detach_mirror_navigation,
        // unmake_mirror).
        std::function<void()> own_body_guard_off;
        // E3: end prints the status line (E10: status_text and hooks.status).
        std::function<void()> report;
        // E4: apply_snapshot; E5: replay_corrected_inputs and force_rewind.
        // The input a navigated ball had at `tick`, for a re-simulation: the
        // own ball's exact record, a remote's relayed frame.  It stays in the
        // host past E5 (design 6.1 had it move with the relay): it tells the
        // own ball by the own replica's entity as the host last named it,
        // whether or not the replica still exists, which own_driven_entity
        // does not say (E9: the own ball's platform half).
        std::function<bool(const std::string& entity, uint32_t tick, bmmo::session::input_frame& out)> recorded_input_at;
        // E4: the own ball's entity name, "" when the level has none (E9: the
        // platform's own_ball).
        std::function<std::string()> own_ball;
        // E4: the movable objects' description, the --trace line after a
        // snapshot rollback (a headless diagnostic, design 2.4).
        std::function<std::string()> describe_movable_objects;
        // E6: the own lives' part of the lifecycle (E7 moves them).  The
        // own body changes due at the frame's tick, made live at our place
        // among the held events (apply_held_lifecycle); whether a replayed
        // tick has any, and making them there (drain_pending_births).
        std::function<void(uint32_t frame_tick)> live_own_lives;
        std::function<bool(uint32_t tick)> own_lives_due;
        std::function<void(uint32_t tick, const char* where, bool live)> apply_own_lives;
    };

    // anchor (design 1.3): what the host's anchor work found.
    struct anchor_report {
        bmmo::physics::world_hash hash;   // the anchor's world hash (SessionReady, the journal header)
        std::string physics_sha256;       // SessionReady::physics_sha256 (headless: "headless-64" / "headless-32")
        std::string build_id;             // SessionReady::build_id and the journal header's
        std::string own_name;             // the journal's own player name
        int load_ticks = 0;               // the ticks the level took to load (the anchored log line)
    };

    // begin: refused when the server runs another physics protocol.
    struct begin_result {
        bool refused = false;
    };

    // poll_hold: still holding (the host steps nothing and polls again), or
    // timed out on this poll (the hold is over; the core logged and journaled
    // it).  Neither: not holding.  The headless host reads only `held`;
    // `timed_out` is kept for the retail host's blocking loop (R1).
    struct hold_state {
        bool held = false;
        bool timed_out = false;
    };

    class session_client_core {
    public:
        // Throws std::invalid_argument when the platform lacks an operation
        // every host must fill or one a declared capability needs (design
        // 2.1: "The constructor asserts that every field a declared
        // capability needs is filled").
        session_client_core(session_platform platform, session_capabilities caps);
        // The journal holds a reference back to the core: neither copied nor
        // moved.
        session_client_core(const session_client_core&) = delete;
        session_client_core& operator=(const session_client_core&) = delete;

        // The core's state.  Public from E1 to E10, while the flow still
        // lives in the host: the headless client reaches it as
        // core_.s.<member>.  E11 makes it private.
        client_core_state s;
        // Transitional back-calls into the host (design 6.1): filled by the
        // headless client, by the fake-platform test with recorders, and left
        // empty by the retail mod, which does not run the core before R1.
        legacy_calls legacy;
        // The host's configuration (design 1.2), taken by begin.
        core_config config;

        const session_platform& platform() const { return platform_; }
        const session_capabilities& capabilities() const { return caps_; }

        // The tick the world stepped last (design 1.3): the tick base until
        // the first frame after the anchor.
        uint32_t current_tick() const {
            return s.frames_since_anchor_ >= 1 ? s.tick_base_ + static_cast<uint32_t>(s.frames_since_anchor_ - 1) : s.tick_base_;
        }
        // The rollback engine's statistics (design 1.1; E4): what a host's
        // keep_live_cells guard compares before and after a rollback.
        const bmmo::session::rollback_stats& rollback_stats() const { return s.rollback_.stats(); }

        // The session's journal (design 1.2).
        core::client_journal& journal() { return journal_; }
        const core::client_journal& journal() const { return journal_; }

        // ---- session lifecycle (design 1.3; E3) ----
        // SessionStart: the session's parameters and fresh runtime state, the
        // rollback switches from `config`.  Refused on a protocol mismatch.
        // The host then does its own resets, requests the level and calls
        // level_requested().
        begin_result begin(const bmmo::session_start_msg& msg, const core_config& config);
        // The level is requested: the host loads it until its anchor.
        void level_requested();
        // The level is up and the host's anchor work done: SessionReady, the
        // journal opened, the hold armed until the tick base arrives.
        void anchor(const anchor_report& report);
        // The anchor hold, once per host loop pass while it holds.
        hold_state poll_hold();
        // SessionEnd: drop the mirrors and go idle.
        void end(const std::string& reason);

        // ---- messages (E3: decoded by the host until on_message exists) ----
        void on_session_assign(const bmmo::session_assign_msg& msg);
        void on_session_end(const bmmo::session_end_msg& msg);
        // SessionSnapshot (E4): counted, then applied when it is this
        // running session's.
        void on_session_snapshot(const bmmo::session_snapshot_msg& msg);
        // SessionRemoteInput (E5): counted; when it is this session's, the
        // held section (our own substituted ticks) and the relayed frames of
        // the other players - stored, journaled, and a replay asked for
        // where a frame differs from the one the prediction used.
        void on_session_remote_input(const bmmo::session_remote_input_msg& msg);
        // SessionLead (E5, late-player design 2.4): this running session's,
        // for our own player, once assigned.  Without the slew capability a
        // protocol error (design 2.3): counted, journaled, not applied, not
        // acknowledged.
        void on_session_lead(const bmmo::session_lead_msg& msg);
        // SessionEvent (E6): counted; when it is this running session's and
        // its first arrival, an echo of our own event is queued for the
        // frame's echoes, another member's for its due tick.
        void on_session_event(bmmo::session_event_msg&& msg);

        // ---- the remote lifecycle (E6; pre_step steps 2 and 8) ----
        // Every queued event due at `tick`, in the server's order; a body
        // change stamped for `tick` itself is held for apply_held_lifecycle.
        void drain_due_events(uint32_t tick);
        // The held body changes, after the frame's rewinds and before the
        // union's pass, with our own changes due at the tick at our place.
        void apply_held_lifecycle(uint32_t frame_tick);
        // Before a replayed tick (the host's binding, before_replayed_tick):
        // the births deferred into the replay, made at their tick.
        void drain_pending_births(uint32_t tick);
        // A physicalized mirror without navigation is driven from now on, if
        // the host has the navigation (post_step step 16, and every birth).
        void attach_remote_navigation(uint32_t player, client_core_state::remote_ball& remote);
        // The frame a live step of `tick` drives a predicted mirror with
        // (post_step step 28).
        static bmmo::session::input_frame live_drive(const client_core_state::remote_ball& remote, uint32_t tick);

        // The event log muted for a scope, nesting (platform mute_event_log;
        // the flag in force is client_core_state::event_log_muted_).  Public
        // while the own body's re-make and removal are the host's (E7).
        class event_log_mute {
        public:
            explicit event_log_mute(session_client_core& core);
            ~event_log_mute();
            event_log_mute(const event_log_mute&) = delete;
            event_log_mute& operator=(const event_log_mute&) = delete;

        private:
            session_client_core& owner;
            const bool was;
        };

        // ---- the frame's rewinds (E5; pre_step steps 5 and 7, design 1.3) ----
        // One replay for every input correction the frame collected (the
        // relay's and our own substituted frames').
        void replay_corrected_inputs();
        // A forced rollback `depth` ticks deep from the current tick (the
        // headless --force-rollback, the retail forced-rollback verb).  The
        // live cells are kept (keep_live_cells) for as long as the caller
        // keeps the result: the host's traces after the rewind still run
        // inside that scope, as they did when the whole of it was the host's.
        // Without rollback, or before `depth + 2` ticks are played, it does
        // nothing (not rolled, not counted, no cells kept).
        struct forced_rewind {
            bool rolled = false;
            scope_guard keep_cells;
        };
        forced_rewind force_rewind(uint32_t depth);

        // ---- part two's forced own frames (E5; design 1.3 steps 11 and 21) ----
        // The forced frame of `tick` (a SessionLead's (c, E)), taken: the
        // older ones are dropped.  False: `tick` is not forced.
        bool take_forced_own(uint32_t tick, client_core_state::forced_own& out);
        // A forced tick plays the server's frame: `input` becomes it, counted
        // as played, and as a received notice when no notice named it first.
        void play_forced_own(uint32_t tick, const client_core_state::forced_own& forced,
                             bmmo::session::input_frame& input);

        // ---- part one (E5; post_step step 24) ----
        // After the live frame of `tick` is recorded: a notice that named it
        // earlier is applied now.
        void take_held_ahead(uint32_t tick);
        // The SessionLead slew ends: acknowledged with what it moved (all of
        // it when `reanchored`, a resync request ended it).  The host's
        // pacer calls it when the slew is done (E9: slew_step).
        void finish_lead_slew(const std::string& why, bool reanchored = false);
        // Another rewind from `anchor` carried a waiting own-only replay
        // (any anchor when `server_rows`): dropped instead of run.
        void drop_carried_own_replay(uint32_t anchor, bool server_rows);
        // The newest relay header is of our own epoch (the status line).
        bool held_header_current() const {
            return s.held_header_known_ && s.held_header_epoch_ == s.tick_base_;
        }

        // ---- numbering and resync ----
        // `owed`: the own correction needs this resync; one the limiter
        // holds back is asked again by retry_owed_resync.
        void request_resync(const char* reason, bool owed = false);
        void retry_owed_resync();

        // ---- sending ----
        // The input history and the event repeats due, once assigned.
        void flush_inputs();
        // The sent ring: the frames an uplink datagram carried, by tick.
        void remember_sent(const bmmo::session_input_msg& msg);
        // An own lifecycle event: stamped, counted, journaled, then on the
        // wire unless hooks.filter_event keeps it.
        void send_event(bmmo::session_event_msg& event);
        // The reliable copy, and the unreliable repeats queued.
        void put_event_on_wire(const std::string& bytes);
        // A held send (hooks.filter_event) goes out now.
        void release_event(const std::string& wire_bytes) { put_event_on_wire(wire_bytes); }

        // ---- reading ----
        bool assigned() const { return s.assigned_; }
        bool holding() const { return s.held_; }
        bool resync_pending() const { return s.resync_pending_; }

        // The core's log line: printf-formatted into a growing buffer and
        // handed to platform().log as one line (design 6.1).  GCC and Clang
        // check the format against its arguments (argument 1 is this).
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        void logf(const char* format, ...);

    private:
        // The snapshot path (E4, client_core_messages.cpp): the resync
        // rebuild, the snapshot rollback, or the legacy ladder.
        void apply_snapshot(const bmmo::session_snapshot_msg& snapshot);

        // The relay (E5, client_core_messages.cpp): a relayed frame for a
        // tick already simulated, against the frame the prediction used.
        void note_input_correction(const client_core_state::remote_ball& remote, uint32_t tick,
                                   const bmmo::session::input_frame& relayed);
        // Section J (E5, client_core_held.cpp): late-player parts one and two.
        static bool same_frame(const bmmo::session::input_frame& a, const bmmo::session::input_frame& b);
        void note_held_section(const bmmo::session_remote_input_msg& msg);
        bool held_tick_is_base(uint32_t tick) const;
        void settle_held_tick(uint32_t tick, const bmmo::session::input_frame& frame);
        void drop_lead_state(const char* why);
        bool resolve_held_frame(const bmmo::session_remote_input_msg::held_record& record,
                                bmmo::session::input_frame& out) const;
        void store_held_frame(uint32_t tick, const bmmo::session::input_frame& frame);
        void hold_ahead(uint32_t tick, const bmmo::session::input_frame& frame, bool correct);
        void correct_own_tick(uint32_t tick, const bmmo::session::input_frame& frame);
        void learn_held_tick(uint32_t tick);
        uint64_t held_learned_upto(uint32_t tick) const;
        static constexpr uint32_t kHeldGapTicks = 64;
        void check_held_complete(const bmmo::session_remote_input_msg& msg);
        void reset_held_epoch(const char* why = "the new base");
        void drop_pending_rewind();
        bool own_notices_coming() const;

        // The remote lifecycle (E6, client_core_lifecycle.cpp).
        uint32_t join_rank(uint32_t player) const;
        void apply_event(const bmmo::session_event_msg& event);
        void make_remote_birth(const bmmo::session_event_msg& event);
        void rewind_for_lifecycle(const bmmo::session_event_msg& event);
        void rewind_for_lifecycle(uint32_t player, uint32_t event_tick);
        bool unphysicalize_remote(uint32_t player, uint32_t tick);
        uint32_t live_first_tick(uint32_t stamp) const;
        void unphysicalize_mirror(const std::string& entity);
        void physicalize_remote(const client_core_state::pending_birth& event, uint32_t first_tick);

        session_platform platform_;
        session_capabilities caps_;
        // Last, so it is destroyed first (the file closes before the state
        // it describes goes away).
        core::client_journal journal_{*this};
    };

}
