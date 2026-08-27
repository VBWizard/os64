// glogo — the IMMEDIATE-MODE template (and xlogo's heir: X11's minimal demo
// was a program whose whole job was to draw the logo and exist; so is this).
//
// THE IDIOM, whole and unadorned: in immediate mode YOU are the painter.
// Every frame you draw the COMPLETE picture — clear, draw everything,
// publish — and the previous frame simply stops existing. No widgets, no
// dirty tracking, no retained anything: state lives in your variables, and
// the screen is a pure function of them, re-derived every frame.
//
//   When you want this: animation, custom drawing, anything that repaints
//   most of itself anyway (gbounce, gterm's grid).
//   When you want RETAINED instead (os64/ui.h; gclock is that template):
//   furniture that mostly sits still — labels, buttons, panels — where
//   repainting only what changed is the whole economy.
//
// Every numbered step below is load-bearing; delete the logo, keep the
// skeleton, and you have a new app.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/fmt.h"

#define WIN_W 280u
#define WIN_H 120u

// Animation state, in MILLI-PIXELS (integer math on purpose: userland builds
// and gbounce's screaming-ball scar taught that speeds are
// per-SECOND quantities scaled by real dt, never per-frame constants).
#define SWEEP_SPEED_MPX_PER_MS 80   // 80 px/sec, spelled in mpx/ms

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// [1] A window. Flags 0 = normal birth: on top, takes focus (and since
	//     2026-08-19 the dethroned window's titlebar repaints honestly —
	//     ask gclock how that was discovered).
	int64_t win = os64_gui_window_create("glogo", 360, 200, WIN_W, WIN_H, 0);
	if (win <= 0)
	{
		os64_printf("glogo: no GUI here (window_create %ld)\n", (long)win);
		return 1;
	}

	// [2] The draw context: fetches the shared canvas (your task-mapped
	//     pixels — drawing costs zero syscalls) and carries fg/bg + a text
	//     pen. Only publish crosses the ring.
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_printf("glogo: get_surface failed\n");
		os64_gui_window_destroy(win);
		return 1;
	}
	int32_t w = (int32_t)ctx.surf.width;
	int32_t h = (int32_t)ctx.surf.height;

	// [3] The frame clock. frame_wait anchors to the last frame BOUNDARY and
	//     sleeps only the remainder of the budget, so the cadence is honest
	//     regardless of how long drawing took — and it returns the REAL
	//     elapsed ms, which is what all animation advances by.
	os64_frame_clock_t tick;
	os64_frame_clock_init(&tick);

	int32_t sweep_mpx = 0;          // the underline's x, in milli-pixels
	uint64_t dt = 16;               // last frame's real duration
	bool running = true;

	while (running)
	{
		// [4] Events, drained first. 'q' quits; a dead window (negative
		//     poll) quits too. Ctrl+C from the launching terminal also
		//     works, for free — the exit sweep reclaims the window.
		os64_gui_event_t ev;
		int64_t rc;
		while ((rc = os64_gui_event_poll(win, &ev)) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii == 'q')
				running = false;
		}
		if (rc < 0)
			break;

		// [5] Advance state by REAL time. One add and a wrap — the point is
		//     the dt-scaling, not the math.
		sweep_mpx += (int32_t)(SWEEP_SPEED_MPX_PER_MS * dt);
		if (sweep_mpx >= w * 1000)
			sweep_mpx = 0;

		// [6] Draw the WHOLE frame, back to front. Clear first: immediate
		//     mode never trusts last frame's pixels.
		os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 0, w, h},
		                    OS64_GUI_COLOR_DESKTOP);

		// The logo (such as it is — xlogo set the bar exactly this high).
		os64_draw_ctx_pen(&ctx, 24, 28);
		os64_draw_ctx_colors(&ctx, OS64_GUI_COLOR_WHITE, OS64_GUI_COLOR_DESKTOP);
		os64_draw_ctx_text(&ctx, "o s 6 4");

		// The sweeping underline: proof the loop is alive and dt-honest.
		os64_draw_fill_rect(&ctx.surf,
		                    (os64_gui_rect_t){sweep_mpx / 1000, 52, 24, 3},
		                    OS64_GUI_COLOR_YELLOW);

		// A live dt readout — the frame loop narrating itself, and a
		// draw_text-with-explicit-args example beside the ctx-pen one above.
		char line[32];
		os64_snprintf(line, sizeof(line), "dt %lums   q quits", dt);
		os64_draw_text(&ctx.surf, 24, h - 28, line,
		               os64_strlen(line), OS64_GUI_COLOR_LIGHT_GRAY,
		               OS64_GUI_COLOR_DESKTOP);

		// [7] Publish. NULL = the whole content; a tighter app passes the
		//     changed rect. Snapshot-on-publish makes the frame atomic —
		//     the compositor never sees a half-drawn canvas.
		os64_draw_publish(&ctx, (const os64_gui_rect_t *)0);

		// [8] Breathe out the rest of the budget; catch the real dt for [5].
		dt = os64_frame_wait(&tick, 16);
	}

	// [9] Leave like you arrived. (Exiting without this also works — the
	//     kernel sweeps windows at task exit — but a template models the
	//     polite path.)
	os64_gui_window_destroy(win);
	return 0;
}
