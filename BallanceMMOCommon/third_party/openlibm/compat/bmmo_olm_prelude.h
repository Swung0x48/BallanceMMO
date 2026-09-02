/* Prelude for building the vendored OpenLibm subset with any compiler.
 *
 * OpenLibm's private headers expect GCC/Clang predefined macros for the
 * architecture and float word order; MSVC spells them differently.  This
 * header is included first (through bmmo_olm_rename.h) by every vendored
 * source and by nothing else.
 */
#ifndef BMMO_OLM_PRELUDE_H
#define BMMO_OLM_PRELUDE_H

#if defined(_MSC_VER)
#  ifndef _USE_MATH_DEFINES
#    define _USE_MATH_DEFINES 1   /* M_PI_2 and friends */
#  endif
#  if defined(_M_X64) && !defined(__x86_64__)
#    define __x86_64__ 1
#    define __LP64__ 1
#  elif defined(_M_IX86) && !defined(__i386__)
#    define __i386__ 1
#  elif defined(_M_ARM64) && !defined(__aarch64__)
#    define __aarch64__ 1
#    define __LP64__ 1
#  endif
#  ifndef __ORDER_LITTLE_ENDIAN__
#    define __ORDER_LITTLE_ENDIAN__ 1234
#  endif
#  ifndef __ORDER_BIG_ENDIAN__
#    define __ORDER_BIG_ENDIAN__ 4321
#  endif
#  ifndef __FLOAT_WORD_ORDER__
#    define __FLOAT_WORD_ORDER__ __ORDER_LITTLE_ENDIAN__
#  endif
#  ifndef __BYTE_ORDER__
#    define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#  endif
#endif

/* GCC predefines __FLOAT_WORD_ORDER__; Clang (NDK, Apple) does not, and
 * OpenLibm's math_private.h then defines no ieee_double_shape_type at all.
 * Every supported target stores doubles in byte order. */
#if !defined(__FLOAT_WORD_ORDER__) && defined(__BYTE_ORDER__)
#  define __FLOAT_WORD_ORDER__ __BYTE_ORDER__
#endif

/* Replace OpenLibm's cdefs-compat.h entirely.  Its alias machinery
 * (weak/strong references such as "acosl -> acos", "ldexp -> scalbn") names
 * the public symbols by string, which the bmmo_pm_* renaming cannot reach,
 * and this build wants no aliases at all. */
#define _CDEFS_COMPAT_H_
#ifndef __BEGIN_DECLS
#  ifdef __cplusplus
#    define __BEGIN_DECLS extern "C" {
#    define __END_DECLS }
#  else
#    define __BEGIN_DECLS
#    define __END_DECLS
#  endif
#endif
#define openlibm_weak_reference(sym, alias)
#define openlibm_strong_reference(sym, alias)
#define openlibm_warn_references(sym, msg)
#ifndef __pure2
#  define __pure2
#endif
#ifndef __unused
#  define __unused
#endif
#ifndef __dead2
#  define __dead2
#endif

/* The long double layout header (fpmath.h) is only needed by long double
 * functions, which this subset does not build, and its 64-bit bitfields do
 * not compile with MSVC. */
#define _FPMATH_H_

/* Our copy is a static library with prefixed names; never dllexport. */
#define OPENLIBM_DEFS_H_
#define OLM_DLLEXPORT

#endif
