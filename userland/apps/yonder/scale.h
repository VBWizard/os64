#ifndef YONDER_SCALE_H
#define YONDER_SCALE_H

// A picture drawn into its box: scaled to the box's size and blended by its
// alpha over what is already there, only where the box meets `clip`
// (YONDER.md § Y5); and a background tiled (§ Y5b). A scaled picture is
// SMOOTHED — each pixel mixed from the four source pixels round where it
// falls — or, not, nearest-neighbour, its pixels kept square
// (`image-rendering: pixelated`). Pure over pixels, so the host harness
// checks them pixel by pixel.

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

#endif
