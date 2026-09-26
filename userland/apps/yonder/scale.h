#ifndef YONDER_SCALE_H
#define YONDER_SCALE_H

// A picture drawn into its box: scaled nearest-neighbour to the box's size
// and blended by its alpha over what is already there, only where the box
// meets `clip` (YONDER.md § Y5). Pure over pixels, so the host harness
// checks it pixel by pixel.

#include <stdint.h>

#include "os64/gui.h"

// `dst` is a surface's pixels, `pitch` pixels a row; `box` and `clip` are
// in the surface's coordinates. `src` is 0xAARRGGBB, `sw` x `sh`, tightly
// packed. What is written is opaque.
void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         const uint32_t *src, uint32_t sw, uint32_t sh);

#endif
