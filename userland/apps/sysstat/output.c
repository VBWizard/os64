#include "sysstat.h"

static void flush(sysstat_output_t *out)
{
    size_t sent = 0;
    while (sent < out->used && !out->failed) {
        int64_t n = os64_write(OS64_STDOUT, out->bytes + sent, out->used - sent);
        if (n == OS64_INTERRUPTED) continue;
        if (n <= 0 || (uint64_t)n > out->used - sent) out->failed = true;
        else sent += (size_t)n;
    }
    out->used = 0;
}

static void bytes(sysstat_output_t *out, const char *text, size_t n)
{
    while (n && !out->failed) {
        size_t room = sizeof(out->bytes) - out->used;
        size_t take = n < room ? n : room;
        os64_memcpy(out->bytes + out->used, text, take);
        out->used += take; text += take; n -= take;
        if (out->used == sizeof(out->bytes)) flush(out);
    }
}

static void emit(sysstat_output_t *out, const char *fmt, ...) OS64_PRINTF(2, 3);
static void emit(sysstat_output_t *out, const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    int32_t n = os64_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(text)) { out->failed = true; return; }
    bytes(out, text, (size_t)n);
}

static void number(sysstat_output_t *out, bool valid, uint64_t n, bool json)
{
    if (valid) emit(out, "%lu", n);
    else emit(out, "%s", json ? "null" : "-");
}

static void difference(sysstat_output_t *out, uint64_t now, uint64_t before)
{
    if (now < before) emit(out, "-%lu", before - now);
    else emit(out, "%lu", now - before);
}

static void string(sysstat_output_t *out, const char *text, bool json)
{
    if (json) emit(out, "\"");
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned char c = *p++;
        if (!json) { emit(out, "%c", c >= 32 && c < 127 ? c : '?'); continue; }
        if (c == '"' || c == '\\') emit(out, "\\%c", c);
        else if (c < 32 || c == 127) emit(out, "\\u%04x", (unsigned)c);
        else if (c < 128) bytes(out, (const char *)&c, 1);
        else {
            // Preserve valid UTF-8, replacing malformed bytes so each record
            // remains valid JSON even when a task name contains arbitrary bytes.
            unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 :
                             c >= 0xe0 && c <= 0xef ? 2 :
                             c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            bool valid = extra != 0;
            for (unsigned i = 0; valid && i < extra; i++)
                if (p[i] < 0x80 || p[i] > 0xbf) valid = false;
            if (valid && ((c == 0xe0 && p[0] < 0xa0) || (c == 0xed && p[0] >= 0xa0) ||
                          (c == 0xf0 && p[0] < 0x90) || (c == 0xf4 && p[0] >= 0x90))) valid = false;
            if (valid) { bytes(out, (const char *)(p - 1), extra + 1); p += extra; }
            else emit(out, "\\ufffd");
        }
    }
    if (json) emit(out, "\"");
}

static void json_cpu(sysstat_output_t *out, bool valid, const uint32_t percent[3],
                      const os64_monitor_core_split_t *split)
{
    emit(out, "{\"busy_percent\":"); number(out, valid, percent[0], true);
    emit(out, ",\"user_percent\":"); number(out, valid && split->valid, split->percent[0], true);
    emit(out, ",\"system_percent\":"); number(out, valid && split->valid, split->percent[1], true);
    emit(out, ",\"scheduler_percent\":"); number(out, valid, percent[2], true);
    emit(out, ",\"idle_percent\":"); number(out, valid, percent[1], true);
    emit(out, "}");
}

static uint64_t thread_count(const os64_monitor_snapshot_t *s)
{
    uint64_t total = 0;
    for (size_t i = 0; i < s->task_count; i++) total += s->tasks[i].info.threads;
    return total;
}

static const os64_monitor_task_t *find_task(const os64_monitor_snapshot_t *s, uint64_t id)
{
    for (size_t i = 0; i < s->task_count; i++)
        if (s->tasks[i].info.pid == id) return &s->tasks[i];
    return NULL;
}

static bool idle_task(const os64_monitor_t *m, const os64_monitor_task_t *task)
{
    os64_monitor_split_t split = {0};
    if (os64_monitor_task_split(m, task->info.pid, &split) && split.idle) return true;
    // Older kernels omit the explicit idle field; use the same fallback as htop.
    const os64_proc_info_t *p = &task->info;
    if (!p->kernel || os64_memcmp(p->name, "idle", 4) || !p->name[4]) return false;
    for (const char *c = p->name + 4; *c; c++) if (*c < '0' || *c > '9') return false;
    return true;
}

static size_t busiest(const os64_monitor_t *m, const os64_monitor_snapshot_t *s,
                       const os64_monitor_task_t **ranked, size_t limit)
{
    size_t count = 0;
    for (size_t i = 0; i < s->task_count; i++) {
        const os64_monitor_task_t *t = &s->tasks[i];
        if (!t->cpu.valid || t->info.state == OS64_PROC_ZOMBIE || idle_task(m, t)) continue;
        size_t at = 0;
        while (at < count && (ranked[at]->cpu.delta_us > t->cpu.delta_us ||
               (ranked[at]->cpu.delta_us == t->cpu.delta_us && ranked[at]->info.pid < t->info.pid))) at++;
        if (at >= limit) continue;
        if (count < limit) count++;
        for (size_t j = count - 1; j > at; j--) ranked[j] = ranked[j - 1];
        ranked[at] = t;
    }
    return count;
}

static void json_usage(sysstat_output_t *out, const os64_monitor_usage_t *cpu,
                        const os64_monitor_split_t *split)
{
    emit(out, "\"cpu_percent\":"); number(out, cpu->valid, cpu->percent, true);
    emit(out, ",\"cpu_delta_us\":"); number(out, cpu->valid, cpu->delta_us, true);
    emit(out, ",\"user_percent\":"); number(out, split->user.valid, split->user.percent, true);
    emit(out, ",\"system_percent\":"); number(out, split->system.valid, split->system.percent, true);
}

static void json_task(sysstat_output_t *out, const os64_monitor_t *m,
                       const os64_monitor_snapshot_t *s, const os64_monitor_task_t *t, bool details)
{
    if (!t) { emit(out, "null"); return; }
    os64_monitor_split_t split = {0};
    os64_monitor_task_split(m, t->info.pid, &split);
    emit(out, "{\"task_id\":%lu,\"name\":", t->info.pid); string(out, t->info.name, true);
    emit(out, ",\"state\":"); string(out, os64_proc_state_name(t->info.state), true);
    emit(out, ",\"core_id\":%u,\"thread_count\":%u,\"heap_bytes\":%lu,",
         t->info.core, t->info.threads, t->info.heap);
    json_usage(out, &t->cpu, &split);
    if (details) {
        os64_monitor_task_events_t events = {0};
        os64_monitor_task_events(m, t->info.pid, &events);
        emit(out, ",\"switches_delta\":"); number(out, events.valid, events.switches, true);
        emit(out, ",\"minor_faults_delta\":"); number(out, events.valid, events.minor_faults, true);
        emit(out, ",\"major_faults_delta\":"); number(out, events.valid, events.major_faults, true);
        bool sampled = s->threads_sampled && t->info.threads >= 2;
        size_t seen = 0;
        emit(out, ",\"threads\":");
        if (!sampled) emit(out, "null");
        else {
            emit(out, "[");
            for (size_t i = 0; i < s->thread_count; i++) {
                const os64_monitor_thread_t *th = &s->threads[i];
                if (th->info.pid != t->info.pid) continue;
                if (seen++) emit(out, ",");
                split = (os64_monitor_split_t){0};
                os64_monitor_thread_split(m, th->info.pid, th->info.tid, &split);
                emit(out, "{\"thread_id\":%lu,\"core_id\":%u,\"state\":", th->info.tid, th->info.core);
                string(out, os64_proc_state_name(th->info.state), true);
                emit(out, ","); json_usage(out, &th->cpu, &split); emit(out, "}");
            }
            emit(out, "]");
        }
        emit(out, ",\"threads_complete\":%s", sampled && seen == t->info.threads &&
             !(s->partial & OS64_MONITOR_THREADS_PARTIAL) ? "true" : "false");
    }
    emit(out, "}");
}

static void text_cpu(sysstat_output_t *out, bool valid, const uint32_t percent[3],
                      const os64_monitor_core_split_t *split)
{
    uint32_t values[] = {percent[0], split->percent[0], split->percent[1], percent[2], percent[1]};
    for (unsigned i = 0; i < 5; i++) {
        if (i) emit(out, " ");
        if (valid && ((i != 1 && i != 2) || split->valid)) emit(out, "%4u", values[i]);
        else emit(out, "%4s", "-");
    }
}

static void text_cores(sysstat_output_t *out, const os64_monitor_t *m,
                        const os64_monitor_snapshot_t *s)
{
    if (!s->core_count) { emit(out, "  No cores observed\n"); return; }
    unsigned id_width = 3;
    for (size_t i = 0; i < s->core_count; i++) {
        unsigned digits = 1;
        for (uint32_t id = s->cores[i].id; id >= 10; id /= 10) digits++;
        if (digits > id_width) id_width = digits;
    }
    // Fit an 80-column console without a terminal query (SSH exec may have
    // no terminal). Wider core IDs reduce the number of side-by-side groups.
    size_t columns = 81 / (id_width + 23);
    if (columns > s->core_count) columns = s->core_count;
    size_t rows = (s->core_count + columns - 1) / columns;
    emit(out, "  Core CPU (%%): BUS=busy USR=user SYS=system SCH=scheduler IDL=idle\n  ");
    for (size_t col = 0; col < columns && col * rows < s->core_count; col++) {
        if (col) emit(out, " | ");
        emit(out, "%*s BUS USR SYS SCH IDL", (int)id_width, "CPU");
    }
    emit(out, "\n");
    for (size_t row = 0; row < rows; row++) {
        emit(out, "  ");
        for (size_t col = 0; col < columns; col++) {
            size_t i = col * rows + row;
            if (i >= s->core_count) break;
            if (col) emit(out, " | ");
            const os64_monitor_core_t *c = &s->cores[i];
            emit(out, "%*u ", (int)id_width, c->id);
            if (c->valid && c->parked) { emit(out, "%-19s", "parked"); continue; }
            os64_monitor_core_split_t split = {0};
            os64_monitor_core_split(m, c->id, &split);
            uint32_t values[] = {c->percent[0], split.percent[0], split.percent[1],
                                 c->percent[2], c->percent[1]};
            for (unsigned k = 0; k < 5; k++) {
                if (k) emit(out, " ");
                if (c->valid && ((k != 1 && k != 2) || split.valid))
                    emit(out, "%3u", values[k]);
                else emit(out, "%3s", "-");
            }
        }
        emit(out, "\n");
    }
}

static void text_size(sysstat_output_t *out, bool valid, uint64_t size)
{
    char text[32];
    if (valid) os64_format_binary_size(size, text, sizeof(text));
    emit(out, "%7s", valid ? text : "-");
}

static void text_task(sysstat_output_t *out, const os64_monitor_t *m,
                       const os64_monitor_snapshot_t *s, const os64_monitor_task_t *t, bool details)
{
    if (!t) { emit(out, "  selected task unavailable (absent or collection incomplete)\n"); return; }
    os64_monitor_split_t split = {0};
    os64_monitor_task_split(m, t->info.pid, &split);
    emit(out, "  task %lu ", t->info.pid); string(out, t->info.name, false);
    emit(out, " cpu="); number(out, t->cpu.valid, t->cpu.percent, false);
    emit(out, "%% user="); number(out, split.user.valid, split.user.percent, false);
    emit(out, "%% system="); number(out, split.system.valid, split.system.percent, false);
    emit(out, "%% state=%s core=%u threads=%u heap_bytes=%lu\n",
         os64_proc_state_name(t->info.state), t->info.core, t->info.threads, t->info.heap);
    if (!details) return;
    os64_monitor_task_events_t events = {0};
    os64_monitor_task_events(m, t->info.pid, &events);
    emit(out, "    interval events: switches="); number(out, events.valid, events.switches, false);
    emit(out, " minor_faults="); number(out, events.valid, events.minor_faults, false);
    emit(out, " major_faults="); number(out, events.valid, events.major_faults, false); emit(out, "\n");
    if (t->info.threads < 2) return;
    size_t seen = 0;
    for (size_t i = 0; i < s->thread_count; i++) {
        const os64_monitor_thread_t *th = &s->threads[i];
        if (th->info.pid != t->info.pid) continue;
        seen++;
        split = (os64_monitor_split_t){0};
        os64_monitor_thread_split(m, th->info.pid, th->info.tid, &split);
        emit(out, "    thread %lu cpu=", th->info.tid); number(out, th->cpu.valid, th->cpu.percent, false);
        emit(out, "%% user="); number(out, split.user.valid, split.user.percent, false);
        emit(out, "%% system="); number(out, split.system.valid, split.system.percent, false);
        emit(out, "%% core=%u state=%s\n", th->info.core, os64_proc_state_name(th->info.state));
    }
    if (seen != t->info.threads || (s->partial & OS64_MONITOR_THREADS_PARTIAL))
        emit(out, "    thread details incomplete\n");
}

// Field offsets let text and JSON share the same names and validity rules.
typedef struct { const char *name; size_t offset; } io_field_t;
#define IO_FIELD(type, member) {#member, offsetof(type, member)}
static const io_field_t net_fields[] = {
    IO_FIELD(os64_monitor_net_values_t, rx_bytes), IO_FIELD(os64_monitor_net_values_t, tx_bytes),
    IO_FIELD(os64_monitor_net_values_t, rx_frames), IO_FIELD(os64_monitor_net_values_t, tx_frames),
    IO_FIELD(os64_monitor_net_values_t, rx_errors), IO_FIELD(os64_monitor_net_values_t, tx_errors),
    IO_FIELD(os64_monitor_net_values_t, rx_dropped_no_handler), IO_FIELD(os64_monitor_net_values_t, rx_dropped_too_big)
};
static const io_field_t tcp_fields[] = {
    IO_FIELD(os64_monitor_tcp_values_t, segments_in), IO_FIELD(os64_monitor_tcp_values_t, segments_out),
    IO_FIELD(os64_monitor_tcp_values_t, retransmits), IO_FIELD(os64_monitor_tcp_values_t, fast_retransmits),
    IO_FIELD(os64_monitor_tcp_values_t, tx_local_drops), IO_FIELD(os64_monitor_tcp_values_t, bad_checksum),
    IO_FIELD(os64_monitor_tcp_values_t, connect_timeouts), IO_FIELD(os64_monitor_tcp_values_t, resets_received)
};
static const io_field_t cache_fields[] = {
    IO_FIELD(os64_monitor_cache_values_t, hits), IO_FIELD(os64_monitor_cache_values_t, misses),
    IO_FIELD(os64_monitor_cache_values_t, fills), IO_FIELD(os64_monitor_cache_values_t, evictions),
    IO_FIELD(os64_monitor_cache_values_t, updates), IO_FIELD(os64_monitor_cache_values_t, bypass_edge),
    IO_FIELD(os64_monitor_cache_values_t, discarded_races)
};
#undef IO_FIELD
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void io_values(sysstat_output_t *out, bool valid, const void *values,
                      const io_field_t *fields, size_t count, bool json)
{
    if (json && !valid) { emit(out, "null"); return; }
    if (json) emit(out, "{");
    for (size_t i = 0; i < count; i++) {
        if (i) emit(out, "%s", json ? "," : " ");
        if (json) { string(out, fields[i].name, true); emit(out, ":"); }
        else emit(out, "%s=", fields[i].name);
        uint64_t value = *(const uint64_t *)((const unsigned char *)values + fields[i].offset);
        number(out, valid, value, json);
    }
    if (json) emit(out, "}");
}

// The caller opens the JSON object; this emits status and counter groups.
static void io_counters(sysstat_output_t *out, const os64_monitor_io_status_t *status,
                        const void *total, const void *delta, const void *rate,
                        const io_field_t *fields, size_t count, bool json)
{
    if (json) {
        emit(out, "\"available\":%s,\"sampled_at_us\":", status->available ? "true" : "false");
        number(out, status->clock_valid, status->sampled_at_us, true);
        emit(out, ",\"interval_us\":"); number(out, status->interval_us != 0, status->interval_us, true);
        emit(out, ",\"total\":"); io_values(out, status->available, total, fields, count, true);
        emit(out, ",\"delta\":"); io_values(out, status->delta_valid, delta, fields, count, true);
        emit(out, ",\"per_second\":"); io_values(out, status->rate_valid, rate, fields, count, true);
    } else {
        emit(out, "    per_second: "); io_values(out, status->rate_valid, rate, fields, count, false);
        emit(out, "\n");
    }
}

static void io_report(sysstat_output_t *out, const os64_monitor_io_snapshot_t *io, bool json)
{
    if (!io) return;
    if (json) emit(out, ",\"io_partial\":{\"interfaces\":%s,\"tcp\":%s,\"cache\":%s,\"clock\":%s}",
        io->partial & OS64_MONITOR_IO_INTERFACES_PARTIAL ? "true" : "false",
        io->partial & OS64_MONITOR_IO_TCP_UNAVAILABLE ? "true" : "false",
        io->partial & OS64_MONITOR_IO_CACHE_UNAVAILABLE ? "true" : "false",
        io->partial & OS64_MONITOR_IO_CLOCK_UNAVAILABLE ? "true" : "false");
    else if (io->partial) emit(out, "  I/O incomplete: interfaces=%s tcp=%s cache=%s clock=%s\n",
        io->partial & OS64_MONITOR_IO_INTERFACES_PARTIAL ? "partial" : "ok",
        io->partial & OS64_MONITOR_IO_TCP_UNAVAILABLE ? "unavailable" : "ok",
        io->partial & OS64_MONITOR_IO_CACHE_UNAVAILABLE ? "unavailable" : "ok",
        io->partial & OS64_MONITOR_IO_CLOCK_UNAVAILABLE ? "unavailable" : "ok");
    if (io->sources & OS64_MONITOR_IO_NETWORK) {
        if (json) emit(out, ",\"network\":{\"interfaces\":[");
        else if (!io->interface_count) emit(out, "  network: no interfaces observed\n");
        for (size_t i = 0; i < io->interface_count; i++) {
            const os64_monitor_interface_t *n = &io->interfaces[i];
            if (json) {
                if (i) emit(out, ",");
                emit(out, "{\"name\":"); string(out, n->name, true);
                emit(out, ",\"mac\":");
                if (n->status.available) string(out, n->mac, true); else emit(out, "null");
                emit(out, ",\"link_up\":%s,", !n->status.available ? "null" : n->link_up ? "true" : "false");
            } else { emit(out, "  interface "); string(out, n->name, false);
                emit(out, " link=%s\n", !n->status.available ? "unavailable" : n->link_up ? "up" : "down"); }
            io_counters(out, &n->status, &n->total, &n->delta, &n->per_second, net_fields, COUNT(net_fields), json);
            if (json) emit(out, "}");
        }
        if (json) emit(out, "],\"tcp\":{"); else emit(out, "  TCP:\n");
        const os64_monitor_tcp_t *t = &io->tcp;
        io_counters(out, &t->status, &t->total, &t->delta, &t->per_second, tcp_fields, COUNT(tcp_fields), json);
        if (json) emit(out, "}}");
    }
    if (io->sources & OS64_MONITOR_IO_CACHE) {
        const os64_monitor_cache_t *c = &io->cache;
        if (json) {
            emit(out, ",\"block_cache\":{\"enabled\":%s,\"bytes\":",
                 !c->status.available ? "null" : c->enabled ? "true" : "false");
            number(out, c->status.available, c->bytes, true);
            emit(out, ",\"capacity_bytes\":"); number(out, c->status.available, c->capacity, true);
            emit(out, ",\"hit_percent\":"); number(out, c->hit_percent_valid, c->hit_percent, true);
            emit(out, ",");
        } else {
            emit(out, "  block cache: %s bytes=", !c->status.available ? "unavailable" : c->enabled ? "enabled" : "disabled");
            number(out, c->status.available, c->bytes, false);
            emit(out, " capacity_bytes="); number(out, c->status.available, c->capacity, false);
            emit(out, " hit_percent="); number(out, c->hit_percent_valid, c->hit_percent, false); emit(out, "\n");
        }
        io_counters(out, &c->status, &c->total, &c->delta, &c->per_second, cache_fields, COUNT(cache_fields), json);
        if (json) emit(out, "}");
    }
}

void sysstat_report(sysstat_output_t *out, const sysstat_options_t *opts,
                    const os64_monitor_t *m, const os64_monitor_snapshot_t *s,
                    const os64_monitor_io_snapshot_t *io, const sysstat_interval_t *time)
{
    os64_monitor_core_split_t split = {0};
    os64_monitor_machine_split(m, &split);
    if (!opts->json && time->number > 1 &&
        (opts->cores || opts->top || opts->selected || opts->network || opts->cache))
        emit(out, "\n");
    if (opts->json) {
        emit(out, "{\"schema_version\":1,\"type\":\"sample\",\"sample\":%lu,\"uptime_us\":%lu,"
                  "\"requested_interval_us\":%lu,\"elapsed_us\":%lu,\"cpu_interval_us\":%lu,"
                  "\"collection_us\":%lu,\"lateness_us\":%lu,\"skipped_intervals\":%lu,",
             time->number, time->uptime_us, (uint64_t)opts->interval_ms * 1000,
             time->elapsed_us, s->interval_us, time->collection_us, time->late_us, time->skipped);
        emit(out, "\"partial\":{\"tasks\":%s,\"threads\":%s,\"cores\":%s},\"cpu\":",
             s->partial & OS64_MONITOR_TASKS_PARTIAL ? "true" : "false",
             s->partial & OS64_MONITOR_THREADS_PARTIAL ? "true" : "false",
             s->partial & OS64_MONITOR_CORES_PARTIAL ? "true" : "false");
        json_cpu(out, s->summary_valid, s->machine_percent, &split);
        emit(out, ",\"task_count\":%lu,\"thread_count\":%lu,\"zombie_count\":%lu,\"core_count\":%lu,\"parked_cores\":",
             (uint64_t)s->task_count, thread_count(s), (uint64_t)s->zombies, (uint64_t)s->core_count);
        number(out, s->summary_valid, s->parked, true);
        emit(out, ",\"memory\":");
        if (!s->memory_valid) emit(out, "null");
        else {
            emit(out, "{\"total_bytes\":%lu,\"usable_bytes\":%lu,\"used_bytes\":%lu,\"available_bytes\":%lu,"
                      "\"largest_free_extent_bytes\":%lu,\"used_delta_bytes\":",
                 s->memory.total, s->memory.usable, s->memory.used, s->memory.available, s->memory.largest_free_extent);
            if (time->memory_before_valid) difference(out, s->memory.used, time->memory_before);
            else emit(out, "null");
            emit(out, "}");
        }
        emit(out, ",\"clock\":{\"tick_interval_us\":%lu,\"ledger_interval_us\":%lu,\"tick_skew_percent\":",
             s->tick_interval_us, s->ledger_interval_us);
        if (s->skew_valid) emit(out, "%s%lu.%lu", s->skew_negative && s->skew_tenths ? "-" : "",
                                s->skew_tenths / 10, s->skew_tenths % 10);
        else emit(out, "null");
        emit(out, "}");
    } else {
        if (time->number == 1)
            emit(out, "SAMPLE INTERVAL(ms) BUSY USER  SYS SCHED IDLE    USED   AVAIL   CHANGE LARGEST TASKS THREADS CORES PARK ZOMB   LATE(ms)\n");
        emit(out, "%6lu %8lu.%03lu ", time->number, time->elapsed_us / 1000, time->elapsed_us % 1000);
        text_cpu(out, s->summary_valid, s->machine_percent, &split);
        emit(out, " "); text_size(out, s->memory_valid, s->memory.used);
        emit(out, " "); text_size(out, s->memory_valid, s->memory.available);
        emit(out, " ");
        if (s->memory_valid && time->memory_before_valid) {
            bool less = s->memory.used < time->memory_before;
            char size[32], signed_size[40];
            os64_format_binary_size(less ? time->memory_before - s->memory.used : s->memory.used - time->memory_before,
                                    size, sizeof(size));
            os64_snprintf(signed_size, sizeof(signed_size), "%c%s", less ? '-' : '+', size);
            emit(out, "%8s", signed_size);
        } else emit(out, "%8s", "-");
        emit(out, " "); text_size(out, s->memory_valid, s->memory.largest_free_extent);
        emit(out, " %5lu %7lu %5lu ", (uint64_t)s->task_count, thread_count(s), (uint64_t)s->core_count);
        if (s->summary_valid) emit(out, "%4lu", (uint64_t)s->parked);
        else emit(out, "%4s", "-");
        emit(out, " %4lu %6lu.%03lu\n", (uint64_t)s->zombies, time->late_us / 1000, time->late_us % 1000);
        if (s->partial || !s->summary_valid || !s->memory_valid)
            emit(out, "  incomplete: tasks=%s threads=%s cores=%s cpu=%s memory=%s\n",
                 s->partial & OS64_MONITOR_TASKS_PARTIAL ? "partial" : "ok",
                 s->partial & OS64_MONITOR_THREADS_PARTIAL ? "partial" : "ok",
                 s->partial & OS64_MONITOR_CORES_PARTIAL ? "partial" : "ok",
                 s->summary_valid ? "ok" : "unavailable", s->memory_valid ? "ok" : "unavailable");
        if (time->skipped) emit(out, "  skipped_intervals=%lu\n", time->skipped);
    }
    if (opts->cores && !opts->json) text_cores(out, m, s);
    if (opts->cores && opts->json) {
        emit(out, ",\"cores\":[");
        for (size_t i = 0; i < s->core_count; i++) {
            const os64_monitor_core_t *c = &s->cores[i];
            split = (os64_monitor_core_split_t){0};
            os64_monitor_core_split(m, c->id, &split);
            if (i) emit(out, ",");
            emit(out, "{\"core_id\":%u,\"parked\":%s,\"cpu\":", c->id,
                 !c->valid ? "null" : c->parked ? "true" : "false");
            json_cpu(out, c->valid && !c->parked, c->percent, &split); emit(out, "}");
        }
        emit(out, "]");
    }
    if (opts->top) {
        const os64_monitor_task_t *ranked[SYSSTAT_TASKS];
        size_t count = busiest(m, s, ranked, opts->top);
        if (opts->json) emit(out, ",\"top_tasks\":[");
        for (size_t i = 0; i < count; i++) {
            if (opts->json) {
                if (i) emit(out, ",");
                json_task(out, m, s, ranked[i], false);
            } else text_task(out, m, s, ranked[i], false);
        }
        if (opts->json) emit(out, "]");
    }
    if (opts->selected) {
        const os64_monitor_task_t *task = find_task(s, opts->task_id);
        if (opts->json) {
            emit(out, ",\"selected_task_id\":%lu,\"selected_task\":", opts->task_id);
            json_task(out, m, s, task, true);
        } else {
            emit(out, "  selected TID %lu:\n", opts->task_id);
            text_task(out, m, s, task, true);
        }
    }
    io_report(out, io, opts->json);
    if (opts->json) emit(out, "}\n");
    flush(out);
}

void sysstat_finish(sysstat_output_t *out, const sysstat_options_t *opts,
                    const sysstat_summary_t *s, bool complete)
{
    bool average = s->average_valid && s->cpu_weight_us;
    uint64_t mean = average ? s->busy_weighted / s->cpu_weight_us : 0;
    if (opts->json) {
        emit(out, "{\"schema_version\":1,\"type\":\"summary\",\"complete\":%s,\"samples\":%lu,"
                  "\"cpu_samples\":%lu,\"memory_samples\":%lu,\"partial_samples\":%lu,\"average_busy_percent\":",
             complete ? "true" : "false", s->samples, s->cpu_samples, s->memory_samples, s->partial_samples);
        number(out, average, mean, true);
        emit(out, ",\"peak_busy_percent\":"); number(out, s->cpu_samples != 0, s->peak_busy, true);
        emit(out, ",\"min_available_bytes\":"); number(out, s->memory_samples != 0, s->min_available, true);
        emit(out, ",\"max_interval_us\":%lu,\"max_lateness_us\":%lu,\"skipped_intervals\":%lu}\n",
             s->max_interval_us, s->max_late_us, s->skipped);
    } else {
        emit(out, "Summary: %lu/%u samples, average_busy=", s->samples, opts->count);
        number(out, average, mean, false);
        emit(out, "%% peak_busy="); number(out, s->cpu_samples != 0, s->peak_busy, false);
        emit(out, "%% min_available_bytes="); number(out, s->memory_samples != 0, s->min_available, false);
        emit(out, " max_interval_ms=%lu.%03lu max_lateness_ms=%lu.%03lu skipped=%lu partial=%lu%s\n",
             s->max_interval_us / 1000, s->max_interval_us % 1000,
             s->max_late_us / 1000, s->max_late_us % 1000,
             s->skipped, s->partial_samples, complete ? "" : " incomplete");
    }
    flush(out);
}
