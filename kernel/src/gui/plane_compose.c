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
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

// All four channels at once, each in a 16-bit lane of one 64-bit word
// (`spread`, and `gather` back). `keep` is 1 to 254 here, so a lane's
// `t = p * keep + 128` is at most 64,898 and `t + (t >> 8)` at most 65,151:
// lanes never carry into each other, and `(t + (t >> 8)) >> 8` is
// `(p * keep + 127) / 255` exactly (every p and keep checked once by brute
// force; tools/test_plane_compose_host.sh checks every content and picture
// byte against the formula). A lane that passes 255 when the
// content is added (content that is not premultiplied) is held at 255.
#define LANES_LOW 0x00FF00FF00FF00FFull

static inline uint64_t spread(uint32_t x)
{
	return (uint64_t)(x & 0x00FF00FFu) | ((uint64_t)(x & 0xFF00FF00u) << 24);
}

static inline uint32_t gather(uint64_t v)
{
	return (uint32_t)(v & 0x00FF00FFu) | (uint32_t)((v >> 24) & 0xFF00FF00u);
}

// A content pixel whose top byte `a` is neither 0 nor 255, over the picture.
static inline uint32_t blend_between(uint32_t content, uint32_t picture, uint32_t a)
{
	uint64_t t = spread(picture) * (255 - a) + 0x0080008000800080ull;
	uint64_t v = (((t + ((t >> 8) & LANES_LOW)) >> 8) & LANES_LOW) + spread(content);
	uint64_t over = v & 0x0100010001000100ull;
	return gather((v | (over - (over >> 8))) & LANES_LOW);
}

// The whole rule for one pixel, inlined into overlay_run's loop, where the
// two cases that need no arithmetic cost a test each.
static inline uint32_t blend_one(uint32_t content, uint32_t picture)
{
	uint32_t a = content >> 24;
	if (a == 255)
		return content;
	if (a == 0)
		return picture;
	return blend_between(content, picture, a);
}

uint32_t plane_blend(uint32_t content, uint32_t picture)
{
	return blend_one(content, picture);
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

// The part [*px0, *px1) of the run [x0, x1) of content row y that the
// picture reaches, and the picture's pixel for column *px0; NULL, with both
// ends at x1, when it reaches none of it. A run is then three parts in
// order, any of them empty: before the picture, the picture, after it. A
// position relative to the origin is worked out in 64 bits: a valid origin
// may lie anywhere, INT32_MIN included, and the distance from it to a
// content pixel need not fit in 32.
static const uint32_t *picture_span(const pc_image_t *picture, const pc_place_t *place, int32_t y, int32_t x0,
                                    int32_t x1, int32_t *px0, int32_t *px1)
{
	int64_t py = (int64_t)y - place->origin.y;
	*px0 = *px1 = x1;
	if (py < 0 || py >= (int64_t)picture->h)
		return NULL;
	int64_t ox = place->origin.x;
	*px0 = (int32_t)min64(max64(x0, ox), x1);
	*px1 = (int32_t)max64(min64(x1, ox + picture->w), *px0);
	if (*px1 == *px0) {
		*px0 = *px1 = x1;
		return NULL;
	}
	return picture->pixels + (size_t)py * picture->pitch_px + ((int64_t)*px0 - ox);
}

// The content of row y from x0 to x1, as opaque.
static void opaque_part(uint32_t *dst, const uint32_t *crow, int32_t x0, int32_t x1, int32_t ax, pc_counts_t *counts)
{
	if (x1 > x0) {
		copy_px(dst + (x0 - ax), crow + x0, x1 - x0);
		counts->opaque += (uint64_t)(x1 - x0);
	}
}

// The run [x0, x1) of content row y inside the clip and outside every
// overlay: the picture's pixels where it reaches, the content as opaque
// where it does not.
static void picture_run(uint32_t *dst, const pc_image_t *content, const pc_image_t *picture,
                        const pc_place_t *place, int32_t y, int32_t x0, int32_t x1, int32_t ax,
                        pc_counts_t *counts)
{
	int32_t px0, px1;
	const uint32_t *prow = picture_span(picture, place, y, x0, x1, &px0, &px1);
	const uint32_t *crow = content->pixels + (size_t)y * content->pitch_px;
	opaque_part(dst, crow, x0, px0, ax, counts);
	if (prow != NULL) {
		copy_px(dst + (px0 - ax), prow, px1 - px0);
		counts->copied += (uint64_t)(px1 - px0);
	}
	opaque_part(dst, crow, px1, x1, ax, counts);
}

// The run [x0, x1) of content row y inside an overlay: each content pixel
// over the picture's where it reaches, the content as opaque where it does
// not.
static void overlay_run(uint32_t *dst, const pc_image_t *content, const pc_image_t *picture,
                        const pc_place_t *place, int32_t y, int32_t x0, int32_t x1, int32_t ax,
                        pc_counts_t *counts)
{
	int32_t px0, px1;
	const uint32_t *prow = picture_span(picture, place, y, x0, x1, &px0, &px1);
	const uint32_t *crow = content->pixels + (size_t)y * content->pitch_px;
	opaque_part(dst, crow, x0, px0, ax, counts);
	if (prow != NULL) {
		uint32_t *d = dst + (px0 - ax);
		const uint32_t *c = crow + px0;
		int32_t n = px1 - px0;
		for (int32_t i = 0; i < n; i++)
			d[i] = blend_one(c[i], prow[i]);
		counts->blended += (uint64_t)n;
	}
	opaque_part(dst, crow, px1, x1, ax, counts);
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
