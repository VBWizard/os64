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

// The step's time shared between the slices it ran across, by time; what
// runs past the last slice is not kept.
static void spread(yonder_lt_t *t, yonder_lt_step_t step, uint64_t from, uint64_t to)
{
    while (from < to && from < LT_END_US) {
        uint64_t i = from / YONDER_LT_SLICE_US;
        uint64_t edge = (i + 1) * YONDER_LT_SLICE_US;
        uint64_t end = to < edge ? to : edge;
        t->slices[i].step_us[step] += end - from;
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
    spread(t, (yonder_lt_step_t)t->now.step, from, to);
    t->now.us = to - from;
    // A wait is no work: an idle page's waits would crowd the work out of
    // the list, and the slices say how long the loop slept.
    if (t->now.step != YONDER_LT_WAIT && t->now.us >= YONDER_LT_LONG_US && from < LT_END_US)
        keep_long(t, &t->now);
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

void yonder_lt_layout(yonder_lt_t *t, int64_t from_us, uint64_t us)
{
    if (!t->on)
        return;
    uint64_t from = since(t, from_us);
    if (from < LT_END_US) {
        yonder_lt_slice_t *s = &t->slices[from / YONDER_LT_SLICE_US];
        s->layouts++;
        s->layout_us += us;
    }
    if (t->in_step) {
        t->now.layouts++;
        t->now.layout_us += us;
    }
}

void yonder_lt_task(yonder_lt_t *t, int64_t at_us, const char *what, uint64_t us, uint32_t layouts,
                    uint64_t layout_us)
{
    if (!t->on)
        return;
    uint64_t at = since(t, at_us);
    if (at < LT_END_US) {
        yonder_lt_slice_t *s = &t->slices[at / YONDER_LT_SLICE_US];
        s->tasks++;
        s->forced += layouts;
        s->forced_us += layout_us;
    }
    if (t->in_step) {
        t->now.forced += layouts;
        t->now.forced_us += layout_us;
    }
    if (t->in_step && us > t->now.task_us) {
        copy_name(t->now.task, sizeof(t->now.task), what);
        t->now.task_us = us;
        t->now.task_layouts = layouts;
        t->now.task_layout_us = layout_us;
    }
}

void yonder_lt_mark(yonder_lt_t *t, yonder_lt_mark_t mark, int64_t at_us)
{
    if (!t->on || (unsigned)mark >= YONDER_LT_MARKS || t->marks[mark] >= 0)
        return;
    t->marks[mark] = (int64_t)since(t, at_us);
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

static void put_ms(Out *o, uint64_t us)
{
    put_tenths(o, (us + 50) / 100);
    put(o, " ms");
}

static Out out_of(char *s, size_t cap)
{
    Out o = {s, cap, 0};
    if (cap > 0)
        s[0] = '\0';
    return o;
}

uint32_t yonder_lt_slices_used(const yonder_lt_t *t)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < YONDER_LT_SLICES; i++) {
        const yonder_lt_slice_t *s = &t->slices[i];
        bool any = s->layouts != 0 || s->tasks != 0;
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
// time; the rest of the slice is summed.
#define LT_SAID_US 10000ull

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
    // The steps, longest first: an order made by picking, eleven at most.
    bool said[YONDER_LT_STEPS] = {false};
    uint64_t rest = 0;
    const char *sep = ": ";
    for (;;) {
        int best = -1;
        for (int k = 0; k < YONDER_LT_STEPS; k++)
            if (!said[k] && s->step_us[k] != 0 && (best < 0 || s->step_us[k] > s->step_us[best]))
                best = k;
        if (best < 0)
            break;
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
    put(&o, "; script tasks ");
    put_u(&o, s->tasks);
    put(&o, ", forcing ");
    put_u(&o, s->forced);
    put(&o, " layouts in ");
    put_ms(&o, s->forced_us);
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
    put_ms(&o, l->layout_us);
    put(&o, ", its script tasks forcing ");
    put_u(&o, l->forced);
    put(&o, " in ");
    put_ms(&o, l->forced_us);
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
