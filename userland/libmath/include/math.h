/*
 * math.h — libmath.so, os64's binary64 maths library.
 *
 * The functions keep their C names (MATH.md § Consumer interface): they are
 * the names every numerical codebase calls, and the names GCC itself emits
 * when it cannot expand a maths builtin inline, so a prefixed library could
 * not satisfy those calls. The routines are musl 1.2.5's, with musl's later
 * fixes to these routines backported, and sqrt is the CPU's sqrtsd
 * (userland/libmath/README.md).
 *
 * This header is the WHOLE public surface. It declares exactly what
 * libmath.so exports, and its macros are compiler builtins, so it needs
 * no other header and pulls no libc into a consumer.
 *
 * THE FLOATING-POINT CONTRACT
 *
 * - Errors are reported by the RESULT and by the IEEE status flags in
 *   MXCSR, never by errno — os64 has none. A domain error returns a NaN and
 *   raises invalid (sqrt(-1), acos(2), log(-1)); a pole returns an infinity
 *   and raises divide-by-zero (log(0), atanh(1)); overflow and underflow
 *   return the rounded result and raise the matching flag.
 * - Signed zero, infinities and NaNs follow C's Annex F special cases. A NaN
 *   result is a NaN; its sign and payload are not part of the contract.
 * - The calling thread's floating-point CONTROL settings (rounding mode,
 *   exception masks, flush-to-zero) are read and never changed. The library
 *   is validated in the state every os64 thread starts in: round to
 *   nearest, every exception masked, gradual underflow. In another rounding
 *   mode, ceil, floor, trunc, round, fabs, fmax, fmin and fmod give the
 *   same exact answers, sqrt is correctly rounded in that mode and lrint
 *   rounds in it; the accuracy of the others is measured in round-to-
 *   nearest only.
 * - Compile consumers WITHOUT -ffast-math: it lets the compiler assume no
 *   NaN or infinity exists, which turns isnan() and isfinite() into
 *   constants.
 */
#ifndef OS64_MATH_H
#define OS64_MATH_H

/* Type-generic classification, by compiler builtin so no libm helper is
 * needed and the answer is exact for any floating type. */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#define NAN      __builtin_nanf("")
#define INFINITY __builtin_inff()
#define HUGE_VAL __builtin_huge_val()

/* Trigonometric, with full range reduction: sin(1e300) is the sine of that
 * exact double, not of whatever survives a reduction modulo a rounded pi. */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);   /* y first: the angle of the point (x, y) */

/* Hyperbolic. */
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

/* Exponential and logarithmic. expm1 and log1p stay accurate where x is
 * near zero and exp(x) - 1 or log(1 + x) would cancel to nothing. */
double exp(double x);
double expm1(double x);
double log(double x);
double log10(double x);
double log2(double x);
double log1p(double x);

/* Powers and roots. sqrt is correctly rounded in every rounding mode. */
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);

/* Rounding to an integral value, returned as a double. round() takes a tie
 * AWAY from zero (round(-2.5) == -3), which is C's rule and not
 * JavaScript's Math.round. */
double ceil(double x);
double floor(double x);
double trunc(double x);
double round(double x);

/* lrint rounds in the CURRENT rounding mode (to nearest, ties to even, in
 * the default state) and converts to long, which is 64 bits. A result that
 * does not fit, or a NaN, returns LONG_MIN (0x8000000000000000, the
 * conversion instruction's "integer indefinite") and raises invalid. */
long lrint(double x);

/* Exact operations: no rounding happens, so no mode can change them. fmax
 * and fmin return the other operand when one is a NaN. fmod's result has
 * the sign of x. */
double fabs(double x);
double fmax(double x, double y);
double fmin(double x, double y);
double fmod(double x, double y);

#endif /* OS64_MATH_H */
