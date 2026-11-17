// glassbench — what a full-window publisher at a video's rate costs, and how
// evenly its frames reach the glass (docs/design/pending/VIDEO.md § 7).
//
//   glassbench [-w WxH | -m] [-r fps] [-s seconds] [-p sleep|spin] [-o file]
//   glassbench all [-o file]
//
// One window (-m maximized, the default, or -w a content size) shows a test
// card for the eye: a vertical bar that moves a fixed step each frame over a
// plain field, and the frame number drawn large. Uneven pacing shows as a
// bar that stutters, tearing as a bar that breaks.
//
// Each frame does what a video player does after it has a decoded picture:
// it copies a ready frame into the window's canvas with libos64's memcpy and
// publishes the whole content. Two frames are prepared in memory before the
// timing starts, and between publishes the next one is updated in place
// (the bar's old place cleared, its new place and the number drawn), which
// is outside everything timed. Then it waits for the next frame time:
//
//   sleep  os64_sleep to the frame's time, as an ordinary program would; the
//          sleep is on the tick clock, so its lateness is part of the answer
//   spin   busy-wait on os64_micros to an exact schedule, so the publisher
//          is known to be on time and what unevenness remains is the
//          compositor's
//
// A frame that starts more than a whole period late moves the schedule to
// one period from now (a resync, counted) rather than queueing frames
// behind it.
//
// Per frame, with os64_micros: the copy, the publish call, and how late the
// frame started. Around the run, /sys/gui is read before and after and the
// differences reported: the publish and composite sites' lock holds, the
// compositor's passes and pixels, and the publish-to-glass waits. The
// before-read also restarts /sys/gui's since-read worsts, so the after-read's
// are the run's own (if nothing else reads the file meanwhile). The /sys/gui
// numbers are the whole desktop's: another window publishing during a run
// adds its waits and holds to the run's, so run it on an idle desktop.
//
// Each run's result is printed, sent to the serial wire, and appended as ONE
// line to -o (default /home/glassbench.txt). `all` runs the set the video design needs, ten
// seconds each. Escape or a close request ends the run (and `all`).
// Exit 0 when every run finished, 1 on a failure to start, 2 if stopped.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/slurp.h"

#define BAR_W        48
#define BAR_STEP     8                 // pixels a frame
#define FIELD_COLOR  0xFF203048U
#define BAR_COLOR    0xFFF0F0F0U
#define DIGIT_COLOR  0xFFFFC040U
#define DIGIT_SCALE  14                // one font cell, in pixels
#define DIGITS       6
// After the last publish, before the after-read: the compositor counts a
// pass's waits at its next pass, and an idle compositor wakes about ten
// times a second.
#define SETTLE_MS    250

typedef struct {
	bool     maximized;
	uint32_t w, h;                     // content size asked, when not maximized
	uint32_t fps;
	uint32_t seconds;
	bool     spin;
} run_cfg_t;

static const char *s_out = "/home/glassbench.txt";

// ── /sys/gui ────────────────────────────────────────────────────────────────

// The numbers on the /sys/gui line that begins with `prefix`, in order (every
// run of digits is one), into `out`; how many there were, or 0 without the
// line.
static bool starts_with(const char *s, const char *prefix, size_t plen)
{
	for (size_t i = 0; i < plen; i++)
		if (s[i] != prefix[i])
			return false;
	return true;
}

static int gui_line_numbers(const char *text, const char *prefix, uint64_t *out, int cap)
{
	size_t plen = os64_strlen(prefix);
	const char *line = text;
	while (*line)
	{
		if (starts_with(line, prefix, plen))
		{
			int n = 0;
			const char *p = line + plen;
			while (*p && *p != '\n' && n < cap)
			{
				if (*p >= '0' && *p <= '9')
				{
					uint64_t v = 0;
					while (*p >= '0' && *p <= '9')
						v = v * 10 + (uint64_t)(*p++ - '0');
					out[n++] = v;
				}
				else
					p++;
			}
			return n;
		}
		while (*line && *line != '\n')
			line++;
		if (*line)
			line++;
	}
	return 0;
}

// The parts of /sys/gui a run compares.
typedef struct {
	uint64_t publish[6], composite[6];  // acquisitions, waited, us waiting, us holding, worst wait, worst hold
	uint64_t comp[5];                   // passes, drew, rectangles, pixels, collapses
	uint64_t glass[20];                 // waits, us waiting, average, longest, longest since read, voided, then (label, count) x7
	bool     have_glass;
} gui_reading_t;

static bool read_gui(gui_reading_t *r)
{
	uint8_t *text = NULL;
	size_t len = 0;
	if (os64_slurp("/sys/gui", 65536, &text, &len) != OS64_SLURP_OK)
		return false;
	os64_memset(r, 0, sizeof(*r));
	gui_line_numbers((const char *)text, "lock publish:", r->publish, 6);
	gui_line_numbers((const char *)text, "lock composite:", r->composite, 6);
	gui_line_numbers((const char *)text, "compositor: ", r->comp, 5);
	r->have_glass = gui_line_numbers((const char *)text, "publish to glass:", r->glass, 20) == 20;
	os64_free(text);
	return true;
}

// ── The test card ───────────────────────────────────────────────────────────

// A 3x5 cell for each digit, drawn DIGIT_SCALE pixels a cell: large enough to
// read a frame number off a photograph of the glass.
static const uint8_t kDigits[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
	{ 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

typedef struct { uint32_t *pixels; uint32_t w, h; } frame_t;

static void fill(frame_t *f, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int32_t)f->w) w = (int32_t)f->w - x;
	if (y + h > (int32_t)f->h) h = (int32_t)f->h - y;
	for (int32_t r = 0; r < h; r++)
	{
		uint32_t *row = f->pixels + (size_t)(y + r) * f->w + x;
		for (int32_t c = 0; c < w; c++)
			row[c] = color;
	}
}

static int32_t bar_x(const frame_t *f, uint32_t frame)
{
	uint32_t span = f->w > BAR_W ? f->w - BAR_W : 1;
	return (int32_t)((frame * BAR_STEP) % span);
}

static void draw_number(frame_t *f, uint32_t n)
{
	int32_t x0 = DIGIT_SCALE, y0 = DIGIT_SCALE;
	fill(f, x0, y0, DIGITS * 4 * DIGIT_SCALE, 5 * DIGIT_SCALE, FIELD_COLOR);
	for (int d = DIGITS - 1; d >= 0; d--)
	{
		uint32_t digit = n % 10;
		n /= 10;
		for (int row = 0; row < 5; row++)
			for (int col = 0; col < 3; col++)
				if (kDigits[digit][row] & (4 >> col))
					fill(f, x0 + (d * 4 + col) * DIGIT_SCALE, y0 + row * DIGIT_SCALE,
					     DIGIT_SCALE, DIGIT_SCALE, DIGIT_COLOR);
	}
}

// Make `f` show frame `next`; it last showed `prev` (or nothing, prev < 0).
static void card_update(frame_t *f, int64_t prev, uint32_t next)
{
	if (prev >= 0)
		fill(f, bar_x(f, (uint32_t)prev), 0, BAR_W, (int32_t)f->h, FIELD_COLOR);
	fill(f, bar_x(f, next), 0, BAR_W, (int32_t)f->h, BAR_COLOR);
	draw_number(f, next);
}

// The result line to the serial wire too, for a headless harness. The
// kernel refuses a log line of 256 bytes or more, so a long one goes in
// pieces cut at its "; " separators where it can and at a space where it
// cannot, each after the first marked "+".
#define WIRE_PIECE 200

static void wire(const char *line)
{
	char piece[WIRE_PIECE + 32];
	size_t len = os64_strlen(line), at = 0;
	bool first = true;
	while (at < len)
	{
		size_t take = len - at;
		if (take > WIRE_PIECE)
		{
			// At a "; " if there is one in the second half, else at a space.
			size_t space = 0;
			take = WIRE_PIECE;
			for (size_t i = WIRE_PIECE; i > WIRE_PIECE / 2; i--)
			{
				if (line[at + i] == ';' && line[at + i + 1] == ' ')
				{
					take = i + 1;
					space = 0;
					break;
				}
				if (space == 0 && line[at + i] == ' ')
					space = i;
			}
			if (take == WIRE_PIECE && space != 0)
				take = space;
		}
		piece[0] = '\0';
		if (!first)
			os64_strcopy(piece, sizeof(piece), "glassbench + ");
		size_t n = os64_strlen(piece);
		for (size_t i = 0; i < take && n < sizeof(piece) - 1; i++)
			piece[n++] = line[at + i];
		piece[n] = '\0';
		os64_serial_log(piece);
		at += take;
		while (at < len && line[at] == ' ')
			at++;
		first = false;
	}
}

// ── One run ─────────────────────────────────────────────────────────────────

typedef struct {
	uint64_t frames, resyncs, late_over_2ms;
	uint64_t copy_sum, copy_max, pub_sum, pub_max, late_sum, late_max;
	int64_t  elapsed_us;
} run_stats_t;

static void wait_until(int64_t when, bool spin)
{
	if (spin)
	{
		while (os64_micros() < when)
			;
		return;
	}
	int64_t remaining = when - os64_micros();
	if (remaining > 0)
		os64_sleep((uint64_t)((remaining + 999) / 1000));
}

static void copy_into(os64_gui_surface_t *dst, const frame_t *src)
{
	uint32_t w = dst->width < src->w ? dst->width : src->w;
	uint32_t h = dst->height < src->h ? dst->height : src->h;
	if (dst->pitch_px == src->w && w == src->w)
	{
		os64_memcpy(dst->pixels, src->pixels, (size_t)w * h * 4);
		return;
	}
	for (uint32_t y = 0; y < h; y++)
		os64_memcpy(dst->pixels + (size_t)y * dst->pitch_px, src->pixels + (size_t)y * src->w, (size_t)w * 4);
}

static uint64_t diff(uint64_t after, uint64_t before)
{
	return after >= before ? after - before : 0;
}

// 0 = finished, 1 = could not start, 2 = stopped by Escape or a close.
static int run_one(const run_cfg_t *cfg)
{
	uint32_t sw = 0, sh = 0;
	os64_gui_screen_info(&sw, &sh);
	int64_t win;
	if (cfg->maximized)
		win = os64_gui_window_create("glassbench", 40, 40, sw / 2, sh / 2, OS64_GUI_CREATE_MAXIMIZED);
	else
	{
		int32_t x = sw > cfg->w ? (int32_t)(sw - cfg->w) / 2 : 0;
		int32_t y = sh > cfg->h ? (int32_t)(sh - cfg->h) / 2 : 0;
		win = os64_gui_window_create_content("glassbench", x, y, cfg->w, cfg->h, 0);
	}
	if (win == OS64_GUI_ERR_NOT_RUNNING)
	{
		os64_printf("glassbench: no GUI on this boot\n");
		return 1;
	}
	if (win <= 0)
	{
		os64_hprintf(OS64_STDERR, "glassbench: window create failed (%ld)\n", (long)win);
		return 1;
	}
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_gui_window_destroy(win);
		return 1;
	}

	// Let a maximize or placement settle before the size is taken.
	os64_sleep(100);
	os64_gui_event_t ev;
	while (os64_gui_event_poll(win, &ev) == 1)
		if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE)
			os64_draw_ctx_refresh(&ctx);

	frame_t frames[2];
	for (int i = 0; i < 2; i++)
	{
		frames[i].w = ctx.surf.width;
		frames[i].h = ctx.surf.height;
		frames[i].pixels = os64_malloc((size_t)frames[i].w * frames[i].h * 4);
		if (!frames[i].pixels)
		{
			os64_hprintf(OS64_STDERR, "glassbench: no memory for a %ux%u frame\n", frames[i].w, frames[i].h);
			if (i > 0)
				os64_free(frames[0].pixels);
			os64_gui_window_destroy(win);
			return 1;
		}
		fill(&frames[i], 0, 0, (int32_t)frames[i].w, (int32_t)frames[i].h, FIELD_COLOR);
	}
	card_update(&frames[0], -1, 0);
	card_update(&frames[1], -1, 1);

	gui_reading_t before, after;
	bool have_gui = read_gui(&before);

	run_stats_t st;
	os64_memset(&st, 0, sizeof(st));
	// The run lasts its seconds of wall time, however many frames that is: a
	// publisher that cannot keep up shows as a lower rate, not a longer run.
	int64_t period = 1000000 / (int64_t)cfg->fps;
	int64_t start = os64_micros() + period;
	int64_t end = start + (int64_t)cfg->seconds * 1000000;
	int64_t when = start;
	int stopped = 0;
	for (uint64_t k = 0; when < end && !stopped; k++)
	{
		wait_until(when, cfg->spin);
		int64_t t0 = os64_micros();
		int64_t late = t0 - when;
		if (late < 0)
			late = 0;
		frame_t *f = &frames[k & 1];
		copy_into(&ctx.surf, f);
		int64_t t1 = os64_micros();
		os64_draw_publish(&ctx, NULL);
		int64_t t2 = os64_micros();

		st.frames++;
		st.copy_sum += (uint64_t)(t1 - t0);
		st.pub_sum += (uint64_t)(t2 - t1);
		st.late_sum += (uint64_t)late;
		if ((uint64_t)(t1 - t0) > st.copy_max) st.copy_max = (uint64_t)(t1 - t0);
		if ((uint64_t)(t2 - t1) > st.pub_max) st.pub_max = (uint64_t)(t2 - t1);
		if ((uint64_t)late > st.late_max) st.late_max = (uint64_t)late;
		if (late > 2000)
			st.late_over_2ms++;

		// The buffer just shown becomes frame k + 2, outside the timing.
		card_update(f, (int64_t)k, (uint32_t)(k + 2));

		while (os64_gui_event_poll(win, &ev) == 1)
		{
			if (ev.type == OS64_GUI_EVENT_WINDOW_CLOSE ||
			    (ev.type == OS64_GUI_EVENT_KEY_DOWN && (ev.key.ascii == 27 || ev.key.scancode == 0x01)))
				stopped = 1;
			else if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE)
				os64_draw_ctx_refresh(&ctx);
		}

		if (late > period)
		{
			when = os64_micros() + period;
			st.resyncs++;
		}
		else
			when += period;
	}
	st.elapsed_us = os64_micros() - start;

	os64_sleep(SETTLE_MS);   // the compositor counts a pass's waits at its next pass
	if (have_gui)
		have_gui = read_gui(&after);

	// Each piece is appended where the last one ended, and the length is
	// taken from the buffer after each, so a line that ever outgrew it would
	// be cut short rather than written past.
	char line[640];
	uint64_t n = st.frames ? st.frames : 1;
	uint64_t rate_x10 = st.elapsed_us > 0 ? st.frames * 10000000ull / (uint64_t)st.elapsed_us : 0;
	size_t len = 0;
	os64_snprintf(line, sizeof(line),
		"glassbench %ux%u%s %ufps %s: %lu frames, %lu.%lu fps; copy %lu/%lu us; publish %lu/%lu us; "
		"late %lu/%lu us, %lu over 2 ms, %lu resyncs",
		frames[0].w, frames[0].h, cfg->maximized ? " max" : "", cfg->fps, cfg->spin ? "spin" : "sleep",
		st.frames, rate_x10 / 10, rate_x10 % 10, st.copy_sum / n, st.copy_max,
		st.pub_sum / n, st.pub_max, st.late_sum / n, st.late_max, st.late_over_2ms, st.resyncs);
	len = os64_strlen(line);
	if (have_gui)
	{
		os64_snprintf(line + len, sizeof(line) - (size_t)len,
			"; lock publish %lu us held, worst %lu; lock composite %lu us held, worst %lu; "
			"compositor %lu passes, %lu drew, %lu pixels",
			diff(after.publish[3], before.publish[3]), after.publish[5],
			diff(after.composite[3], before.composite[3]), after.composite[5],
			diff(after.comp[0], before.comp[0]), diff(after.comp[1], before.comp[1]),
			diff(after.comp[3], before.comp[3]));
		len = os64_strlen(line);
		if (after.have_glass && before.have_glass)
		{
			uint64_t waits = diff(after.glass[0], before.glass[0]);
			uint64_t us = diff(after.glass[1], before.glass[1]);
			os64_snprintf(line + len, sizeof(line) - (size_t)len,
				"; glass %lu waits, %lu us avg, worst %lu, %lu voided, by ms <1/2/4/8/16/32/more "
				"%lu/%lu/%lu/%lu/%lu/%lu/%lu",
				waits, waits ? us / waits : 0, after.glass[4], diff(after.glass[5], before.glass[5]),
				diff(after.glass[7], before.glass[7]), diff(after.glass[9], before.glass[9]),
				diff(after.glass[11], before.glass[11]), diff(after.glass[13], before.glass[13]),
				diff(after.glass[15], before.glass[15]), diff(after.glass[17], before.glass[17]),
				diff(after.glass[19], before.glass[19]));
		}
		else
			os64_snprintf(line + len, sizeof(line) - (size_t)len, "; glass: no publish-to-glass line");
		len = os64_strlen(line);
	}
	if (stopped)
		os64_snprintf(line + len, sizeof(line) - (size_t)len, " [stopped]");
	len = os64_strlen(line);
	os64_printf("%s\n", line);
	wire(line);

	int64_t h = os64_open(s_out, "a");
	if (h < 0)
		h = os64_open(s_out, "x");
	if (h >= 0)
	{
		if (len >= sizeof(line) - 1)
			len = sizeof(line) - 2;
		line[len++] = '\n';
		os64_write((int32_t)h, line, len);
		os64_close((int32_t)h);
	}
	else
		os64_hprintf(OS64_STDERR, "glassbench: could not append to %s\n", s_out);

	os64_free(frames[0].pixels);
	os64_free(frames[1].pixels);
	os64_gui_window_destroy(win);
	return stopped ? 2 : 0;
}

// ── The arguments ───────────────────────────────────────────────────────────

static bool parse_size(const char *s, uint32_t *w, uint32_t *h)
{
	uint32_t a = 0, b = 0;
	while (*s >= '0' && *s <= '9')
		a = a * 10 + (uint32_t)(*s++ - '0');
	if (*s != 'x' && *s != 'X')
		return false;
	s++;
	while (*s >= '0' && *s <= '9')
		b = b * 10 + (uint32_t)(*s++ - '0');
	if (*s || a == 0 || b == 0)
		return false;
	*w = a;
	*h = b;
	return true;
}

static uint32_t parse_u32(const char *s)
{
	uint32_t v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (uint32_t)(*s++ - '0');
	return v;
}

static int usage(void)
{
	os64_printf("usage: glassbench [-w WxH | -m] [-r fps] [-s seconds] [-p sleep|spin] [-o file]\n"
	            "       glassbench all [-o file]\n");
	return 1;
}

int main(int argc, char **argv)
{
	run_cfg_t cfg = { .maximized = true, .w = 0, .h = 0, .fps = 60, .seconds = 10, .spin = false };
	bool all = false;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (os64_streq(a, "all"))
			all = true;
		else if (os64_streq(a, "-m"))
			cfg.maximized = true;
		else if (os64_streq(a, "-w") && v)
		{
			if (!parse_size(v, &cfg.w, &cfg.h))
				return usage();
			cfg.maximized = false;
			i++;
		}
		else if (os64_streq(a, "-r") && v)
		{
			cfg.fps = parse_u32(v);
			i++;
		}
		else if (os64_streq(a, "-s") && v)
		{
			cfg.seconds = parse_u32(v);
			i++;
		}
		else if (os64_streq(a, "-p") && v)
		{
			if (os64_streq(v, "spin"))
				cfg.spin = true;
			else if (os64_streq(v, "sleep"))
				cfg.spin = false;
			else
				return usage();
			i++;
		}
		else if (os64_streq(a, "-o") && v)
		{
			s_out = v;
			i++;
		}
		else
			return usage();
	}
	if (cfg.fps == 0 || cfg.fps > 1000 || cfg.seconds == 0)
		return usage();

	if (!all)
		return run_one(&cfg);

	// The set VIDEO.md § 7 needs: the full screen at both rates and both
	// pacers, then two smaller windows on the exact schedule.
	static const run_cfg_t kSet[] = {
		{ true,  0,    0,   30, 10, false },
		{ true,  0,    0,   30, 10, true  },
		{ true,  0,    0,   60, 10, false },
		{ true,  0,    0,   60, 10, true  },
		{ false, 1280, 720, 30, 10, true  },
		{ false, 1280, 720, 60, 10, true  },
		{ false, 640,  360, 60, 10, true  },
	};
	for (size_t i = 0; i < sizeof(kSet) / sizeof(kSet[0]); i++)
	{
		int rc = run_one(&kSet[i]);
		if (rc != 0)
			return rc;
	}
	return 0;
}
