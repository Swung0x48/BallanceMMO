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
//   apply_event()  Physicalize / Unphysicalize / Sector / Finish / BodyRevived
//                  / Trafo (a world with trafo_inputs: run_trafo on the
//                  player's own piece set).
//   tick()         one behaviour frame: per-player navigation, sector union
//                  activations, the trafo frame, CKContext::Process, parking
//                  of the retail ball, the trafo hold.
//   snapshot()     the balls plus the movable mechanism bodies.
//
// Every method runs on the simulation thread that created the world.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <entity/session.hpp>
#include <game/navigation_graph.hpp>
#include <game/proximity_union.hpp>
#include <game/script_replay.hpp>
#include <game/script_state.hpp>
#include <game/checkpoint_phase.hpp>
#include <game/tutorial_exit.hpp>
#include <game/depth_test.hpp>
#include <game/sector_cell_split.hpp>
#include <game/sector_activation.hpp>
#include <game/sector_placement.hpp>
#include <game/trafo_sets.hpp>
#include <session/sector_union.hpp>
#include <session/trafo_input.hpp>
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
        // recording that has no impulse).  Step SPX: a session's world takes
        // SessionStart's value, 0 while the spawn ghost covers every spawn
        // birth (session_spawn_separation); a replay takes its journal's.
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
        // session::WORLD_TRAFO_INPUTS (trafo-replication design 2.14, step
        // T2): every player gets its own trafo piece set at add_player, the
        // level's originals are quiesced at the anchor and held after every
        // tick, Trafo events run the sets' scripts in the trafo frame, a
        // piece never collides with a local body.  Off: every world as before.
        bool trafo_inputs = false;
        // session::WORLD_PROXIMITY_UNION (world-agreement-gaps-design.md
        // §2.2-§2.4, step G2): the proximity union's rules - the anchor
        // rewires with the union rules (frames at the first reset point,
        // BMMO_NoBall, one gating probe per identity read, all or nothing)
        // and the frames, identity cells and latches are placed by
        // place_probes() from the candidates (the physicalized clones at the
        // end of the previous tick) under each block's own metric, with the
        // hold.  The session runner calls place_probes() first in its step,
        // before the leavers and the events; tick() places itself only when
        // nobody did for its tick (SimTool's scenes).  Off: the bit-clear
        // path (place_legacy inside tick(), step G1), bit for bit.
        bool proximity_union = false;
        // session::WORLD_PLAYER_MECHANISMS (world-agreement-gaps-design.md
        // §2.10, step Q7): with the union, the slot count K the anchor copies
        // every per-player unit into (game/proximity_union.hpp's copy step);
        // place_probes() binds slot k to the clone of the player whose join
        // order is k.  0: no units (G3's single-ball rules).
        uint8_t mechanism_slots = 0;
        // Sector placement (docs/sector-placement-design.md 2.4, step S2):
        // at the anchor every PH row whose object an earlier row placed at
        // another matrix gets its own copy (game/sector_placement.hpp).  Off
        // by default until the session option exists (S5); the signature is
        // computed either way.  placement_fail_at: SimTool's test hook
        // (--placement-fail-at, design 7.2 V12), -1 = none.
        bool placement_clones = false;
        int placement_fail_at = -1;
        // session::WORLD_SESSION_CLOCK (record-format-gaps-design.md 8 row
        // K3 = C4, option (c); step RK3): on Level 1 the tutorial's exit
        // event (game/tutorial_exit.hpp) is activated once, right before the
        // world's frame tutorial_exit_frame after the anchor (frame 1 = the
        // first tick).  Off: the tutorial runs as the level has it.
        // tutorial_exit_frame: SimTool's --tutorial-exit-frame (a
        // measurement switch); every session world uses the shared constant.
        bool session_clock = false;
        int tutorial_exit_frame = bmmo::game::kTutorialExitFrame;
        // N2 (step B2SW, game/sector_deactivate_pin.hpp): the checkpoint's and
        // the level reset's IngameParameter[0][2] Set Cell pinned to 0 at the
        // anchor.  Every world pins; false only for SimTool's A/B
        // (--n2-pin off).
        bool pin_checkpoint_deactivation = true;
        // Step B2SX (game/sector_cell_split.hpp): the checkpoint chain's
        // IngameParameter[0][1] Set Cell writes a private copy, so a clone's
        // crossing in the frame of an update_sectors pass of another sector
        // cannot make the manager activate the crossed one.  Every world
        // splits; false only for SimTool's A/B (--n2-cell off).
        bool split_sector_cell = true;
        // FULL step PRX (game/depth_test.hpp): Gameplay_Ingame/DepthTest's
        // Test pinned at the anchor and its rule run by tick() for every
        // member at every tick, as every client world runs it.  Every world
        // does; false only for SimTool's A/B (--depth-test-rule off).
        bool depth_test_rule = true;
        // SimTool's id-layout test hooks (sector placement design 7.2 V7/V9,
        // step S7: --free-ids-before-load, --perturb-ids), unset in every
        // other world: before_level_load runs after the boot, right before
        // the level request; before_anchor at the start of anchor(), after
        // the load and before the restores and the placement.  Each may
        // create and destroy CK objects (ids only: no tick runs, no scene,
        // no physics).
        std::function<void(CKContext*)> before_level_load;
        std::function<void(CKContext*)> before_anchor;
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
        // Trafo (protocol 11): ball_type = the piece type, flags =
        // session::TRAFO_*, position = the explosion's referential.
        // The tick this event was submitted for (session_runner::pending_event
        // tick): Physicalize's spawn impulse direction is derived from it, so
        // both sides pick the same table entry (session/spawn_impulse.hpp).
        uint32_t tick = 0;
    };

    // Determinism contract (design 9.15).  The world's entire input surface is
    // create(), add_player(), remove_player(), set_input(), apply_event() and
    // tick(): given those calls in that order, the simulation is reproducible
    // bit for bit, which is what makes the session journal a black box and not
    // a log.  Anything that ever gives the world another input must be
    // journalled by session_runner (BallanceMMOCommon session/journal.hpp) in
    // the same breath, or every replay of a session that used it silently
    // diverges from the session it is meant to reproduce.
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
        // Numbers the world's FIRST simulated tick (design 9.25 phase
        // alignment).  A session's members are numbered from a common start
        // base B (sim/late_tick.hpp session_start_tick_base) and each of them
        // steps its own world for the first time under that number, so the
        // world has to step for the first time under it too: with the counter
        // left at 0 the server's world was B steps further from the anchor
        // than every client's at the same tick number, and every script-driven
        // mechanism (Level 11's sandbag Delayer) flipped B ticks early on this
        // side.  Ticks below the base are never simulated - nothing is skipped,
        // the first step is only renamed.  Call once, before the first step:
        // the input buffers, the scheduler, the snapshot cadence and the
        // journal all take their first tick from tick_index().
        void set_tick_index(uint32_t tick) { tick_ = tick; }

        // The world's own counters that a re-step reads and the IVP savestate
        // (physics/physics_savestate.hpp) does not carry: the blob holds the
        // environment, the manager's floats, the RNG cursors as the
        // generators have them and the entity matrices, but tick() starts
        // with ivp_srand(rng_cursor_) / random_set_state(random_cursor_) - so a
        // whole-frame re-step after a restore would put the LIVE world's
        // cursors back over the restored ones - and update_sectors() runs the
        // sector union (pacer included) against tick_.  The pending flag of
        // the per-tick PreSimulate callback rides along: at a tick boundary it
        // is false (the frame's Simulate consumed the callback).
        //
        // Test and diagnostics only (BallanceMMOSimTool --savestate-check
        // records one per tick of its straight pass and puts the anchor's
        // back before every restore).  Nothing else in the world is rewound
        // by it: players, inputs, navigation replicas, proximity probes and
        // the body bookkeeping of snapshot() stay as they are, so a caller
        // must not change any of those inside the window it rewinds.  The
        // session runner never rewinds a world.
        struct step_state {
            uint32_t tick = 0;
            int rng_cursor = 0;
            int random_cursor = 0;
            bool callback_pending = false;
            bmmo::session::gated_sector_union sectors;
            // Step G2, under WORLD_PROXIMITY_UNION: the union's hold section
            // (proximity_union::capture, §4.2) - the frames where they are
            // and the by-name cells - put back by restore_step_state, so a
            // no-candidate tick of a rewound window reads the hold the
            // straight run read.  Empty with the bit clear.
            std::vector<uint8_t> proximity_hold;
        };
        step_state capture_step_state() const;
        void restore_step_state(const step_state& state);

        int level() const { return options_.level; }
        const bmmo::game::navigation_graph& navigation() const { return navigation_; }
        // Force value of a ball type (Physicalize_GameBall "Force"), 0 if unknown.
        float force_value(uint8_t ball_type) const;
        // Retail spawn matrix of the level (CurrentLevel[0,3] at the anchor).
        const VxMatrix& spawn_matrix() const { return spawn_matrix_; }
        // Every resetpoint of the level, in checkpoint order: the ResetPoints
        // array, whose row k Gameplay_Events copies into CurrentLevel[0,3]
        // when the ball takes the k-th checkpoint (row 0 is the spawn).  A
        // player's own world respawns its ball at the row of the last
        // checkpoint IT took, so any of them is a legitimate Physicalize
        // pose (design 9.4's validation).  The spawn alone when the level
        // has no such array.
        const std::vector<VxVector>& resetpoints() const { return resetpoints_; }

        // `join_order` is the player's slot when free (kMaxSlots stays 64: it
        // is also the nocoll group index and the spawn direction table's
        // per-player offset, design 9.10); a taken slot falls back to the
        // lowest free one and logs it.
        // `life` (step TRL): the life this add begins (the runner's number,
        // the journal's PLAYER record); 0 numbers it here - the n-th add of
        // the id in this world (a replay of a journal without lives).  The
        // id's trafo set is that life's: every Trafo event of the id acts on
        // the set of the id's newest life added (J3: a life's events are
        // applied inside it, and the leaver's final resets before a rejoin's
        // add).
        bool add_player(uint32_t id, uint8_t join_order, std::string& error, uint16_t life = 0);
        // The id's newest life added in this world (0: never added).
        uint16_t life_of(uint32_t id) const {
            const auto it = lives_.find(id);
            return it == lives_.end() ? 0 : it->second;
        }
        // Step M1: how many adds found their join order taken (J2: 0).
        uint64_t join_orders_taken() const { return join_orders_taken_; }
        void remove_player(uint32_t id);
        bool has_player(uint32_t id) const { return players_.count(id) != 0; }
        size_t player_count() const { return players_.size(); }
        std::vector<uint32_t> player_ids() const;
        void set_input(uint32_t id, const bmmo::session::input_frame& frame);
        bool apply_event(uint32_t id, const lifecycle_event& event, std::string& error);
        bool player_physicalized(uint32_t id) const;
        uint8_t player_ball_type(uint32_t id) const;
        // The entity of a physicalized player's ball clone ("" otherwise).
        // Read only (SimTool's --replay-session maps a resync state's rows).
        std::string player_ball_entity(uint32_t id) const;
        int player_sector(uint32_t id) const;

        bool tick(std::string& error);

        // Balls of every physicalized player and, in full snapshots, every
        // movable mechanism body (delta snapshots: only the simulated ones).
        // Mechanism indices are stable per world; names are filled for full.
        void snapshot(bool full, std::vector<bmmo::session::body_state>& out);
        // The same bodies a full snapshot carries, read only: the session
        // journal must not move the snapshot cadence or the mechanism
        // numbering by looking at the world (design 9.15).
        void snapshot_for_journal(std::vector<bmmo::session::body_state>& out);
        // Step BALA: the players whose ball entered the airship zone since
        // the last call, with the tick (the session journal notes them).
        std::vector<std::pair<uint32_t, uint32_t>> take_airship_entries() {
            std::vector<std::pair<uint32_t, uint32_t>> out;
            out.swap(airship_entries_);
            return out;
        }
        // Whether the set of movable mechanism bodies changed since the last
        // snapshot() call (a full snapshot is then due).
        bool body_set_changed() const { return body_set_changed_; }

        // The resync state of the tick just stepped, for `player` (resync
        // design 2.3, step A3): the rows of a full snapshot (numbered as the
        // snapshots number them, the body-set bookkeeping of snapshot()
        // untouched), the player's own clone's name, the sector union, the
        // portable script record of the session's selection and the IVP
        // savestate.  Nothing the world computes, its hash or its journal
        // changes (the record reads every parameter with
        // GetReadDataPtr(FALSE), the force hook reads the controller, the
        // savestate capture reads); the one write is the mechanism numbering
        // (body_index_): a body new to it is numbered now, with the number the
        // next full snapshot would have given it, so the numbering a later
        // snapshot carries may depend on whether a resync came first (never
        // the simulation or the hash).  The server never reads it back.
        struct resync_state {
            std::vector<bmmo::session::body_state> rows;
            std::vector<std::pair<uint32_t, std::string>> clones;   // every physicalized player's ball entity
            std::vector<std::pair<uint32_t, bmmo_physics_nav_state>> navs;   // every player's ball navigation
            std::vector<uint8_t> union_state, scripts, savestate;
            // The scripts and the savestate are packed (CKPackData, zlib):
            // their unpacked sizes (0 when a section is carried as it is).
            uint32_t scripts_size = 0, savestate_size = 0;
            // Trafo-replication design 4.7 (step T2): every type set that has
            // bodies at this tick, a live explosion, or a reset not yet past
            // its Restore IC (empty without trafo_inputs).
            std::vector<bmmo::session::trafo_ledger_entry> trafo_ledger;
            std::string error;   // what could not be captured ("" when all of it was)
        };
        void capture_resync_state(uint32_t player, resync_state& out);

        // The sectors the world is running: every sector any member has
        // entered (session/sector_union.hpp), each started by update_sectors()
        // before the step of the tick the shared rule picks.
        const std::set<int>& active_sectors() const { return sectors_.active(); }
        // Step B2SX: the checkpoint chain's private [0][1] (false when this
        // world did not split it); SimTool's --cells-trace.
        bool chain_sector_cell(int& cell) const { return sector_cell_.read(engine_ ? engine_->context() : nullptr, cell); }
        const bmmo::session::gated_sector_union& sectors() const { return sectors_; }
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
        // Proximity union (design 8.3; world-agreement-gaps-design.md §2.3,
        // step G1: game/proximity_union.hpp): every "TT Scaleable Proximity"
        // whose ObjectA is Ball_Pos_Frame is rewired to a private frame, and
        // every CurrentLevel[0,ActiveBall] read inside such a script to a
        // private copy of the array.  The world option's bit is clear, so the
        // union keeps the server's path (place_legacy): every tick each frame
        // goes to the player ball nearest that block's ObjectB, and each copy
        // names the player nearest to one of its script's mechanisms (the
        // retail entity of its ball type where the script tests the name -
        // the rope bridge P_Modul_29 tears only for "Ball_Stone" - its clone
        // otherwise).
        void place_proximity_legacy();
        // Step G2 (§2.2, §2.4), under WORLD_PROXIMITY_UNION: the candidates
        // - every player whose clone is physicalized, in player id order,
        // with the clone's entity name - and the union's place() from them.
        // The session runner calls it first in its step (before the leavers
        // and the events), SimTool's --replay-session before a tick's PLAYER
        // and EVENT records; tick() calls it when nobody did for its tick.
        // A no-op with the bit clear.
        void place_probes();
        std::vector<bmmo::session::probe_candidate> probe_candidates() const;
        bool proximity_union_on() const { return options_.proximity_union; }
        // Step G2's review: restore_step_state's hold restores that failed
        // (each also logged); SimTool's savestate rounds fail on a change.
        uint64_t proximity_hold_failures() const { return proximity_hold_failures_; }
        // SimTool's --probe-scramble (step G2's review): every proximity
        // frame moved by dx along x through the hold section (capture,
        // edit, restore); a no-op with the bit clear.
        void displace_probe_frames(float dx);
        size_t proximity_probe_count() const { return proximity_.probe_count(); }
        const bmmo::game::proximity_union& proximity() const { return proximity_; }
        // SimTool's --probe-census --release-check (§7.2 V5): release the
        // union, and rewire it again (`union_rules`: §2.3 under the bit, with
        // `which`; otherwise the anchor's bit-clear rewire of every block).
        int release_proximity();
        // `slots` (step Q7, V6): the per-player units copied that many times
        // (union rules only; 0 = none).  `sel` (step F1, the UGRD fixture):
        // a selection in place of the world's own.
        std::string rewire_proximity(bool union_rules, bmmo::game::probe_class which, uint8_t slots = 0,
                                     const bmmo::game::script_replay::selection* sel = nullptr);
        // Step F1 (UGRD fixture): the union's mechanism class marked again
        // from `sel` (the server's anchor marks it from its own selection).
        void classify_proximity(const bmmo::game::script_replay::selection& sel) { proximity_.classify(sel); }
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

        // Called by tick() after the sector union's pass and before the
        // engine's frame: where the trafo frame runs (trafo-replication
        // design 2.5; step T1 uses it from SimTool, step T2 from the
        // world itself).  Unset: nothing runs.
        void set_frame_hook(std::function<void()> hook) { frame_hook_ = std::move(hook); }

        // The trafo as an input (trafo-replication design, step T2), with
        // trafo_inputs on.  Every P_Trafo_* of the level and its position,
        // read at the anchor: what a client's explosion is validated against
        // (session/trafo_input.hpp trafo_validate).
        bool trafo_inputs() const { return options_.trafo_inputs; }
        const std::vector<bmmo::session::trafo_point>& trafo_points() const { return trafo_points_; }
        const bmmo::game::trafo_registry& trafo_registry() const { return trafo_registry_; }
        // Step TRL: the sets held, "p,pLn,..." in (player, life) order.
        std::string trafo_set_list() const {
            std::string out;
            for (const auto& s: trafo_registry_.sets())
                out += (out.empty() ? "" : ",") + bmmo::session::trafo_life_label(s.player, s.life);
            return out.empty() ? std::string("-") : out;
        }
        // A set's build failed its invariants at add_player (fail closed:
        // the session cannot run that member's trafo, the runner ends it).
        bool trafo_failed() const { return trafo_failed_; }
        // The server's hold fired (an own Trafo Manager acting on the parked
        // ball - a bug) or an original leaked (a body or a running script).
        uint64_t trafo_hold_reports() const { return trafo_hold_reports_; }
        uint64_t trafo_original_leaks() const { return trafo_leaks_; }

        // The sector placement (docs/sector-placement-design.md 2.4, step
        // S2): the signature of the PH table at the anchor (placement on or
        // off) and the copies this world made (placement_clones).
        uint64_t anchor_placement() const { return anchor_placement_; }
        // Step G3 (world agreement 2.3, 5): the mechanism-class signature of
        // this world's proximity rewire - the clients' anchor_probes.
        uint64_t probe_signature() const { return proximity_.signature(bmmo::game::probe_class::mechanism); }
        // Step G5: BodyRevived events this world did not apply (the world
        // option: every world wakes its own mechanisms).
        uint64_t revived_ignored() const { return revived_ignored_; }
        const bmmo::game::sector_placement& placement() const { return placement_; }

    private:
        physics_world() = default;
        // Shared by snapshot() and snapshot_for_journal(); `bookkeeping` says
        // whether the call owns the body-set flag and the mechanism numbering.
        // `number`: a body without a number gets one (the snapshots' and the
        // resync state's), never with `bookkeeping` off otherwise.
        void collect_bodies(bool full, bool bookkeeping, std::vector<bmmo::session::body_state>& out, bool number = false);
        bool boot(std::string& error);
        bool anchor(std::string& error);
        // trafo_inputs: the quiesce, the hold, the filter rule and the
        // trafo list at the anchor; a member's set at add_player; the hold,
        // the effect checks and retirement after every tick.
        bool anchor_trafo(std::string& error);
        bool make_trafo_set(uint32_t id, uint16_t life, std::string& error);
        bool apply_trafo(uint32_t id, const lifecycle_event& event, std::string& error);
        void after_trafo_tick();
        void capture_trafo_ledger(std::vector<bmmo::session::trafo_ledger_entry>& out) const;
        bool ensure_collision_filter(std::string& error);
        // Step BALA: the airship zone over this world (no own ball: every
        // player ball is a clone), installed once the level is loaded.
        void install_airship_zone();
        // Step SPGH: the spawn ghost over this world.
        void install_spawn_ghost();
        std::vector<std::pair<uint32_t, uint32_t>> airship_entries_;   // (player, tick) not yet taken
        bool airship_box_logged_ = false;
        // Step SPGH2: the spawn ghost's counters as last logged (a body that
        // came to a solid group - a trafo's new ball - and the births).
        uint64_t spawn_ghost_resumed_logged_ = 0, spawn_ghost_births_logged_ = 0;

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
            // Step BALA: the tick its ball was first found in the airship
            // zone in this life (a ghost from there: frozen, out of the
            // sync); 0 while it is not.
            uint32_t airship_tick = 0;
            // Step SPGH: its ball a spawn ghost after the last step.
            bool spawn_ghost = false;
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
        std::function<void()> frame_hook_;
        bmmo::game::navigation_graph navigation_;
        bool navigation_keys_known_ = false;
        // The mechanism Sequencer counters as the level file has them (design
        // 9.26): recorded while the level loads, written back at the anchor.
        bmmo::game::sequencer_state level_sequencers_;
        // The TT Scaleable Proximity countdowns as the level file has them
        // (session-client-core design 6.2, step Q): the same rule.
        bmmo::game::proximity_countdown_state level_proximities_;
        std::vector<ball_row> ball_rows_;             // Physicalize_GameBall rows
        VxMatrix spawn_matrix_{};
        std::vector<VxVector> resetpoints_;
        CK_ID retail_ball_ = 0;
        bool retail_parked_ = false;
        bool callback_pending_ = false;
        CK_ID sector_manager_ = 0;
        CK_ID ingame_parameter_ = 0;
        bmmo::session::gated_sector_union sectors_;   // the rule every side runs (session/sector_union.hpp)
        bmmo::game::checkpoint_phase_split checkpoint_phase_;   // the chain's own phase cell (game/checkpoint_phase.hpp)
        bmmo::game::sector_cell_split sector_cell_;   // the chain's own [0][1] (game/sector_cell_split.hpp, step B2SX)
        // Step RK3: the Level 1 tutorial's exit (resolved at the anchor under
        // WORLD_SESSION_CLOCK), and whether tick() activated it - once, never
        // again after a step-state restore (the exit is live-only).
        bmmo::game::tutorial_exit tutorial_exit_;
        // Step RK3H: the tutorial's frame-9 write held at the in-play factor
        // (made at the anchor; this world dies with its session and never
        // hands it back).
        bmmo::game::tutorial_factor_hold tutorial_hold_;
        // FULL step PRX: the level's fall handler for free props (pinned at
        // the anchor; this world dies with its session and never hands it
        // back).
        bmmo::game::depth_test_rule depth_test_;
        bool tutorial_exit_done_ = false;
        // The tick index of the world's first step after its anchor (-1:
        // none yet): the frame count is tick_ - first_step_tick_ + 1, so a
        // world numbered from a session's start base (set_tick_index) counts
        // its frames from its anchor as every client does (step RK3's
        // review: tick_ itself is the session's tick on the server).
        int64_t first_step_tick_ = -1;
        std::map<uint32_t, player> players_;
        std::set<int> used_slots_;
        uint64_t join_orders_taken_ = 0;   // step M1: add_player's slot fallbacks (J2 says 0)
        bmmo::game::proximity_union proximity_;   // step G1 (game/proximity_union.hpp)
        bmmo::game::proximity_union::rewire_options proximity_options() const;
        // Step G2: the tick place_probes() last placed for (tick() places
        // itself when this is not its tick).
        bool probes_placed_ = false;
        uint32_t probes_placed_tick_ = 0;
        uint64_t proximity_hold_failures_ = 0;
        uint64_t revived_ignored_ = 0;   // step G5
        CK_ID current_level_ = 0;
        // The session's script selection (game/session_host_support.hpp
        // session_script_selection: the selection both clients make at their
        // anchor, so the three have the same keys) and the force hook over
        // this world's controllers, made at anchor() for the resync state's
        // portable record (step A3).
        bmmo::game::script_replay::selection script_selection_;
        bmmo::game::script_replay_hooks script_hooks_;
        std::unordered_map<std::string, uint16_t> body_index_;
        std::set<std::string> last_body_set_;
        // Whether each mechanism was simulated when the last snapshot was
        // taken: a delta snapshot carries the simulated bodies only, so a body
        // that falls asleep between two of them would otherwise leave the
        // clients with the pose of its last moving tick and no word that it
        // stopped (design 9.25).  The transition awake -> asleep puts one more
        // row (simulated = 0) into the next delta snapshot.
        std::unordered_map<std::string, bool> last_simulated_;
        bool body_set_changed_ = true;
        // The trafo sets (trafo_inputs): the registry, the bridge's hooks,
        // the hold of the originals and what each type set was last given -
        // the newest explosion applied (its tick and referential) and the
        // newest reset applied after it - for the resync state's ledger.
        bmmo::game::trafo_registry trafo_registry_;
        bmmo::game::trafo_set_hooks trafo_hooks_;
        bmmo::game::trafo_hold trafo_hold_;
        std::vector<bmmo::session::trafo_point> trafo_points_;
        struct trafo_applied {
            bool explosion = false, reset = false;
            uint32_t explode_applied = 0, reset_applied = 0;
            float position[3] = {};
        };
        std::map<std::tuple<uint32_t, uint16_t, uint8_t>, trafo_applied> trafo_applied_;   // (player, life, type)
        // Step TRL: each id's newest life added here, and how many adds (a
        // life numbered here when add_player is not told one).
        std::map<uint32_t, uint16_t> lives_;
        std::map<uint32_t, uint16_t> adds_;
        uint64_t trafo_missing_sets_ = 0;   // Trafo events whose life had no set here (never built for one)
        bool trafo_failed_ = false;
        uint64_t trafo_hold_reports_ = 0, trafo_leaks_ = 0;
        uint64_t trafo_after_us_ = 0, trafo_after_ticks_ = 0;   // what the hold and the checks cost (describe)
        // The sector placement (step S2): run at the anchor after the
        // Sequencer and proximity restores, before the anchor hash.
        bool place_sectors(std::string& error);
        bmmo::game::sector_placement placement_;
        uint64_t anchor_placement_ = 0;
        void* filter_environment_ = nullptr;
        int rng_cursor_ = 0;
        int random_cursor_ = 0;   // bmmo::physics::random_* state, saved/restored around tick() like rng_cursor_
        int exact_log_ticks_ = 0;   // debug: exact core dumps after a Physicalize event
        int rng_last_seed_ = 0, rng_last_cores_ = -1;   // debug: rng / awake-body change log
        float rng_last_pdelta_ = -1.0f;                  // debug: physics pause/resume detection
    };
}
