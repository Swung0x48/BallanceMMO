#pragma once

// Physics world hashing, session clock reset and body manipulation against the
// Ballanced physics_RT (IVP).  Compiled into the server's static engine and
// into the client's physics_RT.dll (through physics_rt_bridge.cpp), so both
// sides run exactly this code.

#include <string>

#include <physics/deterministic_random.hpp>
#include <physics/physics_rt_api.h>
#include <physics/world_hash.hpp>

class CKContext;
class CKIpionManager;

namespace bmmo::physics {
    // "ballanced-<engine revision>+bmmo-<repository revision>" of the build
    // this engine came from (cmake/BuildId.cmake, resolved before every
    // build).  The client's plugin reports the same string in session_ready
    // and the server compares the engine half of it against its own.
    const char* build_id();

    // The bridge ABI structs are used directly: no field-by-field copy at the
    // C boundary, and the two sides can never drift apart.
    using body_state = bmmo_physics_body_state;
    using ball_recipe = bmmo_physics_ball_recipe;

    bool capture_world_hash(CKIpionManager* physics, bmmo::physics::world_hash& out,
                            std::string& error);
    bool reset_session_clock(CKIpionManager* physics, int seed, std::string& error);
    // One physics step outside the frame loop (design 9.6 rollback): the
    // manager's Simulate() with the given behaviour delta in milliseconds.
    bool step_physics(CKIpionManager* physics, float delta_ms, std::string& error);
    // Engine change #6: while enabled, the retail Unphysicalize block keeps
    // every body except `except_entity` (the player's ball; null or empty =
    // none), so the client-side sector reset after a death does not delete
    // the shared mechanisms the server keeps.
    bool set_body_guard(CKIpionManager* physics, bool enable, const char* except_entity, std::string& error);
    // The manager's clock: time factor (0 while the retail scripts freeze
    // physics: Level 1 tutorial, pause menu) and the next step's physics
    // delta in seconds.
    bool get_clock(CKIpionManager* physics, float& time_factor, float& physics_delta, std::string& error);
    // "name[state];name[state];..." for every entry of m_MovableObjects, in
    // list order (diagnostics: which bodies the world simulates).
    std::string describe_movable_objects(CKIpionManager* physics);
    // Diagnostics: the integration state of one body's core (times, last-PSI
    // position, per-PSI delta, speed, movement state, simulation unit).
    std::string describe_core(CKIpionManager* physics, const char* entity_name);
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
    // Diagnostics: the per-body terms of the world hash's surface signature
    // (name, surface type, compact-surface size and hash), one line per body,
    // sorted by name so two worlds can be diffed textually.
    std::string describe_surfaces_exact(CKIpionManager* physics);
    // Diagnostics: route IVP's own impact / mindist / PSI debug prints
    // (IVP_Debug_Manager::file_out_impacts) to `path`; null path stops.
    bool set_impact_trace(CKIpionManager* physics, const char* path, std::string& error);

    // ---- bridge API v2 (design 8.4) ----

    // Every physicalized entity in physics-object table order.  Fills at most
    // `max` entries and returns the total count (like snprintf), so `out` may
    // be null or too small.  Poses are the object pose at the environment's
    // current time, which after a tick is the last PSI.
    int list_bodies(CKIpionManager* physics, body_state* out, int max);
    // One body, by CK3dEntity name.
    bool get_body_state(CKIpionManager* physics, const char* entity_name, body_state& out,
                        std::string& error);
    // Beam the body and overwrite its core speeds (pending async pushes are
    // dropped).  Any array may be null to leave that part untouched.  `wake`
    // wakes the body; otherwise a simulated body is frozen.  The CK entity's
    // world matrix is refreshed exactly like CKIpionManager does after a step.
    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], bool wake,
                        std::string& error);
    // The retail Physicalize block's recipe path, driven from a POD recipe
    // instead of a behavior graph.  Already-physicalized entities succeed
    // without doing anything (like the block).  `collision_group` is the IVP
    // nocoll group ident: at most IVP_NO_COLL_GROUP_STRING_LEN - 1 characters.
    bool physicalize(CKIpionManager* physics, const char* entity_name, const ball_recipe& recipe,
                     const char* collision_group, std::string& error);
    // The block's Unphysicalize branch.  Not an error when nothing is there.
    bool unphysicalize(CKIpionManager* physics, const char* entity_name, std::string& error);
    // Add the BMMO player filter to the environment's IVP_Meta_Collision_Filter
    // (design 8.2): player balls (nocoll group ident starting with
    // `player_group_prefix`) collide with each other but not with the level's
    // "Ball" group.  Idempotent per environment.
    bool install_player_collision_filter(CKIpionManager* physics, const char* player_group_prefix,
                                         std::string& error);
    // IVP_Real_Object::change_nocoll_group_ident on a physicalized entity.
    bool set_body_group(CKIpionManager* physics, const char* entity_name, const char* collision_group,
                        std::string& error);

    // ---- bridge API v6 (design 9.10) ----

    // Push the body of `entity_name` at its mass centre with the impulse
    // direction_ws * speed * mass: the retail Physics Impulse block's path for
    // Referential == the entity and Position 0,0,0 (no spin).  Applied at
    // once when the body exists; otherwise, when `behavior_id` names a live
    // behavior, queued into the manager's PreSimulate pass of the current
    // frame - the Physicalize block that creates the body runs later in the
    // same frame - and dropped with a log line if the body never appears.
    bool push_impulse(CKIpionManager* physics, const char* entity_name, const float direction_ws[3], float speed,
                      uint32_t behavior_id, std::string& error);
    // The deterministic generator behind the hooked "Random" block
    // (physics/deterministic_random.hpp).  reset_session_clock seeds it too.
    void random_reset(int32_t seed);
    int32_t random_get_state();
    void random_set_state(int32_t state);
    int32_t random_next();
    // Routes the Virtools "Random" blocks (GUID 0c622386-1c3054f7) inside the
    // trafo explosion scripts Ball_Explosion_Wood/Paper/Stone through the
    // generator: the block's own arithmetic with random_next() in place of
    // rand() and the Microsoft RAND_MAX.  Only those blocks: every other
    // Random in the game keeps the C runtime, so a draw that happens on one
    // side only can never shift the pieces' sequence.  Idempotent; needs the
    // scripts to exist (Balls.nmo loaded); reset_session_clock calls it too.
    // Returns the number of blocks patched by this call, -1 without a context
    // or when Logics is not registered.  The scripts' opening "Set Position"
    // block is wrapped as well, so every explosion starts with
    // restore_explosion_pieces().
    int install_random_block(CKContext* context);
    // Puts the trafo explosion pieces (the Ball_*Pieces_Frame hierarchies)
    // back to their initial conditions, what the retail "TT Restore IC" of
    // Ball_ResetPieces_* does after every explosion.  Needed once before the
    // first explosion of a process: Balls_Init physicalizes the pieces for a
    // few frames at game start, and how far they fall then depends on the
    // start-up frame times (real time on the game, fixed on the headless
    // engine), so without this the first explosion starts from poses that
    // differ per process.  reset_session_clock calls it.  Returns the number
    // of objects restored, -1 without a scene.
    int restore_explosion_pieces(CKContext* context);
}
