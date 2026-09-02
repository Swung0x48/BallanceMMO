#pragma once

/* BallanceMMO physics bridge exported by the open-source physics_RT plugin.
 *
 * The client mod never touches IVP layouts itself: it resolves the exported
 * symbol BMMO_PHYSICS_API_SYMBOL from the loaded physics_RT module and calls
 * through this table.  The retail physics_RT.dll has no such export, which is
 * how the mod detects that physics sessions are unavailable.
 *
 * Plain C so the table is ABI-stable across compilers; append-only per
 * version, bump BMMO_PHYSICS_API_VERSION for incompatible changes.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMMO_PHYSICS_API_VERSION 1u
#define BMMO_PHYSICS_API_SYMBOL "bmmo_physics_api"

typedef struct bmmo_physics_world_hash {
    uint64_t hash;               /* environment clock + every movable core */
    uint64_t pose;               /* movable cores only, no absolute times */
    int32_t cores;
    int32_t ivp_seed;            /* ivp_srand cursor */
    double ivp_time;             /* IVP_Environment current time (s) */
    float delta_time_ms;         /* CKIpionManager smoothed delta */
    float physics_delta_time;
    float time_factor;
    uint64_t surfaces;           /* signature of every body's collision surface */
    char probe_name[32];         /* the simulated game ball, else the first simulated core */
    double probe_position[3];
    float probe_speed[3];
    float probe_rot_speed[3];
} bmmo_physics_world_hash;

typedef struct bmmo_physics_api_v1 {
    uint32_t struct_size;
    uint32_t api_version;
    const char* build_id;        /* engine revision + bridge revision */

    /* ipion_manager is the CKBaseManager with GUID 6BED328B-141F5148.
     * Return 1 on success, 0 on failure with a message in error. */
    int32_t (*capture_world_hash)(void* ipion_manager, bmmo_physics_world_hash* out,
                                  char* error, uint32_t error_size);
    /* Zero the IVP clocks, re-arm the PSI schedule, reset the smoothed
     * delta to 1/66 s and seed the IVP RNG (design 3.1). */
    int32_t (*reset_session_clock)(void* ipion_manager, int32_t seed,
                                   char* error, uint32_t error_size);
    int32_t (*get_random_seed)(void);
    void (*set_random_seed)(int32_t seed);
    /* Diagnostics: "name[movement_state];..." of the movable objects.
     * Returns the full length (like snprintf); truncates to error_size. */
    int32_t (*describe_movable_objects)(void* ipion_manager, char* buffer, uint32_t buffer_size);
    /* Diagnostics: every physicalized entity, "name[movement_state](x,y,z);..." */
    int32_t (*describe_physics_objects)(void* ipion_manager, char* buffer, uint32_t buffer_size);
    /* Diagnostics: IVP object lifecycle events since the previous call. */
    int32_t (*drain_event_log)(void* ipion_manager, char* buffer, uint32_t buffer_size);
    /* Diagnostics: exact (hex float) state of every simulated core, sorted by name. */
    int32_t (*describe_cores_exact)(void* ipion_manager, char* buffer, uint32_t buffer_size);
} bmmo_physics_api_v1;

/* Signature of the exported entry point: returns the table for the requested
 * major version, or NULL when that version is not provided. */
typedef const bmmo_physics_api_v1* (*bmmo_physics_api_fn)(uint32_t requested_version);

#ifdef __cplusplus
}
#endif
