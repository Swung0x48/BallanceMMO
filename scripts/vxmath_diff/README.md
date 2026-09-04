# Comparing the fork's VxMath against the game's

The BallanceMMO server runs a reimplementation of the Virtools engine; the
client runs the 2002 binaries. For a recording made on one to replay
bit-exactly on the other, every routine on the physics path has to agree to
the last bit, and `VxMath.dll` sits under all of it: `CK3dEntity` (which lives
in `CK2_3D.dll`, not `CK2.dll`) derives a child's local matrix by calling
`Vx3DMultiplyMatrix`.

These tools answer "do the two agree, and if not, exactly how" by running both
in one process and comparing bit patterns. They are the evidence behind engine
change #11 in `docs/engine-changes.md` and design section 9.12.

## Why it is built this way

Three details are what make the comparison meaningful rather than misleading.

**The retail DLL is loaded, not linked.** The fork defines the `Vx3D*`
functions as header inlines with the same mangled names the retail DLL
exports. Linking the import library puts both in one image and the linker
rejects it. `retail_side.cpp` therefore resolves each export by decorated name
with `GetProcAddress`, and passes raw float arrays, which have the same layout
as `VxMatrix`, `VxVector` and `VxQuaternion`. `__thiscall` members are called
through a member-function-pointer union, the only way to make the compiler
emit that convention.

**The x87 unit is set to 24-bit precision.** The retail client runs with
control word `000a001f`. Retail VxMath's internal arithmetic rounds to float at
every step there, and comparing against any other setting measures a
configuration the game never runs.

**Conventions are checked before the numbers are.** The harness first
multiplies identity, translation and scale matrices on both sides. If storage
order or operand order disagreed, every later result would look like huge
arithmetic drift. They agree.

## The tools

| tool | what it answers |
| --- | --- |
| `vxmath_diff` | which functions differ, and by how much, over 200000 random and Ballance-shaped inputs |
| `solve_mul` | the exact order retail sums the four products of a matrix multiply |
| `solve_rest` | the same for the two vector transforms and `VxVector::Normalize` |
| `solve_inv` | 36 formulations of the cofactor inverse, none of which reproduce retail |
| `probe_inv` | prints retail's inverse next to textbook candidates, to pick a family to search |

Each takes the path to the game's `VxMath.dll` as its first argument, and
defaults to a local install. `vxmath_diff` accepts `--pc53` as a second
argument to leave the x87 precision alone, for contrast.

## What it found

Retail sums a matrix multiply's four products pairwise, `(p0+p1)+(p2+p3)`;
the fork summed them left to right. Float addition is not associative, so
every element was one or two ulp out, which is what had been drifting the
level mechanisms. The two vector transforms accumulate left to right in
retail, which the fork's scalar path already did but its SSE kernels did not.
Fixing those four brought five of the seven diverging levels to bit-exact.

Still unreconciled, in rough order of how much they matter: `Vx3DInverseMatrix`
(used by `GetInverseWorldMatrix`), `VxVector::Normalize`,
`VxQuaternion::FromMatrix`, `Vx3DDecomposeMatrix`, `Vx3DInterpolateMatrix`,
`Vx3DMatrixFromRotation`.

Two of those are worth a warning. The inverse is not a rounding-order
difference — 36 plausible formulations all failed — so retail uses a different
algorithm. And `Vx3DMatrixFromRotation` differs semantically, not numerically:
the shipped function returns the transpose of the fork's result, and its sine
and cosine carry only about four correct digits. Anything that starts
depending on it should be investigated first.
