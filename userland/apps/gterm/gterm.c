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
// around too: the pty is sized from the actual content area and selected
// cell metrics. A refused resize leaves the old grid clipped or letterboxed.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/pty.h"
#include "os64/proc.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/clip.h"
#include "os64/mem.h"
#include "font_grid.h"
#include "os64/font_settings.h"

// Prefer a grid wide enough for top without wrapping its usual columns.
// Pixel dimensions follow the selected font and are capped to the screen.
#define WANT_COLS 100u
#define WANT_ROWS 38u
#define MAX_CELLS GTERM_MAX_CELLS

#define GTERM_BG 0xff000000u      // the console's black, honored

static os64_pty_header_t gHdr;
/* Header probes and incomplete reads must not overwrite the displayed grid. */
static os64_pty_cell_t gCellStorage[2][MAX_CELLS];
static os64_pty_cell_t *gCells = gCellStorage[0];
static bool gSnapshotValid;
static uint64_t gRenderedGen = UINT64_MAX;
static int64_t gMaster = -1;
static gterm_grid_t gGrid;
static uint64_t gFontGeneration;
static bool gFontSettingsReady;
static os64_text_context_t *gText;

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

static void *font_allocate(void *user, size_t bytes)
{ (void)user; return os64_malloc(bytes); }
static void font_release(void *user, void *ptr, size_t bytes)
{ (void)user; (void)bytes; os64_free(ptr); }

static int64_t resize_pty(void *user, uint32_t cols, uint32_t rows)
{
	// Resize travels WM -> gterm -> PTY, which sends SIGWINCH to slave
	// listeners so applications can reflow. See PTY.md.
	(void)user;
	return gMaster < 0 ? 0 : os64_pty_resize(gMaster, cols, rows);
}
static void invalidate_grid(void *user)
{
	(void)user;
	gSelLive = gDragging = gSnapshotValid = false;
	gRenderedGen = UINT64_MAX;
}

/* Both shared settings and the guest fixture use the grid transaction. */
static os64_font_status_t replace_fonts(os64_font_set_t *candidate)
{
	os64_font_consumer_t consumer = gterm_grid_consumer(&gGrid);
	return os64_font_adopt(candidate, &consumer, 1, NULL);
}

static void refresh_font_settings(void)
{
	os64_font_config_t config;
	uint64_t generation;
	if (os64_font_settings_current(&config, &generation) != 0) return;
	if (gFontSettingsReady && gFontGeneration == generation) return;
	os64_font_set_t *candidate = NULL;
	os64_font_config_error_t error;
	if (os64_font_config_prepare(gText, &config, &candidate, &error)) {
		char line[256];
		os64_snprintf(line, sizeof(line), "gterm: font line %lu: %s; keeping current grid",
					(unsigned long)error.line, os64_font_config_status_name(error.status));
		os64_debug_log(line);
		return;
	}
	os64_font_status_t status = replace_fonts(candidate);
	os64_font_set_release(candidate);
	if (!status) { gFontSettingsReady = true; gFontGeneration = generation; }
	else {
		char line[128];
		os64_snprintf(line, sizeof(line),
					  "gterm: font/grid change refused (%u); keeping current grid", status);
		os64_debug_log(line);
	}
}

static void cell_at(int32_t x, int32_t y, uint32_t *row, uint32_t *col)
{ gterm_grid_cell_at(&gGrid, x, y, row, col); }

static void render(os64_draw_ctx_t *ctx)
{
	if (!gSnapshotValid) return;
	os64_gui_rect_t all = {0, 0, (int32_t)ctx->surf.width, (int32_t)ctx->surf.height};
	os64_draw_fill_rect(&ctx->surf, all, GTERM_BG);
	for (uint32_t r = 0; r < gHdr.rows; r++) {
		uint32_t hf = 0, ht = 0;
		bool hl = row_highlight(r, &hf, &ht);
		for (uint32_t c = 0; c < gHdr.cols; c++) {
			const os64_pty_cell_t *cell = &gCells[r * gHdr.cols + c];
			uint32_t fg = cell->color, bg = os64_ansi_bg_color(cell->bg, GTERM_BG);
			os64_ansi_apply_attrs(cell->attrs, &fg, &bg);
			if (hl && c >= hf && c <= ht) { uint32_t swap = fg; fg = bg; bg = swap; }
			os64_draw_fill_rect(&ctx->surf, gterm_grid_cell_rect(&gGrid, r, c), bg);
			gterm_grid_draw_cell(&gGrid, &ctx->surf, r, c, (uint8_t)cell->ch, cell->charset, fg);
		}
	}
	// Snapshots contain cells and cursor coordinates, not the kernel
	// renderer's cursor pixels; the graphical terminal paints its own.
	if (gHdr.cur_row < gHdr.rows && gHdr.cur_col < gHdr.cols)
		os64_draw_fill_rect(&ctx->surf,
			gterm_grid_cell_rect(&gGrid, gHdr.cur_row, gHdr.cur_col), 0xffc0c0c0u);
	os64_draw_publish(ctx, NULL);
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
			line[n++] = (char)gterm_grid_byte((uint8_t)ch);
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
	os64_text_options_t options = {.memory = {NULL, font_allocate, font_release}};
	os64_font_set_t *initial = NULL;
	gGrid = (gterm_grid_t){.memory = options.memory, .resize = resize_pty,
		.invalidate = invalidate_grid};
	os64_font_status_t startup = os64_font_context_create(&options, &gText);
	if (!startup) {
		os64_font_config_t config;
		os64_font_config_error_t error;
		if (!os64_font_settings_current(&config, &gFontGeneration) &&
			!os64_font_config_prepare(gText, &config, &initial, &error))
			gFontSettingsReady = true;
		else startup = os64_font_set_prepare(gText, NULL, &initial);
	}
	if (startup != OS64_FONT_OK) {
		os64_text_destroy(gText);
		os64_printf("gterm: cannot prepare default font\n");
		return 1;
	}
	os64_font_role_view_t font;
	os64_font_set_view(initial, OS64_FONT_ROLE_TERMINAL, &font);
	uint32_t frame_w, frame_h;
	os64_gui_frame_for_content(WANT_COLS * (uint32_t)font.cell_width_px,
		WANT_ROWS * (uint32_t)font.row_height_px, 0, &frame_w, &frame_h);
	// A scalable startup face can make the preferred grid larger than the
	// display. Keep the frame reachable; the grid/PTY use the actual surface.
	int32_t initial_x = 140, initial_y = 120;
	uint32_t screen_w, screen_h;
	if (os64_gui_screen_info(&screen_w, &screen_h) == 0 && screen_w && screen_h) {
		uint32_t max_w = screen_w > 32 ? screen_w - 32 : screen_w;
		uint32_t max_h = screen_h > 64 ? screen_h - 64 : screen_h;
		if (frame_w > max_w) frame_w = max_w;
		if (frame_h > max_h) frame_h = max_h;
		if ((uint64_t)initial_x + frame_w > screen_w) initial_x = (int32_t)((screen_w - frame_w) / 2);
		if ((uint64_t)initial_y + frame_h > screen_h) initial_y = (int32_t)((screen_h - frame_h) / 2);
	}
	// `gterm [program args...]` seats that program instead of husk — the
	// root menu's way of running a console program in a window (`item
	// "Top" /bin/gterm /bin/top`). Full path: there is no shell in that
	// chain to search PATH. The window wears the program's name.
	const char *prog = (argc > 1) ? argv[1] : "/bin/husk";
	char *const *prog_argv = (argc > 1) ? &argv[1] : (char *[]){ "/bin/husk", 0 };
	const char *title = prog;
	for (const char *p = prog; *p; p++)
		if (*p == '/')
			title = p + 1;
	if (title[0] == 0)
		title = "gterm";

	int64_t win = os64_gui_window_create(title, initial_x, initial_y,
	                                     frame_w, frame_h, 0);
	if (win <= 0)
	{
		os64_printf("gterm: no GUI here (window_create %ld)\n", (long)win);
		os64_font_set_release(initial); os64_text_destroy(gText);
		return 1;
	}

	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_printf("gterm: get_surface failed\n");
		os64_font_set_release(initial); os64_text_destroy(gText);
		os64_gui_window_destroy(win);
		return 1;
	}

	gGrid.width = ctx.surf.width; gGrid.height = ctx.surf.height;
	os64_font_status_t status = replace_fonts(initial);
	os64_font_set_release(initial);
	if (status != OS64_FONT_OK) {
		os64_printf("gterm: font/grid preparation failed (%u)\n", status);
		os64_gui_window_destroy(win); os64_text_destroy(gText);
		return 1;
	}
	int64_t master = os64_pty_create(gGrid.cols, gGrid.rows);
	gMaster = master;
	if (master < 0) {
		os64_printf("gterm: pty_create failed (%ld)\n", (long)master);
		gterm_grid_destroy(&gGrid); os64_text_destroy(gText);
		os64_gui_window_destroy(win);
		return 1;
	}

	int64_t seated = os64_spawn_seated(prog, prog_argv, master);
	if (seated <= 0)
	{
		os64_printf("gterm: %s would not seat (%ld)\n", prog, (long)seated);
		gterm_grid_destroy(&gGrid); os64_text_destroy(gText);
		os64_close((int32_t)master);
		os64_gui_window_destroy(win);
		return 1;
	}

	os64_frame_clock_t clock;
	os64_frame_clock_init(&clock);
	bool window_alive = true;
	bool covered = false;   // nobody can see the window: read the pty, skip the paint

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
			if (ev.type == OS64_GUI_EVENT_APPEARANCE) {
				refresh_font_settings();
				repaint = true;
			}
			else if (ev.type == OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii != 0)
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
				if (ev.mouse.button == OS64_GUI_MOUSE_LEFT && gSnapshotValid)
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
				if (os64_draw_ctx_refresh(&ctx) == 0) {
					/* Allocation and bounds refusals retain the old PTY grid;
					 * the resized surface clips or letterboxes that grid. */
					gterm_grid_resize(&gGrid, ctx.surf.width, ctx.surf.height);
					gRenderedGen = UINT64_MAX;
				}
			}
			// COVERED / UNCOVERED events need no branch here: the flag is
			// asked every pass below, and a nudge that can be dropped from a
			// full queue is no basis for a decision either way.
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
		os64_pty_header_t probe;
		if (os64_pty_snapshot(master, &probe, NULL, 0) < 0)
			break;
		if (probe.flags & OS64_PTY_HUNGUP)
			break;                  // the session ended: exit closes the window
		// Can anyone see us? Asked EVERY pass — one get_state per frame —
		// never inferred from the COVERED/UNCOVERED events, either of which
		// a full queue can drop. gterm does not bind its frame clock the way
		// an animation does: a hung-up session must still close this window
		// and a paste in flight must still feed the shell whether or not
		// anyone is looking, so the loop keeps its cadence and its header
		// poll, and only the cell snapshot and the PAINT stop while covered.
		{
			os64_gui_window_state_t st;
			bool now_covered = covered;
			if (os64_gui_window_get_state(win, &st) == 0)
				now_covered = (st.flags & OS64_GUI_WINDOW_COVERED) != 0;
			if (covered && !now_covered)
				gRenderedGen = ~(uint64_t)0;   // show what happened while hidden
			covered = now_covered;
		}
		if (covered)
		{
			// Whatever moved is noted by the stale gRenderedGen; the first
			// uncovered pass paints it all at once.
		}
		else if (!gSnapshotValid || probe.generation != gRenderedGen)
		{
			os64_pty_header_t next = {0};
			os64_pty_cell_t *cells = gCells == gCellStorage[0] ? gCellStorage[1] : gCellStorage[0];
			int64_t copied = os64_pty_snapshot(master, &next, cells, MAX_CELLS);
			if (!gterm_grid_snapshot_matches(&gGrid, &next, copied)) {
				/* A successful PTY barrier is not undone by a transient read.
				 * Keep the displayed snapshot intact and retry next frame. */
				os64_frame_wait(&clock, 33);
				continue;
			}
			gHdr = next; gCells = cells; gSnapshotValid = true;
			// The text moved underneath. A highlight is a claim about WHICH
			// TEXT you have, and those coordinates now name something else —
			// so it goes out rather than lying. (The drag itself survives: if
			// a button is still down, the next move re-lights the selection
			// against what is on the screen now, which is what the eye is
			// following anyway.)
			gSelLive = false;
			render(&ctx);
			gRenderedGen = gHdr.generation;
		}
		else if (repaint)
		{
			render(&ctx);   // selection-only change: gCells is still current
		}

		os64_frame_wait(&clock, 33);   // the ratified ~30Hz poll
	}

	gterm_grid_destroy(&gGrid);
	os64_text_destroy(gText);
	paste_end();                       // a paste cut off by the hangup lets the snarf go
	os64_close((int32_t)master);       // hangup: the slave orphans benignly
	if (window_alive)
		os64_gui_window_destroy(win);
	return 0;
}
