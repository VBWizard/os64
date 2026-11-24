// Host test of yonder's thread reading (userland/apps/yonder/thread_reading.c):
// a step's time is its calls' and a call's time runs to the next call; a
// call in another step is a turn of it; the call under way is ended at the
// line's moment without being ended in the reading; a reset starts the
// counts again and the call under way goes on from it; nothing is taken
// while off; what ran inside lands in the step under way; timers by site,
// costliest first, the rest summed, past the table and unnamed apart; the
// collector over the reading; and the lines as the record says them. Run by
// tools/test_thread_reading_host.sh.

#include "thread_reading.h"

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

#define MS 1000ll       // ticks in a millisecond, for these cases
static yonder_tr_t t;   // big: off the stack

// One turn of a quiet loop from `at`: waiting w ms, the dispatcher 1 ms, a
// task found and run `task` ms (a task of `task` - 1 inside it), the
// rendering step after it 2 ms, the paint 3 ms, the close 1 ms.
static int64_t turn(int64_t at, int64_t w, int64_t task)
{
    yonder_tr_call(&t, YONDER_TR_WAIT, at);
    at += w * MS;
    yonder_tr_call(&t, YONDER_TR_DISPATCH, at);
    at += 1 * MS;
    yonder_tr_call(&t, YONDER_TR_OWED, at);
    yonder_tr_call(&t, YONDER_TR_TASK, at);
    yonder_tr_inside(&t, YONDER_TR_IN_TASKS, (uint64_t)(task - 1) * MS);
    at += task * MS;
    yonder_tr_call(&t, YONDER_TR_AFTER, at);
    yonder_tr_inside(&t, YONDER_TR_IN_MODELS, 1 * MS);
    at += 2 * MS;
    yonder_tr_call(&t, YONDER_TR_PAINT, at);
    yonder_tr_inside(&t, YONDER_TR_IN_DRAWING, 2 * MS);
    at += 3 * MS;
    yonder_tr_call(&t, YONDER_TR_SCHEDULE, at);
    at += 1 * MS;
    return at;
}

static void steps_cases(void)
{
    char line[2048];
    memset(&t, 0, sizeof(t));
    // A reset with no call under way (the first): the reading's span begins
    // at its first call, so it is what the calls cover.
    yonder_tr_reset(&t, 0, NULL, NULL);
    yonder_tr_call(&t, YONDER_TR_DISPATCH, 7 * MS);
    yonder_tr_steps_line(&t, 9 * MS, MS, line, sizeof(line));
    LINE(line, "in 2.0 ms the loop's steps took 2.0 ms, busy 100%; input 2.0 ms in 1 turns (the dispatcher's turn "
               "2.0 ms 1 times)");
    yonder_tr_off(&t);
    memset(&t, 0, sizeof(t));
    // Off: nothing is taken, and the line says nothing happened.
    yonder_tr_call(&t, YONDER_TR_TASK, 0);
    yonder_tr_inside(&t, YONDER_TR_IN_TASKS, 5 * MS);
    yonder_tr_timer(&t, "x", 5, 0, 0);
    CHECK(t.call_n[YONDER_TR_TASK] == 0 && t.in_n[YONDER_LT_SCRIPTS][YONDER_TR_IN_TASKS] == 0 && t.nsites == 0);
    // Inside with no call under way lands nowhere.
    yonder_tr_reset(&t, 1000 * MS, NULL, NULL);
    yonder_tr_inside(&t, YONDER_TR_IN_TASKS, 5 * MS);
    CHECK(t.in_n[YONDER_LT_SCRIPTS][YONDER_TR_IN_TASKS] == 0);

    int64_t at = 1000 * MS;
    at = turn(at, 10, 4);
    at = turn(at, 10, 4);
    CHECK(t.turns[YONDER_LT_SCRIPTS] == 2 && t.turns[YONDER_LT_WAIT] == 2 && t.turns[YONDER_LT_CLOSE] == 2);
    CHECK(t.call_n[YONDER_TR_OWED] == 2 && t.call_ticks[YONDER_TR_OWED] == 0);
    CHECK(t.call_ticks[YONDER_TR_TASK] == 8 * MS && t.call_ticks[YONDER_TR_AFTER] == 4 * MS);
    // The close's call is still under way: 1 ms of it in the first turn,
    // and the second's runs to the line's moment.
    CHECK(t.call == YONDER_TR_SCHEDULE && t.call_ticks[YONDER_TR_SCHEDULE] == 1 * MS);
    yonder_tr_steps_line(&t, at + 4 * MS, MS, line, sizeof(line));
    LINE(line, "in 46.0 ms the loop's steps took 46.0 ms, busy 56%; waiting 20.0 ms in 2 turns (waiting 20.0 ms 2 "
               "times); scripts 12.0 ms in 2 turns (a task found and run 8.0 ms 2 times, the rendering step after a "
               "task 4.0 ms 2 times, the other calls 0.0 ms 2 times; inside: tasks 2 in 6.0 ms, model rebuilds 2 in "
               "2.0 ms); paint 6.0 ms in 2 turns (the paint 6.0 ms 2 times; inside: drawing 2 in 4.0 ms); the turn's "
               "close 6.0 ms in 2 turns (the next moment scheduled 6.0 ms 2 times); input 2.0 ms in 2 turns (the "
               "dispatcher's turn 2.0 ms 2 times)");
    // The line ended nothing: the reading still has the close under way.
    CHECK(t.call == YONDER_TR_SCHEDULE && t.call_ticks[YONDER_TR_SCHEDULE] == 1 * MS);

    // A reset: the counts start again from it, and the close goes on from
    // it as the reading's first call and its step's first turn; the next
    // call in the same step is no new turn.
    yonder_tr_reset(&t, at + 4 * MS, NULL, NULL);
    CHECK(t.call == YONDER_TR_SCHEDULE && t.call_n[YONDER_TR_SCHEDULE] == 1 && t.turns[YONDER_LT_CLOSE] == 1 &&
          t.call_ticks[YONDER_TR_SCHEDULE] == 0);
    yonder_tr_call(&t, YONDER_TR_FRAME_CLOSE, at + 5 * MS);
    CHECK(t.call_ticks[YONDER_TR_SCHEDULE] == 1 * MS && t.turns[YONDER_LT_CLOSE] == 1 &&
          t.call_n[YONDER_TR_FRAME_CLOSE] == 1);
    yonder_tr_call(&t, YONDER_TR_WAIT, at + 6 * MS);
    CHECK(t.turns[YONDER_LT_WAIT] == 1 && t.call_ticks[YONDER_TR_FRAME_CLOSE] == 1 * MS);
    yonder_tr_steps_line(&t, at + 6 * MS, MS, line, sizeof(line));
    LINE(line, "in 2.0 ms the loop's steps took 2.0 ms, busy 100%; the turn's close 2.0 ms in 1 turns (the frame "
               "closed 1.0 ms 1 times, the next moment scheduled 1.0 ms 1 times); waiting 0.0 ms in 1 turns");
    // Off: no call is under way, and none is taken.
    yonder_tr_off(&t);
    yonder_tr_call(&t, YONDER_TR_PAINT, at + 9 * MS);
    CHECK(t.call == YONDER_TR_CALLS && t.call_n[YONDER_TR_PAINT] == 0);
    // A call outside the table is refused.
    yonder_tr_reset(&t, 0, NULL, NULL);
    yonder_tr_call(&t, YONDER_TR_CALLS, 1);
    CHECK(t.call == YONDER_TR_CALLS);
    CHECK(yonder_tr_call_step(YONDER_TR_ASKS) == YONDER_LT_SCRIPTS && yonder_tr_call_step(YONDER_TR_CALLS) == YONDER_LT_STEPS);
    CHECK(strcmp(yonder_tr_call_name(YONDER_TR_CALLS), "?") == 0);
    // Every call has a name and a step.
    for (int c = 0; c < YONDER_TR_CALLS; c++)
        CHECK(yonder_tr_call_name((yonder_tr_call_t)c) != NULL &&
              yonder_tr_call_step((yonder_tr_call_t)c) < YONDER_LT_STEPS);
}

static void timer_cases(void)
{
    char line[4096];
    memset(&t, 0, sizeof(t));
    yonder_tr_reset(&t, 0, NULL, NULL);
    yonder_tr_timers_line(&t, MS, line, sizeof(line));
    LINE(line, "none");
    yonder_tr_timer(&t, "tick at a.js:1:1", 2 * MS, 0, 0);
    yonder_tr_timer(&t, "poll at b.js:9:3", 5 * MS, 1, 3 * MS);
    yonder_tr_timer(&t, "tick at a.js:1:1", 4 * MS, 0, 0);
    yonder_tr_timer(&t, NULL, 1 * MS, 0, 0);
    yonder_tr_timers_line(&t, MS, line, sizeof(line));
    LINE(line, "4 timer tasks in 12.0 ms, from 2 sites; tick at a.js:1:1: 2 tasks, 6.0 ms (each 3.0, the most 4.0); "
               "poll at b.js:9:3: 1 task, 5.0 ms (each 5.0, the most 5.0), forcing 1 layouts in 3.0 ms; with no site "
               "known: 1 task, 1.0 ms (each 1.0, the most 1.0)");
    // Past the shown: summed; past the table: apart.
    char site[32];
    for (int i = 0; i < YONDER_TR_SITES + 3; i++) {
        snprintf(site, sizeof(site), "f%d", i);
        yonder_tr_timer(&t, site, MS / 10, 0, 0);
    }
    CHECK(t.nsites == YONDER_TR_SITES && t.other.n == 5);
    yonder_tr_timers_line(&t, MS, line, sizeof(line));
    CHECK(strstr(line, "; the 40 other sites: 40 tasks, 4.0 ms (each 0.1, the most 0.1)") != NULL);
    CHECK(strstr(line, "; sites past the 48 kept: 5 tasks, 0.5 ms") != NULL);
    // A long site is cut to the table's text and still found again.
    char longsite[400];
    memset(longsite, 'x', sizeof(longsite) - 1);
    longsite[sizeof(longsite) - 1] = '\0';
    memset(&t, 0, sizeof(t));
    yonder_tr_reset(&t, 0, NULL, NULL);
    yonder_tr_timer(&t, longsite, 1, 0, 0);
    CHECK(strlen(t.sites[0].site) == YONDER_TR_SITE_TEXT - 1);
    // A cut line is terminated.
    yonder_tr_timers_line(&t, MS, line, 10);
    CHECK(strlen(line) == 9);
}

static void collector_cases(void)
{
    char line[1024];
    memset(&t, 0, sizeof(t));
    yonder_tr_collector_t from = {.runs = 10, .runs_by_allocation = 10, .walked = 1000, .freed = 50,
                                  .bytes_before = 10ull << 20, .bytes_after = 8ull << 20, .ticks = 20 * MS};
    yonder_tr_reset(&t, 0, &from, NULL);
    yonder_tr_collector_t now = from;
    now.threshold = 300ull << 20;
    now.ticks_most = 90 * MS;
    now.walked_most = 400000;
    yonder_tr_collector_line(&t, &now, 200ull << 20, 512ull << 20, MS, line, sizeof(line));
    LINE(line, "no runs; the next run waits for the engine's heap to reach 300.0 MiB; the script heap now 200.0 MiB of "
               "512.0 MiB; since the script began, the longest run 90.0 ms, the most walked in one 400000");
    now.runs += 2;
    now.runs_by_allocation += 2;
    now.walked += 600000;
    now.freed += 7;
    now.bytes_before += 400ull << 20;
    now.bytes_after += 390ull << 20;
    now.ticks += 160 * MS;
    yonder_tr_collector_line(&t, &now, 200ull << 20, 512ull << 20, MS, line, sizeof(line));
    LINE(line, "2 runs (2 asked by the allocation threshold) in 160.0 ms, each 80.0 ms on average; walked 600000 "
               "objects, 300000 a run, and freed 7 in cycles; the engine's heap 200.0 MiB before a run and 195.0 MiB "
               "after, on average; the next run waits for the engine's heap to reach 300.0 MiB; the script heap now "
               "200.0 MiB of 512.0 MiB; since the script began, the longest run 90.0 ms, the most walked in one 400000");
    // Counts behind the reading's start (the runtime let go, another made):
    // said from their own start, never wrapped.
    yonder_tr_collector_t fresh = {.runs = 1, .runs_by_allocation = 1, .walked = 30, .freed = 2,
                                   .bytes_before = 3ull << 20, .bytes_after = 2ull << 20, .ticks = 5 * MS};
    yonder_tr_collector_line(&t, &fresh, 0, 512ull << 20, MS, line, sizeof(line));
    CHECK(strstr(line, "counted from the script's start, the reading's being gone: 1 run (1 asked by the "
                       "allocation threshold) in 5.0 ms") == line);
    yonder_tr_collector_t gone = {0};
    yonder_tr_collector_line(&t, &gone, 0, 512ull << 20, MS, line, sizeof(line));
    CHECK(strstr(line, "counted from the script's start, the reading's being gone: no runs;") == line);
}

static void walks_cases(void)
{
    char line[512];
    memset(&t, 0, sizeof(t));
    yonder_tr_list_t from = {.live = 3, .arms = 100, .walked = 400};
    yonder_tr_intersections(&t, 9);     // off: not taken
    yonder_tr_checked(&t, 9);           // off: not taken
    yonder_tr_reset(&t, 0, NULL, &from);
    yonder_tr_list_t now = from;
    yonder_tr_walks_line(&t, &now, 12, line, sizeof(line));
    LINE(line, "no intersection steps; the every-turn checks visited 0 records in 0 turns, the most in one 0; the "
               "timer list: 3 live now, 0 arms walked 0 entries to find their places, 0 an arm; the binding holds 12 "
               "values for the engine");
    yonder_tr_intersections(&t, 40);
    yonder_tr_intersections(&t, 60);
    yonder_tr_checked(&t, 0);
    yonder_tr_checked(&t, 31);
    yonder_tr_checked(&t, 0);
    yonder_tr_checked(&t, 4);
    now.live = 7;
    now.arms += 10;
    now.walked += 250;
    yonder_tr_walks_line(&t, &now, 5000, line, sizeof(line));
    LINE(line, "2 intersection steps walked 100 observed targets, 50 a step, the most 60; the every-turn checks "
               "visited 35 records in 4 turns, the most in one 31; the timer list: 7 live now, 10 arms walked 250 "
               "entries to find their places, 25 an arm; the binding holds 5000 values for the engine");
    yonder_tr_reset(&t, 0, NULL, &from);
    yonder_tr_checked(&t, 1);
    yonder_tr_walks_line(&t, &now, 1, line, sizeof(line));
    CHECK(strstr(line, "; the every-turn checks visited 1 record in 1 turn, the most in one 1;") != NULL);
    // A list behind the reading's start is another binding's.
    yonder_tr_list_t other = {.live = 2, .arms = 4, .walked = 8};
    yonder_tr_walks_line(&t, &other, 10, line, sizeof(line));
    CHECK(strstr(line, "; the timer list, counted from its binding's start: 2 live now, 4 arms walked 8 entries") != NULL);
}

int main(void)
{
    walks_cases();
    steps_cases();
    timer_cases();
    collector_cases();
    if (s_failed != 0) {
        fprintf(stderr, "thread reading: %d failed\n", s_failed);
        return 1;
    }
    printf("thread reading: all cases passed\n");
    return 0;
}
