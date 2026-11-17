// frame_clock.h — the frame clock's arithmetic (FRAME_CLOCK.md § 2), pure:
// no kernel headers, no locks, no clock of its own, so
// tools/test_frame_clock_host.sh drives it under ASan and UBSan beside a model
// display, and tools/test_frame_clock_mutants.py breaks each rule.
//
// THE FRAME, AS THE DISPLAY SEES IT. A display reads its picture one line at a
// time, top to bottom, `lines` lines a frame, and a counter in the display
// core says which line it is on now. The counter's numbering is the core's,
// not the screen's: on the P5 the picture occupies counter lines 38..1477 and
// the vertical blank is the rest, 1478..1480 and then 0..37 after the wrap.
// So the blank is [blank_start, blank_end) read around the wrap, and screen
// row r is read while the counter shows blank_end + r.
//
// SWEEP POSITION is that numbering turned into the screen's: s = counter -
// blank_end, modulo the frame, so 0..active-1 are the rows being shown and
// active..lines-1 are the blank before the next frame's row 0. Within one
// frame it only moves forward, which is what makes the verdicts arithmetic.

#ifndef FRAME_CLOCK_H
#define FRAME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// What the display core says about one frame, and how long one lasts.
typedef struct {
	uint32_t lines;         // lines in a frame, blank included
	uint32_t blank_start;   // the counter's first line of the vertical blank
	uint32_t blank_end;     // its first line after the blank: screen row 0
	uint64_t period_ns;     // one frame, measured (frame_clock_measure_*)
} fc_frame_t;

// The frame's facts make sense: lines between 2 and 32768, both blank lines
// inside the frame and different, and a period. Every function below
// assumes it and answers as if nothing were shown when it does not hold.
bool frame_clock_frame_valid(const fc_frame_t *f);

// The rows a frame shows (the frame less its blank).
uint32_t frame_clock_active_rows(const fc_frame_t *f);

// A counter reading as a sweep position (above), and whether it is in the
// blank. A reading beyond the frame is not a position: the caller has
// already refused it (frame_clock_line_ok).
uint32_t frame_clock_sweep(const fc_frame_t *f, uint32_t line);
bool frame_clock_in_blank(const fc_frame_t *f, uint32_t line);

// A raw reading is a line of this frame: not all ones (nothing answered),
// and below `lines`.
bool frame_clock_line_ok(const fc_frame_t *f, uint32_t raw);

// When the next blank begins, in microseconds on the caller's clock, given
// the line read at `now_us`. ROUNDED DOWN: a wake set for it is early by
// less than a microsecond, never late. On the blank's first line it is now;
// anywhere else in the blank it is the next frame's.
uint64_t frame_clock_next_blank_us(const fc_frame_t *f, uint32_t line, uint64_t now_us);

// Which eighth of the shown rows the sweep was in, 0 (the top) to 7, or
// FC_BAND_BLANK when it was in the blank: the histogram /sys/gui keeps of
// where each copy began.
#define FC_BANDS 9
#define FC_BAND_BLANK 8
uint32_t frame_clock_band(const fc_frame_t *f, uint32_t line);

// ── The verdict: was a copy torn? ──────────────────────────────────────────
// A copy to the framebuffer is a list of PIECES copied one after another,
// each top row to bottom row, at one speed in pixels, so each piece takes
// its share of the copy's time by its pixels. A row counts as new from the
// moment the copy has finished it. A piece is TORN when one sweep of the
// display reads some of its rows new and some old: the picture splits there.
// A piece the sweep reads wholly before its copy, or wholly after, is clean,
// even if the sweep passes through its rows while the copy runs; a copy that
// leads the sweep down the screen is exactly that.
//
// The copy began with the counter at `begin_line` and ended with it at
// `end_line`, `elapsed_us` apart: two readings, and the sweep's own steady
// pace between them. The readings place the sweep within a frame and the
// time says how many times it went round. A copy that lasted a whole frame
// or more is torn (every row was read while it ran). Rows below the shown
// area are never read, so they cannot tear.
typedef struct {
	int32_t top, bottom;    // screen rows [top, bottom)
	uint64_t pixels;        // its share of the copy's work
} fc_piece_t;

typedef struct {
	bool torn;
	bool began_in_blank;
	uint32_t band;          // frame_clock_band of begin_line
} fc_verdict_t;

fc_verdict_t frame_clock_judge(const fc_frame_t *f, uint32_t begin_line, uint32_t end_line,
                               uint64_t elapsed_us, const fc_piece_t *pieces, uint32_t count);

// ── The lead (FRAME_CLOCK.md § 3) ──────────────────────────────────────────
// How long before the blank the compositor must wake: the worst of its last
// FC_LEAD_PASSES passes, plus a quarter of that or FC_LEAD_MARGIN_US,
// whichever is more, between FC_LEAD_MIN_US and FC_LEAD_MAX_US. With no pass
// recorded it is the most.
#define FC_LEAD_PASSES 16
#define FC_LEAD_MARGIN_US 250
#define FC_LEAD_MIN_US 500
#define FC_LEAD_MAX_US 8000
typedef struct {
	uint32_t cost_us[FC_LEAD_PASSES];
	uint32_t count, next;
} fc_lead_t;

void frame_clock_lead_record(fc_lead_t *l, uint64_t pass_us);
uint32_t frame_clock_lead_us(const fc_lead_t *l);

// ── The copy's order (FRAME_CLOCK.md § 4) ──────────────────────────────────
// The copy runs in horizontal strips, top to bottom, across all the damage
// at once. For the strip [strip_top, strip_top + strip_rows), its pieces are
// the damage rectangles' parts in it, left to right; parts that overlap
// across are merged into the box around them, so nothing in a strip is
// copied twice and a strip never copies more than its own area. (A merged
// box can take in pixels nobody damaged: they are copied from a back buffer
// that is current everywhere, which costs time and nothing else.) `out`
// must hold `count` rectangles; the return is how many it holds.
typedef struct {
	int32_t x, y, w, h;
} fc_rect_t;

uint32_t frame_clock_strip(const fc_rect_t *damage, uint32_t count, int32_t strip_top,
                           int32_t strip_rows, fc_rect_t *out);

// ── When the clock runs (FRAME_CLOCK.md § 5) ───────────────────────────────
// While there is something to show or someone waiting for a frame, and for
// FC_LINGER_FRAMES frames after; then it stops until either returns. One
// call a frame, saying what that frame had.
#define FC_LINGER_FRAMES 4
typedef struct {
	uint32_t idle_frames;   // frames in a row with nothing to show and nobody waiting
} fc_run_t;

bool frame_clock_keep_running(fc_run_t *r, bool damage, uint32_t waiting);

// ── Measuring the period at start ──────────────────────────────────────────
// The counter is read in a tight loop, each reading stamped, and fed here.
// The period is measured between two LINE CHANGES, each seen within
// FC_MEASURE_EDGE_US of the reading before it (so it is the start of a line
// to within that), with the lines in between counted through every wrap, so
// a reading delayed by an interrupt costs nothing unless the delay is long
// enough to hide a wrap. The window closes at the first such change
// that is at least FC_MEASURE_US after the first one AND has FC_MEASURE_WRAPS
// whole frames of lines behind it; the caller stops feeding after
// FC_MEASURE_BOUND_US whatever has happened, which leaves room for three
// frames of the slowest rate taken.
// What a display does, and what the result checks: every reading is a line
// of the frame, the counter moves, and only forward except at the wrap, no
// two readings are so far apart that a wrap could hide between them (half a
// frame, and a step back across half the fastest frame is taken for one),
// and the rate is a display's. Anything else is no beam, with the reason.
#define FC_MEASURE_US 70000
#define FC_MEASURE_EDGE_US 2
#define FC_MEASURE_BOUND_US 250000
#define FC_MEASURE_WRAPS 3
#define FC_MEASURE_HZ_MIN 20
#define FC_MEASURE_HZ_MAX 500
typedef enum {
	FC_MEASURE_OK = 0,
	FC_MEASURE_STILL,           // the counter never moved
	FC_MEASURE_BEYOND_FRAME,    // a reading was not a line of the frame
	FC_MEASURE_BACKWARD,        // the counter went back without wrapping
	FC_MEASURE_TOO_FEW_WRAPS,   // it moved, but the window never closed
	FC_MEASURE_GAP,             // readings too far apart to count the wraps
	FC_MEASURE_RATE,            // the rate is not a display's
} fc_measure_reason_t;

typedef struct {
	uint32_t lines;                     // the frame's lines, given at start
	bool started, anchored, done;
	uint32_t prev;
	uint64_t prev_us;
	uint64_t wraps;
	uint64_t anchor_us, anchor_pos;     // the first change: time, lines counted
	uint64_t end_us, end_pos;           // the change that closed the window
	uint64_t gap_max_us;
	uint32_t beyond, backward, hidden;   // hidden: steps back across a long gap
} fc_measure_t;

void frame_clock_measure_begin(fc_measure_t *m, uint32_t lines);
// One reading. Returns true once the window has closed; feed no more then.
bool frame_clock_measure_feed(fc_measure_t *m, uint32_t raw, uint64_t now_us);
fc_measure_reason_t frame_clock_measure_result(const fc_measure_t *m, uint64_t *period_ns);
const char *frame_clock_measure_reason_name(fc_measure_reason_t reason);

#endif
