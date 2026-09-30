#ifndef OS64_MONITOR_H
#define OS64_MONITOR_H

#include "os64/procfs.h"
#include "os64/memory.h"
#include "os64/ticks.h"

// Sampling state belongs to the caller, so separate monitors may refresh at
// different rates. Serialize access to one instance; different instances are
// independent. This module has no terminal, GUI, sleep, or process-control code.
typedef struct os64_monitor os64_monitor_t;

typedef struct {
    uint64_t delta_us;
    uint64_t percent;       // rounded half up; 100 means one CPU, may exceed 100
    bool valid;            // false on first sighting, a gap, or counter reset
} os64_monitor_usage_t;

typedef struct {
    os64_proc_info_t info;
    os64_monitor_usage_t cpu;
} os64_monitor_task_t;

typedef struct {
    os64_thread_info_t info;
    os64_monitor_usage_t cpu;
} os64_monitor_thread_t;

typedef struct {
    uint64_t total, busy, idle, sched; // microseconds, from the CPU ledger
} os64_monitor_time_t;

typedef struct {
    uint32_t id;
    os64_monitor_time_t time, delta;
    uint32_t percent[3];    // busy, idle, scheduler; largest-remainder rounding
    bool valid, parked;
} os64_monitor_core_t;

enum {
    OS64_MONITOR_TASKS_PARTIAL   = 1u << 0,
    OS64_MONITOR_THREADS_PARTIAL = 1u << 1,
    OS64_MONITOR_CORES_PARTIAL   = 1u << 2
};

typedef struct {
    const os64_monitor_task_t *tasks;
    const os64_monitor_thread_t *threads;
    const os64_monitor_core_t *cores;
    size_t task_count, thread_count, core_count;
    size_t zombies, parked;
    uint32_t partial;       // bounded output or a directory/report read failed
    bool threads_sampled, memory_valid, summary_valid;
    os64_memory_t memory;
    os64_ticks_t ticks;
    uint64_t sequence;
    uint64_t interval_us, tick_interval_us, ledger_interval_us;
    uint32_t machine_percent[3];
    bool skew_valid, skew_negative;
    uint64_t skew_tenths;   // tick clock's deviation from the CPU ledger
} os64_monitor_snapshot_t;

// Limits bound memory and collection work; choose them for the application.
// Task/core capacity must be positive. Thread capacity may be zero. NULL on
// invalid sizes or allocation failure. Sample buffers are allocated here.
os64_monitor_t *os64_monitor_create(size_t tasks, size_t threads, size_t cores);
void os64_monitor_destroy(os64_monitor_t *monitor);

// Collect tasks (including hidden/zombie tasks), optional threads of tasks
// with >=2 threads, core ledgers and memory. Files are individual snapshots,
// not an atomic whole-machine census. Vanished tasks/threads are skipped.
//
// Returns 0 and a borrowed, read-only snapshot. Its storage is valid until
// the next sample call or destruction. -1 means /proc enumeration or the
// clock failed; *out is NULL and history is reset, so recovery cannot charge
// several missed intervals to one refresh. Missing optional sources are
// indicated by flags rather than fabricated zero measurements. Check partial
// even on success. Turning thread sampling back on starts a new baseline.
int32_t os64_monitor_sample(os64_monitor_t *monitor, bool threads,
                            const os64_monitor_snapshot_t **out);

#endif
