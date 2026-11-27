// task_order.c — which of a page's ready tasks runs next (task_order.h).

#include "task_order.h"

static bool old(const task_source_t *s, uint64_t now_ms)
{
    return s->ready && now_ms >= s->since_ms && now_ms - s->since_ms >= TASK_ORDER_BOUND_MS;
}

int task_order(const task_source_t *sources, int n, uint64_t now_ms, int *order)
{
    int count = 0;
    // The old first, oldest first; equal ages keep the order of priority,
    // since a later source is placed only after an earlier one as old.
    for (int i = 0; i < n; i++) {
        if (!old(&sources[i], now_ms))
            continue;
        int at = count++;
        while (at > 0 && sources[order[at - 1]].since_ms > sources[i].since_ms) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    // Then the young, in the order of priority.
    for (int i = 0; i < n; i++)
        if (sources[i].ready && !old(&sources[i], now_ms))
            order[count++] = i;
    return count;
}

task_source_t task_age_observe(task_age_t *age, bool ready, uint64_t now_ms)
{
    if (!ready)
        age->since_ms = 0;
    else if (age->since_ms == 0)
        age->since_ms = now_ms != 0 ? now_ms : 1;
    return (task_source_t){ready, age->since_ms};
}

task_source_t task_age_stamped(const task_age_t *age, bool ready, uint64_t stamp_ms)
{
    return (task_source_t){ready, stamp_ms > age->ran_ms ? stamp_ms : age->ran_ms};
}

void task_age_ran(task_age_t *age, bool ready, uint64_t now_ms)
{
    uint64_t at = now_ms != 0 ? now_ms : 1;
    age->since_ms = ready ? at : 0;
    age->ran_ms = at;
}
