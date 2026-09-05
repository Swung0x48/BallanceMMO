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
#include <chrono>
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
#include <session/journal.hpp>
#include <session/timeline.hpp>

#include "physics_world.hpp"

namespace bmmo::sim {
    struct runner_config {
        bool trace = false;                 // world_options::trace
        std::filesystem::path game_root;
        uint32_t input_delay = 6;           // floor; each session sizes its own from ping
        uint32_t snapshot_interval = 2;
        uint32_t full_snapshot_interval = 66;
        uint32_t max_catch_up_ticks = 10;   // ticks simulated per loop when behind
        int seed = 1;
        // The session black box (design 9.15): every input the world consumed
        // goes to <journal_dir>/session_<id>_level<N>_<UTC>.bmjr, so a bug that
        // only happens in a live room can be replayed offline bit for bit.
        // Empty = record nothing.
        std::filesystem::path journal_dir;
        uint64_t journal_max_bytes = 256ull << 20;   // per session file; 0 = no limit
        uint32_t journal_checkpoint_ticks = 660;     // full body checkpoint cadence; 0 = none
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
        // `input_delay` is this session's, sized by the caller from the
        // members' round trips; 0 falls back to the runner's configured floor.
        // `spawn_impulse` is this session's kick speed in metres per second -
        // the same number session_start_msg carries to every member, so the
        // world is built with what the clients were told.  The runner has no
        // copy of its own: there is nowhere else for the world to get it.
        // `note` goes into the journal as `start: <note>` (the room and its
        // members: what a human needs to recognise the recording later) and
        // `names` are the players' display names, parallel to `players` -
        // either the same length or empty.
        void create_session(uint32_t session, int level, const std::vector<std::pair<uint32_t, uint8_t>>& players,
                            uint32_t input_delay, float spawn_impulse, const std::string& note = {},
                            std::vector<std::string> names = {});
        // `reason` goes into the journal as `end: <reason>` before it closes.
        void destroy_session(uint32_t session, const std::string& reason = {});
        // `name` is the player's display name for the journal's PLAYER record.
        void add_player(uint32_t session, uint32_t player, uint8_t join_order, const std::string& name = {});
        void remove_player(uint32_t session, uint32_t player);
        // The player anchored at first_tick; ticking starts when every player
        // of a not-yet-running session is ready.
        void player_ready(uint32_t session, uint32_t player, uint32_t first_tick);
        void submit_input(uint32_t session, uint32_t player, uint32_t first_tick,
                          std::vector<bmmo::session::input_frame> frames);
        // Events due in the same tick are applied in the members' JOIN ORDER,
        // never in arrival order: the order is part of the determinism
        // contract (design 9.15), because it is the only one a client can
        // reproduce offline - its own journal cannot know which of two players
        // reached the server first, but every member knows every join order.
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
        // The session's journal file, empty when it is not recording.
        std::string journal_path(uint32_t session) const;

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
            // Join order per player: the rank step() sorts a tick's due events
            // by (the world keeps its own copy, but only for players it could
            // add, and the order is needed for every member).
            std::map<uint32_t, uint8_t> join_orders;
            std::set<uint32_t> ready;
            std::map<uint32_t, bmmo::session::input_buffer> inputs;
            std::map<uint32_t, uint32_t> acked;
            std::vector<pending_event> events;   // sorted by tick on insertion
            uint32_t input_delay = 0;   // ticks of grace this session gives inputs
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
            // The black box (design 9.15).  Touched on the simulation thread
            // only; `journal_start` is the instant its header was written, the
            // origin every TICK record's `ms` is measured from.
            bmmo::session::journal_writer journal;
            std::chrono::steady_clock::time_point journal_start{};
        };
        bool waiting_for_lifecycle(session_state& s, uint32_t tick);
        void open_journal(session_state& s, float spawn_impulse, uint64_t anchor_hash,
                          uint64_t anchor_surfaces, uint32_t first_tick);

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
        struct view { uint32_t next_tick = 0; bool running = false; std::chrono::steady_clock::time_point start{}; uint32_t first_tick = 0;
                      std::string journal; };
        std::map<uint32_t, view> views_;
    };
}
