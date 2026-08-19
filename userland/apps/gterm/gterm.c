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

#define CELL_W 8
#define CELL_H 16
#define WANT_COLS 100u
#define WANT_ROWS 38u
#define MAX_CELLS 16384u          // 128KB of BSS; caps a resize-crazy future

#define GTERM_BG 0xff000000u      // the console's black, honored

static os64_pty_header_t gHdr;
static os64_pty_cell_t   gCells[MAX_CELLS];

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
		char run[512];
		uint32_t c = 0;
		while (c < gHdr.cols)
		{
			uint32_t color = row[c].color;
			uint32_t start = c, n = 0;
			while (c < gHdr.cols && row[c].color == color && n < sizeof(run))
			{
				char ch = row[c].ch;
				run[n++] = (ch >= ' ') ? ch : ' ';
				c++;
			}
			os64_draw_text(&ctx->surf, (int32_t)(start * CELL_W),
			               (int32_t)(r * CELL_H), run, n, color, GTERM_BG);
		}
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
		os64_gui_event_t ev;
		int64_t erc;
		while ((erc = os64_gui_event_poll(win, &ev)) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii != 0)
				os64_write((int32_t)master, &ev.key.ascii, 1);
		}
		if (erc < 0)
		{
			window_alive = false;   // the window died under us (swept?)
			break;
		}

		// Grid in — header first, cells only when the generation moved.
		if (os64_pty_snapshot(master, &gHdr, (os64_pty_cell_t *)0, 0) < 0)
			break;
		if (gHdr.flags & OS64_PTY_HUNGUP)
			break;                  // the session ended: exit closes the window
		if (gHdr.generation != rendered_gen)
		{
			if (os64_pty_snapshot(master, &gHdr, gCells, cols * rows) < 0)
				break;
			render(&ctx);
			rendered_gen = gHdr.generation;
		}

		os64_frame_wait(&clock, 33);   // the ratified ~30Hz poll
	}

	os64_close((int32_t)master);       // hangup: the slave orphans benignly
	if (window_alive)
		os64_gui_window_destroy(win);
	return 0;
}
