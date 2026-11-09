/* Run libos64's real calendar and TZ code with a controlled clock syscall. */
#include <stdint.h>
#include "os64/date.h"
#include "os64/syscall_numbers.h"
extern os64_time_t js_test_clock;
extern int js_test_clock_failed;
#define OS64_ABI_SYSCALL_H
static uint64_t os64_syscall1(uint64_t number, uint64_t value)
{
    if (number != SYSCALL_TIME || js_test_clock_failed) return (uint64_t)-1;
    *(os64_time_t *)(uintptr_t)value = js_test_clock;
    return 0;
}
static inline uint64_t os64_syscall2(uint64_t number, uint64_t first, uint64_t second)
{
    (void)number; (void)first; (void)second;
    return (uint64_t)-1;
}
#include "../userland/libos64/date.c"
