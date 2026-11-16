// avxprobe — ring 3 gets working AVX, AVX2 and FMA, on more than one CPU,
// and a program is born with clean upper halves (docs/design/pending/AVX.md
// § Proof).
//
//   avxprobe          ask the CPU (CPUID leaf 1: OSXSAVE, AVX, FMA; leaf 7:
//                     AVX2; then XGETBV) and, when every answer is yes, fill
//                     all sixteen upper halves with a pattern of its own and
//                     spawn itself twice, pinned to CPU 0 and to CPU 1
//                     (PINCPU, inherited at spawn; on a machine with one
//                     CPU the second runs unpinned), as `avxprobe --child`.
//   avxprobe --child  check every upper half is ZERO before anything else
//                     (a spawned program starts from the reset image, R5,
//                     however dirty the CPU's registers were), then run
//                     256-bit float, integer and FMA operations whose
//                     answers are known exactly.
//
// Exit 0 = every answer right; 1 = a wrong answer; 2 = AVX not available
// (testrun's SKIP: `sse` mode, or a CPU without it); 3 = a child was born
// with a nonzero upper half; 4 = a child could not be spawned.
//
// No compiler flag changes: userland stays SSE2, the assembler takes the
// VEX instructions in the asm text, and the CPU takes them only after the
// program has asked. Each operation is ONE asm block from memory to memory,
// so the compiler's register choices at -O0 cannot come between a load and
// the result it is checked against. Nothing outside those blocks is
// VEX-encoded, and legacy SSE leaves the upper halves alone, which is what
// makes the birth check meaningful.

#include "os64/os64.h"

#define CPUID1_ECX_FMA     (1U << 12)
#define CPUID1_ECX_OSXSAVE (1U << 27)
#define CPUID1_ECX_AVX     (1U << 28)
#define CPUID7_EBX_AVX2    (1U << 5)
#define XCR0_SSE_AVX       0x6U

#define AVXPROBE_OK          0
#define AVXPROBE_WRONG       1
#define AVXPROBE_NO_AVX      2
#define AVXPROBE_DIRTY_BIRTH 3
#define AVXPROBE_NO_CHILD    4

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static int s_failures;

// Every line goes to the terminal AND the serial wire, so a headless harness
// reads the verdict without a screendump. Each wire line names the APIC ID
// the program is on (CPUID.1:EBX[31:24]), which is how the log shows a pin.
static void say(const char *fmt, ...) OS64_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    char text[200], wire[240];
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    os64_printf("%s\n", text);
    os64_snprintf(wire, sizeof(wire), "avxprobe apic %u: %s", b >> 24, text);
    os64_serial_log(wire);
}

static bool avx_available(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    bool osxsave = (c & CPUID1_ECX_OSXSAVE) != 0;
    bool avx = (c & CPUID1_ECX_AVX) != 0;
    bool fma = (c & CPUID1_ECX_FMA) != 0;
    uint32_t max_leaf, unused;
    cpuid(0, 0, &max_leaf, &unused, &unused, &unused);
    bool avx2 = false;
    if (max_leaf >= 7)
    {
        cpuid(7, 0, &a, &b, &c, &d);
        avx2 = (b & CPUID7_EBX_AVX2) != 0;
    }
    uint64_t xcr0 = 0;
    if (osxsave)
    {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = ((uint64_t)hi << 32) | lo;
    }
    say("avxprobe: CPUID.1 OSXSAVE=%d AVX=%d FMA=%d; CPUID.7 AVX2=%d; XCR0=0x%lx",
        osxsave, avx, fma, avx2, xcr0);
    return osxsave && avx && avx2 && fma && (xcr0 & XCR0_SSE_AVX) == XCR0_SSE_AVX;
}

// The upper 128 bits of every YMM register, in register order.
static void read_upper_halves(uint8_t out[16][16])
{
    __asm__ volatile(
        "vextractf128 [%0 + 0x00], ymm0, 1\n\t"  "vextractf128 [%0 + 0x10], ymm1, 1\n\t"
        "vextractf128 [%0 + 0x20], ymm2, 1\n\t"  "vextractf128 [%0 + 0x30], ymm3, 1\n\t"
        "vextractf128 [%0 + 0x40], ymm4, 1\n\t"  "vextractf128 [%0 + 0x50], ymm5, 1\n\t"
        "vextractf128 [%0 + 0x60], ymm6, 1\n\t"  "vextractf128 [%0 + 0x70], ymm7, 1\n\t"
        "vextractf128 [%0 + 0x80], ymm8, 1\n\t"  "vextractf128 [%0 + 0x90], ymm9, 1\n\t"
        "vextractf128 [%0 + 0xA0], ymm10, 1\n\t" "vextractf128 [%0 + 0xB0], ymm11, 1\n\t"
        "vextractf128 [%0 + 0xC0], ymm12, 1\n\t" "vextractf128 [%0 + 0xD0], ymm13, 1\n\t"
        "vextractf128 [%0 + 0xE0], ymm14, 1\n\t" "vextractf128 [%0 + 0xF0], ymm15, 1\n\t"
        : : "r"(out) : "memory");
}

// Fill every upper half with `fill` and keep each low half as it was
// (vinsertf128 with the register itself as the source). Left dirty on
// purpose: this is the state a careless switch would hand the next program.
static void dirty_upper_halves(const uint8_t fill[16])
{
    __asm__ volatile(
        "vinsertf128 ymm0, ymm0, [%0], 1\n\t"   "vinsertf128 ymm1, ymm1, [%0], 1\n\t"
        "vinsertf128 ymm2, ymm2, [%0], 1\n\t"   "vinsertf128 ymm3, ymm3, [%0], 1\n\t"
        "vinsertf128 ymm4, ymm4, [%0], 1\n\t"   "vinsertf128 ymm5, ymm5, [%0], 1\n\t"
        "vinsertf128 ymm6, ymm6, [%0], 1\n\t"   "vinsertf128 ymm7, ymm7, [%0], 1\n\t"
        "vinsertf128 ymm8, ymm8, [%0], 1\n\t"   "vinsertf128 ymm9, ymm9, [%0], 1\n\t"
        "vinsertf128 ymm10, ymm10, [%0], 1\n\t" "vinsertf128 ymm11, ymm11, [%0], 1\n\t"
        "vinsertf128 ymm12, ymm12, [%0], 1\n\t" "vinsertf128 ymm13, ymm13, [%0], 1\n\t"
        "vinsertf128 ymm14, ymm14, [%0], 1\n\t" "vinsertf128 ymm15, ymm15, [%0], 1\n\t"
        : : "r"(fill) : "memory");
}

static void check_u32(const char *what, const uint32_t *got, const uint32_t *want, int n)
{
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (got[i] != want[i])
        {
            say("avxprobe: FAIL %s lane %d: got 0x%08x want 0x%08x", what, i, got[i], want[i]);
            bad = 1;
        }
    if (!bad)
        say("avxprobe: ok   %s", what);
    s_failures += bad;
}

// Floats are compared as their bit patterns: every expected value below is
// exactly representable, so the right answer is one exact pattern.
static void check_f32(const char *what, const float *got, const float *want, int n)
{
    check_u32(what, (const uint32_t *)got, (const uint32_t *)want, n);
}

static void avx_vaddps(const float *a, const float *b, float *out)
{
    __asm__ volatile(
        "vmovups ymm0, [%0]\n\t"
        "vmovups ymm1, [%1]\n\t"
        "vaddps ymm2, ymm0, ymm1\n\t"
        "vmovups [%2], ymm2\n\t"
        "vzeroupper\n\t"
        : : "r"(a), "r"(b), "r"(out) : "xmm0", "xmm1", "xmm2", "memory");
}

static void avx2_vpaddd(const uint32_t *a, const uint32_t *b, uint32_t *out)
{
    __asm__ volatile(
        "vmovdqu ymm0, [%0]\n\t"
        "vmovdqu ymm1, [%1]\n\t"
        "vpaddd ymm2, ymm0, ymm1\n\t"
        "vmovdqu [%2], ymm2\n\t"
        "vzeroupper\n\t"
        : : "r"(a), "r"(b), "r"(out) : "xmm0", "xmm1", "xmm2", "memory");
}

// out = a * b + c, one rounding (vfmadd231ps: dst += src2 * src3).
static void fma_vfmadd(const float *a, const float *b, const float *c, float *out)
{
    __asm__ volatile(
        "vmovups ymm0, [%0]\n\t"
        "vmovups ymm1, [%1]\n\t"
        "vmovups ymm2, [%2]\n\t"
        "vfmadd231ps ymm2, ymm0, ymm1\n\t"
        "vmovups [%3], ymm2\n\t"
        "vzeroupper\n\t"
        : : "r"(a), "r"(b), "r"(c), "r"(out) : "xmm0", "xmm1", "xmm2", "memory");
}

// The upper half three ways: stored whole, extracted on its own, and after
// a legacy-SSE write to the low half, which must leave the upper 128 bits
// alone (the rule that lets SSE2 code and AVX code share a program).
static void avx_upper(const uint32_t *pattern, const uint32_t *low_sse, uint32_t *whole,
                      uint32_t *upper, uint32_t *after_sse)
{
    __asm__ volatile(
        "vmovdqu ymm3, [%0]\n\t"
        "vmovdqu [%2], ymm3\n\t"
        "vextracti128 xmm4, ymm3, 1\n\t"
        "vmovdqu [%3], xmm4\n\t"
        "movdqu xmm3, [%1]\n\t"           // legacy SSE: low 128 only
        "vmovdqu [%4], ymm3\n\t"
        "vzeroupper\n\t"
        : : "r"(pattern), "r"(low_sse), "r"(whole), "r"(upper), "r"(after_sse)
        : "xmm3", "xmm4", "memory");
}

static int child_main(void)
{
    // FIRST, before anything VEX-encoded runs: the state this program was
    // born with. libos64's start-up is SSE2, which cannot touch an upper half.
    uint8_t born[16][16];
    bool asked = avx_available();
    if (!asked)
        return AVXPROBE_NO_AVX;
    read_upper_halves(born);
    int dirty = 0;
    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 16; i++)
            if (born[r][i] != 0)
                dirty = r + 1;
    if (dirty)
    {
        say("avxprobe: FAIL born with ymm%d's upper half nonzero (0x%02x%02x...) - exit 3",
            dirty - 1, born[dirty - 1][15], born[dirty - 1][14]);
        return AVXPROBE_DIRTY_BIRTH;
    }
    say("avxprobe: ok   born with all sixteen upper halves zero");

    float fa[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    float fb[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    float fc[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    float fout[8];
    float want_add[8] = { 11, 22, 33, 44, 55, 66, 77, 88 };
    avx_vaddps(fa, fb, fout);
    check_f32("vaddps ymm (AVX float)", fout, want_add, 8);

    uint32_t ia[8] = { 1, 2, 3, 4, 5, 6, 7, 0xFFFFFFFFU };
    uint32_t ib[8] = { 100, 200, 300, 400, 500, 600, 700, 2 };
    uint32_t iout[8];
    uint32_t want_padd[8] = { 101, 202, 303, 404, 505, 606, 707, 1 };
    avx2_vpaddd(ia, ib, iout);
    check_u32("vpaddd ymm (AVX2 integer, lane 7 wraps)", iout, want_padd, 8);

    float want_fma[8] = { 10.5f, 40.5f, 90.5f, 160.5f, 250.5f, 360.5f, 490.5f, 640.5f };
    fma_vfmadd(fa, fb, fc, fout);
    check_f32("vfmadd231ps ymm (FMA)", fout, want_fma, 8);

    uint32_t pattern[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444,
                            0xA5A5A5A5, 0x5A5A5A5A, 0xDEADBEEF, 0xC0FFEE00 };
    uint32_t low_sse[4] = { 0x01010101, 0x02020202, 0x03030303, 0x04040404 };
    uint32_t whole[8], upper[4], after_sse[8];
    avx_upper(pattern, low_sse, whole, upper, after_sse);
    check_u32("ymm stored whole", whole, pattern, 8);
    check_u32("upper half via vextracti128", upper, &pattern[4], 4);
    uint32_t want_after[8] = { 0x01010101, 0x02020202, 0x03030303, 0x04040404,
                               0xA5A5A5A5, 0x5A5A5A5A, 0xDEADBEEF, 0xC0FFEE00 };
    check_u32("upper half kept across a legacy-SSE low write", after_sse, want_after, 8);

    if (s_failures)
    {
        say("avxprobe: %d check(s) FAILED - exit 1", s_failures);
        return AVXPROBE_WRONG;
    }
    say("avxprobe: every check passed on this CPU");
    return AVXPROBE_OK;
}

static int run_child_on(const char *self, const char *cpu)
{
    os64_setenv("PINCPU", cpu);
    char *const argv[] = { (char *)self, "--child", NULL };
    int64_t pid = os64_spawn(self, argv);
    if (pid < 0)
    {
        say("avxprobe: could not spawn %s pinned to CPU %s - exit 4", self, cpu);
        return AVXPROBE_NO_CHILD;
    }
    int32_t code = 0;
    while (os64_wait(pid, &code) == OS64_INTERRUPTED)
        ;
    say("avxprobe: the child pinned to CPU %s exited %d", cpu, code);
    return code;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && os64_streq(argv[1], "--child"))
        return child_main();

    if (!avx_available())
    {
        say("avxprobe: AVX not available - exit 2");
        return AVXPROBE_NO_AVX;
    }
    // Leave this program's own upper halves dirty while the children are
    // born: a switch that did not start them from the reset image would
    // hand them this pattern.
    static const uint8_t fill[16] = { 0xD1, 0x47, 0xD1, 0x47, 0xD1, 0x47, 0xD1, 0x47,
                                      0xD1, 0x47, 0xD1, 0x47, 0xD1, 0x47, 0xD1, 0x47 };
    dirty_upper_halves(fill);
    const char *self = "/tests/avxprobe";
    int first = run_child_on(self, "0");
    int second = run_child_on(self, "1");
    os64_unsetenv("PINCPU");
    __asm__ volatile("vzeroupper" ::: "memory");
    if (first != AVXPROBE_OK)
        return first;
    if (second != AVXPROBE_OK)
        return second;
    say("avxprobe: every check passed on CPU 0 and CPU 1 - exit 0");
    return AVXPROBE_OK;
}
