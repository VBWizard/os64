// Run the actual os64get batch/URL control flow against host filesystem and
// transport adapters. Faults are injected at I/O boundaries, not in the app.
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

// The host must never execute the guest syscall instruction. Inline ABI
// wrappers still compile, but their raw entry points are replaced here.
#define OS64_ABI_SYSCALL_H
#include "os64/syscall_numbers.h"
uint64_t os64_syscall0(uint64_t n);
uint64_t os64_syscall1(uint64_t n, uint64_t a);
uint64_t os64_syscall2(uint64_t n, uint64_t a, uint64_t b);
uint64_t os64_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c);
uint64_t os64_syscall4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t os64_syscall6(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
#define main os64get_entry
#include "../userland/apps/os64get/os64get.c"
#include "fetch/transport.h"
#undef main
#include "os64/slurp.h"

static char sandbox[512];
static char fdpaths[1024][256];
static const char *scenario;
static unsigned installs, backup_reads;
static bool injected, run_active;
static unsigned blocked_scratch_attempts;
static char output_text[16384];
static size_t output_used;
static os64_signal_fn handler;
static const char *names[] = { "a", "b", "c" };
static const char *payloads[] = { "incoming A", "incoming B", "incoming C" };
static const char *targets[] = { "/bin/a", "/home/b", "/fat/c" };
static char network[16][1024];
static size_t network_len[16], network_pos[16];
static unsigned connections, tls_created, tls_freed, trust_loads;
static bool network_closed[16];

static bool is(const char *name) { return strcmp(scenario, name) == 0; }
static void interrupt_run(void) { assert(handler); handler(OS64_SIGINT); injected = true; }

static void host_path(const char *path, char out[1024])
{
    assert(path[0] == '/' && strstr(path, "/../") == NULL);
    assert(snprintf(out, 1024, "%s%s", sandbox, path) < 1024);
    // Model FAT lookup through the filesystem adapter, including case-folded
    // directory components. Ext2 paths retain case-sensitive host lookup.
    if (strncmp(path, "/fat/", 5) != 0) return;
    size_t begin = strlen(sandbox) + 5;
    for (size_t end = begin; ; end++) {
        if (out[end] && out[end] != '/') continue;
        char saved = out[end]; out[end] = 0;
        char parent[1024]; memcpy(parent, out, begin); parent[begin ? begin-1 : 0] = 0;
        DIR *dir = opendir(parent);
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir))) {
                if (strcasecmp(e->d_name, out + begin) == 0) {
                    assert(strlen(e->d_name) == end - begin);
                    memcpy(out + begin, e->d_name, end - begin);
                    break;
                }
            }
            closedir(dir);
        }
        out[end] = saved;
        if (!saved) break;
        begin = end + 1;
    }
}

uint64_t os64_syscall2(uint64_t n, uint64_t a, uint64_t b)
{
    assert(n == SYSCALL_SIGNAL_HANDLER && a == OS64_SIGINT);
    handler = (os64_signal_fn)b; return 0;
}
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
uint64_t os64_taskid(void) { return 42; }
int64_t os64_getcwd(char *out, size_t cap) { return snprintf(out, cap, "/bin"); }
const char *os64_getenv(const char *key)
{
    if ((is("url-https") || is("url-tls-bypass")) && !strcmp(key, "https_proxy")) return "http://proxy:8888/";
    if (is("url-tls-bypass") && !strcmp(key, "no_proxy")) return "host";
    return NULL;
}
int64_t os64_ticks(os64_ticks_t *out)
{ static uint64_t ticks; out->ticks = ticks++; out->per_second = 1000; return 0; }
int64_t os64_write_for(int32_t h, const void *p, size_t n, uint64_t ms)
{ assert(ms > 0 && ms <= FETCH_IDLE_MS_DEFAULT); return os64_write(h, p, n); }
int64_t __wrap_os64_time(os64_time_t *out) { memset(out, 0, sizeof(*out)); out->epoch = 1788739200; return 0; }
bool os64_parse_ipv4(const char *s, const char *end, uint32_t *ip)
{ (void)s; (void)end; *ip = 0x7f000001; return true; }
const char *os64_dial_reason(int64_t error) { (void)error; return "fixture refusal"; }
int64_t os64_conf_find(const char *name, char *out, size_t cap)
{ (void)name; snprintf(out, cap, "/etc/os64get.conf"); return 0; }
int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *ctx)
{
    (void)path;
    fn("a", "/bin", ctx); fn("b", "/home", ctx); fn("c", "/fat", ctx);
    fn("archive", "/home/archive", ctx);
    if (!strncmp(scenario, "enclose-", 8)) {
        fn("archive", is("enclose-fat-alias") ? "/fat/NEW/archive" :
                      is("enclose-sibling") ? "/home/news/archive" : "/home/new/archive", ctx);
        fn("new", is("enclose-fat-alias") ? "/fat" : "/home", ctx);
    }
    if (is("review-duplicate-unchanged") || is("review-duplicate-new")) fn("C", "/fat", ctx);
    return 0;
}
os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **out, size_t *len)
{
    assert(strcmp(path, "/sys/mounts") == 0);
    const char *text = "# prefix fstype device part name guid mode blocksz total free open_files open_dirs\n"
        "/ ext2 d 1 root g rw 4096 10000000 9000000 0 0\n"
        "/home ext2 d 2 home g rw 4096 10000000 9000000 0 0\n"
        "/fat fat d 3 fat g rw 512 10000000 9000000 0 0\n";
    *len = strlen(text); assert(*len < cap); *out = (uint8_t *)strdup(text);
    if (is("review-ro") || is("review-empty-ro") || is("review-force-ro")) {
        char *fat = strstr((char *)*out, "/fat fat"); assert(fat);
        char *mode = strstr(fat, " rw "); assert(mode); mode[2] = 'o';
    }
    return OS64_SLURP_OK;
}
int64_t os64_open(const char *path, const char *mode)
{
    char full[1024]; host_path(path, full);
    int flags = strcmp(mode, "r") == 0 ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    if (strcmp(mode, "x") == 0) flags = O_WRONLY | O_CREAT | O_EXCL;
    int h = open(full, flags, 0600);
    if (h >= 0) { assert(h < 1024); snprintf(fdpaths[h], 256, "%s", path); }
    return h;
}
int64_t os64_close(int32_t h)
{
    if (h >= 10000) { assert(!network_closed[h-10000]); network_closed[h-10000] = true; return 0; }
    bool fail = is("close") && !injected && strstr(fdpaths[h], "backup-");
    int rc = close(h); fdpaths[h][0] = 0;
    if (fail) { injected = true; return -1; }
    return rc;
}
int64_t os64_stat(const char *path, os64_dirent_t *out)
{
    char full[1024]; host_path(path, full); struct stat st;
    if (stat(full, &st) < 0) return -1;
    memset(out, 0, sizeof(*out)); out->size = st.st_size; out->mtime = st.st_mtime;
    if (S_ISDIR(st.st_mode)) out->flags |= OS64_DE_DIR;
    if (!strcmp(path, "/") || !strcmp(path, "/home") || !strcmp(path, "/fat")) out->flags |= OS64_DE_MOUNT;
    snprintf(out->name, sizeof(out->name), "%s", strrchr(full, '/') + 1);
    return 0;
}
int64_t os64_mkdir(const char *path)
{
    bool fat = !strncmp(path, "/fat/.os64get-tmp", 17);
    bool root = !strncmp(path, "/tmp/os64get", 12);
    if (run_active && ((fat && (is("review-ro") || is("review-empty-ro") || is("review-full"))) ||
                       (root && is("review-single-full")) ||
                       ((fat || root || strstr(path, ".os64get-tmp")) && is("review-all-full")))) {
        blocked_scratch_attempts++; return -1;
    }
    char full[1024]; host_path(path, full); return mkdir(full, 0700);
}
int64_t os64_unlink(const char *path)
{
    if (is("cancel-cleanup")) interrupt_run();
    char full[1024]; host_path(path, full);
    struct stat st; if (stat(full, &st) < 0) return -1;
    return S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
}
int64_t os64_sync(int32_t h)
{
    if (is("sync") && !injected && strstr(fdpaths[h], "backup-")) { injected = true; return -1; }
    return fsync(h);
}
int64_t os64_rename_with_flags(const char *from, const char *to, uint64_t flags)
{
    bool publish = !strcmp(to, targets[0]) || !strcmp(to, targets[1]) || !strcmp(to, targets[2]);
    if (publish && is("cancel-commit") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    if (publish && is("publish") && !strcmp(to, targets[1])) return -1;
    if (!publish && strstr(to, "/home/archive/") &&
        (is("review-empty-backup") || (is("review-partial-backup") && installs == 0 && strstr(to, "/home/b")))) return -1;
    if (publish) installs++;
    // Assert that application publication does not attempt a cross-mount move.
    int a = !strncmp(from, "/home/", 6) ? 1 : !strncmp(from, "/fat/", 5) ? 2 : 0;
    int b = !strncmp(to, "/home/", 6) ? 1 : !strncmp(to, "/fat/", 5) ? 2 : 0;
    assert(a == b);
    char f[1024], t[1024]; host_path(from, f); host_path(to, t);
    if ((flags & OS64_RENAME_NOREPLACE) && access(t, F_OK) == 0) return -1;
    return rename(f, t);
}
int64_t os64_read(int32_t h, void *buf, size_t cap)
{
    if (h >= 10000) {
        unsigned i = (unsigned)(h - 10000);
        if (is("url-cancel") && !injected && network_pos[i] >= 39) { interrupt_run(); return OS64_INTERRUPTED; }
        if (!injected && ((is("cancel-list") && i == 0) || (is("cancel-download") && i == 2 && network_pos[i] > 18))) {
            interrupt_run(); return OS64_INTERRUPTED;
        }
        size_t n = network_len[i] - network_pos[i]; if (n > cap) n = cap;
        if (n > 3) n = 3;
        memcpy(buf, network[i] + network_pos[i], n); network_pos[i] += n; return (int64_t)n;
    }
    if (run_active && is("integrity-read") && strstr(fdpaths[h], "/run-") &&
        !strstr(fdpaths[h], "backup-")) return -1;
    if (run_active && is("integrity-cancel") && !injected &&
        strstr(fdpaths[h], "/run-") && !strstr(fdpaths[h], "backup-")) {
        interrupt_run(); return OS64_INTERRUPTED;
    }
    if (strstr(fdpaths[h], "backup-")) {
        backup_reads++;
        if (is("backup-read") && !injected) { injected = true; return -1; }
        if (is("cancel-verify") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    }
    int64_t result = read(h, buf, cap);
    if (is("backup-corrupt") && !injected && result > 0 && strstr(fdpaths[h], "backup-")) {
        ((unsigned char *)buf)[0] ^= 1; injected = true;
    }
    return result;
}
int64_t os64_read_for(int32_t h, void *buf, size_t cap, uint64_t ms)
{ (void)ms; return os64_read(h, buf, cap); }
int64_t os64_dial(const char *dial)
{ (void)dial; assert(connections < 16); return (int64_t)(10000 + connections++); }
int64_t os64_write(int32_t h, const void *buf, size_t n)
{
    if (h < 3) {
        assert(output_used + n < sizeof(output_text));
        memcpy(output_text + output_used, buf, n); output_used += n; output_text[output_used] = 0;
        return (int64_t)fwrite(buf, 1, n, h == 2 ? stderr : stdout);
    }
    if (h >= 10000) {
        unsigned i = (unsigned)(h - 10000);
        if (n == 5 && !memcmp(buf, "LIST\n", 5)) {
            size_t used = 0;
            for (unsigned j = 0; j < 3; j++) {
                if (is("review-legacy-list")) {
                    used += (size_t)snprintf(network[i] + used, sizeof(network[i]) - used, "%s\n", names[j]);
                    continue;
                }
                used += (size_t)snprintf(network[i] + used, sizeof(network[i]) - used,
                                        "%s %zu %08x\n", names[j], strlen(payloads[j]),
                                        os64_crc32(payloads[j], strlen(payloads[j])));
            }
            strcat(network[i], ".\n");
        } else if ((n > 5 && !memcmp(buf, "GET /", 5)) ||
                   (n > 12 && !memcmp(buf, "GET https://", 12))) {
            if (is("integrity-gzip") || is("integrity-gzip-ok")) {
                static const unsigned char compressed[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xcb, 0xcc, 0x4b, 0xce, 0xcf, 0xcd, 0xcc, 0x4b, 0x57, 0x70, 0x04, 0x00, 0x27, 0x87, 0x35, 0x5b, 0x0a, 0x00, 0x00, 0x00};
                int head = snprintf(network[i], sizeof(network[i]),
                    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %zu\r\n\r\n", sizeof(compressed));
                memcpy(network[i] + head, compressed, sizeof(compressed));
                network_len[i] = (size_t)head + sizeof(compressed); return (int64_t)n;
            }
            if ((is("url-tls-downgrade") || is("url-tls-redirect") || is("url-upgrade") || is("url-tls-other") || is("url-tls-head-alert")) && i == 0) {
                snprintf(network[i], sizeof network[i], "HTTP/1.1 302 Found\r\nLocation: %s://%s/next\r\nContent-Length: 0\r\n\r\n",
                         is("url-tls-downgrade") ? "http" : "https",
                         is("url-tls-other") ? "other" : "host");
                network_len[i] = strlen(network[i]); return (int64_t)n;
            }
            if (is("url-tls-cut") || is("url-tls-close")) {
                strcpy(network[i], "HTTP/1.1 200 OK\r\n\r\nincoming A");
                network_len[i] = strlen(network[i]); return (int64_t)n;
            }
            snprintf(network[i], sizeof(network[i]), "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n%s",
                     is("url-short") ? "cut" : "incoming A");
        } else {
            unsigned j;
            for (j = 0; j < 3; j++)
                if (n == strlen(names[j]) + 5 && !memcmp((const char *)buf + 4, names[j], strlen(names[j]))) break;
            assert(j < 3);
            uint32_t crc = os64_crc32(payloads[j], strlen(payloads[j]));
            if (is("crc") && j == 1) crc++;
            snprintf(network[i], sizeof(network[i]), "OK %zu %08x\n%s", strlen(payloads[j]), crc,
                     is("short") && j == 1 ? "cut" : payloads[j]);
        }
        network_len[i] = strlen(network[i]); return (int64_t)n;
    }
    if (strstr(fdpaths[h], "backup-")) {
        if (is("backup-write") && !injected) { injected = true; return -1; }
        if (is("cancel-backup") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    }
    if (run_active && !strncmp(scenario, "integrity-", 10) &&
        !is("integrity-read") && !is("integrity-cancel") && !is("integrity-gzip-ok") && !injected && n &&
        strstr(fdpaths[h], "/run-") && !strstr(fdpaths[h], "backup-")) {
        // Store different bytes while reporting a successful full write.
        unsigned char damaged[65536]; assert(n <= sizeof(damaged)); memcpy(damaged, buf, n);
        if (is("integrity-append")) {
            assert(write(h, buf, n) == (ssize_t)n); assert(write(h, "x", 1) == 1);
        } else {
            damaged[0] ^= 1; assert(write(h, damaged, n) == (ssize_t)n);
        }
        injected = true; return (int64_t)n;
    }
    return write(h, buf, n);
}

static void put(const char *path, const char *contents)
{
    int h = (int)os64_open(path, "w"); assert(h >= 0);
    assert(write(h, contents, strlen(contents)) == (ssize_t)strlen(contents)); assert(os64_close(h) == 0);
}
static void contents(const char *path, const char *want)
{
    int h = (int)os64_open(path, "r"); assert(h >= 0);
    char buf[128] = {0}; ssize_t n = read(h, buf, sizeof(buf)); assert(n >= 0); os64_close(h);
    assert(n == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)n));
}
static unsigned file_count(const char *path)
{
    char full[1024]; host_path(path, full); DIR *dir = opendir(full); if (!dir) return 0;
    unsigned count = 0; struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[512]; assert(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < (int)sizeof(child));
        os64_dirent_t de; assert(os64_stat(child, &de) == 0);
        count += de.flags & OS64_DE_DIR ? file_count(child) : 1;
    }
    closedir(dir); return count;
}

// This seam supplies authenticated bytes; cryptography and TCP pumping are
// exercised separately by the real TLS peer. No fixture trust enters the app.
struct os64_tls_transport { int32_t h; os64_tls_status_t status; };
struct os64_tls_trust { int unused; };
static struct os64_tls_trust host_trust;
os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **out, os64_tls_store_report_t *report)
{
    trust_loads++;
    memset(report, 0, sizeof *report);
    if (is("url-tls-roots")) return OS64_TLS_STORE_OPEN;
    *out = &host_trust; return OS64_TLS_STORE_OK;
}
void os64_tls_trust_free(os64_tls_trust *t) { assert(!t || t == &host_trust); }
const char *os64_tls_store_status_name(os64_tls_store_status_t s) { (void)s; return "fixture store"; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s; return "fixture TLS"; }
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c, int32_t h,
    const os64_tls_transport_limits_t *limits, os64_tls_transport **out)
{
    assert(limits && limits->handshake_ms == FETCH_IDLE_MS_DEFAULT && c->trust == &host_trust && c->alpn_count == 1);
    const char *hostname = is("url-tls-ip") ? "10.0.2.2" :
        is("url-tls-name") ? "bad_name" :
        is("url-tls-other") && tls_created ? "other" : "host";
    assert(c->hostname.length == strlen(hostname) && !memcmp(c->hostname.data, hostname, strlen(hostname)));
    assert(c->alpn[0].length == 8 && !memcmp(c->alpn[0].data, "http/1.1", 8));
    *out = NULL;
    if (is("url-tls-cert")) return OS64_TLS_CERTIFICATE;
    if (is("url-tls-ip")) return OS64_TLS_UNSUPPORTED;
    if (is("url-tls-name")) return OS64_TLS_BAD_ARGUMENT;
    *out = calloc(1, sizeof **out); assert(*out); (*out)->h = h;
    tls_created++; return OS64_TLS_OK;
}
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{ return (os64_tls_state_t){.status=t->status, .flags=OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN}; }
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n)
{ return (os64_tls_transfer_t){OS64_TLS_OK, (size_t)os64_write(t->h,p,n)}; }
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t cap)
{
    if (t->status != OS64_TLS_OK) return (os64_tls_transfer_t){t->status,0};
    int64_t n = os64_read(t->h,p,cap);
    if (!n) t->status = is("url-tls-cut") ? OS64_TLS_TRUNCATED : OS64_TLS_CLEAN_EOF;
    if (n < 0) { t->status = OS64_TLS_TRANSPORT; n = 0; }
    if (n > 0 && network_pos[t->h-10000] == network_len[t->h-10000]) {
        if (is("url-tls-alert") || is("url-tls-head-alert")) t->status = OS64_TLS_PROTOCOL;
        if (is("url-tls-framed-cut")) t->status = OS64_TLS_TRUNCATED;
    }
    return (os64_tls_transfer_t){t->status,(size_t)n};
}
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t ms)
{ (void)ms; return t->status; }
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) { return t->status; }
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t)
{ return t->status = OS64_TLS_CLEAN_EOF; }
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t, os64_tls_status_t s)
{ return t->status = s; }
void os64_tls_transport_free(os64_tls_transport *t) { if(t) { os64_close(t->h); free(t); tls_freed++; } }

static void integrity_scenario(void)
{
    bool absent = is("integrity-absent");
    bool url = is("integrity-url") || is("integrity-gzip") || is("integrity-gzip-ok");
    if (!absent) put(targets[0], "local A");
    put(targets[1], "local B"); put(targets[2], "local C");
    char *batch[] = { "os64get", "-a", "-q", "host", NULL };
    char *web[] = { "os64get", "-q", "http://host/a", "/bin/a", NULL };
    char *noarchive[] = { "os64get", "-a", "-q", "-n", "host", NULL };
    run_active = true;
    int rc = url ? os64get_entry(4, web) : is("integrity-no-archive") ?
        os64get_entry(5, noarchive) : os64get_entry(4, batch);
    run_active = false;
    bool success = is("integrity-gzip-ok");
    assert(rc == (success ? GET_OK : is("integrity-cancel") ? GET_CANCELLED : GET_PREPARE_FAILED));
    if (!success && !is("integrity-read")) assert(injected);
    assert(installs == (success ? 1u : 0u));
    if (absent) { os64_dirent_t e; assert(os64_stat(targets[0], &e) < 0); }
    else contents(targets[0], success ? payloads[0] : "local A");
    contents(targets[1], "local B"); contents(targets[2], "local C");
    assert(file_count("/home/archive") == 0);
    assert(file_count("/tmp/os64get") == 0);
    assert(file_count("/home/.os64get-tmp") == 0);
    assert(file_count("/fat/.os64get-tmp") == 0);
    for (int i = 3; i < 1024; i++) assert(fdpaths[i][0] == 0);
    printf("PASS %s\n", scenario);
}

static void enclosure_scenario(void)
{
    bool sibling = is("enclose-sibling");
    targets[0] = is("enclose-fat-alias") ? "/fat/new" : "/home/new";
    if (is("enclose-fat-scratch")) {
        assert(os64_mkdir("/fat/.OS64GET-TMP") == 0);
        assert(os64_mkdir("/fat/.OS64GET-TMP/sub") == 0);
        targets[0] = "/fat/.OS64GET-TMP/sub/new";
    }
    put(targets[1], "local B"); put(targets[2], "local C");
    char destination[256]; snprintf(destination, sizeof(destination), "%s", targets[0]);
    char *one[] = { "os64get", "-q", "host", "a", destination, NULL };
    char *batch[] = { "os64get", "-a", "-q", "host", NULL };
    bool all = is("enclose-batch") || is("enclose-fat-alias");
    if (all) names[0] = "new";
    run_active = true;
    int rc = all ? os64get_entry(4, batch) : os64get_entry(5, one);
    run_active = false;
    assert(sibling ? rc == GET_OK : rc == GET_WRITE_FAILED);
    assert(installs == (sibling ? 1u : 0u));
    if (sibling) contents(targets[0], payloads[0]);
    else { os64_dirent_t e; assert(os64_stat(targets[0], &e) < 0); }
    contents(targets[1], "local B"); contents(targets[2], "local C");
    assert(file_count("/tmp/os64get") == 0);
    assert(file_count("/home/.os64get-tmp") == 0);
    assert(file_count("/fat/.os64get-tmp") == 0);
    for (int i = 3; i < 1024; i++) assert(fdpaths[i][0] == 0);
    printf("PASS %s\n", scenario);
}

static void review_scenario(void)
{
    bool duplicate = is("review-duplicate-unchanged") || is("review-duplicate-new");
    bool single = is("review-single-full");
    if (is("review-empty-ro")) payloads[2] = "";
    if (duplicate) {
        names[0] = "c"; names[1] = "C"; names[2] = "b";
        payloads[1] = payloads[0];
        if (is("review-duplicate-unchanged")) put("/fat/c", payloads[0]);
    } else {
        put(targets[0], single || is("review-all-full") ? payloads[0] : "local A");
        put(targets[1], is("review-all-full") ? payloads[1] : "local B");
        put(targets[2], is("review-empty-backup") || is("review-partial-backup") ? "local C" : payloads[2]);
    }
    if (is("review-legacy-list")) put(targets[2], "");
    char *batch[] = { "os64get", "-a", "-q", "host", NULL };
    char *force[] = { "os64get", "-a", "-q", "-f", "host", NULL };
    char *one[] = { "os64get", "-q", "host", "a", "/bin/a", NULL };
    run_active = true;
    int rc = single ? os64get_entry(5, one) : is("review-force-ro") ? os64get_entry(5, force) : os64get_entry(4, batch);
    run_active = false;
    bool backup_failure = is("review-empty-backup") || is("review-partial-backup");
    assert(rc == (duplicate ? GET_USAGE : backup_failure ? GET_PREPARE_FAILED : is("review-force-ro") ? GET_WRITE_FAILED : GET_OK));
    if (backup_failure) {
        contents(targets[0], "local A"); contents(targets[1], "local B"); contents(targets[2], "local C");
        bool kept = is("review-partial-backup");
        assert(file_count("/home/archive") == (kept ? 1u : 0u));
        assert((strstr(output_text, "originals kept at") != NULL) == kept);
        assert(installs == 0);
    } else if (duplicate || is("review-force-ro")) assert(installs == 0);
    else {
        contents(targets[0], payloads[0]);
        contents(targets[1], single ? "local B" : payloads[1]);
        contents(targets[2], payloads[2]);
        assert(blocked_scratch_attempts == 0);
        unsigned expected = single || is("review-all-full") ? 0u : is("review-legacy-list") ? 3u : 2u;
        assert(installs == expected);
        assert(file_count("/home/archive") == expected);
    }
    assert(file_count("/tmp/os64get") == 0);
    assert(file_count("/home/.os64get-tmp") == 0);
    assert(file_count("/fat/.os64get-tmp") == 0);
    for (int i = 3; i < 1024; i++) assert(fdpaths[i][0] == 0);
    printf("PASS %s\n", scenario);
}

int main(int argc, char **argv)
{
    assert(argc == 3); scenario = argv[1]; snprintf(sandbox, sizeof(sandbox), "%s", argv[2]);
    assert(os64_mkdir("/bin") == 0); assert(os64_mkdir("/home") == 0);
    assert(os64_mkdir("/fat") == 0); assert(os64_mkdir("/tmp") == 0);
    if (!strncmp(scenario, "integrity-", 10)) { integrity_scenario(); return 0; }
    if (!strncmp(scenario, "enclose-", 8)) { enclosure_scenario(); return 0; }
    if (!strncmp(scenario, "review-", 7)) { review_scenario(); return 0; }
    if (is("archive-overlap")) {
        install_file_t f;
        assert(install_init("/fat/.OS64GET-TMP"));
        put("/bin/a", "original");
        assert(install_plan(&f, "/bin/a"));
        put(f.part, "incoming");
        install_received(&f, 8, os64_crc32("incoming", 8));
        assert(!install_prepare(&f));
        assert(install_cleanup(&f, 1));
        contents("/bin/a", "original");
        assert(file_count("/fat/.os64get-tmp") == 0);
        puts("PASS archive-overlap");
        return 0;
    }
    if (is("aliases") || is("appeared")) {
        assert(install_init("/home/archive"));
        install_file_t f[2];
        if (is("aliases")) {
            assert(os64_mkdir("/fat/Directory") == 0);
            assert(install_plan(&f[0], "/fat/directory/New"));
            assert(install_plan(&f[1], "/fat/Directory/new"));
            assert(install_conflicts(&f[0], &f[1]));
            assert(install_cleanup(f, 2));
            assert(install_init("/home/archive"));
            assert(install_plan(&f[0], "/bin/New"));
            assert(install_plan(&f[1], "/bin/new"));
            assert(!install_conflicts(&f[0], &f[1]));
            assert(install_cleanup(f, 2));
        } else {
            assert(install_plan(&f[0], "/bin/new"));
            put(f[0].part, "download");
            install_received(&f[0], 8, os64_crc32("download", 8));
            assert(install_prepare(&f[0]));
            put("/bin/new", "external writer");
            assert(install_begin_commit());
            assert(!install_commit(&f[0]));
            contents("/bin/new", "external writer");
            assert(install_cleanup(f, 1));
            assert(file_count("/home/archive") == 0);
        }
        printf("PASS %s\n", scenario);
        return 0;
    }
    if (is("unsafe-name")) names[1] = "../b";
    if (!is("absent")) {
        put(targets[0], is("unchanged") || is("force-identical") ? payloads[0] : "local A");
        put(targets[1], is("unchanged") || is("force-identical") ? payloads[1] : "local B");
        put(targets[2], is("unchanged") || is("force-identical") ? payloads[2] : "local C");
    }
    char *args[] = { "os64get", "-a", "-q", "host", NULL };
    char *force[] = { "os64get", "-a", "-q", "-f", "host", NULL };
    char *noarchive[] = { "os64get", "-a", "-q", "-n", "host", NULL };
    char *url[] = { "os64get", "-q", "http://host/a", "/bin/a", NULL };
    if (is("url-https") || !strncmp(scenario, "url-tls-", 8)) url[2] = "https://host/a";
    if (is("url-tls-ip")) url[2] = "https://10.0.2.2/a";
    if (is("url-tls-name")) url[2] = "https://bad_name/a";
    if (is("url-archive-blocked")) put("/home/archive", "not a directory");
    bool url_mode = !strncmp(scenario, "url", 3);
    char *single[] = { "os64get", "-q", "host", "a", "/bin/a", NULL };
    int rc = is("single") ? os64get_entry(5, single) : is("force-identical") ? os64get_entry(5, force) :
             is("no-archive") ? os64get_entry(5, noarchive) :
             url_mode ? os64get_entry(4, url) : os64get_entry(4, args);
    bool cancel = !strncmp(scenario, "cancel-", 7) || is("url-cancel");
    bool success = is("success") || is("absent") || is("unchanged") || is("force-identical") || is("no-archive") || is("url") || is("url-https") || is("url-archive-blocked") || is("single") || is("url-tls-good") || is("url-tls-close") || is("url-tls-framed-cut") || is("url-tls-redirect") || is("url-upgrade") || is("url-tls-other") || is("url-tls-bypass");
    assert(success ? rc == 0 : rc != 0);
    assert(tls_created == tls_freed);
    for (unsigned i = 0; i < connections; i++) assert(network_closed[i]);
    if (!strncmp(scenario, "url-tls-", 8) || is("url-upgrade")) assert(trust_loads == 1);
    else assert(trust_loads == 0);
    if (is("url-tls-downgrade")) {
        assert(rc == GET_REDIRECT && connections == 1);
        assert(strstr(output_text, "refusing HTTPS-to-HTTP downgrade"));
        assert(strstr(output_text, "os64get 'http://host/next'"));
    }
    if (is("url-tls-head-alert")) assert(rc == GET_BAD_HEADER && connections == 1);
    if (is("url-tls-redirect")) assert(connections == 2 && tls_created == 2);
    if (is("url-tls-roots") || is("url-tls-cert") || is("url-tls-ip") || is("url-tls-name")) assert(rc == GET_TLS_FAILED);
    if (is("url-tls-alert") || is("url-tls-cut")) assert(rc == GET_SHORT);
    bool published = success || is("cancel-commit") || is("cancel-cleanup") || is("publish");
    assert(!cancel || rc == GET_CANCELLED);
    for (unsigned i = 0; i < 3; i++) {
        bool landed = published && !(is("publish") && i == 1) && !((url_mode || is("single")) && i != 0);
        contents(targets[i], landed ? payloads[i] : i == 0 ? "local A" : i == 1 ? "local B" : "local C");
        if (stages[i].backup[0]) contents(stages[i].backup, i == 0 ? "local A" : i == 1 ? "local B" : "local C");
    }
    if (is("absent") || is("unchanged") || is("force-identical") || is("no-archive")) assert(file_count("/home/archive") == 0);
    if (is("success") || is("publish") || is("cancel-commit") || is("cancel-transition"))
        assert(file_count("/home/archive") == 3);
    if (url_mode) {
        assert(backup_reads == 0);
        if (is("url-archive-blocked")) contents("/home/archive", "not a directory");
        else {
            os64_dirent_t archive;
            assert(os64_stat("/home/archive", &archive) < 0);
        }
    }
    if (is("single")) assert(file_count("/home/archive") == 1);
    if (!published) assert(installs == 0);
    assert(file_count("/tmp/os64get") == 0);
    assert(file_count("/home/.os64get-tmp") == 0);
    assert(file_count("/fat/.os64get-tmp") == 0);
    for (int i = 3; i < 1024; i++) assert(fdpaths[i][0] == 0);
    printf("PASS %s\n", scenario);
    return 0;
}
