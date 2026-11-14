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
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#endif
