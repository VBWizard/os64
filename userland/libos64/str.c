#include "os64/utf8_scalar.h"
// str.c — libos64 string primitives. Contract and the case against strcpy
// live in <os64/str.h>; this file is just the doing.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/str.h"

size_t os64_strlen(const char *s)
{
    size_t n = 0;

    if (s == NULL)
        return 0;
    while (s[n] != '\0')
        n++;
    return n;
}

size_t os64_strcopy(char *dst, size_t cap, const char *src)
{
    size_t srclen = os64_strlen(src);

    if (dst == NULL || cap == 0)
        return srclen;      // nowhere to put it; still answer how big it was

    // Copy at most cap-1 bytes, then terminate unconditionally. The
    // termination is the whole point: a caller that ignores the return value
    // still gets a valid string, just a shorter one than it asked for. Silent
    // truncation is a bug; silent NON-termination is a crash somewhere else
    // entirely, hours later, in code that did nothing wrong.
    size_t n = (srclen < cap - 1) ? srclen : cap - 1;
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
    dst[n] = '\0';

    return srclen;
}

bool os64_streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;        // both at their NUL == same string
}

int os64_strcmp(const char *a, const char *b)
{
    const unsigned char *left = (const unsigned char *)a;
    const unsigned char *right = (const unsigned char *)b;
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return (int)*left - (int)*right;
}

char *os64_strchr(const char *s, int c)
{
    unsigned char byte = (unsigned char)c;
    for (;;) {
        if ((unsigned char)*s == byte)
            return (char *)s;
        if (!*s)
            return NULL;
        s++;
    }
}

char *os64_strrchr(const char *s, int c)
{
    unsigned char byte = (unsigned char)c;
    const char *last = NULL;
    do {
        if ((unsigned char)*s == byte)
            last = s;
    } while (*s++);
    return (char *)last;
}

void *os64_memchr(const void *block, int c, size_t n)
{
    const unsigned char *bytes = (const unsigned char *)block;
    unsigned char byte = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        if (bytes[i] == byte)
            return (void *)(bytes + i);
    return NULL;
}

// ASCII only, deliberately: os64 has no locale and a config key is written in
// the same 26 letters everywhere. A table-driven fold would be a lie about
// capabilities this system does not have.
static inline char fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool os64_streq_nocase(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    while (*a != '\0' && fold(*a) == fold(*b))
    {
        a++;
        b++;
    }
    return fold(*a) == fold(*b);
}

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
// that fits, overlapping in the middle; then 16-byte SSE2 moves, which every
// x86-64 has, 64 bytes a pass past 64 with the stores aligned. The CPU's own
// string instructions start slowly but then move more a cycle than SSE2
// can, so a copy of 2 KiB up to 128 KiB is `rep movsb` and a fill of 4 KiB
// or more is `rep stosb`. Past 128 KiB `rep movsb` falls behind SSE2 again
// on that machine — by more than twice at 1 MiB — while `rep stosb` does
// not, hence a window for the one and a floor for the other. No CPUID is
// asked, so one path serves every CPU; another machine's reading may move
// the cutoffs.
#define COPY_REP_FROM ((size_t)2 << 10)
#define COPY_REP_UNTIL ((size_t)128 << 10)
#define FILL_REP_FROM ((size_t)4 << 10)

typedef uint8_t v16 __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint8_t v16_aligned __attribute__((vector_size(16), aligned(16), may_alias));
typedef uint64_t u64_any __attribute__((aligned(1), may_alias));
typedef uint32_t u32_any __attribute__((aligned(1), may_alias));
typedef uint16_t u16_any __attribute__((aligned(1), may_alias));

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

// More than 64 bytes, first to last: `rep movsb` in its window, else the
// first 16 and the last 64 read before anything is written, and between
// them passes that each read 64 and then write 64, the stores aligned. With
// `d` below `s` a pass's writes land wholly below the next pass's reads, and
// `rep movsb` is defined byte by byte from the first, so memmove may copy
// forward through here too.
OS64_NO_LIBCALL_LOOPS
static void copy_up(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n >= COPY_REP_FROM && n < COPY_REP_UNTIL) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
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

// ── the compiler's four ─────────────────────────────────────────────────────
// GCC's freestanding contract (documented, and non-negotiable): even under
// -ffreestanding the compiler may emit CALLS to memcpy, memmove, memset,
// and memcmp whenever it likes — a struct assignment, a big initializer, an
// array copy. scribe's save writer found the hole (a 64KB `= { ... }`
// emitted a memset call this world didn't link). These four exist to honor
// that contract, NOT as API: os64 code says os64_memset and friends; the
// bare names are for code the compiler writes on our behalf.

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

int memcmp(const void *a, const void *b, size_t n)
{
    return os64_memcmp(a, b, n);
}

int os64_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    for (size_t i = 0; i < n; i++)
        if (pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

// copy_short's and copy_up's shapes, one byte everywhere, and `rep stosb`
// from its floor (the cutoffs above).
OS64_NO_LIBCALL_LOOPS
void *os64_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst, b = (uint8_t)c;
    uint64_t w = b * 0x0101010101010101ull;
    v16 v = {0};
    v += b;
    if (n >= FILL_REP_FROM) {
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(b) : "memory");
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

int64_t os64_atoi(const char *s)
{
    int64_t sign = 1;
    int64_t v = 0;

    if (s == NULL)
        return 0;
    if (*s == '+' || *s == '-')
    {
        if (*s == '-')
            sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

uint64_t os64_atou(const char *s)
{
    uint64_t v = 0;

    if (s == NULL)
        return 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

bool os64_parse_u64(const char *s, uint64_t *out)
{
    if (s == NULL || out == NULL || *s == '\0')
        return false;

    uint64_t value = 0;
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    *out = value;
    return true;
}

// One hex digit's value, or -1 if the character isn't one. Kept separate so
// the "is it a digit" test and the "what is it worth" conversion are the same
// question asked once — the classic hand-rolled hex loop asks it twice and
// gets the two out of step exactly often enough to matter.
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Contract, and the argument for the escaping, in str.h.
void os64_unescape_field(char *s)
{
    char *out;
    const char *in;

    if (s == NULL)
        return;

    for (in = s, out = s; *in != '\0'; )
    {
        if (*in != '\\')
        {
            *out++ = *in++;
            continue;
        }
        if (in[1] == '\\')
        {
            *out++ = '\\';
            in += 2;
            continue;
        }
        if (in[1] == 'x')
        {
            int hi = hex_digit(in[2]);
            int lo = (hi >= 0) ? hex_digit(in[3]) : -1;
            if (lo >= 0)
            {
                *out++ = (char)((hi << 4) | lo);
                in += 4;
                continue;
            }
        }
        // Not an escape this encoder produces. Pass the backslash through
        // rather than guess — a report that says something we cannot read is
        // better read literally than read wrong.
        *out++ = *in++;
    }
    *out = '\0';
}

uint64_t os64_xtou(const char *s, const char **end)
{
    uint64_t v = 0;

    if (s == NULL)
    {
        if (end != NULL)
            *end = NULL;
        return 0;
    }

    const char *p = s;

    // An optional 0x, accepted but never REQUIRED — the kernel's %p prints
    // bare hex, and a parser that insisted on the prefix would reject every
    // string it exists to read.
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hex_digit(p[2]) >= 0)
        p += 2;

    // Remember where the digits began: "0x" alone is not a number, and the
    // no-digits verdict has to survive our having stepped over a prefix.
    const char *digits = p;

    int d;
    while ((d = hex_digit(*p)) >= 0)
    {
        v = v * 16 + (uint64_t)d;
        p++;
    }

    if (end != NULL)
        *end = (p == digits) ? s : p;   // no digits: report NOTHING consumed,
                                        // pointing at the original start
    return (p == digits) ? 0 : v;
}

bool os64_parse_range(const char *s, uint64_t *lo, uint64_t *hi)
{
    if (s == NULL)
        return false;

    const char *after_lo = NULL;
    uint64_t low = os64_xtou(s, &after_lo);
    if (after_lo == s || *after_lo != '-')
        return false;               // no first number, or no separator

    const char *after_hi = NULL;
    const char *second = after_lo + 1;
    uint64_t high = os64_xtou(second, &after_hi);
    if (after_hi == second)
        return false;               // a '-' with nothing behind it

    // Both halves are real: publish together, so a caller can never read one
    // fresh value beside one stale one.
    if (lo != NULL) *lo = low;
    if (hi != NULL) *hi = high;
    return true;
}

// ── UTF-8 (str.h says who this is for) ──────────────────────────────────

size_t os64_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
    return os64_utf8_scalar(s, n, cp);
}

size_t os64_utf8_encode(uint32_t cp, char *out)
{
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        cp = OS64_UTF8_REPLACEMENT;
    unsigned char *o = (unsigned char *)out;
    if (cp < 0x80) {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

bool os64_glob_match(const char *pattern, const char *text)
{
    if (pattern == NULL || text == NULL)
        return false;

    while (*pattern != '\0')
    {
        if (*pattern == '*')
        {
            pattern++;
            if (*pattern == '\0')
                return true;                  // trailing '*' takes the rest
            for (const char *candidate = text;; candidate++)
            {
                if (os64_glob_match(pattern, candidate))
                    return true;
                if (*candidate == '\0')
                    return false;
            }
        }
        if (*text == '\0')
            return false;
        if (*pattern == '?')
        {
            pattern++;
            text++;
            continue;
        }
        if (*pattern == '[')
        {
            const char *member = pattern + 1;
            bool negate = *member == '!' || *member == '^';
            if (negate)
                member++;
            bool matched = false;
            bool first = true;
            // `first` lets a ']' immediately after '[' (or after '!') be a
            // literal member, the way every shell since the Bourne shell does.
            while (*member != '\0' && (*member != ']' || first))
            {
                first = false;
                if (member[1] == '-' && member[2] != '\0' &&
                    member[2] != ']')
                {
                    if ((unsigned char)*text >= (unsigned char)member[0] &&
                        (unsigned char)*text <= (unsigned char)member[2])
                        matched = true;
                    member += 3;
                }
                else
                {
                    if (*text == *member)
                        matched = true;
                    member++;
                }
            }
            if (*member != ']')
            {
                // No closing ']': this '[' is an ordinary character, so a
                // pattern can name /bin/[ (husk's rule, and it has to be the
                // same rule here — husk's own expansion runs through this).
                if (*pattern != *text)
                    return false;
                pattern++;
                text++;
                continue;
            }
            if (matched == negate)
                return false;
            pattern = member + 1;
            text++;
            continue;
        }
        if (*pattern != *text)
            return false;
        pattern++;
        text++;
    }
    return *text == '\0';
}
