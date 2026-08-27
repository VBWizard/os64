// fpu_orbit — the same central-force simulation with two number systems.
// Whole-pixel state quantizes weak acceleration into coarse steps; double
// state retains the fractions and traces the smooth orbit they add up to.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define WIN_W       520
#define WIN_H       270
#define FRAME_MS     33
#define STEP_MS      33
#define TRAIL_POINTS 128
#define FORCE_DIVISOR 64

#define BG_COLOR       0xFF101722U
#define PANEL_COLOR    0xFF182331U
#define INTEGER_COLOR  OS64_GUI_COLOR_YELLOW
#define FPU_COLOR      0xFFFF5FA2U

typedef struct trail_point
{
	int32_t x;
	int32_t y;
} trail_point_t;

static void draw_dot(os64_gui_surface_t *surface, int32_t x, int32_t y,
                     int32_t radius, uint32_t color)
{
	for (int32_t dy = -radius; dy <= radius; dy++)
	{
		int32_t half = 0;
		while ((half + 1) * (half + 1) + dy * dy <= radius * radius)
			half++;
		os64_draw_hline(surface, x - half, y + dy, half * 2 + 1, color);
	}
}

static void trail_add(trail_point_t *trail, uint32_t *next, uint32_t *count,
                      int32_t x, int32_t y)
{
	trail[*next] = (trail_point_t){x, y};
	*next = (*next + 1U) % TRAIL_POINTS;
	if (*count < TRAIL_POINTS)
		(*count)++;
}

static int32_t integer_force(int32_t position)
{
	// Round to the nearest representable whole-pixel acceleration. Truncating
	// 40 / 64 to zero removes the horizontal force and degenerates into a line.
	if (position >= 0)
		return (position + FORCE_DIVISOR / 2) / FORCE_DIVISOR;
	return (position - FORCE_DIVISOR / 2) / FORCE_DIVISOR;
}

static void draw_trail(os64_gui_surface_t *surface, const trail_point_t *trail,
                       uint32_t next, uint32_t count, int32_t center_x,
                       int32_t center_y, uint32_t color)
{
	uint32_t first = (next + TRAIL_POINTS - count) % TRAIL_POINTS;
	for (uint32_t i = 0; i < count; i++)
	{
		const trail_point_t *point = &trail[(first + i) % TRAIL_POINTS];
		os64_draw_fill_rect(surface,
			(os64_gui_rect_t){center_x + point->x, center_y + point->y, 2, 2}, color);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int64_t win = os64_gui_window_create("Integer / FPU Orbit", 230, 150,
	                                     WIN_W, WIN_H, 0);
	if (win <= 0)
		return 1;

	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_gui_window_destroy(win);
		return 1;
	}

	// Both simulations begin with the same displayed state and force law. The
	// integer side must choose a whole acceleration at every simulation step.
	int32_t integer_x = 40, integer_y = 0;
	int32_t integer_vx = 0, integer_vy = 5;
	double fpu_x = 40.0, fpu_y = 0.0;
	double fpu_vx = 0.0, fpu_vy = 5.0;

	trail_point_t integer_trail[TRAIL_POINTS] = {{0, 0}};
	trail_point_t fpu_trail[TRAIL_POINTS] = {{0, 0}};
	uint32_t integer_next = 0, integer_count = 0;
	uint32_t fpu_next = 0, fpu_count = 0;

	os64_frame_clock_t clock;
	os64_frame_clock_init(&clock);
	os64_frame_clock_bind(&clock, win);   // sleep while nobody can see the orbit
	uint64_t elapsed_ms = STEP_MS;
	uint64_t accumulated_ms = 0;
	bool running = true;

	while (running)
	{
		os64_gui_event_t event;
		int64_t event_result;
		while ((event_result = os64_gui_event_poll(win, &event)) == 1)
		{
			if (event.type == OS64_GUI_EVENT_KEY_DOWN && event.key.ascii == 'q')
				running = false;
			if (event.type == OS64_GUI_EVENT_WINDOW_RESIZE &&
			    os64_draw_ctx_refresh(&ctx) != 0)
				running = false;
		}
		if (!running || event_result < 0)
			break;

		accumulated_ms += elapsed_ms;
		while (accumulated_ms >= STEP_MS)
		{
			integer_vx -= integer_force(integer_x);
			integer_vy -= integer_force(integer_y);
			integer_x += integer_vx;
			integer_y += integer_vy;

			fpu_vx -= fpu_x / (double)FORCE_DIVISOR;
			fpu_vy -= fpu_y / (double)FORCE_DIVISOR;
			fpu_x += fpu_vx;
			fpu_y += fpu_vy;

			trail_add(integer_trail, &integer_next, &integer_count,
			          integer_x, integer_y);
			trail_add(fpu_trail, &fpu_next, &fpu_count,
			          (int32_t)fpu_x, (int32_t)fpu_y);
			accumulated_ms -= STEP_MS;
		}

		int32_t width = (int32_t)ctx.surf.width;
		int32_t height = (int32_t)ctx.surf.height;
		int32_t half_width = width / 2;
		int32_t footer_y = height - 38;
		int32_t panel_height = footer_y - 38;
		int32_t center_y = 30 + panel_height / 2;
		int32_t integer_center_x = width / 4;
		int32_t fpu_center_x = width * 3 / 4;

		os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 0, width, height},
		                    BG_COLOR);
		os64_draw_fill_rect(&ctx.surf,
		                    (os64_gui_rect_t){8, 30, half_width - 12, panel_height},
		                    PANEL_COLOR);
		os64_draw_fill_rect(&ctx.surf,
		                    (os64_gui_rect_t){half_width + 4, 30,
		                                      half_width - 12, panel_height},
		                    PANEL_COLOR);
		os64_draw_text(&ctx.surf, integer_center_x - 80, 8,
		               "WHOLE-NUMBER INTEGER", 20,
		               INTEGER_COLOR, BG_COLOR);
		os64_draw_text(&ctx.surf, fpu_center_x - 60, 8, "HARDWARE DOUBLE", 15,
		               FPU_COLOR, BG_COLOR);

		draw_dot(&ctx.surf, integer_center_x, center_y, 3, OS64_GUI_COLOR_WHITE);
		draw_dot(&ctx.surf, fpu_center_x, center_y, 3, OS64_GUI_COLOR_WHITE);
		draw_trail(&ctx.surf, integer_trail, integer_next, integer_count,
		           integer_center_x, center_y, INTEGER_COLOR);
		draw_trail(&ctx.surf, fpu_trail, fpu_next, fpu_count,
		           fpu_center_x, center_y, FPU_COLOR);
		draw_dot(&ctx.surf, integer_center_x + integer_x, center_y + integer_y,
		         4, INTEGER_COLOR);
		draw_dot(&ctx.surf, fpu_center_x + (int32_t)fpu_x,
		         center_y + (int32_t)fpu_y, 4, FPU_COLOR);

		os64_draw_text(&ctx.surf, 12, footer_y,
		               "Fixed-point can match this with explicit scaling.",
		               sizeof("Fixed-point can match this with explicit scaling.") - 1,
		               OS64_GUI_COLOR_LIGHT_GRAY, BG_COLOR);
		os64_draw_text(&ctx.surf, 12, footer_y + 18,
		               "Same simulated time; this compares precision.  q quits",
		               sizeof("Same simulated time; this compares precision.  q quits") - 1,
		               OS64_GUI_COLOR_LIGHT_GRAY, BG_COLOR);
		os64_draw_publish(&ctx, (const os64_gui_rect_t *)0);
		elapsed_ms = os64_frame_wait(&clock, FRAME_MS);
	}

	os64_gui_window_destroy(win);
	return 0;
}
