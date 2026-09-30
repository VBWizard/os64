#include "os64/monitor.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/mem.h"
#include "os64/fmt.h"
#include "os64/str.h"

typedef struct {
    os64_monitor_snapshot_t snapshot;
    os64_monitor_task_t *tasks;
    os64_monitor_thread_t *threads;
    os64_monitor_core_t *cores;
} monitor_buffer_t;

struct os64_monitor {
    monitor_buffer_t buffers[2];
    size_t task_capacity, thread_capacity, core_capacity;
    unsigned current;
    bool have_previous;
    uint64_t sequence;
    os64_ticks_t initial_ticks;
};

// Compute floor(part * scale / whole) without overflowing the intermediate
// product. The small scales used here are 100 and 1000. Keep the remainder
// for both half-up rounding and largest-remainder apportionment.
static uint64_t scaled_ratio(uint64_t part, uint64_t whole, uint32_t scale,
                              uint64_t *remainder)
{
    *remainder = 0;
    if (!whole) return 0;
    uint64_t q = part / whole, r = part % whole;
    if (q > UINT64_MAX / scale) return UINT64_MAX;
    uint64_t value = q * scale;
    for (uint32_t i = 0; i < scale; i++) {
        if (*remainder >= whole - r) {
            *remainder -= whole - r;
            if (value == UINT64_MAX) return value;
            value++;
        } else *remainder += r;
    }
    return value;
}

static void percentages(const os64_monitor_time_t *d, uint32_t out[3])
{
    uint64_t parts[3] = {d->busy, d->idle, d->sched};
    uint64_t whole = d->busy + d->idle + d->sched, rem[3];
    uint32_t assigned = 0;
    for (unsigned i = 0; i < 3; i++) {
        out[i] = (uint32_t)scaled_ratio(parts[i], whole, 100, &rem[i]);
        assigned += out[i];
    }
    for (unsigned pass = 0; assigned < 100 && pass < 3; pass++) {
        unsigned best = 3;
        for (unsigned i = 0; i < 3; i++)
            if (rem[i] && (best == 3 || rem[i] > rem[best])) best = i;
        if (best == 3) break;
        out[best]++; rem[best] = 0; assigned++;
    }
}

static uint64_t ticks_us(uint64_t ticks, uint64_t rate)
{
    if (!rate) return 0;
    uint64_t seconds = ticks / rate;
    if (seconds > UINT64_MAX / 1000000) return UINT64_MAX;
    uint64_t rem;
    // Rates are uint32_t in the ABI, so this fractional product fits.
    rem = (ticks % rate) * 1000000 / rate;
    uint64_t value = seconds * 1000000;
    return value > UINT64_MAX - rem ? UINT64_MAX : value + rem;
}

static void *array_alloc(size_t count, size_t size)
{
    if (!count || count > SIZE_MAX / size) return NULL;
    return os64_malloc(count * size);
}

os64_monitor_t *os64_monitor_create(size_t tasks, size_t threads, size_t cores)
{
    if (!tasks || !cores) return NULL;
    os64_monitor_t *m = os64_malloc(sizeof(*m));
    if (!m) return NULL;
    os64_memset(m, 0, sizeof(*m));
    m->task_capacity = tasks; m->thread_capacity = threads; m->core_capacity = cores;
    for (unsigned i = 0; i < 2; i++) {
        monitor_buffer_t *b = &m->buffers[i];
        b->tasks = array_alloc(tasks, sizeof(*b->tasks));
        if (threads) b->threads = array_alloc(threads, sizeof(*b->threads));
        b->cores = array_alloc(cores, sizeof(*b->cores));
        if (!b->tasks || (threads && !b->threads) || !b->cores) {
            os64_monitor_destroy(m);
            return NULL;
        }
    }
    os64_ticks(&m->initial_ticks);
    return m;
}

void os64_monitor_destroy(os64_monitor_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < 2; i++) {
        os64_free(m->buffers[i].tasks);
        os64_free(m->buffers[i].threads);
        os64_free(m->buffers[i].cores);
    }
    os64_free(m);
}

static void collect_threads(os64_monitor_t *m, monitor_buffer_t *b, uint64_t pid)
{
    os64_monitor_snapshot_t *s = &b->snapshot;
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/thread", pid);
    int64_t h = os64_opendir(path);
    if (h < 0) { s->partial |= OS64_MONITOR_THREADS_PARTIAL; return; }
    os64_dirent_t entry;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)h, &entry)) == 1) {
        uint64_t tid;
        if (!os64_parse_u64(entry.name, &tid)) continue;
        if (s->thread_count == m->thread_capacity) {
            s->partial |= OS64_MONITOR_THREADS_PARTIAL;
            break;
        }
        os64_monitor_thread_t *t = &b->threads[s->thread_count];
        os64_memset(t, 0, sizeof(*t));
        if (os64_proc_read_thread(pid, tid, &t->info) == 0) s->thread_count++;
    }
    if (os64_close((int32_t)h) < 0 || rc < 0) s->partial |= OS64_MONITOR_THREADS_PARTIAL;
}

static int collect_tasks(os64_monitor_t *m, monitor_buffer_t *b, bool threads)
{
    os64_monitor_snapshot_t *s = &b->snapshot;
    int64_t h = os64_opendir("/proc");
    if (h < 0) return -1;
    os64_dirent_t entry;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)h, &entry)) == 1) {
        uint64_t pid;
        if (!os64_parse_u64(entry.name, &pid)) continue;
        if (s->task_count == m->task_capacity) {
            s->partial |= OS64_MONITOR_TASKS_PARTIAL;
            break;
        }
        os64_monitor_task_t *t = &b->tasks[s->task_count];
        os64_memset(t, 0, sizeof(*t));
        if (os64_proc_read(pid, &t->info) < 0) continue;
        s->task_count++;
        if (t->info.state == OS64_PROC_ZOMBIE) s->zombies++;
        if (threads && t->info.threads >= 2) collect_threads(m, b, pid);
    }
    int64_t closed = os64_close((int32_t)h);
    return rc < 0 || closed < 0 ? -1 : 0;
}

static bool read_core(uint32_t id, os64_monitor_time_t *time)
{
    char path[64], line[256];
    os64_snprintf(path, sizeof(path), "/sys/cpu/%u/time", id);
    int64_t h = os64_open(path, NULL);
    if (h < 0) return false;
    const char *keys[4] = {"total_us:", "busy_us:", "idle_us:", "sched_us:"};
    uint64_t values[4] = {0};
    unsigned fields = 0;
    bool good = true;
    int64_t rc;
    while ((rc = os64_readline((int32_t)h, line, sizeof(line))) == 1) {
        char *value = line;
        while (*value && *value != ' ' && *value != '\t') value++;
        if (!*value) continue;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') value++;
        for (unsigned i = 0; i < 4; i++) if (os64_streq(line, keys[i])) {
            if ((fields & (1u << i)) || !os64_parse_u64(value, &values[i])) good = false;
            fields |= 1u << i;
        }
    }
    if (os64_close((int32_t)h) < 0 || rc < 0) good = false;
    if (!good || fields != 15) return false;
    *time = (os64_monitor_time_t){values[0], values[1], values[2], values[3]};
    // The producer rounds each bucket to microseconds independently. Permit
    // that rounding, but reject impossible ledgers and arithmetic overflow.
    if (time->busy > time->total || time->idle > time->total || time->sched > time->total ||
        time->busy > UINT64_MAX - time->idle ||
        time->busy + time->idle > UINT64_MAX - time->sched) return false;
    uint64_t sum = time->busy + time->idle + time->sched;
    return sum > time->total ? sum - time->total <= 2 : time->total - sum <= 2;
}

static void collect_cores(os64_monitor_t *m, monitor_buffer_t *b)
{
    os64_monitor_snapshot_t *s = &b->snapshot;
    int64_t h = os64_opendir("/sys/cpu");
    if (h < 0) { s->partial |= OS64_MONITOR_CORES_PARTIAL; return; }
    os64_dirent_t entry;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)h, &entry)) == 1) {
        uint64_t id;
        if (!os64_parse_u64(entry.name, &id) || id > UINT32_MAX) continue;
        if (s->core_count == m->core_capacity) { s->partial |= OS64_MONITOR_CORES_PARTIAL; break; }
        os64_monitor_core_t *c = &b->cores[s->core_count];
        os64_memset(c, 0, sizeof(*c)); c->id = (uint32_t)id;
        if (!read_core(c->id, &c->time)) { s->partial |= OS64_MONITOR_CORES_PARTIAL; continue; }
        s->core_count++;
    }
    if (os64_close((int32_t)h) < 0 || rc < 0) s->partial |= OS64_MONITOR_CORES_PARTIAL;
}

static const os64_monitor_core_t *find_core(const os64_monitor_snapshot_t *s, uint32_t id)
{
    for (size_t i = 0; i < s->core_count; i++) if (s->cores[i].id == id) return &s->cores[i];
    return NULL;
}

static os64_monitor_usage_t usage(uint64_t now, uint64_t before, uint64_t interval)
{
    os64_monitor_usage_t u = {0};
    if (!interval || now < before) return u;
    u.valid = true; u.delta_us = now - before;
    uint64_t rem;
    u.percent = scaled_ratio(u.delta_us, interval, 100, &rem);
    if (rem >= interval - rem && u.percent < UINT64_MAX) u.percent++;
    return u;
}

static bool add_time(os64_monitor_time_t *sum, const os64_monitor_time_t *d)
{
    if (sum->busy > UINT64_MAX - d->busy || sum->idle > UINT64_MAX - d->idle ||
        sum->sched > UINT64_MAX - d->sched) return false;
    sum->busy += d->busy; sum->idle += d->idle; sum->sched += d->sched;
    return sum->busy <= UINT64_MAX - sum->idle &&
           sum->busy + sum->idle <= UINT64_MAX - sum->sched;
}

static void calculate(os64_monitor_t *m, monitor_buffer_t *b,
                       const os64_monitor_snapshot_t *previous)
{
    os64_monitor_snapshot_t *s = &b->snapshot;
    const os64_ticks_t *then = m->have_previous ? &previous->ticks : &m->initial_ticks;
    if (then->per_second == s->ticks.per_second && s->ticks.ticks >= then->ticks)
        s->tick_interval_us = ticks_us(s->ticks.ticks - then->ticks, s->ticks.per_second);
    s->interval_us = s->tick_interval_us;
    for (size_t i = 0; i < s->core_count; i++) {
        os64_monitor_core_t *c = &b->cores[i];
        const os64_monitor_core_t *p = m->have_previous ? find_core(previous, c->id) : NULL;
        if (!p || c->time.total < p->time.total || c->time.busy < p->time.busy ||
            c->time.idle < p->time.idle || c->time.sched < p->time.sched) continue;
        c->valid = true;
        c->delta = (os64_monitor_time_t){c->time.total - p->time.total,
            c->time.busy - p->time.busy, c->time.idle - p->time.idle,
            c->time.sched - p->time.sched};
        if (c->id == 0) s->ledger_interval_us = c->delta.total;
    }
    // Match top's clock policy: use core 0's ledger within the sanity window,
    // otherwise fall back to ticks. Task CPU time and this ledger share a clock.
    uint64_t tick = s->tick_interval_us, ledger = s->ledger_interval_us;
    if (ledger > tick / 2 && (tick > UINT64_MAX / 4 || ledger < tick * 4)) s->interval_us = ledger;
    if (ledger) {
        s->skew_valid = true; s->skew_negative = tick < ledger;
        uint64_t mag = tick < ledger ? ledger - tick : tick - ledger, rem;
        s->skew_tenths = scaled_ratio(mag, ledger, 1000, &rem);
    }
    os64_monitor_time_t machine = {0};
    s->summary_valid = m->have_previous && s->interval_us && s->core_count &&
                       !(s->partial & OS64_MONITOR_CORES_PARTIAL);
    for (size_t i = 0; i < s->core_count; i++) {
        os64_monitor_core_t *c = &b->cores[i];
        if (!c->valid) { s->summary_valid = false; continue; }
        c->parked = !c->delta.total || c->delta.total < s->interval_us / 100;
        if (c->parked) {
            s->parked++;
            os64_monitor_time_t idle = {.idle = s->interval_us};
            if (!add_time(&machine, &idle)) s->summary_valid = false;
        } else {
            percentages(&c->delta, c->percent);
            if (!add_time(&machine, &c->delta)) s->summary_valid = false;
        }
    }
    if (s->summary_valid) percentages(&machine, s->machine_percent);
    if (!m->have_previous) return;
    for (size_t i = 0; i < s->task_count; i++) {
        os64_monitor_task_t *t = &b->tasks[i];
        for (size_t j = 0; j < previous->task_count; j++) {
            const os64_monitor_task_t *p = &previous->tasks[j];
            if (t->info.pid != p->info.pid) continue;
            t->cpu = usage(t->info.runtime_us, p->info.runtime_us, s->interval_us);
            break;
        }
    }
    for (size_t i = 0; i < s->thread_count; i++) {
        os64_monitor_thread_t *t = &b->threads[i];
        for (size_t j = 0; j < previous->thread_count; j++) {
            const os64_monitor_thread_t *p = &previous->threads[j];
            if (t->info.pid != p->info.pid || t->info.tid != p->info.tid) continue;
            t->cpu = usage(t->info.runtime_us, p->info.runtime_us, s->interval_us);
            break;
        }
    }
}

int32_t os64_monitor_sample(os64_monitor_t *m, bool threads,
                            const os64_monitor_snapshot_t **out)
{
    if (!out) return -1;
    *out = NULL;
    if (!m) return -1;
    monitor_buffer_t *b = &m->buffers[m->current ^ 1u];
    os64_monitor_snapshot_t *s = &b->snapshot;
    os64_memset(s, 0, sizeof(*s));
    s->tasks = b->tasks; s->threads = b->threads; s->cores = b->cores;
    s->threads_sampled = threads;
    if (collect_tasks(m, b, threads) < 0) goto failed;
    collect_cores(m, b);
    if (os64_ticks(&s->ticks) < 0 || !s->ticks.per_second) goto failed;
    s->memory_valid = os64_memory(&s->memory) == 0;
    calculate(m, b, &m->buffers[m->current].snapshot);
    s->sequence = ++m->sequence;
    m->current ^= 1u; m->have_previous = true;
    *out = s;
    return 0;
failed:
    m->have_previous = false;
    os64_memset(&m->initial_ticks, 0, sizeof(m->initial_ticks));
    return -1;
}
