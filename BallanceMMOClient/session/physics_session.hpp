#pragma once

// Client-side state of a physics session (design section 8.5).  The logic
// lives in session/physics_session_client.cpp as BallanceMMOClient methods
// (like room_client.cpp); this header only holds the data so the mod's main
// header stays readable.
//
// Timeline: the anchor frame (first OnProcess with Gameplay_Ingame active
// after the restart) is frame 0; OnProcess of frame f >= 1 represents session
// tick tick_base + f - 1, where tick_base is the number the server assigns
// (0 for the members present at the start).

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
#include <session/rollback.hpp>

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
        float spawn_offset[3] = {};          // ring offset relative to the retail resetpoint

        // Restart / anchor detection.
        bool saw_ingame_inactive = false;
        std::chrono::steady_clock::time_point restart_deadline{};
        bool anchored = false;
        int64_t frames_since_anchor = -1;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;

        // Tick base from SessionAssign.
        bool assigned = false;
        uint32_t tick_base = 0;
        uint32_t current_tick() const {
            return frames_since_anchor >= 1 ? tick_base + static_cast<uint32_t>(frames_since_anchor - 1) : tick_base;
        }

        // Inputs: the recent frames (tick, frame), newest last; the backlog
        // before the assignment is flushed when it arrives.
        std::deque<std::pair<uint32_t, input_frame>> input_history;
        static constexpr size_t kInputHistory = 8;
        float previous_cam[3][3] = {};        // Cam_OrientRef rows at the end of the previous frame
        bool previous_cam_valid = false;
        uint64_t inputs_sent = 0;

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

        // Shared mechanisms: one corrector per dictionary name, same ladder as
        // the own ball (a snapshot for tick T is compared with the local state
        // recorded at T, never with the current one).
        std::map<std::string, body_corrector> mechanism_correctors;
        uint64_t mechanism_blends = 0, mechanism_hard = 0;
        uint64_t corrections_logged = 0;

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

        // Mechanism dictionary from full snapshots.
        std::map<uint32_t, std::string> mechanism_names;
        uint32_t last_snapshot_tick = 0;
        bool have_snapshot = false;
        uint64_t snapshots_received = 0, snapshots_applied = 0, snapshots_stale = 0;
        uint64_t body_writes = 0, body_write_errors = 0, mechanism_matches = 0;
        uint64_t events_sent = 0, events_received = 0;
        std::set<std::string> revived_reported_this_frame;

        // Queues filled by the network thread, drained on the game thread.
        std::mutex queue_mutex;
        std::deque<session_snapshot_msg> snapshot_queue;
        std::deque<session_event_msg> event_queue;
        std::deque<session_remote_input_msg> remote_input_queue;
        uint64_t remote_inputs_received = 0;
        static constexpr size_t kMaxQueuedSnapshots = 64;

        // Resync (design 9.2): after a re-assignment the next full snapshot
        // rebuilds every body; triggers are a tick-driver rebase (pause, long
        // stall), 3 hard corrections in a row, or 30 unmatched snapshots.
        bool resync_pending = false;
        uint64_t last_rebases = 0;
        int consecutive_hard = 0, consecutive_unmatched = 0;
        uint64_t last_unmatched = 0;
        std::chrono::steady_clock::time_point last_resync_request{};
        uint64_t resyncs_sent = 0, resyncs_done = 0;

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
            tick_base = 0;
            input_history.clear();
            previous_cam_valid = false;
            inputs_sent = 0;
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
            mechanism_correctors.clear();
            mechanism_blends = mechanism_hard = 0;
            corrections_logged = 0;
            remotes.clear();
            mechanism_names.clear();
            last_snapshot_tick = 0;
            have_snapshot = false;
            snapshots_received = snapshots_applied = snapshots_stale = 0;
            body_writes = body_write_errors = mechanism_matches = 0;
            events_sent = events_received = 0;
            resync_pending = false;
            last_rebases = 0;
            consecutive_hard = consecutive_unmatched = 0;
            last_unmatched = 0;
            last_resync_request = {};
            resyncs_sent = resyncs_done = 0;
            remote_inputs_received = 0;
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
        }
    };
}
