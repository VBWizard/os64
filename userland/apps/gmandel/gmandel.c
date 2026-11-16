// gmandel — two windows race through the same Mandelbrot zoom: one computes
// four pixels at a time with SSE2, the other eight at a time with AVX2 and
// FMA, and each says how many frames a second it manages
// (docs/design/pending/AVX.md § The demo).
//
// The two are the same algorithm written twice at two widths. A pixel's
// escape-time loop runs in every lane of a vector at once; a lane that has
// escaped keeps its last z (a blend under the "still inside" mask) while its
// neighbours finish, and the vector is done when no lane is still inside.
// SSE2 is the x86-64 baseline, so its window always runs. The AVX window
// asks the CPU first (CPUID's OSXSAVE, AVX, AVX2 and FMA, then XGETBV) and
// says so when the answer is no — on the bare QEMU model, or under NOAVX.
//
// It doubles as a standing proof of the kernel's AVX context switch: both
// threads run flat out while the desktop switches threads around them, and
// an upper half lost or swapped on a switch shows as garbage in the picture.
//
// SINGLE PRECISION, because that is what makes the lanes four and eight. A
// float's 24-bit mantissa runs out of digits about 2^14 into a zoom near
// these centers (the spacing between pixels falls toward the spacing between
// floats, and neighbouring pixels compute the same point), so the tour turns
// around at 2^13 — shallower than fpu_demo's double-precision dive, which is
// the price of the extra lanes and worth knowing.
//
// The tour runs on one clock that both windows read, so they show the same
// view at the same moment; the faster window simply shows more of them.
// Keys, in either window: space pauses, n skips to the next coordinate,
// q (or closing either window) quits both.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/lock.h"

#define VIEW_W 400
#define VIEW_H 300
#define BAND_H 20                     // the status line along the top
#define GAP    16                     // between the two windows

#define ZOOM_MAX_LOG2         13.0f   // the float floor, with a margin (see the header)
#define DOUBLINGS_PER_SECOND  0.75f
#define HOLD_S                1.5f    // a beat at the bottom before climbing out

#define ITER_BASE         64
#define ITER_PER_DOUBLING 40
#define BAILOUT           256.0f      // |z|^2 bailout of 256^2 keeps the smooth colour honest
#define BAILOUT_SQ        (BAILOUT * BAILOUT)

typedef float v4f __attribute__((vector_size(16)));
typedef int   v4i __attribute__((vector_size(16)));
typedef float v8f __attribute__((vector_size(32)));
typedef int   v8i __attribute__((vector_size(32)));

typedef struct { float x, y; const char *name; } target_t;

static const target_t kTargets[] = {
	{ -0.7436439f,  0.1318259f, "seahorse valley"   },
	{  0.2549870f, -0.0005680f, "elephant valley"   },
	{ -0.7756838f,  0.1364674f, "misiurewicz point" },
	{ -1.2537500f,  0.0461000f, "the west bulb"     },
};
#define TARGETS ((int)(sizeof(kTargets) / sizeof(kTargets[0])))

// ── The tour, shared by both windows ────────────────────────────────────────
//
// A target's dive is a function of time since its phase began: down at
// DOUBLINGS_PER_SECOND, a hold, back up. Whichever window first sees the
// phase end moves the tour on, under the lock; pausing freezes the clock
// both windows read, and resuming moves the phase's start by the time spent
// paused, so nothing jumps.

typedef struct {
	os64_lock_t lock;
	int     target;
	int64_t phase_start_us;
	bool    paused;
	int64_t paused_at_us;
	volatile bool quit;
	volatile int32_t fps_x10[2];      // each window's frame rate, for the other to compare
} tour_t;

static tour_t s_tour;

static float phase_seconds(void)
{
	return 2.0f * ZOOM_MAX_LOG2 / DOUBLINGS_PER_SECOND + HOLD_S;
}

static void tour_now(int *target, float *log2zoom, bool *paused)
{
	os64_lock_acquire(&s_tour.lock);
	int64_t now = s_tour.paused ? s_tour.paused_at_us : os64_micros();
	float t = (float)(now - s_tour.phase_start_us) / 1e6f;
	while (t >= phase_seconds())
	{
		s_tour.target = (s_tour.target + 1) % TARGETS;
		s_tour.phase_start_us += (int64_t)(phase_seconds() * 1e6f);
		t -= phase_seconds();
	}
	float dive = ZOOM_MAX_LOG2 / DOUBLINGS_PER_SECOND;
	if (t < dive)
		*log2zoom = t * DOUBLINGS_PER_SECOND;
	else if (t < dive + HOLD_S)
		*log2zoom = ZOOM_MAX_LOG2;
	else
		*log2zoom = ZOOM_MAX_LOG2 - (t - dive - HOLD_S) * DOUBLINGS_PER_SECOND;
	if (*log2zoom < 0.0f)
		*log2zoom = 0.0f;
	*target = s_tour.target;
	*paused = s_tour.paused;
	os64_lock_release(&s_tour.lock);
}

static void tour_key(uint32_t ascii)
{
	os64_lock_acquire(&s_tour.lock);
	int64_t now = os64_micros();
	if (ascii == ' ')
	{
		if (s_tour.paused)
			s_tour.phase_start_us += now - s_tour.paused_at_us;
		else
			s_tour.paused_at_us = now;
		s_tour.paused = !s_tour.paused;
	}
	else if (ascii == 'n')
	{
		s_tour.target = (s_tour.target + 1) % TARGETS;
		s_tour.phase_start_us = s_tour.paused ? s_tour.paused_at_us : now;
	}
	else if (ascii == 'q')
		s_tour.quit = true;
	os64_lock_release(&s_tour.lock);
}

// ── The two kernels ─────────────────────────────────────────────────────────
//
// One stretch of a row each — `width` pixels starting at pixel `first` —
// giving the escape count of every pixel and |z|^2 where it escaped (for
// the smooth colour). The buffers are padded to a multiple of eight, so the
// last vector may compute pixels past the stretch and nobody reads them.
// Each mirrors the scalar loop exactly: a lane counts an
// iteration only while it is still inside, and stops updating z the moment
// it is not.

static void row_sse2(float ci, float left, float scale, int first, int width, int max_iter,
                     int *counts, float *mags)
{
	const v4f lane = { 0, 1, 2, 3 };
	const v4f bail = { BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ };
	const v4f civ = { ci, ci, ci, ci };
	for (int x = 0; x < width; x += 4)
	{
		v4f cr = left + scale * ((float)(first + x) + lane);
		v4f zr = { 0 }, zi = { 0 }, zr2 = { 0 }, zi2 = { 0 };
		v4i inside = { -1, -1, -1, -1 }, count = { 0 };
		for (int n = 0; n < max_iter; n++)
		{
			inside &= (zr2 + zi2) < bail;
			if (__builtin_ia32_movmskps((v4f)inside) == 0)
				break;
			v4f nzi = 2.0f * zr * zi + civ;
			v4f nzr = zr2 - zi2 + cr;
			zr = (v4f)(((v4i)nzr & inside) | ((v4i)zr & ~inside));
			zi = (v4f)(((v4i)nzi & inside) | ((v4i)zi & ~inside));
			zr2 = zr * zr;
			zi2 = zi * zi;
			count -= inside;               // inside is -1 per live lane
		}
		v4f mag = zr2 + zi2;
		for (int i = 0; i < 4; i++)
		{
			counts[x + i] = count[i];
			mags[x + i] = mag[i];
		}
	}
}

// The same loop at twice the width, with the complex multiply's 2*zr*zi+ci
// as one fused multiply-add. Compiled for AVX2 and FMA on its own, so the
// rest of the program stays the SSE2 every program is built for; called only
// after the CPU has said yes.
__attribute__((target("avx2,fma"), noinline))
static void row_avx2(float ci, float left, float scale, int first, int width, int max_iter,
                     int *counts, float *mags)
{
	const v8f lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const v8f bail = { BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ,
	                   BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ, BAILOUT_SQ };
	const v8f civ = { ci, ci, ci, ci, ci, ci, ci, ci };
	for (int x = 0; x < width; x += 8)
	{
		v8f cr = left + scale * ((float)(first + x) + lane);
		v8f zr = { 0 }, zi = { 0 }, zr2 = { 0 }, zi2 = { 0 };
		v8i inside = { -1, -1, -1, -1, -1, -1, -1, -1 }, count = { 0 };
		for (int n = 0; n < max_iter; n++)
		{
			inside &= (zr2 + zi2) < bail;
			if (__builtin_ia32_movmskps256((v8f)inside) == 0)
				break;
			v8f nzi = __builtin_ia32_vfmaddps256(zr + zr, zi, civ);
			v8f nzr = zr2 - zi2 + cr;
			zr = (v8f)(((v8i)nzr & inside) | ((v8i)zr & ~inside));
			zi = (v8f)(((v8i)nzi & inside) | ((v8i)zi & ~inside));
			zr2 = zr * zr;
			zi2 = zi * zi;
			count -= inside;
		}
		v8f mag = zr2 + zi2;
		for (int i = 0; i < 8; i++)
		{
			counts[x + i] = count[i];
			mags[x + i] = mag[i];
		}
	}
}

// ── Colour (fpu_demo's palette, in float) ───────────────────────────────────

// log2 from the float's own exponent bits plus a quartic for the mantissa:
// within 2e-3, a fiftieth of a palette step. Positive finite values only.
static float log2_approx(float x)
{
	union { float f; uint32_t u; } bits = { .f = x };
	int e = (int)((bits.u >> 23) & 0xFF) - 127;
	bits.u = (bits.u & 0x007FFFFFU) | 0x3F800000U;
	float t = bits.f - 1.0f;
	return (float)e + t * (1.4425f + t * (-0.7071f + t * (0.3597f - t * 0.0952f)));
}

#define PALETTE_PERIOD 48.0f
static const uint32_t kStops[] = { 0x000764, 0x206BCB, 0xEDFFFF, 0xFFAA00, 0x000200, 0x000764 };
#define STOPS ((int)(sizeof(kStops) / sizeof(kStops[0])))

static uint32_t palette(float mu)
{
	float f = mu / PALETTE_PERIOD;
	f -= (float)(int)f;
	if (f < 0.0f)
		f += 1.0f;
	float pos = f * (STOPS - 1);
	int i = (int)pos;
	float w = pos - (float)i;
	uint32_t a = kStops[i], b = kStops[i + 1];
	uint32_t r  = (uint32_t)(((a >> 16) & 255) * (1.0f - w) + ((b >> 16) & 255) * w);
	uint32_t g  = (uint32_t)(((a >> 8) & 255) * (1.0f - w) + ((b >> 8) & 255) * w);
	uint32_t bl = (uint32_t)((a & 255) * (1.0f - w) + (b & 255) * w);
	return 0xFF000000U | (r << 16) | (g << 8) | bl;
}

// 2^p for the zoom: 2^floor(p) times a short series for the fraction.
static float pow2f(float p)
{
	int ip = (int)p;
	float fr = p - (float)ip;
	float f = 1.0f + fr * (0.6931472f + fr * (0.2402265f + fr * (0.0555041f + fr * 0.0096181f)));
	union { float f; uint32_t u; } bits = { .u = (uint32_t)(ip + 127) << 23 };
	return bits.f * f;
}

// ── One window ──────────────────────────────────────────────────────────────

typedef struct {
	int which;                         // 0 = SSE2, 1 = AVX2
	const char *title;
	int32_t x, y;
	bool usable;
	const char *why_not;
} view_t;

// A row goes through the kernel a stretch at a time, so any surface width
// is drawn whole with fixed buffers. A multiple of eight, so every stretch
// starts on a vector of either width.
#define STRETCH 512

static void render(os64_draw_ctx_t *ctx, const view_t *v, const target_t *t, float log2zoom, int max_iter)
{
	static int   counts[2][STRETCH + 8];   // one set per window's thread
	static float mags[2][STRETCH + 8];
	int *count = counts[v->which];
	float *mag = mags[v->which];
	int width = (int)ctx->surf.width;
	int height = (int)ctx->surf.height - BAND_H;
	float scale = 3.2f / pow2f(log2zoom) / (float)width;
	float left = t->x - scale * (float)width / 2.0f;
	float top  = t->y - scale * (float)height / 2.0f;
	uint32_t *px = ctx->surf.pixels;
	uint32_t pitch = ctx->surf.pitch_px;
	for (int y = 0; y < height; y++)
	{
		float ci = top + scale * (float)y;
		uint32_t *out = &px[(uint32_t)(y + BAND_H) * pitch];
		for (int first = 0; first < width; first += STRETCH)
		{
			int n_px = width - first < STRETCH ? width - first : STRETCH;
			if (v->which == 1)
				row_avx2(ci, left, scale, first, n_px, max_iter, count, mag);
			else
				row_sse2(ci, left, scale, first, n_px, max_iter, count, mag);
			for (int x = 0; x < n_px; x++)
			{
				int n = count[x];
				if (n >= max_iter)
					out[first + x] = 0xFF000000U;   // inside: black
				else
					out[first + x] = palette((float)n + 1.0f -
					                         log2_approx(log2_approx(mag[x]) * 0.5f));
			}
		}
	}
}

#define NAP_MS    100   // covered or unusable: how often to look again
#define PAUSED_MS 33    // paused: poll keys at about thirty a second

// THE FLAG IS THE TRUTH (gui.h § OS64_GUI_WINDOW_COVERED): the COVERED and
// UNCOVERED events are only nudges, and a full queue can drop them.
static bool window_covered(int64_t win)
{
	os64_gui_window_state_t st;
	return os64_gui_window_get_state(win, &st) >= 0 && (st.flags & OS64_GUI_WINDOW_COVERED);
}

static void status(os64_draw_ctx_t *ctx, const char *text)
{
	os64_draw_fill_rect(&ctx->surf, (os64_gui_rect_t){ 0, 0, (int32_t)ctx->surf.width, BAND_H }, 0xFF000000U);
	os64_draw_text(&ctx->surf, 6, 2, text, os64_strlen(text), OS64_GUI_COLOR_WHITE, 0xFF000000U);
}

static int64_t run_view(void *arg)
{
	view_t *v = arg;
	int64_t win = os64_gui_window_create_content(v->title, v->x, v->y, VIEW_W, VIEW_H + BAND_H, 0);
	if (win <= 0)
	{
		s_tour.quit = true;
		if (win == OS64_GUI_ERR_NOT_RUNNING)
		{
			os64_printf("gmandel: no GUI on this boot\n");
			return 0;
		}
		os64_hprintf(OS64_STDERR, "gmandel: window create failed (%ld)\n", (long)win);
		return 1;
	}
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		s_tour.quit = true;
		os64_gui_window_destroy(win);
		return 1;
	}
	if (!v->usable)
	{
		os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){ 0, 0, (int32_t)ctx.surf.width,
		                    (int32_t)ctx.surf.height }, 0xFF101018U);
		status(&ctx, v->why_not);
		os64_draw_publish(&ctx, NULL);
	}

	int frames = 0;
	int64_t mark_us = os64_micros();
	int64_t last_frame_us = 0;
	int drawn_target = -1;
	float drawn_zoom = -1.0f;
	char line[128];
	while (!s_tour.quit)
	{
		os64_gui_event_t ev;
		int64_t erc;
		while ((erc = os64_gui_event_poll(win, &ev)) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_KEY_DOWN)
				tour_key(ev.key.ascii);
			else if (ev.type == OS64_GUI_EVENT_WINDOW_CLOSE)
				s_tour.quit = true;
			else if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE)
			{
				os64_draw_ctx_refresh(&ctx);
				drawn_target = -1;
				if (!v->usable)
				{
					os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){ 0, 0, (int32_t)ctx.surf.width,
					                    (int32_t)ctx.surf.height }, 0xFF101018U);
					status(&ctx, v->why_not);
					os64_draw_publish(&ctx, NULL);
				}
			}
		}
		if (erc < 0 || s_tour.quit)
			break;
		// Nobody watching this window, or nothing to race: nap, a bounded
		// one, then poll again. Not the frame clock's covered wait — that
		// sleeps until THIS window has an event, and a quit from the other
		// window would leave this thread asleep and the program unable to
		// end until something reached a window nobody can see.
		if (!v->usable || window_covered(win))
		{
			frames = 0;                    // a hidden second is not a slow one
			mark_us = os64_micros();
			os64_sleep(NAP_MS);
			continue;
		}

		int target;
		float log2zoom;
		bool paused;
		tour_now(&target, &log2zoom, &paused);
		int max_iter = ITER_BASE + (int)(ITER_PER_DOUBLING * log2zoom);
		// Paused, the picture is the last one: no frame is owed, and the
		// rate stays what it was while the race was on. Running, every pass
		// renders, even through the hold at the bottom of a dive where the
		// view stands still: the rates measure rendering, not change.
		if (!paused || target != drawn_target || log2zoom != drawn_zoom)
		{
			int64_t t0 = os64_micros();
			render(&ctx, v, &kTargets[target], log2zoom, max_iter);
			last_frame_us = os64_micros() - t0;
			drawn_target = target;
			drawn_zoom = log2zoom;
			frames++;
		}
		int64_t now = os64_micros();
		if (paused)
		{
			frames = 0;                    // the rate keeps its last value; a fresh
			mark_us = now;                 // second starts when the race does
		}
		else if (now - mark_us >= 1000000)
		{
			s_tour.fps_x10[v->which] = (int32_t)((int64_t)frames * 10000000 / (now - mark_us));
			frames = 0;
			mark_us = now;
		}
		int32_t mine = s_tour.fps_x10[v->which], other = s_tour.fps_x10[1 - v->which];
		// Short enough for VIEW_W in the 8-pixel font: the rate, the last
		// frame's cost, the depth, and then the coordinate's name in the
		// SSE2 window and the speed against SSE2 in the AVX one (both rates
		// are whole frames over a second).
		int n = os64_snprintf(line, sizeof(line), "%s  %d.%d fps  %ld ms  2^%d%s",
		                      v->which ? "AVX2 x8" : "SSE2 x4", mine / 10, mine % 10,
		                      (long)(last_frame_us / 1000), (int)log2zoom,
		                      paused ? "  [paused]" : "");
		if (v->which == 0 && !paused)
			os64_snprintf(line + n, sizeof(line) - (size_t)n, "  %s", kTargets[target].name);
		else if (v->which == 1 && other > 0 && !paused)
		{
			int32_t ratio_x10 = mine * 10 / other;
			os64_snprintf(line + n, sizeof(line) - (size_t)n, "  %d.%dx SSE2",
			              ratio_x10 / 10, ratio_x10 % 10);
		}
		status(&ctx, line);
		os64_draw_publish(&ctx, NULL);
		// Racing, no pause between frames: the race is how fast each can go.
		if (paused)
			os64_sleep(PAUSED_MS);
	}
	os64_gui_window_destroy(win);
	return 0;
}

// What the CPU says about AVX2 and FMA, as avxprobe asks it.
static const char *avx_refusal(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	if (!(c & (1U << 28)))
		return "AVX2 x8: this CPU has no AVX";
	if (!(c & (1U << 27)))
		return "AVX2 x8: AVX is off on this boot (sse mode)";
	if (!(c & (1U << 12)))
		return "AVX2 x8: this CPU has no FMA";
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	if ((lo & 0x6U) != 0x6U)
		return "AVX2 x8: the kernel did not enable AVX state";
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
	if (!(b & (1U << 5)))
		return "AVX2 x8: this CPU has no AVX2";
	return NULL;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	uint32_t sw = 0, sh = 0;
	os64_gui_screen_info(&sw, &sh);
	int32_t x0 = 40, y0 = 60;
	int32_t x1 = x0 + VIEW_W + GAP, y1 = y0;
	if ((uint32_t)(x1 + VIEW_W) > sw)          // too narrow for side by side: stack them
	{
		x1 = x0;
		y1 = y0 + VIEW_H + BAND_H + GAP + 24;
	}
	s_tour.phase_start_us = os64_micros();

	const char *refusal = avx_refusal();
	// Titles fit OS64_GUI_TITLE_MAX, which refuses a longer one outright.
	static view_t sse = { 0, "gmandel: SSE2, 4 lanes", 0, 0, true, NULL };
	static view_t avx = { 1, "gmandel: AVX2, 8 lanes", 0, 0, true, NULL };
	sse.x = x0; sse.y = y0;
	avx.x = x1; avx.y = y1;
	avx.usable = refusal == NULL;
	avx.why_not = refusal;

	int64_t other = os64_thread(run_view, &avx);
	int64_t rc = run_view(&sse);
	s_tour.quit = true;
	if (other >= 0)
	{
		int64_t orc = 0;
		os64_thread_join((int32_t)other, &orc);
		if (rc == 0)
			rc = orc;
	}
	return (int)rc;
}
