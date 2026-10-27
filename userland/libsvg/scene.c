#include "internal.h"

svg_style_t svg_initial_style(void)
{
    return (svg_style_t){.fill = {SVG_PAINT_COLOR, 0xff000000},
                         .stroke = {SVG_PAINT_NONE, 0},
                         .fill_opacity = 1,
                         .stroke_opacity = 1,
                         .opacity = 1,
                         .stroke_style = {1, 4, OS64_GEOMETRY_BUTT, OS64_GEOMETRY_MITER},
                         .display = true,
                         .visible = true,
                         .transform = {1, 0, 0, 1, 0, 0}};
}
static bool opacity(double value)
{
    return isfinite(value) && value >= 0 && value <= 1;
}
static Status style_status(const svg_style_t *s)
{
    if (!opacity(s->opacity) || !opacity(s->fill_opacity) || !opacity(s->stroke_opacity) ||
        !svg_map_valid(&s->transform) || !isfinite(s->stroke_style.width) ||
        s->stroke_style.width < 0 || !isfinite(s->stroke_style.miter_limit) ||
        s->stroke_style.miter_limit < 1 || s->stroke_style.cap < OS64_GEOMETRY_BUTT ||
        s->stroke_style.cap > OS64_GEOMETRY_SQUARE || s->stroke_style.join < OS64_GEOMETRY_MITER ||
        s->stroke_style.join > OS64_GEOMETRY_ARCS || s->fill.kind < SVG_PAINT_NONE ||
        s->fill.kind > SVG_PAINT_UNSUPPORTED || s->stroke.kind < SVG_PAINT_NONE ||
        s->stroke.kind > SVG_PAINT_UNSUPPORTED)
        return SVG_PROVIDER_ERROR;
    if (s->viewport.active &&
        (!isfinite(s->viewport.width) || !isfinite(s->viewport.height) || s->viewport.width < 0 ||
         s->viewport.height < 0 || !svg_map_valid(&s->viewport.content_transform)))
        return SVG_PROVIDER_ERROR;
    if (s->dash.count > SVG_DASH_MAX)
        return SVG_LIMIT;
    if ((s->dash.count && !s->dash.values) || !isfinite(s->dash.offset))
        return SVG_PROVIDER_ERROR;
    for (size_t i = 0; i < s->dash.count; ++i)
        if (!isfinite(s->dash.values[i]) || s->dash.values[i] < 0)
            return SVG_PROVIDER_ERROR;
    if (s->fill.kind == SVG_PAINT_UNSUPPORTED || s->stroke.kind == SVG_PAINT_UNSUPPORTED)
        return SVG_UNSUPPORTED;
    return SVG_OK;
}
static Map compose(Map a, Map b)
{
    return (Map){a.a * b.a + a.c * b.b,          a.b * b.a + a.d * b.b,
                 a.a * b.c + a.c * b.d,          a.b * b.c + a.d * b.d,
                 a.a * b.tx + a.c * b.ty + a.tx, a.b * b.tx + a.d * b.ty + a.ty};
}
static Status provider_status(Status s)
{
    switch (s) {
    case SVG_OK:
    case SVG_UNSUPPORTED:
    case SVG_LIMIT:
    case SVG_NO_MEMORY:
    case SVG_CANCELLED:
    case SVG_PENDING:
        return s;
    default:
        return SVG_PROVIDER_ERROR;
    }
}
static void free_scene(Context *c, svg_scene_t *scene)
{
    for (size_t i = 0; i < scene->count; ++i) {
        svg_free_path(c, (svg_path_t *)scene->items[i].path);
        svg_free(c, (void *)scene->items[i].style.dash.values);
    }
    svg_free(c, scene->items);
    if (scene->pinned)
        scene->provider.release(scene->provider.context, scene->snapshot);
    svg_free(c, scene);
}
typedef struct {
    uint64_t next;
    size_t parent;
    bool reference;
} Frame;

Status svg_scene_prepare(Context *c, const svg_provider_t *provider, svg_mode_t mode,
                         const Map *viewport, svg_scene_t **out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    if (!provider || !provider->acquire || !provider->release || !provider->node ||
        !provider->style || !svg_map_valid(viewport) ||
        (mode != SVG_MODE_INLINE && mode != SVG_MODE_IMAGE))
        return SVG_BAD_ARGUMENT;
    Status status = svg_enter(c);
    if (status != SVG_OK)
        return status;
    svg_scene_t *scene = svg_alloc(c, sizeof(*scene));
    if (!scene)
        return svg_leave(c, c->why);
    *scene = (svg_scene_t){.owner = c, .provider = *provider};
    Frame *stack = NULL;
    double *pending_dash = NULL;
    size_t capacity = 0, depth = 0, visits = 0;
    uint64_t root = 0;
    if (!svg_spend(c, 1)) {
        status = c->why;
        goto done;
    }
    status = provider_status(provider->acquire(provider->context, mode, &scene->snapshot, &root));
    if (status != SVG_OK)
        goto done;
    scene->pinned = true;
    if (!root) {
        status = SVG_PROVIDER_ERROR;
        goto done;
    }
    stack = svg_grow(c, stack, &capacity, depth, sizeof(*stack), c->options.limits.depth);
    if (c->why != SVG_OK) {
        status = c->why;
        goto done;
    }
    stack[depth++] = (Frame){root, SIZE_MAX, false};
    while (depth) {
        Frame *frame = &stack[depth - 1];
        if (!frame->next) {
            --depth;
            continue;
        }
        if (!svg_spend(c, 1)) {
            status = c->why;
            break;
        }
        if (visits >= c->options.limits.scene_nodes) {
            status = SVG_LIMIT;
            break;
        }
        ++visits;
        uint64_t source = frame->next;
        size_t parent = frame->parent;
        /* Classify the whole loop, not just its closing edge. A use expansion
         * can close through a source child; a reference above a malformed
         * child-only loop does not make that loop valid. */
        bool reference_on_path = false, cycle = false;
        for (size_t i = depth; i-- > 1;) {
            if (!svg_spend(c, 1)) {
                status = c->why;
                goto done;
            }
            reference_on_path |= stack[i].reference;
            if (scene->items[stack[i].parent].source == source) {
                if (!reference_on_path) {
                    status = SVG_PROVIDER_ERROR;
                    goto done;
                }
                cycle = true;
                break;
            }
        }
        svg_node_t node = {0};
        status = provider_status(provider->node(provider->context, scene->snapshot, source, &node));
        if (status != SVG_OK)
            break;
        frame->next = parent == SIZE_MAX || frame->reference ? 0 : node.next_sibling;
        // Empty the repeated occurrence, preserving its ordinary siblings.
        if (cycle)
            continue;
        svg_style_request_t request = {
            .source = source,
            .instance = visits,
            .parent_instance = parent == SIZE_MAX ? 0 : scene->items[parent].instance,
            .parent_style = parent == SIZE_MAX ? NULL : &scene->items[parent].style,
            .mode = mode};
        svg_scene_item_t item = {.source = source,
                                 .instance = visits,
                                 .parent = parent,
                                 .kind = node.kind,
                                 .style = svg_initial_style()};
        svg_string_t d = {0};
        if (!svg_spend(c, 1)) {
            status = c->why;
            break;
        }
        status = provider_status(
            provider->style(provider->context, scene->snapshot, &request, &item.style, &d));
        if (status != SVG_OK)
            break;
        if (!item.style.display)
            continue;
        status = style_status(&item.style);
        if (status != SVG_OK)
            break;
        if (node.kind != SVG_NODE_GROUP && node.kind != SVG_NODE_PATH &&
            node.kind != SVG_NODE_SHAPE && node.kind != SVG_NODE_USE) {
            status = node.kind == SVG_NODE_UNSUPPORTED ? SVG_UNSUPPORTED : SVG_PROVIDER_ERROR;
            break;
        }
        if (node.kind != SVG_NODE_GROUP && node.first_child) {
            status = SVG_UNSUPPORTED;
            break;
        }
        if (item.style.viewport.active) {
            if (node.kind != SVG_NODE_GROUP && node.kind != SVG_NODE_USE) {
                status = SVG_PROVIDER_ERROR;
                break;
            }
            if (!item.style.viewport.width || !item.style.viewport.height)
                continue;
        }
        Map inherited = parent == SIZE_MAX ? *viewport : scene->items[parent].user_to_viewport;
        if (parent != SIZE_MAX && scene->items[parent].style.viewport.active)
            inherited = compose(inherited, scene->items[parent].style.viewport.content_transform);
        item.user_to_viewport = compose(inherited, item.style.transform);
        if (!svg_map_valid(&item.user_to_viewport)) {
            status = SVG_LIMIT;
            break;
        }
        if (item.style.dash.count) {
            pending_dash = svg_alloc(c, item.style.dash.count * sizeof(*pending_dash));
            if (!pending_dash) {
                status = c->why;
                break;
            }
            for (size_t i = 0; i < item.style.dash.count; ++i)
                pending_dash[i] = item.style.dash.values[i];
        }
        item.style.dash.values = pending_dash;
        scene->items = svg_grow(c, scene->items, &scene->capacity, scene->count,
                                sizeof(*scene->items), c->options.limits.scene_nodes);
        if (c->why != SVG_OK) {
            status = c->why;
            break;
        }
        if (node.kind == SVG_NODE_PATH || node.kind == SVG_NODE_SHAPE) {
            svg_path_t *path = NULL;
            if (node.kind == SVG_NODE_SHAPE) {
                svg_shape_t shape = {0};
                // Growing scene items may have moved the parent style.
                request.parent_style = parent == SIZE_MAX ? NULL : &scene->items[parent].style;
                if (!provider->shape) {
                    status = SVG_PROVIDER_ERROR;
                    break;
                }
                if (!svg_spend(c, 1)) {
                    status = c->why;
                    break;
                }
                status = provider_status(
                    provider->shape(provider->context, scene->snapshot, &request, &shape));
                if (status != SVG_OK)
                    break;
                status = svg_make_shape(c, &shape, &path, &item.path_result);
            } else {
                status = svg_parse(c, d, &path, &item.path_result);
            }
            if (status != SVG_OK && status != SVG_MALFORMED)
                break;
            path->in_scene = true;
            item.path = path;
            status = SVG_OK; /* Prefix diagnostics remain attached to this item. */
        }
        size_t index = scene->count;
        scene->items[scene->count++] = item;
        pending_dash = NULL;
        uint64_t child = node.first_child;
        if (node.kind == SVG_NODE_USE) {
            if (!provider->reference) {
                status = SVG_PROVIDER_ERROR;
                break;
            }
            if (!svg_spend(c, 1)) {
                status = c->why;
                break;
            }
            request.parent_style = parent == SIZE_MAX ? NULL : &scene->items[parent].style;
            status = provider_status(
                provider->reference(provider->context, scene->snapshot, &request, &child));
            if (status != SVG_OK)
                break;
        }
        if (child) {
            stack = svg_grow(c, stack, &capacity, depth, sizeof(*stack), c->options.limits.depth);
            if (c->why != SVG_OK) {
                status = c->why;
                break;
            }
            stack[depth++] = (Frame){child, index, node.kind == SVG_NODE_USE};
        }
    }
done:
    svg_free(c, pending_dash);
    svg_free(c, stack);
    if (status == SVG_OK) {
        ++c->stats.scenes;
        *out = scene;
    } else {
        free_scene(c, scene);
    }
    return svg_leave(c, status);
}
const svg_scene_item_t *svg_scene_items(const svg_scene_t *scene, size_t *count)
{
    if (count)
        *count = scene ? scene->count : 0;
    return scene ? scene->items : NULL;
}
Status svg_scene_release(Context *c, svg_scene_t *scene)
{
    if (!c || !scene || scene->owner != c)
        return SVG_BAD_ARGUMENT;
    if (c->busy)
        return SVG_BUSY;
    c->busy = true;
    free_scene(c, scene);
    --c->stats.scenes;
    return svg_leave(c, SVG_OK);
}
