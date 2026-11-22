// Host test of yonder's load timeline (userland/apps/yonder/load_timeline.c):
// steps shared between slices by time, layouts counted where they began and
// tasks (with what they forced) where they ended, both inside the step under
// way, the long list kept longest first with waits left out and what fell
// off it counted, milestones kept first-told, nothing taken
// past the last slice or before a begin, and the lines as the record says
// them. Run by tools/test_load_timeline_host.sh.

#include "load_timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_failed++;                                                          \
        }                                                                        \
    } while (0)

static void check_line(const char *got, const char *want, int line)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "line %d:\n  got:  %s\n  want: %s\n", line, got, want);
        s_failed++;
    }
}
#define LINE(got, want) check_line(got, want, __LINE__)

#define S 1000000ll     // a second, in microseconds
#define BASE 777000000ll

static yonder_lt_t t;

static void test_off_until_begun(void)
{
    memset(&t, 0, sizeof(t));
    CHECK(!yonder_lt_running(&t, BASE));
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE);
    yonder_lt_step_end(&t, BASE + 9 * S);
    yonder_lt_layout(&t, BASE, 1000);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE);
    CHECK(yonder_lt_slices_used(&t) == 0);
    CHECK(t.nlongs == 0);
    CHECK(t.marks[YONDER_LT_MARK_PAINT] == 0);  // untouched zero: never begun
}

static void test_spread_across_slices(void)
{
    yonder_lt_begin(&t, BASE);
    CHECK(yonder_lt_running(&t, BASE));
    // A 7 s script step from 4 s: 1 s in slice 0, 5 s in slice 1, 1 s in slice 2.
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 4 * S);
    yonder_lt_step_end(&t, BASE + 11 * S);
    CHECK(t.slices[0].step_us[YONDER_LT_SCRIPTS] == 1 * S);
    CHECK(t.slices[1].step_us[YONDER_LT_SCRIPTS] == 5 * S);
    CHECK(t.slices[2].step_us[YONDER_LT_SCRIPTS] == 1 * S);
    CHECK(yonder_lt_slices_used(&t) == 3);
    CHECK(t.nlongs == 1 && t.longs[0].at_us == 4 * S && t.longs[0].us == 7 * S);
}

static void test_begin_ends_the_step_under_way(void)
{
    yonder_lt_begin(&t, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_WAIT, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_PAINT, BASE + 2 * S);    // ends the wait at 2 s
    yonder_lt_step_end(&t, BASE + 3 * S);
    yonder_lt_step_end(&t, BASE + 4 * S);                       // nothing under way: ignored
    CHECK(t.slices[0].step_us[YONDER_LT_WAIT] == 2 * S);
    CHECK(t.slices[0].step_us[YONDER_LT_PAINT] == 1 * S);
    // A time before the navigation counts as its start; an end before its
    // begin as no time.
    yonder_lt_step_begin(&t, YONDER_LT_INPUT, BASE - 5 * S);
    yonder_lt_step_end(&t, BASE - 6 * S);
    CHECK(t.slices[0].step_us[YONDER_LT_INPUT] == 0);
}

static void test_layouts_and_tasks_inside_a_step(void)
{
    yonder_lt_begin(&t, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 1 * S);
    yonder_lt_layout(&t, BASE + 1 * S + 100, 300000);
    yonder_lt_layout(&t, BASE + 6 * S, 200000);          // slice 1
    yonder_lt_task(&t, BASE + 2 * S, "timer a", 400000, 1, 300000);
    yonder_lt_task(&t, BASE + 7 * S, "animation-frame b", 900000, 3, 700000);
    yonder_lt_task(&t, BASE + 7 * S, "event c", 100000, 0, 0);  // shorter: not the step's longest
    yonder_lt_step_end(&t, BASE + 8 * S);
    // Layouts count where they began; what tasks forced, where each ended.
    CHECK(t.slices[0].layouts == 1 && t.slices[0].layout_us == 300000);
    CHECK(t.slices[1].layouts == 1 && t.slices[1].layout_us == 200000);
    CHECK(t.slices[0].forced == 1 && t.slices[0].forced_us == 300000);
    CHECK(t.slices[1].forced == 3 && t.slices[1].forced_us == 700000);
    CHECK(t.slices[0].tasks == 1 && t.slices[1].tasks == 2);
    CHECK(t.nlongs == 1);
    const yonder_lt_long_t *l = &t.longs[0];
    CHECK(l->layouts == 2 && l->layout_us == 500000 && l->forced == 4 && l->forced_us == 1000000);
    CHECK(strcmp(l->task, "animation-frame b") == 0 && l->task_us == 900000 && l->task_layouts == 3);
    // Outside a step a layout counts in its slice alone.
    yonder_lt_layout(&t, BASE + 9 * S, 50000);
    CHECK(t.slices[1].layouts == 2);
}

static void test_long_list(void)
{
    yonder_lt_begin(&t, BASE);
    int64_t at = BASE;
    // Short steps are not listed.
    yonder_lt_step_begin(&t, YONDER_LT_PAINT, at);
    yonder_lt_step_end(&t, at + (int64_t)YONDER_LT_LONG_US - 1);
    CHECK(t.nlongs == 0);
    // LONG_MAX + 4 long steps of rising length: the four shortest fall off.
    for (int i = 0; i < YONDER_LT_LONG_MAX + 4; i++) {
        at += S;
        yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, at);
        yonder_lt_step_end(&t, at + (int64_t)YONDER_LT_LONG_US + i * 1000);
    }
    CHECK(t.nlongs == YONDER_LT_LONG_MAX);
    CHECK(t.more_longs == 4);
    uint64_t fell = 0;
    for (int i = 0; i < 4; i++)
        fell += YONDER_LT_LONG_US + (uint64_t)i * 1000;
    CHECK(t.more_longs_us == fell);
    for (uint32_t i = 1; i < t.nlongs; i++)
        CHECK(t.longs[i - 1].us >= t.longs[i].us);
    CHECK(t.longs[0].us == YONDER_LT_LONG_US + (YONDER_LT_LONG_MAX + 3) * 1000);
    // One shorter than all listed goes straight to the count.
    at += S;
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, at);
    yonder_lt_step_end(&t, at + (int64_t)YONDER_LT_LONG_US);
    CHECK(t.more_longs == 5 && t.nlongs == YONDER_LT_LONG_MAX);
}

static void test_the_end(void)
{
    yonder_lt_begin(&t, BASE);
    int64_t end = BASE + (int64_t)(YONDER_LT_SLICES * YONDER_LT_SLICE_US);
    CHECK(yonder_lt_running(&t, end - 1));
    CHECK(!yonder_lt_running(&t, end));
    // A step running across the end keeps what fell inside.
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, end - S);
    yonder_lt_step_end(&t, end + 4 * S);
    CHECK(t.slices[YONDER_LT_SLICES - 1].step_us[YONDER_LT_SCRIPTS] == 1 * S);
    CHECK(t.nlongs == 1);
    // One beginning past it is not kept anywhere.
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, end + 5 * S);
    yonder_lt_step_end(&t, end + 9 * S);
    yonder_lt_layout(&t, end + 5 * S, 1000);
    yonder_lt_task(&t, end + 5 * S, "late", 1000, 0, 0);
    CHECK(t.nlongs == 1);
    uint32_t layouts = 0, tasks = 0;
    for (int i = 0; i < YONDER_LT_SLICES; i++) {
        layouts += t.slices[i].layouts;
        tasks += t.slices[i].tasks;
    }
    CHECK(layouts == 0 && tasks == 0);
    yonder_lt_stop(&t);
    CHECK(!yonder_lt_running(&t, BASE));
}

static void test_marks(void)
{
    yonder_lt_begin(&t, BASE);
    CHECK(t.marks[YONDER_LT_MARK_PAINT] == -1);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE + 2 * S);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE + 3 * S);     // the first stands
    yonder_lt_mark(&t, YONDER_LT_MARK_PICTURE, BASE - S);       // before the start: the start
    CHECK(t.marks[YONDER_LT_MARK_PAINT] == 2 * S);
    CHECK(t.marks[YONDER_LT_MARK_PICTURE] == 0);
    CHECK(t.marks[YONDER_LT_MARK_ARRIVED] == -1);
}

static void test_lines(void)
{
    char line[1024];
    yonder_lt_begin(&t, BASE);
    yonder_lt_mark(&t, YONDER_LT_MARK_ARRIVED, BASE + 2100000);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE + 2150000);
    // Slice 0: 1 s waiting, 3.5 s of stream, 0.495 s scripts, 5 ms input
    // (summed as the rest).
    yonder_lt_step_begin(&t, YONDER_LT_WAIT, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_STREAM, BASE + 1 * S);
    yonder_lt_layout(&t, BASE + 2 * S, 250000);
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 4500000);
    yonder_lt_task(&t, BASE + 4900000, "timer https://x/y.js", 300000, 2, 120000);
    yonder_lt_layout(&t, BASE + 4600000, 120000);
    yonder_lt_step_begin(&t, YONDER_LT_INPUT, BASE + 4995000);
    yonder_lt_step_end(&t, BASE + 5 * S);
    yonder_lt_slice_line(&t, 0, line, sizeof(line));
    LINE(line, "busy 80% of 5.0 s: the stream 3500.0 ms, waiting 1000.0 ms, scripts 495.0 ms, the rest 5.0 ms; "
               "layouts 2 in 370.0 ms; script tasks 1, forcing 2 layouts in 120.0 ms");
    yonder_lt_long_line(&t, 0, line, sizeof(line));
    LINE(line, "at 1.0 s, the stream 3500.0 ms; layouts in it 1 in 250.0 ms, its script tasks forcing 0 in 0.0 ms");
    // The second-long wait is no work, and is not listed.
    yonder_lt_long_line(&t, 1, line, sizeof(line));
    LINE(line, "at 4.5 s, scripts 495.0 ms; layouts in it 1 in 120.0 ms, its script tasks forcing 2 in 120.0 ms; "
               "its longest task timer https://x/y.js, 300.0 ms, forcing 2 layouts in 120.0 ms");
    yonder_lt_summary(&t, line, sizeof(line));
    LINE(line, "from the navigation's start, in slices of 5 s, the window thread's steps; arrived 2.1 s, "
               "first paint 2.2 s, the video's first picture -; 2 steps of 200 ms or more");
    // Past the list: the summary says how many and how long.
    t.more_longs = 3;
    t.more_longs_us = 900000;
    yonder_lt_summary(&t, line, sizeof(line));
    LINE(line, "from the navigation's start, in slices of 5 s, the window thread's steps; arrived 2.1 s, "
               "first paint 2.2 s, the video's first picture -; 5 steps of 200 ms or more, the 2 longest "
               "listed, the other 3 0.9 s in all");
    // An empty slice, and one past the list, write empty or nothing.
    yonder_lt_slice_line(&t, 7, line, sizeof(line));
    LINE(line, "busy 0% of 0.0 s; layouts 0 in 0.0 ms; script tasks 0, forcing 0 layouts in 0.0 ms");
    yonder_lt_long_line(&t, 2, line, sizeof(line));
    LINE(line, "");
    // A small buffer is cut, and terminated.
    char tiny[8];
    yonder_lt_slice_line(&t, 0, tiny, sizeof(tiny));
    LINE(tiny, "busy 80");
    // A long task name is cut to its room.
    char name[300];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 6 * S);
    yonder_lt_task(&t, BASE + 6 * S, name, 300000, 0, 0);
    yonder_lt_step_end(&t, BASE + 7 * S);
    CHECK(t.longs[1].at_us == 6 * S && strlen(t.longs[1].task) == YONDER_LT_TASK_MAX - 1);
}

int main(void)
{
    test_off_until_begun();
    test_spread_across_slices();
    test_begin_ends_the_step_under_way();
    test_layouts_and_tasks_inside_a_step();
    test_long_list();
    test_the_end();
    test_marks();
    test_lines();
    if (s_failed != 0) {
        fprintf(stderr, "load timeline: %d checks failed\n", s_failed);
        return 1;
    }
    printf("load timeline: all checks passed\n");
    return 0;
}
