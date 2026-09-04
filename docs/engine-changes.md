# Engine changes (Ballanced submodule)

Every change to the engine sources under `submodule/Ballanced` is listed here
with the reason and the evidence. The rule for the collision overhaul is: no
engine change unless proven necessary, and every change recorded.

## 1. Platform-independent vector-FPU block size (physics_RT / IVP)

File: `Source/BuildingBlocks/physics_RT/ivp/ivp_physics/ivp_great_matrix.hxx`

`IVP_VECFPU_SIZE` selected the block size of the matrix row helpers
(`IVP_VecFPU::fpu_large_dot_product` and friends, used by the friction
system's linear solver). Under `PLATFORM_64BITS` it was 4 ("taken from
vphysics retail"), otherwise 1. With 4, the dot product accumulates four
partial sums and combines them at the end; with 1 it accumulates
sequentially. Both are valid arithmetic, but they round differently, so a
64-bit server disagreed with the 32-bit game whenever a body had several
contact points (boxes resting on the floor).

Evidence (2026-09-02, Level 1 recording replayed headlessly): x86 and x64
builds with identical compiler flags, identical libm and pattern-filled
allocations diverged at the first frame in which `P_Box_MF002` was solved
through the friction system; IVP's own debug trace showed identical inputs
and a different velocity after the solve. Removing the 64-bit branch makes
every platform use the scalar path the original 32-bit physics_RT ran.

The alignment mask is written as `~((uintp)7)` so it is correct for 64-bit
addresses (the old 32-bit literal would have truncated them).

## 2. Deterministic quicksort for engine arrays (VxMath) and qhull

Files: `Source/VxMath/include/XDeterministicSort.h` (new),
`Source/VxMath/include/XArray.h`, `Source/VxMath/include/XSArray.h`
(`Sort()` calls `XDeterministicQSort` instead of `qsort`).

`XArray::Sort` / `XSArray::Sort` sorted with the C library's `qsort()`. The
level builder sorts the physics-object table `PH` by sector ("Sort Rows" in
`Levelinit_build`), and dozens of rows share a sector, so the order of rows
with equal keys is whatever the C library's algorithm leaves. The Microsoft
runtime (the retail game) uses a median-of-three quicksort; glibc a
mergesort/introsort; bionic and musl their own. The objects were therefore
physicalized in a different order on Linux, and the whole level (script
timing, body order, collision order) diverged from the Windows client at
the first physics frame.

`XDeterministicQSort` reimplements the Microsoft runtime's algorithm
(median-of-three, cutoff 8 with selection sort, explicit stack, smaller
partition first) so every platform produces the Microsoft permutation.
It is verified by `scripts/test_det_qsort.cpp`: 300000 random arrays with
heavy key duplication, identical permutation to the MSVC `qsort` on x64 and
x86 (2026-09-02). On Windows the replay result is unchanged (2345/2345
frames); on Linux the divergence at frame 6 was traced to this sort by
diffing the per-tick block execution trace of both engines
(`--debug-ticks`): the first differing block was the type switch inside the
`PH` iteration at tick 456.

qhull (inside `ivp_compact_builder`) also calls `qsort()` on merge sets
sorted by angle and on neighbour sets, where ties are common (a box has four
equal merge angles). Its sources are not modified: the BallanceMMO build
force-includes `BallanceMMOCommon/include/physics/deterministic_qsort_shim.h`
into that target (`bmmo_apply_deterministic_sort` in
`cmake/PhysicsFloatingPoint.cmake`), which maps `qsort` to
`XDeterministicQSort` for both the client plugin and the headless server.

## 3. Explicit transcendental math entry points (physics_RT / IVP)

Files: `Source/BuildingBlocks/physics_RT/ivp/ivp_utility/ivu_libm.hxx` (new);
call sites in `ivu_linear.hxx` (the `IVP_Inline_Math` wrappers),
`ivu_linear_macros.hxx`, `ivu_quat.cxx`, `ivp_car_system.cxx`,
`ivp_calc_next_psi_solver.cxx`, `ivp_environment.cxx`, and geompack
(`geompack_cutfac.cxx`, `geompack_dsphdc.cxx`, `geompack_resedg.cxx`).

Every C library computes sin, cos, tan, asin, acos, atan, atan2, exp, log
and pow with its own polynomials and tables, so the same IVP source gives
different last bits on MSVC, glibc and bionic. The physics now calls these
functions only through `ivp_libm::` wrappers. By default they forward to
the C library (upstream behaviour). When the build defines
`IVP_PORTABLE_LIBM`, the wrappers call `ivp_libm_*` symbols that the build
must provide; BallanceMMO provides them from the vendored OpenLibm subset
(`BallanceMMOCommon/src/physics/ivp_libm_portable.cpp`). sqrt, fabs, floor,
ceil and fmod are exact operations and stay on the C library.

The double-named wrappers take double even for float arguments and the
float-named ones take float, which is the mapping the earlier force-included
rename shim used, so the results are unchanged: after the change all four
platforms still replay the recording bit-exact (2026-09-02). This replaces
the build-level shim (option A) with an explicit contract in the engine
(option B), so an engine built without BallanceMMO's CMake is not silently
non-deterministic: the entry points are visible in the source, and a plain
build simply uses the C library.

The 44 rerouted call sites were found with a word-boundary regex over the
IVP sources (qhull has no transcendental calls; havana and 3dsimport are not
compiled). The grep in `scripts/check_ivp_libm.sh` verifies that no raw
call remains.

## 4. String parameter size in the Player hotfix helper (Player)

File: `Source/Player/src/ScriptUtils.h`, `GenerateInputParameter<const char*>`.

The helper created a local string parameter for a boot-script hotfix
(`PatchReplacePathRoot` passes the composition directory to `TT_ReplacePath`)
and called `CKParameter::SetValue(value)` without a size. `SetValue` then
copies the parameter's current buffer size, 64 bytes for a fresh string
parameter, out of a C string that is much shorter, reading past its end.
AddressSanitizer reported the over-read while validating the headless engine
(recorded below as benign); in the BallanceMMO server process the source
string ended near a page boundary and the read faulted (access violation in
`memcpy`, 2026-09-02, first physics-session world boot). The call now passes
`strlen(value) + 1`, which is how the retail building blocks size string
parameters (for example `PhysicalizeCallBack` in physics_RT). No gameplay
effect: the parameter held the same characters before, followed by garbage
that the string consumer never read.

## 5. Per-simulation-unit movement check (physics_RT / IVP)

Files: `Source/BuildingBlocks/physics_RT/ivp/ivp_intern/ivp_sim_unit.hxx`
(new member `next_movement_check`, inline `must_perform_movement_check`),
`ivp_sim_unit.cxx` (constructor initialisation; the two calls in
`simulate_single_sim_unit_psi` use the unit's counter).

`IVP_Simulation_Unit::simulate_single_sim_unit_psi` decides whether to run
the "movement check" (the test that puts a calm unit to sleep) by calling
`IVP_Environment::must_perform_movement_check()`: one counter per
environment, decremented once per simulated unit per PSI, re-armed with
`ivp_rand()` to 15..19 when it reaches zero. So the PSI at which any given
unit is examined depended on how many *other* units were awake since the
level started, and on the global RNG cursor those re-arms consumed. Two
worlds that are bit-identical for one ball but differ in unrelated bodies
therefore freeze that ball a few PSIs apart, and a frozen body stops at a
slightly different pose.

Evidence (2026-09-02, networked physics session, retail client vs headless
server): after the player's ball died, the client's retail sector reset
re-physicalized the eight sector-1 mechanisms while the server (which does
not reset shared mechanisms on a personal death) only woke its resting
copies; from that tick the `ivp_srand` cursors of the two worlds diverged
(logged every change on both sides) and the ball, still driven by identical
inputs, needed a ~1 cm blend every ~50 ticks afterwards. Before the death
the two worlds had been identical for 2000 ticks including driving.

Now every unit owns its countdown: first check after 10 PSIs (the retail
environment also started at 10), then every 17 PSIs (inside the retail
15..19 range), no RNG. `ivp_rand()` is no longer used by the simulation.
Merged and split units start a fresh countdown like any new unit, which is
deterministic because merging follows contacts. Gameplay difference to
retail: a body may fall asleep up to two PSIs (30 ms) earlier or later.

## 6. Body guard for the Unphysicalize block (physics_RT)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.h` / `.cpp` (new
members `m_KeepLevelBodies`, `m_KeepLevelBodiesExcept`, appended after the
retail layout and zeroed in the constructor), `Behaviors/Physicalize.cpp`
(Unphysicalize branch and the "already physicalized" early return),
`Behaviors/SetPhysicsGlobals.cpp` (diagnostic print of the time factor
changes when `BMMO_TRACE_TIMEFACTOR` is set).

The retail death sequence resets the current sector: `Gameplay_SectorManager`
runs the deactivation pass (Unphysicalize of every mechanism of the sector),
restores the objects' initial matrices and physicalizes them again. In a
networked physics session the server keeps the shared mechanisms (a personal
death does not reset them, design section 2), so the client must not delete
and recreate its bodies either: a recreated body starts from the initial
pose with fresh contact state, and every snapshot after the death mismatched
by about 1.5 m at first and by 1-10 mm for a second afterwards (the freshly
created bodies settle on the floor differently from the server's resting
ones), a rollback per snapshot.

Now the bridge sets `m_KeepLevelBodies` for the duration of a session with
`m_KeepLevelBodiesExcept` = the player's ball entity: the Unphysicalize input
returns without deleting any other body, and the Physicalize input, which
already returns early for a physicalized entity, additionally writes the
body pose back to the entity so the script's matrix reset does not show. The
mechanisms therefore keep the state the server has. Measured (2026-09-02,
retail client vs headless server, Level 1, three deaths): 2040 snapshots
compared, 0 mismatches, before the change 40 mismatches per run.

Gameplay difference to retail: within a physics session a death does not
reset the sector's mechanisms; outside a session the guard is off and the
block behaves exactly as before.

## 7. Deterministic ball-ball synapse order (physics_RT / IVP)

File: `Source/BuildingBlocks/physics_RT/ivp/ivp_physics/ivp_mindist.cxx`
(`IVP_Mindist::init_mindist`, new helper `ivp_ball_pair_order`).

When both objects of a minimal-distance pair are balls, `init_mindist`
decided which of them becomes synapse 0 by comparing their `client_data`
pointers - the addresses of the `CK3dEntity` objects. That order fixes the
sign of the contact plane and the roles of the two cores in the impact and
friction solve, so two processes simulating the same pair (the game and its
authoritative server, whose heaps differ) could diverge from the first
ball-ball contact on. Retail Ballance never exercises the branch: the player
ball is the only `IVP_Ball` in a level, every other body is a polygon. The
collision overhaul makes ball-ball contact routine (player balls collide with
each other, and design 9.10 spawns them at the same point), so the order now
comes from keys both processes give the same objects: the nocoll group ident
first (a player's ball carries `P#<join order>` on every peer), the object
name as the tie-break.

Evidence (2026-09-03): with the pointer comparison restored and both criteria
traced, three headless builds disagree about the very first pairs of a
`--spawn-test` run (Level 2, three wood balls spawning at the same point,
kicked apart at 3 m/s). The mindists are created in the tick the balls
appear, so the disagreement is immediate:

| pair | x64 | x86 | Linux |
| --- | --- | --- | --- |
| player 3 / player 2 | 1 | 0 | 0 |
| player 3 / player 1 | 1 | 1 | 0 |
| player 2 / player 1 | 1 | 1 | 0 |

The per-tick world hashes then diverge: x64 against x86 at tick 30, x64
against Linux and x86 against Linux at tick 20. With the ordering of this
change the same command gives identical traces on x64 and Linux over the
whole run, and the `--explode` traces match on x86 as well. The processes
compared here are three headless builds; the pair that actually matters is
the game against the server, whose allocator states differ at least as much.
Single-player replays are unaffected because the branch is never reached
(`rec_m3b.bmrc` still 4169/4169).

## 8. Quaternion of a physicalized entity computed in the plugin (physics_RT)

File: `Source/BuildingBlocks/physics_RT/CKIpionManager.cpp`
(`FillTemplateInfo`, new `CKIpionManager::PhysicsQuaternionFromMatrix`).

`FillTemplateInfo` turned the entity's world matrix into the IVP quaternion
with `VxQuaternion::FromMatrix`, a function of the host's VxMath: the game's
`VxMath.dll` on the client, the reimplementation on the headless server. The
two agree on axis-aligned matrices (every body of the M1 replays) but differ
in the last bit for general rotations, so a rotated entity physicalized on
both peers started from different quaternions. Found with the trafo
explosion pieces (design 9.10): with everything else made exact, one paper
piece and two wood pieces - the ones with general rotations - still differed
by a float ulp in the first frame. The conversion is now a copy of the
reimplementation's algorithm compiled into the plugin (`sqrtf` is exact
everywhere), so both peers compute it alike; the headless result is
unchanged, so the M1 replays still match (`rec_m3b.bmrc` 4169/4169 after the
change). With it the paper explosion replays bit-exact between the game and
the headless engine (3456/3456 frames); two of the sixteen wood pieces still
differ in the last bit from their first frame on, a residue that is not
present between the x86, x64 and Linux headless builds (identical) and has
not been located yet.

## 9. Forgetting one named collision surface (physics_RT)

File: `Source/BuildingBlocks/physics_RT/CKIpionManager.cpp` / `.h`
(new `CKIpionManager::RemoveCollisionSurface`).

`CreatePhysicsObjectOnParameters` compiles a convex hull the first time an
entity is physicalized with a given Collision Surface name and caches it under
that name; every later Physicalize reuses it. The manager could add to that
cache and clear it whole, but not forget a single entry.

The hull is built from the entity's scale, which `CK3dEntity::GetScale`
derives from the *local* matrix, which CK derives from a product with the
parent's inverse. The trafo explosion pieces hang under `Ball_WoodPieces_Frame`
below `Balls_MF`, which the level file gives a 0.99999 scale, so that product
is inexact and the game's CK2 rounds it differently from the reimplementation:
three of the 51 pieces get a scale that differs by one ulp (`piece08`'s z is
`0x1.0000620p+0` in the game and `0x1.0000640p+0` headless). The scale
multiplies every mesh vertex, so the hull and its rotation inertia differ, and
those pieces spin differently from their first simulated frame. Since the
hulls are compiled at game start (`Balls_Init` physicalizes every piece once),
flattening the hierarchy later cannot help - the cached hull is already wrong.

With this entry point, `restore_explosion_pieces` (design 9.10) drops each
piece's cached hull while the piece has no body, and the next Physicalize
compiles it from the flattened hierarchy, which both engines derive
identically. All three explosions then replay bit-exact between the game and
the headless engine (wood 3457/3457, stone 3457/3457, paper 3456/3456), and
the `--explode` traces stay identical across the x64, x86 and Linux headless
builds. Nothing else calls it, so no other body's hull changes;
`rec_m3b.bmrc` still replays 4169/4169.

## 10. Geometry the physics consumes is derived in the plugin (physics_RT)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.cpp` / `.h` (new
`PhysicsScaleFromMatrix`, `PhysicsAxisFromMatrix`, used by
`CreatePhysicsObjectOnParameters` and `UpdateObjectWorldMatrix`),
`Source/BuildingBlocks/physics_RT/Behaviors/PhysicsHinge.cpp`.

`CK3dEntity::GetScale` and `CK3dEntity::GetOrientation` do not return stored
data: they take the length of a world- or local-matrix row, which means a
square root, and `GetOrientation` divides by it. A level's matrix rows are
never exactly unit length, so the game's `VxMath.dll` and a reimplementation
differ in the last bit. What the physics does with those numbers is not
cosmetic: the scale multiplies every mesh vertex before a collision hull is
compiled, and the orientation *is* the axis of a `Set Physics Hinge`
constraint, so a hinged mechanism swings differently from its first frame.

Evidence (2026-09-03): Level 8's tilting board `P_Modul_30_Wippe`, a hinged
body that simulates from level start, diverged between the game and the
headless replay at frame 64, in the last bit of one velocity component, with
its position still identical; Level 9 diverged the same way at frame 534.
Both are gone with this change. The three quantities are now computed here,
with the reimplementation's own algorithm, so a headless result is unchanged:
`rec_m3b.bmrc` still replays 4169/4169, and the explosion and spawn traces of
design 9.10 are byte-identical before and after.

## Notes on things that were verified *not* to need engine changes

- Floating-point flags: `/fp:precise` (MSVC) and `-ffp-contract=off
  -fexcess-precision=standard` (GCC/Clang) give bit-identical arithmetic
  across x86, x64 and ARM64 (probe in the harness); `/fp:strict` on x86
  crashed the collision code and changed nothing.
- Transcendental functions: every C runtime differs (x86 vs x64 UCRT on
  ~5% of inputs, Android bionic on far more). First handled by a
  force-included rename header (no engine change); now handled by change #3
  above, which makes the entry points explicit in the engine.
- Heap addresses: perturbing allocation between body creations does not
  change results; IVP's containers iterate in insertion order.
- The event scheduler's tie-breaking (`SORT_MINDIST_ELEMENTS`) is already
  deterministic in the upstream sources.
- The layout-dependent failures seen while validating (some builds failed to
  unphysicalize the boxes at the level reset, teardown crashes in
  `RCKMesh::DeleteRenderGroup`, an abort on Android) were not engine bugs:
  AddressSanitizer traced them to BallanceMMO's null sound manager, which
  treated the `SoundMinion` wrapper that `CKWaveSound::PlayMinion` passes to
  `Play(nullptr, minion)` as its own source object and wrote past the
  64-byte wrapper. Fixed in `BallanceMMOServer/sim/null_managers.cpp`.
- AddressSanitizer also reported two engine-side issues that do not affect
  determinism: `CKFileObject::CleanData` releases a `new`-allocated
  `CKStateChunk` through `CKDeletePointer` (`delete[]`), left untouched, and
  the Player hotfix string over-read, since fixed as change #4 above after it
  crashed the server.
- The deterministic "Random" block behind the trafo explosion pieces and the
  spawn impulse (design section 9.10) needed no engine change either: the
  function of the Random block instances inside the three explosion scripts
  is replaced from BallanceMMO code at run time (`CKBehavior::SetFunction`
  through `install_random_block`, both the headless engine and the client
  mod), a mechanism CK already exposes for any behavior; the Logics sources
  are untouched.
