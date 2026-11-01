/* The console as the yonder hosts see it (yonder_host_build.sh): what yonder
 * said there through os64_complain, every line kept in order, so a case can
 * ask what it said and how often. */
#ifndef YONDER_HOST_CONSOLE_H
#define YONDER_HOST_CONSOLE_H

#include <stddef.h>

extern char yonder_host_console[16384];
extern size_t yonder_host_console_lines;

void yonder_host_console_clear(void);

#endif
