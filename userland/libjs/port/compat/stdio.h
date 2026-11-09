#ifndef OS64_JSPORT_STDIO_H
#define OS64_JSPORT_STDIO_H
/* Private QuickJS compatibility declarations, not a public libc surface. */
#include "platform.h"
typedef JSPortFile FILE;
#define stdout (&jsport_stdout)
#define stderr (&jsport_stderr)
#define EOF (-1)
#define snprintf(...) jsport_snprintf(__VA_ARGS__)
#define vsnprintf(...) jsport_vsnprintf(__VA_ARGS__)
#define fprintf(...) jsport_fprintf(__VA_ARGS__)
#define vfprintf(...) jsport_vfprintf(__VA_ARGS__)
#define printf(...) jsport_printf(__VA_ARGS__)
#define fwrite(...) jsport_fwrite(__VA_ARGS__)
#define putchar(...) jsport_putchar(__VA_ARGS__)
#define fputc(...) jsport_fputc(__VA_ARGS__)
#endif
