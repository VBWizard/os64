#ifndef YONDER_PAINT_H
#define YONDER_PAINT_H

// The page view's painter (YONDER.md § The page view): a laid-out tree and
// a viewport in, drawing out — through its verbs, never straight to a
// surface, so the host harness can record what a page paints and check it
// by hand the way libflow's dumps are checked.

#include "flow/flow.h"

// Everything is in PAGE coordinates — a box in a fixed subtree moved there
// by the scroll, which the painter does for every verb — and the executor
// moves it onto the glass. `clip` is the viewport, handed to every verb
// because a text run and a picture are drawn whole and must be cut by
// whoever draws them.
// A colour is flow's (flow.h): its top byte how transparent it is, which a
// verb blends by.
typedef struct {
    void *ctx;
    void (*fill)(void *ctx, os64_gui_rect_t rect, uint32_t colour);
    // A TEXT or MARKER box: its run with its origin at (x, baseline), which
    // is the box's rect.x and baseline in page coordinates.
    void (*text)(void *ctx, const flow_box_t *box, int32_t x, int32_t baseline,
                 os64_gui_rect_t clip, uint32_t colour);
    // A picture, and a form control, at the box's content rectangle.
    void (*image)(void *ctx, const flow_box_t *box, os64_gui_rect_t content, os64_gui_rect_t clip);
    void (*control)(void *ctx, const flow_box_t *box, os64_gui_rect_t content,
                    os64_gui_rect_t clip);
    // A picture BEHIND a box (YONDER.md § Y5b): true when the box has one,
    // and then, unless `area` is NULL (only asking), it is tiled across
    // `area` from (ox, oy) — drawn if it has arrived, nothing yet if not.
    bool (*backdrop)(void *ctx, const flow_box_t *box, const os64_gui_rect_t *area, int32_t ox,
                     int32_t oy, os64_gui_rect_t clip);
    // A GROUP (flow_visit_groups): everything painted between an open and
    // its close is laid over what was under it at `alpha` of 255 (CSS
    // Color 4 § 3.2). `bounds` holds all of it, cut to the viewport — empty
    // when none of it shows — in page coordinates. Groups nest. Either NULL:
    // groups are painted opaque.
    void (*group_open)(void *ctx, os64_gui_rect_t bounds);
    void (*group_close)(void *ctx, uint8_t alpha);
    // A SHADOW's pixels: `colour`'s RGB laid over each pixel of `rect`
    // (page coordinates, already cut to the viewport) at its own `alpha`,
    // `rect.w` a row, alpha 0 leaving the pixel as it is.
    void (*mask)(void *ctx, os64_gui_rect_t rect, const uint8_t *alpha, uint32_t colour);
} yonder_verbs_t;

// Paints every box that meets `viewport`, canvas first, the page scrolled
// to `scroll` (flow_visit's). `paper` is the canvas when neither the root
// nor the body has a background, colour or picture.
void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                  uint32_t paper, const yonder_verbs_t *verbs);

// Where a sheet's picture behind `box` starts, and on which axes it
// repeats (CSS Backgrounds 3 § 3.4, § 3.6): its position from the corner
// of `rect` — the box's rect in page coordinates — a percentage being of
// the room the `iw` x `ih` picture leaves there, so 100% puts it against
// the far edge and 50% in the middle.
void yonder_background_place(const flow_box_t *box, os64_gui_rect_t rect, uint32_t iw,
                             uint32_t ih, int32_t *ox, int32_t *oy, bool *repeat_x,
                             bool *repeat_y);

// The two tones a bevelled border is drawn in, from its colour.
uint32_t yonder_lighter(uint32_t colour);
uint32_t yonder_darker(uint32_t colour);

#endif
