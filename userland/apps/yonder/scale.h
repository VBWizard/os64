#ifndef YONDER_SCALE_H
#define YONDER_SCALE_H

// A picture drawn into its box: scaled nearest-neighbour to the box's size
// and blended by its alpha over what is already there, only where the box
// meets `clip` (YONDER.md § Y5); and a background tiled (§ Y5b). Pure over
// pixels, so the host harness checks them pixel by pixel.

#include <stdint.h>

#include "os64/gui.h"

// `dst` is a surface's pixels, `pitch` pixels a row; `box` and `clip` are
// in the surface's coordinates. `src` is 0xAARRGGBB, `sw` x `sh`, tightly
// packed. What is written is opaque.
void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         const uint32_t *src, uint32_t sw, uint32_t sh);

// A picture TILED across `area`, its copies laid from (`ox`, `oy`) in
// every direction, blended the same way, only where `area` meets `clip`.
// Unscaled: a background is drawn at its own size.
void yonder_tile_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t area,
                         int32_t ox, int32_t oy, const uint32_t *src, uint32_t sw, uint32_t sh);

#endif
