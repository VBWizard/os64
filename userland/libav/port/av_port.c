// av_port.c — the half of the FFmpeg port's C library that touches os64:
// the doors FFmpeg's sources reach for that a library on os64 keeps shut
// (files, the console), the stopwatch clock() reads, and the two functions
// that need something beyond pure C (strtod uses FFmpeg's own scanf; abort
// ends the program).
//
// FFmpeg never opens a file here. Its bytes come through the AVIOContext
// its caller builds, so every file door answers the way a missing file
// does, and FFmpeg takes the path it already has for that.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

// The stream FFmpeg's default log callback writes to. Nothing is kept:
// libmedia installs its own callback, and a program that never does should
// not have a decoder's chatter on its terminal.
struct avport_file { int unused; };
static struct avport_file s_stderr;
FILE *const avport_stderr = &s_stderr;

int avport_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    (void)f; (void)fmt; (void)ap;
    return 0;
}

int avport_fprintf(FILE *f, const char *fmt, ...)
{
    (void)f; (void)fmt;
    return 0;
}

int avport_fputs(const char *s, FILE *f)
{
    (void)s; (void)f;
    return 0;
}

FILE *avport_fopen(const char *path, const char *mode)
{
    (void)path; (void)mode;
    return NULL;
}

FILE *avport_fdopen(int fd, const char *mode)
{
    (void)fd; (void)mode;
    return NULL;
}

int avport_fclose(FILE *f)
{
    (void)f;
    return EOF;
}

int avport_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    errno = ENOENT;
    return -1;
}

int avport_close(int fd)
{
    (void)fd;
    return -1;
}

int64_t avport_read(int fd, void *buffer, size_t length)
{
    (void)fd; (void)buffer; (void)length;
    return -1;
}

int avport_fstat(int fd, struct stat *out)
{
    (void)fd; (void)out;
    return -1;
}

int avport_mkdir(const char *path, int mode)
{
    (void)path; (void)mode;
    return -1;
}

clock_t avport_clock(void)
{
    return (clock_t)os64_micros();
}

// C's strtod through FFmpeg's own scanf (musl's, libavutil/avsscanf.c),
// which skips the same leading space and reads the same forms: decimal,
// hexadecimal, inf and nan. `end` is where the number stopped.
double avport_strtod(const char *s, char **end)
{
    double value = 0;
    int used = 0;
    if (av_sscanf(s, "%lf%n", &value, &used) != 1) used = 0;
    if (end) *end = (char *)s + used;
    return value;
}

// FFmpeg aborts on a broken internal invariant (av_assert0). os64 has no
// SIGABRT, so the program ends with a badge code that names the library,
// in the shape QuickJS's port uses for its own fatal stop (JSFA).
#define OS64_AV_FATAL_EXIT ((int32_t)0x41564641) /* AVFA */

_Noreturn void avport_abort(void)
{
    os64_exit(OS64_AV_FATAL_EXIT);
}
