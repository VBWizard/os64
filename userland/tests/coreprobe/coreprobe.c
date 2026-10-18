// coreprobe — what one CPU says about itself, and how fast it runs, measured
// from wherever the scheduler put this task; the `apic` lines say where that
// was. It was born to find why the P5's odd APICs ran the compositor three
// times slower (2026-10-18: SPEC_CTRL inherited with SSBD set on every
// second thread — smp_core.c, spec_ctrl_init_this_cpu), and it stays as the
// proof: after bring-up every thread of a core should post the same `imul`
// rate within noise, and every branch kind should cost the same on both.
//
//   topology  CPUID leaf 1's APIC id, leaf 0xB's threads-per-core and x2APIC
//             id, and AMD's leaf 0x8000001E (core id, threads per core, node)
//             — the processor's OWN statement of which CPUs share a core.
//   imul      a dependent multiply-add chain: iterations per microsecond.
//             Built -O2 (the makefile's OPTIMIZED_APPS), so each step is one
//             imul plus one add on the chain and the loop's own branch; on
//             a Zen that is about four cycles, so the core clock is roughly
//             4x this number in MHz.
//   memcpy    libos64's memcpy, 8 MB to 8 MB, and a plain 32-bit copy loop
//             like the compositor's blit, both in MB/s.
//   syscalls  micros() round trips per millisecond.
//
// coreprobe [ms]     — how long each measurement runs (default 500).
// coreprobe -s [sec] — SAMPLE instead: for that many seconds, sleep a
//                      little, then time the imul chain for 100 ms and print
//                      one line — `<ms since start> apic <n> imul <rate>` —
//                      so a thread the scheduler moves about reports every
//                      core it visits and how fast that core ran it.
#include "os64/os64.h"

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static uint32_t apic_now(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return b >> 24;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void copy_u32(uint32_t *dst, const uint32_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
}

// A pointer chase over `bytes` of cache lines in a random cycle: the time
// of one dependent load. Inside L1 or L2 it is a cycle count, so it scales
// with the core clock; from DRAM it is nanoseconds of wire, so it does not.
__attribute__((unused)) static uint64_t chase_ns10(uint8_t *buf, size_t bytes, int64_t ms)
{
    size_t lines = bytes / 64;
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    // Sattolo's shuffle: one cycle through every line.
    for (size_t i = 0; i < lines; i++)
        *(uint64_t *)(buf + i * 64) = i;
    for (size_t i = lines - 1; i > 0; i--) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t j = (size_t)((seed >> 33) % i);
        uint64_t t = *(uint64_t *)(buf + i * 64);
        *(uint64_t *)(buf + i * 64) = *(uint64_t *)(buf + j * 64);
        *(uint64_t *)(buf + j * 64) = t;
    }
    uint64_t p = 0, loads = 0;
    int64_t t0 = os64_micros(), t1 = t0;
    while (t1 - t0 < ms * 1000) {
        for (uint32_t i = 0; i < 4096; i++)
            p = *(uint64_t *)(buf + p * 64);
        loads += 4096;
        t1 = os64_micros();
    }
    __asm__ volatile("" :: "r"(p));
    return (uint64_t)(t1 - t0) * 10000 / loads;   // tenths of a ns per load
}

// Three shapes of the same work, to tell a per-instruction cost from a
// per-branch or per-fetch one. Each returns loop bodies per microsecond.
//   chain64: the imul chain unrolled 64 deep — one taken branch per 64 steps
//   nops:    256 nops per branch — pure front-end bandwidth, no dependencies
//   indep:   16 independent adds per branch — ALU throughput, no latency
__attribute__((unused)) static uint64_t chain64_rate(int64_t ms)
{
    uint64_t x = 1, iters = 0;
    int64_t t0 = os64_micros(), t1 = t0;
    while (t1 - t0 < ms * 1000) {
        for (uint32_t i = 0; i < (1u << 12); i++) {
#define STEP x = x * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;
#define STEP8 STEP STEP STEP STEP STEP STEP STEP STEP
            STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8
            __asm__ volatile("" : "+r"(x));
        }
        iters += 1u << 12;
        t1 = os64_micros();
    }
    __asm__ volatile("" :: "r"(x));
    return iters / (uint64_t)(t1 - t0);
}

__attribute__((unused)) static uint64_t nops_rate(int64_t ms)
{
    uint64_t iters = 0;
    int64_t t0 = os64_micros(), t1 = t0;
    while (t1 - t0 < ms * 1000) {
        for (uint32_t i = 0; i < (1u << 14); i++)
            __asm__ volatile(".rept 256\n\tnop\n\t.endr");
        iters += 1u << 14;
        t1 = os64_micros();
    }
    return iters / (uint64_t)(t1 - t0);
}

__attribute__((unused)) static uint64_t indep_rate(int64_t ms)
{
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0, iters = 0;
    int64_t t0 = os64_micros(), t1 = t0;
    while (t1 - t0 < ms * 1000) {
        for (uint32_t i = 0; i < (1u << 16); i++) {
            a0 += i; a1 += i; a2 += i; a3 += i; a4 += i; a5 += i; a6 += i; a7 += i;
            a0 ^= i; a1 ^= i; a2 ^= i; a3 ^= i; a4 ^= i; a5 ^= i; a6 ^= i; a7 ^= i;
            __asm__ volatile("" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7));
        }
        iters += 1u << 16;
        t1 = os64_micros();
    }
    return iters / (uint64_t)(t1 - t0);
}

// The imul chain with one extra branch of a given KIND inside each loop
// body, so the cost of that kind of branch is the difference from `imul`.
#define BRANCH_KERNEL(name, asmtext)                                          \
static uint64_t name(int64_t ms)                                              \
{                                                                             \
    uint64_t x = 1, iters = 0;                                                \
    int64_t t0 = os64_micros(), t1 = t0;                                      \
    while (t1 - t0 < ms * 1000) {                                             \
        for (uint32_t i = 0; i < (1u << 20); i++) {                           \
            x = x * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;            \
            __asm__ volatile(asmtext : "+r"(x) :: "rax", "memory");          \
        }                                                                     \
        iters += 1u << 20;                                                    \
        t1 = os64_micros();                                                   \
    }                                                                         \
    __asm__ volatile("" :: "r"(x));                                           \
    return iters / (uint64_t)(t1 - t0);                                       \
}
BRANCH_KERNEL(taken_rate,    "jmp 1f\n\t1:")
BRANCH_KERNEL(nottaken_rate, "test %0, %0\n\tjz 1f\n\t1:")
BRANCH_KERNEL(indirect_rate, "lea rax, [rip + 1f]\n\tjmp rax\n\t1:")
BRANCH_KERNEL(callret_rate,  "call 1f\n\tjmp 2f\n\t1: ret\n\t2:")

static uint64_t imul_rate(int64_t ms)
{
    uint64_t x = 1, iters = 0;
    int64_t t0 = os64_micros(), t1 = t0;
    while (t1 - t0 < ms * 1000) {
        for (uint32_t i = 0; i < (1u << 18); i++)
            x = x * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;
        iters += 1u << 18;
        t1 = os64_micros();
    }
    __asm__ volatile("" :: "r"(x));
    return iters / (uint64_t)(t1 - t0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 's') {
        int64_t sec = (argc > 2) ? os64_atoi(argv[2]) : 30;
        uint8_t *l1 = os64_malloc(16384), *l2 = os64_malloc(262144), *mem = os64_malloc(64u << 20);
        if (l1 == NULL || l2 == NULL || mem == NULL) {
            os64_printf("coreprobe: cannot map the chase buffers\n");
            return 1;
        }
        int64_t start = os64_micros();
        while (os64_micros() - start < sec * 1000000) {
            os64_sleep(20);
            // Each kernel is bracketed by its own APIC reads: a line is one
            // core's only if every bracket agreed, else it is marked and the
            // numbers are nobody's. (A move away and back inside one 50 ms
            // kernel is the gap this cannot see.)
            uint32_t apic = apic_now();
            bool moved = false;
            uint64_t rate = imul_rate(50);      moved |= apic_now() != apic;
            uint64_t rt = taken_rate(50);       moved |= apic_now() != apic;
            uint64_t rnt = nottaken_rate(50);   moved |= apic_now() != apic;
            uint64_t rind = indirect_rate(50);  moved |= apic_now() != apic;
            uint64_t rcr = callret_rate(50);    moved |= apic_now() != apic;
            // Time since BOOT, so two samplers started apart share one clock.
            os64_printf("%ld apic %u%s imul %lu +taken %lu +nottaken %lu +indirect %lu +callret %lu\n",
                        os64_micros() / 1000, apic, moved ? "(moved)" : "",
                        rate, rt, rnt, rind, rcr);
        }
        return 0;
    }
    int64_t ms = (argc > 1) ? os64_atoi(argv[1]) : 500;
    if (ms <= 0)
        ms = 500;

    uint32_t a, b, c, d;
    char vendor[13] = {0}, brand[49] = {0};
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maxleaf = a;
    os64_memcpy(vendor, &b, 4); os64_memcpy(vendor + 4, &d, 4); os64_memcpy(vendor + 8, &c, 4);
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t maxext = a;
    if (maxext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            os64_memcpy(brand + i * 16, &a, 4); os64_memcpy(brand + i * 16 + 4, &b, 4);
            os64_memcpy(brand + i * 16 + 8, &c, 4); os64_memcpy(brand + i * 16 + 12, &d, 4);
        }
    }
    os64_printf("vendor %s  brand %s\n", vendor, brand);
    uint32_t apic0 = apic_now();
    os64_printf("apic %u", apic0);
    if (maxleaf >= 0xB) {
        cpuid(0xB, 0, &a, &b, &c, &d);
        os64_printf("  leaf0B: threads/core %u x2apic %u", b & 0xFFFF, d);
    }
    if (maxext >= 0x8000001E) {
        cpuid(0x8000001E, 0, &a, &b, &c, &d);
        os64_printf("  leaf8000001E: extapic %u core %u threads/core %u node %u",
                    a, b & 0xFF, ((b >> 8) & 0xFF) + 1, c & 0xFF);
    }
    os64_printf("\n");

    // imul chain
    {
        volatile uint64_t sink;
        uint64_t x = 1, iters = 0;
        int64_t t0 = os64_micros(), t1 = t0;
        while (t1 - t0 < ms * 1000) {
            for (uint32_t i = 0; i < (1u << 20); i++)
                x = x * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;
            iters += 1u << 20;
            t1 = os64_micros();
        }
        sink = x;
        (void)sink;
        uint64_t per_us = iters / (uint64_t)(t1 - t0);
        os64_printf("imul %lu iter/us (~%lu MHz if 4 cycles each)\n", per_us, per_us * 4);
    }

    // copies
    {
        size_t n = 8u << 20;
        uint8_t *src = os64_malloc(n), *dst = os64_malloc(n);
        if (src == NULL || dst == NULL) {
            os64_printf("coreprobe: cannot map 16 MB\n");
            return 1;
        }
        for (size_t i = 0; i < n; i += 4096) { src[i] = (uint8_t)i; dst[i] = 0; }
        uint64_t bytes = 0;
        int64_t t0 = os64_micros(), t1 = t0;
        while (t1 - t0 < ms * 1000) {
            os64_memcpy(dst, src, n);
            bytes += n;
            t1 = os64_micros();
        }
        os64_printf("memcpy %lu MB/s\n", bytes / (uint64_t)(t1 - t0));
        bytes = 0;
        t0 = os64_micros(); t1 = t0;
        while (t1 - t0 < ms * 1000) {
            copy_u32((uint32_t *)dst, (const uint32_t *)src, n / 4);
            bytes += n;
            t1 = os64_micros();
        }
        os64_printf("u32copy %lu MB/s\n", bytes / (uint64_t)(t1 - t0));
        os64_free(src);
        os64_free(dst);
    }

    // syscalls
    {
        uint64_t calls = 0;
        int64_t t0 = os64_micros(), t1 = t0;
        while (t1 - t0 < ms * 1000) {
            for (uint32_t i = 0; i < 1000; i++)
                t1 = os64_micros();
            calls += 1000;
        }
        os64_printf("syscalls %lu per ms\n", calls * 1000 / (uint64_t)(t1 - t0));
    }
    os64_printf("apic end %u\n", apic_now());
    return 0;
}
