// test_av_libc_host.c — every function in userland/libav/port/av_libc.c
// against the host's glibc, under ASan and UBSan. av_libc.c is the C
// library FFmpeg's sources call where libos64 has no function of that name,
// so "what glibc does" is the specification, within the subset av_libc.c
// says it covers (strftime's conversions, UTC only).

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "av_libc.h"

static int s_failures, s_checks;

#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

static int sgn(int v) { return (v > 0) - (v < 0); }

static void test_strings(void)
{
    const char *words[] = { "", "a", "ab", "abc", "abd", "b", "\xff", "abc\xff", "hello world", "hell" };
    size_t n = sizeof(words) / sizeof(words[0]);
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++)
            for (size_t k = 0; k < 14; k++)
                CHECK(sgn(avlibc_strncmp(words[i], words[j], k)) == sgn(strncmp(words[i], words[j], k)),
                      "strncmp(\"%s\", \"%s\", %zu)", words[i], words[j], k);
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) {
            CHECK(avlibc_strstr(words[i], words[j]) == strstr(words[i], words[j]),
                  "strstr(\"%s\", \"%s\")", words[i], words[j]);
            CHECK(avlibc_strspn(words[i], words[j]) == strspn(words[i], words[j]),
                  "strspn(\"%s\", \"%s\")", words[i], words[j]);
            CHECK(avlibc_strcspn(words[i], words[j]) == strcspn(words[i], words[j]),
                  "strcspn(\"%s\", \"%s\")", words[i], words[j]);
        }
    char a[32], b[32];
    for (size_t i = 0; i < n; i++) {
        memset(a, 'x', sizeof(a));
        memset(b, 'x', sizeof(b));
        CHECK(avlibc_strcpy(a, words[i]) == a && (strcpy(b, words[i]), memcmp(a, b, sizeof(a)) == 0),
              "strcpy(\"%s\")", words[i]);
    }
}

static void test_integers(void)
{
    const char *inputs[] = {
        "", " ", "0", "-0", "+0", "1", "-1", "42abc", "  \t\n 17", "+-3", "-+3", "0x", "0X1f", "0x1G",
        "0xg", "010", "08", "0b1", "z", "Zz", "9223372036854775807", "9223372036854775808",
        "-9223372036854775808", "-9223372036854775809", "18446744073709551615", "18446744073709551616",
        "-18446744073709551615", "-18446744073709551616", "99999999999999999999999999",
        "0xffffffffffffffff", "0x10000000000000000", "-0x8000000000000000", "777777777777777777777",
        "1777777777777777777777", "2000000000000000000000",
    };
    int bases[] = { 0, 2, 8, 10, 16, 36, 1, 37 };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
        for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
            const char *s = inputs[i];
            int base = bases[b];
            char *e1, *e2;
            if (base == 1 || base == 37) {
                // C says nothing is converted; glibc sets EINVAL and *end = s.
                avlibc_strtoll(s, &e1, base);
                CHECK(e1 == s, "strtoll(\"%s\", %d) end", s, base);
                continue;
            }
            errno = 0; avlibc_errno = 0;
            long long v1 = avlibc_strtoll(s, &e1, base);
            long long v2 = strtoll(s, &e2, base);
            CHECK(v1 == v2 && e1 == e2 && (errno == ERANGE) == (avlibc_errno == 34),
                  "strtoll(\"%s\", %d): %lld end %td vs %lld end %td", s, base, v1, e1 - s, v2, e2 - s);
            errno = 0; avlibc_errno = 0;
            unsigned long long u1 = avlibc_strtoull(s, &e1, base);
            unsigned long long u2 = strtoull(s, &e2, base);
            CHECK(u1 == u2 && e1 == e2 && (errno == ERANGE) == (avlibc_errno == 34),
                  "strtoull(\"%s\", %d): %llu end %td vs %llu end %td", s, base, u1, e1 - s, u2, e2 - s);
            CHECK(avlibc_strtol(s, NULL, base) == strtol(s, NULL, base), "strtol(\"%s\", %d)", s, base);
            CHECK(avlibc_strtoul(s, NULL, base) == strtoul(s, NULL, base), "strtoul(\"%s\", %d)", s, base);
        }
}

static int same_double(double a, double b)
{
    return (isnan(a) && isnan(b)) || memcmp(&a, &b, sizeof(a)) == 0;
}

static void test_floats(void)
{
    double values[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 3.0, 1e-310, -4.9e-324, 2.2250738585072014e-308,
                        1.7976931348623157e308, INFINITY, -INFINITY, NAN, 123456.789, 1e100 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        int e1 = 99, e2 = 99;
        double m1 = avlibc_frexp(values[i], &e1), m2 = frexp(values[i], &e2);
        CHECK(same_double(m1, m2) && (isnan(values[i]) || isinf(values[i]) || e1 == e2),
              "frexp(%a): %a,%d vs %a,%d", values[i], m1, e1, m2, e2);
    }
    for (int r = 0; r < 200000; r++) {
        union { double d; uint64_t u; } v = { .u = rnd() };
        int e1, e2;
        double m1 = avlibc_frexp(v.d, &e1), m2 = frexp(v.d, &e2);
        CHECK(same_double(m1, m2) && (isnan(v.d) || isinf(v.d) || e1 == e2), "frexp(%a)", v.d);
        int n = (int)(rnd() % 4400) - 2200;
        CHECK(same_double(avlibc_scalbn(v.d, n), scalbn(v.d, n)), "scalbn(%a, %d)", v.d, n);
    }
    int edges[] = { INT_MIN, -2200, -1075, -1074, -1023, -1022, -1, 0, 1, 1023, 1024, 2100, INT_MAX };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        for (size_t j = 0; j < sizeof(edges) / sizeof(edges[0]); j++)
            CHECK(same_double(avlibc_scalbn(values[i], edges[j]), scalbn(values[i], edges[j])),
                  "scalbn(%a, %d)", values[i], edges[j]);
}

// The span strtod would consume, against glibc's own end pointer.
static void test_strtod_span(void)
{
    const char *inputs[] = {
        "", " ", "1", "-1", "+.5", ".", "-.", "1.", ".e1", "1e", "1e+", "1e-", "1e5", "1E-5x", "2.5e+10q",
        "0x", "0X", "0x1", "0x1p", "0x1p3", "0x.8p-1", "0x.p1", "0xg", "  \t 42", "inf", "-INF", "infinity",
        "Infinit", "nan", "NaN(abc_1)", "nan(", "nan(x", "nan()", "+nan", "e5", "-e5", "1e5e5", "00012",
        "1.2.3", "9" "99999999999999999999999999999999999999", "1e400", "-1e-400", "0x1.fffffffffffffp1023",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        char *end;
        strtod(inputs[i], &end);
        size_t want = (size_t)(end - inputs[i]);
        CHECK(avlibc_strtod_span(inputs[i]) == want, "strtod span(\"%s\"): %zu vs %zu",
              inputs[i], avlibc_strtod_span(inputs[i]), want);
    }
    const char alphabet[] = "0123456789.eEpPxX+-abcfinINF() ";
    for (int r = 0; r < 200000; r++) {
        char buf[16];
        int n = (int)(rnd() % 12);
        for (int i = 0; i < n; i++) buf[i] = alphabet[rnd() % (sizeof(alphabet) - 1)];
        buf[n] = 0;
        char *end;
        strtod(buf, &end);
        CHECK(avlibc_strtod_span(buf) == (size_t)(end - buf), "strtod span(\"%s\")", buf);
    }
}

// The formatter, against glibc's snprintf: exactly, for everything but the
// digits of a double past what av_format.c promises.
#define SAME(fmt, ...) do { char a_[512], b_[512]; \
    int na_ = avlibc_snprintf(a_, sizeof(a_), fmt, __VA_ARGS__); \
    int nb_ = snprintf(b_, sizeof(b_), fmt, __VA_ARGS__); \
    CHECK(na_ == nb_ && strcmp(a_, b_) == 0, "snprintf(\"%s\"): \"%s\" vs \"%s\"", fmt, a_, b_); } while (0)

static void test_format(void)
{
    const char *int_formats[] = { "%d", "%5d", "%-5d|", "%05d", "%+d", "% d", "%.3d", "%8.3d", "%-+8.3d|",
                                  "%.0d", "%x", "%#x", "%#X", "%08x", "%#010x", "%o", "%#o", "%#.0o", "%u",
                                  "%hhd", "%hd", "%hhu", "%hx", "%c" };
    int ints[] = { 0, 1, -1, 7, 255, 256, -129, 65535, 65536, 2147483647, (int)0x80000000, 12345, -98765 };
    for (size_t f = 0; f < sizeof(int_formats) / sizeof(int_formats[0]); f++)
        for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
            if (int_formats[f][1] == 'c' && (ints[i] <= 0 || ints[i] > 126)) continue;
            SAME(int_formats[f], ints[i]);
        }
    const char *long_formats[] = { "%ld", "%lld", "%lu", "%llx", "%zu", "%zd", "%td", "%jd", "%ju",
                                   "%20ld", "%-20lx|", "%#lo", "%+lld" };
    int64_t longs[] = { 0, 1, -1, INT64_MAX, INT64_MIN, (int64_t)0x123456789abcdefLL, 4294967296LL };
    for (size_t f = 0; f < sizeof(long_formats) / sizeof(long_formats[0]); f++)
        for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++)
            SAME(long_formats[f], longs[i]);
    // The case this formatter exists for: a conversion os64's own formatter
    // does not know, then more arguments, all landing where they belong.
    SAME("%zu then %s then %d", (size_t)42, "words", 7);
    SAME("%td|%hhx|%s|%p", (ptrdiff_t)-3, 300, "x", (void *)0x1234);
    SAME("%s|%.2s|%-6s|%6s|%.0s|", "hello", "hello", "ab", "ab", "zzz");
    SAME("%*d|%-*d|%.*d|%*.*s|", 6, 42, 6, 42, 4, 7, 8, 3, "abcdef");
    SAME("%*d|", -6, 42);
    SAME("100%% %c%c", 'o', 'k');
    SAME("%p %p", (void *)0, (void *)&s_checks);
    char small[4];
    CHECK(avlibc_snprintf(small, sizeof(small), "%s", "truncated") == 9 && strcmp(small, "tru") == 0,
          "truncation keeps the full count and terminates");
    CHECK(avlibc_snprintf(NULL, 0, "%d", 12345) == 5, "a count with no room at all");
    int count = -1;
    char tmp[32];
    avlibc_snprintf(tmp, sizeof(tmp), "abc%n", &count);
    CHECK(count == 3, "%%n");

    const char *float_formats[] = { "%f", "%.0f", "%.1f", "%.3f", "%10.2f", "%-10.2f|", "%+.2f", "% .2f",
                                    "%#.0f", "%e", "%.0e", "%.3e", "%E", "%12.4e", "%g", "%.0g", "%.3g", "%G",
                                    "%#g", "%.10g", "%010.3f", "%-+12.3e|" };
    double floats[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, 0.125, 3.14159265358979, 123456.789, 1e-5,
                        9.9999, 99.5, 1e10, 1e15, 6.02e23, 1.23e-10, 0.1, 0.07, 100.0, 1e100, 1e-300,
                        INFINITY, -INFINITY, NAN };
    for (size_t f = 0; f < sizeof(float_formats) / sizeof(float_formats[0]); f++)
        for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++) {
            if (strchr(float_formats[f], 'f') && fabs(floats[i]) > 1e17) continue;   // past 17 digits
            SAME(float_formats[f], floats[i]);
        }
    // Random doubles: the layout exactly, the value to the digits a double has.
    int differ = 0;
    for (int r = 0; r < 100000; r++) {
        union { double d; uint64_t u; } v = { .u = rnd() };
        if (isnan(v.d) || isinf(v.d)) continue;
        int p = (int)(rnd() % 16);
        char fmt[16], a[512], b[512];
        snprintf(fmt, sizeof(fmt), "%%.%d%c", p, "eg"[r & 1]);
        avlibc_snprintf(a, sizeof(a), fmt, v.d);
        snprintf(b, sizeof(b), fmt, v.d);
        if (strcmp(a, b) != 0) {
            differ++;
            double x = strtod(a, NULL), y = strtod(b, NULL);
            CHECK(strlen(a) == strlen(b) && fabs(x - y) <= fabs(y) * 1e-15 * pow(10, 16 - p) * 1.0000001,
                  "%s of %a: \"%s\" vs \"%s\"", fmt, v.d, a, b);
        }
    }
    printf("  random doubles: %d of 100000 differ from glibc in the last digit\n", differ);
}

static int compare_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef struct { int key; char pad[13]; } Wide;
static int compare_wide(const void *a, const void *b) { return compare_int(a, b); }

static void test_sorting(void)
{
    for (int round = 0; round < 400; round++) {
        size_t count = (size_t)(rnd() % 300);
        int mine[300], theirs[300];
        for (size_t i = 0; i < count; i++) mine[i] = theirs[i] = (int)(rnd() % (round % 3 ? 1000 : 5)) - 50;
        avlibc_qsort(mine, count, sizeof(int), compare_int);
        qsort(theirs, count, sizeof(int), compare_int);
        CHECK(count == 0 || memcmp(mine, theirs, count * sizeof(int)) == 0, "qsort round %d", round);
        for (int probe = -60; probe < 960; probe += 7) {
            int *a = avlibc_bsearch(&probe, mine, count, sizeof(int), compare_int);
            int *b = bsearch(&probe, theirs, count, sizeof(int), compare_int);
            CHECK((a == NULL) == (b == NULL) && (!a || *a == probe), "bsearch %d round %d", probe, round);
        }
    }
    Wide w[50];
    for (int i = 0; i < 50; i++) { w[i].key = (int)(rnd() % 20); memset(w[i].pad, i, sizeof(w[i].pad)); }
    avlibc_qsort(w, 50, sizeof(Wide), compare_wide);
    for (int i = 1; i < 50; i++) CHECK(w[i - 1].key <= w[i].key, "qsort of 17-byte records");
}

static void test_time(void)
{
    int64_t times[] = { 0, 1, -1, 86399, 86400, -86400, 951782400 /* 2000-02-29 */, 4107542400 /* 2100-03-01 */,
                        -2208988800 /* 1900-01-01 */, 253402300799 /* 9999-12-31 */, -62135596800 /* year 1 */ };
    for (int r = 0; r < 100000 + (int)(sizeof(times) / sizeof(times[0])); r++) {
        int64_t t = r < (int)(sizeof(times) / sizeof(times[0])) ? times[r]
                  : (int64_t)(rnd() % 600000000000ull) - 300000000000ll;
        time_t tt = (time_t)t;
        struct tm g;
        gmtime_r(&tt, &g);
        // A copy: gmtime's answer is one static struct, which mktime below
        // reuses, as C's is.
        struct avlibc_tm *m = &(struct avlibc_tm){ 0 };
        *m = *avlibc_gmtime(&t);
        CHECK(m->tm_year == g.tm_year && m->tm_mon == g.tm_mon && m->tm_mday == g.tm_mday
              && m->tm_hour == g.tm_hour && m->tm_min == g.tm_min && m->tm_sec == g.tm_sec
              && m->tm_wday == g.tm_wday && m->tm_yday == g.tm_yday,
              "gmtime(%lld): %d-%d-%d wday %d yday %d vs %d-%d-%d wday %d yday %d", (long long)t,
              m->tm_year, m->tm_mon, m->tm_mday, m->tm_wday, m->tm_yday,
              g.tm_year, g.tm_mon, g.tm_mday, g.tm_wday, g.tm_yday);

        // mktime is UTC here, which is glibc's timegm; fields out of range
        // are normalised, so feed it some.
        struct tm in = g;
        in.tm_mon += (int)(rnd() % 30) - 15;
        in.tm_mday += (int)(rnd() % 80) - 40;
        in.tm_sec += (int)(rnd() % 200000) - 100000;
        struct avlibc_tm mine = { in.tm_sec, in.tm_min, in.tm_hour, in.tm_mday, in.tm_mon, in.tm_year, 0, 0, 0 };
        int64_t got = avlibc_mktime(&mine);
        int64_t want = (int64_t)timegm(&in);
        CHECK(got == want && mine.tm_mday == in.tm_mday && mine.tm_mon == in.tm_mon && mine.tm_yday == in.tm_yday,
              "mktime: %lld vs %lld", (long long)got, (long long)want);

        if (r % 97 == 0 && g.tm_year + 1900 >= 1000) {
            const char *fmt = "%Y-%m-%dT%H:%M:%S %% done";
            char a[64], b[64];
            size_t na = avlibc_strftime(a, sizeof(a), fmt, m), nb = strftime(b, sizeof(b), fmt, &g);
            CHECK(na == nb && strcmp(a, b) == 0, "strftime: \"%s\" vs \"%s\"", a, b);
            CHECK(avlibc_strftime(a, 5, fmt, m) == 0, "strftime into too little room");
        }
    }
    char out[32];
    struct avlibc_tm *m = avlibc_gmtime(&times[0]);
    CHECK(avlibc_strftime(out, sizeof(out), "%A", m) == 0, "an unsupported conversion fails the call");
    CHECK(avlibc_strftime(out, sizeof(out), "", m) == 0 && out[0] == 0, "empty format");
}

int main(void)
{
    test_strings();
    test_integers();
    test_floats();
    test_strtod_span();
    test_format();
    test_sorting();
    test_time();
    if (s_failures) {
        fprintf(stderr, "FAIL av_libc: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS av_libc: %d checks against glibc\n", s_checks);
    return 0;
}
