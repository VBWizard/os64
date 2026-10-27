#ifndef OS64_SVG_H
#define OS64_SVG_H
#include <os64/geometry_backend.h>

/* Native SVG foundation. The scene provider is a source interface, not a
 * stable browser ABI. No DOM, CSS-engine or FreeType private types cross it. */
typedef struct svg_context svg_context_t;
typedef struct svg_path svg_path_t;
typedef struct svg_geometry svg_geometry_t;
typedef struct svg_scene svg_scene_t;
typedef enum {
    SVG_OK,
    SVG_MALFORMED,
    SVG_UNSUPPORTED,
    SVG_LIMIT,
    SVG_NO_MEMORY,
    SVG_CANCELLED,
    SVG_PENDING,
    SVG_BAD_ARGUMENT,
    SVG_BUSY,
    SVG_PROVIDER_ERROR,
    SVG_ENGINE_ERROR
} svg_status_t;
typedef struct {
    const char *data;
    size_t size;
} svg_string_t;
typedef struct {
    size_t input_bytes, path_commands, geometry_commands, scene_nodes, depth;
    uint64_t work;
} svg_limits_t;
typedef struct {
    /* Same admission callbacks as geometry: the owner charges SVG, geometry,
     * document and destination allocations once in its shared ledger. */
    os64_geometry_memory_t memory;
    void *cancel_context;
    bool (*cancelled)(void *);
    svg_limits_t limits; /* Literal values, including zero. Use defaults explicitly. */
} svg_options_t;
typedef struct {
    size_t live_bytes, peak_bytes, paths, geometries, scenes;
    uint64_t last_work;
} svg_stats_t;
typedef enum { SVG_MOVE, SVG_LINE, SVG_QUADRATIC, SVG_CUBIC, SVG_CLOSE, SVG_ARC } svg_verb_t;
typedef struct {
    svg_verb_t verb;
    os64_geometry_point_t p[3]; /* Same point order as the geometry backend. */
    /* ARC uses p[0] as endpoint; radii are nonnegative, rotation in degrees.
     * Endpoint arcs remain exact parameters until transform-specific lowering. */
    double rx, ry, rotation;
    bool large, sweep;
} svg_command_t;
typedef struct {
    svg_status_t status;
    size_t offset; /* Scanner position at failure, or input size on success. */
} svg_parse_result_t;

typedef enum { SVG_MODE_INLINE, SVG_MODE_IMAGE } svg_mode_t;
typedef enum {
    SVG_NODE_GROUP,
    SVG_NODE_PATH,
    SVG_NODE_UNSUPPORTED,
    SVG_NODE_SHAPE,
    SVG_NODE_USE
} svg_node_kind_t;
typedef enum {
    SVG_SHAPE_RECT,
    SVG_SHAPE_ELLIPSE,
    SVG_SHAPE_LINE,
    SVG_SHAPE_POLYLINE,
    SVG_SHAPE_POLYGON
} svg_shape_kind_t;
typedef struct {
    svg_shape_kind_t kind;
    /* Resolved user units: x/y is rect origin, ellipse center or line start.
     * Width/height and radii are nonnegative. Auto radii are resolved by the
     * provider; rectangle radii are clamped during path construction.
     * Initialize unused fields to zero. */
    double x, y, width, height, rx, ry, x2, y2;
    svg_string_t points; /* Borrowed coordinate pairs for polyline/polygon. */
} svg_shape_t;
typedef enum { SVG_PAINT_NONE, SVG_PAINT_COLOR, SVG_PAINT_UNSUPPORTED } svg_paint_kind_t;
typedef struct {
    svg_paint_kind_t kind;
    uint32_t argb; /* Straight-alpha color. */
} svg_paint_t;
#define SVG_DASH_MAX 256u
typedef struct {
    const double *values; /* Nonnegative user-unit lengths; borrowed by a provider. */
    size_t count;         /* Zero is solid; odd lists repeat to even length. */
    double offset;        /* Signed user units, reduced modulo the pattern. */
} svg_dash_t;
typedef struct {
    svg_paint_t fill, stroke;
    double fill_opacity, stroke_opacity, opacity;
    os64_geometry_stroke_t stroke_style;
    svg_dash_t dash;
    bool even_odd, display, visible;
    /* Resolved winning CSS/attribute transform, including its resolved origin
     * and node placement such as use x/y. The provider must not multiply a
     * losing transform attribute into it. */
    os64_geometry_map_t transform;
} svg_style_t;
typedef struct {
    svg_node_kind_t kind;
    uint64_t first_child, next_sibling; /* Zero means absent; identity is snapshot-local. */
} svg_node_t;
typedef struct {
    uint64_t source, instance, parent_instance;
    const svg_style_t *parent_style; /* Borrowed for the callback; NULL at root. */
    svg_mode_t mode;
} svg_style_request_t;
typedef struct {
    void *context;
    /* Success supplies a nonzero root and a snapshot token (which may be NULL).
     * acquire pins lifetime and establishes a stable tree/style view. The host
     * serializes mutation for preparation; a pin alone does not freeze data.
     * Failure owns no token. release runs once for each successful acquire. */
    svg_status_t (*acquire)(void *, svg_mode_t, void **snapshot, uint64_t *root);
    void (*release)(void *, void *snapshot);
    svg_status_t (*node)(void *, void *snapshot, uint64_t source, svg_node_t *);
    /* Resolve cascade using source AND instance ancestry. Supply d after the
     * cascade; returned bytes survive until this callback is called again.
     * The inherited input is the parent's complete style, not a directive to
     * inherit non-inherited properties such as opacity or transform. Return
     * UNSUPPORTED for active features this profile cannot express: vector
     * effects, clipping, paint order, resources, non-sRGB interpolation or
     * non-normal blending. Dash values survive until the next provider
     * callback; preparation copies them into scene ownership. Providers must
     * honor mode; IMAGE callbacks must not fetch. */
    svg_status_t (*style)(void *, void *snapshot, const svg_style_request_t *, svg_style_t *,
                          svg_string_t *d);
    /* Required for SHAPE nodes. Called after a displayed element's style;
     * returned point bytes survive until the next provider callback. The
     * library copies geometry into its owned path without text serialization. */
    svg_status_t (*shape)(void *, void *snapshot, const svg_style_request_t *, svg_shape_t *);
    /* Required for displayed USE nodes, whose first_child must be zero.
     * Return one source (zero for an empty reference); its next_sibling is
     * excluded from the instance. Active-ancestry reference cycles are empty.
     * Source identity remains shared; style receives fresh instance ancestry.
     * Resolve inside the pinned snapshot and honor the processing mode. */
    svg_status_t (*reference)(void *, void *snapshot, const svg_style_request_t *, uint64_t *source);
} svg_provider_t;
typedef struct {
    uint64_t source, instance;
    size_t parent; /* SIZE_MAX for root; parent index precedes child. */
    svg_node_kind_t kind;
    svg_style_t style;
    os64_geometry_map_t user_to_viewport;
    const svg_path_t *path; /* Borrowed from scene, never release separately. */
    svg_parse_result_t path_result;
} svg_scene_item_t;

typedef struct {
    double origin_x, origin_y; /* Device coordinate of output pixel (0,0). */
    uint32_t width, height;
    size_t stride, capacity; /* Bytes; stride is a multiple of sizeof(uint32_t). */
    uint32_t *pixels;        /* Caller-owned, aligned straight-alpha 0xAARRGGBB words. */
} svg_render_target_t;
typedef struct {
    const os64_geometry_backend_t *geometry; /* Requires the optional bounds tail. */
    os64_geometry_map_t viewport_to_device;
    double arc_error, quantization_error;
    size_t max_pixels;
    uint32_t max_dimension;
    uint64_t max_work, max_raster_work, max_geometry_work;
} svg_render_options_t;
typedef struct {
    uint64_t raster_work, geometry_work;
    size_t painted_paths, peak_layers;
} svg_render_stats_t;

#pragma GCC visibility push(default)
svg_limits_t svg_default_limits(void);
svg_style_t svg_initial_style(void);
svg_status_t svg_create(const svg_options_t *, svg_context_t **);
svg_status_t svg_destroy(svg_context_t *); /* BUSY while products live. */
svg_status_t svg_stats(svg_context_t *, svg_stats_t *);
/* Calls require owner serialization. Callbacks must not reenter this context.
 * Products belong to their creating context. Failed output handles are NULL,
 * except MALFORMED parsing: it returns an owned, renderable complete prefix.
 * LIMIT/NOMEM/CANCELLED never publish a prefix. Release does no allocation or
 * cancellation polling. Input strings need not outlive the call. */
svg_status_t svg_path_parse(svg_context_t *, svg_string_t, svg_path_t **, svg_parse_result_t *);
/* Construct the equivalent path of resolved shape geometry. Malformed point
 * lists retain complete coordinate pairs with the same prefix ownership as d. */
svg_status_t svg_path_from_shape(svg_context_t *, const svg_shape_t *, svg_path_t **,
                                 svg_parse_result_t *);
const svg_command_t *svg_path_commands(const svg_path_t *, size_t *count);
svg_status_t svg_path_release(svg_context_t *, svg_path_t *);
/* Lower arcs to cubic Hermite pieces using a per-axis device-space positional
 * error budget. This is separate from backend quantization, stroke-boundary
 * error and raster coverage. Lowering retains user-space coordinates and adds
 * MOVE after CLOSE when necessary for the backend's command contract. */
svg_status_t svg_path_lower(svg_context_t *, const svg_path_t *, const os64_geometry_map_t *,
                            double arc_error, svg_geometry_t **);
const os64_geometry_command_t *svg_geometry_commands(const svg_geometry_t *, size_t *count);
svg_status_t svg_geometry_release(svg_context_t *, svg_geometry_t *);
/* Prepares groups, paths and basic shapes. The caller supplies the resolved root viewport
 * map; viewport/layout negotiation, use expansion and resources are separate
 * features. Painting uses svg_scene_render. Unsupported visible nodes fail explicitly. display:none
 * suppresses a subtree; visibility:hidden retains it for visible descendants.
 * No resolver or I/O callback exists. IMAGE does not authorize resource loads.
 * A successful scene retains the snapshot for source identity lifetime. */
svg_status_t svg_scene_prepare(svg_context_t *, const svg_provider_t *, svg_mode_t,
                               const os64_geometry_map_t *viewport, svg_scene_t **);
const svg_scene_item_t *svg_scene_items(const svg_scene_t *, size_t *count);
svg_status_t svg_scene_release(svg_context_t *, svg_scene_t *);
svg_render_options_t svg_render_options_default(const os64_geometry_backend_t *);
/* Paints solid fills followed by ordinary user-space strokes, using source-over
 * in sRGB and isolated element/group opacity. Replaces the target, including
 * transparent pixels; preserves row padding. RGB is zero when exported alpha
 * is zero. The scene stays reusable and the renderer never frees the target.
 * On failure discard the target; a caller must not publish partial pixels.
 * Options are literal, including zero budgets. Rendering uses max_work instead
 * of the context's preparation work limit, plus separate raster/backend limits.
 * Geometry allocations use the same owner callbacks as SVG, directly (SVG
 * allocation counters exclude that backend subdivision). A conforming injected
 * backend table and its code must remain alive for this call. No callbacks into
 * the document/style provider occur during painting.
 * Vector effects, clipping, non-default paint order, resources and
 * non-sRGB interpolation/non-normal blending require the provider to report
 * UNSUPPORTED. Unsupported joins and nonzero strokes that collapse below
 * the backend working grid propagate explicitly. Exact zero-length caps are
 * constructed by libsvg. stats is optional, including on failure. */
svg_status_t svg_scene_render(svg_context_t *, const svg_scene_t *, const svg_render_options_t *,
                              const svg_render_target_t *, svg_render_stats_t *stats);
#pragma GCC visibility pop
#endif
