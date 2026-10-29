#ifndef OS64_CANVAS_H
#define OS64_CANVAS_H
#include <draw2d/paint.h>
#include <os64/geometry_backend.h>

/* Native bitmap/context, with no DOM or script objects. The binding owns this
 * handle and keeps it alive for retained element/context wrappers. Calls are
 * serialized by that owner; callbacks must not reenter the context. */
typedef struct canvas_context canvas_context_t;
typedef enum {
    CANVAS_OK, CANVAS_BAD_ARGUMENT, CANVAS_LIMIT, CANVAS_NO_MEMORY, CANVAS_BUSY
} canvas_status_t;
typedef struct {
    os64_geometry_memory_t memory; /* Shared owner ledger, including temporary resize peaks. */
    uint32_t max_dimension;
    uint64_t max_pixels, max_draw_pixels;
    size_t max_bytes, max_saved_states; /* Literal limits, including zero. */
} canvas_options_t;
typedef struct {
    uint32_t fill_argb; /* Straight-alpha sRGB. */
    double global_alpha;
} canvas_state_t;
typedef struct {
    const uint32_t *pixels; /* Borrowed premultiplied 0xAARRGGBB, sRGB. */
    uint32_t width, height;
    size_t stride, capacity;
} canvas_bitmap_t;
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
/* Nonfinite/out-of-range values are ignored, matching Canvas assignment. */
canvas_status_t canvas_set_alpha(canvas_context_t *, double);
/* Axis-aligned bitmap-space rectangles; transform/path support is tracked in
 * CANVAS.md C3. clear ignores the fill color and global alpha. */
canvas_status_t canvas_fill_rect(canvas_context_t *, double x, double y, double w, double h);
canvas_status_t canvas_clear_rect(canvas_context_t *, double x, double y, double w, double h);
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
