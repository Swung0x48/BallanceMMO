/* Deterministic transcendental functions for the physics code.
 *
 * Every C runtime ships its own sin/cos/asin/...: the x86 and x64 Windows
 * CRTs already disagree in the last bit on a few percent of inputs, and
 * glibc / Apple / Android libm differ again.  IVP calls these functions in
 * every physics step, and one differing bit turns into a different contact
 * branch within a frame.  The library therefore compiles a subset of
 * OpenLibm (FreeBSD msun, portable C, plain IEEE double arithmetic) under
 * the bmmo_pm_* names, so the same bits come out on every platform as long
 * as the compiler does not contract or reassociate (see
 * cmake/PhysicsFloatingPoint.cmake).
 *
 * The vendored sources in src/ are compiled as separate translation units
 * (they share static constant names), each with compat/bmmo_olm_rename.h
 * force-included; cmake/PortableMath.cmake lists them.  This file only
 * carries the build's identification so the library is never empty.
 */
const char* bmmo_portable_math_origin(void) {
    return "OpenLibm subset, see BallanceMMOCommon/third_party/openlibm/README.md";
}
