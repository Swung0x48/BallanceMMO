#pragma once

// One physics-session world on the server (design section 8.3): a headless
// engine that booted base.cmo, entered the level through the retail menus and
// reached the anchor (first tick with Gameplay_Ingame active), with the retail
// ball parked and one clone ball per player.
//
//   create()       boot + level + anchor + session reset; records the anchor
//                  world hash that clients must match in SessionReady.
//   add_player()   clone entities and a camera reference frame for a player;
//                  the ball body itself appears with the client's Physicalize
//                  event (design 8.2: lifecycle events are client-reported).
//   set_input()    the player's input for the coming tick (keys, camera basis,
//                  nav_active flag).
//   apply_event()  Physicalize / Unphysicalize / Sector / Finish / BodyRevived.
//   tick()         one behaviour frame: per-player navigation, sector union
//                  activations, CKContext::Process, parking of the retail ball.
//   snapshot()     the balls plus the movable mechanism bodies.
//
// Every method runs on the simulation thread that created the world.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <entity/session.hpp>
#include <game/navigation_graph.hpp>
#include <physics/physics_rt_api.h>

#include "headless_engine.hpp"
#include <physics/ball_navigation.hpp>

class CK3dEntity;
class CKBehavior;
class CKDataArray;
class CKIpionManager;

namespace bmmo::sim {
    struct world_options {
        std::filesystem::path game_root;
        int level = 1;
        int seed = 1;
        // Spawn kick speed, m/s (design 9.10); 0 disables (solo sessions and
        // debug/replay worlds that never set it stay bit-exact with a
        // recording that has no impulse).
        float spawn_impulse = 0.0f;
        int boot_ticks = 400;          // ticks for the composition to reach the main menu
        int anchor_timeout = 3000;     // ticks to wait for Gameplay_Ingame after the level request
        // Per-tick diagnostics in the log: rng / awake-body changes, input
        // edges, exact core dumps around physicalize / resume / input edges
        // (config physics.debug_trace; the client mirrors it with
        // "session trace on" so the two logs can be diffed tick by tick).
        bool trace = false;
        // Debug switches for the offline navigation replay (design 8.6):
        //   park_retail_ball = false keeps the retail ball live so a player can
        //   be attached to it; auto_clone_players physicalizes every player's
        //   clone with the retail recipe at the retail ball's pose in the tick
        //   the retail script physicalizes the retail ball.
        bool park_retail_ball = true;
        bool auto_clone_players = false;
        // Debug: force the retail navigation leaves to push zero force every
        // tick (their Force Value parameter is overwritten before Process), so
        // the C++ navigation attached to the retail ball is the only real push.
        bool zero_retail_force = false;
        // Debug: take nav_active from the retail script's own Key Event state
        // (read after the scripts ran) instead of the input flag.
        bool retail_nav_from_script = false;
        // Debug (with auto_clone_players): the retail ball entity mirrors the
        // clone's pose after every tick so the retail gameplay scripts (depth
        // test, checkpoints, trafos) react to the clone; a Hide of the retail
        // ball unphysicalizes the clone, a new retail body re-physicalizes it.
        bool mirror_clone_to_retail = false;
        std::function<void(const std::string&)> log;
    };

    // A client-reported lifecycle transition (protocol section 2.2,
    // session_event_msg), decoupled from the wire message.
    struct lifecycle_event {
        bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
        uint8_t ball_type = 0;
        uint8_t flags = 0;   // Physicalize: bmmo::session::PHYSICALIZE_FLAG_* (design 9.10)
        float position[3] = {};
        float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // world matrix rows
        bmmo_physics_ball_recipe recipe{};
        int32_t sector = 0;
        std::string name;   // BodyRevived: the mechanism body
        // The tick this event was submitted for (session_runner::pending_event
        // tick): Physicalize's spawn impulse direction is derived from it, so
        // both sides pick the same table entry (session/spawn_impulse.hpp).
        uint32_t tick = 0;
    };

    class physics_world {
    public:
        static std::unique_ptr<physics_world> create(const world_options& options, std::string& error);
        ~physics_world();
        physics_world(const physics_world&) = delete;
        physics_world& operator=(const physics_world&) = delete;

        // One row of the level's Physicalize_GameBall array: the physics the
        // retail scripts hand the Physicalize block for that ball type.
        struct ball_row { std::string name; float friction = 0, elasticity = 0, mass = 0, linear_damp = 0, rot_damp = 0, force = 0; };

        uint64_t anchor_hash() const { return anchor_hash_; }
        uint64_t anchor_surfaces() const { return anchor_surfaces_; }
        // The Physicalize_GameBall rows in row order (= ball type order).
        const std::vector<ball_row>& ball_rows() const { return ball_rows_; }
        uint32_t tick_index() const { return tick_; }
        int level() const { return options_.level; }
        const bmmo::game::navigation_graph& navigation() const { return navigation_; }
        // Force value of a ball type (Physicalize_GameBall "Force"), 0 if unknown.
        float force_value(uint8_t ball_type) const;
        // Retail spawn matrix of the level (CurrentLevel[0,3] at the anchor).
        const VxMatrix& spawn_matrix() const { return spawn_matrix_; }

        // `join_order` is the player's slot when free (kMaxSlots stays 64: it
        // is also the nocoll group index and the spawn direction table's
        // per-player offset, design 9.10); a taken slot falls back to the
        // lowest free one and logs it.
        bool add_player(uint32_t id, uint8_t join_order, std::string& error);
        void remove_player(uint32_t id);
        bool has_player(uint32_t id) const { return players_.count(id) != 0; }
        size_t player_count() const { return players_.size(); }
        std::vector<uint32_t> player_ids() const;
        void set_input(uint32_t id, const bmmo::session::input_frame& frame);
        bool apply_event(uint32_t id, const lifecycle_event& event, std::string& error);
        bool player_physicalized(uint32_t id) const;
        uint8_t player_ball_type(uint32_t id) const;
        int player_sector(uint32_t id) const;

        bool tick(std::string& error);

        // Balls of every physicalized player and, in full snapshots, every
        // movable mechanism body (delta snapshots: only the simulated ones).
        // Mechanism indices are stable per world; names are filled for full.
        void snapshot(bool full, std::vector<bmmo::session::body_state>& out);
        // Whether the set of movable mechanism bodies changed since the last
        // snapshot() call (a full snapshot is then due).
        bool body_set_changed() const { return body_set_changed_; }

        // The sectors the world is running: the union of the sectors its
        // players are in, applied by update_sectors() in the tick it changes.
        const std::set<int>& active_sectors() const { return active_sectors_; }
        void update_sectors();

        std::string describe() const;
        CKIpionManager* physics() const;
        CKIpionManager* manager_for_debug() const;
        headless_engine& engine() { return *engine_; }
        void log(const std::string& text) const;

        // Parks the retail ball: its physics body is removed (a merely frozen
        // body is still counted by IVP's calm-check schedule and shifts the
        // environment RNG).  Runs in the PreSimulate pass of the tick the retail
        // script physicalizes it and is re-checked after every tick.
        bool park_retail_ball();
        // Proximity union (design 8.3): every "TT Scaleable Proximity" whose
        // ObjectA is Ball_Pos_Frame is rewired to a private frame that is moved,
        // every tick, to the player ball nearest that block's ObjectB.
        void rewire_proximity_probes();
        void update_proximity_probes();
        size_t proximity_probe_count() const { return probes_.size(); }
        // Ball identity union (design 8.3): a mechanism that gates on the
        // active ball (the rope bridge P_Modul_29 tears only for "Ball_Stone")
        // reads CurrentLevel[0,ActiveBall], which on the server names the
        // parked retail ball and never changes.  Every such read inside a
        // script whose proximity blocks were rewired is retargeted at a
        // private copy of the array whose ActiveBall cell follows the player
        // the probe frames follow.
        void rewire_ball_identity_reads();
        void update_ball_identity_reads();
        CK3dEntity* retail_ball() const;
        // Runs in the physics manager's PreSimulate pass of every tick: after
        // the scripts of that tick, before its PSIs.  Parks the retail ball
        // when it appears and applies the per-player navigation edges.
        void pre_simulate();
        // Debug: drive the retail ball itself with the C++ navigation (the
        // world must have been created with park_retail_ball = false).
        bool attach_player_to_retail_ball(uint32_t id, std::string& error);
        // The retail script's own BallNav state (any navigation Key Event active).
        bool retail_navigation_active() const;
        // The retail Cam_OrientRef frame (direction reference of the leaves).
        CK3dEntity* retail_direction_ref() const;
        // The retail Physicalize recipe for a ball type (Physicalize_GameBall row
        // plus the shape rule of "physicalize new Ball").
        bmmo_physics_ball_recipe retail_recipe(uint8_t ball_type) const;
        // Ball type index of a ball entity name ("Ball_Wood" -> row), -1 if none.
        int ball_type_of(const std::string& entity_name) const;

    private:
        physics_world() = default;
        bool boot(std::string& error);
        bool anchor(std::string& error);
        bool ensure_collision_filter(std::string& error);

        struct player {
            uint32_t id = 0;
            int slot = 0;                             // per-world slot, 0..63
            std::string group;                        // nocoll group ident "P#<slot>" (IVP limit: 7 chars)
            std::map<uint8_t, CK3dEntity*> balls;     // clone entity per ball type
            CK3dEntity* ball = nullptr;               // the physicalized one (or null)
            CK3dEntity* cam_ref = nullptr;
            std::unique_ptr<bmmo::physics::player_navigation> navigation;
            bmmo::session::input_frame input{};
            bool have_input = false;
            bool physicalized = false;
            uint8_t ball_type = 0;
            int sector = 1;
            bool finished = false;
        };
        CK3dEntity* clone_ball(player& p, uint8_t ball_type, std::string& error);
        std::string ball_name(uint8_t ball_type) const;
        // The retail entity of a ball type ("Ball_Stone"), the object the
        // mechanism scripts compare names against; null if the level has none.
        CK3dEntity* retail_ball_entity(uint8_t ball_type) const;

        world_options options_;
        std::unique_ptr<headless_engine> engine_;
        uint64_t anchor_hash_ = 0, anchor_surfaces_ = 0;
        uint32_t tick_ = 0;
        bmmo::game::navigation_graph navigation_;
        bool navigation_keys_known_ = false;
        std::vector<ball_row> ball_rows_;             // Physicalize_GameBall rows
        VxMatrix spawn_matrix_{};
        CK_ID retail_ball_ = 0;
        bool retail_parked_ = false;
        bool callback_pending_ = false;
        CK_ID sector_manager_ = 0;
        CK_ID ingame_parameter_ = 0;
        std::set<int> active_sectors_;
        int sector_idle_ticks_ = 0;     // ticks the sector manager has been idle
        std::map<uint32_t, player> players_;
        std::set<int> used_slots_;
        struct proximity_probe { CK_ID block = 0; CK_ID frame = 0; CK_ID parameter = 0; };
        std::vector<proximity_probe> probes_;
        // A "Get Cell" that reads the active ball of a mechanism script: the
        // block, the private CurrentLevel copy it reads instead, the local
        // parameter that feeds it, the ActiveBall column, whether the script
        // only tests the ball's name (then it gets the retail entity of the
        // player's ball type, otherwise the player's clone) and the mechanisms
        // (proximity ObjectB) the script watches.
        struct identity_probe {
            CK_ID block = 0;
            CK_ID array = 0;
            CK_ID parameter = 0;
            int column = 1;
            bool by_name = false;
            std::vector<CK_ID> references;
        };
        std::vector<identity_probe> identities_;
        CK_ID current_level_ = 0;
        CK_ID ball_pos_frame_ = 0;
        std::unordered_map<std::string, uint16_t> body_index_;
        std::set<std::string> last_body_set_;
        bool body_set_changed_ = true;
        void* filter_environment_ = nullptr;
        int rng_cursor_ = 0;
        int random_cursor_ = 0;   // bmmo::physics::random_* state, saved/restored around tick() like rng_cursor_
        int exact_log_ticks_ = 0;   // debug: exact core dumps after a Physicalize event
        int rng_last_seed_ = 0, rng_last_cores_ = -1;   // debug: rng / awake-body change log
        float rng_last_pdelta_ = -1.0f;                  // debug: physics pause/resume detection
    };
}
