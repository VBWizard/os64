// task_order.h — which of a page's ready tasks runs next (the loop's
// fairness).
//
// A page's ordinary tasks come from several sources (scripts ready to
// run, image events, media events, sheet events, the intersection
// observers' delivery, messages and timers, the view's events, engine
// cleanup), and the loop runs one a turn. PRIORITY IS A PREFERENCE AMONG
// THE YOUNG, AGE IS THE LAW AMONG THE OLD: a task ready less than
// TASK_ORDER_BOUND_MS waits behind the sources above it, as the order of
// the sources says; a task ready that long or longer goes ahead of every
// young one, and the old go oldest first (priority breaking a tie). So no
// source, however full, keeps another's ready task waiting past the bound
// and the task that is running when the bound passes. Input, the history
// events and the view's due events are chosen before any of these, and an
// idle callback after them, by the loop itself.
//
// When a task became ready is its source's to say where the source keeps
// it (a timer's due time, a message's posting). A source that only says
// WHETHER it has one is aged by the loop (task_age_*): ready since the
// first turn that found its head ready, so such a head can be aged up to
// one turn late, and the head that follows a task's run counts as ready
// from then.
//
// Pure: no os64 headers, so tools/test_task_order_host.sh runs it.

#ifndef YONDER_TASK_ORDER_H
#define YONDER_TASK_ORDER_H

#include <stdbool.h>
#include <stdint.h>

#define TASK_ORDER_BOUND_MS 100u

// One source, as the loop asks it this turn: whether a task is ready, and
// since when (yonder_now_ms' clock).
typedef struct {
    bool ready;
    uint64_t since_ms;
} task_source_t;

// The ready sources of `sources` (`n` of them, in the order of priority)
// in the order to try them at `now_ms`, into `order`, which has room for
// `n`; how many. The loop tries them in turn until one runs a task: a
// source may say it is ready and have nothing that can run now.
int task_order(const task_source_t *sources, int n, uint64_t now_ms, int *order);

// A source aged by the loop: since when its head has been ready, 0 for
// none ready.
typedef struct {
    uint64_t since_ms;
} task_age_t;

// This turn's answer from a source that says only whether it has a task:
// the source for task_order.
task_source_t task_age_observe(task_age_t *age, bool ready, uint64_t now_ms);
// The source ran a task: its next head, if it has one, is ready from now.
void task_age_ran(task_age_t *age, bool ready, uint64_t now_ms);

#endif
