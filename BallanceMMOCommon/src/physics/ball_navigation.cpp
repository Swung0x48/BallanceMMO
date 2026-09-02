#include <physics/ball_navigation.hpp>

#include "CKAll.h"
#include "CKInputManager.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include "ivp_controller.hxx"

#include <map>
#include <memory>

namespace bmmo::physics {
    // Verbatim equivalent of PhysicsControllerForce in
    // physics_RT/Behaviors/PhysicsForce.cpp: the same maths in the same order,
    // so a ball driven by this controller integrates exactly like the retail
    // ball driven by the script.
    class force_controller final : public IVP_Controller_Independent {
    public:
        force_controller(IVP_Real_Object* object, const IVP_U_Point& position, const IVP_U_Point& force) {
            core_ = object->get_core();
            position_ = position;
            force_ = force;
            if (core_) IVP_Controller_Manager::add_controller_to_core(this, core_);
        }

        ~force_controller() override {
            if (core_) IVP_Controller_Manager::remove_controller_from_core(this, core_);
        }

        void core_is_going_to_be_deleted_event(IVP_Core* core) override {
            if (core == core_) {
                core->rem_core_controller(this);
                core_ = nullptr;
            }
        }

        void do_simulation_controller(IVP_Event_Sim*, IVP_U_Vector<IVP_Core>* core_list) override {
            if (core_list && core_list->len() != 0) {
                IVP_U_Matrix matrix;
                core_->calc_at_matrix(core_->get_environment()->get_current_time(), &matrix);
                IVP_U_Point out;
                matrix.vimult3(&force_, &out);
                IVP_U_Float_Point position(position_.k[0], position_.k[1], position_.k[2]);
                IVP_U_Float_Point force_cs(out.k[0], out.k[1], out.k[2]);
                IVP_U_Float_Point force_ws(force_.k[0], force_.k[1], force_.k[2]);
                core_->async_push_core(&position, &force_cs, &force_ws);
            }
        }

        IVP_CONTROLLER_PRIORITY get_controller_priority() override { return IVP_CP_ACTUATOR; }
        const char* get_controller_name() override { return "bmmo:force"; }

        IVP_Core* core_;
        IVP_U_Point position_;
        IVP_U_Point force_;
    };

    player_navigation::player_navigation(CKContext* context, CKIpionManager* physics, CK3dEntity* ball,
                                         CK3dEntity* direction_ref,
                                         const std::vector<bmmo::game::navigation_leaf>& leaves, float force_value)
        : context_(context), physics_(physics), ball_id_(ball ? ball->GetID() : 0),
          direction_ref_id_(direction_ref ? direction_ref->GetID() : 0), force_value_(force_value) {
        leaves_.reserve(leaves.size());
        for (const auto& definition: leaves) {
            leaf l;
            l.definition = definition;
            leaves_.push_back(l);
        }
    }

    player_navigation::~player_navigation() {
        for (auto& l: leaves_) {
            delete l.controller;
            l.controller = nullptr;
        }
    }

    CK3dEntity* player_navigation::ball() const {
        return context_ && ball_id_ ? CK3dEntity::Cast(context_->GetObject(ball_id_)) : nullptr;
    }

    CK3dEntity* player_navigation::direction_ref() const {
        return context_ && direction_ref_id_ ? CK3dEntity::Cast(context_->GetObject(direction_ref_id_)) : nullptr;
    }

    void player_navigation::set_ball(CK3dEntity* ball) {
        const uint32_t id = ball ? ball->GetID() : 0;
        if (id == ball_id_) return;
        shutdown_all();
        ball_id_ = id;
    }

    uint8_t player_navigation::held_mask() const {
        uint8_t mask = 0;
        for (const auto& l: leaves_)
            if (l.key_state && l.definition.index < 8) mask |= static_cast<uint8_t>(1u << l.definition.index);
        return mask;
    }

    int player_navigation::controller_count() const {
        int count = 0;
        for (const auto& l: leaves_) count += l.controller ? 1 : 0;
        return count;
    }

    // PhysicsForceCallback::Execute for the ball with the leaf's parameters.
    // Returns false when the ball has no physics object yet; the retail
    // container then keeps the callback queued and retries in every
    // PreSimulate pass until it succeeds, which we mirror with create_pending.
    bool player_navigation::try_create(leaf& l) {
        CK3dEntity* ball_entity = ball();
        if (!ball_entity || !physics_ || !physics_->GetEnvironment()) return false;
        PhysicsObject* object = physics_->GetPhysicsObject(ball_entity);
        if (!object || !object->m_RealObject) return false;
        IVP_Real_Object* real = object->m_RealObject;

        // Position 0,0,0 with Pos Referential == the ball itself: the retail
        // callback skips the transform and keeps the local origin.
        VxVector direction = l.definition.direction;
        VxVector vec;
        if (CK3dEntity* ref = direction_ref()) ref->TransformVector(&vec, &direction);
        else vec = direction;

        IVP_U_Point force(vec.x, vec.y, vec.z);
        if (force.quad_length() <= 0.0001) force.set(1.0, 0.0, 0.0);
        else force.normize();
        force.mult(force_value_);

        IVP_U_Point position(0.0, 0.0, 0.0);
        l.controller = new force_controller(real, position, force);
        return true;
    }

    bool player_navigation::create_with_force(leaf& l, const float force_ws[3]) {
        CK3dEntity* ball_entity = ball();
        if (!ball_entity || !physics_ || !physics_->GetEnvironment()) return false;
        PhysicsObject* object = physics_->GetPhysicsObject(ball_entity);
        if (!object || !object->m_RealObject) return false;
        IVP_U_Point force(force_ws[0], force_ws[1], force_ws[2]);
        IVP_U_Point position(0.0, 0.0, 0.0);
        l.controller = new force_controller(object->m_RealObject, position, force);
        return true;
    }

    void player_navigation::get_state(bmmo_physics_nav_state& out) const {
        out = {};
        out.active = active_ ? 1 : 0;
        for (const auto& l: leaves_) {
            const int i = l.definition.index;
            if (i < 0 || i >= 8) continue;
            if (l.key_state) out.key_mask |= static_cast<uint8_t>(1u << i);
            if (l.create_pending) out.create_pending_mask |= static_cast<uint8_t>(1u << i);
            if (l.controller) {
                out.controller_mask |= static_cast<uint8_t>(1u << i);
                for (int k = 0; k < 3; ++k) out.force[i][k] = static_cast<float>(l.controller->force_.k[k]);
            }
        }
    }

    bool player_navigation::set_state(const bmmo_physics_nav_state& state) {
        bool ok = true;
        for (auto& l: leaves_) {
            delete l.controller;
            l.controller = nullptr;
            const int i = l.definition.index;
            if (i < 0 || i >= 8) continue;
            l.key_state = (state.key_mask & (1u << i)) != 0;
            l.create_pending = (state.create_pending_mask & (1u << i)) != 0;
            if (state.controller_mask & (1u << i)) {
                if (!create_with_force(l, state.force[i])) {
                    l.create_pending = true;
                    ok = false;
                }
            }
        }
        active_ = state.active != 0;
        wake_pending_ = false;
        return ok;
    }

    // Physics WakeUp: ensure_in_simulation() on the ball's object.  Like the
    // retail callback it is retried until the object exists.
    void player_navigation::wake_up() {
        CK3dEntity* ball_entity = ball();
        if (!ball_entity || !physics_) return;
        PhysicsObject* object = physics_->GetPhysicsObject(ball_entity);
        if (!object || !object->m_RealObject) {
            wake_pending_ = true;
            return;
        }
        object->m_RealObject->ensure_in_simulation();
        wake_pending_ = false;
    }

    void player_navigation::create(leaf& l) {
        // SetPhysicsForce.Create is ignored while a controller exists.
        if (l.controller) return;
        l.create_pending = !try_create(l);
        wake_up();  // Out1 -> Physics WakeUp.In
    }

    void player_navigation::shutdown(leaf& l) {
        delete l.controller;  // the destructor removes it from the core
        l.controller = nullptr;
        l.create_pending = false;
        wake_up();  // Out2 -> Physics WakeUp.In
    }

    void player_navigation::shutdown_all() {
        for (auto& l: leaves_) {
            l.key_state = false;
            if (l.controller || l.create_pending) shutdown(l);
        }
        active_ = false;
    }

    void player_navigation::apply(uint8_t keys, bool active) {
        // Retries queued by the retail callback container happen inside the
        // next Simulate; we run them at the start of the next tick instead.
        for (auto& l: leaves_)
            if (l.create_pending && !l.controller) l.create_pending = !try_create(l);
        if (wake_pending_) wake_up();

        if (!active) {
            // BallNav deactivate: Nop -> every SetPhysicsForce.Shutdown and
            // every Key Event.Off.
            if (active_) shutdown_all();
            return;
        }
        if (!active_) {
            // BallNav activate: Key Event.On resets the remembered state, then
            // the block evaluates the keys in the same frame.
            for (auto& l: leaves_) l.key_state = false;
            active_ = true;
        }
        for (auto& l: leaves_) {
            const bool down = l.definition.index < 8 && (keys & (1u << l.definition.index)) != 0;
            if (l.key_state && !down) {
                l.key_state = false;
                shutdown(l);       // Released -> Shutdown
            } else if (!l.key_state && down) {
                l.key_state = true;
                create(l);         // Pressed -> Create
            }
        }
    }

    // ------------------------------------------------------------ registry

    namespace {
        struct nav_entry {
            std::unique_ptr<player_navigation> navigation;
            uint32_t cam_ref = 0;      // CK_ID of the direction reference entity
            bool pending = false;
            uint8_t keys = 0;
            float rows[3][3] = {};
            bool active = false;
            // polling mode (design 9.6): keys from the input manager, active
            // from the retail Key Event blocks
            bool poll = false;
            int poll_count = 0;
            int key_codes[8] = {};
            uint32_t key_blocks[8] = {};
        };

        struct world_navs {
            std::map<std::string, nav_entry> entries;   // by ball entity name
            uint32_t behavior = 0;
            bool callback_queued = false;
        };

        std::map<CKIpionManager*, world_navs>& registry() {
            // Never destroyed: the physics manager may outlive static storage
            // at process exit (see the event log listener in physics_state.cpp).
            static auto* worlds = new std::map<CKIpionManager*, world_navs>();
            return *worlds;
        }

        CK3dEntity* find_entity(CKIpionManager* physics, const std::string& name) {
            if (!physics || !physics->m_Context || name.empty()) return nullptr;
            return CK3dEntity::Cast(physics->m_Context->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        }

        // Queued into m_PreSimulateCallbacks like the server's tick callback:
        // Process(pc) executes it at once, the first call only arms it (return
        // 0 keeps it queued); the PreSimulate pass then applies the pending
        // inputs and returns 1, which removes and deletes it.  The container
        // also deletes it when the manager resets, so the destructor clears
        // the queued flag.
        class navigation_callback final : public PhysicsCallback {
        public:
            navigation_callback(CKIpionManager* manager, CKBehavior* behavior) : PhysicsCallback(manager, behavior, 2) {}
            ~navigation_callback() override {
                auto it = registry().find(m_IpionManager);
                if (it != registry().end()) it->second.callback_queued = false;
            }
            int Execute() override {
                if (!armed_) {
                    armed_ = true;
                    return 0;
                }
                navigation_apply_pending(m_IpionManager);
                return 1;
            }
        private:
            bool armed_ = false;
        };

        bool ensure_callback(CKIpionManager* physics, world_navs& world, std::string& error) {
            if (world.callback_queued) return true;
            if (!physics->m_PreSimulateCallbacks) {
                error = "the physics manager has no PreSimulate callback container";
                return false;
            }
            CKBehavior* behavior = physics->m_Context
                ? CKBehavior::Cast(physics->m_Context->GetObject(world.behavior)) : nullptr;
            if (!behavior) {
                error = "the navigation callback needs an existing behavior to attach to";
                return false;
            }
            world.callback_queued = true;   // before Process: the arming call runs inside it
            physics->m_PreSimulateCallbacks->Process(new navigation_callback(physics, behavior));
            return world.callback_queued;
        }
    }

    bool navigation_create(CKIpionManager* physics, const char* ball_entity, const char* direction_ref_entity,
                           uint32_t behavior_id, const float (*directions)[3], int leaf_count, float force_value,
                           std::string& error) {
        error.clear();
        if (!physics || !physics->m_Context || !ball_entity || !*ball_entity || !direction_ref_entity
                || !*direction_ref_entity || !directions || leaf_count <= 0 || leaf_count > 8) {
            error = "invalid navigation arguments";
            return false;
        }
        CK3dEntity* ball = find_entity(physics, ball_entity);
        if (!ball) {
            error = std::string("no 3D entity named '") + ball_entity + "'";
            return false;
        }
        CK3dEntity* ref = find_entity(physics, direction_ref_entity);
        if (!ref) {
            ref = CK3dEntity::Cast(physics->m_Context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(direction_ref_entity)));
            if (!ref) {
                error = std::string("cannot create the direction reference '") + direction_ref_entity + "'";
                return false;
            }
        }
        std::vector<bmmo::game::navigation_leaf> leaves;
        for (int i = 0; i < leaf_count; ++i) {
            bmmo::game::navigation_leaf leaf;
            leaf.index = i;
            leaf.direction = VxVector(directions[i][0], directions[i][1], directions[i][2]);
            leaf.force_value = force_value;
            leaves.push_back(leaf);
        }
        world_navs& world = registry()[physics];
        world.behavior = behavior_id;
        nav_entry& entry = world.entries[ball_entity];
        entry.navigation = std::make_unique<player_navigation>(physics->m_Context, physics, ball, ref, leaves, force_value);
        entry.cam_ref = ref->GetID();
        entry.pending = false;
        return true;
    }

    bool navigation_input(CKIpionManager* physics, const char* ball_entity, uint8_t keys, const float right[3],
                          const float up[3], const float dir[3], bool active, std::string& error) {
        error.clear();
        if (!physics || !ball_entity) {
            error = "invalid navigation arguments";
            return false;
        }
        auto wit = registry().find(physics);
        if (wit == registry().end()) {
            error = "no navigation registered for this physics manager";
            return false;
        }
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end()) {
            error = std::string("no navigation for '") + ball_entity + "'";
            return false;
        }
        nav_entry& entry = it->second;
        entry.keys = keys;
        for (int k = 0; k < 3; ++k) {
            entry.rows[0][k] = right ? right[k] : (k == 0 ? 1.0f : 0.0f);
            entry.rows[1][k] = up ? up[k] : (k == 1 ? 1.0f : 0.0f);
            entry.rows[2][k] = dir ? dir[k] : (k == 2 ? 1.0f : 0.0f);
        }
        entry.active = active;
        entry.pending = true;
        return ensure_callback(physics, wit->second, error);
    }

    bool navigation_set_ball(CKIpionManager* physics, const char* ball_entity, const char* new_ball_entity,
                             float force_value, std::string& error) {
        error.clear();
        if (!physics || !ball_entity || !new_ball_entity || !*new_ball_entity) {
            error = "invalid navigation arguments";
            return false;
        }
        auto wit = registry().find(physics);
        if (wit == registry().end()) {
            error = "no navigation registered for this physics manager";
            return false;
        }
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end()) {
            error = std::string("no navigation for '") + ball_entity + "'";
            return false;
        }
        CK3dEntity* ball = find_entity(physics, new_ball_entity);
        if (!ball) {
            error = std::string("no 3D entity named '") + new_ball_entity + "'";
            return false;
        }
        nav_entry entry = std::move(it->second);
        wit->second.entries.erase(it);
        entry.navigation->set_ball(ball);
        entry.navigation->set_force_value(force_value);
        wit->second.entries[new_ball_entity] = std::move(entry);
        return true;
    }

    bool navigation_destroy(CKIpionManager* physics, const char* ball_entity, std::string& error) {
        error.clear();
        if (!physics || !ball_entity) {
            error = "invalid navigation arguments";
            return false;
        }
        auto wit = registry().find(physics);
        if (wit == registry().end()) return true;
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end()) return true;
        if (it->second.navigation) it->second.navigation->shutdown_all();
        wit->second.entries.erase(it);
        return true;
    }

    int navigation_count(CKIpionManager* physics) {
        auto wit = registry().find(physics);
        return wit == registry().end() ? 0 : static_cast<int>(wit->second.entries.size());
    }

    void navigation_apply_pending(CKIpionManager* physics) {
        auto wit = registry().find(physics);
        if (wit == registry().end() || !physics->m_Context) return;
        for (auto& [name, entry]: wit->second.entries) {
            if (!entry.pending || !entry.navigation) continue;
            entry.pending = false;
            // Direction reference: rotation rows from the input, position kept
            // (the retail Cam_OrientRef sits at the camera; only the rotation
            // reaches TransformVector).
            if (CK3dEntity* ref = CK3dEntity::Cast(physics->m_Context->GetObject(entry.cam_ref))) {
                VxMatrix matrix = ref->GetWorldMatrix();
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k) matrix[r][k] = entry.rows[r][k];
                ref->SetWorldMatrix(matrix);
            }
            uint8_t keys = static_cast<uint8_t>(entry.keys & 0x0F);
            bool active = entry.active;
            if (entry.poll) {
                keys = 0;
                active = false;
                auto* input = static_cast<CKInputManager*>(physics->m_Context->GetManagerByGuid(INPUT_MANAGER_GUID));
                const unsigned char* state = input ? input->GetKeyboardState() : nullptr;
                for (int i = 0; i < entry.poll_count && i < 8; ++i) {
                    const int code = entry.key_codes[i];
                    if (state && code > 0 && code < 256 && (state[code] & KS_PRESSED)) keys |= static_cast<uint8_t>(1u << i);
                    if (auto* block = CKBehavior::Cast(physics->m_Context->GetObject(entry.key_blocks[i])))
                        if (block->IsActive()) active = true;
                }
            }
            entry.navigation->apply(keys, active);
        }
    }

    bool navigation_poll(CKIpionManager* physics, const char* ball_entity, bool enable, const int* key_codes,
                         const uint32_t* key_blocks, int count, std::string& error) {
        error.clear();
        if (!physics || !ball_entity || (enable && (!key_codes || !key_blocks || count <= 0 || count > 8))) {
            error = "invalid navigation arguments";
            return false;
        }
        auto wit = registry().find(physics);
        if (wit == registry().end()) {
            error = "no navigation registered for this physics manager";
            return false;
        }
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end()) {
            error = std::string("no navigation for '") + ball_entity + "'";
            return false;
        }
        nav_entry& entry = it->second;
        entry.poll = enable;
        entry.poll_count = enable ? count : 0;
        for (int i = 0; i < 8; ++i) {
            entry.key_codes[i] = enable && i < count ? key_codes[i] : 0;
            entry.key_blocks[i] = enable && i < count ? key_blocks[i] : 0;
        }
        return true;
    }

    bool navigation_get_state(CKIpionManager* physics, const char* ball_entity, bmmo_physics_nav_state& out,
                              std::string& error) {
        error.clear();
        out = {};
        auto wit = physics ? registry().find(physics) : registry().end();
        if (wit == registry().end() || !ball_entity) {
            error = "no navigation registered for this physics manager";
            return false;
        }
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end() || !it->second.navigation) {
            error = std::string("no navigation for '") + (ball_entity ? ball_entity : "") + "'";
            return false;
        }
        it->second.navigation->get_state(out);
        return true;
    }

    bool navigation_set_state(CKIpionManager* physics, const char* ball_entity, const bmmo_physics_nav_state& state,
                              std::string& error) {
        error.clear();
        auto wit = physics ? registry().find(physics) : registry().end();
        if (wit == registry().end() || !ball_entity) {
            error = "no navigation registered for this physics manager";
            return false;
        }
        auto it = wit->second.entries.find(ball_entity);
        if (it == wit->second.entries.end() || !it->second.navigation) {
            error = std::string("no navigation for '") + (ball_entity ? ball_entity : "") + "'";
            return false;
        }
        if (!it->second.navigation->set_state(state)) {
            error = "controllers could not be recreated (no body?)";
            return false;
        }
        return true;
    }
}
