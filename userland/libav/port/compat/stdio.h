#ifndef OS64_AV_STDIO_H
#define OS64_AV_STDIO_H
/* Private to the FFmpeg port. FILE is opaque and no stream ever opens:
 * FFmpeg's bytes arrive through libmedia's AVIOContext, and its words go to
 * av_log, whose callback libmedia owns. stderr exists so the default log
 * callback compiles; writes to it are discarded (av_port.c). The formatter
 * is libos64's, which has no floating point: a %f prints as itself. */
#include <stdarg.h>
#include <stddef.h>
#include "os64/fmt.h"
typedef struct avport_file FILE;
extern FILE *const avport_stderr;
#define stderr avport_stderr
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define snprintf os64_snprintf
#define vsnprintf os64_vsnprintf
/* FFmpeg carries its own scanf (libavutil/avsscanf.c, musl's); plain
 * sscanf calls are routed to it. */
int av_sscanf(const char *s, const char *fmt, ...);
#define sscanf av_sscanf
int avport_fprintf(FILE *f, const char *fmt, ...);
int avport_vfprintf(FILE *f, const char *fmt, va_list ap);
int avport_fputs(const char *s, FILE *f);
#define fprintf avport_fprintf
#define vfprintf avport_vfprintf
#define fputs avport_fputs
#define fflush(f) 0
FILE *avport_fopen(const char *path, const char *mode);
int avport_fclose(FILE *f);
FILE *avport_fdopen(int fd, const char *mode);
#define fopen avport_fopen
#define fdopen avport_fdopen
#define fclose avport_fclose
#endif
