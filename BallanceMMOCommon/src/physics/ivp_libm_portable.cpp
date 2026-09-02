// Platform-independent implementations of the IVP transcendental entry
// points (ivu_libm.hxx, built with IVP_PORTABLE_LIBM).  Every one forwards
// to the vendored OpenLibm subset, so the physics computes the same bits on
// MSVC, glibc and bionic.
#include <physics/portable_math.h>

extern "C" {
double ivp_libm_sin(double x) { return bmmo_pm_sin(x); }
double ivp_libm_cos(double x) { return bmmo_pm_cos(x); }
double ivp_libm_tan(double x) { return bmmo_pm_tan(x); }
double ivp_libm_asin(double x) { return bmmo_pm_asin(x); }
double ivp_libm_acos(double x) { return bmmo_pm_acos(x); }
double ivp_libm_atan(double x) { return bmmo_pm_atan(x); }
double ivp_libm_atan2(double y, double x) { return bmmo_pm_atan2(y, x); }
double ivp_libm_exp(double x) { return bmmo_pm_exp(x); }
double ivp_libm_log(double x) { return bmmo_pm_log(x); }
double ivp_libm_pow(double x, double y) { return bmmo_pm_pow(x, y); }

float ivp_libm_sinf(float x) { return bmmo_pm_sinf(x); }
float ivp_libm_cosf(float x) { return bmmo_pm_cosf(x); }
float ivp_libm_tanf(float x) { return bmmo_pm_tanf(x); }
float ivp_libm_asinf(float x) { return bmmo_pm_asinf(x); }
float ivp_libm_acosf(float x) { return bmmo_pm_acosf(x); }
float ivp_libm_atanf(float x) { return bmmo_pm_atanf(x); }
float ivp_libm_atan2f(float y, float x) { return bmmo_pm_atan2f(y, x); }
float ivp_libm_expf(float x) { return bmmo_pm_expf(x); }
float ivp_libm_logf(float x) { return bmmo_pm_logf(x); }
float ivp_libm_powf(float x, float y) { return bmmo_pm_powf(x, y); }
}
