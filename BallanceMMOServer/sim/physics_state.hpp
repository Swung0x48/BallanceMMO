#pragma once

// Physics world hashing and session clock reset for the headless engine.
// Field order matches BallanceMMOClient/physics/retail_physics_view.cpp.

#include <string>

#include <physics/world_hash.hpp>

class CKIpionManager;

namespace bmmo::sim {
    bool capture_world_hash(CKIpionManager* physics, bmmo::physics::world_hash& out,
                            std::string& error);
    bool reset_session_clock(CKIpionManager* physics, int seed, std::string& error);
}
