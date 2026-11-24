#ifndef YONDER_THREAD_READING_H
#define YONDER_THREAD_READING_H

// WHERE THE WINDOW'S THREAD WENT SINCE THE LAST READING
// (YONDER_DIAGNOSTICS.md § Where the thread goes, per reading). The load's
// timeline (load_timeline.h) says it for a navigation's first minutes in
// slices; this says it for every reading, however late, and finer:
//   - each step of the window loop (load_timeline.h's), its time and the
//     turns that entered it;
//   - each CALL the loop makes inside a step, its time and how many times
//     it was made, so a step's time is the sum of its calls' and none of it
//     goes unnamed;
//   - inside each step, what ran in it wherever the call: script tasks,
//     layouts, model rebuilds, what the tasks said afterwards (the status
//     line, the record, the audit) and drawing, each with a count;
//   - the timer tasks by the site that set them (the dom's
//     os64_dom_timer_sites), the costliest first;
//   - the script runtime's collector over the reading, from two readings
//     of its counts (os64_js_collector) the caller hands over.
// A reading is the span between two resets: the caller resets after it
// writes the lines, and the call under way goes on from the reset.
//
// Pure: no clock (every time is the caller's, in ticks of one counter, and
// the lines turn ticks into time with the rate the caller says), no
// allocation. tools/test_thread_reading_host.sh.

#include "load_timeline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The loop's calls, each inside one step (the table in thread_reading.c
// says which), in the order a turn makes them.
typedef enum {
    YONDER_TR_WAIT,             // waiting: asleep for the next event
    YONDER_TR_DISPATCH,         // input: the dispatcher's turn, and the paint of a bar it settled
    YONDER_TR_RELAYOUT,         // the window's layouts: one the window owed
    YONDER_TR_STREAM,           // the stream: stream_turn
    YONDER_TR_COMING,           // the page waiting for its sheets: coming_turn
    YONDER_TR_OWED,             // scripts: the rendering step a turn owes before a task, and its guards
    YONDER_TR_FORMS,            // scripts: the fields flushed to the page
    YONDER_TR_TASK,             // scripts: a task found and run (cleanup, history, view, ordinary, idle)
    YONDER_TR_FRAMES,           // scripts: the animations' update and the animation frames
    YONDER_TR_AFTER,            // scripts: the rendering step after a task
    YONDER_TR_ASKS,             // scripts: what the task asked of the window
    YONDER_TR_POINTER,          // scripts: the pointer's events owed
    YONDER_TR_BODIES,           // bodies and media: response bodies
    YONDER_TR_PLAYERS_ASKED,    // bodies and media: the players asked to run
    YONDER_TR_MEDIA,            // bodies and media: the media elements' turn
    YONDER_TR_VIDEOS,           // bodies and media: the videos' turn
    YONDER_TR_KEYS,             // bodies and media: the media keys
    YONDER_TR_PLAYERS_SHOWN,    // bodies and media: the players' widgets
    YONDER_TR_MOTION_TICK,      // motion: the animations' tick
    YONDER_TR_VIDEOS_TICK,      // motion: the videos' tick
    YONDER_TR_STORAGE,          // upkeep: storage
    YONDER_TR_COOKIES,          // upkeep: the cookie file
    YONDER_TR_CENSUS,           // upkeep: the census's sample
    YONDER_TR_CHILDREN,         // upkeep: children collected
    YONDER_TR_DOWNLOADS,        // upkeep: downloads
    YONDER_TR_HOLD,             // upkeep: a held Back or Forward
    YONDER_TR_PICTURES,         // upkeep: pictures settled
    YONDER_TR_TITLE,            // upkeep: the window's title
    YONDER_TR_RESIZES,          // observers: resize observers
    YONDER_TR_BEFORE_PAINT,     // observers: the work before a paint
    YONDER_TR_INTERSECTIONS,    // observers: intersection observers
    YONDER_TR_PAINT,            // paint: the paint, and a second one the cost rule asks for
    YONDER_TR_MOTION_PAINTED,   // paint: what the animations hear of the paint
    YONDER_TR_BELLS,            // the turn's close: the bells the loop rings itself
    YONDER_TR_EXIT,             // the turn's close: an --exit-when-loaded run's look
    YONDER_TR_FRAME_CLOSE,      // the turn's close: the frame closed
    YONDER_TR_SCHEDULE,         // the turn's close: the next moment handed the ticker
    YONDER_TR_CALLS
} yonder_tr_call_t;

// What ran inside a step, whichever call made it. The parts may overlap: a
// layout a task forced is inside both, and so is what a task said.
typedef enum {
    YONDER_TR_IN_TASKS,
    YONDER_TR_IN_LAYOUTS,
    YONDER_TR_IN_MODELS,
    YONDER_TR_IN_SAID,
    YONDER_TR_IN_DRAWING,
    YONDER_TR_INS
} yonder_tr_in_t;

#define YONDER_TR_SITES 48
#define YONDER_TR_SITE_TEXT 200
#define YONDER_TR_SITES_SHOWN 8

// The script runtime's collector counts at a moment (os64_js_collector's,
// copied): the lines take the difference of two.
typedef struct {
    uint64_t runs, runs_by_allocation;
    uint64_t walked, freed, walked_most;
    uint64_t bytes_before, bytes_after, threshold;
    uint64_t ticks, ticks_most;
} yonder_tr_collector_t;

typedef struct {
    uint64_t n, ticks, most, layouts, layout_ticks;
} yonder_tr_cost_t;

// The page's timer list at a moment (os64_dom_timer_counts's, copied): live
// now, and since the dom was made the arms and the list entries they walked.
typedef struct {
    uint64_t live, arms, walked;
} yonder_tr_list_t;

typedef struct {
    bool on;
    // The reading's start: the reset's moment, or with no call under way
    // then, the first call's, so the span is what the calls cover.
    int64_t from;
    bool from_first_call;
    // The call under way and when it began; YONDER_TR_CALLS for none.
    yonder_tr_call_t call;
    int64_t call_from;
    yonder_lt_step_t step;
    uint64_t call_ticks[YONDER_TR_CALLS];
    uint64_t call_n[YONDER_TR_CALLS];
    uint64_t turns[YONDER_LT_STEPS];
    uint64_t in_ticks[YONDER_LT_STEPS][YONDER_TR_INS];
    uint64_t in_n[YONDER_LT_STEPS][YONDER_TR_INS];
    // The timer tasks by site: the first YONDER_TR_SITES each by itself,
    // the rest in `other`, and those with no site in `unnamed`.
    struct {
        char site[YONDER_TR_SITE_TEXT];
        yonder_tr_cost_t cost;
    } sites[YONDER_TR_SITES];
    uint32_t nsites;
    yonder_tr_cost_t other, unnamed;
    // The collector's counts and the timer list's when the reading began.
    yonder_tr_collector_t collector_from;
    yonder_tr_list_t list_from;
    // The intersection steps: how many, the observed targets they walked,
    // and the most one walked.
    uint64_t observed_steps, observed, observed_most;
} yonder_tr_t;

// The reading starts at `now`, its counts zero, the collector's and the
// timer list's counts as `collector` and `list` (NULL for none); the call
// under way, if any, goes on from now as the reading's first call and its
// step's first turn.
void yonder_tr_reset(yonder_tr_t *t, int64_t now, const yonder_tr_collector_t *collector,
                     const yonder_tr_list_t *list);
// Nothing more is taken until the next reset, which turns it on again.
void yonder_tr_off(yonder_tr_t *t);
// The loop makes `call` from `now`: the call under way ends there, and a
// call in another step than the one under way is a turn of its step.
void yonder_tr_call(yonder_tr_t *t, yonder_tr_call_t call, int64_t now);
// `ticks` of `in` inside the step under way (nothing outside any).
void yonder_tr_inside(yonder_tr_t *t, yonder_tr_in_t in, uint64_t ticks);
// A timer task that took `ticks`, forcing `layouts` in `layout_ticks`, set
// at `site` (the dom's words; NULL for a timer whose site is not known).
void yonder_tr_timer(yonder_tr_t *t, const char *site, uint64_t ticks, uint64_t layouts, uint64_t layout_ticks);
// An intersection step that walked `targets` observed targets.
void yonder_tr_intersections(yonder_tr_t *t, uint64_t targets);

// The lines, written into `out` (cut to `cap`, always terminated), for the
// reading as it stands at `now` with the call under way ended there (the
// timer and collector lines have no call under way to end). `per_ms` is
// the counter's ticks in a millisecond.
// The steps, the costliest first, each with its calls of a millisecond or
// more (the rest summed) and what ran inside it.
void yonder_tr_steps_line(const yonder_tr_t *t, int64_t now, uint64_t per_ms, char *out, size_t cap);
// The timers by site: the costliest YONDER_TR_SITES_SHOWN, then the rest.
void yonder_tr_timers_line(const yonder_tr_t *t, uint64_t per_ms, char *out, size_t cap);
// The collector between the reading's start and `now_counts` (counts behind
// the start are another runtime's, said from their own start), with the
// script heap's bytes now and the limit they are held against
// (os64_js_heap_bytes and the page's memory limit: the allocator's count,
// which is not the engine's own count a run's bytes and threshold are in).
void yonder_tr_collector_line(const yonder_tr_t *t, const yonder_tr_collector_t *now_counts, uint64_t heap,
                              uint64_t limit, uint64_t per_ms, char *out, size_t cap);

// The record walks a turn can make whose size the page sets: the
// intersection steps and the targets they walked; the timer list, live now,
// with the arms since the reading began and the entries they walked past
// (`list_now` against the reading's start, or from its own start when it is
// behind it: another binding's); and the values the binding
// holds for the engine now (`held`, os64_dom_registry_count: wrappers,
// listeners, timers, observers), which every collector run walks with
// whatever they keep alive.
void yonder_tr_walks_line(const yonder_tr_t *t, const yonder_tr_list_t *list_now, uint64_t held, char *out,
                          size_t cap);

const char *yonder_tr_call_name(yonder_tr_call_t call);
yonder_lt_step_t yonder_tr_call_step(yonder_tr_call_t call);

#endif
