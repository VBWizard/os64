#ifndef OS64_JSPORT_MATH_H
#define OS64_JSPORT_MATH_H
/* Standard-name binary64 ABI specified by MATH.md; builtins classify values. */
#define NAN (__builtin_nan(""))
#define INFINITY (__builtin_inf())
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double acos(double);
double acosh(double);
double asin(double);
double asinh(double);
double atan(double);
double atanh(double);
double cbrt(double);
double ceil(double);
double cos(double);
double cosh(double);
double exp(double);
double expm1(double);
double fabs(double);
double floor(double);
double log(double);
double log10(double);
double log1p(double);
double log2(double);
double round(double);
double sin(double);
double sinh(double);
double sqrt(double);
double tan(double);
double tanh(double);
double trunc(double);
double atan2(double, double);
double fmax(double, double);
double fmin(double, double);
double fmod(double, double);
double hypot(double, double);
double pow(double, double);
long lrint(double);
#endif
