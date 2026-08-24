// gui_demo_bounce.c — the /gbounce demo app: a ball bouncing in a window.
//
// This is deliberately written like a CLIENT, not like kernel GUI code: it
// talks only through gui_client.h (create/get_surface/present/event_poll),
// which is the exact surface a userland app will have over SYSCALL later.
// The compositing acid test: drag another window across this one while the
// ball animates — no flicker, no trails, correct overlap.

#include "gui/gui_client.h"
#include "gui/gui_demos.h"
#include "gui/window.h"   // GUI_WINDOW_START_UNFOCUSED — decline the boot focus race
#include "gui/surface.h"

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "signals.h"
#include "smp_core.h"
#include "thread.h"

#define BALL_R       12
#define BALL_BG      0xff20242c   // window content background
#define BALL_COLOR   GUI_COLOR_YELLOW
#define FRAME_TICKS  1            // animation cadence request (see GRAPHICS.md
                                  // on actual SIGSLEEP wake granularity)

static void draw_ball(surface_t *s, int32_t cx, int32_t cy, uint32_t color)
{
	// Filled circle as a stack of hlines; integer math only.
	for (int32_t dy = -BALL_R; dy <= BALL_R; dy++) {
		int32_t half = 0;
		while ((half + 1) * (half + 1) + dy * dy <= BALL_R * BALL_R)
			half++;
		surface_draw_hline(s, cx - half, cy + dy, 2 * half + 1, color);
	}
}

static inline rect_t ball_rect(int32_t cx, int32_t cy)
{
	return (rect_t){cx - BALL_R, cy - BALL_R, 2 * BALL_R + 1, 2 * BALL_R + 1};
}

bool gbounce_thread(bool daemon)
{
	(void)daemon;
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	int64_t win = gui_window_create("bounce", 660, 430, 300, 220, GUI_WINDOW_START_UNFOCUSED);
	if (win <= 0) {
		printd(DEBUG_GUI, "gbounce: window create failed (%ld)\n", win);
		return false;
	}
	surface_t content;
	gui_window_get_surface(win, &content);

	surface_fill_rect(&content,
	                  (rect_t){0, 0, (int32_t)content.width, (int32_t)content.height}, BALL_BG);
	gui_window_publish(win, NULL);

	int32_t x = BALL_R + 5, y = BALL_R + 5;
	int32_t vx = 7, vy = 5;

	while (1) {
		// Erase at the old spot, advance, bounce, redraw.
		rect_t old_spot = ball_rect(x, y);
		surface_fill_rect(&content, old_spot, BALL_BG);

		x += vx;
		y += vy;
		if (x - BALL_R < 0)                        { x = BALL_R;                          vx = -vx; }
		if (x + BALL_R >= (int32_t)content.width)  { x = (int32_t)content.width - 1 - BALL_R;  vx = -vx; }
		if (y - BALL_R < 0)                        { y = BALL_R;                          vy = -vy; }
		if (y + BALL_R >= (int32_t)content.height) { y = (int32_t)content.height - 1 - BALL_R; vy = -vy; }

		draw_ball(&content, x, y, BALL_COLOR);

		// Publish exactly what changed: old spot ∪ new spot.
		rect_t damage = rect_union(old_spot, ball_rect(x, y));
		gui_window_publish(win, &damage);

		// Stay polite: drain our event queue even though we ignore it (a
		// full queue would just drop events, but this is what real apps do).
		input_event_t ev;
		while (gui_event_poll(win, &ev) == 1)
			;

		signal_raise(SIGSLEEP, kTicksSinceStart + FRAME_TICKS, self);
	}
}
