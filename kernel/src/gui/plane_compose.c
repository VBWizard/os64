// plane_compose.c — a window's content and a plane's picture, made into the
// pixels the compositor draws (plane_compose.h).

#include "gui/plane_compose.h"

#include <stddef.h>

// No libc: the kernel's own copy is not reachable from a pure file.
static void copy_px(uint32_t *to, const uint32_t *from, int32_t n)
{
	for (int32_t i = 0; i < n; i++)
		to[i] = from[i];
}

static int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

uint32_t plane_blend(uint32_t content, uint32_t picture)
{
	uint32_t a = content >> 24;
	if (a == 255)
		return content;
	if (a == 0)
		return picture;
	uint32_t keep = 255 - a, out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		uint32_t c = (content >> shift) & 0xFF, p = (picture >> shift) & 0xFF;
		uint32_t v = c + (p * keep + 127) / 255;
		out |= (v > 255 ? 255 : v) << shift;
	}
	return out;
}

// One row's overlay intervals: each overlay covering row `y`, as [x0, x1),
// cut to the clip's columns, sorted by start and merged where they touch or
// overlap. The count.
typedef struct {
	int32_t x0, x1;
} span_t;

static int row_spans(const pc_place_t *place, int32_t y, span_t out[PLANE_OVERLAYS_MAX])
{
	int n = 0;
	int32_t cx0 = place->clip.x, cx1 = place->clip.x + place->clip.w;
	for (uint32_t i = 0; i < place->noverlays && i < PLANE_OVERLAYS_MAX; i++) {
		const pc_rect_t *o = &place->overlay[i];
		if (o->w <= 0 || o->h <= 0 || y < o->y || y >= o->y + o->h)
			continue;
		int32_t x0 = max32(o->x, cx0), x1 = min32(o->x + o->w, cx1);
		if (x0 >= x1)
			continue;
		int at = n++;
		while (at > 0 && out[at - 1].x0 > x0) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = (span_t){x0, x1};
	}
	int merged = 0;
	for (int i = 0; i < n; i++) {
		if (merged > 0 && out[i].x0 <= out[merged - 1].x1) {
			if (out[i].x1 > out[merged - 1].x1)
				out[merged - 1].x1 = out[i].x1;
		} else {
			out[merged++] = out[i];
		}
	}
	return merged;
}

uint64_t plane_overlay_area(const pc_place_t *place)
{
	if (place->clip.w <= 0 || place->clip.h <= 0)
		return 0;
	// The rows any overlay touches, each row's merged spans summed.
	uint64_t area = 0;
	int32_t y0 = place->clip.y + place->clip.h, y1 = place->clip.y;
	for (uint32_t i = 0; i < place->noverlays && i < PLANE_OVERLAYS_MAX; i++) {
		const pc_rect_t *o = &place->overlay[i];
		if (o->w <= 0 || o->h <= 0)
			continue;
		y0 = min32(y0, max32(o->y, place->clip.y));
		y1 = max32(y1, min32(o->y + o->h, place->clip.y + place->clip.h));
	}
	span_t spans[PLANE_OVERLAYS_MAX];
	for (int32_t y = y0; y < y1; y++) {
		int n = row_spans(place, y, spans);
		for (int i = 0; i < n; i++)
			area += (uint64_t)(spans[i].x1 - spans[i].x0);
	}
	return area;
}

// A rectangle whose sizes are no smaller than zero and whose far edges do
// not overflow.
static bool rect_sane(const pc_rect_t *r)
{
	return r->w >= 0 && r->h >= 0 && (int64_t)r->x + r->w <= INT32_MAX && (int64_t)r->y + r->h <= INT32_MAX;
}

static bool rect_inside(const pc_rect_t *inner, const pc_rect_t *outer)
{
	return inner->x >= outer->x && inner->y >= outer->y && (int64_t)inner->x + inner->w <= (int64_t)outer->x + outer->w &&
	       (int64_t)inner->y + inner->h <= (int64_t)outer->y + outer->h;
}

bool plane_place_valid(const pc_place_t *place, uint32_t content_w, uint32_t content_h, uint64_t overlay_budget)
{
	if (!rect_sane(&place->clip) || !rect_sane(&place->origin) || place->noverlays > PLANE_OVERLAYS_MAX)
		return false;
	if (place->clip.w == 0 || place->clip.h == 0)
		return true;
	pc_rect_t content = {0, 0, (int32_t)content_w, (int32_t)content_h};
	if (content_w > INT32_MAX || content_h > INT32_MAX || !rect_inside(&place->clip, &content))
		return false;
	for (uint32_t i = 0; i < place->noverlays; i++)
		if (!rect_sane(&place->overlay[i]) || !rect_inside(&place->overlay[i], &place->clip))
			return false;
	return plane_overlay_area(place) <= overlay_budget;
}

// The run [x0, x1) of content row y inside the clip and outside every
// overlay: the picture's pixels where it reaches, the content as opaque
// where it does not.
static void picture_run(uint32_t *dst, const pc_image_t *content, const pc_image_t *picture,
                        const pc_place_t *place, int32_t y, int32_t x0, int32_t x1, int32_t ax,
                        pc_counts_t *counts)
{
	// The run is three parts in order, any of them empty: before the
	// picture, the picture, after it ([x0, px0), [px0, px1), [px1, x1)).
	int32_t py = y - place->origin.y;
	int32_t px0 = x1, px1 = x1;
	if (py >= 0 && py < (int32_t)picture->h) {
		px0 = min32(max32(x0, place->origin.x), x1);
		px1 = max32(min32(x1, place->origin.x + (int32_t)picture->w), px0);
	}
	const uint32_t *crow = content->pixels + (size_t)y * content->pitch_px;
	if (px0 > x0) {
		copy_px(dst + (x0 - ax), crow + x0, px0 - x0);
		counts->opaque += (uint64_t)(px0 - x0);
	}
	if (px1 > px0) {
		const uint32_t *prow = picture->pixels + (size_t)py * picture->pitch_px;
		copy_px(dst + (px0 - ax), prow + (px0 - place->origin.x), px1 - px0);
		counts->copied += (uint64_t)(px1 - px0);
	}
	if (x1 > px1) {
		copy_px(dst + (px1 - ax), crow + px1, x1 - px1);
		counts->opaque += (uint64_t)(x1 - px1);
	}
}

// The run [x0, x1) of content row y inside an overlay: each content pixel
// over the picture's, or the content alone where the picture does not reach.
static void overlay_run(uint32_t *dst, const pc_image_t *content, const pc_image_t *picture,
                        const pc_place_t *place, int32_t y, int32_t x0, int32_t x1, int32_t ax,
                        pc_counts_t *counts)
{
	const uint32_t *crow = content->pixels + (size_t)y * content->pitch_px;
	int32_t py = y - place->origin.y;
	bool row_in = py >= 0 && py < (int32_t)picture->h;
	const uint32_t *prow = row_in ? picture->pixels + (size_t)py * picture->pitch_px : 0;
	for (int32_t x = x0; x < x1; x++) {
		int32_t px = x - place->origin.x;
		if (row_in && px >= 0 && px < (int32_t)picture->w) {
			dst[x - ax] = plane_blend(crow[x], prow[px]);
			counts->blended++;
		} else {
			dst[x - ax] = crow[x];
			counts->opaque++;
		}
	}
}

void plane_compose(const pc_image_t *dst, const pc_image_t *content, const pc_image_t *picture,
                   const pc_place_t *place, pc_rect_t area, pc_counts_t *counts)
{
	if (area.w <= 0 || area.h <= 0)
		return;
	int32_t cx0 = place->clip.x, cx1 = place->clip.x + place->clip.w;
	int32_t cy0 = place->clip.y, cy1 = place->clip.y + place->clip.h;
	bool clip_empty = place->clip.w <= 0 || place->clip.h <= 0;
	span_t spans[PLANE_OVERLAYS_MAX];
	for (int32_t y = area.y; y < area.y + area.h; y++) {
		uint32_t *drow = dst->pixels + (size_t)(y - area.y) * dst->pitch_px;
		const uint32_t *crow = content->pixels + (size_t)y * content->pitch_px;
		int32_t ax0 = area.x, ax1 = area.x + area.w;
		if (clip_empty || y < cy0 || y >= cy1 || ax1 <= cx0 || ax0 >= cx1) {
			copy_px(drow, crow + ax0, area.w);
			continue;
		}
		int32_t in0 = max32(ax0, cx0), in1 = min32(ax1, cx1);
		if (in0 > ax0)
			copy_px(drow, crow + ax0, in0 - ax0);
		if (ax1 > in1)
			copy_px(drow + (in1 - ax0), crow + in1, ax1 - in1);
		int n = row_spans(place, y, spans);
		int32_t x = in0;
		for (int i = 0; i < n && x < in1; i++) {
			int32_t s0 = max32(spans[i].x0, in0), s1 = min32(spans[i].x1, in1);
			if (s1 <= s0)
				continue;
			if (s0 > x)
				picture_run(drow, content, picture, place, y, x, s0, ax0, counts);
			overlay_run(drow, content, picture, place, y, s0, s1, ax0, counts);
			x = s1;
		}
		if (x < in1)
			picture_run(drow, content, picture, place, y, x, in1, ax0, counts);
	}
}
