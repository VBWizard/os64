// corpus.h — the vectors libmath is checked against, shared by the guest
// fixture (mathtest.c) and the host harness (tools/test_math_host.c).
//
// ONE GENERATOR, TWO MACHINES. The inputs are not stored anywhere: both
// sides rebuild them from this file, and only the RESULTS cross between
// them, as a digest per block (expected.h). A difference between the host
// build of the pinned source and libmath.so in the guest is a PORT defect —
// a flag, the loader, the kernel's FPU save and restore — because the
// arithmetic itself is the same code on both sides.
//
// The generator therefore does integer work only. Every input is assembled
// from its bits (sign, biased exponent, fraction), so no rounding mode,
// compiler or library can make the two sides disagree about what was asked.
//
// The includer supplies <stdint.h> and os64's <math.h>.
#ifndef MATHTEST_CORPUS_H
#define MATHTEST_CORPUS_H

enum { MATH_UNARY, MATH_BINARY, MATH_LRINT };

// One input domain: the biased exponent field drawn uniformly from
// [bexp_lo, bexp_hi] (0 = subnormal, 2047 = infinity/NaN), a uniform
// fraction, and the sign as asked. Uniform in the EXPONENT is what floating
// point testing wants: every binade gets the same attention.
typedef struct {
    uint16_t bexp_lo, bexp_hi;
    uint8_t  sign;               // 0 positive, 1 negative, 2 either
} math_domain_t;

// MATH_D takes unbiased exponents (2^lo .. 2^hi); MATH_FINITE spans every
// finite double including subnormals, MATH_ANY adds infinities and NaNs.
#define MATH_D(lo, hi, s) { (uint16_t)(1023 + (lo)), (uint16_t)(1023 + (hi)), (s) }
#define MATH_FINITE(s)    { 0, 2046, (s) }
#define MATH_ANY          { 0, 2047, 2 }

typedef struct {
    const char *name;
    uint8_t kind;
    double (*f1)(double);
    double (*f2)(double, double);
    long   (*fl)(double);
    // Random draws alternate between the two domains; for a binary
    // function, dx drives x and dy drives y.
    math_domain_t dx, dy;
} math_fn_t;

#define U(n, a, b)    { #n, MATH_UNARY,  n, 0, 0, a, b }
#define B(n, a, b)    { #n, MATH_BINARY, 0, n, 0, a, b }

static const math_fn_t kMathFns[] = {
    // Range reduction is the hard part of the trigonometric three, so half
    // their draws are huge: up to 2^1023, where the reduction needs
    // __rem_pio2_large and its long table of 2/pi.
    U(sin,   MATH_D(-30, 30, 2),   MATH_D(30, 1023, 2)),
    U(cos,   MATH_D(-30, 30, 2),   MATH_D(30, 1023, 2)),
    U(tan,   MATH_D(-30, 30, 2),   MATH_D(30, 1023, 2)),
    U(asin,  MATH_D(-60, -1, 2),   MATH_D(-8, -1, 2)),
    U(acos,  MATH_D(-60, -1, 2),   MATH_D(-8, -1, 2)),
    U(atan,  MATH_D(-60, 60, 2),   MATH_D(-4, 4, 2)),
    B(atan2, MATH_D(-60, 60, 2),   MATH_D(-60, 60, 2)),
    U(sinh,  MATH_D(-60, 9, 2),    MATH_D(-4, 4, 2)),
    U(cosh,  MATH_D(-60, 9, 2),    MATH_D(-4, 4, 2)),
    U(tanh,  MATH_D(-60, 5, 2),    MATH_D(-4, 2, 2)),
    U(asinh, MATH_D(-60, 1023, 2), MATH_D(-4, 4, 2)),
    // Half of acosh's draws are outside its domain, where every answer is
    // a NaN: x <= -2 took the |x| >= 2 paths in musl 1.2.5 and some came
    // back finite (patches/0001).
    U(acosh, MATH_D(0, 1023, 0),   MATH_D(1, 25, 1)),
    U(atanh, MATH_D(-60, -1, 2),   MATH_D(-4, -1, 2)),
    U(exp,   MATH_D(-60, 9, 2),    MATH_D(-2, 9, 2)),
    U(expm1, MATH_D(-60, 9, 2),    MATH_D(-60, -1, 2)),
    U(log,   MATH_D(-1022, 1023, 0), MATH_D(-1, 0, 0)),
    U(log10, MATH_D(-1022, 1023, 0), MATH_D(-1, 0, 0)),
    U(log2,  MATH_D(-1022, 1023, 0), MATH_D(-1, 0, 0)),
    U(log1p, MATH_D(-60, -1, 2),   MATH_D(-60, 1023, 0)),
    B(pow,   MATH_D(-4, 4, 0),     MATH_D(-6, 8, 2)),
    U(sqrt,  MATH_FINITE(0),       MATH_D(-4, 4, 0)),
    U(cbrt,  MATH_FINITE(2),       MATH_D(-4, 4, 2)),
    B(hypot, MATH_FINITE(2),       MATH_FINITE(2)),
    // The rounding functions earn their keep where the fraction runs out:
    // below 2^-1 everything rounds to a neighbour of zero, above 2^52 there
    // is no fraction left. Draws straddle both edges.
    U(ceil,  MATH_D(-3, 55, 2),    MATH_D(-1, 2, 2)),
    U(floor, MATH_D(-3, 55, 2),    MATH_D(-1, 2, 2)),
    U(trunc, MATH_D(-3, 55, 2),    MATH_D(-1, 2, 2)),
    U(round, MATH_D(-3, 55, 2),    MATH_D(-1, 2, 2)),
    { "lrint", MATH_LRINT, 0, 0, lrint, MATH_D(-3, 64, 2), MATH_D(-1, 2, 2) },
    U(fabs,  MATH_ANY,             MATH_ANY),
    B(fmax,  MATH_ANY,             MATH_ANY),
    B(fmin,  MATH_ANY,             MATH_ANY),
    B(fmod,  MATH_FINITE(2),       MATH_FINITE(2)),
};
#undef U
#undef B

#define MATH_FN_COUNT ((int)(sizeof(kMathFns) / sizeof(kMathFns[0])))

// Edge inputs every unary function sees, as bits so a NaN means exactly
// one NaN: signed zeros, the subnormal edges, the binade either side of 1,
// rounding ties, the end of the fraction (2^52, 2^53), lrint's conversion
// limits (2^63 and its predecessor), the thresholds where exp, sinh and
// cosh overflow and exp underflows, pi's neighbours for the trigonometric
// three, very large arguments for their range reduction, infinities and NaNs.
static const uint64_t kMathSpecials[] = {
    0x0000000000000000, 0x8000000000000000,   // +0, -0
    0x0000000000000001, 0x8000000000000001,   // smallest subnormal
    0x000fffffffffffff, 0x800fffffffffffff,   // largest subnormal
    0x0010000000000000, 0x8010000000000000,   // DBL_MIN
    0x0170000000000000,                       // 2^-1000
    0x3c30000000000000,                       // 2^-60
    0x3e40000000000000,                       // 2^-27
    0x3fd0000000000000,                       // 0.25
    0x3fe0000000000000, 0xbfe0000000000000,   // 0.5
    0x3fdfffffffffffff,                       // 0.49999999999999994
    0x3fefffffffffffff, 0xbfefffffffffffff,   // 1 - 2^-53
    0x3ff0000000000000, 0xbff0000000000000,   // 1
    0x3ff0000000000001, 0xbff0000000000001,   // 1 + 2^-52
    0x3ff8000000000000, 0xbff8000000000000,   // 1.5
    0x4000000000000000, 0xc000000000000000,   // 2
    0x4004000000000000, 0xc004000000000000,   // 2.5
    0x400c000000000000, 0xc00c000000000000,   // 3.5
    0x3fe921fb54442d18,                       // pi/4
    0x3ff921fb54442d18, 0xbff921fb54442d18,   // pi/2
    0x400921fb54442d18,                       // pi
    0x4024000000000000, 0x4059000000000000,   // 10, 100
    0xc0e8000000000000,                       // -0x1.8p15: acosh's upstream report
    0x4330000000000000, 0x4330000000000001,   // 2^52, 2^52 + 1
    0x4340000000000000,                       // 2^53
    0x43dfffffffffffff, 0xc3dfffffffffffff,   // 2^63 - 1024
    0x43e0000000000000, 0xc3e0000000000000,   // 2^63
    0x4086232bdd7abcd2, 0xc086232bdd7abcd2,   // 708.396...: exp near 2^1022
    0x40862e42fefa39ef, 0x40862e42fefa39f0,   // exp's overflow edge, 709.78...
    0x408633ce8fb9f87d, 0x408633ce8fb9f87e,   // sinh/cosh overflow edge, 710.47...
    0xc0874910d52d3051, 0xc0874910d52d3052,   // exp's underflow to zero, -745.13...
    0x44b52d02c7e14af6,                       // 1e23
    0x7e37e43c8800759c,                       // 1e300
    0x7fe0000000000000,                       // 2^1023
    0x7fefffffffffffff, 0xffefffffffffffff,   // DBL_MAX
    0x7ff0000000000000, 0xfff0000000000000,   // infinity
    0x7ff8000000000000, 0xfff8000000000000,   // quiet NaN, both signs
};

// A binary function sees every PAIR of these: enough to reach each of
// pow's and atan2's special-case rows (odd and even integer powers of a
// negative base, a zero or infinite operand of each sign) at 324 vectors.
static const uint64_t kMathPairSpecials[] = {
    0x0000000000000000, 0x8000000000000000,   // +0, -0
    0x0000000000000001,                       // smallest subnormal
    0x3fe0000000000000, 0xbfe0000000000000,   // 0.5
    0x3ff0000000000000, 0xbff0000000000000,   // 1
    0x4000000000000000, 0xc000000000000000,   // 2
    0x4008000000000000, 0xc008000000000000,   // 3
    0x4004000000000000,                       // 2.5
    0x7e37e43c8800759c, 0xfe37e43c8800759c,   // 1e300
    0x7ff0000000000000, 0xfff0000000000000,   // infinity
    0x7ff8000000000000, 0xfff8000000000000,   // quiet NaN
};

#define MATH_COUNT(a)       ((int)(sizeof(a) / sizeof((a)[0])))
#define MATH_RANDOM_DRAWS   256
#define MATH_BLOCK          64

static inline int math_vector_count(const math_fn_t *fn)
{
    int specials = fn->kind == MATH_BINARY
        ? MATH_COUNT(kMathPairSpecials) * MATH_COUNT(kMathPairSpecials)
        : MATH_COUNT(kMathSpecials);
    return specials + MATH_RANDOM_DRAWS;
}

static inline int math_block_count(const math_fn_t *fn)
{
    return (math_vector_count(fn) + MATH_BLOCK - 1) / MATH_BLOCK;
}

static inline double math_from_bits(uint64_t bits)
{
    union { uint64_t u; double d; } v = { bits };
    return v.d;
}

static inline uint64_t math_to_bits(double d)
{
    union { double d; uint64_t u; } v = { d };
    return v.u;
}

// splitmix64: small, well mixed, and the same on every machine. Each
// (function, vector) pair seeds its own stream, so a vector's inputs do not
// depend on how many vectors came before it.
static inline uint64_t math_mix(uint64_t z)
{
    z += 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

static inline uint64_t math_draw(const math_domain_t *d, uint64_t r)
{
    uint64_t span = (uint64_t)d->bexp_hi - d->bexp_lo + 1;
    uint64_t bexp = d->bexp_lo + (r % span);
    uint64_t frac = math_mix(r) & 0x000fffffffffffff;
    uint64_t sign = d->sign == 2 ? (math_mix(r ^ 0x5157) & 1) : d->sign;
    return sign << 63 | bexp << 52 | frac;
}

// The inputs of vector i of function f.
static inline void math_vector(int f, int i, uint64_t *x, uint64_t *y)
{
    const math_fn_t *fn = &kMathFns[f];
    *y = 0;
    if (fn->kind == MATH_BINARY) {
        int n = MATH_COUNT(kMathPairSpecials);
        if (i < n * n) {
            *x = kMathPairSpecials[i / n];
            *y = kMathPairSpecials[i % n];
            return;
        }
        i -= n * n;
    } else if (i < MATH_COUNT(kMathSpecials)) {
        *x = kMathSpecials[i];
        return;
    } else {
        i -= MATH_COUNT(kMathSpecials);
    }
    uint64_t r = math_mix(((uint64_t)f << 32) | (uint64_t)i);
    *x = math_draw((i & 1) ? &fn->dy : &fn->dx, r);
    if (fn->kind == MATH_BINARY)
        *y = math_draw(&fn->dy, math_mix(r ^ 0xb1a5));
}

// The result of vector i, as the bits a digest covers. A NaN's sign and
// payload are not part of the contract (os64's <math.h>), so every NaN
// counts as one value; lrint's long is carried as its own bits.
static inline uint64_t math_result(int f, uint64_t xb, uint64_t yb)
{
    const math_fn_t *fn = &kMathFns[f];
    double x = math_from_bits(xb), y = math_from_bits(yb), r;
    if (fn->kind == MATH_LRINT)
        return (uint64_t)fn->fl(x);
    r = fn->kind == MATH_BINARY ? fn->f2(x, y) : fn->f1(x);
    return isnan(r) ? 0x7ff8000000000000 : math_to_bits(r);
}

// FNV-1a over the result bits of one block of vectors.
static inline uint64_t math_block_digest(int f, int block)
{
    uint64_t h = 0xcbf29ce484222325;
    int end = (block + 1) * MATH_BLOCK, n = math_vector_count(&kMathFns[f]);
    if (end > n)
        end = n;
    for (int i = block * MATH_BLOCK; i < end; i++) {
        uint64_t x, y, r;
        math_vector(f, i, &x, &y);
        r = math_result(f, x, y);
        for (int b = 0; b < 8; b++) {
            h ^= (r >> (8 * b)) & 0xff;
            h *= 0x100000001b3;
        }
    }
    return h;
}

#endif
