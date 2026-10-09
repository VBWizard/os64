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
    // A picture BEHIND a box (YONDER.md § Y5b). With `area` NULL, only
    // asks: true when a sheet or an attribute puts one there. Otherwise
    // sheet layer `layer`'s picture — or, at -1, the `background`
    // attribute's — is tiled across `area` (the layer's clip box, or the
    // view for the canvas's): a layer's as its size and position place it
    // in `origin` (yonder_background_tile), an attribute's from (ox, oy);
    // drawn if it has arrived, nothing yet if not. `origin` is the
    // painter's answer to which box: the layer's origin box of the box, or
    // for the canvas the ROOT's, whichever element the picture came from
    // (Backgrounds 3 § 2.11.2).
    bool (*backdrop)(void *ctx, const flow_box_t *box, int32_t layer, const os64_gui_rect_t *area,
                     os64_gui_rect_t origin, int32_t ox, int32_t oy, os64_gui_rect_t clip);
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
    // A picture the painter made — a GRADIENT's pixels: `rect` (page
    // coordinates, already cut to the viewport) of 0xAARRGGBB, straight
    // alpha, `rect.w` a row, each laid over what is there.
    void (*pixels)(void *ctx, os64_gui_rect_t rect, const uint32_t *argb);
} yonder_verbs_t;

// Whose background is the CANVAS's (CSS 2.1 § 14.2): the root's when it has
// one, colour or picture, else the body's when it has one, else none. The
// painter's own rule, for a face that must know it apart from painting —
// the canvas is behind the whole view wherever its owner's box has gone.
// `verbs->backdrop` is asked with no area, which only asks.
const flow_box_t *yonder_canvas_owner(const flow_tree_t *tree, const yonder_verbs_t *verbs);

// DARK PAGES (yonder.conf's `appearance = dark`): every colour the page
// paints is drawn at its HSL lightness moved, hue and saturation kept. A
// background or a gradient is PAPER, and goes dark if it is lighter than
// half; text, a decoration, a bullet or a border is INK, and goes light if
// it is darker than half. Each other colour, and every picture, is the
// page's. A page with a dark design of its own (it was told, by
// `prefers-color-scheme`) has nothing to move. `paper` is the canvas of a
// page that set none.
typedef struct {
    uint32_t paper;
} yonder_dark_t;

// Paints every box that meets `viewport`, canvas first, the page scrolled
// to `scroll` (flow_visit's). `paper` is the canvas when neither the root
// nor the body has a background, colour or picture; `dark`, when not NULL,
// paints the page dark (above) and its paper is the canvas instead.
void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                  uint32_t paper, const yonder_dark_t *dark, const yonder_verbs_t *verbs);

// One of `box`'s boxes — its border box `rect` (in any coordinates), or
// the padding or content box inside it. A percentage padding counts as
// none, as content_of's does; TEXT is the border box.
os64_gui_rect_t yonder_box_edge(const flow_box_t *box, os64_gui_rect_t rect, flow_edge_t edge);

// A sheet's background layer, one copy of it: where it is and how big, and
// on which axes it repeats (CSS Backgrounds 3 § 3.4–§ 3.9), from layer `l`
// and its ORIGIN box `area`. `iw` x `ih` is the picture's own size, 0 x 0
// for one with none (a gradient), which is then the origin box's. A
// percentage position is of the room the copy leaves, so 100% puts it
// against the far edge and 50% in the middle. False when the copy is
// empty, and nothing is drawn.
typedef struct {
    os64_gui_rect_t at;
    bool repeat_x, repeat_y;
} yonder_tile_t;
bool yonder_background_tile(const flow_layer_t *l, os64_gui_rect_t area, uint32_t iw, uint32_t ih,
                            yonder_tile_t *out);

// The two tones a bevelled border is drawn in, from its colour.
uint32_t yonder_lighter(uint32_t colour);
uint32_t yonder_darker(uint32_t colour);

#endif
