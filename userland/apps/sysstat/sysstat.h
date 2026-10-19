#ifndef SYSSTAT_H
#define SYSSTAT_H

#include "os64/os64.h"
#include "os64/monitor.h"
#include "os64/monitor_io.h"

#define SYSSTAT_TASKS 512
#define SYSSTAT_THREADS 1024
#define SYSSTAT_CORES 128

typedef struct {
    uint32_t count, interval_ms, top;
    uint64_t task_id;
    bool selected, cores, json, network, cache;
} sysstat_options_t;

typedef struct {
    uint64_t number, uptime_us, elapsed_us, collection_us, late_us, skipped;
    bool memory_before_valid;
    uint64_t memory_before;
} sysstat_interval_t;

typedef struct {
    uint64_t samples, cpu_samples, memory_samples, partial_samples;
    uint64_t cpu_weight_us, busy_weighted, peak_busy, min_available;
    uint64_t max_interval_us, max_late_us, skipped;
    bool average_valid;
} sysstat_summary_t;

typedef struct {
    char bytes[4096];
    size_t used;
    bool failed;
} sysstat_output_t;

void sysstat_report(sysstat_output_t *out, const sysstat_options_t *options,
                    const os64_monitor_t *monitor, const os64_monitor_snapshot_t *sample,
                    const os64_monitor_io_snapshot_t *io, const sysstat_interval_t *interval);
void sysstat_finish(sysstat_output_t *out, const sysstat_options_t *options,
                    const sysstat_summary_t *summary, bool complete);

#endif
