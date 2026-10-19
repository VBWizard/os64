// Real sysfs parser and rate calculations with controlled source contents.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libos64/monitor_io.c"

static char nic[2][2048], tcp[2048], cache[2048];
static const char *names[] = {"eth0", "eth1"};
static const char *cursor[8];
static bool handles[8], reverse_order, missing_nic, missing_tcp, missing_cache;
static bool bad_dir, bad_read, bad_close, duplicate_entry, unsafe_entry;
static unsigned entries, dir_index, allocations, fail_alloc, open_calls;
static int64_t clock_us;

void *os64_malloc(size_t n)
{
    if (fail_alloc && --fail_alloc == 0) return NULL;
    void *p = malloc(n); if (p) allocations++; return p;
}
void os64_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
int64_t os64_write(int32_t h, const void *p, size_t n)
{ (void)h; (void)p; return (int64_t)n; }
int64_t os64_micros(void) { return clock_us; }
int64_t os64_opendir(const char *path)
{
    assert(!strcmp(path, "/sys/net")); open_calls++;
    if (bad_dir) return -1;
    assert(!handles[1]); handles[1] = true; dir_index = 0; return 1;
}
int64_t os64_readdir(int32_t h, os64_dirent_t *e)
{
    assert(h == 1 && handles[h]);
    if (dir_index == entries) return 0;
    memset(e, 0, sizeof(*e));
    unsigned index = reverse_order ? entries - dir_index - 1 : dir_index;
    strcpy(e->name, unsafe_entry ? "../bad" : names[duplicate_entry ? 0 : index]);
    dir_index++; return 1;
}
int64_t os64_open(const char *path, const char *mode)
{
    assert(!mode); open_calls++;
    int h;
    if (!strcmp(path, "/sys/net/eth0")) { if (missing_nic) return -1; h = 2; cursor[h] = nic[0]; }
    else if (!strcmp(path, "/sys/net/eth1")) { h = 3; cursor[h] = nic[1]; }
    else if (!strcmp(path, "/sys/net/tcp")) { if (missing_tcp) return -1; h = 4; cursor[h] = tcp; }
    else { assert(!strcmp(path, "/sys/cache")); if (missing_cache) return -1; h = 5; cursor[h] = cache; }
    assert(!handles[h]); handles[h] = true; return h;
}
int64_t os64_readline(int32_t h, char *buf, size_t cap)
{
    assert(h >= 2 && h <= 5 && handles[h]);
    if (bad_read) return -1;
    const char *p = cursor[h];
    if (!*p) return 0;
    size_t n = strcspn(p, "\n"), take = n < cap ? n : cap - 1;
    memcpy(buf, p, take); buf[take] = 0; cursor[h] = p + n + (p[n] == '\n'); return 1;
}
int64_t os64_close(int32_t h)
{ assert(h > 0 && h < 8 && handles[h]); handles[h] = false; return bad_close ? -1 : 0; }

static void fixtures(uint64_t v)
{
    for (unsigned i = 0; i < 2; i++) snprintf(nic[i], sizeof(nic[i]),
        "name: eth%u\nmodel: test card\nmac: 00:11:22:33:44:0%u\nlink: up, 1000/full\n"
        "rx_bytes: %lu\ntx_bytes: %lu\nrx_frames: %lu\ntx_frames: 0\n"
        "rx_errors: 0\ntx_errors: 0\nrx_dropped_no_handler: 0\nrx_dropped_too_big: 0\n", i, i, v * (i+1), v, v / 10);
    snprintf(tcp, sizeof(tcp), "segments_in: %lu\nsegments_out: %lu\nretransmits: %lu\n"
        "fast_retransmits: 0\ntx_local_drops: 0\nbad_checksum: 0\nconnect_timeouts: 0\n"
        "resets_received: 0\nlisteners: 500\nsegments_in: garbage ignored in table\n", v, v, v/10);
    snprintf(cache, sizeof(cache), "state: enabled\ndevices: none (nothing cacheable found)\n"
        "bytes: 4096\ncapacity: 1048576\nhits: %lu\nmisses: %lu\nhit_pct: 999\n"
        "fills: %lu\nevictions: 0\nupdates: 0\nbypass_edge: 0\ndiscarded_races: 0\n", v, v/4, v/4);
}
static const os64_monitor_io_snapshot_t *sample(os64_monitor_io_t *m)
{
    const os64_monitor_io_snapshot_t *s = NULL;
    assert(!os64_monitor_io_sample(m, &s) && s);
    for (unsigned i = 0; i < 8; i++) assert(!handles[i]);
    return s;
}
static void lifecycle(void)
{
    assert(!os64_monitor_io_create(0, 1)); assert(!os64_monitor_io_create(8, 1));
    assert(!os64_monitor_io_create(OS64_MONITOR_IO_NETWORK, 0));
    assert(!os64_monitor_io_create(OS64_MONITOR_IO_NETWORK, SIZE_MAX));
    for (unsigned i = 1; i <= 3; i++) { fail_alloc = i; assert(!os64_monitor_io_create(3, 2)); assert(!allocations); }
    fail_alloc = 0;
    const os64_monitor_io_snapshot_t *s = (void *)1;
    assert(os64_monitor_io_sample(NULL, &s) < 0 && !s);
    os64_monitor_io_destroy(NULL);
}
static void rates(void)
{
    entries = 2; clock_us = 1000000; fixtures(100);
    os64_monitor_io_t *a = os64_monitor_io_create(3, 2), *b = os64_monitor_io_create(3, 2);
    assert(a && b); const os64_monitor_io_snapshot_t *s = sample(a);
    assert(!s->partial && s->interface_count == 2 && !s->interfaces[0].status.delta_valid);
    assert(s->cache.status.available && !s->cache.hit_percent_valid);
    sample(b);
    fixtures(300); clock_us += 2000000; reverse_order = true; s = sample(a);
    assert(!s->partial && !strcmp(s->interfaces[0].name, "eth1"));
    assert(s->interfaces[0].delta.rx_bytes == 400 && s->interfaces[0].per_second.rx_bytes == 200);
    assert(s->interfaces[1].per_second.rx_bytes == 100 && s->interfaces[1].status.interval_us == 2000000);
    assert(s->tcp.per_second.retransmits == 10 && s->cache.hit_percent == 80 && s->cache.hit_percent_valid);
    assert(s->cache.delta.fills == 50 && s->cache.per_second.fills == 25);
    fixtures(400); clock_us += 1000000; s = sample(b);
    assert(s->interfaces[1].per_second.rx_bytes == 100 && s->interfaces[1].delta.rx_bytes == 300);
    s = sample(a); assert(s->cache.hit_percent == 80);
    clock_us += 1000000; s = sample(a);
    assert(s->interfaces[0].status.rate_valid && !s->interfaces[0].per_second.rx_bytes);
    assert(!s->cache.hit_percent_valid && s->cache.status.rate_valid);
    fixtures(0); clock_us++; s = sample(a);
    assert(!s->interfaces[0].status.delta_valid && !s->tcp.status.delta_valid && !s->cache.status.delta_valid);
    fixtures(100); clock_us += 1000000; s = sample(a); assert(s->tcp.status.rate_valid);
    // A different MAC under the same name starts a new baseline.
    char *mac = strstr(nic[0], "00:11:22:33:44:00"); assert(mac); mac[0] = '1';
    clock_us++; s = sample(a); assert(!s->interfaces[1].status.delta_valid && s->interfaces[0].status.rate_valid);
    // Failed reads and disappearing devices break consecutive-observation history.
    missing_nic = missing_tcp = missing_cache = true; clock_us++; s = sample(a);
    assert(s->partial == 7 && !s->interfaces[1].status.available);
    missing_nic = missing_tcp = missing_cache = false; clock_us++; s = sample(a);
    assert(!s->interfaces[1].status.delta_valid && !s->tcp.status.delta_valid && !s->cache.status.delta_valid);
    entries = 0; sample(a); entries = 2; s = sample(a); assert(!s->interfaces[0].status.delta_valid);
    bad_dir = true; s = sample(a); assert(s->partial == 1 && !s->interface_count); bad_dir = false;
    s = sample(a); assert(!s->interfaces[0].status.delta_valid);
    clock_us = -1; s = sample(a); assert(s->partial == 8 && s->cache.status.delta_valid && !s->cache.status.rate_valid);
    clock_us = 5000000; s = sample(a); assert(!s->cache.status.rate_valid);
    clock_us++; s = sample(a); assert(s->cache.status.rate_valid);
    s = sample(a); assert(!s->cache.status.rate_valid && s->cache.status.delta_valid);
    clock_us--; s = sample(a); assert(!s->cache.status.rate_valid);
    strcpy(cache, "state: disabled\n"); // A missing required field is unavailable, not zero.
    s = sample(a); assert(s->partial == 4 && !s->cache.status.available);
    os64_monitor_io_destroy(a); os64_monitor_io_destroy(b); assert(!allocations);
    reverse_order = false;
}
static void failures(void)
{
    os64_monitor_io_t *m = os64_monitor_io_create(3, 1); assert(m);
    fixtures(100); entries = 2; clock_us = 1000;
    assert(sample(m)->partial == 1); // Bounded census.
    duplicate_entry = true; assert(sample(m)->partial == 1); duplicate_entry = false;
    unsafe_entry = true; assert(sample(m)->interface_count == 0); unsafe_entry = false;
    entries = 1; sample(m);
    strcat(nic[0], "rx_bytes: 1\n"); assert(!sample(m)->interfaces[0].status.available);
    fixtures(100); strcat(cache, "hits: 1\n"); assert(!sample(m)->cache.status.available);
    fixtures(100); strcat(cache, "state: disabled\n"); assert(!sample(m)->cache.status.available);
    fixtures(100); strcpy(strstr(nic[0], "rx_bytes:"), "rx_bytes: 18446744073709551616\n");
    assert(!sample(m)->interfaces[0].status.available);
    fixtures(100); memset(tcp, 'x', 600); tcp[600] = '\n'; tcp[601] = 0;
    assert(!sample(m)->tcp.status.available);
    fixtures(100); bad_read = true; assert(sample(m)->partial == 7); bad_read = false;
    bad_close = true; assert(sample(m)->partial == 7); bad_close = false;
    fixtures(100); sample(m);
    char *state = strstr(cache, "enabled"); memcpy(state, "disable", 7); // Invalid state.
    assert(!sample(m)->cache.status.available);
    fixtures(100); sample(m);
    memmove(cache + 15, cache + 14, strlen(cache + 14) + 1); memcpy(cache, "state: disabled", 15);
    const os64_monitor_io_snapshot_t *s = sample(m);
    assert(s->cache.status.available && !s->cache.status.delta_valid);
    os64_monitor_io_destroy(m);
    m = os64_monitor_io_create(OS64_MONITOR_IO_CACHE, 0); assert(m); open_calls = 0;
    sample(m); assert(open_calls == 1); os64_monitor_io_destroy(m); assert(!allocations);
}
static void arithmetic(void)
{
    // The host's wider integer supplies an independent oracle for freestanding arithmetic.
    uint64_t seed = 0x192837465abcdULL;
    for (unsigned i = 0; i < 20000; i++) {
        seed = seed * 6364136223846793005ULL + 1; uint64_t value = seed;
        seed = seed * 6364136223846793005ULL + 1; uint64_t divisor = i % 3 ? seed : i;
        uint32_t scale = i & 1 ? 1000000 : 100; uint64_t result = 0;
        __uint128_t expected = divisor ? (__uint128_t)value * scale / divisor : 0;
        bool good = scale_value(value, scale, divisor, &result);
        assert(good == (divisor && expected <= UINT64_MAX));
        if (good) assert(result == expected);
    }
    uint64_t n; assert(!scale_value(UINT64_MAX, 1000000, 1, &n));
    assert(scale_value(UINT64_MAX, 100, UINT64_MAX, &n) && n == 100);
}
int main(void)
{
    lifecycle(); rates(); failures(); arithmetic();
    puts("monitor I/O: parsing, rates, identities, gaps, resets, failures and overflow PASS");
}
