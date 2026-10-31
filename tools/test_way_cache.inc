// The browser's cache (way/cache.h, CACHE.md): RFC 9111's rules as pure
// functions, and the store against a real temporary directory, reached
// through libos64's file calls over POSIX (test_os64_files.inc). Every
// epoch was worked out with a calendar: 1790758509 is danlegt.com's Date,
// Wed, 30 Sep 2026 08:55:09 GMT; 1790195818 its pictures' Last-Modified,
// Wed, 23 Sep 2026 20:36:58 GMT.

#include <utime.h>

#include "os64/date.h"

#define D 1790758509
#define LM 1790195818

// libos64's file calls over POSIX, shared with the fetch job's suite.
#include "test_os64_files.inc"

static int64_t s_clock = D;

int64_t os64_time(os64_time_t *t)
{
    memset(t, 0, sizeof(*t));
    t->epoch = s_clock;
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
    // An escaped quote inside a quoted value ends nothing: what follows the
    // value is still read.
    f = fields("x=\"a\\\"b\", no-store", "", "");
    expect("keep: an escaped quote does not hide a no-store", !way_keep_read(&f, D, &k), NULL);
    f = fields("x=\"a\\\"b, c\", no-cache", "", "");
    expect("keep: nor a no-cache after a comma inside quotes",
           way_keep_read(&f, D, &k) && k.no_cache, NULL);

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
    // A 304 repeating a Vary other than Accept-Encoding: the varied
    // confirmation keeps what the plain one would let go (Codex #425 rd4).
    f = fields("max-age=60", kDate, "");
    (void)way_keep_read(&f, D, &k);
    n = fields("max-age=600", "", "");
    snprintf(n.vary, sizeof(n.vary), "Origin");
    way_keep_t plain = k;
    expect("keep: a 304 varying on Origin does not confirm a plain entry", !way_keep_confirm(&plain, &n, D + 300),
           NULL);
    expect("keep: but does confirm a varied one", way_keep_confirm_varied(&k, &n, D + 300) && k.max_age == 600, NULL);

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
    // ...and from when it came, not when the chain is laid over the reply:
    // a second-long redirect two seconds before is spent.
    way_chain_start(&chain);
    hop = fields("max-age=1", kDate, "");
    way_chain_hop(&chain, &hop, D);
    end = fields("max-age=600", kDate, "");
    (void)way_keep_read(&end, D + 2, &k);
    expect("chain: a redirect's time runs while the chain is followed",
           way_chain_apply(&chain, &k, D + 2) && !way_keep_fresh(&k, D + 2) && k.max_age >= 0,
           NULL);
    end = fields("max-age=600", kDate, "");
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

// The cases' page: every entry is kept for it, credentialed, unless a case
// says otherwise. KEY spells the key for an address in place.
static const char kPartition[] = "https page.test";
#define KEY(url) (&(way_cache_key_t){kPartition, true, (url)})

// The name the store gives a key: FNV-1a over its parts, as cache.c spells it.
static void key_path(const char *dir, const char *partition, bool credentials, const char *url, char kind,
                     char *out, size_t cap)
{
    uint64_t h = 0xcbf29ce484222325ull;
    char joined[2048];
    snprintf(joined, sizeof(joined), "%s\n%d\n%s", partition, credentials ? 1 : 0, url);
    for (const char *s = joined; *s != '\0'; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001b3ull;
    }
    snprintf(out, cap, "%s/%016llx.%c", dir, (unsigned long long)h, kind);
}

static void entry_path(const char *dir, const char *url, char *out, size_t cap)
{
    key_path(dir, kPartition, true, url, 'e', out, cap);
}

static way_entry_t entry(const char *url, const char *cc)
{
    way_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.partition, sizeof(e.partition), "%s", kPartition);
    e.credentials = true;
    snprintf(e.url, sizeof(e.url), "%s", url);
    snprintf(e.final, sizeof(e.final), "%s", url);
    e.status = 200;
    snprintf(e.content_type, sizeof(e.content_type), "image/png");
    os64_fetch_keep_t f = fields(cc, kDate, kModified);
    snprintf(f.etag, sizeof(f.etag), "\"v1\"");
    (void)way_keep_read(&f, s_clock, &e.keep);
    return e;
}

// Twelve puts into the cache a sweep is running for, from the listing's
// end (s_after_listing).
static way_cache_t *s_during_cache;
static void puts_during_sweep(void)
{
    static uint8_t body[7000];
    for (int i = 0; i < 12; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://during.test/b%d.png", i);
        way_entry_t x = entry(u, "max-age=60");
        (void)way_cache_put(s_during_cache, &x, NULL, body, sizeof(body));
    }
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
    expect("store: an entry is kept", way_cache_put(c, &e, NULL, body, sizeof(body)), NULL);
    way_entry_t got;
    uint8_t *bytes = NULL;
    size_t len = 0;
    bool found = way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20);
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
           !way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 999) && bytes == NULL, NULL);
    expect("store: another address is a miss",
           !way_cache_find(c, KEY("https://danlegt.com/"), &got, &bytes, &len, NULL, 1 << 20), NULL);

    // A collision: the file under another address's name holds this one.
    char from[1200], to[1200];
    entry_path(dir, url, from, sizeof(from));
    entry_path(dir, "https://example.com/x.png", to, sizeof(to));
    rename(from, to);
    expect("store: the address inside is checked",
           !way_cache_find(c, KEY("https://example.com/x.png"), &got, &bytes, &len, NULL, 1 << 20), NULL);
    rename(to, from);
    // A file cut short, or with more than it says, is no entry.
    expect("store: an entry cut short", truncate(from, 900) == 0, NULL);
    expect("store: a torn entry is a miss", !way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20),
           NULL);
    FILE *grow = fopen(from, "a");
    fputs("more than it says it is, and then some more", grow);
    fclose(grow);
    expect("store: an entry grown", truncate(from, 2000) == 0, NULL);
    expect("store: a longer entry is a miss", !way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20),
           NULL);
    expect("store: replaced whole", way_cache_put(c, &e, NULL, body, sizeof(body)) &&
           way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20), NULL);
    os64_free(bytes);

    // Off, it neither finds nor keeps; what it holds stays.
    way_cache_enable(c, false);
    expect("store: turned off, nothing is found", !way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20),
           NULL);
    expect("store: nor kept", !way_cache_put(c, &e, NULL, body, sizeof(body)), NULL);
    way_cache_enable(c, true);
    expect("store: and back", way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20), NULL);
    os64_free(bytes);

    // Larger than an eighth of the cap is not kept.
    static uint8_t big[9000];
    way_entry_t b = entry("https://x.test/big.png", "max-age=60");
    expect("store: an entry over an eighth of the cap is not kept",
           !way_cache_put(c, &b, NULL, big, sizeof(big)), NULL);

    // Full: sixty entries of about 1.4 KB pass a 64 KB cap, and the oldest
    // go, by their files' times.
    static char names[60][1200];
    for (int i = 0; i < 60; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://x.test/%d.png", i);
        way_entry_t x = entry(u, "max-age=60");
        entry_path(dir, u, names[i], sizeof(names[i]));
        (void)way_cache_put(c, &x, NULL, body, sizeof(body));
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
    (void)way_cache_put(c, &gone, NULL, body, sizeof(body));
    way_cache_drop(c, KEY("https://x.test/gone.png"));
    expect("store: a dropped entry is gone",
           !way_cache_find(c, KEY("https://x.test/gone.png"), &got, &bytes, &len, NULL, 1 << 20), NULL);

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
        (void)way_cache_put(i % 2 == 0 ? one : two, &x, NULL, seven, sizeof(seven));
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "%llu bytes", (unsigned long long)dir_bytes(dir));
    expect("store: two browsers on one directory stay under its cap", dir_bytes(dir) <= 65536,
           detail);
    way_cache_close(one);
    way_cache_close(two);

    // Writes that finish while a sweep is running: the sweeper, not they,
    // sweeps again for them before it lets the sweep go (Quinn's
    // interleaving: twelve 7000-byte puts between a sweep's count of the
    // directory and its last word). Measured on the disk, since stats
    // would sweep.
    snprintf(dir, sizeof(dir), "%s/during", root);
    s_during_cache = way_cache_open(dir, 65536);
    for (int i = 0; i < 8; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://during.test/a%d.png", i);
        way_entry_t x = entry(u, "max-age=60");
        (void)way_cache_put(s_during_cache, &x, NULL, seven, sizeof(seven));
    }
    s_after_listing = puts_during_sweep;
    way_entry_t last = entry("https://during.test/last.png", "max-age=60");
    (void)way_cache_put(s_during_cache, &last, NULL, seven, sizeof(seven));
    snprintf(detail, sizeof(detail), "%llu bytes", (unsigned long long)dir_bytes(dir));
    expect("store: what was written during a sweep is swept for", s_after_listing == NULL &&
           dir_bytes(dir) <= 65536, detail);
    way_cache_close(s_during_cache);

    // A directory that cannot be made keeps nothing, and says so.
    char blocked[700];
    snprintf(blocked, sizeof(blocked), "%s/file", root);
    fclose(fopen(blocked, "w"));
    snprintf(dir, sizeof(dir), "%s/file/cache", root);
    c = way_cache_open(dir, 64 * 1024);
    way_cache_stats(c, &st);
    expect("store: an unusable directory", c != NULL && !st.usable && !way_cache_enabled(c) &&
           !way_cache_put(c, &e, NULL, body, sizeof(body)), NULL);
    way_cache_close(c);

    char rm[700];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0)
        expect("store: the temporary directory is removed", false, root);
}

// A companion's name: its entry's, ending `.b`.
static void companion_path(const char *dir, const char *url, char *out, size_t cap)
{
    key_path(dir, kPartition, true, url, 'b', out, cap);
}

// BYTECODE_CACHE.md § 5: what a face keeps beside an entry.
static void companion_cases(void)
{
    char root[] = "/tmp/way_companionXXXXXX";
    if (mkdtemp(root) == NULL) {
        expect("companion: a temporary directory", false, NULL);
        return;
    }
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/cache", root);
    s_clock = D;
    way_cache_t *c = way_cache_open(dir, 64 * 1024);
    const char *url = "https://x.test/app.js";
    const char *tag = "classic 0123456789abcdef 4096";
    uint8_t blob[3000];
    for (size_t i = 0; i < sizeof(blob); i++)
        blob[i] = (uint8_t)(i * 13);
    uint8_t *bytes = NULL;
    size_t len = 0;
    expect("companion: kept", way_cache_companion_put(c, KEY(url), tag, blob, sizeof(blob)), NULL);
    bool found = way_cache_companion_get(c, KEY(url), tag, &bytes, &len, 1 << 20);
    expect("companion: and read back whole", found && len == sizeof(blob) &&
           memcmp(bytes, blob, len) == 0, NULL);
    os64_free(bytes);
    expect("companion: another tag is a miss",
           !way_cache_companion_get(c, KEY(url), "classic 0123456789abcdef 4097", &bytes, &len, 1 << 20) &&
           bytes == NULL, NULL);
    expect("companion: another address is a miss",
           !way_cache_companion_get(c, KEY("https://x.test/other.js"), tag, &bytes, &len, 1 << 20), NULL);
    expect("companion: larger than the caller's cap is a miss",
           !way_cache_companion_get(c, KEY(url), tag, &bytes, &len, sizeof(blob) - 1), NULL);
    int32_t entries_before = 0;
    way_cache_stats_t st;
    way_cache_stats(c, &st);
    entries_before = st.entries;
    expect("companion: counted in the bytes, not as an entry", entries_before == 0 &&
           st.bytes > sizeof(blob), NULL);

    // The address inside is checked; a file cut short or grown is a miss.
    char from[1200], to[1200];
    companion_path(dir, url, from, sizeof(from));
    companion_path(dir, "https://x.test/collides.js", to, sizeof(to));
    rename(from, to);
    expect("companion: the address inside is checked",
           !way_cache_companion_get(c, KEY("https://x.test/collides.js"), tag, &bytes, &len, 1 << 20), NULL);
    rename(to, from);
    struct stat whole;
    stat(from, &whole);
    expect("companion: cut short", truncate(from, whole.st_size - 1) == 0, NULL);
    expect("companion: a torn companion is a miss",
           !way_cache_companion_get(c, KEY(url), tag, &bytes, &len, 1 << 20), NULL);
    expect("companion: grown", truncate(from, whole.st_size + 1) == 0, NULL);
    expect("companion: a longer companion is a miss",
           !way_cache_companion_get(c, KEY(url), tag, &bytes, &len, 1 << 20), NULL);

    // A tag is one line, not empty and not too long; an eighth of the cap.
    char long_tag[WAY_COMPANION_TAG_MAX + 1];
    memset(long_tag, 'k', sizeof(long_tag) - 1);
    long_tag[sizeof(long_tag) - 1] = '\0';
    expect("companion: a tag with a line break is refused",
           !way_cache_companion_put(c, KEY(url), "classic\nlength 1", blob, sizeof(blob)), NULL);
    expect("companion: an empty tag is refused", !way_cache_companion_put(c, KEY(url), "", blob, sizeof(blob)),
           NULL);
    expect("companion: a tag too long is refused",
           !way_cache_companion_put(c, KEY(url), long_tag, blob, sizeof(blob)), NULL);
    static uint8_t big[9000];
    expect("companion: over an eighth of the cap is not kept",
           !way_cache_companion_put(c, KEY(url), tag, big, sizeof(big)), NULL);
    way_cache_enable(c, false);
    expect("companion: turned off, nothing is kept or read",
           !way_cache_companion_put(c, KEY(url), tag, blob, sizeof(blob)) &&
           !way_cache_companion_get(c, KEY(url), tag, &bytes, &len, 1 << 20), NULL);
    way_cache_enable(c, true);

    // It goes with its entry: dropped, and cleared.
    uint8_t body[1000] = {0};
    way_entry_t e = entry(url, "max-age=60");
    (void)way_cache_put(c, &e, NULL, body, sizeof(body));
    (void)way_cache_companion_put(c, KEY(url), tag, blob, sizeof(blob));
    way_cache_drop(c, KEY(url));
    expect("companion: dropped with its entry", access(from, F_OK) != 0 &&
           !way_cache_companion_get(c, KEY(url), tag, &bytes, &len, 1 << 20), NULL);
    (void)way_cache_put(c, &e, NULL, body, sizeof(body));
    (void)way_cache_companion_put(c, KEY(url), tag, blob, sizeof(blob));
    expect("companion: cleared with the entries, which alone are counted",
           way_cache_clear(c) == 1 && access(from, F_OK) != 0 && count_entries(dir) == 0, NULL);

    // The sweep takes companions by the same oldest-first rule.
    static char names[30][1200];
    for (int i = 0; i < 30; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://x.test/%d.js", i);
        companion_path(dir, u, names[i], sizeof(names[i]));
        (void)way_cache_companion_put(c, KEY(u), tag, blob, sizeof(blob));
        set_mtime(names[i], (time_t)(D + 10 + i));
    }
    way_cache_stats(c, &st);
    struct stat s0, s29;
    expect("companion: swept under the cap, the oldest first", st.bytes <= 64 * 1024 &&
           stat(names[0], &s0) != 0 && stat(names[29], &s29) == 0, NULL);
    way_cache_close(c);

    char rm[700];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0)
        expect("companion: the temporary directory is removed", false, root);
}

// SCRIPT_CACHE.md § 2-4: what is kept is found by its whole key, and keeps
// what the live CORS check and Vary read.
static void key_cases(void)
{
    char root[] = "/tmp/way_keysXXXXXX";
    if (mkdtemp(root) == NULL) {
        expect("keys: a temporary directory", false, NULL);
        return;
    }
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/cache", root);
    s_clock = D;
    way_cache_t *c = way_cache_open(dir, 1 << 20);
    const char *url = "https://cdn.test/app.js";
    uint8_t body[100];
    memset(body, 'b', sizeof(body));
    way_entry_t e = entry(url, "max-age=600");
    way_entry_t got;
    uint8_t *bytes = NULL;
    size_t len = 0;
    expect("keys: kept for one site's page", way_cache_put(c, &e, NULL, body, sizeof(body)), NULL);
    expect("keys: found by that site's page", way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20),
           NULL);
    os64_free(bytes);
    expect("keys: another site's page does not find it",
           !way_cache_find(c, &(way_cache_key_t){"https other.test", true, url}, &got, &bytes, &len, NULL, 1 << 20),
           NULL);
    expect("keys: an anonymous request does not find a credentialed reply",
           !way_cache_find(c, &(way_cache_key_t){kPartition, false, url}, &got, &bytes, &len, NULL, 1 << 20), NULL);
    way_entry_t anon = e;
    anon.credentials = false;
    uint8_t other[3] = {'a', 'n', 'o'};
    expect("keys: an anonymous reply is kept beside it, not over it",
           way_cache_put(c, &anon, NULL, other, sizeof(other)) &&
               way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20) && len == sizeof(body), NULL);
    os64_free(bytes);
    expect("keys: and a credentialed request does not find it",
           way_cache_find(c, &(way_cache_key_t){kPartition, false, url}, &got, &bytes, &len, NULL, 1 << 20) &&
               len == sizeof(other) && !got.credentials, NULL);
    os64_free(bytes);
    way_entry_t theirs = e;
    snprintf(theirs.partition, sizeof(theirs.partition), "https other.test");
    expect("keys: another site's reply is kept beside it, not over it",
           way_cache_put(c, &theirs, NULL, other, sizeof(other)) &&
               way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20) && len == sizeof(body), NULL);
    os64_free(bytes);

    // A page of an opaque origin, or a caller that names no page, keeps nothing.
    way_entry_t opaque = e;
    snprintf(opaque.partition, sizeof(opaque.partition), "opaque 7");
    way_entry_t none = e;
    none.partition[0] = '\0';
    expect("keys: an opaque page keeps nothing", !way_cache_put(c, &opaque, NULL, body, sizeof(body)), NULL);
    expect("keys: nor one not named", !way_cache_put(c, &none, NULL, body, sizeof(body)), NULL);
    expect("keys: nor is anything found for them",
           !way_cache_find(c, &(way_cache_key_t){"opaque 7", true, url}, &got, &bytes, &len, NULL, 1 << 20) &&
               !way_cache_find(c, &(way_cache_key_t){"", true, url}, &got, &bytes, &len, NULL, 1 << 20), NULL);

    // The blocks come back as they were kept, after the body.
    const char headers[] = "Content-Type: text/javascript\r\nAccess-Control-Allow-Origin: *\r\nVary: Origin\r\n";
    const char omitted[] = "x-long\r\n";
    const char vary[] = "Origin: https://page.test\r\n";
    way_blocks_t kept = {headers, sizeof(headers) - 1, omitted, sizeof(omitted) - 1, vary, sizeof(vary) - 1};
    way_blocks_t back;
    expect("keys: an entry with its blocks is kept", way_cache_put(c, &e, &kept, body, sizeof(body)), NULL);
    bool found = way_cache_find(c, KEY(url), &got, &bytes, &len, &back, 1 << 20);
    expect("keys: and its body and blocks come back whole",
           found && len == sizeof(body) && memcmp(bytes, body, len) == 0 &&
               back.headers_len == sizeof(headers) - 1 && memcmp(back.headers, headers, back.headers_len) == 0 &&
               back.omitted_len == sizeof(omitted) - 1 && memcmp(back.omitted, omitted, back.omitted_len) == 0 &&
               back.vary_len == sizeof(vary) - 1 && memcmp(back.vary, vary, back.vary_len) == 0, NULL);
    os64_free(bytes);
    expect("keys: the body's cap is the body's, the blocks beside it",
           !way_cache_find(c, KEY(url), &got, &bytes, &len, &back, sizeof(body) - 1) &&
               way_cache_find(c, KEY(url), &got, &bytes, &len, &back, sizeof(body)), NULL);
    os64_free(bytes);
    static char huge[(256 << 10) + 1];
    way_blocks_t too_big = {huge, sizeof(huge), NULL, 0, NULL, 0};
    expect("keys: blocks past what a response's head can be are not kept",
           !way_cache_put(c, &e, &too_big, body, sizeof(body)), NULL);

    // A version 1 entry, under the name version 2 would give it, is a miss.
    char path[1200];
    entry_path(dir, url, path, sizeof(path));
    FILE *f = fopen(path, "w");
    fprintf(f, "os64-cache 1\nurl %s\nfrom %s\nstatus 200\nstored %ld\nlength 3\n\nold", url, url, (long)D);
    fclose(f);
    expect("keys: a version 1 entry is a miss", !way_cache_find(c, KEY(url), &got, &bytes, &len, NULL, 1 << 20),
           NULL);

    // A companion is its entry's key's.
    const char *tag = "classic 00 1";
    uint8_t blob[10] = {0};
    expect("keys: a companion is kept beside its entry",
           way_cache_companion_put(c, KEY(url), tag, blob, sizeof(blob)), NULL);
    expect("keys: and another site's page does not find it",
           !way_cache_companion_get(c, &(way_cache_key_t){"https other.test", true, url}, tag, &bytes, &len,
                                    1 << 20), NULL);
    expect("keys: nor an anonymous request",
           !way_cache_companion_get(c, &(way_cache_key_t){kPartition, false, url}, tag, &bytes, &len, 1 << 20),
           NULL);
    expect("keys: an opaque page keeps no companion",
           !way_cache_companion_put(c, &(way_cache_key_t){"opaque 7", true, url}, tag, blob, sizeof(blob)), NULL);
    way_cache_close(c);

    char rm[700];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0)
        expect("keys: the temporary directory is removed", false, root);
}

// A request that sends Origin and Accept, says nothing of X-Absent, and
// cannot say what it would send for X-Unknown.
// X-Absent is not sent, unless the case says it is, sent empty.
static bool s_absent_sent;
static way_said_t request_value(void *ctx, const char *name, size_t nlen, char *out, size_t cap)
{
    const char *origin = ctx;
    if (nlen == 6 && strncasecmp(name, "origin", 6) == 0) {
        snprintf(out, cap, "%s", origin);
        return WAY_SAID_VALUE;
    }
    if (nlen == 6 && strncasecmp(name, "accept", 6) == 0) {
        snprintf(out, cap, "*/*");
        return WAY_SAID_VALUE;
    }
    if (nlen == 8 && strncasecmp(name, "x-absent", 8) == 0) {
        snprintf(out, cap, "%s", "");
        return s_absent_sent ? WAY_SAID_VALUE : WAY_SAID_NONE;
    }
    return WAY_SAID_UNKNOWN;
}

// SCRIPT_CACHE.md § 4, § 6: Vary recorded and held to, the cookies never
// kept, a 304 laid over what was kept.
static void reply_rules(void)
{
    char lines[512];
    size_t n = way_vary_lines("Origin, accept-encoding, Accept, X-Absent", request_value, "https://a.test", lines,
                              sizeof(lines));
    expect("vary: what the request said of each header, the encoding left out, one not sent marked so",
           n != SIZE_MAX && n == strlen("Origin: =https://a.test\r\nAccept: =*/*\r\nX-Absent: -\r\n") &&
               memcmp(lines, "Origin: =https://a.test\r\nAccept: =*/*\r\nX-Absent: -\r\n", n) == 0, lines);
    expect("vary: the same request matches", way_vary_matches(lines, n, request_value, "https://a.test"), NULL);
    s_absent_sent = true;
    expect("vary: one sending empty what the other did not send does not (Codex #425 rd3)",
           !way_vary_matches(lines, n, request_value, "https://a.test"), NULL);
    s_absent_sent = false;
    expect("vary: another origin does not", !way_vary_matches(lines, n, request_value, "https://b.test"), NULL);
    expect("vary: a header nobody can say keeps nothing",
           way_vary_lines("X-Unknown", request_value, "https://a.test", lines, sizeof(lines)) == SIZE_MAX, NULL);
    expect("vary: nor does *", way_vary_lines("Origin, *", request_value, "https://a.test", lines, sizeof(lines)) ==
                                   SIZE_MAX, NULL);
    expect("vary: nor lines that do not fit",
           way_vary_lines("Origin", request_value, "https://a.test", lines, 10) == SIZE_MAX, NULL);
    expect("vary: nothing varied on matches anything", way_vary_matches("", 0, request_value, "https://z.test"),
           NULL);

    os64_fetch_keep_t f;
    memset(&f, 0, sizeof(f));
    snprintf(f.cache_control, sizeof(f.cache_control), "max-age=60");
    snprintf(f.vary, sizeof(f.vary), "Origin");
    way_keep_t k;
    expect("vary: way_keep_read keeps no reply that varies on Origin", !way_keep_read(&f, D, &k), NULL);
    expect("vary: way_keep_read_varied does, the caller recording it", way_keep_read_varied(&f, D, &k), NULL);
    snprintf(f.vary, sizeof(f.vary), "Origin, *");
    expect("vary: but never one that varies on *", !way_keep_read_varied(&f, D, &k), NULL);

    const char got[] = "Content-Type: text/javascript\r\nSet-Cookie: sid=1\r\nETag: \"v1\"\r\nset-cookie2: x=2\r\n";
    char kept[256];
    n = way_headers_storable(got, sizeof(got) - 1, kept, sizeof(kept));
    expect("storable: the cookies are never kept",
           n != SIZE_MAX && n == strlen("Content-Type: text/javascript\r\nETag: \"v1\"\r\n") &&
               memcmp(kept, "Content-Type: text/javascript\r\nETag: \"v1\"\r\n", n) == 0, NULL);
    expect("storable: a block that does not fit is SIZE_MAX",
           way_headers_storable(got, sizeof(got) - 1, kept, 8) == SIZE_MAX, NULL);

    const char stored[] = "Content-Type: text/javascript\r\nContent-Length: 1200\r\nETag: \"v1\"\r\n"
                          "Access-Control-Allow-Origin: https://a.test\r\nCache-Control: max-age=60\r\n";
    const char three04[] = "ETag: \"v1\"\r\nCache-Control: max-age=600\r\nContent-Length: 0\r\n"
                           "Access-Control-Allow-Origin: *\r\nSet-Cookie: sid=2\r\n";
    char merged[512];
    n = way_headers_update(stored, sizeof(stored) - 1, three04, sizeof(three04) - 1, merged, sizeof(merged));
    const char want[] = "Content-Type: text/javascript\r\nContent-Length: 1200\r\nETag: \"v1\"\r\n"
                        "Cache-Control: max-age=600\r\nAccess-Control-Allow-Origin: *\r\n";
    expect("update: the 304's headers replace the kept ones; the body's length and the cookies do not",
           n != SIZE_MAX && n == sizeof(want) - 1 && memcmp(merged, want, n) == 0, merged);
}

static void cache_cases(void)
{
    keep_rules();
    chain_rules();
    reply_rules();
    store_cases();
    companion_cases();
    key_cases();
}
