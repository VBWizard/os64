// state.h — libmath and the thread's floating-point state, shared by the
// guest fixture and the host harness like corpus.h.
//
// os64 publishes no fenv API, but the state is observable all the same:
// native code can load MXCSR, and lrint's answer depends on the rounding
// mode it finds there. So the library promises two things (os64's
// <math.h>): it never CHANGES the caller's control settings, and the
// operations that are exact in every mode stay exact in every mode. These
// six are the checks MATH.md § Consumer interface names:
//
//   lrint directed rounding   1.75 and -1.75 under all four modes
//   lrint ties to even        2.5 and -2.5 to nearest
//   lrint inexact             1.75 raises inexact, 2.0 does not
//   sqrt invalid              sqrt(-1) is a NaN and raises invalid
//   log pole                  log(0) is -infinity and raises divide-by-zero
//   exact sqrt                sqrt(4) is 2 and raises nothing
//
// plus two more that the header's promises need:
//
//   exact in every mode       the exact functions' whole corpus gives the
//                             same bits under all four rounding modes
//   control preserved         every export, every mode: the CONTROL half of
//                             MXCSR and the x87 control word come back as
//                             they went in
//
// Status flags are sticky and the library may set them; control bits are
// the caller's and it may not.
//
// math_state_checks saves both environments first and restores them
// before it returns, whatever happened; each check inside it puts MXCSR
// back to the default when it is done. The includer defines
// MATH_REPORT(fmt, ...) and supplies <stdint.h> and os64's <math.h>.
#ifndef MATHTEST_STATE_H
#define MATHTEST_STATE_H

#define MXCSR_FLAGS     0x003fu           // IE DE ZE OE UE PE: sticky status
#define MXCSR_IE        0x0001u           // invalid
#define MXCSR_ZE        0x0004u           // divide-by-zero
#define MXCSR_PE        0x0020u           // inexact
#define MXCSR_DEFAULT   0x1f80u           // every exception masked, to nearest
#define MXCSR_RC_SHIFT  13

enum { MATH_RN, MATH_RD, MATH_RU, MATH_RZ };   // MXCSR.RC's own order
static const char *const kMathModeNames[] = { "nearest", "downward", "upward", "toward-zero" };

static inline uint32_t math_mxcsr(void)
{
    uint32_t m;
    __asm__ volatile("stmxcsr %0" : "=m"(m) : : "memory");
    return m;
}

static inline void math_set_mxcsr(uint32_t m)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(m) : "memory");
}

static inline uint16_t math_x87cw(void)
{
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw) : : "memory");
    return cw;
}

static inline void math_set_x87cw(uint16_t cw)
{
    __asm__ volatile("fldcw %0" : : "m"(cw) : "memory");
}

// The environment a check runs in: MXCSR with the rounding mode asked for
// and every status flag clear, and the caller's x87 control word.
static inline uint32_t math_enter(int mode)
{
    uint32_t m = MXCSR_DEFAULT | (uint32_t)mode << MXCSR_RC_SHIFT;
    math_set_mxcsr(m);
    return m;
}

// Inputs pass through volatile storage so the compiler cannot evaluate a
// call itself; the calls are real calls to libmath's exports.
static volatile double s_math_in[8];

static int math_state_lrint_modes(void)
{
    static const long expect[4][2] = {
        [MATH_RN] = { 2, -2 }, [MATH_RD] = { 1, -2 },
        [MATH_RU] = { 2, -1 }, [MATH_RZ] = { 1, -1 },
    };
    int failures = 0;
    s_math_in[0] = 1.75;
    s_math_in[1] = -1.75;
    for (int mode = 0; mode < 4; mode++) {
        math_enter(mode);
        long a = lrint(s_math_in[0]);
        long b = lrint(s_math_in[1]);
        math_set_mxcsr(MXCSR_DEFAULT);
        if (a != expect[mode][0] || b != expect[mode][1]) {
            MATH_REPORT("  FAIL lrint %s: 1.75 -> %ld, -1.75 -> %ld (want %ld, %ld)\n",
                        kMathModeNames[mode], a, b, expect[mode][0], expect[mode][1]);
            failures++;
        }
    }
    return failures;
}

static int math_state_lrint_ties(void)
{
    s_math_in[0] = 2.5;
    s_math_in[1] = -2.5;
    math_enter(MATH_RN);
    long a = lrint(s_math_in[0]);
    long b = lrint(s_math_in[1]);
    math_set_mxcsr(MXCSR_DEFAULT);
    if (a == 2 && b == -2)
        return 0;
    MATH_REPORT("  FAIL lrint ties: 2.5 -> %ld, -2.5 -> %ld (want 2, -2)\n", a, b);
    return 1;
}

static int math_state_lrint_inexact(void)
{
    s_math_in[0] = 1.75;
    s_math_in[1] = 2.0;
    math_enter(MATH_RN);
    long a = lrint(s_math_in[0]);
    uint32_t after_a = math_mxcsr();
    math_enter(MATH_RN);
    long b = lrint(s_math_in[1]);
    uint32_t after_b = math_mxcsr();
    math_set_mxcsr(MXCSR_DEFAULT);
    if (a == 2 && (after_a & MXCSR_PE) && b == 2 && !(after_b & MXCSR_FLAGS))
        return 0;
    MATH_REPORT("  FAIL lrint inexact: 1.75 -> %ld flags 0x%02x (want PE), "
                "2.0 -> %ld flags 0x%02x (want none)\n",
                a, after_a & MXCSR_FLAGS, b, after_b & MXCSR_FLAGS);
    return 1;
}

static int math_state_sqrt_invalid(void)
{
    s_math_in[0] = -1.0;
    math_enter(MATH_RN);
    double r = sqrt(s_math_in[0]);
    uint32_t after = math_mxcsr();
    math_set_mxcsr(MXCSR_DEFAULT);
    if (isnan(r) && (after & MXCSR_IE))
        return 0;
    MATH_REPORT("  FAIL sqrt(-1): %s, flags 0x%02x (want NaN and IE)\n",
                isnan(r) ? "NaN" : "not NaN", after & MXCSR_FLAGS);
    return 1;
}

static int math_state_log_pole(void)
{
    s_math_in[0] = 0.0;
    math_enter(MATH_RN);
    double r = log(s_math_in[0]);
    uint32_t after = math_mxcsr();
    math_set_mxcsr(MXCSR_DEFAULT);
    if (isinf(r) && signbit(r) && (after & MXCSR_ZE))
        return 0;
    MATH_REPORT("  FAIL log(0): flags 0x%02x, %s (want -infinity and ZE)\n",
                after & MXCSR_FLAGS, isinf(r) && signbit(r) ? "-infinity" : "wrong value");
    return 1;
}

static int math_state_sqrt_exact(void)
{
    s_math_in[0] = 4.0;
    math_enter(MATH_RN);
    double r = sqrt(s_math_in[0]);
    uint32_t after = math_mxcsr();
    math_set_mxcsr(MXCSR_DEFAULT);
    if (r == 2.0 && !(after & MXCSR_FLAGS))
        return 0;
    MATH_REPORT("  FAIL sqrt(4): flags 0x%02x (want 2.0 and no flags)\n",
                after & MXCSR_FLAGS);
    return 1;
}

static int math_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

// The exact functions have one right answer whatever the rounding mode,
// and <math.h> promises they give it: their whole corpus must come out the
// same bits in all four modes. The promise is not free — floor and round
// find their integer by adding and subtracting 2^52, which DOES round in
// the current mode, and only their correction step makes that safe.
static int math_state_exact_modes(void)
{
    static const char *const exact[] = {
        "ceil", "floor", "trunc", "round", "fabs", "fmax", "fmin", "fmod",
    };
    int failures = 0;
    for (int f = 0; f < MATH_FN_COUNT; f++) {
        int is_exact = 0;
        for (int e = 0; e < MATH_COUNT(exact); e++)
            is_exact |= math_streq(kMathFns[f].name, exact[e]);
        if (!is_exact)
            continue;
        for (int b = 0; b < math_block_count(&kMathFns[f]); b++) {
            math_enter(MATH_RN);
            uint64_t nearest = math_block_digest(f, b);
            for (int mode = MATH_RD; mode <= MATH_RZ; mode++) {
                math_enter(mode);
                uint64_t got = math_block_digest(f, b);
                math_set_mxcsr(MXCSR_DEFAULT);
                if (got != nearest) {
                    MATH_REPORT("  FAIL %s block %d differs %s from to-nearest\n",
                                kMathFns[f].name, b, kMathModeNames[mode]);
                    failures++;
                }
            }
        }
    }
    math_set_mxcsr(MXCSR_DEFAULT);
    return failures;
}

// Every export, every mode: the control bits that went in come back out.
// The inputs are a finite value, a NaN, an infinity and zero, which between
// them walk each routine's main path and its special-case exits.
static int math_state_control_sweep(void)
{
    static const uint64_t inputs[] = {
        0x3ff8000000000000, 0x7ff8000000000000, 0x7ff0000000000000, 0,
    };
    int failures = 0;
    uint16_t cw = math_x87cw();
    for (int mode = 0; mode < 4; mode++) {
        for (int f = 0; f < MATH_FN_COUNT; f++) {
            for (int i = 0; i < MATH_COUNT(inputs); i++) {
                uint32_t want = math_enter(mode) & ~MXCSR_FLAGS;
                (void)math_result(f, inputs[i], inputs[(i + 1) % MATH_COUNT(inputs)]);
                uint32_t got = math_mxcsr() & ~MXCSR_FLAGS;
                uint16_t got_cw = math_x87cw();
                math_set_mxcsr(MXCSR_DEFAULT);
                if (got != want || got_cw != cw) {
                    math_set_x87cw(cw);
                    MATH_REPORT("  FAIL %s (%s, input %d): MXCSR control 0x%04x -> 0x%04x, "
                                "x87 control 0x%04x -> 0x%04x\n",
                                kMathFns[f].name, kMathModeNames[mode], i, want, got, cw, got_cw);
                    failures++;
                }
            }
        }
    }
    return failures;
}

// The whole set. Returns the number of failed checks; the caller's MXCSR
// and x87 control word are as they were.
static int math_state_checks(void)
{
    uint32_t saved = math_mxcsr();
    uint16_t saved_cw = math_x87cw();
    int failures = math_state_lrint_modes()
                 + math_state_lrint_ties()
                 + math_state_lrint_inexact()
                 + math_state_sqrt_invalid()
                 + math_state_log_pole()
                 + math_state_sqrt_exact()
                 + math_state_exact_modes()
                 + math_state_control_sweep();
    math_set_mxcsr(saved);
    math_set_x87cw(saved_cw);
    return failures;
}

#endif
