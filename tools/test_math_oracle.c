// test_math_oracle.c — the independent judge of libmath's numbers.
//
// Reads test_math_host's "name i x y result" lines and recomputes each
// vector with MPFR at 256 bits, which is far past where a binary64 answer
// can be wrong by rounding. Agreement between the host build and the guest
// proves the port is consistent; this proves the imported code computes the
// right function to within what its upstream promises.
//
// Each result is judged in ULPs of the exact answer, measured at the
// binade of the EXACT value (and never finer than 2^-1074, the subnormal
// spacing). Special values are judged by kind, not distance: a NaN must be
// a NaN, an infinity must be the same infinity, and a zero answer must
// carry the exact answer's sign.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpfr.h>

#define PREC 256

typedef int (*mpfr1_t)(mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
typedef int (*mpfr2_t)(mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);

// mpfr_ceil and its siblings take no rounding argument; these adapters
// give them the common shape.
static int ceil_(mpfr_ptr r, mpfr_srcptr x, mpfr_rnd_t m)  { (void)m; return mpfr_ceil(r, x); }
static int floor_(mpfr_ptr r, mpfr_srcptr x, mpfr_rnd_t m) { (void)m; return mpfr_floor(r, x); }
static int trunc_(mpfr_ptr r, mpfr_srcptr x, mpfr_rnd_t m) { (void)m; return mpfr_trunc(r, x); }
static int round_(mpfr_ptr r, mpfr_srcptr x, mpfr_rnd_t m) { (void)m; return mpfr_round(r, x); }
static int fmod_(mpfr_ptr r, mpfr_srcptr x, mpfr_srcptr y, mpfr_rnd_t m) { return mpfr_fmod(r, x, y, m); }

// The bound each function is held to, in ULPs, and where the number comes
// from (libmath/UPSTREAM_REVIEW.md § Accuracy has the table with the
// measured maxima beside it):
//   - upstream's own stated worst case, where its source states one: the
//     Arm-derived exp, log, log2 and pow (without FMA, which os64 has not
//     got); musl's acosh, asinh, atanh and tanh; cbrt's Newton step
//   - fdlibm's faithful-rounding standard, 1 ULP, where it states none
//   - sinh and cosh are built from expm1 exactly as tanh is and state
//     nothing, so they are held to tanh's 2 ULP (sinh measures past 1)
//   - 0.5 for sqrt, which is correctly rounded, and 0 for the functions
//     whose result is always representable and so must be exact
static const struct {
    const char *name;
    mpfr1_t f1;
    mpfr2_t f2;
    double bound;
} kOracle[] = {
    { "sin", mpfr_sin, 0, 1.0 },     { "cos", mpfr_cos, 0, 1.0 },
    { "tan", mpfr_tan, 0, 1.0 },     { "asin", mpfr_asin, 0, 1.0 },
    { "acos", mpfr_acos, 0, 1.0 },   { "atan", mpfr_atan, 0, 1.0 },
    { "atan2", 0, mpfr_atan2, 1.0 }, { "sinh", mpfr_sinh, 0, 2.0 },
    { "cosh", mpfr_cosh, 0, 2.0 },   { "tanh", mpfr_tanh, 0, 2.0 },
    { "asinh", mpfr_asinh, 0, 1.6 }, { "acosh", mpfr_acosh, 0, 2.0 },
    { "atanh", mpfr_atanh, 0, 1.7 }, { "exp", mpfr_exp, 0, 0.511 },
    { "expm1", mpfr_expm1, 0, 1.0 }, { "log", mpfr_log, 0, 0.54 },
    { "log10", mpfr_log10, 0, 1.0 }, { "log2", mpfr_log2, 0, 0.55 },
    { "log1p", mpfr_log1p, 0, 1.0 }, { "pow", 0, mpfr_pow, 0.54 },
    { "sqrt", mpfr_sqrt, 0, 0.5 },   { "cbrt", mpfr_cbrt, 0, 0.667 },
    { "hypot", 0, mpfr_hypot, 1.0 }, { "ceil", ceil_, 0, 0 },
    { "floor", floor_, 0, 0 },       { "trunc", trunc_, 0, 0 },
    { "round", round_, 0, 0 },       { "lrint", 0, 0, 0 },
    { "fabs", mpfr_abs, 0, 0 },      { "fmax", 0, mpfr_max, 0 },
    { "fmin", 0, mpfr_min, 0 },      { "fmod", 0, fmod_, 0 },
};
#define ORACLE_COUNT ((int)(sizeof(kOracle) / sizeof(kOracle[0])))

static double from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

typedef struct {
    long vectors;
    double worst;
    uint64_t worst_x, worst_y;
    long failures;
} tally_t;

static tally_t s_tally[ORACLE_COUNT];

// lrint: round to nearest-even, then convert. A result outside long's
// range, or a NaN, is the conversion instruction's "integer indefinite",
// LONG_MIN — os64's <math.h> promises exactly that.
static int judge_lrint(uint64_t xb, uint64_t rb)
{
    double x = from_bits(xb);
    int64_t want = INT64_MIN;
    if (x == x) {
        mpfr_t t;
        mpfr_init2(t, PREC);
        mpfr_set_d(t, x, MPFR_RNDN);
        mpfr_rint(t, t, MPFR_RNDN);
        if (mpfr_fits_slong_p(t, MPFR_RNDN))
            want = mpfr_get_si(t, MPFR_RNDN);
        mpfr_clear(t);
    }
    return (int64_t)rb == want;
}

// The error of `got` against the exact `ref`, in ULPs; negative when the
// answer is wrong in kind (NaN, infinity, zero sign) rather than distance.
static double ulp_error(mpfr_srcptr ref, double got)
{
    if (mpfr_nan_p(ref))
        return got != got ? 0 : -1;
    if (got != got)
        return -1;
    if (mpfr_inf_p(ref))
        return (got == from_bits(0x7ff0000000000000) && mpfr_sgn(ref) > 0) ||
               (got == from_bits(0xfff0000000000000) && mpfr_sgn(ref) < 0) ? 0 : -1;

    mpfr_t diff, ulp;
    mpfr_inits2(PREC, diff, ulp, (mpfr_ptr)0);
    if (got - got != 0) {
        // An infinite answer to a finite question is right only past the
        // point where round-to-nearest overflows: 2^1024 - 2^970.
        mpfr_set_ui_2exp(ulp, 1, 1024, MPFR_RNDN);
        mpfr_sub_d(ulp, ulp, 0x1p970, MPFR_RNDN);
        int right = mpfr_cmpabs(ref, ulp) >= 0 && (got > 0) == (mpfr_sgn(ref) > 0);
        mpfr_clears(diff, ulp, (mpfr_ptr)0);
        return right ? 0 : -1;
    }
    if (mpfr_zero_p(ref) && got == 0) {
        int same = (mpfr_signbit(ref) != 0) == (__builtin_signbit(got) != 0);
        mpfr_clears(diff, ulp, (mpfr_ptr)0);
        return same ? 0 : -1;
    }
    long e = mpfr_zero_p(ref) ? -1074 : mpfr_get_exp(ref) - 53;   // ref in [2^(E-1), 2^E)
    if (e < -1074)
        e = -1074;
    mpfr_set_d(diff, got, MPFR_RNDN);
    mpfr_sub(diff, diff, ref, MPFR_RNDN);
    mpfr_abs(diff, diff, MPFR_RNDN);
    mpfr_set_ui_2exp(ulp, 1, e, MPFR_RNDN);
    mpfr_div(diff, diff, ulp, MPFR_RNDN);
    double err = mpfr_get_d(diff, MPFR_RNDU);
    mpfr_clears(diff, ulp, (mpfr_ptr)0);
    return err;
}

int main(void)
{
    char name[32];
    int index;
    uint64_t xb, yb, rb;
    long lines = 0;
    mpfr_t x, y, ref;
    mpfr_inits2(PREC, x, y, ref, (mpfr_ptr)0);

    while (scanf("%31s %d %" SCNx64 " %" SCNx64 " %" SCNx64, name, &index, &xb, &yb, &rb) == 5) {
        int o = 0;
        while (o < ORACLE_COUNT && strcmp(kOracle[o].name, name) != 0)
            o++;
        if (o == ORACLE_COUNT) {
            fprintf(stderr, "oracle: unknown function %s\n", name);
            return 2;
        }
        lines++;
        tally_t *t = &s_tally[o];
        t->vectors++;
        double err;
        if (kOracle[o].f1 == 0 && kOracle[o].f2 == 0) {
            err = judge_lrint(xb, rb) ? 0 : -1;
        } else {
            mpfr_set_d(x, from_bits(xb), MPFR_RNDN);
            mpfr_set_d(y, from_bits(yb), MPFR_RNDN);
            if (kOracle[o].f1)
                kOracle[o].f1(ref, x, MPFR_RNDN);
            else
                kOracle[o].f2(ref, x, y, MPFR_RNDN);
            err = ulp_error(ref, from_bits(rb));
        }
        if (err < 0 || err > kOracle[o].bound) {
            if (t->failures++ < 5)
                printf("  FAIL %s x=%016" PRIx64 " y=%016" PRIx64 " -> %016" PRIx64 ": %s %.4f\n",
                       name, xb, yb, rb, err < 0 ? "wrong kind" : "ulp", err);
        }
        if (err > t->worst) {
            t->worst = err;
            t->worst_x = xb;
            t->worst_y = yb;
        }
    }
    mpfr_clears(x, y, ref, (mpfr_ptr)0);

    long failures = 0;
    printf("  %-6s %8s %10s %10s\n", "func", "vectors", "max ulp", "bound");
    for (int o = 0; o < ORACLE_COUNT; o++) {
        const tally_t *t = &s_tally[o];
        printf("  %-6s %8ld %10.4f %10.2f%s\n", kOracle[o].name, t->vectors, t->worst,
               kOracle[o].bound, t->failures ? "  FAIL" : "");
        if (t->vectors == 0) {
            printf("  FAIL %s: no vectors\n", kOracle[o].name);
            failures++;
        }
        failures += t->failures;
    }
    printf("oracle: %s (%ld vectors, %ld failures)\n", failures ? "FAIL" : "PASS", lines, failures);
    return failures ? 1 : 0;
}
