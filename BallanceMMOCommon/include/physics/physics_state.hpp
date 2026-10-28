#pragma once

// Physics world hashing, session clock reset and body manipulation against the
// Ballanced physics_RT (IVP).  Compiled into the server's static engine and
// into the client's physics_RT.dll (through physics_rt_bridge.cpp), so both
// sides run exactly this code.

#include <string>
#include <vector>

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
    using trafo_set_report = bmmo_physics_trafo_set_report;

    bool capture_world_hash(CKIpionManager* physics, bmmo::physics::world_hash& out,
                            std::string& error);
    bool reset_session_clock(CKIpionManager* physics, int seed, std::string& error);
    // One physics step outside the frame loop (design 9.6 rollback): the
    // manager's Simulate() with the given behaviour delta in milliseconds.
    // ---- bridge API v11: the core's queued impulses ----
    //
    // What set_body_state throws away and a rollback restore needs back.  See
    // bmmo_physics_core_extra in physics_rt_api.h for the mechanism and the
    // measurement; set_core_extra is meant to run immediately after the pose
    // write, which is where the zeroing has already happened and nothing later
    // in that call touches the queue again.
    bool get_core_extra(CKIpionManager* physics, const char* entity_name,
                        bmmo_physics_core_extra& out, std::string& error);
    bool set_core_extra(CKIpionManager* physics, const char* entity_name,
                        const bmmo_physics_core_extra& in, std::string& error);
    // The environment's clock and time-manager state, for a rollback restore
    // to put back BEFORE it writes the bodies (the beam stamps each core with
    // the environment's time_of_last_psi).  See bmmo_physics_clock_state.
    bool get_clock_state(CKIpionManager* physics, bmmo_physics_clock_state& out, std::string& error);
    bool set_clock_state(CKIpionManager* physics, const bmmo_physics_clock_state& in, std::string& error);
    // v14, the script replay (game/script_replay.hpp): a SetPhysicsForce
    // block's controller, and the event log's mute.
    bool force_block_get(CKIpionManager* physics, uint32_t behavior_id, bmmo_physics_force_block& out, std::string& error);
    bool force_block_set(CKIpionManager* physics, uint32_t behavior_id, const bmmo_physics_force_block& in, std::string& error);
    void set_event_log_muted(bool muted);
    // v23, the PreSimulate queue (rollback-engine-gaps design 2.2.2, gap Q;
    // engine change #20).  physics_savestate_common.cpp; see the table entries
    // of the same names in physics_rt_api.h.  rebuild returns the entries
    // removed (-1: refused, nothing changed), hold the entries held (-1:
    // already holding), unhold the entries put back by the clamp.
    bool callbacks_get(CKIpionManager* physics, std::vector<bmmo_physics_callback>& out, std::string& error);
    int callbacks_rebuild(CKIpionManager* physics, const std::vector<uint32_t>& scope,
                          const std::vector<bmmo_physics_callback>& target, std::string& error);
    int callbacks_hold(CKIpionManager* physics, const std::vector<uint32_t>& executed, std::string& error);
    int callbacks_unhold(CKIpionManager* physics, std::string& error);
    bool callback_scope_set(CKIpionManager* physics, int mode, const std::vector<uint32_t>& scope, std::string& error);
    // While muted, keep the "recipe" entries of the bodies created (the
    // format drain_event_log has) in a buffer of their own, drained by
    // drain_muted_recipes: a client's record of the births a replayed frame
    // made.  Off by default; everything else stays muted.
    void set_event_log_keep_muted_recipes(bool keep);
    std::string drain_muted_recipes();

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
    // Diagnostics: the IVP controllers attached to one body's core - what
    // pushes it besides contacts: "n=K name/priority[ f=(..)] ...".  The force
    // of the bridge's own navigation controllers is named; a retail
    // SetPhysicsForce controller is counted here and read through the block
    // that owns it (game/script_replay.hpp, describe_force_blocks).
    std::string describe_controllers(CKIpionManager* physics, const char* entity_name);
    // Diagnostics: one body's IVP contact state - its friction contact points
    // (partner, the distance keeper's turn-on counter, the gap, the pull
    // counter, the friction spans, the pressure) and its exact mindists
    // (partner, status, length).  What a beam cannot carry: a rewind to an
    // anchor whose contact has since ended re-creates the contact fresh.
    std::string describe_contacts(CKIpionManager* physics, const char* entity_name);
    // The PreSimulate callback queue: every queued callback's class, the
    // behavior it belongs to (name, root script), that behavior's target and
    // whether the target has a body now - the pending ones are blocks whose
    // target had no body when they fired (PhysicsForce etc. return 0 and are
    // re-queued until it does).
    std::string describe_presimulate_queue(CKIpionManager* physics);
    // Diagnostics: the collision state of one body that no record carries -
    // its core's movement state, velocities and queued changes, its contacts
    // (describe_contacts), the friction pairs it is in (easing countdown,
    // integrated anti-energy, time since the pair's last impact) and its exact
    // mindists with IVP's collision-distance bookkeeping (selector, decrease
    // counter, extra radius, angular hull time) and the time until the
    // mindist's scheduled event.  For diffing a rewound run against a control
    // tick by tick.
    std::string describe_body(CKIpionManager* physics, const char* entity_name);

    // Step OWNB's probe: with BMMO_HULL_PROBE=<name prefix> and
    // BMMO_HULL_PROBE_FILE set, appends the body's collision bookkeeping
    // (hull manager, hull entries, overlap element) under `label`.
    void hull_probe(CKIpionManager* physics, const char* entity_name, const char* label);

    // ---- bridge API v16: the collision recheck as a pass of its own ----

    // Rebuild the body's collision pairs against the world as it stands now
    // (IVP_Real_Object::recheck_collision_filter - the recheck a
    // non-optimized beam runs right after its pose write).  A restore writes
    // every pose without the recheck, then rechecks every moved body, so no
    // body's pairs are built against a partner still standing at its live
    // pose (measured: a restored prop ball with a contact at 0.0100 m to a
    // plate the record had 0.0456 m away).
    bool recheck_body(CKIpionManager* physics, const char* entity_name, std::string& error);

    // ---- bridge API v15: a body's friction contacts ----

    // The body's contact points, at most `max` of them into `out` (may be
    // null); returns the count, or -1 with `error`.
    int get_contacts(CKIpionManager* physics, const char* entity_name, bmmo_physics_contact* out, int max,
                     std::string& error);
    // Re-creates every recorded contact the body lacks now, through IVP's own
    // creation path (the exact mindist to the partner, then
    // IVP_Mindist::try_to_generate_managed_friction, then the recorded
    // per-contact state written onto the new point).  Returns how many it
    // made, -1 when the body is unknown; a partner without a mindist in range
    // is named in `error` and skipped.
    int set_contacts(CKIpionManager* physics, const char* entity_name, const bmmo_physics_contact* in, int count,
                     uint32_t flags, std::string& error);
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
    // A lookup scope (step TR3, the rollback record's capture): between
    // begin and the matching end on this thread, every by-name entity lookup
    // of the functions here (get_body_state, get_core_extra, get_contacts,
    // ...) on `physics` is served from one pass over the context's 3D
    // entities, made at the scope's first lookup - the first entity of each
    // name in the order GetObjectByNameAndParentClass scans, so a lookup
    // finds what the scan would.  Only for a stretch that creates, destroys
    // and renames no object (a capture reads only); scopes nest.  Without a
    // scope every lookup scans the context as before.
    void entity_lookup_scope_begin(CKIpionManager* physics);
    void entity_lookup_scope_end(CKIpionManager* physics);
    // Beam the body and overwrite its core speeds (pending async pushes are
    // dropped).  Any array may be null to leave that part untouched.  `wake`
    // wakes the body; otherwise a simulated body is frozen.  The CK entity's
    // world matrix is refreshed exactly like CKIpionManager does after a step.
    // `recheck` beams through the unoptimized IVP path
    // (IVP_Real_Object::beam_object_to_new_position with
    // optimize_for_repeated_calls = IVP_FALSE).  The default optimized path is
    // the one that drops contacts: "if the object penetrates other objects at
    // the new position, no collisions will be generated !!!"
    // (ivp_real_object.hxx:294-303), because it skips recheck_ov_element
    // (ivp_calc_next_psi_solver.cxx:318).  Every per-frame pose write wants
    // that optimization; a snap teleport of a server-authoritative mechanism
    // onto a sleeping ball needs the fresh contact pair instead, so only that
    // caller passes true.  Left false the call is bit-identical to what it has
    // always done.
    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], bool wake,
                        std::string& error, bool recheck = false);
    // What a restore wants of the body's sleep state (bridge API v10; the
    // values are the wake_mode argument of set_body_state_ex).
    enum class wake_mode : int {
        keep = 0,    // never touch it: the caller is only correcting a pose
        wake = 1,    // ensure_in_simulation(), but only when the body is not
                     // simulated - on an awake body the call re-arms IVP's
                     // freeze counters, which the bool overload does and a
                     // per-tick correction must not
        freeze = 2,  // disable_simulation() when the body is simulated
    };
    // The same write with the sleep state under the caller's control, and the
    // one a rollback restore wants: a beam through this overload also carries
    // the core's leapfrog state across (the per-PSI translation delta
    // delta_world_f_core_psis, rewritten from `linear`, and the predicted
    // rotation of the next PSI), which IVP's set_transformation throws away -
    // without it every restore costs the body one PSI of motion, |v|/66 m.
    // The bool overload is this one with `wake ? <unconditional wake> :
    // freeze` and no leapfrog restore, byte-identical to what it has always
    // done.
    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], wake_mode mode,
                        bool recheck, std::string& error);
    // A body put where its Physicalize made it, the way the Physicalize did:
    // the entity's world matrix is set to `matrix` (row-major, the last row
    // the translation) and the body is beamed to the position and quaternion
    // physics_RT derives from that matrix at creation (FillTemplateInfo, the
    // one algorithm of engine change #8), at rest, awake or frozen as the
    // Physicalize left it, with the collision recheck - into the overlap tree
    // at this moment.  A rollback uses it at the start of a body's birth tick
    // for a body born inside its window, so the body enters IVP's structures
    // where and when the live creation did (plan doc 9).
    bool set_body_matrix(CKIpionManager* physics, const char* entity_name, const float matrix[16], bool simulated,
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
    // differ per process.  reset_session_clock calls it.  Also names the
    // pieces to the body guard as entities it must not cover (engine change
    // #13): they are the one set of bodies the retail scripts create and
    // destroy again within a session.  Returns the number of objects
    // restored, -1 without a scene.
    int restore_explosion_pieces(CKContext* context);

    // ---- bridge API v21: the trafo sets (docs/trafo-replication-design.md
    // 2.4, 2.6, 2.10; game/trafo_sets.hpp) ----

    // Registers player's piece set: `frames` are its three piece frames by
    // type (wood, stone, paper), `explosions` its explosion scripts.  The
    // scripts' Random blocks are routed through the generator and their
    // opening Set Position blocks wrapped (idempotent; a copy may carry the
    // patched functions already); the frames and every piece below them go
    // on the body guard's exemption list (engine change #13); the pieces'
    // names into the collision filter's registry.  `out` counts what the
    // set has routed, exempt and registered afterwards (invariant I5).
    // Registering a player that has a registered set fails (unregister
    // first).  A set whose frame ids no longer name the registered frames (a
    // world torn down without unregistering) is dropped before any decision
    // reads the registry.
    bool trafo_set_register(CKIpionManager* physics, uint32_t player, const uint32_t frames[3], const uint32_t* explosions,
                            int explosion_count, trafo_set_report& out, std::string& error);
    // The set's entries removed from the registry, the seed registry and the
    // exemption list.
    bool trafo_set_unregister(CKIpionManager* physics, uint32_t player, std::string& error);
    // The seed the next explosion of `frame_id` (a piece frame) draws from:
    // its wrapped Set Position restarts the generator from it, after the
    // per-set restore.  0 removes the entry.  An original frame may have one
    // (tests only); without an entry the generator's cursor is left alone.
    void trafo_set_seed(CKIpionManager* physics, uint32_t frame_id, int32_t seed);
    // The per-set restore for an unmake (the reconcile, the teardown): every
    // piece of the registered frame loses its body, then the frame is put
    // back detached, squared and at its initial conditions, its pieces' hulls
    // forgotten.  Returns the bodies removed, -1 with `error`.
    int trafo_set_unmake(CKIpionManager* physics, uint32_t frame_id, std::string& error);
    // The piece x local-body rule (design 2.10) on or off: while on, a
    // registered piece never collides with a body the resync's keep-local
    // predicate names (session/resync_rules.hpp keep_local_body).  The
    // filter is installed into the environment's meta filter the first time
    // it is switched on there; off, it lets every pair through.
    bool trafo_set_filter(CKIpionManager* physics, bool enable, std::string& error);
    // Diagnostics: the registry as one line.
    std::string describe_trafo_sets(CKIpionManager* physics);

    // ---- bridge API v24: the airship zone (step BALA; the user's rule of
    // 2026-10-28, docs/rewind-cost-design.md "As built (step BALA)") ----
    //
    // The level end's airship (PE_Balloon*, PE_Box_slide: the resync's
    // keep-local bodies) is every world's own copy, outside the sync.  The
    // zone is a static box: the walkable airship parts (PE_Balloon_Platform*,
    // PE_Balloon_Platte*), each part's mesh box through its world matrix,
    // taken at the first PSI after the platform has its body - the airship's
    // starting pose, where the level-end script placed it - widened to the
    // 1/8 m grid outward and then by the margins (physics_state.cpp).  Until
    // then no ball is in the zone.  With the zone on:
    //   * a player ball (nocoll group "P#...") that is not this world's own
    //     ball (`own_group`, "" on the server) never collides with an
    //     airship body;
    //   * such a ball whose core is inside the box collides with nothing,
    //     and at the start of every PSI it is taken out of the simulation
    //     (its pairs rechecked away, then frozen): a ghost, still there,
    //     which no synced body depends on.  Positional, so a rollback's
    //     restore and replay re-derive it with no state of their own;
    //   * the own ball, from the first PSI its core is inside the box until
    //     its body is gone (one way, like the ghost's freeze: the life's
    //     latch), collides with the static level and its own airship only
    //     (no other ball, no movable mechanism); at that first PSI its pairs
    //     are rechecked, so a contact it had is dropped where the other
    //     worlds freeze its copy.  The latch is kept as IVP clock intervals
    //     ([entered, gone)), which a rollback's restore puts back with the
    //     clock: a replay re-enters at the same PSI and rechecks again there.
    // `out` says whether it is on (the level has an airship) and the box
    // when it is already taken.  enable = false switches it off.
    struct airship_zone_box {
        bool active = false;   // switched on, the level has an airship
        bool ready = false;    // the box is taken (the platform has its body)
        double min[3] = {}, max[3] = {};
        uint64_t frozen = 0;   // ghosts the PSI hook took out of the simulation so far
    };
    bool airship_zone_set(CKIpionManager* physics, bool enable, const char* own_group, airship_zone_box& out,
                          std::string& error);
    // The zone as it stands (`ready` once the box is taken).
    bool airship_zone_get(CKIpionManager* physics, airship_zone_box& out);
    // 1 when `entity`'s body is a player ball inside the zone (a ghost when
    // it is not the own ball; the own ball: latched this life, inside the
    // box or not), 0 when it is not (or the zone is off), -1 when it has no
    // body.
    int airship_zone_contains(CKIpionManager* physics, const char* entity_name);

    // ---- bridge API v25: the spawn ghost (steps SPGH, SPGH2; the user's
    // rule of 2026-10-28, docs/rewind-cost-design.md "As built (step SPGH)"
    // and "As built (step SPGH2)") ----
    //
    // Spawn areas: a sphere of `radius` around the level start
    // (CurrentLevel[0][3]) and around every row of the level's ResetPoints
    // table, read when the rule is switched on (again at a PSI while none
    // was found).  With the rule on, a player ball (nocoll group "P#...") is
    // a GHOST in its spawn life: it never collides with another player ball
    // (every other pair is unchanged: it rolls, falls and steers as usual).
    // A spawn life begins at a group's first body and at every spawn birth
    // (spawn_ghost_birth: a Physicalize the lifecycle flags SPAWN - a
    // respawn at a resetpoint), and only there: a trafo, a beam or a body a
    // rollback makes again keeps the group's state.  At the start of every
    // PSI each ghost whose core is outside every area and overlaps no other
    // player ball (centre distance at least the sum of the cores' upper limit
    // radii; every player ball in collision detection counts, asleep or not)
    // turns SOLID; a body that came since the last PSI to a solid group (a
    // trafo) and a new solid ball have their pairs rechecked there where
    // another player ball's overlap-tree sphere meets theirs (a group with no
    // body at the last PSI reads as a ghost while its new body is inserted),
    // sorted by group.  `respawns` false: only a group's first life is a
    // ghost.  enable = false switches it off; `out` then carries the counters
    // as they stood (active false).
    // The state is a few flags per group (spawn_ghost_record below) that the
    // IVP savestate carries (physics_savestate.hpp, format 3): a rollback's
    // restore and the one state transfer (a late join's or a resync's
    // travelling blob) put it back with the world it belongs to.  Every
    // decision reads only the world's own copies, so the server and every
    // client decide at the same PSI from the same inputs.
    struct spawn_ghost_info {
        bool active = false;
        uint32_t areas = 0;            // spawn areas found
        double radius = 0.0;
        uint64_t solidified = 0;       // ghost -> solid transitions so far
        uint64_t rechecks = 0;         // pair rechecks (new solid balls and resumed bodies near another ball)
        uint64_t overlap_waits = 0;    // PSIs a ghost outside every area waited for an overlap to end
        uint64_t births = 0;           // spawn births marked (spawn_ghost_birth)
        uint64_t resumed = 0;          // bodies that came to a solid group (a trafo) and stayed solid
    };
    bool spawn_ghost_set(CKIpionManager* physics, bool enable, bool respawns, double radius, spawn_ghost_info& out,
                         std::string& error);
    bool spawn_ghost_get(CKIpionManager* physics, spawn_ghost_info& out);
    // 1 when `entity`'s body is a player ball that is a spawn ghost now, 0
    // when it is solid (or not a player ball, or the rule is off), -1 when
    // it has no body.
    int spawn_ghost_state(CKIpionManager* physics, const char* entity_name);
    // A spawn birth of `group` ("P#<join order>"): the Physicalize the
    // lifecycle flags PHYSICALIZE_FLAG_SPAWN, in every world at its applied
    // tick, before that tick's step (the server's apply_event, a mirror's
    // birth, the own ball's capture or remake, a replay's re-issue).  The
    // group's state is a ghost from here (with `respawns` false only before
    // its first life).  Idempotent.  False when the rule is off.
    bool spawn_ghost_birth(CKIpionManager* physics, const char* group);
    // A member left (the server's remove_player, after its ball is gone):
    // `group`'s state is dropped, so the next member that takes the join
    // order begins its first life as a ghost however its first body is made
    // (step SPGH2 review: a leave ends a life).  The server's world only: a
    // late joiner and a resynced member take the server's records through the
    // one state transfer; the members, which are not told of a leave yet,
    // follow with world agreement's member messages (M0/M1).  Nothing when
    // the rule is off.
    void spawn_ghost_forget(CKIpionManager* physics, const char* group);

    // One group's spawn ghost state, as the IVP savestate records it.
    struct spawn_ghost_record {
        std::string group;
        uint8_t seen = 0;      // a body of the group was seen at a PSI (its first life began)
        uint8_t present = 0;   // the group had a body in collision detection at the last PSI
        uint8_t solid = 0;     // solid (0: a ghost)
        uint8_t marked = 0;    // a spawn birth was marked since the last PSI
    };
    // The state of every group, sorted by group.  False (and `out` empty)
    // when the rule is off in this world.
    bool spawn_ghost_record_get(CKIpionManager* physics, std::vector<spawn_ghost_record>& out);
    // Replaces the state (a restore); nothing when the rule is off.  An
    // empty list is a world before any body was seen.
    void spawn_ghost_record_set(CKIpionManager* physics, const std::vector<spawn_ghost_record>& in);

    // ---- bridge API v26 (step OWG): the own ball's player group before its
    // birth tick's PSIs ----
    //
    // The retail script makes the own ball with its own collision group (not
    // a player group), while the server's clone and every remade copy are
    // made in the player's group "P#<join order>".  Both client hosts give
    // the live own body its group in the PreSimulate pass of its birth tick,
    // after the block made it and before the PSIs (headless:
    // capture_own_physicalize; retail: a callback queued at OnPhysicalize),
    // so the birth tick's PSIs see a player ball, as the server's do.  The
    // pairs it was given at its insertion under the old group are re-asked:
    // IVP re-asks the filter only when the overlap tree re-inserts the body,
    // so the group alone would leave a pair with a ghost (or no pair with a
    // solid ball) until then.
    struct own_group_result {
        std::string entity;
        std::string old_group;      // the group the block made the body in
        int player_pairs_before = -1;   // the body's collision pairs with player balls before the recheck
        int player_pairs_after = -1;    // and after (-1: not rechecked)
        bool rechecked = false;     // the group changed and the body is in collision detection
        int ghost = -1;             // spawn_ghost_state of the body after the change
    };
    // Sets `group` on the body of `entity_name` now and, when the group
    // changed, re-asks the filter for its pairs (recheck_collision_filter).
    bool adopt_own_group(CKIpionManager* physics, const char* entity_name, const char* group, own_group_result& out,
                         std::string& error);
    // adopt_own_group when the body exists (1); otherwise, when `behavior_id`
    // names a live behavior, queued into the manager's PreSimulate pass of
    // this frame as push_impulse does (2) - the Physicalize block that makes
    // the body runs before it.  0 with a message on failure.  Either path
    // logs its adoption to stdout (the bridge's BMMO_PHYSICS_STDOUT).
    int queue_own_group(CKIpionManager* physics, const char* entity_name, const char* group, uint32_t behavior_id,
                        std::string& error);
    std::string describe_own_group(const own_group_result& r);

    // ---- bridge API v8: session clock guard ----

    // Keeps the retail pause menu (which stops Gameplay_Ingame and writes the
    // physics time factor 0) from freezing or slowing the session: while
    // pause_behavior_id is inactive the factor is pinned to the value the run
    // had before, and every other pass only samples it, so the level scripts
    // drive the clock exactly as they do on the server.
    bool set_clock_guard(CKIpionManager* physics, bool enable, float time_factor, uint32_t behavior_id,
                         uint32_t pause_behavior_id, std::string& error);
}
