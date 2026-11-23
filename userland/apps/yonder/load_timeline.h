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
//   - per slice, the time each STEP of the window loop took, and inside
//     each step the time its script tasks, its layouts and its drawing
//     took (a step that runs across a slice's edge is shared between the
//     slices by time, and so is what was inside it);
//   - per slice, the layouts begun in it by WHO ASKED (yonder_lt_why_t),
//     what its script tasks forced (as each task says when it ends: the
//     DOM's own count, which needs no ledger), and the page's bytes parsed;
//   - for the whole timeline, every layout counted by who asked and the
//     step it ran in, and, when the layout profile times them, their
//     cascades: how long, and how many were built fresh;
//   - the YONDER_LT_LONG_MAX longest steps of YONDER_LT_LONG_US or more,
//     waits left out (the slices say how long the loop slept), each with
//     when it began, the layouts made inside it by who asked, its drawing,
//     and the longest script task inside it with what that task forced;
//   - the fresh cascades libgarb timed by part (reading the sheets,
//     sizing its table, matching the elements), by who asked;
//   - the updates libgarb refused as broad, by the caller's name for what
//     crossed the half in each, and what triggered those whose trigger
//     marked the most;
//   - what the subject :has() filter visited, marked and turned away, by
//     the compounds' filters and by the changes;
//   - what the cascade's readers were asked and passed, by the kind of
//     question, the front indexes counted again and the unbounded
//     position walks;
//   - what the back indexes (libgarb's index_walk) cost the updates, and
//     what the updates that succeeded restyled: marked by their changes,
//     and marked below a changed winner;
//   - the arriving page's parser calls, and the slowest of them;
//   - the milestones, each the first time it was told.
// Past the last slice nothing begun after it is taken by what has a time
// (steps, layouts, tasks, bytes, parser calls, broad refusals); what has
// none (a part inside a step, a fresh build) is taken until the caller
// stops the timeline (yonder_lt_stop), which yonder does at the first step
// past the end (yonder_lt_running false). After that nothing is taken, so
// a page left open for an hour costs the loop only the caller's test.
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
#define YONDER_LT_BROAD_MAX 8
#define YONDER_LT_BROAD_TEXT 320
#define YONDER_LT_PARSE_MAX 8
#define YONDER_LT_REASONS 10
#define YONDER_LT_MARK_KINDS 16
// What the cascade's readers did in one update (yonder_lt_readers): for
// each kind of question, YONDER_LT_ASKERS at most, the readers asked, those
// whose filters passed, and the questions that marked a reach; the bounded
// front indexes counted again (walks, children, turned); the unbounded
// position walks and the siblings they passed over; and of the rules, the
// readers and the bare ones.
#define YONDER_LT_ASKERS 8
typedef struct {
    uint64_t asked[YONDER_LT_ASKERS], passed[YONDER_LT_ASKERS], reached[YONDER_LT_ASKERS];
    uint64_t front_walks, front_walked, front_turned, walks, walked;
    uint64_t readers, bare;
} yonder_lt_readers_t;
#define YONDER_LT_PARSE_TEXT 128

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
    YONDER_LT_PAINT,        // the paint, and a second one the animations' cost rule asks for
    YONDER_LT_CLOSE,        // the turn's close: its bells, the exit, the frame closed, pictures scheduled
    YONDER_LT_STEPS
} yonder_lt_step_t;
// The grid's column for a layout made outside any step.
#define YONDER_LT_OUTSIDE YONDER_LT_STEPS

// What was inside a step: its script tasks, its layouts, its drawing, the
// arriving page's parser calls, its looks for new sheets, and a page being
// made the shown one. The parts may overlap: a layout a task forced is
// inside both the task and the layouts, and the arrival's own layout inside
// both the arriving and the layouts.
typedef enum {
    YONDER_LT_IN_TASKS, YONDER_LT_IN_LAYOUTS, YONDER_LT_IN_DRAWING, YONDER_LT_IN_PARSING, YONDER_LT_IN_SHEETS,
    YONDER_LT_IN_ARRIVING, YONDER_LT_INS
} yonder_lt_in_t;

// WHO ASKED for a layout: the outermost reason told (yonder.c's lt_why),
// so a rebuild a script's read asked for is the read's.
typedef enum {
    YONDER_LT_WHY_OTHER,    // no reason told: a site the tagging does not reach
    YONDER_LT_WHY_READ,     // a script's read: a box, a hit, text rects, a computed style, a scroll
    YONDER_LT_WHY_REBUILD,  // yonder bringing the shown page up to date after a script's changes
    YONDER_LT_WHY_ARRIVAL,  // a page arriving or being shown
    YONDER_LT_WHY_SHEET,    // a style sheet arriving
    YONDER_LT_WHY_PICTURE,  // pictures' or faces' sizes arriving
    YONDER_LT_WHY_MOTION,   // the animations' frames
    YONDER_LT_WHY_WINDOW,   // the window: its size, its zoom, a setting
    YONDER_LT_WHYS
} yonder_lt_why_t;

typedef enum {
    YONDER_LT_MARK_ARRIVED,         // the page arrived whole: its stream ended, it is the shown one
    YONDER_LT_MARK_PAINT,           // the first paint of the shown page
    YONDER_LT_MARK_PICTURE,         // a video's first picture handed to the paint
    YONDER_LT_MARKS
} yonder_lt_mark_t;

typedef struct {
    uint64_t at_us, us;             // from the navigation's start; how long
    uint8_t step;                   // yonder_lt_step_t
    uint64_t in_us[YONDER_LT_INS];  // what was inside it
    uint32_t layouts;               // made inside it, and by who asked
    uint32_t why_n[YONDER_LT_WHYS];
    uint64_t why_us[YONDER_LT_WHYS];
    uint32_t forced;                // what the tasks inside it forced
    uint64_t forced_us;
    char task[YONDER_LT_TASK_MAX];  // the longest task inside it, "" for none
    uint64_t task_us;
    uint32_t task_layouts;          // what that task forced
    uint64_t task_layout_us;
} yonder_lt_long_t;

typedef struct {
    uint64_t step_us[YONDER_LT_STEPS];
    uint64_t in_us[YONDER_LT_STEPS][YONDER_LT_INS];
    uint32_t layouts;               // begun in the slice, and by who asked
    uint64_t layout_us;
    uint32_t why_n[YONDER_LT_WHYS];
    uint64_t why_us[YONDER_LT_WHYS];
    uint32_t tasks;                 // script tasks that ended in it
    uint32_t forced;                // what those tasks forced
    uint64_t forced_us;
    uint64_t bytes;                 // the arriving page's bytes parsed in it
} yonder_lt_slice_t;

typedef struct {
    bool on;
    int64_t began_us;
    yonder_lt_slice_t slices[YONDER_LT_SLICES];
    int64_t marks[YONDER_LT_MARKS];             // -1 until told
    // Every layout of the timeline by who asked and the step it ran in, a
    // layout outside any step in the column past the steps,
    // YONDER_LT_OUTSIDE ("no step": before the loop's first turn, or after a
    // navigation that began inside a step, until the loop's next step); and, of those the layout profile
    // timed, their cascades by who asked: how many, how long, and of them
    // those built fresh.
    uint32_t grid_n[YONDER_LT_WHYS][YONDER_LT_STEPS + 1];
    uint64_t grid_us[YONDER_LT_WHYS][YONDER_LT_STEPS + 1];
    uint32_t cascades[YONDER_LT_WHYS], fresh[YONDER_LT_WHYS];
    uint64_t cascade_us[YONDER_LT_WHYS], fresh_us[YONDER_LT_WHYS];
    // The fresh cascades libgarb timed by part (yonder_lt_build), by who
    // asked: how many, their parts' time, and the rules and elements they
    // read and matched, summed.
    uint32_t builds[YONDER_LT_WHYS], kept[YONDER_LT_WHYS];
    uint64_t build_sheets[YONDER_LT_WHYS], build_table[YONDER_LT_WHYS], build_matching[YONDER_LT_WHYS];
    uint64_t build_rules[YONDER_LT_WHYS], build_elements[YONDER_LT_WHYS];
    // Why the builds that did not take a kept read read the sheets again,
    // by the caller's name for the reason, YONDER_LT_REASONS at most (the
    // rest counted in `again_other`).
    struct {
        const char *name;
        uint32_t n;
    } again[YONDER_LT_REASONS];
    uint32_t nagain, again_other;
    // The updates refused as broad (yonder_lt_broad): how many, and the
    // YONDER_LT_BROAD_MAX whose trigger marked the most, most first.
    uint32_t nbroad, broad_kept;
    struct {
        uint64_t at_us;
        uint32_t marked;
        char text[YONDER_LT_BROAD_TEXT];
    } broad[YONDER_LT_BROAD_MAX];
    // Every broad refusal by the caller's name for what crossed the half
    // in it, YONDER_LT_REASONS at most (the rest in `broad_by_other`).
    struct {
        const char *name;
        uint32_t n;
    } broad_by[YONDER_LT_REASONS];
    uint32_t nbroad_by, broad_by_other;
    // The back indexes counted again (yonder_lt_back): the updates that
    // counted any, and the walks, children and turns they counted.
    uint64_t back_updates, back_walks, back_walked, back_turned;
    // The subject :has() filter (yonder_lt_has_subjects): the elements the
    // updates' climbs visited, marked, turned away by the compounds'
    // filters and turned away by the changes, and the latest count of the
    // rules' subject :has() compounds and the bare ones.
    uint64_t has_visited, has_marked, has_away, has_away_change, has_entries, has_bare;
    // The readers (yonder_lt_readers): the updates that asked any, their
    // sums (the rules' counts the latest), and the caller's names for the
    // kinds of question (the first call's).
    uint64_t readers_updates;
    yonder_lt_readers_t readers;
    uint32_t nasker_kinds;
    const char *asker_names[YONDER_LT_ASKERS];
    // What the updates that succeeded restyled (yonder_lt_marks): how many
    // updates, the elements their changes marked, and those marked below an
    // element whose winners came out changed; and every mark by the kind
    // that made it, under the caller's names for the kinds (the first
    // call's, YONDER_LT_MARK_KINDS at most).
    uint64_t marks_updates, marks_marked, marks_inherited;
    uint64_t marks_by[YONDER_LT_MARK_KINDS];
    const char *const *mark_names;
    uint32_t nmark_kinds;
    // The arriving page's parser calls (yonder_lt_parse): how many and how
    // long in all, and the YONDER_LT_PARSE_MAX slowest, slowest first.
    uint32_t nparse, parse_kept;
    uint64_t parse_us;
    struct {
        uint64_t at_us, us;
        char text[YONDER_LT_PARSE_TEXT];
    } parse[YONDER_LT_PARSE_MAX];
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
// `out` made the timeline as it would stand if the step under way ended at
// `now_us`, `t` left as it is: what a record written in the middle of a
// step (at a page's arrival, inside the stream's step) says of that step.
void yonder_lt_snapshot(const yonder_lt_t *t, int64_t now_us, yonder_lt_t *out);
// Whether a time `now_us` is still one the timeline takes.
bool yonder_lt_running(const yonder_lt_t *t, int64_t now_us);

// A step of the loop from `from_us`: what the calls below tell before its
// end is counted inside it. A begin while one is under way ends that one
// at `from_us` first.
void yonder_lt_step_begin(yonder_lt_t *t, yonder_lt_step_t step, int64_t from_us);
void yonder_lt_step_end(yonder_lt_t *t, int64_t to_us);

// A layout's cascade as the layout profile timed it: how long, and whether
// it was built fresh rather than updated or kept.
typedef struct {
    uint64_t us;
    bool fresh;
} yonder_lt_cascade_t;
// A layout begun at `from_us` that took `us`, asked for by `why`, its
// cascade `cascade` (NULL when the profile did not time it). Counted in the
// slice it began in, in the grid, and in the step under way.
void yonder_lt_layout(yonder_lt_t *t, int64_t from_us, uint64_t us, yonder_lt_why_t why,
                      const yonder_lt_cascade_t *cascade);
// A script task ended at `at_us` after `us`, forcing `layouts` in
// `layout_us`, named `what` (cut to fit). Its forced layouts count in the
// slice it ended in and in the step under way, its time inside the step,
// and the longest task of a step is the one its long-step line names.
void yonder_lt_task(yonder_lt_t *t, int64_t at_us, const char *what, uint64_t us, uint32_t layouts,
                    uint64_t layout_us);
// `us` of `in` inside the step under way (a drawing, a look for sheets, a
// page arriving; tasks, layouts and parser calls have calls of their own).
void yonder_lt_inside(yonder_lt_t *t, yonder_lt_in_t in, uint64_t us);
// A parser call of the arriving page's begun at `from_us` that took `us`,
// said by `text` (the caller's words: what call, how many bytes, where in
// the page, inside what): inside the step under way, counted, and kept
// when among the slowest.
void yonder_lt_parse(yonder_lt_t *t, int64_t from_us, uint64_t us, const char *text);
// `n` bytes of the arriving page parsed at `at_us`.
void yonder_lt_bytes(yonder_lt_t *t, int64_t at_us, uint64_t n);
// A milestone at `at_us`: the first one told stands.
void yonder_lt_mark(yonder_lt_t *t, yonder_lt_mark_t mark, int64_t at_us);
// A fresh cascade libgarb timed by part (garb_cascade_build_cost, on the
// timeline's clock), asked for by `why`: whether it took a kept read, and
// if not, why it read the sheets again (`again`, the caller's name for the
// reason, a string that outlives the timeline; NULL when it took one).
typedef struct {
    uint64_t sheets_us, table_us, matching_us;
    uint32_t rules, elements;
    bool kept;
    const char *again;
} yonder_lt_build_t;
void yonder_lt_build(yonder_lt_t *t, yonder_lt_why_t why, const yonder_lt_build_t *b);
// An update refused as broad at `at_us`, its trigger marking `marked`
// elements, `by` naming what crossed the half in it (a name that lives
// as long as the timeline, compared by address) and `text` saying it (the
// caller's words: libgarb's trigger, cut to fit). Counted always, by `by`
// too; kept when among the most marked.
void yonder_lt_broad(yonder_lt_t *t, int64_t at_us, uint32_t marked, const char *by, const char *text);
// An update at `at_us` that counted back indexes again: `walks` walks,
// `walked` children, `turned` of them marked. Nothing for walks 0.
void yonder_lt_back(yonder_lt_t *t, int64_t at_us, uint64_t walks, uint64_t walked, uint64_t turned);
// An update at `at_us` whose :has() climbs visited `visited` elements while
// the rules hold a subject :has() (`entries` compounds, `bare` with no
// filter), marking `marked` alone, turning `away` away by the compounds'
// filters and `away_change` by the changes. Nothing for visited 0.
void yonder_lt_has_subjects(yonder_lt_t *t, int64_t at_us, uint64_t visited, uint64_t marked, uint64_t away,
                            uint64_t away_change, uint64_t entries, uint64_t bare);
// An update at `at_us` whose readers were asked or walked (`r`), its first
// `nkinds` kinds of question named `names[i]` (names that live as long as
// the timeline). Nothing when it asked and walked nothing.
void yonder_lt_readers(yonder_lt_t *t, int64_t at_us, const yonder_lt_readers_t *r, uint32_t nkinds,
                       const char *const *names);
// An update at `at_us` that succeeded, its changes marking `marked`
// elements and restyling `inherited` more below changed winners; `by`
// holds its `nby` counts by kind, kind i named `names[i]` (names that live
// as long as the timeline).
void yonder_lt_marks(yonder_lt_t *t, int64_t at_us, uint64_t marked, uint64_t inherited, const uint64_t *by,
                     uint32_t nby, const char *const *names);

// The lines, each written into `out` (cut to `cap`, always terminated).
// The summary: the slices' size, the milestones, the long steps' count.
void yonder_lt_summary(const yonder_lt_t *t, char *out, size_t cap);
// The slices from the first through the last that holds anything.
uint32_t yonder_lt_slices_used(const yonder_lt_t *t);
// Slice `i`: its busy share (the time not waiting, of the time its steps
// took), its steps of 10 ms or more, longest first, each with what was
// inside it, the rest summed; its layouts by who asked; its script tasks;
// its bytes parsed.
void yonder_lt_slice_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);
// The grid: every cell that holds a layout, the costliest first, then the
// cascades by who asked when the profile timed any.
void yonder_lt_layouts_line(const yonder_lt_t *t, char *out, size_t cap);
// Long step `i` (0 is the longest), i < t->nlongs.
void yonder_lt_long_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);
// Broad refusal `i` kept (0 marked the most), i < t->broad_kept: when, and
// the caller's words.
void yonder_lt_broad_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);
// Parser call `i` kept (0 the slowest), i < t->parse_kept: when, how long,
// and the caller's words.
void yonder_lt_parse_line(const yonder_lt_t *t, uint32_t i, char *out, size_t cap);

const char *yonder_lt_step_name(yonder_lt_step_t step);
const char *yonder_lt_why_name(yonder_lt_why_t why);

#endif
