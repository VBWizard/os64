#include "sysstat.h"

static int options(int argc, char **argv, sysstat_options_t *out)
{
    *out = (sysstat_options_t){.count = 10, .interval_ms = 1000};
    const os64_optspec_t specs[] = {
        {'n', "count", true, "Reports to print (1..4294967295; default 10)", .flag = NULL},
        {'i', "interval-ms", true, "Sampling interval in ms (100..60000; default 1000)", .flag = NULL},
        {'p', "tid", true, "Select a task ID (TID in top); include its worker threads", .flag = NULL},
        {'t', "top", true, "Show the busiest N non-idle, live tasks (0..512; default 0)", .flag = NULL},
        {'c', "cores", false, "Include one record per CPU core", .flag = NULL},
        {'N', "network", false, "Include interface traffic and TCP counters", .flag = NULL},
        {'C', "cache", false, "Include block-cache activity (not device I/O)", .flag = NULL},
        {'j', "json", false, "Newline-delimited JSON: samples followed by a summary", .flag = NULL},
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    args.about = "Sample CPU, memory and task activity without a terminal";
    args.details = "An initial baseline precedes the first report. Machine CPU: 100% = the machine; task CPU: 100% = one core.";
    int32_t arg;
    while ((arg = os64_args_next(&args)) != OS64_ARG_END) {
        uint64_t value;
        switch (arg) {
        case OS64_ARG_HELP:
            os64_args_help(&args, "sysstat [options]");
            return 1;
        case 'c': out->cores = true; break;
        case 'N': out->network = true; break;
        case 'C': out->cache = true; break;
        case 'j': out->json = true; break;
        case 'n': case 'i': case 'p': case 't':
            if (!os64_parse_u64(args.value, &value)) goto bad;
            if (arg == 'n') {
                if (!value || value > UINT32_MAX) goto bad;
                out->count = (uint32_t)value;
            } else if (arg == 'i') {
                if (value < 100 || value > 60000) goto bad;
                out->interval_ms = (uint32_t)value;
            } else if (arg == 't') {
                if (value > SYSSTAT_TASKS) goto bad;
                out->top = (uint32_t)value;
            } else {
                out->selected = true;
                out->task_id = value;
            }
            break;
        default: goto bad;
        }
    }
    return 0;
bad:
    os64_hprintf(OS64_STDERR, "sysstat: invalid argument; see sysstat --help\n");
    return -1;
}

static int64_t wait_until(uint64_t deadline, uint64_t last)
{
    for (;;) {
        int64_t now = os64_micros();
        if (now < 0 || (uint64_t)now < last) return -1;
        if ((uint64_t)now >= deadline) return now;
        uint64_t remaining = deadline - (uint64_t)now;
        int64_t rc = os64_sleep(remaining / 1000 + (remaining % 1000 != 0));
        if (rc < 0 && rc != OS64_INTERRUPTED) return -1;
        last = (uint64_t)now;
    }
}

static void accumulate(sysstat_summary_t *sum, const os64_monitor_snapshot_t *s,
                        const os64_monitor_io_snapshot_t *io, const sysstat_interval_t *time)
{
    sum->samples++;
    if (s->partial || !s->memory_valid || !s->summary_valid || (io && io->partial)) sum->partial_samples++;
    if (time->elapsed_us > sum->max_interval_us) sum->max_interval_us = time->elapsed_us;
    if (time->late_us > sum->max_late_us) sum->max_late_us = time->late_us;
    sum->skipped += time->skipped;
    if (s->memory_valid) {
        if (!sum->memory_samples || s->memory.available < sum->min_available)
            sum->min_available = s->memory.available;
        sum->memory_samples++;
    }
    if (s->summary_valid && s->interval_us) {
        uint64_t busy = s->machine_percent[0];
        sum->cpu_samples++;
        if (busy > sum->peak_busy) sum->peak_busy = busy;
        if (sum->average_valid) {
            if ((busy && s->interval_us > UINT64_MAX / busy) ||
                sum->cpu_weight_us > UINT64_MAX - s->interval_us ||
                sum->busy_weighted > UINT64_MAX - busy * s->interval_us)
                sum->average_valid = false;
            else {
                sum->cpu_weight_us += s->interval_us;
                sum->busy_weighted += busy * s->interval_us;
            }
        }
    }
}

int main(int argc, char **argv)
{
    sysstat_options_t opts;
    int parsed = options(argc, argv, &opts);
    if (parsed) return parsed > 0 ? 0 : 2;
    os64_monitor_t *monitor = os64_monitor_create(SYSSTAT_TASKS,
        opts.selected ? SYSSTAT_THREADS : 0, SYSSTAT_CORES);
    if (!monitor) {
        os64_hprintf(OS64_STDERR, "sysstat: could not allocate monitor\n");
        return 1;
    }
    uint32_t sources = (opts.network ? OS64_MONITOR_IO_NETWORK : 0) |
                       (opts.cache ? OS64_MONITOR_IO_CACHE : 0);
    os64_monitor_io_t *io_monitor = sources ? os64_monitor_io_create(sources, 16) : NULL;
    if (sources && !io_monitor) {
        os64_hprintf(OS64_STDERR, "sysstat: could not allocate I/O monitor\n");
        os64_monitor_destroy(monitor);
        return 1;
    }
    const os64_monitor_io_snapshot_t *io = NULL;
    sysstat_output_t output = {0};
    sysstat_summary_t summary = {.average_valid = true};
    const os64_monitor_snapshot_t *s;
    int status = 1;
    const char *error = "could not collect baseline";
    if (os64_monitor_sample(monitor, opts.selected, &s) < 0) goto done;
    if (io_monitor && os64_monitor_io_sample(io_monitor, &io) < 0) goto done;
    int64_t stamp = os64_micros();
    error = "monotonic clock unavailable or invalid";
    if (stamp < 0) goto done;
    uint64_t last = (uint64_t)stamp;
    uint64_t period = (uint64_t)opts.interval_ms * 1000;
    if (last > INT64_MAX - period) goto done;
    uint64_t deadline = last + period, skipped = 0;
    bool memory_valid = s->memory_valid;
    uint64_t memory_used = s->memory.used;
    for (uint64_t n = 1; n <= opts.count; n++) {
        error = "clock or sleep failed";
        int64_t start = wait_until(deadline, last);
        if (start < 0) goto done;
        error = "could not collect sample";
        if (os64_monitor_sample(monitor, opts.selected, &s) < 0) goto done;
        if (io_monitor && os64_monitor_io_sample(io_monitor, &io) < 0) goto done;
        stamp = os64_micros();
        error = "monotonic clock unavailable or invalid";
        if (stamp < start) goto done;
        uint64_t end = (uint64_t)stamp;
        sysstat_interval_t interval = {
            .number = n, .uptime_us = end, .elapsed_us = end - last,
            .collection_us = end - (uint64_t)start,
            .late_us = (uint64_t)start - deadline, .skipped = skipped,
            .memory_before_valid = memory_valid, .memory_before = memory_used
        };
        accumulate(&summary, s, io, &interval);
        sysstat_report(&output, &opts, monitor, s, io, &interval);
        error = "output write failed";
        if (output.failed) goto done;
        last = end;
        memory_valid = s->memory_valid;
        memory_used = s->memory.used;
        if (n == opts.count) break;
        stamp = os64_micros();
        error = "monotonic clock unavailable or invalid";
        if (stamp < 0 || (uint64_t)stamp < end) goto done;
        // Keep the cadence anchored to its deadlines. Skip elapsed deadlines
        // after slow collection/output instead of emitting a catch-up burst.
        skipped = ((uint64_t)stamp - deadline) / period;
        if (skipped + 1 > (INT64_MAX - deadline) / period) goto done;
        deadline += (skipped + 1) * period;
    }
    status = 0;
done:
    if (status) os64_hprintf(OS64_STDERR, "sysstat: %s\n", error);
    if (summary.samples && !output.failed) {
        sysstat_finish(&output, &opts, &summary, status == 0);
        if (output.failed) {
            if (!status) os64_hprintf(OS64_STDERR, "sysstat: output write failed\n");
            status = 1;
        }
    }
    os64_monitor_io_destroy(io_monitor);
    os64_monitor_destroy(monitor);
    return status;
}
