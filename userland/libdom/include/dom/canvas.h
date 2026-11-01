#ifndef OS64_DOM_CANVAS_H
#define OS64_DOM_CANVAS_H
#include <dom/dom.h>
#include <canvas/canvas.h>
#include <os64/font_provider.h>

typedef enum { OS64_DOM_IMAGE_PENDING, OS64_DOM_IMAGE_READY, OS64_DOM_IMAGE_BROKEN,
               OS64_DOM_IMAGE_REFUSED } os64_dom_image_status_t;
/* A synchronous read-only image loan. acquire does not enter script. READY
 * supplies natural dimensions and origin-clean status; pixels=true also asks
 * for natural-size sRGB pixels. An optional token tracks the loan. release is
 * called for every acquire, including status-only queries and refusal. Providers
 * must keep the view stable until release and may not lend decoder-private
 * storage that can change on a worker. A zeroed provider reports pending. */
typedef struct {
    draw2d_surface_t surface;
    bool origin_clean;
    void *token;
} os64_dom_image_view_t;
typedef struct {
    os64_dom_image_status_t (*acquire)(void *,const os64_html_node_t *,bool pixels,os64_dom_image_view_t *);
    void (*release)(void *,os64_dom_image_view_t *);
    void *opaque;
} os64_dom_image_provider_t;
/* The host resolves its existing font families and lays out a retained run.
 * No script reentry. Inputs are borrowed for the call; success transfers run
 * ownership to the caller. No drawing state or markup crosses this boundary. */
typedef struct {
    os64_font_status_t (*layout)(void *,const os64_font_family_list_t *,bool bold,bool italic,
        uint32_t pixel_height,const uint8_t *,size_t,os64_text_run_t **);
    void *opaque;
} os64_dom_text_provider_t;
#pragma GCC visibility push(default)
void os64_dom_set_text(os64_dom_t *,const os64_dom_text_provider_t *);
void os64_dom_set_images(os64_dom_t *,const os64_dom_image_provider_t *);
/* Poll wrapped image elements for a completed request. No script enters here;
 * consume marks one event delivered before the host dispatches it as a task. */
bool os64_dom_image_event(os64_dom_t *,const os64_html_node_t **,bool *failed,bool consume);
/* Borrow a read-only premultiplied bitmap from an existing context; never
 * creates a JS context. Applies pending resets. False for absent/lost/closed
 * contexts. The view expires at the next Canvas call or DOM mutation. */
bool os64_dom_canvas_bitmap(os64_dom_t *, const os64_html_node_t *, canvas_bitmap_t *);
/* Pending pixels/resets need presentation, not layout. The host may dirty a
 * conservative viewport; hidden pixels will be sampled when exposed. */
bool os64_dom_canvas_dirty(os64_dom_t *, bool (*visible)(void *, const os64_html_node_t *), void *);
/* Visit connected canvases with pending presentation damage. No script runs;
 * the callback must not mutate the DOM or Canvas. A NULL damage pointer means
 * a pending reset and requires conservative invalidation. Otherwise width,
 * height and damage use bitmap coordinates; the record is borrowed for this
 * callback. Visiting does not acknowledge damage or apply pending resets. */
typedef void (*os64_dom_canvas_damage_fn)(void *,const os64_html_node_t *,uint32_t,uint32_t,
                                         const draw2d_damage_t *);
void os64_dom_canvas_visit_damage(os64_dom_t *,os64_dom_canvas_damage_fn,void *);
/* After synchronous successful consumption of this canvas, with no script
 * or DOM mutation between the bitmap lookup and acknowledgement. Failure
 * leaves damage pending. */
void os64_dom_canvas_painted(os64_dom_t *, const os64_html_node_t *);
#pragma GCC visibility pop
#endif
