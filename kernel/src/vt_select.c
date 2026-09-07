// vt_select.c — mouse selection on a text terminal. CLIPBOARD.md's slice 4,
// and the oldest gesture in this whole arc: highlight to copy, right-click to
// paste, on a console made of character cells. See vt_select.h for the
// lineage and for why this is NOT a daemon.
//
// THE SHAPE, in one paragraph. The compositor thread already drains the one
// input ring every frame, whether or not it owns the glass — so the fork is a
// routing decision, not a second input path. Events arrive here under
// kGuiLock and do NOTHING but move state (vtsel_mouse_event). Once per frame,
// with that lock released, vtsel_paint() does everything that touches the
// world: it paints the overlay, performs a pending copy, and dribbles a
// pending paste into the terminal's input ring. That split is deliberate —
// painting needs the tty and renderer locks, and reaching those while holding
// the compositor's would invent a lock order nothing else in the system has.
//
// THE OVERLAY IS A LIE ABOUT SOME CELLS, and it knows it. The terminal owns
// its grid; we paint inverse video over a few cells and remember which rows we
// touched, so the next frame can repaint those rows from the grid and put the
// truth back. If the terminal repainted itself in the meantime (its
// generation moved, or the scrollback view scrolled), our lie was already
// overwritten by the truth — so we drop the selection instead of restoring
// stale glyphs over new output. A highlight is a claim about WHICH TEXT you
// have; the moment the text moves, the claim is false and it goes.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vt_select.h"
#include "tty.h"
#include "clipboard.h"
#include "BasicRenderer.h"
#include "driver/system/keyboard.h"
#include "memory/kmalloc.h"
#include "spinlock.h"
#include "serial_logging.h"
#include "CONFIG.h"

// ── State ───────────────────────────────────────────────────────────────────
// Single-threaded by construction: every function here runs on the compositor
// thread and nowhere else. No lock of its own, on purpose — a lock would
// suggest a second caller exists.

static tty_t   *s_tty;            // the terminal the overlay belongs to
static uint64_t s_gen;            // its generation when we last painted
static uint32_t s_view;           // its scrollback offset when we last painted

static bool     s_have_ptr;       // is there a pointer cell to draw?
static uint32_t s_ptr_row, s_ptr_col;

static bool     s_dragging;       // button 1 is down
static bool     s_sel;            // ...and it has moved: a selection exists
static uint32_t s_anchor_row, s_anchor_col;
static uint32_t s_end_row, s_end_col;

static bool     s_dirty;          // the overlay needs repainting
static bool     s_painted;        // we currently have rows overlaid
static uint32_t s_paint_r0, s_paint_r1;   // ...this inclusive row range

static bool     s_copy_pending;   // a release asked for a copy

// A paste in flight. The entry is REFERENCE-HELD for as long as we are
// feeding it in, which is the refcounted-entry design earning its keep: the
// console's input ring is small (a paste bigger than it must arrive in
// installments), and somebody copying something else meanwhile cannot pull
// the bytes out from under us. Never drop a byte — dribble instead.
static snarf_entry_t *s_paste;
static size_t         s_paste_pos;

// ── Geometry ────────────────────────────────────────────────────────────────

static bool cell_at(const tty_t *t, int32_t x, int32_t y,
                    uint32_t *row, uint32_t *col)
{
	if (t == NULL || t->rows == 0 || t->cols == 0)
		return false;

	int32_t r = (y < 0) ? 0 : y / (int32_t)FONT_HEIGHT;
	int32_t c = (x < 0) ? 0 : x / (int32_t)FONT_WIDTH;
	if (r >= (int32_t)t->rows) r = (int32_t)t->rows - 1;
	if (c >= (int32_t)t->cols) c = (int32_t)t->cols - 1;

	*row = (uint32_t)r;
	*col = (uint32_t)c;
	return true;
}

// Normalized selection bounds, INCLUSIVE at both ends — the cell under the
// pointer is part of the selection, which is what the eye expects and what
// the highlight shows.
static void sel_bounds(uint32_t *sr, uint32_t *sc, uint32_t *er, uint32_t *ec)
{
	bool forward = (s_anchor_row < s_end_row) ||
	               (s_anchor_row == s_end_row && s_anchor_col <= s_end_col);
	*sr = forward ? s_anchor_row : s_end_row;
	*sc = forward ? s_anchor_col : s_end_col;
	*er = forward ? s_end_row    : s_anchor_row;
	*ec = forward ? s_end_col    : s_anchor_col;
}

// The lit range on one row: [*from, *to] inclusive, false if nothing is lit
// there. TRAILING BLANKS ARE EXCLUDED, exactly as the copy excludes them —
// the highlight shows what the clipboard will receive, not one cell more.
// (Same rule gterm follows; the two consumers agreeing is not a coincidence,
// it is the point.)
static bool row_highlight(const tty_t *t, uint32_t r,
                          const tty_cell_t *line, uint32_t *from, uint32_t *to)
{
	if (!s_sel || line == NULL)
		return false;

	uint32_t sr, sc, er, ec;
	sel_bounds(&sr, &sc, &er, &ec);
	if (r < sr || r > er)
		return false;

	uint32_t f = (r == sr) ? sc : 0;
	uint32_t tt = (r == er) ? ec : t->cols - 1;
	if (tt >= t->cols)
		tt = t->cols - 1;
	if (f > tt)
		return false;

	while (tt > f && (line[tt].ch == 0 || line[tt].ch == ' '))
		tt--;
	if (line[tt].ch == 0 || line[tt].ch == ' ')
		return false;

	*from = f;
	*to   = tt;
	return true;
}

// ── The event side: state only, nothing painted, nothing allocated ─────────

void vtsel_mouse_event(const input_event_t *ev)
{
	tty_t *t = kTTYFocused;

	// A pty is never focused (tty.h) and a dormant VT still has a grid, so
	// the only real gate is "is there a terminal on the glass at all".
	if (t == NULL || t->is_pty)
		return;

	// Focus moved since our last event: whatever we drew is gone with the
	// switch's repaint, and the old coordinates mean nothing here.
	if (t != s_tty)
		vtsel_forget();

	uint32_t row, col;
	if (!cell_at(t, ev->mouse.x, ev->mouse.y, &row, &col))
		return;

	switch (ev->type) {
	case INPUT_EVENT_MOUSE_MOVE:
		if (!s_have_ptr || row != s_ptr_row || col != s_ptr_col) {
			s_ptr_row = row;
			s_ptr_col = col;
			s_have_ptr = true;
			s_dirty = true;
		}
		if (s_dragging && (row != s_end_row || col != s_end_col)) {
			s_end_row = row;
			s_end_col = col;
			// One cell is a click, not a selection.
			s_sel = (row != s_anchor_row || col != s_anchor_col);
			s_dirty = true;
		}
		break;

	case INPUT_EVENT_MOUSE_BUTTON_DOWN:
		if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
			s_anchor_row = s_end_row = row;
			s_anchor_col = s_end_col = col;
			s_dragging = true;
			s_sel = false;          // a press lights nothing; a DRAG does
			s_dirty = true;
		} else if (ev->mouse.button == INPUT_MOUSE_BUTTON_RIGHT) {
			// Take the reference now, feed it in from the paint pass. A
			// second right-click while one paste is still dribbling replaces
			// it — last gesture wins, the only honest policy for a pointer.
			if (s_paste != NULL)
				clipboard_release(s_paste);
			s_paste = clipboard_acquire();
			s_paste_pos = 0;
		}
		break;

	case INPUT_EVENT_MOUSE_BUTTON_UP:
		if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT && s_dragging) {
			s_dragging = false;
			if (s_sel)
				s_copy_pending = true;   // SELECT IS COPY, done at paint time
		}
		break;

	default:
		break;
	}

	s_tty = t;
}

// ── The copy ────────────────────────────────────────────────────────────────
// Runs with no lock held on entry. Gathers the text into a scratch buffer
// under the tty's lock, then hands it to the clipboard OUTSIDE that lock —
// clipboard_append allocates, and allocating under a lock a print path can
// also take is how a deadlock gets built (MEMORY.md's rule).

static void do_copy(tty_t *t)
{
	uint32_t sr, sc, er, ec;
	sel_bounds(&sr, &sc, &er, &ec);
	if (er >= t->rows)
		er = t->rows - 1;

	// Worst case: every selected row full, plus a newline each.
	size_t cap = (size_t)(er - sr + 1) * ((size_t)t->cols + 1);
	char *text = (char *)kmalloc(cap);
	if (text == NULL) {
		printd(DEBUG_CLIPBOARD, "CLIPBOARD: no memory for a %lu byte console copy\n",
		       (unsigned long)cap);
		return;
	}

	size_t len = 0;
	uint64_t flags = spinlock_acquire_irqsave(&t->lock);
	for (uint32_t r = sr; r <= er; r++) {
		const tty_cell_t *line = tty_visible_line(t, r);
		if (line == NULL)
			continue;

		uint32_t from = (r == sr) ? sc : 0;
		uint32_t to   = (r == er) ? ec : t->cols - 1;
		if (to >= t->cols)
			to = t->cols - 1;

		size_t start = len;
		for (uint32_t c = from; c <= to && len < cap; c++) {
			char ch = line[c].ch;
			text[len++] = (ch >= ' ') ? ch : ' ';
		}
		// Trailing blanks are padding, not text — a console row is padded to
		// the full width with spaces nobody typed.
		while (len > start && text[len - 1] == ' ')
			len--;

		if (r < er && len < cap)
			text[len++] = '\n';
	}
	spinlock_release_irqrestore(&t->lock, flags);

	snarf_pending_t *p = clipboard_begin();
	if (p != NULL) {
		clipboard_append(p, text, len);
		clipboard_seal(p);
		printd(DEBUG_CLIPBOARD, "CLIPBOARD: console selection, %lu bytes from tty%u\n",
		       (unsigned long)len, (unsigned)t->index + 1);
	}
	kfree(text);
}

// ── The paste ───────────────────────────────────────────────────────────────
// Bytes go into the terminal's input ring as if typed — the same door the
// keyboard uses, so husk reads them without knowing a mouse exists. The ring
// is small and a paste can be large, so we feed it as far as it will go and
// come back next frame: NEVER DROP A BYTE, just take longer. (Chris found the
// other end of this experimentally on 2026-08-21, right-clicking into gterm
// until husk's command line stopped accepting bytes.)

static void do_paste_step(tty_t *t)
{
	if (s_paste == NULL)
		return;

	while (s_paste_pos < s_paste->length) {
		char byte = (char)s_paste->bytes[s_paste_pos];

		// A CR is dropped, alone or as half a CRLF: os64's keyboard emits
		// '\n' for Enter and never a CR, so a carriage return in the snarf
		// came from a file format, not from a keystroke.
		if (byte == '\r') {
			s_paste_pos++;
			continue;
		}

		keyboard_event_t ev = { .ascii = byte, .scancode = 0,
		                        .shift = false, .ctrl = false, .alt = false };
		if (!tty_input_push_if_room(t, &ev))
			return;      // ring full: the rest arrives next frame
		s_paste_pos++;
	}

	printd(DEBUG_CLIPBOARD, "CLIPBOARD: pasted %lu bytes into tty%u\n",
	       (unsigned long)s_paste->length, (unsigned)t->index + 1);
	clipboard_release(s_paste);
	s_paste = NULL;
	s_paste_pos = 0;
}

// ── The paint ───────────────────────────────────────────────────────────────

// Repaint one row from the grid, applying the overlay where it falls.
static void paint_row_locked(tty_t *t, uint32_t r)
{
	const tty_cell_t *line = tty_visible_line(t, r);
	if (line == NULL)
		return;

	uint32_t hf = 0, ht = 0;
	bool hl = row_highlight(t, r, line, &hf, &ht);
	bool ptr_here = s_have_ptr && s_ptr_row == r;

	for (uint32_t c = 0; c < t->cols; c++) {
		char ch = line[c].ch ? line[c].ch : ' ';
		uint32_t fg, bg;
		tty_cell_colors(t, &line[c], &fg, &bg);
		bool inverse = (hl && c >= hf && c <= ht) ||
		               (ptr_here && c == s_ptr_col);

		// Inverse video, and nothing else: no cursor bitmap, no theme, no
		// second color to agree on. This IS the text-mode mouse pointer —
		// gpm drew exactly this on the Linux console, and it works on any
		// glass that can paint a character.
		if (inverse)
			renderer_glass_putc_bg_locked(ch, r, c, bg, fg);
		else
			renderer_glass_putc_bg_locked(ch, r, c, fg, bg);
	}
}

void vtsel_paint(void)
{
	tty_t *t = kTTYFocused;

	if (t == NULL || t->is_pty) {
		vtsel_forget();
		return;
	}
	if (t != s_tty && (s_painted || s_have_ptr))
		vtsel_forget();

	// The work that is not painting, first — a copy has to read the grid
	// before the next frame of output can move it. And if output ALREADY
	// moved it (the generation or the scrollback view changed since the
	// highlight was drawn), the copy is skipped: the lit cells were a claim
	// about text that is no longer there, and the paint below is about to
	// drop the selection for exactly that reason — copying first would hand
	// the clipboard whatever scrolled into those coordinates in the 33ms
	// between the release and this frame. Same test, same verdict, both
	// halves agree.
	if (s_copy_pending) {
		s_copy_pending = false;
		bool moved = (t != s_tty) || (t->generation != s_gen) ||
		             (t->view_offset != s_view);
		if (!moved)
			do_copy(t);
		else
			printd(DEBUG_CLIPBOARD, "CLIPBOARD: console selection dropped — "
			       "the text moved before the copy\n");
	}
	if (s_paste != NULL)
		do_paste_step(t);

	// Two reasons to paint: our state changed, or the terminal repainted over
	// us (output flowed, or the scrollback view moved) and owes the overlay a
	// redraw. If neither, what is on the glass is already right — a pointer
	// sitting still costs nothing per frame.
	bool clobbered = (t != s_tty) || (t->generation != s_gen) ||
	                 (t->view_offset != s_view);
	if (!s_dirty && !clobbered)
		return;

	uint64_t tflags = spinlock_acquire_irqsave(&t->lock);

	// Re-read under the lock: the check above was a hint, this is the fact.
	// If the terminal repainted itself, our overlay is already gone — painted
	// over by the truth — and the selection's coordinates now name text that
	// has moved. Drop it and do not "restore" stale glyphs.
	clobbered = (t != s_tty) || (t->generation != s_gen) ||
	            (t->view_offset != s_view);
	if (clobbered && s_painted) {
		s_painted = false;
		if (s_sel || s_dragging) {
			s_sel = false;
			s_dragging = false;
		}
	}

	// The rows to touch: everything we overlaid last time (to put it back)
	// plus everything we are overlaying now.
	uint32_t r0 = 0, r1 = 0;
	bool any = false;

	if (s_painted) {
		r0 = s_paint_r0; r1 = s_paint_r1; any = true;
	}
	if (s_have_ptr) {
		uint32_t a = s_ptr_row, b = s_ptr_row;
		if (s_sel) {
			uint32_t sr, sc, er, ec;
			sel_bounds(&sr, &sc, &er, &ec);
			if (sr < a) a = sr;
			if (er > b) b = er;
		}
		if (!any) { r0 = a; r1 = b; any = true; }
		else { if (a < r0) r0 = a; if (b > r1) r1 = b; }
	} else if (s_sel) {
		uint32_t sr, sc, er, ec;
		sel_bounds(&sr, &sc, &er, &ec);
		if (!any) { r0 = sr; r1 = er; any = true; }
		else { if (sr < r0) r0 = sr; if (er > r1) r1 = er; }
	}

	if (any) {
		if (r1 >= t->rows)
			r1 = t->rows - 1;

		uint64_t rflags = renderer_glass_begin();
		for (uint32_t r = r0; r <= r1; r++)
			paint_row_locked(t, r);

		// Relight the listening light on exactly the terms tty_repaint uses:
		// a parked reader, the live screen, a terminal with someone seated.
		bool show = (t->waiter != NULL && t->view_offset == 0 &&
		             t->state == TTY_LIVE);
		renderer_glass_end(rflags, t->cur_row, t->cur_col, show);
	}

	// Remember what we now own, so the next frame can put it back.
	s_painted = s_have_ptr || s_sel;
	if (s_painted) {
		s_paint_r0 = r0;
		s_paint_r1 = r1;
	}
	s_tty  = t;
	s_gen  = t->generation;
	s_view = t->view_offset;
	s_dirty = false;

	spinlock_release_irqrestore(&t->lock, tflags);
}

void vtsel_forget(void)
{
	// No un-painting: every caller of this is a moment when the glass is
	// being repainted from state anyway (a VT switch, the GUI taking the
	// iron). Trying to restore cells here would race that repaint.
	s_painted = false;
	s_have_ptr = false;
	s_dragging = false;
	s_sel = false;
	s_dirty = false;
	s_copy_pending = false;
	s_tty = NULL;

	if (s_paste != NULL) {
		clipboard_release(s_paste);
		s_paste = NULL;
		s_paste_pos = 0;
	}
}
