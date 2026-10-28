#include "internal.h"

typedef struct {
    uint32_t x0, y0, x1, y1;
} Rect;
typedef struct {
    uint16_t a, r, g, b;
} Pixel;
typedef struct {
    Rect box;
    Pixel *pixels;
} Layer;
typedef struct {
    bool active, paint[2];
    svg_linear_sampler_t gradient[2];
    svg_radial_sampler_t radial[2];
    Map map;
    Rect box, ink[2];
    os64_geometry_outline_t *outline[2], *clip[2];
    Map clip_map[2];
    Rect clip_box[2];
    bool clips[2], clip_even_odd[2];
} Item;
typedef struct {
    size_t index;
    Layer layer, *surface;
    uint16_t opacity;
    bool owns;
} Frame;
typedef struct {
    Context *c;
    const svg_render_options_t *options;
    const svg_render_target_t *target;
    const os64_geometry_backend_t *backend;
    os64_geometry_context_t *engine;
    svg_render_stats_t stats;
    uint64_t quota;
    size_t layers;
} Render;

svg_render_options_t svg_render_options_default(const os64_geometry_backend_t *backend)
{
    return (svg_render_options_t){.geometry = backend,
                                  .viewport_to_device = {1, 0, 0, 1, 0, 0},
                                  .arc_error = 1.0 / 64,
                                  .quantization_error = 1.0 / 64,
                                  .max_pixels = 8388608,
                                  .max_dimension = 16384,
                                  .max_work = 128u * 1024u * 1024u,
                                  .max_raster_work = 64000000,
                                  .max_geometry_work = 64000000};
}
static bool empty(Rect r)
{
    return r.x0 == r.x1 || r.y0 == r.y1;
}
static size_t area(Rect r)
{
    return (size_t)(r.x1 - r.x0) * (r.y1 - r.y0);
}
static Rect unite(Rect a, Rect b)
{
    if (empty(a))
        return b;
    if (empty(b))
        return a;
    return (Rect){a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                  a.y1 > b.y1 ? a.y1 : b.y1};
}
static Rect intersect(Rect a, Rect b)
{
    Rect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
              a.y1 < b.y1 ? a.y1 : b.y1};
    return r.x1 <= r.x0 || r.y1 <= r.y0 ? (Rect){0} : r;
}
static Map compose(Map a, Map b)
{
    return (Map){a.a * b.a + a.c * b.b,          a.b * b.a + a.d * b.b,
                 a.a * b.c + a.c * b.d,          a.b * b.c + a.d * b.d,
                 a.a * b.tx + a.c * b.ty + a.tx, a.b * b.tx + a.d * b.ty + a.ty};
}
static bool raster_work(Render *r, uint64_t count)
{
    if (count > r->options->max_raster_work - r->stats.raster_work) {
        r->c->why = SVG_LIMIT;
        return false;
    }
    if (!svg_spend(r->c, count))
        return false;
    r->stats.raster_work += count;
    return true;
}
static bool engine_cancelled(void *context)
{
    return !svg_spend(context, 1);
}
static Status operation(Render *r, os64_geometry_status_t result)
{
    if (r->engine) {
        os64_geometry_stats_t stats;
        if (r->backend->stats(r->engine, &stats) != OS64_GEOMETRY_OK ||
            stats.last_work > r->quota ||
            stats.last_work > r->options->max_geometry_work - r->stats.geometry_work)
            return SVG_ENGINE_ERROR;
        r->stats.geometry_work += stats.last_work;
    }
    if (r->c->why != SVG_OK)
        return r->c->why;
    switch (result) {
    case OS64_GEOMETRY_OK:
        return SVG_OK;
    case OS64_GEOMETRY_LIMIT:
        return SVG_LIMIT;
    case OS64_GEOMETRY_NO_MEMORY:
        return SVG_NO_MEMORY;
    case OS64_GEOMETRY_CANCELLED:
        return SVG_CANCELLED;
    case OS64_GEOMETRY_UNSUPPORTED:
        return SVG_UNSUPPORTED;
    default:
        return SVG_ENGINE_ERROR;
    }
}
static Status clip_bounds(Render *r, os64_geometry_bounds_t b, Rect *out)
{
    *out = (Rect){0};
    if (b.empty)
        return SVG_OK;
    if (!isfinite(b.x_min) || !isfinite(b.y_min) || !isfinite(b.x_max) || !isfinite(b.y_max) ||
        b.x_min > b.x_max || b.y_min > b.y_max)
        return SVG_ENGINE_ERROR;
    /* Bounds come from the actual expanded outline. A two-pixel fringe covers
     * raster quantization and a changed integer clip origin; it does not guess
     * stroke extent from a centerline or clip that centerline before expansion. */
    double x0 = fmax(0, floor(b.x_min - 2)), y0 = fmax(0, floor(b.y_min - 2));
    double x1 = fmin(r->target->width, ceil(b.x_max + 2));
    double y1 = fmin(r->target->height, ceil(b.y_max + 2));
    if (x1 <= x0 || y1 <= y0)
        return SVG_OK;
    *out = (Rect){(uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1};
    return SVG_OK;
}
static Status stroke_outline(Render *r, const svg_geometry_t *geometry, const svg_style_t *style,
                             const Map *map, os64_geometry_outline_t **out)
{
    svg_geometry_t *center = NULL, *caps = NULL;
    Status status =
        svg_stroke_geometry(r->c, geometry, map, r->options->arc_error, style, &center, &caps);
    if (status == SVG_OK && (center->count || caps->count)) {
        os64_geometry_status_t result;
        if (caps->count) {
            if (r->backend->struct_size < sizeof(*r->backend) || !r->backend->stroke_with_caps) {
                status = SVG_UNSUPPORTED;
                goto done;
            }
            result = r->backend->stroke_with_caps(
                r->engine, center->commands, center->count, caps->commands, caps->count, map,
                r->options->quantization_error, &style->stroke_style, out);
        } else {
            result = r->backend->stroke(r->engine, center->commands, center->count, map,
                                        r->options->quantization_error, &style->stroke_style, out);
        }
        status = operation(r, result);
        if (status == SVG_OK && !*out)
            status = SVG_ENGINE_ERROR;
    }
done:
    svg_free_geometry(r->c, center);
    svg_free_geometry(r->c, caps);
    return status;
}
static Status prepare_outlines(Render *r, const svg_scene_t *scene, Item *items)
{
    size_t paints = 0;
    for (size_t i = 0; i < scene->count; ++i) {
        if (!svg_spend(r->c, 1))
            return r->c->why;
        const svg_scene_item_t *s = &scene->items[i];
        Item *p = &items[i];
        p->active = s->style.opacity > 0 && (s->parent == SIZE_MAX || items[s->parent].active);
        if (!p->active)
            continue;
        p->map = compose(r->options->viewport_to_device, s->user_to_viewport);
        if (!svg_map_valid(&p->map))
            return SVG_LIMIT;
        p->clips[0] = s->style.viewport.active && s->style.viewport.clip;
        p->clips[1] = s->clip.active;
        p->clip_map[1] = s->clip.transform;
        if (p->clips[1] && s->clip.object_bbox) {
            svg_bounds_t box;
            Status status = svg_object_bounds(r->c, scene, i, &box);
            if (status != SVG_OK)
                return status;
            /* SVG object-box effects are ignored for zero geometric extent,
             * even when a stroke gives that geometry visible thickness. */
            if (box.empty || box.min_x == box.max_x || box.min_y == box.max_y) {
                p->clips[1] = false;
            } else {
                Map units = {box.max_x - box.min_x, 0, 0, box.max_y - box.min_y,
                             box.min_x, box.min_y};
                p->clip_map[1] = compose(s->clip.outer_transform, compose(units, s->clip.transform));
                if (!svg_map_valid(&units) || !svg_map_valid(&p->clip_map[1]))
                    return SVG_LIMIT;
            }
        }
        if (paints > (UINT64_MAX - 1) / 3 - 3)
            return SVG_LIMIT;
        paints += p->clips[0] + p->clips[1];
        if ((s->kind != SVG_NODE_PATH && s->kind != SVG_NODE_SHAPE) || !s->style.visible ||
            !s->path->count)
            continue;
        const svg_paint_t *paints_for_item[] = {&s->style.fill, &s->style.stroke};
        for (unsigned part = 0; part < 2; ++part) {
            const svg_paint_t *paint = paints_for_item[part];
            bool ink = paint->kind == SVG_PAINT_COLOR && (paint->argb >> 24);
            if (paint->kind == SVG_PAINT_LINEAR || paint->kind == SVG_PAINT_RADIAL) {
                svg_bounds_t box = {0};
                Status status = SVG_OK;
                bool radial = paint->kind == SVG_PAINT_RADIAL;
                if (radial ? paint->radial->object_bbox : paint->linear->object_bbox)
                    status = svg_object_bounds(r->c, scene, i, &box);
                if (status == SVG_OK)
                    status = radial ? svg_radial_gradient_prepare(paint->radial, &p->map, &box,
                                                                  &p->radial[part])
                                    : svg_linear_gradient_prepare(paint->linear, &p->map, &box,
                                                                  &p->gradient[part]);
                if (status != SVG_OK)
                    return status;
                ink = (radial ? p->radial[part].count : p->gradient[part].count) != 0;
            }
            p->paint[part] =
                ink && (part ? s->style.stroke_opacity > 0 && s->style.stroke_style.width > 0
                             : s->style.fill_opacity > 0);
        }
        if (paints > (UINT64_MAX - 1) / 3 - 2)
            return SVG_LIMIT;
        paints += p->paint[0] + p->paint[1];
    }
    if (!paints)
        return SVG_OK;
    /* The backend's work cap is per call. Reserve an equal quota for create
     * plus build/bounds/raster for each paint or clip mask, before work starts.
     * Calls may leave quota unused; they cannot collectively exceed the render
     * budget. This deliberately conservative division can refuse uneven jobs. */
    r->quota = r->options->max_geometry_work / (1 + 3 * (uint64_t)paints);
    if (r->quota > OS64_GEOMETRY_WORK_DEFAULT)
        r->quota = OS64_GEOMETRY_WORK_DEFAULT;
    if (!r->quota)
        return SVG_LIMIT;
    os64_geometry_options_t options = {.memory = r->c->options.memory,
                                       .max_work = r->quota,
                                       .cancel_context = r->c,
                                       .cancelled = engine_cancelled};
    Status status = operation(r, r->backend->create(&options, &r->engine));
    if (status != SVG_OK)
        return status;
    if (!r->engine)
        return SVG_ENGINE_ERROR;
    for (size_t i = 0; i < scene->count; ++i) {
        if (!svg_spend(r->c, 1))
            return r->c->why;
        Item *p = &items[i];
        const svg_scene_item_t *s = &scene->items[i];
        if (p->clips[0]) {
            p->clip_map[0] = p->map;
            double w = s->style.viewport.width, h = s->style.viewport.height;
            os64_geometry_command_t rectangle[] = {{OS64_GEOMETRY_MOVE, {{0, 0}}},
                                                   {OS64_GEOMETRY_LINE, {{w, 0}}},
                                                   {OS64_GEOMETRY_LINE, {{w, h}}},
                                                   {OS64_GEOMETRY_LINE, {{0, h}}},
                                                   {OS64_GEOMETRY_CLOSE, {{0, 0}}}};
            status = operation(r, r->backend->path(r->engine, rectangle, 5, &p->map,
                                                   r->options->quantization_error, &p->clip[0]));
            if (status != SVG_OK)
                return status;
            if (!p->clip[0])
                return SVG_ENGINE_ERROR;
            os64_geometry_bounds_t bounds;
            status =
                operation(r, r->backend->bounds(r->engine, p->clip[0], &p->map, r->target->origin_x,
                                                r->target->origin_y, &bounds));
            if (status != SVG_OK)
                return status;
            status = clip_bounds(r, bounds, &p->clip_box[0]);
            if (status != SVG_OK)
                return status;
        }
        if (p->clips[1] && s->clip.path->count) {
            Map base = p->map;
            if (s->style.viewport.active)
                base = compose(base, s->style.viewport.content_transform);
            p->clip_map[1] = compose(base, p->clip_map[1]);
            if (!svg_map_valid(&p->clip_map[1]))
                return SVG_LIMIT;
            p->clip_even_odd[1] = s->clip.even_odd;
            svg_geometry_t *g = NULL;
            status = svg_lower(r->c, s->clip.path, &p->clip_map[1], r->options->arc_error, &g);
            if (status == SVG_OK)
                status =
                    operation(r, r->backend->path(r->engine, g->commands, g->count, &p->clip_map[1],
                                                  r->options->quantization_error, &p->clip[1]));
            svg_free_geometry(r->c, g);
            if (status != SVG_OK)
                return status;
            if (!p->clip[1])
                return SVG_ENGINE_ERROR;
            os64_geometry_bounds_t bounds;
            status =
                operation(r, r->backend->bounds(r->engine, p->clip[1], &p->clip_map[1],
                                                r->target->origin_x, r->target->origin_y, &bounds));
            if (status == SVG_OK)
                status = clip_bounds(r, bounds, &p->clip_box[1]);
            if (status != SVG_OK)
                return status;
        }
        if (!p->paint[0] && !p->paint[1])
            continue;
        svg_geometry_t *g = NULL;
        status = svg_lower(r->c, s->path, &p->map, r->options->arc_error, &g);
        if (status != SVG_OK)
            return status;
        for (unsigned part = 0; part < 2; ++part) {
            if (!p->paint[part])
                continue;
            if (part) {
                status = stroke_outline(r, g, &s->style, &p->map, &p->outline[part]);
            } else {
                status = operation(r, r->backend->path(r->engine, g->commands, g->count, &p->map,
                                                       r->options->quantization_error,
                                                       &p->outline[part]));
                if (status == SVG_OK && !p->outline[part])
                    status = SVG_ENGINE_ERROR;
            }
            if (status != SVG_OK)
                break;
            if (!p->outline[part])
                continue; // Empty dash results and move-only strokes have no ink.
            os64_geometry_bounds_t bounds;
            status =
                operation(r, r->backend->bounds(r->engine, p->outline[part], &p->map,
                                                r->target->origin_x, r->target->origin_y, &bounds));
            if (status != SVG_OK)
                break;
            status = clip_bounds(r, bounds, &p->ink[part]);
            if (status != SVG_OK)
                break;
            p->box = unite(p->box, p->ink[part]);
        }
        svg_free_geometry(r->c, g);
        if (status != SVG_OK)
            return status;
    }
    for (size_t i = scene->count; i-- > 0;) {
        if (!svg_spend(r->c, 1))
            return r->c->why;
        for (unsigned mask = 0; mask < 2; ++mask)
            if (items[i].clips[mask])
                items[i].box = intersect(items[i].box, items[i].clip_box[mask]);
        size_t parent = scene->items[i].parent;
        if (parent != SIZE_MAX)
            items[parent].box = unite(items[parent].box, items[i].box);
    }
    // Descendants must fit the ancestor layer before any pixel writes. The
    // rectangle bounds only reduce allocation; the raster mask clips the edges.
    for (size_t i = 0; i < scene->count; ++i) {
        if (!svg_spend(r->c, 1))
            return r->c->why;
        size_t parent = scene->items[i].parent;
        if (parent != SIZE_MAX)
            items[i].box = intersect(items[i].box, items[parent].box);
        for (unsigned part = 0; part < 2; ++part)
            items[i].ink[part] = intersect(items[i].ink[part], items[i].box);
    }
    return SVG_OK;
}
static Pixel *at(Layer *l, uint32_t x, uint32_t y)
{
    return l->pixels + (size_t)(y - l->box.y0) * (l->box.x1 - l->box.x0) + (x - l->box.x0);
}
static Status new_layer(Render *r, Rect box, Layer *out)
{
    *out = (Layer){.box = box};
    if (empty(box))
        return SVG_OK;
    if (area(box) > SIZE_MAX / sizeof(Pixel))
        return SVG_LIMIT;
    out->pixels = svg_alloc(r->c, area(box) * sizeof(Pixel));
    if (!out->pixels)
        return r->c->why;
    for (uint32_t y = box.y0; y < box.y1; ++y) {
        if (!raster_work(r, box.x1 - box.x0)) {
            svg_free(r->c, out->pixels);
            out->pixels = NULL;
            return r->c->why;
        }
        Pixel *row = at(out, box.x0, y);
        for (uint32_t x = box.x0; x < box.x1; ++x)
            row[x - box.x0] = (Pixel){0};
    }
    ++r->layers;
    if (r->stats.peak_layers < r->layers)
        r->stats.peak_layers = r->layers;
    return SVG_OK;
}
static void free_layer(Render *r, Layer *l)
{
    if (!l->pixels)
        return;
    svg_free(r->c, l->pixels);
    l->pixels = NULL;
    --r->layers;
}
static uint16_t alpha(double a)
{
    return (uint16_t)(a * 65535 + .5);
}
static uint16_t multiply(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a * b + 32767) / 65535);
}
static Pixel over(Pixel source, Pixel dest)
{
    uint16_t inverse = 65535 - source.a;
    return (Pixel){source.a + multiply(dest.a, inverse), source.r + multiply(dest.r, inverse),
                   source.g + multiply(dest.g, inverse), source.b + multiply(dest.b, inverse)};
}
static Status composite(Render *r, Layer *dest, Layer *source, uint16_t opacity)
{
    Rect b = source->box;
    for (uint32_t y = b.y0; y < b.y1; ++y) {
        if (!raster_work(r, b.x1 - b.x0))
            return r->c->why;
        Pixel *s = at(source, b.x0, y), *d = at(dest, b.x0, y);
        for (uint32_t x = b.x0; x < b.x1; ++x, ++s, ++d) {
            Pixel p = {multiply(s->a, opacity), multiply(s->r, opacity), multiply(s->g, opacity),
                       multiply(s->b, opacity)};
            *d = over(p, *d);
        }
    }
    return SVG_OK;
}
static Status clip_one(Render *r, const Item *p, unsigned mask_index, Layer *layer)
{
    Rect b = layer->box;
    if (!p->clips[mask_index] || empty(b))
        return SVG_OK;
    uint8_t *coverage = svg_alloc(r->c, area(b));
    if (!coverage)
        return r->c->why;
    os64_geometry_target_t target = {.origin_x = r->target->origin_x + b.x0,
                                     .origin_y = r->target->origin_y + b.y0,
                                     .width = b.x1 - b.x0,
                                     .height = b.y1 - b.y0,
                                     .stride = b.x1 - b.x0,
                                     .capacity = area(b),
                                     .pixels = coverage,
                                     .even_odd = p->clip_even_odd[mask_index]};
    Status status = SVG_OK;
    if (!raster_work(r, area(b)))
        status = r->c->why;
    if (status == SVG_OK)
        status = operation(r, r->backend->raster(r->engine, p->clip[mask_index],
                                                 &p->clip_map[mask_index], &target));
    if (status == SVG_OK)
        for (uint32_t y = b.y0; y < b.y1; ++y) {
            if (!raster_work(r, b.x1 - b.x0)) {
                status = r->c->why;
                break;
            }
            Pixel *dest = at(layer, b.x0, y);
            const uint8_t *mask = coverage + (size_t)(y - b.y0) * (b.x1 - b.x0);
            for (uint32_t x = b.x0; x < b.x1; ++x, ++dest, ++mask) {
                uint16_t a = (uint16_t)(*mask * 257);
                *dest = (Pixel){multiply(dest->a, a), multiply(dest->r, a), multiply(dest->g, a),
                                multiply(dest->b, a)};
            }
        }
    svg_free(r->c, coverage);
    return status;
}
static Status clip_layer(Render *r, const Item *p, Layer *layer)
{
    Status status = clip_one(r, p, 0, layer);
    return status == SVG_OK ? clip_one(r, p, 1, layer) : status;
}
static Status paint_path(Render *r, const svg_scene_item_t *s, Item *p, Layer *surface)
{
    for (unsigned part = 0; part < 2; ++part) {
        Rect b = p->ink[part];
        if (empty(b))
            continue;
        uint8_t *coverage = svg_alloc(r->c, area(b));
        if (!coverage)
            return r->c->why;
        os64_geometry_target_t target = {.origin_x = r->target->origin_x + b.x0,
                                         .origin_y = r->target->origin_y + b.y0,
                                         .width = b.x1 - b.x0,
                                         .height = b.y1 - b.y0,
                                         .stride = b.x1 - b.x0,
                                         .capacity = area(b),
                                         .pixels = coverage,
                                         .even_odd = !part && s->style.even_odd};
        Status status = SVG_OK;
        if (!raster_work(r, area(b)))
            status = r->c->why;
        if (status == SVG_OK)
            status =
                operation(r, r->backend->raster(r->engine, p->outline[part], &p->map, &target));
        if (status != SVG_OK) {
            svg_free(r->c, coverage);
            return status;
        }
        const svg_paint_t *paint = part ? &s->style.stroke : &s->style.fill;
        uint16_t opacity = alpha(part ? s->style.stroke_opacity : s->style.fill_opacity);
        bool gradient = paint->kind == SVG_PAINT_LINEAR || paint->kind == SVG_PAINT_RADIAL;
        bool radial = paint->kind == SVG_PAINT_RADIAL;
        for (uint32_t y = b.y0; y < b.y1; ++y) {
            if (!raster_work(r, (uint64_t)(b.x1 - b.x0) * (radial ? 32 : gradient ? 12 : 1))) {
                status = r->c->why;
                break;
            }
            Pixel *dest = at(surface, b.x0, y);
            uint8_t *mask = coverage + (size_t)(y - b.y0) * (b.x1 - b.x0);
            for (uint32_t x = b.x0; x < b.x1; ++x, ++dest, ++mask) {
                double sx = r->target->origin_x + x + .5, sy = r->target->origin_y + y + .5;
                uint32_t color = radial     ? svg_radial_gradient_sample(&p->radial[part], sx, sy)
                                 : gradient ? svg_linear_gradient_sample(&p->gradient[part], sx, sy)
                                            : paint->argb;
                uint16_t a = multiply((uint16_t)((color >> 24) * 257), opacity);
                uint16_t red = (uint16_t)(((color >> 16) & 255) * 257);
                uint16_t green = (uint16_t)(((color >> 8) & 255) * 257);
                uint16_t blue = (uint16_t)((color & 255) * 257);
                uint16_t sa = multiply(a, (uint16_t)(*mask * 257));
                Pixel source = {sa, multiply(red, sa), multiply(green, sa), multiply(blue, sa)};
                *dest = over(source, *dest);
            }
        }
        svg_free(r->c, coverage);
        if (status != SVG_OK)
            return status;
    }
    ++r->stats.painted_paths;
    return SVG_OK;
}
static Status export_pixels(Render *r, Layer *root)
{
    const svg_render_target_t *t = r->target;
    for (uint32_t y = 0; y < t->height; ++y) {
        if (!raster_work(r, t->width))
            return r->c->why;
        uint32_t *row = t->pixels + (size_t)y * (t->stride / sizeof(uint32_t));
        for (uint32_t x = 0; x < t->width; ++x) {
            uint32_t result = 0;
            if (x >= root->box.x0 && x < root->box.x1 && y >= root->box.y0 && y < root->box.y1) {
                Pixel p = *at(root, x, y);
                uint32_t a = ((uint32_t)p.a * 255 + 32767) / 65535;
                if (a)
                    result = (a << 24) | (((uint32_t)p.r * 255 + p.a / 2) / p.a << 16) |
                             (((uint32_t)p.g * 255 + p.a / 2) / p.a << 8) |
                             (((uint32_t)p.b * 255 + p.a / 2) / p.a);
            }
            row[x] = result;
        }
    }
    return SVG_OK;
}
Status svg_render_validate(const svg_render_options_t *o, const svg_render_target_t *t)
{
    if (!o || !t || !t->pixels || (uintptr_t)t->pixels % _Alignof(uint32_t) || !t->width ||
        !t->height || !isfinite(t->origin_x) || !isfinite(t->origin_y) ||
        !svg_map_valid(&o->viewport_to_device) || !isfinite(o->arc_error) || o->arc_error <= 0 ||
        o->arc_error > 1 || !isfinite(o->quantization_error) ||
        o->quantization_error <= 1.0 / 128 || o->quantization_error > 1 ||
        t->stride % sizeof(uint32_t))
        return SVG_BAD_ARGUMENT;
    if (t->width > 65535 || t->height > 65535)
        return SVG_LIMIT;
    size_t row = (size_t)t->width * sizeof(uint32_t);
    if (t->stride < row)
        return SVG_BAD_ARGUMENT;
    if (t->height > 1 && t->stride > (SIZE_MAX - row) / (t->height - 1))
        return SVG_LIMIT;
    if ((size_t)(t->height - 1) * t->stride + row > t->capacity || t->width > o->max_dimension ||
        t->height > o->max_dimension || t->width > 65535 || t->height > 65535 ||
        t->width > o->max_pixels / t->height)
        return SVG_LIMIT;
    const os64_geometry_backend_t *b = o->geometry;
    if (!b || b->revision != OS64_GEOMETRY_BACKEND_REVISION ||
        b->struct_size < offsetof(os64_geometry_backend_t, stroke_with_caps))
        return SVG_UNSUPPORTED;
    if (!b->create || !b->destroy || !b->stats || !b->path || !b->stroke || !b->release ||
        !b->raster || !b->bounds)
        return SVG_BAD_ARGUMENT;
    return SVG_OK;
}
Status svg_scene_render(Context *c, const svg_scene_t *scene, const svg_render_options_t *o,
                        const svg_render_target_t *target, svg_render_stats_t *stats)
{
    if (stats)
        *stats = (svg_render_stats_t){0};
    if (!c || !scene || scene->owner != c)
        return SVG_BAD_ARGUMENT;
    Status status = svg_render_validate(o, target);
    if (status != SVG_OK)
        return status;
    status = svg_enter(c);
    if (status != SVG_OK)
        return status;
    c->work_limit = o->max_work;
    Render r = {.c = c, .options = o, .target = target, .backend = o->geometry};
    Item *items = NULL;
    Frame *frames = NULL;
    size_t depth = 0;
    Layer root = {0};
    if (!svg_spend(c, 1)) {
        status = c->why;
        goto done;
    }
    if (scene->count) {
        if (scene->count > SIZE_MAX / sizeof(Item)) {
            status = SVG_LIMIT;
            goto done;
        }
        items = svg_alloc(c, scene->count * sizeof(Item));
        if (!items) {
            status = c->why;
            goto done;
        }
        for (size_t i = 0; i < scene->count; ++i)
            items[i] = (Item){0};
        status = prepare_outlines(&r, scene, items);
        if (status != SVG_OK)
            goto done;
        size_t capacity =
            scene->count < c->options.limits.depth ? scene->count : c->options.limits.depth;
        if (!capacity || capacity > SIZE_MAX / sizeof(Frame)) {
            status = SVG_LIMIT;
            goto done;
        }
        frames = svg_alloc(c, capacity * sizeof(Frame));
        if (!frames) {
            status = c->why;
            goto done;
        }
        status = new_layer(&r, items[0].box, &root);
        if (status != SVG_OK)
            goto done;
        for (size_t i = 0; i <= scene->count; ++i) {
            if (!svg_spend(c, 1)) {
                status = c->why;
                goto done;
            }
            size_t parent = i == scene->count ? SIZE_MAX : scene->items[i].parent;
            while (depth && frames[depth - 1].index != parent) {
                Frame *frame = &frames[depth - 1];
                if (frame->owns) {
                    Layer *dest = depth > 1 ? frames[depth - 2].surface : &root;
                    status = clip_layer(&r, &items[frame->index], &frame->layer);
                    if (status == SVG_OK)
                        status = composite(&r, dest, &frame->layer, frame->opacity);
                    if (status != SVG_OK)
                        goto done;
                    free_layer(&r, &frame->layer);
                }
                --depth;
            }
            if (i == scene->count)
                break;
            const svg_scene_item_t *s = &scene->items[i];
            Layer *surface = depth ? frames[depth - 1].surface : &root;
            if (s->kind == SVG_NODE_GROUP || s->kind == SVG_NODE_USE) {
                if (depth == capacity) {
                    status = SVG_LIMIT;
                    goto done;
                }
                Frame *frame = &frames[depth];
                *frame =
                    (Frame){.index = i, .surface = surface, .opacity = alpha(s->style.opacity)};
                if ((s->style.opacity < 1 || items[i].clips[0] || items[i].clips[1]) &&
                    !empty(items[i].box)) {
                    status = new_layer(&r, items[i].box, &frame->layer);
                    if (status != SVG_OK)
                        goto done;
                    frame->surface = &frame->layer;
                    frame->owns = true;
                }
                ++depth;
            } else if (!empty(items[i].box)) {
                Layer element = {0};
                bool isolate = s->style.opacity < 1 || items[i].clips[1];
                if (isolate) {
                    status = new_layer(&r, items[i].box, &element);
                    if (status != SVG_OK)
                        goto done;
                }
                status = paint_path(&r, s, &items[i], isolate ? &element : surface);
                if (status == SVG_OK && isolate)
                    status = clip_layer(&r, &items[i], &element);
                if (status == SVG_OK && isolate)
                    status = composite(&r, surface, &element, alpha(s->style.opacity));
                free_layer(&r, &element);
                if (status != SVG_OK)
                    goto done;
            }
        }
    }
    status = export_pixels(&r, &root);
done:
    for (size_t i = 0; i < depth; ++i)
        if (frames[i].owns)
            free_layer(&r, &frames[i].layer);
    free_layer(&r, &root);
    if (items)
        for (size_t i = 0; i < scene->count; ++i) {
            for (unsigned part = 0; part < 2; ++part)
                if (items[i].outline[part] &&
                    r.backend->release(r.engine, items[i].outline[part]) != OS64_GEOMETRY_OK)
                    status = SVG_ENGINE_ERROR;
            for (unsigned mask = 0; mask < 2; ++mask)
                if (items[i].clip[mask] &&
                    r.backend->release(r.engine, items[i].clip[mask]) != OS64_GEOMETRY_OK)
                    status = SVG_ENGINE_ERROR;
        }
    if (r.engine && r.backend->destroy(r.engine) != OS64_GEOMETRY_OK)
        status = SVG_ENGINE_ERROR;
    svg_free(c, frames);
    svg_free(c, items);
    if (stats)
        *stats = r.stats;
    return svg_leave(c, status);
}
