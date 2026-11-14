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
    test_sorting();
    test_time();
    if (s_failures) {
        fprintf(stderr, "FAIL av_libc: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS av_libc: %d checks against glibc\n", s_checks);
    return 0;
}
