#ifndef OS64_AV_LIBC_H
#define OS64_AV_LIBC_H
// The pure half of the FFmpeg port's C library (av_libc.c), declared without
// any libc header so the host test can include it beside glibc's. The
// compat headers declare the same functions under the C names FFmpeg uses;
// `struct avlibc_tm` is compat/time.h's `struct tm` field for field, which
// compat/time.h asserts.
#include <stddef.h>
#include <stdint.h>

struct avlibc_tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };

extern int avlibc_errno;
int avlibc_strncmp(const char *a, const char *b, size_t n);
char *avlibc_strstr(const char *haystack, const char *needle);
size_t avlibc_strspn(const char *s, const char *accept);
size_t avlibc_strcspn(const char *s, const char *reject);
char *avlibc_strcpy(char *dst, const char *src);
char *avlibc_strerror(int code);
long avlibc_strtol(const char *s, char **end, int base);
unsigned long avlibc_strtoul(const char *s, char **end, int base);
long long avlibc_strtoll(const char *s, char **end, int base);
unsigned long long avlibc_strtoull(const char *s, char **end, int base);
double avlibc_frexp(double x, int *exponent);
double avlibc_scalbn(double x, int n);
void avlibc_qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *avlibc_bsearch(const void *key, const void *base, size_t count, size_t size,
                      int (*compare)(const void *, const void *));
struct avlibc_tm *avlibc_gmtime(const int64_t *t);
int64_t avlibc_mktime(struct avlibc_tm *tm);
size_t avlibc_strftime(char *out, size_t cap, const char *fmt, const struct avlibc_tm *tm);
#endif
