// Host test of yonder's load timeline (userland/apps/yonder/load_timeline.c):
// steps shared between slices by time with what was inside them in the same
// shares, layouts counted where they began by who asked and in the grid by
// the step they ran in, their cascades when timed, tasks (with what they
// forced) where they ended, both inside the step under way, the long list
// kept longest first with waits left out and what fell off it counted,
// bytes parsed, milestones kept first-told, nothing taken past the last
// slice or before a begin, and the lines as the record says them. Run by
// tools/test_load_timeline_host.sh.

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
#define READ YONDER_LT_WHY_READ
#define REBUILD YONDER_LT_WHY_REBUILD

static yonder_lt_t t;

static void test_off_until_begun(void)
{
    memset(&t, 0, sizeof(t));
    CHECK(!yonder_lt_running(&t, BASE));
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE);
    yonder_lt_step_end(&t, BASE + 9 * S);
    yonder_lt_layout(&t, BASE, 1000, READ, NULL);
    yonder_lt_bytes(&t, BASE, 100);
    yonder_lt_inside(&t, YONDER_LT_IN_DRAWING, 1000);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE);
    CHECK(yonder_lt_slices_used(&t) == 0);
    CHECK(t.nlongs == 0);
    CHECK(t.marks[YONDER_LT_MARK_PAINT] == 0);  // untouched zero: never begun
}

static void test_spread_across_slices(void)
{
    yonder_lt_begin(&t, BASE);
    CHECK(yonder_lt_running(&t, BASE));
    // A 7 s script step from 4 s: 1 s in slice 0, 5 s in slice 1, 1 s in
    // slice 2, and its 3.5 s of tasks and 0.7 s of drawing in the same shares.
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 4 * S);
    yonder_lt_task(&t, BASE + 10 * S, "frame", 3500000, 0, 0);
    yonder_lt_inside(&t, YONDER_LT_IN_DRAWING, 700000);
    yonder_lt_step_end(&t, BASE + 11 * S);
    CHECK(t.slices[0].step_us[YONDER_LT_SCRIPTS] == 1 * S);
    CHECK(t.slices[1].step_us[YONDER_LT_SCRIPTS] == 5 * S);
    CHECK(t.slices[2].step_us[YONDER_LT_SCRIPTS] == 1 * S);
    CHECK(t.slices[0].in_us[YONDER_LT_SCRIPTS][YONDER_LT_IN_TASKS] == 500000);
    CHECK(t.slices[1].in_us[YONDER_LT_SCRIPTS][YONDER_LT_IN_TASKS] == 2500000);
    CHECK(t.slices[2].in_us[YONDER_LT_SCRIPTS][YONDER_LT_IN_TASKS] == 500000);
    CHECK(t.slices[1].in_us[YONDER_LT_SCRIPTS][YONDER_LT_IN_DRAWING] == 500000);
    CHECK(yonder_lt_slices_used(&t) == 3);
    CHECK(t.nlongs == 1 && t.longs[0].at_us == 4 * S && t.longs[0].us == 7 * S);
    // A step inside one slice keeps what was inside it whole.
    yonder_lt_step_begin(&t, YONDER_LT_PAINT, BASE + 12 * S);
    yonder_lt_inside(&t, YONDER_LT_IN_DRAWING, 333333);
    yonder_lt_step_end(&t, BASE + 13 * S);
    CHECK(t.slices[2].in_us[YONDER_LT_PAINT][YONDER_LT_IN_DRAWING] == 333333);
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
    yonder_lt_layout(&t, BASE + 1 * S + 100, 300000, READ, NULL);
    yonder_lt_layout(&t, BASE + 6 * S, 200000, REBUILD, NULL);  // slice 1
    yonder_lt_task(&t, BASE + 2 * S, "timer a", 400000, 1, 300000);
    yonder_lt_task(&t, BASE + 7 * S, "animation-frame b", 900000, 3, 700000);
    yonder_lt_task(&t, BASE + 7 * S, "event c", 100000, 0, 0);  // shorter: not the step's longest
    yonder_lt_step_end(&t, BASE + 8 * S);
    // Layouts count where they began, by who asked; what tasks forced,
    // where each ended.
    CHECK(t.slices[0].layouts == 1 && t.slices[0].layout_us == 300000);
    CHECK(t.slices[0].why_n[READ] == 1 && t.slices[0].why_us[READ] == 300000);
    CHECK(t.slices[1].layouts == 1 && t.slices[1].why_n[REBUILD] == 1 && t.slices[1].why_us[REBUILD] == 200000);
    CHECK(t.slices[0].forced == 1 && t.slices[0].forced_us == 300000);
    CHECK(t.slices[1].forced == 3 && t.slices[1].forced_us == 700000);
    CHECK(t.slices[0].tasks == 1 && t.slices[1].tasks == 2);
    // The grid counts each in the step it ran in.
    CHECK(t.grid_n[READ][YONDER_LT_SCRIPTS] == 1 && t.grid_us[READ][YONDER_LT_SCRIPTS] == 300000);
    CHECK(t.grid_n[REBUILD][YONDER_LT_SCRIPTS] == 1);
    CHECK(t.nlongs == 1);
    const yonder_lt_long_t *l = &t.longs[0];
    CHECK(l->layouts == 2 && l->in_us[YONDER_LT_IN_LAYOUTS] == 500000);
    CHECK(l->why_n[READ] == 1 && l->why_n[REBUILD] == 1);
    CHECK(l->in_us[YONDER_LT_IN_TASKS] == 1400000);
    CHECK(l->forced == 4 && l->forced_us == 1000000);
    CHECK(strcmp(l->task, "animation-frame b") == 0 && l->task_us == 900000 && l->task_layouts == 3);
    // Outside a step a layout counts in its slice, and in the grid's column
    // past the steps; a reason out of range is no reason.
    yonder_lt_layout(&t, BASE + 9 * S, 50000, (yonder_lt_why_t)99, NULL);
    CHECK(t.slices[1].layouts == 2 && t.slices[1].why_n[YONDER_LT_WHY_OTHER] == 1);
    CHECK(t.grid_n[YONDER_LT_WHY_OTHER][YONDER_LT_OUTSIDE] == 1);
}

static void test_cascades(void)
{
    yonder_lt_begin(&t, BASE);
    const yonder_lt_cascade_t fresh = {600000, true}, updated = {40000, false};
    yonder_lt_layout(&t, BASE, 700000, READ, &fresh);
    yonder_lt_layout(&t, BASE, 50000, READ, &updated);
    yonder_lt_layout(&t, BASE, 60000, REBUILD, NULL);           // not timed
    CHECK(t.cascades[READ] == 2 && t.cascade_us[READ] == 640000);
    CHECK(t.fresh[READ] == 1 && t.fresh_us[READ] == 600000);
    CHECK(t.cascades[REBUILD] == 0);
}

static void test_builds_and_broad(void)
{
    char line[2048];
    yonder_lt_begin(&t, BASE);
    const yonder_lt_build_t a = {100000, 5000, 200000, 20000, 400, false, "sheets"},
                            b = {120000, 7000, 300000, 20000, 600, false, "environment"},
                            k = {0, 5000, 200000, 20000, 400, true, NULL};
    yonder_lt_build(&t, READ, &a);
    yonder_lt_build(&t, READ, &b);
    yonder_lt_build(&t, (yonder_lt_why_t)99, &a);              // out of range: no reason told
    yonder_lt_build(&t, READ, &k);
    CHECK(t.kept[READ] == 1 && t.builds[READ] == 3 && t.nagain == 2 && t.again[0].n == 2 && t.again[1].n == 1);
    CHECK(t.build_sheets[READ] == 220000 && t.build_matching[READ] == 700000);
    CHECK(t.builds[YONDER_LT_WHY_OTHER] == 1);
    yonder_lt_layouts_line(&t, line, sizeof(line));
    LINE(line, "by who asked, in the step they ran in, the costliest first: none; fresh cascades by part: a "
               "script's read 3 in 937.0 ms (1 kept; reading the sheets 220.0, the table 17.0, matching 700.0; 20000 "
               "rules and 466 elements a build), no reason told 1 in 305.0 ms (0 kept; reading the sheets 100.0, the "
               "table 5.0, matching 200.0; 20000 rules and 400 elements a build); read again because: sheets 2, "
               "environment 1; cascades not timed whole (yonder --layout-profile times them)");
    // Broad refusals: counted always, the most marked kept, most first.
    for (uint32_t i = 0; i < YONDER_LT_BROAD_MAX + 3; i++) {
        char text[32];
        snprintf(text, sizeof(text), "trigger %u", i);
        yonder_lt_broad(&t, BASE + (int64_t)i * S, 100 + i * 10, text);
    }
    CHECK(t.nbroad == YONDER_LT_BROAD_MAX + 3 && t.broad_kept == YONDER_LT_BROAD_MAX);
    for (uint32_t i = 1; i < t.broad_kept; i++)
        CHECK(t.broad[i - 1].marked >= t.broad[i].marked);
    yonder_lt_broad_line(&t, 0, line, sizeof(line));
    LINE(line, "at 10.0 s, trigger 10");
    yonder_lt_broad_line(&t, YONDER_LT_BROAD_MAX, line, sizeof(line));
    LINE(line, "");
    yonder_lt_summary(&t, line, sizeof(line));
    CHECK(strstr(line, "; updates refused as broad: 11; ") != NULL);
    // Fewer marked than every one kept: counted, not kept.
    yonder_lt_broad(&t, BASE, 1, "small");
    CHECK(t.nbroad == YONDER_LT_BROAD_MAX + 4 && strcmp(t.broad[YONDER_LT_BROAD_MAX - 1].text, "small") != 0);
}

static void test_parse(void)
{
    char line[512];
    yonder_lt_begin(&t, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_STREAM, BASE);
    for (uint32_t i = 0; i < YONDER_LT_PARSE_MAX + 2; i++) {
        char text[48];
        snprintf(text, sizeof(text), "a feed %u", i);
        yonder_lt_parse(&t, BASE + (int64_t)i * 1000, 1000 + i * 100, text);
    }
    yonder_lt_inside(&t, YONDER_LT_IN_SHEETS, 500);
    yonder_lt_inside(&t, (yonder_lt_in_t)99, 500);              // no such part: nothing
    yonder_lt_step_end(&t, BASE + S);
    CHECK(t.nparse == YONDER_LT_PARSE_MAX + 2 && t.parse_kept == YONDER_LT_PARSE_MAX);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < YONDER_LT_PARSE_MAX + 2; i++)
        sum += 1000 + i * 100;
    CHECK(t.parse_us == sum);
    CHECK(t.slices[0].in_us[YONDER_LT_STREAM][YONDER_LT_IN_PARSING] == sum);
    CHECK(t.slices[0].in_us[YONDER_LT_STREAM][YONDER_LT_IN_SHEETS] == 500);
    for (uint32_t i = 1; i < t.parse_kept; i++)
        CHECK(t.parse[i - 1].us >= t.parse[i].us);
    yonder_lt_parse_line(&t, 0, line, sizeof(line));
    LINE(line, "at 0.0 s, 1.9 ms: a feed 9");
    yonder_lt_parse_line(&t, YONDER_LT_PARSE_MAX, line, sizeof(line));
    LINE(line, "");
}

static void test_long_list(void)
{
    yonder_lt_begin(&t, BASE);
    int64_t at = BASE;
    // Short steps are not listed, and neither is a long wait.
    yonder_lt_step_begin(&t, YONDER_LT_PAINT, at);
    yonder_lt_step_end(&t, at + (int64_t)YONDER_LT_LONG_US - 1);
    yonder_lt_step_begin(&t, YONDER_LT_WAIT, at);
    yonder_lt_step_end(&t, at + 3 * S);
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
    yonder_lt_layout(&t, end + 5 * S, 1000, READ, NULL);
    yonder_lt_task(&t, end + 5 * S, "late", 1000, 0, 0);
    yonder_lt_parse(&t, end + 5 * S, 1000, "late feed");
    yonder_lt_broad(&t, end + 5 * S, 99, "late broad");
    CHECK(t.nparse == 0 && t.parse_us == 0 && t.nbroad == 0 && t.broad_kept == 0);
    yonder_lt_bytes(&t, end + 5 * S, 4096);
    CHECK(t.nlongs == 1);
    uint32_t layouts = 0, tasks = 0, grid = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < YONDER_LT_SLICES; i++) {
        layouts += t.slices[i].layouts;
        tasks += t.slices[i].tasks;
        bytes += t.slices[i].bytes;
    }
    for (int w = 0; w < YONDER_LT_WHYS; w++)
        for (int s = 0; s <= YONDER_LT_STEPS; s++)
            grid += t.grid_n[w][s];
    CHECK(layouts == 0 && tasks == 0 && bytes == 0 && grid == 0);
    yonder_lt_stop(&t);
    CHECK(!yonder_lt_running(&t, BASE));
}

static void test_snapshot(void)
{
    // A record written in the middle of a step counts the step up to then,
    // and the timeline goes on as it was.
    static yonder_lt_t snap;
    yonder_lt_begin(&t, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_STREAM, BASE + 1 * S);
    yonder_lt_parse(&t, BASE + 1 * S, 300000, "a feed");
    yonder_lt_snapshot(&t, BASE + 4 * S, &snap);
    CHECK(snap.slices[0].step_us[YONDER_LT_STREAM] == 3 * S);
    CHECK(snap.slices[0].in_us[YONDER_LT_STREAM][YONDER_LT_IN_PARSING] == 300000);
    CHECK(snap.nlongs == 1 && snap.longs[0].us == 3 * S);
    CHECK(t.in_step && t.slices[0].step_us[YONDER_LT_STREAM] == 0 && t.nlongs == 0);
    yonder_lt_step_end(&t, BASE + 5 * S);
    CHECK(t.slices[0].step_us[YONDER_LT_STREAM] == 4 * S && t.nlongs == 1 && t.longs[0].us == 4 * S);
    // With no step under way the snapshot is the timeline.
    yonder_lt_snapshot(&t, BASE + 9 * S, &snap);
    CHECK(memcmp(&snap, &t, sizeof(t)) == 0);
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
    char line[2048];
    yonder_lt_begin(&t, BASE);
    yonder_lt_mark(&t, YONDER_LT_MARK_ARRIVED, BASE + 2100000);
    yonder_lt_mark(&t, YONDER_LT_MARK_PAINT, BASE + 2150000);
    // Slice 0: 1 s waiting, 3.5 s of stream (a 250 ms layout a sheet asked
    // for, 64 KiB parsed), 0.495 s scripts (a 300 ms task forcing a 120 ms
    // read), 5 ms input (summed as the rest).
    yonder_lt_step_begin(&t, YONDER_LT_WAIT, BASE);
    yonder_lt_step_begin(&t, YONDER_LT_STREAM, BASE + 1 * S);
    yonder_lt_layout(&t, BASE + 2 * S, 250000, YONDER_LT_WHY_SHEET, NULL);
    yonder_lt_bytes(&t, BASE + 2 * S, 65536);
    yonder_lt_step_begin(&t, YONDER_LT_SCRIPTS, BASE + 4500000);
    yonder_lt_layout(&t, BASE + 4600000, 120000, READ, NULL);
    yonder_lt_task(&t, BASE + 4900000, "timer https://x/y.js", 300000, 2, 120000);
    yonder_lt_step_begin(&t, YONDER_LT_INPUT, BASE + 4995000);
    yonder_lt_step_end(&t, BASE + 5 * S);
    yonder_lt_slice_line(&t, 0, line, sizeof(line));
    LINE(line, "busy 80% of 5.0 s: the stream 3500.0 ms (layouts 250.0), waiting 1000.0 ms, scripts 495.0 ms "
               "(tasks 300.0, layouts 120.0), the rest 5.0 ms; layouts 2 in 370.0 ms (a sheet arriving 1 in 250.0, "
               "a script's read 1 in 120.0); script tasks 1, forcing 2 layouts in 120.0 ms; parsed 64 KiB");
    yonder_lt_long_line(&t, 0, line, sizeof(line));
    LINE(line, "at 1.0 s, the stream 3500.0 ms; layouts in it 1 in 250.0 ms (a sheet arriving 1 in 250.0), "
               "its script tasks forcing 0 in 0.0 ms");
    yonder_lt_long_line(&t, 1, line, sizeof(line));
    LINE(line, "at 4.5 s, scripts 495.0 ms; layouts in it 1 in 120.0 ms (a script's read 1 in 120.0), its script "
               "tasks forcing 2 in 120.0 ms; its longest task timer https://x/y.js, 300.0 ms, forcing 2 layouts "
               "in 120.0 ms");
    yonder_lt_summary(&t, line, sizeof(line));
    LINE(line, "from the navigation's start, in slices of 5 s, the window thread's steps; arrived 2.1 s, "
               "first paint 2.2 s, the video's first picture -; 0 parser calls in 0.0 ms; updates refused as broad: 0; 2 steps of 200 ms or more");
    yonder_lt_layouts_line(&t, line, sizeof(line));
    LINE(line, "by who asked, in the step they ran in, the costliest first: a sheet arriving in the stream 1 in "
               "250.0 ms, a script's read in scripts 1 in 120.0 ms; cascades not timed whole (yonder --layout-profile "
               "times them)");
    // With cascades timed, they follow, the costliest first.
    const yonder_lt_cascade_t c = {200000, true};
    yonder_lt_layout(&t, BASE + 4700000, 210000, READ, &c);
    yonder_lt_layouts_line(&t, line, sizeof(line));
    LINE(line, "by who asked, in the step they ran in, the costliest first: a sheet arriving in the stream 1 in "
               "250.0 ms, a script's read in no step 1 in 210.0 ms, a script's read in scripts 1 in 120.0 ms; cascades "
               "the layout profile timed: a script's read 1 in 200.0 ms (1 built fresh in 200.0 ms)");
    // Past the list: the summary says how many and how long.
    t.more_longs = 3;
    t.more_longs_us = 900000;
    yonder_lt_summary(&t, line, sizeof(line));
    LINE(line, "from the navigation's start, in slices of 5 s, the window thread's steps; arrived 2.1 s, "
               "first paint 2.2 s, the video's first picture -; 0 parser calls in 0.0 ms; updates refused as broad: 0; 5 steps of 200 ms or more, the 2 longest "
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
    // A long step's drawing is said; a long task name is cut to its room.
    char name[300];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    yonder_lt_step_begin(&t, YONDER_LT_PAINT, BASE + 6 * S);
    yonder_lt_inside(&t, YONDER_LT_IN_DRAWING, 250000);
    yonder_lt_task(&t, BASE + 6 * S, name, 300000, 0, 0);
    yonder_lt_step_end(&t, BASE + 7 * S);
    // A second long, it lists after the stream's 3.5 s.
    CHECK(t.longs[1].at_us == 6 * S && strlen(t.longs[1].task) == YONDER_LT_TASK_MAX - 1);
    yonder_lt_long_line(&t, 1, line, sizeof(line));
    CHECK(strstr(line, "; drawing 250.0 ms; its longest task nnn") != NULL);
}

int main(void)
{
    test_off_until_begun();
    test_spread_across_slices();
    test_begin_ends_the_step_under_way();
    test_layouts_and_tasks_inside_a_step();
    test_cascades();
    test_builds_and_broad();
    test_parse();
    test_long_list();
    test_the_end();
    test_snapshot();
    test_marks();
    test_lines();
    if (s_failed != 0) {
        fprintf(stderr, "load timeline: %d checks failed\n", s_failed);
        return 1;
    }
    printf("load timeline: all checks passed\n");
    return 0;
}
