#include "internal.h"

#define PI 3.14159265358979323846264338327950288

static const Map identity = {1, 0, 0, 1, 0, 0};

static Point mapped(const Map *m, Point p)
{
    return (Point){m->a * p.x + m->c * p.y + m->tx, m->b * p.x + m->d * p.y + m->ty};
}
static bool include(Context *c, svg_bounds_t *b, Point p)
{
    if (!svg_point_valid(p)) {
        c->why = SVG_LIMIT;
        return false;
    }
    if (b->empty)
        *b = (svg_bounds_t){false, p.x, p.y, p.x, p.y};
    else {
        b->min_x = fmin(b->min_x, p.x);
        b->min_y = fmin(b->min_y, p.y);
        b->max_x = fmax(b->max_x, p.x);
        b->max_y = fmax(b->max_y, p.y);
    }
    return true;
}
static Point lerp(Point a, Point b, double t)
{
    return (Point){(1 - t) * a.x + t * b.x, (1 - t) * a.y + t * b.y};
}
static bool at_root(Context *c, svg_bounds_t *b, const Point *p, unsigned degree, double t)
{
    if (!(t > 0 && t < 1))
        return true;
    Point q[4];
    for (unsigned i = 0; i <= degree; ++i)
        q[i] = p[i];
    for (unsigned n = degree; n; --n)
        for (unsigned i = 0; i < n; ++i)
            q[i] = lerp(q[i], q[i + 1], t);
    return include(c, b, q[0]);
}
static bool bezier(Context *c, svg_bounds_t *b, const Point *p, unsigned degree)
{
    for (unsigned i = 0; i <= degree; ++i)
        if (!svg_point_valid(p[i])) {
            c->why = SVG_LIMIT;
            return false;
        }
    if (!include(c, b, p[0]) || !include(c, b, p[degree]))
        return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
        double v[4], scale = 0;
        for (unsigned i = 0; i <= degree; ++i) {
            v[i] = axis ? p[i].y : p[i].x;
            scale = fmax(scale, fabs(v[i]));
        }
        if (!scale)
            continue;
        /* Normalize before differences: finite endpoints need not have a
         * representable difference or unscaled polynomial coefficient. */
        for (unsigned i = 0; i <= degree; ++i)
            v[i] /= scale;
        double d0 = v[1] - v[0], d1 = v[2] - v[1];
        if (degree == 2) {
            if (d0 != d1 && !at_root(c, b, p, degree, d0 / (d0 - d1)))
                return false;
            continue;
        }
        double d2 = v[3] - v[2];
        double a = d0 - 2 * d1 + d2, bb = 2 * (d1 - d0), cc = d0;
        if (!a) {
            if (bb && !at_root(c, b, p, degree, -cc / bb))
                return false;
            continue;
        }
        double disc = bb * bb - 4 * a * cc;
        if (disc < 0)
            continue;
        double q = -.5 * (bb + (bb < 0 ? -sqrt(disc) : sqrt(disc)));
        if (!q) {
            if (!at_root(c, b, p, degree, -bb / (2 * a)))
                return false;
        } else if (!at_root(c, b, p, degree, q / a) || !at_root(c, b, p, degree, cc / q))
            return false;
    }
    return true;
}
static bool arc_bounds(Context *c, svg_bounds_t *b, Point from, const svg_command_t *cmd,
                       const Map *map)
{
    if (!include(c, b, mapped(map, from)) || !include(c, b, mapped(map, cmd->p[0])))
        return false;
    if ((from.x == cmd->p[0].x && from.y == cmd->p[0].y) || !cmd->rx || !cmd->ry)
        return true;
    SvgArc a;
    if (!svg_arc_center(c, from, cmd, &a))
        return false;
    a.center = mapped(map, a.center);
    Map linear = *map;
    linear.tx = linear.ty = 0;
    a.u = mapped(&linear, a.u);
    a.v = mapped(&linear, a.v);
    if (!svg_point_valid(a.center) || !svg_point_valid(a.u) || !svg_point_valid(a.v)) {
        c->why = SVG_LIMIT;
        return false;
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
        double theta = axis ? atan2(a.v.y, a.u.y) : atan2(a.v.x, a.u.x);
        for (unsigned opposite = 0; opposite < 2; ++opposite, theta += PI) {
            double distance = fmod(a.sweep > 0 ? theta - a.angle : a.angle - theta, 2 * PI);
            if (distance < 0)
                distance += 2 * PI;
            if (distance <= fabs(a.sweep)) {
                Point p = {a.center.x + a.u.x * cos(theta) + a.v.x * sin(theta),
                           a.center.y + a.u.y * cos(theta) + a.v.y * sin(theta)};
                if (!include(c, b, p))
                    return false;
            }
        }
    }
    return true;
}
Status svg_geometric_bounds(Context *c, const svg_path_t *path, const Map *map, svg_bounds_t *out)
{
    svg_bounds_t b = {.empty = true};
    Point current = {0, 0}, start = {0, 0};
    for (size_t i = 0; i < path->count; ++i) {
        if (!svg_spend(c, 1))
            return c->why;
        const svg_command_t *cmd = &path->commands[i];
        Point p[4] = {mapped(map, current)};
        switch (cmd->verb) {
        case SVG_MOVE:
            start = current = cmd->p[0];
            include(c, &b, mapped(map, current));
            break;
        case SVG_CLOSE:
            current = start;
            include(c, &b, mapped(map, current));
            break;
        case SVG_LINE:
            current = cmd->p[0];
            include(c, &b, mapped(map, current));
            break;
        case SVG_QUADRATIC:
        case SVG_CUBIC: {
            unsigned degree = cmd->verb == SVG_CUBIC ? 3 : 2;
            for (unsigned j = 0; j < degree; ++j)
                p[j + 1] = mapped(map, cmd->p[j]);
            bezier(c, &b, p, degree);
            current = cmd->p[degree - 1];
            break;
        }
        case SVG_ARC:
            arc_bounds(c, &b, current, cmd, map);
            current = cmd->p[0];
            break;
        }
        if (c->why != SVG_OK)
            return c->why;
    }
    *out = b;
    return SVG_OK;
}
static Map compose(Map a, Map b)
{
    return (Map){a.a * b.a + a.c * b.b,          a.b * b.a + a.d * b.b,
                 a.a * b.c + a.c * b.d,          a.b * b.c + a.d * b.d,
                 a.a * b.tx + a.c * b.ty + a.tx, a.b * b.tx + a.d * b.ty + a.ty};
}
Status svg_object_bounds(Context *c, const svg_scene_t *scene, size_t index, svg_bounds_t *out)
{
    svg_bounds_t b = {.empty = true};
    for (size_t i = index; i < scene->count; ++i) {
        const svg_scene_item_t *s = &scene->items[i];
        if (i != index && s->parent < index)
            break; // Scene order is preorder, so this leaves the requested subtree.
        if (!svg_spend(c, 1))
            return c->why;
        if (!s->path)
            continue;
        Map map = identity;
        for (size_t j = i; j != index; j = scene->items[j].parent) {
            if (!svg_spend(c, 1))
                return c->why;
            const svg_scene_item_t *child = &scene->items[j];
            map = compose(child->style.transform, map);
            const svg_scene_item_t *parent = &scene->items[child->parent];
            if (child->parent != index && parent->style.viewport.active)
                map = compose(parent->style.viewport.content_transform, map);
        }
        if (!svg_map_valid(&map))
            return SVG_LIMIT;
        /* Evaluate curves in the requested space. Transforming an already
         * axis-aligned child box would overestimate rotated/sheared curves. */
        svg_bounds_t child = {.empty = true};
        Status status = svg_geometric_bounds(c, s->path, &map, &child);
        if (status != SVG_OK)
            return status;
        if (!child.empty) {
            include(c, &b, (Point){child.min_x, child.min_y});
            include(c, &b, (Point){child.max_x, child.max_y});
        }
    }
    *out = b;
    return SVG_OK;
}
Status svg_path_bounds(Context *c, const svg_path_t *path, const Map *map, svg_bounds_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (svg_bounds_t){.empty = true};
    if (!c || !path || path->owner != c || !svg_map_valid(map))
        return SVG_BAD_ARGUMENT;
    Status status = svg_enter(c);
    if (status != SVG_OK)
        return status;
    return svg_leave(c, svg_geometric_bounds(c, path, map, out));
}
Status svg_scene_object_bounds(Context *c, const svg_scene_t *scene, size_t item, svg_bounds_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (svg_bounds_t){.empty = true};
    if (!c || !scene || scene->owner != c || item >= scene->count)
        return SVG_BAD_ARGUMENT;
    Status status = svg_enter(c);
    if (status != SVG_OK)
        return status;
    return svg_leave(c, svg_object_bounds(c, scene, item, out));
}
