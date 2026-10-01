// The browser's cache (way/cache.h, CACHE.md): RFC 9111's rules as pure
// functions, and the store against a real temporary directory, reached
// through the libos64 file calls below — each a few lines over POSIX. Every
// epoch was worked out with a calendar: 1790758509 is danlegt.com's Date,
// Wed, 30 Sep 2026 08:55:09 GMT; 1790195818 its pictures' Last-Modified,
// Wed, 23 Sep 2026 20:36:58 GMT.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>

#include "os64/date.h"
#include "os64/io.h"

#define D 1790758509
#define LM 1790195818

// ── libos64's file calls, over POSIX ────────────────────────────────────
//
// A handle is a descriptor, or a directory stream, plus FILE_BASE, so it
// cannot be mistaken for the console the rest of the harness writes to.

#define FILE_BASE 1000
#define DIRS_MAX 8
static DIR *s_dirs[DIRS_MAX];
static int64_t s_clock = D;

int64_t os64_time(os64_time_t *t)
{
    memset(t, 0, sizeof(*t));
    t->epoch = s_clock;
    return 0;
}

uint64_t os64_taskid(void)
{
    return (uint64_t)getpid();
}

int64_t os64_open(const char *path, const char *mode)
{
    int flags = mode == NULL || mode[0] == 'r' ? O_RDONLY
              : mode[0] == 'x'                 ? O_WRONLY | O_CREAT | O_EXCL
                                               : O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0644);
    return fd < 0 ? -1 : fd + FILE_BASE;
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    return handle >= FILE_BASE ? read(handle - FILE_BASE, buf, len) : -1;
}

int64_t os64_close(int32_t handle)
{
    if (handle >= FILE_BASE + 4096 && handle < FILE_BASE + 4096 + DIRS_MAX) {
        closedir(s_dirs[handle - FILE_BASE - 4096]);
        s_dirs[handle - FILE_BASE - 4096] = NULL;
        return 0;
    }
    return handle >= FILE_BASE ? close(handle - FILE_BASE) : -1;
}

int64_t os64_unlink(const char *path)
{
    return unlink(path) == 0 || rmdir(path) == 0 ? 0 : -1;
}

int64_t os64_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : -1;
}

int64_t os64_mkdir(const char *path)
{
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

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

static char s_dir_paths[DIRS_MAX][512];

int64_t os64_opendir(const char *path)
{
    for (int i = 0; i < DIRS_MAX; i++)
        if (s_dirs[i] == NULL) {
            s_dirs[i] = opendir(path);
            if (s_dirs[i] == NULL)
                return -1;
            snprintf(s_dir_paths[i], sizeof(s_dir_paths[i]), "%s", path);
            return FILE_BASE + 4096 + i;
        }
    return -1;
}

int64_t os64_readdir(int32_t handle, os64_dirent_t *e)
{
    int i = handle - FILE_BASE - 4096;
    struct dirent *d;
    while ((d = readdir(s_dirs[i])) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", s_dir_paths[i], d->d_name);
        if (stat(path, &st) != 0)
            continue;
        dirent_of(&st, d->d_name, e);
        return 1;
    }
    return 0;
}

// ── The rules ───────────────────────────────────────────────────────────

static os64_fetch_keep_t fields(const char *cc, const char *date, const char *lm)
{
    os64_fetch_keep_t f;
    memset(&f, 0, sizeof(f));
    snprintf(f.cache_control, sizeof(f.cache_control), "%s", cc);
    snprintf(f.date, sizeof(f.date), "%s", date);
    snprintf(f.last_modified, sizeof(f.last_modified), "%s", lm);
    return f;
}

static const char *kDate = "Wed, 30 Sep 2026 08:55:09 GMT";
static const char *kModified = "Wed, 23 Sep 2026 20:36:58 GMT";

static void keep_rules(void)
{
    way_keep_t k;
    // danlegt.com's pictures: a year, from when they were sent.
    os64_fetch_keep_t f = fields("public, max-age=31536000, immutable", kDate, kModified);
    expect("keep: danlegt's picture is kept", way_keep_read(&f, D, &k), NULL);
    expect("keep: its lifetime is a year", way_keep_lifetime(&k) == 31536000, NULL);
    expect("keep: fresh a day later", way_keep_fresh(&k, D + 86400), NULL);
    expect("keep: stale a year later", !way_keep_fresh(&k, D + 31536000), NULL);
    expect("keep: its dates read", k.date == D && k.last_modified == LM, NULL);

    f = fields("no-store", "", "");
    expect("keep: no-store is not kept", !way_keep_read(&f, D, &k), NULL);
    f = fields("No-Cache, max-age=600", "", "");
    expect("keep: no-cache is kept, and never fresh",
           way_keep_read(&f, D, &k) && !way_keep_fresh(&k, D), NULL);
    f = fields("", "", "");
    snprintf(f.pragma, sizeof(f.pragma), "no-cache");
    expect("keep: Pragma: no-cache without Cache-Control",
           way_keep_read(&f, D, &k) && k.no_cache, NULL);
    f = fields("max-age=60", "", "");
    snprintf(f.pragma, sizeof(f.pragma), "no-cache");
    expect("keep: Pragma yields to a Cache-Control",
           way_keep_read(&f, D, &k) && !k.no_cache && way_keep_fresh(&k, D + 59), NULL);
    f = fields("max-age=100, max-age=50", "", "");
    expect("keep: of two max-ages, the shorter", way_keep_read(&f, D, &k) && k.max_age == 50, NULL);
    f = fields("max-age=soon", "", "");
    expect("keep: a max-age that does not read is stale at once",
           way_keep_read(&f, D, &k) && k.max_age == 0 && !way_keep_fresh(&k, D), NULL);
    f = fields("max-age=\"30\"", "", "");
    expect("keep: a quoted max-age", way_keep_read(&f, D, &k) && k.max_age == 30, NULL);

    f = fields("max-age=60", "", "");
    snprintf(f.vary, sizeof(f.vary), "Accept-Encoding");
    expect("keep: a Vary on the encoding is kept", way_keep_read(&f, D, &k), NULL);
    snprintf(f.vary, sizeof(f.vary), "accept-encoding, User-Agent");
    expect("keep: a Vary on the agent is not", !way_keep_read(&f, D, &k), NULL);
    snprintf(f.vary, sizeof(f.vary), "*");
    expect("keep: Vary: * is not", !way_keep_read(&f, D, &k), NULL);
    f = fields("max-age=60", "", "");
    f.unreadable = true;
    expect("keep: a field not read whole is not kept", !way_keep_read(&f, D, &k), NULL);

    // Expires, from the reply's own Date; one that is not a date is past.
    f = fields("", kDate, "");
    snprintf(f.expires, sizeof(f.expires), "Wed, 30 Sep 2026 09:55:09 GMT");
    expect("keep: Expires less Date", way_keep_read(&f, D + 500, &k) &&
           way_keep_lifetime(&k) == 3600, NULL);
    snprintf(f.expires, sizeof(f.expires), "0");
    expect("keep: an Expires of 0 is stale", way_keep_read(&f, D, &k) &&
           way_keep_lifetime(&k) == 0 && !way_keep_fresh(&k, D), NULL);
    f = fields("max-age=10", kDate, "");
    snprintf(f.expires, sizeof(f.expires), "Wed, 30 Sep 2026 09:55:09 GMT");
    expect("keep: max-age outranks Expires", way_keep_read(&f, D, &k) &&
           way_keep_lifetime(&k) == 10, NULL);

    // The heuristic: a tenth of how long it had gone unmodified, held to a
    // week. (D - LM) is 562691 seconds.
    f = fields("", kDate, kModified);
    expect("keep: a tenth of the unmodified time", way_keep_read(&f, D, &k) &&
           way_keep_lifetime(&k) == 56269, NULL);
    f = fields("", kDate, "Mon, 01 Jan 1990 00:00:00 GMT");
    expect("keep: no more than a week", way_keep_read(&f, D, &k) &&
           way_keep_lifetime(&k) == WAY_KEEP_HEURISTIC_MAX, NULL);
    f = fields("", "", "");
    expect("keep: nothing said is stale", way_keep_read(&f, D, &k) &&
           way_keep_lifetime(&k) == 0, NULL);

    // Age: the larger of the Age it came with and how late its Date was,
    // plus how long it has been kept; a clock behind the server's adds
    // nothing.
    f = fields("max-age=1000", kDate, "");
    snprintf(f.age, sizeof(f.age), "100");
    expect("keep: Age beats a Date 30 late", way_keep_read(&f, D + 30, &k) &&
           way_keep_age(&k, D + 80) == 150, NULL);
    snprintf(f.age, sizeof(f.age), "10");
    expect("keep: a Date 30 late beats Age", way_keep_read(&f, D + 30, &k) &&
           way_keep_age(&k, D + 80) == 80, NULL);
    f = fields("max-age=1000", kDate, "");
    expect("keep: a clock behind the server's", way_keep_read(&f, D - 500, &k) &&
           way_keep_age(&k, D - 400) == 100, NULL);
    expect("keep: a clock gone back", way_keep_age(&k, D - 900) == 0, NULL);

    // What is asked when it is stale.
    char out[256];
    f = fields("max-age=0", kDate, kModified);
    snprintf(f.etag, sizeof(f.etag), "\"v1\"");
    (void)way_keep_read(&f, D, &k);
    expect("keep: an ETag is asked with If-None-Match",
           way_keep_conditions(&k, out, sizeof(out)) > 0 &&
           strcmp(out, "If-None-Match: \"v1\"\r\n") == 0, out);
    f = fields("max-age=0", kDate, kModified);
    (void)way_keep_read(&f, D, &k);
    expect("keep: a Last-Modified alone with If-Modified-Since",
           way_keep_conditions(&k, out, sizeof(out)) > 0 &&
           strcmp(out, "If-Modified-Since: Wed, 23 Sep 2026 20:36:58 GMT\r\n") == 0, out);
    f = fields("max-age=0", kDate, "");
    (void)way_keep_read(&f, D, &k);
    expect("keep: nothing to ask with", way_keep_conditions(&k, out, sizeof(out)) == 0 &&
           out[0] == '\0', NULL);
    expect("keep: too small a buffer asks nothing",
           way_keep_conditions(&k, out, 4) == 0, NULL);

    // A 304 lays its fields over the kept ones and restarts the clock.
    f = fields("max-age=60", kDate, kModified);
    snprintf(f.etag, sizeof(f.etag), "\"v1\"");
    (void)way_keep_read(&f, D, &k);
    os64_fetch_keep_t n = fields("max-age=600", "Wed, 30 Sep 2026 09:00:09 GMT", "");
    expect("keep: a 304 confirms", way_keep_confirm(&k, &n, D + 300), NULL);
    expect("keep: its max-age, the kept ETag and Last-Modified",
           k.max_age == 600 && strcmp(k.etag, "\"v1\"") == 0 && k.last_modified == LM &&
           k.stored == D + 300, NULL);
    expect("keep: fresh again", way_keep_fresh(&k, D + 800) && !way_keep_fresh(&k, D + 900), NULL);
    f = fields("max-age=60", kDate, "");
    (void)way_keep_read(&f, D, &k);
    n = fields("", "", "");
    expect("keep: a 304 saying nothing keeps the max-age",
           way_keep_confirm(&k, &n, D + 300) && k.max_age == 60, NULL);
    n = fields("no-store", "", "");
    expect("keep: a 304 saying no-store ends it", !way_keep_confirm(&k, &n, D + 300), NULL);

    // Stale while the server cannot be reached.
    f = fields("max-age=60", "", "");
    (void)way_keep_read(&f, D, &k);
    expect("keep: stale may serve when unreachable", way_keep_stale_ok(&k), NULL);
    f = fields("max-age=60, must-revalidate", "", "");
    (void)way_keep_read(&f, D, &k);
    expect("keep: not with must-revalidate", !way_keep_stale_ok(&k), NULL);
}

// What the redirects on the way to a reply said, laid over it.
static void chain_rules(void)
{
    way_keep_t k;
    os64_fetch_keep_t end = fields("max-age=600", kDate, "");
    way_chain_t chain;

    way_chain_start(&chain);
    (void)way_keep_read(&end, D, &k);
    expect("chain: none is the reply's own", way_chain_apply(&chain, &k, D) &&
           way_keep_lifetime(&k) == 600 && !k.no_cache, NULL);

    os64_fetch_keep_t hop = fields("no-store", "", "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a redirect's no-store keeps nothing", !way_chain_apply(&chain, &k, D), NULL);

    way_chain_start(&chain);
    hop = fields("max-age=60", "", "");
    hop.unreadable = true;
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a redirect not read whole keeps nothing", !way_chain_apply(&chain, &k, D), NULL);

    way_chain_start(&chain);
    hop = fields("no-cache", "", "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a redirect's no-cache is the entry's",
           way_chain_apply(&chain, &k, D) && k.no_cache && !way_keep_fresh(&k, D), NULL);

    way_chain_start(&chain);
    hop = fields("max-age=600, must-revalidate", "", "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a redirect's must-revalidate is the entry's",
           way_chain_apply(&chain, &k, D) && !way_keep_stale_ok(&k), NULL);

    // The shorter-lived redirect holds the entry: fresh for its minute.
    way_chain_start(&chain);
    hop = fields("max-age=60", kDate, "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: held to a redirect's minute", way_chain_apply(&chain, &k, D) &&
           way_keep_fresh(&k, D + 59) && !way_keep_fresh(&k, D + 61), NULL);
    // ...counted from when it arrived: a redirect 20 seconds old has 40 left.
    way_chain_start(&chain);
    hop = fields("max-age=60", kDate, "");
    snprintf(hop.age, sizeof(hop.age), "20");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a redirect's own age counts", way_chain_apply(&chain, &k, D) &&
           way_keep_fresh(&k, D + 39) && !way_keep_fresh(&k, D + 41), NULL);
    // A longer-lived one limits nothing.
    way_chain_start(&chain);
    hop = fields("max-age=86400", kDate, "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a longer-lived redirect limits nothing",
           way_chain_apply(&chain, &k, D) && way_keep_lifetime(&k) == 600, NULL);
    // One that says nothing of freshness is kept by heuristic and limits
    // nothing either.
    way_chain_start(&chain);
    hop = fields("", "", "");
    way_chain_hop(&chain, &hop, D);
    (void)way_keep_read(&end, D, &k);
    expect("chain: a silent permanent redirect limits nothing",
           way_chain_apply(&chain, &k, D) && way_keep_lifetime(&k) == 600 && !k.no_cache, NULL);
}

// The bytes of entries in `dir`, as the disk has them.
static uint64_t dir_bytes(const char *dir)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    struct dirent *x;
    while (d != NULL && (x = readdir(d)) != NULL) {
        char path[1200];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, x->d_name);
        if (strlen(x->d_name) == 18 && stat(path, &st) == 0)
            total += (uint64_t)st.st_size;
    }
    if (d != NULL)
        closedir(d);
    return total;
}

// ── The store ───────────────────────────────────────────────────────────

// The name the store gives an address: FNV-1a, as cache.c spells it.
static void entry_path(const char *dir, const char *url, char *out, size_t cap)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *s = url; *s != '\0'; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001b3ull;
    }
    snprintf(out, cap, "%s/%016llx.e", dir, (unsigned long long)h);
}

static way_entry_t entry(const char *url, const char *cc)
{
    way_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.url, sizeof(e.url), "%s", url);
    snprintf(e.final, sizeof(e.final), "%s", url);
    e.status = 200;
    snprintf(e.content_type, sizeof(e.content_type), "image/png");
    os64_fetch_keep_t f = fields(cc, kDate, kModified);
    snprintf(f.etag, sizeof(f.etag), "\"v1\"");
    (void)way_keep_read(&f, s_clock, &e.keep);
    return e;
}

static int count_entries(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *x;
    while (d != NULL && (x = readdir(d)) != NULL)
        n += strlen(x->d_name) == 18 && strcmp(x->d_name + 16, ".e") == 0;
    if (d != NULL)
        closedir(d);
    return n;
}

static void set_mtime(const char *path, time_t when)
{
    struct utimbuf t = {when, when};
    (void)utime(path, &t);
}

static void store_cases(void)
{
    char root[] = "/tmp/way_cacheXXXXXX";
    if (mkdtemp(root) == NULL) {
        expect("store: a temporary directory", false, NULL);
        return;
    }
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/var/cache/yonder", root);
    s_clock = D;
    way_cache_t *c = way_cache_open(dir, 64 * 1024);
    way_cache_stats_t st;
    way_cache_stats(c, &st);
    expect("store: the directory is made, and usable", c != NULL && st.usable && st.enabled &&
           st.entries == 0, NULL);

    const char *url = "https://danlegt.com/static/icons/directory_open_cool-4.png";
    uint8_t body[1000];
    for (size_t i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)(i * 7);
    way_entry_t e = entry(url, "public, max-age=31536000, immutable");
    snprintf(e.final, sizeof(e.final), "https://danlegt.com/moved.png");
    expect("store: an entry is kept", way_cache_put(c, &e, body, sizeof(body)), NULL);
    way_entry_t got;
    uint8_t *bytes = NULL;
    size_t len = 0;
    bool found = way_cache_find(c, url, &got, &bytes, &len, 1 << 20);
    expect("store: and found, whole", found && len == sizeof(body) &&
           memcmp(bytes, body, len) == 0, NULL);
    expect("store: with what came with it", found && got.status == 200 &&
           strcmp(got.final, "https://danlegt.com/moved.png") == 0 &&
           strcmp(got.content_type, "image/png") == 0 && got.keep.max_age == 31536000 &&
           got.keep.date == D && got.keep.last_modified == LM &&
           strcmp(got.keep.etag, "\"v1\"") == 0 && strcmp(got.keep.last_modified_text, kModified) == 0,
           NULL);
    os64_free(bytes);
    expect("store: larger than the caller's cap is a miss",
           !way_cache_find(c, url, &got, &bytes, &len, 999) && bytes == NULL, NULL);
    expect("store: another address is a miss",
           !way_cache_find(c, "https://danlegt.com/", &got, &bytes, &len, 1 << 20), NULL);

    // A collision: the file under another address's name holds this one.
    char from[1200], to[1200];
    entry_path(dir, url, from, sizeof(from));
    entry_path(dir, "https://example.com/x.png", to, sizeof(to));
    rename(from, to);
    expect("store: the address inside is checked",
           !way_cache_find(c, "https://example.com/x.png", &got, &bytes, &len, 1 << 20), NULL);
    rename(to, from);
    // A file cut short, or with more than it says, is no entry.
    expect("store: an entry cut short", truncate(from, 900) == 0, NULL);
    expect("store: a torn entry is a miss", !way_cache_find(c, url, &got, &bytes, &len, 1 << 20),
           NULL);
    FILE *grow = fopen(from, "a");
    fputs("more than it says it is, and then some more", grow);
    fclose(grow);
    expect("store: an entry grown", truncate(from, 2000) == 0, NULL);
    expect("store: a longer entry is a miss", !way_cache_find(c, url, &got, &bytes, &len, 1 << 20),
           NULL);
    expect("store: replaced whole", way_cache_put(c, &e, body, sizeof(body)) &&
           way_cache_find(c, url, &got, &bytes, &len, 1 << 20), NULL);
    os64_free(bytes);

    // Off, it neither finds nor keeps; what it holds stays.
    way_cache_enable(c, false);
    expect("store: turned off, nothing is found", !way_cache_find(c, url, &got, &bytes, &len, 1 << 20),
           NULL);
    expect("store: nor kept", !way_cache_put(c, &e, body, sizeof(body)), NULL);
    way_cache_enable(c, true);
    expect("store: and back", way_cache_find(c, url, &got, &bytes, &len, 1 << 20), NULL);
    os64_free(bytes);

    // Larger than an eighth of the cap is not kept.
    static uint8_t big[9000];
    way_entry_t b = entry("https://x.test/big.png", "max-age=60");
    expect("store: an entry over an eighth of the cap is not kept",
           !way_cache_put(c, &b, big, sizeof(big)), NULL);

    // Full: sixty entries of about 1.4 KB pass a 64 KB cap, and the oldest
    // go, by their files' times.
    static char names[60][1200];
    for (int i = 0; i < 60; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://x.test/%d.png", i);
        way_entry_t x = entry(u, "max-age=60");
        entry_path(dir, u, names[i], sizeof(names[i]));
        (void)way_cache_put(c, &x, body, sizeof(body));
        set_mtime(names[i], (time_t)(D + 10 + i));
    }
    way_cache_stats(c, &st);
    struct stat s0, s59;
    // Each sweep leaves it under nine-tenths, and the puts after it fill it
    // again, so what holds whenever it is looked at is the cap itself.
    expect("store: never over its cap", st.bytes <= 64 * 1024 && st.entries > 0, NULL);
    expect("store: the oldest went, the newest stayed",
           stat(names[0], &s0) != 0 && stat(names[59], &s59) == 0, NULL);

    // A lower cap sweeps at once, to under nine-tenths of it, the oldest
    // first; the newest stays.
    way_cache_set_cap(c, 16 * 1024);
    way_cache_stats(c, &st);
    expect("store: a lower cap sweeps at once", st.cap == 16 * 1024 &&
           st.bytes <= 16 * 1024 / 10 * 9 && st.entries > 0 && stat(names[59], &s59) == 0, NULL);
    way_cache_set_cap(c, 64 * 1024);

    // A temp file a dead writer left is swept; one being written is not.
    char dead[1300], live[1300];
    snprintf(dead, sizeof(dead), "%s/0123456789abcdef.7.1.new", dir);
    snprintf(live, sizeof(live), "%s/0123456789abcdef.7.2.new", dir);
    fclose(fopen(dead, "w"));
    fclose(fopen(live, "w"));
    set_mtime(dead, D - 7200);
    set_mtime(live, D - 60);
    way_cache_stats(c, &st);
    expect("store: an abandoned temp is swept, a live one kept",
           access(dead, F_OK) != 0 && access(live, F_OK) == 0, NULL);
    unlink(live);

    // An entry dropped is gone.
    way_entry_t gone = entry("https://x.test/gone.png", "max-age=60");
    (void)way_cache_put(c, &gone, body, sizeof(body));
    way_cache_drop(c, "https://x.test/gone.png");
    expect("store: a dropped entry is gone",
           !way_cache_find(c, "https://x.test/gone.png", &got, &bytes, &len, 1 << 20), NULL);

    int held = count_entries(dir);
    expect("store: emptied", held > 0 && way_cache_clear(c) == held && count_entries(dir) == 0, NULL);
    way_cache_stats(c, &st);
    expect("store: and says so", st.entries == 0 && st.bytes == 0 && st.stored > 0, NULL);
    way_cache_close(c);

    // Two browsers on one directory: each counts the directory again as it
    // writes, so what both wrote stays under the cap, though neither alone
    // would have reached it (Quinn's case: eight 7000-byte bodies each).
    snprintf(dir, sizeof(dir), "%s/shared", root);
    way_cache_t *one = way_cache_open(dir, 65536), *two = way_cache_open(dir, 65536);
    static uint8_t seven[7000];
    for (int i = 0; i < 16; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://shared.test/%d.png", i);
        way_entry_t x = entry(u, "max-age=60");
        (void)way_cache_put(i % 2 == 0 ? one : two, &x, seven, sizeof(seven));
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "%llu bytes", (unsigned long long)dir_bytes(dir));
    expect("store: two browsers on one directory stay under its cap", dir_bytes(dir) <= 65536,
           detail);
    way_cache_close(one);
    way_cache_close(two);

    // A directory that cannot be made keeps nothing, and says so.
    char blocked[700];
    snprintf(blocked, sizeof(blocked), "%s/file", root);
    fclose(fopen(blocked, "w"));
    snprintf(dir, sizeof(dir), "%s/file/cache", root);
    c = way_cache_open(dir, 64 * 1024);
    way_cache_stats(c, &st);
    expect("store: an unusable directory", c != NULL && !st.usable && !way_cache_enabled(c) &&
           !way_cache_put(c, &e, body, sizeof(body)), NULL);
    way_cache_close(c);

    char rm[700];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0)
        expect("store: the temporary directory is removed", false, root);
}

static void cache_cases(void)
{
    keep_rules();
    chain_rules();
    store_cases();
}
