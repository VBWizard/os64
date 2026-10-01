// cache.c — the browser's cache (way/cache.h, CACHE.md): RFC 9111's rules
// as pure functions, and the store, a directory of one file per reply.

#include "internal.h"
#include "way/cache.h"
#include "way/jar.h"
#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

// ── Reading what a reply said ───────────────────────────────────────────

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool same_nocase(const char *a, size_t n, const char *b)
{
    size_t m = os64_strlen(b);
    if (n != m)
        return false;
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != b[i])
            return false;
    return true;
}

static bool space(char c)
{
    return c == ' ' || c == '\t';
}

// The next comma-separated member of a list field, trimmed: its name up to
// an `=`, and its value after one (quotes taken off). False at the end.
typedef struct {
    const char *s;
    size_t at, len;
} Members;

static bool member(Members *m, const char **name, size_t *nlen, const char **value, size_t *vlen)
{
    while (m->at < m->len && (space(m->s[m->at]) || m->s[m->at] == ','))
        m->at++;
    if (m->at >= m->len)
        return false;
    size_t start = m->at;
    bool quoted = false;
    while (m->at < m->len && (quoted || m->s[m->at] != ',')) {
        if (m->s[m->at] == '"')
            quoted = !quoted;
        m->at++;
    }
    size_t end = m->at;
    while (end > start && space(m->s[end - 1]))
        end--;
    const char *p = m->s + start;
    size_t n = end - start, eq = 0;
    while (eq < n && p[eq] != '=')
        eq++;
    size_t name_end = eq;
    while (name_end > 0 && space(p[name_end - 1]))
        name_end--;
    *name = p;
    *nlen = name_end;
    *value = "";
    *vlen = 0;
    if (eq < n) {
        size_t v = eq + 1;
        while (v < n && space(p[v]))
            v++;
        const char *vp = p + v;
        size_t vn = n - v;
        if (vn >= 2 && vp[0] == '"' && vp[vn - 1] == '"') {
            vp++;
            vn -= 2;
        }
        *value = vp;
        *vlen = vn;
    }
    return true;
}

// A delta-seconds value (§ 1.2.2): digits only, held rather than wrapped.
// -1 for anything else.
static int64_t delta_seconds(const char *s, size_t n)
{
    if (n == 0)
        return -1;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v > INT32_MAX ? v : v * 10 + (s[i] - '0');
    }
    return v > INT32_MAX ? INT32_MAX : v;
}

static int64_t http_date(const char *text)
{
    int64_t when;
    size_t n = os64_strlen(text);
    return n != 0 && way_cookie_date(text, n, &when) ? when : -1;
}

bool way_keep_read(const os64_fetch_keep_t *f, int64_t now, way_keep_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->stored = now;
    out->max_age = -1;
    if (f->unreadable)
        return false;
    bool no_store = false;
    Members m = {f->cache_control, 0, os64_strlen(f->cache_control)};
    const char *name, *value;
    size_t nlen, vlen;
    while (member(&m, &name, &nlen, &value, &vlen)) {
        if (same_nocase(name, nlen, "no-store")) {
            no_store = true;
        } else if (same_nocase(name, nlen, "no-cache")) {
            out->no_cache = true;
        } else if (same_nocase(name, nlen, "must-revalidate")) {
            out->must_revalidate = true;
        } else if (same_nocase(name, nlen, "max-age")) {
            // A max-age that does not read is stale at once (§ 4.2.1), and
            // of two, the shorter.
            int64_t v = delta_seconds(value, vlen);
            v = v < 0 ? 0 : v;
            out->max_age = out->max_age < 0 || v < out->max_age ? v : out->max_age;
        }
    }
    if (f->cache_control[0] == '\0') {
        Members p = {f->pragma, 0, os64_strlen(f->pragma)};
        while (member(&p, &name, &nlen, &value, &vlen))
            if (same_nocase(name, nlen, "no-cache"))
                out->no_cache = true;
    }
    // A reply that varies by what the request said is kept only when what
    // it varies by is the encoding, which libfetch asks for the same way
    // every time.
    Members v = {f->vary, 0, os64_strlen(f->vary)};
    while (member(&v, &name, &nlen, &value, &vlen))
        if (!same_nocase(name, nlen, "accept-encoding"))
            return false;
    out->date = http_date(f->date);
    // An Expires that is not a date is a time already past (§ 5.3).
    out->expires = f->expires[0] == '\0' ? -1 : http_date(f->expires);
    if (f->expires[0] != '\0' && out->expires < 0)
        out->expires = 0;
    out->last_modified = http_date(f->last_modified);
    int64_t age = delta_seconds(f->age, os64_strlen(f->age));
    out->age = age < 0 ? 0 : age;
    os64_strcopy(out->etag, sizeof(out->etag), f->etag);
    os64_strcopy(out->last_modified_text, sizeof(out->last_modified_text), f->last_modified);
    return !no_store;
}

int64_t way_keep_lifetime(const way_keep_t *k)
{
    if (k->max_age >= 0)
        return k->max_age;
    // Expires is measured from the reply's own Date, which a clock here
    // cannot skew; with no Date, from when it arrived.
    int64_t from = k->date >= 0 ? k->date : k->stored;
    if (k->expires >= 0)
        return k->expires > from ? k->expires - from : 0;
    if (k->last_modified >= 0 && from > k->last_modified) {
        int64_t tenth = (from - k->last_modified) / 10;
        return tenth > WAY_KEEP_HEURISTIC_MAX ? WAY_KEEP_HEURISTIC_MAX : tenth;
    }
    return 0;
}

int64_t way_keep_age(const way_keep_t *k, int64_t now)
{
    // How old it was when it arrived — what its Age says, or how late its
    // Date was against this clock, the larger — and how long it has been
    // kept since. A clock that went back keeps nothing younger.
    int64_t apparent = k->date >= 0 && k->stored > k->date ? k->stored - k->date : 0;
    int64_t initial = apparent > k->age ? apparent : k->age;
    int64_t resident = now > k->stored ? now - k->stored : 0;
    return initial + resident;
}

bool way_keep_fresh(const way_keep_t *k, int64_t now)
{
    return !k->no_cache && way_keep_lifetime(k) > way_keep_age(k, now);
}

size_t way_keep_conditions(const way_keep_t *k, char *out, size_t cap)
{
    int n = 0;
    if (k->etag[0] != '\0')
        n = os64_snprintf(out, cap, "If-None-Match: %s\r\n", k->etag);
    else if (k->last_modified_text[0] != '\0')
        n = os64_snprintf(out, cap, "If-Modified-Since: %s\r\n", k->last_modified_text);
    if (n <= 0 || (size_t)n >= cap) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool way_keep_confirm(way_keep_t *k, const os64_fetch_keep_t *fields, int64_t now)
{
    way_keep_t fresh;
    if (!way_keep_read(fields, now, &fresh))
        return false;
    // A validator the 304 did not repeat is still the one it confirmed.
    if (fresh.etag[0] == '\0')
        os64_strcopy(fresh.etag, sizeof(fresh.etag), k->etag);
    if (fresh.last_modified_text[0] == '\0') {
        os64_strcopy(fresh.last_modified_text, sizeof(fresh.last_modified_text),
                     k->last_modified_text);
        fresh.last_modified = k->last_modified;
    }
    // Neither does a 304 that says nothing about freshness take away what
    // the stored reply said (§ 3.2: it updates the fields it sends).
    if (fields->cache_control[0] == '\0' && fields->pragma[0] == '\0') {
        fresh.max_age = k->max_age;
        fresh.no_cache = k->no_cache;
        fresh.must_revalidate = k->must_revalidate;
    }
    if (fields->expires[0] == '\0')
        fresh.expires = k->expires;
    *k = fresh;
    return true;
}

bool way_keep_stale_ok(const way_keep_t *k)
{
    return !k->no_cache && !k->must_revalidate;
}

void way_chain_start(way_chain_t *chain)
{
    os64_memset(chain, 0, sizeof(*chain));
    chain->left = -1;
}

void way_chain_hop(way_chain_t *chain, const os64_fetch_keep_t *fields, int64_t now)
{
    way_keep_t k;
    if (!way_keep_read(fields, now, &k)) {
        chain->no_store = true;
        return;
    }
    chain->no_cache |= k.no_cache;
    chain->must_revalidate |= k.must_revalidate;
    if (k.max_age < 0 && k.expires < 0)
        return;
    int64_t left = way_keep_lifetime(&k) - way_keep_age(&k, now);
    left = left < 0 ? 0 : left;
    chain->left = chain->left < 0 || left < chain->left ? left : chain->left;
}

bool way_chain_apply(const way_chain_t *chain, way_keep_t *k, int64_t now)
{
    if (chain->no_store)
        return false;
    k->no_cache |= chain->no_cache;
    k->must_revalidate |= chain->must_revalidate;
    // Fresh no longer than the chain is: a max-age that ends `left` seconds
    // from now, on the reply's own clock.
    if (chain->left >= 0) {
        int64_t end = way_keep_age(k, now) + chain->left;
        if (end < way_keep_lifetime(k))
            k->max_age = end;
    }
    return true;
}

// ── An entry on disk ────────────────────────────────────────────────────
//
// A text head, `key value` a line, then a blank line and the body. Every
// value is one line: addresses and the fields are HTTP's, which carry no
// line breaks, and a head that does not read whole is a miss.

#define ENTRY_MAGIC "os64-cache 1"
#define ENTRY_HEAD_MAX (4 * OS64_FETCH_URL_MAX + 8 * OS64_FETCH_KEEP_FIELD)
// A temp file older than this was left by a writer that died: removed when
// the directory is next swept.
#define TEMP_ABANDONED (60 * 60)

static size_t entry_head(const way_entry_t *e, size_t len, char *out, size_t cap)
{
    const way_keep_t *k = &e->keep;
    int n = os64_snprintf(out, cap,
                          ENTRY_MAGIC "\nurl %s\nfrom %s\nstatus %d\ntype %s\ncharset %s\n"
                          "stored %ld\ndate %ld\nexpires %ld\nmodified %ld\nage %ld\n"
                          "max-age %ld\nno-cache %d\nmust-revalidate %d\netag %s\n"
                          "last-modified %s\nlength %lu\n\n",
                          e->url, e->final, (int)e->status, e->content_type, e->charset,
                          (long)k->stored, (long)k->date, (long)k->expires,
                          (long)k->last_modified, (long)k->age, (long)k->max_age,
                          k->no_cache ? 1 : 0, k->must_revalidate ? 1 : 0, k->etag,
                          k->last_modified_text, (unsigned long)len);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static char *find_char(char *s, char c)
{
    for (; *s != '\0'; s++)
        if (*s == c)
            return s;
    return NULL;
}

static bool number(const char *v, int64_t *out)
{
    bool neg = v[0] == '-';
    uint64_t u;
    if (!os64_parse_u64(v + (neg ? 1 : 0), &u) || u > INT64_MAX)
        return false;
    *out = neg ? -(int64_t)u : (int64_t)u;
    return true;
}

// Reads a head (NUL-terminated, the blank line gone) into `e`. The body's
// length in `*len`. False for anything that is not a whole head.
static bool entry_parse(char *head, way_entry_t *e, uint64_t *len)
{
    os64_memset(e, 0, sizeof(*e));
    char *line = head;
    char *nl = find_char(line, '\n');
    if (nl == NULL)
        return false;
    *nl = '\0';
    if (!os64_streq(line, ENTRY_MAGIC))
        return false;
    int have = 0;
    for (line = nl + 1; *line != '\0'; line = nl + 1) {
        nl = find_char(line, '\n');
        if (nl == NULL)
            return false;
        *nl = '\0';
        char *sp = find_char(line, ' ');
        const char *v = sp != NULL ? sp + 1 : "";
        if (sp != NULL)
            *sp = '\0';
        way_keep_t *k = &e->keep;
        int64_t x = 0;
        bool is_num = number(v, &x);
        if (os64_streq(line, "url")) {
            have |= os64_strcopy(e->url, sizeof(e->url), v) < sizeof(e->url) ? 1 : 0;
        } else if (os64_streq(line, "from")) {
            have |= os64_strcopy(e->final, sizeof(e->final), v) < sizeof(e->final) ? 2 : 0;
        } else if (os64_streq(line, "status") && is_num) {
            e->status = (int32_t)x;
            have |= 4;
        } else if (os64_streq(line, "type")) {
            os64_strcopy(e->content_type, sizeof(e->content_type), v);
        } else if (os64_streq(line, "charset")) {
            os64_strcopy(e->charset, sizeof(e->charset), v);
        } else if (os64_streq(line, "stored") && is_num) {
            k->stored = x;
            have |= 8;
        } else if (os64_streq(line, "date") && is_num) {
            k->date = x;
        } else if (os64_streq(line, "expires") && is_num) {
            k->expires = x;
        } else if (os64_streq(line, "modified") && is_num) {
            k->last_modified = x;
        } else if (os64_streq(line, "age") && is_num) {
            k->age = x;
        } else if (os64_streq(line, "max-age") && is_num) {
            k->max_age = x;
        } else if (os64_streq(line, "no-cache")) {
            k->no_cache = x != 0;
        } else if (os64_streq(line, "must-revalidate")) {
            k->must_revalidate = x != 0;
        } else if (os64_streq(line, "etag")) {
            os64_strcopy(k->etag, sizeof(k->etag), v);
        } else if (os64_streq(line, "last-modified")) {
            os64_strcopy(k->last_modified_text, sizeof(k->last_modified_text), v);
        } else if (os64_streq(line, "length") && is_num && x >= 0) {
            *len = (uint64_t)x;
            have |= 16;
        }
    }
    return have == 31;
}

// ── The store ───────────────────────────────────────────────────────────

struct way_cache {
    char dir[OS64_PATH_MAX];
    uint64_t cap;               // way_cache_set_cap may change it under a fetch: cap_of reads it
    bool usable, enabled;
    os64_lock_t lock;
    // Bytes the directory holds, as last counted and since added here.
    // Other browsers share the directory and add to it unseen, so it is
    // counted again whenever this one has written a sixteenth of the cap
    // since (`since`); what this one adds while a sweep counts is added
    // after it (`during`).
    uint64_t total, since, during;
    uint64_t hits, confirmed, stale, stored;
    bool evicting;              // one thread sweeps at a time, outside the lock
    uint32_t seq;               // this browser's temp names
};

// FNV-1a: a name for an address, checked against the address inside.
static uint64_t hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s != '\0'; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t cap_of(way_cache_t *c)
{
    return __atomic_load_n(&c->cap, __ATOMIC_RELAXED);
}

static bool path_of(const way_cache_t *c, const char *url, char *out, size_t cap)
{
    int n = os64_snprintf(out, cap, "%s/%016lx.e", c->dir, (unsigned long)hash(url));
    return n > 0 && (size_t)n < cap;
}

static bool is_entry(const char *name)
{
    size_t n = os64_strlen(name);
    return n == 18 && name[16] == '.' && name[17] == 'e';
}

static bool is_temp(const char *name)
{
    size_t n = os64_strlen(name);
    return n > 20 && name[16] == '.' && os64_streq(name + n - 4, ".new");
}

static int64_t clock_now(void)
{
    os64_time_t t;
    return os64_time(&t) == 0 ? t.epoch : 0;
}

// Every directory on the way to `dir`, made where missing.
static void make_dirs(const char *dir)
{
    char path[OS64_PATH_MAX];
    size_t n = os64_strcopy(path, sizeof(path), dir);
    if (n >= sizeof(path))
        return;
    for (size_t i = 1; i <= n; i++)
        if (path[i] == '/' || path[i] == '\0') {
            char c = path[i];
            path[i] = '\0';
            (void)os64_mkdir(path);
            path[i] = c;
        }
}

// What the directory holds: its entries' count and bytes. Temp files a
// dead writer left are removed on the way.
static void survey(way_cache_t *c, int32_t *entries, uint64_t *bytes)
{
    *entries = 0;
    *bytes = 0;
    int64_t d = os64_opendir(c->dir);
    if (d < 0)
        return;
    int64_t now = clock_now();
    os64_dirent_t e;
    char path[OS64_PATH_MAX];
    while (os64_readdir((int32_t)d, &e) == 1) {
        if (is_entry(e.name)) {
            (*entries)++;
            *bytes += e.size;
        } else if (is_temp(e.name) && e.mtime != 0 && (int64_t)e.mtime + TEMP_ABANDONED < now &&
                   os64_snprintf(path, sizeof(path), "%s/%s", c->dir, e.name) > 0) {
            (void)os64_unlink(path);
        }
    }
    os64_close((int32_t)d);
}

way_cache_t *way_cache_open(const char *dir, uint64_t cap)
{
    way_cache_t *c = os64_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->cap = cap;
    c->enabled = true;
    if (os64_strcopy(c->dir, sizeof(c->dir), dir) >= sizeof(c->dir))
        return c;
    make_dirs(c->dir);
    os64_dirent_t e;
    c->usable = os64_stat(c->dir, &e) >= 0 && (e.flags & OS64_DE_DIR) != 0;
    if (c->usable) {
        int32_t entries;
        survey(c, &entries, &c->total);
    }
    return c;
}

void way_cache_close(way_cache_t *c)
{
    os64_free(c);
}

void way_cache_enable(way_cache_t *c, bool on)
{
    __atomic_store_n(&c->enabled, on, __ATOMIC_RELAXED);
}

bool way_cache_enabled(way_cache_t *c)
{
    return c != NULL && c->usable && __atomic_load_n(&c->enabled, __ATOMIC_RELAXED);
}

const char *way_cache_dir(way_cache_t *c)
{
    return c->dir;
}

static void sweep_if_due(way_cache_t *c);

void way_cache_stats(way_cache_t *c, way_cache_stats_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->usable = c->usable;
    out->enabled = way_cache_enabled(c);
    out->cap = cap_of(c);
    if (c->usable)
        survey(c, &out->entries, &out->bytes);
    os64_lock_acquire(&c->lock);
    out->hits = c->hits;
    out->confirmed = c->confirmed;
    out->stale = c->stale;
    out->stored = c->stored;
    c->total = out->bytes;
    os64_lock_release(&c->lock);
    // What other browsers wrote may have put it over: counted, it is swept.
    if (c->usable)
        sweep_if_due(c);
}

void way_cache_count(way_cache_t *c, way_served_t how)
{
    os64_lock_acquire(&c->lock);
    if (how == WAY_SERVED_KEPT)
        c->hits++;
    else if (how == WAY_SERVED_CONFIRMED)
        c->confirmed++;
    else if (how == WAY_SERVED_STALE)
        c->stale++;
    os64_lock_release(&c->lock);
}

int32_t way_cache_clear(way_cache_t *c)
{
    int32_t gone = 0;
    int64_t d = c->usable ? os64_opendir(c->dir) : -1;
    if (d < 0)
        return 0;
    os64_dirent_t e;
    char path[OS64_PATH_MAX];
    while (os64_readdir((int32_t)d, &e) == 1)
        if (is_entry(e.name) && os64_snprintf(path, sizeof(path), "%s/%s", c->dir, e.name) > 0 &&
            os64_unlink(path) == 0)
            gone++;
    os64_close((int32_t)d);
    os64_lock_acquire(&c->lock);
    c->total = 0;
    os64_lock_release(&c->lock);
    return gone;
}

void way_cache_drop(way_cache_t *c, const char *url)
{
    char path[OS64_PATH_MAX];
    if (c != NULL && c->usable && path_of(c, url, path, sizeof(path)))
        (void)os64_unlink(path);
}

// Reads exactly `len` bytes, or fails.
static bool read_exactly(int32_t h, void *buf, size_t len)
{
    size_t at = 0;
    while (at < len) {
        int64_t n = os64_read(h, (uint8_t *)buf + at, len - at);
        if (n <= 0)
            return false;
        at += (size_t)n;
    }
    return true;
}

static bool write_all(int32_t h, const void *buf, size_t len)
{
    size_t at = 0;
    while (at < len) {
        int64_t n = os64_write(h, (const uint8_t *)buf + at, len - at);
        if (n <= 0)
            return false;
        at += (size_t)n;
    }
    return true;
}

bool way_cache_find(way_cache_t *c, const char *url, way_entry_t *e, uint8_t **body,
                    size_t *len, size_t cap)
{
    *body = NULL;
    *len = 0;
    char path[OS64_PATH_MAX];
    if (!way_cache_enabled(c) || !path_of(c, url, path, sizeof(path)))
        return false;
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return false;
    // The head is read a piece at a time up to its blank line; what came
    // with it is the body's first bytes.
    char *head = os64_malloc(ENTRY_HEAD_MAX + 1);
    bool ok = head != NULL;
    size_t have = 0, end = 0;
    while (ok && end == 0) {
        int64_t n = have < ENTRY_HEAD_MAX ? os64_read((int32_t)h, head + have, ENTRY_HEAD_MAX - have)
                                          : 0;
        if (n <= 0) {
            ok = false;
            break;
        }
        size_t from = have > 0 ? have - 1 : 0;
        have += (size_t)n;
        for (size_t i = from; i + 1 < have; i++)
            if (head[i] == '\n' && head[i + 1] == '\n') {
                end = i + 2;
                break;
            }
    }
    uint64_t want = 0;
    if (ok) {
        head[end - 1] = '\0';
        ok = entry_parse(head, e, &want) && os64_streq(e->url, url) && want <= cap;
    }
    uint8_t *bytes = ok ? os64_malloc(want > 0 ? (size_t)want : 1) : NULL;
    ok = bytes != NULL;
    if (ok) {
        size_t early = have - end;
        if (early > want) {
            ok = false;                 // longer than it says: not what was written
        } else {
            os64_memcpy(bytes, head + end, early);
            ok = read_exactly((int32_t)h, bytes + early, (size_t)want - early);
            uint8_t extra;
            ok = ok && os64_read((int32_t)h, &extra, 1) == 0;
        }
    }
    os64_free(head);
    os64_close((int32_t)h);
    if (!ok) {
        os64_free(bytes);
        return false;
    }
    *body = bytes;
    *len = (size_t)want;
    return true;
}

// ── Keeping the cache under its cap ─────────────────────────────────────

typedef struct {
    char name[20];
    uint64_t size, mtime;
} Kept;

static void sort_by_time(Kept *v, Kept *tmp, size_t n)
{
    if (n < 2)
        return;
    size_t h = n / 2;
    sort_by_time(v, tmp, h);
    sort_by_time(v + h, tmp, n - h);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = v[j].mtime < v[i].mtime ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    os64_memcpy(v, tmp, n * sizeof(*v));
}

// The directory counted, and when it is over the cap, the oldest entries
// go, by their files' times, until what is left is under nine-tenths of
// it. Disk work, so it runs outside the lock, by the one thread that set
// `evicting`.
static void evict(way_cache_t *c)
{
    int64_t d = os64_opendir(c->dir);
    size_t n = 0, capn = 256;
    if (d < 0) {
        os64_lock_acquire(&c->lock);
        c->evicting = false;
        os64_lock_release(&c->lock);
        return;
    }
    uint64_t total = 0;
    Kept *v = os64_malloc(capn * sizeof(*v));
    os64_dirent_t e;
    while (v != NULL && os64_readdir((int32_t)d, &e) == 1) {
        if (!is_entry(e.name))
            continue;
        if (n == capn) {
            Kept *grown = os64_realloc(v, capn * 2 * sizeof(*v));
            if (grown == NULL)
                break;
            v = grown;
            capn *= 2;
        }
        os64_strcopy(v[n].name, sizeof(v[n].name), e.name);
        v[n].size = e.size;
        v[n].mtime = e.mtime;
        total += e.size;
        n++;
    }
    os64_close((int32_t)d);
    Kept *tmp = v != NULL ? os64_malloc(n * sizeof(*tmp) + 1) : NULL;
    if (tmp != NULL) {
        sort_by_time(v, tmp, n);
        char path[OS64_PATH_MAX];
        uint64_t keep = total > cap_of(c) ? cap_of(c) / 10 * 9 : total;
        for (size_t i = 0; i < n && total > keep; i++)
            if (os64_snprintf(path, sizeof(path), "%s/%s", c->dir, v[i].name) > 0 &&
                os64_unlink(path) == 0)
                total -= v[i].size;
    }
    os64_free(tmp);
    os64_free(v);
    os64_lock_acquire(&c->lock);
    c->total = total + c->during;
    c->during = 0;
    c->evicting = false;
    os64_lock_release(&c->lock);
}

// Past the cap, or due a count, one thread sweeps: the first to see it,
// while no other is. A browser over the cap by what others wrote finds out
// within a sixteenth of the cap of its own writing.
static void sweep_if_due(way_cache_t *c)
{
    os64_lock_acquire(&c->lock);
    uint64_t cap = cap_of(c);
    bool sweep = (c->total > cap || c->since >= cap / 16) && !c->evicting;
    if (sweep) {
        c->evicting = true;
        c->since = 0;
        c->during = 0;
    }
    os64_lock_release(&c->lock);
    if (sweep)
        evict(c);
}

bool way_cache_put(way_cache_t *c, const way_entry_t *e, const uint8_t *body, size_t len)
{
    char path[OS64_PATH_MAX], temp[OS64_PATH_MAX];
    if (!way_cache_enabled(c) || len > cap_of(c) / 8 || !path_of(c, e->url, path, sizeof(path)))
        return false;
    char *head = os64_malloc(ENTRY_HEAD_MAX);
    size_t hlen = head != NULL ? entry_head(e, len, head, ENTRY_HEAD_MAX) : 0;
    uint32_t seq = __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELAXED);
    int n = os64_snprintf(temp, sizeof(temp), "%s.%lu.%lu.new", path,
                          (unsigned long)os64_taskid(), (unsigned long)seq);
    int64_t h = hlen != 0 && n > 0 && (size_t)n < sizeof(temp) ? os64_open(temp, "x") : -1;
    bool ok = h >= 0 && write_all((int32_t)h, head, hlen) && write_all((int32_t)h, body, len);
    if (h >= 0)
        ok = os64_close((int32_t)h) == 0 && ok;
    ok = ok && os64_rename(temp, path) == 0;
    if (!ok && h >= 0)
        (void)os64_unlink(temp);
    os64_free(head);
    if (!ok)
        return false;
    os64_lock_acquire(&c->lock);
    c->stored++;
    c->total += hlen + len;
    c->since += hlen + len;
    if (c->evicting)
        c->during += hlen + len;
    os64_lock_release(&c->lock);
    sweep_if_due(c);
    return true;
}

void way_cache_set_cap(way_cache_t *c, uint64_t cap)
{
    __atomic_store_n(&c->cap, cap, __ATOMIC_RELAXED);
    if (c->usable)
        sweep_if_due(c);
}
