// diag.c — one page load's record, and the file it renders (diag.h).

#include "diag.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

typedef struct {
    char *a, *b;            // MISSING: kind, name; FAILED: what, message; fact: key, value
    uint32_t count;
} Line;

typedef struct {
    Line *lines;
    uint32_t n, cap, lost;
} Table;

struct yonder_diag {
    char *url;
    uint32_t seq;
    int64_t began_us;
    Table missing, failed, facts;
};

static char *copy(const char *s)
{
    size_t n = os64_strlen(s) + 1;
    char *c = os64_malloc(n);
    if (c != NULL)
        os64_memcpy(c, s, n);
    return c;
}

static void table_free(Table *t)
{
    for (uint32_t i = 0; i < t->n; i++) {
        os64_free(t->lines[i].a);
        os64_free(t->lines[i].b);
    }
    os64_free(t->lines);
}

// The line keyed `a` (and `b`, when `both`), found or added. NULL when the
// table is full or memory ran out: the line is then counted as lost.
static Line *table_line(Table *t, const char *a, const char *b, bool both, uint32_t max)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (os64_streq(t->lines[i].a, a) && (!both || os64_streq(t->lines[i].b, b)))
            return &t->lines[i];
    if (t->n == max) {
        t->lost++;
        return NULL;
    }
    if (t->n == t->cap) {
        uint32_t bigger = t->cap != 0 ? t->cap * 2 : 16;
        Line *next = os64_malloc((size_t)bigger * sizeof(Line));
        if (next == NULL) {
            t->lost++;
            return NULL;
        }
        if (t->n != 0)
            os64_memcpy(next, t->lines, (size_t)t->n * sizeof(Line));
        os64_free(t->lines);
        t->lines = next;
        t->cap = bigger;
    }
    Line one = {copy(a), copy(b), 0};
    if (one.a == NULL || one.b == NULL) {
        os64_free(one.a);
        os64_free(one.b);
        t->lost++;
        return NULL;
    }
    t->lines[t->n] = one;
    return &t->lines[t->n++];
}

yonder_diag_t *yonder_diag_new(const char *url, uint32_t seq, int64_t began_us)
{
    yonder_diag_t *d = os64_calloc(1, sizeof(*d));
    if (d == NULL)
        return NULL;
    d->url = copy(url != NULL ? url : "");
    if (d->url == NULL) {
        os64_free(d);
        return NULL;
    }
    d->seq = seq;
    d->began_us = began_us;
    return d;
}

void yonder_diag_free(yonder_diag_t *d)
{
    if (d == NULL)
        return;
    table_free(&d->missing);
    table_free(&d->failed);
    table_free(&d->facts);
    os64_free(d->url);
    os64_free(d);
}

uint32_t yonder_diag_seq(const yonder_diag_t *d)
{
    return d != NULL ? d->seq : 0;
}

int64_t yonder_diag_began(const yonder_diag_t *d)
{
    return d != NULL ? d->began_us : 0;
}

static uint32_t saturating(uint32_t a, uint32_t b)
{
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

void yonder_diag_missing(yonder_diag_t *d, const char *kind, const char *name, uint32_t times)
{
    Line *l = d != NULL ? table_line(&d->missing, kind, name, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL)
        l->count = saturating(l->count, times);
}

void yonder_diag_missing_seen(yonder_diag_t *d, const char *kind, const char *name, uint32_t count)
{
    Line *l = d != NULL ? table_line(&d->missing, kind, name, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL && count > l->count)
        l->count = count;
}

void yonder_diag_failed(yonder_diag_t *d, const char *what, const char *message)
{
    Line *l = d != NULL ? table_line(&d->failed, what, message, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL)
        l->count = saturating(l->count, 1);
}

void yonder_diag_fact(yonder_diag_t *d, const char *key, const char *value)
{
    if (d == NULL)
        return;
    Line *l = table_line(&d->facts, key, "", false, YONDER_DIAG_FACTS_MAX);
    char *v = l != NULL ? copy(value) : NULL;
    if (v == NULL)
        return;
    os64_free(l->b);
    l->b = v;
}

uint32_t yonder_diag_missing_lines(const yonder_diag_t *d)
{
    return d != NULL ? d->missing.n : 0;
}

uint32_t yonder_diag_failed_lines(const yonder_diag_t *d)
{
    return d != NULL ? d->failed.n : 0;
}

// ── The text ────────────────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, at;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++, o->at++)
        if (o->at + 1 < o->cap)
            o->out[o->at] = s[i];
}

static void puts_(Out *o, const char *s)
{
    put(o, s, os64_strlen(s));
}

static bool starts(const char *s, const char *word)
{
    size_t n = os64_strlen(word);
    return os64_memcmp(s, word, n) == 0;
}

// `s` as one line holds it: a byte that would end or bend a line, and a
// backslash, spelled as an escape, and a token spelled with its second
// letter percent-encoded wherever it appears in data, record lines' data
// included, so a grep for a token finds only the records it begins.
static void put_text(Out *o, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (starts(s, YONDER_DIAG_MISSING) || starts(s, YONDER_DIAG_FAILED)) {
            char spelled[4] = {s[0], '%', hex[(unsigned char)s[1] >> 4], hex[(unsigned char)s[1] & 15]};
            put(o, spelled, 4);
            s++;
        } else if (c == '\\') {
            puts_(o, "\\\\");
        } else if (c < 0x20 || c == 0x7F) {
            char esc[4] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            put(o, esc, 4);
        } else {
            put(o, s, 1);
        }
    }
}

static void put_count(Out *o, uint32_t n)
{
    char num[24];
    os64_snprintf(num, sizeof(num), " (%lu)\n", (unsigned long)n);
    puts_(o, num);
}

static void put_lost(Out *o, const char *what, uint32_t lost)
{
    if (lost == 0)
        return;
    char line[96];
    os64_snprintf(line, sizeof(line), "%s not kept: %lu\n", what, (unsigned long)lost);
    puts_(o, line);
}

size_t yonder_diag_render(const yonder_diag_t *d, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    puts_(&o, "address: ");
    put_text(&o, d->url);
    puts_(&o, "\n");
    if (d->missing.n == 0 && d->failed.n == 0) {
        puts_(&o, "verdict: clean\n");
    } else {
        // Only a token with records is named, so the verdict never answers
        // a grep for the other one.
        char line[96];
        if (d->missing.n != 0 && d->failed.n != 0)
            os64_snprintf(line, sizeof(line), "verdict: %lu " YONDER_DIAG_MISSING ", %lu " YONDER_DIAG_FAILED "\n",
                          (unsigned long)d->missing.n, (unsigned long)d->failed.n);
        else
            os64_snprintf(line, sizeof(line), "verdict: %lu %s\n",
                          (unsigned long)(d->missing.n != 0 ? d->missing.n : d->failed.n),
                          d->missing.n != 0 ? YONDER_DIAG_MISSING : YONDER_DIAG_FAILED);
        puts_(&o, line);
    }
    for (uint32_t i = 0; i < d->missing.n; i++) {
        const Line *l = &d->missing.lines[i];
        puts_(&o, YONDER_DIAG_MISSING " ");
        put_text(&o, l->a);
        puts_(&o, " ");
        put_text(&o, l->b);
        put_count(&o, l->count);
    }
    for (uint32_t i = 0; i < d->failed.n; i++) {
        const Line *l = &d->failed.lines[i];
        puts_(&o, YONDER_DIAG_FAILED " ");
        put_text(&o, l->a);
        puts_(&o, ": ");
        put_text(&o, l->b);
        put_count(&o, l->count);
    }
    for (uint32_t i = 0; i < d->facts.n; i++) {
        put_text(&o, d->facts.lines[i].a);
        puts_(&o, ": ");
        put_text(&o, d->facts.lines[i].b);
        puts_(&o, "\n");
    }
    // What met no room is said, in words that are neither token: the
    // verdict already counts every line that was kept.
    put_lost(&o, "missing names", d->missing.lost);
    put_lost(&o, "failures", d->failed.lost);
    put_lost(&o, "facts", d->facts.lost);
    if (cap != 0)
        out[o.at < cap ? o.at : cap - 1] = '\0';
    return o.at;
}

static bool begins(const uint8_t *bytes, size_t len, size_t at, const char *magic)
{
    size_t n = os64_strlen(magic);
    return len >= at + n && os64_memcmp(bytes + at, magic, n) == 0;
}

// An SVG is text, so it is found by its element after any XML
// declaration, white space, comments or doctype in its first kilobyte.
const char *yonder_diag_image_format(const uint8_t *bytes, size_t len)
{
    if (begins(bytes, len, 0, "RIFF") && begins(bytes, len, 8, "WEBP"))
        return "webp";
    if (begins(bytes, len, 4, "ftypavif") || begins(bytes, len, 4, "ftypavis"))
        return "avif";
    if (len >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1 && bytes[3] == 0)
        return "ico";
    if (len >= 4 && ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == '*' && bytes[3] == 0) ||
                     (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == '*')))
        return "tiff";
    for (size_t i = 0; i + 4 <= len && i < 1024; i++)
        if (os64_memcmp(bytes + i, "<svg", 4) == 0)
            return "svg";
    return "unknown";
}

// ── The file ────────────────────────────────────────────────────────────

bool yonder_diag_file_name(const yonder_diag_t *d, char *out, size_t cap)
{
    char host[64];
    size_t n = 0;
    const char *s = d->url;
    const char *scheme_end = s;
    while (*scheme_end != '\0' && *scheme_end != ':')
        scheme_end++;
    bool file = scheme_end - s == 4 && os64_memcmp(s, "file", 4) == 0;
    if (!file && scheme_end[0] == ':' && scheme_end[1] == '/' && scheme_end[2] == '/') {
        const char *h = scheme_end + 3;
        const char *end = h;
        while (*end != '\0' && *end != '/' && *end != '?' && *end != '#')
            end++;
        // Userinfo is not the host, and a port is not either.
        for (const char *at = h; at < end; at++)
            if (*at == '@')
                h = at + 1;
        for (; h < end && *h != ':' && n + 1 < sizeof(host); h++) {
            char c = *h;
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            host[n++] = keep ? c : '_';
        }
    }
    host[n] = '\0';
    int w = os64_snprintf(out, cap, "%s-%04lu.txt", file ? "file" : n != 0 ? host : "page",
                          (unsigned long)d->seq);
    return w > 0 && (size_t)w < cap;
}

static bool write_all(int32_t h, const char *p, size_t n)
{
    while (n != 0) {
        int64_t w = os64_write(h, p, n);
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

int64_t yonder_diag_write(const yonder_diag_t *d, const char *dir)
{
    char name[96], path[OS64_PATH_MAX], temp[OS64_PATH_MAX];
    if (d == NULL || dir == NULL || !yonder_diag_file_name(d, name, sizeof(name)))
        return -1;
    int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, name);
    int t = os64_snprintf(temp, sizeof(temp), "%s.%lu.new", path, (unsigned long)os64_taskid());
    if (n <= 0 || (size_t)n >= sizeof(path) || t <= 0 || (size_t)t >= sizeof(temp))
        return -1;
    size_t need = yonder_diag_render(d, NULL, 0);
    char *text = os64_malloc(need + 1);
    if (text == NULL)
        return -1;
    yonder_diag_render(d, text, need + 1);
    int64_t h = os64_open(temp, "w");
    bool ok = h >= 0 && write_all((int32_t)h, text, need);
    if (h >= 0)
        ok = os64_close((int32_t)h) == 0 && ok;
    ok = ok && os64_rename(temp, path) == 0;
    if (!ok && h >= 0)
        (void)os64_unlink(temp);
    os64_free(text);
    return ok ? 0 : -1;
}

// The sequence a name `<anything>-<digits>.txt` carries, 0 for any other.
static uint32_t seq_of(const char *name)
{
    size_t n = os64_strlen(name);
    if (n < 6 || !os64_streq(name + n - 4, ".txt"))
        return 0;
    size_t end = n - 4, at = end;
    while (at > 0 && name[at - 1] >= '0' && name[at - 1] <= '9')
        at--;
    if (at == end || at == 0 || name[at - 1] != '-' || end - at > 9)
        return 0;
    uint32_t v = 0;
    for (size_t i = at; i < end; i++)
        v = v * 10 + (uint32_t)(name[i] - '0');
    return v;
}

uint32_t yonder_diag_next_seq(const char *dir)
{
    int64_t h = os64_opendir(dir);
    if (h < 0)
        return 1;
    uint32_t high = 0;
    os64_dirent_t e;
    while (os64_readdir((int32_t)h, &e) == 1) {
        uint32_t s = seq_of(e.name);
        if (s > high)
            high = s;
    }
    os64_close((int32_t)h);
    return high < UINT32_MAX ? high + 1 : high;
}
