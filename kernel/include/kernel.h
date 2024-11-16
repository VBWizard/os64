#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>

/// @brief Number of IRQ0 ticks per second
extern uint64_t kTicksPerSecond;
/// @brief Is kernel initialization complete?
extern bool kInitDone;
void kernel_main();

#endif
