#include "physics_world.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include <physics/physics_state.hpp>
#include <session/spawn_impulse.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>

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

        // Whether the script only wants a value computed from the ball rather
        // than the ball itself.  The rope bridge's name test is a parameter
        // *operation* ("Get Name") feeding a Test block, while a mechanism
        // that works on the ball (the fan's SetPhysicsForce, its box test)
        // hands it to a behaviour.  Parameter links live on the reading side
        // (CKParameterIn::m_OutSource), so the consumers are found by looking
        // for readers, not through the producer's destination list.
        std::string operation_on(CKContext* context, CKParameter* value) {
            if (!context || !value) return {};
            const int count = context->GetObjectsCountByClassID(CKCID_PARAMETEROPERATION);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_PARAMETEROPERATION);
            for (int i = 0; i < count; ++i) {
                auto* operation = CKParameterOperation::Cast(context->GetObject(ids[i]));
                if (!operation) continue;
                for (CKParameterIn* in: {operation->GetInParameter1(), operation->GetInParameter2()})
                    if (in && in->GetDirectSource() == value)
                        return operation->GetName() ? operation->GetName() : "?";
            }
            return {};
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
        // Players first: their navigation controllers live in the environment.
        players_.clear();
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
        for (int i = 0; i < options_.boot_ticks; ++i)
            if (!engine_->tick(error)) return false;
        bmmo::physics::drain_event_log(engine_->physics());  // install the listener
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
            if (!engine_->tick(error)) return false;
        }
        log("world: anchor reached after " + std::to_string(waited) + " ticks (engine tick "
            + std::to_string(engine_->ticks()) + ")");
        return true;
    }

    bool physics_world::anchor(std::string& error) {
        CKContext* context = engine_->context();
        CKIpionManager* manager = engine_->physics();
        if (!bmmo::physics::reset_session_clock(manager, options_.seed, error)) return false;
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
        active_sectors_.insert(1);
        if (!ensure_collision_filter(error)) return false;
        rewire_proximity_probes();
        rewire_ball_identity_reads();
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

    void physics_world::rewire_proximity_probes() {
        CKContext* context = engine_->context();
        if (CKObject* frame = context->GetObjectByNameAndParentClass(const_cast<CKSTRING>("Ball_Pos_Frame"), CKCID_3DENTITY, nullptr))
            ball_pos_frame_ = frame->GetID();
        if (!ball_pos_frame_) {
            log("world: Ball_Pos_Frame not found; proximity union disabled");
            return;
        }
        const VxMatrix origin = CK3dEntity::Cast(context->GetObject(ball_pos_frame_))->GetWorldMatrix();
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            CKBehavior* block = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!block || std::string_view(bmmo::game::behavior_prototype_name(block)) != "TT Scaleable Proximity") continue;
            CKParameterIn* in = block->GetInputParameter(1);   // ObjectA
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            CKObject* object = source ? source->GetValueObject() : nullptr;
            if (!object || object->GetID() != ball_pos_frame_) continue;
            const std::string name = "BMMO_Prox_" + std::to_string(probes_.size());
            auto* frame = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(name.c_str())));
            if (!frame) continue;
            frame->SetWorldMatrix(origin);
            CKParameterLocal* parameter = context->CreateCKParameterLocal(const_cast<CKSTRING>(name.c_str()), CKPGUID_3DENTITY, TRUE);
            if (!parameter) continue;
            CK_ID frame_id = frame->GetID();
            parameter->SetValue(&frame_id, sizeof(frame_id));
            if (in->SetDirectSource(parameter) != CK_OK) continue;
            probes_.push_back({block->GetID(), frame_id, parameter->GetID()});
        }
        log("world: " + std::to_string(probes_.size()) + " proximity blocks rewired to per-player frames");
    }

    // Before the scripts of a tick run: each probe frame goes to the player
    // ball nearest to the block's ObjectB (the mechanism), so the retail
    // "any ball in range" test fires for whichever player gets there.
    void physics_world::update_proximity_probes() {
        if (probes_.empty()) return;
        CKContext* context = engine_->context();
        std::vector<CK3dEntity*> balls;
        for (const auto& [id, p]: players_)
            if (p.physicalized && p.ball) balls.push_back(p.ball);
        VxVector fallback(0.0f, 0.0f, 0.0f);
        if (auto* bpf = CK3dEntity::Cast(context->GetObject(ball_pos_frame_))) bpf->GetPosition(&fallback);
        for (const auto& probe: probes_) {
            auto* block = CKBehavior::Cast(context->GetObject(probe.block));
            auto* frame = CK3dEntity::Cast(context->GetObject(probe.frame));
            if (!block || !frame) continue;
            VxVector target = fallback;
            if (!balls.empty()) {
                VxVector reference = fallback;
                if (auto* object_b = CK3dEntity::Cast(block->GetInputParameterObject(2))) object_b->GetPosition(&reference);
                float best = 0.0f;
                bool have = false;
                for (CK3dEntity* ball: balls) {
                    VxVector position;
                    ball->GetPosition(&position);
                    const float distance = (position - reference).SquareMagnitude();
                    if (!have || distance < best) { best = distance; target = position; have = true; }
                }
            }
            frame->SetPosition(&target);
        }
    }

    // Ball identity union (design 8.3).  A mechanism that gates on the ball
    // type reads the active ball out of the level array and compares its name:
    // the rope bridge P_Modul_29 does
    //     TT Scaleable Proximity -> Get Cell(CurrentLevel[0,ActiveBall])
    //         -> Get Name -> Test(Equal, "Ball_Stone") -> shut the 10 hinges
    // On the server that cell names the parked retail ball, which starts as
    // Ball_Wood and never trafos (the retail Trafo Manager sees a ball that
    // does not move), so the gate stayed shut however many stone balls rolled
    // over the bridge.  Every such read inside a script whose proximity blocks
    // were rewired gets a private copy of the array; update_ball_identity_reads
    // writes the retail entity of the nearest player's ball type into it, so
    // the retail "the ball on this mechanism decides" semantics hold for
    // whichever player is there -- the same player the probe frames follow.
    void physics_world::rewire_ball_identity_reads() {
        CKContext* context = engine_->context();
        auto* current_level = CKDataArray::Cast(context->GetObject(current_level_));
        if (!current_level) return;
        int ball_column = -1;
        for (int c = 0; c < current_level->GetColumnCount(); ++c) {
            CKSTRING column = current_level->GetColumnName(c);
            if (column && std::string_view(column) == "ActiveBall") { ball_column = c; break; }
        }
        if (ball_column < 0) {
            log("world: CurrentLevel has no ActiveBall column; ball identity union disabled");
            return;
        }
        // The mechanisms (proximity ObjectB) each rewired script watches.
        std::unordered_map<CK_ID, std::vector<CK_ID>> mechanisms;
        for (const auto& probe: probes_) {
            auto* block = CKBehavior::Cast(context->GetObject(probe.block));
            if (!block) continue;
            auto* object_b = CK3dEntity::Cast(block->GetInputParameterObject(2));
            if (!object_b) continue;
            CKBehavior* root = block;
            while (root->GetParent()) root = root->GetParent();
            mechanisms[root->GetID()].push_back(object_b->GetID());
        }
        std::map<std::string, int> rewired, untouched;   // for one summary line
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            CKBehavior* block = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!block || std::string_view(bmmo::game::behavior_prototype_name(block)) != "Get Cell") continue;
            CKParameterIn* target = block->GetTargetParameter();
            CKParameter* array_source = target ? target->GetRealSource() : nullptr;
            CKObject* array = array_source ? array_source->GetValueObject() : nullptr;
            if (!array || array->GetID() != current_level_) continue;
            CKParameterIn* column_in = block->GetInputParameterCount() > 1 ? block->GetInputParameter(1) : nullptr;
            CKParameter* column_source = column_in ? column_in->GetRealSource() : nullptr;
            int column = -1;
            if (column_source) column_source->GetValue(&column);
            if (column != ball_column) continue;
            CKBehavior* root = block;
            while (root->GetParent()) root = root->GetParent();
            const std::string script = root->GetName() ? root->GetName() : "?";
            auto watched = mechanisms.find(root->GetID());
            if (watched == mechanisms.end()) {
                // Retail gameplay logic (Ball_Shadow, Sound_Manager, the
                // Gameplay scripts): it acts on the parked retail ball and has
                // to keep doing so.
                ++untouched[script];
                continue;
            }
            // What the block's consumers want out of the cell decides which
            // object goes in it: a name test ("Ball_Stone") wants the retail
            // entity, anything else (P_Modul_18's fan pushes the ball with
            // SetPhysicsForce and tests its box) wants the body that is
            // actually there, i.e. the player's clone.
            const std::string operation = operation_on(context, block->GetOutputParameter(0));
            const bool by_name = !operation.empty();
            const std::string name = "BMMO_Ball_" + std::to_string(identities_.size());
            CKDependencies dependencies;
            dependencies.Resize(CKCID_MAXCLASSID);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            // The rows come along, the objects they name are shared.
            dependencies[CKCID_DATAARRAY] = CK_DEPENDENCIES_COPY_DATAARRAY_DATA;
            const std::string suffix = "_" + name;
            auto* copy = CKDataArray::Cast(context->CopyObject(current_level, &dependencies,
                                                               const_cast<CKSTRING>(suffix.c_str())));
            if (!copy) {
                log("world: CopyObject failed for CurrentLevel; ball identity union disabled for " + script);
                continue;
            }
            if (copy->GetRowCount() == 0) copy->AddRow();
            CKParameterLocal* parameter = context->CreateCKParameterLocal(const_cast<CKSTRING>(name.c_str()),
                                                                          CKPGUID_DATAARRAY, TRUE);
            if (!parameter) continue;
            CK_ID array_id = copy->GetID();
            parameter->SetValue(&array_id, sizeof(array_id));
            if (target->SetDirectSource(parameter) != CK_OK) continue;
            identities_.push_back({block->GetID(), array_id, parameter->GetID(), ball_column, by_name,
                                   watched->second});
            ++rewired[script + (by_name ? " (" + operation + ")" : " (body)")];
        }
        const auto describe = [](const std::map<std::string, int>& counts) {
            std::string text;
            for (const auto& [script, count]: counts)
                text += (text.empty() ? "" : ", ") + script + " x" + std::to_string(count);
            return text.empty() ? std::string("none") : text;
        };
        log("world: " + std::to_string(identities_.size()) + " ball identity reads rewired to per-player copies ("
            + describe(rewired) + "); left on the retail array: " + describe(untouched));
    }

    // Before the scripts of a tick run: the ActiveBall cell of every private
    // copy names the ball of the player nearest to one of that script's
    // mechanisms -- the retail entity of that player's ball type where the
    // script tests the name ("Ball_Stone"), the player's clone where it works
    // on the ball itself (the fan's SetPhysicsForce and box test), so both the
    // name and the body are the ones actually on the mechanism.  With no
    // player around it keeps naming the parked retail ball, which is what the
    // retail script would have read.
    void physics_world::update_ball_identity_reads() {
        if (identities_.empty()) return;
        CKContext* context = engine_->context();
        for (const auto& identity: identities_) {
            auto* array = CKDataArray::Cast(context->GetObject(identity.array));
            if (!array) continue;
            CKObject* active = retail_ball();
            float best = 0.0f;
            bool have = false;
            for (const auto& [id, p]: players_) {
                if (!p.physicalized || !p.ball) continue;
                CK3dEntity* entity = identity.by_name ? retail_ball_entity(p.ball_type) : p.ball;
                if (!entity) continue;
                VxVector position;
                p.ball->GetPosition(&position);
                for (CK_ID reference: identity.references) {
                    auto* mechanism = CK3dEntity::Cast(context->GetObject(reference));
                    if (!mechanism) continue;
                    VxVector origin;
                    mechanism->GetPosition(&origin);
                    const float distance = (position - origin).SquareMagnitude();
                    if (have && distance >= best) continue;
                    best = distance;
                    have = true;
                    active = entity;
                }
            }
            array->SetElementObject(0, identity.column, active);
        }
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
    // Physicalize_GameBall row; mass centre explicitly (0,0,0).
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

    int physics_world::player_sector(uint32_t id) const {
        auto it = players_.find(id);
        return it != players_.end() ? it->second.sector : 0;
    }

    // Sector union (design 8.3, revised).  The retail Gameplay_SectorManager
    // knows one ball: it activates the sector that ball entered and resets the
    // one it left.  The server has many balls, so the sectors that run are the
    // sectors its players are in -- every one of them, and only those: a
    // mechanism nobody is at stops, one that any player reaches starts.
    //
    // Every change is taken the tick it appears, but they are handed to the
    // manager one at a time: its PH-table walk spans about seven frames (it
    // waits for each Activate Script), and starting a new run in the middle of
    // one both resets that walk and flips CurrentLevel[Activation Phase?]
    // under the mechanism scripts it has just started -- they read that cell
    // in their first frame and stay dead if it is false.  So one pass per idle
    // manager, deactivate and activate paired in it exactly like a retail
    // checkpoint does, and the rest of the changes follow in the next frames.
    void physics_world::update_sectors() {
        std::set<int> desired;
        for (const auto& [id, p]: players_)
            if (p.sector > 0) desired.insert(p.sector);
        if (desired.empty() || desired == active_sectors_) return;
        CKContext* context = engine_->context();
        auto* parameters = CKDataArray::Cast(context->GetObject(ingame_parameter_));
        auto* manager_script = CKBehavior::Cast(context->GetObject(sector_manager_));
        if (!parameters || !manager_script) return;
        // Idle means the walk is over; the two extra ticks give the mechanism
        // scripts it started their own first frame before the flag moves again.
        sector_idle_ticks_ = manager_script->IsActive() ? 0 : sector_idle_ticks_ + 1;
        if (sector_idle_ticks_ < 3) return;
        int activate = 0, deactivate = 0;
        for (int sector: desired)
            if (!active_sectors_.count(sector)) { activate = sector; break; }
        for (int sector: active_sectors_)
            if (!desired.count(sector)) { deactivate = sector; break; }
        parameters->SetElementValue(0, 2, &deactivate, sizeof(deactivate));
        parameters->SetElementValue(0, 1, &activate, sizeof(activate));
        if (CKScene* scene = context->GetCurrentScene()) scene->Activate(manager_script, TRUE);
        sector_idle_ticks_ = 0;
        if (activate) active_sectors_.insert(activate);
        if (deactivate) active_sectors_.erase(deactivate);
        body_set_changed_ = true;
        std::string text = "world: sector";
        if (activate) text += " +" + std::to_string(activate);
        if (deactivate) text += " -" + std::to_string(deactivate);
        text += " at tick " + std::to_string(tick_) + ", running:";
        for (int sector: active_sectors_) text += " " + std::to_string(sector);
        if (desired != active_sectors_) text += " (more to come)";
        log(text);
    }

    bool physics_world::apply_event(uint32_t id, const lifecycle_event& event, std::string& error) {
        using bmmo::session::event_type;
        error.clear();
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
            if (!bmmo::physics::physicalize(manager, clone->GetName(), event.recipe, p.group.c_str(), error))
                return false;
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
            // The sector union follows the players (update_sectors).
            p.sector = event.sector;
            return true;
        case event_type::Finish:
            p.finished = true;
            return true;
        case event_type::BodyRevived: {
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
        update_proximity_probes();
        update_ball_identity_reads();
        update_sectors();
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
        if (options_.park_retail_ball && retail_parked_) park_retail_ball();
        if (options_.mirror_clone_to_retail) {
            // The retail entity (no body) follows the first physicalized clone.
            if (CK3dEntity* retail = retail_ball())
                for (const auto& [id, p]: players_)
                    if (p.physicalized && p.ball) { retail->SetWorldMatrix(p.ball->GetWorldMatrix()); break; }
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
            const bool window = (done >= 4 && done <= 12);
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
        return true;
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

    void physics_world::collect_bodies(bool full, bool bookkeeping, std::vector<bmmo::session::body_state>& out) {
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
            if (!full && !body.simulated) continue;
            auto index = body_index_.find(name);
            if (index == body_index_.end() && bookkeeping)
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
    }

    std::string physics_world::describe() const {
        std::string text = "tick=" + std::to_string(tick_) + " players=" + std::to_string(players_.size())
            + " parked=" + (retail_parked_ ? "1" : "0") + " sectors=";
        for (int s: active_sectors_) text += std::to_string(s) + ",";
        for (const auto& [id, p]: players_) {
            text += " [" + std::to_string(id) + ": type=" + std::to_string(p.ball_type)
                + " phys=" + (p.physicalized ? "1" : "0") + " sector=" + std::to_string(p.sector)
                + " keys=" + std::to_string(p.input.keys) + " controllers="
                + std::to_string(p.navigation ? p.navigation->controller_count() : 0) + "]";
        }
        return text;
    }
}
