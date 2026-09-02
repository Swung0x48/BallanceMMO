#pragma once

// Replica of the retail Ball Navigation for one ball driven from network
// input (design sections 8.3 and 9.1).  The retail graph is a set of
// SetPhysicsForce leaves, each driven by a Key Event and followed by Physics
// WakeUp; this class reproduces the same engine calls, in the same order,
// from the key bits a client sends every tick, so a ball driven by it
// integrates bit-identically to the retail ball driven by the script.
//
//   Key Event   : per leaf, a level-triggered state; Pressed fires on the tick
//                 the key goes down, Released on the tick it comes up, and
//                 turning navigation off/on resets the state (retail
//                 Controllers/KeyEvent.cpp).
//   Create      : PhysicsForceCallback::Execute - force vector =
//                 direction_ref.TransformVector(direction), normalised, times
//                 the ball's force value; a PhysicsControllerForce pushes the
//                 core every PSI (physics_RT/Behaviors/PhysicsForce.cpp).
//   Shutdown    : the controller is deleted immediately.
//   WakeUp      : ensure_in_simulation() after every Create / Shutdown.
//
// Compiled into the client's physics_RT.dll (through the bridge) and into
// the headless server, so both sides run exactly this code.  The server
// drives its clones through player_navigation directly from its PreSimulate
// callback; a client drives the mirrored remote balls through the registry
// functions below, which queue the same calls into the manager's
// PreSimulate pass (the moment the retail script's forces would act).
//
// Entities are held by CK_ID: a level change destroys the objects, and a
// stale pointer must not be dereferenced.

#include <cstdint>
#include <string>
#include <vector>

#include <game/navigation_graph.hpp>
#include <physics/physics_rt_api.h>

class CKIpionManager;
class CK3dEntity;
class CKContext;

namespace bmmo::physics {
    class force_controller;

    class player_navigation {
    public:
        player_navigation(CKContext* context, CKIpionManager* physics, CK3dEntity* ball,
                          CK3dEntity* direction_ref, const std::vector<bmmo::game::navigation_leaf>& leaves,
                          float force_value);
        ~player_navigation();
        player_navigation(const player_navigation&) = delete;
        player_navigation& operator=(const player_navigation&) = delete;

        // The ball entity the forces act on (changes after a transformation).
        void set_ball(CK3dEntity* ball);
        CK3dEntity* ball() const;
        CK3dEntity* direction_ref() const;
        // Force value of the current ball type (Physicalize_GameBall "Force").
        void set_force_value(float value) { force_value_ = value; }
        float force_value() const { return force_value_; }

        // One tick of input: bit i of `keys` = leaf i held; `active` mirrors the
        // client's BallNav activate/deactivate state.  Must run before the
        // physics step of the tick the keys belong to (PreSimulate).
        void apply(uint8_t keys, bool active);
        // Shutdown of every leaf (BallNav deactivate, player leaves, ball dies).
        void shutdown_all();

        uint8_t held_mask() const;
        int controller_count() const;

        // Rollback support (design 9.6): the internal state a re-simulation
        // has to start from.  set_state deletes every controller and recreates
        // the ones in controller_mask with the stored force vectors.
        void get_state(bmmo_physics_nav_state& out) const;
        bool set_state(const bmmo_physics_nav_state& state);
        bool active() const { return active_; }

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
        bool create_with_force(leaf& l, const float force[3]);

        CKContext* context_;
        CKIpionManager* physics_;
        uint32_t ball_id_ = 0;
        uint32_t direction_ref_id_ = 0;
        std::vector<leaf> leaves_;
        float force_value_ = 0.0f;
        bool active_ = false;
        bool wake_pending_ = false;
    };

    // ---- registry: navigation for balls driven through the bridge ----
    //
    // One entry per ball entity name and physics manager.  navigation_input()
    // stores the input of the next tick and makes sure one PreSimulate
    // callback (anchored to `behavior_id`, the level's "Ball Navigation"
    // script, because the container drops callbacks without a behavior) is
    // queued; the callback writes every entry's direction reference matrix
    // from its camera rows and calls apply(), in entity-name order.
    // `directions` are the leaf force directions in leaf order (index i).

    bool navigation_create(CKIpionManager* physics, const char* ball_entity, const char* direction_ref_entity,
                           uint32_t behavior_id, const float (*directions)[3], int leaf_count, float force_value,
                           std::string& error);
    bool navigation_input(CKIpionManager* physics, const char* ball_entity, uint8_t keys, const float right[3],
                          const float up[3], const float dir[3], bool active, std::string& error);
    // The player's ball changed entity (transformation): forces move to the
    // new entity, the old ones are shut down.
    bool navigation_set_ball(CKIpionManager* physics, const char* ball_entity, const char* new_ball_entity,
                             float force_value, std::string& error);
    bool navigation_destroy(CKIpionManager* physics, const char* ball_entity, std::string& error);
    // Number of registry entries for the manager (diagnostics).
    int navigation_count(CKIpionManager* physics);
    // Polling mode (design 9.6, the own ball): at apply time the keys come
    // from the input manager (key_codes[i] = CKKEY of leaf i) and BallNav
    // active from the Key Event blocks (key_blocks[i]); navigation_input then
    // only supplies the camera rows.
    bool navigation_poll(CKIpionManager* physics, const char* ball_entity, bool enable, const int* key_codes,
                         const uint32_t* key_blocks, int count, std::string& error);
    bool navigation_get_state(CKIpionManager* physics, const char* ball_entity, bmmo_physics_nav_state& out,
                              std::string& error);
    bool navigation_set_state(CKIpionManager* physics, const char* ball_entity, const bmmo_physics_nav_state& state,
                              std::string& error);
    // Applies every pending input of the manager now (what the queued
    // PreSimulate callback does); exposed for hosts with their own callback.
    void navigation_apply_pending(CKIpionManager* physics);
}
