#ifndef YONDER_SVG_H
#define YONDER_SVG_H
#include "flow/flow.h"
#include "garb/cascade.h"
#include "svg/svg.h"

// Synchronous document adapter. The window serializes mutation through the
// call; a scene pins document lifetime while preparing and rendering. No I/O.
// SVG, backend and destination allocations share the supplied owner ledger.
// initial_color is the face's initial ink in straight ARGB.
// A successful output belongs to that ledger and is released with yonder_svg_pixels_free.
typedef struct {
    uint32_t *pixels;
    size_t bytes;
    os64_gui_rect_t rect;
} yonder_svg_pixels_t;
svg_status_t yonder_svg_render(const os64_html_document_t *, const garb_cascade_t *,
                               const flow_box_t *, os64_gui_rect_t content, os64_gui_rect_t clip,
                               uint32_t zoom, uint32_t initial_color,
                               const os64_geometry_memory_t *, const os64_geometry_backend_t *,
                               yonder_svg_pixels_t *);
/* Image viewport dimensions are caller-resolved CSS pixels at resolution 1.
 * Root presentation is independent of a host page's box/style. */
typedef struct {
    double width, height;
    // Both zero: one output pixel per CSS pixel. Otherwise the raster extent;
    // zoom changes these, not the CSS viewport or media-query environment.
    uint32_t pixel_width, pixel_height;
    bool dark, reduced_motion;
    void *cancel_context;
    bool (*cancelled)(void *);
} yonder_svg_image_view_t;
/* Internal document adapter entry; the standalone owner constructs the isolated
 * cascade. It uses IMAGE mode and never invokes flow or a resource resolver. */
svg_status_t yonder_svg_render_image_tree(const os64_html_document_t *, const garb_cascade_t *,
                                         const yonder_svg_image_view_t *,
                                         const os64_geometry_memory_t *,
                                         const os64_geometry_backend_t *, yonder_svg_pixels_t *);
void yonder_svg_pixels_free(const os64_geometry_memory_t *, yonder_svg_pixels_t *);
bool yonder_svg_natural_size(const os64_html_node_t *, const garb_cascade_t *, int32_t *, int32_t *);
bool yonder_svg_size(const os64_html_node_t *, int32_t *, int32_t *);
#endif
