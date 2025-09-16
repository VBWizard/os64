#ifndef PANIC_H
#define PANIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void panic(const char *format, ...);
void panic_no_shutdown(const char *format, ...);
void debug_print_mem(uint64_t address, uint64_t byteCount);
#ifdef __cplusplus
}
#endif

#endif /* PANIC_H */