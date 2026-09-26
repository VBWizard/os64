// jar.c — the cookie jar and the Referer policy (way/jar.h).
//
// RFC 6265's sections are cited where their rule is applied; where the
// 6265bis draft differs, the browsers ship the draft, and so does this.

#include "way/jar.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef enum { SAMESITE_DEFAULT, SAMESITE_NONE, SAMESITE_LAX, SAMESITE_STRICT } SameSite;

typedef struct {
    char *name, *value, *domain, *path;     // one allocation, `name` its start
    int64_t expiry;                         // INT64_MAX: until the browser closes
    int64_t created;                        // seconds, and `order` within one
    uint64_t order;
    bool host_only, secure, http_only;
    SameSite same_site;
} Cookie;

struct way_jar {
    os64_lock_t lock;
    Cookie *cookies;
    int32_t n, cap;
    uint64_t next_order;
};

way_jar_t *way_jar_new(void)
{
    return os64_calloc(1, sizeof(way_jar_t));
}

void way_jar_free(way_jar_t *jar)
{
    if (jar == NULL)
        return;
    for (int32_t i = 0; i < jar->n; i++)
        os64_free(jar->cookies[i].name);
    os64_free(jar->cookies);
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

static void purge_expired(way_jar_t *jar, int64_t now)
{
    for (int32_t i = 0; i < jar->n;)
        if (jar->cookies[i].expiry <= now)
            drop(jar, i);
        else
            i++;
}

// Past a limit, the oldest goes: in the domain for the domain's limit, in
// the jar for the jar's.
static void enforce_limits(way_jar_t *jar, const char *domain)
{
    for (;;) {
        int32_t count = 0, oldest = -1;
        for (int32_t i = 0; i < jar->n; i++) {
            if (!os64_streq(jar->cookies[i].domain, domain))
                continue;
            count++;
            if (oldest < 0 || older(&jar->cookies[i], &jar->cookies[oldest]))
                oldest = i;
        }
        if (count <= WAY_COOKIES_PER_DOMAIN)
            break;
        drop(jar, oldest);
    }
    while (jar->n > WAY_COOKIES_MAX) {
        int32_t oldest = 0;
        for (int32_t i = 1; i < jar->n; i++)
            if (older(&jar->cookies[i], &jar->cookies[oldest]))
                oldest = i;
        drop(jar, oldest);
    }
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
    if (h->nlen + h->vlen == 0 || h->nlen + h->vlen > WAY_COOKIE_BYTES_MAX ||
        !clean(h->name, h->nlen) || !clean(h->value, h->vlen))
        return false;
    attributes(h, semi < end ? semi + 1 : end, end, now);

    // §5.3 step 6: a Domain the setting host is not in is refused, and so
    // is one naming a single label that is not the host itself — the most
    // this jar can tell of a public suffix without the list (booked).
    if (h->have_domain) {
        if (ip_literal(from->host) ? !os64_streq(from->host, h->domain)
                                   : !domain_match(from->host, h->domain))
            return false;
        bool dotted = false;
        for (const char *p = h->domain; *p != '\0'; p++)
            dotted |= *p == '.';
        if (!dotted && !os64_streq(from->host, h->domain))
            return false;
    }
    if (!h->have_path)
        default_path(from->path, h->path, sizeof(h->path));
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

void way_jar_hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                  size_t len, int64_t now)
{
    if (jar == NULL || from == NULL || value == NULL)
        return;
    Heard h;
    if (!heard(&h, from, encrypted, value, len, now))
        return;
    const char *domain = h.have_domain ? h.domain : from->host;
    int64_t expiry = h.have_max_age ? h.max_age_expiry : h.have_expires ? h.expires : INT64_MAX;

    os64_lock_acquire(&jar->lock);
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
        if (os64_streq(c->domain, domain) && os64_streq(c->path, h.path) &&
            c->host_only == !h.have_domain)
            same = i;
    }
    // §5.3 step 11: a cookie replaces its namesake, keeping its age; one
    // already expired deletes it and is not kept.
    int64_t created = now;
    uint64_t order = jar->next_order++;
    if (same >= 0) {
        created = jar->cookies[same].created;
        order = jar->cookies[same].order;
        drop(jar, same);
    }
    if (expiry <= now) {
        os64_lock_release(&jar->lock);
        return;
    }
    size_t dlen = os64_strlen(domain), plen = os64_strlen(h.path);
    char *block = os64_malloc(h.nlen + h.vlen + dlen + plen + 4);
    bool room = block != NULL;
    if (room && jar->n == jar->cap) {
        int32_t cap = jar->cap > 0 ? jar->cap * 2 : 16;
        Cookie *grown = os64_realloc(jar->cookies, (size_t)cap * sizeof(Cookie));
        room = grown != NULL;
        if (room) {
            jar->cookies = grown;
            jar->cap = cap;
        }
    }
    if (!room) {
        os64_free(block);
        os64_lock_release(&jar->lock);
        return;
    }
    Cookie *c = &jar->cookies[jar->n++];
    c->name = block;
    os64_memcpy(c->name, h.name, h.nlen);
    c->name[h.nlen] = '\0';
    c->value = c->name + h.nlen + 1;
    os64_memcpy(c->value, h.value, h.vlen);
    c->value[h.vlen] = '\0';
    c->domain = c->value + h.vlen + 1;
    os64_memcpy(c->domain, domain, dlen + 1);
    c->path = c->domain + dlen + 1;
    os64_memcpy(c->path, h.path, plen + 1);
    c->expiry = expiry;
    c->created = created;
    c->order = order;
    c->host_only = !h.have_domain;
    c->secure = h.secure;
    c->http_only = h.http_only;
    c->same_site = h.same_site;
    enforce_limits(jar, c->domain);
    os64_lock_release(&jar->lock);
}

// ── Sending ─────────────────────────────────────────────────────────────

// §5.4 step 2: the longer path first, then the older.
static bool before(const Cookie *a, const Cookie *b)
{
    size_t la = os64_strlen(a->path), lb = os64_strlen(b->path);
    return la != lb ? la > lb : older(a, b);
}

size_t way_jar_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                       char *out, size_t cap, int32_t *left_out)
{
    if (left_out != NULL)
        *left_out = 0;
    if (cap > 0)
        out[0] = '\0';
    if (jar == NULL || to == NULL || cap == 0)
        return 0;
    size_t rn = path_length(to->path);
    size_t used = 0;
    os64_lock_acquire(&jar->lock);
    purge_expired(jar, now);
    // Sent in order without sorting the jar: each pass takes the first
    // matching cookie after the last one taken. The jar is small, and a
    // request asks once.
    const Cookie *last = NULL;
    for (;;) {
        const Cookie *next = NULL;
        for (int32_t i = 0; i < jar->n; i++) {
            const Cookie *c = &jar->cookies[i];
            bool host = c->host_only ? os64_streq(to->host, c->domain)
                                     : domain_match(to->host, c->domain);
            if (!host || !path_match(to->path, rn, c->path) || (c->secure && !encrypted))
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
        size_t need = (used > 0 ? 2 : 0) + nlen + (nlen > 0 ? 1 : 0) + vlen;
        if (used + need + 1 > cap) {
            if (left_out != NULL)
                (*left_out)++;
            continue;
        }
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
    }
    os64_lock_release(&jar->lock);
    return used;
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
    // "Cookie: " + the value + "\r\n" + the NUL.
    if (cap > sizeof(kCookie) + 2) {
        size_t room = cap - (sizeof(kCookie) - 1) - 2;
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
