// load_timeline.c — where a navigation's first minutes went, in order
// (load_timeline.h).

#include "load_timeline.h"

static const char *const kStep[YONDER_LT_STEPS] = {
    [YONDER_LT_WAIT] = "waiting",
    [YONDER_LT_INPUT] = "input",
    [YONDER_LT_RELAYOUT] = "the window's layouts",
    [YONDER_LT_STREAM] = "the stream",
    [YONDER_LT_COMING] = "the page waiting for its sheets",
    [YONDER_LT_SCRIPTS] = "scripts",
    [YONDER_LT_MEDIA] = "bodies and media",
    [YONDER_LT_MOTION] = "motion",
    [YONDER_LT_UPKEEP] = "upkeep",
    [YONDER_LT_OBSERVERS] = "observers",
    [YONDER_LT_PAINT] = "paint",
    [YONDER_LT_CLOSE] = "the turn's close",
};

// A grid column's name: a step's, or the column past them.
static const char *column(int s)
{
    return s < YONDER_LT_STEPS ? kStep[s] : "no step";
}

static const char *const kIn[YONDER_LT_INS] = {
    [YONDER_LT_IN_TASKS] = "tasks",
    [YONDER_LT_IN_LAYOUTS] = "layouts",
    [YONDER_LT_IN_DRAWING] = "drawing",
    [YONDER_LT_IN_PARSING] = "parsing",
    [YONDER_LT_IN_SHEETS] = "finding sheets",
    [YONDER_LT_IN_ARRIVING] = "arriving",
};

static const char *const kWhy[YONDER_LT_WHYS] = {
    [YONDER_LT_WHY_OTHER] = "no reason told",
    [YONDER_LT_WHY_READ] = "a script's read",
    [YONDER_LT_WHY_REBUILD] = "after a script's changes",
    [YONDER_LT_WHY_ARRIVAL] = "a page arriving",
    [YONDER_LT_WHY_SHEET] = "a sheet arriving",
    [YONDER_LT_WHY_PICTURE] = "sizes arriving",
    [YONDER_LT_WHY_MOTION] = "the animations",
    [YONDER_LT_WHY_WINDOW] = "the window",
};

static const char *const kMark[YONDER_LT_MARKS] = {
    [YONDER_LT_MARK_ARRIVED] = "arrived",
    [YONDER_LT_MARK_PAINT] = "first paint",
    [YONDER_LT_MARK_PICTURE] = "the video's first picture",
};

const char *yonder_lt_step_name(yonder_lt_step_t step)
{
    return (unsigned)step < YONDER_LT_STEPS ? kStep[step] : "?";
}

const char *yonder_lt_why_name(yonder_lt_why_t why)
{
    return (unsigned)why < YONDER_LT_WHYS ? kWhy[why] : "?";
}

static void zero(void *p, size_t n)
{
    unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

void yonder_lt_begin(yonder_lt_t *t, int64_t began_us)
{
    zero(t, sizeof(*t));
    t->on = true;
    t->began_us = began_us;
    for (int i = 0; i < YONDER_LT_MARKS; i++)
        t->marks[i] = -1;
}

void yonder_lt_stop(yonder_lt_t *t)
{
    t->on = false;
    t->in_step = false;
}

// Microseconds from the navigation's start; a time before it is its start.
static uint64_t since(const yonder_lt_t *t, int64_t us)
{
    return us > t->began_us ? (uint64_t)(us - t->began_us) : 0;
}

#define LT_END_US ((uint64_t)YONDER_LT_SLICES * YONDER_LT_SLICE_US)

bool yonder_lt_running(const yonder_lt_t *t, int64_t now_us)
{
    return t->on && since(t, now_us) < LT_END_US;
}

// The step's time shared between the slices it ran across, by time, and
// what was inside it in the same shares; what runs past the last slice is
// not kept.
static void spread(yonder_lt_t *t, const yonder_lt_long_t *l, uint64_t from, uint64_t to)
{
    uint64_t whole = to - from;
    while (from < to && from < LT_END_US) {
        uint64_t i = from / YONDER_LT_SLICE_US;
        uint64_t edge = (i + 1) * YONDER_LT_SLICE_US;
        uint64_t end = to < edge ? to : edge;
        uint64_t part = end - from;
        t->slices[i].step_us[l->step] += part;
        for (int k = 0; k < YONDER_LT_INS; k++)
            t->slices[i].in_us[l->step][k] += part == whole ? l->in_us[k] : l->in_us[k] * part / whole;
        from = end;
    }
}

static void copy_name(char *out, size_t cap, const char *in)
{
    size_t i = 0;
    for (; in != NULL && in[i] != '\0' && i + 1 < cap; i++)
        out[i] = in[i];
    out[i] = '\0';
}

// A long step into the list, longest first; past its room the shortest
// goes to the count of those not listed.
static void keep_long(yonder_lt_t *t, const yonder_lt_long_t *l)
{
    uint32_t at = t->nlongs;
    if (at == YONDER_LT_LONG_MAX) {
        const yonder_lt_long_t *last = &t->longs[at - 1];
        if (last->us >= l->us) {
            t->more_longs++;
            t->more_longs_us += l->us;
            return;
        }
        t->more_longs++;
        t->more_longs_us += last->us;
        at--;
    } else {
        t->nlongs++;
    }
    while (at > 0 && t->longs[at - 1].us < l->us) {
        t->longs[at] = t->longs[at - 1];
        at--;
    }
    t->longs[at] = *l;
}

void yonder_lt_step_end(yonder_lt_t *t, int64_t to_us)
{
    if (!t->on || !t->in_step)
        return;
    t->in_step = false;
    uint64_t to = since(t, to_us);
    uint64_t from = t->now.at_us;
    if (to < from)
        to = from;
    t->now.us = to - from;
    spread(t, &t->now, from, to);
    // A wait is no work: an idle page's waits would crowd the work out of
    // the list, and the slices say how long the loop slept.
    if (t->now.step != YONDER_LT_WAIT && t->now.us >= YONDER_LT_LONG_US && from < LT_END_US)
        keep_long(t, &t->now);
}

void yonder_lt_snapshot(const yonder_lt_t *t, int64_t now_us, yonder_lt_t *out)
{
    *out = *t;
    if (out->in_step)
        yonder_lt_step_end(out, now_us);
}

void yonder_lt_step_begin(yonder_lt_t *t, yonder_lt_step_t step, int64_t from_us)
{
    if (!t->on || (unsigned)step >= YONDER_LT_STEPS)
        return;
    if (t->in_step)
        yonder_lt_step_end(t, from_us);
    zero(&t->now, sizeof(t->now));
    t->now.at_us = since(t, from_us);
    t->now.step = (uint8_t)step;
    t->in_step = true;
}

void yonder_lt_layout(yonder_lt_t *t, int64_t from_us, uint64_t us, yonder_lt_why_t why,
                      const yonder_lt_cascade_t *cascade)
{
    if (!t->on)
        return;
    if ((unsigned)why >= YONDER_LT_WHYS)
        why = YONDER_LT_WHY_OTHER;
    uint64_t from = since(t, from_us);
    if (from >= LT_END_US)
        return;
    yonder_lt_slice_t *s = &t->slices[from / YONDER_LT_SLICE_US];
    s->layouts++;
    s->layout_us += us;
    s->why_n[why]++;
    s->why_us[why] += us;
    uint8_t step = t->in_step ? t->now.step : YONDER_LT_OUTSIDE;
    t->grid_n[why][step]++;
    t->grid_us[why][step] += us;
    if (cascade != NULL) {
        t->cascades[why]++;
        t->cascade_us[why] += cascade->us;
        if (cascade->fresh) {
            t->fresh[why]++;
            t->fresh_us[why] += cascade->us;
        }
    }
    if (t->in_step) {
        t->now.layouts++;
        t->now.why_n[why]++;
        t->now.why_us[why] += us;
        t->now.in_us[YONDER_LT_IN_LAYOUTS] += us;
    }
}

void yonder_lt_task(yonder_lt_t *t, int64_t at_us, const char *what, uint64_t us, uint32_t layouts,
                    uint64_t layout_us)
{
    if (!t->on)
        return;
    uint64_t at = since(t, at_us);
    if (at >= LT_END_US)
        return;
    yonder_lt_slice_t *s = &t->slices[at / YONDER_LT_SLICE_US];
    s->tasks++;
    s->forced += layouts;
    s->forced_us += layout_us;
    if (!t->in_step)
        return;
    t->now.in_us[YONDER_LT_IN_TASKS] += us;
    t->now.forced += layouts;
    t->now.forced_us += layout_us;
    if (us > t->now.task_us) {
        copy_name(t->now.task, sizeof(t->now.task), what);
        t->now.task_us = us;
        t->now.task_layouts = layouts;
        t->now.task_layout_us = layout_us;
    }
}

void yonder_lt_inside(yonder_lt_t *t, yonder_lt_in_t in, uint64_t us)
{
    if (t->on && t->in_step && (unsigned)in < YONDER_LT_INS)
        t->now.in_us[in] += us;
}

void yonder_lt_parse(yonder_lt_t *t, int64_t from_us, uint64_t us, const char *text)
{
    if (!t->on || since(t, from_us) >= LT_END_US)
        return;
    yonder_lt_inside(t, YONDER_LT_IN_PARSING, us);
    t->nparse++;
    t->parse_us += us;
    uint32_t at = t->parse_kept;
    if (at == YONDER_LT_PARSE_MAX) {
        if (t->parse[at - 1].us >= us)
            return;
        at--;
    } else {
        t->parse_kept++;
    }
    while (at > 0 && t->parse[at - 1].us < us) {
        t->parse[at] = t->parse[at - 1];
        at--;
    }
    t->parse[at].at_us = since(t, from_us);
    t->parse[at].us = us;
    copy_name(t->parse[at].text, sizeof(t->parse[at].text), text);
}

void yonder_lt_bytes(yonder_lt_t *t, int64_t at_us, uint64_t n)
{
    if (!t->on)
        return;
    uint64_t at = since(t, at_us);
    if (at < LT_END_US)
        t->slices[at / YONDER_LT_SLICE_US].bytes += n;
}

void yonder_lt_mark(yonder_lt_t *t, yonder_lt_mark_t mark, int64_t at_us)
{
    if (!t->on || (unsigned)mark >= YONDER_LT_MARKS || t->marks[mark] >= 0)
        return;
    t->marks[mark] = (int64_t)since(t, at_us);
}

void yonder_lt_build(yonder_lt_t *t, yonder_lt_why_t why, const yonder_lt_build_t *b)
{
    if (!t->on || b == NULL)
        return;
    if ((unsigned)why >= YONDER_LT_WHYS)
        why = YONDER_LT_WHY_OTHER;
    t->builds[why]++;
    if (b->kept) {
        t->kept[why]++;
    } else if (b->again != NULL) {
        uint32_t i = 0;
        while (i < t->nagain && t->again[i].name != b->again)
            i++;
        if (i == t->nagain && t->nagain < YONDER_LT_REASONS)
            t->again[t->nagain++].name = b->again;
        if (i < t->nagain)
            t->again[i].n++;
        else
            t->again_other++;
    }
    t->build_sheets[why] += b->sheets_us;
    t->build_table[why] += b->table_us;
    t->build_matching[why] += b->matching_us;
    t->build_rules[why] += b->rules;
    t->build_elements[why] += b->elements;
}

void yonder_lt_back(yonder_lt_t *t, int64_t at_us, uint64_t walks, uint64_t walked, uint64_t turned)
{
    if (!t->on || walks == 0 || since(t, at_us) >= LT_END_US)
        return;
    t->back_updates++;
    t->back_walks += walks;
    t->back_walked += walked;
    t->back_turned += turned;
}

void yonder_lt_marks(yonder_lt_t *t, int64_t at_us, uint64_t marked, uint64_t inherited, const uint64_t *by,
                     uint32_t nby, const char *const *names)
{
    if (!t->on || since(t, at_us) >= LT_END_US)
        return;
    t->marks_updates++;
    t->marks_marked += marked;
    t->marks_inherited += inherited;
    if (t->mark_names == NULL && names != NULL) {
        t->mark_names = names;
        t->nmark_kinds = nby < YONDER_LT_MARK_KINDS ? nby : YONDER_LT_MARK_KINDS;
    }
    for (uint32_t k = 0; by != NULL && k < t->nmark_kinds && k < nby; k++)
        t->marks_by[k] += by[k];
}

void yonder_lt_broad(yonder_lt_t *t, int64_t at_us, uint32_t marked, const char *by, const char *text)
{
    if (!t->on || since(t, at_us) >= LT_END_US)
        return;
    t->nbroad++;
    uint32_t k = 0;
    while (k < t->nbroad_by && t->broad_by[k].name != by)
        k++;
    if (k == t->nbroad_by && t->nbroad_by < YONDER_LT_REASONS)
        t->broad_by[t->nbroad_by++].name = by;
    if (k < t->nbroad_by)
        t->broad_by[k].n++;
    else
        t->broad_by_other++;
    uint32_t at = t->broad_kept;
    if (at == YONDER_LT_BROAD_MAX) {
        if (t->broad[at - 1].marked >= marked)
            return;
        at--;
    } else {
        t->broad_kept++;
    }
    while (at > 0 && t->broad[at - 1].marked < marked) {
        t->broad[at] = t->broad[at - 1];
        at--;
    }
    t->broad[at].at_us = since(t, at_us);
    t->broad[at].marked = marked;
    copy_name(t->broad[at].text, sizeof(t->broad[at].text), text);
}

// ── The lines ────────────────────────────────────────────────────────
// Written by hand into the caller's buffer so the file needs no library:
// text, unsigned numbers, and microseconds as seconds or milliseconds with
// one decimal.

typedef struct {
    char *s;
    size_t cap, at;
} Out;

static void put(Out *o, const char *text)
{
    while (*text != '\0' && o->at + 1 < o->cap)
        o->s[o->at++] = *text++;
    o->s[o->at] = '\0';
}

static void put_u(Out *o, uint64_t v)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    char text[24];
    for (int i = 0; i < n; i++)
        text[i] = digits[n - 1 - i];
    text[n] = '\0';
    put(o, text);
}

// `v` tenths as "<whole>.<tenth>".
static void put_tenths(Out *o, uint64_t v)
{
    put_u(o, v / 10);
    put(o, ".");
    put_u(o, v % 10);
}

static void put_s(Out *o, uint64_t us)
{
    put_tenths(o, (us + 50000) / 100000);
    put(o, " s");
}

// Milliseconds with one decimal, and without the unit: a list says it once.
static void put_ms_bare(Out *o, uint64_t us)
{
    put_tenths(o, (us + 50) / 100);
}

static void put_ms(Out *o, uint64_t us)
{
    put_ms_bare(o, us);
    put(o, " ms");
}

static Out out_of(char *s, size_t cap)
{
    Out o = {s, cap, 0};
    if (cap > 0)
        s[0] = '\0';
    return o;
}

// The index of the largest of `v[0..n)` not yet `said` and not zero, or -1.
static int largest(const uint64_t *v, const uint32_t *nonzero, const bool *said, int n)
{
    int best = -1;
    for (int k = 0; k < n; k++)
        if (!said[k] && (v[k] != 0 || (nonzero != NULL && nonzero[k] != 0)) && (best < 0 || v[k] > v[best]))
            best = k;
    return best;
}

// Layouts by who asked, the costliest first: "a script's read 5 in 214.4,
// after a script's changes 18 in 970.0" (milliseconds).
static void put_whys(Out *o, const uint32_t *n, const uint64_t *us)
{
    bool said[YONDER_LT_WHYS] = {false};
    const char *sep = "";
    for (int w; (w = largest(us, n, said, YONDER_LT_WHYS)) >= 0;) {
        said[w] = true;
        put(o, sep);
        sep = ", ";
        put(o, kWhy[w]);
        put(o, " ");
        put_u(o, n[w]);
        put(o, " in ");
        put_ms_bare(o, us[w]);
    }
}

uint32_t yonder_lt_slices_used(const yonder_lt_t *t)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < YONDER_LT_SLICES; i++) {
        const yonder_lt_slice_t *s = &t->slices[i];
        bool any = s->layouts != 0 || s->tasks != 0 || s->bytes != 0;
        for (int k = 0; k < YONDER_LT_STEPS && !any; k++)
            any = s->step_us[k] != 0;
        if (any)
            used = i + 1;
    }
    return used;
}

void yonder_lt_summary(const yonder_lt_t *t, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    put(&o, "from the navigation's start, in slices of ");
    put_u(&o, YONDER_LT_SLICE_US / 1000000);
    put(&o, " s, the window thread's steps");
    for (int m = 0; m < YONDER_LT_MARKS; m++) {
        put(&o, m == 0 ? "; " : ", ");
        put(&o, kMark[m]);
        put(&o, " ");
        if (t->marks[m] < 0)
            put(&o, "-");
        else
            put_s(&o, (uint64_t)t->marks[m]);
    }
    put(&o, "; ");
    put_u(&o, t->nparse);
    put(&o, " parser calls in ");
    put_ms(&o, t->parse_us);
    put(&o, "; ");
    put(&o, "updates refused as broad: ");
    put_u(&o, t->nbroad);
    for (uint32_t i = 0; i < t->nbroad_by; i++) {
        put(&o, i == 0 ? " (by what crossed the half: " : ", ");
        put(&o, t->broad_by[i].name != NULL ? t->broad_by[i].name : "no mark told");
        put(&o, " ");
        put_u(&o, t->broad_by[i].n);
    }
    if (t->broad_by_other != 0) {
        put(&o, ", other ");
        put_u(&o, t->broad_by_other);
    }
    if (t->nbroad_by != 0)
        put(&o, ")");
    if (t->back_updates != 0) {
        put(&o, "; back indexes counted again in ");
        put_u(&o, t->back_updates);
        put(&o, " updates: ");
        put_u(&o, t->back_walks);
        put(&o, " walks, ");
        put_u(&o, t->back_walked);
        put(&o, " children counted, ");
        put_u(&o, t->back_turned);
        put(&o, " turned");
    }
    if (t->marks_updates != 0) {
        put(&o, "; ");
        put_u(&o, t->marks_updates);
        put(&o, " updates restyled ");
        put_u(&o, t->marks_marked);
        put(&o, " elements their changes marked and ");
        put_u(&o, t->marks_inherited);
        put(&o, " below a changed winner");
        bool first = true;
        for (uint32_t k = 0; k < t->nmark_kinds; k++) {
            if (t->marks_by[k] == 0)
                continue;
            put(&o, first ? " (every mark by kind: " : ", ");
            put(&o, t->mark_names[k] != NULL ? t->mark_names[k] : "?");
            put(&o, " ");
            put_u(&o, t->marks_by[k]);
            first = false;
        }
        if (!first)
            put(&o, ")");
    }
    put(&o, "; ");
    put_u(&o, t->nlongs + t->more_longs);
    put(&o, " steps of ");
    put_u(&o, YONDER_LT_LONG_US / 1000);
    put(&o, " ms or more");
    if (t->more_longs != 0) {
        put(&o, ", the ");
        put_u(&o, t->nlongs);
        put(&o, " longest listed, the other ");
        put_u(&o, t->more_longs);
        put(&o, " ");
        put_s(&o, t->more_longs_us);
        put(&o, " in all");
    }
}

// A step's share is said when it is at least this much of the slice's
// time, and a part inside a step when it is at least a millisecond; the
// rest of the slice is summed.
#define LT_SAID_US 10000ull
#define LT_PART_US 1000ull

void yonder_lt_slice_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    if (i >= YONDER_LT_SLICES)
        return;
    const yonder_lt_slice_t *s = &t->slices[i];
    uint64_t covered = 0;
    for (int k = 0; k < YONDER_LT_STEPS; k++)
        covered += s->step_us[k];
    uint64_t busy = covered - s->step_us[YONDER_LT_WAIT];
    put(&o, "busy ");
    put_u(&o, covered != 0 ? (busy * 100 + covered / 2) / covered : 0);
    put(&o, "% of ");
    put_s(&o, covered);
    // The steps, longest first, each with what was inside it.
    bool said[YONDER_LT_STEPS] = {false};
    uint64_t rest = 0;
    const char *sep = ": ";
    for (int best; (best = largest(s->step_us, NULL, said, YONDER_LT_STEPS)) >= 0;) {
        said[best] = true;
        if (s->step_us[best] < LT_SAID_US) {
            rest += s->step_us[best];
            continue;
        }
        put(&o, sep);
        sep = ", ";
        put(&o, kStep[best]);
        put(&o, " ");
        put_ms(&o, s->step_us[best]);
        const char *in = " (";
        for (int k = 0; k < YONDER_LT_INS; k++) {
            if (s->in_us[best][k] < LT_PART_US)
                continue;
            put(&o, in);
            in = ", ";
            put(&o, kIn[k]);
            put(&o, " ");
            put_ms_bare(&o, s->in_us[best][k]);
        }
        if (in[0] == ',')
            put(&o, ")");
    }
    if (rest != 0) {
        put(&o, sep);
        put(&o, "the rest ");
        put_ms(&o, rest);
    }
    put(&o, "; layouts ");
    put_u(&o, s->layouts);
    put(&o, " in ");
    put_ms(&o, s->layout_us);
    if (s->layouts != 0) {
        put(&o, " (");
        put_whys(&o, s->why_n, s->why_us);
        put(&o, ")");
    }
    put(&o, "; script tasks ");
    put_u(&o, s->tasks);
    put(&o, ", forcing ");
    put_u(&o, s->forced);
    put(&o, " layouts in ");
    put_ms(&o, s->forced_us);
    if (s->bytes != 0) {
        put(&o, "; parsed ");
        put_u(&o, (s->bytes + 512) / 1024);
        put(&o, " KiB");
    }
}

void yonder_lt_layouts_line(const yonder_lt_t *t, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    put(&o, "by who asked, in the step they ran in, the costliest first");
    // The grid's cells, picked costliest first.
    bool said[YONDER_LT_WHYS][YONDER_LT_STEPS + 1] = {{false}};
    const char *sep = ": ";
    for (;;) {
        int bw = -1, bs = -1;
        for (int w = 0; w < YONDER_LT_WHYS; w++)
            for (int s = 0; s <= YONDER_LT_STEPS; s++)
                if (!said[w][s] && t->grid_n[w][s] != 0 &&
                    (bw < 0 || t->grid_us[w][s] > t->grid_us[bw][bs])) {
                    bw = w;
                    bs = s;
                }
        if (bw < 0)
            break;
        said[bw][bs] = true;
        put(&o, sep);
        sep = ", ";
        put(&o, kWhy[bw]);
        put(&o, " in ");
        put(&o, column(bs));
        put(&o, " ");
        put_u(&o, t->grid_n[bw][bs]);
        put(&o, " in ");
        put_ms(&o, t->grid_us[bw][bs]);
    }
    if (sep[0] == ':')
        put(&o, ": none");
    // The fresh builds libgarb timed by part, by who asked, the costliest
    // first.
    uint64_t built[YONDER_LT_WHYS];
    uint32_t nbuilt = 0;
    for (int w = 0; w < YONDER_LT_WHYS; w++) {
        built[w] = t->build_sheets[w] + t->build_table[w] + t->build_matching[w];
        nbuilt += t->builds[w];
    }
    if (nbuilt != 0) {
        put(&o, "; fresh cascades by part");
        bool done[YONDER_LT_WHYS] = {false};
        sep = ": ";
        for (int w; (w = largest(built, t->builds, done, YONDER_LT_WHYS)) >= 0;) {
            done[w] = true;
            put(&o, sep);
            sep = ", ";
            put(&o, kWhy[w]);
            put(&o, " ");
            put_u(&o, t->builds[w]);
            put(&o, " in ");
            put_ms(&o, built[w]);
            put(&o, " (");
            put_u(&o, t->kept[w]);
            put(&o, " kept; reading the sheets ");
            put_ms_bare(&o, t->build_sheets[w]);
            put(&o, ", the table ");
            put_ms_bare(&o, t->build_table[w]);
            put(&o, ", matching ");
            put_ms_bare(&o, t->build_matching[w]);
            put(&o, "; ");
            put_u(&o, t->build_rules[w] / t->builds[w]);
            put(&o, " rules and ");
            put_u(&o, t->build_elements[w] / t->builds[w]);
            put(&o, " elements a build)");
        }
    }
    if (t->nagain != 0) {
        put(&o, "; read again because");
        for (uint32_t i = 0; i < t->nagain; i++) {
            put(&o, i == 0 ? ": " : ", ");
            put(&o, t->again[i].name);
            put(&o, " ");
            put_u(&o, t->again[i].n);
        }
        if (t->again_other != 0) {
            put(&o, ", other ");
            put_u(&o, t->again_other);
        }
    }
    uint32_t timed = 0;
    for (int w = 0; w < YONDER_LT_WHYS; w++)
        timed += t->cascades[w];
    if (timed == 0) {
        put(&o, "; cascades not timed whole (yonder --layout-profile times them)");
        return;
    }
    put(&o, "; cascades the layout profile timed");
    bool done[YONDER_LT_WHYS] = {false};
    sep = ": ";
    for (int w; (w = largest(t->cascade_us, t->cascades, done, YONDER_LT_WHYS)) >= 0;) {
        done[w] = true;
        put(&o, sep);
        sep = ", ";
        put(&o, kWhy[w]);
        put(&o, " ");
        put_u(&o, t->cascades[w]);
        put(&o, " in ");
        put_ms(&o, t->cascade_us[w]);
        put(&o, " (");
        put_u(&o, t->fresh[w]);
        put(&o, " built fresh in ");
        put_ms(&o, t->fresh_us[w]);
        put(&o, ")");
    }
}

void yonder_lt_long_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    if (i >= t->nlongs)
        return;
    const yonder_lt_long_t *l = &t->longs[i];
    put(&o, "at ");
    put_s(&o, l->at_us);
    put(&o, ", ");
    put(&o, kStep[l->step < YONDER_LT_STEPS ? l->step : 0]);
    put(&o, " ");
    put_ms(&o, l->us);
    put(&o, "; layouts in it ");
    put_u(&o, l->layouts);
    put(&o, " in ");
    put_ms(&o, l->in_us[YONDER_LT_IN_LAYOUTS]);
    if (l->layouts != 0) {
        put(&o, " (");
        put_whys(&o, l->why_n, l->why_us);
        put(&o, ")");
    }
    put(&o, ", its script tasks forcing ");
    put_u(&o, l->forced);
    put(&o, " in ");
    put_ms(&o, l->forced_us);
    if (l->in_us[YONDER_LT_IN_DRAWING] >= LT_PART_US) {
        put(&o, "; drawing ");
        put_ms(&o, l->in_us[YONDER_LT_IN_DRAWING]);
    }
    if (l->task[0] != '\0') {
        put(&o, "; its longest task ");
        put(&o, l->task);
        put(&o, ", ");
        put_ms(&o, l->task_us);
        put(&o, ", forcing ");
        put_u(&o, l->task_layouts);
        put(&o, " layouts in ");
        put_ms(&o, l->task_layout_us);
    }
}

void yonder_lt_broad_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    if (i >= t->broad_kept)
        return;
    put(&o, "at ");
    put_s(&o, t->broad[i].at_us);
    put(&o, ", ");
    put(&o, t->broad[i].text);
}

void yonder_lt_parse_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap)
{
    Out o = out_of(out, cap);
    if (i >= t->parse_kept)
        return;
    put(&o, "at ");
    put_s(&o, t->parse[i].at_us);
    put(&o, ", ");
    put_ms(&o, t->parse[i].us);
    put(&o, ": ");
    put(&o, t->parse[i].text);
}
