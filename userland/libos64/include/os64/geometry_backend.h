#ifndef OS64_GEOMETRY_BACKEND_H
#define OS64_GEOMETRY_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent of the font table. No FreeType structs cross this interface. */
#define OS64_GEOMETRY_BACKEND_REVISION 1u
#define OS64_GEOMETRY_COMMAND_MAX 4096u
#define OS64_GEOMETRY_POINT_MAX 65535u
#define OS64_GEOMETRY_WORK_DEFAULT (4u * 1024u * 1024u)

typedef enum {
    OS64_GEOMETRY_OK,
    OS64_GEOMETRY_BAD_ARGUMENT,
    OS64_GEOMETRY_UNSUPPORTED,
    OS64_GEOMETRY_LIMIT,
    OS64_GEOMETRY_NO_MEMORY,
    OS64_GEOMETRY_CANCELLED,
    OS64_GEOMETRY_BUSY,
    OS64_GEOMETRY_ENGINE_ERROR
} os64_geometry_status_t;
typedef struct os64_geometry_context os64_geometry_context_t;
typedef struct os64_geometry_outline os64_geometry_outline_t;
typedef struct {
    double x, y;
} os64_geometry_point_t;
typedef enum {
    OS64_GEOMETRY_MOVE,
    OS64_GEOMETRY_LINE,
    OS64_GEOMETRY_QUADRATIC,
    OS64_GEOMETRY_CUBIC,
    OS64_GEOMETRY_CLOSE
} os64_geometry_verb_t;
/* MOVE/LINE use p[0]. QUADRATIC uses control/end; CUBIC control/control/end.
 * CLOSE uses no points. A segment after CLOSE needs an explicit MOVE.
 * Coordinates are finite doubles with x rightwards and y downwards. */
typedef struct {
    os64_geometry_verb_t verb;
    os64_geometry_point_t p[3];
} os64_geometry_command_t;
typedef struct {
    double a, b, c, d, tx, ty;
} os64_geometry_map_t;
typedef enum {
    OS64_GEOMETRY_BUTT,
    OS64_GEOMETRY_ROUND_CAP,
    OS64_GEOMETRY_SQUARE
} os64_geometry_cap_t;
typedef enum {
    OS64_GEOMETRY_MITER,
    OS64_GEOMETRY_MITER_CLIP,
    OS64_GEOMETRY_ROUND_JOIN,
    OS64_GEOMETRY_BEVEL,
    OS64_GEOMETRY_ARCS
} os64_geometry_join_t;
typedef struct {
    double width, miter_limit;
    os64_geometry_cap_t cap;
    os64_geometry_join_t join;
} os64_geometry_stroke_t;
/* Budget refusal and physical exhaustion remain distinct. Allocation includes
 * adapter/FreeType headers. Success supplies max_align_t-aligned storage;
 * failure supplies NULL and LIMIT or NO_MEMORY. Free receives that same size.
 * Callbacks outlive the context and must not reenter it. */
typedef struct {
    void *context;
    os64_geometry_status_t (*alloc)(void *, size_t bytes, void **out);
    void (*free)(void *, void *, size_t bytes);
} os64_geometry_memory_t;
typedef struct {
    os64_geometry_memory_t memory;
    uint64_t max_work; /* Literal per-operation adapter work limit, including zero. */
    void *cancel_context;
    bool (*cancelled)(void *);
} os64_geometry_options_t;
typedef struct {
    size_t live_bytes, peak_bytes;
    uint32_t live_outlines;
    uint64_t last_work;
} os64_geometry_stats_t;
typedef struct {
    uint32_t points, contours;
} os64_geometry_outline_info_t;
typedef struct {
    bool empty;
    double x_min, y_min, x_max, y_max;
} os64_geometry_bounds_t;
typedef struct {
    double origin_x, origin_y; /* Device coordinate of coverage pixel (0,0). */
    uint32_t width, height;
    size_t stride, capacity;
    uint8_t *pixels;
    bool even_odd;
} os64_geometry_target_t;

/* Calls on a context and its outlines require caller serialization. An outline
 * belongs to its creating context; destroy returns BUSY while outlines live.
 * Inputs are borrowed for the call; successful path/stroke creation returns an
 * independent outline. Failed output handles are NULL. Release does not poll
 * cancellation or allocate. Stats describe allocation subdivisions, not a
 * second admission budget. Work counts adapter visits, allocation bytes and
 * output bytes/spans; it is not a count of upstream CPU instructions.
 * Cancellation is cooperative at these checkpoints. A FreeType call already
 * executing can finish before cancellation returns. */
typedef struct {
    uint32_t revision, struct_size;
    os64_geometry_status_t (*create)(const os64_geometry_options_t *, os64_geometry_context_t **);
    os64_geometry_status_t (*destroy)(os64_geometry_context_t *);
    os64_geometry_status_t (*stats)(os64_geometry_context_t *, os64_geometry_stats_t *);
    /* planned_map controls normalization precision; raster rechecks the actual
     * map. quantization_error is the allowed device-pixel conversion error
     * per axis (default caller choice 1/64), in (1/128,1]. This excludes the
     * stroker's curve approximation and raster coverage approximation. Stroke
     * radius rounds to the working grid; miter ratio rounds to 16.16. The
     * coordinate bound does not promise final stroke-boundary accuracy. */
    os64_geometry_status_t (*path)(os64_geometry_context_t *, const os64_geometry_command_t *,
                                   size_t count, const os64_geometry_map_t *planned_map,
                                   double quantization_error, os64_geometry_outline_t **);
    os64_geometry_status_t (*stroke)(os64_geometry_context_t *, const os64_geometry_command_t *,
                                     size_t count, const os64_geometry_map_t *planned_map,
                                     double quantization_error, const os64_geometry_stroke_t *,
                                     os64_geometry_outline_t **);
    os64_geometry_status_t (*outline_info)(os64_geometry_context_t *,
                                           const os64_geometry_outline_t *,
                                           os64_geometry_outline_info_t *);
    os64_geometry_status_t (*release)(os64_geometry_context_t *, os64_geometry_outline_t *);
    /* Raster preserves the complete shape while clipping coverage. On any
     * non-OK result discard the destination; cancellation/backend errors may
     * leave partial coverage. Row padding is not written. No inversion is
     * required, including for singular maps. */
    os64_geometry_status_t (*raster)(os64_geometry_context_t *, const os64_geometry_outline_t *,
                                     const os64_geometry_map_t *, const os64_geometry_target_t *);
    /* Optional v1 table tail: check struct_size before reading this member.
     * Bounds enclose the transformed, quantized outline's control points,
     * relative to the supplied device origin. These are conservative raster
     * allocation bounds, not SVG geometric/object bounds. No allocation.
     * Uses raster's precision/range checks and charges point visits. */
    os64_geometry_status_t (*bounds)(os64_geometry_context_t *, const os64_geometry_outline_t *,
                                     const os64_geometry_map_t *, double origin_x, double origin_y,
                                     os64_geometry_bounds_t *);
    /* Optional v1 tail: stroke the centerline and add caller-supplied filled
     * cap contours to the same nonzero-winding outline. Caps are clockwise in
     * x-right/y-down user coordinates, without holes. They share normalization,
     * allocation and rasterization with the stroke, so overlapping dots paint
     * once. Either command stream may be empty, but not both. */
    os64_geometry_status_t (*stroke_with_caps)(
        os64_geometry_context_t *, const os64_geometry_command_t *, size_t count,
        const os64_geometry_command_t *caps, size_t cap_count,
        const os64_geometry_map_t *planned_map, double quantization_error,
        const os64_geometry_stroke_t *, os64_geometry_outline_t **);
} os64_geometry_backend_t;

/* Width zero produces an empty outline. ARCS joins and non-butt degenerate
 * subpaths containing segments return UNSUPPORTED, including curves discarded
 * by the stroker's working-grid epsilon. SVG must supply fallback join/cap
 * geometry explicitly. Move-only contours paint nothing.
 * Miter limits below one are invalid. The backend never clamps coordinates or
 * divides a filled outline to evade a point limit. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const os64_geometry_backend_t *
os64_freetype_geometry_v1(void);
#endif
