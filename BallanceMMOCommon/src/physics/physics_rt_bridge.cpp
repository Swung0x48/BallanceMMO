// The BallanceMMO bridge compiled into the open-source physics_RT plugin.
// See physics/physics_rt_api.h for the contract; the work is done by the
// shared physics_state.cpp, which the headless server links directly.

#include <physics/physics_rt_api.h>
#include <physics/physics_state.hpp>

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

    const bmmo_physics_api_v1 kApi = {
        sizeof(bmmo_physics_api_v1),
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
    };
}

extern "C" BMMO_PHYSICS_EXPORT const bmmo_physics_api_v1* bmmo_physics_api(uint32_t requested_version) {
    redirect_stdout_once();
    return requested_version == BMMO_PHYSICS_API_VERSION ? &kApi : nullptr;
}
