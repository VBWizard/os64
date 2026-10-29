#ifndef OS64_DOM_CANVAS_H
#define OS64_DOM_CANVAS_H
#include <dom/dom.h>
#include <canvas/canvas.h>

#pragma GCC visibility push(default)
/* Borrow a read-only premultiplied bitmap from an existing context; never
 * creates a JS context. Applies pending resets. False for absent/lost/closed
 * contexts. The view expires at the next Canvas call or DOM mutation. */
bool os64_dom_canvas_bitmap(os64_dom_t *, const os64_html_node_t *, canvas_bitmap_t *);
/* Pending pixels/resets need presentation, not layout. The host may dirty a
 * conservative viewport; hidden pixels will be sampled when exposed. */
bool os64_dom_canvas_dirty(os64_dom_t *, bool (*visible)(void *, const os64_html_node_t *), void *);
/* After synchronous successful consumption of this canvas, with no script
 * or DOM mutation between the bitmap lookup and acknowledgement. Failure
 * leaves damage pending. */
void os64_dom_canvas_painted(os64_dom_t *, const os64_html_node_t *);
#pragma GCC visibility pop
#endif
