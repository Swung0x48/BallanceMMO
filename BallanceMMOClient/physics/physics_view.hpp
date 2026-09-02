#pragma once

// The client's window onto the physics world, through the BallanceMMO bridge
// exported by the open-source physics_RT plugin (physics/physics_rt_api.h).
// The mod never touches IVP layouts: everything runs inside the plugin, from
// the same physics_state.cpp the headless server links.  With the retail
// physics_RT.dll there is no bridge and initialize() fails, which is how
// physics sessions get reported as unavailable.

#include <string>
#include <vector>

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

        // ---- bridge API v2 (design 8.4) ----

        // Every physicalized entity of the world, in physics-table order.
        std::vector<bmmo_physics_body_state> list_bodies() const;
        bool get_body_state(const char* entity_name, bmmo_physics_body_state& out,
                            std::string& error) const;
        // Beam a body and overwrite its core speeds; `wake` wakes it, otherwise
        // a simulated body is frozen.  Any array may be null to skip that part.
        bool set_body_state(const char* entity_name, const double position[3],
                            const double rotation[4], const float linear[3], const float angular[3],
                            bool wake, std::string& error) const;
        // The retail Physicalize recipe, applied without a behavior graph.
        bool physicalize(const char* entity_name, const bmmo_physics_ball_recipe& recipe,
                         const char* collision_group, std::string& error) const;
        bool unphysicalize(const char* entity_name, std::string& error) const;
        // Idempotent; must be re-run whenever the IVP environment is rebuilt.
        bool install_player_collision_filter(const char* player_group_prefix,
                                             std::string& error) const;
        bool set_body_group(const char* entity_name, const char* collision_group, std::string& error) const;

        // ---- bridge API v3 (design 9.1): navigation for mirrored remote balls ----
        bool navigation_create(const char* ball_entity, const char* direction_ref_entity, uint32_t behavior_id,
                               const float (*directions)[3], int leaf_count, float force_value, std::string& error) const;
        bool navigation_input(const char* ball_entity, uint8_t keys, const float right[3], const float up[3],
                              const float dir[3], bool active, std::string& error) const;
        bool navigation_set_ball(const char* ball_entity, const char* new_ball_entity, float force_value,
                                 std::string& error) const;
        bool navigation_destroy(const char* ball_entity, std::string& error) const;

        // Identification of the loaded physics module (filled even when the
        // bridge is missing, so the retail DLL can be reported).
        const std::string& dll_sha256() const { return dll_sha256_; }
        const std::string& dll_path() const { return dll_path_; }
        const std::string& build_id() const { return build_id_; }

    private:
        CKContext* context_ = nullptr;
        void* manager_ = nullptr;
        const bmmo_physics_api_v2* api_ = nullptr;
        std::string dll_sha256_;
        std::string dll_path_;
        std::string build_id_;
    };
}
