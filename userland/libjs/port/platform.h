#ifndef OS64_JSPORT_PLATFORM_H
#define OS64_JSPORT_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
/* Target-private dtoa flag; language conversions never request it. */
#define JSPORT_DTOA_TIES_EVEN (1 << 5)
struct jsport_file { int32_t handle; };
typedef struct jsport_file JSPortFile;
extern JSPortFile jsport_stdout, jsport_stderr;
struct jsport_timeval { int64_t tv_sec, tv_usec; };
int jsport_gettimeofday(struct jsport_timeval *tv, void *zone);
int jsport_timezone_offset(int64_t milliseconds);
void jsport_fatal(const char *expression, const char *file, int line) __attribute__((noreturn));
int jsport_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) __attribute__((format(printf,3,0)));
int jsport_snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf,3,4)));
int jsport_vfprintf(JSPortFile *stream, const char *fmt, va_list ap) __attribute__((format(printf,2,0)));
int jsport_fprintf(JSPortFile *stream, const char *fmt, ...) __attribute__((format(printf,2,3)));
int jsport_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
size_t jsport_fwrite(const void *bytes, size_t size, size_t count, JSPortFile *stream);
int jsport_putchar(int c);
int jsport_fputc(int c, JSPortFile *stream);
#endif
