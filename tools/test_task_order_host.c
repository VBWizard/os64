// The loop's fairness (userland/apps/yonder/task_order.c) on the host:
// the order of a turn's ready sources by cases, Fable's guarantee (a
// source that is never empty holds another's ready task no longer than
// the bound and one task), and a random walk of many sources held to the
// bound. Run by tools/test_task_order_host.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_order.h"

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static int order_is(const task_source_t *s, int n, uint64_t now, const int *want, int count)
{
    int got[16];
    int c = task_order(s, n, now, got);
    if (c != count) {
        printf("  got %d sources, wanted %d\n", c, count);
        return 0;
    }
    for (int i = 0; i < c; i++)
        if (got[i] != want[i]) {
            printf("  at %d got source %d, wanted %d\n", i, got[i], want[i]);
            return 0;
        }
    return 1;
}

static void cases(void)
{
    task_source_t none[3] = {{0}};
    check(order_is(none, 3, 1000, NULL, 0), "nothing ready: nothing to try");

    task_source_t young[4] = {{1, 990}, {0, 0}, {1, 950}, {1, 999}};
    check(order_is(young, 4, 1000, (int[]){0, 2, 3}, 3), "all young: the order of priority, the not-ready left out");

    task_source_t one_old[3] = {{1, 990}, {1, 995}, {1, 900}};
    check(order_is(one_old, 3, 1000, (int[]){2, 0, 1}, 3), "one ready the bound: it goes first, the young after in priority");

    task_source_t at_bound[2] = {{1, 999}, {1, 1000 - TASK_ORDER_BOUND_MS}};
    check(order_is(at_bound, 2, 1000, (int[]){1, 0}, 2), "ready exactly the bound: old");
    task_source_t under_bound[2] = {{1, 999}, {1, 1001 - TASK_ORDER_BOUND_MS}};
    check(order_is(under_bound, 2, 1000, (int[]){0, 1}, 2), "ready a millisecond under the bound: young");

    task_source_t two_old[4] = {{1, 999}, {1, 700}, {1, 500}, {1, 800}};
    check(order_is(two_old, 4, 1000, (int[]){2, 1, 3, 0}, 4), "several old: oldest first, then the young");

    task_source_t tie[3] = {{1, 999}, {1, 500}, {1, 500}};
    check(order_is(tie, 3, 1000, (int[]){1, 2, 0}, 3), "old of one age: the order of priority breaks the tie");

    task_source_t ahead[2] = {{1, 990}, {1, 5000}};
    check(order_is(ahead, 2, 1000, (int[]){0, 1}, 2), "a time ahead of the clock is young, never old");

    task_age_t a = {0};
    task_source_t s = task_age_observe(&a, true, 1000);
    check(s.ready && s.since_ms == 1000, "aged: ready since the turn that first found it");
    s = task_age_observe(&a, true, 1500);
    check(s.since_ms == 1000, "aged: still the same head a later turn");
    task_age_ran(&a, true, 1600);
    s = task_age_observe(&a, true, 1700);
    check(s.since_ms == 1600, "aged: the head after a run is ready from the run");
    task_age_ran(&a, false, 1800);
    s = task_age_observe(&a, false, 1900);
    check(!s.ready && a.since_ms == 0, "aged: none ready, no age kept");
    s = task_age_observe(&a, true, 0);
    check(s.ready && s.since_ms != 0, "aged: a head found at time zero is still ready");

    task_age_t st = {0};
    s = task_age_stamped(&st, true, 100);
    check(s.ready && s.since_ms == 100, "stamped: never run, its own stamp");
    task_age_ran(&st, true, 500);
    s = task_age_stamped(&st, true, 100);
    check(s.since_ms == 500, "stamped: a stamp before the source's last run counts from the run");
    s = task_age_stamped(&st, true, 900);
    check(s.since_ms == 900, "stamped: a stamp after it is its own");
    s = task_age_stamped(&st, false, 100);
    check(!s.ready, "stamped: none ready is none ready");
}

// Two sources: A, first in priority, never empty, its tasks `dur` long,
// aged by the loop, or (`backlog`) a stamped source whose every task was
// stamped at time 1, long before anything else (a flood of messages posted
// at once); B second, ready at `ready_at`, with its own time (`stamped`, a
// timer's due time) or aged by the loop. The wait from B's readiness to the
// start of its task.
static uint64_t two_sources(uint64_t ready_at, uint64_t dur, int stamped, int backlog)
{
    task_age_t aa = {0}, ab = {0};
    uint64_t now = 1000;
    for (int turn = 0; turn < 100000; turn++) {
        task_source_t src[2];
        src[0] = backlog ? task_age_stamped(&aa, true, 1) : task_age_observe(&aa, true, now);
        if (stamped)
            src[1] = task_age_stamped(&ab, now >= ready_at, ready_at);
        else
            src[1] = task_age_observe(&ab, now >= ready_at, now);
        int order[2];
        int c = task_order(src, 2, now, order);
        if (c > 0 && order[0] == 1)
            return now - ready_at;
        task_age_ran(&aa, true, now + dur);
        now += dur;
    }
    return UINT64_MAX;
}

static void guarantee(void)
{
    char what[160];
    for (uint64_t dur = 1; dur <= 400; dur += 7)
        for (uint64_t off = 0; off < dur; off += (dur / 5) + 1) {
            uint64_t ready_at = 1000 + 3 * dur + off;
            for (int backlog = 0; backlog < 2; backlog++) {
                const char *a = backlog ? "a stamped backlog" : "a never-empty source";
                uint64_t w = two_sources(ready_at, dur, 1, backlog);
                snprintf(what, sizeof(what), "%s holds a timed task %lu ms (bound %u, tasks %lu ms)", a,
                         (unsigned long)w, TASK_ORDER_BOUND_MS, (unsigned long)dur);
                check(w <= TASK_ORDER_BOUND_MS + dur, what);
                w = two_sources(ready_at, dur, 0, backlog);
                snprintf(what, sizeof(what), "%s holds an aged task %lu ms (bound %u, tasks %lu ms)", a,
                         (unsigned long)w, TASK_ORDER_BOUND_MS, (unsigned long)dur);
                check(w <= TASK_ORDER_BOUND_MS + 2 * dur, what);
            }
        }
}

// Many sources, each aged by the loop or stamped, tasks of random length:
// once a head is found ready it waits no more than the bound and one task
// per source (each older head can go once ahead of it, and one is
// running), plus a turn to be found when it is aged.
static void walk(unsigned seed)
{
    srand(seed);
    enum { N = 6 };
    task_age_t ages[N] = {{0}};
    uint64_t found[N] = {0}, stamp[N] = {0};
    int ready[N] = {0}, stamped[N];
    uint64_t now = 1000, longest = 0, worst = 0;
    for (int i = 0; i < N; i++)
        stamped[i] = rand() % 3 == 0;
    for (int turn = 0; turn < 200000; turn++) {
        // Sources fill at random, a few never empty.
        for (int i = 0; i < N; i++)
            if (!ready[i] && (i < 2 || rand() % 4 == 0)) {
                ready[i] = 1;
                stamp[i] = now;
                found[i] = now;
            }
        task_source_t src[N];
        for (int i = 0; i < N; i++)
            src[i] = stamped[i] ? task_age_stamped(&ages[i], ready[i] != 0, stamp[i])
                                : task_age_observe(&ages[i], ready[i] != 0, now);
        int order[N];
        int c = task_order(src, N, now, order);
        uint64_t dur = (uint64_t)(rand() % 120) + 1;
        if (dur > longest)
            longest = dur;
        if (c > 0) {
            int ran = order[0];
            // An aged source's head counts from when it was found, a
            // stamped one's from its stamp; both are `found` here.
            uint64_t waited = now - found[ran];
            if (waited > worst)
                worst = waited;
            // Each source but the never-empty ones empties when it runs. A
            // never-empty stamped source is a backlog: its next task keeps
            // the stamp it was queued with, long ago; its head is the head
            // from this run on, which is when its wait begins.
            ready[ran] = ran < 2;
            if (ready[ran])
                found[ran] = now + dur;
            task_age_ran(&ages[ran], ready[ran] != 0, now + dur);
        }
        now += dur;
    }
    char what[160];
    snprintf(what, sizeof(what), "walk %u: the longest wait %lu ms, within the bound and a task per source", seed,
             (unsigned long)worst);
    check(worst <= TASK_ORDER_BOUND_MS + (N + 1) * longest, what);
}

int main(void)
{
    cases();
    guarantee();
    for (unsigned seed = 1; seed <= 8; seed++)
        walk(seed);
    printf("task order: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
