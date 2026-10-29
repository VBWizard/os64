#ifndef OS64_CANVAS_H
#define OS64_CANVAS_H
#include <draw2d/paint.h>
#include <os64/geometry_backend.h>

/* Native bitmap/context, with no DOM or script objects. The binding owns this
 * handle and keeps it alive for retained element/context wrappers. Calls are
 * serialized by that owner; callbacks must not reenter the context. */
typedef struct canvas_context canvas_context_t;
typedef enum {
    CANVAS_OK, CANVAS_BAD_ARGUMENT, CANVAS_LIMIT, CANVAS_NO_MEMORY, CANVAS_BUSY, CANVAS_UNSUPPORTED
} canvas_status_t;
typedef struct {
    os64_geometry_memory_t memory; /* Shared owner ledger, including temporary resize peaks. */
    uint32_t max_dimension;
    uint64_t max_pixels, max_draw_pixels;
    size_t max_bytes, max_saved_states; /* Literal limits, including zero. */
    const os64_geometry_backend_t *geometry; /* Borrowed backend table; NULL refuses path rendering. */
    size_t max_commands;
    uint64_t max_geometry_work;
} canvas_options_t;
typedef struct {
    uint32_t fill_argb; /* Straight-alpha sRGB. */
    double global_alpha;
    double fill_alpha; /* Unquantized source alpha for state serialization. */
    uint32_t stroke_argb;
    double stroke_alpha;
    os64_geometry_map_t transform;
    os64_geometry_stroke_t line;
    double dash[256], dash_offset;
    size_t dash_count;
    draw2d_operator_t composite;
} canvas_state_t;
typedef struct {
    const uint32_t *pixels; /* Borrowed premultiplied 0xAARRGGBB, sRGB. */
    uint32_t width, height;
    size_t stride, capacity;
} canvas_bitmap_t;
/* Byte-addressed, straight-alpha RGBA8 in sRGB. The descriptor borrows
 * caller storage; padding is not image data. */
typedef struct {
    uint8_t *bytes;
    uint32_t width, height;
    size_t stride, capacity;
} canvas_rgba8_t;
typedef struct {
    size_t live_bytes, peak_bytes, saved_states;
} canvas_stats_t;

#pragma GCC visibility push(default)
/* Caller supplies memory callbacks. Defaults are policy caps, not a promise
 * that allocation succeeds. Width/height zero is a valid empty bitmap. */
canvas_options_t canvas_options_default(os64_geometry_memory_t);
canvas_status_t canvas_create(const canvas_options_t *, uint32_t width, uint32_t height,
                               canvas_context_t **out);
canvas_status_t canvas_destroy(canvas_context_t *);
/* Successful reset clears pixels, restores defaults and empties the save
 * stack, even for unchanged dimensions. The handle stays stable. Failure
 * leaves dimensions, pixels and state intact; the binding must handle the
 * status rather than claim a DOM resize succeeded. */
canvas_status_t canvas_reset(canvas_context_t *, uint32_t width, uint32_t height);
canvas_status_t canvas_save(canvas_context_t *);
canvas_status_t canvas_restore(canvas_context_t *); /* Empty stack is a no-op. */
canvas_status_t canvas_set_fill(canvas_context_t *, uint32_t straight_argb);
/* RGB is straight sRGB in the low 24 bits; retain the finite [0,1] alpha
 * in state and quantize it at the ARGB8 paint boundary. */
canvas_status_t canvas_set_fill_rgb_alpha(canvas_context_t *, uint32_t rgb, double alpha);
/* Nonfinite/out-of-range values are ignored, matching Canvas assignment. */
canvas_status_t canvas_set_alpha(canvas_context_t *, double);
/* Rectangles use the current transform and clip without altering the current
 * path. clear ignores paint, alpha and the composite operator. */
canvas_status_t canvas_fill_rect(canvas_context_t *, double x, double y, double w, double h);
canvas_status_t canvas_clear_rect(canvas_context_t *, double x, double y, double w, double h);
/* The current path stores transformed points and is not saved/restored.
 * Appends are transactional on resource refusal; nonfinite arguments leave
 * it unchanged. arcTo establishes an empty path's first point before rejecting
 * a negative radius. Stroke uses current transform's pen in user space. */
canvas_status_t canvas_begin_path(canvas_context_t *);
canvas_status_t canvas_path_command(canvas_context_t *, const os64_geometry_command_t *);
canvas_status_t canvas_path_rect(canvas_context_t *, double x,double y,double w,double h);
canvas_status_t canvas_ellipse(canvas_context_t *,double x,double y,double rx,double ry,
    double rotation,double start,double end,bool anticlockwise);
canvas_status_t canvas_arc_to(canvas_context_t *,double x1,double y1,double x2,double y2,double radius);
canvas_status_t canvas_fill(canvas_context_t *,bool even_odd);
canvas_status_t canvas_stroke(canvas_context_t *);
canvas_status_t canvas_clip(canvas_context_t *,bool even_odd);
canvas_status_t canvas_stroke_rect(canvas_context_t *,double x,double y,double w,double h);
canvas_status_t canvas_transform(canvas_context_t *,const os64_geometry_map_t *,bool replace);
canvas_status_t canvas_set_stroke(canvas_context_t *,uint32_t rgb,double alpha);
canvas_status_t canvas_set_line(canvas_context_t *,const os64_geometry_stroke_t *);
canvas_status_t canvas_set_dash(canvas_context_t *,const double *,size_t);
canvas_status_t canvas_set_dash_offset(canvas_context_t *,double);
canvas_status_t canvas_set_composite(canvas_context_t *,draw2d_operator_t);
/* Pixel transfer ignores drawing state. Read fills out-of-bitmap samples
 * with transparent black; put replaces the intersection, optionally limited
 * by a source-coordinate dirty rectangle. The binding normalizes WebIDL
 * arguments before calling. Buffers may not overlap the retained bitmap;
 * descriptors/output must not alias their byte storage. No allocations or
 * partial writes on refusal. Zero extents are native no-ops (the binding
 * implements the web API's zero-size errors). */
canvas_status_t canvas_read_pixels(canvas_context_t *, int64_t x, int64_t y,
                                    const canvas_rgba8_t *out);
canvas_status_t canvas_put_pixels(canvas_context_t *, const canvas_rgba8_t *source,
                                   int64_t x, int64_t y, const draw2d_damage_t *dirty);
canvas_status_t canvas_state(const canvas_context_t *, canvas_state_t *);
/* Views are read-only and expire at the next successful reset/destroy;
 * drawing changes their contents synchronously. No ownership is transferred. */
canvas_status_t canvas_bitmap(const canvas_context_t *, canvas_bitmap_t *);
canvas_status_t canvas_stats(const canvas_context_t *, canvas_stats_t *);
/* Bounded union since reset or last acknowledgement, in bitmap coordinates.
 * Acknowledge after successful synchronous consumption with no intervening
 * draw/reset; a failed presentation must leave damage pending. */
canvas_status_t canvas_damage(const canvas_context_t *, draw2d_damage_t *);
canvas_status_t canvas_ack_damage(canvas_context_t *);
#pragma GCC visibility pop
#endif
