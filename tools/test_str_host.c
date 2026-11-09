// The copy and fill (shared/copy_fill.c) against a byte-by-byte reference,
// once per build, since ring 3 and the kernel change method at different
// sizes: every length to 300 at every alignment of both ends, the lengths
// either side of each build's cutoffs, and memmove overlapping by every
// shift either way — each checked for the bytes it wrote, the bytes either
// side it must not have, and the pointer it returns.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *os64_memcpy(void *, const void *, size_t);
void *os64_memmove(void *, const void *, size_t);
void *os64_memset(void *, int, size_t);

static int s_checks, s_failed;

static void expect(const char *what, size_t n, size_t a, size_t b, int ok)
{
    s_checks++;
    if (!ok && s_failed++ < 20)
        fprintf(stderr, "FAIL %s: n %zu, offsets %zu %zu\n", what, n, a, b);
}

#define GUARD 64
#define BIG ((size_t)1 << 21)

static uint8_t *s_src, *s_dst, *s_ref;

static void pattern(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 7 + seed + (i >> 8) * 13);
}

// `n` bytes from s_src + a to s_dst + b, the rest of s_dst a known byte.
static void copy_case(size_t n, size_t a, size_t b)
{
    memset(s_dst, 0xee, b + n + 2 * GUARD);
    uint8_t *to = s_dst + GUARD + b, *from = s_src + GUARD + a;
    int back = os64_memcpy(to, from, n) == to;
    int same = memcmp(to, from, n) == 0;
    int kept = 1;
    for (size_t i = 0; i < GUARD + b; i++)
        kept &= s_dst[i] == 0xee;
    for (size_t i = 0; i < GUARD; i++)
        kept &= to[n + i] == 0xee;
    expect("memcpy copies every byte", n, a, b, same);
    expect("memcpy writes nothing outside", n, a, b, kept);
    expect("memcpy returns where it wrote", n, a, b, back);
}

static void fill_case(size_t n, size_t b, int c)
{
    memset(s_dst, 0xee, b + n + 2 * GUARD);
    uint8_t *to = s_dst + GUARD + b;
    int back = os64_memset(to, c, n) == to;
    int same = 1, kept = 1;
    for (size_t i = 0; i < n; i++)
        same &= to[i] == (uint8_t)c;
    for (size_t i = 0; i < GUARD + b; i++)
        kept &= s_dst[i] == 0xee;
    for (size_t i = 0; i < GUARD; i++)
        kept &= to[n + i] == 0xee;
    expect("memset fills every byte with c's low byte", n, b, (size_t)c, same);
    expect("memset writes nothing outside", n, b, (size_t)c, kept);
    expect("memset returns where it wrote", n, b, (size_t)c, back);
}

// `n` bytes moved by `shift` within one buffer, from `a` past its start.
static void move_case(size_t n, size_t a, long shift)
{
    size_t span = n + 2 * GUARD + (size_t)labs(shift) + a + 16;
    pattern(s_dst, span, (unsigned)n);
    memcpy(s_ref, s_dst, span);
    uint8_t *from = s_dst + GUARD + 8 + a + (shift < 0 ? (size_t)-shift : 0);
    uint8_t *to = from + shift;
    size_t f = (size_t)(from - s_dst), t = (size_t)(to - s_dst);
    for (size_t i = 0; i < n; i++)          // the reference: through a copy
        s_ref[span + i] = s_ref[f + i];
    for (size_t i = 0; i < n; i++)
        s_ref[t + i] = s_ref[span + i];
    int back = os64_memmove(to, from, n) == to;
    expect("memmove moves every byte, overlapping", n, a, (size_t)shift, memcmp(s_dst, s_ref, span) == 0);
    expect("memmove returns where it wrote", n, a, (size_t)shift, back);
}

int main(void)
{
    s_src = malloc(BIG + 4 * GUARD);
    s_dst = malloc(BIG + 4 * GUARD);
    s_ref = malloc(2 * (BIG + 4 * GUARD));
    if (s_src == NULL || s_dst == NULL || s_ref == NULL)
        return 2;
    pattern(s_src, BIG + 4 * GUARD, 3);
    for (size_t n = 0; n <= 300; n++)
        for (size_t a = 0; a < 16; a++)
            for (size_t b = 0; b < 16; b++)
                copy_case(n, a, b);
    // Either side of where the method changes, and well past the last.
    static const size_t kEdges[] = {511, 512, 513, 1535, 1536, 1537, 2047, 2048, 2049, 4095, 4096,
                                    4097, 131071, 131072, 131073, 200001, (size_t)1 << 20};
    for (size_t k = 0; k < sizeof kEdges / sizeof *kEdges; k++)
        for (size_t a = 0; a < 16; a += 5)
            for (size_t b = 0; b < 16; b += 3) {
                copy_case(kEdges[k], a, b);
                fill_case(kEdges[k], b, 0x5a);
            }
    static const int kBytes[] = {0, 0x5a, 0xff, 0x1a5, -1};
    for (size_t n = 0; n <= 300; n++)
        for (size_t b = 0; b < 16; b++)
            for (size_t k = 0; k < sizeof kBytes / sizeof *kBytes; k++)
                fill_case(n, b, kBytes[k]);
    for (size_t n = 0; n <= 200; n++)
        for (long shift = -80; shift <= 80; shift++)
            move_case(n, n % 16, shift);
    for (size_t k = 0; k < sizeof kEdges / sizeof *kEdges; k++)
        for (long shift = -130; shift <= 130; shift += 13)
            move_case(kEdges[k], k, shift == 0 ? 1 : shift);
    // Overlapping by less than a word, where a string copy that moves eight
    // bytes a step reads what an earlier step of the same instruction wrote.
    for (size_t k = 0; k < sizeof kEdges / sizeof *kEdges; k++)
        for (long shift = -9; shift <= 9; shift++)
            if (shift != 0)
                move_case(kEdges[k], k, shift);
    printf("str: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
