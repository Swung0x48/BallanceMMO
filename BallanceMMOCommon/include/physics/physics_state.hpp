#pragma once

// Physics world hashing, session clock reset and body manipulation against the
// Ballanced physics_RT (IVP).  Compiled into the server's static engine and
// into the client's physics_RT.dll (through physics_rt_bridge.cpp), so both
// sides run exactly this code.

#include <string>

#include <physics/physics_rt_api.h>
#include <physics/world_hash.hpp>

class CKIpionManager;

namespace bmmo::physics {
    // The bridge ABI structs are used directly: no field-by-field copy at the
    // C boundary, and the two sides can never drift apart.
    using body_state = bmmo_physics_body_state;
    using ball_recipe = bmmo_physics_ball_recipe;

    bool capture_world_hash(CKIpionManager* physics, bmmo::physics::world_hash& out,
                            std::string& error);
    bool reset_session_clock(CKIpionManager* physics, int seed, std::string& error);
    // "name[state];name[state];..." for every entry of m_MovableObjects, in
    // list order (diagnostics: which bodies the world simulates).
    std::string describe_movable_objects(CKIpionManager* physics);
    // Every physicalized entity: "name[movement_state](x,y,z);..." (all
    // bodies, simulated or asleep; position = core position at last PSI).
    std::string describe_physics_objects(CKIpionManager* physics);
    // Diagnostics: object lifecycle events (created/deleted/revived/frozen)
    // observed through a global IVP listener since the previous call;
    // installs the listener on first use (and after the environment changes).
    std::string drain_event_log(CKIpionManager* physics);
    // Diagnostics: every simulated core with its exact state (hex floats), one
    // line per core, sorted by name so two worlds can be diffed textually.
    std::string describe_cores_exact(CKIpionManager* physics);
    // Diagnostics: route IVP's own impact / mindist / PSI debug prints
    // (IVP_Debug_Manager::file_out_impacts) to `path`; null path stops.
    bool set_impact_trace(CKIpionManager* physics, const char* path, std::string& error);

    // ---- bridge API v2 (design 8.4) ----

    // Every physicalized entity in physics-object table order.  Fills at most
    // `max` entries and returns the total count (like snprintf), so `out` may
    // be null or too small.  Poses are the object pose at the environment's
    // current time, which after a tick is the last PSI.
    int list_bodies(CKIpionManager* physics, body_state* out, int max);
    // One body, by CK3dEntity name.
    bool get_body_state(CKIpionManager* physics, const char* entity_name, body_state& out,
                        std::string& error);
    // Beam the body and overwrite its core speeds (pending async pushes are
    // dropped).  Any array may be null to leave that part untouched.  `wake`
    // wakes the body; otherwise a simulated body is frozen.  The CK entity's
    // world matrix is refreshed exactly like CKIpionManager does after a step.
    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], bool wake,
                        std::string& error);
    // The retail Physicalize block's recipe path, driven from a POD recipe
    // instead of a behavior graph.  Already-physicalized entities succeed
    // without doing anything (like the block).  `collision_group` is the IVP
    // nocoll group ident: at most IVP_NO_COLL_GROUP_STRING_LEN - 1 characters.
    bool physicalize(CKIpionManager* physics, const char* entity_name, const ball_recipe& recipe,
                     const char* collision_group, std::string& error);
    // The block's Unphysicalize branch.  Not an error when nothing is there.
    bool unphysicalize(CKIpionManager* physics, const char* entity_name, std::string& error);
    // Add the BMMO player filter to the environment's IVP_Meta_Collision_Filter
    // (design 8.2): player balls (nocoll group ident starting with
    // `player_group_prefix`) collide with each other but not with the level's
    // "Ball" group.  Idempotent per environment.
    bool install_player_collision_filter(CKIpionManager* physics, const char* player_group_prefix,
                                         std::string& error);
    // IVP_Real_Object::change_nocoll_group_ident on a physicalized entity.
    bool set_body_group(CKIpionManager* physics, const char* entity_name, const char* collision_group,
                        std::string& error);
}
