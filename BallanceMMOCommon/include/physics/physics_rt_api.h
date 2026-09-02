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
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BMMO_PHYSICS_API_VERSION 2u
#define BMMO_PHYSICS_API_SYMBOL "bmmo_physics_api"

/* Everything below crosses the C boundary by value, so every array is inline
 * and bounded.  The limits cover the retail recipes with room to spare (the
 * game balls use one convex mesh or one sphere). */
#define BMMO_PHYSICS_NAME_SIZE 64
#define BMMO_PHYSICS_MAX_CONVEX 8
#define BMMO_PHYSICS_MAX_BALLS 4
#define BMMO_PHYSICS_MAX_CONCAVE 8

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
    /* v2: environment details (diagnostics) */
    int16_t next_movement_check;
    double time_of_last_psi;
    double time_of_next_psi;
} bmmo_physics_world_hash;

/* One physicalized entity (v2).  Poses are the OBJECT pose at the
 * environment's current time; right after a tick that time sits on a PSI
 * boundary, so this is the last-PSI pose the renderer also sees. */
typedef struct bmmo_physics_body_state {
    char name[BMMO_PHYSICS_NAME_SIZE]; /* CK3dEntity name, else the IVP object name */
    bool movable;                      /* the core is not physical_unmoveable */
    bool simulated;                    /* IVP_MTIS_SIMULATED(core->movement_state) */
    bool collision_enabled;            /* IVP_Real_Object::is_collision_detection_enabled */
    uint8_t movement_state;            /* IVP_Movement_Type of the core */
    double position[3];                /* world position [m] */
    double rotation[4];                /* quaternion x, y, z, w */
    float linear[3];                   /* core->speed, world space [m/s] */
    float angular[3];                  /* core->rot_speed, core space [rad/s] */
} bmmo_physics_body_state;

/* The full parameter set of the retail Physicalize block, which is also what
 * BMLPlus reports through IMod::OnPhysicalize.  Meshes and the collision
 * surface are named, not pointed at, so a recipe survives the network. */
typedef struct bmmo_physics_ball_recipe {
    bool fixed;                        /* "Fixed ?": the body never moves */
    bool start_frozen;                 /* "Start Frozen" */
    bool enable_collision;             /* "Enable Collision" */
    bool calc_mass_center;             /* "Automatic Calculate Mass Center"; when
                                        * false, mass_center is the explicit shift */
    float friction;
    float elasticity;
    float mass;
    float linear_damp;                 /* "Linear Speed Dampening" */
    float rot_damp;                    /* "Rot Speed Dampening" */
    float mass_center[3];              /* the block's "Shift Mass Center" setting */
    char collision_surface[BMMO_PHYSICS_NAME_SIZE]; /* cache key of the compiled surface */
    int32_t convex_count;
    char convex[BMMO_PHYSICS_MAX_CONVEX][BMMO_PHYSICS_NAME_SIZE];   /* CKMesh names */
    int32_t ball_count;
    float ball_center[BMMO_PHYSICS_MAX_BALLS][3];
    float ball_radius[BMMO_PHYSICS_MAX_BALLS];
    int32_t concave_count;
    char concave[BMMO_PHYSICS_MAX_CONCAVE][BMMO_PHYSICS_NAME_SIZE]; /* CKMesh names */
} bmmo_physics_ball_recipe;

typedef struct bmmo_physics_api_v2 {
    uint32_t struct_size;
    uint32_t api_version;
    const char* build_id;        /* engine revision + bridge revision */

    /* ipion_manager is the CKBaseManager with GUID 6BED328B-141F5148.
     * Return 1 on success, 0 on failure with a message in error. */
    int32_t (*capture_world_hash)(void* ipion_manager, bmmo_physics_world_hash* out,
                                  char* error, uint32_t error_size);
    /* Zero the IVP clocks, re-arm the PSI schedule, reset the smoothed
     * delta to 1/66 s and the physics time factor to the fresh-world 1.0,
     * and seed the IVP RNG (design 3.1). */
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

    /* ---- v2 ---- */

    /* Every physicalized entity, in physics-object table order.  Writes at
     * most `max` entries and returns the total number of bodies (like
     * snprintf), so a short buffer can be resized and the call repeated.
     * `out` may be NULL to query the count only. */
    int32_t (*list_bodies)(void* ipion_manager, bmmo_physics_body_state* out, int32_t max);
    /* One body by CK3dEntity name.  Returns 1 on success, 0 with a message in
     * error when the entity does not exist or is not physicalized. */
    int32_t (*get_body_state)(void* ipion_manager, const char* entity_name,
                              bmmo_physics_body_state* out, char* error, uint32_t error_size);
    /* Beam a body to `position` / `rotation` (quaternion x,y,z,w) and write
     * its core speeds; pending async pushes are dropped.  Any of the four
     * arrays may be NULL to leave that part alone.  wake != 0 calls
     * ensure_in_simulation(), otherwise a simulated body is frozen with
     * disable_simulation().  The CK entity's world matrix is refreshed the
     * same way the manager does after a step, so the render side matches. */
    int32_t (*set_body_state)(void* ipion_manager, const char* entity_name,
                              const double position[3], const double rotation[4],
                              const float linear[3], const float angular[3], int32_t wake,
                              char* error, uint32_t error_size);
    /* Physicalize a named CK3dEntity exactly like the retail Physicalize
     * block would, without going through a behavior graph.  Meshes are looked
     * up by name (CKCID_MESH).  An entity that already has a physics object
     * succeeds without doing anything, again like the block.  collision_group
     * is the IVP nocoll group ident: at most 7 characters, may be NULL. */
    int32_t (*physicalize)(void* ipion_manager, const char* entity_name,
                           const bmmo_physics_ball_recipe* recipe, const char* collision_group,
                           char* error, uint32_t error_size);
    /* The block's Unphysicalize branch: delete_silently() on the body, if any.
     * An entity without a physics object is not an error. */
    int32_t (*unphysicalize)(void* ipion_manager, const char* entity_name,
                             char* error, uint32_t error_size);
    /* Add the BMMO player filter to the environment's IVP_Meta_Collision_Filter
     * (design 8.2): bodies whose nocoll group ident starts with
     * player_group_prefix collide with each other, but not with the level's
     * "Ball" group, so player balls keep the retail semantics against the
     * scenery while still hitting each other.  Idempotent per environment; the
     * filter forgets itself when the environment goes away. */
    int32_t (*install_player_collision_filter)(void* ipion_manager, const char* player_group_prefix,
                                               char* error, uint32_t error_size);
    /* Change a body's IVP nocoll group ident (at most 7 characters, NULL or
     * "" clears it).  The client moves its own retail ball into its player
     * group so the player filter lets it hit the mirrored remote balls. */
    int32_t (*set_body_group)(void* ipion_manager, const char* entity_name, const char* collision_group,
                              char* error, uint32_t error_size);
} bmmo_physics_api_v2;

/* Signature of the exported entry point: returns the table for the requested
 * major version, or NULL when that version is not provided. */
typedef const bmmo_physics_api_v2* (*bmmo_physics_api_fn)(uint32_t requested_version);

#ifdef __cplusplus
}
#endif
