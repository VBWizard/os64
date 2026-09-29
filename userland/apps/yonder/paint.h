#ifndef YONDER_PAINT_H
#define YONDER_PAINT_H

// The page view's painter (YONDER.md § The page view): a laid-out tree and
// a viewport in, drawing out — through four verbs, never straight to a
// surface, so the host harness can record what a page paints and check it
// by hand the way libflow's dumps are checked.

#include "flow/flow.h"

// Everything is in PAGE coordinates; the executor moves it onto the glass.
// `clip` is the viewport, handed to every verb because a text run and a
// picture are drawn whole and must be cut by whoever draws them.
typedef struct {
    void *ctx;
    void (*fill)(void *ctx, os64_gui_rect_t rect, uint32_t colour);
    // A TEXT or MARKER box: its run at (rect.x, baseline).
    void (*text)(void *ctx, const flow_box_t *box, os64_gui_rect_t clip, uint32_t colour);
    // A picture, and a form control, at the box's content rectangle.
    void (*image)(void *ctx, const flow_box_t *box, os64_gui_rect_t content, os64_gui_rect_t clip);
    void (*control)(void *ctx, const flow_box_t *box, os64_gui_rect_t content,
                    os64_gui_rect_t clip);
} yonder_verbs_t;

// Paints every box that meets `viewport`, canvas first. `paper` is the
// canvas when neither the root nor the body has a background.
void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, uint32_t paper,
                  const yonder_verbs_t *verbs);

// The two tones a bevelled border is drawn in, from its colour.
uint32_t yonder_lighter(uint32_t colour);
uint32_t yonder_darker(uint32_t colour);

#endif
