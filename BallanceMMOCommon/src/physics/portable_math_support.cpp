// Hardware square roots for the portable math library: IEEE 754 requires
// sqrt to be correctly rounded, so sqrtsd / fsqrt / vsqrt agree everywhere.
#include <physics/portable_math.h>

#include <cmath>

extern "C" double bmmo_pm_sqrt(double x) { return std::sqrt(x); }
extern "C" float bmmo_pm_sqrtf(float x) { return std::sqrt(x); }
