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

void *os64_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
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
