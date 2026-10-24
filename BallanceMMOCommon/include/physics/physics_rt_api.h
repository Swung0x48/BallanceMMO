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

#define BMMO_PHYSICS_API_VERSION 20u  /* v20: savestate_set_travelling restores another process's blob (the resync state, resync design A3) (v19: set_body_matrix puts a body where its Physicalize made it (v18: savestate_set names the bodies whose contacts it dropped (v17: the IVP savestate; v16: the collision recheck as a pass of its own; v15: a body's friction contacts, for the rollback restore; v14: the force block's controller and the event-log mute; v13: IVP's freeze bookkeeping) */
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

/* The part of IVP's integrator state that bmmo_physics_body_state does not
 * carry, because the wire and the renderer do not need it: the impulses IVP has
 * queued for the next PSI.
 *
 * A body state is a DESCRIPTION of a body - an interpolated pose and a
 * velocity.  This is the integrator's own working state, and a rollback restore
 * needs it: the impact solver runs in mindist events BETWEEN PSIs and
 * delay_decision parks the push on the core with the larger virt_mass, so when
 * a ball hits a plate it is the PLATE that carries a queued impulse into the
 * next PSI.  set_body_state zeroes that queue by design 8.4, which is right for
 * a hard set and wrong for a replay restore - a measured 1.6-3.6 cm per rewind
 * on a hinged plate, against a floor of zero (docs/lockstep-without-snapshots.md).
 *
 * This never goes on the wire.  It is a LOCAL savestate, which is what GGPO's
 * save_game_state always was, and it is deliberately a separate struct so that
 * the shared bmmo_physics_body_state ABI does not grow. */
/* Which members of a bmmo_physics_core_extra are meaningful.  A caller fills
 * the mask; set_core_extra writes only what is named.  This is what lets the two
 * halves be measured against each other in one binary, which matters because
 * two builds' runs differ by more than either half is worth. */
#define BMMO_PHYSICS_EXTRA_IMPULSES 1u /* speed_change, rot_speed_change */
#define BMMO_PHYSICS_EXTRA_QUATS    2u /* q_last_psi, q_next_psi */
#define BMMO_PHYSICS_EXTRA_CALM     4u /* the calm references (movement_state is read only) */
#define BMMO_PHYSICS_EXTRA_UNIT     8u /* the simulation unit's movement-check countdown */

typedef struct bmmo_physics_core_extra {
    uint32_t fields;           /* BMMO_PHYSICS_EXTRA_* */
    float speed_change[3];     /* core->speed_change, world space [m/s] */
    float rot_speed_change[3]; /* core->rot_speed_change, core space [rad/s] */
    /* IVP integrates a core's rotation between two quaternions, the pose at the
     * last PSI and the predicted pose at the next one.  A body state carries
     * neither: read_body_state captures calc_at_quaternion, ONE quaternion
     * interpolated to the current time, which is the pose the renderer wants.
     * So write_body_state has to re-derive the second, and it does it from the
     * rotation step of the LIVE core - which at a rewind of depth d is d ticks
     * away from the step the anchor had, leaving the restored rotational phase
     * wrong by however much the body's angular velocity moved.  Recording both
     * removes the derivation, and with it the depth-scaling error. */
    double q_last_psi[4];      /* core->q_world_f_core_last_psi, x y z w */
    double q_next_psi[4];      /* core->q_world_f_core_next_psi, x y z w */
    /* v13: IVP's freeze bookkeeping (BMMO_PHYSICS_EXTRA_CALM).  Every
     * movement check (IVP_Core::calc_movement_state) compares the core's pose
     * against two calm references - a short-term and a long-term one - and
     * the time each was taken, to decide whether the body may be frozen.  A
     * restore that puts pose and velocity back but leaves these at their
     * live values makes the replay's sleep decisions from the wrong history:
     * measured on the solo probe (scripts/sleep_divergence.py), 44 bodies
     * fell asleep or stayed awake where the plain run did not, and a hinged
     * chain born in the replayed window settled 0.3-0.6 m away.  The times
     * are absolute IVP seconds; a caller that does not rewind the clock
     * shifts them by the clock offset before writing them back.
     * movement_state is reported for diagnostics and never written: the
     * sleep state is changed through the wake modes, which keep the
     * simulation-unit manager's lists consistent. */
    int32_t movement_state;    /* IVP_Core::movement_state (read only) */
    double calm_time[2];       /* core->time_of_calm_reference[k] [s] */
    double calm_quat[2][4];    /* core->q_world_f_core_calm_reference[k], x y z w */
    double calm_position[2][3];/* core->position_world_f_core_calm_reference[k] */
    /* v13 (BMMO_PHYSICS_EXTRA_UNIT): the movement-check countdown of the
     * simulation unit the core is in (engine change #5 put it per unit).  A
     * unit is several cores, so a caller writing it per body gets
     * last-writer-wins within a unit; the value is in PSIs, clock-free.  Read
     * as 0 and written as a no-op by a bridge built without the engine
     * change. */
    int32_t unit_next_movement_check;
} bmmo_physics_core_extra;

/* A SetPhysicsForce block's controller (v14, game/script_replay.hpp). */
typedef struct bmmo_physics_force_block {
    int32_t exists;       /* the block holds a controller */
    double position[3];   /* PhysicsControllerForce::m_Position (object space) */
    double force[3];      /* PhysicsControllerForce::m_Force */
} bmmo_physics_force_block;

/* One friction contact point of a body (v15): the other object, and the
 * per-contact state IVP keeps on the point (ivp_friction.hxx,
 * IVP_Contact_Point_Fast).  What a beam cannot carry: IVP creates a contact
 * point only on an impact, so a body restored to the resting pose it had at
 * an anchor whose contact has since ended rests on nothing until it leaves
 * (measured: a ball rolling off the road end inside a rewind's window,
 * 0.76 m low afterwards, exact when the contact still existed). */
typedef struct bmmo_physics_contact {
    char partner[BMMO_PHYSICS_NAME_SIZE];   /* the other object's name */
    float gap;          /* last_gap_len */
    float span[2];      /* span_friction_s */
    float pressure;     /* now_friction_pressure */
    int32_t pull;       /* has_negative_pull_since */
    int32_t keeper;     /* slowly_turn_on_keeper */
} bmmo_physics_contact;

/* contacts_set flags: which recorded fields are written onto a re-created
 * point (the rest keep what IVP_Contact_Point's constructor set), and whether
 * a body that still has some contact is left alone. */
#define BMMO_PHYSICS_CONTACT_GAP       1u
#define BMMO_PHYSICS_CONTACT_SPAN      2u
#define BMMO_PHYSICS_CONTACT_PRESSURE  4u
#define BMMO_PHYSICS_CONTACT_PULL      8u
#define BMMO_PHYSICS_CONTACT_KEEPER    16u
#define BMMO_PHYSICS_CONTACT_ALL_FIELDS 31u
#define BMMO_PHYSICS_CONTACT_ONLY_WHEN_NONE 256u   /* re-create only for a body with no contact left */
#define BMMO_PHYSICS_CONTACT_STATIC_ONLY    512u   /* re-create only contacts with an unmovable partner (a floor) */
#define BMMO_PHYSICS_CONTACT_REVIVE         1024u  /* a movable pair with one core asleep: wake it as an impact would,
                                                      instead of leaving the pair to IVP */
#define BMMO_PHYSICS_CONTACT_PRUNE          2048u  /* first delete every live contact whose partner the record does not
                                                      name: a beam keeps the body's contact points, so a restored body
                                                      otherwise rests on the partners it had at the LIVE tick */
/* The default the rollback uses - measured (docs/lockstep-without-snapshots.md,
 * "The one the ball still had"): a fresh point, only for a body with no
 * contact left, only against static geometry.  Copying the recorded fields
 * onto the point cost 5-11 cm on a ball crossing floor blocks, and a point
 * made between two movable bodies crashed the next impact solve. */
#define BMMO_PHYSICS_CONTACT_DEFAULT (BMMO_PHYSICS_CONTACT_ONLY_WHEN_NONE | BMMO_PHYSICS_CONTACT_STATIC_ONLY)

/* The environment's clock, which a rollback restore has to put back too.
 *
 * The bodies are restored, but IVP's time is not, so every replayed tick runs
 * at an absolute time one tick later than the tick it replaces - and absolute
 * time is read by the freeze logic (psi_time - time_of_calm_reference), by
 * every event the time manager schedules, and by the beam itself, which stamps
 * a core with the environment's time_of_last_psi.  Measured with an otherwise
 * exact restore, time_of_last_psi moved on 85 of 85 cores across a depth-1
 * rewind, by exactly one tick, and was the only field to move on bodies that
 * did not then diverge.
 *
 * Queued events are stored relative to base_time, so a caller that moves the
 * base shifts every queued value by the same amount to keep each event at the
 * absolute time it was scheduled for, and repositions the PSI event - the one
 * queued event whose time the record knows - at time_of_next_psi.
 *
 * C++ entry points only until the effect is measured; no v-table entry yet. */
typedef struct bmmo_physics_clock_state {
    double current_time;         /* IVP_Environment::current_time [s] */
    double time_of_last_psi;     /* IVP_Environment::time_of_last_psi */
    double time_of_next_psi;     /* IVP_Environment::time_of_next_psi */
    double base_time;            /* IVP_Time_Manager::base_time */
    double last_time;            /* IVP_Time_Manager::last_time, relative to base_time */
    int32_t next_movement_check; /* IVP_Environment::next_movement_check */
    int32_t queued_events;       /* diagnostic: how many events the manager holds */
    float queue_min;             /* diagnostic: the earliest queued time, relative */
} bmmo_physics_clock_state;

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

/* Internal state of one navigation replica (design 9.6): what a rollback
 * has to restore.  Bit i of the masks = leaf i; force[i] is the world-space
 * force vector the leaf's controller was created with. */
typedef struct bmmo_physics_nav_state {
    uint8_t active;                    /* BallNav active */
    uint8_t key_mask;                  /* leaves whose key was down */
    uint8_t controller_mask;           /* leaves with a live force controller */
    uint8_t create_pending_mask;       /* leaves whose Create waits for the body */
    float force[8][3];
} bmmo_physics_nav_state;

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

    /* ---- v3 (design 9.1): the retail Ball Navigation replica for a ball
     * driven from network input (a mirrored remote ball).  The same code
     * drives the server's clones, so a remote ball predicted this way
     * integrates exactly like the authoritative one while its input holds. */

    /* Register navigation for `ball_entity`.  `direction_ref_entity` is the
     * per-ball camera reference frame (created when missing); `behavior_id`
     * is the CK_ID of the level's "Ball Navigation" script, which anchors
     * the PreSimulate callback; `directions[i]` is leaf i's force direction
     * (leaf order = key order of the graph); `force_value` the ball type's
     * Physicalize_GameBall force. */
    int32_t (*navigation_create)(void* ipion_manager, const char* ball_entity, const char* direction_ref_entity,
                                 uint32_t behavior_id, const float (*directions)[3], int32_t leaf_count,
                                 float force_value, char* error, uint32_t error_size);
    /* Input of the next tick: leaf key bits, camera basis rows, BallNav
     * active flag.  Applied in the manager's next PreSimulate pass. */
    int32_t (*navigation_input)(void* ipion_manager, const char* ball_entity, uint8_t keys, const float right[3],
                                const float up[3], const float dir[3], int32_t active,
                                char* error, uint32_t error_size);
    /* The ball changed entity (transformation). */
    int32_t (*navigation_set_ball)(void* ipion_manager, const char* ball_entity, const char* new_ball_entity,
                                   float force_value, char* error, uint32_t error_size);
    int32_t (*navigation_destroy)(void* ipion_manager, const char* ball_entity, char* error, uint32_t error_size);

    /* ---- v4 (design 9.6): client-side rollback ---- */

    /* One physics step of delta_ms outside the frame loop, exactly what the
     * manager does in PostProcess: PreSimulate callbacks (navigation),
     * simulate_dtime, contacts, PostSimulate, entity matrices. */
    int32_t (*step_physics)(void* ipion_manager, float delta_ms, char* error, uint32_t error_size);
    /* Polling mode for the own ball: at PreSimulate the replica reads the
     * keyboard state itself (key_codes[i] = CKKEY of leaf i) and takes
     * BallNav active from the Key Event blocks (key_blocks[i] = CK_ID); the
     * camera rows still come from navigation_input, whose keys/active are
     * then ignored.  enable = 0 goes back to explicit input. */
    int32_t (*navigation_poll)(void* ipion_manager, const char* ball_entity, int32_t enable,
                               const int32_t* key_codes, const uint32_t* key_blocks, int32_t count,
                               char* error, uint32_t error_size);
    int32_t (*navigation_get_state)(void* ipion_manager, const char* ball_entity, bmmo_physics_nav_state* out,
                                    char* error, uint32_t error_size);
    /* Restores the leaf states and recreates the controllers with the stored
     * force vectors (the body must exist). */
    int32_t (*navigation_set_state)(void* ipion_manager, const char* ball_entity, const bmmo_physics_nav_state* state,
                                    char* error, uint32_t error_size);

    /* ---- v5 (engine change #6): body guard ---- */

    /* While enabled, the retail Unphysicalize block keeps every body except
     * except_entity (the player's ball, may be NULL/empty): the client-side
     * sector reset after a death must not delete the shared mechanisms the
     * server keeps.  Disable at the end of the session.  The trafo explosion
     * pieces are exempt as well (engine change #13): restore_explosion_pieces
     * names them, so the scripts that create and drop them again within a
     * session keep working. */
    int32_t (*set_body_guard)(void* ipion_manager, int32_t enable, const char* except_entity,
                              char* error, uint32_t error_size);
    /* The manager's clock: time factor (seconds per behaviour millisecond,
     * 0 while the scripts freeze physics) and the physics delta of the next
     * step in seconds. */
    int32_t (*get_clock)(void* ipion_manager, float* time_factor, float* physics_delta, char* error,
                         uint32_t error_size);

    /* ---- v6 (design 9.10): spawn impulse, deterministic "Random" block ---- */

    /* Push the body of entity_name at its mass centre with the impulse
     * direction_ws * speed * mass: the retail Physics Impulse block's path
     * for Referential == the entity and Position 0,0,0, so no spin.  Applied
     * at once when the body exists; otherwise, when behavior_id names a live
     * behavior, queued into the manager's PreSimulate pass of the current
     * frame (the Physicalize block that creates the body runs later in the
     * same frame) and dropped with a log line if the body never appears. */
    int32_t (*push_impulse)(void* ipion_manager, const char* entity_name, const float direction_ws[3],
                            float speed, uint32_t behavior_id, char* error, uint32_t error_size);
    /* The deterministic generator behind the hooked "Random" block (the
     * Microsoft runtime's LCG, RAND_MAX 32767; physics/deterministic_random.hpp).
     * reset_session_clock seeds it as well; the server saves and restores the
     * state per world. */
    void (*random_reset)(int32_t seed);
    int32_t (*random_get_state)(void);
    void (*random_set_state)(int32_t state);
    int32_t (*random_next)(void);
    /* Route the Virtools "Random" blocks (GUID 0c622386-1c3054f7) inside the
     * trafo explosion scripts Ball_Explosion_Wood/Paper/Stone through the
     * generator (the block's own arithmetic, so the pieces keep the retail
     * look).  Idempotent; call it after the scripts exist (Balls.nmo is
     * loaded).  reset_session_clock calls it as well.  Returns the number of
     * blocks patched by this call, -1 without a context or when Logics is not
     * registered. */
    int32_t (*install_random_block)(void* ck_context);

    /* ---- v8: session clock guard ---- */

    /* Keep the session's clock out of the retail pause menu's hands.  The
     * guard samples the manager's physics time factor on every PreSimulate
     * pass, so the level scripts stay in charge of it exactly as they are on
     * the server, but while pause_behavior_id names a behavior that is
     * deactivated (the Event_handler's "Pause Level" chain stops
     * Gameplay_Ingame) the factor is pinned to the value the run had before
     * the menu opened - and for the one pass in which it closes, which is the
     * pass the unpause chain uses to write 2.0.  Idempotent while enabled;
     * needs a live behavior (behavior_id) to attach the callback to, and
     * re-attaches after the engine destroyed it.  time_factor is only the
     * starting pin, used if the guard is installed while the menu is open. */
    int32_t (*set_clock_guard)(void* ipion_manager, int32_t enable, float time_factor, uint32_t behavior_id,
                               uint32_t pause_behavior_id, char* error, uint32_t error_size);

    /* ---- v9: new contacts when a body is beamed ---- */

    /* Exactly set_body_state, except that the beam always rechecks the
     * object's collision pairs: IVP_Real_Object::beam_object_to_new_position
     * is called with optimize_for_repeated_calls = IVP_FALSE instead of
     * IVP_TRUE.  The optimized path is documented to generate no collision
     * when the body is beamed into another one ("if the object penetrates
     * other objects at the new position, no collisions will be generated !!!",
     * ivp_real_object.hxx:294-303) because it skips recheck_ov_element
     * (ivp_calc_next_psi_solver.cxx:318).  The client's snap teleports of
     * server-authoritative mechanisms are the one writer that must create the
     * contact pair; the server keeps calling set_body_state, so its
     * simulation is byte for byte unchanged. */
    int32_t (*set_body_state_recheck)(void* ipion_manager, const char* entity_name,
                                      const double position[3], const double rotation[4],
                                      const float linear[3], const float angular[3], int32_t wake,
                                      char* error, uint32_t error_size);

    /* ---- v10: sleep-state-preserving, contact-rechecking body writes ---- */

    /* The write a rollback restore wants.  wake_mode: 0 = keep (never touch
     * the body's sleep state), 1 = wake (ensure_in_simulation only when the
     * body is NOT simulated, so an awake body's freeze timers are left
     * alone - the old entries call it unconditionally and re-arm them),
     * 2 = freeze (disable_simulation when the body is simulated).
     * recheck != 0 beams with optimize_for_repeated_calls = IVP_FALSE, i.e.
     * exactly like set_body_state_recheck.  A beam through any of these
     * entries also restores the core's leapfrog delta
     * (delta_world_f_core_psis) from `linear`: IVP integrates the position
     * of a PSI from the delta written at the PREVIOUS one, and
     * IVP_Calc_Next_PSI_Solver::set_transformation zeroes it, so without the
     * restore every beam costs the body one PSI of translation (|v|/66 m).
     * See physics_state.cpp. */
    int32_t (*set_body_state_ex)(void* ipion_manager, const char* entity_name,
                                 const double position[3], const double rotation[4],
                                 const float linear[3], const float angular[3],
                                 int32_t wake_mode, int32_t recheck, char* error, uint32_t error_size);

    /* ---- v11: the core's integrator state, for a rollback restore ---- */

    /* Read / write the integrator state a body state does not carry: the
     * queued impulses and the two rotation quaternions.  See
     * bmmo_physics_core_extra for why a rollback needs them.
     *
     * set_core_extra is meant to run AFTER a set_body_state_ex that restored
     * the pose: that call zeroes the queue (design 8.4) and neither
     * ensure_in_simulation nor disable_simulation touches it afterwards, so
     * writing the recorded queue last puts the body back exactly as it was.
     * Writing a queue onto a body that is being FROZEN is incoherent and no
     * caller should do it - a frozen body's queue is never restored, because a
     * restore that freezes is a hard set from an authoritative row, which has
     * no queue to restore in the first place. */
    int32_t (*get_core_extra)(void* ipion_manager, const char* entity_name,
                              bmmo_physics_core_extra* out, char* error, uint32_t error_size);
    int32_t (*set_core_extra)(void* ipion_manager, const char* entity_name,
                              const bmmo_physics_core_extra* in, char* error, uint32_t error_size);

    /* ---- v13: the environment's clock, for a rollback restore ---- */

    /* Read / write bmmo_physics_clock_state.  A restore that keeps the calm
     * references coherent needs at least to READ it (to shift the reference
     * times by the clock offset); writing it back is the opt-in rewind the
     * struct's comment describes. */
    int32_t (*get_clock_state)(void* ipion_manager, bmmo_physics_clock_state* out, char* error, uint32_t error_size);
    int32_t (*set_clock_state)(void* ipion_manager, const bmmo_physics_clock_state* in, char* error, uint32_t error_size);

    /* ---- v14: the script replay (game/script_replay.hpp) ---- */

    /* The IVP controller a SetPhysicsForce block (behavior_id) holds: whether
     * it has one, and the point and force it was built with, in the body's
     * object space.  The restore deletes or re-creates the controller so the
     * block's state matches `in` (and writes the block's pointer parameter),
     * which is how a replayed script starts from the anchor's forces. */
    int32_t (*force_block_get)(void* ipion_manager, uint32_t behavior_id, bmmo_physics_force_block* out,
                               char* error, uint32_t error_size);
    int32_t (*force_block_set)(void* ipion_manager, uint32_t behavior_id, const bmmo_physics_force_block* in,
                               char* error, uint32_t error_size);
    /* While muted, the bridge event log records nothing: a replayed script's
     * Physics WakeUp would otherwise show up as a `revived` the frame's drain
     * turns into a BodyRevived event for the server. */
    int32_t (*set_event_log_muted)(void* ipion_manager, int32_t muted, char* error, uint32_t error_size);

    /* ---- v15: a body's friction contacts (bmmo_physics_contact) ---- */

    /* contacts_get fills at most `max` entries and returns the body's contact
     * count (like snprintf; `out` may be null), or -1 with a message.
     * contacts_set re-creates, through IVP's own creation path, every recorded
     * contact the body lacks now, with the recorded per-contact state, and
     * returns how many it made (-1 with a message when the body is unknown;
     * a partner with no mindist in range is reported in `error` and skipped). */
    int32_t (*contacts_get)(void* ipion_manager, const char* entity, bmmo_physics_contact* out, int32_t max,
                            char* error, uint32_t error_size);
    int32_t (*contacts_set)(void* ipion_manager, const char* entity, const bmmo_physics_contact* in, int32_t count,
                            uint32_t flags, char* error, uint32_t error_size);

    /* ---- v16: the collision recheck as a pass of its own ---- */

    /* body_recheck rebuilds the body's collision pairs against the world as
     * it stands now (IVP_Real_Object::recheck_collision_filter, what a
     * non-optimized beam does right after its pose write).  A restore writes
     * every pose without the recheck and then rechecks every moved body, so
     * no body's pairs are built against a partner still at its live pose.
     * Returns 1, or -1 with a message. */
    int32_t (*body_recheck)(void* ipion_manager, const char* entity, char* error, uint32_t error_size);

    /* ---- v17: the IVP savestate (docs/ivp-savestate-design.md) ---- */

    /* The engine's whole dynamic state at a tick boundary as one byte blob:
     * every core, every object's hull manager and overlap element, every
     * collision checker (mindists, recursive mindists, watchers) with its
     * scheduled event, every friction system with its contact points and
     * pairs, the simulation units, the revive list, the clock, the object
     * cache ring and the physics_RT side (the movable-object list, the CK
     * world matrices, the manager's three clock floats).  Where the other
     * entries restore a body and leave IVP's collision bookkeeping as the
     * LIVE world had it, savestate_set tears that bookkeeping down and
     * rebuilds it from the blob, so a replay from the restored tick is meant
     * to be bit for bit the straight run.  The blob is LOCAL: its layout is
     * private to one build, little-endian and versioned, it is only ever read
     * back by the binary that wrote it, and it never goes on the wire.
     *
     * savestate_get writes at most `max` bytes and returns the blob's full
     * size (like snprintf; `out` may be NULL to query it), or -1 with a
     * message in error.  savestate_set returns 1 on success, 0 on failure with
     * a message in error; on a failure past the teardown the world is put
     * back into a runnable (not exact) state and the caller falls back to its
     * per-body restore.  `flags` are the BMMO_PHYSICS_SAVESTATE_* bits below.
     * v18: `dropped` (may be NULL) receives the ';'-separated names of the
     * bodies whose friction contacts the blob could not put back (their
     * system had a body the world no longer has), truncated to
     * `dropped_size`; the caller re-creates those from its own record.
     * `unknown` (may be NULL) receives, the same way, the live bodies the
     * blob does not know: they keep their place in the movable list but are
     * left out of collision detection, and the caller decides (a body it
     * re-poses at its birth tick stays inert until then; one it has no record
     * of it rechecks back in at once). */
    int32_t (*savestate_get)(void* ipion_manager, uint8_t* out, uint32_t max, char* error, uint32_t error_size);
    int32_t (*savestate_set)(void* ipion_manager, const uint8_t* data, uint32_t size, uint32_t flags, char* error,
                             uint32_t error_size, char* dropped, uint32_t dropped_size, char* unknown, uint32_t unknown_size);

    /* ---- v19: a body born inside a rollback's window, put where its
     * Physicalize made it (docs/rollback-core-unification-plan.md, 9) ----
     * The entity's world matrix is set to `matrix` (row-major, translation in
     * the last row) and the body beamed to the position and quaternion the
     * creation derives from it, at rest, awake (simulated != 0) or frozen,
     * with the collision recheck.  Returns 1, or -1 with a message. */
    int32_t (*set_body_matrix)(void* ipion_manager, const char* entity, const float matrix[16], int32_t simulated,
                               char* error, uint32_t error_size);

    /* ---- v20: a TRAVELLING savestate (docs/resync-and-sector-timing-design.md
     * 2.3, step A3) ----
     * The server's blob of one tick, restored once into this world: as
     * savestate_set, but the capturing environment's identity is not checked,
     * `renames` ("old=new;old=new", may be NULL) maps a blob object's name to
     * the live one's (the resynced member's own clone to its own ball), a
     * name several objects share is matched by occurrence, objects whose
     * (renamed) name starts with one of `excluded_prefixes` (";"-joined, may
     * be NULL) are left out on both sides (the keep-local bodies), and the
     * PreSimulate queue is not compared (bmmo::physics::savestate_travel).
     * `summary` (may be NULL) receives "skipped=N excluded=N renamed=N" and
     * the restore's first warnings, truncated to `summary_size`. */
    int32_t (*savestate_set_travelling)(void* ipion_manager, const uint8_t* data, uint32_t size, uint32_t flags,
                                        const char* renames, const char* excluded_prefixes, char* error,
                                        uint32_t error_size, char* dropped, uint32_t dropped_size, char* unknown,
                                        uint32_t unknown_size, char* summary, uint32_t summary_size);
} bmmo_physics_api_v2;

/* savestate_set flags: the same values as SAVESTATE_STRICT and
 * SAVESTATE_KEEP_UNKNOWN in physics/physics_savestate.hpp. */
#define BMMO_PHYSICS_SAVESTATE_STRICT       1u  /* the first blob/world inconsistency fails the restore instead of being skipped */
#define BMMO_PHYSICS_SAVESTATE_KEEP_UNKNOWN 2u  /* a body alive now but absent from the blob keeps its live collision state */

/* Signature of the exported entry point: returns the table for the requested
 * major version, or NULL when that version is not provided. */
typedef const bmmo_physics_api_v2* (*bmmo_physics_api_fn)(uint32_t requested_version);

#ifdef __cplusplus
}
#endif
