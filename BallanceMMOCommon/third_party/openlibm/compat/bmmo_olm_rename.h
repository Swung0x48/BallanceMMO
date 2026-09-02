/* Force-included into every vendored OpenLibm source: compiler prelude, the
 * host CRT's <math.h> under its real names, then function-like macros that
 * rename the public functions this subset defines and calls.  OpenLibm binds
 * its internal __ieee754_* names to the public ones in math_private.h, so
 * the renaming covers the whole subset.  sqrt stays on the hardware unit
 * (portable_math_support.cpp): IEEE sqrt is correctly rounded everywhere.
 *
 * See bmmo_portable_math.c for why these functions exist at all. */
#ifndef BMMO_OLM_RENAME_H
#define BMMO_OLM_RENAME_H

#include "bmmo_olm_prelude.h"

#include <math.h>

/* glibc declares a function named __nan; OpenLibm declares a union of the
 * same name.  The host header is already parsed, so rename OpenLibm's. */
#define __nan bmmo_olm_nan_union

#define sin(x) bmmo_pm_sin(x)
#define cos(x) bmmo_pm_cos(x)
#define tan(x) bmmo_pm_tan(x)
#define asin(x) bmmo_pm_asin(x)
#define acos(x) bmmo_pm_acos(x)
#define atan(x) bmmo_pm_atan(x)
#define atan2(y, x) bmmo_pm_atan2(y, x)
#define exp(x) bmmo_pm_exp(x)
#define log(x) bmmo_pm_log(x)
#define pow(x, y) bmmo_pm_pow(x, y)
#define fmod(x, y) bmmo_pm_fmod(x, y)
#define scalbn(x, n) bmmo_pm_scalbn(x, n)
#define floor(x) bmmo_pm_floor(x)
#define fabs(x) bmmo_pm_fabs(x)
#define copysign(x, y) bmmo_pm_copysign(x, y)
#define rint(x) bmmo_pm_rint(x)
#define sqrt(x) bmmo_pm_sqrt(x)

#define sinf(x) bmmo_pm_sinf(x)
#define cosf(x) bmmo_pm_cosf(x)
#define tanf(x) bmmo_pm_tanf(x)
#define asinf(x) bmmo_pm_asinf(x)
#define acosf(x) bmmo_pm_acosf(x)
#define atanf(x) bmmo_pm_atanf(x)
#define atan2f(y, x) bmmo_pm_atan2f(y, x)
#define expf(x) bmmo_pm_expf(x)
#define logf(x) bmmo_pm_logf(x)
#define powf(x, y) bmmo_pm_powf(x, y)
#define fmodf(x, y) bmmo_pm_fmodf(x, y)
#define scalbnf(x, n) bmmo_pm_scalbnf(x, n)
#define floorf(x) bmmo_pm_floorf(x)
#define fabsf(x) bmmo_pm_fabsf(x)
#define copysignf(x, y) bmmo_pm_copysignf(x, y)
#define sqrtf(x) bmmo_pm_sqrtf(x)

#endif
