// copybench — what a syscall's copy costs, with no file system and no
// scheduler under it (docs/design/pending/USER_COPY.md § The proof).
//
// Run the same binary under two kernels and compare: it touches only the
// pipe and the clock, so it runs unchanged on a kernel without the user
// copy routines.
//
//   1. Small copies: os64_ticks, which copies a 16-byte struct out, against
//      os64_taskid, which copies nothing; the difference is the copy and
//      its check.
//   2. Large copies: one thread writes N bytes into an empty pipe and reads
//      them back, so each round trip is one copy in and one copy out, plus
//      the pipe's own copies into and out of its ring, and never blocks.
//      N from 64 bytes to the pipe's 64 KiB, the user buffer at page
//      offset 0.
//   3. The relationship within a page: the 64 KiB round trip with the user
//      buffer at every page offset 0..4095. The kernel's copy buffer sits
//      at one fixed page offset, so this crosses every (dst - src) & 0xFFF
//      in both directions, which is the condition some cores' string moves
//      are slow under. It prints the median, the slowest offsets, and how
//      many are over twice the median.
//
// Every figure is a median (the offset sweep: the best of three per offset,
// then the median of those), in nanoseconds from the TSC, whose rate is
// measured against micros() first.
//
//   copybench            all three
//   copybench quick      fewer rounds

#include "os64/os64.h"

#define PIPE_BYTES (64u * 1024u)
#define ROUNDS     201u

static int32_t s_pipe[2];
static uint8_t s_buf[PIPE_BYTES + 8192] __attribute__((aligned(4096)));
static uint8_t s_back[PIPE_BYTES + 8192] __attribute__((aligned(4096)));
static double s_ns_per_tick;

static uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void sort_u64(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint64_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
}

static uint64_t median(uint64_t *v, unsigned n)
{
    sort_u64(v, n);
    return v[n / 2];
}

static unsigned ns(uint64_t ticks)
{
    return (unsigned)((double)ticks * s_ns_per_tick + 0.5);
}

// One write of n bytes from `from` into the empty pipe and one read of them
// back into `to`; its TSC ticks, or 0 when the pipe misbehaved.
static uint64_t round_trip(const uint8_t *from, uint8_t *to, uint32_t n)
{
    uint64_t t0 = tsc();
    int64_t w = os64_write(s_pipe[1], from, n);
    int64_t r = os64_read(s_pipe[0], to, n);
    uint64_t t = tsc() - t0;
    return (w == (int64_t)n && r == (int64_t)n) ? t : 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = (argc > 1 && argv[1][0] == 'q') ? 41 : ROUNDS;
    static uint64_t v[ROUNDS];

    // The TSC rate against micros(), over a quarter of a second.
    int64_t m0 = os64_micros();
    uint64_t c0 = tsc();
    while (os64_micros() - m0 < 250000)
        ;
    int64_t m1 = os64_micros();
    uint64_t c1 = tsc();
    s_ns_per_tick = (double)(m1 - m0) * 1000.0 / (double)(c1 - c0);
    os64_printf("copybench: TSC %u MHz\n", (unsigned)(1000.0 / s_ns_per_tick));

    if (os64_pipe(s_pipe) < 0) {
        os64_printf("copybench: no pipe\n");
        return 1;
    }
    for (unsigned i = 0; i < sizeof(s_buf); i++)
        s_buf[i] = (uint8_t)(i * 7 + 1);

    // 1. Small copies.
    os64_ticks_t tk;
    uint64_t none[ROUNDS], small[ROUNDS];
    for (unsigned r = 0; r < rounds; r++) {
        uint64_t t0 = tsc();
        for (int i = 0; i < 100; i++)
            (void)os64_taskid();
        none[r] = tsc() - t0;
        t0 = tsc();
        for (int i = 0; i < 100; i++)
            (void)os64_ticks(&tk);
        small[r] = tsc() - t0;
    }
    uint64_t mn = median(none, rounds), ms = median(small, rounds);
    os64_printf("copybench: a syscall that copies nothing %u ns, one that copies %u bytes out %u ns (%d ns for the copy and its check)\n",
                ns(mn) / 100, (unsigned)sizeof(tk), ns(ms) / 100, (int)(ns(ms) / 100) - (int)(ns(mn) / 100));

    // 2. Large copies at page offset 0.
    static const uint32_t sizes[] = { 64, 256, 1024, 4096, 16384, 32768, 65536 };
    os64_printf("copybench: pipe round trip (one write, one read), user buffer at page offset 0\n");
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint32_t n = sizes[s];
        unsigned got = 0;
        for (unsigned r = 0; r < rounds; r++) {
            uint64_t t = round_trip(s_buf, s_back, n);
            if (t != 0)
                v[got++] = t;
        }
        if (got == 0) {
            os64_printf("copybench: the pipe refused %u bytes\n", n);
            return 2;
        }
        uint64_t m = median(v, got);
        unsigned mbps = (unsigned)((double)n * 2.0 / ((double)ns(m) / 1e9) / 1e6);
        os64_printf("  %6u bytes: %7u ns  (%u MB/s through the two copies and the ring)\n", n, ns(m), mbps);
    }
    if (os64_memcmp(s_buf, s_back, PIPE_BYTES) != 0) {
        os64_printf("copybench: FAIL - the bytes read back differ\n");
        return 3;
    }

    // 3. Every page offset of the user buffer, 64 KiB each way.
    static uint64_t by_off[4096];
    uint64_t sorted[4096];
    for (unsigned off = 0; off < 4096; off++) {
        uint64_t best = UINT64_MAX;
        for (int r = 0; r < 3; r++) {
            uint64_t t = round_trip(s_buf + off, s_back + off, PIPE_BYTES);
            if (t != 0 && t < best)
                best = t;
        }
        by_off[off] = best;
        sorted[off] = best;
    }
    uint64_t med = median(sorted, 4096);
    unsigned over = 0;
    for (unsigned off = 0; off < 4096; off++)
        if (by_off[off] > 2 * med)
            over++;
    os64_printf("copybench: 64 KiB round trip over all 4096 page offsets: median %u ns, fastest %u, slowest %u; %u offsets over twice the median\n",
                ns(med), ns(sorted[0]), ns(sorted[4095]), over);
    os64_printf("copybench: the slowest offsets:");
    for (int k = 0; k < 8; k++) {
        unsigned worst = 0;
        for (unsigned off = 1; off < 4096; off++)
            if (by_off[off] > by_off[worst])
                worst = off;
        os64_printf(" %u (%u ns)", worst, ns(by_off[worst]));
        by_off[worst] = 0;
    }
    os64_printf("\n");
    return 0;
}
