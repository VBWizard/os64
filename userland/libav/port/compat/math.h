#ifndef OS64_AV_MATH_H
#define OS64_AV_MATH_H
/* Private to the FFmpeg port: libmath's <math.h> (musl, double precision),
 * plus the few functions FFmpeg calls that libmath does not export.
 * FFmpeg's own libavutil/libm.h supplies the float variants and the rest
 * from what is here, because config.h says HAVE_<name> 0 for each. */
#include_next <math.h>
double avlibc_frexp(double x, int *exponent);
double avlibc_scalbn(double x, int n);
#define frexp avlibc_frexp
#define scalbn avlibc_scalbn
static inline float logf(float x) { return (float)log(x); }
/* The AAC decoder's, which config.h has no HAVE_ for: sqrtf and fabsf
 * through libmath's double forms, ldexp as scalbn (the same for a binary
 * floating point). */
static inline float sqrtf(float x) { return (float)sqrt(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline double ldexp(double x, int n) { return avlibc_scalbn(x, n); }
/* The scaler's, which config.h has no HAVE_ for either: each is exact
 * through libmath's double form (a float widens to a double exactly, and
 * floor, fmax and fmin only choose or truncate). */
static inline float floorf(float x) { return (float)floor(x); }
static inline float fmaxf(float x, float y) { return (float)fmax(x, y); }
static inline float fminf(float x, float y) { return (float)fmin(x, y); }
/* Declared and defined nowhere: libswscale/filters.c names it in jinc(),
 * a static filter kernel nothing in this release calls, so the compiler
 * drops it and no reference reaches the link. A release that calls it
 * fails the link (--no-undefined) by name, and libmath gains j1 then. */
double j1(double x);
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#endif
