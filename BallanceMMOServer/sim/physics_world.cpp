#include "physics_world.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include <game/sector_deactivate_pin.hpp>
#include <game/session_host_support.hpp>
#include <physics/physics_direct.hpp>
#include <physics/physics_savestate.hpp>
#include <physics/physics_state.hpp>
#include <session/spawn_impulse.hpp>

#include "trafo_hooks.hpp"
#include "live_order_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <CKTimeManager.h>

int ivp_srand_read();
void ivp_srand(int seed);

namespace bmmo::sim {
    namespace {
        // IVP nocoll group idents hold at most 7 characters, so player balls are
        // grouped by a short per-world slot ("P#0".."P#63"); entity names still
        // carry the connection id.
        constexpr const char* kPlayerGroupPrefix = "P#";
        constexpr const char* kPlayerNameTag = "_BMMO_";
        constexpr int kMaxSlots = 64;

        bool gameplay_ingame_active(CKContext* context) {
            return bmmo::game::script_active(context, "Gameplay_Ingame");
        }

        // Exact world matrix from the event: rotation rows as the client read
        // them from its entity, so the physicalize template gets identical bits.
        VxMatrix matrix_from_pose(const float position[3], const float rotation[9]) {
            VxMatrix matrix;
            matrix.SetIdentity();
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) matrix[r][k] = rotation[r * 3 + k];
            matrix[3][0] = position[0];
            matrix[3][1] = position[1];
            matrix[3][2] = position[2];
            matrix[3][3] = 1.0f;
            return matrix;
        }

        // Per-tick hook in the physics manager's PreSimulate pass: the retail
        // callback container executes a new callback immediately and keeps it
        // queued while Execute returns 0, so the first call only arms it and
        // the second (inside Simulate, after the scripts) does the work.
        class tick_callback final : public PhysicsCallback {
        public:
            tick_callback(CKIpionManager* manager, CKBehavior* behavior, physics_world* world)
                : PhysicsCallback(manager, behavior, 2), world_(world) {}
            int Execute() override {
                if (!armed_) {
                    armed_ = true;
                    return 0;
                }
                world_->pre_simulate();
                return 1;
            }
        private:
            physics_world* world_;
            bool armed_ = false;
        };
    }

    std::unique_ptr<physics_world> physics_world::create(const world_options& options, std::string& error) {
        std::unique_ptr<physics_world> world(new physics_world);
        world->options_ = options;
        if (!world->boot(error)) return nullptr;
        if (!world->anchor(error)) return nullptr;
        return world;
    }

    physics_world::~physics_world() {
        // The proximity union's frames and copies (step G1), before the
        // placement's release takes the copied blocks it may have rewired.
        if (engine_) proximity_.release(engine_->context());
        // Sector placement: see place_sectors(); the one release helper (step S10), silent here.
        if (engine_) bmmo::game::release_placement(placement_, engine_->context(), "the world is destroyed", {});
        // Players first: their navigation controllers live in the environment.
        players_.clear();
        if (engine_) bmmo::physics::set_controller_alias(engine_->physics(), {});
        engine_.reset();
    }

    void physics_world::log(const std::string& text) const {
        if (options_.log) options_.log(text);
    }

    CKIpionManager* physics_world::physics() const {
        return engine_ ? engine_->physics() : nullptr;
    }

    CKIpionManager* physics_world::manager_for_debug() const { return physics(); }

    CK3dEntity* physics_world::retail_ball() const {
        return engine_ ? CK3dEntity::Cast(engine_->context()->GetObject(retail_ball_)) : nullptr;
    }

    bool physics_world::boot(std::string& error) {
        engine_options eo;
        eo.game_root = options_.game_root;
        eo.log = options_.log;
        engine_ = headless_engine::create(eo, error);
        if (!engine_) return false;
        if (!engine_->load_composition(error)) return false;
        // The sets' force controllers in the savestate by the names every
        // world gives them (step T3's fix-up): the resync and join states'
        // blobs travel to clients that built their sets at other moments.
        if (options_.trafo_inputs)
            bmmo::physics::set_controller_alias(engine_->physics(), [this](const char* name, std::string& alias) {
                return bmmo::game::trafo_controller_alias(trafo_registry_, name, alias);
            });
        // Trafo-replication design 2.4 step 6 (T1's level reference): the 8
        // original trafo scripts' values as Balls.nmo has them, captured on
        // the first tick they exist - before any original explosion - and
        // written into every copy the world builds.
        const auto capture_trafo_reference = [this]() {
            if (!options_.trafo_inputs || !trafo_registry_.reference().empty()) return;
            bmmo::game::trafo_reference reference;
            std::string reference_error;
            if (!bmmo::game::capture_trafo_reference(engine_->context(), reference, reference_error)) return;
            size_t blocks = 0;
            for (const auto& [name, b]: reference.scripts) blocks += b.size();
            trafo_registry_.set_reference(std::move(reference));
            log("world: trafo level reference captured at engine tick " + std::to_string(engine_->ticks()) + ": 8 scripts, "
                + std::to_string(blocks) + " blocks");
        };
        capture_trafo_reference();
        for (int i = 0; i < options_.boot_ticks; ++i) {
            if (!engine_->tick(error)) return false;
            capture_trafo_reference();
        }
        bmmo::physics::drain_event_log(engine_->physics());  // install the listener
        if (options_.before_level_load) options_.before_level_load(engine_->context());   // SimTool only (step S7)
        engine_->request_level(options_.level);
        int waited = 0;
        while (!gameplay_ingame_active(engine_->context())) {
            if (!engine_->level_request().error.empty()) {
                error = "level request failed: " + engine_->level_request().error;
                return false;
            }
            if (++waited > options_.anchor_timeout) {
                error = "Gameplay_Ingame never activated";
                return false;
            }
            // Design 9.26: each mechanism Sequencer is recorded on the tick it
            // appears, before its script ran - the counter the level file has;
            // step Q: the proximity countdowns, recorded the same way.
            bmmo::game::capture_level_file_counters(engine_->context(), level_sequencers_, level_proximities_,
                                                    [this](const std::string& line) { log("world: " + line); });
            if (!engine_->tick(error)) return false;
        }
        log("world: anchor reached after " + std::to_string(waited) + " ticks (engine tick "
            + std::to_string(engine_->ticks()) + ")");
        return true;
    }

    bool physics_world::anchor(std::string& error) {
        CKContext* context = engine_->context();
        CKIpionManager* manager = engine_->physics();
        if (options_.before_anchor) options_.before_anchor(context);   // SimTool only (step S7)
        if (!bmmo::physics::reset_session_clock(manager, options_.seed, error)) return false;
        // Design 9.26: the mechanisms start from the level file's Sequencer
        // counters on every side (the boot's own first play advanced them).
        // Step Q: the proximity blocks' countdown phase is the level file's on
        // every side (the load ticks before the anchor run some of them: 13 of
        // the arena's 24 differ here).  One copy for every host (step HU1).
        bmmo::game::restore_level_file_counters(context, level_sequencers_, level_proximities_,
                                                [this](const std::string& line) { log("world: " + line); });
        if (!place_sectors(error)) return false;   // sector placement (step S2): after the restores, before the hash
        bmmo::physics::world_hash hash;
        if (!bmmo::physics::capture_world_hash(manager, hash, error)) return false;
        // The handshake compares the movable-core pose hash: the full hash
        // also covers the physics time factor, which the client's in-level
        // restart sets one tick earlier than a fresh load (harmless with no
        // movable core at the anchor).
        anchor_hash_ = hash.pose;
        anchor_surfaces_ = hash.surfaces;
        tick_ = 0;
        rng_cursor_ = ivp_srand_read();
        random_cursor_ = bmmo::physics::random_get_state();

        navigation_ = bmmo::game::read_navigation_graph(context);
        if (!navigation_.valid()) {
            error = "navigation graph: " + navigation_.error;
            return false;
        }
        if (CKDataArray* current_level = engine_->data_array("CurrentLevel")) {
            current_level_ = current_level->GetID();
            if (CKObject* ball = current_level->GetElementObject(0, 1)) retail_ball_ = ball->GetID();
            current_level->GetElementValue(0, 3, &spawn_matrix_);
        }
        resetpoints_.clear();
        if (CKDataArray* table = engine_->data_array("ResetPoints")) {
            VxMatrix row_matrix;
            for (int row = 0; row < table->GetRowCount(); ++row)
                if (table->GetElementValue(row, 0, &row_matrix))
                    resetpoints_.emplace_back(row_matrix[3][0], row_matrix[3][1], row_matrix[3][2]);
        }
        if (resetpoints_.empty()) resetpoints_.emplace_back(spawn_matrix_[3][0], spawn_matrix_[3][1], spawn_matrix_[3][2]);
        if (!retail_ball_) {
            error = "CurrentLevel[0,1] has no active ball";
            return false;
        }
        if (CKBehavior* manager_script = bmmo::game::find_root_script(context, "Gameplay_SectorManager"))
            sector_manager_ = manager_script->GetID();
        if (CKDataArray* parameters = engine_->data_array("IngameParameter"))
            ingame_parameter_ = parameters->GetID();
        if (!sector_manager_ || !ingame_parameter_) {
            error = "Gameplay_SectorManager or IngameParameter is missing";
            return false;
        }
        if (CKDataArray* table = engine_->data_array("Physicalize_GameBall")) {
            const int rows = table->GetRowCount();
            for (int row = 0; row < rows; ++row) {
                ball_row entry;
                char name[128] = {};
                table->GetElementStringValue(row, 0, name, static_cast<int>(sizeof(name)));
                name[sizeof(name) - 1] = 0;
                entry.name = name;
                table->GetElementValue(row, 1, &entry.friction);
                table->GetElementValue(row, 2, &entry.elasticity);
                table->GetElementValue(row, 3, &entry.mass);
                table->GetElementValue(row, 5, &entry.linear_damp);
                table->GetElementValue(row, 6, &entry.rot_damp);
                table->GetElementValue(row, 7, &entry.force);
                ball_rows_.push_back(entry);
            }
        }
        if (ball_rows_.empty()) {
            error = "Physicalize_GameBall table is missing";
            return false;
        }
        // These are what a client's Physicalize recipe is validated against
        // (design 9.4), so the log has to say what the level actually holds.
        for (size_t row = 0; row < ball_rows_.size(); ++row) {
            const ball_row& b = ball_rows_[row];
            char text[256];
            std::snprintf(text, sizeof(text),
                "world: ball type %zu = %s (friction %.4f elasticity %.4f mass %.4f linear damp %.4f rot damp %.4f force %.4f)",
                row, b.name.c_str(), b.friction, b.elasticity, b.mass, b.linear_damp, b.rot_damp, b.force);
            log(text);
        }
        sectors_.reset();
        if (!ensure_collision_filter(error)) return false;
        install_airship_zone();
        install_spawn_ghost();
        // The proximity union (step G1, game/proximity_union.hpp): every
        // block, class `all`.  Before the selection, as the rewire always ran
        // (the CK objects it creates keep their place in the creation order);
        // classified after it.  With the world option's bit clear the
        // server's path; under WORLD_PROXIMITY_UNION (step G2) the union
        // rules (§2.3), all or nothing: a world that cannot rewire cannot
        // keep P1, so its anchor fails.
        {
            auto union_options = proximity_options();
            union_options.union_rules = options_.proximity_union;
            proximity_.has_body = [this](CK3dEntity* entity) {
                CKIpionManager* manager = physics();
                PhysicsObject* object = manager && entity ? manager->GetPhysicsObject(entity) : nullptr;
                return object && object->m_RealObject;
            };
            const std::string union_error =
                proximity_.rewire(context, bmmo::game::probe_class::all, {}, union_options);
            for (const std::string& line: proximity_.lines()) log("world: " + line);
            // Only under the bit: the bit-clear path never failed its anchor
            // for the rewire (step G1), and keeps not failing it.
            if (options_.proximity_union && !union_error.empty()) {
                error = "proximity union (world option) refused: " + union_error;
                return false;
            }
        }
        // Step A3 (resync design 2.3): the selection the clients make at
        // their anchor (trafo scripts included, so the three have the same
        // keys) and the force hook over this world, for the resync state's
        // portable record.  Selecting reads the graphs; it changes nothing.
        if (options_.trafo_inputs && !anchor_trafo(error)) return false;
        script_selection_ = bmmo::game::session_script_selection(context, options_.trafo_inputs);
        proximity_.classify(script_selection_);
        {
            char text[192];
            std::snprintf(text, sizeof(text), "world: proximity union: %zu blocks (mechanism %zu), %zu identity reads "
                          "(mechanism %zu), signature mechanism %016llx all %016llx, bit %s",
                          proximity_.probe_count(), proximity_.mechanism_probe_count(), proximity_.identity_count(),
                          proximity_.mechanism_identity_count(),
                          static_cast<unsigned long long>(proximity_.signature(bmmo::game::probe_class::mechanism)),
                          static_cast<unsigned long long>(proximity_.signature(bmmo::game::probe_class::all)),
                          options_.proximity_union ? "set" : "clear");
            log(text);
        }
        script_hooks_ = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(manager));   // step HU1: one copy (game/host_backend.hpp)
        // The level's own checkpoint activation, held (game/sector_activation.hpp):
        // update_sectors() is the only path that runs Gameplay_SectorManager in
        // this world.  The rewired proximity probes let any player's clone fire
        // a checkpoint, and the chain then ran the manager at the tick the
        // server's clone got there - no client activates at that tick - and
        // deactivated the sector it left, whoever was still in it.  Measured: a
        // retail crossing put the server's sector-2 bodies 14 ticks ahead of
        // the client's and took sector 1's away at the crossing.
        bool held = false;
        {
            std::string hold_error;
            held = bmmo::game::hold_checkpoint_activation(engine_->context(), hold_error) != nullptr;
            if (held)
                log("world: the level's checkpoint sector activation is held; update_sectors() alone runs the sector manager");
            else
                log("world: the level's checkpoint sector activation was NOT held (" + hold_error
                    + "): a checkpoint a clone crosses activates a sector behind update_sectors()");
        }
        // N2 (step B2SW, game/sector_deactivate_pin.hpp): the held chains'
        // deactivate cell pinned to 0, as the retail mod does, so a clone's
        // crossing in the frame of an update_sectors pass deactivates nothing
        // here either.  Pinned once: this world is booted for its session and
        // destroyed with it, and no code of it hands an input back (the
        // checkpoint phase split below is applied once for the same reason).
        // Off only for SimTool's A/B (--n2-pin off).
        if (options_.pin_checkpoint_deactivation) {
            std::string report;
            const int pinned = bmmo::game::pin_checkpoint_deactivation(engine_->context(), report);
            log("world: the chains' sector deactivation (IngameParameter[0][2]) pinned to 0 in " + std::to_string(pinned)
                + " of 2 (" + report + ")");
        } else {
            log("world: the chains' sector deactivation is NOT pinned (--n2-pin off)");
        }
        // The chain behind the hold keeps writing CurrentLevel[0][4], the cell
        // the modules of a pass branch on in their first frame; a clone's
        // crossing three ticks after a pass would cancel that pass's
        // Physicalize in this world (game/checkpoint_phase.hpp).
        {
            const std::string trouble = checkpoint_phase_.apply(engine_->context());
            if (trouble.empty())
                log("world: the checkpoint chain's phase cell is split: " + checkpoint_phase_.describe());
            else
                log("world: the checkpoint chain's phase cell was NOT split (" + trouble
                    + "): a clone's crossing 3 ticks after a sector pass can cancel that pass's module Physicalize");
        }
        // Step B2SX (game/sector_cell_split.hpp): the held chain's [0][1]
        // write goes to a private cell, so a clone's crossing of sector K in
        // the frame of an update_sectors pass of J cannot make the manager
        // activate K here.  Once, like the phase split (nothing hands it back);
        // only where the hold is in force (otherwise the chain runs the
        // manager, which reads the chain's cell).  Off only for SimTool's A/B.
        if (options_.split_sector_cell && held) {
            const std::string trouble = sector_cell_.apply(engine_->context());
            if (trouble.empty())
                log("world: the checkpoint chain's sector cell is split: " + sector_cell_.describe());
            else
                log("world: the checkpoint chain's sector cell was NOT split (" + trouble
                    + "): a clone's crossing in the frame of a sector pass can make the manager activate the crossed sector");
        } else {
            log(std::string("world: the checkpoint chain's sector cell is NOT split (")
                + (held ? "--n2-cell off" : "the activation is not held") + ")");
        }
        {
            // Hex, like every other place these two are printed (the client's
            // anchor line, SessionEnd's mismatch reason, the sim tool): the
            // whole point of the line is to be compared against those.
            char text[192];
            std::snprintf(text, sizeof(text), "world: anchored, hash=%016llx surfaces=%016llx leaves=%zu balls=%zu",
                static_cast<unsigned long long>(anchor_hash_),
                static_cast<unsigned long long>(anchor_surfaces_),
                navigation_.leaves.size(), ball_rows_.size());
            log(text);
        }
        {
            char text[512];
            std::snprintf(text, sizeof(text),
                "world: anchor state: cktime=%.3f dt=%.4f frames=%d cores=%d ivp_time=%.6f seed=%d mc=%d psi=%.6f/%.6f delta=%.4f pdelta=%.6f factor=%.6f movable=%s",
                engine_->time_manager()->GetTime(), engine_->time_manager()->GetLastDeltaTime(),
                engine_->time_manager()->GetMainTickCount(), hash.cores, hash.ivp_time, hash.ivp_seed, static_cast<int>(hash.next_movement_check),
                hash.time_of_last_psi, hash.time_of_next_psi, hash.delta_time_ms, hash.physics_delta_time,
                hash.time_factor, bmmo::physics::describe_movable_objects(manager).c_str());
            log(text);
            log("world: anchor bodies: " + bmmo::physics::describe_physics_objects(manager).substr(0, 900));
        }
        return true;
    }

    bool physics_world::ensure_collision_filter(std::string& error) {
        CKIpionManager* manager = physics();
        IVP_Environment* environment = manager ? manager->GetEnvironment() : nullptr;
        if (!environment) return true;  // nothing to filter yet
        if (filter_environment_ == environment) return true;
        if (!bmmo::physics::install_player_collision_filter(manager, kPlayerGroupPrefix, error)) return false;
        filter_environment_ = environment;
        return true;
    }

    // Step BALA: the airship zone (physics_state.hpp airship_zone_set) with
    // no own ball - the server's airship collides with no ball, and every
    // clone that enters the zone is frozen there as a ghost.  The server
    // never applies a relayed ghost pose: it only forwards it.
    void physics_world::install_airship_zone() {
        airship_box_logged_ = false;   // a new level: its box is logged when taken
        bmmo::physics::airship_zone_box box;
        std::string error;
        if (!bmmo::physics::airship_zone_set(physics(), true, "", box, error)) {
            log("world: airship zone: " + error);
            return;
        }
        if (!box.active) {
            log("world: airship zone: the level has no airship");
            return;
        }
        log("world: airship zone on (the box is taken once the platform has its body)");
    }

    // Step SPGH: the spawn ghost (physics_state.hpp spawn_ghost_set), the
    // same rule as every client's: a clone is born a ghost and turns solid
    // once it left every spawn area and overlaps no other ball.
    void physics_world::install_spawn_ghost() {
        spawn_ghost_resumed_logged_ = spawn_ghost_births_logged_ = 0;   // spawn_ghost_set restarts the counters
        bmmo::physics::spawn_ghost_info info;
        std::string error;
        if (!bmmo::physics::spawn_ghost_set(physics(), true, bmmo::session::kSpawnGhostRespawns,
                                            bmmo::session::kSpawnGhostRadius, info, error)) {
            log("world: spawn ghost: " + error);
            return;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "world: spawn ghost on: %u spawn areas of %.3f m, respawns %s", info.areas, info.radius,
                      bmmo::session::kSpawnGhostRespawns ? "included" : "excluded");
        log(text);
    }

    bool physics_world::park_retail_ball() {
        CKIpionManager* manager = physics();
        CK3dEntity* ball = retail_ball();
        if (!manager || !ball) return false;
        PhysicsObject* object = manager->GetPhysicsObject(ball);
        if (!object || !object->m_RealObject) return false;
        if (options_.auto_clone_players) {
            // Debug replay: every player's clone appears where and when the
            // retail ball did, with the retail recipe (still before the PSI);
            // a retail body appearing again (respawn, trafo) replaces the clone.
            const int type = ball_type_of(ball->GetName() ? ball->GetName() : "");
            for (auto& [id, p]: players_) {
                if (p.ball == ball || type < 0) continue;
                std::string error;
                if (p.physicalized && p.ball) {
                    if (p.navigation) p.navigation->shutdown_all();
                    bmmo::physics::unphysicalize(manager, p.ball->GetName(), error);
                    p.physicalized = false;
                }
                CK3dEntity* clone = clone_ball(p, static_cast<uint8_t>(type), error);
                if (!clone) { log("world: auto clone failed: " + error); continue; }
                clone->SetWorldMatrix(ball->GetWorldMatrix());
                const bmmo_physics_ball_recipe recipe = retail_recipe(static_cast<uint8_t>(type));
                if (!bmmo::physics::physicalize(manager, clone->GetName(), recipe, p.group.c_str(), error)) {
                    log("world: auto clone physicalize failed: " + error);
                    continue;
                }
                p.ball = clone;
                p.ball_type = static_cast<uint8_t>(type);
                p.physicalized = true;
                if (p.navigation) {
                    p.navigation->set_ball(clone);
                    p.navigation->set_force_value(force_value(p.ball_type));
                }
                body_set_changed_ = true;
                log("world: auto clone for player " + std::to_string(id) + " physicalized at tick " + std::to_string(tick_));
            }
        }
        if (options_.park_retail_ball) {
            std::string ignored;
            bmmo::physics::unphysicalize(manager, ball->GetName(), ignored);
            if (!retail_parked_) log("world: retail ball parked (body removed) at tick " + std::to_string(tick_));
        }
        retail_parked_ = true;
        return true;
    }

    // The union's options with the world option's bit clear (step G1): the
    // server's path, with the reset point and the ball names the signature
    // and the union rules take.
    bmmo::game::proximity_union::rewire_options physics_world::proximity_options() const {
        bmmo::game::proximity_union::rewire_options o;
        if (!resetpoints_.empty()) {
            o.reset[0] = resetpoints_.front().x;
            o.reset[1] = resetpoints_.front().y;
            o.reset[2] = resetpoints_.front().z;
        }
        for (const ball_row& row: ball_rows_) o.ball_names.push_back(row.name);
        return o;
    }

    // Before the scripts of a tick run: the union's bit-clear path (what
    // update_proximity_probes / update_ball_identity_reads did, moved into
    // game/proximity_union.hpp at step G1): the physicalized player clones in
    // id order, each with the retail entity of its ball type; with none, the
    // frames go to Ball_Pos_Frame and the copies name the parked retail ball.
    void physics_world::place_proximity_legacy() {
        if (!proximity_.probe_count() && !proximity_.identity_count()) return;
        std::vector<bmmo::game::proximity_union::legacy_ball> balls;
        for (const auto& [id, p]: players_)
            if (p.physicalized && p.ball) balls.push_back({p.ball, retail_ball_entity(p.ball_type)});
        proximity_.place_legacy(engine_->context(), balls, retail_ball());
    }

    // Step G2 (§2.2): the candidates of the coming tick - every player
    // whose clone has a body at the end of the previous one, in player id
    // order (players_ is a std::map), with the clone's entity name.
    std::vector<bmmo::session::probe_candidate> physics_world::probe_candidates() const {
        std::vector<bmmo::session::probe_candidate> candidates;
        for (const auto& [id, p]: players_)
            if (p.physicalized && p.ball && p.ball->GetName()) candidates.push_back({id, p.ball->GetName()});
        return candidates;
    }

    // Step G2 (§2.4): the union's place() from the candidates, before
    // anything of the tick (the runner's leavers and events, the sector
    // pass, the behaviour pass).  Once per tick: tick() places only when
    // this did not run for its tick.
    void physics_world::place_probes() {
        if (!options_.proximity_union) return;
        probes_placed_ = true;
        probes_placed_tick_ = tick_;
        if (!proximity_.probe_count() && !proximity_.identity_count()) return;
        proximity_.place(engine_->context(), probe_candidates());
    }

    void physics_world::displace_probe_frames(float dx) {
        if (!options_.proximity_union) return;
        std::vector<uint8_t> hold;
        proximity_.capture(hold);
        if (hold.size() < 2) return;
        // §4.2's layout: u16 frames, each f32 x, y, z (little endian).
        const size_t frames = static_cast<size_t>(hold[0] | (hold[1] << 8));
        for (size_t f = 0; f < frames && 2 + f * 12 + 4 <= hold.size(); ++f) {
            uint8_t* x = hold.data() + 2 + f * 12;
            uint32_t u = 0;
            for (int s = 0; s < 4; ++s) u |= static_cast<uint32_t>(x[s]) << (8 * s);
            float v;
            std::memcpy(&v, &u, 4);
            v += dx;
            std::memcpy(&u, &v, 4);
            for (int s = 0; s < 4; ++s) x[s] = static_cast<uint8_t>(u >> (8 * s));
        }
        std::string error;
        if (!proximity_.restore(engine_->context(), hold.data(), hold.size(), error))
            log("world: --probe-scramble: " + error);
    }

    int physics_world::release_proximity() {
        return proximity_.release(engine_->context());
    }

    std::string physics_world::rewire_proximity(bool union_rules, bmmo::game::probe_class which) {
        auto options = proximity_options();
        options.union_rules = union_rules;
        const std::string error = proximity_.rewire(engine_->context(), which, script_selection_, options);
        for (const std::string& line: proximity_.lines()) log("world: " + line);
        return error;
    }

    bool physics_world::attach_player_to_retail_ball(uint32_t id, std::string& error) {
        auto it = players_.find(id);
        if (it == players_.end()) {
            error = "unknown player";
            return false;
        }
        CK3dEntity* ball = retail_ball();
        CK3dEntity* direction_ref = retail_direction_ref();
        if (!ball || !direction_ref) {
            error = "retail ball or Cam_OrientRef missing";
            return false;
        }
        player& p = it->second;
        const int type = ball_type_of(ball->GetName() ? ball->GetName() : "");
        p.ball = ball;
        p.ball_type = type < 0 ? 0 : static_cast<uint8_t>(type);
        p.physicalized = true;
        // The direction reference is the player's own camera frame, fed from
        // the input of every tick (the previous tick's Cam_OrientRef), exactly
        // like a networked player.
        p.navigation = std::make_unique<bmmo::physics::player_navigation>(engine_->context(), physics(), ball, p.cam_ref,
                                                                          navigation_.leaves, force_value(p.ball_type));
        return true;
    }

    bool physics_world::retail_navigation_active() const {
        CKContext* context = engine_->context();
        for (const auto& leaf: navigation_.leaves)
            if (auto* key_event = CKBehavior::Cast(context->GetObject(leaf.key_block)))
                if (key_event->IsActive()) return true;
        return false;
    }

    CK3dEntity* physics_world::retail_direction_ref() const {
        return CK3dEntity::Cast(engine_->context()->GetObject(navigation_.direction_ref));
    }

    int physics_world::ball_type_of(const std::string& entity_name) const {
        for (size_t i = 0; i < ball_rows_.size(); ++i)
            if (ball_rows_[i].name == entity_name) return static_cast<int>(i);
        return -1;
    }

    // "physicalize new Ball" (Gameplay_Ingame): Paper is a convex hull of its
    // mesh, Wood and Stone are spheres of radius 2; the rest comes from the
    // Physicalize_GameBall row; mass centre explicitly (0,0,0).  Copied field
    // by field by session_client.cpp's fill_retail_recipe and SimTool's
    // --ball-off (sim_tool.cpp, eg0::ball_off): change them together.
    bmmo_physics_ball_recipe physics_world::retail_recipe(uint8_t ball_type) const {
        bmmo_physics_ball_recipe recipe{};
        if (ball_type >= ball_rows_.size()) return recipe;
        const ball_row& row = ball_rows_[ball_type];
        recipe.fixed = false;
        recipe.start_frozen = false;
        recipe.enable_collision = true;
        recipe.calc_mass_center = false;
        recipe.friction = row.friction;
        recipe.elasticity = row.elasticity;
        recipe.mass = row.mass;
        recipe.linear_damp = row.linear_damp;
        recipe.rot_damp = row.rot_damp;
        const std::string mesh = row.name + "_Mesh";
        std::snprintf(recipe.collision_surface, sizeof(recipe.collision_surface), "%s", mesh.c_str());
        if (row.name == "Ball_Paper") {
            recipe.convex_count = 1;
            std::snprintf(recipe.convex[0], sizeof(recipe.convex[0]), "%s", mesh.c_str());
        } else {
            recipe.ball_count = 1;
            recipe.ball_radius[0] = 2.0f;
        }
        return recipe;
    }

    float physics_world::force_value(uint8_t ball_type) const {
        return ball_type < ball_rows_.size() ? ball_rows_[ball_type].force : 0.0f;
    }

    std::string physics_world::ball_name(uint8_t ball_type) const {
        return ball_type < ball_rows_.size() ? ball_rows_[ball_type].name : std::string();
    }

    std::vector<uint32_t> physics_world::player_ids() const {
        std::vector<uint32_t> ids;
        for (const auto& [id, _]: players_) ids.push_back(id);
        return ids;
    }

    CK3dEntity* physics_world::retail_ball_entity(uint8_t ball_type) const {
        const std::string name = ball_name(ball_type);
        if (name.empty()) return nullptr;
        return CK3dEntity::Cast(engine_->context()->GetObjectByNameAndClass(
            const_cast<CKSTRING>(name.c_str()), CKCID_3DOBJECT, nullptr));
    }

    CK3dEntity* physics_world::clone_ball(player& p, uint8_t ball_type, std::string& error) {
        auto it = p.balls.find(ball_type);
        if (it != p.balls.end()) return it->second;
        const std::string name = ball_name(ball_type);
        if (name.empty()) {
            error = "unknown ball type " + std::to_string(ball_type);
            return nullptr;
        }
        CKContext* context = engine_->context();
        auto* source = CK3dEntity::Cast(context->GetObjectByNameAndClass(
            const_cast<CKSTRING>(name.c_str()), CKCID_3DOBJECT, nullptr));
        if (!source) {
            error = "ball entity " + name + " not found";
            return nullptr;
        }
        // Share the mesh, copy no scripts: the clone is a bare physics body.
        CKDependencies dependencies;
        dependencies.Resize(40);
        dependencies.Fill(0);
        dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
        dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
        const std::string suffix = kPlayerNameTag + std::to_string(p.id);
        auto* clone = CK3dEntity::Cast(context->CopyObject(source, &dependencies, const_cast<CKSTRING>(suffix.c_str())));
        if (!clone) {
            error = "CopyObject failed for " + name;
            return nullptr;
        }
        p.balls[ball_type] = clone;
        return clone;
    }

    bool physics_world::add_player(uint32_t id, uint8_t join_order, std::string& error) {
        if (players_.count(id)) return true;
        player p;
        p.id = id;
        int slot = join_order;
        if (slot >= kMaxSlots || used_slots_.count(slot)) {
            const int wanted = slot;
            slot = 0;
            while (slot < kMaxSlots && used_slots_.count(slot)) ++slot;
            if (slot >= kMaxSlots) {
                error = "no free player slot";
                return false;
            }
            log("world: join order " + std::to_string(wanted) + " for player " + std::to_string(id)
                + " is taken, falling back to slot " + std::to_string(slot));
        }
        p.slot = slot;
        p.group = kPlayerGroupPrefix + std::to_string(slot);
        CKContext* context = engine_->context();
        const std::string cam_name = "CamRef" + std::string(kPlayerNameTag) + std::to_string(id);
        p.cam_ref = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(cam_name.c_str())));
        if (!p.cam_ref) {
            error = "cannot create the camera reference frame";
            return false;
        }
        if (CK3dEntity* direction_ref = CK3dEntity::Cast(context->GetObject(navigation_.direction_ref)))
            p.cam_ref->SetWorldMatrix(direction_ref->GetWorldMatrix());
        p.navigation = std::make_unique<bmmo::physics::player_navigation>(
            context, physics(), nullptr, p.cam_ref, navigation_.leaves,
            navigation_.leaves.empty() ? 0.0f : navigation_.leaves.front().force_value);
        used_slots_.insert(slot);
        players_.emplace(id, std::move(p));
        log("world: player " + std::to_string(id) + " added");
        // Trafo-replication design 2.4: every member's own piece set, built
        // at its add (a set kept from an earlier stay is selected again).
        if (options_.trafo_inputs && !make_trafo_set(id, error)) {
            const std::string why = error;
            remove_player(id);
            error = why;
            return false;
        }
        return true;
    }

    void physics_world::remove_player(uint32_t id) {
        auto it = players_.find(id);
        if (it == players_.end()) return;
        player& p = it->second;
        if (p.navigation) p.navigation->shutdown_all();
        p.navigation.reset();
        CKIpionManager* manager = physics();
        CKContext* context = engine_->context();
        for (auto& [type, entity]: p.balls) {
            if (!entity) continue;
            std::string ignored;
            if (manager && manager->GetPhysicsObject(entity))
                bmmo::physics::unphysicalize(manager, entity->GetName(), ignored);
            context->DestroyObject(entity);
        }
        if (p.cam_ref) context->DestroyObject(p.cam_ref);
        // Step SPGH2: a leave ends the group's life (physics_state.hpp
        // spawn_ghost_forget): the next member that takes this join order
        // begins its first life as a ghost.
        if (manager && !p.group.empty()) bmmo::physics::spawn_ghost_forget(manager, p.group.c_str());
        used_slots_.erase(p.slot);
        players_.erase(it);
        body_set_changed_ = true;
        log("world: player " + std::to_string(id) + " removed");
    }

    void physics_world::set_input(uint32_t id, const bmmo::session::input_frame& frame) {
        auto it = players_.find(id);
        if (it == players_.end()) return;
        const uint8_t nav_mask = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        if (options_.trace && it->second.have_input && (it->second.input.keys != frame.keys
                                      || (it->second.input.flags & nav_mask) != (frame.flags & nav_mask))) {
            exact_log_ticks_ = 24;   // debug: exact dumps around an input edge
            char text[256];
            std::snprintf(text, sizeof(text), "world: input edge at tick %u keys=%u flags=%u cam=%a,%a,%a|%a,%a,%a",
                          tick_, frame.keys, frame.flags, static_cast<double>(frame.cam_right[0]),
                          static_cast<double>(frame.cam_right[1]), static_cast<double>(frame.cam_right[2]),
                          static_cast<double>(frame.cam_dir[0]), static_cast<double>(frame.cam_dir[1]),
                          static_cast<double>(frame.cam_dir[2]));
            log(text);
        }
        it->second.input = frame;
        it->second.have_input = true;
    }

    bool physics_world::player_physicalized(uint32_t id) const {
        auto it = players_.find(id);
        return it != players_.end() && it->second.physicalized;
    }

    uint8_t physics_world::player_ball_type(uint32_t id) const {
        auto it = players_.find(id);
        return it != players_.end() ? it->second.ball_type : 0;
    }

    std::string physics_world::player_ball_entity(uint32_t id) const {
        auto it = players_.find(id);
        if (it == players_.end() || !it->second.physicalized || !it->second.ball || !it->second.ball->GetName()) return {};
        return it->second.ball->GetName();
    }

    int physics_world::player_sector(uint32_t id) const {
        auto it = players_.find(id);
        return it != players_.end() ? it->second.sector : 0;
    }

    // The sector union (session/sector_union.hpp, which has the rule and its
    // reasons).  Every sector any member has entered runs here, each started
    // by one pass of Gameplay_SectorManager - deactivate nothing, activate the
    // sector, run the manager from its start - at the tick the shared rule
    // picks: after the tick's events, before its step, one pass per idle
    // manager, the idle count running every tick.  The clients run the same
    // rule on the same Sector events, so a sector's mechanisms are born on
    // the same tick on every side.  Nothing is deactivated.
    void physics_world::update_sectors() {
        CKContext* context = engine_->context();
        auto* parameters = CKDataArray::Cast(context->GetObject(ingame_parameter_));
        auto* manager_script = CKBehavior::Cast(context->GetObject(sector_manager_));
        if (!parameters || !manager_script) return;
        const bool walk_open = sectors_.walk().seen;
        const auto pass = sectors_.tick(tick_, manager_script->IsActive() != FALSE);
        // The level-walk gate opening (sector_union.hpp, step S4): the tick
        // every side logs alike.
        if (!walk_open && sectors_.walk().seen)
            log(std::string("world: level walk ") + (sectors_.walk_open().timed_out ? "timeout" : "seen") + " at tick "
                + std::to_string(sectors_.walk_open().tick));
        if (!pass) return;
        bmmo::game::run_sector_pass(context, parameters, manager_script, pass.sector);   // step HU1: one copy
        body_set_changed_ = true;
        std::string text = "world: sector +" + std::to_string(pass.sector) + " at tick " + std::to_string(tick_)
            + " (player " + std::to_string(pass.player) + ", stamped " + std::to_string(pass.stamped_tick) + "), running:";
        for (int sector: sectors_.active()) text += " " + std::to_string(sector);
        if (sectors_.pass_pending()) text += " (more to come)";
        log(text);
    }

    bool physics_world::apply_event(uint32_t id, const lifecycle_event& event, std::string& error) {
        using bmmo::session::event_type;
        error.clear();
        // A Trafo event acts on the player's piece set, which outlives the
        // player's slot (a leaver's final resets fade after it left).
        if (event.type == event_type::Trafo) return apply_trafo(id, event, error);
        auto it = players_.find(id);
        if (it == players_.end()) {
            error = "unknown player";
            return false;
        }
        player& p = it->second;
        CKIpionManager* manager = physics();
        switch (event.type) {
        case event_type::Physicalize: {
            CK3dEntity* clone = clone_ball(p, event.ball_type, error);
            if (!clone) return false;
            if (p.ball && p.ball != clone && manager->GetPhysicsObject(p.ball)) {
                std::string ignored;
                bmmo::physics::unphysicalize(manager, p.ball->GetName(), ignored);
            }
            clone->SetWorldMatrix(matrix_from_pose(event.position, event.rotation));
            if (!ensure_collision_filter(error)) return false;
            // Step SPGH2: a spawn birth begins the ball's spawn ghost life;
            // a trafo's or a beam's Physicalize (no SPAWN flag) keeps it.
            if (event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN)
                bmmo::physics::spawn_ghost_birth(manager, p.group.c_str());
            if (!bmmo::physics::physicalize(manager, clone->GetName(), event.recipe, p.group.c_str(), error))
                return false;
            bmmo::physics::hull_probe(manager, clone->GetName(), "clone_made");
            p.ball = clone;
            p.ball_type = event.ball_type;
            p.physicalized = true;
            if (options_.trace) exact_log_ticks_ = 12;
            {
                char text[256];
                std::snprintf(text, sizeof(text), "world: physicalize %s at tick %u pos=%a,%a,%a rows=%a,%a,%a|%a,%a,%a|%a,%a,%a",
                    clone->GetName(), tick_, event.position[0], event.position[1], event.position[2],
                    event.rotation[0], event.rotation[1], event.rotation[2], event.rotation[3], event.rotation[4],
                    event.rotation[5], event.rotation[6], event.rotation[7], event.rotation[8]);
                log(text);
            }
            if (p.navigation) {
                p.navigation->set_ball(clone);
                p.navigation->set_force_value(force_value(event.ball_type));
            }
            body_set_changed_ = true;
            if ((event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && options_.spawn_impulse > 0.0f) {
                const uint32_t idx = bmmo::session::spawn_direction_index(
                        static_cast<int32_t>(options_.seed), static_cast<uint8_t>(p.slot), event.tick);
                std::string impulse_error;
                if (!bmmo::physics::push_impulse(manager, clone->GetName(), bmmo::session::kSpawnDirectionTable[idx],
                            options_.spawn_impulse, 0, impulse_error))
                    log("world: spawn impulse for player " + std::to_string(id) + " failed: " + impulse_error);
                else {
                    char text[160];
                    std::snprintf(text, sizeof(text), "world: spawn impulse index=%u speed=%.3f for player %u at tick %u",
                            idx, static_cast<double>(options_.spawn_impulse), id, event.tick);
                    log(text);
                }
                bmmo::physics::hull_probe(manager, clone->GetName(), "clone_impulse");
            }
            return true;
        }
        case event_type::Unphysicalize: {
            if (p.navigation) p.navigation->shutdown_all();
            if (p.ball && manager->GetPhysicsObject(p.ball)) {
                if (!bmmo::physics::unphysicalize(manager, p.ball->GetName(), error)) return false;
            }
            p.physicalized = false;
            body_set_changed_ = true;
            return true;
        }
        case event_type::Sector:
            // A sector this member entered: the union grows by it, and the
            // pass that starts it runs in update_sectors() under the shared
            // rule (session/sector_union.hpp).  Handed over at the tick the
            // runner dequeued the event at - the stamp, or later for an event
            // that arrived late - which is the tick the journal files it under.
            //
            // Deliberately the one event that does NOT mark body_set_changed_.
            // Asking for a full snapshot here is tempting - a player entering a
            // sector another player is already in leaves the union unchanged,
            // so update_sectors() returns early and nothing asks for one, at
            // exactly the tick that player's client starts the sector's
            // mechanisms from their IC pose.  It was tried and measured: on
            // deterministic runs the mechanism errors come out byte-identical
            // with and without it, because the leading ball keeps those
            // mechanisms awake and their rows are in every delta snapshot
            // anyway.  See docs/mechanism-phase-at-sector-activation.md before
            // adding it back.
            sectors_.request(id, event.sector, event.tick);
            p.sector = event.sector;
            return true;
        case event_type::Finish:
            p.finished = true;
            return true;
        case event_type::BodyRevived: {
            // Step G5 (world agreement 2.6): under WORLD_PROXIMITY_UNION no
            // world changes a body because another world's script woke it -
            // the live server never submits one (server.cpp drops them), and
            // a journal's replay applies none.
            if (options_.proximity_union) {
                ++revived_ignored_;
                return true;
            }
            auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(event.name.c_str()), CKCID_3DENTITY, nullptr));
            if (!entity) {
                error = "no 3D entity named " + event.name;
                return false;
            }
            PhysicsObject* object = manager->GetPhysicsObject(entity, FALSE);
            // Not physicalized yet: the retail script physicalizes it during
            // this tick on this side as well, and a fresh body starts awake.
            if (!object || !object->m_RealObject) return true;
            object->m_RealObject->ensure_in_simulation();
            return true;
        }
        default:
            error = "unknown event type";
            return false;
        }
    }

    bool physics_world::tick(std::string& error) {
        ivp_srand(rng_cursor_);
        bmmo::physics::random_set_state(random_cursor_);
        CKContext* context = engine_->context();
        // The Key Event bindings and force values are outputs of
        // Gameplay_Refresh, which runs a few frames after the anchor: re-read
        // the graph until every leaf has a key.
        if (!navigation_keys_known_) {
            bmmo::game::navigation_graph refreshed = bmmo::game::read_navigation_graph(context);
            bool complete = refreshed.valid();
            for (const auto& leaf: refreshed.leaves) complete = complete && leaf.key != 0;
            if (complete) {
                navigation_ = refreshed;
                navigation_keys_known_ = true;
                std::string keys;
                for (const auto& leaf: navigation_.leaves) keys += std::to_string(leaf.key) + ",";
                log("world: navigation keys known at tick " + std::to_string(tick_) + ": " + keys);
            }
        }
        // The proximity union: under WORLD_PROXIMITY_UNION placed by the
        // caller before the tick's events (place_probes, step G2) - here only
        // when nobody did for this tick (SimTool's scenes); with the bit
        // clear the server's path, here, after the events (step G1).
        if (options_.proximity_union) {
            if (!probes_placed_ || probes_placed_tick_ != tick_) place_probes();
        } else {
            place_proximity_legacy();
        }
        update_sectors();
        // The trafo frame (design 2.5): after the tick's inputs and the
        // union's pass, before the behaviour pass and the step - every
        // selected set's scripts, sets by player id, with the fixed delta.
        if (options_.trafo_inputs) bmmo::game::trafo_frame(context, trafo_registry_, kFixedDeltaMs);
        if (frame_hook_) frame_hook_();
        // Navigation edges and parking run from the PreSimulate pass (see
        // tick_callback); one callback at a time in case a tick had no
        // physics step.
        CKIpionManager* manager = physics();
        if (!callback_pending_ && manager && manager->m_PreSimulateCallbacks)
            if (auto* behavior = CKBehavior::Cast(context->GetObject(navigation_.ball_navigation))) {
                manager->m_PreSimulateCallbacks->Process(new tick_callback(manager, behavior, this));
                callback_pending_ = true;
            }
        if (!engine_->tick(error)) return false;
        if (live_order_trace_on()) {
            // Step RO2's measurement trace (live_order_trace.hpp; inert when
            // BMMO_TRACE_LIVE_ORDER is absent): this live frame's script
            // order and the movable list, logged on a change.
            static std::unordered_map<const void*, std::pair<std::vector<uint32_t>, std::string>> last;
            auto& [order_was, movable_was] = last[this];
            std::vector<uint32_t> order = bmmo::game::script_replay::live_frame_order(context, script_selection_);
            if (order != order_was) {
                log("live-order t=" + std::to_string(tick_) + " " + live_order_names(context, order));
                order_was = std::move(order);
            }
            std::string movable = movable_order_names(bmmo::physics::describe_movable_objects(physics()));
            if (movable != movable_was) {
                log("movable-order t=" + std::to_string(tick_) + " " + movable);
                movable_was = std::move(movable);
            }
        }
        if (options_.park_retail_ball && retail_parked_) park_retail_ball();
        if (options_.trafo_inputs) after_trafo_tick();
        if (options_.mirror_clone_to_retail) {
            // The retail entity (no body) follows the first physicalized clone.
            if (CK3dEntity* retail = retail_ball())
                for (const auto& [id, p]: players_)
                    if (p.physicalized && p.ball) { retail->SetWorldMatrix(p.ball->GetWorldMatrix()); break; }
        }
        // Step BALA: a clone found in the airship zone after this step is a
        // ghost from here (the PSI hook froze it); noted once per life.
        if (!airship_box_logged_) {
            bmmo::physics::airship_zone_box box;
            bmmo::physics::airship_zone_get(manager, box);
            if (box.ready) {
                airship_box_logged_ = true;
                char text[200];
                std::snprintf(text, sizeof(text), "world: airship zone (%.3f %.3f %.3f) .. (%.3f %.3f %.3f) taken by tick %u",
                              box.min[0], box.min[1], box.min[2], box.max[0], box.max[1], box.max[2], tick_);
                log(text);
            }
        }
        for (auto& [id, p]: players_) {
            const int in_zone = p.physicalized && p.ball ? bmmo::physics::airship_zone_contains(manager, p.ball->GetName()) : -1;
            if (in_zone == 1 && p.airship_tick == 0) {
                p.airship_tick = tick_;
                airship_entries_.emplace_back(id, tick_);
                log("world: airship: player " + std::to_string(id) + "'s ball " + p.ball->GetName()
                    + " is in the airship zone at tick " + std::to_string(tick_) + " (a ghost from here)");
            } else if (in_zone != 1) {
                p.airship_tick = 0;
            }
        }
        // Step SPGH: each clone's spawn ghost transitions, logged.
        for (auto& [id, p]: players_) {
            const int ghost = p.physicalized && p.ball ? bmmo::physics::spawn_ghost_state(manager, p.ball->GetName()) : -1;
            if (ghost == 1 && !p.spawn_ghost)
                log("world: spawn ghost: player " + std::to_string(id) + "'s ball " + p.ball->GetName()
                    + " is a spawn ghost at tick " + std::to_string(tick_));
            else if (ghost == 0 && p.spawn_ghost)
                log("world: spawn ghost: player " + std::to_string(id) + "'s ball " + p.ball->GetName() + " is solid at tick "
                    + std::to_string(tick_));
            p.spawn_ghost = ghost == 1;
        }
        // Step SPGH2: the counters, logged when they move - a resumed body
        // is a trafo's new ball that stayed solid.
        {
            bmmo::physics::spawn_ghost_info info;
            if (bmmo::physics::spawn_ghost_get(manager, info) && info.active
                && (info.resumed != spawn_ghost_resumed_logged_ || info.births != spawn_ghost_births_logged_)) {
                char text[200];
                std::snprintf(text, sizeof(text),
                              "world: spawn ghost at tick %u: %llu births, %llu resumed solid, %llu solidified, %llu rechecks",
                              tick_, static_cast<unsigned long long>(info.births),
                              static_cast<unsigned long long>(info.resumed),
                              static_cast<unsigned long long>(info.solidified),
                              static_cast<unsigned long long>(info.rechecks));
                log(text);
                spawn_ghost_resumed_logged_ = info.resumed;
                spawn_ghost_births_logged_ = info.births;
            }
        }
        ++tick_;
        rng_cursor_ = ivp_srand_read();
        random_cursor_ = bmmo::physics::random_get_state();
        if (options_.trace) {
            bmmo::physics::world_hash h;
            std::string ignored;
            if (bmmo::physics::capture_world_hash(physics(), h, ignored)) {
                if (rng_last_pdelta_ == 0.0f && h.physics_delta_time > 0.0f) exact_log_ticks_ = 12;   // physics resumed
                rng_last_pdelta_ = h.physics_delta_time;
                if (h.ivp_seed != rng_last_seed_ || h.cores != rng_last_cores_) {
                    rng_last_seed_ = h.ivp_seed;
                    rng_last_cores_ = h.cores;
                    char timing[128];
                    std::snprintf(timing, sizeof(timing), " cktime=%.3f dt=%.4f frames=%d",
                                  engine_->time_manager()->GetTime(), engine_->time_manager()->GetLastDeltaTime(),
                                  engine_->time_manager()->GetMainTickCount());
                    log("rng t=" + std::to_string(tick_ - 1) + " seed=" + std::to_string(h.ivp_seed) + " mc="
                        + std::to_string(h.next_movement_check) + " cores=" + std::to_string(h.cores) + timing
                        + " movable=" + bmmo::physics::describe_movable_objects(physics()));
                }
            }
        }
        if (options_.trace) {
            const uint32_t done = tick_ - 1;   // the tick just simulated (client numbering)
            const bool window = (done >= 4 && done <= 60);
            if (exact_log_ticks_ > 0 || window) {
                if (exact_log_ticks_ > 0) --exact_log_ticks_;
                const std::string exact = bmmo::physics::describe_cores_exact(physics());
                std::istringstream lines(exact);
                std::string line;
                while (std::getline(lines, line))
                    log("exact t=" + std::to_string(done) + " " + line);
                bmmo::physics::world_hash h;
                std::string ignored;
                if (bmmo::physics::capture_world_hash(physics(), h, ignored)) {
                    char text[320];
                    std::snprintf(text, sizeof(text),
                                  "exact t=%u env seed=%d mc=%d cores=%d pose=%016llx time=%a psi=%a/%a pdelta=%a factor=%a",
                                  done, h.ivp_seed, static_cast<int>(h.next_movement_check), h.cores,
                                  static_cast<unsigned long long>(h.pose), h.ivp_time, h.time_of_last_psi, h.time_of_next_psi,
                                  static_cast<double>(h.physics_delta_time), static_cast<double>(h.time_factor));
                    log(text);
                }
            }
        }
        if (options_.trace) {
            // Design 9.26 diagnostics: the constraints the retail scripts built
            // this tick, in creation order, as the client logs them.
            const std::string events = bmmo::physics::drain_event_log(physics());
            size_t pos = 0;
            while (pos < events.size()) {
                const size_t end = events.find(';', pos);
                if (end == std::string::npos) break;
                const std::string entry = events.substr(pos, end - pos);
                pos = end + 1;
                if (entry.find(" constraint ") != std::string::npos || entry.find(" birth ") != std::string::npos
                        || entry.find(" psi ") != std::string::npos || entry.find(" force ") != std::string::npos)
                    log("bridge at tick " + std::to_string(tick_ - 1) + ": " + entry);
            }
        }
        return true;
    }

    physics_world::step_state physics_world::capture_step_state() const {
        step_state state;
        state.tick = tick_;
        state.rng_cursor = rng_cursor_;
        state.random_cursor = random_cursor_;
        state.callback_pending = callback_pending_;
        state.sectors = sectors_;
        if (options_.proximity_union) proximity_.capture(state.proximity_hold);   // step G2
        return state;
    }

    void physics_world::restore_step_state(const step_state& state) {
        tick_ = state.tick;
        rng_cursor_ = state.rng_cursor;
        random_cursor_ = state.random_cursor;
        callback_pending_ = state.callback_pending;
        sectors_ = state.sectors;
        // Step G2: the restored tick has not been placed for (tick() places
        // unless the caller does first).
        probes_placed_ = false;
        if (options_.proximity_union && !state.proximity_hold.empty()) {   // step G2
            std::string hold_error;
            if (!proximity_.restore(engine_->context(), state.proximity_hold.data(), state.proximity_hold.size(),
                                    hold_error)) {
                ++proximity_hold_failures_;
                log("world: proximity hold not restored: " + hold_error);
            }
        }
    }

    void physics_world::pre_simulate() {
        callback_pending_ = false;
        if (options_.park_retail_ball || options_.auto_clone_players) park_retail_ball();
        if (options_.mirror_clone_to_retail) {
            // Deactivate Ball hid the retail entity: the retail ball died.
            if (CK3dEntity* retail = retail_ball(); retail && !retail->IsVisible()) {
                for (auto& [id, p]: players_) {
                    if (!p.physicalized || !p.ball) continue;
                    std::string error;
                    if (p.navigation) p.navigation->shutdown_all();
                    bmmo::physics::unphysicalize(manager_for_debug(), p.ball->GetName(), error);
                    p.physicalized = false;
                    body_set_changed_ = true;
                    log("world: retail ball hidden, clone of player " + std::to_string(id) + " unphysicalized at tick "
                        + std::to_string(tick_));
                }
            }
        }
        for (auto& [id, p]: players_) {
            if (p.have_input && p.cam_ref) {
                VxMatrix matrix = p.cam_ref->GetWorldMatrix();
                for (int k = 0; k < 3; ++k) {
                    matrix[0][k] = p.input.cam_right[k];
                    matrix[1][k] = p.input.cam_up[k];
                    matrix[2][k] = p.input.cam_dir[k];
                }
                p.cam_ref->SetWorldMatrix(matrix);
            }
            if (p.navigation) {
                bool nav_active = p.have_input && (p.input.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                if (options_.retail_nav_from_script) nav_active = retail_navigation_active();
                p.navigation->apply(p.have_input ? static_cast<uint8_t>(p.input.keys & 0x0F) : 0, nav_active);
            }
        }
    }

    // ---- the trafo as an input (trafo-replication design, step T2)

    // At the anchor, before the selection: the level's P_Trafo_* positions
    // (validation), the originals quiesced (design 2.3: nothing to remove in
    // this world - a non-zero count is a bug), the hold armed (it never
    // fires here: the retail ball is parked), the piece x local-body filter
    // rule on (design 2.10).
    bool physics_world::anchor_trafo(std::string& error) {
        CKContext* context = engine_->context();
        CKIpionManager* manager = physics();
        trafo_hooks_ = make_trafo_hooks(manager, [this](const std::string& line) { log("world: " + line); });
        if (trafo_registry_.reference().empty()) {
            // Not seen during the load (never expected: Balls.nmo is part of
            // the composition); the anchor is still before any explosion.
            bmmo::game::trafo_reference reference;
            if (!bmmo::game::capture_trafo_reference(context, reference, error)) {
                error = "trafo level reference: " + error;
                return false;
            }
            trafo_registry_.set_reference(std::move(reference));
            log("world: trafo level reference captured at the anchor");
        }
        // The places a trafo can be: every PH row of a P_Trafo_* type (the
        // level's placement table - the original levels pool their trafos
        // across sectors, and a pass moves the pooled object to its row:
        // L11's P_Trafo_Stone_MF001 sits at its sector-6 row at the anchor
        // and at its sector-1 row (60.95, 5.10, -12.58) once sector 1 is
        // placed), then every P_Trafo_* entity where it stands now (a level
        // without a PH table).  Static level data either way.
        trafo_points_.clear();
        if (CKDataArray* ph = engine_->data_array("PH")) {
            int type_column = -1, matrix_column = -1, object_column = -1;
            for (int c = 0; c < ph->GetColumnCount(); ++c) {
                const char* name = ph->GetColumnName(c);
                if (!name) continue;
                if (std::strcmp(name, "Type Name") == 0) type_column = c;
                else if (std::strcmp(name, "Matrix") == 0) matrix_column = c;
                else if (std::strcmp(name, "Object") == 0) object_column = c;
            }
            for (int row = 0; type_column >= 0 && matrix_column >= 0 && row < ph->GetRowCount(); ++row) {
                char type[128] = {};
                ph->GetElementStringValue(row, type_column, type, static_cast<int>(sizeof(type)));
                type[sizeof(type) - 1] = 0;
                if (std::strncmp(type, "P_Trafo_", 8) != 0) continue;
                VxMatrix m;
                if (!ph->GetElementValue(row, matrix_column, &m)) continue;
                bmmo::session::trafo_point point;
                CKObject* object = object_column >= 0 ? ph->GetElementObject(row, object_column) : nullptr;
                point.name = std::string(object && object->GetName() ? object->GetName() : type) + "@PH" + std::to_string(row);
                point.position[0] = m[3][0];
                point.position[1] = m[3][1];
                point.position[2] = m[3][2];
                trafo_points_.push_back(std::move(point));
            }
        }
        const size_t placements = trafo_points_.size();
        const XObjectPointerArray& all = context->GetObjectListByType(CKCID_3DENTITY, TRUE);
        for (int i = 0; i < all.Size(); ++i) {
            auto* e = CK3dEntity::Cast(all[i]);
            if (!e || !e->GetName() || std::strncmp(e->GetName(), "P_Trafo_", 8) != 0) continue;
            VxVector at;
            e->GetPosition(&at);
            bmmo::session::trafo_point point;
            point.name = e->GetName();
            point.position[0] = at.x;
            point.position[1] = at.y;
            point.position[2] = at.z;
            trafo_points_.push_back(std::move(point));
        }
        std::sort(trafo_points_.begin() + static_cast<std::ptrdiff_t>(placements), trafo_points_.end(),
                  [](const bmmo::session::trafo_point& a, const bmmo::session::trafo_point& b) { return a.name < b.name; });
        const auto quiesce = bmmo::game::quiesce_originals(context, trafo_hooks_);
        log("world: " + quiesce.text() + (quiesce.bodies || quiesce.controllers || quiesce.scripts
                                            ? " - NOT EXPECTED in the server's world (a bug)" : ""));
        trafo_hold_.arm(context, kFixedDeltaMs);
        if (!bmmo::physics::trafo_set_filter(manager, true, error)) {
            error = "trafo filter: " + error;
            return false;
        }
        log("world: trafo inputs on: " + std::to_string(placements) + " P_Trafo_* placements in PH and "
            + std::to_string(trafo_points_.size() - placements) + " P_Trafo_* entities (reach "
            + std::to_string(bmmo::session::kTrafoReach) + " m), the originals held, the piece x local-body filter on");
        return true;
    }

    // The player's set, by the rule every world runs (game/trafo_sets.hpp
    // ensure_trafo_set): kept when selected (still fading from an earlier
    // stay), otherwise built - the server destroys a set at its deselect, so
    // the reselect branch never runs here.
    bool physics_world::make_trafo_set(uint32_t id, std::string& error) {
        const auto r = bmmo::game::ensure_trafo_set(engine_->context(), trafo_registry_, id, trafo_hooks_);
        if (!r.ok) {
            // Fail closed (design 2.4): the session cannot run this member's
            // trafo; the runner ends it.
            for (const auto& line: r.lines) log("world: trafo build " + std::to_string(id) + ": " + line);
            trafo_failed_ = true;
            error = "trafo set of player " + std::to_string(id) + " failed: " + r.error;
            return false;
        }
        if (r.how == bmmo::game::trafo_ensure::kept) return true;
        if (r.how == bmmo::game::trafo_ensure::reselected) {
            for (const auto& line: r.lines) log("world: trafo set " + std::to_string(id) + " " + line);
            return true;
        }
        const auto* set = trafo_registry_.find(id);
        log("world: trafo set " + std::to_string(id) + " built: " + std::to_string(set->objects.size()) + " objects, "
            + std::to_string(set->scripts.size()) + " scripts, digest " + set->digest + ", value digest " + set->value_digest);
        return true;
    }

    // A Trafo event at its applied tick (event.tick): the type set's
    // referential and seed for an explosion, the next-frame flags of the
    // explosion or the reset; the trafo frame of this tick runs it.
    bool physics_world::apply_trafo(uint32_t id, const lifecycle_event& event, std::string& error) {
        if (!options_.trafo_inputs) {
            error = "a Trafo event in a world without trafo inputs";
            return false;
        }
        const uint8_t type = event.ball_type;
        if (type >= bmmo::session::kTrafoTypes) {
            error = "trafo piece type " + std::to_string(type);
            return false;
        }
        // A set before the player's input (design 2.4), by the shared rule:
        // a set retired since the player's add_player is built afresh.
        if (!make_trafo_set(id, error)) return false;
        const bool reset = bmmo::session::trafo_is_reset(event.flags);
        const int32_t seed = reset ? 0 : bmmo::session::trafo_seed(static_cast<uint32_t>(options_.seed), id, event.tick, type);
        if (!bmmo::game::run_trafo(engine_->context(), trafo_registry_, trafo_hooks_, id, event.flags, type, event.position, seed, error))
            return false;
        if (event.flags & bmmo::session::TRAFO_FINAL) bmmo::game::trafo_set_final(trafo_registry_, id, type, event.tick);
        auto& a = trafo_applied_[{id, type}];
        if (reset) {
            a.reset = true;
            a.reset_applied = event.tick;
        } else {
            a.explosion = true;
            a.explode_applied = event.tick;
            for (int k = 0; k < 3; ++k) a.position[k] = event.position[k];
            a.reset = false;
            a.reset_applied = 0;
            body_set_changed_ = true;   // the trafo frame of this tick physicalizes the pieces
        }
        char text[256];
        std::snprintf(text, sizeof(text), "world: trafo %s %s of player %u at tick %u%s%s pos=%a,%a,%a seed=%d",
                      bmmo::session::trafo_type_name(type), reset ? "reset" : "explode", id, event.tick,
                      (event.flags & bmmo::session::TRAFO_SERVER) ? " (server)" : "",
                      (event.flags & bmmo::session::TRAFO_FINAL) ? " (final)" : "",
                      static_cast<double>(event.position[0]), static_cast<double>(event.position[1]),
                      static_cast<double>(event.position[2]), seed);
        log(text);
        return true;
    }

    // After the engine's frame of a tick (tick_ is still that tick's): the
    // server's hold (design 2.3: it reports, cancels and sends nothing - it
    // should never fire), the effect checks, the body-set flag of a reset's
    // De Physicalize frame (R + 132), retirement (the server has no
    // rollback: a retired set is destroyed at its deselect).
    void physics_world::after_trafo_tick() {
        const auto started = std::chrono::steady_clock::now();
        CKContext* context = engine_->context();
        for (const auto& r: trafo_hold_.poll(context, tick_, false)) {
            ++trafo_hold_reports_;
            if (trafo_hold_reports_ <= 20)
                log("world: trafo hold FIRED at tick " + std::to_string(tick_) + ": " + bmmo::session::trafo_type_name(r.type)
                    + (bmmo::session::trafo_is_reset(r.flags) ? " reset" : " explosion") + " from " + r.source
                    + " cancelled - an own Trafo Manager acting on the parked ball (a bug)");
        }
        const auto effect = trafo_hold_.effect_check(context, trafo_hooks_);
        if (effect.leaks()) {
            trafo_leaks_ += static_cast<uint64_t>(effect.leaks());
            log("world: trafo_original_leaks at tick " + std::to_string(tick_) + ": " + std::to_string(effect.bodies) + " bodies, "
                + std::to_string(effect.scripts) + " scripts, first " + effect.first);
        }
        for (const auto& [key, a]: trafo_applied_)
            if (a.reset && a.reset_applied + bmmo::session::kTrafoResetDephysicalize == tick_) body_set_changed_ = true;
        for (const auto& line: bmmo::game::retire_trafo_sets(context, trafo_registry_, trafo_hooks_, tick_,
                                                             96 /* history_ticks */, true))
            log("world: " + line);
        trafo_after_us_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        ++trafo_after_ticks_;
    }

    // The ledger a resync or join state carries (design 4.7), after the
    // step of S = tick_ - 1: the three type sets of every set this world
    // holds (the server destroys a set at its deselect, so every set it
    // holds is selected: exactly the sets the portable record lists), in
    // the registry's order - bodies, the newest explosion and reset, the
    // final bit and the set's newest final reset.  As built at T2's
    // review: the design's filter (bodies, a live explosion, or a reset
    // before its Restore IC) left out a member's set that never exploded
    // and a leaver's set between its final resets and its retirement,
    // which the portable record still lists, and never said a set was
    // final.
    void physics_world::capture_trafo_ledger(std::vector<bmmo::session::trafo_ledger_entry>& out) const {
        out.clear();
        CKContext* context = engine_->context();
        for (const auto& set: trafo_registry_.sets()) {
            if (!set.selected) continue;
            for (uint8_t type = 0; type < bmmo::session::kTrafoTypes; ++type) {
                bmmo::session::trafo_ledger_entry e;
                e.player = set.player;
                e.type = type;
                for (CK_ID piece: set.types[type].pieces) {
                    CKObject* o = context->GetObject(piece);
                    if (o && o->GetName() && trafo_hooks_.has_body(o->GetName())) {
                        e.has_bodies = true;
                        break;
                    }
                }
                if (const auto a = trafo_applied_.find({set.player, type}); a != trafo_applied_.end()) {
                    e.has_explosion = a->second.explosion;
                    e.explode_applied = a->second.explode_applied;
                    for (int k = 0; k < 3; ++k) e.position[k] = a->second.position[k];
                    e.has_reset = a->second.reset;
                    e.reset_applied = a->second.reset_applied;
                }
                e.final = set.types[type].final;
                e.final_applied = e.final ? set.final_tick : 0;
                out.push_back(e);
            }
        }
    }

    void physics_world::snapshot(bool full, std::vector<bmmo::session::body_state>& out) {
        collect_bodies(full, true, out);
    }

    // The bodies a full snapshot would carry, without the bookkeeping: the
    // black box reads the world, it must not answer the runner's "did the body
    // set change" question with its own call, nor number a mechanism the
    // server's own snapshots have not numbered yet (that number is the `owner`
    // field on the wire).  An unnumbered body gets the index it would be given
    // next, so the record still lines up with the run's numbering.
    void physics_world::snapshot_for_journal(std::vector<bmmo::session::body_state>& out) {
        collect_bodies(true, false, out);
    }

    void physics_world::capture_resync_state(uint32_t player, resync_state& out) {
        out = resync_state{};
        // The rows a full snapshot of this tick carries, numbered as the
        // snapshots number them (a body new to the numbering gets its number
        // now, the one the next full snapshot would give it), without the
        // body-set bookkeeping: the snapshot cadence is the runner's.
        collect_bodies(true, false, out.rows, true);
        // Every player's ball entity: the client maps each to its own entity
        // of that player (the resynced member's own ball, a mirror).
        (void)player;
        for (const auto& [id, p]: players_) {
            if (p.physicalized && p.ball && p.ball->GetName()) out.clones.emplace_back(id, p.ball->GetName());
            // ...and its navigation: the keys down and each live force
            // controller's force, which the camera at the key's press made.
            if (p.navigation) {
                bmmo_physics_nav_state nav{};
                p.navigation->get_state(nav);
                out.navs.emplace_back(id, nav);
            }
        }
        out.union_state = bmmo::session::sector_union::serialize(sectors_.state());
        if (out.union_state.empty()) out.error = "the sector union does not serialize";
        std::string e;
        // Step G3 (world-agreement-gaps design 4.4): under the bit the record
        // carries the union's hold by name (version 3): a resync's or a late
        // join's re-step starts from the server's frames and by-name cells.
        const bmmo::game::script_replay::portable_hold_capture hold = [this](std::vector<uint8_t>& section) {
            proximity_.capture_portable(section);
        };
        if (!bmmo::game::script_replay::capture_portable(engine_->context(), script_selection_, script_hooks_, out.scripts, &e,
                                                         nullptr, options_.trafo_inputs ? &trafo_registry_.part() : nullptr,
                                                         options_.proximity_union ? &hold : nullptr)) {
            out.scripts.clear();
            if (out.error.empty()) out.error = "portable script record: " + e;
        }
        e.clear();
        if (!bmmo::physics::get_savestate(physics(), out.savestate, e)) {
            out.savestate.clear();
            if (out.error.empty()) out.error = "savestate: " + e;
        }
        // Packed for the wire (CKPackData: zlib, the CK file format's own):
        // the client unpacks with its CK's CKUnPackData.  A section that does
        // not pack is carried as it is (size 0).
        const auto pack = [](std::vector<uint8_t>& bytes, uint32_t& size) {
            if (bytes.empty()) return;
            int packed_size = 0;
            char* packed = CKPackData(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), packed_size, 6);
            if (!packed) return;
            if (packed_size > 0 && static_cast<size_t>(packed_size) < bytes.size()) {
                size = static_cast<uint32_t>(bytes.size());
                bytes.assign(reinterpret_cast<const uint8_t*>(packed), reinterpret_cast<const uint8_t*>(packed) + packed_size);
            }
            CKDeletePointer(packed);
        };
        pack(out.scripts, out.scripts_size);
        pack(out.savestate, out.savestate_size);
        if (options_.trafo_inputs) capture_trafo_ledger(out.trafo_ledger);
    }

    void physics_world::collect_bodies(bool full, bool bookkeeping, std::vector<bmmo::session::body_state>& out, bool number) {
        using bmmo::session::body_kind;
        using bmmo::session::body_state;
        out.clear();
        CKIpionManager* manager = physics();
        if (!manager) return;
        std::vector<bmmo_physics_body_state> bodies(256);
        int count = bmmo::physics::list_bodies(manager, bodies.data(), static_cast<int>(bodies.size()));
        if (count > static_cast<int>(bodies.size())) {
            bodies.resize(static_cast<size_t>(count));
            count = bmmo::physics::list_bodies(manager, bodies.data(), static_cast<int>(bodies.size()));
        }
        bodies.resize(static_cast<size_t>(std::max(0, count)));
        std::unordered_map<std::string, uint32_t> ball_owner;
        std::string retail_name;
        if (CK3dEntity* retail = retail_ball()) retail_name = retail->GetName() ? retail->GetName() : "";
        for (const auto& [id, p]: players_)
            if (p.physicalized && p.ball && p.ball->GetName()) ball_owner[p.ball->GetName()] = id;

        std::set<std::string> current_set;
        std::unordered_map<std::string, bool> current_simulated;
        size_t unnumbered = 0;
        for (const auto& body: bodies) {
            if (!body.movable) continue;
            const std::string name = body.name;
            body_state state;
            for (int k = 0; k < 3; ++k) {
                state.position[k] = body.position[k];
                state.linear[k] = body.linear[k];
                state.angular[k] = body.angular[k];
            }
            for (int k = 0; k < 4; ++k) state.rotation[k] = body.rotation[k];
            state.flags = static_cast<uint8_t>((body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0u)
                | (body.collision_enabled ? bmmo::session::BODY_FLAG_COLLISION_ENABLED : 0u));
            if (auto owner = ball_owner.find(name); owner != ball_owner.end()) {
                state.kind = body_kind::Ball;
                state.owner = owner->second;
                out.push_back(std::move(state));
                continue;
            }
            if (name == retail_name || name.find(kPlayerNameTag) != std::string::npos) continue;
            current_set.insert(name);
            current_simulated[name] = body.simulated;
            // A delta snapshot carries the simulated bodies only.  The one
            // exception is the tick a body goes to sleep on: without that row
            // a client keeps predicting a mechanism the server has stopped,
            // and only the next full snapshot (up to 98 ticks later) tells it
            // otherwise - so the transition awake -> asleep is reported once,
            // with BODY_FLAG_SIMULATED clear (design 9.25).
            if (!full && !body.simulated) {
                auto previous = last_simulated_.find(name);
                if (previous == last_simulated_.end() || !previous->second) continue;
            }
            auto index = body_index_.find(name);
            if (index == body_index_.end() && (bookkeeping || number))
                index = body_index_.emplace(name, static_cast<uint16_t>(body_index_.size())).first;
            state.kind = body_kind::Mechanism;
            state.owner = index != body_index_.end()
                        ? index->second
                        : static_cast<uint16_t>(body_index_.size() + unnumbered++);
            if (full) state.name = name;
            out.push_back(std::move(state));
        }
        if (!bookkeeping) return;
        body_set_changed_ = current_set != last_body_set_;
        last_body_set_ = std::move(current_set);
        last_simulated_ = std::move(current_simulated);
    }

    std::string physics_world::describe() const {
        std::string text = "tick=" + std::to_string(tick_) + " players=" + std::to_string(players_.size())
            + " parked=" + (retail_parked_ ? "1" : "0");
        if (options_.trafo_inputs)
            text += " trafo_sets=" + std::to_string(trafo_registry_.sets().size()) + " trafo_hold=" + std::to_string(trafo_hold_reports_)
                  + " trafo_leaks=" + std::to_string(trafo_leaks_) + " trafo_after_us="
                  + std::to_string(trafo_after_ticks_ ? trafo_after_us_ / trafo_after_ticks_ : 0) + "/tick";
        text += " sectors=";
        for (int s: sectors_.active()) text += std::to_string(s) + ",";
        for (const auto& [id, p]: players_) {
            text += " [" + std::to_string(id) + ": type=" + std::to_string(p.ball_type)
                + " phys=" + (p.physicalized ? "1" : "0") + " sector=" + std::to_string(p.sector)
                + " keys=" + std::to_string(p.input.keys) + " controllers="
                + std::to_string(p.navigation ? p.navigation->controller_count() : 0) + "]";
        }
        return text;
    }

    // ---- sector placement (docs/sector-placement-design.md 2.4, step S2)
    //
    // At the anchor, after the Sequencer and proximity restores (a copy
    // inherits its source's level-file counters) and before the anchor hash,
    // the rewiring and the selection (the copies' probes, ball reads and
    // scripts are the world's like any other): with placement_clones every
    // PH row gets its own object (game/sector_placement.hpp apply, all or
    // nothing - a refusal fails the anchor); the signature of the table as
    // it then stands is computed either way.  Option off: a level that still
    // holds copies is released first and the anchor refused if that did not
    // clear it (design 2.4 "Option off"; the server's world runs it since
    // step HU1 - a fresh world's registry is empty and its PH table names no
    // _R<n> object, so nothing changes here).  The destructor releases the
    // copies before the engine goes: the context reset destroys dynamic
    // objects in their creation order, which inside a copy is CK's hash
    // order, and a P_Extra copy whose Extra block ran must be torn down root
    // first (sector_placement::release).
    bool physics_world::place_sectors(std::string& error) {
        // The placement's host logic is game/sector_placement.hpp's
        // place_at_anchor (step HU1, one copy for every host); this world
        // supplies its backend, the test hook and its log prefix.
        return bmmo::game::place_at_anchor(placement_, engine_->context(), bmmo::physics::physics_direct(engine_->physics()),
                                           options_.placement_clones, options_.placement_fail_at,
                                           [this](const std::string& line) { log("world: " + line); }, anchor_placement_, error);
    }
}
