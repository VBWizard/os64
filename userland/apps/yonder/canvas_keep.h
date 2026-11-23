#ifndef YONDER_CANVAS_KEEP_H
#define YONDER_CANVAS_KEEP_H

// KEPT CANVAS PICTURES: a canvas drawn smoothed into a box of another size
// is its bitmap scaled to the box (scale.c), the dearest thing a canvas
// costs a paint, and paid again at every draw though the bitmap may stand
// still for seconds (a page's glow redrawn once in five, a chart never).
// So the scaled pixels are kept in the paint cache's store and budget
// (paint_cache.h), in KEEP_BLOCK-pixel blocks of the box, each found by
// the bitmap's revision (canvas_bitmap_t's: equal revisions are equal
// pixels), the box's size, the source's alpha convention and the block's
// place, and a draw lays what is kept (yonder_lay_scaled) instead of
// scaling again: the same pixels, since the blocks hold exactly what the
// scaler would have laid (yonder_scale_part), wherever a draw's edges fall.
//
// The rules, each held by tools/test_yonder_host.sh's canvas_keep_cases:
// - A canvas keeps nothing at a revision and size until a paint of the
//   window after the one that first drew it there: every draw in that
//   first paint (a group's two sheets, the parts of a view a cover splits)
//   draws as ever, so a canvas redrawn at every frame never pays for a
//   keep it will not use. A draw in a later paint keeps the blocks it
//   reads, and finds them after.
// - A canvas drawn at another revision or size lets go of every block it
//   kept (counted as dropped for a change): they can never be found again.
// - A draw that copies at the bitmap's own size, or draws its pixels
//   square (not smoothed), keeps nothing: there is nothing dear to keep.
// - Blocks that do not fit the budget, or find no memory, are not kept:
//   the draw draws as ever and is counted.
// One window's, on its one thread: no lock.

#include "paint.h"
#include <draw2d/paint.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct yonder_canvas_keeps yonder_canvas_keeps_t;

// A keep for at most KEEP_CANVASES canvases at once, the one drawn longest
// ago forgotten past them (its blocks age out of the cache as any do); NULL
// for no memory.
yonder_canvas_keeps_t *yonder_canvas_keeps_new(void);
void yonder_canvas_keeps_free(yonder_canvas_keeps_t *k);
// A paint of the window begins: every draw until the next is this paint's,
// however many parts of the view it paints.
void yonder_canvas_keeps_paint(yonder_canvas_keeps_t *k);

// yonder_draw_surface's draw of canvas `who` (any pointer naming it; two
// canvases never share one) at `revision` (0: unknown, never kept), its
// pixels from what `cache` keeps by the rules above. True as that draw
// says; a NULL keep or cache draws as ever.
bool yonder_canvas_keep_draw(yonder_canvas_keeps_t *k, yonder_paint_cache_t *cache, const void *who,
                             uint64_t revision, uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip,
                             os64_gui_rect_t box, bool smooth, const draw2d_surface_t *source);

// Since the counts last restarted: blocks kept (made), found, let go
// because their canvas was drawn at another revision or size, and draws
// that drew as ever because their blocks did not fit or found no memory.
typedef struct {
    uint64_t kept, found, dropped, refused;
} yonder_canvas_keep_counts_t;
yonder_canvas_keep_counts_t yonder_canvas_keep_counts(const yonder_canvas_keeps_t *k);
void yonder_canvas_keep_counts_restart(yonder_canvas_keeps_t *k);

#endif
