/* Stub: the vendored subset contains no complex functions, and MSVC's C
 * compiler has no _Complex.  Shadows OpenLibm's include/openlibm_complex.h. */
#ifndef OPENLIBM_COMPLEX_H
#define OPENLIBM_COMPLEX_H
/* math_private.h declares a few complex helpers unconditionally; with
 * "complex" defined away they become plain (never called) declarations. */
#define complex
#endif
