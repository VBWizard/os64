/* The console as the yonder hosts see it (yonder_host_build.sh): what yonder
 * said there through os64_complain, every line kept in order, so a case can
 * ask what it said and how often. And the os64 syscalls the hosts' code
 * made, each refused (os64_host_syscall, syscall.h's host hook). */
#ifndef YONDER_HOST_CONSOLE_H
#define YONDER_HOST_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

extern char yonder_host_console[16384];
extern size_t yonder_host_console_lines;

extern uint64_t yonder_host_syscalls;

void yonder_host_console_clear(void);

#endif
