#ifndef TTY_REFLOW_H
#define TTY_REFLOW_H

// tty_reflow.h — carry a terminal's whole grid into a grid of another shape
// WITHOUT LOSING ANY OF IT. CONSOLE_FONTS.md § The contents survive.
//
// WHY THIS IS NOT tty_resize. A resize is a window being dragged: the program
// inside repaints, so tty_resize keeps the left edge of every line, lets the
// rest go, and sizes the ring by the new row count. A FONT CHANGE has nobody
// to repaint for it — the likeliest moment is just after boot, with the boot
// log on the glass and a larger face cutting 240 columns to 160 — so the rule
// here is the opposite one: what was on the screen or in the scrollback is
// still there afterwards, in the same order, and going back to the old shape
// puts it back the way it was. The two things that can still be lost are
// forced by the caller's own limits, and each is COUNTED in the result
// (`history_dropped`, `below_clipped`) rather than going quietly.
//
// THE UNIT IS THE PARAGRAPH: a row, plus every row after it whose first cell
// carries TTY_ATTR_WRAPPED. A paragraph is laid into the new grid as one run
// of cells — onto more rows when the grid is narrower, fewer when it is
// wider — and each row after its first is marked, so the next reflow finds
// the same paragraph. Rows the terminal wrapped while PRINTING carry no mark
// and are paragraphs of their own; they lose nothing, they just never rejoin.
//
// Pure: cells in, cells out, no allocation and no lock, so
// tools/test_tty_reflow_host.sh drives it under ASan. Two calls because the
// caller owns the memory and cannot size it until the wrapping is counted:
// PLAN says how many lines the result needs, RUN lays them down.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tty_cell.h"

typedef struct
{
	const tty_cell_t *cells;           // the ring, total_lines * cols cells
	uint32_t cols, rows;
	uint32_t total_lines;
	uint32_t screen_top;               // ring index of live row 0
	uint32_t hist_lines;
	uint32_t view_offset;
	uint32_t cur_row, cur_col;
	uint32_t save_row, save_col;
} tty_reflow_in_t;

typedef struct
{
	// The result is laid down from ring line 0: history first, then the
	// screen, so screen_top == hist_lines (tty_resize's convention too).
	uint32_t lines_needed;             // hist_lines + rows: the least total_lines that holds it
	uint32_t hist_lines;
	// The same line stays at the top of the view — or, when the fence below
	// threw that line away, the oldest line there still is.
	uint32_t view_offset;
	uint32_t cur_row, cur_col;
	uint32_t save_row, save_col;
	// What did NOT come across, which is zero unless a fence forced it:
	uint32_t history_dropped;          // oldest lines, when max_total could not hold them all
	uint32_t below_clipped;            // screen rows under the cursor that no longer fit
} tty_reflow_out_t;

// Count, place nothing. `max_total` is the most ring lines the caller is
// willing to pay for; history beyond it is dropped OLDEST FIRST and counted.
// Returns false for geometry it cannot work with (a NULL ring, fewer than 2
// columns or rows either side, a cursor outside its own screen, or a
// max_total that cannot hold even one screen).
bool tty_reflow_plan(const tty_reflow_in_t *in, uint32_t new_cols, uint32_t new_rows,
                     uint32_t max_total, tty_reflow_out_t *out);

// Lay the grid into `fresh`, which is `fresh_total` lines of `new_cols`
// ZEROED cells with fresh_total >= the plan's lines_needed (and <= the same
// max_total, so the two calls agree about what history survives). Returns
// false, having written nothing, if that does not hold.
bool tty_reflow_run(const tty_reflow_in_t *in, uint32_t new_cols, uint32_t new_rows,
                    uint32_t max_total, tty_cell_t *fresh, uint32_t fresh_total,
                    tty_reflow_out_t *out);

#endif // TTY_REFLOW_H
