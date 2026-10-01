// test_way_fetch_host.c — way_fetch_whole end to end on the host: the real
// libfetch and the real cache (CACHE.md) against scripted peers and a real
// temporary directory, under the sanitizers. Where test_way_cache.inc proves
// the rules and the store one at a time, this proves how load.c wires them
// to the wire: a 304 through a redirect, a 304 that ends an entry, what a
// redirect says about keeping it, a field too long to read, and the
// caller's options left as they were.
//
// The seam is libos64's calls, as in test_fetch_host.c: a dial reaches a
// peer by host, and each dial of a host takes its next scripted reply; a
// handle below FILE_BASE is a connection, at or above it a file.

#define _DEFAULT_SOURCE

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fetch/fetch.h"
#include "fetch/transport.h"
#include "internal.h"
#include "os64/date.h"
#include "os64/io.h"
#include "os64/os64.h"
#include "way/cache.h"
#include "way/way.h"

// ── The heap ────────────────────────────────────────────────────────────

void *os64_malloc(size_t size) { return malloc(size != 0 ? size : 1); }
void *os64_calloc(size_t n, size_t size) { return calloc(n != 0 ? n : 1, size != 0 ? size : 1); }
void *os64_realloc(void *p, size_t size) { return realloc(p, size != 0 ? size : 1); }
void os64_free(void *p) { free(p); }
void os64_yield(void) {}
uint64_t os64_taskid(void) { return (uint64_t)getpid(); }
const char *os64_getenv(const char *key) { (void)key; return NULL; }

// ── The clocks ──────────────────────────────────────────────────────────

#define D 1790758509                     // Wed, 30 Sep 2026 08:55:09 GMT
static int64_t s_clock = D;
static uint64_t s_ms;
int64_t os64_time(os64_time_t *t)
{
    memset(t, 0, sizeof(*t));
    t->epoch = s_clock;
    return 0;
}
int64_t os64_ticks(os64_ticks_t *t)
{
    *t = (os64_ticks_t){.ticks = s_ms, .per_second = 1000};
    return 0;
}

// ── The peers ───────────────────────────────────────────────────────────

#define REPLIES_MAX 8
typedef struct {
    const char *host;
    const char *replies[REPLIES_MAX];   // NULL: the dial is refused
    int n, next;
} peer_t;
static peer_t s_peers[8];
static int s_npeers, s_dials;

static peer_t *peer(const char *host)
{
    for (int i = 0; i < s_npeers; i++)
        if (strcmp(s_peers[i].host, host) == 0)
            return &s_peers[i];
    peer_t *p = &s_peers[s_npeers++];
    memset(p, 0, sizeof(*p));
    p->host = host;
    return p;
}

// The next dial of `host` gets `reply`; NULL refuses it.
static void script(const char *host, const char *reply)
{
    peer_t *p = peer(host);
    assert(p->n < REPLIES_MAX);
    p->replies[p->n++] = reply;
}

typedef struct {
    bool open;
    const char *reply;
    size_t served;
    char request[8192];
    size_t reqlen;
} conn_t;
static conn_t s_conns[8];
static char s_last_request[8][8192];   // per host index: what it was last asked
static int s_conn_peer[8];

int64_t os64_dial(const char *dialstring)
{
    char host[256];
    unsigned port;
    if (sscanf(dialstring, "tcp!%255[^!]!%u", host, &port) != 2)
        return -1;
    s_dials++;
    for (int i = 0; i < s_npeers; i++) {
        peer_t *p = &s_peers[i];
        if (strcmp(p->host, host) != 0)
            continue;
        if (p->next >= p->n || p->replies[p->next] == NULL) {
            p->next++;
            return -3;                  // refused
        }
        for (int h = 0; h < 8; h++)
            if (!s_conns[h].open) {
                memset(&s_conns[h], 0, sizeof(s_conns[h]));
                s_conns[h].open = true;
                s_conns[h].reply = p->replies[p->next++];
                s_conn_peer[h] = i;
                return 100 + h;
            }
        abort();
    }
    return -2;
}
const char *os64_dial_reason(int64_t err) { return err == -3 ? "connection refused" : "no such host"; }

int64_t os64_read_for(int32_t h, void *buf, size_t cap, uint64_t ms)
{
    // A look that does not wait finds nothing: the peer answers a request
    // once it has been sent, as test_fetch_host.c's peers do.
    if (ms == 0)
        return OS64_ERR_TIMEOUT;
    conn_t *c = &s_conns[h - 100];
    size_t left = strlen(c->reply) - c->served;
    size_t n = cap < left ? cap : left;
    memcpy(buf, c->reply + c->served, n);
    c->served += n;
    return (int64_t)n;
}
int64_t os64_write_for(int32_t h, const void *buf, size_t n, uint64_t ms)
{
    (void)ms;
    conn_t *c = &s_conns[h - 100];
    assert(c->reqlen + n < sizeof(c->request));
    memcpy(c->request + c->reqlen, buf, n);
    c->reqlen += n;
    c->request[c->reqlen] = '\0';
    return (int64_t)n;
}

// TLS is never reached: every address here is http.
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c, int32_t h,
                                            const os64_tls_transport_limits_t *l,
                                            os64_tls_transport **out)
{ (void)c; (void)h; (void)l; (void)out; abort(); }
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t) { (void)t; abort(); }
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t ms) { (void)t; (void)ms; abort(); }
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n) { (void)t; (void)p; (void)n; abort(); }
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t n) { (void)t; (void)p; (void)n; abort(); }
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) { (void)t; abort(); }
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t) { (void)t; abort(); }
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t, os64_tls_status_t s) { (void)t; (void)s; abort(); }
void os64_tls_transport_free(os64_tls_transport *t) { (void)t; abort(); }
os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **c, os64_tls_store_report_t *r) { (void)c; (void)r; abort(); }
void os64_tls_trust_free(os64_tls_trust *s) { (void)s; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s; return "tls"; }
const char *os64_tls_error_description(os64_tls_status_t s, os64_tls_policy_reason_t p, int e) { (void)s; (void)p; (void)e; return "tls"; }
const char *os64_tls_store_status_name(os64_tls_store_status_t s) { (void)s; return "store"; }

// ── The files ───────────────────────────────────────────────────────────

#define FILE_BASE 1000
static DIR *s_dirs[4];
static char s_dir_paths[4][512];

int64_t os64_open(const char *path, const char *mode)
{
    int flags = mode == NULL || mode[0] == 'r' ? O_RDONLY
              : mode[0] == 'x'                 ? O_WRONLY | O_CREAT | O_EXCL
                                               : O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0644);
    return fd < 0 ? -1 : fd + FILE_BASE;
}
int64_t os64_read(int32_t h, void *buf, size_t len)
{
    return h >= FILE_BASE ? read(h - FILE_BASE, buf, len) : -1;
}
int64_t os64_write(int32_t h, const void *buf, size_t len)
{
    return h >= FILE_BASE ? write(h - FILE_BASE, buf, len) : (int64_t)len;
}
int64_t os64_close(int32_t h)
{
    if (h >= 100 && h < 108) {
        conn_t *c = &s_conns[h - 100];
        memcpy(s_last_request[s_conn_peer[h - 100]], c->request, sizeof(c->request));
        c->open = false;
        return 0;
    }
    if (h >= FILE_BASE + 4096) {
        closedir(s_dirs[h - FILE_BASE - 4096]);
        s_dirs[h - FILE_BASE - 4096] = NULL;
        return 0;
    }
    return close(h - FILE_BASE);
}
int64_t os64_unlink(const char *path) { return unlink(path) == 0 ? 0 : -1; }
int64_t os64_rename(const char *from, const char *to) { return rename(from, to) == 0 ? 0 : -1; }
int64_t os64_mkdir(const char *path) { return mkdir(path, 0755) == 0 ? 0 : -1; }

static void dirent_of(const struct stat *st, const char *name, os64_dirent_t *e)
{
    memset(e, 0, sizeof(*e));
    e->size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
    e->flags = S_ISDIR(st->st_mode) ? OS64_DE_DIR : 0;
    e->mtime = (uint64_t)st->st_mtime;
    snprintf(e->name, sizeof(e->name), "%s", name);
}
int64_t os64_stat(const char *path, os64_dirent_t *e)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    const char *slash = strrchr(path, '/');
    dirent_of(&st, slash != NULL ? slash + 1 : path, e);
    return 0;
}
int64_t os64_opendir(const char *path)
{
    for (int i = 0; i < 4; i++)
        if (s_dirs[i] == NULL) {
            if ((s_dirs[i] = opendir(path)) == NULL)
                return -1;
            snprintf(s_dir_paths[i], sizeof(s_dir_paths[i]), "%s", path);
            return FILE_BASE + 4096 + i;
        }
    return -1;
}
int64_t os64_readdir(int32_t h, os64_dirent_t *e)
{
    int i = h - FILE_BASE - 4096;
    struct dirent *d;
    while ((d = readdir(s_dirs[i])) != NULL) {
        if (d->d_name[0] == '.')
            continue;
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", s_dir_paths[i], d->d_name);
        if (stat(path, &st) == 0) {
            dirent_of(&st, d->d_name, e);
            return 1;
        }
    }
    return 0;
}

// ── The cases ───────────────────────────────────────────────────────────

static int checks, failures;
static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    }
}

static char s_dir[600];
static way_cache_t *s_cache;
static way_jar_t *s_jar;

static void fresh(void)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", s_dir);
    if (system(cmd) != 0)
        abort();
    s_npeers = 0;
    s_dials = 0;
    memset(s_last_request, 0, sizeof(s_last_request));
    s_clock = D;
}

// A GET of `url` through the cache, with options a caller would make.
static bool whole(const char *url, way_whole_t *out, os64_fetch_options_t *opt)
{
    os64_fetch_options_t local;
    if (opt == NULL) {
        memset(&local, 0, sizeof(local));
        local.method = OS64_FETCH_METHOD_GET;
        opt = &local;
    }
    way_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.jar = s_jar;
    hooks.cache = s_cache;
    return way_fetch_whole(&hooks, url, opt, 1 << 20, out);
}

static bool body_is(const way_whole_t *w, const char *text)
{
    return w->len == strlen(text) && memcmp(w->bytes, text, w->len) == 0;
}

static const char *kOld = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nCache-Control: max-age=0\r\n"
                          "ETag: \"v1\"\r\n\r\nOLD";

static void options_left_alone(void)
{
    fresh();
    script("a.test", kOld);
    script("a.test", "HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=0\r\n\r\n");
    os64_fetch_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = OS64_FETCH_METHOD_GET;
    way_whole_t w;
    expect("options: a first load", whole("http://a.test/x", &w, &opt) && body_is(&w, "OLD"), NULL);
    free(w.bytes);
    expect("options: a conditional load", whole("http://a.test/x", &w, &opt) &&
           w.served == WAY_SERVED_CONFIRMED, NULL);
    free(w.bytes);
    // The question that asked whether it changed was the fetch's, not the
    // caller's: its options do not point into a frame that has returned.
    expect("options: the caller's extra headers are as they were", opt.extra_headers == NULL,
           NULL);
}

static void redirect_then_304(void)
{
    fresh();
    const char *moved = "HTTP/1.1 301 Moved Permanently\r\nLocation: http://b.test/y\r\n"
                        "Content-Length: 0\r\n\r\n";
    script("a.test", moved);
    script("b.test", kOld);
    script("a.test", moved);
    script("b.test", "HTTP/1.1 304 Not Modified\r\n\r\n");
    way_whole_t w;
    expect("redirect 304: a first load through the redirect",
           whole("http://a.test/x", &w, NULL) && body_is(&w, "OLD"), NULL);
    free(w.bytes);
    bool ok = whole("http://a.test/x", &w, NULL);
    expect("redirect 304: the kept body, confirmed at the redirect's end",
           ok && w.status == 200 && w.served == WAY_SERVED_CONFIRMED && body_is(&w, "OLD"), NULL);
    free(w.bytes);
    int b = 1;
    expect("redirect 304: asked with its validator at the end of the chain",
           strstr(s_last_request[b], "If-None-Match: \"v1\"") != NULL, s_last_request[b]);
}

static void restrictive_304(void)
{
    fresh();
    script("c.test", kOld);
    script("c.test", "HTTP/1.1 304 Not Modified\r\nCache-Control: no-store\r\n\r\n");
    script("c.test", NULL);
    way_whole_t w;
    (void)whole("http://c.test/x", &w, NULL);
    free(w.bytes);
    expect("304 no-store: this once, the kept body",
           whole("http://c.test/x", &w, NULL) && w.served == WAY_SERVED_CONFIRMED &&
           body_is(&w, "OLD"), NULL);
    free(w.bytes);
    bool ok = whole("http://c.test/x", &w, NULL);
    expect("304 no-store: after it, nothing to serve stale", !ok && w.bytes == NULL, NULL);
    free(w.bytes);
}

static void redirect_restrictions(void)
{
    fresh();
    const char *fresh600 = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nCache-Control: max-age=600\r\n\r\nOLD";
    // A redirect that asks to be checked every time: the chain is.
    script("d.test", "HTTP/1.1 301 Moved\r\nLocation: http://e.test/\r\nCache-Control: no-cache\r\n"
                     "Content-Length: 0\r\n\r\n");
    script("e.test", fresh600);
    script("d.test", "HTTP/1.1 301 Moved\r\nLocation: http://f.test/\r\nContent-Length: 0\r\n\r\n");
    script("f.test", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nNEW");
    way_whole_t w;
    (void)whole("http://d.test/", &w, NULL);
    free(w.bytes);
    int before = s_dials;
    expect("redirect no-cache: asked again, and the new answer taken",
           whole("http://d.test/", &w, NULL) && body_is(&w, "NEW") && s_dials > before, NULL);
    free(w.bytes);

    // A redirect that forbids keeping: nothing is.
    fresh();
    script("g.test", "HTTP/1.1 301 Moved\r\nLocation: http://h.test/\r\nCache-Control: no-store\r\n"
                     "Content-Length: 0\r\n\r\n");
    script("h.test", fresh600);
    script("g.test", NULL);
    (void)whole("http://g.test/", &w, NULL);
    free(w.bytes);
    expect("redirect no-store: nothing kept", !whole("http://g.test/", &w, NULL), NULL);
    free(w.bytes);

    // A redirect fresh for a minute: the chain is, though its end says ten.
    fresh();
    script("i.test", "HTTP/1.1 301 Moved\r\nLocation: http://j.test/\r\nCache-Control: max-age=60\r\n"
                     "Content-Length: 0\r\n\r\n");
    script("j.test", fresh600);
    script("i.test", "HTTP/1.1 301 Moved\r\nLocation: http://j.test/\r\nContent-Length: 0\r\n\r\n");
    script("j.test", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nCache-Control: max-age=600\r\n\r\nNEW");
    (void)whole("http://i.test/", &w, NULL);
    free(w.bytes);
    s_clock = D + 30;
    before = s_dials;
    expect("redirect max-age: fresh within the redirect's minute",
           whole("http://i.test/", &w, NULL) && w.served == WAY_SERVED_KEPT && s_dials == before,
           NULL);
    free(w.bytes);
    s_clock = D + 90;
    expect("redirect max-age: asked again past it",
           whole("http://i.test/", &w, NULL) && body_is(&w, "NEW") && s_dials > before, NULL);
    free(w.bytes);

    // A permanent redirect that says nothing is kept by heuristic, as before.
    fresh();
    script("k.test", "HTTP/1.1 308 Permanent\r\nLocation: http://l.test/\r\nContent-Length: 0\r\n\r\n");
    script("l.test", fresh600);
    (void)whole("http://k.test/", &w, NULL);
    free(w.bytes);
    before = s_dials;
    expect("redirect silent: the chain kept for its end's ten minutes",
           whole("http://k.test/", &w, NULL) && w.served == WAY_SERVED_KEPT && s_dials == before,
           NULL);
    free(w.bytes);
}

static void field_too_long(void)
{
    fresh();
    static char reply[4096];
    char pad[2600];
    memset(pad, 'p', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    snprintf(reply, sizeof(reply),
             "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nCache-Control: no-store, x=\"%s\"\r\n"
             "Last-Modified: Wed, 23 Sep 2026 20:36:58 GMT\r\nDate: Wed, 30 Sep 2026 08:55:09 GMT\r\n"
             "\r\nSECRET",
             pad);
    script("m.test", reply);
    script("m.test", NULL);
    way_whole_t w;
    expect("long field: read", whole("http://m.test/", &w, NULL) && body_is(&w, "SECRET"), NULL);
    free(w.bytes);
    expect("long field: a Cache-Control too long to read is no permission to keep",
           !whole("http://m.test/", &w, NULL), NULL);
    free(w.bytes);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char root[] = "/tmp/way_fetchXXXXXX";
    if (mkdtemp(root) == NULL)
        return 1;
    snprintf(s_dir, sizeof(s_dir), "%s/cache", root);
    s_cache = way_cache_open(s_dir, 1 << 24);
    s_jar = way_jar_new();
    options_left_alone();
    redirect_then_304();
    restrictive_304();
    redirect_restrictions();
    field_too_long();
    way_cache_close(s_cache);
    way_jar_free(s_jar);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0)
        printf("could not remove %s\n", root);
    printf("way_fetch_whole: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
