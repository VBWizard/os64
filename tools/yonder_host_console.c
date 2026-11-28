/* os64_complain for the yonder hosts: the line formatted as libos64 formats
 * it, kept in yonder_host_console (yonder_host_console.h). */
#include "yonder_host_console.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/syscall.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Every os64 syscall a yonder host's code makes (syscall.h's host hook):
 * nobody answers, so it is refused, as the kernel refuses a handle it does
 * not know; the count says how many were made. */
uint64_t yonder_host_syscalls;

uint64_t os64_host_syscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    yonder_host_syscalls++;
    return (uint64_t)-1;
}

char yonder_host_console[16384];
size_t yonder_host_console_lines;

void yonder_host_console_clear(void)
{
    yonder_host_console[0] = '\0';
    yonder_host_console_lines = 0;
}

void os64_complain(const char *fmt, ...)
{
    // Formatted into the buffer libos64's io.c formats into, so a line that
    // would be cut there is cut here.
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t at = strlen(yonder_host_console);
    snprintf(yonder_host_console + at, sizeof(yonder_host_console) - at, "%s", line);
    yonder_host_console_lines++;
}
