// frame_clock.c — the frame clock's arithmetic. Pure: gui/frame_clock.h says
// what each rule is and why; this file is only how.

#include "gui/frame_clock.h"

#include <stddef.h>

bool frame_clock_frame_valid(const fc_frame_t *f)
{
	return f != NULL && f->lines >= 2 && f->lines <= 32768 && f->blank_start < f->lines &&
	       f->blank_end < f->lines && f->blank_start != f->blank_end &&
	       f->period_ns > 0 && f->period_ns <= 1000000000ull;
}

uint32_t frame_clock_active_rows(const fc_frame_t *f)
{
	if (!frame_clock_frame_valid(f))
		return 0;
	return (f->blank_start + f->lines - f->blank_end) % f->lines;
}

uint32_t frame_clock_sweep(const fc_frame_t *f, uint32_t line)
{
	if (!frame_clock_frame_valid(f))
		return 0;
	return (line % f->lines + f->lines - f->blank_end) % f->lines;
}

bool frame_clock_in_blank(const fc_frame_t *f, uint32_t line)
{
	return frame_clock_sweep(f, line) >= frame_clock_active_rows(f);
}

bool frame_clock_line_ok(const fc_frame_t *f, uint32_t raw)
{
	return frame_clock_frame_valid(f) && raw != 0xFFFFFFFFu && raw < f->lines;
}

uint64_t frame_clock_next_blank_us(const fc_frame_t *f, uint32_t line, uint64_t now_us)
{
	if (!frame_clock_frame_valid(f))
		return now_us;
	uint64_t lines_to = (f->blank_start + f->lines - line % f->lines) % f->lines;
	return now_us + lines_to * f->period_ns / f->lines / 1000;
}

uint32_t frame_clock_band(const fc_frame_t *f, uint32_t line)
{
	uint32_t active = frame_clock_active_rows(f);
	uint32_t s = frame_clock_sweep(f, line);
	if (active == 0 || s >= active)
		return FC_BAND_BLANK;
	return (uint32_t)((uint64_t)s * 8 / active);
}

// ── The verdict ─────────────────────────────────────────────────────────────
// Time is measured in sweep rows, scaled by the copy's total pixels P so that
// every piece's start and end are whole numbers: the sweep reads row r of
// pass p at τ = (p·lines + r)·P, and the copy runs from τ = s0·P to s1·P,
// each piece taking D·pixels of it in turn (D = s1 - s0). A piece's front
// moves from its top row to its bottom row across [τa, τb], and row r is new
// once the front has finished it: (r + 1 - top)·(τb - τa) <= H·(τ - τa).

// Totals are scaled below this so the times fit in 64 bits; the products
// that compare them are taken in 128.
#define FC_PIXELS_MAX (1ull << 40)

static bool row_new(int64_t r, int64_t pass, int64_t lines, int64_t total, int64_t ta, int64_t tb,
                    int64_t top, int64_t height)
{
	int64_t tr = (pass * lines + r) * total;
	if (tr <= ta)
		return false;   // read before (or as) the piece's copy began
	if (tr >= tb)
		return true;    // read after it ended
	return (__int128)(r + 1 - top) * (tb - ta) <= (__int128)height * (tr - ta);
}

// One sweep pass over rows [lo, hi) of a piece: does it read some rows new
// and some old? The answer is a threshold in r within each of the three
// stretches (read before the copy, during it, after it; within the middle one
// the test is linear in r), so the rows at the stretches' ends decide it.
static bool pass_mixed(int64_t pass, int64_t lines, int64_t total, int64_t ta, int64_t tb,
                       int64_t top, int64_t height, int64_t lo, int64_t hi)
{
	int64_t first_during = ta / total + 1 - pass * lines;               // first row read after τa
	int64_t first_after = (tb + total - 1) / total - pass * lines;      // first row read at or after τb
	int64_t rows[6] = { lo, hi - 1, first_during - 1, first_during, first_after - 1, first_after };
	bool seen = row_new(lo, pass, lines, total, ta, tb, top, height);
	for (int i = 1; i < 6; i++) {
		int64_t r = rows[i] < lo ? lo : rows[i] > hi - 1 ? hi - 1 : rows[i];
		if (row_new(r, pass, lines, total, ta, tb, top, height) != seen)
			return true;
	}
	return false;
}

fc_verdict_t frame_clock_judge(const fc_frame_t *f, uint32_t begin_line, uint32_t end_line,
                               uint64_t elapsed_us, const fc_piece_t *pieces, uint32_t count)
{
	fc_verdict_t v = { .torn = false, .began_in_blank = false, .band = FC_BAND_BLANK };
	if (!frame_clock_frame_valid(f) || begin_line >= f->lines || end_line >= f->lines)
		return v;
	v.band = frame_clock_band(f, begin_line);
	v.began_in_blank = v.band == FC_BAND_BLANK;

	unsigned __int128 sum = 0;
	for (uint32_t i = 0; i < count; i++)
		sum += pieces[i].pixels;
	unsigned shift = 0;
	while ((sum >> shift) >= FC_PIXELS_MAX)
		shift++;
	int64_t total = 0;
	for (uint32_t i = 0; i < count; i++)
		total += (int64_t)(pieces[i].pixels >> shift);
	if (total == 0)
		return v;

	// How far the sweep moved: the readings say where it ended within a
	// frame, the time says how many whole frames it went round, and the
	// whole frames are the ones that put the two closest together. A
	// microsecond clock reads a copy of exactly one frame a little short, so
	// the readings alone would call it no time at all.
	int64_t lines = f->lines, active = frame_clock_active_rows(f);
	int64_t s0 = frame_clock_sweep(f, begin_line), s1 = frame_clock_sweep(f, end_line);
	int64_t d = (s1 - s0 + lines) % lines;
	bool outlasted;
	if (elapsed_us >= 2 * f->period_ns / 1000 + 1) {
		outlasted = true;
	} else {
		int64_t moved = (int64_t)(elapsed_us * 1000 * f->lines / f->period_ns);
		int64_t best = d;
		for (int64_t round = 1; round <= 2; round++) {
			int64_t candidate = d + round * lines;
			int64_t off = candidate > moved ? candidate - moved : moved - candidate;
			int64_t best_off = best > moved ? best - moved : moved - best;
			if (off < best_off)
				best = candidate;
		}
		d = best;
		outlasted = d >= lines;
	}
	int64_t done = 0;
	for (uint32_t i = 0; i < count && !v.torn; i++) {
		int64_t px = (int64_t)(pieces[i].pixels >> shift);
		int64_t ta = s0 * total + d * done;
		done += px;
		int64_t tb = s0 * total + d * done;
		if (pieces[i].pixels == 0 || pieces[i].top >= pieces[i].bottom)
			continue;
		int64_t top = pieces[i].top, height = (int64_t)pieces[i].bottom - top;
		int64_t lo = top < 0 ? 0 : top, hi = pieces[i].bottom > active ? active : pieces[i].bottom;
		if (lo >= hi)
			continue;   // nothing of it is ever shown
		if (outlasted) {
			v.torn = true;
			break;
		}
		// The copy is within one frame, so only the pass it began in and the
		// one after can read it part old and part new.
		for (int64_t pass = 0; pass <= 1 && !v.torn; pass++)
			v.torn = pass_mixed(pass, lines, total, ta, tb, top, height, lo, hi);
	}
	return v;
}

// ── The lead ────────────────────────────────────────────────────────────────

void frame_clock_lead_record(fc_lead_t *l, uint64_t pass_us)
{
	l->cost_us[l->next] = pass_us > UINT32_MAX ? UINT32_MAX : (uint32_t)pass_us;
	l->next = (l->next + 1) % FC_LEAD_PASSES;
	if (l->count < FC_LEAD_PASSES)
		l->count++;
}

uint32_t frame_clock_lead_us(const fc_lead_t *l)
{
	if (l->count == 0)
		return FC_LEAD_MAX_US;
	uint64_t worst = 0;
	for (uint32_t i = 0; i < l->count; i++)
		if (l->cost_us[i] > worst)
			worst = l->cost_us[i];
	uint64_t margin = worst / 4 > FC_LEAD_MARGIN_US ? worst / 4 : FC_LEAD_MARGIN_US;
	uint64_t lead = worst + margin;
	if (lead < FC_LEAD_MIN_US)
		return FC_LEAD_MIN_US;
	if (lead > FC_LEAD_MAX_US)
		return FC_LEAD_MAX_US;
	return (uint32_t)lead;
}

// ── The copy's order ────────────────────────────────────────────────────────

uint32_t frame_clock_strip(const fc_rect_t *damage, uint32_t count, int32_t strip_top,
                           int32_t strip_rows, fc_rect_t *out)
{
	int64_t strip_end = (int64_t)strip_top + strip_rows;
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++) {
		const fc_rect_t *r = &damage[i];
		int64_t y0 = r->y > strip_top ? r->y : strip_top;
		int64_t y1 = (int64_t)r->y + r->h < strip_end ? (int64_t)r->y + r->h : strip_end;
		if (r->w <= 0 || y0 >= y1)
			continue;
		fc_rect_t piece = { r->x, (int32_t)y0, r->w, (int32_t)(y1 - y0) };
		// Insertion by left edge: a strip holds a handful.
		uint32_t at = n;
		while (at > 0 && out[at - 1].x > piece.x) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = piece;
		n++;
	}
	// Left to right, a part that starts inside the box before it joins it.
	uint32_t kept = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (kept > 0) {
			fc_rect_t *box = &out[kept - 1];
			int64_t box_right = (int64_t)box->x + box->w;
			if (out[i].x < box_right) {
				int64_t right = (int64_t)out[i].x + out[i].w > box_right ? (int64_t)out[i].x + out[i].w : box_right;
				int64_t y0 = out[i].y < box->y ? out[i].y : box->y;
				int64_t y1 = (int64_t)out[i].y + out[i].h > (int64_t)box->y + box->h
				                 ? (int64_t)out[i].y + out[i].h : (int64_t)box->y + box->h;
				box->w = (int32_t)(right - box->x);
				box->y = (int32_t)y0;
				box->h = (int32_t)(y1 - y0);
				continue;
			}
		}
		out[kept++] = out[i];
	}
	return kept;
}

// ── When the clock runs ─────────────────────────────────────────────────────

bool frame_clock_keep_running(fc_run_t *r, bool damage, uint32_t waiting)
{
	if (damage || waiting > 0) {
		r->idle_frames = 0;
		return true;
	}
	if (r->idle_frames <= FC_LINGER_FRAMES)
		r->idle_frames++;
	return r->idle_frames <= FC_LINGER_FRAMES;
}

// ── Measuring the period ────────────────────────────────────────────────────

void frame_clock_measure_begin(fc_measure_t *m, uint32_t lines)
{
	*m = (fc_measure_t){ .lines = lines };
}

bool frame_clock_measure_feed(fc_measure_t *m, uint32_t raw, uint64_t now_us)
{
	if (m->done)
		return true;
	if (m->lines == 0 || raw >= m->lines) {
		m->beyond++;
		return false;
	}
	if (!m->started) {
		m->started = true;
		m->prev = raw;
		m->prev_us = now_us;
		return false;
	}
	uint64_t gap = now_us > m->prev_us ? now_us - m->prev_us : 0;
	if (gap > m->gap_max_us)
		m->gap_max_us = gap;
	if (raw < m->prev) {
		if (m->prev - raw > m->lines / 2)
			m->wraps++;
		else if (gap * 2 >= 1000000 / FC_MEASURE_HZ_MAX)
			m->hidden++;    // long enough to have hidden a wrap at the fastest rate
		else
			m->backward++;
	}
	uint64_t pos = m->wraps * m->lines + raw;
	// Only a change seen right after the reading before it is the start of a
	// line; one seen across a stall happened somewhere inside the stall.
	if (raw != m->prev && gap <= FC_MEASURE_EDGE_US) {
		if (!m->anchored) {
			m->anchored = true;
			m->anchor_us = now_us;
			m->anchor_pos = pos;
		} else if (now_us - m->anchor_us >= FC_MEASURE_US &&
		           pos - m->anchor_pos >= (uint64_t)FC_MEASURE_WRAPS * m->lines) {
			m->end_us = now_us;
			m->end_pos = pos;
			m->done = true;
		}
	}
	m->prev = raw;
	m->prev_us = now_us;
	return m->done;
}

fc_measure_reason_t frame_clock_measure_result(const fc_measure_t *m, uint64_t *period_ns)
{
	*period_ns = 0;
	if (m->beyond > 0)
		return FC_MEASURE_BEYOND_FRAME;
	uint64_t period = 0;
	if (m->done)
		period = (m->end_us - m->anchor_us) * 1000 * m->lines / (m->end_pos - m->anchor_pos);
	// A gap of half a frame could hide a wrap, and a step back across a gap
	// is one that may have, so either is the reason ahead of a backward step.
	if (m->hidden > 0 || (m->done && m->gap_max_us * 2000 >= period))
		return FC_MEASURE_GAP;
	if (m->backward > 0)
		return FC_MEASURE_BACKWARD;
	if (!m->anchored)
		return FC_MEASURE_STILL;
	if (!m->done)
		return FC_MEASURE_TOO_FEW_WRAPS;
	if (period < 1000000000ull / FC_MEASURE_HZ_MAX || period > 1000000000ull / FC_MEASURE_HZ_MIN)
		return FC_MEASURE_RATE;
	*period_ns = period;
	return FC_MEASURE_OK;
}

const char *frame_clock_measure_reason_name(fc_measure_reason_t reason)
{
	switch (reason) {
	case FC_MEASURE_OK:            return "measured";
	case FC_MEASURE_STILL:         return "the line counter never moved";
	case FC_MEASURE_BEYOND_FRAME:  return "a reading was not a line of the frame";
	case FC_MEASURE_BACKWARD:      return "the line counter went back without wrapping";
	case FC_MEASURE_TOO_FEW_WRAPS: return "the line counter did not sweep the frame often enough";
	case FC_MEASURE_GAP:           return "readings too far apart to count the frames";
	case FC_MEASURE_RATE:          return "the refresh rate is not a display's";
	}
	return "unknown";
}
