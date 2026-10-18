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
// To measure one core on purpose rather than wherever the scheduler put it:
// `env PINCORE=<apic> coreprobe`.
//
// coreprobe [ms]     — how long each measurement runs (default 500).
// coreprobe -s [sec] — SAMPLE instead: for that many seconds, sleep a
//                      little, then time the imul chain and the chain with
//                      one extra branch of each kind (taken, never-taken,
//                      indirect, call/ret), 50 ms each, and print one line —
//                      `<ms since boot> apic <n> imul <rate> +taken <rate> ...`
//                      — so a thread the scheduler moves about reports every
//                      core it visits and what a branch costs there.
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
    // Every measurement below is bracketed by its own APIC reads and says
    // so if the scheduler moved the task during it, the sampler's rule: a
    // number from two cores is nobody's number.
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
        os64_printf("imul %lu iter/us (~%lu MHz if 4 cycles each)%s\n", per_us, per_us * 4,
                    apic_now() == apic0 ? "" : " (moved)");
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
        os64_printf("memcpy %lu MB/s%s\n", bytes / (uint64_t)(t1 - t0),
                    apic_now() == apic0 ? "" : " (moved)");
        bytes = 0;
        t0 = os64_micros(); t1 = t0;
        while (t1 - t0 < ms * 1000) {
            copy_u32((uint32_t *)dst, (const uint32_t *)src, n / 4);
            bytes += n;
            t1 = os64_micros();
        }
        os64_printf("u32copy %lu MB/s%s\n", bytes / (uint64_t)(t1 - t0),
                    apic_now() == apic0 ? "" : " (moved)");
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
        os64_printf("syscalls %lu per ms%s\n", calls * 1000 / (uint64_t)(t1 - t0),
                    apic_now() == apic0 ? "" : " (moved)");
    }
    os64_printf("apic end %u\n", apic_now());
    return 0;
}
