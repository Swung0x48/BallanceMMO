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
// Step E7 moves the own lives and echoes (design 6.1 row E7): the own births
// reported (report_physicalize, or a host's own_physicalized committed at
// step 19), the beam (request_beam), the own_life records made again or
// taken out at the history's changes, and the echoes of our own events
// (process_own_echoes, move_own_birth, resync_own_life, expire_own_echoes),
// in client_core_lifecycle.cpp, over the platform's ball_recipe_of,
// own_birth_capture, make_own and unmake_own, with the own_body_control
// fallback of design 2.3 and the own_echoes switch.
//
// Step E8 moves the sectors (design 6.1 row E8): request_sector with the
// start sector pending until the assignment, hold_sector, the union's pass
// before the step and the own sector poll after it, and the anchor's
// sector_cell_, in client_core_frame.cpp, over the platform's
// sector_manager_active, run_sector, record_activation and poll_sector.
//
// Step E9 moves the frame (design 6.1 row E9): pre_step and post_step (the
// tables of design 1.3) replace the body of the headless frame(), with the
// input building, the rollback record and amend, the mirrors' drive, the
// legacy blends, recorded_input_at and the pacer's slew_step /
// rebase_threshold / schedule_rebased, in client_core_frame.cpp; the host
// keeps the keys, the step and its hooks.  The rollback binding is the
// host's through the shared bind_session_world, with the core's half
// (core_world_hooks).
//
// Step E10 moves the reports (design 6.1 row E10): status_text and
// window_text, in client_core_report.cpp.
//
// Step E11 closes the core (design 6.1 row E11): the state is private (the
// host reads what it needs through the accessors under "reading", and tells
// the core its login id with set_own_id); the frame's steps E5-E9 had left
// public are private too (pre_step / post_step call them); `legacy_calls`
// is gone (its last two entries are the optional hooks.ended and
// hooks.describe_movables, client_platform.hpp).  The unit tests reach the
// private state and steps through core_test_access, which only they define.
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
#include "member_lives.hpp"
#include "mechanism_tracking.hpp"
#include "own_correction.hpp"
#include "rollback.hpp"
#include "sector_union.hpp"
#include "trafo_input.hpp"
#include "../entity/session.hpp"
#include "../message/session_assign_msg.hpp"
#include "../message/session_end_msg.hpp"
#include "../message/session_event_msg.hpp"
#include "../message/session_input_msg.hpp"
#include "../message/session_lead_msg.hpp"
#include "../message/session_member_msg.hpp"
#include "../message/session_remote_input_msg.hpp"
#include "../message/session_resync_state_msg.hpp"
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

    // An own birth as the host reports it (design 1.3 own_physicalized; E7):
    // the ball's entity and its row in the level's ball table (-1: none, sent
    // as type 0), the tick whose step it was made before, the pose and spawn
    // flag of its Physicalize, and its recipe.
    struct own_birth_report {
        std::string entity;
        int ball_type = -1;
        uint32_t tick = 0;
        own_birth_pose pose;
        bmmo::session::ball_recipe recipe;
    };

    // Design 1.2: everything the session flow decides from, as the headless
    // client declared it (session_client.cpp at 9d9cb08), in the same order
    // and with the same initializers and comments.  The nested types moved
    // with it (remote_ball, own_life, pending_birth, forced_own,
    // applied_death, window_base); the host names them through aliases.
    // Of the dead members design 1.2 lists, E11 deleted spawn_position_,
    // spawn_known_ and anchor_hash_ from the host; snapshot_interval_ is
    // live core state since E7 (the own_body_control fallback reads it).
    // Private to session_client_core since E11.
    struct client_core_state {
        using clock_type = std::chrono::steady_clock;

        enum class phase { idle, loading, running };

        struct pending_birth {
            uint32_t player = 0, tick = 0, origin = 0;   // tick: applied (protocol 9)
            uint8_t ball_type = 0, flags = 0;
            float position[3] = {};
            float rotation[9] = {};
            bmmo::session::ball_recipe recipe;
            // Step LB: the player's current mirror made again inside a
            // replay (remake_mirror_birth) - the same entity and lifetime,
            // nothing of the remote's live state touched but its body and
            // navigation.
            bool remake = false;
            // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): the
            // mirror entity the live event made, filled from the life log -
            // physicalize_remote then makes that entity and never mints a
            // new one (mirror_entity).  Empty: a birth not made yet (its
            // first make mints it, as before).
            std::string entity;
            // Queued by the reconcile of an ended life (reconcile_remote_
            // lives): one that no replay made is made live after the rewind
            // (settle_unreplayed_lives), as it was before the rewind.
            bool reconciled = false;
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
            // Step LB: the Physicalize the current body was made from, when
            // it was made at its stamp (inside a replay, or live on time):
            // every later restore anchored before born_tick makes it again
            // from this at that tick (remake_mirror_birth), so the birth
            // tick's step is re-simulated with the inputs the history has
            // then.  False for a body started elsewhere (a late live birth;
            // before R3 also retail's row start): it keeps the parking path.
            pending_birth birth{};
            bool birth_known = false;
            std::map<uint32_t, bmmo::session::input_frame> inputs;   // relayed frames by tick
            bmmo::session::body_corrector corrector;
            uint64_t blends = 0, hard_sets = 0;
            // Step BALA (the airship rule): the tick this world first found
            // the mirror's body in the airship zone - a ghost from there,
            // frozen by the zone's PSI hook, out of the sync - 0 while it is
            // not.  Positional: a rewind before it and a death clear it.
            uint32_t airship_tick = 0;
            // Whether the host shows it as an airship ghost (step BALL: the
            // legacy ball stream moves it, translucent).
            bool ghost_shown = false;
            // Step SPGH: whether this world's copy of the ball is a spawn
            // ghost after the last frame, and whether it is shown as one.
            bool spawn_ghost = false;
            bool spawn_ghost_shown = false;
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

        uint32_t own_id_ = 0;
        phase phase_ = phase::idle;
        uint32_t session_ = 0;
        // SessionStart::snapshot_interval (the server clamps a longer one to
        // 255: "snapshots off").  Dead state in the headless client until E7,
        // whose own-echo fallback without own body control reads it.
        uint8_t snapshot_interval_ = 2;
        uint8_t input_delay_ = 6;
        uint64_t life_rewinds_ = 0, life_rewinds_missed_ = 0;
        // Births stamped in the past, waiting to be made inside a replay at
        // their tick (apply_event / drain_pending_lives).
        std::vector<pending_birth> pending_births_;
        uint64_t input_rewinds_ = 0, input_rewinds_missed_ = 0;
        uint64_t forced_rollbacks_ = 0, forced_rollbacks_missed_ = 0;
        bool start_sector_pending_ = false;  // a --start-sector waiting for the first assigned frame
        bmmo::session::gated_sector_union sectors_;   // the sectors this session runs, the rule every side runs
        uint8_t sector_lead_ = 0;            // SessionStart::sector_lead
        std::set<int> sectors_requested_;    // every sector THIS client reported (1 from the anchor)
        int sector_cell_ = 0;                // IngameParameter[0][1] as last seen or written here
        int chain_cell_ = 0;                 // the checkpoint chain's private [0][1] as last polled (step B2SX)
        // The last two union passes (tick, sector; tick 0 = none) and the
        // passes whose modules found the phase cell 0 in their first frame
        // (poll_own_sector: the checkpoint chain's race, or any writer like it).
        std::pair<uint32_t, int> recent_passes_[2] = {};
        uint64_t phase_misses_ = 0;
        // Step B1 (resync design 2.5, 2.7, 2.8): the union as rollback state.
        // The stamp log: one entry per (player, sector), the tick the server
        // applies it at (a MOVED own echo rewrites it) and its arrival order;
        // every stamp this world was given goes in - the own ones handed to
        // the union, the relayed ones - and it lives for the session (its
        // ticks are shared ticks).  A re-derivation takes its stamps from it.
        struct sector_stamp {
            uint32_t tick = 0;
            uint64_t seq = 0;
        };
        std::map<std::pair<uint32_t, int>, sector_stamp> sector_stamps_;
        uint64_t sector_stamp_seq_ = 0;
        // The union's state after tick(t), beside the rollback record of t.
        std::map<uint32_t, bmmo::session::sector_union::state_type> union_records_;
        // Every union pass this world ran (tick -> sector), live or
        // re-derived, the last history_ticks + 128 ticks.
        std::map<uint32_t, int> pass_log_;
        // This frame's late Sector stamps inside the reach and moved own
        // stamps (the frame's Sector rewind, pre_step), with the tick each
        // needs re-derived from.
        struct late_sector {
            uint32_t tick = 0;      // the anchor tick: the stamp (a moved own one: its origin)
            uint32_t player = 0;
            int sector = 0;
            bool moved = false;     // an own stamp the server applied at `applied`
            uint32_t applied = 0;
        };
        std::vector<late_sector> sector_late_;
        // A re-derivation in progress (the frame's Sector rewind): the rule
        // (sector_rederivation: the union as the replay re-derives it, the
        // live passes of the window it replaces, the passes it made), active
        // only inside the rewind, and the live tick it rewound from.
        struct union_rederivation {
            bool active = false;
            uint32_t live_tick = 0;
            bmmo::session::sector_rederivation r;
        };
        union_rederivation rederive_;
        bool last_rewind_rederived_ = false;   // read by the host's live-cells guard (adopt mode)
        // Sector rewinds made and missed (a resync each), and the live passes
        // a re-derivation did not reproduce at their tick (moved or forgotten).
        uint64_t sector_rewinds_ = 0, sector_rewinds_missed_ = 0, sector_rewind_depth_ = 0;
        uint64_t sector_passes_forgotten_ = 0;
        // The sectors start_sector_pending_ stands for, each with why it was
        // asked for, in the order requested (request_sector before the
        // assignment; E8: the headless --start-sector, which was
        // args_.start_sector until then, is the only one there; a retail
        // host may ask for more than one before its assignment).
        std::vector<std::pair<int, std::string>> start_sectors_;
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
        // Step M1 (world-agreement-gaps-design.md 2.8): every life of every
        // member this client was told of - SessionStart's (from 0) and each
        // SessionMember(added) since, with its removal tick once heard.
        // Nothing is erased during a session: a replay across a life's R
        // still reads its entry.  Every lookup that used to take the id alone
        // takes the life the applied tick names (member_life_at; J3: two
        // lives of one id never overlap, and an event of a life is applied
        // inside it).
        std::vector<bmmo::session::member_life> lives_;
        // An event of a life not told of yet (its SessionMember still on the
        // way), by (player, life): applied once the life arrives, through
        // the late path when its tick has passed.  Never a mirror made with a
        // placeholder join order.
        std::map<std::pair<uint32_t, uint16_t>, std::vector<bmmo::session_event_msg>> parked_events_;
        // A SessionMember(removed) of a life whose `added` has not come yet:
        // (player, life) -> R, applied when it arrives.
        std::map<std::pair<uint32_t, uint16_t>, uint32_t> early_removals_;
        uint16_t own_member_life_ = 0;   // the own entry's life (SessionStart)
        uint64_t member_messages_ = 0, member_copies_ = 0, events_parked_ = 0, parked_applied_ = 0;
        uint64_t member_life_outside_ = 0, mirrors_outside_lives_ = 0, own_other_life_echoes_ = 0;
        uint64_t catchup_unknown_lives_ = 0;
        int own_join_order_ = -1;
        float spawn_impulse_ = 0.0f;             // SessionStart::spawn_impulse (design 9.10)
        float spawn_spacing_ = 0.0f;             // SessionStart::spawn_spacing (session/spawn_impulse.hpp)
        std::string own_body_entity_;            // the own ball entity that had a body after the last frame
        // Every entity the own ball has been this session (own_ball's entity
        // after each frame: the ball table's rows a trafo made current), with
        // or without a body: with the mirrors, what is_ball calls a ball
        // (step P).  Reset by begin.
        std::set<std::string> own_ball_entities_;
        std::string own_group_;
        // Step BALA: the airship zone of this world (installed at the
        // anchor), the first live tick the physics layer's latch held the
        // own ball in this life (0: not; cleared at its death and when a
        // rewind took the latch back), and the ghosts handed to the host's
        // display (step BALL: the mirror's ghost spans, not frames).
        bool airship_active_ = false;
        bool airship_box_logged_ = false;
        uint32_t own_airship_tick_ = 0;
        uint64_t ghosts_shown_ = 0;
        uint64_t own_corrections_out_of_sync_ = 0;   // held notices / forced frames of a tick the own ball had left the sync at
        uint64_t resyncs_own_kept_ = 0;              // resyncs that kept the out-of-sync own ball local
        uint64_t airship_rows_skipped_ = 0;          // snapshot ball rows of a ball in its zone, not applied
        // Step SPGH: the spawn ghost rule of this world (installed at the
        // anchor), and the own ball's state after the last frame.
        bool spawn_ghost_active_ = false;
        bool own_spawn_ghost_ = false;
        bool own_spawn_ghost_shown_ = false;
        std::string own_spawn_ghost_entity_;         // the entity shown translucent
        uint64_t spawn_ghost_transitions_ = 0;       // ghost -> solid seen after a frame (own and mirrors)
        uint64_t spawn_ghost_births_ = 0;            // step SPGH2: spawn births marked (live and replayed)
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
        // How long an own birth waits for its echo: the server's tick (the
        // relay header's, or the snapshots') this far past its stamp with no
        // echo, and the Physicalize either never reached the server or will
        // be applied past the reach - a resync either way, and a never-made
        // body has to go first.  Twice the history (2.9 s): far past every
        // hold arm, so a merely late event's echo always comes first.
        // (Moved from the headless client at E7.)
        static constexpr uint32_t kOwnEchoDeadlineTicks = 192;
        // ...counted only once the server has applied a frame of ours at or
        // past the birth's origin, and this long after that was first seen
        // (one second for the reliable copy's retransmission).
        static constexpr uint32_t kOwnEchoUplinkSettleTicks = 66;
        // A birth unanswered this long retires anyway (retire_own_lives).
        static constexpr uint32_t kOwnRecordKeepTicks = 512;
        // A beam asked for (session_client_core::request_beam: the headless
        // --beam, the retail verb) and not made yet, with its pose (E7).
        bool beam_requested_ = false;
        double beam_position_[3] = {};
        // The own body changes a host_hooks host reported in the behaviour
        // pass (own_physicalized / own_unphysicalized), committed in order
        // at the frame's step 19 (commit_own_reports; E7).
        struct own_report {
            bool birth = true;            // a Physicalize; false: an Unphysicalize of report.tick
            own_birth_report report;
        };
        std::vector<own_report> own_reports_;
        std::map<uint32_t, remote_ball> remotes_;
        bool have_snapshot_ = false;
        uint32_t last_snapshot_tick_ = 0;
        uint64_t inputs_sent_ = 0, events_sent_ = 0, events_received_ = 0;
        uint64_t snapshots_received_ = 0, snapshots_applied_ = 0, snapshots_stale_ = 0;
        uint64_t snapshots_uncorrected_ = 0;   // step RC3: full snapshots of a snapshots-off session taken for their names only
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
        // Late-player plan step 12: the relay lag of the server's newest
        // step at each first arrival of it (current - header tick, the
        // own notices' lag; it needs no remote) over the last 4 windows,
        // and what it set: reach changes, the deepest reach of the
        // session, the change lines logged.
        bmmo::session::reach_meter reach_meter_;
        uint64_t reach_changes_ = 0;
        uint32_t reach_max_ = 0;
        uint32_t reach_logs_ = 0;
        // Step R12 (protocol 13): the server's wait in the newest relay
        // header (any epoch: it is the session's step timing, not the
        // numbering's), which the reach adds to its margin, and whether
        // one was received (the WAIT journal record on each change).
        uint8_t server_wait_ = 0;
        bool server_wait_known_ = false;
        static constexpr uint32_t kRelayLagBuckets = 129;
        std::array<uint32_t, kRelayLagBuckets> relay_lag_hist_{};
        // The window: line (window_text).  The status line's counters are
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
        // Step LB: mirrors a restore took out and made again at their birth
        // tick (the replay, or make_unmade_remakes live), and those neither
        // could make again (left bodiless).  The engine's births_recreated
        // counts the remakes too, among the mechanisms it re-creates.
        uint64_t mirror_remakes_ = 0, mirror_remakes_lost_ = 0;
        // Gap L, L2 (step EL7): mirror lives alive at an anchor and ended
        // inside the window that the reconcile made again (revived) or could
        // not (revive_failed: no log entry, an inconsistent log, a refused
        // make, the parked arm); window events no replay applied, made live
        // after the rewind (settled); late births kept on the parking path
        // (made live after their stamp: the RC stamp rule's documented
        // class).
        uint64_t mirror_revives_ = 0, mirror_revive_failures_ = 0, lives_settled_live_ = 0, late_births_kept_ = 0;
        // Players whose window lives the reconcile replayed with no ended
        // A-life (a life born and ended inside the window, two births in it,
        // or a body made after a stamp inside it; review finding 1).
        uint64_t window_lives_ = 0;
        // The anchor of the restore being planned (set by the reconcile,
        // which every restore with a replay asks): late_births_kept counts
        // only births stamped at or before it, once per (player, stamp).
        uint32_t lives_anchor_ = 0;
        std::set<std::pair<uint32_t, uint32_t>> late_births_counted_;
        // Every mirror entity this session made (step EL7): the binding's
        // mirror test knows a life's entity after the remote moved on to
        // another (a later life the reconcile takes out and the replay
        // makes again from its inputs).
        std::set<std::string> mirror_entities_;
        // Step G3 (world agreement 2.5): every mirror entity this session
        // made, with its player and ball type - the candidates of the
        // proximity union's place point (a life's entity is a candidate of
        // its player while it has a body, the current one or a revived one).
        std::map<std::string, std::pair<uint32_t, uint8_t>> mirror_owners_;
        // The proximity union (step G3): on in this session (the world
        // option, and the host places), its place calls live and replayed,
        // and the invariant guard: two bodies of one player at a place point
        // (counted, logged once; the first by name is taken).
        bool probes_active_ = false;
        uint64_t probe_places_live_ = 0, probe_places_replayed_ = 0, probe_two_bodies_ = 0;
        // Step G5 (world agreement 2.6): the script wakes BodyRevived would
        // have reported, kept for the status line (revived_local=) when the
        // session runs the union and sends none.
        uint64_t revived_local_ = 0;
        // ---- gap L, L3: the own ball (rollback-engine gaps design 2.3.4,
        // 9.7; step EL8) ----
        // An own entity's body removed live (the drain's "deleted" entry of
        // an own ball entity: a death, or the old ball of a trafo): its tick
        // D, the life's generation (before the bump) and the entity's CK
        // world matrix after frame D.  Kept while a rewind can be anchored
        // before it, as the other own records.  (Adapted: a list of its own,
        // not a third own_life kind - the echo matching scans own_life_ by
        // origin and verdict and would take a death for a birth.)
        struct own_death {
            uint32_t tick = 0;
            std::string entity;
            uint32_t generation = 0;
            float matrix[16] = {};
            bool has_matrix = false;
        };
        std::vector<own_death> own_deaths_;
        // What the own reconcile queued into the replay, applied at the end
        // of drain_pending_lives(tick): a revived life's death (its matrix)
        // or a window life's birth remade at its stamp.
        struct own_due {
            uint32_t tick = 0;
            bool death = false;
            std::string entity;
            float matrix[16] = {};
            own_life life;   // the birth (death = false)
            // Step OWNB: a birth the scripts made that a restore took out
            // (destroy_moved_own_birth): made again from what is known now,
            // so the replay's step is its state (own_birth_remade).
            bool scripted = false;
        };
        std::vector<own_due> own_due_;
        // Step OWNB: own births the scripts made that a restore took out and
        // queued for the replay's drain (destroy_moved_own_birth).
        uint64_t own_births_remade_ = 0;
        // Design 9.7 (a): each live tick's CK world matrix of every own
        // entity without a body after that frame (the death sequence and the
        // trafo animation move it, scripts no replay runs), written before
        // each replayed tick's scripts from that same tick's record (the
        // movers run before the modules that read it; step EL8's gate).
        struct own_matrix {
            std::string entity;
            float matrix[16] = {};
        };
        std::map<uint32_t, std::vector<own_matrix>> own_matrices_;
        // olives=revived/failed/remade/settled/matrices: own lives revived
        // at a restore, those that could not be (no logged death, no record,
        // a refused make), window lives remade at their stamp, queued own
        // changes no replay applied (made live after the rewind), and the
        // matrices written into replayed ticks.
        uint64_t own_revives_ = 0, own_revive_failures_ = 0, own_window_remakes_ = 0, own_due_settled_ = 0;
        uint64_t own_matrix_writes_ = 0;
        uint64_t births_overtaken_ = 0;   // older births dropped (same tick) or re-ordered (earlier) behind a death
        bool resync_pending_ = false;
        // ---- step A3: the exact resync (resync design 2.6) ----
        // The pending resync is answered by a SessionResyncState of tick
        // resync_base_ - 1 (the assignment carried ASSIGN_FLAG_STATE_FOLLOWS):
        // the history is anchored there (the synthetic anchor), and the
        // state is applied as that tick and re-stepped to the live one.
        bool resync_exact_ = false;
        // Step EL7: an exact resync's restore is running (the ended-life
        // reconcile leaves its mirrors to the resync's own, 2.6.3).
        bool resync_restoring_ = false;
        uint32_t resync_base_ = 0;
        uint32_t resync_old_tick_ = 0;      // R: the old numbering's tick at the re-assign
        int64_t resync_delta_ = 0;          // delta = R - (base - 1)
        // The state, kept while the apply waits for the body set (2.6.4
        // step 2), and when it arrived.
        bool have_resync_state_ = false;
        bmmo::session_resync_state_msg resync_state_;
        uint32_t resync_state_arrived_ = 0;
        uint32_t resync_wait_frames_ = 0;
        uint64_t resync_states_ = 0, resync_states_stale_ = 0, resync_states_refused_ = 0;
        uint64_t resyncs_restepped_ = 0, resync_restep_depth_ = 0;
        uint64_t resync_deferred_ = 0, resync_deferred_ticks_ = 0;   // applies put off for the body set, and their frames
        uint64_t snapshots_while_resync_ = 0;
        uint64_t resync_scripts_refused_ = 0, resync_blobs_refused_ = 0;
        uint64_t resync_unexplained_ = 0, resync_unattributed_ = 0, resync_rows_unmapped_ = 0, resync_unportable_ = 0;
        uint64_t resync_destroyed_ = 0;
        // The resync hold (2.6.6): the world steps nothing while a pending
        // resync's history would lose its anchor.
        bool resync_held_ = false;
        // A hold that timed out is spent for that resync: the world runs on
        // (a new assignment, after the next trigger, holds again).
        bool resync_hold_spent_ = false;
        clock_type::time_point resync_hold_deadline_{}, resync_hold_started_{};
        uint64_t resync_holds_ = 0, resync_hold_timeouts_ = 0;
        double resync_hold_ms_max_ = 0.0;
        // Every remote lifecycle event this world applied, by applied tick
        // (a shared tick), the last history_ticks + 128 ticks: the
        // reconcile's evidence for a mirror born or dead after S (2.6.3).
        // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): the
        // evidence of the ended mirror lives' reconcile too.  `applied` is
        // the event's stamp (its applied tick from the server); `made_at`
        // the tick this world applied it (the stamp inside a replay or a
        // lifecycle rewind, the live tick otherwise; 0 until applied);
        // `birth` the Physicalize as it came (pose, recipe, ball type,
        // flags, origin); `entity` the mirror entity the event made or
        // removed and `generation` the one it gave that entity; `skipped`:
        // the event changed no body here (an overtaken birth dropped, a
        // superseded death, a death of no mirror).  Made before the event's
        // own rewind; the trim keeps each player's newest Physicalize older
        // than the horizon besides every newer entry (log_life).
        struct life_entry {
            uint32_t applied = 0, origin = 0, player = 0;
            bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
            uint32_t made_at = 0;
            pending_birth birth;
            std::string entity;
            uint32_t generation = 0;
            bool skipped = false;
        };
        std::deque<life_entry> life_log_;
        // Deaths re-applied inside a replay at their tick: a resync's (a
        // mirror made from its row at S whose Unphysicalize came after S)
        // and, since step EL7, the window deaths the reconcile of an ended
        // life queues (`reconciled`).  `origin` orders them among the
        // tick's births (drain_pending_lives: the server's order).
        struct pending_death {
            uint32_t player = 0, tick = 0;
            uint32_t origin = 0;
            bool reconciled = false;
        };
        std::vector<pending_death> pending_deaths_;
        // The re-step window (S, applied_at] of the last exact resync applied
        // (step A3 fix-up): a correction of a tick in it - a held notice for
        // an own tick, a relayed frame of a remote - amends a record that
        // re-step made, and its rewind may go as deep as the re-step itself
        // (resync_reach, not max_resim_ticks).  The server's substitutions
        // of the new epoch's first ticks come after the state, D = 51-53
        // ticks deep (a5u_lead), and each asked for the next resync; the
        // relayed frames of the window that come after it were dropped as
        // late and left the mirrors on their prediction.
        bool resync_window_ = false;
        uint32_t resync_window_anchor_ = 0, resync_window_end_ = 0;
        uint64_t resync_window_rewinds_ = 0;
        uint64_t resync_rewinds_subsumed_ = 0;   // pending corrections the apply's re-step covered
        uint64_t resync_amends_ = 0;   // own records amended while an exact resync was pending
        uint64_t resync_resent_ = 0;   // own frames of the old numbering played again (Delta > 0)
        // Step A5: the pending exact resync is a late joiner's (its first
        // assignment carried the flag), until its apply.  Its world before
        // the apply is the level start plus its catch-up passes, which the
        // server's state replaces: the wakeups those passes cause are no
        // event of the shared world, and are not reported (withheld).
        bool resync_join_ = false;
        uint64_t join_wakes_withheld_ = 0;
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
        // Session-core design 7.1 decision 1 (step B2SW, a core rule for both
        // hosts since): authoritative snapshots in a row whose tick the
        // history has no record of - a numbering desync - and the resyncs
        // that run asked for.  Reset by a matched snapshot and by every
        // resync request that goes out.
        int consecutive_unmatched_ = 0;
        uint64_t unmatched_resyncs_ = 0;
        clock_type::time_point last_resync_request_{};
        uint64_t resyncs_sent_ = 0, resyncs_done_ = 0, sessions_ended_ = 0;
        bmmo::session::rollback_engine rollback_;
        std::map<uint32_t, bmmo::session::input_frame> own_inputs_;
        // The event log's mute in force (session_client_core::event_log_mute;
        // moved from the headless client at E6).
        bool event_log_muted_ = false;
        // The --trace input edge (post_step step 21): the keys and the
        // navigation state of the last frame (moved from the headless client
        // at E9).
        uint8_t last_keys_ = 0;
        bool last_nav_active_ = false;
        // The black box (design 9.15) and its bookkeeping moved into
        // core::client_journal at E2 (session_client_core::journal()).

        // ---- R1 (design 4.1, the retail host's policies and readers) ----
        // (mirror_start_from_row's row cache was deleted at R3: with the
        // lifecycle rewind a late birth is made at its stamp in a replay.)
        // own_body_backstop: the last own Physicalize sent (what a re-report
        // carries but the pose), the snapshots in a row that named no body of
        // ours while ours was physicalized, the last re-report, and the count
        // (the retail mod's last_physicalize / snapshots_without_own /
        // physicalize_resends, PSC check_own_body).
        struct own_last_birth {
            bool valid = false;
            uint8_t ball_type = 0, flags = 0;
            bmmo::session::ball_recipe recipe;
        } own_last_birth_;
        uint32_t snapshots_without_own_ = 0;
        clock_type::time_point last_backstop_{};
        uint64_t own_backstops_ = 0;
        // (The snapshots' acked_input_tick for the retail starvation
        // detector went with the detector at R4: the relay header's `acked`
        // is read in note_held_section, with snapshots off too.)

        // ---- step T3: the trafo as an input (trafo-replication design) ----
        // SessionStart's world options, and whether this session runs the
        // trafo as an input here (WORLD_TRAFO_INPUTS and the host's
        // trafo_inputs capability).
        uint32_t world_options_ = 0;
        // Step Q7: SessionStart's mechanism_slots under WORLD_PLAYER_MECHANISMS, else 0.
        uint8_t mechanism_slots_ = 0;
        // Step RK3 (record-format-gaps-design.md 8 row K3 = C4): the world
        // steps since the anchor (-1 before it; a re-assign does not reset
        // it, unlike frames_since_anchor_), and whether the Level 1
        // tutorial's exit is still to be activated for this anchor.
        int64_t world_frames_ = -1;
        bool tutorial_exit_pending_ = false;
        // Step RK3: the live time factor after the previous frame, for the
        // log of its changes in the first frames after the anchor (the
        // session-start write, the tutorial's freeze and its exit).
        float globals_logged_factor_ = 0.0f;
        bool globals_logged_ = false;
        bool trafo_active_ = false;
        // Every applied or pending trafo input by applied tick, own, remote
        // and server-stamped (design 2.5): the live tick's applied before the
        // union pass, a replayed tick's re-applied in the replay.
        bmmo::session::trafo_input_log trafo_log_;
        // The sets (player, life) this world holds selected (design 2.4;
        // step TRL: keyed by life): a life without one gets make_trafo_set.
        std::set<bmmo::session::trafo_key> trafo_sets_;
        // Step TRL: the sets this world retired.  No sight (a member
        // message, an event, a relayed input, the anchor) makes one again:
        // a retired life is gone ('no resume'); only a resync ledger that
        // still names it does (the server's state at S holds it).
        std::set<bmmo::session::trafo_key> trafo_retired_sets_;
        // Their pieces' entities and sets (the host's trafo_type_set at the
        // make): a snapshot row of one restores it only on a breach
        // (rollback_world::row_on_breach_only).
        std::map<std::string, bmmo::session::trafo_key> trafo_pieces_;
        // The hold's reports made before the assignment: stamped on the
        // first assigned frame (design 2.3, hold_pending_trafo).
        std::vector<trafo_report> trafo_pending_;
        // This frame's pending input rewind carries a trafo input (its
        // refusal is a resync: a snapshot cannot make or reset pieces).
        bool input_rewind_trafo_ = false;
        // Counters (the status line's trafo: section): own inputs stamped,
        // remote and server-stamped inputs received, inputs applied live and
        // in replays, late inputs rewound, missed ones (a resync), late ones
        // the pending resync's state covers, the effect checks' leaks, own
        // echoes moved and dropped (REJECTED, SUPERSEDED), sets made and
        // failed, inputs that found no set, the reconcile's unmakes and
        // remakes, sets retired.
        uint64_t trafo_sent_ = 0, trafo_received_ = 0, trafo_applied_ = 0, trafo_replayed_ = 0;
        uint64_t trafo_late_ = 0, trafo_missed_ = 0, trafo_covered_ = 0, trafo_leaks_ = 0;
        uint64_t trafo_echo_moved_ = 0, trafo_echo_dropped_ = 0;
        uint64_t trafo_sets_made_ = 0, trafo_set_failures_ = 0, trafo_run_failures_ = 0;
        uint64_t trafo_unmade_ = 0, trafo_remade_ = 0, trafo_retired_ = 0;
        // Step TRL: sights of a retired set (not made again) and of a
        // player whose life this client cannot name (no set made).
        uint64_t trafo_retired_sights_ = 0, trafo_unnamed_lives_ = 0;
        uint32_t trafo_leak_logs_ = 0;
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
        // Test only (headless --accept-protocol N, step BALA): a SessionStart
        // of protocol N is taken as well as this build's - the replay of a
        // reference capture recorded before a protocol bump that changed
        // nothing the capture's inbound messages carry.  0: none.
        uint32_t accept_protocol = 0;
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
        // Design 1.2 feature switch (E7): act on the echoes of our own events
        // (move_own_birth, resync_own_life, expire_own_echoes and the
        // moved-Unphysicalize list).  Off, they are counted and journaled
        // only.  Acting needs `rollback` too, as before; headless sets it
        // from --no-rollback, so the pair is today's `rollback` alone.
        bool own_echoes = true;
        // Design 1.2 feature switches (E9).  `input_rewind`: a relayed frame
        // that differs from the prediction asks for a replay (the remote
        // half of note_input_correction; replay_corrected_inputs runs what
        // was asked for).  `own_corrections`: a held notice of our own
        // substituted tick amends the tick's record and asks for the own
        // replay (correct_own_tick, late-player part one).  Each needs
        // `rollback` too; headless sets both from --no-rollback, so with
        // them its behaviour is today's `rollback` alone.
        bool input_rewind = true;
        bool own_corrections = true;
        // Design 1.2 feature switch (R1): `lifecycle_rewind` - a remote birth
        // stamped in the past is made inside a replay at its tick
        // (make_remote_birth's pending path) and a late body change rewinds
        // (rewind_for_lifecycle).  Off, each is made or taken out at the
        // drain, at the live tick, with no rewind (both hosts set it on
        // since R3).  Needs `rollback` too; the headless client leaves it
        // on, so with it its behaviour is today's `rollback` alone.
        bool lifecycle_rewind = true;
        // Gap L, L2 (rollback-engine gaps design 2.3.3, L-H6's control arm;
        // step EL7): true = a mirror born inside a rewind's window keeps the
        // parking path (no step LB remake, and the reconcile of an ended
        // life does not take a later life out, so it revives only a player
        // with no body now).  Default false; headless
        // --remote-births-parked.
        bool remote_births_parked = false;
        // Step OWNB (SFD (A), the user's answer 2 of 2026-10-28): true = an
        // own birth the scripts made live inside a rewind's window keeps the
        // parking path.  Default false: with own body control, the body is
        // taken out at the restore and made again at its birth tick the way
        // the Physicalize made it (remake_own_body: the pose, recipe and
        // group it carried, the spawn kick of that tick - the server's clone
        // and every mirror are made so), so the replayed world's own ball
        // starts from a fresh body's collision bookkeeping instead of the
        // live body's beamed back to its birth pose.  Headless
        // --own-births-parked (the control arm).
        bool own_births_parked = false;
        // Retail policy (design 4.1; R1), off by default (the headless
        // client never sets it).  `own_body_backstop`: 30 snapshots in a row
        // without a body of ours while our ball is physicalized re-report its
        // last Physicalize (without the spawn flag, at most every 2 s; design
        // 7.1 decision 2: retail-only until R6); while the own echoes are
        // acted on, not while an own birth still waits for its echo (R3:
        // the echo or its deadline answers that one).  (The other R1 policy,
        // `mirror_start_from_row` - a mirror made live started from its
        // player's newest snapshot row, the 9.17 workaround - was deleted at
        // R3 with the retail lifecycle rewind on, design 4.2.)
        bool own_body_backstop = false;
        // Retail policy (R1, while input_rewind is off): a predicted mirror's
        // frame for the step of T is chosen at pre_step(T), from the relay
        // as the messages between the frames left it (the legacy retail
        // frame's order: the relay taken, then the next tick's drive), not
        // at post_step(T - 1) before them.  Without the input rewind nothing
        // corrects that tick's prediction but the next snapshot, so a key
        // edge relayed in between is played a tick late and every edge
        // costs one more mismatched snapshot (R1 (b): 22-29 against 17-21).
        bool drive_from_newest_relay = false;
        // The window: line's phase (E10; design 1.2): a line closes the
        // window on the ticks t with (t - window_shift) % kWindowTicks == 0
        // (headless --window-origin; 0 = lines at multiples of 60).
        uint32_t window_shift = 0;
        // Late-player plan step 12 (design 2.5): 0 = the per-client reach
        // (rollback.hpp reach_meter, from the relay lag); N = the reach
        // pinned at clamp(N, kReachFloorTicks, kReachCapTicks) for the whole
        // session (headless --reach: the open question 1 measurement, an A/B
        // arm).  Both hosts leave it 0.
        uint32_t reach_ticks = 0;
        // Step RC3 (rewind-cost design 3, roadmap 4.3): while the session's
        // snapshots are off (snapshots_run() false), the full snapshot the
        // server still broadcasts at a body-set change is no correction -
        // its names and rows go to the mechanism registry, its rows to the
        // journal, and the rollback engine never compares or restores from
        // it.  true = the pre-RC3 behaviour, the snapshot compared and a
        // breach rewound.  The default is false on every host (step B5RX,
        // the user, 2026-10-28: work as far as possible as if snapshots
        // did not exist): a snapshots-off session with rollback on (both
        // hosts' default) is snapshot-free, and the
        // drifts these snapshots used to correct (B5RC: a Ball_Stone mirror
        // 0.1 -> 21 m on resync_snapoff c1, the P_Modul_29 plates 0.03 ->
        // 1.4-1.8 m on l8_cross) are gaps to fix, not to hide.  true is the
        // measured control arm (headless --body-set-corrections).  Only the
        // rollback path reads it: with rollback off (headless --no-rollback,
        // retail 'session rollback off', diagnostic modes) the blend ladder
        // still sets the balls from every snapshot, as before.
        bool body_set_corrections = false;
    };

    // (legacy_calls, the transitional back-calls of design 6.1 "Linking while
    // the core is incomplete", was deleted at E11.  Its last two entries
    // became optional host hooks: own_body_guard_off is hooks.ended, and
    // describe_movable_objects is hooks.describe_movables.)

    // anchor (design 1.3): what the host's anchor work found.
    struct anchor_report {
        bmmo::physics::world_hash hash;   // the anchor's world hash (SessionReady, the journal header)
        std::string physics_sha256;       // SessionReady::physics_sha256 (headless: "headless-64" / "headless-32")
        std::string build_id;             // SessionReady::build_id and the journal header's
        std::string own_name;             // the journal's own player name
        int load_ticks = 0;               // the ticks the level took to load (the anchored log line)
        int sector_cell = 0;              // IngameParameter[0][1] at the anchor (E8: the core's sector_cell_)
        // Step S5 (sector placement design 2.4): the placement signature of
        // the host's world at the anchor (game/sector_placement.hpp
        // signature, placement on or off), read only with
        // session_capabilities::placement: SessionReady's anchor_placement
        // and the journal header's.
        uint64_t placement = 0;
        // Step G3 (world agreement 2.3, 5): the mechanism-class signature
        // of the host's proximity rewire (game/proximity_union.hpp
        // signature) when the session runs the union, else 0: SessionReady's
        // anchor_probes, which the server compares with its world's under
        // WORLD_PROXIMITY_UNION.
        uint64_t probes = 0;
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

    // A body's quaternion (x, y, z, w) as the rows of the 3x3 rotation a
    // Physicalize event carries (rotation[r * 3 + k] = row r), the formula of
    // VxQuaternion::ToMatrix (R1: the own_body_backstop's re-report; until
    // R3 also a mirror started from a snapshot row).
    inline void quaternion_rows(const double q[4], float rows[9]) {
        const float x = static_cast<float>(q[0]), y = static_cast<float>(q[1]);
        const float z = static_cast<float>(q[2]), w = static_cast<float>(q[3]);
        const float norm = x * x + y * y + z * z + w * w;
        if (norm < 1e-12f) {
            for (int i = 0; i < 9; ++i) rows[i] = i % 4 == 0 ? 1.0f : 0.0f;
            return;
        }
        const float s = 2.0f / norm;
        const float xs = x * s, ys = y * s, zs = z * s;
        const float wx = w * xs, wy = w * ys, wz = w * zs;
        const float xx = x * xs, xy = x * ys, xz = x * zs;
        const float yy = y * ys, yz = y * zs, zz = z * zs;
        rows[0] = 1.0f - (yy + zz); rows[1] = xy - wz;          rows[2] = xz + wy;
        rows[3] = xy + wz;          rows[4] = 1.0f - (xx + zz); rows[5] = yz - wx;
        rows[6] = xz - wy;          rows[7] = yz + wx;          rows[8] = 1.0f - (xx + yy);
    }

    // The unit tests' door to the private state and steps
    // (tests/session_client_core_test.cpp defines it; nothing else does).
    struct core_test_access;

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

    private:
        // The core's state (design 1.2).  Public from E1 to E10, while the
        // flow still lived in the host (core_.s.<member>); private since E11.
        client_core_state s;
        // The journal reads the state and the configuration (E2).
        friend class core::client_journal;
        friend struct core_test_access;

    public:
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
        // The player id the server gave this client at its login (headless:
        // LoginAcceptedV3, the entry with its own name); begin finds the
        // own join order by it (E11: the host wrote the state until then).
        // Set it before begin: begin without it finds no own join order
        // (-1, group P#63).  A host whose core does not exist at login
        // (retail, design 3.2: created at the first SessionStart) keeps the
        // id and calls this after it creates the core and before begin.
        void set_own_id(uint32_t id) { s.own_id_ = id; }
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
        // The anchor hold given up by the host (R1; design 1.3): the retail
        // host's blocking hold when the connection is lost.  Nothing while
        // not holding.  No headless path calls it.
        void release_hold(const char* why);
        // SessionEnd: drop the mirrors and go idle.
        void end(const std::string& reason);

        // ---- messages (E3: decoded by the host; on_message is not built) ----
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
        // SessionMember (protocol 18; world-agreement-gaps-design.md 2.8,
        // step M1): a life of another member added at J or removed at R.
        // Accepted in every phase after begin() of the same session - not
        // dropped while loading: nothing sends a lost member entry again.
        // A copy is ignored; a life re-announced with another join order
        // ends the session locally (no mirror group could be trusted).  The
        // life's parked events go to the drain; its PLAYER records go to the
        // journal at J and R.
        void on_session_member(const bmmo::session_member_msg& msg);
        // Step M1: every life this client was told of (tests, diagnostics).
        const std::vector<bmmo::session::member_life>& member_lives() const { return s.lives_; }
        uint64_t events_parked() const { return s.events_parked_; }
        uint64_t member_life_outside() const { return s.member_life_outside_; }
        uint64_t mirrors_outside_lives() const { return s.mirrors_outside_lives_; }
        // SessionResyncState (step A3, resync design 2.6.4): the server's
        // state of tick base - 1 for the pending exact resync; applied as
        // that tick and re-stepped to the live one now, or - its bodies not
        // all here yet - kept and tried again every frame.
        void on_session_resync_state(bmmo::session_resync_state_msg&& msg);
        // The resync hold (2.6.6), once per host loop pass before a step:
        // while a pending exact resync's history would lose its anchor with
        // the next step, the world must not step (`held`).  Bounded (2 s):
        // `timed_out` on the poll it gives up (logged, journaled).
        hold_state poll_resync_hold();
        bool resync_held() const { return s.resync_held_; }
        uint64_t resyncs_restepped() const { return s.resyncs_restepped_; }

        // ---- own reports from a host_hooks host (E7; design 1.3) ----
        // The own ball's Physicalize and Unphysicalize from a host_hooks
        // host's hooks, fired in the behaviour pass and stamped
        // current_tick() + 1: queued, and committed in order at post_step
        // step 19 (after the drain's generation bump), where the event log's
        // reports (report_physicalize) run.
        void own_physicalized(const own_birth_report& report);
        void own_unphysicalized(uint32_t tick);
        // Step SPGH2: the live own ball's Physicalize as a host saw it before
        // the block made the body (own_physicalized calls it; a host whose
        // own reports come from the event log calls it from its own hook).
        // The core decides, with the predicate commit_own_birth flags the
        // event SPAWN by (own_birth_is_spawn), whether it begins the own
        // spawn ghost life (physics_state.hpp spawn_ghost_birth), as the
        // server's copy's does at the same tick.
        void own_birth_seen(const own_birth_pose& pose, uint32_t tick);
        // The one spawn predicate of an own birth: the event's
        // PHYSICALIZE_FLAG_SPAWN, the spawn impulse's and the ghost life's.
        static bool own_birth_is_spawn(const own_birth_pose& pose) { return pose.captured && pose.spawn; }
        // A beam of the own ball to `position`, upright and at rest (the
        // headless --beam, the retail verb): made at post_step step 29 of
        // the frame that stepped T, stamped T + 1.
        void request_beam(const double position[3]);

        // ---- the tick (E9, client_core_frame.cpp; design 1.3) ----
        // The tick pre_step prepares: the tick base on the first frame
        // after the anchor or an assignment.
        uint32_t next_tick() const {
            return s.tick_base_ + static_cast<uint32_t>(std::max<int64_t>(s.frames_since_anchor_, 0));
        }
        // Prepare T = next_tick() (design 1.3 steps 1-12): the frame's
        // events, echoes and rewinds, the union's pass, the mirrors' and
        // the forced own input of T.  The host then steps the world once.
        void pre_step();
        // The world has stepped T (steps 13-32): T becomes the current tick,
        // and the own ball, the bodies, the input, the record, the sends,
        // the sector poll, the drive of T + 1, the beam, the amend and the
        // journal follow.  `stepped` false: the step failed - only the
        // forced own polling is restored (step 13), and false is returned
        // (the host ends the session).  A frame no pre_step prepared is
        // refused (R1: false, logged, nothing done): the retail anchor path
        // could skip one, the headless frame always pairs them.
        bool post_step(bool stepped);

        // ---- pacing (E9; the host owns the clock, design 1.3) ----
        // How far behind its schedule the host's pacer may fall before it
        // rebases: 33 frames plus what a slew still has to move.
        double rebase_threshold() const { return bmmo::session::lead_rebase_threshold(s.lead_slew_.outstanding()); }
        // The pacer rebased its schedule: a resync is asked for (the
        // limiter applies).
        void schedule_rebased(const char* why) { request_resync(why); }
        // One step of a SessionLead's slew at `now`: the tick periods since
        // the previous step (at `tick_rate`) and how far the pacer is behind
        // go to lead_slew::advance; the whole delta moved is acknowledged.
        // Returns the ticks the host moves its origin by (+1: a tick
        // earlier, the clock runs ahead).
        int slew_step(std::chrono::steady_clock::time_point now, double tick_rate, double behind_ticks);
        bool slewing() const { return s.lead_slew_.active(); }
        // The input basis (design 1.2): the last camera reference seen,
        // nullptr until one was.
        const float (*previous_cam() const)[3] { return s.previous_cam_valid_ ? s.previous_cam_ : nullptr; }

        // ---- sectors (E8, client_core_frame.cpp; design 1.3 steps 1, 9, 26) ----
        // A sector this client enters (the headless --start-sector, the
        // retail checkpoint hook and verb): before the assignment it waits
        // for the first assigned frame (pre_step step 1), since the anchor
        // precedes the renumbering; after it, stamped for the tick after
        // the current one plus the sector lead.  Once per sector; every
        // request made before the assignment waits, in order.  It always
        // holds: a host that reports a crossing after the fact instead (the
        // retail hook without the checkpoint hold, or before its
        // assignment) decides that before it calls (design 6.2).
        void request_sector(int sector, const char* why);
        // The lead the server sized for this session (SessionStart).
        uint32_t sector_lead_ticks() const { return s.sector_lead_; }
        // This session's union passes whose modules read CurrentLevel[0][4]
        // = 0 on their first frame (poll_own_sector's check; 1 is the only
        // right answer there).
        uint64_t phase_misses() const { return s.phase_misses_; }
        // Step B1 (resync design 4.6): the rewind in progress re-derived the
        // union inside its replay - the host's live-cells guard adopts the
        // replayed cells instead of writing the live ones back.
        bool last_rewind_rederived() const { return s.last_rewind_rederived_; }
        uint64_t sector_rewinds() const { return s.sector_rewinds_; }
        // The tick this client stamped its own entry into `sector` for (the
        // stamp log), false when it has not (the headless --resync-at s<K>+D).
        bool own_sector_stamp(int sector, uint32_t& tick) const {
            const auto it = s.sector_stamps_.find({s.own_id_, sector});
            if (it == s.sector_stamps_.end()) return false;
            tick = it->second.tick;
            return true;
        }

        // ---- rewinds asked for by the host (E5; design 1.3) ----
        // A forced rollback `depth` ticks deep from the current tick (the
        // headless --force-rollback, the retail forced-rollback verb).  The
        // live cells are kept (keep_live_cells) for as long as the caller
        // keeps the result: the host's traces after the rewind still run
        // inside that scope, as they did when the whole of it was the host's.
        // Without rollback, or before `depth + 2` ticks are played, it does
        // nothing (not rolled, not counted, no cells kept).  A depth above
        // max_resim_ticks re-steps up to the resync reach (history_ticks -
        // 1), the depth an exact resync re-steps (step A4, resync design 9
        // Q2); deeper than the history it is a counted miss.
        struct forced_rewind {
            bool rolled = false;
            scope_guard keep_cells;
        };
        forced_rewind force_rewind(uint32_t depth);

        // ---- numbering and resync ----
        // `owed`: the own correction needs this resync; one the limiter
        // holds back is asked again by the frame (retry_owed_resync).
        void request_resync(const char* reason, bool owed = false);

        // ---- sending ----
        // An own lifecycle event: stamped, counted, journaled, then on the
        // wire unless hooks.filter_event keeps it.
        void send_event(bmmo::session_event_msg& event);
        // A held send (hooks.filter_event) goes out now.
        void release_event(const std::string& wire_bytes) { put_event_on_wire(wire_bytes); }

        // ---- reading ----
        bool assigned() const { return s.assigned_; }
        bool holding() const { return s.held_; }
        bool resync_pending() const { return s.resync_pending_; }
        // What the headless host reads of the state (E11; it read the
        // members themselves until then): the session's phase, id, level,
        // seed and own player parameters (its loop, its anchor and its
        // PreSimulate capture of an own spawn), the frames since the anchor
        // (the record's frame index and the pacer), the own body as the
        // last frame saw it (--keep-lane and the traces), the mirrors (the
        // --trace step lines), the own ball's legacy corrections (the exit
        // code) and the rollback records (--dump-savestate,
        // --compare-scripts).
        uint32_t own_id() const { return s.own_id_; }
        client_core_state::phase session_phase() const { return s.phase_; }
        uint32_t session_id() const { return s.session_; }
        int level() const { return s.level_; }
        int32_t seed() const { return s.seed_; }
        int own_join_order() const { return s.own_join_order_; }
        const std::string& own_group() const { return s.own_group_; }
        float spawn_impulse() const { return s.spawn_impulse_; }
        float spawn_spacing() const { return s.spawn_spacing_; }
        int64_t frames_since_anchor() const { return s.frames_since_anchor_; }
        bool own_physicalized() const { return s.own_physicalized_; }
        const std::string& own_body_entity() const { return s.own_body_entity_; }
        // A ball, known by what it is (step P): an entity the own ball has
        // been this session, or a remote player's mirror.  Not a "Ball_"
        // name: the trafo explosion pieces (Ball_*Pieces_Frame's children)
        // share the prefix and are tracked, restored and journaled as the
        // mechanisms they are.
        bool is_ball(const std::string& name) const {
            if (name.empty()) return false;
            if (s.own_ball_entities_.count(name)) return true;
            for (const auto& [id, remote]: s.remotes_)
                if (remote.entity == name) return true;
            return false;
        }
        const std::map<uint32_t, client_core_state::remote_ball>& remotes() const { return s.remotes_; }
        const bmmo::session::correction_stats& own_correction_stats() const { return s.corrector_.stats(); }
        const std::vector<uint8_t>* recorded_savestate(uint32_t tick) const { return s.rollback_.recorded_savestate(tick); }
        const std::vector<uint8_t>* recorded_scripts(uint32_t tick) const { return s.rollback_.recorded_scripts(tick); }
        // A body's state in the record of `tick` (diagnostics: the retail
        // host's mirror-birth log, R3).
        bool recorded_body_state(const std::string& entity, uint32_t tick, bmmo_physics_body_state& out) const {
            return s.rollback_.recorded_body_state(entity, tick, out);
        }
        // What the retail host reads (R1): the own_body_backstop's
        // re-reports and the resync requests sent.
        uint64_t own_body_backstops() const { return s.own_backstops_; }
        uint64_t resyncs_sent() const { return s.resyncs_sent_; }
        // Session-core design 7.1 decision 1 (step B2SW): the resyncs asked
        // for by kUnmatchedSnapshotRun snapshots in a row with no record of
        // their tick.
        static constexpr int kUnmatchedSnapshotRun = 30;
        uint64_t unmatched_resyncs() const { return s.unmatched_resyncs_; }
        int unmatched_run() const { return s.consecutive_unmatched_; }
        // Step T3: this session runs the trafo as an input (the host's sets,
        // the hold, the quiesce at its anchor); the counters the gates read.
        bool trafo_active() const { return s.trafo_active_; }
        // SessionStart's session::WORLD_* bits (step S5: the host builds the
        // placement copies at its anchor when WORLD_PLACEMENT_CLONES is set
        // and it declares session_capabilities::placement).
        uint32_t world_options() const { return s.world_options_; }
        // Step S6 (sector placement design 2.4, the core's part): whether
        // this host builds the placement copies at its anchor - the session's
        // world option and the host's capability.  Without it the host still
        // reports the signature of its unplaced table (and releases copies a
        // level still holds, design 2.4 "Option off").
        bool placement_active() const {
            return caps_.placement && (s.world_options_ & WORLD_PLACEMENT_CLONES) != 0;
        }
        // Step G3 (world agreement 2.4, 2.5): whether this host rewires the
        // mechanism proximity blocks at its anchor and the core places them
        // before every live and replayed tick - the session's world option
        // WORLD_PROXIMITY_UNION and a platform with place_probes.  A host
        // without the operation sends no signature; the server refuses it.
        bool proximity_union_active() const {
            return (s.world_options_ & WORLD_PROXIMITY_UNION) != 0 && static_cast<bool>(platform_.place_probes);
        }
        // Step Q7 (world-agreement-gaps-design.md 2.10): the number of slots
        // the host copies the per-player mechanism units into at its anchor
        // (proximity_union's rewire_options::slots) - SessionStart's K under
        // WORLD_PLAYER_MECHANISMS while the union is active, else 0 (no
        // copies: G3's single-ball rules).
        uint8_t mechanism_slots() const {
            return (s.world_options_ & WORLD_PLAYER_MECHANISMS) && proximity_union_active() ? s.mechanism_slots_ : 0;
        }
        // Step RK3 (record-format-gaps-design.md 8 row K3 = C4, option (c)):
        // whether this session switches the Level 1 tutorial off - the
        // world option WORLD_SESSION_CLOCK on Level 1.  The host finds the
        // tutorial's exit at its anchor when it is set; the core activates it
        // (platform tutorial_exit) before frame
        // SESSION_CLOCK_TUTORIAL_EXIT_FRAME.
        bool tutorial_off() const {
            return (s.world_options_ & WORLD_SESSION_CLOCK) != 0 && s.level_ == SESSION_CLOCK_TUTORIAL_LEVEL;
        }
        uint64_t probe_two_bodies() const { return s.probe_two_bodies_; }
        uint64_t revived_local() const { return s.revived_local_; }
        uint64_t trafo_missed() const { return s.trafo_missed_; }
        uint64_t trafo_sent() const { return s.trafo_sent_; }
        uint64_t trafo_original_leaks() const { return s.trafo_leaks_; }
        const bmmo::session::trafo_input_log& trafo_log() const { return s.trafo_log_; }
        // Step TRL: the sets this world holds selected, "p,pLn,..." in
        // (player, life) order (the anchor's, the reconcile's and the status
        // line's logs).
        std::string trafo_set_list() const;

        // ---- reports (E10, client_core_report.cpp; design 1.3) ----
        // The status line (the headless 5 s report and the session's end;
        // the retail F3 overlay), with the host's counters at their places.
        std::string status_text(const host_counters& h) const;
        // The window: line that `tick` closes (post_step step 31), "" when
        // it closes none - and on the first window edge, which only sets the
        // base.  Every call at a new window index moves the base, so the
        // frame is its only caller: a host gets the line from hooks.window
        // and never calls this (an extra call would take the window's counts
        // out early).  Public for the tests.
        std::string window_text(uint32_t tick, const host_counters& h);
        // Whether window_text(tick) would close a window (or set the first
        // base): the frame reads the host's counters only then.
        bool window_due(uint32_t tick) const { return s.window_index_ != window_index_of(tick); }

        // The core's log line: printf-formatted into a growing buffer and
        // handed to platform().log as one line (design 6.1).  GCC and Clang
        // check the format against its arguments (argument 1 is this).
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        void logf(const char* format, ...);

    private:
        // ---- the frame's own steps (private since E11: pre_step and
        // post_step call them; E5-E9 left them public for the host's frame
        // and the unit tests) ----

        // The remote lifecycle (E6; pre_step steps 2 and 8, post_step steps
        // 16 and 28, and the binding's before_replayed_tick).
        // Every queued event due at `tick`, in the server's order; a body
        // change stamped for `tick` itself is held for apply_held_lifecycle.
        void drain_due_events(uint32_t tick);
        // The held body changes, after the frame's rewinds and before the
        // union's pass, with our own changes due at the tick at our place.
        void apply_held_lifecycle(uint32_t frame_tick);
        // Before a replayed tick (the binding's before_replayed_tick): the
        // remote births and deaths deferred into the replay (the window
        // events the reconcile queued, a late birth's, a resync's deaths),
        // applied at their tick in the server's order with our own lives due
        // at the tick at our place (step EL7: was drain_pending_births).
        void drain_pending_lives(uint32_t tick);
        // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): the core's
        // half of rollback_world::restore_lives for the mirrors - each mirror
        // life alive at `anchor` that ends inside the window is made again
        // from its life log entry at the anchor record's pose (no kick), the
        // player's later lives in the window taken out, and every window
        // event of that player queued at its stamp; answered per name.
        // Then the other players whose window LB cannot replay (a life born
        // and ended in it, two births, a body made after a stamp in it):
        // the body now out, every window event queued (review finding 1).
        void reconcile_remote_lives(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended,
                                    std::map<std::string, life_outcome>& out);
        // Gap L, L3 (rollback-engine gaps design 2.3.4, step EL8): the own
        // half - an own life alive at `anchor` that ended inside the window
        // is made again at the A record's pose (make_own, muted, with the
        // replica) and its logged death queued at D; a later life of the same
        // name, and an own life born and ended inside the window, are taken
        // out and remade at their stamp.  Only with own_body_in_replay.
        void reconcile_own_lives(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended,
                                 std::map<std::string, life_outcome>& out);
        // The own changes reconcile_own_lives queued that no replay applied:
        // made live after the rewind (olives=.../settled).
        void settle_own_due();
        // The core's scope around every rewind (step EL8): the host's
        // keep_live_cells, and the CK matrices of the own entities without a
        // body put back to their pre-rewind values at the end (design 2.3.4
        // "At the replay's end", 9.7).
        scope_guard keep_live_cells();
        // After a rewind: the reconcile's queued events no replay applied
        // (the replay stopped short), made live in stamp order.
        void settle_unreplayed_lives(uint32_t now);
        // The life log entry of a remote event that was just applied here:
        // the entity it made or removed, its generation, where it was made.
        void note_life_made(uint32_t player, bmmo::session::event_type type, uint32_t stamp, uint32_t origin,
                            const std::string& entity, uint32_t made_at);
        void note_life_skipped(uint32_t player, bmmo::session::event_type type, uint32_t stamp, uint32_t origin);
        // The player whose mirror `entity` is or was (the life log), 0 none.
        uint32_t mirror_player_of(const std::string& entity) const;
        // A physicalized mirror without navigation is driven from now on, if
        // the host has the navigation (post_step step 16, and every birth).
        void attach_remote_navigation(uint32_t player, client_core_state::remote_ball& remote, bool quiet = false);
        // A restore's destroy_body for a mirror (core_world_hooks::
        // remake_mirror_birth, step LB): the mirror born at `birth_tick` is
        // taken out (body and navigation) and its birth queued again, so the
        // replay makes it at that tick the way it was made first
        // (drain_pending_lives).  False, and nothing done, when `entity` is
        // not a mirror whose birth at that tick is known, or without the
        // lifecycle rewind.
        bool remake_mirror_birth(const std::string& entity, uint32_t birth_tick);
        // The mirror's current body is one remake_mirror_birth makes again
        // at its birth tick (made at its stamp, navigated from it, the
        // lifecycle rewind on).
        bool remakes_birth(const client_core_state::remote_ball& remote) const;
        // A mirror taken out for a remake that no replay made again (the
        // replay stopped before its birth tick): made now, live.
        void make_unmade_remakes();
        // The frame a live step of `tick` drives a predicted mirror with
        // (post_step step 28).
        static bmmo::session::input_frame live_drive(const client_core_state::remote_ball& remote, uint32_t tick);

        // Own lives and echoes (E7, client_core_lifecycle.cpp).
        // The echoes of our own events that arrived since the last frame
        // (pre_step step 3): the stale ones, the rest, then the births whose
        // echo is overdue.
        void process_own_echoes();
        // The own ball's Physicalize the event log reported for `tick`
        // (own_lifecycle_source::event_log; post_step step 19): the host's
        // capture (own_birth_capture) and recipe (ball_recipe_of), sent and
        // recorded.  And its Unphysicalize, sent.
        void report_physicalize(const std::string& entity, uint32_t tick);
        void report_unphysicalize(uint32_t tick);
        // The host_hooks reports (own_physicalized, own_unphysicalized),
        // committed in order at step 19.
        void commit_own_reports();
        // post_step step 29: the requested beam, made after the step of
        // `tick` and stamped tick + 1.
        void apply_requested_beam(uint32_t tick);
        // Step BALA: after each live frame - the own ball's and every
        // mirror's place in the airship zone, and the ghosts handed to the
        // host's display (step BALL: the legacy ball stream moves them).
        void airship_frame(uint32_t tick);
        // Step SPGH: the spawn ghosts' bookkeeping and display after each
        // live frame (the rule itself is the physics layer's).
        void spawn_ghost_frame(uint32_t tick);
        // Step SPGH2: a spawn birth of `group` marked in this world's rule
        // (platform spawn_ghost_birth), when the rule is on.
        void note_spawn_birth(const std::string& group, uint32_t tick, const char* where);
        // Whether `tick` is at or after the tick the own ball left the sync.
        bool own_out_of_sync_at(uint32_t tick) const {
            return s.own_airship_tick_ != 0 && static_cast<int32_t>(tick - s.own_airship_tick_) >= 0;
        }
        // A restore's destroy_body (core_world_hooks::destroy_own_birth): a
        // moved own birth whose body the replay makes again at its applied
        // tick is taken out here.  True when `entity` at `birth_tick` is one
        // (`destroyed`: taken out); false, and nothing done, otherwise and
        // always without own_body_control (design 2.3: the branch is inert).
        bool destroy_moved_own_birth(const std::string& entity, uint32_t birth_tick, bool& destroyed);

        // The sectors (E8, client_core_frame.cpp).
        // pre_step step 1: the requests waiting for the assignment, stamped
        // for `tick` (the tick about to be stepped) plus the lead, once
        // assigned.
        void hold_pending_sector(uint32_t tick);
        // pre_step step 9: one pass of the sector union before the step of
        // `tick` (session/sector_union.hpp), its cell write the core's.
        void sector_union_pass(uint32_t tick);
        // post_step step 26: the own sector poll after the step of `tick` -
        // a crossing stamped ahead (the checkpoint chain held) or reported
        // after the fact.
        void poll_own_sector(uint32_t tick);
        // Step B1 (resync design 2.5, 2.7, 2.8).  A stamp into the stamp log
        // (`rewrite`: a MOVED own echo's applied tick over the entry).
        void note_sector_stamp(uint32_t player, int sector, uint32_t tick, bool rewrite = false);
        // The stamp log as the union's stamps.
        std::vector<bmmo::session::sector_union::stamped> stamp_log() const;
        // A Sector stamp for `tick` can be re-derived: the record and the
        // union record of tick - 1 exist and the rewind is inside the reach.
        bool sector_rewind_reachable(uint32_t tick) const;
        // A MOVED own Sector echo: the own pass moved to the server's tick.
        // False when this world has no stamp of it at `origin` (kept where
        // stamped, as before B1).
        bool move_own_sector(int sector, uint32_t origin, uint32_t applied);
        // pre_step: the frame's late set, one rewind to the lowest tick - 1
        // with the union re-derived inside the replay (or, refused, every
        // stamp live and a resync).
        void rewind_for_sectors();
        // The core's before_replayed_tick half of the re-derivation: the
        // union's tick for the replayed `tick`, its pass recorded for the
        // host's re-issue (set_union_pass) or a live pass forgotten.
        void rederive_union_tick(uint32_t tick);
        // A union pass this world ran at `tick` (live or re-derived) into
        // pass_log_, trimmed.
        void log_pass(uint32_t tick, int sector);

        // The frame's rewinds (E5; pre_step steps 5 and 6).
        // One replay for every input correction the frame collected (the
        // relay's and our own substituted frames').
        void replay_corrected_inputs();
        void retry_owed_resync();
        // Every rewind the core asks of the engine outside a snapshot
        // (inputs, lifecycle events, sector stamps, the forced one):
        // rollback_engine::rewind_to, then the frozen clock's re-anchor
        // (design 4.1, 7 Q1).  A frozen rewind sets the bodies and drops the
        // history with no replay and still returns true; with the
        // session's snapshots off (step RC3, B5RX) on_snapshot's check no
        // longer runs, so this is where the re-anchor is asked for.
        bool rewind_world(uint32_t anchor, uint32_t now,
                          const std::function<bool(const std::string& entity, uint32_t tick,
                                                   bmmo::session::input_frame& out)>& input_at,
                          const std::string& reason, uint32_t reach = 0);

        // Part two's forced own frames (E5; design 1.3 steps 11 and 21).
        // The forced frame of `tick` (a SessionLead's (c, E)), taken: the
        // older ones are dropped.  False: `tick` is not forced.
        bool take_forced_own(uint32_t tick, client_core_state::forced_own& out);
        // A forced tick plays the server's frame: `input` becomes it, counted
        // as played, and as a received notice when no notice named it first.
        void play_forced_own(uint32_t tick, const client_core_state::forced_own& forced,
                             bmmo::session::input_frame& input);

        // Part one (E5; post_step step 24).
        // After the live frame of `tick` is recorded: a notice that named it
        // earlier is applied now.
        void take_held_ahead(uint32_t tick);
        // The SessionLead slew ends: acknowledged with what it moved (all of
        // it when `reanchored`, a resync request ended it).  slew_step calls
        // it when the slew is done.
        void finish_lead_slew(const std::string& why, bool reanchored = false);
        // Another rewind from `anchor` carried a waiting own-only replay
        // (any anchor when `server_rows`): dropped instead of run.
        void drop_carried_own_replay(uint32_t anchor, bool server_rows);
        // The newest relay header is of our own epoch (the status line).
        bool held_header_current() const {
            return s.held_header_known_ && s.held_header_epoch_ == s.tick_base_;
        }

        // Sending (E3).
        // The input history and the event repeats due, once assigned.
        void flush_inputs();
        // The sent ring: the frames an uplink datagram carried, by tick.
        void remember_sent(const bmmo::session_input_msg& msg);
        // The reliable copy, and the unreliable repeats queued.
        void put_event_on_wire(const std::string& bytes);

        // The reports' printf formatting into a growing buffer (logf without
        // the log; E10).
#if defined(__GNUC__)
        __attribute__((format(printf, 1, 2)))
#endif
        static std::string textf(const char* format, ...);
        // The host's counters (platform counters), zeros without them.
        host_counters host_values() const;
        // The window index of `tick`: a line closes the window on the first
        // frame of the next index (the ticks t with (t - shift) % 60 == 0).
        uint32_t window_index_of(uint32_t tick) const {
            return (tick + client_core_state::kWindowTicks - config.window_shift) / client_core_state::kWindowTicks;
        }
        // The 99th percentile of the relayed frames' arrival lag (the status
        // line's lag=).
        uint32_t relay_lag_p99() const;

        // A sector this client enters, as an input: stamped for `due`, sent,
        // and handed to the union (E8).  Once per sector per session.
        void hold_sector(int sector, uint32_t due, const char* why);

        // The frame (E9, client_core_frame.cpp).  The input a navigated
        // ball had at `tick`, for a re-simulation: the own ball's exact
        // record (told by the own replica's name, platform
        // own_replica_name), a remote's relayed frame.
        bool recorded_input_at(const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) const;
        // The host's world with the core's half of the binding (bind_world).
        rollback_world world() const { return platform_.bind_world(world_hooks_); }
        // One step of a legacy blend in flight (post_step step 27).
        void apply_blend(const std::string& name, bmmo::session::body_corrector& corrector);
        // What pre_step leaves for post_step: the frame number and tick it
        // prepared, and the forced own frame of that tick (step 11) with the
        // entity the host forced.
        // `prepared`: a pre_step ran and no post_step has taken its frame
        // yet (R1: post_step refuses a frame without one; begin clears it).
        struct frame_scratch {
            bool prepared = false;
            int64_t f = 0;
            uint32_t tick = 0;
            bool forced = false;
            client_core_state::forced_own forced_frame;
            std::string forced_entity;
        } frame_;
        core_world_hooks world_hooks_;

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

        // The exact resync (step A3, client_core_resync.cpp).
        // At the re-assign: the synthetic anchor of base - 1 (2.6.1).
        void anchor_resync(uint32_t base, uint32_t old_tick);
        // The apply (2.6.4), from on_session_resync_state and every pre_step
        // while a state waits; true when it ran (applied or refused for good).
        bool try_apply_resync(const char* why);
        // The tracked set of a record made now (post_step's, for the
        // synthetic anchor).
        bmmo::session::rollback_tracked tracked_now(bool physicalized) const;
        // A remote lifecycle event applied (life_log_).
        void log_life(const bmmo::session_event_msg& event);
        // Before a replayed tick: the deaths a resync re-applies there.
        void drain_pending_deaths(uint32_t tick);
        // Step G3 (world agreement 2.2, 2.5): the proximity union's
        // candidates of this world at the place point - the own ball's
        // entity with a body and every mirror entity with a body, one per
        // player, by player id, minus the bodies `held_out` names (a
        // replay's); and the place itself (the platform's place_probes).
        // Step Q7: each candidate's `slot` is the join order of the life
        // its player has at `tick` (the placed tick), -1 when none.
        std::vector<bmmo::session::probe_candidate> probe_candidates(
            const std::function<bool(const std::string& entity)>& held_out, uint32_t tick = 0);
        void place_probes(uint32_t tick, const std::function<bool(const std::string& entity)>& held_out, bool replayed);
        // How deep the correction rewind of `tick` (its anchor tick - 1) may
        // go: max_resim_ticks, or the resync reach inside the last exact
        // resync's re-step window.
        uint32_t window_rewind_reach(uint32_t tick) const;
        // Late-player plan step 12: one relay lag sample (the header of a
        // datagram's first arrival of a server tick) and the reach it gives
        // (config.reach_ticks 0), or the pinned reach (apply_reach at begin).
        void sample_reach(uint32_t lag);
        void apply_reach(uint32_t reach, const char* why);
        // A held notice's tick whose record a pending exact resync's
        // re-step replays (client_core_held.cpp), and its amendment.
        bool resync_amends(uint32_t tick) const;
        void amend_for_resync(uint32_t tick, const bmmo::session::input_frame& frame);

        // The remote lifecycle (E6, client_core_lifecycle.cpp).
        // Step M1: a member's place in the server's order of a tick's events
        // - the join order of its life that `tick` names (256: none).
        uint32_t join_rank(uint32_t player, uint32_t tick) const;
        // The join order a mirror of `player` made for `tick` is grouped and
        // kicked with: its life's; 63 (counted, mirrors_outside_lives) for a
        // player with no life - unreachable since events are parked.
        int mirror_join_order(uint32_t player, uint32_t tick);
        // The PLAYER records of the lives not journalled yet (a life's add
        // waits for its id's older lives to close, so a rejoin's PLAYER(-)
        // precedes its PLAYER(+) at one tick in file order).
        void journal_member_lives();
        void apply_event(const bmmo::session_event_msg& event);
        void make_remote_birth(const bmmo::session_event_msg& event);
        void rewind_for_lifecycle(const bmmo::session_event_msg& event);
        void rewind_for_lifecycle(uint32_t player, uint32_t event_tick);
        bool unphysicalize_remote(uint32_t player, uint32_t tick);
        uint32_t live_first_tick(uint32_t stamp) const;
        void unphysicalize_mirror(const std::string& entity);
        // True when the mirror's body was made.
        bool physicalize_remote(const client_core_state::pending_birth& event, uint32_t first_tick);
        // The snapshot path's retail policy (R1): own_body_backstop.
        void check_own_body(const bmmo::session_snapshot_msg& snapshot);
        // The mechanisms a record tracks (step P): the platform's movable
        // bodies but the balls (is_ball), in the platform's order.
        std::vector<std::string> movable_mechanisms() const;

        // The trafo as an input (step T3, client_core_trafo.cpp;
        // trafo-replication design 2.3-2.9, 4.6-4.8).
        // The set of (player, life) on first sight (make_trafo_set),
        // outside any replay; true when the world holds one.  A set this
        // world retired is made again only `from_ledger` (step TRL).
        bool ensure_trafo_set(uint32_t player, uint16_t life, const char* why, bool from_ledger = false);
        // Step TRL: the life of `player` a sight at `tick` names - the
        // member life covering it (member_life_at), 0 when none is known.
        uint16_t trafo_life_at(uint32_t player, uint32_t tick) const;
        // The own player's life (SessionStart's own entry; 1 before
        // protocol 18).
        uint16_t own_trafo_life() const { return s.own_member_life_ ? s.own_member_life_ : uint16_t{1}; }
        // The hold's report of an own activation, stamped for `due` (sent,
        // logged as an own input); before the assignment it waits.
        void hold_trafo(const trafo_report& report, uint32_t due, const char* why);
        // pre_step step 1: the reports that waited for the assignment,
        // stamped for `tick` + the lead.
        void hold_pending_trafo(uint32_t tick);
        // post_step: the hold and the effect checks after the live frame.
        void poll_own_trafo(uint32_t tick);
        // A remote or server-stamped Trafo event on arrival: logged at its
        // applied tick, rewound for when late.
        void on_trafo_event(const bmmo::session_event_msg& msg);
        // The inputs logged for `tick`, applied (live: pre_step; replayed:
        // before_replayed_tick).
        void apply_trafo_inputs(uint32_t tick, bool replayed);
        // A trafo input applied at `tick` <= the current tick: the pending
        // input rewind from it, or a resync past the reach (design 4.7).
        void trafo_rewind(uint32_t tick, uint32_t player, uint16_t life, const char* why);
        // An own Trafo event's echo (design 4.6).
        void process_trafo_echo(const bmmo::session_event_msg& msg, bool stale);
        // The reconcile (design 4.7), at a resync's apply before its
        // restore: every type set made to hold what the server's ledger has
        // at S; `gone` gets the pieces the server has no body for.
        void reconcile_trafo(const bmmo::session_resync_state_msg& st, uint32_t anchor, std::set<std::string>& gone,
                             std::vector<journal_trafo_entry>& journal);
        // post_step: retirement and the log's pruning.
        void retire_trafo(uint32_t tick);

        // The event log muted for a scope, nesting (platform mute_event_log;
        // the flag in force is client_core_state::event_log_muted_).  Private
        // since E7: the own body's re-make and removal moved into the core.
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

        // Own lives and echoes (E7, client_core_lifecycle.cpp).
        using own_life = client_core_state::own_life;
        bool acting_on_echoes() const { return config.rollback && config.own_echoes; }
        // SessionStart's interval byte: the server clamps every interval of
        // 255 or more to 255 (server.cpp:942), and 255 is treated as off -
        // a snapshot at most every 255 ticks reconciles nothing in time, so
        // a moved birth without own body control resyncs instead (the safe
        // side: one extra resync).
        bool snapshots_run() const { return s.snapshot_interval_ < 255; }
        void commit_own_birth(const own_birth_report& report);
        void beam_own_ball(uint32_t tick);
        bool write_beam(const std::string& name, const double position[3], uint32_t tick);
        void record_own_birth(const bmmo::session_event_msg& event, const std::string& entity);
        // `tick` and `live` (step EL8, design 2.3.4): a replayed tick tests
        // the generation of the live record of tick - 1, a live frame the
        // live generation.
        bool own_life_current(const own_life& life, uint32_t tick, bool live) const;
        // Step EL8: an own entity's body gone live (the drain), the matrices
        // of the bodiless own entities after a live frame, written before a
        // replayed tick, and the queued own changes of a replayed tick.
        void note_own_death(const std::string& entity, uint32_t tick, uint32_t generation);
        void record_own_matrices(uint32_t tick);
        void write_own_matrices(uint32_t tick);
        void apply_own_due(uint32_t tick);
        void retire_own_deaths();
        void clear_own_window_state();
        bool unmake_own_body(const own_life& life, uint32_t tick, const char* where);
        bool remake_own_body(const own_life& life, uint32_t tick, const char* where);
        void apply_own_lives(uint32_t tick, const char* where, bool live);
        bool own_lives_due(uint32_t tick) const;
        void live_own_lives(uint32_t frame_tick);
        void retire_own_lives();
        void rebase_own_lives();
        void set_aside_unanswered(own_life::aside why);
        own_life* find_unanswered(uint32_t origin, uint8_t ball_type);
        const own_life* moved_own_life(const std::string& entity, uint32_t birth_tick) const;
        const own_life* destroyable_own_birth(const std::string& entity, uint32_t birth_tick) const;
        const own_life* scripted_own_life(const std::string& entity, uint32_t birth_tick) const;   // step OWNB
        void process_own_echo(const bmmo::session_event_msg& msg, bool stale);
        void answer_set_aside(own_life& entry, bool made, bool superseded, uint32_t applied, const char* verdict);
        void expire_own_echoes();
        void resync_own_life(own_life& life, bool made, uint32_t applied, const std::string& why);
        void re_report_own_birth(const own_life& life, uint32_t now);
        void move_own_birth(own_life& life, uint32_t applied, bool made);

        session_platform platform_;
        session_capabilities caps_;
        // Last, so it is destroyed first (the file closes before the state
        // it describes goes away).
        core::client_journal journal_{*this};
    };

}
