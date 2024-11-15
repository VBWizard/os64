#ifndef CONFIG_H
#define CONFIG_H

// Timing related configuration
#define TICKS_PER_SECOND 100
// Milliseconds per tick
#define MS_PER_TICK 1000 / TICKS_PER_SECOND
// Microseconds per tick
#define MIS_PER_TICK 1000000 / TICKS_PER_SECOND

// Memory related configuration
#define PAGE_SIZE 0x1000
#define KERNEL_PAGED_BASE_ADDRESS 0xFFFFFFFF80000000
#define INITIAL_MEMORY_STATUS_COUNT 0x1000

//Framebuffer related
#define FRAMEBUFFER_FONT "zap-ext-light16.psf"

// Debugging related configuration
#define DEBUG_EVERYTHING 0xFFFFFFFFFFFFFFFF
#define DEBUG_NOTHING 0x0000000000000000
#define DEBUG_BOOT 1 << 0

#define DEBUG_OPTIONS DEBUG_BOOT 

#endif // CONFIG_H
