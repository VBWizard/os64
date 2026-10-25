// jar.c — the cookie jar and the Referer policy (way/jar.h).
//
// RFC 6265's sections are cited where their rule is applied; where the
// 6265bis draft differs, the browsers ship the draft, and so does this.

#include "way/jar.h"
#include "os64/fmt.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef enum { SAMESITE_DEFAULT, SAMESITE_NONE, SAMESITE_LAX, SAMESITE_STRICT } SameSite;

typedef struct {
    char *name, *value, *domain, *path;     // one allocation, `name` its start
    int64_t expiry;                         // INT64_MAX: until the browser closes
    int64_t created;                        // seconds, and `order` within one
    uint64_t order;
    // §5.3 step 12's last-access time, as the jar's own count: stamped as
    // the cookie is stored and each time it is sent, so eviction takes the
    // one used longest ago — never the login every page carries.
    uint64_t used;
    bool host_only, secure, http_only;
    SameSite same_site;
    // Scratch under the jar's lock, for one sending (cookies(): whether
    // this cookie matches the request, and whether it is to be sent), one
    // following of a file (read_text: whether a line has named it yet) or
    // one writing of it (way_jar_text: whether it is written).
    uint8_t pick;
    // The number of the change that made this cookie what it is, while a
    // file of the jar has still to hear it (§ Kept across restarts); 0 once
    // it has, and for a cookie read from one.
    uint64_t changed;
} Cookie;

// A cookie a file of the jar may hold that this jar let go: its name,
// domain and path (one allocation, `name` its start), host-only or not,
// and the number of the change that let it go.
typedef struct {
    char *name, *domain, *path;
    bool host_only;
    uint64_t changed;
} Gone;

struct way_jar {
    os64_lock_t lock;
    Cookie *cookies;
    int32_t n, cap;
    uint64_t next_order, next_use;
    // What a file of the jar has still to hear (§ Kept across restarts):
    // the number of the latest change, and the cookies let go since.
    uint64_t changes;
    Gone *gone;
    int32_t ngone, gonecap;
    // `cookies = off`: nothing is heard and nothing is sent.
    bool refusing;
};

way_jar_t *way_jar_new(void)
{
    return os64_calloc(1, sizeof(way_jar_t));
}

static void gone_clear(way_jar_t *jar)
{
    for (int32_t i = 0; i < jar->ngone; i++)
        os64_free(jar->gone[i].name);
    jar->ngone = 0;
}

void way_jar_free(way_jar_t *jar)
{
    if (jar == NULL)
        return;
    for (int32_t i = 0; i < jar->n; i++)
        os64_free(jar->cookies[i].name);
    os64_free(jar->cookies);
    gone_clear(jar);
    os64_free(jar->gone);
    os64_free(jar);
}

// ── Text ────────────────────────────────────────────────────────────────

static bool wsp(char c)
{
    return c == ' ' || c == '\t';
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool same_nocase(const char *a, size_t alen, const char *b)
{
    size_t blen = os64_strlen(b);
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

static bool starts_nocase(const char *a, size_t alen, const char *prefix)
{
    size_t n = os64_strlen(prefix);
    return alen >= n && same_nocase(a, n, prefix);
}

// [*from, *to) with the white space at both ends taken off.
static void trim(const char **from, const char **to)
{
    while (*from < *to && wsp(**from))
        (*from)++;
    while (*to > *from && wsp((*to)[-1]))
        (*to)--;
}

// White space at either end of [s, s + n), which trim() would take off.
static bool padded(const char *s, size_t n)
{
    return n > 0 && (wsp(s[0]) || wsp(s[n - 1]));
}

// 6265bis: a name or value holding a control byte other than a tab is
// refused whole, since it would be sent back as something else.
static bool clean(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }
    return true;
}

// ── §5.1.1 Dates ────────────────────────────────────────────────────────

static bool delimiter(unsigned char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2f) || (c >= 0x3b && c <= 0x40) ||
           (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

// 1*`max` digits at `t`, then anything but a digit (or the end): the value
// and how many digits, or 0 digits when not.
static int32_t digits(const char *t, size_t n, size_t max, size_t *used)
{
    size_t i = 0;
    int32_t v = 0;
    while (i < n && digit(t[i]) && i < max) {
        v = v * 10 + (t[i] - '0');
        i++;
    }
    if (i == 0 || (i < n && digit(t[i])))
        i = 0;
    *used = i;
    return v;
}

static bool leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian).
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool way_cookie_date(const char *text, size_t len, int64_t *epoch)
{
    static const char *const kMonths[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                            "jul", "aug", "sep", "oct", "nov", "dec"};
    bool have_time = false, have_day = false, have_month = false, have_year = false;
    int32_t hour = 0, minute = 0, second = 0, day = 0, month = 0, year = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && delimiter((unsigned char)text[i]))
            i++;
        size_t start = i;
        while (i < len && !delimiter((unsigned char)text[i]))
            i++;
        const char *tok = text + start;
        size_t n = i - start;
        if (n == 0)
            continue;
        size_t used = 0, at = 0;
        if (!have_time) {
            // hms-time: 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT ( non-digit *OCTET )
            int32_t h = digits(tok, n, 2, &used);
            if (used > 0 && used < n && tok[used] == ':') {
                at = used + 1;
                int32_t m = digits(tok + at, n - at, 2, &used);
                if (used > 0 && at + used < n && tok[at + used] == ':') {
                    at += used + 1;
                    int32_t s = digits(tok + at, n - at, 2, &used);
                    if (used > 0) {
                        have_time = true;
                        hour = h;
                        minute = m;
                        second = s;
                        continue;
                    }
                }
            }
        }
        if (!have_day) {
            int32_t d = digits(tok, n, 2, &used);
            if (used > 0) {
                have_day = true;
                day = d;
                continue;
            }
        }
        if (!have_month && n >= 3) {
            int32_t found = -1;
            for (int32_t k = 0; k < 12 && found < 0; k++)
                if (same_nocase(tok, 3, kMonths[k]))
                    found = k;
            if (found >= 0) {
                have_month = true;
                month = found + 1;
                continue;
            }
        }
        if (!have_year) {
            int32_t y = digits(tok, n, 4, &used);
            if (used >= 2) {
                have_year = true;
                year = y;
                continue;
            }
        }
    }
    if (have_year && year >= 70 && year <= 99)
        year += 1900;
    else if (have_year && year >= 0 && year <= 69)
        year += 2000;
    if (!have_time || !have_day || !have_month || !have_year || day < 1 || day > 31 ||
        year < 1601 || hour > 23 || minute > 59 || second > 59)
        return false;
    static const int32_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int32_t last = kDays[month - 1] + (month == 2 && leap(year));
    if (day > last)
        return false;
    *epoch = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

// ── Addresses ───────────────────────────────────────────────────────────

// An address spelled as an IPv4 literal: a Domain attribute is no use to
// one, since no name sits under it.
static bool ip_literal(const char *host)
{
    if (*host == '\0')
        return false;
    for (const char *p = host; *p != '\0'; p++)
        if (!digit(*p) && *p != '.')
            return false;
    return true;
}

// §5.1.3: `host` is `domain`, or a name under it.
static bool domain_match(const char *host, const char *domain)
{
    size_t h = os64_strlen(host), d = os64_strlen(domain);
    if (h == d)
        return os64_streq(host, domain);
    return h > d && host[h - d - 1] == '.' && os64_streq(host + h - d, domain) &&
           !ip_literal(host);
}

// The path of an address, without its query.
static size_t path_length(const char *path)
{
    size_t n = 0;
    while (path[n] != '\0' && path[n] != '?')
        n++;
    return n;
}

// §5.1.4: a request's path is in a cookie's path.
static bool path_match(const char *req, size_t rn, const char *cpath)
{
    size_t cn = os64_strlen(cpath);
    if (cn > rn || os64_memcmp(req, cpath, cn) != 0)
        return false;
    return cn == rn || cpath[cn - 1] == '/' || req[cn] == '/';
}

// §5.1.4: the directory of the address that set a cookie.
static void default_path(const char *path, char *out, size_t cap)
{
    size_t n = path_length(path);
    size_t last = 0;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '/')
            last = i;
    if (n == 0 || path[0] != '/' || last == 0) {
        os64_strcopy(out, cap, "/");
        return;
    }
    if (last >= cap)
        last = cap - 1;
    os64_memcpy(out, path, last);
    out[last] = '\0';
}

// ── Storing ─────────────────────────────────────────────────────────────

static void drop(way_jar_t *jar, int32_t i)
{
    os64_free(jar->cookies[i].name);
    jar->cookies[i] = jar->cookies[--jar->n];
}

static bool older(const Cookie *a, const Cookie *b)
{
    return a->created < b->created || (a->created == b->created && a->order < b->order);
}

// §5.3 step 12: past a limit, the cookie used longest ago goes (expired
// ones are purged before this is asked).
static bool staler(const Cookie *a, const Cookie *b)
{
    return a->used < b->used;
}

static void gone_note(way_jar_t *jar, const Cookie *c);

// Cookie `i` let go by the jar's own rules (past its expiry, or past a
// limit), not by a site. A file may still hold a line in its place: the
// cookie itself when it is kept, or, for a change the file has not heard,
// the value that change replaced. Either way the file is told it is gone
// (gone_note), or the next save keeps that line and a following brings it
// back. An unchanged kept cookie past its expiry needs no telling: its line
// expires with it.
static void let_go(way_jar_t *jar, int32_t i, bool expired)
{
    const Cookie *c = &jar->cookies[i];
    if (c->changed != 0 || (!expired && c->expiry != INT64_MAX))
        gone_note(jar, c);
    drop(jar, i);
}

static void purge_expired(way_jar_t *jar, int64_t now)
{
    for (int32_t i = 0; i < jar->n;)
        if (jar->cookies[i].expiry <= now)
            let_go(jar, i, true);
        else
            i++;
}

// Past a limit, the cookie used longest ago goes: in the domain for the
// domain's limit, in the jar for the jar's. `domain` must outlive every
// drop here: the string of the cookie just heard (the one used MOST
// recently, so never the one dropped; a drop moves Cookie records, not the
// blocks their strings live in), or a copy. True when one went.
static bool enforce_limits(way_jar_t *jar, const char *domain)
{
    bool dropped = false;
    for (;;) {
        int32_t count = 0, stalest = -1;
        for (int32_t i = 0; i < jar->n; i++) {
            if (!os64_streq(jar->cookies[i].domain, domain))
                continue;
            count++;
            if (stalest < 0 || staler(&jar->cookies[i], &jar->cookies[stalest]))
                stalest = i;
        }
        if (count <= WAY_COOKIES_PER_DOMAIN)
            break;
        let_go(jar, stalest, false);
        dropped = true;
    }
    while (jar->n > WAY_COOKIES_MAX) {
        int32_t stalest = 0;
        for (int32_t i = 1; i < jar->n; i++)
            if (staler(&jar->cookies[i], &jar->cookies[stalest]))
                stalest = i;
        let_go(jar, stalest, false);
        dropped = true;
    }
    return dropped;
}

// Every domain held to its limit, and the jar to its own, after a batch of
// put_in: a cookie a file names must not be let go for a stale one the
// batch is about to drop (a following lets go what no line named only
// after every line is in).
static void enforce_every(way_jar_t *jar)
{
    for (bool dropped = true; dropped;) {
        dropped = false;
        for (int32_t i = 0; i < jar->n; i++) {
            bool first = true;
            for (int32_t j = 0; j < i && first; j++)
                first = !os64_streq(jar->cookies[j].domain, jar->cookies[i].domain);
            if (!first)
                continue;
            char domain[OS64_URL_HOST_MAX];
            os64_strcopy(domain, sizeof(domain), jar->cookies[i].domain);
            if (enforce_limits(jar, domain)) {
                dropped = true;
                break;
            }
        }
    }
}

// A cookie into the jar with its four strings copied into one block, its
// other fields zero. NULL on no memory, the jar as it was.
static Cookie *insert(way_jar_t *jar, const char *name, size_t nlen, const char *value, size_t vlen,
                      const char *domain, const char *path)
{
    size_t dlen = os64_strlen(domain), plen = os64_strlen(path);
    char *block = os64_malloc(nlen + vlen + dlen + plen + 4);
    if (block == NULL)
        return NULL;
    if (jar->n == jar->cap) {
        int32_t cap = jar->cap > 0 ? jar->cap * 2 : 16;
        Cookie *grown = os64_realloc(jar->cookies, (size_t)cap * sizeof(Cookie));
        if (grown == NULL) {
            os64_free(block);
            return NULL;
        }
        jar->cookies = grown;
        jar->cap = cap;
    }
    Cookie *c = &jar->cookies[jar->n++];
    os64_memset(c, 0, sizeof(*c));
    c->name = block;
    os64_memcpy(c->name, name, nlen);
    c->name[nlen] = '\0';
    c->value = c->name + nlen + 1;
    os64_memcpy(c->value, value, vlen);
    c->value[vlen] = '\0';
    c->domain = c->value + vlen + 1;
    os64_memcpy(c->domain, domain, dlen + 1);
    c->path = c->domain + dlen + 1;
    os64_memcpy(c->path, path, plen + 1);
    return c;
}

// The same cookie, as RFC 6265 § 5.3 step 11 says: name, domain and path,
// and here host-only or not too, since the jar keeps one of each.
static bool same_cookie(const char *name, const char *domain, const char *path, bool host_only,
                        const char *name2, const char *domain2, const char *path2, bool host_only2)
{
    return host_only == host_only2 && os64_streq(name, name2) && os64_streq(domain, domain2) &&
           os64_streq(path, path2);
}

static int32_t gone_find(const way_jar_t *jar, const Cookie *c)
{
    for (int32_t i = 0; i < jar->ngone; i++) {
        const Gone *g = &jar->gone[i];
        if (same_cookie(g->name, g->domain, g->path, g->host_only, c->name, c->domain, c->path, c->host_only))
            return i;
    }
    return -1;
}

static void gone_drop(way_jar_t *jar, int32_t i)
{
    os64_free(jar->gone[i].name);
    jar->gone[i] = jar->gone[--jar->ngone];
}

// `c`, a cookie a file keeps, is let go before the file has heard: it is
// remembered until the file has (way_jar_saved). At WAY_COOKIES_MAX
// remembered the one let go longest ago is forgotten, and with no memory
// to remember this one, it is not: either way that cookie's line stays in
// the file, until it expires there.
static void gone_note(way_jar_t *jar, const Cookie *c)
{
    int32_t at = gone_find(jar, c);
    if (at >= 0) {
        jar->gone[at].changed = ++jar->changes;
        return;
    }
    if (jar->ngone == WAY_COOKIES_MAX) {
        int32_t oldest = 0;
        for (int32_t i = 1; i < jar->ngone; i++)
            if (jar->gone[i].changed < jar->gone[oldest].changed)
                oldest = i;
        gone_drop(jar, oldest);
    }
    if (jar->ngone == jar->gonecap) {
        int32_t cap = jar->gonecap > 0 ? jar->gonecap * 2 : 8;
        Gone *grown = os64_realloc(jar->gone, (size_t)cap * sizeof(Gone));
        if (grown == NULL)
            return;
        jar->gone = grown;
        jar->gonecap = cap;
    }
    size_t nlen = os64_strlen(c->name), dlen = os64_strlen(c->domain), plen = os64_strlen(c->path);
    char *block = os64_malloc(nlen + dlen + plen + 3);
    if (block == NULL)
        return;
    Gone *g = &jar->gone[jar->ngone++];
    g->name = block;
    os64_memcpy(g->name, c->name, nlen + 1);
    g->domain = g->name + nlen + 1;
    os64_memcpy(g->domain, c->domain, dlen + 1);
    g->path = g->domain + dlen + 1;
    os64_memcpy(g->path, c->path, plen + 1);
    g->host_only = c->host_only;
    g->changed = ++jar->changes;
}

// `c` is set again: whatever it replaces in a file is its to say now.
static void gone_forget(way_jar_t *jar, const Cookie *c)
{
    int32_t at = gone_find(jar, c);
    if (at >= 0)
        gone_drop(jar, at);
}

typedef struct {
    const char *name, *value;
    size_t nlen, vlen;
    bool have_max_age, have_expires;
    int64_t max_age_expiry, expires;
    char domain[OS64_URL_HOST_MAX];
    bool have_domain;
    char path[OS64_URL_PATH_MAX];
    bool have_path;
    bool secure, http_only;
    SameSite same_site;
} Heard;

// §5.2: the attributes after the name and value, the last of a kind winning.
static void attributes(Heard *h, const char *p, const char *end, int64_t now)
{
    while (p < end) {
        const char *semi = p;
        while (semi < end && *semi != ';')
            semi++;
        const char *an = p, *ae = semi, *eq = p;
        while (eq < semi && *eq != '=')
            eq++;
        const char *vn = eq < semi ? eq + 1 : semi, *ve = semi;
        ae = eq;
        trim(&an, &ae);
        trim(&vn, &ve);
        size_t alen = (size_t)(ae - an), vlen = (size_t)(ve - vn);
        p = semi < end ? semi + 1 : end;
        if (vlen > 1024)
            continue;                   // 6265bis: an attribute that long is ignored
        if (same_nocase(an, alen, "expires")) {
            int64_t when;
            if (way_cookie_date(vn, vlen, &when)) {
                h->have_expires = true;
                h->expires = when;
            }
        } else if (same_nocase(an, alen, "max-age")) {
            size_t i = vlen > 0 && vn[0] == '-' ? 1 : 0;
            bool ok = i < vlen;
            int64_t delta = 0;
            for (; ok && i < vlen; i++) {
                if (!digit(vn[i]))
                    ok = false;
                else if (delta < INT64_MAX / 20)
                    delta = delta * 10 + (vn[i] - '0');
            }
            if (!ok)
                continue;
            h->have_max_age = true;
            h->max_age_expiry = vn[0] == '-' || delta == 0 ? INT64_MIN
                                : delta > INT64_MAX - now ? INT64_MAX - 1 : now + delta;
        } else if (same_nocase(an, alen, "domain")) {
            if (vlen == 0)
                continue;
            if (vn[0] == '.') {
                vn++;
                vlen--;
            }
            if (vlen == 0 || vlen >= sizeof(h->domain))
                continue;
            for (size_t i = 0; i < vlen; i++)
                h->domain[i] = lower(vn[i]);
            h->domain[vlen] = '\0';
            h->have_domain = true;
        } else if (same_nocase(an, alen, "path")) {
            h->have_path = vlen > 0 && vn[0] == '/' && vlen < sizeof(h->path);
            if (h->have_path) {
                os64_memcpy(h->path, vn, vlen);
                h->path[vlen] = '\0';
            }
        } else if (same_nocase(an, alen, "secure")) {
            h->secure = true;
        } else if (same_nocase(an, alen, "httponly")) {
            h->http_only = true;
        } else if (same_nocase(an, alen, "samesite")) {
            h->same_site = same_nocase(vn, vlen, "strict") ? SAMESITE_STRICT
                           : same_nocase(vn, vlen, "lax") ? SAMESITE_LAX
                           : same_nocase(vn, vlen, "none") ? SAMESITE_NONE
                           : SAMESITE_DEFAULT;
        }
    }
}

// THE RULES A COOKIE IS HELD TO, wherever it came from: `h` as heard or as
// a file spelled it, set by `from_host` (whose page was at `from_path`)
// over a connection that was `encrypted` or not. False for one refused.
static bool judged(Heard *h, const char *from_host, const char *from_path, bool encrypted)
{
    // 6265bis: a name and value of at most WAY_COOKIE_BYTES_MAX, neither
    // holding a control byte but a tab.
    if (h->nlen + h->vlen == 0 || h->nlen + h->vlen > WAY_COOKIE_BYTES_MAX ||
        !clean(h->name, h->nlen) || !clean(h->value, h->vlen))
        return false;
    // §5.3 step 6: a Domain the setting host is not in is refused, and so
    // is one naming a single label that is not the host itself — the most
    // this jar can tell of a public suffix without the list (booked).
    if (h->have_domain) {
        if (ip_literal(from_host) ? !os64_streq(from_host, h->domain)
                                  : !domain_match(from_host, h->domain))
            return false;
        bool dotted = false;
        for (const char *p = h->domain; *p != '\0'; p++)
            dotted |= *p == '.';
        if (!dotted && !os64_streq(from_host, h->domain))
            return false;
    }
    if (!h->have_path)
        default_path(from_path, h->path, sizeof(h->path));
    // A secure cookie comes only over an encrypted connection, and the
    // prefixes promise what their names say (6265bis §4.1.3).
    if (h->secure && !encrypted)
        return false;
    if (h->nlen >= 9 && starts_nocase(h->name, h->nlen, "__secure-") && !(h->secure && encrypted))
        return false;
    if (h->nlen >= 7 && starts_nocase(h->name, h->nlen, "__host-") &&
        !(h->secure && encrypted && !h->have_domain && os64_streq(h->path, "/")))
        return false;
    return true;
}

// The cookie a Set-Cookie value describes, or false for one refused.
static bool heard(Heard *h, const os64_url_t *from, bool encrypted, const char *value,
                  size_t len, int64_t now)
{
    os64_memset(h, 0, sizeof(*h));
    const char *end = value + len, *semi = value;
    while (semi < end && *semi != ';')
        semi++;
    const char *eq = value;
    while (eq < semi && *eq != '=')
        eq++;
    const char *nb = value, *ne = eq < semi ? eq : value;
    const char *vb = eq < semi ? eq + 1 : value, *ve = semi;
    trim(&nb, &ne);
    trim(&vb, &ve);
    h->name = nb;
    h->nlen = (size_t)(ne - nb);
    h->value = vb;
    h->vlen = (size_t)(ve - vb);
    if (h->nlen + h->vlen == 0 || h->nlen + h->vlen > WAY_COOKIE_BYTES_MAX)
        return false;
    attributes(h, semi < end ? semi + 1 : end, end, now);
    return judged(h, from->host, from->path, encrypted);
}

// `script`: heard from a page's script (document.cookie), RFC 6265's
// "non-HTTP API", which may neither set an HttpOnly cookie nor set one where
// an HttpOnly cookie of the same name, domain and path is.
static void hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                 size_t len, int64_t now, bool script)
{
    if (jar == NULL || from == NULL || value == NULL)
        return;
    Heard h;
    if (!heard(&h, from, encrypted, value, len, now) || (script && h.http_only))
        return;
    const char *domain = h.have_domain ? h.domain : from->host;
    int64_t expiry = h.have_max_age ? h.max_age_expiry : h.have_expires ? h.expires : INT64_MAX;

    os64_lock_acquire(&jar->lock);
    if (jar->refusing) {
        os64_lock_release(&jar->lock);
        return;
    }
    // §5.3 step 12 takes the expired first, so they go before any limit is
    // counted — and before anything below holds an index into the jar.
    purge_expired(jar, now);
    // 6265bis §5.7 step 12: over a connection that is not encrypted, a
    // cookie may not shadow a secure one of the same name.
    int32_t same = -1;
    for (int32_t i = 0; i < jar->n; i++) {
        Cookie *c = &jar->cookies[i];
        bool named = os64_strlen(c->name) == h.nlen && os64_memcmp(c->name, h.name, h.nlen) == 0;
        if (!named)
            continue;
        if (!encrypted && c->secure &&
            (domain_match(c->domain, domain) || domain_match(domain, c->domain)) &&
            path_match(h.path, os64_strlen(h.path), c->path)) {
            os64_lock_release(&jar->lock);
            return;
        }
        bool identity = os64_streq(c->domain, domain) && os64_streq(c->path, h.path);
        // RFC 6265 § 5.3 step 11.2: a script may not touch an HttpOnly
        // cookie of its name, domain and path, whichever host-only flag
        // either has: one set beside it would ride the same requests.
        if (identity && script && c->http_only) {
            os64_lock_release(&jar->lock);
            return;
        }
        if (identity && c->host_only == !h.have_domain)
            same = i;
    }
    // §5.3 step 11: a cookie replaces its namesake, keeping its age; one
    // already expired deletes it and is not kept.
    int64_t created = now;
    uint64_t order = jar->next_order++;
    // The namesake was one a file keeps, or a session cookie set in such a
    // one's place that the file has still to hear of: either way the file
    // holds a line in this place, and this cookie is what it says now.
    bool was_kept = false;
    if (same >= 0) {
        created = jar->cookies[same].created;
        order = jar->cookies[same].order;
        was_kept = jar->cookies[same].expiry != INT64_MAX || jar->cookies[same].changed != 0;
        if (was_kept && expiry <= now)
            gone_note(jar, &jar->cookies[same]);
        drop(jar, same);
    }
    if (expiry <= now) {
        os64_lock_release(&jar->lock);
        return;
    }
    Cookie *c = insert(jar, h.name, h.nlen, h.value, h.vlen, domain, h.path);
    if (c == NULL) {
        os64_lock_release(&jar->lock);
        return;
    }
    c->expiry = expiry;
    c->created = created;
    c->order = order;
    c->used = jar->next_use++;
    c->host_only = !h.have_domain;
    c->secure = h.secure;
    c->http_only = h.http_only;
    c->same_site = h.same_site;
    // A cookie a file keeps, or one in the place of one, is a change the
    // file has still to hear; a session cookie beside nothing kept is not.
    if (expiry != INT64_MAX || was_kept) {
        c->changed = ++jar->changes;
        gone_forget(jar, c);
    }
    enforce_limits(jar, c->domain);
    os64_lock_release(&jar->lock);
}

void way_jar_hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                  size_t len, int64_t now)
{
    hear(jar, from, encrypted, value, len, now, false);
}

// ── Sending ─────────────────────────────────────────────────────────────

// §5.4 step 2: the longer path first, then the older.
static bool before(const Cookie *a, const Cookie *b)
{
    size_t la = os64_strlen(a->path), lb = os64_strlen(b->path);
    return la != lb ? la > lb : older(a, b);
}

// A cookie's bytes in a Cookie header, without the "; " before it.
static size_t spelled(const Cookie *c)
{
    size_t nlen = os64_strlen(c->name);
    return nlen + (nlen > 0 ? 1 : 0) + os64_strlen(c->value);
}

enum { PICK_NO = 0, PICK_MATCHES, PICK_SENT };

// Which of two cookies is kept first when not all fit (pick_to_send): an
// HttpOnly one before one that is not, and then the newer.
static bool kept_before(const Cookie *a, const Cookie *b)
{
    return a->http_only != b->http_only ? a->http_only : older(b, a);
}

// WHICH COOKIES GO WHEN NOT ALL FIT: the HttpOnly ones first and then the
// rest, the newest first within each, as many as fit, each taken whole (a
// smaller one may still fit after a larger one did not). RFC 6265 § 5.4
// orders the header and says nothing of which to leave out, and its order
// — the oldest first — would cut exactly the cookies a site set last,
// which after a login are the login: a session cookie is created when the
// person signs in, after everything the site set on the visits before. And
// a session is the server's own memory, which it sets HttpOnly so no
// script reads it, where the cookies a page's scripts set (analytics,
// consent, a test's bucket) can never be HttpOnly: so those are what is cut
// first, however new. The ones chosen are still written in the RFC's order
// (cookies()). Answers how many matching cookies were left out.
static int32_t pick_to_send(way_jar_t *jar, size_t room)
{
    size_t used = 0;
    int32_t taken = 0, left = 0;
    for (;;) {
        Cookie *first = NULL;
        for (int32_t i = 0; i < jar->n; i++) {
            Cookie *c = &jar->cookies[i];
            if (c->pick == PICK_MATCHES && (first == NULL || kept_before(c, first)))
                first = c;
        }
        if (first == NULL)
            return left;
        size_t need = (taken > 0 ? 2 : 0) + spelled(first);
        if (used + need <= room) {
            first->pick = PICK_SENT;
            used += need;
            taken++;
        } else {
            first->pick = PICK_NO;
            left++;
        }
    }
}

// `script`: the cookies a page's script reads (document.cookie), which
// leaves out the HttpOnly ones (RFC 6265 § 5.4 step 1).
static size_t cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                      char *out, size_t cap, int32_t *left_out, bool script)
{
    if (left_out != NULL)
        *left_out = 0;
    if (cap > 0)
        out[0] = '\0';
    // No room is still asked: every cookie that matches is counted in
    // `left_out`, which is how a caller learns it needs room at all.
    if (jar == NULL || to == NULL)
        return 0;
    size_t rn = path_length(to->path);
    size_t used = 0;
    os64_lock_acquire(&jar->lock);
    if (jar->refusing) {
        os64_lock_release(&jar->lock);
        return 0;
    }
    purge_expired(jar, now);
    // Which match, and the room they take together: all of them go when
    // they fit, and those pick_to_send keeps first when they do not.
    size_t all = 0;
    int32_t matching = 0;
    for (int32_t i = 0; i < jar->n; i++) {
        Cookie *c = &jar->cookies[i];
        bool host = c->host_only ? os64_streq(to->host, c->domain) : domain_match(to->host, c->domain);
        bool match = host && path_match(to->path, rn, c->path) && !(c->secure && !encrypted) &&
                     !(script && c->http_only);
        c->pick = match ? PICK_SENT : PICK_NO;
        if (match)
            all += (matching++ > 0 ? 2 : 0) + spelled(c);
    }
    size_t room = cap > 0 ? cap - 1 : 0;        // the NUL's byte
    if (all > room) {
        for (int32_t i = 0; i < jar->n; i++)
            if (jar->cookies[i].pick == PICK_SENT)
                jar->cookies[i].pick = PICK_MATCHES;
        int32_t left = pick_to_send(jar, room);
        if (left_out != NULL)
            *left_out = left;
    }
    // Sent in order without sorting the jar: each pass takes the first
    // chosen cookie after the last one taken. The jar is small, and a
    // request asks once.
    const Cookie *last = NULL;
    for (;;) {
        Cookie *next = NULL;
        for (int32_t i = 0; i < jar->n; i++) {
            Cookie *c = &jar->cookies[i];
            if (c->pick != PICK_SENT)
                continue;
            if (last != NULL && !before(last, c))
                continue;
            if (next == NULL || before(c, next))
                next = c;
        }
        if (next == NULL)
            break;
        last = next;
        size_t nlen = os64_strlen(next->name), vlen = os64_strlen(next->value);
        // The room was counted with spelled() above, so this does not
        // stop a cookie that was chosen; it guards `out` should the two
        // counts ever disagree.
        if (used + (used > 0 ? 2 : 0) + spelled(next) >= cap)
            break;
        if (used > 0) {
            out[used++] = ';';
            out[used++] = ' ';
        }
        os64_memcpy(out + used, next->name, nlen);
        used += nlen;
        if (nlen > 0)
            out[used++] = '=';
        os64_memcpy(out + used, next->value, vlen);
        used += vlen;
        out[used] = '\0';
        next->used = jar->next_use++;       // §5.4 step 3: sent is used
    }
    os64_lock_release(&jar->lock);
    return used;
}

size_t way_jar_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                       char *out, size_t cap, int32_t *left_out)
{
    return cookies(jar, to, encrypted, now, out, cap, left_out, false);
}

void way_jar_script_hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                         size_t len, int64_t now)
{
    hear(jar, from, encrypted, value, len, now, true);
}

size_t way_jar_script_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                              char *out, size_t cap, int32_t *left_out)
{
    return cookies(jar, to, encrypted, now, out, cap, left_out, true);
}

int32_t way_jar_count(way_jar_t *jar)
{
    if (jar == NULL)
        return 0;
    os64_lock_acquire(&jar->lock);
    int32_t n = jar->n;
    os64_lock_release(&jar->lock);
    return n;
}

// ── Kept across restarts ────────────────────────────────────────────────
//
// THE FILE IS NETSCAPE'S cookies.txt (1994), as curl and wget write it: a
// line a cookie, seven fields between tabs — the domain, whether it
// includes the names under it (TRUE, with the domain spelled with a leading
// dot) or is the host's alone (FALSE), the path, whether it is Secure, its
// expiry in seconds since 1970, its name, its value — and the domain
// prefixed `#HttpOnly_` for an HttpOnly cookie. SameSite has no field: a
// cookie read from the file has none, which the jar does not judge by yet.
// A session cookie is never written, and an expired one never written nor
// read; curl's session lines (expiry 0) are read as what they say, a
// session that has ended.

static const char kFileHead[] =
    "# Netscape HTTP Cookie File\n"
    "# yonder's cookies: the sites you are signed in to. Whoever can read this file\n"
    "# can sign in as you. Clear cookies, in yonder's Settings, empties it.\n";

// The latest expiry a line holds: eighteen digits, all the reader takes
// (line_expiry). A cookie set to last longer — a Max-Age past any calendar
// — is written as lasting this long, some thirty billion years.
#define FILE_EXPIRY_MAX 999999999999999999LL

// Whether a field may go into a line: nothing that would end a field or
// the line, nor any other control byte.
static bool field_ok(const char *s)
{
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            return false;
    return true;
}

// Indices of cookies, oldest first by older(): a merge sort from the
// bottom up, `tmp` as long as `idx`.
static void by_age(const Cookie *cookies, int32_t *idx, int32_t *tmp, int32_t n)
{
    for (int32_t width = 1; width < n; width *= 2) {
        for (int32_t lo = 0; lo < n; lo += 2 * width) {
            int32_t mid = lo + width < n ? lo + width : n;
            int32_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            int32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi)
                tmp[k++] = older(&cookies[idx[b]], &cookies[idx[a]]) ? idx[b++] : idx[a++];
            while (a < mid)
                tmp[k++] = idx[a++];
            while (b < hi)
                tmp[k++] = idx[b++];
        }
        os64_memcpy(idx, tmp, (size_t)n * sizeof(*idx));
    }
}

_Static_assert(sizeof(kFileHead) <= WAY_JAR_TEXT_MAX - (size_t)WAY_COOKIES_MAX * WAY_JAR_LINE_MAX,
               "WAY_JAR_TEXT_MAX leaves the file's head too little room");

// `c`'s line, its newline included, into `out`: the length, or 0 when it
// does not fit in `cap` with a NUL.
static size_t spell_line(const Cookie *c, char *out, size_t cap)
{
    int w = os64_snprintf(out, cap, "%s%s%s\t%s\t%s\t%s\t%ld\t%s\t%s\n", c->http_only ? "#HttpOnly_" : "",
                          c->host_only ? "" : ".", c->domain, c->host_only ? "FALSE" : "TRUE", c->path,
                          c->secure ? "TRUE" : "FALSE",
                          (long)(c->expiry < FILE_EXPIRY_MAX ? c->expiry : FILE_EXPIRY_MAX), c->name, c->value);
    return w > 0 && (size_t)w < cap ? (size_t)w : 0;
}

static bool round_trips(const Cookie *c, int64_t now, char *scratch);

char *way_jar_text(way_jar_t *jar, int64_t now, size_t *len)
{
    *len = 0;
    if (jar == NULL)
        return NULL;
    char *scratch = os64_malloc(WAY_JAR_LINE_MAX);
    if (scratch == NULL)
        return NULL;
    os64_lock_acquire(&jar->lock);
    int32_t n = 0;
    size_t need = sizeof(kFileHead);
    for (int32_t i = 0; i < jar->n; i++) {
        Cookie *c = &jar->cookies[i];
        c->pick = round_trips(c, now, scratch) ? PICK_MATCHES : PICK_NO;
        if (c->pick == PICK_NO)
            continue;
        n++;
        // "#HttpOnly_" "." domain, two flags, the path, an expiry of at
        // most twenty digits, the name, the value, six tabs and a newline.
        need += 10 + 1 + os64_strlen(c->domain) + 5 + os64_strlen(c->path) + 5 + 20 +
                os64_strlen(c->name) + os64_strlen(c->value) + 7;
    }
    int32_t *idx = os64_malloc((size_t)(n > 0 ? n : 1) * 2 * sizeof(int32_t));
    char *text = os64_malloc(need);
    if (idx == NULL || text == NULL) {
        os64_lock_release(&jar->lock);
        os64_free(scratch);
        os64_free(idx);
        os64_free(text);
        return NULL;
    }
    int32_t k = 0;
    for (int32_t i = 0; i < jar->n; i++)
        if (jar->cookies[i].pick == PICK_MATCHES)
            idx[k++] = i;
    by_age(jar->cookies, idx, idx + n, n);
    size_t at = sizeof(kFileHead) - 1;
    os64_memcpy(text, kFileHead, at);
    for (int32_t i = 0; i < n; i++)
        at += spell_line(&jar->cookies[idx[i]], text + at, need - at);
    os64_lock_release(&jar->lock);
    os64_free(scratch);
    os64_free(idx);
    text[at] = '\0';
    *len = at;
    return text;
}

// One line's cookie: the jar's own rules over what the line says, as if
// the cookie were set by its domain over a connection as safe as it
// claims. A domain cookie on a single label is refused: online only that
// host could set one, and a file cannot say which host set it.
typedef struct {
    Heard h;
    int64_t expiry;
    bool host_only;
} Line;

static int64_t line_expiry(const char *s)
{
    int64_t v = 0;
    size_t n = 0;
    // Eighteen digits at most (FILE_EXPIRY_MAX), which no sum below can
    // carry past INT64_MAX.
    for (; s[n] != '\0'; n++) {
        if (!digit(s[n]) || n == 18)
            return -1;
        v = v * 10 + (s[n] - '0');
    }
    return n == 0 ? -1 : v;
}

enum { LINE_COOKIE, LINE_NONE, LINE_REFUSED };

// `line` (NUL-terminated, the caller's to cut up) as a cookie line. NONE
// for a comment, a blank line, a session line and an expired one.
static int line_read(char *line, int64_t now, Line *out)
{
    os64_memset(out, 0, sizeof(*out));
    bool http_only = false;
    if (starts_nocase(line, os64_strlen(line), "#HttpOnly_")) {
        http_only = true;
        line += 10;
    } else if (line[0] == '#' || line[0] == '\0') {
        return LINE_NONE;
    }
    char *field[7];
    int32_t fields = 0;
    for (char *p = line;;) {
        if (fields == 7)
            return LINE_REFUSED;
        field[fields++] = p;
        while (*p != '\0' && *p != '\t')
            p++;
        if (*p == '\0')
            break;
        *p++ = '\0';
    }
    if (fields != 7)
        return LINE_REFUSED;
    int64_t expiry = line_expiry(field[4]);
    if (expiry < 0)
        return LINE_REFUSED;
    if (expiry == 0 || expiry <= now)
        return LINE_NONE;
    bool sub = os64_streq(field[1], "TRUE"), secure = os64_streq(field[3], "TRUE");
    if ((!sub && !os64_streq(field[1], "FALSE")) || (!secure && !os64_streq(field[3], "FALSE")))
        return LINE_REFUSED;
    const char *domain = field[0][0] == '.' ? field[0] + 1 : field[0];
    size_t dlen = os64_strlen(domain), plen = os64_strlen(field[2]);
    Heard *h = &out->h;
    if (dlen == 0 || dlen >= sizeof(h->domain) || plen == 0 || plen >= sizeof(h->path) || field[2][0] != '/' ||
        !field_ok(domain) || !field_ok(field[2]))
        return LINE_REFUSED;
    bool dotted = false;
    for (size_t i = 0; i < dlen; i++) {
        if (domain[i] == ' ' || domain[i] == '/' || (domain[i] == '.' && (i == 0 || domain[i - 1] == '.')))
            return LINE_REFUSED;
        h->domain[i] = lower(domain[i]);
        dotted |= domain[i] == '.';
    }
    h->domain[dlen] = '\0';
    if (sub && !dotted)
        return LINE_REFUSED;
    h->have_domain = sub;
    os64_memcpy(h->path, field[2], plen + 1);
    h->have_path = true;
    h->name = field[5];
    h->nlen = os64_strlen(field[5]);
    h->value = field[6];
    h->vlen = os64_strlen(field[6]);
    // What no Set-Cookie could have made: a name holding '=' or ';', a value
    // holding ';', each a byte where the header's parse splits, and white
    // space at either end of either, which heard() takes off.
    if (os64_strchr(field[5], '=') != NULL || os64_strchr(field[5], ';') != NULL ||
        os64_strchr(field[6], ';') != NULL || padded(h->name, h->nlen) || padded(h->value, h->vlen))
        return LINE_REFUSED;
    h->secure = secure;
    h->http_only = http_only;
    if (!judged(h, h->domain, h->path, secure))
        return LINE_REFUSED;
    out->expiry = expiry;
    out->host_only = !sub;
    return LINE_COOKIE;
}

// WHETHER A FILE KEEPS `c`: one with an expiry not yet passed at `now`
// whose line the reader takes back as the same cookie. The writer and a
// following ask this one question, so a cookie no line can hold (a byte
// that would break its line, a domain cookie on a single label) is never
// written, and is the jar's alone, as a session cookie is: no following
// lets it go for want of a line. An expired one is no line the reader
// takes either. `scratch` is WAY_JAR_LINE_MAX bytes.
static bool round_trips(const Cookie *c, int64_t now, char *scratch)
{
    if (c->expiry == INT64_MAX)
        return false;
    size_t n = spell_line(c, scratch, WAY_JAR_LINE_MAX);
    if (n == 0)
        return false;
    scratch[n - 1] = '\0';                  // the newline
    Line l;
    return line_read(scratch, now, &l) == LINE_COOKIE && l.host_only == c->host_only &&
           l.h.secure == c->secure && l.h.http_only == c->http_only && os64_streq(l.h.name, c->name) &&
           os64_streq(l.h.value, c->value) && os64_streq(l.h.domain, c->domain) &&
           os64_streq(l.h.path, c->path);
}

// A cookie this jar has a change to of its own that a file has not heard
// — set, or let go — which outranks whatever a file says of it.
static bool changed_here(const way_jar_t *jar, const Cookie *probe)
{
    for (int32_t i = 0; i < jar->n; i++) {
        const Cookie *c = &jar->cookies[i];
        if (c->changed != 0 &&
            same_cookie(c->name, c->domain, c->path, c->host_only, probe->name, probe->domain, probe->path,
                        probe->host_only))
            return true;
    }
    return gone_find(jar, probe) >= 0;
}

// `c` (from another jar, or a line) into this one in place of its
// namesake, which keeps its age and its last use; a new one is the jar's
// newest, made at `now`, and its least used. Newest, not `c`'s own age: a
// file holds no age, so the cookies a jar read from one were made when it
// was read, and an older `c` would sort before every one of them — a new
// login the oldest cookie in the file, the first the budget cuts. The
// limits are the caller's to hold once its whole batch is in
// (enforce_every). Under the lock. False on no memory, the namesake left
// as it was.
static bool put_in(way_jar_t *jar, const Cookie *c, int64_t now)
{
    int64_t created = now;
    uint64_t order = jar->next_order++, used = 0;
    int32_t same = -1;
    for (int32_t i = 0; i < jar->n && same < 0; i++) {
        const Cookie *o = &jar->cookies[i];
        if (same_cookie(o->name, o->domain, o->path, o->host_only, c->name, c->domain, c->path, c->host_only)) {
            created = o->created;
            order = o->order;
            used = o->used;
            same = i;
        }
    }
    if (insert(jar, c->name, os64_strlen(c->name), c->value, os64_strlen(c->value), c->domain, c->path) == NULL)
        return false;
    // The namesake goes now the new cookie is in; drop() moves the last
    // record, the new one, into its place.
    Cookie *n = &jar->cookies[jar->n - 1];
    if (same >= 0) {
        drop(jar, same);
        n = &jar->cookies[same];
    }
    n->expiry = c->expiry;
    n->created = created;
    n->order = order;
    n->used = used;
    n->host_only = c->host_only;
    n->secure = c->secure;
    n->http_only = c->http_only;
    n->same_site = c->same_site;
    return true;
}

// A file's text into the jar (way_jar_read), and when `follow`, every
// cookie a file keeps that the jar has no change to of its own and the
// text does not hold let go after (way_jar_follow) — but only when the
// whole text was read: a cookie a read short of memory did not reach is
// not one the file let go. The limits are held last (enforce_every).
static void read_text(way_jar_t *jar, const char *text, size_t len, int64_t now, way_jar_read_t *report,
                      bool follow)
{
    way_jar_read_t mine;
    if (report == NULL)
        report = &mine;
    os64_memset(report, 0, sizeof(*report));
    if (jar == NULL || (text == NULL && len > 0))
        return;
    char *line = os64_malloc(WAY_JAR_LINE_MAX);
    if (line == NULL) {
        report->short_of_memory = true;
        return;
    }
    os64_lock_acquire(&jar->lock);
    // Marked: the cookies a file keeps (round_trips) with no change of the
    // jar's own, which a line must name for them to stay. A replaced one
    // goes with its mark, and its replacement has none. `line` is free
    // until the first line is read.
    for (int32_t i = 0; i < jar->n; i++) {
        Cookie *c = &jar->cookies[i];
        c->pick = follow && c->changed == 0 && round_trips(c, now, line) ? PICK_MATCHES : PICK_NO;
    }
    for (size_t at = 0; at < len && !jar->refusing && !report->short_of_memory;) {
        size_t end = at;
        while (end < len && text[end] != '\n')
            end++;
        size_t n = end - at;
        if (n > 0 && text[end - 1] == '\r')
            n--;
        bool whole = n < WAY_JAR_LINE_MAX && os64_memchr(text + at, '\0', n) == NULL;
        if (whole) {
            os64_memcpy(line, text + at, n);
            line[n] = '\0';
        }
        at = end + 1;
        Line l;
        int kind = whole ? line_read(line, now, &l) : LINE_REFUSED;
        if (kind == LINE_NONE)
            continue;
        if (kind == LINE_REFUSED) {
            report->refused++;
            continue;
        }
        // The line as a cookie: its name and value are fields of `line`,
        // each ended by the NUL line_read put in its tab's place, until
        // put_in copies them.
        Cookie c = {.name = (char *)l.h.name, .value = (char *)l.h.value, .domain = l.h.domain,
                    .path = l.h.path, .expiry = l.expiry, .host_only = l.host_only,
                    .secure = l.h.secure, .http_only = l.h.http_only};
        bool read = changed_here(jar, &c) || put_in(jar, &c, now);
        report->read += read;
        report->short_of_memory |= !read;
    }
    if (follow && !jar->refusing && !report->short_of_memory) {
        for (int32_t i = 0; i < jar->n;) {
            if (jar->cookies[i].pick == PICK_MATCHES)
                drop(jar, i);
            else
                i++;
        }
    }
    enforce_every(jar);
    os64_lock_release(&jar->lock);
    os64_free(line);
}

void way_jar_read(way_jar_t *jar, const char *text, size_t len, int64_t now, way_jar_read_t *report)
{
    read_text(jar, text, len, now, report, false);
}

void way_jar_follow(way_jar_t *jar, const char *text, size_t len, int64_t now, way_jar_read_t *report)
{
    read_text(jar, text, len, now, report, true);
}

bool way_jar_overlay(way_jar_t *to, way_jar_t *from, uint64_t through, int64_t now)
{
    if (to == NULL || from == NULL || to == from)
        return false;
    bool whole = true;
    os64_lock_acquire(&from->lock);
    os64_lock_acquire(&to->lock);
    for (int32_t i = 0; i < from->ngone; i++) {
        const Gone *g = &from->gone[i];
        if (g->changed > through)
            continue;
        for (int32_t k = 0; k < to->n; k++) {
            const Cookie *c = &to->cookies[k];
            if (same_cookie(c->name, c->domain, c->path, c->host_only, g->name, g->domain, g->path, g->host_only)) {
                drop(to, k);
                break;
            }
        }
    }
    // The changes oldest first (by_age): a cookie new to `to` is its newest
    // as it goes in (put_in), so the order they go in is the order the file
    // keeps their ages in, and where `from` happens to hold them (a drop
    // moves its last record) must not decide which a restart calls newest.
    int32_t *idx = os64_malloc((size_t)(from->n > 0 ? from->n : 1) * 2 * sizeof(int32_t));
    int32_t n = 0;
    whole = idx != NULL;
    for (int32_t i = 0; i < from->n && whole; i++)
        if (from->cookies[i].changed != 0 && from->cookies[i].changed <= through)
            idx[n++] = i;
    if (whole)
        by_age(from->cookies, idx, idx + n, n);
    for (int32_t i = 0; i < n && whole; i++) {
        const Cookie *c = &from->cookies[idx[i]];
        if (c->expiry != INT64_MAX && c->expiry > now) {
            whole = put_in(to, c, now);
            continue;
        }
        // A session cookie in a kept one's place: the kept one goes.
        for (int32_t k = 0; k < to->n; k++) {
            const Cookie *o = &to->cookies[k];
            if (same_cookie(o->name, o->domain, o->path, o->host_only, c->name, c->domain, c->path, c->host_only)) {
                drop(to, k);
                break;
            }
        }
    }
    enforce_every(to);
    os64_lock_release(&to->lock);
    os64_lock_release(&from->lock);
    os64_free(idx);
    return whole;
}

uint64_t way_jar_changes(way_jar_t *jar)
{
    if (jar == NULL)
        return 0;
    os64_lock_acquire(&jar->lock);
    uint64_t n = jar->changes;
    os64_lock_release(&jar->lock);
    return n;
}

void way_jar_saved(way_jar_t *jar, uint64_t through)
{
    if (jar == NULL)
        return;
    os64_lock_acquire(&jar->lock);
    for (int32_t i = 0; i < jar->n; i++)
        if (jar->cookies[i].changed <= through)
            jar->cookies[i].changed = 0;
    for (int32_t i = 0; i < jar->ngone;)
        if (jar->gone[i].changed <= through)
            gone_drop(jar, i);
        else
            i++;
    os64_lock_release(&jar->lock);
}

void way_jar_clear(way_jar_t *jar)
{
    if (jar == NULL)
        return;
    os64_lock_acquire(&jar->lock);
    for (int32_t i = 0; i < jar->n; i++)
        os64_free(jar->cookies[i].name);
    jar->n = 0;
    gone_clear(jar);
    os64_lock_release(&jar->lock);
}

void way_jar_refuse(way_jar_t *jar, bool refuse)
{
    if (jar == NULL)
        return;
    os64_lock_acquire(&jar->lock);
    jar->refusing = refuse;
    os64_lock_release(&jar->lock);
}

// ── A hop's lines ───────────────────────────────────────────────────────

int32_t way_hop_headers(way_jar_t *jar, const char *referrer, const os64_url_t *to,
                        bool encrypted, int64_t now, char *out, size_t cap)
{
    static const char kCookie[] = "Cookie: ", kReferer[] = "Referer: ";
    int32_t left = 0;
    size_t used = 0;
    if (cap == 0)
        return 0;
    out[0] = '\0';
    // "Cookie: " + the value + "\r\n" + the NUL, within WAY_COOKIE_LINE_MAX
    // whatever room the caller has: the rest is the Referer's.
    size_t line_cap = cap < WAY_COOKIE_LINE_MAX ? cap : WAY_COOKIE_LINE_MAX;
    if (line_cap > sizeof(kCookie) + 2) {
        size_t room = line_cap - (sizeof(kCookie) - 1) - 2;
        size_t n = way_jar_cookies(jar, to, encrypted, now, out + sizeof(kCookie) - 1, room, &left);
        if (n > 0) {
            os64_memcpy(out, kCookie, sizeof(kCookie) - 1);
            used = sizeof(kCookie) - 1 + n;
            out[used++] = '\r';
            out[used++] = '\n';
        }
        out[used] = '\0';
    }
    char ref[OS64_URL_REF_MAX];
    if (referrer != NULL && referrer[0] != '\0' && way_referrer(referrer, to, ref, sizeof(ref))) {
        size_t rn = os64_strlen(ref);
        if (used + sizeof(kReferer) - 1 + rn + 2 < cap) {
            os64_memcpy(out + used, kReferer, sizeof(kReferer) - 1);
            used += sizeof(kReferer) - 1;
            os64_memcpy(out + used, ref, rn);
            used += rn;
            out[used++] = '\r';
            out[used++] = '\n';
            out[used] = '\0';
        }
    }
    return left;
}

void way_cookies_short_note(way_cookies_short_t *s, const os64_url_t *to, int32_t left)
{
    if (s == NULL || to == NULL || left <= s->left)
        return;
    s->left = left;
    os64_strcopy(s->host, sizeof(s->host), to->host);
}

// ── Referer ─────────────────────────────────────────────────────────────

static uint16_t port_of(const os64_url_t *u)
{
    return u->port != 0 ? u->port : os64_streq(u->scheme, "https") ? 443 : 80;
}

bool way_referrer(const char *from, const os64_url_t *to, char *out, size_t cap)
{
    os64_url_t page;
    if (from == NULL || to == NULL || os64_url_parse(from, &page) != OS64_URL_OK)
        return false;
    bool web = os64_streq(page.scheme, "http") || os64_streq(page.scheme, "https");
    if (!web || (os64_streq(page.scheme, "https") && !os64_streq(to->scheme, "https")))
        return false;
    if (os64_streq(page.scheme, to->scheme) && os64_streq(page.host, to->host) &&
        port_of(&page) == port_of(to))
        return os64_url_spell(&page, out, cap);
    os64_strcopy(page.path, sizeof(page.path), "/");
    return os64_url_spell(&page, out, cap);
}
