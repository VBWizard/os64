#ifndef YONDER_LOAD_TIMELINE_H
#define YONDER_LOAD_TIMELINE_H

// THE LOAD'S TIMELINE (YONDER_DIAGNOSTICS.md § The load's timeline): where
// the window's thread spent the first minutes of a navigation, IN ORDER.
// The record's other lines say how much each kind of work cost over a
// span; this says when it happened, so "why did the page take a minute"
// reads as a story: the stream's parse in the first seconds, the scripts
// after, the layouts they forced, the first paint and the video's first
// picture, each at its time.
//
// It keeps, from the navigation's start for YONDER_LT_SLICES slices of
// YONDER_LT_SLICE_US each:
//   - per slice, the time each STEP of the window loop took (a step that
//     runs across a slice's edge is shared between the slices by time),
//     the layouts begun in it, and the layouts its script tasks forced (as
//     each task says when it ends: the DOM's own count, which needs no
//     ledger);
//   - the YONDER_LT_LONG_MAX longest steps of YONDER_LT_LONG_US or more,
//     waits left out (the slices say how long the loop slept), each with
//     when it began, the layouts made inside it and the longest script task
//     inside it with what that task forced;
//   - the milestones, each the first time it was told.
// Past the last slice it takes nothing more (yonder_lt_running is false),
// so a page left open for an hour costs the loop nothing after.
//
// Pure: no clock (every time is the caller's, in microseconds on one
// clock), no allocation, no window. tools/test_load_timeline_host.sh.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YONDER_LT_SLICE_US 5000000ull
#define YONDER_LT_SLICES 36
#define YONDER_LT_LONG_US 200000ull
#define YONDER_LT_LONG_MAX 16
#define YONDER_LT_TASK_MAX 96

// The window loop's steps, in the order a turn takes them. Each names the
// loop's calls it brackets (yonder.c's window loop); a script task runs
// inside whichever step called it.
typedef enum {
    YONDER_LT_WAIT,         // asleep, waiting for the next event
    YONDER_LT_INPUT,        // the dispatcher's turn: input and window events, the handlers they run
    YONDER_LT_RELAYOUT,     // a layout the window owed (a resize, a zoom)
    YONDER_LT_STREAM,       // stream_turn: a slice of the arriving page's parse, or one of its scripts
    YONDER_LT_COMING,       // coming_turn: a page waiting for its sheets, its scripts and timers
    YONDER_LT_SCRIPTS,      // script_turn: the shown page's tasks, timers and frames
    YONDER_LT_MEDIA,        // response bodies, players, media, videos and keys
    YONDER_LT_MOTION,       // the animations' and the videos' ticks
    YONDER_LT_UPKEEP,       // storage, cookies, census, child frames, downloads, pictures, title
    YONDER_LT_OBSERVERS,    // resize and intersection observers, the work before a paint
    YONDER_LT_PAINT,        // the paint and the rest of the turn after it
    YONDER_LT_STEPS
} yonder_lt_step_t;

typedef enum {
    YONDER_LT_MARK_ARRIVED,         // the page arrived whole: its stream ended, it is the shown one
    YONDER_LT_MARK_PAINT,           // the first paint of the shown page
    YONDER_LT_MARK_PICTURE,         // a video's first picture handed to the paint
    YONDER_LT_MARKS
} yonder_lt_mark_t;

typedef struct {
    uint64_t at_us, us;             // from the navigation's start; how long
    uint8_t step;                   // yonder_lt_step_t
    uint32_t layouts;               // made inside it
    uint64_t layout_us;
    uint32_t forced;                // what the tasks inside it forced
    uint64_t forced_us;
    char task[YONDER_LT_TASK_MAX];  // the longest task inside it, "" for none
    uint64_t task_us;
    uint32_t task_layouts;          // what that task forced
    uint64_t task_layout_us;
} yonder_lt_long_t;

typedef struct {
    uint64_t step_us[YONDER_LT_STEPS];
    uint32_t layouts;               // begun in the slice
    uint64_t layout_us;
    uint32_t tasks;                 // script tasks that ended in it
    uint32_t forced;                // what those tasks forced
    uint64_t forced_us;
} yonder_lt_slice_t;

typedef struct {
    bool on;
    int64_t began_us;
    yonder_lt_slice_t slices[YONDER_LT_SLICES];
    int64_t marks[YONDER_LT_MARKS];             // -1 until told
    // The step under way (yonder_lt_step_begin), what was made inside it.
    bool in_step;
    yonder_lt_long_t now;
    // The longest steps, longest first, and those past the list: how many
    // and how long, since none of a long step's time may go unsaid.
    yonder_lt_long_t longs[YONDER_LT_LONG_MAX];
    uint32_t nlongs, more_longs;
    uint64_t more_longs_us;
} yonder_lt_t;

// A navigation began at `began_us`: everything before is let go.
void yonder_lt_begin(yonder_lt_t *t, int64_t began_us);
// No navigation is measured: nothing is taken until the next begin.
void yonder_lt_stop(yonder_lt_t *t);
// Whether a time `now_us` is still one the timeline takes.
bool yonder_lt_running(const yonder_lt_t *t, int64_t now_us);

// A step of the loop from `from_us`: what the calls below tell before its
// end is counted inside it. A begin while one is under way ends that one
// at `from_us` first.
void yonder_lt_step_begin(yonder_lt_t *t, yonder_lt_step_t step, int64_t from_us);
void yonder_lt_step_end(yonder_lt_t *t, int64_t to_us);
// A layout begun at `from_us` that took `us`, whoever asked for it.
// Counted in the slice it began in, and in the step under way.
void yonder_lt_layout(yonder_lt_t *t, int64_t from_us, uint64_t us);
// A script task ended at `at_us` after `us`, forcing `layouts` in
// `layout_us`, named `what` (cut to fit). Its forced layouts count in the
// slice it ended in and in the step under way, and the longest task of a
// step is the one its long-step line names.
void yonder_lt_task(yonder_lt_t *t, int64_t at_us, const char *what, uint64_t us, uint32_t layouts,
                    uint64_t layout_us);
// A milestone at `at_us`: the first one told stands.
void yonder_lt_mark(yonder_lt_t *t, yonder_lt_mark_t mark, int64_t at_us);

// The lines, each written into `out` (cut to `cap`, always terminated).
// The summary: the slices' size, the milestones, the long steps' count.
void yonder_lt_summary(const yonder_lt_t *t, char *out, size_t cap);
// The slices from the first through the last that holds anything.
uint32_t yonder_lt_slices_used(const yonder_lt_t *t);
// Slice `i`: its busy share (the time not waiting, of the time its steps
// took), its steps of 10 ms or more, longest first, with the rest summed,
// its layouts and its script tasks.
void yonder_lt_slice_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);
// Long step `i` (0 is the longest), i < t->nlongs.
void yonder_lt_long_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);

const char *yonder_lt_step_name(yonder_lt_step_t step);

#endif
