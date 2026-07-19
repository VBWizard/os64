#ifndef SERIAL_LOGGING_H
#define SERIAL_LOGGING_H

#include <stdarg.h>
#include <stdint.h>
#include "CONFIG.h"

// THE TYPE→FORMAT TABLE (house convention 2026-07-19: stdint.h names in all
// new code). os64 is LP64 — five lines cover everything, no PRIu64 ever:
//   uint8_t / uint16_t / uint32_t     %u   (%x hex)
//   int8_t  / int16_t  / int32_t      %d
//   uint64_t / uintptr_t / size_t     %lu  (%lx hex)
//   int64_t                           %ld
//   any pointer                       %p

void serial_print_string(const char *message);
void printd(__uint128_t debug_level, const char *fmt, ...);

#endif
