# The IVP savestate: carrying the engine's collision state across a rewind

Status: design and inventory only (2026-10-20). No code written yet. This is
the handoff for the implementation: everything below was read from the IVP
sources in `submodule/Ballanced/Source/BuildingBlocks/physics_RT/ivp` and from
the bridge (`BallanceMMOCommon/src/physics/physics_state.cpp`) and the rollback
engine (`BallanceMMOCommon/include/session/rollback.hpp`).

## Why

A rewind today restores every core bit-exactly (pose, both quaternions, the
leapfrog delta, the queued impulses, the calm references, the unit countdown,
the clock) and re-creates a narrow class of friction contacts. What it cannot
put back is the collision bookkeeping IVP builds only by running the impacts
itself, and a beam keeps whatever the live world has instead: which ledge
pairs hold contact points, the per-point friction state, the per-pair easing
and anti-energy, the mindists' lengths and features, the scheduled impact
events. Measured on the solo prop probe and the four-player start pile: a ball
nobody touches replays exactly, a touched one is metres off
(`docs/lockstep-without-snapshots.md`, "What the trace shows"). The fix is to
record that state with every tick and rebuild it at the anchor - a real
savestate, the thing GGPO's save/load always was.

## Can it be done through IVP's API alone?

Mostly yes, and that is the plan: no change to any IVP function.

* **Reading**: every scalar needed is a public member or has a public getter,
  or is a protected/private *non-bitfield* member reachable with the
  member-pointer shim the bridge already uses (`ivp_private_access.hpp`,
  `thief<>`: explicit instantiation is exempt from access checks). Lists are
  walked through public links (`exact_synapses`, `friction_synapses`,
  `collision_fvector`, `fr_pairs_of_objs`, `sim_unit_cores`, `min_hash`).
* **Writing**: objects are rebuilt with IVP's own public constructors and
  bookkeeping calls (`new IVP_Mindist(env, delegator)` + `init_mindist`,
  `IVP_Contact_Point(IVP_Mindist*)`, `IVP_Friction_System` +
  `add_core_to_system` / `add_dist_to_system` / `dist_added_update_pair_info`,
  `IVP_Core::add_friction_info`, `IVP_Mindist_Manager::insert_exact_mindist` /
  `insert_hull_mindist` / `remove_*`, `IVP_U_Min_List::add` /
  `remove_minlist_elem`, `IVP_Simulation_Unit::throw_cores_into_my_sim_unit` /
  `sim_unit_remove_core` / `clean_sim_unit` / `sim_unit_calc_redundants`,
  `IVP_OV_Tree_Manager::insert_ov_element` / `remove_ov_element`), and the
  recorded scalars are then written over what the constructors computed.
* **The exceptions**: four *protected bitfields* that a member pointer cannot
  address and that are genuine history:
  `IVP_Mindist_Base::coll_type` (the predicted event type the scheduled event
  reads), `IVP_Mindist_Base::synapse_sort_flag` (toggled by the minimizer;
  decides which synapse is "sorted 0", i.e. the sign of the contact normal and
  the core roles in the impact and the contact point),
  `IVP_Contact_Point_Fast::cp_status` (NORMAL / NEEDS_RECHECK / BROKEN) and
  `IVP_Contact_Point_Fast_Static::two_friction_values` (derivable: call
  `calc_virtual_mass_of_mindist()`). Two ways to set the first three:
  1. **a friend declaration** under `#ifdef IVP_PORTABLE_BMMO` in the two
     headers (`friend class IVP_BMMO_State;`) - no logic change, the
     accessor class lives in the bridge; or
  2. byte-level writes at a layout computed per compiler (MSVC and the
     Itanium ABI pack these units differently: `cp_status` sits at
     `&has_negative_pull_since - 4` under MSVC and `- 2` under GCC/Clang), with
     a runtime probe on the public bitfields of the same unit as a guard.
  The recommendation is 1: it is the same kind of header-only access the
  shim gives, declared where a reader can see it, and it cannot rot.
  Everything else - every function IVP runs - stays as it is.

## What a tick boundary holds (the inventory)

Right after a tick, `current_time` is on a PSI boundary and `state == IVP_ES_AT`.
The PSI (`IVP_Environment::simulate_psi`) ran, in order: revive list,
`simulate_sim_units_psi` (per unit: commit async pushes, controllers in
priority order, `calc_next_PSI_matrix` queued per core, movement check on the
unit's countdown, invalid mindists recalculated), then
`commit_all_calc_next_PSI_matrix` (integrates every touched core, advances
each object's hull manager), `commit_all_hull_managers` (hull events: OV
rechecks, hull mindists going exact, watchers spawning children),
`recalc_all_exact_mindists`, `recalc_all_exact_mindists_events` (every exact
mindist's next event scheduled from the post-PSI core state). Then deferred
union-find splits of units. The collision state is therefore a consistent
snapshot of all of the below.

### Environment and time manager
| field | access | note |
|---|---|---|
| `current_time`, `time_of_last_psi`, `time_of_next_psi` | shim | already in `bmmo_physics_clock_state` |
| `next_movement_check` | shim | retail counter; the BMMO build uses the per-unit one |
| `IVP_Time_Manager::base_time`, `last_time` | public | events are floats relative to `base_time`, rebased at every PSI |
| `min_hash` (event queue) | public list, `elems` via friend | entries: the PSI event + one per scheduled exact mindist; `IVP_U_Min_List::add` puts a new element BEFORE existing equal values, so ties sit newest first; the PSI event is added LAST by the live PSI (`IVP_Time_Event_PSI::simulate_time_event`), so it sits before every tied mindist event |
| `core_revive_list` + `IVP_Core::is_in_wakeup_vec` | shim / public | cores to revive at the next PSI (`ensure_core_in_simulation_delayed`) |
| `current_time_code` | friend | monotonic cache token: NEVER rewind it; bump it right after the clock, BEFORE any IVP computation of the restore, and invalidate every object's cache then (a bump alone invalidates only SIMULATED objects' caches: `IVP_Real_Object::get_cache_object` checks the code only under `IVP_MTIS_SIMULATED`) |
| `mindist_event_timestamp_reference` | friend | monotonic; leave it, and leave the cores' `mindist_event_already_done`: `update_exact_mindist_events_of_object` compares core against core, and the reference is incremented before every impact stamps anything, so a stale stamp can never equal a fresh one |
| `delta_PSI_time` | constant | |

### Per core (`IVP_Core`, all public)
Already restored: pose, both quaternions, `delta_world_f_core_psis`, `speed`,
`rot_speed`, `speed_change`, `rot_speed_change`, calm references, unit
countdown. Still to record: `time_of_last_psi` per core (for a frozen core it is the freeze or beam time, not the clock),
`current_speed`, `abs_omega`, `max_surface_rot_speed`,
`rotation_axis_world_space`, `i_delta_time` (all written by
`calc_next_PSI_matrix` and ZEROED by a beam), `movement_state`,
`temporarily_unmovable`, `impacts_since_last_PSI`, `pinned`,
`controllers_of_core` order (see units), `fast_piling_allowed_flag` (sticky: set by
`static_fr_oversized_matrix_panic`, read by `reorder_mindists_for_complex`),
and `m_world_f_core_last_psi` VERBATIM (12 doubles): it is written only by
`init_moving_core_for_psi` from the pre-commit `q_next`, and a core that froze
in its last PSI keeps the matrix of `q_next_old` and the advanced position while
`q_last`/`pos_last` stay put, so it is not a function of the recorded
quaternion. `tmp_null.old_sync_info` must be null at a boundary;
`mindist_event_already_done` left live.

### Per object (`IVP_Real_Object`)
| field | access | note |
|---|---|---|
| `flags.object_movement_state` | public | mirrors the core |
| hull manager scalars: `last_vpsi_time`, `gradient`, `center_gradient`, `hull_value_last_vpsi`, `hull_center_value_last_vpsi`, `hull_value_next_psi`, `time_of_next_reset` | public members of a protected base: C-style cast to `IVP_Hull_Manager_Base_Gradient*` is allowed | `check_for_reset` rebases every 10 s of absolute time, so the clock must be rewound for the reset to land on the same PSI |
| hull manager `sorted_synapses` (min-list) | `get_sorted_synapses()` public; element array via shim; `IVP_Listener_Hull::minlist_index` private non-bitfield via shim | entries in sorted order: the object's OV element, OO-watcher synapses, hull-mindist synapses, actuator anchors (none in Ballance; verify) |
| `ov_element->center`, `radius`, `hull_manager` (set or not), `node`, `collision_fvector` order | public | the element's node, the node's place in the tree, its siblings' order and the element's position in `node->elements` are insertion history (see "The overlap tree" below), and `remove_allow_resort` on a partner's fvector reorders it: both are rebuilt from the record, not re-derived |
| `exact_synapses` / `invalid_synapses` lists | public getters; order = prepend order | per-object views of the manager's lists |
| `friction_synapses` list | public getter; prepend order | one `IVP_Synapse_Friction` per contact point per object |
| `cache_object` | - | invalidate (`IVP_Cache_Object_Manager::invalid_cache_object`, public static) |

Statics matter too: a hull mindist registers a synapse on BOTH objects' hull
managers, and a static core keeps an `IVP_Friction_Hash` of its friction
infos. Record the hull min-list of every object that has entries.

### Per collision checker (`IVP_Collision` tree under each OV element)
Kinds: `IVP_Mindist` (two single-ledge objects), `IVP_Mindist_Recursive`
(ledge-tree node pair; children in `mindists`, `recursive_status`,
`spawned_mindist_count`), `IVP_OO_Watcher` (any object with a ledge tree:
children in private `mindists`, two `IVP_Synapse_OO` hull listeners). The
delegator of a root is `env->collision_delegator_roots[k]`, of a child its
parent. `fvector_index[2]` are rebuilt by the vectors.

Per mindist (`IVP_Mindist_Base` public unless noted): `synapse[0..1]` =
(object, compact edge, status POINT/EDGE/TRIANGLE/BALL) - set with the public
`update_synapse(edge, status)`; `coll_type` (protected bitfield),
`synapse_sort_flag` (protected bitfield), `is_in_phantom_set`,
`mindist_function`, `recalc_result`, `disable_halfspace_optimization`,
`mindist_status` (UNINITIALIZED/INVALID/EXACT/HULL_RECURSIVE/HULL),
`coll_dist_selector`, `coll_dist_decrease_counter`, `sum_extra_radius`,
`len_numerator`, `contact_dot_diff_center`, `sum_angular_hull_time`,
`contact_plane`; `IVP_Mindist::recalc_time_stamp` (protected, shim; write 0),
`last_visited_triangle` (wheels only; null); the scheduled event
(`index != IVP_U_MINLIST_UNUSED`, time = element value + `base_time`); for a
HULL one the two hull-manager values; list membership (exact list, invalid
list, or hull = synapses in hull managers).

An edge is named by (object, byte offset inside the object's compact surface,
status). Polygons: `IVP_SurfaceManager_Polygon::get_compact_surface()` base
and `get_size()` bound. Balls share one static dummy ledge
(`IVP_SurfaceManager_Ball::compact_ledge`, via `get_single_convex()`); name it
"ball".

### Friction
Enumerate systems through the movable cores
(`core_friction_info.for_moveables.moveable_core_friction_info->l_friction_system`).
Per system: `cores_of_friction_system` order, `moveable_cores_of_friction_system`
order, the contact-point list from `get_first_friction_dist()` (prepend order:
`add_dist_to_system` pushes front; the friction solver's matrix columns follow
this order, and `bubble_sort_dists_importance` is a stable insertion sort on
`has_negative_pull_since`, so the order IS history), `fr_pairs_of_objs` order
with per pair: `objs[2]`, `next_ease_nr_psi`, `last_impact_time_pair`,
`integrated_anti_energy`, `fr_dists` order (private, shim); private counters
`friction_obj_number`, `friction_dist_number`, `complex_not_necessary_number`
(shim); public bitfields `union_find_necessary`, `fr_sys_simulated`. Per core
and system an `IVP_Friction_Info_For_Core` with `friction_springs` order.

Per contact point (`IVP_Contact_Point_Fast*`, protected non-bitfields via the
shim the bridge already has for six of them): `synapse[2]` (object, edge,
status - from the dummy mindist handed to the constructor), `last_gap_len`,
`span_friction_s[2]`, `now_friction_pressure`, `has_negative_pull_since`,
`slowly_turn_on_keeper`, `real_friction_factor`, `integrated_destroyed_energy`,
`inv_triangle_det`, `old_energy_dynamic_fr`, `last_time_of_recalc_friction_s_vals`
(IVP_Time), `last_contact_point_ws`, `surf_normal_ws`,
`inv_virt_mass_mindist_no_dir`, `cp_status` (protected bitfield),
`two_friction_values` (bitfield, derivable), `l_friction_system`.
`tmp_contact_info` is transaction memory, re-set by `recalc_friction_s_vals()`.

Constructing a contact point needs an `IVP_Mindist*` whose sorted synapses
carry the recorded objects/edges/statuses: make a throw-away `IVP_Mindist`
(public constructor, no `init_mindist`) with a private no-op
`IVP_Collision_Delegator`, set `synapse[i].l_obj` (public) and
`update_synapse(edge, status)`, balance `add_reference_to_ledge` for polygon
ledges, construct the point, delete the dummy (its destructor only removes a
ledge reference and tells the delegator).

### Simulation units
Per unit: `sim_unit_cores` order, movement type (getter/setter), the three
public flags, `next_movement_check` (BMMO), and the ORDER of `controller_cores`
(one entry per controller; the live list is built incrementally by
`add_controller_of_core` - append, then a stable insertion sort - so for equal
priorities it is history that `clean_sim_unit` + `sim_unit_calc_redundants`
cannot reproduce: that rebuild walks cores and controllers in reverse). The
record keeps a descriptor per entry and the restore permutes the public vector
with `swap_elems` after the rebuild. The
units manager keeps two linked lists only (`sim_units_slots[0]` for simulated
units, `still_slot` for the rest), prepend order; `simulate_sim_units_psi` is
prefetch-unrolled, so for a list u0..u(n-1) it processes u0..u(n-4) then u(n-1),
u(n-2), u(n-3) - a fixed function of the list order, which therefore has to be
exact. Every core has a unit, static cores too (one each, in the still slot),
and `delete_friction_distance` dirties their `union_find_needed` flag, so the
record carries EVERY unit. Only slot 0 is ever used. Per core: `controllers_of_core` order, with the three
friction handles (static 0 / dynamic 600 / energy 2000) and the others
(gravity 1000, constraints 405, forces) - record a descriptor per entry and
reorder the public vector after the friction systems are rebuilt.

### The overlap tree (`IVP_OV_Tree_Manager`)

`recheck_ov_element` gets its candidate partners in tree-traversal order
(`collect_collision_partners`: elements walked from the back, children in
vector order), and that order sets the order new collisions are created in:
exact-list prepend order, event-queue tie order, `collision_fvector` order.
The tree is history: nodes are created by `connect_boxes` / `expand_tree` and
deleted only when empty, `find_smallest_box` takes the first child in children
order that contains the box, and `node->elements` keeps insertion order. So the
record carries the tree itself: every node's `data` (x, y, z, rasterlevel,
sizelevel), its parent, its children in order, its elements (objects) in
order, the root, and each element's node. All public members except `root`
and `hash_table` (friend). The teardown removes every element (statics
included), which empties the tree (`cleanup_node` deletes empty nodes up the
chain and clears the root); the rebuild creates the nodes with the public
constructor, links them, adds them to the hash (`IVP_ov_tree_hash::add_node`,
public), sets the root, and files the elements.

### The object cache ring (`IVP_Cache_Object_Manager`)

Every object that collides owns a slot in a ring of cache objects holding its
`q_world_f_object`, `m_world_f_object` and `core_pos`. A simulated object's
slot is refreshed whenever the time code advanced; a NOT-simulated object's
slot is never revalidated, so it holds the matrix computed when the slot was
taken: the raw quaternion when that was a PSI boundary, otherwise
`set_interpolate_smoothly(q, q, t)` + `normize_correct_step(3)`, which moves the
low bits. Static and sleeping partners feed every mindist recalc and every
contact recalc through those caches, so the contents are history. The record
carries the ring: per slot the object (or none), whether the slot was fresh
(`valid_until_time_code == current_time_code`), the quaternion, the matrix and
the core position, plus `reuse_loop_index`; the restore writes them back as its
LAST step, with the freshness flag resolved against the bumped code, and sets
each object's `cache_object` back-link. Until then every object's cache is
invalidated (`IVP_Cache_Object_Manager::invalid_cache_object`), so nothing the
restore computes reads a stale pose.

### The physics_RT side (outside IVP, read by the next step)

* `CKIpionManager::m_MovableObjects`: membership and order (duplicates
  included), maintained only by the object listener on revived/frozen events.
  `Simulate` writes CK matrices back only for bodies in it, and the world hash
  and `describe_cores_exact` enumerate it. Recorded as object indices; written
  back last.
* Every physicalized entity's CK world matrix (16 floats): the scripts read it
  (hinge/spring referentials, Physics Impulse, the proximity sensors), and
  `UpdateObjectWorldMatrix` re-derives the scale from the live matrix, which
  drifts by ulps over a chain of updates. Recorded; written with
  `SetWorldMatrix` after the cores are final.
* `m_DeltaTime`, `m_PhysicsDeltaTime`, `m_PhysicsTimeFactor`: the step's delta
  comes from the first two (an exponential moving average, a fixed point under
  the fixed tick) and the factor is what the pause scripts toggle. Recorded.
* The IVP RNG cursor (`ivp_srand_read` / `ivp_srand`) and the bridge's
  deterministic Random state (`random_get_state` / `random_set_state`).
  Recorded.
* Gravity (`IVP_Environment::get_gravity`): recorded and asserted.
* The PreSimulate callback queue (`m_PreSimulateCallbacks->m_Callbacks[0..2]`,
  run newest first before `simulate_dtime`): retail blocks park a callback
  there while a body does not exist yet, and the bridge cannot rebuild a
  retail callback. Recorded as descriptors (slot, behaviour id, type); the
  restore compares the live queue with the record (BMMO's own callbacks
  excluded) and fails closed on a difference.
* The environment's identity (the `IVP_Environment` pointer, as an integer):
  a blob is never applied to a rebuilt environment.
* Listeners: the teardown fires `friction_deleted` for every contact point and
  the rebuild fires no `friction_created` (only `try_to_generate_managed_friction`
  does), so during a restore the environment's collision listeners are
  swapped out, every object's `collision_listener_exists` flag is cleared, the
  bridge event log is muted, and all three are put back afterwards; with the
  cores written directly (no IVP revive/freeze), no object event fires either.
  `PhysicsObject::m_FrictionCount` has no reader; the continuous-contact
  outputs drive sounds only.

## The restore, in order

Two rules decide the order. First, nothing IVP computes may run on the live
world's poses or caches, and nothing IVP computes may survive: every value a
constructor or bookkeeping call sets is overwritten from the record
afterwards. Second, no IVP transition (revive, freeze, recheck, impact) runs
at all: they rebuild exactly what the teardown removed, with the live
world's history. Cores are written directly.

0. **Preconditions.** The environment identity matches the blob; no object
   has a phantom controller (else fail); every object is indexed by name. Mute:
   the bridge event log, the environment's collision listeners (swap the
   vector out) and every object's `collision_listener_exists` flag.
1. **Clock.** Environment times, `base_time`, `last_time`,
   `next_movement_check`, `state`; the manager's three floats; the RNG cursors.
   Then `current_time_code++` and `invalid_cache_object` on every object.
2. **Teardown.** Every contact point through its system
   (`delete_friction_distance`), then `P_DELETE` each system once
   `friction_dist_number == 0` (never `remove_core_from_system` by hand: the
   deletion already did it, and a second call writes `elems[-1]` in release).
   Every root collision of every object (`delete coll`: the destructor removes
   events, list membership, hull registrations, ledge references, children
   and the delegator's fvector entries). Every OV element (statics too):
   `remove_ov_element`, `remove_synapse` from its hull manager,
   `hull_manager = NULL` - the tree is now empty. The revive list is emptied
   with `is_in_wakeup_vec = FALSE` on each core. What remains in a hull list
   must be nothing (an entry of a kind the record does not know is an error).
3. **Cores and objects, first pass.** Every recorded core scalar written
   directly, `m_world_f_core_last_psi` verbatim, `flags.object_movement_state`
   on each object, `fast_piling_allowed`, the hull-manager scalars of every
   object through the base cast. Gravity asserted.
4. **Units.** In REVERSE recorded order: a new `IVP_Simulation_Unit`, each
   recorded core moved in (`old->sim_unit_remove_core(core)`, which deletes an
   emptied unit, then `add_sim_unit_core`, `sim_unit_of_core`), then
   `clean_sim_unit` + `sim_unit_calc_redundants`, type via the setter, flags,
   countdown, `add_sim_unit_to_manager` (prepend - hence the reverse order).
   Static cores' units included.
5. **Collisions.** For every recorded collision, in record order (parents
   before children): a mindist is `new IVP_Mindist` / `new IVP_Mindist_Recursive`
   with the parent (or the environment's root delegator) and NO `init_mindist`:
   the synapses are written through the friend (object, edge, status),
   polygon ledges get `add_reference_to_ledge`, every field is written,
   `mindist_status` is written directly, children are appended to the parent's
   vector. A watcher has to go through its constructor, which creates
   children: the constructor runs in a sandbox (every movable core's
   `speed_change` / `rot_speed_change` and the revive list are saved and put
   back), its children are deleted, the recorded children are built as above
   into the private vector. Roots are added to both objects' fvectors
   (`add_oo_collision`), then each object's fvector is permuted to the record
   with `swap_elems`.
6. **The overlap tree.** Nodes created and linked as recorded, hash entries
   added, root set, elements filed in recorded order with `node` set back,
   `center`/`radius` written.
7. **Lists.** `exact_mindists` and `invalid_mindists` relinked (next/prev,
   head) to the recorded order; every object's `exact_synapses` and
   `invalid_synapses` relinked likewise.
8. **Hull lists.** Every object's min-list emptied (`remove_minlist_elem` on
   every entry), then refilled by `add` in REVERSE recorded order with the
   recorded values (newest-first ties reproduce the record), each listener's
   `minlist_index` written, the OV element's `hull_manager` set when recorded.
9. **Event queue.** Every element removed, PSI event included, then re-added
   in REVERSE recorded order: the PSI event at its recorded value and every
   scheduled mindist (`index = add(...)`), `coll_type` from the record.
10. **Friction.** Systems in recorded order (`new IVP_Friction_System`, cores
    via `add_core_to_system` in order, one `IVP_Friction_Info_For_Core` per
    recorded info via `add_friction_info`). Contact points constructed from a
    throw-away mindist carrying the recorded synapses (private no-op
    delegator), then every recorded scalar written, `tmp_contact_info = NULL`,
    `l_friction_system`, and NO `recalc_friction_s_vals` (the energy
    controller recomputes it first thing in every PSI, every impact path
    recalcs first). The system's intrusive list relinked to the record
    (`first_friction_dist`, next/prev, `friction_dist_number`), pairs created
    with the recorded `objs`, fields and `fr_dists` order and appended to
    `fr_pairs_of_objs` in order (no `add_fr_pair`: it fires an event), infos'
    `friction_springs` filled in order, counters written. Every object's
    `friction_synapses` list relinked to the record.
11. **Cores, second pass.** Every core scalar of step 3 written again: the
    watcher sandbox and `add_core_to_system` may have touched calm references
    or queues.
12. **Controllers.** Each core's `controllers_of_core` permuted to its
    descriptors (friction handles by system and part; others matched by
    priority and name, equal keys in live relative order), then each unit's
    `controller_cores` permuted to its descriptors. The rollback engine calls
    `set_nav` (which re-creates the navigation force controllers) BEFORE
    `set_savestate`, so they are in place to be ordered.
13. **Revive list.** `ensure_core_in_simulation_delayed()` per recorded core,
    in order, then per unknown core the live list held (the teardown kept
    their names).
13b. **Unknown bodies.** Every live object the record does not know (born
    after the anchor, a sector physicalized inside the window) is now out of
    collision detection - the teardown took its tree node and hull entry, the
    rebuild filed only recorded ones, and the engine re-inserts an object only
    on a hull event, of which there is none without an entry. The restore
    leaves them so and names them in the report; the rollback engine decides:
    a body it will re-pose at its birth tick stays inert until then (the live
    run had no such body before that tick; a parked body with collision
    detection on was measured leaving its spot 5 m up in the first live tick
    after a 48-tick replay), and a body it has no record of at all is
    rechecked back in at once (`rollback_world::recheck_body`). Under
    `SAVESTATE_KEEP_UNKNOWN` they keep their entry and their kept collisions
    instead.
14. **The physics_RT side.** `m_MovableObjects` rebuilt in recorded order with
    the unknown bodies kept after it in their live order (membership is how
    `Simulate` writes a body's pose to its CK entity; a body dropped here keeps
    rolling in IVP while the entity the scripts read stays where the anchor
    left it), every recorded entity's CK matrix set, the PreSimulate queue
    compared with the record (fail closed), listeners and the event log put
    back. The frame delta's moving average is written back in step 1; the
    time factor and the gravity are compared and left LIVE - they are written
    by the Gameplay script (`SetPhysicsGlobals`: 1.0 while the level loads,
    2.0 once it runs, 0 in the pause menu), which the replay does not
    re-execute, so a blob from before that write would otherwise put 1.0 back
    for good.
15. **Caches.** The ring written verbatim with freshness resolved against the
    new code and the objects' back-links set; every mindist's
    `recalc_time_stamp` set to the new code when it was fresh, else 0.

Bodies alive now but absent from the record (born inside the window) keep
their place in the movable list and the revive list, are parked inert (no
collision detection, 13b) and are rechecked in when the rollback re-poses them
at their birth tick. Bodies in the record but gone now are skipped
(lenient) or fail the restore (strict), and so is every record that refers to
them; an overlap-tree node left with no occupant by such a skip is pruned as
`cleanup_node` would have pruned it, or the next teardown finds a non-empty
tree. A failure past the teardown no longer leaves the world as it fell: the
restorer salvages it through the engine's entry points (the PSI event queued,
a unit per core, every body out of the tree rechecked back in) so it keeps
simulating, and the caller's per-body fallback writes into a consistent world.
Measured before the salvage: a refused restore at step 6 was followed by an
access violation in `~IVP_OO_Watcher` at the next rewind.

**Bodies born inside the window, re-created (2026-10-22, plan doc 9).** Parking
was exact in value and wrong in order: the 20:12 and 50:48 arena arms came out
of every rewind with their blobs equal to the control's in every field but the
overlap-tree layout (two extra nodes from the first rewind that spanned births),
the unit indices and the cache reuse index, and the plate row then drifted by
ulps in the floor's hull entries a few live ticks after any rewind through it,
while the 7:5 arm, whose rewinds left the tree identical, stayed exact.  A
recheck after the birth tick's step files the body in the tree, in a unit and
in a cache slot at a different moment and by a different path than the live
Physicalize did, and IVP's order is part of the arithmetic in a pile.  So,
when the caller installs `rollback_world::destroy_body` and `recreate_body`
(the headless client and the retail mod do; the SimTool scenes have no birth
inside a window), a body born inside the window is destroyed BEFORE the blob
is applied - the blob then meets exactly the object set it recorded and parks
nothing - and made again at the start of its birth tick, before that tick's
scripts and step, with the recipe, collision group and entity matrix its
Physicalize used: engine change #19 keeps `CKIpionManager::m_CurrentCreate`
pointing at the creation arguments while the body is made, the bridge's
object listener writes them into the event log as a `recipe` line at every
creation, the headless client keeps those by tick (`local_births_`), the retail
mod keeps BML's `OnPhysicalize` parameters the same way.  After the birth
tick's step the body is expected bit-identical to its birth record
(`births_recreated_exact`); a difference is counted (`birth_recreate_mismatches`)
and corrected as the parked path corrected it.  A body with no recipe (born
before the session's log started) is parked as before
(`birth_recreate_failures`).  The event log is muted around both operations:
they are not lifetime changes of the body the records describe.
Measured (20:12 arm, 46 births inside windows): 45 came back bit-identical
after their birth tick's step - the one that did not had been moved by its own
script in the same frame after the Physicalize.  But a body's joints and force
controllers are made by module scripts that were activated inside the window
and are kept live rather than re-run (bridge API v14), so they went with the
destroyed body and the plate row fell off its joints.  Re-creation is therefore
the `--recreate-births` A/B, not the default.

The default keeps the body parked and fixes what it can of the ORDER:
`birth_order` applies a tick's births in the live creation order (the tree
takes its layout from insertion order), and `birth_tick_of` lets a body the
blob does not know AND the records do not track - a fixed part of a module,
born inside the window - wait out of the tree until its birth tick instead of
being rechecked in at the anchor (that was the two extra overlap nodes, from
the first rewind that spanned the pusher's rail and filler).  With these the
overlap tree is identical to the control's at every compared tick, the 7:5
arm stays at one body off (the swing), and the cache reuse index and the unit
numbering still differ.  `insert_birth` - the parked body put where its
Physicalize made it at the start of its birth tick, before that tick's scripts
and step, with the recheck (bridge API v19 `set_body_matrix`: the entity's
matrix set, the pose derived from it as the creation derives it, a beam at
rest, awake or frozen as the Physicalize left it) - is an A/B
(`--insert-births`): a beam normalizes the quaternion where the creation does
not, so most insertions miss their birth record in the last bit and fall back
to the re-pose, a second recheck after the step, and the 7:5 arm came out off
on six bodies with it.  The plate row still drifts after a deep rewind through
it: `--savestate-diff --ignore-ov --ignore-order` puts the first physical
difference in the hull entries of the plates' mindists, the collision state a
recheck creates at a different moment than the creation.  The end of this
road is plan doc 10, built 2026-10-22 (plan doc 11-12): the sector activation
is re-issued inside the replay at its recorded tick, every selected script is
restored (the manager among them, its in-flight delayed links and the joint
blocks' handle presence in the record, three data-array cells with them), the
window-born mechanism bodies are destroyed at the restore with their entities
put back at the matrix the Physicalize found (the local one under a parent),
and the replayed module scripts make them again with their joints and forces.
Result: the 50:48, 20:12 and 7:5 arms are EXACT against the control on all 53
bodies (49/46/50 births made again, all bit-identical after their birth step);
the parking path remains as the A/B (`--park-births`).

## Pitfalls already found

* `IVP_IF(x)` is `if(0==1)` in release: `remove_exact_mindist` and friends do
  NOT reset `mindist_status`; write it yourself.
* `coll_dist_decrease_counter` is advanced by `update_exact_mindist_events`;
  do not recompute events at the restore, restore the queue directly.
* `recalc_mindist` starts the minimizer from the synapses' current features;
  the minimum is unique but the float bits and the representative feature are
  path-dependent, so the features must be restored, not re-derived.
* `IVP_Calc_Next_PSI_Solver::set_transformation` (the beam) zeroes
  `current_speed` / `abs_omega` / `max_surface_rot_speed`, jumps the hull and
  rechecks the OV element: the savestate path must write cores directly and
  never beam a body it restores. Beams stay for snapshot rows written on top.
* `IVP_U_FVector::remove_allow_resort` swaps the last element in: vector
  order is history. Record and permute.
* The min-list's skip pointers (`first_long`) are an index only; iteration
  order is what the engine reads.
* Static objects' hull managers have zero gradients but carry entries.
* `IVP_U_Vector::remove` on an absent element writes `elems[-1]` in release
  (the assert is compiled out): never remove what IVP already removed.
* `rem_sim_unit_from_manager` picks the slot from the unit's CURRENT type:
  remove with the old type, then set the new one, then re-add.
* `add_revive_core` returns early when `is_in_wakeup_vec` is already set:
  clear the flag when emptying the list, write it only through the re-add.
* `insert_hull_mindist` recomputes `sum_angular_hull_time` and sets the status
  to HULL even for HULL_RECURSIVE: the restore never calls it; it links hull
  entries itself.
* The savestate translation units do no floating-point arithmetic of their
  own (BallanceMMOSim is built without the physics FP flags): every value is
  copied, never re-derived.
* Phantoms (`IVP_Controller_Phantom`, the PhysicsBuoyancy block) keep a set of
  mindists and per-core buoyancy controllers with PSI history; deleting a
  phantom mindist destroys that controller. No retail script uses the block
  (every root script of level 1 and every P_/PE_/PC_/PS_ script of levels 1-13
  was dumped and checked), so the savestate FAILS CLOSED: a world with a
  phantom object refuses to capture and to restore.
* The physics time factor and the gravity are SCRIPT state that happens to
  live in the engine: the Gameplay script writes them, the replay re-executes
  only the mechanism scripts. Record and compare, never write (found on the
  arena map: the first forced rewind of a 20:12 cadence restored the blob of
  tick 8, factor 1.0, and the client's whole world ran at half rate for the
  rest of the session - ball x 29.99 against the control's 122.53 at tick 604,
  every per-tick position still bit-identical to a control state, just at the
  wrong tick).
* A live body the record does not know must keep its `m_MovableObjects`
  membership and be rechecked into collision detection; both were dropped by
  the first restore. Found as a mechanism that never woke: the ball born
  inside the first rewind's window fell out of the list, `Simulate` stopped
  writing its CK matrix, and `P_Modul_01`'s proximity sensor measured the
  distance to a ball that never left its spawn, on every later rewind, for the
  rest of the session (the body itself stayed bit-exact in IVP, which is why
  the journals looked fine).
* A recorded overlap-tree node whose only occupants were skipped (a ball
  unphysicalized between the anchor and now) must be pruned at the restore;
  IVP never keeps an empty node, and the next teardown's "tree is empty"
  check fails on it (seen once, followed by the crash above).
* Controllers are matched by (priority, name), and every retail controller is
  named `"sys:unknown"`: two force blocks on one body, or two held navigation
  keys, were indistinguishable and came back in rebuild order. Our navigation
  leaves are named `bmmo:force/<leaf>` and the retail force controller
  `rt:force/<behavior id>` (engine change #18, physics_RT only).
* Bodies are keyed by name and Ballance maps repeat names (two instances of
  one element type, e.g. Level 8's two `P_Modul_30_Wippe`): the record
  carries the entity id and the live index resolves a shared name by it.
* `worst_bodies.py` compared journal keys by name, and the player's ball is
  keyed by its session-specific player id: the one body the test was about
  was never compared. It now pairs the single ball of two solo journals.

## Record size

Per tick: every movable core (~100 B) + every object's hull scalars and
listener entries + every collision (~80 B) + every contact point (~120 B).
A four-ball bay with props: a few hundred collisions, a few dozen contacts,
well under 100 KB per tick, so a 96-tick history is under 10 MB.

## Bridge and engine plumbing

* Bridge API v17/v18: `savestate_get(mgr, uint8_t* out, uint32_t max, err)`
  returning the full size (snprintf style) and
  `savestate_set(mgr, const uint8_t*, uint32_t size, uint32_t flags, err,
  char* dropped, uint32_t dropped_size)` (v18: `dropped` names the bodies
  whose friction contacts the blob could not carry, `;`-joined); C++ entries
  `bmmo::physics::get_savestate` / `set_savestate` with a `savestate_report`
  (counts, `unknown_bodies`, `contacts_dropped`, warnings); the blob is a
  versioned little-endian byte vector written by the writer/reader in
  `physics_savestate_common.cpp`.
* `rollback_world::get_savestate` / `set_savestate(blob, contacts_dropped)`;
  `tick_state::savestate`; `restore_and_replay` takes the savestate path
  (clock + teardown + rebuild) when the anchor has one and a replay will
  follow, then writes snapshot rows on top with the existing beam path; a
  covered body named in `contacts_dropped` still gets its contacts from our
  own record (`restore::recontact`); the old per-body path stays for records
  without a savestate and as an A/B (`--no-savestate` on the headless client,
  `set_exact_restore(false)` on the engine). Stats: `savestates_restored`,
  `savestate_failures` (refused blobs), `savestate_capture_failures`.
* One binding of the engine's hooks for every user of the engine:
  `session/rollback_binding.hpp` (`bind_rollback_world<Backend>`) binds body,
  extra, clock, contact, navigation, savestate and script-replay hooks plus
  the replayed tick's step (the scripts' frame, then one physics step) over a
  backend with physics_view's method names - `physics/physics_direct.hpp`
  (in process, calls `bmmo::physics::*` on the CKIpionManager) for the
  headless client and the SimTool, the retail mod's `physics_view` (the
  plugin's C API) for the mod.  Everything that differs between the users -
  the headless client's deferred births, traces and `--replay-scripts`, the
  mod's pause-menu input mute, logger and journal - comes in as
  `rollback_binding_options`; `grep "world\.set_body = "` hits the binding
  alone (`docs/rollback-core-unification-plan.md`).  `BMMO_PHYSICS_API_VERSION
  18u`; protocol unchanged (local only).
* Engine change #17 (header-only, `IVP_PORTABLE_BMMO`): `friend class
  IVP_BMMO_State;` in nine IVP headers (`ivp_mindist.hxx`,
  `ivp_friction.hxx`, `ivp_listener_hull.hxx`, `ivp_real_object.hxx`,
  `ivu_min_list.hxx`, `ivp_environment.hxx`, `ivp_sim_unit.hxx`,
  `ivp_clustering_longrange.hxx`, `ivp_cache_object.hxx`; 69 inserted lines,
  nothing else in IVP changed). Engine change #18 (physics_RT only): the force
  controller named after its block. Both in `docs/engine-changes.md`. Submodule
  `Swung0x48/Ballanced`, branch `bmmo-collision-overhaul`; CI fetches it, so
  push the submodule first.

## Tests that prove it (the robustness matrix)

The claim is "the physics after any number of rewinds is bit for bit the
physics of the straight run".  The proof is a SimTool mode plus two end-to-end
checks, and every one of them compares WHOLE savestate blobs, not poses:
a blob is the engine's entire dynamic state, so two identical blobs mean the
next PSI cannot differ, and the first differing field names the bug.

### SimTool `--savestate-check` (BallanceMMOSimTool, physics only)

One process, two passes over the same scene:

1. **Straight pass**: load the scene, step `N` physics ticks with
   `bmmo::physics::step_physics` (no scripts: a pure physics proof; the
   frame mode below covers the scripts), and keep the blob `B[t]` of every
   tick (or every tick up to a memory cap, then every k-th plus every
   anchor the schedule will use) and the world hash `H[t]`.
2. **Rollback pass**, `R` rounds from a seeded schedule of (anchor, depth)
   pairs covering depths 1..48 and anchors over the whole run, chained: the
   world is wherever the previous round left it when the next restore runs,
   which is the real-world case (the live world has moved on with another
   history).  Each round: `set_savestate(B[a])` (strict), then
   - **restore identity**: capture again and compare with `B[a]` byte for
     byte (a mismatch here is a rebuild bug - list order, a field the
     constructor set that the restore did not overwrite - and shows before
     any physics runs);
   - **replay identity**: step `d` ticks and compare the blob with
     `B[a + d]` (and `H[a + d]` and `describe_cores_exact` as independent
     witnesses).
   The first failure is reported with `diff_savestate` (record kind, object,
   field, both values in hex) and the round's (anchor, depth), then the tool
   exits non-zero.  `--savestate-check` prints a one-line summary per round
   on `--verbose` and a final `PASS rounds=R` / `FAIL` line.

Scenes (each is an existing SimTool scene switch; the mode composes with
them): `--level 13` arena with `--spawn-test 4` (four balls born at one
point - the coincident spawn), `--drop` of the prop balls onto a bay,
`--activate` of a sector (mechanisms, hinges, the plate row of
`P_Modul_29`), Level 8 (`P_Modul_30_Wippe` see-saw), Level 3 (`Dreharme`),
a level with a floating/buoyancy phantom if one exists, plus
`--explode wood AT` (bodies created and destroyed inside the run - a
restore across births; the mode must document whether those rounds are in
or out of scope and why).

Schedule knobs: `--rounds R --max-depth D --seed S --ticks N`, and
`--savestate-every k` for the memory cap.  Depth 0 rounds are included (a
pure restore on the state the world is already in) and depths beyond the
rollback's `max_resim_ticks` are not needed.

### Frame mode (`--savestate-check --frames`)

Same, but the straight pass steps whole engine frames and also keeps the
script-replay bytes (`game/script_replay.hpp`), and a round is the rollback
engine's own `rewind_to` with `set_replay_scripts(true)`: the scripts
restored at the anchor the way the rollback does, then every replayed tick
re-stepped the way the headless client re-steps - the selected mechanism
scripts' frame (`script_replay::execute_frame`), the world's PreSimulate
pass, one physics step - never a whole frame, whose other scripts
(Gameplay_*, the checkpoints) are not rewound and would run on from where the
live run left them.  So a frame-mode mismatch is a defect of the recipe the
client uses.  This is the integration check for the mechanism levels.  Not
covered here: a body born inside the window (the harness sets every event up
before it); the arena journals cover that case, and it is where the
movable-list and recheck gaps above were found.

In every mode the check drives the real engine: the straight pass records each
tick with `rollback_engine::record` through the shared binding
(`session/rollback_binding.hpp`), a round is `rewind_to(anchor,
anchor+depth)`, the restored world is captured in the binding's
`before_replayed_tick` hook for the restore-identity check and every replayed
step's blob in a wrapped `step` for the replay-identity check;
`--legacy-restore` is `set_exact_restore(false)`, the engine's own per-body
path, so the negative control is the production code with the savestate
switched off and nothing in the tool re-implements a restore.

Two clients' records can be compared field by field as well:
`BallanceMMOSessionClient --dump-savestate A B DIR` writes the record's blob of
every tick in [A, B] as `<t>.sav` (again after a rewind re-recorded it), and
beside it the mechanism scripts' state of that tick as `<t>.scr`;
`BallanceMMOSimTool --savestate-diff DIR1/<t>.sav DIR2/<t>.sav` prints the
first field that differs (`--savestate-describe FILE` prints a blob whole),
the environment identity excepted.  That is how an arena journal's position
drift is traced to the engine field behind it.  The scripts' side of the same
question - where two runs' SCRIPT states part, which neither a journal nor a
blob diff shows - is `--compare-scripts DIR`: at every tick for which DIR holds
another run's `<t>.scr`, this run captures its own scripts and logs every
change of the first difference by script, block and field (the harness runs
the control with `--dump-savestate` and the arm with `--compare-scripts
../../../control/sector1/client0/savestates`; the control compares itself with
its own files and stays quiet).  A replayed record (a rewind re-records the
ticks it replayed, and re-dumps them) is labelled so in the log: a script
activated inside the window keeps its live state there by design, so those
lines are read apart from the live records'.  `--savestate-diff A B --ignore-ov`
leaves the overlap tree out of the comparison (two runs that inserted a body at
different moments keep different tree layouts for good; the question is what
else differs), `--describe-script NAME` prints a root script's graph - blocks
with prototype, id and flags, links with their activation delay, every
parameter with its value - the way to read what a script mismatch names, and
the headless client's `--trace-restore` logs the PreSimulate callback queue
(class, behavior, script, target, body or not) before and after every forced
rewind.

### `BallanceMMOMessageTests` (rollback_engine_test.cpp)

The engine's savestate path on a mock world: a record with a blob restores
through `set_savestate` BEFORE any body write, the per-body / contact /
clock writes are skipped for the bodies it covers, snapshot rows are still
written on top, a record without a blob falls back to the old path, and
`set_exact_restore(false)` disables it.  Blob format round trip and
`diff_savestate` on synthetic records.

### End to end (headless client, `scripts/savestate_check.py`)

- Solo run with `--force-rollback every:depth` for several (every, depth)
  pairs against a control without: the journals must be bit-exact
  (`scripts/journal_trace.py` / `worst_bodies.py` worst = 0.0 on every
  body), where today the prop ball is 1.4-7.5e-02 m off per rewind.
- Four clients, lane drive, snapshots off, 0 and 150 ms: every ball's
  drift from the server (`server_client_drift.py`) at the untouched balls'
  floor, through the start pile.
- The whole matrix on x64, x86 (`build-sim-x86`) and Linux (WSL): PASS
  everywhere, and the straight-pass hashes equal across builds as before.

## Verification

1. Solo prop probe, same batch control (`scratchpad/runs_trace.sh`,
   `cross.sh` label 0 ms, `--force-rollback 870:12 --trace-body P_Ball_Paper_MF
   --trace-ticks 856 880 --trace-restore`): with the savestate the arm must be
   exact against the control on every body (`scripts/worst_bodies.py`
   ticks 830-1300), where today the prop ball is 1.4-7.5e-02 m off.
2. Forced rollbacks every N ticks over a whole solo run: journal bit-exact
   against a run without them.
3. Four players, lane drive, snapshots off (`arch_server_snap.sh wsl 26700
   100000 100000`), 0 and 150 ms, spawn spacing 4.2: host and touched balls
   against the server with `scripts/server_client_drift.py` - the current
   floor is 0.3-5 m through the start pile; the target is the untouched
   balls' 0.000-0.002 m on every ball.
4. The retail client on the arena (compiles with the same engine).
