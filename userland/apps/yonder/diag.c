// diag.c — one page load's record, and the file it renders (diag.h).

#include "diag.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

typedef struct {
    char *a, *b;            // MISSING: kind, name; FAILED: what, message; fact: key, value
    uint32_t count;         // a fact's: 1 when its latest value had no memory to keep
} Line;

// The lines that met no room are counted as distinct lines, as the file
// says them: each is kept by its key (`gone`) so a line asked for again —
// a condition looked at once a layout, a section written again whole —
// is not counted again. Past GONE_MAX of them, or with no memory to keep
// one, a line cannot be told from one counted already, so the count
// stands and the file says there are more (`more`).
#define GONE_MAX 4096u
typedef struct {
    Line *lines;
    uint32_t n, cap;
    Line *gone;
    uint32_t ngone, cap_gone;
    bool more;
} Table;

struct yonder_diag {
    char *url;
    uint32_t seq;
    int64_t began_us;
    Table missing, failed, facts;
    // A line went with no memory even to name it (yonder_diag_short).
    bool short_of_memory;
    // THE PAGE KEPT (diag.h): the folder its name claimed (yonder_diag_
    // claim; NULL when none was), the temporary file the page's bytes are
    // appended to while its parser is fed them (`open`, its `handle` and
    // path), whether they were renamed into place (`ended`), how many
    // sub-resources are kept and the index's lines, and whether a step
    // failed, which ends the keeping of this page.
    struct {
        char *folder;
        bool open, ended, failed;
        int32_t handle;
        char *temp;
        uint32_t resources;
        char *index;
        size_t index_len;
    } keep;
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
    for (uint32_t i = 0; i < t->ngone; i++) {
        os64_free(t->gone[i].a);
        os64_free(t->gone[i].b);
    }
    os64_free(t->gone);
}

static bool same_key(const Line *l, const char *a, const char *b, bool both)
{
    return os64_streq(l->a, a) && (!both || os64_streq(l->b, b));
}

// The line keyed `a` (and `b`) met no room: counted once, however often it
// is asked for.
static void table_lose(Table *t, const char *a, const char *b, bool both)
{
    for (uint32_t i = 0; i < t->ngone; i++)
        if (same_key(&t->gone[i], a, b, both))
            return;
    if (t->ngone == t->cap_gone && t->ngone < GONE_MAX) {
        uint32_t bigger = t->cap_gone != 0 ? t->cap_gone * 2 : 16;
        Line *next = os64_malloc((size_t)bigger * sizeof(Line));
        if (next != NULL) {
            if (t->ngone != 0)
                os64_memcpy(next, t->gone, (size_t)t->ngone * sizeof(Line));
            os64_free(t->gone);
            t->gone = next;
            t->cap_gone = bigger;
        }
    }
    Line one = {t->ngone < t->cap_gone ? copy(a) : NULL, t->ngone < t->cap_gone ? copy(b) : NULL, 0};
    if (one.a == NULL || one.b == NULL) {
        os64_free(one.a);
        os64_free(one.b);
        t->more = true;
        return;
    }
    t->gone[t->ngone++] = one;
}

// A key given room after all is no longer lost.
static void table_found(Table *t, const char *a, const char *b, bool both)
{
    for (uint32_t i = 0; i < t->ngone; i++)
        if (same_key(&t->gone[i], a, b, both)) {
            os64_free(t->gone[i].a);
            os64_free(t->gone[i].b);
            t->gone[i] = t->gone[--t->ngone];
            return;
        }
}

// The line keyed `a` (and `b`, when `both`), found or added. NULL when the
// table holds `max` lines already (more, when lines kept past the others'
// room were added: yonder_diag_head_fact) or memory ran out: the line is
// then counted as lost, once.
static Line *table_line(Table *t, const char *a, const char *b, bool both, uint32_t max)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (same_key(&t->lines[i], a, b, both))
            return &t->lines[i];
    if (t->n >= max) {
        table_lose(t, a, b, both);
        return NULL;
    }
    if (t->n == t->cap) {
        uint32_t bigger = t->cap != 0 ? t->cap * 2 : 16;
        Line *next = os64_malloc((size_t)bigger * sizeof(Line));
        if (next == NULL) {
            table_lose(t, a, b, both);
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
        table_lose(t, a, b, both);
        return NULL;
    }
    table_found(t, a, b, both);
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

static void keep_abandon(yonder_diag_t *d);

void yonder_diag_free(yonder_diag_t *d)
{
    if (d == NULL)
        return;
    keep_abandon(d);
    // A folder that kept nothing goes; its name stays the record's, whose
    // file holds it.
    if (d->keep.folder != NULL && d->keep.resources == 0)
        (void)os64_unlink(d->keep.folder);
    os64_free(d->keep.folder);
    os64_free(d->keep.index);
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

// A plain line set, in a table of `max` lines.
static void fact_set(yonder_diag_t *d, const char *key, const char *value, uint32_t max)
{
    if (d == NULL)
        return;
    Line *l = table_line(&d->facts, key, "", false, max);
    if (l == NULL)
        return;
    // A value with no memory to keep is not left empty, nor the one before
    // it standing as if it were this one: the line says it went.
    char *v = copy(value);
    l->count = v == NULL;
    if (v == NULL)
        return;
    os64_free(l->b);
    l->b = v;
}

void yonder_diag_fact(yonder_diag_t *d, const char *key, const char *value)
{
    fact_set(d, key, value, YONDER_DIAG_FACTS_MAX);
}

void yonder_diag_head_fact(yonder_diag_t *d, const char *key, const char *value)
{
    fact_set(d, key, value, YONDER_DIAG_FACTS_MAX + YONDER_DIAG_HEAD_FACTS);
}

void yonder_diag_forget(yonder_diag_t *d, const char *prefix)
{
    if (d == NULL)
        return;
    size_t len = os64_strlen(prefix);
    // The lines, and the lines that met no room: the set is asked for
    // again whole.
    Line *lists[2] = {d->facts.lines, d->facts.gone};
    uint32_t *counts[2] = {&d->facts.n, &d->facts.ngone};
    for (int k = 0; k < 2; k++) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < *counts[k]; i++) {
            Line *l = &lists[k][i];
            if (os64_strlen(l->a) >= len && os64_memcmp(l->a, prefix, len) == 0) {
                os64_free(l->a);
                os64_free(l->b);
            } else {
                lists[k][kept++] = *l;
            }
        }
        *counts[k] = kept;
    }
}

void yonder_diag_short(yonder_diag_t *d)
{
    if (d != NULL)
        d->short_of_memory = true;
}

void yonder_diag_failed_seen(yonder_diag_t *d, const char *what, const char *message)
{
    Line *l = d != NULL ? table_line(&d->failed, what, message, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL && l->count == 0)
        l->count = 1;
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
// backslash, spelled as an escape, and, with `tokens`, a token spelled with
// its second letter percent-encoded wherever it appears in data, record
// lines' data included, so a grep for a token finds only the records it
// begins. With `tokens`, a `%` of the data's own that a reader would take
// for part of a spelling is written `%25`: one that begins `%25` itself, or
// that stands where a token's spelled letter does (`M%49SSING`,
// `F%41ILED`). That makes the spelling injective, so a literal `M%49SSING`
// in an address reads back as itself (`M%2549SSING`), never as MISSING,
// while every other `%` (`75%`, `%20`) is written as it is. A reader
// decodes `M%49SSING`, `F%41ILED` and `%25` in one pass from the left. A
// kept page's index spells no token: its addresses are matched byte for
// byte, and nothing greps it for records.
static void put_field(Out *o, const char *s, bool tokens)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *first = s;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (tokens && (starts(s, YONDER_DIAG_MISSING) || starts(s, YONDER_DIAG_FAILED))) {
            char spelled[4] = {s[0], '%', hex[(unsigned char)s[1] >> 4], hex[(unsigned char)s[1] & 15]};
            put(o, spelled, 4);
            s++;
        } else if (tokens && c == '%' &&
                   (starts(s + 1, "25") || (s > first && s[-1] == 'M' && starts(s + 1, "49SSING")) ||
                    (s > first && s[-1] == 'F' && starts(s + 1, "41ILED")))) {
            puts_(o, "%25");
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

static void put_text(Out *o, const char *s)
{
    put_field(o, s, true);
}

static void put_count(Out *o, uint32_t n)
{
    char num[24];
    os64_snprintf(num, sizeof(num), " (%lu)\n", (unsigned long)n);
    puts_(o, num);
}

static void put_lost(Out *o, const char *what, const Table *t)
{
    if (t->ngone == 0 && !t->more)
        return;
    char line[96];
    os64_snprintf(line, sizeof(line), "%s not kept: %lu%s\n", what, (unsigned long)t->ngone,
                  t->more ? ", and more not counted" : "");
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
        if (d->facts.lines[i].count != 0)
            puts_(&o, "no memory to write this line");
        else
            put_text(&o, d->facts.lines[i].b);
        puts_(&o, "\n");
    }
    // What met no room is said, in words that are neither token: the
    // verdict already counts every line that was kept.
    put_lost(&o, "missing names", &d->missing);
    put_lost(&o, "failures", &d->failed);
    put_lost(&o, "facts", &d->facts);
    if (d->short_of_memory)
        puts_(&o, "some lines not kept: no memory to name them\n");
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

// The temporary file beside `path` a whole write goes through: named for
// this task, so two yonders writing one name never share one.
static bool temp_beside(const char *path, char *out, size_t cap)
{
    int t = os64_snprintf(out, cap, "%s.%lu.new", path, (unsigned long)os64_taskid());
    return t > 0 && (size_t)t < cap;
}

// `n` bytes written whole as `path`: into the temporary file beside it,
// renamed over it, so a reader never meets half a file. The temporary is
// removed when a step fails.
static bool write_whole(const char *path, const void *bytes, size_t n)
{
    char temp[OS64_PATH_MAX];
    if (!temp_beside(path, temp, sizeof(temp)))
        return false;
    int64_t h = os64_open(temp, "w");
    bool ok = h >= 0 && write_all((int32_t)h, bytes, n);
    if (h >= 0)
        ok = os64_close((int32_t)h) == 0 && ok;
    ok = ok && os64_rename(temp, path) == 0;
    if (!ok && h >= 0)
        (void)os64_unlink(temp);
    return ok;
}

int64_t yonder_diag_write(const yonder_diag_t *d, const char *dir)
{
    char name[96], path[OS64_PATH_MAX];
    if (d == NULL || dir == NULL || !yonder_diag_file_name(d, name, sizeof(name)))
        return -1;
    int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return -1;
    size_t need = yonder_diag_render(d, NULL, 0);
    char *text = os64_malloc(need + 1);
    if (text == NULL)
        return -1;
    yonder_diag_render(d, text, need + 1);
    bool ok = write_whole(path, text, need);
    os64_free(text);
    return ok ? 0 : -1;
}

// ── The page kept ───────────────────────────────────────────────────────

// `<dir>/<name><suffix>`, where `<name>` is the page file's name without
// its `.txt`.
static bool kept_path(const yonder_diag_t *d, const char *dir, const char *suffix, char *out, size_t cap)
{
    char name[96];
    if (dir == NULL || !yonder_diag_file_name(d, name, sizeof(name)))
        return false;
    name[os64_strlen(name) - 4] = '\0';
    int n = os64_snprintf(out, cap, "%s/%s%s", dir, name, suffix);
    return n > 0 && (size_t)n < cap;
}

static int64_t keep_failed(yonder_diag_t *d);

// The record's file made at `path` only if nothing is there, with the
// record as it stands in it, so the name is claimed by a file that reads as
// a record from the start. False when something is there, or it could not
// be made.
static bool claim_file(const yonder_diag_t *d, const char *path)
{
    int64_t h = os64_open(path, "x");
    if (h < 0)
        return false;
    size_t need = yonder_diag_render(d, NULL, 0);
    char *text = os64_malloc(need + 1);
    if (text != NULL) {
        yonder_diag_render(d, text, need + 1);
        (void)write_all((int32_t)h, text, need);
        os64_free(text);
    }
    (void)os64_close((int32_t)h);
    return true;
}

int64_t yonder_diag_claim(yonder_diag_t *d, const char *dir, bool keep)
{
    if (d == NULL || dir == NULL)
        return 0;
    char path[OS64_PATH_MAX], html[OS64_PATH_MAX], folder[OS64_PATH_MAX];
    os64_dirent_t e;
    uint32_t first = d->seq;
    for (uint32_t tries = 0; tries < YONDER_DIAG_CLAIM_TRIES; tries++, d->seq++) {
        if (!kept_path(d, dir, ".txt", path, sizeof(path)) || !claim_file(d, path))
            continue;
        if (!keep)
            return 0;
        // Its kept files' names must be fresh too: the folder made here,
        // and no page's bytes by that name left from before.
        if (kept_path(d, dir, ".html", html, sizeof(html)) && os64_stat(html, &e) != 0 &&
            kept_path(d, dir, ".d", folder, sizeof(folder)) && os64_mkdir(folder) == 0) {
            size_t n = os64_strlen(folder) + 1;
            d->keep.folder = os64_malloc(n);
            if (d->keep.folder != NULL) {
                os64_memcpy(d->keep.folder, folder, n);
                return 0;
            }
            (void)os64_unlink(folder);
            return keep_failed(d);
        }
        (void)os64_unlink(path);
    }
    // No name with fresh kept files: the record takes the first that is
    // free, or the one it began with, and nothing of the page is kept.
    d->seq = first;
    for (uint32_t tries = 0; tries < YONDER_DIAG_CLAIM_TRIES; tries++, d->seq++)
        if (kept_path(d, dir, ".txt", path, sizeof(path)) && claim_file(d, path))
            return keep ? keep_failed(d) : 0;
    d->seq = first;
    return keep ? keep_failed(d) : 0;
}

// The page's bytes are let go unkept: their temporary file is closed and
// removed.
static void keep_abandon(yonder_diag_t *d)
{
    if (d->keep.open) {
        (void)os64_close(d->keep.handle);
        (void)os64_unlink(d->keep.temp);
        d->keep.open = false;
    }
    os64_free(d->keep.temp);
    d->keep.temp = NULL;
}

// A step of keeping the page failed: nothing more of it is kept, and its
// record says so, so a replay of what was kept is not taken for the page.
static int64_t keep_failed(yonder_diag_t *d)
{
    keep_abandon(d);
    d->keep.failed = true;
    yonder_diag_head_fact(d, "kept", "not whole: a step of keeping the page failed");
    return -1;
}

// The temporary file the page's bytes go to, begun: only for a record whose
// name claimed its kept files' names (yonder_diag_claim).
static bool keep_begin(yonder_diag_t *d, const char *dir)
{
    char path[OS64_PATH_MAX], temp[OS64_PATH_MAX];
    if (d->keep.folder == NULL || !kept_path(d, dir, ".html", path, sizeof(path)) ||
        !temp_beside(path, temp, sizeof(temp)))
        return false;
    size_t n = os64_strlen(temp) + 1;
    d->keep.temp = os64_malloc(n);
    if (d->keep.temp == NULL)
        return false;
    os64_memcpy(d->keep.temp, temp, n);
    int64_t h = os64_open(temp, "w");
    if (h < 0)
        return false;
    d->keep.handle = (int32_t)h;
    d->keep.open = true;
    return true;
}

int64_t yonder_diag_keep_bytes(yonder_diag_t *d, const char *dir, const void *bytes, size_t n)
{
    if (d == NULL)
        return 0;
    if (d->keep.failed || d->keep.ended)
        return d->keep.failed ? -1 : 0;
    if (!d->keep.open && !keep_begin(d, dir))
        return keep_failed(d);
    return write_all(d->keep.handle, bytes, n) ? 0 : keep_failed(d);
}

int64_t yonder_diag_keep_bytes_end(yonder_diag_t *d, const char *dir)
{
    if (d == NULL)
        return 0;
    if (d->keep.failed || d->keep.ended)
        return d->keep.failed ? -1 : 0;
    // Never over a page's bytes already there: the name was claimed fresh
    // (yonder_diag_claim), so one there now is another's.
    char path[OS64_PATH_MAX];
    os64_dirent_t e;
    if ((!d->keep.open && !keep_begin(d, dir)) || !kept_path(d, dir, ".html", path, sizeof(path)) ||
        os64_stat(path, &e) == 0)
        return keep_failed(d);
    d->keep.open = false;
    bool ok = os64_close(d->keep.handle) == 0;
    ok = ok && os64_rename(d->keep.temp, path) == 0;
    if (!ok) {
        (void)os64_unlink(d->keep.temp);
        return keep_failed(d);
    }
    os64_free(d->keep.temp);
    d->keep.temp = NULL;
    d->keep.ended = true;
    return 0;
}

int64_t yonder_diag_keep_resource(yonder_diag_t *d, const char *dir, yonder_diag_kept_t kind,
                                  const char *url, const char *final, const char *label,
                                  const void *bytes, size_t n)
{
    if (d == NULL)
        return 0;
    if (d->keep.failed)
        return -1;
    (void)dir;
    // The folder is the one the record's name claimed, made then and so
    // this record's alone: nothing in it is anyone else's to replace.
    const char *folder = d->keep.folder;
    char path[OS64_PATH_MAX], index[OS64_PATH_MAX];
    if (folder == NULL)
        return keep_failed(d);
    uint32_t number = d->keep.resources + 1;
    const char *word = kind == YONDER_DIAG_KEPT_SCRIPT ? "script" : "sheet";
    int p = os64_snprintf(path, sizeof(path), "%s/%lu.%s", folder, (unsigned long)number,
                          kind == YONDER_DIAG_KEPT_SCRIPT ? "js" : "css");
    int i = os64_snprintf(index, sizeof(index), "%s/index", folder);
    if (p <= 0 || (size_t)p >= sizeof(path) || i <= 0 || (size_t)i >= sizeof(index) ||
        (bytes != NULL && !write_whole(path, bytes, n)))
        return keep_failed(d);
    // The index's new line, measured and then written onto the lines it
    // has, and the index written again whole under its header.
#define DIAG_STRING(x) #x
#define DIAG_VERSION(x) DIAG_STRING(x)
    static const char kHeader[] =
        "# yonder kept index " DIAG_VERSION(YONDER_DIAG_INDEX_VERSION) "\n"
        "# what this page fetched, as yonder received it: <n>\t<kind>\tthen what the kind carries\n"
        "# script, sheet: <address asked for>\tcame|failed\t<address after redirects>\t<charset label>;"
        " the file <n>.js or <n>.css when it came\n"
        "# fonts and pictures are not kept\n";
#undef DIAG_VERSION
#undef DIAG_STRING
    char lead[32];
    os64_snprintf(lead, sizeof(lead), "%lu\t%s\t", (unsigned long)number, word);
    const char *fields[4] = {url, bytes != NULL ? "came" : "failed", final, label};
    Out m = {NULL, 0, 0};
    for (int f = 0; f < 4; f++) {
        put_field(&m, fields[f] != NULL ? fields[f] : "", false);
        put(&m, "\t", 1);
    }
    size_t line = os64_strlen(lead) + m.at;
    size_t total = sizeof(kHeader) - 1 + d->keep.index_len + line;
    char *text = os64_malloc(total + 1);
    if (text == NULL)
        return keep_failed(d);
    os64_memcpy(text, kHeader, sizeof(kHeader) - 1);
    if (d->keep.index_len != 0)
        os64_memcpy(text + sizeof(kHeader) - 1, d->keep.index, d->keep.index_len);
    Out o = {text + sizeof(kHeader) - 1 + d->keep.index_len, line + 1, 0};
    puts_(&o, lead);
    for (int f = 0; f < 4; f++) {
        put_field(&o, fields[f] != NULL ? fields[f] : "", false);
        put(&o, f < 3 ? "\t" : "\n", 1);
    }
    bool ok = write_whole(index, text, total);
    if (ok) {
        // The lines without the header: the next index begins from them.
        os64_memmove(text, text + sizeof(kHeader) - 1, total - (sizeof(kHeader) - 1));
        os64_free(d->keep.index);
        d->keep.index = text;
        d->keep.index_len = total - (sizeof(kHeader) - 1);
        d->keep.resources = number;
        return 0;
    }
    os64_free(text);
    return keep_failed(d);
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
