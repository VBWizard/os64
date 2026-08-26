// gterm — the terminal window. The g era's xterm, and named for it: the
// convention (Chris's ruling, 2026-08-19) is that GUI apps wear a g the way
// X apps wore an x, so `ls /bin/g*` enumerates the desktop the way
// `ls /usr/bin/x*` once mapped a workstation.
//
// This is ptyprobe wearing pixels — the whole program is PTY.md's promised
// shape: GRID IN, KEYS OUT.
//
//   keys out: every KEY_DOWN with an ascii byte goes to the pty master.
//     That one rule carries more than it looks like: Ctrl+C arrives here as
//     0x03 (the keyboard's 1963-vintage translation), crosses to the master,
//     runs the SLAVE's intercept, and kills the program in the window —
//     never us. Arrow keys arrive as the VT100's ESC [ A burst and flow
//     through untouched, so husk's history works in a window because the
//     bytes never knew where they were.
//   grid in: poll the snapshot header at frame cadence (the ratified 30Hz;
//     the client-notification seam retires the poll when it lands), copy
//     cells only when the generation moved, repaint, publish. The kernel's
//     ONE terminal interpreter did all the hard work before we ever saw it.
//
// The window closes when the session ends — HUNGUP after `exit`, xterm's
// contract since 1984 — or when the window dies under us. No menu, no
// chrome: the app-driven doctrine says a menu widget arrives when some app
// truly needs one, and a terminal doesn't (xterm went decades; settings
// live in theme.conf's world, not in bars). The X-button's SIGHUP rides
// GRAPHICS #5, already booked in PTY.md with its modem etymology.
//
// Deliberately libdraw, not libui: a terminal is ONE full-bleed custom
// surface with no widgets in it — os64_ui buys dispatch and theming for
// widget TREES, and wrapping a single grid in one would be ceremony. The
// day gterm wants chrome (scrollbar, tabs), it adopts libui and becomes
// the custom-widget-class precedent. Geometry is honest the other way
// around too: the pty is sized FROM the window's real content area, so
// whatever the chrome costs, the grid fits exactly.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/pty.h"
#include "os64/proc.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/clip.h"

#define CELL_W 8
#define CELL_H 16
#define WANT_COLS 100u
#define WANT_ROWS 38u
#define MAX_CELLS 16384u          // 128KB of BSS; past it a resize keeps the old grid

#define GTERM_BG 0xff000000u      // the console's black, honored

static os64_pty_header_t gHdr;
static os64_pty_cell_t   gCells[MAX_CELLS];

// ── the selection (CLIPBOARD.md slice 3) ────────────────────────────────────
// SELECT IS COPY: releasing a drag publishes the highlighted text to
// /sys/clipboard, and right-click writes the clipboard to the master as if
// it had been typed. That pair is the oldest mouse idiom Unix has — gpm on
// the Linux console (Alessandro Rubini, 1994) and xterm before it — and it
// is the gesture Chris asked for by name.
//
// The selection covers the VISIBLE GRID only, because a grid is all a pty
// has: there is no scrollback to select into yet. When one arrives, the
// anchor/end pair below becomes a pair of positions in the scrollback's
// coordinates and everything else here stays as it is.
static bool     gSelLive;                    // is anything highlighted?
static bool     gDragging;                   // button 1 is down inside us
static uint32_t gAnchorRow, gAnchorCol;      // where the drag began
static uint32_t gEndRow, gEndCol;            // the cell under the pointer now

// Normalized selection bounds, INCLUSIVE at both ends (the cell you are over
// is part of the selection — xterm's feel, and the one that matches what the
// highlight shows).
static void sel_bounds(uint32_t *sr, uint32_t *sc, uint32_t *er, uint32_t *ec)
{
	bool forward = (gAnchorRow < gEndRow) ||
	               (gAnchorRow == gEndRow && gAnchorCol <= gEndCol);
	*sr = forward ? gAnchorRow : gEndRow;
	*sc = forward ? gAnchorCol : gEndCol;
	*er = forward ? gEndRow    : gAnchorRow;
	*ec = forward ? gEndCol    : gAnchorCol;
}

// The lit range on one row: [*from, *to] INCLUSIVE, false when this row has
// nothing lit.
//
// TRAILING BLANKS ARE EXCLUDED, and that is the point: selection_copy trims
// them, so a highlight that covered them would be a highlight promising bytes
// the clipboard is never going to receive. THE HIGHLIGHT SHOWS EXACTLY WHAT
// THE COPY WILL CONTAIN. (The pleasant side effect is that it hugs the text
// instead of running a white bar out to the right margin — xterm paints the
// bar; os64 would rather tell the truth twice than paint once.)
static bool row_highlight(uint32_t r, uint32_t *from, uint32_t *to)
{
	if (!gSelLive || r >= gHdr.rows || gHdr.cols == 0)
		return false;

	uint32_t sr, sc, er, ec;
	sel_bounds(&sr, &sc, &er, &ec);
	if (r < sr || r > er)
		return false;

	uint32_t f = (r == sr) ? sc : 0;
	uint32_t t = (r == er) ? ec : gHdr.cols - 1;
	if (t >= gHdr.cols)
		t = gHdr.cols - 1;
	if (f > t)
		return false;

	const os64_pty_cell_t *row = &gCells[r * gHdr.cols];
	while (t > f && (row[t].ch == 0 || row[t].ch == ' '))
		t--;
	if (row[t].ch == 0 || row[t].ch == ' ')
		return false;   // all blank: nothing shown, and nothing copied either

	*from = f;
	*to   = t;
	return true;
}

// Which cell is under a content-local point? Clamped, because a GRABBED drag
// legitimately reports coordinates outside the window (the compositor's
// implicit pointer grab, 2026-08-21) — dragging past the bottom edge should
// select to the last row, not compute a wild index.
static void cell_at(int32_t x, int32_t y, uint32_t *row, uint32_t *col)
{
	int32_t r = (y < 0) ? 0 : y / CELL_H;
	int32_t c = (x < 0) ? 0 : x / CELL_W;
	if (r >= (int32_t)gHdr.rows) r = (int32_t)gHdr.rows - 1;
	if (c >= (int32_t)gHdr.cols) c = (int32_t)gHdr.cols - 1;
	if (r < 0) r = 0;
	if (c < 0) c = 0;
	*row = (uint32_t)r;
	*col = (uint32_t)c;
}

static void render(os64_draw_ctx_t *ctx)
{
	// Repaint from state, whole grid: the damage list downstream is what
	// keeps this cheap on the glass, and a grid's worth of RAM writes is
	// the same order as the composite that follows.
	os64_gui_rect_t all = {0, 0, (int32_t)ctx->surf.width,
	                       (int32_t)ctx->surf.height};
	os64_draw_fill_rect(&ctx->surf, all, GTERM_BG);

	for (uint32_t r = 0; r < gHdr.rows; r++)
	{
		const os64_pty_cell_t *row = &gCells[r * gHdr.cols];
		// Batch runs of same-colored glyphs into single text calls: a
		// mostly-monochrome row (the usual) draws in one or two runs.
		// One trim per ROW, not per cell — the range is the same for every
		// cell in it, and asking per cell would be O(cols^2) for a picture
		// that never changes within the row.
		uint32_t hf = 0, ht = 0;
		bool hl = row_highlight(r, &hf, &ht);
		#define SELECTED(col) (hl && (col) >= hf && (col) <= ht)

		char run[512];
		uint32_t c = 0;
		while (c < gHdr.cols)
		{
			uint32_t color = row[c].color;
			// Selection breaks a run exactly like a color change does — it IS
			// a color change, just one the grid doesn't store. Highlighting
			// therefore costs no extra pass over the row.
			bool sel = SELECTED(c);
			uint32_t start = c, n = 0;
			while (c < gHdr.cols && row[c].color == color &&
			       SELECTED(c) == sel && n < sizeof(run))
			{
				char ch = row[c].ch;
				run[n++] = (ch >= ' ') ? ch : ' ';
				c++;
			}
			// Inverse video for the highlight: the oldest "this is selected"
			// signal there is, and it needs no theme, no second color, and no
			// agreement with whatever the program inside chose to paint.
			os64_draw_text(&ctx->surf, (int32_t)(start * CELL_W),
			               (int32_t)(r * CELL_H), run, n,
			               sel ? GTERM_BG : color,
			               sel ? color : GTERM_BG);
		}
		#undef SELECTED
	}

	// The cursor: a filled cell, the block style the glass console wears.
	// The grid doesn't contain it (the kernel's cursor is renderer state,
	// not cell state), so the terminal paints its own — every terminal
	// emulator ever written rediscovers this on day one.
	if (gHdr.cur_row < gHdr.rows && gHdr.cur_col < gHdr.cols)
	{
		os64_gui_rect_t cur = {(int32_t)(gHdr.cur_col * CELL_W),
		                       (int32_t)(gHdr.cur_row * CELL_H),
		                       CELL_W, CELL_H};
		os64_draw_fill_rect(&ctx->surf, cur, 0xffc0c0c0u);
	}

	os64_draw_publish(ctx, (const os64_gui_rect_t *)0);
}

// ── select is copy ──────────────────────────────────────────────────────────
// Write the highlighted cells to /sys/clipboard. Streams straight into the
// open handle a row at a time; the clipboard seals the whole thing as ONE
// snarf when we close it, which is what makes a multi-row copy one copy.
static void selection_copy(void)
{
	if (!gSelLive)
		return;

	uint32_t sr, sc, er, ec;
	sel_bounds(&sr, &sc, &er, &ec);

	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "w");
	if (h < 0)
		return;

	char line[512];
	for (uint32_t r = sr; r <= er && r < gHdr.rows; r++)
	{
		const os64_pty_cell_t *row = &gCells[r * gHdr.cols];
		uint32_t from = (r == sr) ? sc : 0;
		uint32_t to   = (r == er) ? ec : gHdr.cols - 1;
		uint32_t n = 0;

		for (uint32_t c = from; c <= to && c < gHdr.cols && n < sizeof(line); c++)
		{
			char ch = row[c].ch;
			line[n++] = (ch >= ' ') ? ch : ' ';
		}
		// TRAILING BLANKS ARE NOT TEXT. A terminal row is padded out to the
		// full width with spaces nobody typed, and copying them would make
		// every line of every copy 100 columns wide. xterm has trimmed since
		// the 80s; so do we. (A wrapped long line still copies as TWO lines,
		// because the cell carries a glyph and a color and no wrap bit — a
		// limitation worth naming rather than a bit worth inventing today.)
		while (n > 0 && line[n - 1] == ' ')
			n--;

		if (n > 0 && os64_write((int32_t)h, line, n) < 0)
			break;
		if (r < er && os64_write((int32_t)h, "\n", 1) < 0)
			break;
	}

	os64_close((int32_t)h);   // the seal
}

// ── right-click pastes ──────────────────────────────────────────────────────
// Straight into the pty master, as if typed — Chris's ruling, 2026-08-21,
// with the newline question asked and answered: a pasted newline RUNS the
// line, exactly as pressing Enter would. Bracketed paste (xterm's ESC[200~
// wrapper, 2002) is declined on purpose: "If I want to run multiple lines I
// copy multiple lines." The alternative was teaching husk a mode so it could
// second-guess its own user.
//
// A CR in the snarf is dropped, alone or as half a CRLF: os64's keyboard
// emits '\n' for Enter and never a CR, so a carriage return in there came
// from a file format, not from a keystroke. Same rule scribe's paste uses.
//
// THE PASTE DRIBBLES (2026-08-22). The slave's input ring holds 127 events
// and a snarf can be a whole boot log, so the master's write may stop SHORT
// — it returns how many bytes it took, pipe-style — and the rest has to
// wait for husk to read the ring down. So a paste is a small state machine
// stepped once per frame, not a loop in one frame: the clipboard handle
// stays open (it pins ONE snarf for its whole life, so a copy elsewhere
// mid-paste cannot change what we are feeding in), and each frame pushes as
// much as the ring will take. The first version wrote the whole thing in one
// call and the kernel reported success while dropping everything past the
// first 127 bytes — which is exactly what Chris saw when a long paste
// "stopped accepting bytes". NEVER DROP A BYTE; take longer instead. The
// text console's paste (vt_select.c) is the same machine on the other side
// of the glass.
static int64_t  gPasteHandle = -1;       // the open snarf, -1 = no paste in flight
static char     gPasteChunk[512];
static uint32_t gPasteLen, gPastePos;    // the chunk's CR-stripped bytes and how far they went

static void paste_end(void)
{
	if (gPasteHandle >= 0)
		os64_close((int32_t)gPasteHandle);
	gPasteHandle = -1;
	gPasteLen = gPastePos = 0;
}

// Right-click: start a paste. A second right-click while one is still
// dribbling REPLACES it — last gesture wins, the only honest policy for a
// pointer (and the same one vt_select.c keeps).
static void paste_begin(void)
{
	paste_end();
	gPasteHandle = os64_open(OS64_CLIPBOARD_PATH, "r");
}

// One frame's worth: feed the master until it refuses or the snarf is done.
static void paste_step(int32_t master)
{
	while (gPasteHandle >= 0)
	{
		if (gPastePos >= gPasteLen)
		{
			// Chunk spent: read the next, dropping CRs as it comes in.
			int64_t n = os64_read((int32_t)gPasteHandle, gPasteChunk, sizeof(gPasteChunk));
			if (n <= 0)
			{
				paste_end();    // EOF (or a read error, which ends it the same way)
				return;
			}
			uint32_t keep = 0;
			for (int64_t i = 0; i < n; i++)
				if (gPasteChunk[i] != '\r')
					gPasteChunk[keep++] = gPasteChunk[i];
			gPasteLen = keep;
			gPastePos = 0;
			continue;       // an all-CR chunk just reads again
		}

		int64_t took = os64_write(master, gPasteChunk + gPastePos, gPasteLen - gPastePos);
		if (took < 0)
		{
			paste_end();    // the master is gone; nothing left to paste into
			return;
		}
		gPastePos += (uint32_t)took;
		if (gPastePos < gPasteLen)
			return;         // ring full: the rest goes next frame
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// Ask for chrome + a WANT_COLS x WANT_ROWS content area (100x38 since
	// Chris's first day driving it — top with no wrapping is uber important);
	// then believe the SURFACE we actually got and size the pty from it —
	// the grid fits the window by construction, whatever the decorations
	// cost this month. Window geometry persistence (/home/.config) is a
	// wished-for future; until then the constants ARE the config.
	int64_t win = os64_gui_window_create("gterm", 140, 120,
	                                     WANT_COLS * CELL_W + 8,
	                                     WANT_ROWS * CELL_H + 24, 0);
	if (win <= 0)
	{
		os64_printf("gterm: no GUI here (window_create %ld)\n", (long)win);
		return 1;
	}

	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_printf("gterm: get_surface failed\n");
		os64_gui_window_destroy(win);
		return 1;
	}

	uint32_t cols = ctx.surf.width / CELL_W;
	uint32_t rows = ctx.surf.height / CELL_H;
	if (cols * rows > MAX_CELLS)
	{
		os64_printf("gterm: window absurdly large (%ux%u cells)\n", cols, rows);
		os64_gui_window_destroy(win);
		return 1;
	}

	int64_t master = os64_pty_create(cols, rows);
	if (master < 0)
	{
		os64_printf("gterm: pty_create failed (%ld)\n", (long)master);
		os64_gui_window_destroy(win);
		return 1;
	}

	int64_t shell = os64_spawn_seated("/bin/husk",
	                                  (char *[]){ "/bin/husk", 0 }, master);
	if (shell <= 0)
	{
		os64_printf("gterm: husk would not seat (%ld)\n", (long)shell);
		os64_close((int32_t)master);
		os64_gui_window_destroy(win);
		return 1;
	}

	os64_frame_clock_t clock;
	os64_frame_clock_init(&clock);
	uint64_t rendered_gen = ~(uint64_t)0;
	bool window_alive = true;

	for (;;)
	{
		// Keys out. Only presses, only bytes: releases and chord-consumed
		// keys never reach us, and a key with no ascii has nothing to say
		// to a byte stream (until STREAM mode teaches us better).
		//
		// Mouse in, since 2026-08-21: drag-select, and right-click paste.
		// The compositor's implicit pointer grab is what makes the drag
		// reliable — a press inside our content area routes every move and
		// the release to US, even when the pointer leaves the window.
		os64_gui_event_t ev;
		int64_t erc;
		bool repaint = false;   // the selection changed; the grid may not have
		while ((erc = os64_gui_event_poll(win, &ev)) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii != 0)
			{
				// Typing means you are done looking: the highlight dies on
				// any keystroke (his ruling). Keeping it lit while the screen
				// fills underneath would be a highlight that no longer names
				// the text you selected.
				if (gSelLive)
				{
					gSelLive = false;
					repaint = true;
				}
				os64_write((int32_t)master, &ev.key.ascii, 1);
			}
			else if (ev.type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN)
			{
				if (ev.mouse.button == OS64_GUI_MOUSE_LEFT)
				{
					// A press anchors but lights nothing: a plain click must
					// never touch the clipboard, only a DRAG does.
					cell_at(ev.mouse.x, ev.mouse.y, &gAnchorRow, &gAnchorCol);
					gEndRow = gAnchorRow;
					gEndCol = gAnchorCol;
					gDragging = true;
					if (gSelLive)
					{
						gSelLive = false;   // a new gesture clears the old one
						repaint = true;
					}
				}
				else if (ev.mouse.button == OS64_GUI_MOUSE_RIGHT)
				{
					paste_begin();   // fed in by paste_step, a frame at a time
				}
			}
			else if (ev.type == OS64_GUI_EVENT_MOUSE_MOVE && gDragging)
			{
				uint32_t r, c;
				cell_at(ev.mouse.x, ev.mouse.y, &r, &c);
				if (r != gEndRow || c != gEndCol || !gSelLive)
				{
					gEndRow = r;
					gEndCol = c;
					// Live only once the pointer has actually moved off the
					// anchor cell — one cell is a click, not a selection.
					gSelLive = (r != gAnchorRow || c != gAnchorCol);
					repaint = true;
				}
			}
			else if (ev.type == OS64_GUI_EVENT_MOUSE_BUTTON_UP)
			{
				if (ev.mouse.button == OS64_GUI_MOUSE_LEFT && gDragging)
				{
					gDragging = false;
					selection_copy();   // SELECT IS COPY — the release publishes
				}
			}
			else if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE)
			{
				// The WINDOW resized; the GRID follows (PTY.md § Resize).
				// Hop one, WM -> us, is this event. Hop two, us -> the
				// program inside, is pty_resize: the kernel reallocates the
				// grid, carries the text, and raises SIGWINCH at every task
				// seated on the slave — husk, and whatever it is running.
				// We are the master and the master owns the geometry; the
				// snapshot header reports the new size from the next poll,
				// so render() picks it up without being told.
				os64_draw_ctx_refresh(&ctx);
				uint32_t ncols = ctx.surf.width / CELL_W;
				uint32_t nrows = ctx.surf.height / CELL_H;
				if (ncols >= 2 && nrows >= 2 && ncols * nrows <= MAX_CELLS &&
				    (ncols != cols || nrows != rows))
				{
					if (os64_pty_resize(master, ncols, nrows) == 0)
					{
						cols = ncols;
						rows = nrows;
						gSelLive = false;   // the cells a highlight named just moved
					}
					// A refused resize (the kernel's geometry fence — its only refusal)
					// leaves the old grid: letterboxed or clipped, but honest
					// about its size to the program inside.
				}
				rendered_gen = ~(uint64_t)0;   // force a repaint next pass
			}
		}
		if (erc < 0)
		{
			window_alive = false;   // the window died under us (swept?)
			break;
		}

		// A paste in flight gets this frame's share of the ring — before
		// the snapshot, so whatever husk echoes back lands in this frame's
		// picture rather than the next one's.
		paste_step((int32_t)master);

		// Grid in — header first, cells only when the generation moved.
		if (os64_pty_snapshot(master, &gHdr, (os64_pty_cell_t *)0, 0) < 0)
			break;
		if (gHdr.flags & OS64_PTY_HUNGUP)
			break;                  // the session ended: exit closes the window
		if (gHdr.generation != rendered_gen)
		{
			if (os64_pty_snapshot(master, &gHdr, gCells, cols * rows) < 0)
				break;
			// The text moved underneath. A highlight is a claim about WHICH
			// TEXT you have, and those coordinates now name something else —
			// so it goes out rather than lying. (The drag itself survives: if
			// a button is still down, the next move re-lights the selection
			// against what is on the screen now, which is what the eye is
			// following anyway.)
			gSelLive = false;
			render(&ctx);
			rendered_gen = gHdr.generation;
		}
		else if (repaint)
		{
			render(&ctx);   // selection-only change: gCells is still current
		}

		os64_frame_wait(&clock, 33);   // the ratified ~30Hz poll
	}

	paste_end();                       // a paste cut off by the hangup lets the snarf go
	os64_close((int32_t)master);       // hangup: the slave orphans benignly
	if (window_alive)
		os64_gui_window_destroy(win);
	return 0;
}
