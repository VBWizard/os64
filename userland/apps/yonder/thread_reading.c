// thread_reading.c — where the window's thread went since the last reading
// (thread_reading.h).

#include "thread_reading.h"

static const struct {
    yonder_lt_step_t step;
    const char *name;
} kCall[YONDER_TR_CALLS] = {
    [YONDER_TR_WAIT] = {YONDER_LT_WAIT, "waiting"},
    [YONDER_TR_DISPATCH] = {YONDER_LT_INPUT, "the dispatcher's turn"},
    [YONDER_TR_RELAYOUT] = {YONDER_LT_RELAYOUT, "a layout the window owed"},
    [YONDER_TR_STREAM] = {YONDER_LT_STREAM, "the stream's turn"},
    [YONDER_TR_COMING] = {YONDER_LT_COMING, "the coming page's turn"},
    [YONDER_TR_OWED] = {YONDER_LT_SCRIPTS, "the rendering step owed before a task"},
    [YONDER_TR_FORMS] = {YONDER_LT_SCRIPTS, "the fields flushed"},
    [YONDER_TR_TASK] = {YONDER_LT_SCRIPTS, "a task found and run"},
    [YONDER_TR_FRAMES] = {YONDER_LT_SCRIPTS, "the animations and the animation frames"},
    [YONDER_TR_AFTER] = {YONDER_LT_SCRIPTS, "the rendering step after a task"},
    [YONDER_TR_ASKS] = {YONDER_LT_SCRIPTS, "what a task asked of the window"},
    [YONDER_TR_POINTER] = {YONDER_LT_SCRIPTS, "the pointer's events"},
    [YONDER_TR_BODIES] = {YONDER_LT_MEDIA, "response bodies"},
    [YONDER_TR_PLAYERS_ASKED] = {YONDER_LT_MEDIA, "the players asked to run"},
    [YONDER_TR_MEDIA] = {YONDER_LT_MEDIA, "the media elements"},
    [YONDER_TR_VIDEOS] = {YONDER_LT_MEDIA, "the videos"},
    [YONDER_TR_KEYS] = {YONDER_LT_MEDIA, "the media keys"},
    [YONDER_TR_PLAYERS_SHOWN] = {YONDER_LT_MEDIA, "the players' widgets"},
    [YONDER_TR_MOTION_TICK] = {YONDER_LT_MOTION, "the animations' tick"},
    [YONDER_TR_VIDEOS_TICK] = {YONDER_LT_MOTION, "the videos' tick"},
    [YONDER_TR_STORAGE] = {YONDER_LT_UPKEEP, "storage"},
    [YONDER_TR_COOKIES] = {YONDER_LT_UPKEEP, "the cookie file"},
    [YONDER_TR_CENSUS] = {YONDER_LT_UPKEEP, "the census"},
    [YONDER_TR_CHILDREN] = {YONDER_LT_UPKEEP, "children collected"},
    [YONDER_TR_DOWNLOADS] = {YONDER_LT_UPKEEP, "downloads"},
    [YONDER_TR_HOLD] = {YONDER_LT_UPKEEP, "a held step's list"},
    [YONDER_TR_PICTURES] = {YONDER_LT_UPKEEP, "pictures settled"},
    [YONDER_TR_TITLE] = {YONDER_LT_UPKEEP, "the title"},
    [YONDER_TR_RESIZES] = {YONDER_LT_OBSERVERS, "resize observers"},
    [YONDER_TR_BEFORE_PAINT] = {YONDER_LT_OBSERVERS, "the work before a paint"},
    [YONDER_TR_INTERSECTIONS] = {YONDER_LT_OBSERVERS, "intersection observers"},
    [YONDER_TR_PAINT] = {YONDER_LT_PAINT, "the paint"},
    [YONDER_TR_MOTION_PAINTED] = {YONDER_LT_PAINT, "the animations told of it"},
    [YONDER_TR_BELLS] = {YONDER_LT_CLOSE, "the bells"},
    [YONDER_TR_EXIT] = {YONDER_LT_CLOSE, "the look at leaving"},
    [YONDER_TR_FRAME_CLOSE] = {YONDER_LT_CLOSE, "the frame closed"},
    [YONDER_TR_SCHEDULE] = {YONDER_LT_CLOSE, "the next moment scheduled"},
};

static const char *const kIn[YONDER_TR_INS] = {
    [YONDER_TR_IN_TASKS] = "tasks",
    [YONDER_TR_IN_LAYOUTS] = "layouts",
    [YONDER_TR_IN_MODELS] = "model rebuilds",
    [YONDER_TR_IN_SAID] = "what tasks said",
    [YONDER_TR_IN_DRAWING] = "drawing",
};

const char *yonder_tr_call_name(yonder_tr_call_t call)
{
    return (unsigned)call < YONDER_TR_CALLS ? kCall[call].name : "?";
}

yonder_lt_step_t yonder_tr_call_step(yonder_tr_call_t call)
{
    return (unsigned)call < YONDER_TR_CALLS ? kCall[call].step : YONDER_LT_STEPS;
}

static void zero(void *p, size_t n)
{
    unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

static uint64_t since(int64_t from, int64_t to)
{
    return to > from ? (uint64_t)(to - from) : 0;
}

void yonder_tr_reset(yonder_tr_t *t, int64_t now, const yonder_tr_collector_t *collector,
                     const yonder_tr_list_t *list)
{
    yonder_tr_call_t call = t->on ? t->call : YONDER_TR_CALLS;
    yonder_lt_step_t step = t->on ? t->step : YONDER_LT_STEPS;
    zero(t, sizeof(*t));
    t->on = true;
    t->from = now;
    t->from_first_call = call == YONDER_TR_CALLS;
    t->call = call;
    t->step = step;
    t->call_from = now;
    if (call != YONDER_TR_CALLS) {
        t->call_n[call] = 1;
        t->turns[step] = 1;
    }
    if (collector != NULL)
        t->collector_from = *collector;
    if (list != NULL)
        t->list_from = *list;
}

void yonder_tr_off(yonder_tr_t *t)
{
    t->on = false;
    t->call = YONDER_TR_CALLS;
}

void yonder_tr_call(yonder_tr_t *t, yonder_tr_call_t call, int64_t now)
{
    if (!t->on || (unsigned)call >= YONDER_TR_CALLS)
        return;
    if (t->from_first_call) {
        t->from = now;
        t->from_first_call = false;
    }
    if (t->call != YONDER_TR_CALLS)
        t->call_ticks[t->call] += since(t->call_from, now);
    yonder_lt_step_t step = kCall[call].step;
    if (t->call == YONDER_TR_CALLS || t->step != step)
        t->turns[step]++;
    t->call_n[call]++;
    t->call = call;
    t->step = step;
    t->call_from = now;
}

void yonder_tr_inside(yonder_tr_t *t, yonder_tr_in_t in, uint64_t ticks)
{
    if (!t->on || t->call == YONDER_TR_CALLS || (unsigned)in >= YONDER_TR_INS)
        return;
    t->in_ticks[t->step][in] += ticks;
    t->in_n[t->step][in]++;
}

void yonder_tr_intersections(yonder_tr_t *t, uint64_t targets)
{
    if (!t->on)
        return;
    t->observed_steps++;
    t->observed += targets;
    if (targets > t->observed_most)
        t->observed_most = targets;
}

void yonder_tr_checked(yonder_tr_t *t, uint64_t records)
{
    if (!t->on)
        return;
    t->checked_turns++;
    t->checked += records;
    if (records > t->checked_most)
        t->checked_most = records;
}

static bool same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void cost_add(yonder_tr_cost_t *c, uint64_t ticks, uint64_t layouts, uint64_t layout_ticks)
{
    c->n++;
    c->ticks += ticks;
    if (ticks > c->most)
        c->most = ticks;
    c->layouts += layouts;
    c->layout_ticks += layout_ticks;
}

void yonder_tr_timer(yonder_tr_t *t, const char *site, uint64_t ticks, uint64_t layouts, uint64_t layout_ticks)
{
    if (!t->on)
        return;
    if (site == NULL) {
        cost_add(&t->unnamed, ticks, layouts, layout_ticks);
        return;
    }
    for (uint32_t i = 0; i < t->nsites; i++)
        if (same(t->sites[i].site, site)) {
            cost_add(&t->sites[i].cost, ticks, layouts, layout_ticks);
            return;
        }
    if (t->nsites == YONDER_TR_SITES) {
        cost_add(&t->other, ticks, layouts, layout_ticks);
        return;
    }
    char *to = t->sites[t->nsites].site;
    size_t n = 0;
    while (site[n] != '\0' && n + 1 < YONDER_TR_SITE_TEXT) {
        to[n] = site[n];
        n++;
    }
    to[n] = '\0';
    cost_add(&t->sites[t->nsites++].cost, ticks, layouts, layout_ticks);
}

// ── The lines ──────────────────────────────────────────────────────────

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

static void put_tenths(Out *o, uint64_t v)
{
    put_u(o, v / 10);
    put(o, ".");
    put_u(o, v % 10);
}

// Ticks as milliseconds with one decimal, rounded, without the unit.
static void put_ms(Out *o, uint64_t ticks, uint64_t per_ms)
{
    put_tenths(o, per_ms != 0 ? (ticks * 10 + per_ms / 2) / per_ms : 0);
}

// Bytes as MiB with one decimal.
static void put_mib(Out *o, uint64_t bytes)
{
    put_tenths(o, (bytes * 10 + (1u << 19)) >> 20);
}

void yonder_tr_steps_line(const yonder_tr_t *t, int64_t now, uint64_t per_ms, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    if (cap == 0)
        return;
    out[0] = '\0';
    uint64_t calls[YONDER_TR_CALLS], steps[YONDER_LT_STEPS] = {0}, all = 0;
    for (int c = 0; c < YONDER_TR_CALLS; c++)
        calls[c] = t->call_ticks[c];
    if (t->on && t->call != YONDER_TR_CALLS)
        calls[t->call] += since(t->call_from, now);
    for (int c = 0; c < YONDER_TR_CALLS; c++) {
        steps[kCall[c].step] += calls[c];
        all += calls[c];
    }
    put(&o, "in ");
    put_ms(&o, t->from_first_call ? 0 : since(t->from, now), per_ms);
    put(&o, " ms the loop's steps took ");
    put_ms(&o, all, per_ms);
    put(&o, " ms, busy ");
    put_u(&o, all != 0 ? (all - steps[YONDER_LT_WAIT]) * 100 / all : 0);
    put(&o, "%");
    bool shown[YONDER_LT_STEPS] = {false};
    for (;;) {
        int s = -1;
        for (int k = 0; k < YONDER_LT_STEPS; k++)
            if (!shown[k] && t->turns[k] != 0 && (s < 0 || steps[k] > steps[s]))
                s = k;
        if (s < 0)
            break;
        shown[s] = true;
        put(&o, "; ");
        put(&o, yonder_lt_step_name((yonder_lt_step_t)s));
        put(&o, " ");
        put_ms(&o, steps[s], per_ms);
        put(&o, " ms in ");
        put_u(&o, t->turns[s]);
        put(&o, " turns");
        // Its calls of a millisecond or more, the costliest first; the rest
        // summed, so the parts add up to the step.
        bool listed[YONDER_TR_CALLS] = {false};
        uint64_t rest = 0, rest_n = 0;
        const char *sep = " (";
        for (;;) {
            int c = -1;
            for (int k = 0; k < YONDER_TR_CALLS; k++)
                if (!listed[k] && kCall[k].step == (yonder_lt_step_t)s && t->call_n[k] != 0 &&
                    (c < 0 || calls[k] > calls[c]))
                    c = k;
            if (c < 0)
                break;
            listed[c] = true;
            if (per_ms == 0 || calls[c] < per_ms) {
                rest += calls[c];
                rest_n += t->call_n[c];
                continue;
            }
            put(&o, sep);
            sep = ", ";
            put(&o, kCall[c].name);
            put(&o, " ");
            put_ms(&o, calls[c], per_ms);
            put(&o, " ms ");
            put_u(&o, t->call_n[c]);
            put(&o, " times");
        }
        if (rest_n != 0 && *sep == ',') {
            put(&o, ", the other calls ");
            put_ms(&o, rest, per_ms);
            put(&o, " ms ");
            put_u(&o, rest_n);
            put(&o, " times");
        }
        // What ran inside it, whichever call made it.
        const char *isep = *sep == ',' ? "; inside: " : " (inside: ";
        bool any = false;
        for (int i = 0; i < YONDER_TR_INS; i++) {
            if (t->in_n[s][i] == 0)
                continue;
            put(&o, isep);
            isep = ", ";
            any = true;
            put(&o, kIn[i]);
            put(&o, " ");
            put_u(&o, t->in_n[s][i]);
            put(&o, " in ");
            put_ms(&o, t->in_ticks[s][i], per_ms);
            put(&o, " ms");
        }
        if (*sep == ',' || any)
            put(&o, ")");
    }
}

static void put_cost(Out *o, const yonder_tr_cost_t *c, uint64_t per_ms)
{
    put_u(o, c->n);
    put(o, c->n == 1 ? " task, " : " tasks, ");
    put_ms(o, c->ticks, per_ms);
    put(o, " ms (each ");
    put_ms(o, c->n != 0 ? c->ticks / c->n : 0, per_ms);
    put(o, ", the most ");
    put_ms(o, c->most, per_ms);
    put(o, ")");
    if (c->layouts != 0) {
        put(o, ", forcing ");
        put_u(o, c->layouts);
        put(o, " layouts in ");
        put_ms(o, c->layout_ticks, per_ms);
        put(o, " ms");
    }
}

void yonder_tr_timers_line(const yonder_tr_t *t, uint64_t per_ms, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    if (cap == 0)
        return;
    out[0] = '\0';
    yonder_tr_cost_t all = {0};
    for (uint32_t i = 0; i < t->nsites; i++) {
        all.n += t->sites[i].cost.n;
        all.ticks += t->sites[i].cost.ticks;
    }
    all.n += t->other.n + t->unnamed.n;
    all.ticks += t->other.ticks + t->unnamed.ticks;
    if (all.n == 0) {
        put(&o, "none");
        return;
    }
    put_u(&o, all.n);
    put(&o, " timer tasks in ");
    put_ms(&o, all.ticks, per_ms);
    put(&o, " ms, from ");
    put_u(&o, t->nsites);
    put(&o, t->nsites == 1 ? " site" : " sites");
    bool listed[YONDER_TR_SITES] = {false};
    uint32_t shown = 0;
    yonder_tr_cost_t rest = {0};
    for (;;) {
        int k = -1;
        for (uint32_t i = 0; i < t->nsites; i++)
            if (!listed[i] && (k < 0 || t->sites[i].cost.ticks > t->sites[k].cost.ticks))
                k = (int)i;
        if (k < 0)
            break;
        listed[k] = true;
        const yonder_tr_cost_t *c = &t->sites[k].cost;
        if (shown == YONDER_TR_SITES_SHOWN) {
            rest.n += c->n;
            rest.ticks += c->ticks;
            if (c->most > rest.most)
                rest.most = c->most;
            rest.layouts += c->layouts;
            rest.layout_ticks += c->layout_ticks;
            continue;
        }
        shown++;
        put(&o, "; ");
        put(&o, t->sites[k].site);
        put(&o, ": ");
        put_cost(&o, c, per_ms);
    }
    if (rest.n != 0) {
        put(&o, "; the ");
        put_u(&o, t->nsites - shown);
        put(&o, " other sites: ");
        put_cost(&o, &rest, per_ms);
    }
    if (t->other.n != 0) {
        put(&o, "; sites past the ");
        put_u(&o, YONDER_TR_SITES);
        put(&o, " kept: ");
        put_cost(&o, &t->other, per_ms);
    }
    if (t->unnamed.n != 0) {
        put(&o, "; with no site known: ");
        put_cost(&o, &t->unnamed, per_ms);
    }
}

void yonder_tr_collector_line(const yonder_tr_t *t, const yonder_tr_collector_t *now_counts, uint64_t heap,
                              uint64_t limit, uint64_t per_ms, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    if (cap == 0)
        return;
    out[0] = '\0';
    // Counts behind the reading's start are another runtime's (the page's
    // script let go and its counts gone with it): said from their own start.
    static const yonder_tr_collector_t kNone;
    const yonder_tr_collector_t *a = &t->collector_from, *b = now_counts;
    if (b->runs < a->runs || b->runs_by_allocation < a->runs_by_allocation || b->walked < a->walked ||
        b->freed < a->freed || b->bytes_before < a->bytes_before || b->bytes_after < a->bytes_after ||
        b->ticks < a->ticks) {
        a = &kNone;
        put(&o, "counted from the script's start, the reading's being gone: ");
    }
    uint64_t runs = b->runs - a->runs;
    if (runs == 0) {
        put(&o, "no runs");
    } else {
        put_u(&o, runs);
        put(&o, runs == 1 ? " run (" : " runs (");
        put_u(&o, b->runs_by_allocation - a->runs_by_allocation);
        put(&o, " asked by the allocation threshold) in ");
        put_ms(&o, b->ticks - a->ticks, per_ms);
        put(&o, " ms, each ");
        put_ms(&o, (b->ticks - a->ticks) / runs, per_ms);
        put(&o, " ms on average; walked ");
        put_u(&o, b->walked - a->walked);
        put(&o, " objects, ");
        put_u(&o, (b->walked - a->walked) / runs);
        put(&o, " a run, and freed ");
        put_u(&o, b->freed - a->freed);
        put(&o, " in cycles; the engine's heap ");
        put_mib(&o, (b->bytes_before - a->bytes_before) / runs);
        put(&o, " MiB before a run and ");
        put_mib(&o, (b->bytes_after - a->bytes_after) / runs);
        put(&o, " MiB after, on average");
    }
    put(&o, "; the next run waits for the engine's heap to reach ");
    put_mib(&o, b->threshold);
    put(&o, " MiB; the script heap now ");
    put_mib(&o, heap);
    put(&o, " MiB of ");
    put_mib(&o, limit);
    put(&o, " MiB; since the script began, the longest run ");
    put_ms(&o, b->ticks_most, per_ms);
    put(&o, " ms, the most walked in one ");
    put_u(&o, b->walked_most);
}

void yonder_tr_walks_line(const yonder_tr_t *t, const yonder_tr_list_t *list_now, uint64_t held, char *out,
                          size_t cap)
{
    Out o = {out, cap, 0};
    if (cap == 0)
        return;
    out[0] = '\0';
    if (t->observed_steps == 0) {
        put(&o, "no intersection steps");
    } else {
        put_u(&o, t->observed_steps);
        put(&o, t->observed_steps == 1 ? " intersection step" : " intersection steps");
        put(&o, " walked ");
        put_u(&o, t->observed);
        put(&o, " observed targets, ");
        put_u(&o, t->observed / t->observed_steps);
        put(&o, " a step, the most ");
        put_u(&o, t->observed_most);
    }
    put(&o, "; the every-turn checks visited ");
    put_u(&o, t->checked);
    put(&o, t->checked == 1 ? " record in " : " records in ");
    put_u(&o, t->checked_turns);
    put(&o, t->checked_turns == 1 ? " turn, the most in one " : " turns, the most in one ");
    put_u(&o, t->checked_most);
    // A list behind the reading's start is another binding's, as above.
    bool behind = list_now->arms < t->list_from.arms || list_now->walked < t->list_from.walked;
    uint64_t arms = list_now->arms - (behind ? 0 : t->list_from.arms);
    uint64_t walked = list_now->walked - (behind ? 0 : t->list_from.walked);
    put(&o, behind ? "; the timer list, counted from its binding's start: " : "; the timer list: ");
    put_u(&o, list_now->live);
    put(&o, " live now, ");
    put_u(&o, arms);
    put(&o, arms == 1 ? " arm" : " arms");
    put(&o, " walked ");
    put_u(&o, walked);
    put(&o, " entries to find their places, ");
    put_u(&o, arms != 0 ? walked / arms : 0);
    put(&o, " an arm; the binding holds ");
    put_u(&o, held);
    put(&o, " values for the engine");
}
