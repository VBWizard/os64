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
    for (size_t i = 0; i < scene->count; ++i)
        svg_free_path(c, (svg_path_t *)scene->items[i].path);
    svg_free(c, scene->items);
    if (scene->pinned)
        scene->provider.release(scene->provider.context, scene->snapshot);
    svg_free(c, scene);
}
typedef struct {
    uint64_t next;
    size_t parent;
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
    stack[depth++] = (Frame){root, SIZE_MAX};
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
        /* Check the active ancestry, not a document-global visited set: the
         * same source can legitimately occur in distinct future use instances. */
        for (size_t i = 1; i < depth; ++i) {
            if (!svg_spend(c, 1)) {
                status = c->why;
                goto done;
            }
            if (scene->items[stack[i].parent].source == source) {
                status = SVG_PROVIDER_ERROR;
                goto done;
            }
        }
        svg_node_t node = {0};
        status = provider_status(provider->node(provider->context, scene->snapshot, source, &node));
        if (status != SVG_OK)
            break;
        frame->next = parent == SIZE_MAX ? 0 : node.next_sibling;
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
        if (node.kind != SVG_NODE_GROUP && node.kind != SVG_NODE_PATH) {
            status = node.kind == SVG_NODE_UNSUPPORTED ? SVG_UNSUPPORTED : SVG_PROVIDER_ERROR;
            break;
        }
        if (node.kind == SVG_NODE_PATH && node.first_child) {
            status = SVG_UNSUPPORTED;
            break;
        }
        item.user_to_viewport =
            compose(parent == SIZE_MAX ? *viewport : scene->items[parent].user_to_viewport,
                    item.style.transform);
        if (!svg_map_valid(&item.user_to_viewport)) {
            status = SVG_LIMIT;
            break;
        }
        scene->items = svg_grow(c, scene->items, &scene->capacity, scene->count,
                                sizeof(*scene->items), c->options.limits.scene_nodes);
        if (c->why != SVG_OK) {
            status = c->why;
            break;
        }
        if (node.kind == SVG_NODE_PATH) {
            svg_path_t *path = NULL;
            status = svg_parse(c, d, &path, &item.path_result);
            if (status != SVG_OK && status != SVG_MALFORMED)
                break;
            path->in_scene = true;
            item.path = path;
            status = SVG_OK; /* Prefix diagnostics remain attached to this item. */
        }
        size_t index = scene->count;
        scene->items[scene->count++] = item;
        if (node.first_child) {
            stack = svg_grow(c, stack, &capacity, depth, sizeof(*stack), c->options.limits.depth);
            if (c->why != SVG_OK) {
                status = c->why;
                break;
            }
            stack[depth++] = (Frame){node.first_child, index};
        }
    }
done:
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
