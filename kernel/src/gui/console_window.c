// console_window.c — the GUI console: print_n's sink + a text-grid window.
//
// Data flow: any printf/print_n anywhere → gui_console_sink() appends into
// the character grid (own spinlock, no drawing, no kGuiLock — callable from
// any context) → compositor's frame loop calls gui_console_render_if_dirty()
// → grid is drawn into the window content and presented like any client
// would. Panic reverses the diversion with one store (gui_emergency_disable).

#include "gui/console_window.h"
#include "gui/gui_client.h"
#include "gui/surface.h"

#include "CONFIG.h"
#include "BasicRenderer.h"
#include "memcpy.h"
#include "memset.h"
#include "printd.h"
#include "spinlock.h"

// 64x18 characters at 8x16 px/glyph = 512x288 content.
#define CON_COLS 64
#define CON_ROWS 18
#define CON_BG   0xff101418
#define CON_FG   0xffd0d4c8

static char s_grid[CON_ROWS][CON_COLS];
static uint32_t s_row, s_col;
static spinlock_t s_con_lock = 0;
static volatile bool s_dirty = false;

static int64_t s_win = 0;
static surface_t s_content;

// Scroll the GRID one line (a memmove of characters, not pixels).
// Caller holds s_con_lock.
static void grid_scroll_if_needed(void)
{
	if (s_row < CON_ROWS)
		return;
	memmove(s_grid[0], s_grid[1], (CON_ROWS - 1) * CON_COLS);
	memset(s_grid[CON_ROWS - 1], 0, CON_COLS);
	s_row = CON_ROWS - 1;
}

// Append text to the grid. Runs in the PRINTING thread's context — keep it
// tiny, take only s_con_lock, never draw.
static void gui_console_sink(const char *bytes, size_t length)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_con_lock);

	for (size_t i = 0; i < length; i++) {
		char c = bytes[i];
		if (c == '\n') {
			s_col = 0;
			s_row++;
			grid_scroll_if_needed();
		} else if (c == '\t') {
			s_col = (s_col + 8) & ~7u;
		} else if (c == '\b') {
			// Cursor-back only, clamped at column 0 — erasure comes from the
			// caller overprinting ("\b \b"), same contract as print_n.
			if (s_col > 0)
				s_col--;
		} else if (c == '\r') {
			s_col = 0;
		} else if (c == '\f') {
			// Form feed = fresh page, same contract as print_n. Each console
			// interprets '\f' against ITS OWN surface — this one wipes the
			// grid; the legacy renderer wipes the framebuffer. That per-sink
			// interpretation is what keeps clear(1) working no matter which
			// screen (or future tty pipe) its byte ends up draining into.
			memset(s_grid, 0, sizeof(s_grid));
			s_row = 0;
			s_col = 0;
		} else if (c >= ' ') {
			if (s_col >= CON_COLS) {   // wrap long lines
				s_col = 0;
				s_row++;
				grid_scroll_if_needed();
			}
			s_grid[s_row][s_col++] = c;
		}
	}

	s_dirty = true;
	spinlock_release_irqrestore(&s_con_lock, flags);
}

void gui_console_start(void)
{
	int32_t w = CON_COLS * 8 + 2;                       // + borders
	int32_t h = CON_ROWS * 16 + 20 + 1;                 // + titlebar + border
	s_win = gui_window_create("console", 24, 330, (uint32_t)w, (uint32_t)h, 0);
	if (s_win <= 0) {
		printd(DEBUG_GUI, "console: window create failed (%ld)\n", s_win);
		return;
	}
	gui_window_get_surface(s_win, &s_content);
	surface_fill_rect(&s_content,
	                  (rect_t){0, 0, (int32_t)s_content.width, (int32_t)s_content.height}, CON_BG);
	gui_window_publish(s_win, NULL);

	// Flip the diversion on LAST — from here, every print_n lands in the grid.
	kConsoleSink = gui_console_sink;
	printd(DEBUG_GUI, "console: window %ld attached to kConsoleSink\n", s_win);
}

void gui_console_render_if_dirty(void)
{
	if (s_win <= 0 || !s_dirty)
		return;

	// Draw the whole grid under the console lock (printing threads spin for
	// the duration — a full redraw is ~150µs of RAM writes, acceptable).
	uint64_t flags = spinlock_acquire_irqsave(&s_con_lock);
	for (uint32_t r = 0; r < CON_ROWS; r++) {
		// draw_text paints the full opaque cell, so no pre-clear is needed;
		// but rows may contain NULs mid-line — draw char by char, blanking
		// NULs so stale glyphs never linger.
		for (uint32_t c = 0; c < CON_COLS; c++) {
			char ch = s_grid[r][c] ? s_grid[r][c] : ' ';
			surface_draw_text(&s_content, (int32_t)(c * 8), (int32_t)(r * 16),
			                  &ch, 1, CON_FG, CON_BG);
		}
	}
	s_dirty = false;
	spinlock_release_irqrestore(&s_con_lock, flags);

	gui_window_publish(s_win, NULL);
}
