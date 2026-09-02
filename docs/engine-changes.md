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

## Notes on things that were verified *not* to need engine changes

- Floating-point flags: `/fp:precise` (MSVC) and `-ffp-contract=off
  -fexcess-precision=standard` (GCC/Clang) give bit-identical arithmetic
  across x86, x64 and ARM64 (probe in the harness); `/fp:strict` on x86
  crashed the collision code and changed nothing.
- Transcendental functions: every C runtime differs (x86 vs x64 UCRT on
  ~5% of inputs, Android bionic on far more), so the physics is built against
  the vendored OpenLibm subset (`BallanceMMOCommon/third_party/openlibm`)
  through a force-included rename header; no engine source is touched.
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
  determinism and are left untouched for now: `CKFileObject::CleanData`
  releases a `new`-allocated `CKStateChunk` through `CKDeletePointer`
  (`delete[]`), and the Player hotfix `GenerateInputParameter<const char*>`
  passes a C string to `CKParameter::SetValue`, which copies the parameter's
  full size (64 bytes) from a shorter string.
