// str.c — libos64 string primitives. Contract and the case against strcpy
// live in <os64/str.h>; this file is just the doing.

#include <stdbool.h>
#include <stddef.h>

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

void *os64_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *os64_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    // dst below src: copy forward. dst above: copy backward, so the bytes
    // not yet read are never the bytes already written. Equal: nothing to do.
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
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
    // No os64_ twin yet — nothing human has asked for one (the streq family
    // covers strings). The compiler is memcmp's only client today, so the
    // body lives here until a real consumer promotes it.
    const unsigned char *pa = a, *pb = b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

void *os64_memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
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
