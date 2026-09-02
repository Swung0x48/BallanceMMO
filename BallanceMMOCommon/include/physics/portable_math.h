#pragma once

/* Deterministic libm subset for the physics code (OpenLibm / FreeBSD msun,
 * compiled from BallanceMMOCommon/third_party/openlibm).  Same bits on
 * every platform; see bmmo_portable_math.c for the reasoning. */

#ifdef __cplusplus
extern "C" {
#endif

double bmmo_pm_sin(double);
double bmmo_pm_cos(double);
double bmmo_pm_tan(double);
double bmmo_pm_asin(double);
double bmmo_pm_acos(double);
double bmmo_pm_atan(double);
double bmmo_pm_atan2(double, double);
double bmmo_pm_exp(double);
double bmmo_pm_log(double);
double bmmo_pm_pow(double, double);
double bmmo_pm_fmod(double, double);
double bmmo_pm_scalbn(double, int);
double bmmo_pm_floor(double);
double bmmo_pm_fabs(double);
double bmmo_pm_copysign(double, double);
double bmmo_pm_rint(double);
double bmmo_pm_sqrt(double);     /* hardware IEEE sqrt (correctly rounded everywhere) */

float bmmo_pm_sinf(float);
float bmmo_pm_cosf(float);
float bmmo_pm_tanf(float);
float bmmo_pm_asinf(float);
float bmmo_pm_acosf(float);
float bmmo_pm_atanf(float);
float bmmo_pm_atan2f(float, float);
float bmmo_pm_expf(float);
float bmmo_pm_logf(float);
float bmmo_pm_powf(float, float);
float bmmo_pm_fmodf(float, float);
float bmmo_pm_scalbnf(float, int);
float bmmo_pm_floorf(float);
float bmmo_pm_fabsf(float);
float bmmo_pm_copysignf(float, float);
float bmmo_pm_sqrtf(float);      /* hardware IEEE sqrtf */

#ifdef __cplusplus
}
#endif
