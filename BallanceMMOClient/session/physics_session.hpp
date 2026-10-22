#pragma once

// Client-side state of a physics session (design section 8.5).  The logic
// lives in session/physics_session_client.cpp as BallanceMMOClient methods
// (like room_client.cpp); this header only holds the data so the mod's main
// header stays readable.
//
// Timeline: the anchor frame (first OnProcess with Gameplay_Ingame active
// after the restart) is frame 0; OnProcess of frame f >= 1 represents session
// tick tick_base + f - 1, where tick_base is the number the server assigns
// (session start: the session's start lead; late join and resync: the server's
// current tick plus the same lead).  A nonzero base renumbers the client
// through rebase_tick - the frames stamped under the anchor-relative numbering
// before the assignment are dropped, not relabelled.

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <entity/session.hpp>
#include <game/navigation_graph.hpp>
#include <message/message_all.hpp>
#include <session/correction.hpp>
#include <session/input_relay.hpp>
#include <session/lifecycle.hpp>
#include <session/mechanism_tracking.hpp>
#include <session/rollback.hpp>
#include <session/sector_union.hpp>
#include <game/script_replay.hpp>

namespace bmmo::session {
    struct physics_session_state {
        // counting_down: the "3 - 2 - 1 - Go!" lead-in, before the restart that
        // opens the session; the level is still the one the player was on.
        enum class phase_type { idle, counting_down, restarting, running, ended };
        phase_type phase = phase_type::idle;

        // From SessionStart.
        uint32_t session = 0, room = 0;
        uint8_t snapshot_interval = 2, input_delay = 6;
        int32_t seed = 1;
        bmmo::map map{};
        std::vector<player_entry> players;
        int own_join_order = -1;
        float spawn_position[3] = {};
        float spawn_rotation[4] = {};
        bool spawn_known = false;
        float spawn_impulse = 0.0f;          // design 9.10: kick speed at the resetpoint, 0 = none
        float spawn_spacing = 0.0f;          // birth cell spacing around the resetpoint (session/spawn_impulse.hpp), 0 = none

        // Restart / anchor detection.
        bool saw_ingame_inactive = false;
        std::chrono::steady_clock::time_point restart_deadline{};
        bool anchored = false;
        int64_t frames_since_anchor = -1;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;

        // Tick base from SessionAssign.
        bool assigned = false;
        // Design 9.26 phase alignment: between the anchor and the assignment
        // no frame runs at all - the anchor frame's OnProcess waits, polling
        // the assignment (physics_session_hold_until_assigned) - instead of
        // running frames the assignment would renumber away.  The server's
        // world takes its own first step under the base it hands out, so a
        // client that ran k frames in that window stayed k steps further from
        // the anchor at every tick number for the rest of the session - the
        // offset that made every script-driven mechanism (Level 11's sandbag
        // Delayer) run out of phase and be corrected at every flip.  A frame
        // with a near-zero delta is not still enough (the graphs' frame-counted
        // links still fire), hence the wait.  `hold_deadline` bounds it: a
        // server that never assigns must not freeze the game.
        bool hold_active = false;
        std::chrono::steady_clock::time_point hold_deadline{};
        uint64_t hold_polls = 0;   // 1 ms polls the wait took
        bool hold_timed_out = false;
        uint32_t tick_base = 0;
        uint32_t current_tick() const {
            return frames_since_anchor >= 1 ? tick_base + static_cast<uint32_t>(frames_since_anchor - 1) : tick_base;
        }
        // Re-anchor the numbering at `first_tick`: the frame after this call is
        // numbered `first_tick`.  A resync, a late join and a session start
        // (design 9.2: the server anchors every start member at its start lead,
        // not at 0) all discard what was stamped under the old base - frames the
        // server has already consumed or never asked for, the rollback records
        // keyed by the old numbers (which no incoming snapshot can match, so
        // every one of them arrives as kind 7 unmatched) and the correction
        // rings that would otherwise blend across the gap.
        void rebase_tick(uint32_t first_tick) {
            tick_base = first_tick;
            frames_since_anchor = 0;
            input_history.clear();
            own_inputs.clear();
            rollback.invalidate_history();
            corrector.clear();
            for (auto& [id, remote]: remotes) remote.corrector.clear();
            // The mechanism dictionary is a property of the world, not of the
            // numbering, but the identity guard's rows are keyed by the tick
            // they were received at and the server always sends a full snapshot
            // with a re-anchor - the one that refills both.  Dropping the
            // registry keeps a stale row from deciding which of two same-named
            // instances we drive; the cost is that mechanisms are uncorrected
            // (but still simulated locally, design 9.25) until that snapshot
            // lands.
            mechanism_tracking.clear();
            consecutive_hard = consecutive_unmatched = 0;
        }

        // Inputs: the recent frames (tick, frame), newest last; the backlog
        // before the assignment is flushed when it arrives.
        std::deque<std::pair<uint32_t, input_frame>> input_history;
        // Retransmit window: every frame re-sends the recent ticks, so this is
        // how far back a lagging client can refill a gap, and one message per
        // frame covers the whole window.  It cannot be wider than the wire cap
        // (session_input_msg serializes at most session::MAX_INPUT_FRAMES
        // frames, and the sender trims the ring to this size): a wider ring
        // would make the serializer keep the oldest frames of the window and
        // drop the newest ones the server actually needs.
        static constexpr size_t kInputHistory = bmmo::session::MAX_INPUT_FRAMES;
        float previous_cam[3][3] = {};        // Cam_OrientRef rows at the end of the previous frame
        bool previous_cam_valid = false;
        uint64_t inputs_sent = 0;
        // The pause menu stopped Gameplay_Ingame: the session keeps stepping
        // (PreSimulate clock guard), but the keys are reported as zero and the
        // own navigation replica stops polling them.
        bool input_muted = false;

        // Navigation graph (key bindings arrive a few frames after the anchor).
        bmmo::game::navigation_graph navigation;
        bool navigation_keys_known = false;

        // Own ball.
        std::string own_group;                // "P#<join order>"
        bool own_group_set = false;
        bool own_physicalized = false;
        own_ball_corrector corrector;
        uint64_t hard_sets = 0, blends = 0;
        // Design 9.6: the own ball is driven by the shared navigation replica
        // in polling mode (the retail force leaves push zero), so a rollback
        // can replay the recorded key edges.
        bool own_navigation = false;
        std::string own_nav_entity;
        int own_key_codes[8] = {};
        uint32_t own_key_blocks[8] = {};
        int own_key_count = 0;
        // engine change #6: the retail Unphysicalize keeps every body but this one
        bool body_guard = false;
        std::string body_guard_entity;
        // The last Physicalize we reported for our own ball.  The retail
        // scripts report a ball's life once (physicalize new Ball); if the
        // server never applied that event - rejected by the validation, or
        // dropped - our ball would stay out of the simulation for good, so we
        // report it again when the snapshots keep coming without our body.
        struct own_physicalize_report {
            bool valid = false;
            uint8_t ball_type = 0;
            uint8_t flags = 0;      // design 9.10: PHYSICALIZE_FLAG_* resent unchanged, no new impulse
            float position[3] = {};
            float rotation[9] = {};
            bmmo::session::ball_recipe recipe;
        } last_physicalize;
        int snapshots_without_own = 0;
        uint64_t physicalize_resends = 0;
        std::chrono::steady_clock::time_point last_physicalize_resend{};

        // Design 9.6: rollback instead of blending.  Own inputs per tick and
        // the relayed remote inputs per tick feed the re-simulation.
        bool rollback_enabled = true;      // automation: session rollback on|off
        rollback_engine rollback;
        std::map<uint32_t, input_frame> own_inputs;
        static constexpr size_t kInputRing = 128;

        // Shared mechanisms (design 9.25): the client simulates the
        // script-constrained bodies (rope, sandbag, see-saw) itself, like every
        // other body of its world, and the rollback engine corrects them from
        // the snapshot rows.  Option A (9.17) rendered them from the server's
        // rows instead; it is gone, with its dead-reckoning, its teleport snaps
        // and its per-replayed-tick re-poser - what is left of it is the
        // dictionary and one row per owner, which is all the identity guard
        // needs to say which local body a row is about.
        mechanism_registry mechanism_tracking;
        // Lifetime generation per tracked entity (own ball, remote mirrors,
        // mechanisms): bumped whenever the body behind a name is created or
        // destroyed, so the rollback engine can tell a record of an earlier
        // body under the same name from one of the body that is there now
        // instead of throwing the whole history away (findings/A5 section 2b:
        // 6 % of the 9.24 session was uncorrectable for that reason).
        std::map<std::string, uint32_t> generations;
        // Every body the retail Physicalize block made during the session, by
        // the tick of the record that first carries it (BML's OnPhysicalize
        // runs before the block creates the body, so this frame's record):
        // the block's recipe, collision group and the entity's matrix at that
        // moment.  A rewind anchored before that tick destroys the body at
        // the restore and makes it again at that tick from this
        // (rollback_world::destroy_body / recreate_body, plan doc 9), the way
        // the live frame made it - parking it instead left IVP's overlap tree
        // and unit order different for good.
        struct local_birth {
            std::string name, group;
            bmmo_physics_ball_recipe recipe{};
            float matrix[16] = {};         // the entity's world matrix at the Physicalize
            float local_matrix[16] = {};   // ...and its local one, for a parented entity
            bool has_parent = false;
        };
        std::map<uint32_t, std::vector<local_birth>> local_births;
        uint32_t birth_logs = 0;
        bool insert_births = false;              // the insert_birth A/B (see physics_session_rollback_world)
        // Every activation of Gameplay_SectorManager the live frames made, by
        // the tick whose frame consumed it: the union's passes
        // (physics_session_run_sector) and the ones scripts outside the
        // replay asked for, seen as next-frame flags after their frame.  A
        // rewind re-issues them inside its window (plan doc 10), and for the
        // frame about to run when the replay ended one tick short of one.
        struct manager_activation {
            bool reset = false;
            int activate = 0, deactivate = 0;   // IngameParameter[0][1], [0][2] as they were
        };
        std::map<uint32_t, manager_activation> manager_activations;
        uint64_t activations_replayed = 0;
        bool park_births = false;                // the v14 path (no activation replay, births parked): the A/B
        const local_birth* find_local_birth(const std::string& entity, uint32_t tick) const {
            if (tick) {
                const auto it = local_births.find(tick);
                if (it == local_births.end()) return nullptr;
                for (const auto& b: it->second) if (b.name == entity) return &b;
                return nullptr;
            }
            for (auto it = local_births.rbegin(); it != local_births.rend(); ++it)
                for (const auto& b: it->second) if (b.name == entity) return &b;
            return nullptr;
        }
        uint64_t corrections_logged = 0;
        uint64_t amend_failures = 0;             // amend_record calls whose tick was no longer recorded

        // The freshest ball row per player, kept even while that player has no
        // mirror yet: a Physicalize event is relayed about an input delay after
        // the snapshot that already separated the spawned balls, so creating the
        // mirror at the event's own spawn pose stacks every peer on one spot and
        // the local solver blows the cluster apart (9.17 feedback, spawn twitch).
        struct latest_ball_row {
            bool have = false;
            uint32_t tick = 0;
            double position[3] = {};
            double rotation[4] = {};
            float linear[3] = {};
            float angular[3] = {};
            bool simulated = true;
        };
        std::map<uint32_t, latest_ball_row> latest_ball_rows;

        // Remote balls: player -> mirrored entity.
        // Remote balls (design 9.1): mirrored entity driven by the bridge
        // navigation from the last relayed input; snapshots correct it through
        // the same ladder as the own ball.  Without navigation (no graph yet)
        // the body is written from every snapshot like in M3.
        struct remote_body {
            std::string entity;
            uint8_t ball_type = 0;
            bool physicalized = false;
            bool navigation = false;
            bool have_input = false;
            uint32_t input_tick = 0;
            input_frame input{};
            input_frame applied{};                 // what the last drive fed for the coming tick
            std::map<uint32_t, input_frame> inputs; // relayed frames by tick (rollback)
            body_corrector corrector;
            uint64_t blends = 0, hard_sets = 0;
        };
        std::map<uint32_t, remote_body> remotes;
        std::vector<float> ball_forces;      // Physicalize_GameBall "Force" per ball type (row order)

        uint32_t last_snapshot_tick = 0;
        bool have_snapshot = false;
        uint64_t snapshots_received = 0, snapshots_applied = 0, snapshots_stale = 0;
        uint64_t body_writes = 0, body_write_errors = 0;
        uint64_t events_sent = 0, events_received = 0;
        // The sectors this session runs (session/sector_union.hpp): every
        // sector any member has entered, each started at one tick on every
        // side.  A crossed checkpoint no longer runs Gameplay_SectorManager
        // from the retail chain (neutralization N3): its Sector event is
        // stamped sector_lead() ticks ahead and sent, the other members'
        // arrive relayed with their stamps, and the union runs one pass per
        // idle manager at the end of the frame BEFORE the step a stamp names -
        // where the server's update_sectors() runs relative to its own step.
        // Its pacer is counted once per step with the manager's state after
        // the previous one (the anchor counts for the first step).
        bmmo::session::sector_union sector_union;
        uint64_t sectors_deferred = 0;
        uint32_t last_sector_due = 0;        // the stamp of the last own hold, for the automation's reply
        // The script replay (game/script_replay.hpp): the mechanism scripts
        // whose state travels with the rollback records, selected at the
        // anchor, and the hooks that carry the force blocks' controllers
        // through the bridge.
        bmmo::game::script_replay::selection script_ids;
        std::vector<CK_ID> scripts_replayed;     // the scripts the last restore wrote: active at its anchor
        bmmo::game::script_replay_hooks script_hooks;
        uint32_t script_restore_logs = 0;
        uint32_t contact_restore_logs = 0;
        uint32_t savestate_get_logs = 0;     // savestate capture / restore failures logged this session (the first 8)
        uint32_t savestate_set_logs = 0;
        std::map<CK_ID, uint32_t> script_reset_ticks;   // the tick each script was last told to reset
        uint64_t script_reset_skips = 0;                 // scripts a restore kept live for having been reset after its anchor
        // Every sector this session has held or run, sector 1 (the level's own)
        // from the anchor: a sector is an input once, whichever path reports it
        // - the checkpoint hook, the cell poll, the automation - so a second
        // report never runs the manager over a sector that is already up.
        std::set<int> sectors_requested;
        // IngameParameter[0][1] as this client last saw or wrote it: the poll
        // in physics_session_frame holds a sector the hook did not report.
        int sector_cell = 0;
        // SessionStart::sector_lead: the input delay, one tick because the
        // manager can only run before the NEXT step, and a second one-way
        // budget for the relay to the other members (session::sector_lead_for_ping).
        uint8_t sector_lead_ticks = 0;
        uint32_t sector_lead() const { return sector_lead_ticks; }
        std::set<std::string> revived_reported_this_frame;

        // Queues filled by the network thread, drained on the game thread.
        std::mutex queue_mutex;
        std::deque<session_snapshot_msg> snapshot_queue;
        std::deque<session_event_msg> event_queue;
        std::deque<session_remote_input_msg> remote_input_queue;
        // SessionAssign, from the network thread (handle_session_assign).  Not
        // a run_on_game_thread() post: the anchor hold polls this from inside
        // a frame, where BML's timer queue does not run.
        struct assign_notice { uint32_t session = 0, first_tick = 0; };
        std::deque<assign_notice> assign_queue;
        uint64_t remote_inputs_received = 0;
        // The relayed frames, reassembled (session/input_relay.hpp, protocol
        // 5): first arrivals, re-deliveries of the window, frames parked for a
        // player whose mirror did not exist yet.  A datagram this queue drops
        // under load is re-delivered by the next ones.
        uint64_t relay_new = 0, relay_dup = 0, relay_parked = 0;
        // Protocol 7 (late-player design 1.4): the server's notices of our own
        // substituted ticks, taken once per tick and counted.  Plan step 3
        // only receives them; resolving, amending and journaling them is the
        // retail part one (plan step 6).  The records of a datagram numbered
        // from another base are counted once per (epoch, tick).  The header
        // of the newest datagram of our own epoch: the server's ack and
        // held_total (status line).
        relay_seen held_seen, held_stale_seen;
        uint64_t subst_recv = 0, subst_dup = 0, subst_stale_epoch = 0;
        bool held_header_known = false;
        uint32_t held_header_epoch = 0, held_header_tick = 0, held_header_acked = 0, held_header_total = 0;
        uint32_t last_assign_first_tick = 0;    // the assignment applied last: its redundant copies are ignored
        // Lifecycle events (protocol 6): the first arrival of each counts; our
        // own are repeated unreliably on the next ticks; and, this client
        // applying on arrival with no rewind, a lifecycle event older than the
        // one already applied for that player is superseded.
        event_seen events_seen;
        event_repeats own_event_repeats;
        uint64_t event_dups = 0, events_superseded = 0;
        // Protocol 9: echoes of our own events, by what the server did.
        uint64_t own_echo_acked = 0, own_echo_moved = 0, own_echo_superseded = 0, own_echo_rejected = 0;
        // ORIGINS (protocol 9): of the Physicalize or Unphysicalize applied
        // last, and of the Physicalize applied last, per player.
        std::map<uint32_t, uint32_t> last_lifecycle_tick;
        std::map<uint32_t, uint32_t> last_physicalize_tick;
        relay_seen relay_seen_frames;
        pending_relay pending_relay_frames;
        static constexpr size_t kMaxQueuedSnapshots = 64;
        // Relayed-input datagrams, one per tick: more than the rollback's
        // history (96 ticks) plus a window, so a game-thread stall that long
        // drops nothing a rewind could still have replayed.
        static constexpr size_t kRelayQueue = 160;

        // Resync (design 9.2): after a re-assignment the next full snapshot
        // rebuilds every body; triggers are a tick-driver rebase (pause, long
        // stall), 3 hard corrections in a row, 30 snapshots whose tick could
        // not be reconciled at all, or the input starvation detector below.
        bool resync_pending = false;
        uint64_t last_rebases = 0;
        int consecutive_hard = 0, consecutive_unmatched = 0;
        uint64_t last_unmatched = 0;
        std::chrono::steady_clock::time_point last_resync_request{};
        uint64_t resyncs_sent = 0, resyncs_done = 0;

        // Input starvation (design 9.2 follow-up): every snapshot carries
        // acked_input_tick, the last tick the server consumed a FRESH input
        // frame from us for (protocol 2.2).  If our numbering ever falls behind
        // the server's per-player read cursor, that cursor drops every later
        // frame as stale for the rest of the session and simulates our ball
        // from the last frame it had - nothing else notices, because our own
        // snapshot stream keeps arriving and keeping us corrected.  `frame` is
        // the frames_since_anchor the acked tick was last seen moving at,
        // `snapshot_frame` the one the newest snapshot arrived at: together they
        // separate a numbering hole from a network stall.
        struct acked_input_state {
            bool have = false;             // a baseline exists for the current numbering
            uint32_t tick = 0;             // last acked_input_tick the server reported
            int64_t frame = -1;            // frames_since_anchor when `tick` last advanced
            int64_t snapshot_frame = -1;   // frames_since_anchor of the newest applied snapshot
            void reset() { *this = acked_input_state{}; }
        };
        acked_input_state acked_input;

        std::string last_error;
        bool trace = false;         // per-tick diagnostics (automation: session trace on|off); not reset per session
        int exact_log_frames = 0;   // debug: exact core dumps after our Physicalize
        int rng_last_seed = 0, rng_last_cores = -1;   // debug: rng / awake-body change log
        float rng_last_pdelta = -1.0f;                 // debug: physics pause/resume detection
        uint8_t last_input_keys = 0, last_input_flags = 0;   // debug: input edge detection

        void reset_runtime() {
            saw_ingame_inactive = false;
            anchored = false;
            frames_since_anchor = -1;
            anchor_hash = anchor_surfaces = 0;
            assigned = false;
            hold_active = false;
            hold_deadline = {};
            hold_polls = 0;
            hold_timed_out = false;
            tick_base = 0;
            input_history.clear();
            previous_cam_valid = false;
            inputs_sent = 0;
            input_muted = false;
            navigation = {};
            navigation_keys_known = false;
            own_group_set = false;
            own_physicalized = false;
            own_navigation = false;
            own_nav_entity.clear();
            own_key_count = 0;
            rollback.clear();
            own_inputs.clear();
            body_guard = false;
            body_guard_entity.clear();
            last_physicalize = {};
            snapshots_without_own = 0;
            physicalize_resends = 0;
            last_physicalize_resend = {};
            corrector.clear();
            hard_sets = blends = 0;
            mechanism_tracking.clear();
            generations.clear();
            local_births.clear();
            birth_logs = 0;
            manager_activations.clear();
            activations_replayed = 0;
            latest_ball_rows.clear();
            corrections_logged = 0;
            savestate_get_logs = savestate_set_logs = 0;
            script_reset_ticks.clear();
            script_reset_skips = 0;
            scripts_replayed.clear();
            amend_failures = 0;
            remotes.clear();
            last_snapshot_tick = 0;
            have_snapshot = false;
            snapshots_received = snapshots_applied = snapshots_stale = 0;
            body_writes = body_write_errors = 0;
            events_sent = events_received = 0;
            resync_pending = false;
            last_rebases = 0;
            consecutive_hard = consecutive_unmatched = 0;
            last_unmatched = 0;
            last_resync_request = {};
            resyncs_sent = resyncs_done = 0;
            acked_input.reset();
            remote_inputs_received = 0;
            relay_new = relay_dup = relay_parked = 0;
            subst_recv = subst_dup = subst_stale_epoch = 0;
            held_seen.clear();
            held_stale_seen.clear();
            held_header_known = false;
            last_assign_first_tick = 0;
            events_seen.clear();
            own_event_repeats.clear();
            event_dups = events_superseded = 0;
            own_echo_acked = own_echo_moved = own_echo_superseded = own_echo_rejected = 0;
            last_lifecycle_tick.clear();
            last_physicalize_tick.clear();
            relay_seen_frames.clear();
            pending_relay_frames.clear();
            ball_forces.clear();
            rng_last_seed = 0;
            rng_last_cores = -1;
            rng_last_pdelta = -1.0f;
            last_input_keys = last_input_flags = 0;
            revived_reported_this_frame.clear();
            std::lock_guard lk(queue_mutex);
            snapshot_queue.clear();
            event_queue.clear();
            remote_input_queue.clear();
            assign_queue.clear();
        }
    };
}
