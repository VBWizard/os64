// fpu_demo — a deliberately small floating-point picture. It is not a
// benchmark: the moving Mandelbrot view makes ordinary x87/SSE arithmetic
// visible, while the frame wait gives the scheduler opportunities to prove
// that this thread's vector state survives preemption.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define WIDTH  320
#define HEIGHT 220
#define ITERATIONS 72
#define ZOOM_FACTOR_PER_MS 1.0003466336538454

static uint32_t mandelbrot_color(int iteration)
{
	if (iteration == ITERATIONS)
		return 0xFF06101EU;
	uint32_t v = (uint32_t)(iteration * 255 / ITERATIONS);
	return 0xFF000000U | (v << 16) | ((255U - v) << 8) | (v / 2U + 48U);
}

static double advance_zoom(double zoom, uint64_t elapsed_ms)
{
	// Exponentiation by squaring keeps the zoom tied to elapsed time without
	// requiring libm. The per-millisecond factor doubles the scale every 2s.
	double factor = ZOOM_FACTOR_PER_MS;
	while (elapsed_ms != 0)
	{
		if ((elapsed_ms & 1U) != 0)
			zoom *= factor;
		factor *= factor;
		elapsed_ms >>= 1;
	}
	return zoom;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int64_t win = os64_gui_window_create("FPU Mandelbrot", 180, 120, WIDTH, HEIGHT, 0);
	if (win <= 0)
		return 1;

	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
		return 1;

	os64_frame_clock_t clock;
	os64_frame_clock_init(&clock);
	double zoom = 1.0;
	bool running = true;
	while (running)
	{
		os64_gui_event_t event;
		int64_t event_result;
		while ((event_result = os64_gui_event_poll(win, &event)) == 1)
			if (event.type == OS64_GUI_EVENT_KEY_DOWN && event.key.ascii == 'q')
				running = false;
		if (event_result < 0)
			break;

		// Hold the camera on the boundary point as the scale grows. Drifting the
		// center right eventually moves the colored boundary out of the window.
		double center_x = -0.743643887037151;
		double center_y =  0.131825904205330;
		for (int y = 0; y < HEIGHT; y++)
		{
			for (int x = 0; x < WIDTH; x++)
			{
				double cr = center_x + ((double)x / (double)WIDTH - 0.5) * 3.2 / zoom;
				double ci = center_y + ((double)y / (double)HEIGHT - 0.5) * 2.2 / zoom;
				double zr = 0.0, zi = 0.0;
				int iteration = 0;
				while (zr * zr + zi * zi < 4.0 && iteration < ITERATIONS)
				{
					double next = zr * zr - zi * zi + cr;
					zi = 2.0 * zr * zi + ci;
					zr = next;
					iteration++;
				}
				ctx.surf.pixels[y * ctx.surf.pitch_px + x] = mandelbrot_color(iteration);
			}
		}
		os64_draw_text(&ctx.surf, 12, 12, "FPU Mandelbrot  (q quits)", 25,
		               OS64_GUI_COLOR_WHITE, 0xFF06101EU);
		os64_draw_publish(&ctx, (const os64_gui_rect_t *)0);
		zoom = advance_zoom(zoom, os64_frame_wait(&clock, 33));
	}

	os64_gui_window_destroy(win);
	return 0;
}
