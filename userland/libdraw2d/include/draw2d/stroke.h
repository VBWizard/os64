#ifndef OS64_DRAW2D_STROKE_H
#define OS64_DRAW2D_STROKE_H
#include <os64/geometry_backend.h>
typedef struct { const double *values; size_t count; double offset; } draw2d_dash_t;
typedef struct { draw2d_dash_t dash; } draw2d_stroke_style_t;
/* Geometry is in stroke space. The map bounds device-space approximation
 * error; it does not transform the commands. Output keeps curves and joins
 * across a closed dash seam. The caller supplies cap contours for zero runs using the supplied tangent.
 * Callbacks charge the owner's memory/work ledger and preserve refusal reasons.
 * grow preserves the old allocation on failure. free receives grow's storage.
 * Output callbacks may have emitted a prefix on failure: discard the job.
 * Inputs/ops remain borrowed and immutable throughout the call. */
typedef struct {
    void *opaque;
    size_t command_limit;
    os64_geometry_status_t (*spend)(void *, uint64_t);
    os64_geometry_status_t (*grow)(void *, void **, size_t *, size_t count, size_t size, size_t limit);
    void (*free)(void *, void *);
    os64_geometry_status_t (*emit)(void *, bool caps, const os64_geometry_command_t *);
    os64_geometry_status_t (*cap)(void *, os64_geometry_point_t, os64_geometry_point_t tangent);
} draw2d_stroke_ops_t;
#pragma GCC visibility push(default)
os64_geometry_status_t draw2d_stroke_cut(const draw2d_stroke_ops_t *, const os64_geometry_command_t *,
    size_t, const os64_geometry_map_t *, double error, const draw2d_stroke_style_t *);
#pragma GCC visibility pop
#endif
