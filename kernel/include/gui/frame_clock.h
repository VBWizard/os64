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

// A raw reading is a line of this frame: below `lines`, which all ones
// (nothing answered) never is.
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
// FC_LEAD_PASSES passes (about a second of frames), plus half of that or
// FC_LEAD_MARGIN_US, whichever is more, between FC_LEAD_MIN_US and
// FC_LEAD_MAX_US. With no pass recorded it is the most. It follows recent
// passes, so typing and the pointer keep a short lead and a full-screen
// animation gets a long one. The lead costs no polling (the compositor
// sleeps again after its pass, until shortly before the blank); it is time
// taken from the publisher's frame, which `publish to glass` shows.
#define FC_LEAD_PASSES 64
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

// ── Planning a frame (FRAME_CLOCK.md § 3, § 9) ─────────────────────────────
// From a line read at `now_us`: the blank the next frame aims at, and when
// to wake for it, the lead before. A blank that begins sooner than the lead
// is passed over for the next, since a pass begun now would miss it.
typedef struct {
	uint64_t blank_at_us;   // when the aimed-at blank begins
	uint64_t wake_at_us;    // the lead before it
} fc_plan_t;

fc_plan_t frame_clock_plan(const fc_frame_t *f, uint32_t line, uint64_t now_us, uint32_t lead_us);

// The aim polls the beam after the pass. One reading's verdict: in a blank,
// COPY (the copy then leads the sweep down the screen); in shown rows after
// the aimed-at blank must have ended, LATE; otherwise keep polling. A
// reading in shown rows while the blank should be under way is the blank
// not quite begun (the plan rounds down), never a missed one: a sweep cannot
// cross a whole blank between two readings that close.
typedef enum { FC_AIM_WAIT, FC_AIM_COPY, FC_AIM_LATE } fc_aim_t;

fc_aim_t frame_clock_aim(const fc_frame_t *f, uint32_t line, uint64_t now_us, uint64_t blank_at_us);

// After its pass the compositor sleeps again, until FC_AIM_MARGIN_US before
// the blank (the margin covers the timer's lateness), and only then aims, so
// a long lead costs no polling. How long the aim may poll: `margin_us`, how
// far ahead of the blank it was meant to begin, plus FC_AIM_SLACK_LINES
// lines. Past it, a counter that never moved is frozen (the beam is
// withdrawn) and one that moved is an overrun (the frame is late).
#define FC_AIM_MARGIN_US 250
#define FC_AIM_SLACK_LINES 8
uint64_t frame_clock_aim_bound_us(const fc_frame_t *f, uint32_t margin_us);

// How long `lines` lines take, rounded up. An aim whose first reading is
// already in the blank has seen nothing move, so the counter must change
// within FC_AIM_CONFIRM_LINES before the copy (one stopped at a blank line
// would otherwise look like a perfect aim every frame).
#define FC_AIM_CONFIRM_LINES 3
uint64_t frame_clock_lines_us(const fc_frame_t *f, uint32_t lines);

// Why a frame was late, for the cure: the compositor was off its CPU (a
// pass gave the CPU to another thread between its wake and its aim); the
// wake came too late, including a wake that lost the CPU (a wake on
// time, with the same pass, would have made the blank); the pass ran long
// (it would not have fit even from an on-time wake); or the pass ended in
// time and the sweep was past anyway when the aim looked (the sleep after
// the pass woke too late, or something held the compositor between). An
// overrun (the
// counter moved but no blank came within the aim's bound) is the caller's
// to name, from the aim.
typedef enum {
	FC_LATE_OFF_CPU,
	FC_LATE_WAKE,
	FC_LATE_PASS,
	FC_LATE_PAST,
	FC_LATE_OVERRUN,
	FC_LATE_CAUSES
} fc_late_t;

fc_late_t frame_clock_late_cause(bool off_cpu, uint64_t wake_at_us, uint64_t woke_us,
                                 uint64_t pass_end_us, uint64_t blank_end_us);

// ── A missed blank (FRAME_CLOCK.md § 9, the hold rule) ─────────────────────
// A composed frame that misses its blank is HELD for the next one, which a
// single miss deserves: shown at the next blank, whole. A frame already held
// once is FORCED at its next pass: copied at the blank if that pass makes
// it, and wherever the sweep is if it does not, so it may tear. One rule for
// every cause of lateness, and the property it buys: what the compositor
// has composed is on the glass within two of its own passes, so a starved or
// slow compositor tears, as before the clock, and never freezes the screen.
// `made_blank` is this pass's aim; the step remembers whether the frame
// being carried was held already.
typedef enum { FC_SHOW_COPY, FC_SHOW_HOLD, FC_SHOW_FORCE } fc_show_t;
typedef struct {
	bool held;
} fc_hold_t;

fc_show_t frame_clock_hold_step(fc_hold_t *h, bool made_blank);

// When the aimed-at blank ends (its lines' length after it begins).
uint64_t frame_clock_blank_end_us(const fc_frame_t *f, uint64_t blank_at_us);

// How late a wake arrived, by bucket: under 50 us << k for k = 0..5, then
// 1.6 ms or more.
#define FC_WAKE_BUCKETS 7
uint32_t frame_clock_wake_bucket(uint64_t late_us);

// ── Without a beam (FRAME_CLOCK.md § 7) ────────────────────────────────────
// The free clock: one frame every FC_FREE_PERIOD_NS. The next frame after
// `now_ns` is `prev_ns` plus whole periods, the first past now, so a late
// frame skips forward rather than the clock drifting; 0 starts it at now
// plus a period. In nanoseconds, so the sixtieth of a second is rounded
// once (to 16,666,667 ns) rather than once a frame.
#define FC_FREE_PERIOD_NS 16666667ull
uint64_t frame_clock_free_next_ns(uint64_t prev_ns, uint64_t now_ns);

// ── The wake's timer count ─────────────────────────────────────────────────
// The compositor's CPU's one-shot is set for the wake, unless the lease
// (`lease_ms`, 0 = none) is sooner: never longer than the lease, so the CPU
// is preempted at least as often as the lease alone would. The count is in
// the timer's ticks, the wake's rounded up so it never fires before its
// instant, at least 1 and at most the 32-bit register's reach; `wake_first`
// says which of the two it is.
typedef struct {
	uint32_t count;
	bool wake_first;
} fc_timer_t;

fc_timer_t frame_clock_timer(uint64_t wake_in_us, uint64_t ticks_per_second, uint32_t lease_ms);

// ── The reservation (FRAME_CLOCK.md § 9) ───────────────────────────────────
// While the clock runs, the compositor's CPU is kept for threads pinned to
// it, with at least FC_RESERVE_MIN_CPUS CPUs and the dial on; below that
// the CPU is shared, as without the clock.
#define FC_RESERVE_MIN_CPUS 4
bool frame_clock_reserves(uint32_t cpus, bool dial_on, bool running);

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
// two readings are so far apart that a wrap could hide between them, and the
// rate is a display's. Anything else is no beam, with the reason.
//
// "So far apart" is half a frame, and the frame it is half of is NOT the
// period counted through the wraps: a stall that hides a wrap makes that
// period too long, and so would excuse itself. It is a frame measured from
// the UNBROKEN STRETCHES alone, the steps no longer than FC_MEASURE_RUN_US,
// each too short to hide a wrap, so what they advanced is known without
// counting any. Those stretches must cover a frame's lines.
#define FC_MEASURE_US 70000
#define FC_MEASURE_EDGE_US 2
#define FC_MEASURE_RUN_US 50
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
	uint64_t run_us, run_lines;         // the unbroken stretches: time, lines advanced
	uint32_t beyond, backward;
} fc_measure_t;

void frame_clock_measure_begin(fc_measure_t *m, uint32_t lines);
// One reading. Returns true once the window has closed; feed no more then.
bool frame_clock_measure_feed(fc_measure_t *m, uint32_t raw, uint64_t now_us);
fc_measure_reason_t frame_clock_measure_result(const fc_measure_t *m, uint64_t *period_ns);
const char *frame_clock_measure_reason_name(fc_measure_reason_t reason);

#endif
