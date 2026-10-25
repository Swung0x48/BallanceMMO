# Engine changes (Ballanced submodule)

Every change to the engine sources under `submodule/Ballanced` is listed here
with the reason and the evidence. The rule for the collision overhaul is: no
engine change unless proven necessary, and every change recorded.

**One macro for the IVP changes.** Every BallanceMMO change inside the IVP
sources - the scalar vector-FPU path (1), the portable transcendental entry
points (3), the per-unit movement check (5), the ball-ball synapse order (7),
the merged compact-surface layout (12), the overlap-grid rounding (16) and the
savestate's access declarations (17) -
compiles only when the build defines `IVP_PORTABLE_BMMO`. Without it each file is the upstream code, platform
branches included. BallanceMMO's CMake defines the macro on every IVP target
it builds, the headless server's and the client plugin's alike
(`bmmo_apply_portable_math` in `cmake/PortableMath.cmake`, with
`BMMO_PHYSICS_PORTABLE_MATH`, which every BallanceMMO build turns on). The
physics_RT and CK2 changes are not behind it; they live in the plugin and the
engine, not in IVP.

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

Compiles only under `IVP_PORTABLE_BMMO` (see the note at the top); without
it the header keeps upstream's 64-bit branch.

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
`IVP_PORTABLE_BMMO`, the wrappers call `ivp_libm_*` symbols that the build
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
changes when `BMMO_TRACE_TIMEFACTOR` is set; during a physics session the
client rewrites the `Physic Time Factor` input of the `Set Physics Globals`
block in the `Pause Level`/`Unpause Level` chain to the factor currently in
use, so the pause chain's write is a no-op and the diagnostic prints the
factor in use; the level scripts stay in charge of the clock. A session also
empties the `Script` targets of those two chains' `Deactivate Script` /
`Activate Script` blocks for `Gameplay_Ingame` and `Gameplay_Events`, so the
menu keeps the world running instead of stopping it - see the pause section of
`docs/collision-overhaul-design.md`. The clock guard, bridge API v8, is then
only a sampler: the script its pause sensor watches is never deactivated in a
session, so it never holds the factor, and the menu-closing pass it used to
cover on its own - the pass in which shared scripts such as `Gameplay_Refresh`
write 2.0 - is no longer singled out).

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

## 11. Float accumulation order matched to the game's VxMath (VxMath)

Files: `Source/VxMath/include/VxMatrix.inl` (`Vx3DMultiplyMatrix`,
`Vx3DMultiplyMatrix4` and the SSE kernels `VxSIMDMultiplyMatrixM`,
`VxSIMDMultiplyMatrixM4`), `Source/VxMath/include/VxSIMD.inl`
(`VxSIMDMatrixMultiplyVector3`, `VxSIMDMatrixRotateVector3`).

`CK3dEntity` is implemented in `CK2_3D.dll`, not in `CK2.dll`, and it derives
a child's local matrix as parent-inverse times child-world through
`Vx3DMultiplyMatrix` — a call that lands in the game's `VxMath.dll`. Float
addition is not associative, and the two implementations grouped the four
products differently: the reimplementation summed them left to right, the
shipped DLL sums them pairwise, `(p0+p1)+(p2+p3)`. One or two ulp per element,
which is exactly the size of the mechanism drift that had resisted every
earlier explanation.

Determined by measurement rather than by reading. The harness (`scripts/vxmath_diff`) loads the shipped `VxMath.dll` with `LoadLibrary` so its symbols
never collide with the reimplementation's identically named inline versions,
sets the x87 control word to the 24-bit precision the client actually runs at
(`000a001f`), checks that both sides agree on storage and operand order, and
then exhausts every ordering of the four products against the real DLL.
Exactly one grouping survives 60000 random matrix pairs. The same method
showed the two vector transforms accumulate left to right, which the scalar
reimplementation already did but its SSE kernels did not. Those kernels are
now written with explicit `_mm_mul_ps` / `_mm_add_ps` instead of the
`VX_FMADD_PS` macro, so an FMA-enabled build cannot fuse a rounding step away
and drift from both the shipped DLL and the scalar path.

Evidence (2026-09-03), the 13-level mechanism sweep of design 9.11. Before:
levels 3, 7, 8, 9, 10, 11 and 13 diverged from the client recording. After:
3, 7, 8, 9 and 13 match for their whole run, and every case that already
matched still does — levels 1, 2, 4, 5, 6, 12, the `rec_m3b` gameplay replay
and all three explosions. Levels 10 and 11 still report a hash mismatch, but
that turned out not to be a physics difference at all (see below). Windows
x64, Windows x86, Linux x86_64 and Android arm64 produce identical engine
state on all 17 cases both before and after. After the change the four
functions are bit-identical to the shipped DLL over 200000 inputs of both
random and Ballance-like shape.

What levels 10 and 11 really are: the world hash folds each movable core in
the order `CKIpionManager::m_MovableObjects` happens to hold them, and the two
engines hold two of the bodies the other way round. Every body is
bit-identical. On level 11 the recording's own hash is reproduced exactly by
folding its own core states in the order `P_Modul_26_Rope`, `_Sack`,
`_Rope001`, `_Sack001`, `Ball_Wood`, `P_Modul_08_Schaukel`, `Schaukel001`,
while the reimplementation folds the last two the other way; 809 frames later
all seven bodies still match to the last bit. On level 10 only frames 7 to 27
differ, the window in which `P_Modul_01_Pusher`, `_Rinne` and `_Filler` are
awake, and all nine bodies are identical throughout it; the remaining 4204
frames match. Proven offline by rebuilding the FNV-1a fold from the exact hex
floats the recording stores and searching the permutations
(`scripts/order_solve.py`), so it needs no client run.

That list is appended to from IVP's `event_object_revived` and is only walked
to write entity matrices back, so its order does not reach the solver — which
is consistent with the bodies never actually parting. The physics of all
thirteen levels therefore replays bit-exactly; what is left is that the
diagnostic hash answers a slightly stronger question than "is the state the
same". Fixing that means either folding in a canonical order, which would
invalidate every existing recording, or teaching the replay tool to recognise
the case; neither is done yet.

Note the direction of this change. The client keeps its retail `VxMath.dll`;
the physics plugin links the Virtools SDK import library, so nothing shipped
to players changed. Only the reimplementation moved, to match the binary the
game already runs.

Measured and still different, so far unreconciled: `Vx3DInverseMatrix`,
`VxVector::Normalize`, `VxQuaternion::FromMatrix`, `Vx3DDecomposeMatrix`,
`Vx3DInterpolateMatrix` and `Vx3DMatrixFromRotation`. Thirty-six plausible
formulations of the inverse were tried against the shipped DLL and none
reproduced it, so that one uses a different algorithm, not merely a different
rounding order. `Vx3DMatrixFromRotation` is not a rounding difference at all:
the shipped function returns the transpose of the reimplementation's result
and its sine and cosine carry only about four correct digits, so anything
that comes to depend on it needs its own investigation first.

## 12. One compact-surface layout on every platform (physics_RT / IVP)

File: `Source/BuildingBlocks/physics_RT/ivp/ivp_compact_builder/ivp_template_surbuild.hxx`

`IVP_Template_Surbuild_LedgeSoup`'s default for `merge_points` was
`IVP_SLMP_MERGE_AND_REALLOCATE` under `WIN32` and `IVP_SLMP_NO_MERGE`
everywhere else. Merging folds the hull's duplicate vertices into one shared
point array at the end of the compact surface and points every ledge at it;
without it each ledge carries its own copy. The solid is the same either way -
`mass_center`, `rotation_inertia` and `upper_limit_radius` come out
bit-identical, and so does everything the simulation computes from the hull -
but the blob it lives in has a different size and different bytes, and the two
forms do not answer pointer identity of a shared vertex the same way.

That mattered because the session handshake compares a signature of exactly
those bytes (`bmmo::physics::surface_signature`, `session_ready_msg`'s
`anchor_surfaces`): every Windows client refused every session on a Linux (or
Android) server with "world mismatch", while the world hash the same message
carries agreed. Level 2 anchored at `0ce8aeb84f5f017c` on the client and
`66ad16dd3d381480` on the server; per body the counts and the header floats
matched and only the storage differed (`A01_Floor_00`: 579 nodes, 290 ledges,
580 triangles on both; 32916 bytes merged against 44100 unmerged).

The game runs the merging one, so every platform now does. The change is
inert for the simulation: on Linux, three levels free-run for 1200-1500 ticks,
the three trafo explosions, the spawn-impulse test and a mechanism drop all
produce byte-identical output before and after, and the Level 1 client
recording replays to the same frame with the same hashes. After it, those same
cases - including their surface signatures - are byte-identical between the
Linux x64 and the Windows x86 headless builds, and a retail client starts a
physics session on a Linux server.

## 13. The body guard's exemption list (physics_RT)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.h` (new member
`m_KeepLevelBodiesFree` and the `KeepsBodyOf` predicate, appended after the
retail layout like change #6's own members), `Behaviors/Physicalize.cpp` (both
guard sites ask the predicate now).

Change #6's guard is a blunt "during a session nothing but the player's ball
may be unphysicalized", and it caught one set of bodies it was never meant to:
the trafo explosion pieces. They are the only bodies the retail scripts create
*and destroy again* inside a session - `Ball_Explosion_<type>` physicalizes the
51 pieces, `Ball_ResetPieces_<type>` fades them for two seconds and gives them
back - and the server has none of them, so nothing about them is shared state
the guard has to protect. With the guard on, their De Physicalize was
swallowed, the pieces stayed physical for the rest of the session, and the next
explosion hit the "already physicalized" early return of the Physicalize
block, which under the guard writes the *body's* pose back to the entity: every
trafo after the first showed its pieces lying where the first trafo's pieces had
come to rest (and left an invisible pile of bodies at that trafo for balls to
hit).

The guard now carries a list of entities it never covers.
`bmmo::physics::restore_explosion_pieces` fills it with the three
`Ball_*Pieces_Frame` hierarchies (54 entities) - it walks them anyway, and it
runs at every session anchor and at the start of every explosion, so the list
is always in place before a piece can be physicalized; `set_body_guard(false)`
clears it. The engine keeps knowing nothing about Ballance's object names.

Reproduced and verified headlessly (`BallanceMMOSimTool --level 2 --explode
wood 300 --activate Ball_ResetPieces_Wood 400 --beam 150 10 -141 560 --explode
wood 600 --list-bodies-at 610`, with and without `--body-guard Ball_Wood 250`):
before the change the guarded run's second explosion left every piece at the
first explosion's landing spot while the unguarded one put them at the ball;
after it, all 20 ball bodies and all 215 pose hashes of the two explosions are
identical between the guarded and the unguarded run. The single-explosion
`--explode` output, the `rec_m3b` replay (4169/4169) and `--spawn-test 3` are
unchanged.

## 14. Script wake-ups told apart from IVP revivals (physics_RT)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.h` / `.cpp` (new
`m_ScriptWakeupObserver` member appended after the retail layout like changes
#6 and #13, plus `WakeUpFromScript` and `NotifyScriptWakeup`),
`Behaviors/Physicalize.cpp`, `Behaviors/PhysicsImpulse.cpp` and
`Behaviors/PhysicsWakeUp.cpp` (the three places a script explicitly asks a body
to wake), and `physics_RT/tests/test_physics_regressions.cpp`.

IVP reports every wake through one global listener. That includes script
requests, but also collisions, snapshot restoration and re-simulation after a
rollback, and the client mod cannot tell those apart. It sent all of them to
the server as `BodyRevived`, and the server called `ensure_in_simulation()` on
each one; IVP resets the resting detection even for an already-awake body, so a
client's predicted collision or rollback restore could wake the authoritative
body again. The production journals show the resulting flood: 101,025
`BodyRevived` events in an 18-minute Level 8 session, peaking at 827 per second.

The engine now records only explicit script intent. `WakeUpFromScript` wakes
the body and then calls `NotifyScriptWakeup`, which filters on the observer
being installed, the object belonging to this manager's environment, and the
body being movable. `Physicalize` reports a successful, non-fixed, non-frozen
creation; `PhysicsImpulse` and `PhysicsWakeUp` call `WakeUpFromScript` instead
of `ensure_in_simulation()` directly. The observer is null by default, so an
ordinary physics_RT user (the retail game) is unaffected. BallanceMMO's bridge
installs a callback that appends `script_wakeup <name>;` to its event log next
to the generic `revived` diagnostics, and the client physics API was version 7
at this change. Version 8 later added the session clock guard (design doc
section 9.2): a persistent PreSimulate callback that samples
`m_PhysicsTimeFactor` on every pass, so the level scripts drive the clock
exactly as they do on the server, but pins the factor to the value the run had
before the retail pause menu opened while `pause_behavior_id`
(`Gameplay_Ingame`) is inactive - and for the single pass in which the menu
closes. The pause chain's own write is neutralized on the client by rewriting
that block's `Physic Time Factor` input to the factor in use (no engine
change), so the guard only has to cover the menu-closing pass (9.18). In a
session the guard's pause sensor is now effectively unreachable: the client
also empties the pause chains' `Deactivate Script` / `Activate Script` targets
for `Gameplay_Ingame` and `Gameplay_Events` (see the design doc's pause
section), so the script the sensor watches is never deactivated and the guard
neither pins nor writes anything - it is left in place because the mod's own
frame hook runs after the physics step and could not see a pause in time if
some other actor ever moved that script again. That one is bridge-only and
needed no engine change.

Evidence: the new headless regression test "Script wakeups are distinct from
simulation revival" plus the four existing ones all pass. It creates a sleeping
body and checks that manager-side creation, a direct `ensure_in_simulation()`,
a beam restore and a re-simulation produce ordinary `revived` events but no
script intent, that `WakeUpFromScript` reports its target (also when the body is
already awake), and that a fixed body reports nothing. The production Level 8
journal still replays tick-identical.

## 15. Creation observers (physics_RT, diagnostic only)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.h` / `.cpp` (two
observer members appended after `m_ScriptWakeupObserver`, plus the
`m_ProcessingQueuedCallbacks` counter), `PhysicsCallback.cpp`
(`PhysicsCallbackContainer::Process()` raises the counter while it retries the
queued callbacks), `Behaviors/PhysicsBallJoint.cpp` and
`Behaviors/PhysicsForce.cpp` (one call each, after the constraint / force
controller is built).

Nothing in the simulation changes: the observers are null by default and the
retail game never sets them. BallanceMMO's bridge installs recorders that append
`constraint <reference>|<attached>|<immediate|deferred>|cw=..|mxcsr=..;` and
`force <name>|f=..|p=..|<immediate|deferred>;` to its event log, next to the
`birth` record the bridge's own object listener now writes for every body
(pose, speeds, mass, inertia, damping, clock fields, compact-surface hash, FPU
words) and the optional `psi` probe (`BMMO_PSI_PROBE=<name prefix>`: the first
six PSIs of every body born with that prefix, with its sim unit's core and
controller order). "Deferred" means the block's callback returned 0 when the
block ran (a body was missing) and the pre-simulate pass built the object
later.

Why: the Level 11 sandbag divergence of design 9.27 was invisible to every
state comparison - bodies, constraints, hulls and FPU state were bit-identical
at birth on the two sides that then diverged in the first PSI. The force
observer showed the two sides had built opposite `SetPhysicsForce` actuators,
which led to the script-side cause (a Sequencer counter that survives the level
restart). The hooks stay because that class of question ("what did the scripts
build, and when") comes back with every mechanism.

## 16. One rounding when an object is filed in the overlap grid (physics_RT / IVP)

File: `Source/BuildingBlocks/physics_RT/ivp/ivp_collision/ivp_clustering_longrange.cxx`
(`ivp_int_floor`, `ivp_int_ceil`).

`IVP_OV_Tree_Manager::calc_optimal_box` files every collision-enabled object
in a grid of power-of-two boxes, and it computed the box's integer edges with
`floorf`/`ceilf` on Windows and `floor`/`ceil` under `LINUX` (and on the
PowerPC and GEKKO ports). The argument is a double; the float variant rounds
it to float first, so the two disagree whenever that double lies within half a
float ulp of an integer - a box edge landing on a grid line. The box an object
is filed under decides which node the overlap tree walks first when the
object's collision partners are gathered (`collect_collision_partners` walks a
node's elements, then its children), so two partners at the same distance come
out in one order on Linux and the other on Windows, and with them the order
their contacts are created and solved in.

Found by the server journal's replay check. Four players beamed onto the
mechanism row in one tick (balls recreated at x = 342, 345, 348 and 351, the
one at 345 touching two neighbours at the same distance) replayed bit for bit
on Linux and diverged on Windows from the first tick the balls made contact,
`matched=704 first_divergence=716`, every checkpoint agreeing to 1e-6 m, the
balls' quaternions apart by 1e-9. Instrumented on both sides down to the
cause: identical state and identical events up to the revive of the fourth
ball, then `grow_friction_system` creating its two contacts in opposite
orders, because the ball's collision partners had been gathered in opposite
orders. Compiling the Linux replay tool with the Windows variant reproduced
the Windows result on Linux exactly - the same hash `d6249958608cb54e` at tick
716, the same partner order - which is the proof this change rests on. It only
shows when two or more bodies are created in one tick at symmetric spacing
(join order 0132 in the harness put the fourth ball between two others; 0123
chained them and never failed), so a session of players respawning together
at one checkpoint is the case that matters in play.

The Windows retail game is the reference, so the float variant is the one
every platform keeps. Inert for the simulation on Windows (its own build was
already this); on Linux it changes only ties of this kind.

## 17. Access declarations for the savestate (physics_RT / IVP, header only)

Files, all under `Source/BuildingBlocks/physics_RT/ivp/`:
`ivp_physics/ivp_mindist.hxx` (`IVP_Synapse_OO`, `IVP_OO_Watcher`,
`IVP_Synapse_Real`, `IVP_Mindist_Base`, `IVP_Mindist`),
`ivp_intern/ivp_friction.hxx` (`IVP_Synapse_Friction`,
`IVP_Contact_Point_Fast_Static`, `IVP_Contact_Point_Fast`, `IVP_Contact_Point`,
`IVP_Friction_Core_Pair`, `IVP_Friction_System`),
`ivp_physics/ivp_listener_hull.hxx` (`IVP_Listener_Hull`),
`ivp_physics/ivp_real_object.hxx` (`IVP_Hull_Manager_Base`, `IVP_Real_Object`),
`ivp_utility/ivu_min_list.hxx` (`IVP_U_Min_List`),
`ivp_physics/ivp_environment.hxx` (`IVP_Environment`),
`ivp_intern/ivp_sim_unit.hxx` (`IVP_Simulation_Unit`),
`ivp_intern/ivp_clustering_longrange.hxx` (`IVP_OV_Node`,
`IVP_OV_Tree_Manager`) and `ivp_intern/ivp_cache_object.hxx`
(`IVP_Cache_Object`, `IVP_Cache_Object_Manager`).

Every one of them gains the same line inside the class, under
`IVP_PORTABLE_BMMO`: `friend class IVP_BMMO_State;`. That class is not part of
the engine. It is declared by the bridge
(`BallanceMMOCommon/src/physics/physics_savestate_internal.hpp`) as a bundle of
static functions, and it is the one place the savestate's capture and restore
read and write the engine's protected and private members. No IVP function
changed, nothing was added to a class, no layout moved; without the macro every
file is the upstream one.

Why a friend and not the member-pointer shim the bridge already uses
(`ivp_private_access.hpp`, `thief<>`: an explicit instantiation is exempt from
access checks): a pointer to member cannot name a bitfield, and four of the
fields the savestate has to carry are protected bitfields that are genuine
history rather than something a constructor or a recalculation reproduces -
`IVP_Mindist_Base::coll_type` (the predicted event type the scheduled event
reads), `IVP_Mindist_Base::synapse_sort_flag` (toggled by the minimizer; it
decides which synapse is "sorted 0", i.e. the sign of the contact normal and the
core roles in the impact and the contact point),
`IVP_Contact_Point_Fast::cp_status` (NORMAL / NEEDS_RECHECK / BROKEN) and
`IVP_Contact_Point_Fast_Static::two_friction_values`. The other road, byte-level
writes into the unit, depends on how the compiler packs the bitfields
(`cp_status` sits at `&has_negative_pull_since - 4` under MSVC and `- 2` under
GCC/Clang) and rots with the next member anyone adds. The rest of the list is
reachable by the shim in principle - the overlap tree's root and hash table, the
cache ring (`n_cache_objects`, `reuse_loop_index`, the slot buffer), the hull
min-list's element array, a listener's `minlist_index`, the OO watcher's private
children - but a savestate that is two translation units reading and writing the
same fields wants one access point, and a second explicit instantiation of the
same `thief<>` specialization in another translation unit is ill-formed.

Evidence for needing the state at all: a rewind restores every core bit for bit
and still replays a touched body metres away from the straight run, because the
collision bookkeeping IVP builds only by running the impacts (which pairs hold
contact points, the per-point friction state, the mindists' lengths and
features, the scheduled events) stays as the live world had it
(`docs/lockstep-without-snapshots.md`, "What the trace shows"). The design and
the inventory are in `docs/ivp-savestate-design.md`.

Inert for the simulation: a friend declaration compiles to nothing. The
submodule is `Swung0x48/Ballanced`, branch `bmmo-collision-overhaul`; CI fetches
it, so push the submodule before the superproject.

Like every BallanceMMO change inside IVP, this compiles only under
`IVP_PORTABLE_BMMO` (see the note at the top); without it the file is the
upstream code with its platform branches.

## 18. Force controllers named after their block (physics_RT)

File: `Source/BuildingBlocks/physics_RT/Behaviors/PhysicsForce.cpp`
(`PhysicsControllerForce`, the class the Physics Force block and the bridge's
`BmmoPhysicsForceBlockRestore` instantiate).

`IVP_Controller::get_controller_name()` is a diagnostic hook that every retail
controller leaves at its default, `"sys:unknown"`. The savestate (#17, bridge
API v17) puts a core's controller order back by matching each recorded
controller to a live one by (priority, name), because the order is history IVP
keeps nowhere else: `IVP_Simulation_Unit::add_controller_of_core` appends and
then runs a stable sort by priority, and `simulate_single_sim_unit_psi` calls the
controllers in that order, each adding into `speed_change`. Two Physics Force
blocks on one body are two controllers of priority `IVP_CP_ACTUATOR` named
`"sys:unknown"`, and a restore could only keep whatever order the script replay
re-created them in - identical descriptors, so neither the restore-identity nor
the replay-identity check could see a swap, and the float sum of three or more
pushes into one core then depends on it (the adversarial review of the
savestate found this; the same held for BallanceMMO's own navigation leaves,
which now carry `"bmmo:force/<leaf>"`).

The controller now carries the block that made it: `SetBehavior(beh->GetID())`
is called at both creation sites and `get_controller_name()` returns
`"rt:force/<behavior id>"` (the default name until it is set, so a controller
made any other way is unchanged). A behavior's CK_ID is stable for the life of
the loaded level, which is all a blob needs (it never leaves its process). The
member is appended to a class private to the translation unit; nothing in the
retail layout moves, and IVP reads the name only to print it.

## 19. The creation parameters, visible while a body is made (physics_RT)

Files: `Source/BuildingBlocks/physics_RT/CKIpionManager.h` (a nested
`PhysicsCreateParams` struct and the member `m_CurrentCreate`),
`CKIpionManager.cpp` (the constructor nulls it; `CreatePhysicsObjectOnParameters`
points it at its own arguments for the duration of the call, through a scope
guard, and nulls it on every return).

A rollback whose window spans a body's birth used to park that body at its
birth pose and re-pose it after its birth tick's step, which puts it back into
IVP's overlap tree, simulation units and object cache through a recheck, at a
different moment and by a different path than the live Physicalize did. The
state the blobs compare came back equal, the ORDER did not (two extra overlap
nodes, shifted unit indices, another cache reuse index), and IVP's order is
numerically significant in a pile: the 20:12 and 50:48 arena arms drifted on
the plate row a few ticks after any rewind through it while the 7:5 arm, whose
rewinds left the tree identical, stayed exact. The fix is to make the body
again inside the replay the way the live frame made it, which needs what the
live frame made it with.

`CreatePhysicsObjectOnParameters` is the one path every creation takes - the
retail Physicalize block and the bridge's own `physicalize` both end there - and
IVP announces the creation through `IVP_Listener_Object::event_object_created`
from inside it, before the entity is attached as `client_data`. The listener
the bridge installs (physics_state.cpp, `event_log_listener`) reads
`m_CurrentCreate` at that moment and writes one `recipe` line into the event log:
every argument (the material's friction and elasticity, mass, dampings, mass
centre, the collision surface and mesh names, the balls, the group, the flags)
and the entity's world matrix. The clients keep those lines by tick and a
rewind anchored before the tick destroys the body at the restore and
re-creates it at the start of its birth tick (`rollback_world::destroy_body` /
`recreate_body`), expecting it exactly on its birth record after the step.

Nothing in the retail layout moves (the member is appended), no IVP file is
touched, and a build without the bridge's listener only carries a pointer that
is null between calls.

---

## Bridge notes: the script replay's record, version 2 (no engine change)

`game/script_replay.hpp` `kVersion = 2`: after a block's force section a byte
says whether the block is a joint block (Set Physics Ball Joint, Hinge, Slider,
Spring) and, if so, whether its handle is set; after a graph's children, the
countdown of every sub-link whose initial activation delay is above one frame;
after the script lists, three data-array cells (`CurrentLevel[0][4]`,
`IngameParameter[0][1]`, `[0][2]`).  The restore re-arms an in-flight link by
setting its countdown and re-activating its source output (only when every
link out of that output is delayed), resets a joint block whose handle the
anchor did not have through `CKM_BEHAVIORRESET`, and writes the cells when the
caller replays the sector activation (`replay_activation`), in which mode every
selected script is written and executed.  `select()` takes
`Gameplay_SectorManager` into the scripts.  Public CK API throughout.

Version 3 (trafo-replication design 4.5, step T1) is written only when a
trafo part is passed (`script_replay::trafo_part`: the trafo sets' scripts
and their built states, kept beside the selection, never in it): the version
word 3, the v2 record byte for byte, then the sets' scripts keyed by id.  The
restore writes every recorded trafo script (active or not), fails on a
recorded id that no longer exists, and puts every selected trafo script the
record does not list (its set was made later) to its built state; a v2
record restored with a trafo part puts all of them to built.  Without a
trafo part every record is v2, as before.  The portable record (resync design
2.4) keeps its version 1: its bytes are on the wire (protocol 10).

---

## Bridge notes: a body put where its Physicalize made it (bridge API v19, engine change #19 for the record only)

`set_body_matrix(entity, matrix[16], simulated)` sets the entity's world matrix
and beams the body to the position and quaternion `CKIpionManager::
FillTemplateInfo` derives from that matrix - the creation's own derivation, the
one algorithm of #8 - at rest, awake or frozen, with the collision recheck.
The rollback uses it at the start of a body's birth tick for a body born inside
the window it replays (`rollback_world::insert_birth`), so the body enters the
overlap tree where and when the live creation did (IVP's creation path IS a
recheck: `enable_collision_detection_for_object` makes the OV element and calls
`recheck_ov_element`).  Two things keep it from being bit-identical to a
creation and are measured rather than hidden: a beam normalizes the quaternion
and the creation does not, so the rotation differs in the last bit and the
engine's post-step check re-poses the body from its birth record (counted as
`birth_insert_mismatches`); and the body's core keeps the age it had live.
`--savestate-diff` grew `--ignore-ov`, `--ignore-order` and `--max-diff N` to
look past the bookkeeping two runs order differently (overlap-tree layout, the
cache ring, unit numbering) to the first physical field.

---

## Bridge notes: restoring a body's leapfrog state (no engine change)

Not a numbered engine change: nothing under `submodule/Ballanced` changed for
it except the regression test. The bridge's body write (API v10
`set_body_state_ex`, `bmmo::physics::set_body_state` with a `wake_mode`) now
puts back what `IVP_Calc_Next_PSI_Solver::set_transformation` drops, and asks
for the body's sleep state explicitly.

IVP integrates with a leapfrog. At every PSI, `calc_next_PSI_matrix`
(`ivp_calc_next_psi_solver.cxx:174-181`) first advances the position by the
delta written at the *previous* PSI
(`pos_world_f_core_last_psi.add_multiple(&delta_world_f_core_psis, d_last_time)`)
and only then sets `delta_world_f_core_psis` to the core's current speed;
likewise it takes the rotation of this PSI from the `q_world_f_core_next_psi`
predicted at the previous one. `set_transformation` (`:242-331`) zeroes the
delta, sets `q_world_f_core_next_psi` to the pose it is given and zeroes
`speed`. So a body beamed to the state it is already in loses exactly one PSI
of translation (|v|/66 m) and one PSI of rotation - which is what a rollback
restore does to every body it corrects. The write therefore takes
`q_world_f_core_last_psi^-1 * q_world_f_core_next_psi` before the beam, puts
that increment back on the new pose afterwards, and rewrites
`delta_world_f_core_psis` from the velocity the caller installs (at a PSI
boundary, which is where a snapshot is read, that is the value
`calc_next_PSI_matrix` had put there). The pre-v10 entries `set_body_state` /
`set_body_state_recheck` are deliberately left byte-identical.

Measured with the new SimTool diagnostic `--restore-at TICK [ENTITY]`, which
reads every movable body after that tick and writes it straight back
(Level 11, `--level 11 --level-at 400 --ticks 1200 --report-every 66`, one
restore at tick 700, the sandbag `P_Modul_26_Sack` swinging at 3.5 m/s):
without the restore the sack is 53.7 mm off the untouched run on the next
tick; with the delta alone 0.44 mm; with delta and rotation increment 0.24 µm.
That last residual is not integration loss and does not grow (0.24 µm again
500 ticks later): it is exactly `(current_time - time_of_last_psi) * |v|`, the
same 6.7e-8 s for every body (rope 1.0e-7 m at 1.5 m/s, sack 2.4e-7 m at
3.5 m/s), because a caller reads the *interpolated* pose at the environment's
current time - which sits that rounding remainder past the last PSI - and the
beam makes it the new last-PSI pose, from which the restored leapfrog then
extrapolates again. Restoring every movable body on every tick for 100 ticks:
3.02 m of error without the restore, 14.5 µm with it.

`wake_mode` is the second half of the same write, for a second defect of the
old one: `IVP_Real_Object::ensure_in_simulation`
on an *awake* body is not a no-op, it calls `reset_freeze_check_values()`
(`ivp_object.cxx:1318-1330`), so a per-tick correction through the old
unconditional wake would keep a mechanism from ever falling asleep. v10's
`wake` only revives a body that is frozen, `freeze` only freezes one that is
simulated, and `keep` leaves the sleep state alone.

Evidence: two more headless regression tests in
`physics_RT/tests/test_physics_regressions.cpp` - "Beaming a body to its own
pose keeps its leapfrog state" (three falling, spinning probes: untouched,
restored with the leapfrog state, restored without it; the restored one
follows the untouched one to under 0.1 mm while the naive one visibly lags)
and "Waking an already simulated body restarts its freeze check"
(`time_of_calm_reference` moves on `ensure_in_simulation()` of an awake body).
All seven tests pass.

## Bridge notes: the force block's controller (bridge API v14, no engine change)

Not an engine change either: the only file under `submodule/Ballanced` that
moved is `physics_RT/Behaviors/PhysicsForce.cpp`, and only by two exported
helpers next to the controller class. The client's script replay
(`docs/lockstep-without-snapshots.md`, "The script replay, as built") captures
and restores the mechanism scripts' state through the public CK API, and the
one piece of that state CK cannot carry is the IVP controller a
`SetPhysicsForce` block owns: a raw `PhysicsControllerForce*` in local
parameter 0, created by a deferred `PhysicsForceCallback` and deleted by the
block's Shutdown. `BmmoPhysicsForceBlockState` reads whether the block has one
and the point and force it was built with (object space, as
`PhysicsForceCallback::Execute` computes them); `BmmoPhysicsForceBlockRestore`
deletes a controller the anchor did not have and re-creates one it had, on the
block's current target, so the replayed Shutdown / Create edges meet the object
they expect. The bridge exposes them as `force_block_get` / `force_block_set`
(API v14), and adds `set_event_log_muted`, which silences the bridge's object
event log while replayed script frames run - a replayed `Physics WakeUp` must
not be reported to the server as a live `BodyRevived`. The retail block's own
code is untouched.

## Bridge notes: a body's friction contacts (bridge API v15, no engine change)

Nothing under `submodule/Ballanced` changed for this one either. A rollback
restore beams every body back to its anchor pose, and a beam cannot carry a
body's **friction contact points**: IVP creates a contact point only when an
impact happens (`IVP_Mindist::do_impact` ->
`IVP_Mindist::try_to_generate_managed_friction`), and a body put back onto the
floor it has since rolled off has no impact coming - it rests on nothing until
it leaves. Measured on the mechanism arena with one forced rewind of depth 12:
anchored while the ball still rolled on `A_Floor_035`, with the contact already
gone at the rewind tick, the ball came out 0.76 m low and 0.5 m/s faster
downward; anchored two ticks later, or with the contact still alive at the
rewind tick, exactly reproduced. The contact diagnostic
(`bmmo::physics::describe_contacts`) showed the restored ball with no friction
contact and no mindist at all, the mindists back one step later, and no contact
ever re-created.

So every rollback record carries, per tracked body, its contacts (the partner
and the per-contact state IVP keeps on the point: `last_gap_len`,
`span_friction_s`, `now_friction_pressure`, `has_negative_pull_since`,
`slowly_turn_on_keeper`), and a restore re-creates a recorded contact the body
lacks through IVP's own creation path: the pair's collision checkers in the
body's overlap element driven to exact mindists now - the beam's recheck leaves
them as hulls, since a beamed body has `current_speed` 0 and the hull check
length is then just the collision distance, and the PSI would only turn them
exact a step later; a mesh pair's checker is an `IVP_OO_Watcher`, whose children
are driven one by one with the body's real speed on the core and the
`IVP_MT_GET_MINDIST` movement state, so a mindist made now stays exact - then the
shortest exact mindist to the recorded partner, and
`IVP_Mindist::try_to_generate_managed_friction` on it, which files the point into
a friction system and fires the friction-created listeners the game registers,
exactly as an impact would. Protected members are reached the way
`ivp_private_access.hpp` reaches the environment's clock.

Measured against the variants (`docs/lockstep-without-snapshots.md`, "The one
the ball still had"), the default (`BMMO_PHYSICS_CONTACT_DEFAULT`) is the
narrow one: a fresh point - IVP's constructor values, none of the recorded
fields copied - only for a body that has no contact left at all, and only
against an unmovable partner. Copying the recorded fields onto the new point
cost 5-11 cm on a ball crossing floor blocks inside a window, which the plain
restore got exact, and a point made between two movable bodies crashed the next
impact solve (`IVP_Impact_System::test_loop_all_pairs`). A contact the body
still has is left alone with its own state; one the record lacks is left to
IVP, which drops a contact whose distance has grown. `--no-restore-contacts`
and `--contact-flags` on the headless client are the A/B switches.

The movable-partner crash, read from a minidump: the impact system pulled a
sleeping core (`movement_state == IVP_MT_NOT_SIM`) in through the new pair and
dereferenced `tmp_null.old_sync_info`, which it allocates only for a simulated
core. IVP never gets there because an impact revives both objects before
`try_to_generate_managed_friction` runs, so the movable cores of a friction
system are always all simulated or all asleep. The bridge now keeps that
precondition itself: a pair whose two movable cores differ in simulation state
is left to IVP, or - with `BMMO_PHYSICS_CONTACT_REVIVE` (1024) - the sleeper is
woken first through `IVP_Core::ensure_core_to_be_in_simulation`, the public
entry an impact uses. Still no change under `submodule/Ballanced`: the
precondition is reachable from outside, which is the test.

## Bridge notes: the restore's collision recheck as a pass of its own (bridge API v16, no engine change)

`body_recheck` (`IVP_Real_Object::recheck_collision_filter`, the recheck a
non-optimized beam runs right after its pose write) lets a restore write every
pose with the optimized beam and then recheck every moved body once all the
poses are in, so no body's pairs are rebuilt against a partner still at its
live pose. Measured on the solo prop probe it is no better than rechecking at
each write (`docs/lockstep-without-snapshots.md`, "What the trace shows"):
the recheck decides which pairs exist, not what they hold - a beam leaves the
friction contact points and the mindists' lengths at the live tick's values
either way - so it stays an A/B switch (`--two-pass-recheck`), off by
default. The same trace produced the two further contact policies of
`contacts_set`: `BMMO_PHYSICS_CONTACT_PRUNE` (2048) deletes, through
`IVP_Real_Object::unlink_contact_points_for_object`, every live contact whose
partner the anchor's record does not name before the recorded ones are
re-created, and `BMMO_PHYSICS_CONTACT_REVIVE` (1024) wakes a sleeping partner
as an impact would. Neither beats the narrow default on the probe: what the
restore still lacks is state IVP builds only by running the impacts
themselves (a second contact point on the same plate from an impact in the
anchor tick, the pair's last-impact time and easing countdown, the mindists'
lengths before the next PSI). Nothing under `submodule/Ballanced` changed.

## Bridge notes: the IVP savestate (bridge API v17, engine change #17 for access only)

The only engine change is the header-only friend declarations of #17; the
savestate itself is bridge code (`BallanceMMOCommon/src/physics/physics_savestate_common.cpp`,
`physics_savestate_capture.cpp` and `physics_savestate_restore.cpp`, compiled
into the headless library and into the client's physics_RT plugin alike). The design,
the inventory of what a tick boundary holds, the restore order and the tests
that prove it are in `docs/ivp-savestate-design.md`; this is the summary.

`get_savestate` captures the environment's whole dynamic state at a tick
boundary as one byte blob: the clock and the event queue (the PSI event and
every scheduled mindist, in queue order), every core's integrator scalars, the
simulation units in manager-list order with their cores and controller order,
every object's hull manager (scalars and the sorted listener list), every
overlap element and the overlap tree itself (nodes, children and elements in
insertion order), every collision checker - mindists, recursive mindists and
OO watchers - with its features, statuses and the protected bitfields, the
exact and invalid lists, every friction system with its contact points,
core-pair records and per-core infos in list order, the revive list, the object
cache ring, and on the physics_RT side the movable-object list, the CK world
matrices, the manager's clock floats, the RNG cursors and the PreSimulate
queue's descriptors. Order is part of the state (the friction solver's matrix
follows the contact-point list, the time manager breaks ties newest first, a
simulation unit integrates its cores in vector order), so every list is
recorded and restored in the engine's own order. The blob is local: its layout
is private to one build and versioned, it is only read back by the binary that
wrote it, and it never goes on the wire (the protocol is unchanged). Four balls
and their props are well under 100 KB a tick.

`set_savestate` tears the live collision state down (contact points through
their friction systems, every collision checker through its destructor, every
overlap element) and rebuilds it from the blob with IVP's own constructors and
list calls, then overwrites every value those calls computed with the recorded
one. No IVP transition - revive, freeze, recheck, impact - runs during the
restore, because each of them rebuilds exactly what the teardown removed, from
the live world's history; cores are written directly and never beamed. The
environment's collision listeners and the bridge event log are muted for the
duration, the monotonic caches (`current_time_code`,
`mindist_event_timestamp_reference`) are bumped, never rewound. It fails closed
on a world with a phantom (buoyancy) object, which no retail script uses.
`SAVESTATE_STRICT` turns the first blob/world inconsistency into a failed
restore, `SAVESTATE_KEEP_UNKNOWN` leaves a body born after the anchor with its
live collision checkers as well. A false return before the teardown leaves the
world untouched; one past it leaves a world the restore has salvaged into a
runnable, not exact, state (the PSI event queued, a unit per core, every body
rechecked into collision detection) and the caller falls back to its per-body
path.

What stays LIVE on purpose, because it belongs to scripts the replay does not
re-execute: the physics time factor (`SetPhysicsGlobals`, written by the
Gameplay script: 1.0 while the level loads, 2.0 once it runs, 0 in the pause
menu) and the gravity. Both are recorded and compared, neither is written back;
a blob captured before the Gameplay script wrote 2.0 would otherwise have put
1.0 back for good and the whole world ran at half rate from then on (measured
on the arena map: ball x 29.99 against the control's 122.53 after 400 ticks).
The frame delta's moving average is restored and the physics step recomputed
from it with the live factor, as `CKIpionManager::SetDeltaTime` does.

What the blob does not know but the world has - a body born after the anchor,
a sector physicalized inside the window - keeps its place in
`CKIpionManager::m_MovableObjects` (after the recorded entries, where a later
birth sat live) and in the revive list, and is left out of collision detection
and named to the caller (`unknown`): the rollback engine keeps a body it will
re-pose at its birth tick inert until then and rechecks the rest back in
through `rollback_world::recheck_body`. Without the first a body kept rolling
in IVP while its CK entity, the one thing the scripts' proximity sensors read,
stayed where the anchor left it; without the second nothing would ever give a
recordless body a tree node or a hull entry again, and a parked body with
collision detection on left its spot 5 m up in the first live tick after a
replay. A recorded overlap-tree node left empty because its only
occupants are gone from the world (an unphysicalized ball) is pruned the way
`cleanup_node` would have; same-named bodies (two instances of one element type
on a map) are told apart by the entity id the record carries.

The bridge entries are `savestate_get(manager, out, max, error)` (writes at most
`max` bytes, returns the blob's full size like snprintf, `out` may be NULL to
ask the size) and `savestate_set(manager, data, size, flags, error, dropped)`
(API v18: `dropped` receives the `;`-joined names of the bodies whose friction
contacts the blob could not carry because their system had a body the world
lost; the rollback engine re-creates those from its own per-body record), with
`BMMO_PHYSICS_SAVESTATE_STRICT` and `_KEEP_UNKNOWN`; the C++ entries are
`bmmo::physics::get_savestate`, `set_savestate`, `check_savestate`,
`describe_savestate`, `diff_savestate` and `hash_savestate`
(`physics/physics_savestate.hpp`). The translation units do no floating-point
arithmetic of their own - the headless library is built without the physics FP
flags - so every value is copied, never re-derived. The rollback engine records
a blob with every tick and restores it first at an anchor that has one, then
writes the snapshot rows on top; `--no-savestate` on the headless client
(`rollback_engine::set_exact_restore(false)`) is the per-body restore, for the
A/B, and `--savestate-strict` runs the restore strict. The headless client's
status line carries `sv=<restored>/<refused>/<capture failures>`,
`--trace-clock A B` prints the engine clock, the event queue, the factor and the
physics step after every live tick in the range and before every replayed one,
`--dump-savestate A B DIR` writes the record's blob of every tick in the
range (`<t>.sav`, for `BallanceMMOSimTool --savestate-diff` /
`--savestate-describe`) and the mechanism scripts' state beside it (`<t>.scr`),
and `--compare-scripts DIR` compares this run's scripts, tick by tick, with the
`<t>.scr` files another run wrote, naming the script, block and field of the
first difference whenever it changes.

The engine's hooks are bound once for every in-process world in
`session/rollback_binding.hpp` (`bind_rollback_world`, templated over the in-process backend `physics/physics_direct.hpp` or the retail mod's `physics_view`); the
headless client and the SimTool's savestate check share it, and the SimTool's
rounds are the engine's own `rewind_to` rather than a copy of the restore. The
retail client binds the same hooks through the plugin API (`physics_view`),
the one copy that cannot share the in-process code.

---

## Bridge notes: the trafo sets (bridge API v21, no engine change)

`docs/trafo-replication-design.md` 2.4, 2.6, 2.10, step T1 (the design names
the version v20; A3's travelling savestate took v20, so the sets are v21).
The bridge keeps one registry per physics manager (`physics_state.cpp`,
`trafo_bridge_state`): every registered set's three piece frames by type, the
entities the body guard leaves alone, the piece names, and a seed per piece
frame.  Entries (C++ `bmmo::physics::trafo_set_*`, the plugin API's
`trafo_set_register/unregister/seed/unmake/filter`):

- `register` routes the set's explosions' Random blocks through the
  deterministic generator and wraps their opening Set Position (idempotent: a
  copy may carry the patched function already), puts the frames and every
  piece below them on the exemption list of change #13, and the piece names
  into the filter's registry; it reports what is routed, exempt and
  registered (the builder's invariant I5).  Registering a player that has a
  registered set fails (unregister first).  A set whose frame ids no longer
  name the frames it registered (a world torn down without unregistering, a
  manager at a reused address, CK reusing freed ids) is dropped before any
  decision reads the registry (register, the Set Position wrapper, the
  exemption rebuild): a stale id never sends an original's explosion down the
  per-set path nor takes an unrelated object off the guard.  `restore_explosion_pieces` now
  rebuilds the exemption list from the originals and then appends every
  registered set (none outside a session with sets: the list is the
  originals' as before).
- The wrapped Set Position restores a registered set's frame alone (the
  originals' restore, for that one frame: initial conditions, detached and
  squared, the pieces' hulls forgotten while they have no body - each set
  has its own meshes, so its own hull names) and the originals as before for
  any other target; then, when the seed registry has an entry for the target
  frame, the generator restarts from it (an original's entry is test-only;
  without one the cursor is left alone, as before).
- `unmake` takes a registered frame's pieces' bodies (`delete_silently`, as
  the Unphysicalize block) and restores the frame.
- `filter` installs (once per environment, the first time it is switched on)
  an `IVP_Collision_Filter` into the environment's meta filter that answers
  `IVP_FALSE` only for a registered piece against a body the resync's
  keep-local predicate names (`PE_Balloon*`, `PE_Box_slide`); off, and for
  every other pair, `IVP_TRUE`, which the meta filter ANDs with the others'.

Nothing here runs without registered sets: SimTool's `--explode wood 300`
traces, the 9.14 two-explosion pose hashes and every per-tick world hash of
those free runs are identical to HEAD's binary (step T1's S9), and the wire
check's 24 captures replay identically.  IVP and physics_RT are untouched.
 that were verified *not* to need engine changes

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
