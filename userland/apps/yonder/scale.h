#ifndef YONDER_SCALE_H
#define YONDER_SCALE_H

// A picture drawn into its box: scaled to the box's size and blended by its
// alpha over what is already there, only where the box meets `clip`
// (YONDER.md § Y5); and a background tiled (§ Y5b). A scaled picture is
// SMOOTHED — each pixel mixed from the four source pixels round where it
// falls — or, not, nearest-neighbour, its pixels kept square
// (`image-rendering: pixelated`). And a picture made smaller to be kept
// (yonder_reduce_picture), on a worker. Pure over pixels, so the host
// harness checks them pixel by pixel.

#include <stdbool.h>
#include <stdint.h>

#include "os64/gui.h"

// `dst` is a surface's pixels, `pitch` pixels a row; `box` and `clip` are
// in the surface's coordinates. `src` is 0xAARRGGBB, `sw` x `sh`, tightly
// packed. What is written is opaque.
void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         bool smooth, const uint32_t *src, uint32_t sw, uint32_t sh);

// A picture TILED across `area`: each copy scaled to `tile`'s size, laid
// from `tile` in every direction — or, on an axis that does not repeat,
// the one copy at `tile` — blended the same way, only where `area` meets
// `clip`.
void yonder_tile_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t area,
                         os64_gui_rect_t tile, bool repeat_x, bool repeat_y, bool smooth,
                         const uint32_t *src, uint32_t sw, uint32_t sh);

// THE SIZE A PICTURE IS KEPT AT (PICTURE_SIZES.md): a picture `nw` x `nh`
// drawn to cover a `dw` x `dh` rectangle, scaled by one factor so its shape
// holds — the axis that asks more is met exactly, the other rounded up —
// and never past its own size. A rectangle with no area keeps the picture
// whole. Asked again of its own answer it answers no less, so a picture
// kept at what this said is never found smaller than it needs.
void yonder_picture_kept_size(uint32_t nw, uint32_t nh, uint32_t dw, uint32_t dh, uint32_t *kw,
                              uint32_t *kh);

// `src` (`sw` x `sh`, 0xAARRGGBB) reduced to `dw` x `dh` into `dst`, each
// pixel the average of every source pixel under it, weighed by how much of
// it is under and by its alpha, so a transparent pixel lends no colour. For
// `dw` <= `sw` and `dh` <= `sh`, none of them zero; false (and `dst`
// untouched) otherwise, or with no memory for one row's sums.
bool yonder_reduce_picture(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t *dst,
                           uint32_t dw, uint32_t dh);

#endif
