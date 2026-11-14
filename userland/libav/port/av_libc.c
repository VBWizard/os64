// av_libc.c — the C-library functions FFmpeg's sources call that libos64
// does not have. PURE: no os64 header, so tools/test_av_libc_host.sh builds
// it on the host and compares every function against glibc's.
//
// Each is the standard's function as FFmpeg uses it, not a general libc:
// strftime knows the conversions FFmpeg writes, gmtime and mktime are UTC,
// and nothing here allocates. The state kept is `avlibc_errno` and the one
// struct gmtime answers with, which C's gmtime keeps too.

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "av_libc.h"

int avlibc_errno;

#define AV_LIBC_ERANGE 34   // errno.h's ERANGE, which a host build cannot include beside glibc's

// ---------------------------------------------------------------- strings

int avlibc_strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; a++, b++, n--) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (*a == 0) return 0;
    }
    return 0;
}

char *avlibc_strstr(const char *haystack, const char *needle)
{
    if (*needle == 0) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (*n == 0) return (char *)haystack;
    }
    return NULL;
}

static int in_set(char c, const char *set)
{
    for (; *set; set++)
        if (*set == c) return 1;
    return 0;
}

size_t avlibc_strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && in_set(s[n], accept)) n++;
    return n;
}

size_t avlibc_strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !in_set(s[n], reject)) n++;
    return n;
}

char *avlibc_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {}
    return dst;
}

// FFmpeg's av_strerror answers from its own table first; this is what it
// falls back to for a number outside it.
char *avlibc_strerror(int code)
{
    (void)code;
    return (char *)"Unknown error";
}

// ---------------------------------------------------------------- numbers

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

static int is_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// The one walk behind all four strto* integer functions: C's rules for the
// prefix, the base and `end`, with the magnitude saturated at `limit` and
// ERANGE set when it is. `negative` is reported, not applied.
static unsigned long long scan_integer(const char *s, char **end, int base,
                                       unsigned long long limit, int *negative, int *overflow)
{
    const char *p = s;
    *negative = 0;
    *overflow = 0;
    while (is_space(*p)) p++;
    if (*p == '+' || *p == '-') *negative = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_value(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (end) *end = (char *)s;
        return 0;
    }
    const char *digits = p;
    unsigned long long value = 0;
    for (int d; (d = digit_value(*p)) < base; p++) {
        if (*overflow) continue;
        if (value > (limit - (unsigned)d) / (unsigned)base) {
            *overflow = 1;
            value = limit;
        } else {
            value = value * (unsigned)base + (unsigned)d;
        }
    }
    if (end) *end = (char *)(p == digits ? s : p);
    if (*overflow) avlibc_errno = AV_LIBC_ERANGE;
    return value;
}

long long avlibc_strtoll(const char *s, char **end, int base)
{
    int negative, overflow;
    unsigned long long magnitude = scan_integer(s, end, base, (unsigned long long)LLONG_MAX + 1,
                                                &negative, &overflow);
    if (!negative && magnitude > (unsigned long long)LLONG_MAX) {
        avlibc_errno = AV_LIBC_ERANGE;
        return LLONG_MAX;
    }
    return negative ? (long long)(0 - magnitude) : (long long)magnitude;
}

unsigned long long avlibc_strtoull(const char *s, char **end, int base)
{
    int negative, overflow;
    unsigned long long magnitude = scan_integer(s, end, base, ULLONG_MAX, &negative, &overflow);
    if (overflow) return ULLONG_MAX;
    return negative ? 0 - magnitude : magnitude;
}

long avlibc_strtol(const char *s, char **end, int base)
{
    return (long)avlibc_strtoll(s, end, base);   // long is 64 bits on x86-64
}

unsigned long avlibc_strtoul(const char *s, char **end, int base)
{
    return (unsigned long)avlibc_strtoull(s, end, base);
}

// frexp and scalbn by the bits of the double: exact, as the standard asks.
double avlibc_frexp(double x, int *exponent)
{
    union { double d; uint64_t u; } v = { x };
    int e = (int)(v.u >> 52 & 0x7ff);
    if (e == 0) {
        if (x == 0) {
            *exponent = 0;
            return x;
        }
        x = avlibc_frexp(x * 0x1p64, exponent);   // subnormal: scale into range first
        *exponent -= 64;
        return x;
    }
    if (e == 0x7ff) {            // infinity or NaN comes back as itself
        *exponent = 0;
        return x;
    }
    *exponent = e - 1022;
    v.u = (v.u & 0x800fffffffffffffull) | 0x3fe0000000000000ull;
    return v.d;
}

double avlibc_scalbn(double x, int n)
{
    // musl's shape: multiply by powers of two in steps that cannot overflow
    // or lose bits early, so the one rounding happens last.
    union { double d; uint64_t u; } scale;
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            x *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {
        x *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    scale.u = (uint64_t)(0x3ff + n) << 52;
    return x * scale.d;
}

// ---------------------------------------------------------------- sorting

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(unsigned char *base, size_t root, size_t count, size_t size,
                      int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= count) return;
        if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(base + root * size, base + child * size) >= 0) return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

// A heapsort: O(n log n) at worst, no recursion, no allocation. qsort
// promises no stability and FFmpeg relies on none.
void avlibc_qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    unsigned char *b = base;
    if (count < 2 || size == 0) return;
    for (size_t i = count / 2; i-- > 0;) sift_down(b, i, count, size, compare);
    for (size_t end = count - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift_down(b, 0, end, size, compare);
    }
}

void *avlibc_bsearch(const void *key, const void *base, size_t count, size_t size,
                      int (*compare)(const void *, const void *))
{
    const unsigned char *b = base;
    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int c = compare(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

// ---------------------------------------------------------------- time

// Days since 1970-01-01 of a proleptic Gregorian date, and back (Howard
// Hinnant's civil-from-days, exact for every int64 day count used here).
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static struct avlibc_tm s_gmtime;

struct avlibc_tm *avlibc_gmtime(const int64_t *t)
{
    int64_t secs = *t, days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    struct avlibc_tm *tm = &s_gmtime;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem / 60 % 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int day = (int)(doy - (153 * mp + 2) / 5 + 1);
    int month = (int)(mp < 10 ? mp + 3 : mp - 9);
    int64_t year = yoe + era * 400 + (month <= 2);
    tm->tm_year = (int)(year - 1900);
    tm->tm_mon = month - 1;
    tm->tm_mday = day;
    tm->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    tm->tm_isdst = 0;
    return tm;
}

int64_t avlibc_mktime(struct avlibc_tm *tm)
{
    int64_t month = tm->tm_mon, year = (int64_t)tm->tm_year + 1900 + month / 12;
    month %= 12;
    if (month < 0) { month += 12; year--; }
    int64_t t = days_from_civil(year, (int)month + 1, 1) + tm->tm_mday - 1;
    t = t * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    *tm = *avlibc_gmtime(&t);   // normalised, as mktime does
    return t;
}

static size_t put_number(char *out, size_t at, size_t cap, int64_t value, int width)
{
    char digits[24];
    int n = 0, negative = value < 0;
    uint64_t v = negative ? 0 - (uint64_t)value : (uint64_t)value;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) digits[n++] = '0';
    if (negative) digits[n++] = '-';
    while (n > 0) {
        if (at + 1 >= cap) return cap;
        out[at++] = digits[--n];
    }
    return at;
}

// The conversions FFmpeg writes with (dates in metadata, ISO 8601). Any
// other conversion fails the whole call with 0, strftime's own "it did not
// fit": printing a wrong field would be a quiet lie in a tag.
size_t avlibc_strftime(char *out, size_t cap, const char *fmt, const struct avlibc_tm *tm)
{
    size_t at = 0;
    if (cap == 0) return 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            if (at + 1 >= cap) return 0;
            out[at++] = *f;
            continue;
        }
        switch (*++f) {
        case 'Y': at = put_number(out, at, cap, (int64_t)tm->tm_year + 1900, 4); break;
        case 'm': at = put_number(out, at, cap, tm->tm_mon + 1, 2); break;
        case 'd': at = put_number(out, at, cap, tm->tm_mday, 2); break;
        case 'H': at = put_number(out, at, cap, tm->tm_hour, 2); break;
        case 'M': at = put_number(out, at, cap, tm->tm_min, 2); break;
        case 'S': at = put_number(out, at, cap, tm->tm_sec, 2); break;
        case '%':
            if (at + 1 >= cap) return 0;
            out[at++] = '%';
            break;
        default:
            return 0;
        }
        if (at >= cap) return 0;
    }
    out[at] = 0;
    return at;
}
