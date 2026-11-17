// test_frame_clock_host.c — the frame clock's arithmetic (kernel/src/gui/
// frame_clock.c) beside a model display, under ASan and UBSan.
//
// THE MODEL. A display with `lines` lines a frame, each `line_ns` long, and a
// counter that shows (phase + n) % lines during line n of the model's time.
// A copier writes pieces one after another, each row by row from its top, at
// one speed in pixels. The model decides tearing by brute force: it walks
// every line the display reads around the copy, in nanoseconds, asks whether
// that row's write had finished by then, and calls a piece torn when one
// sweep read it part old and part new. The judge must agree with it on every
// copy, seeing what the kernel will see: two counter readings and the time
// between them. The model's copies start and end on line boundaries, where a
// reading is exact; a real reading can be up to a line late, which changes a
// verdict only for a copy within about a line of tearing, and that is not
// exercised here. A copy lasting a frame or more is torn by rule, which the
// model does not know, so those are checked against the rule.
//
//   tools/test_frame_clock_host.sh [random rounds, default 3000]

#include "gui/frame_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                                        \
	do {                                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_failures++;                                                       \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                \
			fprintf(stderr, __VA_ARGS__);                                       \
			fprintf(stderr, "\n");                                              \
			if (s_failures > 20) exit(1);                                       \
		}                                                                       \
	} while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return s_rng;
}
static int64_t rnd_in(int64_t lo, int64_t hi)   // [lo, hi]
{
	return lo + (int64_t)(rnd() % (uint64_t)(hi - lo + 1));
}

// ── The model display ───────────────────────────────────────────────────────

typedef struct {
	uint32_t lines, blank_start, blank_end;
	uint64_t line_ns;
	uint64_t phase;     // the counter during model line 0
} display_t;

static fc_frame_t frame_of(const display_t *d)
{
	return (fc_frame_t){ d->lines, d->blank_start, d->blank_end, d->line_ns * d->lines };
}

static uint32_t counter_at_line(const display_t *d, uint64_t n)
{
	return (uint32_t)((d->phase + n) % d->lines);
}

static uint32_t shown_rows(const display_t *d)
{
	return (d->blank_start + d->lines - d->blank_end) % d->lines;
}

// The screen row read during model line n, or -1 in the blank; and which
// sweep of the display that read belongs to (a sweep starts at row 0).
static int64_t row_read(const display_t *d, uint64_t n, int64_t *sweep_id)
{
	uint64_t since_row0 = d->phase + n + d->lines - d->blank_end;   // never negative
	*sweep_id = (int64_t)(since_row0 / d->lines);
	uint64_t s = since_row0 % d->lines;
	return s < shown_rows(d) ? (int64_t)s : -1;
}

typedef struct {
	int32_t top, bottom;
	uint64_t pixels;
} piece_t;

// The copy starts at the start of model line `start` and lasts `dur_lines`
// lines; each piece takes its share by pixels, and its row k is finished at
// piece_start + (k + 1) * piece_duration / height. Times are compared
// exactly: everything is scaled by total pixels and by the piece's height.
static bool model_torn(const display_t *d, uint64_t start, uint64_t dur_lines,
                       const piece_t *p, uint32_t count)
{
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; i++)
		total += p[i].pixels;
	if (total == 0)
		return false;
	__int128 t0 = (__int128)start * d->line_ns, dur = (__int128)dur_lines * d->line_ns;
	uint64_t done = 0;
	for (uint32_t i = 0; i < count; i++) {
		// The piece runs over [t0 + dur*done/total, t0 + dur*(done+px)/total].
		__int128 a_num = t0 * total + dur * done;            // over `total`
		__int128 len_num = dur * p[i].pixels;                // over `total`
		done += p[i].pixels;
		if (p[i].pixels == 0 || p[i].top >= p[i].bottom)
			continue;
		int64_t height = p[i].bottom - p[i].top;
		// Walk a frame before the copy to a frame after it.
		int64_t from = (int64_t)start - (int64_t)d->lines - 2;
		if (from < 0)
			from = 0;
		uint64_t to = start + dur_lines + 2 * d->lines + 2;
		int64_t cur_sweep = -1;
		bool seen_new = false, seen_old = false;
		for (uint64_t n = (uint64_t)from; n <= to; n++) {
			int64_t sweep;
			int64_t r = row_read(d, n, &sweep);
			if (sweep != cur_sweep) {
				if (seen_new && seen_old)
					return true;
				cur_sweep = sweep;
				seen_new = seen_old = false;
			}
			if (r < p[i].top || r >= p[i].bottom || r < 0)
				continue;
			// Row r is finished at a + len*(r + 1 - top)/height; read at n*line_ns.
			__int128 done_at = a_num * height + len_num * (r + 1 - p[i].top);   // over total*height
			__int128 read_at = (__int128)n * d->line_ns * total * height;
			// A copy of no length finishes as it starts, after the read of
			// the line it starts on.
			bool is_new = len_num == 0 ? read_at > a_num * height : done_at <= read_at;
			if (is_new) seen_new = true; else seen_old = true;
		}
		if (seen_new && seen_old)
			return true;
	}
	return false;
}

static fc_verdict_t judge_model(const display_t *d, uint64_t start, uint64_t dur_lines,
                                const piece_t *p, uint32_t count)
{
	fc_frame_t f = frame_of(d);
	fc_piece_t pieces[512];
	if (count > 512)
		abort();
	for (uint32_t i = 0; i < count; i++)
		pieces[i] = (fc_piece_t){ p[i].top, p[i].bottom, p[i].pixels };
	uint64_t elapsed_us = dur_lines * d->line_ns / 1000;
	return frame_clock_judge(&f, counter_at_line(d, start), counter_at_line(d, start + dur_lines),
	                         elapsed_us, pieces, count);
}

// The P5's mode: 1481 lines, the picture on counter lines 38..1477.
static const display_t P5 = { 1481, 1478, 38, 11267, 0 };

// A copier that moves a full 2560x1440 screen in `full_lines` lines.
static uint64_t copy_lines(uint64_t pixels, uint64_t full_lines)
{
	return (pixels * full_lines + 2560ull * 1440 - 1) / (2560ull * 1440);
}

// The model line at which the counter shows `line`, at or after `after`.
static uint64_t line_when(const display_t *d, uint32_t line, uint64_t after)
{
	uint64_t n = after;
	while (counter_at_line(d, n) != line)
		n++;
	return n;
}

// ── The frame's facts ──────────────────────────────────────────────────────

static void test_frame_facts(void)
{
	fc_frame_t f = frame_of(&P5);
	CHECK(frame_clock_frame_valid(&f), "the P5's frame is valid");
	CHECK(frame_clock_active_rows(&f) == 1440, "the P5 shows 1440 rows, got %u", frame_clock_active_rows(&f));
	CHECK(frame_clock_sweep(&f, 38) == 0, "counter 38 is row 0");
	CHECK(frame_clock_sweep(&f, 1477) == 1439, "counter 1477 is row 1439");
	CHECK(frame_clock_sweep(&f, 1478) == 1440, "counter 1478 is the blank's first");
	CHECK(frame_clock_sweep(&f, 0) == 1443, "counter 0 is three into the blank");
	CHECK(!frame_clock_in_blank(&f, 38) && !frame_clock_in_blank(&f, 1477), "the picture is not the blank");
	CHECK(frame_clock_in_blank(&f, 1478) && frame_clock_in_blank(&f, 1480) &&
	      frame_clock_in_blank(&f, 0) && frame_clock_in_blank(&f, 37), "the blank wraps through 0");
	CHECK(frame_clock_line_ok(&f, 0) && frame_clock_line_ok(&f, 1480), "lines of the frame are lines");
	CHECK(!frame_clock_line_ok(&f, 1481), "a line beyond the frame is not");
	CHECK(!frame_clock_line_ok(&f, 0xFFFFFFFFu), "all ones is not");
	CHECK(frame_clock_band(&f, 38) == 0 && frame_clock_band(&f, 1477) == 7, "the first and last rows' bands");
	CHECK(frame_clock_band(&f, 1478) == FC_BAND_BLANK && frame_clock_band(&f, 10) == FC_BAND_BLANK, "the blank's band");
	CHECK(frame_clock_band(&f, 38 + 180) == 1 && frame_clock_band(&f, 38 + 179) == 0, "an eighth is 180 rows");

	// A blank that does not wrap.
	fc_frame_t g = { 1000, 900, 950, 16000000 };
	CHECK(frame_clock_active_rows(&g) == 950, "a blank of 50 leaves 950, got %u", frame_clock_active_rows(&g));
	CHECK(frame_clock_sweep(&g, 950) == 0 && frame_clock_sweep(&g, 899) == 949, "rows run on through the wrap");
	CHECK(frame_clock_in_blank(&g, 900) && frame_clock_in_blank(&g, 949) && !frame_clock_in_blank(&g, 950),
	      "the blank is [900, 950)");

	fc_frame_t bad[] = {
		{ 1, 0, 0, 1000 }, { 40000, 1, 2, 1000 }, { 1000, 1000, 2, 1000 }, { 1000, 2, 1000, 1000 },
		{ 1000, 5, 5, 1000 }, { 1000, 1, 2, 0 }, { 1000, 1, 2, 2000000000ull },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(!frame_clock_frame_valid(&bad[i]), "bad frame %zu refused", i);
		CHECK(frame_clock_active_rows(&bad[i]) == 0, "bad frame %zu shows nothing", i);
	}

	// Bands: every eighth, in order, about the same size, over random frames.
	for (int round = 0; round < 200; round++) {
		uint32_t lines = (uint32_t)rnd_in(20, 3000);
		uint32_t bs = (uint32_t)rnd_in(0, lines - 1);
		uint32_t len = (uint32_t)rnd_in(1, lines / 2);
		fc_frame_t h = { lines, bs, (bs + len) % lines, 16000000 };
		uint32_t active = frame_clock_active_rows(&h);
		CHECK(active == lines - len, "active rows %u of %u with a %u blank", active, lines, len);
		uint32_t per[FC_BANDS] = { 0 }, last = 0;
		for (uint32_t s = 0; s < lines; s++) {
			uint32_t line = (h.blank_end + s) % lines;
			uint32_t b = frame_clock_band(&h, line);
			CHECK(frame_clock_sweep(&h, line) == s, "sweep of line %u", line);
			CHECK(frame_clock_in_blank(&h, line) == (s >= active), "in the blank at sweep %u", s);
			if (s < active) {
				CHECK(b < 8 && b >= last, "bands rise down the screen");
				last = b;
			} else {
				CHECK(b == FC_BAND_BLANK, "the blank is its own band");
			}
			per[b]++;
		}
		if (active >= 8)
			for (int b = 0; b < 8; b++)
				CHECK(per[b] >= active / 8 && per[b] <= active / 8 + 1, "band %d holds %u of %u rows", b, per[b], active);
		CHECK(per[FC_BAND_BLANK] == len, "the blank band holds the blank");
	}
}

static void test_next_blank(void)
{
	fc_frame_t f = frame_of(&P5);
	uint64_t period = f.period_ns;
	CHECK(frame_clock_next_blank_us(&f, 1478, 1000) == 1000, "on the blank's first line it is now");
	// One line before: one line away, rounded down.
	CHECK(frame_clock_next_blank_us(&f, 1477, 1000) == 1000 + 11267 / 1000, "a line before");
	// In the blank but not on its first line: the next frame's.
	uint64_t next = frame_clock_next_blank_us(&f, 1479, 0);
	CHECK(next == (1480ull * period / 1481) / 1000, "inside the blank waits a frame less a line, got %lu", next);
	for (int round = 0; round < 2000; round++) {
		display_t d = { (uint32_t)rnd_in(20, 3000), 0, 0, (uint64_t)rnd_in(2000, 60000), 0 };
		d.blank_start = (uint32_t)rnd_in(0, d.lines - 1);
		d.blank_end = (d.blank_start + (uint32_t)rnd_in(1, d.lines / 2)) % d.lines;
		fc_frame_t g = frame_of(&d);
		uint32_t line = (uint32_t)rnd_in(0, d.lines - 1);
		uint64_t now = (uint64_t)rnd_in(0, 1000000000);
		// The model: line starts at 0 ns; the blank begins `k` lines later.
		uint64_t k = 0;
		while ((line + k) % d.lines != d.blank_start)
			k++;
		uint64_t exact_ns = now * 1000 + k * d.line_ns;
		uint64_t got = frame_clock_next_blank_us(&g, line, now);
		CHECK(got * 1000 <= exact_ns && exact_ns - got * 1000 < 1000,
		      "next blank %lu us, exactly %lu ns (line %u, blank at %u)", got, exact_ns, line, d.blank_start);
	}
}

// ── The verdict ─────────────────────────────────────────────────────────────

static void test_verdict_cases(void)
{
	const uint64_t full = 2560ull * 1440, full_lines = 160;   // ~1.8 ms at the P5's line
	piece_t screen = { 0, 1440, full };

	// From the blank's first line, a full screen leads the sweep down: clean.
	uint64_t at = line_when(&P5, 1478, 5000);
	fc_verdict_t v = judge_model(&P5, at, full_lines, &screen, 1);
	CHECK(!v.torn && v.began_in_blank && v.band == FC_BAND_BLANK, "a copy from the blank is clean");
	CHECK(!model_torn(&P5, at, full_lines, &screen, 1), "and the model agrees");

	// From mid-screen it overtakes the sweep: torn.
	at = line_when(&P5, 38 + 400, 5000);
	v = judge_model(&P5, at, full_lines, &screen, 1);
	CHECK(v.torn && !v.began_in_blank && v.band == 2, "a copy from row 400 tears");
	CHECK(model_torn(&P5, at, full_lines, &screen, 1), "and the model agrees");

	// Begun so low the copy cannot catch the sweep before it leaves: that
	// sweep reads it all old and the next all new.
	at = line_when(&P5, 38 + 1420, 5000);
	v = judge_model(&P5, at, full_lines, &screen, 1);
	CHECK(!v.torn && v.band == 7, "a copy begun at row 1420 is read whole");
	CHECK(!model_torn(&P5, at, full_lines, &screen, 1), "and the model agrees");

	// A copy slower than the sweep, begun at the blank, is overtaken by it:
	// 700 rows in 900 lines.
	piece_t upper = { 0, 700, 2560ull * 700 };
	at = line_when(&P5, 1478, 5000);
	v = judge_model(&P5, at, 900, &upper, 1);
	CHECK(v.torn, "a copy slower than the sweep tears");
	CHECK(model_torn(&P5, at, 900, &upper, 1), "and the model agrees");

	// A whole frame or more: torn whatever the readings say.
	at = line_when(&P5, 1478, 5000);
	v = judge_model(&P5, at, 1481, &screen, 1);
	CHECK(v.torn, "a copy lasting a frame tears");
	v = judge_model(&P5, at, 1481 * 2 + 3, &screen, 1);
	CHECK(v.torn, "a copy lasting two frames tears");
	// A few rows copied slowly over exactly one frame: each sweep reads them
	// whole, but the rule is that a copy lasting a frame tears, so the rule
	// itself is what this sees.
	piece_t sliver = { 0, 10, 2560ull * 10 };
	v = judge_model(&P5, at, 1481, &sliver, 1);
	CHECK(v.torn, "a sliver copied over exactly one frame is called torn");
	// An elapsed time so long its arithmetic would wrap.
	fc_frame_t p5 = frame_of(&P5);
	fc_piece_t p5_screen = { 0, 1440, full };
	v = frame_clock_judge(&p5, 1478, 1478, 1ull << 62, &p5_screen, 1);
	CHECK(v.torn, "an enormous elapsed time tears");

	// A small window in the blank's shadow: read before the copy, then after.
	piece_t corner = { 0, 50, 2560ull * 50 };
	at = line_when(&P5, 38 + 700, 5000);
	v = judge_model(&P5, at, copy_lines(corner.pixels, full_lines), &corner, 1);
	CHECK(!v.torn, "a small copy far below the sweep's way is clean");

	// Rows below the picture are never read, so they cannot tear.
	piece_t below = { 1440, 1500, 2560ull * 60 };
	at = line_when(&P5, 38 + 100, 5000);
	v = judge_model(&P5, at, 10, &below, 1);
	CHECK(!v.torn, "rows never shown cannot tear");

	// Nothing copied: not torn.
	v = judge_model(&P5, at, 0, &below, 0);
	CHECK(!v.torn, "no pieces, no tear");
	piece_t empty = { 100, 100, 0 };
	v = judge_model(&P5, at, 0, &empty, 1);
	CHECK(!v.torn, "an empty piece cannot tear");

	// A copy within one line, across the row the sweep is on: the split is
	// that row.
	piece_t band = { 400, 420, 2560ull * 20 };
	at = line_when(&P5, 38 + 410, 5000);
	v = judge_model(&P5, at, 0, &band, 1);
	CHECK(v.torn, "an instant copy across the sweep's row tears");
	CHECK(model_torn(&P5, at, 0, &band, 1), "and the model agrees");

	// The front finishes a row at the very moment the sweep reads it, and
	// only that row: a slow copy (11 lines a row) of rows 10-29 begun on the
	// blank's last line. Row 10 is read new, the rest old: torn.
	piece_t slow = { 10, 30, 2560ull * 20 };
	at = line_when(&P5, 37, 5000);
	v = judge_model(&P5, at, 220, &slow, 1);
	CHECK(v.torn, "a row finished as it is read counts as new");
	CHECK(model_torn(&P5, at, 220, &slow, 1), "and the model agrees");

	// Readings that are not lines of the frame judge nothing.
	fc_frame_t f = frame_of(&P5);
	fc_piece_t whole = { 0, 1440, full };
	// (2000 would be row 481 if it were wrapped, and a full screen copied
	// from there over 160 lines tears.)
	v = frame_clock_judge(&f, 2000, 38 + 481 + 160, 1800, &whole, 1);
	CHECK(!v.torn, "a begin reading beyond the frame judges nothing");
	v = frame_clock_judge(&f, 100, 0xFFFFFFFFu, 100, &whole, 1);
	CHECK(!v.torn, "an end reading of all ones judges nothing");

	// Pixel counts too big for 64-bit times are scaled, not wrapped.
	fc_piece_t huge[2] = { { 0, 720, 1ull << 62 }, { 720, 1440, 1ull << 62 } };
	v = frame_clock_judge(&f, 1478, 1478 + 2, 30, huge, 2);
	CHECK(!v.torn, "huge pixel counts from the blank are clean");
	v = frame_clock_judge(&f, 38 + 100, 38 + 260, 1800, huge, 2);
	CHECK(v.torn, "huge pixel counts from row 100 tear");
}

// Several rectangles copied whole in turn tear where the same damage copied
// in strips does not (FRAME_CLOCK.md § 4).
static void test_rectangles_against_strips(void)
{
	const uint64_t full_lines = 160;
	// A large window low on the screen, then a smaller one at the top.
	fc_rect_t damage[2] = { { 0, 600, 2560, 840 }, { 100, 20, 900, 300 } };
	piece_t whole[2];
	uint64_t px = 0;
	for (int i = 0; i < 2; i++) {
		whole[i] = (piece_t){ damage[i].y, damage[i].y + damage[i].h, (uint64_t)damage[i].w * damage[i].h };
		px += whole[i].pixels;
	}
	uint64_t lines = copy_lines(px, full_lines);
	uint64_t at = line_when(&P5, 1478, 5000);
	fc_verdict_t v = judge_model(&P5, at, lines, whole, 2);
	CHECK(v.torn, "two rectangles copied whole from the blank tear the second");
	CHECK(model_torn(&P5, at, lines, whole, 2), "and the model agrees");

	piece_t strips[400];
	uint32_t n = 0;
	for (int32_t top = 0; top < 1440; top += 16) {
		fc_rect_t out[2];
		uint32_t k = frame_clock_strip(damage, 2, top, 16, out);
		for (uint32_t i = 0; i < k; i++)
			strips[n++] = (piece_t){ out[i].y, out[i].y + out[i].h, (uint64_t)out[i].w * out[i].h };
	}
	v = judge_model(&P5, at, lines, strips, n);
	CHECK(!v.torn, "the same damage in strips from the blank is clean");
	CHECK(!model_torn(&P5, at, lines, strips, n), "and the model agrees");
}

// Random displays, copies and pieces: the judge and the model must agree on
// every one, and both answers must turn up.
static void test_verdict_random(int rounds)
{
	int torn = 0, clean = 0, long_copies = 0;
	for (int round = 0; round < rounds; round++) {
		display_t d;
		d.lines = (uint32_t)rnd_in(8, 600);
		d.blank_start = (uint32_t)rnd_in(0, d.lines - 1);
		d.blank_end = (d.blank_start + (uint32_t)rnd_in(1, d.lines / 3 + 1)) % d.lines;
		if (d.blank_end == d.blank_start)
			d.blank_end = (d.blank_start + 1) % d.lines;
		d.line_ns = (uint64_t)rnd_in(1000, 40000);
		d.phase = (uint64_t)rnd_in(0, 100000);
		uint32_t active = shown_rows(&d);
		uint32_t count = (uint32_t)rnd_in(1, 6);
		piece_t p[8];
		for (uint32_t i = 0; i < count; i++) {
			int32_t a = (int32_t)rnd_in(-5, active + 5), b = (int32_t)rnd_in(-5, active + 5);
			if (a > b) { int32_t t = a; a = b; b = t; }
			p[i].top = a;
			p[i].bottom = b;
			p[i].pixels = rnd() % 5 == 0 ? 0 : (uint64_t)rnd_in(1, 4000) * (uint64_t)(b > a ? b - a : 1);
		}
		uint64_t start = (uint64_t)rnd_in(d.lines * 2, d.lines * 4);
		uint64_t dur = rnd() % 10 == 0 ? 0 : (uint64_t)rnd_in(1, d.lines - 1);
		if (rnd() % 8 == 0) {
			// A frame or more, up to three: torn whenever any of it is shown.
			dur = (uint64_t)rnd_in(d.lines, 3 * d.lines);
			fc_verdict_t v = judge_model(&d, start, dur, p, count);
			bool shown = false;
			for (uint32_t i = 0; i < count; i++)
				shown |= p[i].pixels > 0 && p[i].top < p[i].bottom && p[i].bottom > 0 && p[i].top < (int32_t)active;
			CHECK(v.torn == shown, "round %d: a copy of %lu lines in a %u-line frame is %s, %s shown",
			      round, dur, d.lines, v.torn ? "torn" : "clean", shown ? "something" : "nothing");
			long_copies++;
			continue;
		}
		fc_verdict_t v = judge_model(&d, start, dur, p, count);
		bool truth = model_torn(&d, start, dur, p, count);
		CHECK(v.torn == truth, "round %d: judge says %s, model says %s (lines %u blank %u..%u start %lu dur %lu pieces %u)",
		      round, v.torn ? "torn" : "clean", truth ? "torn" : "clean", d.lines, d.blank_start, d.blank_end,
		      start, dur, count);
		fc_frame_t f = frame_of(&d);
		CHECK(v.band == frame_clock_band(&f, counter_at_line(&d, start)), "the verdict's band is the begin line's");
		CHECK(v.began_in_blank == frame_clock_in_blank(&f, counter_at_line(&d, start)), "began in the blank");
		if (truth) torn++; else clean++;
	}
	CHECK(torn > rounds / 10 && clean > rounds / 10, "both verdicts turn up: %d torn, %d clean", torn, clean);
	CHECK(long_copies > rounds / 20, "copies of a frame or more turn up: %d", long_copies);
}

// ── The lead ────────────────────────────────────────────────────────────────

static void test_lead(void)
{
	fc_lead_t l;
	memset(&l, 0, sizeof(l));
	CHECK(frame_clock_lead_us(&l) == FC_LEAD_MAX_US, "no passes yet: the most");
	frame_clock_lead_record(&l, 100);
	CHECK(frame_clock_lead_us(&l) == FC_LEAD_MIN_US, "cheap passes: the least, got %u", frame_clock_lead_us(&l));
	frame_clock_lead_record(&l, 1000);
	CHECK(frame_clock_lead_us(&l) == 1250, "1000 us passes: plus the margin, got %u", frame_clock_lead_us(&l));
	frame_clock_lead_record(&l, 4000);
	CHECK(frame_clock_lead_us(&l) == 5000, "the worst plus a quarter, got %u", frame_clock_lead_us(&l));
	frame_clock_lead_record(&l, 7000);
	CHECK(frame_clock_lead_us(&l) == FC_LEAD_MAX_US, "never more than the most");
	frame_clock_lead_record(&l, 1ull << 40);
	CHECK(frame_clock_lead_us(&l) == FC_LEAD_MAX_US, "an absurd pass is the most, not a wrap");
	for (int i = 0; i < FC_LEAD_PASSES - 1; i++)
		frame_clock_lead_record(&l, 300);
	CHECK(frame_clock_lead_us(&l) == FC_LEAD_MAX_US, "the absurd pass is still in the window");
	frame_clock_lead_record(&l, 300);
	CHECK(frame_clock_lead_us(&l) == 550, "after the window, the recent worst: got %u", frame_clock_lead_us(&l));
	memset(&l, 0, sizeof(l));
	frame_clock_lead_record(&l, 2400);
	frame_clock_lead_record(&l, 800);
	CHECK(frame_clock_lead_us(&l) == 3000, "the worst of several, not the last: got %u", frame_clock_lead_us(&l));
}

// ── The copy's order ────────────────────────────────────────────────────────

static void test_strips(void)
{
	enum { W = 200, H = 120 };
	static uint8_t damaged[H][W], copied[H][W];
	for (int round = 0; round < 400; round++) {
		uint32_t count = (uint32_t)rnd_in(0, 8);
		fc_rect_t damage[8];
		memset(damaged, 0, sizeof(damaged));
		memset(copied, 0, sizeof(copied));
		for (uint32_t i = 0; i < count; i++) {
			int32_t x = (int32_t)rnd_in(0, W - 1), y = (int32_t)rnd_in(0, H - 1);
			damage[i] = (fc_rect_t){ x, y, (int32_t)rnd_in(0, W - x), (int32_t)rnd_in(0, H - y) };
			for (int32_t r = damage[i].y; r < damage[i].y + damage[i].h; r++)
				for (int32_t c = damage[i].x; c < damage[i].x + damage[i].w; c++)
					damaged[r][c] = 1;
		}
		int32_t rows = (int32_t)rnd_in(1, 40);
		for (int32_t top = 0; top < H; top += rows) {
			fc_rect_t out[8];
			uint32_t k = frame_clock_strip(damage, count, top, rows, out);
			CHECK(k <= count, "a strip holds no more pieces than rectangles");
			uint64_t area = 0;
			for (uint32_t i = 0; i < k; i++) {
				CHECK(out[i].w > 0 && out[i].h > 0, "no empty piece");
				CHECK(out[i].y >= top && out[i].y + out[i].h <= top + rows, "a piece stays in its strip");
				if (i > 0)
					CHECK(out[i].x >= out[i - 1].x + out[i - 1].w, "pieces run left to right without overlapping");
				area += (uint64_t)out[i].w * out[i].h;
				for (int32_t r = out[i].y; r < out[i].y + out[i].h; r++)
					for (int32_t c = out[i].x; c < out[i].x + out[i].w; c++) {
						CHECK(!copied[r][c], "pixel (%d,%d) copied twice", c, r);
						copied[r][c] = 1;
					}
			}
			CHECK(area <= (uint64_t)W * rows, "a strip copies no more than its own area");
		}
		for (int r = 0; r < H; r++)
			for (int c = 0; c < W; c++)
				if (damaged[r][c])
					CHECK(copied[r][c], "damaged pixel (%d,%d) copied", c, r);
	}
	// Two overlapping parts become one box; two apart stay two.
	fc_rect_t two[2] = { { 10, 0, 50, 10 }, { 40, 5, 50, 10 } };
	fc_rect_t out[2];
	uint32_t k = frame_clock_strip(two, 2, 0, 16, out);
	CHECK(k == 1 && out[0].x == 10 && out[0].w == 80 && out[0].y == 0 && out[0].h == 15, "overlap merges into its box");
	fc_rect_t apart[2] = { { 100, 0, 10, 10 }, { 10, 0, 10, 10 } };
	k = frame_clock_strip(apart, 2, 0, 16, out);
	CHECK(k == 2 && out[0].x == 10 && out[1].x == 100, "apart stays apart, sorted left to right");
	fc_rect_t touching[2] = { { 0, 0, 10, 10 }, { 10, 0, 10, 10 } };
	k = frame_clock_strip(touching, 2, 0, 16, out);
	CHECK(k == 2, "touching is not overlapping");
	fc_rect_t outside[1] = { { 0, 50, 10, 10 } };
	k = frame_clock_strip(outside, 1, 0, 16, out);
	CHECK(k == 0, "a rectangle outside the strip gives nothing");
}

// ── When the clock runs ─────────────────────────────────────────────────────

static void test_keep_running(void)
{
	fc_run_t r = { 0 };
	CHECK(frame_clock_keep_running(&r, true, 0), "damage runs it");
	for (int i = 1; i <= FC_LINGER_FRAMES; i++)
		CHECK(frame_clock_keep_running(&r, false, 0), "idle frame %d still runs", i);
	CHECK(!frame_clock_keep_running(&r, false, 0), "after the linger it stops");
	CHECK(!frame_clock_keep_running(&r, false, 0), "and stays stopped");
	CHECK(frame_clock_keep_running(&r, false, 1), "somebody waiting starts it");
	for (int i = 1; i <= FC_LINGER_FRAMES; i++)
		CHECK(frame_clock_keep_running(&r, false, 0), "idle frame %d after a waiter still runs", i);
	CHECK(!frame_clock_keep_running(&r, false, 0), "then it stops");
	for (int i = 0; i < 1000; i++)
		frame_clock_keep_running(&r, false, 0);
	CHECK(r.idle_frames <= FC_LINGER_FRAMES + 1, "an idle count that cannot wrap back to running");
	CHECK(frame_clock_keep_running(&r, true, 0), "damage after a long idle runs it");
}

// ── Measuring the period ────────────────────────────────────────────────────
// The model is read every `read_ns` (with an occasional stall of `stall_ns`)
// and each reading stamped on a microsecond clock, as micros_now would.

typedef struct {
	uint64_t read_ns, stall_ns, stall_every;
	int backstep_at;    // a reading that goes back one line, -1 for none
	int beyond_at;      // a reading beyond the frame, -1 for none
	bool stuck;
} sampler_t;

static fc_measure_reason_t measure_model(const display_t *d, const sampler_t *s, uint64_t *period)
{
	fc_measure_t m;
	frame_clock_measure_begin(&m, d->lines);
	uint64_t t = 123456789;   // ns, arbitrary start
	uint64_t t_end = t + (uint64_t)FC_MEASURE_BOUND_US * 1000;
	for (uint64_t i = 0; t < t_end; i++) {
		uint64_t n = t / d->line_ns;
		uint32_t raw = s->stuck ? 7 : counter_at_line(d, n);
		if ((int64_t)i == s->backstep_at)
			raw = raw == 0 ? 0 : raw - 1;
		if ((int64_t)i == s->beyond_at)
			raw = 0xFFFFFFFFu;
		if (frame_clock_measure_feed(&m, raw, t / 1000))
			break;
		t += s->read_ns;
		if (s->stall_every && i % s->stall_every == s->stall_every - 1)
			t += s->stall_ns;
	}
	return frame_clock_measure_result(&m, period);
}

static void test_measure(void)
{
	uint64_t period;
	sampler_t plain = { 500, 0, 0, -1, -1, false };
	CHECK(measure_model(&P5, &plain, &period) == FC_MEASURE_OK, "the P5 measures");
	uint64_t truth = P5.line_ns * P5.lines;
	CHECK(period + 2000 > truth && period < truth + 2000, "the P5's period %lu ns, truly %lu", period, truth);

	// Stalls (an interrupt, a preemption) shorter than half a frame cost
	// nothing.
	sampler_t stalls = { 500, 4000000, 3001, -1, -1, false };
	CHECK(measure_model(&P5, &stalls, &period) == FC_MEASURE_OK, "4 ms stalls are survived");
	CHECK(period + 2000 > truth && period < truth + 2000, "and cost nothing: %lu ns, truly %lu", period, truth);

	sampler_t long_stall = { 500, 9000000, 30001, -1, -1, false };
	CHECK(measure_model(&P5, &long_stall, &period) == FC_MEASURE_GAP, "a stall over half a frame is a gap");
	CHECK(period == 0, "no period with a reason");

	// A stall of a frame and a little: the counter lands just ahead of where
	// it was, with no step back to give the lost wrap away. Only the gap
	// says so.
	sampler_t lapped = { 500, truth + 300000, 30001, -1, -1, false };
	CHECK(measure_model(&P5, &lapped, &period) == FC_MEASURE_GAP, "a stall of a whole frame is a gap");
	// Several stalls of exactly a frame (Codex #631): each hides a wrap and
	// lands the counter where it was, so the counted period comes out more
	// than twice too long, and a gap judged against that period would pass.
	sampler_t laps = { 500, truth, 20000, -1, -1, false };
	CHECK(measure_model(&P5, &laps, &period) == FC_MEASURE_GAP, "whole-frame stalls are gaps, got period %lu", period);
	// Stalls that break the window into stretches too short to cover a frame
	// between them leave the rate unknown.
	sampler_t chopped = { 500, 60000, 2, -1, -1, false };
	CHECK(measure_model(&P5, &chopped, &period) != FC_MEASURE_OK, "a window broken every other reading is not measured");

	sampler_t stuck = { 500, 0, 0, -1, -1, true };
	CHECK(measure_model(&P5, &stuck, &period) == FC_MEASURE_STILL, "a counter that never moves");

	sampler_t back = { 500, 0, 0, 40000, -1, false };
	CHECK(measure_model(&P5, &back, &period) == FC_MEASURE_BACKWARD, "a counter that steps back");

	sampler_t beyond = { 500, 0, 0, -1, 1000, false };
	CHECK(measure_model(&P5, &beyond, &period) == FC_MEASURE_BEYOND_FRAME, "a reading beyond the frame");

	display_t slow = { 1000, 990, 5, 100000, 0 };     // 10 Hz: too slow to close the window
	CHECK(measure_model(&slow, &plain, &period) == FC_MEASURE_TOO_FEW_WRAPS, "10 Hz never closes the window");
	display_t too_slow = { 1000, 990, 5, 66667, 0 };  // 15 Hz: closes the window, but no display
	CHECK(measure_model(&too_slow, &plain, &period) == FC_MEASURE_RATE, "15 Hz is no display");
	display_t slowest = { 1000, 990, 5, 50000, 0 };   // 20 Hz: the slowest taken
	CHECK(measure_model(&slowest, &plain, &period) == FC_MEASURE_OK, "20 Hz measures");
	display_t fast = { 100, 95, 2, 20000, 0 };        // 500 Hz: the fastest taken
	CHECK(measure_model(&fast, &plain, &period) == FC_MEASURE_OK, "500 Hz measures");
	display_t faster = { 100, 95, 2, 5000, 0 };       // 2000 Hz
	CHECK(measure_model(&faster, &plain, &period) == FC_MEASURE_RATE, "2000 Hz is no display");

	// A measurer that is fed nothing, or one reading.
	fc_measure_t m;
	frame_clock_measure_begin(&m, 1481);
	CHECK(frame_clock_measure_result(&m, &period) == FC_MEASURE_STILL, "nothing fed");
	CHECK(!frame_clock_measure_feed(&m, 5, 10), "one reading closes nothing");
	CHECK(frame_clock_measure_result(&m, &period) == FC_MEASURE_STILL, "one reading is still");

	// Random displays and read speeds.
	for (int round = 0; round < 60; round++) {
		display_t d = { (uint32_t)rnd_in(50, 3000), 0, 0, 0, (uint64_t)rnd_in(0, 5000) };
		uint64_t hz = (uint64_t)rnd_in(24, 240);
		d.line_ns = 1000000000ull / hz / d.lines;
		d.blank_start = (uint32_t)rnd_in(0, d.lines - 1);
		d.blank_end = (d.blank_start + 1 + (uint32_t)rnd_in(0, d.lines / 4)) % d.lines;
		sampler_t s = { (uint64_t)rnd_in(300, 1500), (uint64_t)rnd_in(0, d.line_ns * d.lines / 3), (uint64_t)rnd_in(1000, 50000), -1, -1, false };
		if (d.line_ns < 400)
			continue;
		fc_measure_reason_t why = measure_model(&d, &s, &period);
		uint64_t exact = d.line_ns * d.lines;
		CHECK(why == FC_MEASURE_OK, "round %d (%u lines at %lu Hz): %s", round, d.lines, hz,
		      frame_clock_measure_reason_name(why));
		CHECK(period * 1000 > exact * 999 && period * 1000 < exact * 1001,
		      "round %d: %lu ns, truly %lu", round, period, exact);
	}
	for (int r = FC_MEASURE_OK; r <= FC_MEASURE_RATE; r++)
		CHECK(strcmp(frame_clock_measure_reason_name((fc_measure_reason_t)r), "unknown") != 0, "reason %d has a name", r);
}

// ── Planning a frame, the aim, and why a frame was late ─────────────────────

static void test_plan(void)
{
	fc_frame_t f = frame_of(&P5);
	uint64_t period_us = f.period_ns / 1000;
	// Twenty lines before the blank, a 500 us lead: this blank (225 us away)
	// is too close, so the next one.
	fc_plan_t p = frame_clock_plan(&f, 1458, 1000000, 500);
	uint64_t this_blank = 1000000 + 20ull * f.period_ns / 1481 / 1000;
	CHECK(p.blank_at_us >= this_blank + period_us && p.blank_at_us <= this_blank + period_us + 1,
	      "a blank closer than the lead is passed over: %lu", p.blank_at_us);
	CHECK(p.wake_at_us == p.blank_at_us - 500, "the wake is the lead before");
	// Mid-screen with the same lead: this frame's blank.
	p = frame_clock_plan(&f, 38 + 700, 1000000, 500);
	CHECK(p.blank_at_us < 1000000 + period_us, "mid-screen aims at this frame's blank");
	// A lead longer than a frame skips as many as it must.
	fc_frame_t fast = { 100, 95, 2, 2000000 };   // 500 Hz
	p = frame_clock_plan(&fast, 50, 0, 8000);
	CHECK(p.blank_at_us >= 8000 && p.blank_at_us < 10000, "a lead of four frames: %lu", p.blank_at_us);

	// Against the model: the blank is the first one at least the lead away.
	for (int round = 0; round < 3000; round++) {
		display_t d = { (uint32_t)rnd_in(20, 3000), 0, 0, (uint64_t)rnd_in(2000, 40000), (uint64_t)rnd_in(0, 100000) };
		d.blank_start = (uint32_t)rnd_in(0, d.lines - 1);
		d.blank_end = (d.blank_start + (uint32_t)rnd_in(1, d.lines / 3 + 1)) % d.lines;
		if (d.blank_end == d.blank_start)
			d.blank_end = (d.blank_start + 1) % d.lines;
		fc_frame_t g = frame_of(&d);
		uint64_t n = (uint64_t)rnd_in(1000, 100000);
		uint32_t lead = (uint32_t)rnd_in(0, 9000);
		uint64_t now_ns = n * d.line_ns, now_us = now_ns / 1000;
		uint64_t m = n;
		while (counter_at_line(&d, m) != d.blank_start || m * d.line_ns < now_us * 1000 + lead * 1000ull)
			m++;
		uint64_t exact_us = m * d.line_ns / 1000;
		fc_plan_t q = frame_clock_plan(&g, counter_at_line(&d, n), now_us, lead);
		CHECK(q.blank_at_us + 1 >= exact_us && q.blank_at_us <= exact_us + 1,
		      "round %d: plan %lu, model %lu (lead %u)", round, q.blank_at_us, exact_us, lead);
		CHECK(q.wake_at_us + lead == q.blank_at_us && q.wake_at_us + 1 >= now_us, "round %d: the wake is not in the past", round);
	}
}

static void test_aim(void)
{
	fc_frame_t f = frame_of(&P5);
	uint64_t blank_at = 5000000;
	uint64_t blank_end = frame_clock_blank_end_us(&f, blank_at);
	CHECK(blank_end >= blank_at + 461 && blank_end <= blank_at + 463, "the P5's blank lasts 462 us: %lu", blank_end - blank_at);
	CHECK(frame_clock_aim(&f, 1478, blank_at, blank_at) == FC_AIM_COPY, "the blank's first line copies");
	CHECK(frame_clock_aim(&f, 37, blank_at + 400, blank_at) == FC_AIM_COPY, "its last line copies");
	CHECK(frame_clock_aim(&f, 1400, blank_at - 900, blank_at) == FC_AIM_WAIT, "approaching the blank waits");
	CHECK(frame_clock_aim(&f, 1477, blank_at + 3, blank_at) == FC_AIM_WAIT,
	      "shown rows as the blank should begin: it has not quite begun");
	CHECK(frame_clock_aim(&f, 38, blank_end, blank_at) == FC_AIM_LATE, "row 0 once the blank has ended: late");
	CHECK(frame_clock_aim(&f, 500, blank_end + 4000, blank_at) == FC_AIM_LATE, "mid-screen after it: late");
	CHECK(frame_clock_aim(&f, 500, blank_end - 1, blank_at) == FC_AIM_WAIT, "until the blank must have ended, wait");
	fc_frame_t bad = { 0 };
	CHECK(frame_clock_aim(&bad, 0, 0, 0) == FC_AIM_LATE, "a frame that makes no sense is late, never a copy");
	CHECK(frame_clock_aim_bound_us(&f, 1000) >= 1000 + 90 && frame_clock_aim_bound_us(&f, 1000) <= 1000 + 91,
	      "the aim's bound is the lead and 8 lines: %lu", frame_clock_aim_bound_us(&f, 1000));
}

static void test_late_cause(void)
{
	// The aimed-at blank ends at 10,000; the wake was due at 8,000.
	CHECK(frame_clock_late_cause(true, 8000, 8000, 9000, 10000) == FC_LATE_OFF_CPU, "off its CPU comes first");
	CHECK(frame_clock_late_cause(false, 8000, 8000, 9000, 10000) == FC_LATE_PAST, "the pass ended in time: past");
	CHECK(frame_clock_late_cause(false, 8000, 9500, 10500, 10000) == FC_LATE_WAKE,
	      "a 1 ms pass from a wake 1.5 ms late: an on-time wake would have made it");
	CHECK(frame_clock_late_cause(false, 8000, 8000, 10001, 10000) == FC_LATE_PASS, "on time and still past: the pass");
	CHECK(frame_clock_late_cause(false, 8000, 8100, 10200, 10000) == FC_LATE_PASS,
	      "a 2.1 ms pass would not fit the 2 ms from an on-time wake");
	CHECK(frame_clock_late_cause(false, 8000, 8000, 10000, 10000) == FC_LATE_WAKE,
	      "a pass exactly as long as the budget is the wake's");
	CHECK(frame_clock_late_cause(false, 8000, 7000, 10500, 10000) == FC_LATE_PASS, "an early wake cannot be blamed");
}

static void test_wake_bucket_and_free(void)
{
	CHECK(frame_clock_wake_bucket(0) == 0 && frame_clock_wake_bucket(49) == 0, "under 50 us");
	CHECK(frame_clock_wake_bucket(50) == 1 && frame_clock_wake_bucket(99) == 1, "under 100 us");
	CHECK(frame_clock_wake_bucket(799) == 4 && frame_clock_wake_bucket(800) == 5, "under 1.6 ms");
	CHECK(frame_clock_wake_bucket(1600) == 6 && frame_clock_wake_bucket(1ull << 40) == 6, "1.6 ms or more");

	CHECK(frame_clock_free_next_ns(0, 1000) == 1000 + FC_FREE_PERIOD_NS, "the free clock starts a period on");
	CHECK(frame_clock_free_next_ns(5000, 6000) == 5000 + FC_FREE_PERIOD_NS, "the next period");
	CHECK(frame_clock_free_next_ns(5000, 5000 + FC_FREE_PERIOD_NS) == 5000 + 2 * FC_FREE_PERIOD_NS,
	      "on a boundary, the one after");
	CHECK(frame_clock_free_next_ns(5000, 5000 + 3 * FC_FREE_PERIOD_NS + 7) == 5000 + 4 * FC_FREE_PERIOD_NS,
	      "late by three periods, skips to the fourth on the grid");
	uint64_t t = 0, now = 123;
	for (int i = 0; i < 600; i++) {
		t = frame_clock_free_next_ns(t, now);
		now = t;
	}
	CHECK(t - 123 == 600 * FC_FREE_PERIOD_NS, "600 frames are 600 periods exactly: ten seconds and 200 ns");
}

static void test_timer(void)
{
	uint64_t tps = 62500000;   // a 1 GHz bus divided by 16
	fc_timer_t t = frame_clock_timer(5000, tps, 10);
	CHECK(t.wake_first && t.count == 312500, "a 5 ms wake under a 10 ms lease: %u", t.count);
	t = frame_clock_timer(13000, tps, 10);
	CHECK(!t.wake_first && t.count == 625000, "a 13 ms wake: the lease first");
	t = frame_clock_timer(10000, tps, 10);
	CHECK(!t.wake_first, "a tie is the lease's");
	t = frame_clock_timer(13000, tps, 0);
	CHECK(t.wake_first && t.count == 812500, "no lease: the wake alone");
	t = frame_clock_timer(0, tps, 10);
	CHECK(t.wake_first && t.count == 1, "a wake already due: one tick, never zero (zero stops the timer)");
	t = frame_clock_timer(1, tps, 0);
	CHECK(t.count == 63, "a microsecond is 62.5 ticks, rounded up so it never fires early: %u", t.count);
	t = frame_clock_timer(1ull << 50, tps, 0);
	CHECK(t.count == 0xFFFFFFFFu, "a wake past the register's reach is the most it holds");
	t = frame_clock_timer(5000, 10, 1);
	CHECK(t.count == 1 && !t.wake_first, "a lease too short to count is one tick");
}

static void test_reserve(void)
{
	CHECK(frame_clock_reserves(4, true, true), "four CPUs, the dial on, running: reserved");
	CHECK(!frame_clock_reserves(3, true, true), "three CPUs: shared");
	CHECK(!frame_clock_reserves(12, false, true), "the dial off: shared");
	CHECK(!frame_clock_reserves(12, true, false), "the clock stopped: shared");
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 3000;
	test_frame_facts();
	test_next_blank();
	test_verdict_cases();
	test_rectangles_against_strips();
	test_verdict_random(rounds);
	test_lead();
	test_strips();
	test_keep_running();
	test_measure();
	test_plan();
	test_aim();
	test_late_cause();
	test_wake_bucket_and_free();
	test_timer();
	test_reserve();
	printf("frame_clock: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures ? 1 : 0;
}
