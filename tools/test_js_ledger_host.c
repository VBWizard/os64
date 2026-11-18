/* test_js_ledger_host.c — the JS speed ledger's arithmetic
 * (userland/libjs/runtime/ledger.c), on the host: each kind's own time,
 * nesting (script inside a host call, a layout and a collection inside it),
 * what a callee forced, the lost frames and rows, a reset inside a task, the
 * profile's charge of interpreter time only, and the rule that the kinds'
 * totals add up to the outermost frames' elapsed for any nesting. */
#include "ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static ledger_t *fresh(void)
{
    ledger_t *l = calloc(1, sizeof(*l));
    if (l == NULL) abort();
    return l;
}

static uint64_t total(const ledger_t *l)
{
    uint64_t t = 0;
    for (int k = 0; k < LEDGER_KINDS; k++)
        t += l->self[k];
    return t;
}

static const ledger_callee_t *row(const ledger_t *l, const void *fn, int magic)
{
    for (unsigned i = 0; i < LEDGER_CALLEES; i++)
        if (l->callees[i].used && l->callees[i].function == fn && l->callees[i].magic == magic)
            return &l->callees[i];
    return NULL;
}

static int f1, f2;   /* callee identities */

static void nesting_cases(void)
{
    ledger_t *l = fresh();
    /* task 0..100: script, calls appendChild 10..60 which runs a listener
     * (script) 20..40 that forces a layout through offsetWidth 25..35, and
     * the collector runs inside appendChild 45..50. */
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    ledger_push(l, LEDGER_NATIVE, 10, &f1, 3);
    ledger_push(l, LEDGER_ENGINE, 20, NULL, 0);
    ledger_push(l, LEDGER_NATIVE, 24, &f2, 7);
    ledger_push(l, LEDGER_LAYOUT, 25, NULL, 0);
    ledger_pop(l, 35);
    ledger_pop(l, 36);
    ledger_pop(l, 40);
    ledger_push(l, LEDGER_COLLECTOR, 45, NULL, 0);
    ledger_pop(l, 50);
    ledger_pop(l, 60);
    ledger_pop(l, 100);
    check(l->depth == 0, "nesting: every frame closed");
    check(l->self[LEDGER_LAYOUT] == 10, "nesting: the layout's own time");
    check(l->self[LEDGER_COLLECTOR] == 5, "nesting: the collection's own time");
    /* f2 24..36 less the layout 10 = 2; f1 10..60 less script 20 and the
     * collection 5 = 25. */
    check(l->self[LEDGER_NATIVE] == 27, "nesting: the host calls' own time, script and layout taken out");
    /* engine: 0..100 less f1's 50 = 50, plus the listener 20..40 less f2's 12 = 8. */
    check(l->self[LEDGER_ENGINE] == 58, "nesting: the interpreter's own time, at both depths");
    check(total(l) == 100, "nesting: the kinds add up to the task");
    const ledger_callee_t *a = row(l, &f1, 3), *b = row(l, &f2, 7);
    check(a != NULL && a->calls == 1 && a->self == 25 && a->forced == 0, "nesting: appendChild's row");
    check(b != NULL && b->calls == 1 && b->self == 2 && b->forced == 1 && b->forced_ticks == 10,
          "nesting: offsetWidth's row names the layout it forced");
    check(l->count[LEDGER_NATIVE] == 2 && l->count[LEDGER_ENGINE] == 2, "nesting: frames counted by kind");
    free(l);
}

static void layout_outside_native_case(void)
{
    ledger_t *l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    ledger_push(l, LEDGER_LAYOUT, 3, NULL, 0);
    ledger_pop(l, 9);
    ledger_pop(l, 10);
    check(l->self[LEDGER_LAYOUT] == 6 && l->self[LEDGER_ENGINE] == 4 && total(l) == 10,
          "a layout with no host call open: its own kind, charged to no row");
    free(l);
}

static void lost_cases(void)
{
    ledger_t *l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    for (unsigned i = 1; i < LEDGER_DEPTH + 5; i++)
        ledger_push(l, LEDGER_NATIVE, i, &f1, (int)i);
    check(l->frames_lost == 5, "depth: frames past the stack counted as lost");
    for (unsigned i = 1; i < LEDGER_DEPTH + 5; i++)
        ledger_pop(l, 1000 + i);
    ledger_pop(l, 2000);
    check(l->depth == 0, "depth: pops balance across the lost frames");
    check(total(l) == 2000, "depth: a lost frame's time stays in the frame below");
    free(l);

    l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    for (unsigned i = 0; i < LEDGER_CALLEES + 3; i++) {
        ledger_push(l, LEDGER_NATIVE, 10 * i + 1, &f1, (int)i);
        ledger_pop(l, 10 * i + 3);
    }
    ledger_pop(l, 100000);
    check(l->callees_lost == 3, "callees: calls past a full table counted");
    check(l->self[LEDGER_NATIVE] == 2 * (LEDGER_CALLEES + 3), "callees: an unrecorded callee's time still in NATIVE");
    check(total(l) == 100000, "callees: the kinds still add up");
    free(l);

    l = fresh();
    ledger_pop(l, 5);
    check(l->depth == 0 && total(l) == 0, "a pop with nothing open does nothing");
    ledger_push(l, LEDGER_ENGINE, 50, NULL, 0);
    ledger_pop(l, 40);
    check(total(l) == 0, "a clock that stepped back charges nothing");
    free(l);
}

static void reset_case(void)
{
    ledger_t *l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    ledger_push(l, LEDGER_NATIVE, 10, &f1, 1);
    ledger_pop(l, 20);
    ledger_push(l, LEDGER_NATIVE, 30, &f2, 1);
    ledger_reset(l, 35);
    check(l->depth == 2 && total(l) == 0 && row(l, &f1, 1) == NULL, "reset: totals and rows zeroed, frames still open");
    ledger_pop(l, 45);
    ledger_pop(l, 50);
    check(l->self[LEDGER_NATIVE] == 10 && l->self[LEDGER_ENGINE] == 5 && total(l) == 15,
          "reset: only the time after it is counted, open frames from the reset");
    free(l);
}

static void drop_case(void)
{
    ledger_t *l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    ledger_push(l, LEDGER_ENGINE, 10, NULL, 0);   /* a task begun that never ran */
    ledger_drop(l);
    ledger_push(l, LEDGER_NATIVE, 30, &f1, 0);
    ledger_pop(l, 40);
    ledger_pop(l, 50);
    check(l->depth == 0 && l->self[LEDGER_ENGINE] == 40 && l->self[LEDGER_NATIVE] == 10 && total(l) == 50,
          "drop: a dropped frame charges nothing; its time stays in the frame below");
    check(l->count[LEDGER_ENGINE] == 1, "drop: a dropped frame is not counted");
    ledger_drop(l);
    check(l->depth == 0, "drop: with nothing open, nothing");
    free(l);
}

static ledger_place_t place(const void *fn, const char *name)
{
    ledger_place_t p;
    memset(&p, 0, sizeof(p));
    p.function = fn;
    snprintf(p.name, sizeof(p.name), "%s", name);
    snprintf(p.file, sizeof(p.file), "app.js");
    p.line = 1;
    p.column = 100;
    return p;
}

static const ledger_sample_t *sample_row(const ledger_t *l, const void *fn, const void *caller)
{
    for (unsigned i = 0; i < LEDGER_SAMPLES; i++)
        if (l->profile[i].used && l->profile[i].at.function == fn && l->profile[i].caller.function == caller)
            return &l->profile[i];
    return NULL;
}

static int g1, g2, g3;   /* script functions */

static void profile_cases(void)
{
    ledger_t *l = fresh();
    ledger_place_t a = place(&g1, "f"), b = place(&g2, "g"), c = place(&g3, "h");
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    check(ledger_sample(l, 10, &a, &b), "profile: a sample recorded");
    /* a 50-tick host call between two samples is not interpreter time */
    ledger_push(l, LEDGER_NATIVE, 12, &f1, 0);
    ledger_pop(l, 62);
    ledger_sample(l, 70, &a, &b);
    const ledger_sample_t *s = sample_row(l, &g1, &g2);
    /* 0..10, then 10..12 and 62..70 around the call */
    check(s != NULL && s->samples == 2 && s->ticks == 10 + 10, "profile: a sample after a host call is charged only interpreter time");
    /* script running inside a host call is interpreter time */
    ledger_push(l, LEDGER_NATIVE, 70, &f1, 0);
    ledger_push(l, LEDGER_ENGINE, 75, NULL, 0);
    ledger_sample(l, 95, &c, &a);
    const ledger_sample_t *t = sample_row(l, &g3, &g1);
    check(t != NULL && t->ticks == 20, "profile: script inside a host call is charged, the call itself not");
    ledger_pop(l, 100);
    ledger_pop(l, 105);
    ledger_pop(l, 110);
    check(ledger_engine_live(l, 110) == l->self[LEDGER_ENGINE], "profile: every frame closed, live time is the total");
    check(sample_row(l, &g1, &g3) == NULL, "profile: rows are keyed by the function and its caller");
    free(l);

    l = fresh();
    ledger_push(l, LEDGER_ENGINE, 0, NULL, 0);
    static int many[LEDGER_SAMPLES + 4];
    for (unsigned i = 0; i < LEDGER_SAMPLES + 4; i++) {
        ledger_place_t p = place(&many[i], "x");
        ledger_sample(l, i + 1, &p, NULL);
    }
    check(l->samples_lost == 4 && l->samples == LEDGER_SAMPLES + 4, "profile: samples past a full table counted");
    free(l);
}

/* For any nesting, the kinds add up to the outermost frames' elapsed. */
static void random_sum_case(void)
{
    srand(64);
    for (int round = 0; round < 2000; round++) {
        ledger_t *l = fresh();
        uint64_t now = 0, outer = 0, outer_start = 0;
        unsigned depth = 0;
        int steps = 1 + rand() % 400;
        for (int i = 0; i < steps; i++) {
            now += (uint64_t)(rand() % 50);
            if (depth == 0 || (rand() % 2 == 0 && depth < LEDGER_DEPTH + 10)) {
                if (depth == 0) outer_start = now;
                ledger_push(l, (ledger_kind_t)(rand() % LEDGER_KINDS), now, &f1, rand() % 40);
                depth++;
            } else {
                ledger_pop(l, now);
                if (--depth == 0) outer += now - outer_start;
            }
        }
        while (depth > 0) {
            now += (uint64_t)(rand() % 50);
            ledger_pop(l, now);
            if (--depth == 0) outer += now - outer_start;
        }
        if (total(l) != outer || l->depth != 0) {
            check(0, "random nesting: the kinds add up to the outermost elapsed");
            free(l);
            return;
        }
        free(l);
    }
    check(1, "random nesting: the kinds add up to the outermost elapsed");
}

int main(void)
{
    nesting_cases();
    layout_outside_native_case();
    lost_cases();
    reset_case();
    drop_case();
    profile_cases();
    random_sum_case();
    printf("JS ledger: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
