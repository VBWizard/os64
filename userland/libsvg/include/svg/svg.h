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
    SVG_PROVIDER_ERROR
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
typedef enum { SVG_NODE_GROUP, SVG_NODE_PATH, SVG_NODE_UNSUPPORTED } svg_node_kind_t;
typedef enum { SVG_PAINT_NONE, SVG_PAINT_COLOR, SVG_PAINT_UNSUPPORTED } svg_paint_kind_t;
typedef struct {
    svg_paint_kind_t kind;
    uint32_t argb; /* Straight-alpha color. */
} svg_paint_t;
typedef struct {
    svg_paint_t fill, stroke;
    double fill_opacity, stroke_opacity, opacity;
    os64_geometry_stroke_t stroke_style;
    bool even_odd, display, visible;
    /* Resolved winning CSS/attribute transform, including its resolved origin.
     * The provider must not multiply a losing transform attribute into it. */
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
     * UNSUPPORTED for active features this profile cannot express (dashes,
     * vector effects, clipping, paint order, resources), rather than dropping
     * them. Providers must honor mode; IMAGE callbacks must not fetch. */
    svg_status_t (*style)(void *, void *snapshot, const svg_style_request_t *, svg_style_t *,
                          svg_string_t *d);
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
/* Prepares groups and paths. The caller supplies the resolved root viewport
 * map; viewport/layout negotiation, use expansion, resources and painting are
 * separate features. Unsupported visible nodes fail explicitly. display:none
 * suppresses a subtree; visibility:hidden retains it for visible descendants.
 * No resolver or I/O callback exists. IMAGE does not authorize resource loads.
 * A successful scene retains the snapshot for source identity lifetime. */
svg_status_t svg_scene_prepare(svg_context_t *, const svg_provider_t *, svg_mode_t,
                               const os64_geometry_map_t *viewport, svg_scene_t **);
const svg_scene_item_t *svg_scene_items(const svg_scene_t *, size_t *count);
svg_status_t svg_scene_release(svg_context_t *, svg_scene_t *);
#pragma GCC visibility pop
#endif
