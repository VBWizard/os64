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
#define SVG_GRADIENT_STOPS_MAX 256u
typedef enum { SVG_SPREAD_PAD, SVG_SPREAD_REPEAT, SVG_SPREAD_REFLECT } svg_spread_t;
typedef struct {
    double offset; /* Normalized, nondecreasing [0,1]; duplicate offsets retain order. */
    uint32_t argb; /* Straight-alpha sRGB. */
} svg_gradient_stop_t;
typedef struct {
    double x1, y1, x2, y2;
    os64_geometry_map_t transform;
    bool object_bbox;
    svg_spread_t spread;
    const svg_gradient_stop_t *stops;
    size_t count;
} svg_linear_gradient_t;
/* Prepared numeric sampler. Stop storage is borrowed and must remain immutable
 * until sampling ends. Initialize with svg_linear_gradient_prepare; sample
 * positions use the coordinate space selected by that call's map. */
typedef struct {
    const svg_gradient_stop_t *stops;
    size_t count;
    double x, y, bias;
    svg_spread_t spread;
    bool constant;
} svg_linear_sampler_t;
typedef enum { SVG_PAINT_NONE, SVG_PAINT_COLOR, SVG_PAINT_UNSUPPORTED,
               SVG_PAINT_LINEAR } svg_paint_kind_t;
typedef struct {
    svg_paint_kind_t kind;
    uint32_t argb; /* Straight-alpha color. */
    const svg_linear_gradient_t *linear; /* Borrowed by providers; scene owns a copy. */
} svg_paint_t;
#define SVG_DASH_MAX 256u
typedef struct {
    const double *values; /* Nonnegative user-unit lengths; borrowed by a provider. */
    size_t count;         /* Zero is solid; odd lists repeat to even length. */
    double offset;        /* Signed user units, reduced modulo the pattern. */
} svg_dash_t;
typedef struct {
    bool active, clip;
    double width, height;                  /* Frame at (0,0), before the content transform. */
    os64_geometry_map_t content_transform; /* Applies to descendants, not the frame. */
} svg_viewport_t;
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
    svg_viewport_t viewport; /* Optional on containers; zero dimensions suppress the subtree. */
} svg_style_t;
typedef struct {
    svg_node_kind_t kind;
    uint64_t first_child, next_sibling; /* Zero means absent; identity is snapshot-local. */
} svg_node_t;
typedef struct {
    bool empty; /* No geometry; distinct from a point or a zero-width/height path. */
    double min_x, min_y, max_x, max_y;
} svg_bounds_t;

/* One silhouette in user space or normalized object-box coordinates. The provider
 * retains borrowed strings until the next callback. An active empty path defines
 * an empty clipping region; object-box effects on zero-width/height consumers
 * are ignored. Inactive means no clip. Resource lookup stays with the provider. */
typedef struct {
    bool active, even_odd;
    os64_geometry_map_t transform;
    svg_node_kind_t kind; /* PATH or SHAPE when active. */
    svg_string_t d;
    svg_shape_t shape;
    bool object_bbox;
    /* When object_bbox is true: outer_transform * box-map * transform.
     * The outer map acts in consumer user units; supply identity if absent.
     * Otherwise transform acts directly in consumer user units. */
    os64_geometry_map_t outer_transform;
} svg_clip_source_t;
typedef struct {
    bool active, even_odd;
    os64_geometry_map_t transform;
    const svg_path_t *path; /* Scene owned. */
    bool object_bbox;
    os64_geometry_map_t outer_transform;
} svg_clip_t;
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
     * effects, unsupported clip combinations, paint order, unsupported paint servers, non-sRGB interpolation or
     * non-normal blending. Dash values survive until the next provider
     * callback; gradient descriptors/stops share that lifetime. Preparation copies
     * them into scene ownership. Providers must
     * honor mode; IMAGE callbacks must not fetch. */
    svg_status_t (*style)(void *, void *snapshot, const svg_style_request_t *, svg_style_t *,
                          svg_string_t *d);
    /* Required for SHAPE nodes. Called after a displayed element's style;
     * returned point bytes survive until the next provider callback. The
     * library copies geometry into its owned path without text serialization. */
    svg_status_t (*shape)(void *, void *snapshot, const svg_style_request_t *, svg_shape_t *);
    /* Required for displayed USE nodes, whose first_child must be zero.
     * Return one source (zero for an empty reference); its next_sibling is
     * excluded from the instance. A loop containing a reference edge suppresses
     * the repeated occurrence, retaining ordinary siblings; child-only loops
     * remain provider errors. Source identity stays shared; style receives
     * fresh instance ancestry.
     * Resolve inside the pinned snapshot and honor the processing mode. */
    svg_status_t (*reference)(void *, void *snapshot, const svg_style_request_t *,
                              uint64_t *source);
    /* Optional. Called after this element's ordinary geometry has been copied.
     * Resolve a single silhouette, including its clip-rule and transforms.
     * Resource styling uses resource ancestry, not the referencing element's
     * inherited paint. Unsupported unions/units must be reported explicitly. */
    svg_status_t (*clip)(void *, void *snapshot, const svg_style_request_t *, svg_clip_source_t *);
} svg_provider_t;
typedef struct {
    uint64_t source, instance;
    size_t parent; /* SIZE_MAX for root; parent index precedes child. */
    svg_node_kind_t kind;
    svg_style_t style;
    os64_geometry_map_t user_to_viewport;
    const svg_path_t *path; /* Borrowed from scene, never release separately. */
    svg_parse_result_t path_result;
    svg_clip_t clip;
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
/* No markup, cascade or allocator is involved. user_to_sample maps consumer user
 * space to the sampling space. Object-box gradients require geometric bounds;
 * a singular map or zero object extent produces a transparent sampler. */
svg_status_t svg_linear_gradient_prepare(const svg_linear_gradient_t *,
    const os64_geometry_map_t *user_to_sample, const svg_bounds_t *, svg_linear_sampler_t *);
uint32_t svg_linear_gradient_sample(const svg_linear_sampler_t *, double x, double y);

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
/* Analytic geometric extrema in the supplied coordinate space, evaluated in
 * binary64. Excludes stroke and raster expansion; moves and zero-length
 * segments contribute points. Empty input yields empty=true. No allocation.
 * Failure leaves out empty. Non-finite arithmetic returns LIMIT, not clamped coordinates. */
svg_status_t svg_path_bounds(svg_context_t *, const svg_path_t *, const os64_geometry_map_t *,
                             svg_bounds_t *out);
/* Lower arcs to cubic Hermite pieces using a per-axis device-space positional
 * error budget. This is separate from backend quantization, stroke-boundary
 * error and raster coverage. Lowering retains user-space coordinates and adds
 * MOVE after CLOSE when necessary for the backend's command contract. */
svg_status_t svg_path_lower(svg_context_t *, const svg_path_t *, const os64_geometry_map_t *,
                            double arc_error, svg_geometry_t **);
const os64_geometry_command_t *svg_geometry_commands(const svg_geometry_t *, size_t *count);
svg_status_t svg_geometry_release(svg_context_t *, svg_geometry_t *);
/* Prepares groups, paths, basic shapes and bounded same-document use instances.
 * The provider supplies resolved viewport frames/content maps and reference targets;
 * CSS layout negotiation and resource loading stay outside this API. Painting
 * uses svg_scene_render. Unsupported visible nodes fail explicitly. display:none
 * suppresses a subtree; visibility:hidden retains it for visible descendants.
 * No resolver or I/O callback exists. IMAGE does not authorize resource loads.
 * A successful scene retains the snapshot for source identity lifetime. */
svg_status_t svg_scene_prepare(svg_context_t *, const svg_provider_t *, svg_mode_t,
                               const os64_geometry_map_t *viewport, svg_scene_t **);
const svg_scene_item_t *svg_scene_items(const svg_scene_t *, size_t *count);
svg_status_t svg_scene_release(svg_context_t *, svg_scene_t *);
/* Bounds of retained paths in a subtree, in that item's content user space.
 * Includes descendant transforms/viewports; excludes the item's placement and
 * own viewBox map. Paint, visibility, opacity and clips do not change bounds.
 * display:none and zero-sized viewports were omitted at preparation. Empty
 * paths/containers add nothing. This is not the DOM getBBox interface. */
svg_status_t svg_scene_object_bounds(svg_context_t *, const svg_scene_t *, size_t item,
                                     svg_bounds_t *out);
svg_render_options_t svg_render_options_default(const os64_geometry_backend_t *);
/* Paints solid/linear-gradient fills followed by ordinary user-space strokes, using source-over
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
 * Viewport and silhouette masks clip isolated element/container layers. Vector effects,
 * unsupported clip combinations, non-default paint order, unsupported paint servers and
 * non-sRGB interpolation/non-normal blending require the provider to report
 * UNSUPPORTED. Unsupported joins and nonzero strokes that collapse below
 * the backend working grid propagate explicitly. Exact zero-length caps are
 * constructed by libsvg. stats is optional, including on failure. */
svg_status_t svg_scene_render(svg_context_t *, const svg_scene_t *, const svg_render_options_t *,
                              const svg_render_target_t *, svg_render_stats_t *stats);
#pragma GCC visibility pop
#endif
