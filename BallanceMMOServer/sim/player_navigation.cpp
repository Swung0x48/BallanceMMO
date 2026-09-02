#include "player_navigation.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"

#include "ivp_controller.hxx"

namespace bmmo::sim {
    // Verbatim equivalent of PhysicsControllerForce in
    // physics_RT/Behaviors/PhysicsForce.cpp: the same maths in the same order,
    // so a clone driven by this controller integrates exactly like the retail
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
                                         CK3dEntity* direction_ref, const bmmo::game::navigation_graph& graph)
        : context_(context), physics_(physics), ball_(ball), direction_ref_(direction_ref) {
        leaves_.reserve(graph.leaves.size());
        for (const auto& definition: graph.leaves) {
            leaf l;
            l.definition = definition;
            leaves_.push_back(l);
        }
        if (!graph.leaves.empty()) force_value_ = graph.leaves.front().force_value;
    }

    player_navigation::~player_navigation() {
        for (auto& l: leaves_) {
            delete l.controller;
            l.controller = nullptr;
        }
    }

    void player_navigation::set_ball(CK3dEntity* ball) {
        if (ball == ball_) return;
        shutdown_all();
        ball_ = ball;
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

    // PhysicsForceCallback::Execute for `ball_` with the leaf's parameters.
    // Returns false when the ball has no physics object yet; the retail
    // container then keeps the callback queued and retries in every
    // PreSimulate pass until it succeeds, which we mirror with create_pending.
    bool player_navigation::try_create(leaf& l) {
        if (!ball_ || !physics_ || !physics_->GetEnvironment()) return false;
        PhysicsObject* object = physics_->GetPhysicsObject(ball_);
        if (!object || !object->m_RealObject) return false;
        IVP_Real_Object* real = object->m_RealObject;

        // Position 0,0,0 with Pos Referential == the ball itself: the retail
        // callback skips the transform and keeps the local origin.
        VxVector direction = l.definition.direction;
        VxVector vec;
        if (direction_ref_) direction_ref_->TransformVector(&vec, &direction);
        else vec = direction;

        IVP_U_Point force(vec.x, vec.y, vec.z);
        if (force.quad_length() <= 0.0001) force.set(1.0, 0.0, 0.0);
        else force.normize();
        force.mult(force_value_);

        IVP_U_Point position(0.0, 0.0, 0.0);
        l.controller = new force_controller(real, position, force);
        return true;
    }

    // Physics WakeUp: ensure_in_simulation() on the ball's object.  Like the
    // retail callback it is retried until the object exists.
    void player_navigation::wake_up() {
        if (!ball_ || !physics_) return;
        PhysicsObject* object = physics_->GetPhysicsObject(ball_);
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
}
