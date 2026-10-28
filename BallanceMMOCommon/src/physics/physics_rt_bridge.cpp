// The BallanceMMO bridge compiled into the open-source physics_RT plugin.
// See physics/physics_rt_api.h for the contract; the work is done by the
// shared physics_state.cpp, which the headless server links directly.

#include <physics/physics_rt_api.h>
#include <physics/physics_state.hpp>
#include <physics/physics_savestate.hpp>
#include <physics/ball_navigation.hpp>

#include "CKIpionManager.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <bmmo_build_id.h>

void ivp_srand(int seed);
int ivp_srand_read();

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

    int32_t api_set_clock_guard(void* manager, int32_t enable, float time_factor, uint32_t behavior_id,
                                uint32_t pause_behavior_id, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_clock_guard(static_cast<CKIpionManager*>(manager), enable != 0, time_factor,
                                            behavior_id, pause_behavior_id, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_body_state_recheck(void* manager, const char* entity_name, const double position[3],
                                       const double rotation[4], const float linear[3],
                                       const float angular[3], int32_t wake,
                                       char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_body_state(static_cast<CKIpionManager*>(manager), entity_name,
                                           position, rotation, linear, angular, wake != 0, text, true)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_body_state_ex(void* manager, const char* entity_name, const double position[3],
                                  const double rotation[4], const float linear[3],
                                  const float angular[3], int32_t wake_mode, int32_t recheck,
                                  char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        auto mode = bmmo::physics::wake_mode::keep;
        if (wake_mode == static_cast<int32_t>(bmmo::physics::wake_mode::wake))
            mode = bmmo::physics::wake_mode::wake;
        else if (wake_mode == static_cast<int32_t>(bmmo::physics::wake_mode::freeze))
            mode = bmmo::physics::wake_mode::freeze;
        std::string text;
        if (!bmmo::physics::set_body_state(static_cast<CKIpionManager*>(manager), entity_name,
                                           position, rotation, linear, angular, mode, recheck != 0, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_get_core_extra(void* manager, const char* entity_name, bmmo_physics_core_extra* out,
                               char* error, uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::get_core_extra(static_cast<CKIpionManager*>(manager), entity_name, *out, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_core_extra(void* manager, const char* entity_name, const bmmo_physics_core_extra* in,
                               char* error, uint32_t error_size) {
        if (!manager || !in) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_core_extra(static_cast<CKIpionManager*>(manager), entity_name, *in, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_get_clock_state(void* manager, bmmo_physics_clock_state* out, char* error, uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::get_clock_state(static_cast<CKIpionManager*>(manager), *out, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_clock_state(void* manager, const bmmo_physics_clock_state* in, char* error, uint32_t error_size) {
        if (!manager || !in) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::set_clock_state(static_cast<CKIpionManager*>(manager), *in, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_force_block_get(void* manager, uint32_t behavior_id, bmmo_physics_force_block* out, char* error,
                                uint32_t error_size) {
        if (!manager || !out) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::force_block_get(static_cast<CKIpionManager*>(manager), behavior_id, *out, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_force_block_set(void* manager, uint32_t behavior_id, const bmmo_physics_force_block* in, char* error,
                                uint32_t error_size) {
        if (!manager || !in) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        if (!bmmo::physics::force_block_set(static_cast<CKIpionManager*>(manager), behavior_id, *in, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_contacts_get(void* manager, const char* entity, bmmo_physics_contact* out, int32_t max, char* error,
                             uint32_t error_size) {
        if (!manager || !entity) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::string text;
        const int n = bmmo::physics::get_contacts(static_cast<CKIpionManager*>(manager), entity, out, out ? max : 0, text);
        if (n < 0) set_error(error, error_size, text);
        return n;
    }

    int32_t api_contacts_set(void* manager, const char* entity, const bmmo_physics_contact* in, int32_t count, uint32_t flags,
                             char* error, uint32_t error_size) {
        if (!manager || !entity) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::string text;
        const int made = bmmo::physics::set_contacts(static_cast<CKIpionManager*>(manager), entity, in, count, flags, text);
        if (!text.empty()) set_error(error, error_size, text);
        return made;
    }

    int32_t api_set_body_matrix(void* manager, const char* entity, const float matrix[16], int32_t simulated, char* error,
                                uint32_t error_size) {
        if (!manager || !entity || !matrix) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::string text;
        if (!bmmo::physics::set_body_matrix(static_cast<CKIpionManager*>(manager), entity, matrix, simulated != 0, text)) {
            set_error(error, error_size, text);
            return -1;
        }
        return 1;
    }

    int32_t api_body_recheck(void* manager, const char* entity, char* error, uint32_t error_size) {
        if (!manager || !entity) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::string text;
        if (!bmmo::physics::recheck_body(static_cast<CKIpionManager*>(manager), entity, text)) {
            set_error(error, error_size, text);
            return -1;
        }
        return 1;
    }

    // v17: the IVP savestate.  The blob is captured into a vector and copied
    // out, snprintf style: at most `max` bytes written, the full size returned
    // (`out` may be NULL to query it).
    int32_t api_savestate_get(void* manager, uint8_t* out, uint32_t max, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::vector<uint8_t> blob;
        std::string text;
        if (!bmmo::physics::get_savestate(static_cast<CKIpionManager*>(manager), blob, text)) {
            set_error(error, error_size, text);
            return -1;
        }
        if (out && max && !blob.empty()) std::memcpy(out, blob.data(), blob.size() < max ? blob.size() : static_cast<size_t>(max));
        return static_cast<int32_t>(blob.size());
    }

    int32_t api_savestate_set(void* manager, const uint8_t* data, uint32_t size, uint32_t flags, char* error,
                              uint32_t error_size, char* dropped, uint32_t dropped_size, char* unknown, uint32_t unknown_size) {
        if (dropped && dropped_size) dropped[0] = '\0';
        if (unknown && unknown_size) unknown[0] = '\0';
        if (!manager || (!data && size)) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        std::string text;
        bmmo::physics::savestate_report report;
        const bool ok = bmmo::physics::set_savestate(static_cast<CKIpionManager*>(manager), data, size, flags, text, &report);
        // A name list that does not fit is a failure, not a truncation: a body
        // dropped from it would never be rechecked or given its contacts, and
        // the caller's per-body fallback is the safer outcome.
        const auto join_into = [](char* out, uint32_t out_size, const std::vector<std::string>& names, std::string& overflow) {
            if (!out || !out_size) return;
            std::string joined;
            for (const std::string& name: names) joined += (joined.empty() ? "" : ";") + name;
            if (joined.size() >= out_size) {
                overflow = "the names of " + std::to_string(names.size()) + " bodies (" + std::to_string(joined.size())
                         + " bytes) exceed the caller's buffer of " + std::to_string(out_size);
                out[0] = '\0';
                return;
            }
            std::snprintf(out, out_size, "%s", joined.c_str());
        };
        std::string overflow;
        join_into(dropped, dropped_size, report.contacts_dropped, overflow);
        join_into(unknown, unknown_size, report.unknown_bodies, overflow);
        if (!ok) {
            set_error(error, error_size, text);
            return 0;
        }
        if (!overflow.empty()) {
            set_error(error, error_size, overflow);
            return 0;
        }
        return 1;
    }

    // v20: a travelling savestate (the resync state's server blob).  The
    // rename list is "old=new;old=new", the excluded prefixes ";"-joined.
    int32_t api_savestate_set_travelling(void* manager, const uint8_t* data, uint32_t size, uint32_t flags,
                                         const char* renames, const char* excluded_prefixes, char* error,
                                         uint32_t error_size, char* dropped, uint32_t dropped_size, char* unknown,
                                         uint32_t unknown_size, char* summary, uint32_t summary_size) {
        if (dropped && dropped_size) dropped[0] = '\0';
        if (unknown && unknown_size) unknown[0] = '\0';
        if (summary && summary_size) summary[0] = '\0';
        if (!manager || (!data && size)) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        const auto split = [](const char* joined) {
            std::vector<std::string> out;
            std::string item;
            for (const char* p = joined ? joined : ""; *p; ++p) {
                if (*p == ';') { if (!item.empty()) out.push_back(item); item.clear(); }
                else item += *p;
            }
            if (!item.empty()) out.push_back(item);
            return out;
        };
        bmmo::physics::savestate_travel travel;
        for (const std::string& pair: split(renames)) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            travel.renames.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
        }
        travel.excluded_prefixes = split(excluded_prefixes);
        std::string text;
        bmmo::physics::savestate_report report;
        const bool ok = bmmo::physics::set_savestate(static_cast<CKIpionManager*>(manager), data, size, flags, text, &report,
                                                     travel);
        const auto join_into = [](char* out, uint32_t out_size, const std::vector<std::string>& names, std::string& overflow) {
            if (!out || !out_size) return;
            std::string joined;
            for (const std::string& name: names) joined += (joined.empty() ? "" : ";") + name;
            if (joined.size() >= out_size) {
                overflow = "the names of " + std::to_string(names.size()) + " bodies (" + std::to_string(joined.size())
                         + " bytes) exceed the caller's buffer of " + std::to_string(out_size);
                out[0] = '\0';
                return;
            }
            std::snprintf(out, out_size, "%s", joined.c_str());
        };
        std::string overflow;
        join_into(dropped, dropped_size, report.contacts_dropped, overflow);
        join_into(unknown, unknown_size, report.unknown_bodies, overflow);
        if (summary && summary_size) {
            std::string line = "skipped=" + std::to_string(report.skipped) + " excluded=" + std::to_string(report.excluded)
                             + " renamed=" + std::to_string(report.renamed);
            // A name list that does not fit the caller's buffer is a
            // reporting limit, not a failed restore: the world is restored.
            if (!overflow.empty()) line += " | " + overflow;
            for (size_t i = 0; i < report.warnings.size() && i < 4; ++i) line += " | " + report.warnings[i];
            std::snprintf(summary, summary_size, "%s", line.c_str());
        }
        if (!ok) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    // v21: the trafo sets.
    int32_t api_trafo_set_register(void* manager, uint32_t player, const uint32_t frames[3], const uint32_t* explosions,
                                   int32_t explosion_count, bmmo_physics_trafo_set_report* out, char* error,
                                   uint32_t error_size) {
        if (!manager || !frames) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        bmmo_physics_trafo_set_report report{};
        std::string text;
        const bool ok = bmmo::physics::trafo_set_register(static_cast<CKIpionManager*>(manager), player, frames, explosions,
                                                          explosion_count, report, text);
        if (out) *out = report;
        if (!ok) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_trafo_set_unregister(void* manager, uint32_t player, char* error, uint32_t error_size) {
        std::string text;
        if (!bmmo::physics::trafo_set_unregister(static_cast<CKIpionManager*>(manager), player, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    void api_trafo_set_seed(void* manager, uint32_t frame_id, int32_t seed) {
        bmmo::physics::trafo_set_seed(static_cast<CKIpionManager*>(manager), frame_id, seed);
    }

    int32_t api_trafo_set_unmake(void* manager, uint32_t frame_id, char* error, uint32_t error_size) {
        std::string text;
        const int removed = bmmo::physics::trafo_set_unmake(static_cast<CKIpionManager*>(manager), frame_id, text);
        if (removed < 0) set_error(error, error_size, text);
        return removed;
    }

    int32_t api_trafo_set_filter(void* manager, int32_t enable, char* error, uint32_t error_size) {
        std::string text;
        if (!bmmo::physics::trafo_set_filter(static_cast<CKIpionManager*>(manager), enable != 0, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    // v24: the airship zone (step BALA).
    int32_t api_airship_zone_set(void* manager, int32_t enable, const char* own_group, double* box_out, char* error,
                                 uint32_t error_size) {
        std::string text;
        bmmo::physics::airship_zone_box box;
        if (!bmmo::physics::airship_zone_set(static_cast<CKIpionManager*>(manager), enable != 0, own_group, box, text)) {
            set_error(error, error_size, text);
            return -1;
        }
        if (box_out)
            for (int k = 0; k < 3; ++k) {
                box_out[k] = box.min[k];
                box_out[3 + k] = box.max[k];
            }
        return box.active ? 1 : 0;
    }

    int32_t api_airship_zone_contains(void* manager, const char* entity_name) {
        return bmmo::physics::airship_zone_contains(static_cast<CKIpionManager*>(manager), entity_name);
    }

    // v25: the spawn ghost (step SPGH).
    int32_t api_spawn_ghost_set(void* manager, int32_t enable, int32_t respawns, double radius, double* info_out,
                                char* error, uint32_t error_size) {
        std::string text;
        bmmo::physics::spawn_ghost_info info;
        if (!bmmo::physics::spawn_ghost_set(static_cast<CKIpionManager*>(manager), enable != 0, respawns != 0, radius, info,
                                            text)) {
            set_error(error, error_size, text);
            return -1;
        }
        if (info_out) {
            info_out[0] = static_cast<double>(info.areas);
            info_out[1] = info.radius;
            info_out[2] = static_cast<double>(info.solidified);
            info_out[3] = static_cast<double>(info.rechecks);
            info_out[4] = static_cast<double>(info.overlap_waits);
            info_out[5] = static_cast<double>(info.births);
            info_out[6] = static_cast<double>(info.resumed);
        }
        return info.active ? 1 : 0;
    }

    int32_t api_spawn_ghost_state(void* manager, const char* entity_name) {
        return bmmo::physics::spawn_ghost_state(static_cast<CKIpionManager*>(manager), entity_name);
    }

    int32_t api_spawn_ghost_birth(void* manager, const char* group) {
        return bmmo::physics::spawn_ghost_birth(static_cast<CKIpionManager*>(manager), group) ? 1 : 0;
    }

    int32_t api_airship_zone_get(void* manager, double* box_out) {
        bmmo::physics::airship_zone_box box;
        bmmo::physics::airship_zone_get(static_cast<CKIpionManager*>(manager), box);
        if (!box.active) return -1;
        if (box_out)
            for (int k = 0; k < 3; ++k) {
                box_out[k] = box.min[k];
                box_out[3 + k] = box.max[k];
            }
        return box.ready ? 1 : 0;
    }

    // v22: the savestate's controller alias through a C callback (the retail
    // host's trafo registry answers it; physics_savestate.hpp).
    void api_set_controller_alias(void* manager, int32_t (*alias)(void* user, const char* name, char* out, uint32_t out_size),
                                  void* user) {
        if (!manager) return;
        auto* physics = static_cast<CKIpionManager*>(manager);
        if (!alias) {
            bmmo::physics::set_controller_alias(physics, {});
            return;
        }
        bmmo::physics::set_controller_alias(physics, [alias, user](const char* name, std::string& out) {
            char text[128] = {};
            if (!alias(user, name, text, sizeof(text))) return false;
            text[sizeof(text) - 1] = 0;
            out = text;
            return true;
        });
    }

    // v23: the PreSimulate queue (rollback-engine-gaps design 2.2.2, gap Q).
    int32_t api_callbacks_get(void* manager, bmmo_physics_callback* out, int32_t max, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::vector<bmmo_physics_callback> all;
        std::string text;
        if (!bmmo::physics::callbacks_get(static_cast<CKIpionManager*>(manager), all, text)) {
            set_error(error, error_size, text);
            return -1;
        }
        for (int32_t i = 0; out && i < max && i < static_cast<int32_t>(all.size()); ++i) out[i] = all[static_cast<size_t>(i)];
        return static_cast<int32_t>(all.size());
    }

    int32_t api_callbacks_rebuild(void* manager, const uint32_t* scope, int32_t n, const bmmo_physics_callback* target,
                                  int32_t count, char* error, uint32_t error_size) {
        if (!manager || (n > 0 && !scope) || (count > 0 && !target)) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        const std::vector<uint32_t> s(scope, scope + (n > 0 ? n : 0));
        const std::vector<bmmo_physics_callback> t(target, target + (count > 0 ? count : 0));
        std::string text;
        const int removed = bmmo::physics::callbacks_rebuild(static_cast<CKIpionManager*>(manager), s, t, text);
        if (removed < 0) set_error(error, error_size, text);
        return removed;
    }

    int32_t api_callbacks_hold(void* manager, const uint32_t* executed, int32_t n, char* error, uint32_t error_size) {
        if (!manager || (n > 0 && !executed)) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        const std::vector<uint32_t> e(executed, executed + (n > 0 ? n : 0));
        std::string text;
        const int held = bmmo::physics::callbacks_hold(static_cast<CKIpionManager*>(manager), e, text);
        if (held < 0) set_error(error, error_size, text);
        return held;
    }

    int32_t api_callbacks_unhold(void* manager, char* error, uint32_t error_size) {
        if (!manager) {
            set_error(error, error_size, "null argument");
            return -1;
        }
        std::string text;
        return bmmo::physics::callbacks_unhold(static_cast<CKIpionManager*>(manager), text);
    }

    int32_t api_callback_scope_set(void* manager, int32_t mode, const uint32_t* scope, int32_t n, char* error,
                                   uint32_t error_size) {
        if (!manager || (n > 0 && !scope)) {
            set_error(error, error_size, "null argument");
            return 0;
        }
        const std::vector<uint32_t> s(scope, scope + (n > 0 ? n : 0));
        std::string text;
        if (!bmmo::physics::callback_scope_set(static_cast<CKIpionManager*>(manager), mode, s, text)) {
            set_error(error, error_size, text);
            return 0;
        }
        return 1;
    }

    int32_t api_set_event_log_muted(void* manager, int32_t muted, char* error, uint32_t error_size) {
        (void)manager; (void)error; (void)error_size;
        bmmo::physics::set_event_log_muted(muted != 0);
        return 1;
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
        api_set_clock_guard,
        api_set_body_state_recheck,
        api_set_body_state_ex,
        api_get_core_extra,
        api_set_core_extra,
        api_get_clock_state,
        api_set_clock_state,
        api_force_block_get,
        api_force_block_set,
        api_set_event_log_muted,
        api_contacts_get,
        api_contacts_set,
        api_body_recheck,
        api_savestate_get,
        api_savestate_set,
        api_set_body_matrix,
        api_savestate_set_travelling,
        api_trafo_set_register,
        api_trafo_set_unregister,
        api_trafo_set_seed,
        api_trafo_set_unmake,
        api_trafo_set_filter,
        api_set_controller_alias,
        api_callbacks_get,
        api_callbacks_rebuild,
        api_callbacks_hold,
        api_callbacks_unhold,
        api_callback_scope_set,
        api_airship_zone_set,
        api_airship_zone_contains,
        api_airship_zone_get,
        api_spawn_ghost_set,
        api_spawn_ghost_state,
        api_spawn_ghost_birth,
    };
}

extern "C" BMMO_PHYSICS_EXPORT const bmmo_physics_api_v2* bmmo_physics_api(uint32_t requested_version) {
    redirect_stdout_once();
    return requested_version == BMMO_PHYSICS_API_VERSION ? &kApi : nullptr;
}
