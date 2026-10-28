#ifndef SVG_INTERNAL_H
#define SVG_INTERNAL_H
#include <float.h>
#include <math.h>
#include <svg/svg.h>

typedef svg_context_t Context;
typedef svg_status_t Status;
typedef os64_geometry_point_t Point;
typedef os64_geometry_map_t Map;
struct svg_context {
    svg_options_t options;
    svg_stats_t stats;
    Status why;
    bool busy;
    uint64_t work_limit;
};
struct svg_path {
    Context *owner;
    svg_command_t *commands;
    size_t count, capacity;
    bool in_scene;
};
struct svg_geometry {
    Context *owner;
    os64_geometry_command_t *commands;
    size_t count, capacity;
};
struct svg_scene {
    Context *owner;
    svg_provider_t provider;
    void *snapshot;
    svg_scene_item_t *items;
    size_t count, capacity;
    bool pinned;
    svg_mode_t mode;
};
struct svg_raster_key {
    Context *owner;
    const os64_geometry_backend_t *backend;
    size_t size;
    uint64_t digest;
    double origin_x, origin_y;
    uint32_t width, height;
    unsigned char data[];
};
size_t svg_allocation_cost(size_t);
Status svg_render_validate(const svg_render_options_t *, const svg_render_target_t *);
bool svg_spend(Context *, uint64_t);
Status svg_enter(Context *);
Status svg_leave(Context *, Status);
void *svg_alloc(Context *, size_t);
void svg_free(Context *, void *);
/* Returns the original allocation on failure, with context->why set. On
 * success it transfers ownership to the returned allocation and updates capacity. */
void *svg_grow(Context *, void *, size_t *, size_t count, size_t size, size_t limit);
bool svg_map_valid(const Map *);
bool svg_point_valid(Point);
Status svg_parse(Context *, svg_string_t, svg_path_t **, svg_parse_result_t *);
Status svg_make_shape(Context *, const svg_shape_t *, svg_path_t **, svg_parse_result_t *);
void svg_free_path(Context *, svg_path_t *);
typedef struct {
    Point center, u, v;
    double angle, sweep, numerical_error;
} SvgArc;
bool svg_arc_center(Context *, Point, const svg_command_t *, SvgArc *);
Status svg_geometric_bounds(Context *, const svg_path_t *, const Map *, svg_bounds_t *);
Status svg_object_bounds(Context *, const svg_scene_t *, size_t, svg_bounds_t *);
Status svg_lower(Context *, const svg_path_t *, const Map *, double, svg_geometry_t **);
void svg_free_geometry(Context *, svg_geometry_t *);
Status svg_stroke_geometry(Context *, const svg_geometry_t *, const Map *, double,
                           const svg_style_t *, svg_geometry_t **centerline, svg_geometry_t **caps);
#endif
