#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "smp.h"

//Set to sizeof(log_entry_t)*10 to enable buffer full processing

// 16MB/core (was 5): the DETAILED boot burst peaked at ~11.9k of 17.7k
// entries (~67% of 5MB) — and the burst happens BEFORE any filesystem
// exists, so only capacity can absorb it (the post-mount file sink handles
// steady state; see the klog plan). 3x headroom over worst observed; crank
// freely — never-drop is the house rule and RAM is cheap here.
#define LOG_BUFFER_SIZE_MB 16
#define LOG_BUFFER_SIZE (1024 * 1024 * LOG_BUFFER_SIZE_MB) // Size of buffer per core
#define MAX_LOG_MESSAGE_SIZE 256
// Drain twice a second: smaller accumulations per pass mean shorter serial
// bursts, which keeps QEMU/TCG from stalling the whole VM (and the guest
// clock) on one big once-a-second flush.
#define LOGD_SLEEP_TICKS (TICKS_PER_SECOND / 2)
// Bound on entries drained per kLogDWorkLock hold. The old until-empty merge
// never terminated under sustained production (DEBUG_DETAILED at 100 passes/
// sec): the daemon acquired the lock once and held it FOREVER — the stats
// heartbeat stopped printing, and the producers' forced-flush fallback could
// never take the lock, so its try-lock failure became the "buffer full"
// panic (2026-07-11, VBox). Bounded holds let the lock breathe; the daemon
// loops straight back (no sleep) while a backlog remains, so throughput is
// unchanged.
#define LOGD_DRAIN_CHUNK 64
// While a backlog persists, print the [logd] stats heartbeat at most once
// per this many passes (quiet passes always print) — the heartbeat must
// survive sustained load without becoming its own flood.
#define LOGD_STATS_EVERY 64
typedef struct log_entry {
    uint64_t ticks;     // kTicksSinceStart at log time — used for display
    uint64_t tsc;       // RDTSC at log time — used for cross-core sort order
    uint16_t core_id;
    uint8_t log_level;
    uint8_t category;
	uint64_t threadID;
    /// @brief message is the continuation of a previous entry
    bool continued;
    char message[MAX_LOG_MESSAGE_SIZE];
} log_entry_t;

typedef struct log_buffer {
    log_entry_t *entries;
    size_t head;
    size_t tail;
    size_t capacity;
    _Atomic uint32_t lock;
} log_buffer_t;

extern log_buffer_t core_log_buffers[MAX_CPUS];
extern bool kLoggingInitialized;
void logging_queueing_init();
void dump_log_buffer(uint16_t core);
void log_store_entry(uint16_t core, uint64_t ticks, uint8_t priority, uint8_t category, bool continued, const char *message);
bool logd_thread(bool daemon);
#endif // LOG_H
