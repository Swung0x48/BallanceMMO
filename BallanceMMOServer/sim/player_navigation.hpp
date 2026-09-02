#pragma once

// Server-side replica of the retail ball navigation for one player's ball
// (design section 8.3).  The retail graph is a set of SetPhysicsForce leaves,
// each driven by a Key Event and followed by Physics WakeUp; this class
// reproduces the same engine calls, in the same order, from the key bits a
// client sends every tick, so the server's clone receives bit-identical
// pushes to the client's own ball.
//
//   Key Event   : per leaf, a level-triggered state; Pressed fires on the tick
//                 the key goes down, Released on the tick it comes up, and
//                 turning navigation off/on resets the state (retail
//                 Controllers/KeyEvent.cpp).
//   Create      : PhysicsForceCallback::Execute - force vector =
//                 Cam_OrientRef.TransformVector(direction), normalised, times
//                 the ball's force value; a PhysicsControllerForce pushes the
//                 core every PSI (physics_RT/Behaviors/PhysicsForce.cpp).
//   Shutdown    : the controller is deleted immediately.
//   WakeUp      : ensure_in_simulation() after every Create / Shutdown.
//
// Must be called on the simulation thread before CKContext::Process() of the
// tick the keys belong to.

#include <cstdint>
#include <vector>

#include <game/navigation_graph.hpp>

class CKIpionManager;
class CK3dEntity;
class CKContext;

namespace bmmo::sim {
    class force_controller;

    class player_navigation {
    public:
        player_navigation(CKContext* context, CKIpionManager* physics, CK3dEntity* ball,
                          CK3dEntity* direction_ref, const bmmo::game::navigation_graph& graph);
        ~player_navigation();
        player_navigation(const player_navigation&) = delete;
        player_navigation& operator=(const player_navigation&) = delete;

        // The ball entity the forces act on (changes after a transformation).
        void set_ball(CK3dEntity* ball);
        // Force value of the current ball type (Physicalize_GameBall "Force").
        void set_force_value(float value) { force_value_ = value; }
        float force_value() const { return force_value_; }

        // One tick of input: bit i of `keys` = leaf i held; `active` mirrors the
        // client's BallNav activate/deactivate state.
        void apply(uint8_t keys, bool active);
        // Shutdown of every leaf (BallNav deactivate, player leaves, ball dies).
        void shutdown_all();

        uint8_t held_mask() const;
        int controller_count() const;

    private:
        struct leaf {
            bmmo::game::navigation_leaf definition;
            bool key_state = false;
            force_controller* controller = nullptr;
            bool create_pending = false;
        };

        void create(leaf& l);
        void shutdown(leaf& l);
        void wake_up();
        bool try_create(leaf& l);

        CKContext* context_;
        CKIpionManager* physics_;
        CK3dEntity* ball_;
        CK3dEntity* direction_ref_;
        std::vector<leaf> leaves_;
        float force_value_ = 0.0f;
        bool active_ = false;
        bool wake_pending_ = false;
    };
}
