#include "os64/monitor_io.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"

typedef struct { const char *key; size_t offset; } field_t;
#define FIELD(type, member) { #member ":", offsetof(type, member) }
static const field_t net_fields[] = {
    FIELD(os64_monitor_net_values_t, rx_bytes), FIELD(os64_monitor_net_values_t, tx_bytes),
    FIELD(os64_monitor_net_values_t, rx_frames), FIELD(os64_monitor_net_values_t, tx_frames),
    FIELD(os64_monitor_net_values_t, rx_errors), FIELD(os64_monitor_net_values_t, tx_errors),
    FIELD(os64_monitor_net_values_t, rx_dropped_no_handler), FIELD(os64_monitor_net_values_t, rx_dropped_too_big)
};
static const field_t tcp_fields[] = {
    FIELD(os64_monitor_tcp_values_t, segments_in), FIELD(os64_monitor_tcp_values_t, segments_out),
    FIELD(os64_monitor_tcp_values_t, retransmits), FIELD(os64_monitor_tcp_values_t, fast_retransmits),
    FIELD(os64_monitor_tcp_values_t, tx_local_drops), FIELD(os64_monitor_tcp_values_t, bad_checksum),
    FIELD(os64_monitor_tcp_values_t, connect_timeouts), FIELD(os64_monitor_tcp_values_t, resets_received)
};
static const field_t cache_fields[] = {
    FIELD(os64_monitor_cache_values_t, hits), FIELD(os64_monitor_cache_values_t, misses),
    FIELD(os64_monitor_cache_values_t, fills), FIELD(os64_monitor_cache_values_t, evictions),
    FIELD(os64_monitor_cache_values_t, updates), FIELD(os64_monitor_cache_values_t, bypass_edge),
    FIELD(os64_monitor_cache_values_t, discarded_races), FIELD(os64_monitor_cache_t, bytes),
    FIELD(os64_monitor_cache_t, capacity)
};
#undef FIELD
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    os64_monitor_io_snapshot_t snapshot;
    os64_monitor_interface_t *interfaces;
} io_buffer_t;

struct os64_monitor_io {
    io_buffer_t buffers[2];
    size_t capacity;
    uint32_t sources;
    unsigned current;
    bool sampled;
};

os64_monitor_io_t *os64_monitor_io_create(uint32_t sources, size_t capacity)
{
    if (!sources || (sources & ~(OS64_MONITOR_IO_NETWORK | OS64_MONITOR_IO_CACHE)) ||
        ((sources & OS64_MONITOR_IO_NETWORK) && !capacity) ||
        capacity > SIZE_MAX / sizeof(os64_monitor_interface_t)) return NULL;
    os64_monitor_io_t *m = os64_malloc(sizeof(*m));
    if (!m) return NULL;
    os64_memset(m, 0, sizeof(*m));
    m->capacity = capacity; m->sources = sources;
    if (sources & OS64_MONITOR_IO_NETWORK) {
        for (unsigned i = 0; i < 2; i++) {
            m->buffers[i].interfaces = os64_malloc(capacity * sizeof(os64_monitor_interface_t));
            if (!m->buffers[i].interfaces) { os64_monitor_io_destroy(m); return NULL; }
        }
    }
    return m;
}

void os64_monitor_io_destroy(os64_monitor_io_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < 2; i++) os64_free(m->buffers[i].interfaces);
    os64_free(m);
}

// floor(value * scale / divisor), without an overflowing intermediate product.
static bool scale_value(uint64_t value, uint32_t scale, uint64_t divisor, uint64_t *out)
{
    if (!divisor) return false;
    uint64_t whole = value / divisor, remainder = value % divisor;
    if (whole > UINT64_MAX / scale) return false;
    uint64_t fraction = 0, rem = 0;
    for (uint32_t bit = 1u << 31; bit; bit >>= 1) {
        fraction *= 2;
        if (rem >= divisor - rem) { rem -= divisor - rem; fraction++; }
        else rem += rem;
        if (scale & bit) {
            if (rem >= divisor - remainder) { rem -= divisor - remainder; fraction++; }
            else rem += remainder;
        }
    }
    uint64_t result = whole * scale;
    if (result > UINT64_MAX - fraction) return false;
    *out = result + fraction;
    return true;
}

static uint64_t *field_value(void *values, const field_t *field)
{ return (uint64_t *)((unsigned char *)values + field->offset); }
static uint64_t read_value(const void *values, const field_t *field)
{ return *(const uint64_t *)((const unsigned char *)values + field->offset); }

static void calculate(os64_monitor_io_status_t *now, const os64_monitor_io_status_t *before,
                       void *delta, void *rate, const void *totals, const void *old,
                       const field_t *fields, size_t count)
{
    if (!before || !now->available || !before->available) return;
    for (size_t i = 0; i < count; i++)
        if (read_value(totals, &fields[i]) < read_value(old, &fields[i])) return;
    now->delta_valid = true;
    now->rate_valid = now->clock_valid && before->clock_valid &&
                      now->sampled_at_us > before->sampled_at_us;
    if (now->rate_valid) now->interval_us = now->sampled_at_us - before->sampled_at_us;
    for (size_t i = 0; i < count; i++) {
        uint64_t d = read_value(totals, &fields[i]) - read_value(old, &fields[i]);
        *field_value(delta, &fields[i]) = d;
        if (now->rate_valid && !scale_value(d, 1000000, now->interval_us, field_value(rate, &fields[i])))
            now->rate_valid = false;
    }
}

typedef enum { REPORT_NET, REPORT_TCP, REPORT_CACHE } report_kind_t;
typedef struct { const char *name; char mac[18]; bool flag; unsigned seen; } metadata_t;

static bool mac_address(char out[18], const char *text)
{
    if (os64_strlen(text) != 17) return false;
    for (unsigned i = 0; i < 17; i++) {
        char c = text[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        out[i] = c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c;
    }
    out[17] = 0; return true;
}

static bool metadata(report_kind_t kind, metadata_t *meta, const char *key, const char *value)
{
    unsigned bit = 0;
    bool good = true;
    if (kind == REPORT_NET) {
        if (os64_streq(key, "name:")) { bit = 1; good = os64_streq(value, meta->name); }
        else if (os64_streq(key, "mac:")) { bit = 2; good = mac_address(meta->mac, value); }
        else if (os64_streq(key, "link:")) {
            bit = 4;
            if (os64_streq(value, "down")) meta->flag = false;
            else if (os64_streq(value, "up") || (os64_strlen(value) >= 4 && !os64_memcmp(value, "up, ", 4))) meta->flag = true;
            else good = false;
        }
    } else if (kind == REPORT_CACHE && os64_streq(key, "state:")) {
        bit = 1;
        if (os64_streq(value, "enabled")) meta->flag = true;
        else if (os64_streq(value, "disabled")) meta->flag = false;
        else good = false;
    }
    if (meta->seen & bit) good = false;
    meta->seen |= bit;
    return good;
}

static bool read_report(const char *path, report_kind_t kind, void *values,
                         const field_t *fields, size_t count, metadata_t *meta)
{
    int64_t h = os64_open(path, NULL);
    if (h < 0) return false;
    char line[512];
    uint32_t seen = 0;
    unsigned lines = 0;
    int64_t rc;
    bool good = true;
    while ((rc = os64_readline((int32_t)h, line, sizeof(line))) == 1) {
        if (++lines > 256 || os64_strlen(line) == sizeof(line) - 1) { good = false; break; }
        char *value = line;
        while (*value && *value != ' ' && *value != '\t') value++;
        if (!*value) continue;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') value++;
        // TCP's counters precede its listener/connection tables. Those rows
        // are not counter fields and do not need to be scanned for rates.
        if (kind == REPORT_TCP && (os64_streq(line, "listeners:") ||
            os64_streq(line, "connections:") || os64_streq(line, "#"))) break;
        if (!metadata(kind, meta, line, value)) good = false;
        for (size_t i = 0; i < count; i++) if (os64_streq(line, fields[i].key)) {
            if ((seen & (1u << i)) || !os64_parse_u64(value, field_value(values, &fields[i]))) good = false;
            seen |= 1u << i;
        }
    }
    if (os64_close((int32_t)h) < 0 || rc < 0) good = false;
    unsigned wanted = kind == REPORT_NET ? 7 : kind == REPORT_CACHE ? 1 : 0;
    return good && seen == (1u << count) - 1 && meta->seen == wanted;
}

static void timestamp(os64_monitor_io_snapshot_t *snapshot, os64_monitor_io_status_t *status)
{
    int64_t now = os64_micros();
    status->clock_valid = now >= 0;
    if (status->clock_valid) status->sampled_at_us = (uint64_t)now;
    else snapshot->partial |= OS64_MONITOR_IO_CLOCK_UNAVAILABLE;
}

static bool interface_name(const char *name)
{
    if (!*name || os64_strlen(name) >= sizeof(((os64_monitor_interface_t *)0)->name) ||
        os64_streq(name, ".") || os64_streq(name, "..")) return false;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}

static void network(os64_monitor_io_t *m, io_buffer_t *b, const os64_monitor_io_snapshot_t *old)
{
    os64_monitor_io_snapshot_t *s = &b->snapshot;
    int64_t h = os64_opendir("/sys/net");
    if (h < 0) { s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL; return; }
    os64_dirent_t entry;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)h, &entry)) == 1) {
        if ((entry.flags & OS64_DE_DIR) || os64_streq(entry.name, "ip") || os64_streq(entry.name, "dhcp") ||
            os64_streq(entry.name, "tcp") || os64_streq(entry.name, "knet")) continue;
        if (!interface_name(entry.name)) { s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL; continue; }
        bool duplicate = false;
        for (size_t i = 0; i < s->interface_count; i++)
            if (os64_streq(b->interfaces[i].name, entry.name)) duplicate = true;
        if (duplicate) { s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL; continue; }
        if (s->interface_count == m->capacity) { s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL; break; }
        os64_monitor_interface_t *n = &b->interfaces[s->interface_count++];
        os64_memset(n, 0, sizeof(*n));
        os64_strcopy(n->name, sizeof(n->name), entry.name);
        char path[80]; os64_snprintf(path, sizeof(path), "/sys/net/%s", n->name);
        metadata_t meta = {.name = n->name};
        n->status.available = read_report(path, REPORT_NET, &n->total, net_fields, COUNT(net_fields), &meta);
        if (!n->status.available) { s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL; continue; }
        os64_strcopy(n->mac, sizeof(n->mac), meta.mac); n->link_up = meta.flag;
        timestamp(s, &n->status);
        if (old) for (size_t i = 0; i < old->interface_count; i++) {
            const os64_monitor_interface_t *p = &old->interfaces[i];
            if (!os64_streq(n->name, p->name) || !os64_streq(n->mac, p->mac)) continue;
            calculate(&n->status, &p->status, &n->delta, &n->per_second, &n->total, &p->total, net_fields, COUNT(net_fields));
            break;
        }
    }
    if (os64_close((int32_t)h) < 0 || rc < 0) s->partial |= OS64_MONITOR_IO_INTERFACES_PARTIAL;
}

int32_t os64_monitor_io_sample(os64_monitor_io_t *m, const os64_monitor_io_snapshot_t **out)
{
    if (!out) return -1;
    *out = NULL;
    if (!m) return -1;
    io_buffer_t *b = &m->buffers[m->current ^ 1u];
    os64_monitor_io_snapshot_t *s = &b->snapshot;
    const os64_monitor_io_snapshot_t *old = m->sampled ? &m->buffers[m->current].snapshot : NULL;
    os64_memset(s, 0, sizeof(*s)); s->interfaces = b->interfaces; s->sources = m->sources;
    if (m->sources & OS64_MONITOR_IO_NETWORK) {
        network(m, b, old);
        os64_monitor_tcp_t *t = &s->tcp;
        metadata_t meta = {0};
        t->status.available = read_report("/sys/net/tcp", REPORT_TCP, &t->total, tcp_fields, COUNT(tcp_fields), &meta);
        if (!t->status.available) s->partial |= OS64_MONITOR_IO_TCP_UNAVAILABLE;
        else {
            timestamp(s, &t->status);
            if (old) calculate(&t->status, &old->tcp.status, &t->delta, &t->per_second,
                               &t->total, &old->tcp.total, tcp_fields, COUNT(tcp_fields));
        }
    }
    if (m->sources & OS64_MONITOR_IO_CACHE) {
        os64_monitor_cache_t *c = &s->cache;
        metadata_t meta = {0};
        // Cache gauges live outside the counter tuple, so parse through a
        // temporary field table with offsets relative to the complete record.
        field_t fields[COUNT(cache_fields)];
        for (size_t i = 0; i < COUNT(cache_fields); i++) {
            fields[i] = cache_fields[i];
            if (i < COUNT(cache_fields) - 2) fields[i].offset += offsetof(os64_monitor_cache_t, total);
        }
        c->status.available = read_report("/sys/cache", REPORT_CACHE, c, fields, COUNT(fields), &meta);
        if (!c->status.available) s->partial |= OS64_MONITOR_IO_CACHE_UNAVAILABLE;
        else {
            c->enabled = meta.flag;
            timestamp(s, &c->status);
            if (old && c->enabled == old->cache.enabled)
                calculate(&c->status, &old->cache.status, &c->delta, &c->per_second,
                          &c->total, &old->cache.total, cache_fields, COUNT(cache_fields) - 2);
            if (c->status.delta_valid && c->delta.hits <= UINT64_MAX - c->delta.misses) {
                uint64_t percent;
                c->hit_percent_valid = scale_value(c->delta.hits, 100, c->delta.hits + c->delta.misses, &percent);
                if (c->hit_percent_valid) c->hit_percent = (uint32_t)percent;
            }
        }
    }
    m->current ^= 1u; m->sampled = true; *out = s;
    return 0;
}
