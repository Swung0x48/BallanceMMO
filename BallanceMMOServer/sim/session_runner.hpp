#pragma once

// The simulation thread (design section 8.3): owns every physics_world, takes
// commands from the network thread through a queue, paces each running
// session with a tick_scheduler and hands snapshots back through callbacks.
//
// Thread model: every public method is thread-safe and returns immediately;
// the callbacks run on the simulation thread and must not block (the server
// sends the snapshot straight through GameNetworkingSockets, which is
// thread-safe).  Worlds are created on the simulation thread too, so a boot
// (about ten seconds) stalls the other sessions for that long; M3 runs one
// physics room at a time.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <entity/session.hpp>
#include <session/timeline.hpp>

#include "physics_world.hpp"

namespace bmmo::sim {
    struct runner_config {
        bool trace = false;                 // world_options::trace
        std::filesystem::path game_root;
        uint32_t input_delay = 6;
        uint32_t snapshot_interval = 2;
        uint32_t full_snapshot_interval = 66;
        uint32_t max_catch_up_ticks = 10;   // ticks simulated per loop when behind
        int seed = 1;
        float spawn_impulse = 0.0f;         // world_options::spawn_impulse (design 9.10)
    };

    struct session_snapshot {
        uint32_t session = 0;
        uint32_t tick = 0;
        bool full = false;
        std::vector<bmmo::session::body_state> bodies;
        // per player: the last tick whose input was actually applied
        std::vector<std::pair<uint32_t, uint32_t>> acked_inputs;
    };

    // Outcome of a world boot.  On success anchor_hash/surfaces are what
    // SessionReady must match and spawn_* is the level's retail spawn pose.
    struct world_ready_info {
        uint32_t session = 0;
        bool ok = false;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;
        float spawn_position[3] = {};
        float spawn_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // The level's Physicalize_GameBall rows: the reference the event
        // validation compares a reported ball recipe with (design 9.4).
        std::vector<physics_world::ball_row> ball_rows;
        std::string error;
    };

    struct session_callbacks {
        std::function<void(const world_ready_info&)> on_world_ready;
        std::function<void(const session_snapshot&)> on_snapshot;
        // Every simulated tick: the input frame the world applied for each
        // player (fresh or repeated); the server relays them to the other
        // members so clients predict remote balls with the same input
        // (design 9.1).
        std::function<void(uint32_t session, uint32_t tick,
                           const std::vector<std::pair<uint32_t, bmmo::session::input_frame>>& applied)> on_inputs;
        // A tick failed or the world died; the server ends the session.
        std::function<void(uint32_t session, const std::string& reason)> on_failed;
        std::function<void(const std::string&)> log;
    };

    class session_runner {
    public:
        session_runner(runner_config config, session_callbacks callbacks);
        ~session_runner();
        session_runner(const session_runner&) = delete;
        session_runner& operator=(const session_runner&) = delete;

        // Boots a world for the level; on_world_ready follows.  Each player is
        // paired with its join order (design 9.10): the world's per-player
        // slot and the spawn direction table both key off it.
        void create_session(uint32_t session, int level, const std::vector<std::pair<uint32_t, uint8_t>>& players);
        void destroy_session(uint32_t session);
        void add_player(uint32_t session, uint32_t player, uint8_t join_order);
        void remove_player(uint32_t session, uint32_t player);
        // The player anchored at first_tick; ticking starts when every player
        // of a not-yet-running session is ready.
        void player_ready(uint32_t session, uint32_t player, uint32_t first_tick);
        void submit_input(uint32_t session, uint32_t player, uint32_t first_tick,
                          std::vector<bmmo::session::input_frame> frames);
        void submit_event(uint32_t session, uint32_t player, uint32_t tick, lifecycle_event event);
        void request_full_snapshot(uint32_t session);
        // Diagnostics through the log callback.
        void describe(uint32_t session);

        // Readable from any thread: the tick the session will simulate next
        // (0 when unknown) and whether it is ticking.
        uint32_t current_tick(uint32_t session) const;
        bool running(uint32_t session) const;
        // Tick to assign to a late joiner anchoring now.
        uint32_t late_join_tick(uint32_t session) const;
        size_t session_count() const;

    private:
        struct pending_event {
            uint32_t player;
            uint32_t tick;
            lifecycle_event event;
        };
        struct session_state {
            uint32_t id = 0;
            int level = 0;
            std::unique_ptr<physics_world> world;
            std::set<uint32_t> players;
            std::set<uint32_t> ready;
            std::map<uint32_t, bmmo::session::input_buffer> inputs;
            std::map<uint32_t, uint32_t> acked;
            std::vector<pending_event> events;   // sorted by tick on insertion
            bmmo::session::tick_scheduler scheduler;
            bmmo::session::snapshot_cadence cadence;
            bool running = false;
            bool failed = false;
            // Barrier: a tick whose input shows a not-yet-known physicalized ball
            // waits (briefly) for the reliable Physicalize event that carries the
            // recipe and pose.
            std::chrono::steady_clock::time_point barrier_deadline{};
            bool barrier_armed = false;
            // Players whose Physicalize the barrier already waited a second
            // for: without this the barrier re-arms every tick and a single
            // lost or rejected event pins the whole session at 1 tick/s.
            std::set<uint32_t> lifecycle_missing;
        };
        bool waiting_for_lifecycle(session_state& s, uint32_t tick);

        void run();
        void post(std::function<void()> command);
        void step(session_state& s);
        void emit_snapshot(session_state& s, bool full);
        void log(const std::string& text);
        void publish_state(const session_state& s);

        runner_config config_;
        session_callbacks callbacks_;
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable wake_;
        std::deque<std::function<void()>> commands_;
        bool stopping_ = false;
        std::map<uint32_t, std::unique_ptr<session_state>> sessions_;  // simulation thread only

        // cross-thread view
        mutable std::mutex view_mutex_;
        struct view { uint32_t next_tick = 0; bool running = false; std::chrono::steady_clock::time_point start{}; uint32_t first_tick = 0; };
        std::map<uint32_t, view> views_;
    };
}
