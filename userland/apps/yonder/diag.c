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
    bool head;              // a fact's: set by yonder_diag_head_fact, in the head's own room
} Line;

// The lines that met no room are counted as distinct lines, as the file
// says them: each is kept by its key (`gone`) so a line asked for again —
// a condition looked at once a layout, a section written again whole —
// is not counted again. Past GONE_MAX of them, or with no memory to keep
// one, a line cannot be told from one counted already, so the count
// stands and the file says there are more (`more`).
#define GONE_MAX 4096u
// Each list is found by its key through an index of its own: open
// addressing over the key's hash, each slot a position in the list plus
// one (0 is empty), at most half full, a removal shifting the slots after
// it back. A page decides how many lines a table holds (a FAILED line per
// failure it makes), so finding one must not cost what it holds.
typedef struct {
    uint32_t *slots;
    uint32_t cap;           // a power of two, or 0
} Index;
typedef struct {
    Line *lines;
    uint32_t n, cap;
    Line *gone;
    uint32_t ngone, cap_gone;
    bool more;
    // Lines are keyed by `a` and `b` (MISSING, FAILED) or by `a` alone
    // (facts), the same for a table every time it is asked.
    Index by_line, by_gone;
} Table;

// The index's probes so far, for the host's bound (yonder_diag_steps).
static uint64_t s_steps;

// THE CLAIM (diag.h): the directory this record's name was claimed in, and
// the record it is the proof for. Only yonder_diag_claim sets `dir`.
struct yonder_diag_claim {
    yonder_diag_t *record;
    char *dir;
};

struct yonder_diag {
    char *url;
    uint32_t seq;
    struct yonder_diag_claim claim;
    // The claim's own mark, written in its placeholder and in every record
    // it writes after (`claim: <nonce>`), so a write proves the file at its
    // name is still this record's and not one another yonder claimed after
    // it went (claim_ours). 0 before the claim.
    uint64_t nonce;
    int64_t began_us;
    Table missing, failed, facts;
    // The facts that are the head's (Line.head): they have room of their
    // own, so they are counted apart from the ordinary facts' quota.
    uint32_t head_facts;
    // A line went with no memory even to name it (yonder_diag_short).
    bool short_of_memory;
    // Why the record says it cannot be judged, besides lost lines (a
    // setting changed during the load: yonder_diag_unjudged); NULL for none.
    const char *unjudged;
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
    os64_free(t->by_line.slots);
    os64_free(t->by_gone.slots);
}

static bool same_key(const Line *l, const char *a, const char *b, bool both)
{
    return os64_streq(l->a, a) && (!both || os64_streq(l->b, b));
}

static uint32_t key_hash(const char *a, const char *b, bool both)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = a; *p != '\0'; p++)
        h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    if (both) {
        h = (h ^ 0x1f) * 0x100000001b3ull;
        for (const char *p = b; *p != '\0'; p++)
            h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

// The slot holding the line keyed `a` (and `b`), or the empty one where it
// would go; NULL when the list has no index (its memory was refused).
static uint32_t *index_slot(const Index *x, const Line *list, const char *a, const char *b, bool both)
{
    if (x->cap == 0)
        return NULL;
    uint32_t mask = x->cap - 1;
    for (uint32_t i = key_hash(a, b, both) & mask;; i = (i + 1) & mask) {
        s_steps++;
        uint32_t s = x->slots[i];
        if (s == 0 || same_key(&list[s - 1], a, b, both))
            return &x->slots[i];
    }
}

// The index made again for the list's first `n` lines, with room for one
// more at most half full. False, the old one kept, when memory ran out.
static bool index_build(Index *x, const Line *list, uint32_t n, bool both)
{
    uint32_t cap = 16;
    while (cap < (n + 1) * 4)
        cap *= 2;
    uint32_t *slots = os64_calloc(cap, sizeof(*slots));
    if (slots == NULL)
        return false;
    os64_free(x->slots);
    x->slots = slots;
    x->cap = cap;
    for (uint32_t i = 0; i < n; i++)
        *index_slot(x, list, list[i].a, list[i].b, both) = i + 1;
    return true;
}

// Room to index one more line beside the `n` the list holds.
static bool index_room(Index *x, const Line *list, uint32_t n, bool both)
{
    return (n + 1) * 2 <= x->cap || index_build(x, list, n, both);
}

// The line at *slot leaves the index: the slots after it in its run shift
// back as far as their own keys allow, so no lookup stops short.
static void index_remove(Index *x, const Line *list, uint32_t *slot, bool both)
{
    uint32_t mask = x->cap - 1, i = (uint32_t)(slot - x->slots);
    x->slots[i] = 0;
    for (uint32_t j = (i + 1) & mask; x->slots[j] != 0; j = (j + 1) & mask) {
        s_steps++;
        const Line *l = &list[x->slots[j] - 1];
        uint32_t home = key_hash(l->a, l->b, both) & mask;
        bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (!stays) {
            x->slots[i] = x->slots[j];
            x->slots[j] = 0;
            i = j;
        }
    }
}

// The position of the line keyed `a` (and `b`) in `list`, or -1: through
// the index, or by walking the list when the index's memory was refused.
static int64_t list_find(const Index *x, const Line *list, uint32_t n, const char *a, const char *b, bool both)
{
    uint32_t *slot = index_slot(x, list, a, b, both);
    if (slot != NULL)
        return *slot != 0 ? (int64_t)*slot - 1 : -1;
    for (uint32_t i = 0; i < n; i++) {
        s_steps++;
        if (same_key(&list[i], a, b, both))
            return i;
    }
    return -1;
}

uint64_t yonder_diag_steps(void)
{
    return s_steps;
}

// The line keyed `a` (and `b`) met no room: counted once, however often it
// is asked for.
static void table_lose(Table *t, const char *a, const char *b, bool both)
{
    if (list_find(&t->by_gone, t->gone, t->ngone, a, b, both) >= 0)
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
    bool room = t->ngone < t->cap_gone && index_room(&t->by_gone, t->gone, t->ngone, both);
    Line one = {room ? copy(a) : NULL, room ? copy(b) : NULL, 0, false};
    if (one.a == NULL || one.b == NULL) {
        os64_free(one.a);
        os64_free(one.b);
        t->more = true;
        return;
    }
    t->gone[t->ngone] = one;
    *index_slot(&t->by_gone, t->gone, a, b, both) = ++t->ngone;
}

// A key given room after all is no longer lost. The last lost line takes
// its place in the list, and its slot in the index says so.
static void table_found(Table *t, const char *a, const char *b, bool both)
{
    int64_t at = list_find(&t->by_gone, t->gone, t->ngone, a, b, both);
    if (at < 0)
        return;
    uint32_t i = (uint32_t)at, last = t->ngone - 1;
    if (t->by_gone.cap != 0)
        index_remove(&t->by_gone, t->gone, index_slot(&t->by_gone, t->gone, a, b, both), both);
    os64_free(t->gone[i].a);
    os64_free(t->gone[i].b);
    if (i != last) {
        t->gone[i] = t->gone[last];
        uint32_t *slot = t->by_gone.cap != 0
            ? index_slot(&t->by_gone, t->gone, t->gone[i].a, t->gone[i].b, both) : NULL;
        if (slot != NULL)
            *slot = i + 1;
    }
    t->ngone = last;
}

// The line keyed `a` (and `b`, when `both`), found or added. NULL when the
// table holds `max` lines already (the facts' rooms are decided by
// fact_set, which passes no room as 0) or memory ran out: the line is then
// counted as lost, once.
static Line *table_line(Table *t, const char *a, const char *b, bool both, uint32_t max)
{
    int64_t at = list_find(&t->by_line, t->lines, t->n, a, b, both);
    if (at >= 0)
        return &t->lines[at];
    if (t->n >= max || !index_room(&t->by_line, t->lines, t->n, both)) {
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
    Line one = {copy(a), copy(b), 0, false};
    if (one.a == NULL || one.b == NULL) {
        os64_free(one.a);
        os64_free(one.b);
        table_lose(t, a, b, both);
        return NULL;
    }
    table_found(t, a, b, both);
    t->lines[t->n] = one;
    *index_slot(&t->by_line, t->lines, a, b, both) = t->n + 1;
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
    os64_free(d->claim.dir);
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

// A plain line set. A new one takes a place in its own room: the head's
// YONDER_DIAG_HEAD_FACTS (`head`), or the ordinary facts'
// YONDER_DIAG_FACTS_MAX, counted apart so neither eats the other's.
static void fact_set(yonder_diag_t *d, const char *key, const char *value, bool head)
{
    if (d == NULL)
        return;
    Table *t = &d->facts;
    int64_t at = list_find(&t->by_line, t->lines, t->n, key, "", false);
    Line *l = at >= 0 ? &t->lines[at] : NULL;
    if (l == NULL) {
        bool room = head ? d->head_facts < YONDER_DIAG_HEAD_FACTS : t->n - d->head_facts < YONDER_DIAG_FACTS_MAX;
        l = table_line(t, key, "", false, room ? UINT32_MAX : 0);
        if (l != NULL && head) {
            l->head = true;
            d->head_facts++;
        }
    }
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
    fact_set(d, key, value, false);
}

void yonder_diag_head_fact(yonder_diag_t *d, const char *key, const char *value)
{
    fact_set(d, key, value, true);
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
                if (k == 0 && l->head)
                    d->head_facts--;
                os64_free(l->a);
                os64_free(l->b);
            } else {
                lists[k][kept++] = *l;
            }
        }
        *counts[k] = kept;
    }
    // The lists closed up, so their indexes are made again; one that
    // cannot be is dropped, and its list walked until it can.
    Index *indexes[2] = {&d->facts.by_line, &d->facts.by_gone};
    for (int k = 0; k < 2; k++)
        if (!index_build(indexes[k], lists[k], *counts[k], false)) {
            os64_free(indexes[k]->slots);
            indexes[k]->slots = NULL;
            indexes[k]->cap = 0;
        }
}

void yonder_diag_short(yonder_diag_t *d)
{
    if (d != NULL)
        d->short_of_memory = true;
}

void yonder_diag_unjudged(yonder_diag_t *d, const char *why)
{
    if (d != NULL && d->unjudged == NULL)
        d->unjudged = why;
}

const char *yonder_diag_fact_value(const yonder_diag_t *d, const char *key)
{
    int64_t at = d != NULL ? list_find(&d->facts.by_line, d->facts.lines, d->facts.n, key, "", false) : -1;
    return at >= 0 && d->facts.lines[at].count == 0 ? d->facts.lines[at].b : NULL;
}

void yonder_diag_failed_seen(yonder_diag_t *d, const char *what, const char *message)
{
    Line *l = d != NULL ? table_line(&d->failed, what, message, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL && l->count == 0)
        l->count = 1;
}

void yonder_diag_failed_most(yonder_diag_t *d, const char *what, const char *message, uint32_t count)
{
    Line *l = d != NULL ? table_line(&d->failed, what, message, true, YONDER_DIAG_RECORDS_MAX) : NULL;
    if (l != NULL && count > l->count)
        l->count = count;
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
    if (d->nonce != 0) {
        char mark[48];
        os64_snprintf(mark, sizeof(mark), "claim: %016llx\n", (unsigned long long)d->nonce);
        puts_(&o, mark);
    }
    // What met no room is said, in words that are neither token: the
    // verdict already counts every line that was kept.
    put_lost(&o, "missing names", &d->missing);
    put_lost(&o, "failures", &d->failed);
    put_lost(&o, "facts", &d->facts);
    if (d->short_of_memory)
        puts_(&o, "some lines not kept: no memory to name them\n");
    // ONE MARKER for every way a line was lost, which the readers key on:
    // a replay or a diff of a record missing a line, whatever the cause,
    // cannot judge it, since the line it lost may be the one a change made
    // or mended; the census tally counts the lines it kept.
    bool lost = d->short_of_memory;
    const Table *tables[3] = {&d->missing, &d->failed, &d->facts};
    for (int i = 0; i < 3; i++)
        lost |= tables[i]->ngone != 0 || tables[i]->more;
    for (uint32_t i = 0; i < d->facts.n; i++)
        lost |= d->facts.lines[i].count != 0;
    if (lost)
        puts_(&o, YONDER_DIAG_LINES_LOST "\n");
    if (d->unjudged != NULL) {
        puts_(&o, YONDER_DIAG_UNJUDGED);
        put_text(&o, d->unjudged);
        puts_(&o, "\n");
    }
    // The last line, which a reader of a kept page's record requires, so a
    // record cut short anywhere is never read as a page.
    puts_(&o, YONDER_DIAG_END "\n");
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

static bool kept_path(const yonder_diag_t *d, const char *dir, const char *suffix, char *out, size_t cap);

static bool diag_contains(const char *text, const char *needle);

// Whether the file at `path` is still this record's: its placeholder, or a
// record it wrote, carrying its nonce. A file that is gone, or one that
// carries another nonce or none, means the name is someone else's now.
static bool claim_ours(const yonder_diag_t *d, const char *path)
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return false;
    size_t cap = 4096, n = 0;
    char *text = os64_malloc(cap);
    bool ok = text != NULL;
    while (ok) {
        if (n + 1 == cap) {
            char *more = cap < ((size_t)16 << 20) ? os64_realloc(text, cap * 2) : NULL;
            if (more == NULL) {
                ok = false;
                break;
            }
            text = more;
            cap *= 2;
        }
        int64_t got = os64_read((int32_t)h, text + n, cap - 1 - n);
        if (got < 0)
            ok = false;
        if (got <= 0)
            break;
        n += (size_t)got;
    }
    (void)os64_close((int32_t)h);
    bool ours = false;
    if (ok) {
        text[n] = '\0';
        char mark[64];
        os64_snprintf(mark, sizeof(mark), "\n" YONDER_DIAG_UNWRITTEN " %016llx\n", (unsigned long long)d->nonce);
        ours = diag_contains(text, mark);
        os64_snprintf(mark, sizeof(mark), "\nclaim: %016llx\n", (unsigned long long)d->nonce);
        ours = ours || diag_contains(text, mark);
    }
    os64_free(text);
    return ours;
}

int64_t yonder_diag_write(const yonder_diag_claim_t *c)
{
    char path[OS64_PATH_MAX];
    // The name is proved again before it is written over: the file there
    // must still be this record's own, by its nonce. One that is gone, or
    // that another yonder claimed after it went, is not written over.
    // (A file changed between this look and the rename is not caught.)
    if (c == NULL || !kept_path(c->record, c->dir, ".txt", path, sizeof(path)) || !claim_ours(c->record, path))
        return -1;
    const yonder_diag_t *d = c->record;
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
//
// NOTHING IS WRITTEN UNDER A NAME THIS RECORD DID NOT CLAIM: the record's
// file, its page's bytes and its folder are written only through the
// yonder_diag_claim_t a successful exclusive claim makes, which nothing
// else can.

const yonder_diag_claim_t *yonder_diag_claimed(const yonder_diag_t *d)
{
    return d != NULL && d->claim.dir != NULL ? &d->claim : NULL;
}

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

// A claim's nonce: eight bytes of /dev/random, or, where it cannot be
// read, the microsecond clock, the sequence and the record's own address
// mixed, which no two claims share in practice. Never 0, which is "no
// claim".
static uint64_t claim_nonce(const yonder_diag_t *d)
{
    uint64_t n = 0;
    int64_t h = os64_open("/dev/random", "r");
    if (h >= 0) {
        if (os64_read((int32_t)h, &n, sizeof(n)) != (int64_t)sizeof(n))
            n = 0;
        (void)os64_close((int32_t)h);
    }
    if (n == 0)
        n = ((uint64_t)os64_micros() * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)d->seq << 32) ^ (uint64_t)(uintptr_t)d;
    return n != 0 ? n : 1;
}

// Whether `text` holds `needle`.
static bool diag_contains(const char *text, const char *needle)
{
    size_t n = os64_strlen(needle), t = os64_strlen(text);
    for (size_t i = 0; i + n <= t; i++)
        if (os64_memcmp(text + i, needle, n) == 0)
            return true;
    return false;
}

// The record's file made at `path` only if nothing is there, holding its
// address and `verdict: unwritten <nonce>`, which no reader judges or counts (the
// tally names it as damage), so a name claimed and never written (a yonder
// that died, a write that failed) is never read as a page that met
// nothing. The real record replaces it, and proves the name its own by
// the nonce (claim_ours), so A PLACEHOLDER NOT KNOWN TO BE WHOLE IS NO
// CLAIM: with no memory for its text, or a write or a close that failed,
// it is not known to be whole on disk, so the name is not known to be
// this claim's. The file is removed and the name passed over; one the
// unlink cannot remove is left as a file no reader takes for a page (a
// placeholder, or less than one). False when something is there, or it
// could not be made whole.
static bool claim_file(const yonder_diag_t *d, const char *path)
{
    int64_t h = os64_open(path, "x");
    if (h < 0)
        return false;
    Out m = {NULL, 0, 0};
    char mark[48];
    os64_snprintf(mark, sizeof(mark), "\n" YONDER_DIAG_UNWRITTEN " %016llx\n", (unsigned long long)d->nonce);
    puts_(&m, "address: ");
    put_text(&m, d->url);
    puts_(&m, mark);
    char *text = os64_malloc(m.at + 1);
    bool whole = text != NULL;
    if (whole) {
        Out o = {text, m.at + 1, 0};
        puts_(&o, "address: ");
        put_text(&o, d->url);
        puts_(&o, mark);
        whole = write_all((int32_t)h, text, m.at);
        os64_free(text);
    }
    whole = os64_close((int32_t)h) == 0 && whole;
    if (!whole)
        (void)os64_unlink(path);
    return whole;
}

// The name at `path` taken as this record's: the proof made, which holds
// its own copy of `dir`. False, with the file removed, when there is no
// memory for the copy.
static bool claim_take(yonder_diag_t *d, const char *dir, const char *path)
{
    d->claim.dir = copy(dir);
    if (d->claim.dir == NULL) {
        (void)os64_unlink(path);
        return false;
    }
    d->claim.record = d;
    return true;
}

int64_t yonder_diag_claim(yonder_diag_t *d, const char *dir, bool keep)
{
    if (d == NULL || dir == NULL)
        return YONDER_DIAG_UNCLAIMED;
    if (d->claim.dir != NULL)
        return 0;
    char path[OS64_PATH_MAX], html[OS64_PATH_MAX], folder[OS64_PATH_MAX];
    os64_dirent_t e;
    uint32_t first = d->seq;
    d->nonce = claim_nonce(d);
    for (uint32_t tries = 0; tries < YONDER_DIAG_CLAIM_TRIES; tries++, d->seq++) {
        if (!kept_path(d, dir, ".txt", path, sizeof(path)) || !claim_file(d, path))
            continue;
        // Its kept files' names must be fresh too, whether or not this
        // record keeps its page: no page's bytes and no folder by that
        // name left from before, which a reader would pair with this
        // record. With `keep`, the folder is made here.
        bool fresh = kept_path(d, dir, ".html", html, sizeof(html)) && os64_stat(html, &e) != 0 &&
                     kept_path(d, dir, ".d", folder, sizeof(folder)) && os64_stat(folder, &e) != 0;
        if (fresh && !keep)
            return claim_take(d, dir, path) ? 0 : YONDER_DIAG_UNCLAIMED;
        if (fresh && os64_mkdir(folder) == 0) {
            if (!claim_take(d, dir, path)) {
                (void)os64_unlink(folder);
                break;
            }
            d->keep.folder = copy(folder);
            if (d->keep.folder != NULL) {
                // Its index holds no line yet, said so a replay need not
                // assume it.
                yonder_diag_head_fact(d, "kept lines", "0");
                return 0;
            }
            (void)os64_unlink(folder);
            return keep_failed(d);
        }
        // A fresh name whose folder cannot be made is the record's, and
        // nothing of its page is kept: its record says `kept: not whole`,
        // which no replay judges.
        if (fresh)
            return !claim_take(d, dir, path) ? YONDER_DIAG_UNCLAIMED : keep_failed(d);
        (void)os64_unlink(path);
    }
    // No fresh name has a claim, and nothing of the record is written:
    // a record beside another's kept files would be paired with them.
    d->seq = first;
    return YONDER_DIAG_UNCLAIMED;
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
static bool keep_begin(yonder_diag_t *d)
{
    char path[OS64_PATH_MAX], temp[OS64_PATH_MAX];
    if (d->keep.folder == NULL || !kept_path(d, d->claim.dir, ".html", path, sizeof(path)) ||
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

int64_t yonder_diag_keep_bytes(const yonder_diag_claim_t *c, const void *bytes, size_t n)
{
    if (c == NULL)
        return 0;
    yonder_diag_t *d = c->record;
    if (d->keep.failed || d->keep.ended)
        return d->keep.failed ? -1 : 0;
    if (!d->keep.open && !keep_begin(d))
        return keep_failed(d);
    return write_all(d->keep.handle, bytes, n) ? 0 : keep_failed(d);
}

int64_t yonder_diag_keep_bytes_end(const yonder_diag_claim_t *c)
{
    if (c == NULL)
        return 0;
    yonder_diag_t *d = c->record;
    if (d->keep.failed || d->keep.ended)
        return d->keep.failed ? -1 : 0;
    // Never over a page's bytes already there: the name was claimed fresh
    // (yonder_diag_claim), so one there now is another's.
    char path[OS64_PATH_MAX];
    os64_dirent_t e;
    if ((!d->keep.open && !keep_begin(d)) || !kept_path(d, c->dir, ".html", path, sizeof(path)) ||
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
    // Said positively, so a replay replays only a page whose record says
    // it was kept whole; a later step that fails says otherwise.
    yonder_diag_head_fact(d, "kept", "whole");
    return 0;
}

int64_t yonder_diag_keep_resource(const yonder_diag_claim_t *c, yonder_diag_kept_t kind, uint32_t job,
                                  const char *url, const char *final, const char *label,
                                  const void *bytes, size_t n)
{
    if (c == NULL)
        return 0;
    yonder_diag_t *d = c->record;
    if (d->keep.failed)
        return -1;
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
        "# script, sheet: <address asked for>\tcame|failed\t<address after redirects>\t<charset label>\t"
        "<the job's own identity>; the file <n>.js or <n>.css when it came\n"
        "# fonts and pictures are not kept\n";
#undef DIAG_VERSION
#undef DIAG_STRING
    char lead[32], tail[16];
    os64_snprintf(lead, sizeof(lead), "%lu\t%s\t", (unsigned long)number, word);
    os64_snprintf(tail, sizeof(tail), "%lu\n", (unsigned long)job);
    const char *fields[4] = {url, bytes != NULL ? "came" : "failed", final, label};
    Out m = {NULL, 0, 0};
    for (int f = 0; f < 4; f++) {
        put_field(&m, fields[f] != NULL ? fields[f] : "", false);
        put(&m, "\t", 1);
    }
    size_t line = os64_strlen(lead) + m.at + os64_strlen(tail);
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
        put(&o, "\t", 1);
    }
    puts_(&o, tail);
    bool ok = write_whole(index, text, total);
    if (ok) {
        // The lines without the header: the next index begins from them.
        os64_memmove(text, text + sizeof(kHeader) - 1, total - (sizeof(kHeader) - 1));
        os64_free(d->keep.index);
        d->keep.index = text;
        d->keep.index_len = total - (sizeof(kHeader) - 1);
        d->keep.resources = number;
        // The record says how many lines its index holds, so a replay can
        // tell its own index from one that is not.
        char count[16];
        os64_snprintf(count, sizeof(count), "%lu", (unsigned long)number);
        yonder_diag_head_fact(d, "kept lines", count);
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
