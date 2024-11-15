#ifndef SERIAL_LOGGING_H
#define SERIAL_LOGGING_H

#include <stdarg.h>
#include <stdint.h>
#include "CONFIG.h"

void printd(uint64_t debug_level, const char *fmt, ...);

#endif