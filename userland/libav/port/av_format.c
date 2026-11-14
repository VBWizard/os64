// av_format.c — the snprintf FFmpeg's sources call. PURE: no os64 header,
// so tools/test_av_libc_host.sh compares it with glibc's.
//
// It exists because a formatter that does not know a conversion cannot step
// over that conversion's argument, and every argument after it is then read
// from the wrong place: a %s after an unknown %zu would print from whatever
// the size was. So this one walks the C standard's whole grammar (flags,
// width, precision, every length modifier) and takes each argument at its
// own type, whatever it then prints.
//
// What it prints: integers, characters, strings and pointers exactly as C
// says. Floating point (%f %e %g %a and their capitals) is printed in the
// standard's layout, with the digits computed in 80-bit arithmetic, which is
// exact for every digit a double has to give (17 significant) and may
// differ from glibc's correctly-rounded answer past that. FFmpeg formats
// floats only into logs, dumps and a few text renderings of numbers.

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "av_libc.h"

typedef struct {
    char *out;
    size_t cap, count;
} Sink;

static void put(Sink *s, char c)
{
    if (s->count + 1 < s->cap) s->out[s->count] = c;
    s->count++;
}

static void put_n(Sink *s, char c, size_t n)
{
    while (n--) put(s, c);
}

typedef struct {
    int left, plus, space, alt, zero;
    int width, precision;   // precision -1: none given
} Spec;

// One field: a prefix (sign or 0x), leading zeros the precision asks for,
// the digits, padded to the width as the flags say.
static void field(Sink *s, const Spec *f, const char *prefix, size_t prefix_len,
                  size_t zeros, const char *body, size_t body_len, int zero_pad)
{
    size_t total = prefix_len + zeros + body_len;
    size_t pad = f->width > 0 && (size_t)f->width > total ? (size_t)f->width - total : 0;
    if (!f->left && !zero_pad) put_n(s, ' ', pad);
    for (size_t i = 0; i < prefix_len; i++) put(s, prefix[i]);
    if (!f->left && zero_pad) put_n(s, '0', pad);
    put_n(s, '0', zeros);
    for (size_t i = 0; i < body_len; i++) put(s, body[i]);
    if (f->left) put_n(s, ' ', pad);
}

static void integer(Sink *s, const Spec *f, char conv, uint64_t magnitude, int negative)
{
    char digits[32];
    size_t n = 0;
    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char *glyphs = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    for (uint64_t v = magnitude; v; v /= base) digits[n++] = glyphs[v % base];
    // reverse in place
    for (size_t i = 0; i < n / 2; i++) {
        char t = digits[i]; digits[i] = digits[n - 1 - i]; digits[n - 1 - i] = t;
    }
    // The precision is the minimum number of digits; "%.0d" of zero is empty.
    size_t want = f->precision >= 0 ? (size_t)f->precision : 1;
    size_t zeros = want > n ? want - n : 0;
    char prefix[2];
    size_t prefix_len = 0;
    if (conv == 'd' || conv == 'i') {
        if (negative) prefix[prefix_len++] = '-';
        else if (f->plus) prefix[prefix_len++] = '+';
        else if (f->space) prefix[prefix_len++] = ' ';
    } else if (conv == 'o' && f->alt && zeros == 0 && (n == 0 || digits[0] != '0')) {
        zeros = 1;                                   // # makes the first digit a 0
    } else if (((conv == 'x' || conv == 'X') && f->alt && magnitude != 0) || conv == 'p') {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = conv == 'X' ? 'X' : 'x';
    }
    field(s, f, prefix, prefix_len, zeros, digits, n, f->zero && f->precision < 0 && !f->left);
}

// ---------------------------------------------------------------- floating

static int is_nan(double x) { return x != x; }
static int is_inf(double x) { return x == x && x - x != 0; }

// The decimal digits of |x| rounded to `frac` places after the point, as a
// digit string with `*point` digits before the point. 80-bit arithmetic:
// exact for the digits a double carries, and the reason the header says
// where this may differ from glibc.
static size_t decimal(long double x, int frac, char *out, size_t cap, int *point)
{
    int e10 = 0;
    long double scaled = x;
    if (scaled != 0) {
        while (scaled >= 1e18L) { scaled /= 10; e10++; }
        while (scaled < 1e17L) { scaled *= 10; e10--; }
    }
    // scaled now holds 18 significant digits as an integer-sized value;
    // x == scaled * 10^e10. Round to `frac` places: drop digits below 10^-frac.
    int drop = -frac - e10;
    if (drop > 18) drop = 19;           // everything rounds away (or to one unit)
    long double unit = 1;
    for (int i = 0; i < drop; i++) unit *= 10;
    if (drop > 0) {
        long double q = scaled / unit;
        uint64_t whole = (uint64_t)q;
        long double rest = q - (long double)whole;
        if (rest > 0.5L || (rest == 0.5L && (whole & 1))) whole++;
        scaled = (long double)whole;
        e10 += drop;
    }
    uint64_t v = (uint64_t)scaled;
    char rev[24];
    size_t n = 0;
    do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    // digits rev[n-1..0] times 10^e10; the point sits n + e10 digits in.
    size_t len = 0;
    for (size_t i = n; i-- > 0 && len < cap;) out[len++] = rev[i];
    *point = (int)n + e10;
    // Trailing zeros are implied by e10 > 0; the caller pads.
    return len;
}

// Writes |x| in %f layout with `frac` places into buf; returns its length.
static size_t fixed(long double x, int frac, int alt, char *buf, size_t cap)
{
    char d[24];
    int point;
    size_t n = decimal(x, frac, d, sizeof(d), &point);
    size_t len = 0;
    #define EMIT(c) do { if (len < cap) buf[len++] = (c); } while (0)
    // integer part
    if (point <= 0) EMIT('0');
    for (int i = 0; i < point; i++) EMIT(i < (int)n ? d[i] : '0');
    if (frac > 0 || alt) EMIT('.');
    for (int i = 0; i < frac; i++) {
        int at = point + i;
        EMIT(at >= 0 && at < (int)n ? d[at] : '0');
    }
    #undef EMIT
    return len;
}

// The first `nsig` significant digits of |x| > 0, rounded half to even,
// into out (zeros past the 18 that 80-bit arithmetic gives); returns the
// decimal exponent of the first digit.
static int significant(long double x, int nsig, char *out)
{
    int e10 = 0;
    long double scaled = x;
    while (scaled >= 1e18L) { scaled /= 10; e10++; }
    while (scaled < 1e17L) { scaled *= 10; e10--; }
    uint64_t v = (uint64_t)scaled;
    long double rest = scaled - (long double)v;
    int keep = nsig < 18 ? nsig : 18;
    uint64_t unit = 1;
    for (int i = keep; i < 18; i++) unit *= 10;
    uint64_t whole = v / unit;
    long double frac = ((long double)(v % unit) + rest) / (long double)unit;
    if (frac > 0.5L || (frac == 0.5L && (whole & 1))) whole++;
    int exp10 = e10 + 17;              // exponent of the leading digit of v
    char rev[24];
    int n = 0;
    do { rev[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    if (n > keep) exp10++;             // 9.99 rounded up to 10.0: a power of ten, one digit longer
    for (int i = 0; i < nsig; i++) out[i] = i < n ? rev[n - 1 - i] : '0';
    return exp10;
}

// Writes |x| in %e layout with `frac` mantissa places; returns its length.
static size_t exponent(long double x, int frac, int alt, char upper, char *buf, size_t cap)
{
    char d[420];
    int e = 0;
    if (x != 0) e = significant(x, frac + 1, d);
    else for (int i = 0; i <= frac; i++) d[i] = '0';
    size_t len = 0;
    #define EMIT(c) do { if (len < cap) buf[len++] = (c); } while (0)
    EMIT(d[0]);
    if (frac > 0 || alt) EMIT('.');
    for (int i = 1; i <= frac; i++) EMIT(d[i]);
    EMIT(upper ? 'E' : 'e');
    EMIT(e < 0 ? '-' : '+');
    int ae = e < 0 ? -e : e;
    char digits[8];
    int n = 0;
    do { digits[n++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
    if (n < 2) digits[n++] = '0';
    while (n > 0) EMIT(digits[--n]);
    #undef EMIT
    return len;
}

static void floating(Sink *s, const Spec *f, char conv, double value)
{
    int negative = value < 0 || (value == 0 && 1 / value < 0);
    char prefix[1];
    size_t prefix_len = 0;
    if (negative) prefix[prefix_len++] = '-';
    else if (f->plus) prefix[prefix_len++] = '+';
    else if (f->space) prefix[prefix_len++] = ' ';
    int upper = conv >= 'A' && conv <= 'Z';
    char buf[512];
    size_t len;
    if (is_nan(value) || is_inf(value)) {
        const char *word = is_nan(value) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        field(s, f, prefix, prefix_len, 0, word, 3, 0);
        return;
    }
    long double x = negative ? -(long double)value : (long double)value;
    int precision = f->precision < 0 ? 6 : f->precision;
    if (precision > 400) precision = 400;
    char lower = (char)(conv | 0x20);
    if (lower == 'f') {
        len = fixed(x, precision, f->alt, buf, sizeof(buf));
    } else if (lower == 'e' || lower == 'a') {
        // %a is printed in %e's layout: FFmpeg's sources never ask for it.
        len = exponent(x, precision, f->alt, upper, buf, sizeof(buf));
    } else {   // g: %e or %f by the exponent the %e form would have
        int p = precision == 0 ? 1 : precision;
        char probe[512];
        size_t plen = exponent(x, p - 1, 0, 0, probe, sizeof(probe));
        int e = 0, sign = 1;
        size_t i = plen;
        while (i > 0 && probe[i - 1] != 'e') i--;
        if (i < plen && probe[i] == '-') sign = -1;
        for (size_t j = i + 1; j < plen; j++) e = e * 10 + (probe[j] - '0');
        e *= sign;
        if (p > e && e >= -4) len = fixed(x, p - 1 - e, f->alt, buf, sizeof(buf));
        else len = exponent(x, p - 1, f->alt, upper, buf, sizeof(buf));
        if (!f->alt) {   // %g drops trailing zeros, and a point left bare
            size_t mant_end = len, k = 0;
            while (k < len && buf[k] != 'e' && buf[k] != 'E') k++;
            mant_end = k;
            int has_point = 0;
            for (size_t j = 0; j < mant_end; j++) has_point |= buf[j] == '.';
            if (has_point) {
                size_t cut = mant_end;
                while (cut > 0 && buf[cut - 1] == '0') cut--;
                if (cut > 0 && buf[cut - 1] == '.') cut--;
                for (size_t j = mant_end; j < len; j++) buf[cut + (j - mant_end)] = buf[j];
                len = cut + (len - mant_end);
            }
        }
    }
    field(s, f, prefix, prefix_len, 0, buf, len, f->zero && !f->left);
}

// ---------------------------------------------------------------- walk

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIG_L };

int avlibc_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    Sink s = { out, cap, 0 };
    va_list args;
    va_copy(args, ap);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&s, *p); continue; }
        Spec f = { 0, 0, 0, 0, 0, 0, -1 };
        for (;; p++) {
            char c = p[1];
            if (c == '-') f.left = 1;
            else if (c == '+') f.plus = 1;
            else if (c == ' ') f.space = 1;
            else if (c == '#') f.alt = 1;
            else if (c == '0') f.zero = 1;
            else break;
        }
        p++;
        if (*p == '*') {
            f.width = va_arg(args, int);
            if (f.width < 0) { f.left = 1; f.width = f.width == INT_MIN ? INT_MAX : -f.width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') { if (f.width < 100000) f.width = f.width * 10 + (*p - '0'); p++; }
        }
        if (*p == '.') {
            p++;
            f.precision = 0;
            if (*p == '*') {
                f.precision = va_arg(args, int);
                if (f.precision < 0) f.precision = -1;   // as if none was given
                p++;
            } else {
                while (*p >= '0' && *p <= '9') { if (f.precision < 100000) f.precision = f.precision * 10 + (*p - '0'); p++; }
            }
        }
        int len = LEN_NONE;
        switch (*p) {
        case 'h': len = p[1] == 'h' ? (p++, LEN_HH) : LEN_H; p++; break;
        case 'l': len = p[1] == 'l' ? (p++, LEN_LL) : LEN_L; p++; break;
        case 'q': len = LEN_LL; p++; break;
        case 'j': len = LEN_J; p++; break;
        case 'z': len = LEN_Z; p++; break;
        case 't': len = LEN_T; p++; break;
        case 'L': len = LEN_BIG_L; p++; break;
        default: break;
        }
        char conv = *p;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v;
            if (len == LEN_LL || len == LEN_L || len == LEN_J || len == LEN_T || len == LEN_Z) v = va_arg(args, int64_t);
            else v = va_arg(args, int);
            if (len == LEN_HH) v = (signed char)v;
            else if (len == LEN_H) v = (short)v;
            integer(&s, &f, conv, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, v < 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v;
            if (len == LEN_LL || len == LEN_L || len == LEN_J || len == LEN_T || len == LEN_Z) v = va_arg(args, uint64_t);
            else v = va_arg(args, unsigned);
            if (len == LEN_HH) v = (unsigned char)v;
            else if (len == LEN_H) v = (unsigned short)v;
            integer(&s, &f, conv, v, 0);
            break;
        }
        case 'p': {
            void *ptr = va_arg(args, void *);
            Spec pf = f;
            pf.precision = -1;
            if (ptr) integer(&s, &pf, 'p', (uint64_t)(uintptr_t)ptr, 0);
            else field(&s, &pf, NULL, 0, 0, "(nil)", 5, 0);   // glibc's spelling
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            Spec cf = f;
            field(&s, &cf, NULL, 0, 0, &c, 1, 0);
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);
            if (!str) str = "(null)";
            // The precision is tested FIRST: with one, the array need not
            // be terminated, and no byte past it may be read.
            size_t n = 0;
            while ((f.precision < 0 || n < (size_t)f.precision) && str[n]) n++;
            field(&s, &f, NULL, 0, 0, str, n, 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = len == LEN_BIG_L ? (double)va_arg(args, long double) : va_arg(args, double);
            floating(&s, &f, conv, v);
            break;
        }
        case 'n': {
            void *where = va_arg(args, void *);
            if (len == LEN_LL || len == LEN_L || len == LEN_J || len == LEN_Z || len == LEN_T) *(int64_t *)where = (int64_t)s.count;
            else if (len == LEN_H) *(short *)where = (short)s.count;
            else if (len == LEN_HH) *(signed char *)where = (signed char)s.count;
            else *(int *)where = (int)s.count;
            break;
        }
        case '%':
            put(&s, '%');
            break;
        case 0:
            p--;               // a lone % at the end: print it and stop
            put(&s, '%');
            break;
        default:               // not a conversion C has: print it as written
            put(&s, '%');
            put(&s, conv);
            break;
        }
    }
    va_end(args);
    if (cap > 0) out[s.count < cap ? s.count : cap - 1] = 0;
    return s.count > INT_MAX ? -1 : (int)s.count;
}

int avlibc_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = avlibc_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
