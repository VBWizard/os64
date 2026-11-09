#ifndef YONDER_SVG_H
#define YONDER_SVG_H
#include "flow/flow.h"
#include "garb/cascade.h"
#include "svg/svg.h"

typedef struct yonder_svg_cache yonder_svg_cache_t;
typedef struct {
    svg_raster_cache_stats_t raster;
    size_t owned_bytes; // Includes the wrapper, context and retained entry allocations.
    uint64_t rasterizations, bypasses;
} yonder_svg_cache_stats_t;
// Window-serialized optional storage. Allocation failure leaves rendering usable
// with a NULL cache. The memory owner and backend descriptors must outlive it.
yonder_svg_cache_t *yonder_svg_cache_create(const os64_geometry_memory_t *, size_t bytes,
                                            size_t entries, size_t key_bytes);
void yonder_svg_cache_free(yonder_svg_cache_t *);
yonder_svg_cache_stats_t yonder_svg_cache_stats(const yonder_svg_cache_t *);
// Reclaim entries for the shared allocator. During a cache operation this is a
// no-op, avoiding allocator callback reentry into the owning libsvg context.
void yonder_svg_cache_trim(yonder_svg_cache_t *);

// Synchronous document adapter. The window serializes mutation through the
// call; a scene pins document lifetime while preparing and rendering. No I/O.
// SVG, backend and destination allocations share the supplied owner ledger.
// initial_color is the face's initial ink in straight ARGB.
// Output pixels are straight ARGB8. A successful output belongs to that
// ledger and is released with yonder_svg_pixels_free.
typedef struct {
    uint32_t *pixels;
    size_t bytes;
    os64_gui_rect_t rect;
    // Displayed unsupported drawing subtrees omitted from this successful
    // render, counted per occurrence (including use instances), not per paint.
    // Zero on failure; resources and deliberately inert content do not count.
    uint32_t omitted_subtrees;
} yonder_svg_pixels_t;
svg_status_t yonder_svg_render(const os64_html_document_t *, const garb_cascade_t *,
                               const flow_box_t *, os64_gui_rect_t content, os64_gui_rect_t clip,
                               uint32_t zoom, uint32_t initial_color,
                               const os64_geometry_memory_t *, const os64_geometry_backend_t *,
                               yonder_svg_cache_t *, yonder_svg_pixels_t *);
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
    yonder_svg_cache_t *cache; // Optional; stores resolved pixels, not document/cascade pointers.
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
// Local geometry only; resolves the live cascade without rendering a scene.
svg_status_t yonder_svg_contains(const os64_html_document_t *, const garb_cascade_t *,
    const flow_tree_t *, const os64_html_node_t *, uint32_t zoom,
    const os64_geometry_memory_t *, bool (*cancelled)(void *), void *,
    double x, double y, bool *inside);
#endif
