// copy_fill.c — memcpy, memmove and memset, one source for ring 3 and ring 0.
// libos64 compiles it into str.c, whose header declares the os64_ names; the
// kernel builds it as an object of its own, -mno-sse at -O0, and calls the C
// library's names at the bottom. So it includes only the compiler's headers.

#include <stddef.h>
#include <stdint.h>

// THESE LOOPS ARE WHAT memset, memcpy AND memmove CALL, so they must never
// become calls to them. An optimizing GCC turns a byte-store loop into
// `call memset` (-ftree-loop-distribute-patterns, on at -O2); -ffreestanding
// stops it, but a hosted build of this file at -O2 — any host test that
// omits the flag — is then infinite recursion in the first memset, which a
// sanitizer's own memset hides. The attribute travels with the function, so
// no build of it has to remember.
#define OS64_NO_LIBCALL_LOOPS __attribute__((optimize("no-tree-loop-distribute-patterns")))

// How a copy or a fill moves its bytes, by size — measured, not guessed, on
// a Ryzen 9 3900X (Zen 2). Under 16 bytes, two moves of the widest size
// that fits, overlapping in the middle; then 16-byte moves, 64 bytes a pass
// past 64 with the stores aligned. The CPU's own string instructions start
// slowly but then move more a cycle than the passes can, and where they
// take over depends on what a 16-byte move is:
//
// - With SSE2 (ring 3, -O2) it is one register, so a copy of 2 KiB up to
//   128 KiB is `rep movsb` and a fill of 4 KiB or more is `rep stosb`. Past
//   128 KiB `rep movsb` falls behind SSE2 again on that machine — by more
//   than twice at 1 MiB — while `rep stosb` does not, hence a window for
//   the one and a floor for the other.
// - Without it (the kernel, -O0) GCC splits it into two general registers,
//   and the kernel's string forms (copy_rep, fill_rep) win, aligned or not,
//   from 512 bytes for a copy and 1.5 KiB for a fill, and at every size
//   measured above those: there is no SSE2 to fall behind, so neither has a
//   ceiling.
//
// No CPUID is asked, so one path serves every CPU; another machine's
// reading may move the cutoffs.
#ifdef __SSE2__
#define COPY_REP_FROM ((size_t)2 << 10)
#define COPY_REP_UNTIL ((size_t)128 << 10)
#define FILL_REP_FROM ((size_t)4 << 10)
#else
#define COPY_REP_FROM ((size_t)512)
#define COPY_REP_UNTIL SIZE_MAX
#define FILL_REP_FROM ((size_t)1536)
#endif

typedef uint8_t v16 __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint8_t v16_aligned __attribute__((vector_size(16), aligned(16), may_alias));
typedef uint64_t u64_any __attribute__((aligned(1), may_alias));
typedef uint32_t u32_any __attribute__((aligned(1), may_alias));
typedef uint16_t u16_any __attribute__((aligned(1), may_alias));

// The string instructions, as each build uses them, for at least the floor's
// bytes. Ring 3's are `rep movsb` and `rep stosb`, the measured winners on
// silicon. The kernel's move eight bytes a step, after single bytes up to
// the destination's first 8-byte boundary (unaligned, a 7.5 KiB copy ran 45%
// slower), which on the Zen 2 costs up to 40 TSC ticks a call more than the
// byte forms. That is paid because QEMU without KVM emulates a string
// instruction one step at a time and the kernel copies under locks with
// interrupts off: a byte a step made publish five times slower there than
// the loop it replaced, and eight a step made it faster. Both forms are
// defined step by step from the first byte, so a forward memmove may use
// either.
#ifdef __SSE2__
static inline void copy_rep(uint8_t *d, const uint8_t *s, size_t n)
{
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

static inline void fill_rep(uint8_t *d, uint8_t b, uint64_t w, size_t n)
{
    (void)w;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(b) : "memory");
}
#else
static void copy_rep(uint8_t *d, const uint8_t *s, size_t n)
{
    size_t head = -(uintptr_t)d & 7, quads = (n - head) >> 3, tail = (n - head) & 7;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(head) : : "memory");
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(quads) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
}

static void fill_rep(uint8_t *d, uint8_t b, uint64_t w, size_t n)
{
    size_t head = -(uintptr_t)d & 7, quads = (n - head) >> 3, tail = (n - head) & 7;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(head) : "a"(b) : "memory");
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(quads) : "a"(w) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(b) : "memory");
}
#endif

// Up to 64 bytes: moves of the widest size that fits from each end (two of
// them from each past 32), overlapping in the middle, all read before any
// is written — so it is right however the two regions overlap, which
// memmove relies on.
OS64_NO_LIBCALL_LOOPS
static inline void copy_short(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n > 32) {
        v16 a = *(const v16 *)s, b = *(const v16 *)(s + 16);
        v16 c = *(const v16 *)(s + n - 32), e = *(const v16 *)(s + n - 16);
        *(v16 *)d = a;
        *(v16 *)(d + 16) = b;
        *(v16 *)(d + n - 32) = c;
        *(v16 *)(d + n - 16) = e;
    } else if (n >= 16) {
        v16 a = *(const v16 *)s, b = *(const v16 *)(s + n - 16);
        *(v16 *)d = a;
        *(v16 *)(d + n - 16) = b;
    } else if (n >= 8) {
        uint64_t a = *(const u64_any *)s, b = *(const u64_any *)(s + n - 8);
        *(u64_any *)d = a;
        *(u64_any *)(d + n - 8) = b;
    } else if (n >= 4) {
        uint32_t a = *(const u32_any *)s, b = *(const u32_any *)(s + n - 4);
        *(u32_any *)d = a;
        *(u32_any *)(d + n - 4) = b;
    } else if (n >= 2) {
        uint16_t a = *(const u16_any *)s, b = *(const u16_any *)(s + n - 2);
        *(u16_any *)d = a;
        *(u16_any *)(d + n - 2) = b;
    } else if (n == 1) {
        *d = *s;
    }
}

// More than 64 bytes, first to last: the string copy in its window, else the
// first 16 and the last 64 read before anything is written, and between
// them passes that each read 64 and then write 64, the stores aligned. With
// `d` below `s` a pass's writes land wholly below the next pass's reads, so
// memmove may copy forward through here too.
OS64_NO_LIBCALL_LOOPS
static void copy_up(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n >= COPY_REP_FROM && n < COPY_REP_UNTIL) {
        copy_rep(d, s, n);
        return;
    }
    v16 head = *(const v16 *)s;
    v16 t0 = *(const v16 *)(s + n - 64), t1 = *(const v16 *)(s + n - 48);
    v16 t2 = *(const v16 *)(s + n - 32), t3 = *(const v16 *)(s + n - 16);
    size_t skip = -(uintptr_t)d & 15;           // to the first aligned store
    uint8_t *to = d + skip;
    const uint8_t *from = s + skip;
    for (size_t left = n - skip; left > 64; left -= 64, to += 64, from += 64) {
        v16 a = *(const v16 *)from, b = *(const v16 *)(from + 16);
        v16 c = *(const v16 *)(from + 32), e = *(const v16 *)(from + 48);
        *(v16_aligned *)to = a;
        *(v16_aligned *)(to + 16) = b;
        *(v16_aligned *)(to + 32) = c;
        *(v16_aligned *)(to + 48) = e;
    }
    *(v16 *)d = head;
    *(v16 *)(d + n - 64) = t0;
    *(v16 *)(d + n - 48) = t1;
    *(v16 *)(d + n - 32) = t2;
    *(v16 *)(d + n - 16) = t3;
}

// More than 64 bytes, last to first, for memmove when `d` lies inside the
// source: copy_up mirrored. Each pass's writes land wholly above the next
// pass's reads.
OS64_NO_LIBCALL_LOOPS
static void copy_down(uint8_t *d, const uint8_t *s, size_t n)
{
    v16 tail = *(const v16 *)(s + n - 16);
    v16 h0 = *(const v16 *)s, h1 = *(const v16 *)(s + 16);
    v16 h2 = *(const v16 *)(s + 32), h3 = *(const v16 *)(s + 48);
    size_t skip = (uintptr_t)(d + n) & 15;      // to the end of the last aligned store
    uint8_t *to = d + n - skip;
    const uint8_t *from = s + n - skip;
    for (size_t left = n - skip; left > 64; left -= 64, to -= 64, from -= 64) {
        v16 a = *(const v16 *)(from - 16), b = *(const v16 *)(from - 32);
        v16 c = *(const v16 *)(from - 48), e = *(const v16 *)(from - 64);
        *(v16_aligned *)(to - 16) = a;
        *(v16_aligned *)(to - 32) = b;
        *(v16_aligned *)(to - 48) = c;
        *(v16_aligned *)(to - 64) = e;
    }
    *(v16 *)(d + n - 16) = tail;
    *(v16 *)d = h0;
    *(v16 *)(d + 16) = h1;
    *(v16 *)(d + 32) = h2;
    *(v16 *)(d + 48) = h3;
}

OS64_NO_LIBCALL_LOOPS
void *os64_memcpy(void *dst, const void *src, size_t n)
{
    if (n <= 64)
        copy_short(dst, src, n);
    else
        copy_up(dst, src, n);
    return dst;
}

// Forward unless `dst` lies inside the source past its start, where a
// forward copy would write bytes not yet read: the subtraction wraps when
// `dst` is below `src`, so one comparison asks both.
OS64_NO_LIBCALL_LOOPS
void *os64_memmove(void *dst, const void *src, size_t n)
{
    if (n <= 64)
        copy_short(dst, src, n);
    else if ((uintptr_t)dst - (uintptr_t)src >= n)
        copy_up(dst, src, n);
    else if (dst != src)
        copy_down(dst, src, n);
    return dst;
}

// copy_short's and copy_up's shapes, one byte everywhere, and the string
// fill from its floor (the cutoffs above).
OS64_NO_LIBCALL_LOOPS
void *os64_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst, b = (uint8_t)c;
    uint64_t w = b * 0x0101010101010101ull;
    v16 v = {0};
    v += b;
    if (n >= FILL_REP_FROM) {
        fill_rep(d, b, w, n);
    } else if (n > 64) {
        *(v16 *)d = v;
        uint8_t *to = d + (-(uintptr_t)d & 15);
        for (size_t left = n - (size_t)(to - d); left > 64; left -= 64, to += 64) {
            *(v16_aligned *)to = v;
            *(v16_aligned *)(to + 16) = v;
            *(v16_aligned *)(to + 32) = v;
            *(v16_aligned *)(to + 48) = v;
        }
        *(v16 *)(d + n - 64) = v;
        *(v16 *)(d + n - 48) = v;
        *(v16 *)(d + n - 32) = v;
        *(v16 *)(d + n - 16) = v;
    } else if (n > 32) {
        *(v16 *)d = v;
        *(v16 *)(d + 16) = v;
        *(v16 *)(d + n - 32) = v;
        *(v16 *)(d + n - 16) = v;
    } else if (n >= 16) {
        *(v16 *)d = v;
        *(v16 *)(d + n - 16) = v;
    } else if (n >= 8) {
        *(u64_any *)d = w;
        *(u64_any *)(d + n - 8) = w;
    } else if (n >= 4) {
        *(u32_any *)d = (uint32_t)w;
        *(u32_any *)(d + n - 4) = (uint32_t)w;
    } else if (n >= 2) {
        *(u16_any *)d = (uint16_t)w;
        *(u16_any *)(d + n - 2) = (uint16_t)w;
    } else if (n == 1) {
        *d = b;
    }
    return dst;
}

// ── the C library's names ───────────────────────────────────────────────────
// GCC's freestanding contract (documented, and non-negotiable): even under
// -ffreestanding the compiler may emit CALLS to memcpy, memmove, memset,
// and memcmp whenever it likes — a struct assignment, a big initializer, an
// array copy. scribe's save writer found the hole (a 64KB `= { ... }`
// emitted a memset call ring 3 didn't link). Ring 3 says os64_memset and
// friends, and has the bare names only for code the compiler writes on its
// behalf; the kernel calls them by these names. memcmp is neither a copy
// nor a fill, so each world keeps its own.

void *memset(void *dst, int c, size_t n)
{
    return os64_memset(dst, c, n);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return os64_memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return os64_memmove(dst, src, n);
}
