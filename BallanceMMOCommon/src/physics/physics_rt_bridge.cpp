// The BallanceMMO bridge compiled into the open-source physics_RT plugin.
// See physics/physics_rt_api.h for the contract; the work is done by the
// shared physics_state.cpp, which the headless server links directly.

#include <physics/physics_rt_api.h>
#include <physics/physics_state.hpp>
#include <physics/ball_navigation.hpp>

#include "CKIpionManager.h"

#include <cstdio>
#include <cstdlib>
#include <string>

void ivp_srand(int seed);
int ivp_srand_read();

#ifndef BMMO_PHYSICS_BUILD_ID
#define BMMO_PHYSICS_BUILD_ID "unknown"
#endif

#if defined(_WIN32)
#define BMMO_PHYSICS_EXPORT __declspec(dllexport)
#else
#define BMMO_PHYSICS_EXPORT __attribute__((visibility("default")))
#endif

namespace {
    // BMMO_PHYSICS_STDOUT=<path>: the engine's own diagnostics (IVP messages,
    // qhull errors) go to a file; a GUI host has no console for them.
    void redirect_stdout_once() {
        static bool done = false;
        if (done) return;
        done = true;
        const char* path = std::getenv("BMMO_PHYSICS_STDOUT");
        if (!path || !*path) return;
        if (std::freopen(path, "a", stdout)) std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::freopen(path, "a", stderr);
        std::printf("[bmmo] physics_RT %s: stdout redirected\n", BMMO_PHYSICS_BUILD_ID);
    }

    void set_error(char* buffer, uint32_t size, const std::string& text) {
        if (buffer && size) std::snprintf(buffer, size, "%s", text.c_str());
    }

    int32_t api_capture_world_hash(void* manager, bmmo_physics_world_hash* out,
                                   char* error, uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        bmmo::physics::world_hash hash;
        std::string text;
        if (!bmmo::physics::capture_world_hash(static_cast<CKIpionManager*>(manager), hash, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        *out = {};
        out->hash = hash.hash;
        out->pose = hash.pose;
        out->cores = hash.cores;
        out->ivp_seed = hash.ivp_seed;
        out->ivp_time = hash.ivp_time;
        out->delta_time_ms = hash.delta_time_ms;
        out->physics_delta_time = hash.physics_delta_time;
        out->time_factor = hash.time_factor;
        out->surfaces = hash.surfaces;
        std::snprintf(out->probe_name, sizeof(out->probe_name), "%s", hash.probe_name);
        for (int k = 0; k < 3; ++k) {
            out->probe_position[k] = hash.probe_position[k];
            out->probe_speed[k] = hash.probe_speed[k];
            out->probe_rot_speed[k] = hash.probe_rot_speed[k];
        }
        out->next_movement_check = hash.next_movement_check;
        out->time_of_last_psi = hash.time_of_last_psi;
        out->time_of_next_psi = hash.time_of_next_psi;
        return 1;
    }

    int32_t api_reset_session_clock(void* manager, int32_t seed, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::reset_session_clock(static_cast<CKIpionManager*>(manager), seed, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_get_random_seed() { return ivp_srand_read(); }
    void api_set_random_seed(int32_t seed) { ivp_srand(seed); }

    int32_t api_describe_movable_objects(void* manager, char* buffer, uint32_t buffer_size) {
        const std::string text = bmmo::physics::describe_movable_objects(static_cast<CKIpionManager*>(manager));
        if (buffer && buffer_size) std::snprintf(buffer, buffer_size, "%s", text.c_str());
        return static_cast<int32_t>(text.size());
    }

    int32_t api_describe_physics_objects(void* manager, char* buffer, uint32_t buffer_size) {
        const std::string text = bmmo::physics::describe_physics_objects(static_cast<CKIpionManager*>(manager));
        if (buffer && buffer_size) std::snprintf(buffer, buffer_size, "%s", text.c_str());
        return static_cast<int32_t>(text.size());
    }

    int32_t api_drain_event_log(void* manager, char* buffer, uint32_t buffer_size) {
        const std::string text = bmmo::physics::drain_event_log(static_cast<CKIpionManager*>(manager));
        if (buffer && buffer_size) std::snprintf(buffer, buffer_size, "%s", text.c_str());
        return static_cast<int32_t>(text.size());
    }

    int32_t api_describe_cores_exact(void* manager, char* buffer, uint32_t buffer_size) {
        const std::string text = bmmo::physics::describe_cores_exact(static_cast<CKIpionManager*>(manager));
        if (buffer && buffer_size) std::snprintf(buffer, buffer_size, "%s", text.c_str());
        return static_cast<int32_t>(text.size());
    }

    int32_t api_list_bodies(void* manager, bmmo_physics_body_state* out, int32_t max) {
        return bmmo::physics::list_bodies(static_cast<CKIpionManager*>(manager), out,
                                          max > 0 ? static_cast<int>(max) : 0);
    }

    int32_t api_get_body_state(void* manager, const char* entity_name, bmmo_physics_body_state* out,
                               char* error, uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::get_body_state(static_cast<CKIpionManager*>(manager), entity_name,
                                           *out, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_body_state(void* manager, const char* entity_name, const double position[3],
                               const double rotation[4], const float linear[3],
                               const float angular[3], int32_t wake,
                               char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_body_state(static_cast<CKIpionManager*>(manager), entity_name,
                                           position, rotation, linear, angular, wake != 0, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_physicalize(void* manager, const char* entity_name,
                            const bmmo_physics_ball_recipe* recipe, const char* collision_group,
                            char* error, uint32_t error_size) {
        if (!manager || !recipe) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::physicalize(static_cast<CKIpionManager*>(manager), entity_name,
                                        *recipe, collision_group, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_unphysicalize(void* manager, const char* entity_name,
                              char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::unphysicalize(static_cast<CKIpionManager*>(manager), entity_name, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_install_player_collision_filter(void* manager, const char* player_group_prefix,
                                                char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::install_player_collision_filter(static_cast<CKIpionManager*>(manager),
                                                            player_group_prefix, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_body_group(void* manager, const char* entity_name, const char* collision_group,
                               char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_body_group(static_cast<CKIpionManager*>(manager), entity_name, collision_group, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_create(void* manager, const char* ball_entity, const char* direction_ref_entity,
                                  uint32_t behavior_id, const float (*directions)[3], int32_t leaf_count,
                                  float force_value, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_create(static_cast<CKIpionManager*>(manager), ball_entity, direction_ref_entity,
                                              behavior_id, directions, leaf_count, force_value, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_input(void* manager, const char* ball_entity, uint8_t keys, const float right[3],
                                 const float up[3], const float dir[3], int32_t active,
                                 char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_input(static_cast<CKIpionManager*>(manager), ball_entity, keys, right, up, dir,
                                             active != 0, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_set_ball(void* manager, const char* ball_entity, const char* new_ball_entity,
                                    float force_value, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_set_ball(static_cast<CKIpionManager*>(manager), ball_entity, new_ball_entity,
                                                force_value, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_destroy(void* manager, const char* ball_entity, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_destroy(static_cast<CKIpionManager*>(manager), ball_entity, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_body_guard(void* manager, int32_t enable, const char* except_entity, char* error,
                               uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_body_guard(static_cast<CKIpionManager*>(manager), enable != 0, except_entity, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_get_clock(void* manager, float* time_factor, float* physics_delta, char* error, uint32_t error_size) {
        if (!manager || !time_factor || !physics_delta) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::get_clock(static_cast<CKIpionManager*>(manager), *time_factor, *physics_delta, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_step_physics(void* manager, float delta_ms, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::step_physics(static_cast<CKIpionManager*>(manager), delta_ms, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_poll(void* manager, const char* ball_entity, int32_t enable, const int32_t* key_codes,
                                const uint32_t* key_blocks, int32_t count, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_poll(static_cast<CKIpionManager*>(manager), ball_entity, enable != 0, key_codes,
                                            key_blocks, count, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_get_state(void* manager, const char* ball_entity, bmmo_physics_nav_state* out,
                                     char* error, uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_get_state(static_cast<CKIpionManager*>(manager), ball_entity, *out, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_navigation_set_state(void* manager, const char* ball_entity, const bmmo_physics_nav_state* state,
                                     char* error, uint32_t error_size) {
        if (!manager || !state) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::navigation_set_state(static_cast<CKIpionManager*>(manager), ball_entity, *state, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_push_impulse(void* manager, const char* entity_name, const float direction_ws[3], float speed,
                             uint32_t behavior_id, char* error, uint32_t error_size) {
        if (!manager || !direction_ws) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::push_impulse(static_cast<CKIpionManager*>(manager), entity_name, direction_ws, speed,
                                         behavior_id, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    void api_random_reset(int32_t seed) { bmmo::physics::random_reset(seed); }
    int32_t api_random_get_state() { return bmmo::physics::random_get_state(); }
    void api_random_set_state(int32_t state) { bmmo::physics::random_set_state(state); }
    int32_t api_random_next() { return bmmo::physics::random_next(); }

    int32_t api_install_random_block(void* ck_context) {
        return bmmo::physics::install_random_block(static_cast<CKContext*>(ck_context));
    }

    const bmmo_physics_api_v2 kApi = {
        sizeof(bmmo_physics_api_v2),
        BMMO_PHYSICS_API_VERSION,
        BMMO_PHYSICS_BUILD_ID,
        api_capture_world_hash,
        api_reset_session_clock,
        api_get_random_seed,
        api_set_random_seed,
        api_describe_movable_objects,
        api_describe_physics_objects,
        api_drain_event_log,
        api_describe_cores_exact,
        api_list_bodies,
        api_get_body_state,
        api_set_body_state,
        api_physicalize,
        api_unphysicalize,
        api_install_player_collision_filter,
        api_set_body_group,
        api_navigation_create,
        api_navigation_input,
        api_navigation_set_ball,
        api_navigation_destroy,
        api_step_physics,
        api_navigation_poll,
        api_navigation_get_state,
        api_navigation_set_state,
        api_set_body_guard,
        api_get_clock,
        api_push_impulse,
        api_random_reset,
        api_random_get_state,
        api_random_set_state,
        api_random_next,
        api_install_random_block,
    };
}

extern "C" BMMO_PHYSICS_EXPORT const bmmo_physics_api_v2* bmmo_physics_api(uint32_t requested_version) {
    redirect_stdout_once();
    return requested_version == BMMO_PHYSICS_API_VERSION ? &kApi : nullptr;
}
