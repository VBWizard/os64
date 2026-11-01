#ifndef YONDER_FILTER_H
#define YONDER_FILTER_H

// A filtered group (FILTER.md): painted onto black and onto white, as a
// group laid through its matrix is (warp.h), its pixels and how much of
// each it covers recovered from the two, the filter list run over them in
// order, and the result laid over what is under it. Pure over pixels, so
// the host harness checks it pixel by pixel.
//
// A LAYER is `w` by `h` pixels of premultiplied 0xAARRGGBB, `w` a row:
// AA how much of the pixel the group covers, each colour channel at most
// AA.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flow/flow.h"
#include "warp.h"

// From the two paintings, each pixel's colour and coverage, into `out`
// (s->w by s->h). Painted over black a channel is B = c a; over white it is
// B + 255 (1 - a): so a is 255 less what the white let through, taken
// from green, the channel with the most precision, and the premultiplied
// colour is B itself, held at most a. `out` may be s->black: each pixel
// is read before it is written.
void yonder_filter_recover(const yonder_sheets_t *s, uint32_t *out);

// The bytes of work yonder_filter_run needs for a `w` by `h` layer and
// list `f` of `n`.
size_t yonder_filter_work_bytes(int32_t w, int32_t h, const flow_filter_t *f, int32_t n);

// Each function of `f` (n of them) over the layer in order (Filter Effects
// 1 § 13), each result clamped before the next, as each is its own filter
// primitive. The colour functions work on the colour unpremultiplied; a
// blur and a drop shadow's blur are three box blurs of radius blur / 2
// each (flow_filter_t.shadow), as a box shadow's are, held at
// FLOW_FILTER_BLUR_MAX, reading nothing past the layer; a drop shadow is
// the layer's coverage blurred, moved by its offsets and laid under the
// layer in its colour — `current` for one in currentColor. `work` holds
// yonder_filter_work_bytes. A URL draws the list unfiltered: the caller's
// to have refused (FILTER.md § 2).
void yonder_filter_run(uint32_t *px, int32_t w, int32_t h, const flow_filter_t *f, int32_t n,
                       uint32_t current, void *work);

// The layer laid over `dst` (`pitch` pixels a row, 0x..RRGGBB, opaque):
// layer pixel (i, j) onto dst's (i + dx, j + dy), only inside `clip`
// (dst's coordinates), at `alpha` of 255 — each channel c alpha + d (1 -
// a alpha), the top byte of dst kept.
void yonder_filter_lay(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const uint32_t *px,
                       int32_t w, int32_t h, int32_t dx, int32_t dy, uint8_t alpha);

#endif
