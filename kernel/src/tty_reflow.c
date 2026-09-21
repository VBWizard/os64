// tty_reflow.c — see tty_reflow.h. Pure: cells, stdint, and the one ANSI
// attribute bit that decides whether a cell is blank.

#include "tty_reflow.h"
#include "os64/ansi.h"

// Everything below speaks in LOGICAL rows: 0 is the oldest history line,
// hist_lines is live row 0. The ring index is this function's secret.
static const tty_cell_t *old_row(const tty_reflow_in_t *in, uint32_t logical)
{
	uint32_t base = (in->screen_top + in->total_lines - in->hist_lines) % in->total_lines;
	return in->cells + (size_t)((base + logical) % in->total_lines) * in->cols;
}

// NOTHING TO SEE, which is narrower than "nothing written": a space on a
// coloured background is half of every ANSI picture, and a reversed blank is
// a block. Only a cell that paints as bare paper may be dropped from the end
// of a line.
static bool cell_is_blank(const tty_cell_t *c)
{
	return (c->ch == 0 || c->ch == ' ') && c->bg == 0 &&
	       !(c->attrs & OS64_ANSI_ATTR_REVERSE);
}

static uint32_t row_length(const tty_cell_t *row, uint32_t cols)
{
	while (cols > 0 && cell_is_blank(&row[cols - 1]))
		cols--;
	return cols;
}

// A place in the OLD grid whose place in the NEW one has to be found.
typedef struct
{
	uint32_t row, col;                 // in: old logical row and column
	uint32_t new_row, new_col;         // out
	bool placed;
} anchor_t;

enum { A_SCREEN, A_CURSOR, A_SAVED, A_VIEW, A_COUNT };

typedef struct
{
	anchor_t anchor[A_COUNT];
	uint32_t last;                     // last old logical row that is laid out
	uint32_t new_rows_total;           // rows the whole layout takes
} layout_t;

// Walk the paragraphs once. With `fresh` NULL it only counts and places the
// anchors; with a buffer it also lays the cells down, skipping `drop` rows
// off the top and stopping at `limit` rows.
static void walk(const tty_reflow_in_t *in, uint32_t nc, layout_t *lay,
                 tty_cell_t *fresh, uint32_t drop, uint32_t limit)
{
	uint32_t next_new = 0;             // new row the next paragraph starts on
	uint32_t p0 = 0;
	while (p0 <= lay->last) {
		uint32_t p1 = p0;
		while (p1 < lay->last && (old_row(in, p1 + 1)[0].attrs & TTY_ATTR_WRAPPED))
			p1++;

		// The paragraph as one run of cells: every row but the last counts
		// in full (it was full when it wrapped, and if something has since
		// blanked its tail the blanks still hold the rest in position).
		uint64_t span = (uint64_t)(p1 - p0 + 1) * in->cols;
		uint64_t length = (uint64_t)(p1 - p0) * in->cols + row_length(old_row(in, p1), in->cols);
		uint64_t need = length == 0 ? 1 : (length + nc - 1) / nc;

		// The cursor has to land on a row that exists, even where it sits
		// past the end of the text (a prompt's trailing space). Asked
		// BEFORE any anchor is placed, because the clamp below reads `need`:
		// an anchor clamped against a need the cursor is about to grow
		// lands on a row that is not its own, and for A_SCREEN that means
		// the live screen starting one row up, on a line that was history.
		anchor_t *cursor = &lay->anchor[A_CURSOR];
		if (!cursor->placed && cursor->row >= p0 && cursor->row <= p1) {
			uint64_t off = (uint64_t)(cursor->row - p0) * in->cols + cursor->col;
			if (off / nc + 1 > need)
				need = off / nc + 1;
		}

		for (int a = 0; a < A_COUNT; a++) {
			anchor_t *an = &lay->anchor[a];
			if (an->placed || an->row < p0 || an->row > p1)
				continue;
			uint64_t off = (uint64_t)(an->row - p0) * in->cols + an->col;
			uint64_t r = off / nc;
			// PAST THE PARAGRAPH'S TEXT lands AFTER the paragraph, not on
			// its last row. A blank tail is rows in the old shape and no
			// rows in the new one, so an anchor sitting in that tail is at
			// the boundary with whatever comes next — and for the screen's
			// anchor, pulling it back inside would put text it was below,
			// which is history, on the live screen. The cursor never
			// reaches here: its paragraph was grown to hold it.
			if (r > need) r = need;
			an->new_row = next_new + (uint32_t)r;
			an->new_col = (uint32_t)(off % nc);
			an->placed = true;
		}

		if (fresh != NULL) {
			for (uint64_t j = 0; j < need; j++) {
				uint64_t at = (uint64_t)next_new + j;
				if (at < drop || at - drop >= limit)
					continue;
				tty_cell_t *dst = fresh + (size_t)(at - drop) * nc;
				for (uint32_t x = 0; x < nc; x++) {
					uint64_t k = j * nc + x;
					if (k >= span)
						break;
					dst[x] = old_row(in, p0 + (uint32_t)(k / in->cols))[k % in->cols];
					dst[x].attrs &= (uint8_t)~TTY_ATTR_WRAPPED;
				}
				// The first row that survives is nobody's continuation,
				// whatever it was before the rows above it were dropped.
				if (j > 0 && at > drop)
					dst[0].attrs |= TTY_ATTR_WRAPPED;
			}
		}

		next_new += (uint32_t)need;
		p0 = p1 + 1;
	}
	lay->new_rows_total = next_new;

	// An anchor below the last laid-out row is on blank screen: it keeps its
	// distance from the text above it, and its column as far as that exists.
	for (int a = 0; a < A_COUNT; a++) {
		anchor_t *an = &lay->anchor[a];
		if (an->placed)
			continue;
		an->new_row = next_new + (an->row - lay->last - 1);
		an->new_col = an->col < nc ? an->col : nc - 1;
		an->placed = true;
	}
}

static bool reflow(const tty_reflow_in_t *in, uint32_t nc, uint32_t nr, uint32_t max_total,
                   tty_cell_t *fresh, uint32_t fresh_total, tty_reflow_out_t *out)
{
	if (in == NULL || out == NULL || in->cells == NULL ||
	    in->cols < 2 || in->rows < 2 || nc < 2 || nr < 2 ||
	    in->total_lines < in->rows || in->screen_top >= in->total_lines ||
	    in->hist_lines > in->total_lines - in->rows ||
	    in->view_offset > in->hist_lines ||
	    in->cur_row >= in->rows || in->cur_col >= in->cols ||
	    max_total < nr)
		return false;

	uint32_t logical = in->hist_lines + in->rows;
	uint32_t cursor = in->hist_lines + in->cur_row;

	layout_t lay = {0};
	// Blank screen under the last text is not content; laying it out would
	// push real lines into history to make room for nothing. The cursor's
	// row always counts, text or not.
	lay.last = logical - 1;
	while (lay.last > cursor && row_length(old_row(in, lay.last), in->cols) == 0)
		lay.last--;

	uint32_t saved_row = in->save_row < in->rows ? in->save_row : in->rows - 1;
	uint32_t saved_col = in->save_col < in->cols ? in->save_col : in->cols - 1;
	lay.anchor[A_SCREEN] = (anchor_t){.row = in->hist_lines, .col = 0};
	lay.anchor[A_CURSOR] = (anchor_t){.row = cursor, .col = in->cur_col};
	lay.anchor[A_SAVED]  = (anchor_t){.row = in->hist_lines + saved_row, .col = saved_col};
	lay.anchor[A_VIEW]   = (anchor_t){.row = in->hist_lines - in->view_offset, .col = 0};
	layout_t counted = lay;
	walk(in, nc, &counted, NULL, 0, 0);

	// WHERE THE SCREEN STARTS. Never above where it started before — that
	// would put history back on the live screen, under a cursor that knows
	// nothing of it. Otherwise as far down as it takes to show the last row
	// of text, which also brings the cursor on (it is never below the text);
	// but never so far that the cursor leaves by the top, which is the one
	// case that costs rows, and they are counted.
	uint32_t total = counted.new_rows_total;
	uint32_t cur = counted.anchor[A_CURSOR].new_row;
	uint32_t top = counted.anchor[A_SCREEN].new_row;
	if (total > nr && total - nr > top)
		top = total - nr < cur ? total - nr : cur;
	uint32_t clipped = total > top + nr ? total - (top + nr) : 0;

	uint32_t drop = top + nr > max_total ? top + nr - max_total : 0;
	uint32_t hist = top - drop;

	if (fresh != NULL) {
		if (fresh_total < hist + nr || fresh_total > max_total)
			return false;
		layout_t laid = lay;
		walk(in, nc, &laid, fresh, drop, hist + nr);
	}

	uint32_t saved = counted.anchor[A_SAVED].new_row;
	uint32_t view = counted.anchor[A_VIEW].new_row;
	*out = (tty_reflow_out_t){
		.lines_needed = hist + nr,
		.hist_lines = hist,
		// view_offset counts lines UP from the live screen; a view that was
		// on the live screen stays there, and one whose line was dropped
		// shows the oldest line there still is.
		.view_offset = in->view_offset == 0 || view >= top ? 0
		             : (top - view > hist ? hist : top - view),
		.cur_row = cur - top,
		.cur_col = counted.anchor[A_CURSOR].new_col,
		.save_row = saved < top ? 0 : (saved - top >= nr ? nr - 1 : saved - top),
		.save_col = counted.anchor[A_SAVED].new_col,
		.history_dropped = drop,
		.below_clipped = clipped,
	};
	return true;
}

bool tty_reflow_plan(const tty_reflow_in_t *in, uint32_t new_cols, uint32_t new_rows,
                     uint32_t max_total, tty_reflow_out_t *out)
{
	return reflow(in, new_cols, new_rows, max_total, NULL, 0, out);
}

bool tty_reflow_run(const tty_reflow_in_t *in, uint32_t new_cols, uint32_t new_rows,
                    uint32_t max_total, tty_cell_t *fresh, uint32_t fresh_total,
                    tty_reflow_out_t *out)
{
	if (fresh == NULL)
		return false;
	return reflow(in, new_cols, new_rows, max_total, fresh, fresh_total, out);
}
