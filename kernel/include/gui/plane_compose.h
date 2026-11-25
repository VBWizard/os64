#ifndef GUI_PLANE_COMPOSE_H
#define GUI_PLANE_COMPOSE_H

// plane_compose.h — how a window's content and a plane's picture make the
// pixels the compositor draws where the plane shows through
// (docs/design/pending/VIDEO_PLANE.md § 3.3, § 3.4).
//
// INSIDE A PLANE'S CLIP the content is declared see-through except where an
// overlay rectangle says it carries something drawn over the picture:
//   - outside the overlay rectangles the content is not read: the picture's
//     row is copied;
//   - inside their union each content pixel's top byte `a` says how much of
//     the content shows: 255 the content, 0 the picture, between the two
//     blended, `content + (picture * (255 - a) + 127) / 255` per channel,
//     all four alike, rounded to nearest (the content is premultiplied);
//   - where the clip reaches past the picture (between a window's resize and
//     its next publish), the content there is read as opaque;
//   - outside the clip the content is drawn as it always is.
// Overlay rectangles may overlap; a pixel inside two is blended once.
//
// All coordinates are the window's content's. Pure: no kernel headers,
// integer only, no allocation. Built at -O2 (kernel/GNUmakefile) because it
// runs under kGuiLock at every picture; tools/test_plane_compose_host.sh
// drives it beside a model.

#include <stdbool.h>
#include <stdint.h>

#define PLANE_OVERLAYS_MAX 8

typedef struct {
	int32_t x, y, w, h;
} pc_rect_t;

// A plane's place: where its picture's first pixel lands and its size
// (`origin`), the part of the content it shows through (`clip`), and the
// overlay rectangles inside the clip.
typedef struct {
	pc_rect_t origin;
	pc_rect_t clip;
	pc_rect_t overlay[PLANE_OVERLAYS_MAX];
	uint32_t noverlays;
} pc_place_t;

// Pixels and their shape: a window's content, a plane's picture, or the
// part of the back buffer being drawn.
typedef struct {
	uint32_t *pixels;
	uint32_t pitch_px, w, h;
} pc_image_t;

// What a composition did, for /sys/gui: picture pixels copied, content
// pixels inside an overlay read and put over the picture (whatever their
// top byte: 0 and 255 take plane_blend's shortcuts, and are counted here
// too), and content pixels inside the clip read as opaque because the
// picture did not reach them.
typedef struct {
	uint64_t copied, blended, opaque;
} pc_counts_t;

// One pixel: `content` premultiplied over `picture` by the content's top byte.
uint32_t plane_blend(uint32_t content, uint32_t picture);

// The pixels inside the union of a place's overlay rectangles, each counted
// once.
uint64_t plane_overlay_area(const pc_place_t *place);

// Whether a place can be committed to a window whose content is
// `content_w` x `content_h`: every rectangle has a size no smaller than
// zero and no sum overflows; the clip is inside the content; at most
// PLANE_OVERLAYS_MAX overlays, each inside the clip; their union no larger
// than `overlay_budget` pixels. An empty clip (no width or no height) is
// the empty place and is always valid.
bool plane_place_valid(const pc_place_t *place, uint32_t content_w, uint32_t content_h, uint64_t overlay_budget);

// Draws the part `area` of the content into `dst`, whose first pixel is
// where content pixel (area.x, area.y) lands: the content as it is outside
// the place's clip, and inside it the picture and the overlays as above.
// `area` must lie inside the content; `counts` is added to.
void plane_compose(const pc_image_t *dst, const pc_image_t *content, const pc_image_t *picture,
                   const pc_place_t *place, pc_rect_t area, pc_counts_t *counts);

#endif
