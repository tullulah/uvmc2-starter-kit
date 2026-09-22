#ifndef _AAE_MATH_H
#define _AAE_MATH_H
/* Freestanding math shim. The Asteroids file set only references `log`
 * (acommon.c); the rest are declared for headers that reference them but whose
 * users get dropped by --gc-sections. Implementations live in libc_stub.c. */
double log(double x);
double sin(double x);
double cos(double x);
double sqrt(double x);
double atan2(double y, double x);
double pow(double b, double e);
double floor(double x);
double fabs(double x);
#endif
