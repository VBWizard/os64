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
};
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
Status svg_lower(Context *, const svg_path_t *, const Map *, double, svg_geometry_t **);
void svg_free_geometry(Context *, svg_geometry_t *);
#endif
