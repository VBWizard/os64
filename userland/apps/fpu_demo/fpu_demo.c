// fpu_demo — a Mandelbrot zoom, as a picture of what the FPU slice bought.
//
// Everything on screen is double arithmetic in XMM registers, hundreds of
// millions of operations a minute, and the whole point is that nothing
// about it is special: an ordinary program doing ordinary math. The zoom
// is a tour, not a plunge — it dives into one famous coordinate until the
// doubles run out of digits, climbs back out, and moves on to the next.
//
// Three things make it a demo rather than a screensaver:
//
//   THE ZOOM HAS A FLOOR. Every pixel is center + offset, and once the
//   offset between two neighbouring pixels falls below the double's spacing
//   at that center (about 2^-52 of it), every pixel computes the SAME point
//   and the picture becomes one flat color. The tour turns around well
//   before that, at each target's own max_log2 (never past ZOOM_MAX_LOG2).
//
//   THE ITERATION COUNT GROWS WITH THE ZOOM. Deep in the set, points take
//   longer to escape; a fixed budget paints everything interior black just
//   when it gets interesting. So the budget grows with the depth.
//
//   THE COLOR IS CONTINUOUS. Counting whole iterations gives bands; the
//   classic cure adds the fractional iteration from how far past the
//   bailout the point landed — which needs a log2, and there is no libm.
//   So there is a small one below, made from the double's own exponent
//   bits: on-theme for a program about floating point.
//
// Keys: space pauses the tour, n skips to the next coordinate, q quits.
// Behind another window it pauses entirely (os64_frame_clock_bind) — and
// picks up exactly where it stopped, which is a demo in itself.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define WIDTH  480
#define HEIGHT 320

// Zoom, in doublings: the tour goes 0 -> the target's max_log2 -> 0 on each,
// DOUBLINGS_PER_SECOND at a time. 2^32 keeps the per-frame iteration budget
// affordable (it grows with depth) and stays far below the double's cliff
// (about 2^44 for these centers at this width).
#define ZOOM_MAX_LOG2         32.0    // the deepest any target goes (per-target max_log2 may stop sooner)
#define DOUBLINGS_PER_SECOND  0.5
#define HOLD_MS               1500    // a beat at the bottom before climbing out

#define ITER_BASE   64
#define ITER_PER_DOUBLING 40
#define ITER_MAX    2500
#define BAILOUT_SQ  (256.0 * 256.0)   // a big bailout makes the fractional part honest

typedef struct { double x, y; double max_log2; const char *name; } target_t;

// Four of the coordinates every Mandelbrot tourist knows, each with its own
// depth: a dive stops where its subject fills the window. Past that, a
// coordinate known to 15 digits drifts off the thing it names and the view
// follows a filament beside it (the first mini-brot did exactly that).
static const target_t kTargets[] = {
	{ -0.743643887037151,   0.131825904205330,  32.0, "seahorse valley"  },
	{  0.2549870375144766, -0.0005679790528465, 32.0, "elephant valley"  },
	{ -1.7548776662466927,  0.0,                 5.0, "the antenna mini-brot" },   // period 3; fills the frame at 2^4, and past 2^5 the view is inside its cardioid
	{ -0.7756837699949401,  0.1364673726219178, 32.0, "misiurewicz point" },
};
#define TARGETS ((int)(sizeof(kTargets) / sizeof(kTargets[0])))

// ── a log2 with no libm ─────────────────────────────────────────────────────
//
// A double is sign, an 11-bit exponent, and a 52-bit mantissa: x = m * 2^e
// with m in [1, 2). log2(x) = e + log2(m), and log2(m) on [1, 2) is smooth
// enough that a short polynomial (fitted to the interval) is within 2e-3 —
// a fiftieth of one palette step, invisible. Only ever called on positive
// finite values. tools/test_mandel_math_host.c checks both this and pow2
// against libm — plain gcc, seconds per cycle, no QEMU boot to see a color.
static double log2_approx(double x)
{
	union { double d; uint64_t u; } bits = { .d = x };
	int e = (int)((bits.u >> 52) & 0x7FF) - 1023;
	bits.u = (bits.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;  // m in [1,2)
	double m = bits.d;
	double t = m - 1.0;   // in [0,1)
	// A quartic for log2(1+t) on [0,1): error under 2e-3.
	double p = t * (1.4425 + t * (-0.7071 + t * (0.3597 - t * 0.0952)));
	return (double)e + p;
}

// ── colour ──────────────────────────────────────────────────────────────────

// A cyclic gradient through six stops: the classic "ultra" palette shape,
// dark blue -> pale -> gold -> dark -> back around. `mu` is the continuous
// iteration count; PALETTE_PERIOD iterations make one full lap.
#define PALETTE_PERIOD 48.0
static const uint32_t kStops[] = { 0x000764, 0x206BCB, 0xEDFFFF, 0xFFAA00, 0x000200, 0x000764 };
#define STOPS ((int)(sizeof(kStops) / sizeof(kStops[0])))

static uint32_t palette(double mu)
{
	double f = mu / PALETTE_PERIOD;
	f -= (double)(int64_t)f;               // fractional lap, [0,1)
	if (f < 0.0) f += 1.0;
	double pos = f * (STOPS - 1);
	int i = (int)pos;
	double w = pos - (double)i;
	uint32_t a = kStops[i], b = kStops[i + 1];
	uint32_t r = (uint32_t)(((a >> 16) & 255) * (1.0 - w) + ((b >> 16) & 255) * w);
	uint32_t g = (uint32_t)(((a >> 8)  & 255) * (1.0 - w) + ((b >> 8)  & 255) * w);
	uint32_t bl = (uint32_t)((a & 255) * (1.0 - w) + (b & 255) * w);
	return 0xFF000000U | (r << 16) | (g << 8) | bl;
}

// ── the picture ─────────────────────────────────────────────────────────────

// Two zoom-powers to a scale: the view spans 3.2 / 2^log2zoom units wide.
static double pow2(double p)
{
	// 2^p = 2^floor(p) * 2^frac(p); the fractional part by a short series
	// (within 8e-4 of the truth — a smooth wobble in zoom speed, not a jump).
	int ip = (int)p;
	if (p < 0.0 && (double)ip != p) ip--;
	double fr = p - (double)ip;                       // [0,1)
	double f = 1.0 + fr * (0.6931472 + fr * (0.2402265 + fr * (0.0555041 + fr * 0.0096181)));
	union { double d; uint64_t u; } bits = { .u = (uint64_t)(ip + 1023) << 52 };
	return bits.d * f;
}

static void render(os64_draw_ctx_t *ctx, const target_t *t, double log2zoom, int max_iter)
{
	int width = (int)ctx->surf.width, height = (int)ctx->surf.height;   // the window as it is NOW, not as created
	double scale = 3.2 / pow2(log2zoom) / (double)width;   // units per pixel
	double left = t->x - scale * (width / 2.0);
	double top  = t->y - scale * (height / 2.0);
	uint32_t *px = ctx->surf.pixels;
	uint32_t pitch = ctx->surf.pitch_px;

	for (int y = 0; y < height; y++)
	{
		double ci = top + scale * y;
		for (int x = 0; x < width; x++)
		{
			double cr = left + scale * x;
			double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
			int n = 0;
			while (zr2 + zi2 < BAILOUT_SQ && n < max_iter)
			{
				zi = 2.0 * zr * zi + ci;
				zr = zr2 - zi2 + cr;
				zr2 = zr * zr;
				zi2 = zi * zi;
				n++;
			}
			uint32_t color;
			if (n == max_iter)
				color = 0xFF000000U;   // inside: black, always
			else
			{
				// Continuous escape: how many fractional iterations past the
				// bailout did this point land? log2(log2(|z|)) is the textbook
				// term; |z|^2 is what we have, and log2(|z|) = log2(|z|^2)/2.
				double mu = (double)n + 1.0 - log2_approx(log2_approx(zr2 + zi2) * 0.5);
				color = palette(mu);
			}
			px[y * pitch + x] = color;
		}
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	int64_t win = os64_gui_window_create("FPU Mandelbrot", 180, 120, WIDTH, HEIGHT, 0);
	if (win <= 0)
		return 1;
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
		return 1;

	os64_frame_clock_t clock;
	os64_frame_clock_init(&clock);
	os64_frame_clock_bind(&clock, win);   // sleep while nobody can see the set

	int    target = 0;
	double log2zoom = 0.0;
	int    direction = 1;       // +1 diving, -1 climbing out
	uint64_t hold_ms = 0;       // time left at the bottom
	bool paused = false, running = true;
	uint64_t dt = 33;
	int    drawn_target = -1;        // what the last render was of
	double drawn_log2zoom = -1.0;
	int    drawn_iter = -1;
	bool   drawn_paused = false;

	while (running)
	{
		os64_gui_event_t ev;
		int64_t erc;
		while ((erc = os64_gui_event_poll(win, &ev)) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE)
			{
				// The canvas is bigger or smaller now; the cached picture is
				// the wrong size whatever the tour is doing — including
				// paused, which is exactly when nothing else would repaint.
				os64_draw_ctx_refresh(&ctx);
				drawn_target = -1;
				continue;
			}
			if (ev.type != OS64_GUI_EVENT_KEY_DOWN)
				continue;
			if (ev.key.ascii == 'q') running = false;
			if (ev.key.ascii == ' ') paused = !paused;
			if (ev.key.ascii == 'n') { target = (target + 1) % TARGETS; log2zoom = 0.0; direction = 1; hold_ms = 0; }
		}
		if (erc < 0 || !running)
			break;   // q, or the window died: leave before another frame wait could sleep behind a cover

		// Advance the tour by real time — never by frames — so the pace is
		// the same on a fast machine, a slow one, and after a pause.
		if (!paused)
		{
			if (hold_ms > 0)
				hold_ms = (hold_ms > dt) ? hold_ms - dt : 0;
			else
			{
				log2zoom += direction * DOUBLINGS_PER_SECOND * (double)dt / 1000.0;
				if (log2zoom >= kTargets[target].max_log2) { log2zoom = kTargets[target].max_log2; direction = -1; hold_ms = HOLD_MS; }
				if (log2zoom <= 0.0)           { log2zoom = 0.0; direction = 1; target = (target + 1) % TARGETS; }
			}
		}

		int max_iter = ITER_BASE + (int)(ITER_PER_DOUBLING * log2zoom);
		if (max_iter > ITER_MAX) max_iter = ITER_MAX;

		// The picture is a function of (target, zoom, iterations): paused,
		// or holding at the bottom of a dive, none of them move, and a
		// 200-million-iteration frame identical to the last one is a core
		// burned for nothing. Repaint only when the inputs changed; the loop
		// keeps pacing and polling regardless.
		bool changed = (target != drawn_target || log2zoom != drawn_log2zoom || max_iter != drawn_iter);
		if (changed)
		{
			render(&ctx, &kTargets[target], log2zoom, max_iter);
			drawn_target = target; drawn_log2zoom = log2zoom; drawn_iter = max_iter;
		}
		if (changed || paused != drawn_paused)
		{
			char line[96];
			int32_t n = os64_snprintf(line, sizeof(line), "%s  2^%d  %d iter  %lu ms%s   space/n/q",
			                          kTargets[target].name, (int)log2zoom, max_iter, dt, paused ? "  [paused]" : "");
			// The whole status band is cleared first: an unpause during the
			// hold shortens the line without changing the picture, and the
			// tail of "[paused]" would otherwise stay on the glass.
			os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 6, (int32_t)ctx.surf.width, 20}, 0xFF000000U);
			os64_draw_text(&ctx.surf, 8, 8, line, (size_t)n, OS64_GUI_COLOR_WHITE, 0xFF000000U);
			os64_draw_publish(&ctx, (const os64_gui_rect_t *)0);
			drawn_paused = paused;
		}
		dt = os64_frame_wait(&clock, 33);
	}

	os64_gui_window_destroy(win);
	return 0;
}
