#pragma once

// Force-included (MSVC /FI, GCC/Clang -include) into every physics
// translation unit (IVP libraries and physics_RT) when
// BMMO_PHYSICS_PORTABLE_MATH is on.  Routes the C math calls the engine
// makes to the deterministic implementations in portable_math.h without
// touching the engine sources.
//
// The host <math.h>/<cmath> are included first so their declarations keep
// their real names; the function-like macros below only rewrite call sites
// that follow.  The engine never uses std::-qualified math or <cmath>
// (verified), and uses some of these names as struct fields (ieee.ln.exp),
// which function-like macros leave alone.  sqrt is not rerouted: the
// hardware square root is correctly rounded on every platform.

#include <math.h>
#ifdef __cplusplus
#include <cmath>
#endif

#include "portable_math.h"

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
