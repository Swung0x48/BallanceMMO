#pragma once

// The client's window onto the physics world, through the BallanceMMO bridge
// exported by the open-source physics_RT plugin (physics/physics_rt_api.h).
// The mod never touches IVP layouts: everything runs inside the plugin, from
// the same physics_state.cpp the headless server links.  With the retail
// physics_RT.dll there is no bridge and initialize() fails, which is how
// physics sessions get reported as unavailable.

#include <string>

#include <physics/physics_rt_api.h>
#include <physics/world_hash.hpp>

class CKContext;

namespace bmmo::physics {
    class physics_view {
    public:
        bool initialize(CKContext* context, std::string& error);
        void shutdown();
        bool available() const { return api_ != nullptr && manager_ != nullptr; }

        // Hash of the IVP world after the last physics step.
        bool capture(world_hash& out, std::string& error) const;
        // Session clock + RNG reset performed at the same tick as the server.
        bool reset_session_clock(int seed, std::string& error) const;
        bool reset_random(int seed, std::string& error) const;
        // Diagnostics: the movable bodies the world simulates right now.
        std::string describe_movable_objects() const;
        std::string describe_physics_objects() const;
        std::string drain_event_log() const;
        std::string describe_cores_exact() const;

        // Identification of the loaded physics module (filled even when the
        // bridge is missing, so the retail DLL can be reported).
        const std::string& dll_sha256() const { return dll_sha256_; }
        const std::string& dll_path() const { return dll_path_; }
        const std::string& build_id() const { return build_id_; }

    private:
        CKContext* context_ = nullptr;
        void* manager_ = nullptr;
        const bmmo_physics_api_v1* api_ = nullptr;
        std::string dll_sha256_;
        std::string dll_path_;
        std::string build_id_;
    };
}
