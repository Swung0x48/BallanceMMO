#pragma once

// Physics world hashing and session clock reset against the Ballanced
// physics_RT (IVP).  Compiled into the server's static engine and into the
// client's physics_RT.dll (through physics_rt_bridge.cpp), so both sides run
// exactly this code.

#include <string>

#include <physics/world_hash.hpp>

class CKIpionManager;

namespace bmmo::physics {
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
}
