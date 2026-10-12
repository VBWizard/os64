#ifndef YONDER_WARP_H
#define YONDER_WARP_H

// A group drawn through its matrix, laid onto what is under it (PILE3.md §
// Transforms, the painter). yonder's surfaces have no alpha, and every
// primitive blends as if what is under it were opaque — so a group is not
// painted onto nothing. It is painted TWICE, onto black and onto white:
// source-over is affine in the backdrop, so each channel of each pixel the
// group paints comes out B + T * D for a backdrop D, and the two paintings
// are B (D = 0) and B + 255 T (D = 255), whatever was painted — text edges,
// pictures, nested groups. Pure over pixels, so the host harness checks it
// pixel by pixel.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/gui.h"

// The two paintings, `w` by `h` pixels of 0x00RRGGBB, `w` a row.
typedef struct {
    const uint32_t *black, *white;
    int32_t w, h;
} yonder_sheets_t;

// Lays `s` onto `dst` (`pitch` pixels a row) through `m`, which sends the
// sheets' coordinates to dst's — x' = m[0] x + m[2] y + m[4], y' = m[1] x
// + m[3] y + m[5], a sheet pixel (i, j) being the square from (i, j) to (i
// + 1, j + 1) — only inside `clip` (dst's coordinates), and under the
// group's own opacity `alpha` of 255: each channel a B + (1 - a + a T) D.
// Each dst pixel's centre is taken back through `inv`, `m` undone — the
// caller's, by the one rule the page's matrices are undone by
// (flow_affine_invert), so what is laid and what is hit agree — and
// sampled from the sheets: BY INDEX when `m` is a quarter turn or a mirror
// and its translation whole, an exact copy; else BILINEAR, B and T mixed
// apart from the four pixels round it, outside the sheets nothing painted.
// A pixel nothing reaches is left as it is. False, and nothing laid, when
// there is no `inv` or nothing to lay.
bool yonder_warp(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const yonder_sheets_t *s,
                 const double m[6], const double inv[6], uint8_t alpha);

// Whether `m` is one the warp copies by index.
bool yonder_warp_exact(const double m[6]);

#endif
