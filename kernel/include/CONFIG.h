#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define ENABLE_COM1

#define DISK_WRITING_ENABLED
//#define ENABLE_DOUBLE_BUFFER

//  Timing related configuration
#define TICKS_PER_SECOND 100
// Milliseconds per tick
#define MS_PER_TICK (uint64_t)(1000 / TICKS_PER_SECOND)
// Microseconds per tick
#define MIS_PER_TICK 1000000 / TICKS_PER_SECOND
// Memory related configuration
#define PAGE_SIZE 0x1000
#define KERNEL_PAGED_BASE_ADDRESS 0xFFFFFFFF80000000
#define INITIAL_MEMORY_STATUS_COUNT 100000
#define KERNEL_STACK_SIZE 20 * PAGE_SIZE

//Signal related
#define SIGNAL_PROCESS_TICK_FREQUENCY 1 //20 MS if TICKS_PER_SECOND = 100 (1 tick every 10 MS)

//Scheduler Related
//How many ticks expire between scheduler runs (@100hz - 5=20 ticks per run, 10=10  ticks per run (10 ticks * 10 ms = 100ms @ 100 TPS))
#define MP_SCHEDULER_RUNS_PER_SECOND 10
#define TICKS_PER_SCHEDULER_RUN TICKS_PER_SECOND / MP_SCHEDULER_RUNS_PER_SECOND
#define TICKS_PER_SCHEDULER_RUN_AP TICKS_PER_SCHEDULER_RUN
//#define SCHEDULER_DEBUG 1

// Framebuffer related
#define FRAMEBUFFER_FONT "zap-ext-light16.psf"

// Linux defines so we have access to some header values that aren't accessible otherwise
#define __KERNEL__
#define __USE_MISC

// Debugging related configuration
#define SHUTOFF_ON_PANIC 0
//remark to disable
//#define DEBUG_FOCUS_APIC_ID 1

#define DEBUG_EVERYTHING ((__uint128_t)0xFFFFFFFFFFFFFFFFULL | ((__uint128_t)0xFFFFFFFFFFFFFFFFULL << 64))
#define DEBUG_NOTHING 0x0000000000000000
#define DEBUG_EXCEPTIONS 1 << 0
#define DEBUG_BOOT 1 << 1
#define DEBUG_SMP 1 << 2
#define DEBUG_PCI_DISCOVERY 1 << 3
#define DEBUG_PCI 1 << 4
#define DEBUG_HARDDRIVE 1 << 5
#define DEBUG_AHCI 1 << 6
#define DEBUG_MEMMAP (__uint128_t)1 << 7
#define DEBUG_ACPI (__uint128_t)1 << 8
#define DEBUG_PAGING (__uint128_t)1 << 9
#define DEBUG_ALLOCATOR (__uint128_t)1 << 10
#define DEBUG_KMALLOC (__uint128_t)1 << 10
#define DEBUG_NVME (__uint128_t)1 << 12
#define DEBUG_VFS (__uint128_t)1 << 13
#define DEBUG_SHUTDOWN (__uint128_t)1 << 13
#define DEBUG_THREAD (__uint128_t)1 << 14
#define DEBUG_TASK (__uint128_t)1 << 15
#define DEBUG_SCHEDULER (__uint128_t)1 << 16
#define DEBUG_SIGNALS (__uint128_t)1 << 17

#define DEBUG_DETAILED (__uint128_t)1 << 126
#define DEBUG_EXTRA_DETAILED  (__uint128_t)1 << 127
#define DEBUG_OPTIONS (__uint128_t)(DEBUG_EXCEPTIONS | DEBUG_BOOT | DEBUG_HARDDRIVE | DEBUG_SMP | DEBUG_SCHEDULER | DEBUG_THREAD | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED)
//#define DEBUG_OPTIONS (DEBUG_EXCEPTIONS | DEBUG_BOOT)
extern __uint128_t kDebugLevel;

#endif // CONFIG_H
