#include "internal.h"

#define PI 3.14159265358979323846264338327950288

static bool emit(Context *c, svg_geometry_t *g, os64_geometry_command_t cmd)
{
    for (size_t i = 0; i < 3; ++i) {
        if (!svg_point_valid(cmd.p[i])) {
            c->why = SVG_LIMIT;
            return false;
        }
    }
    size_t limit = c->options.limits.geometry_commands;
    if (limit > OS64_GEOMETRY_COMMAND_MAX)
        limit = OS64_GEOMETRY_COMMAND_MAX;
    if (!svg_spend(c, 1))
        return false;
    g->commands = svg_grow(c, g->commands, &g->capacity, g->count, sizeof(*g->commands), limit);
    if (c->why != SVG_OK)
        return false;
    g->commands[g->count++] = cmd;
    return true;
}
static bool simple(Context *c, svg_geometry_t *g, os64_geometry_verb_t verb, Point p)
{
    os64_geometry_command_t cmd = {.verb = verb, .p = {p}};
    return emit(c, g, cmd);
}
typedef struct {
    Point center, u, v;
    double angle, sweep, numerical_error;
} Arc;

/* SVG endpoint-to-center conversion in a normalized, translated frame. Ratios
 * whose binary64 evaluation loses range return LIMIT; no coordinate clamps.
 * hypot avoids squaring overflowing radii or underflowing endpoint offsets. */
static bool center_arc(Context *c, Point from, const svg_command_t *cmd, Arc *arc)
{
    Point to = cmd->p[0];
    double dx = from.x / 2 - to.x / 2, dy = from.y / 2 - to.y / 2;
    double scale = fmax(fmax(fabs(dx), fabs(dy)), fmax(cmd->rx, cmd->ry));
    double rx = cmd->rx / scale, ry = cmd->ry / scale;
    double phi = fmod(cmd->rotation, 360.0) * (PI / 180);
    double cp = cos(phi), sp = sin(phi);
    dx /= scale;
    dy /= scale;
    double x = cp * dx + sp * dy, y = -sp * dx + cp * dy;
    if (!(rx > 0) || !(ry > 0))
        goto range;
    double qx = x / rx, qy = y / ry;
    double h = hypot(qx, qy);
    if (!isfinite(h) || !(h > 0))
        goto range;
    /* Center selection becomes ill-conditioned where the two possible
     * centers meet. Propagate uncertainty through sqrt(1-h*h), rather than
     * treating a near-semicircle as ordinary linear roundoff. */
    double h_error = 64 * DBL_EPSILON * (1 + (fabs(dx) + fabs(dy)) / fmin(rx, ry));
    double original_h = h;
    if (h > 1) {
        rx *= h;
        ry *= h;
        qx = x / rx;
        qy = y / ry;
        h = 1;
    }
    double sign = cmd->large == cmd->sweep ? -1 : 1;
    double lift = sqrt(fmax(0, (1 - h) * (1 + h)));
    double square_error = h_error * (2 * original_h + h_error);
    double lift_error = original_h > 1 + h_error
                            ? 0
                            : fmin(sqrt(square_error), lift > 0 ? square_error / lift : INFINITY);
    arc->numerical_error = 16 * scale * fmax(rx, ry) * (lift_error + h_error / original_h);
    double cx = sign * rx * (qy / h) * lift;
    double cy = -sign * ry * (qx / h) * lift;
    double ux = (x - cx) / rx, uy = (y - cy) / ry;
    double vx = (-x - cx) / rx, vy = (-y - cy) / ry;
    arc->angle = atan2(uy, ux);
    arc->sweep = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (cmd->sweep && arc->sweep < 0)
        arc->sweep += 2 * PI;
    if (!cmd->sweep && arc->sweep > 0)
        arc->sweep -= 2 * PI;
    arc->center = (Point){from.x / 2 + to.x / 2 + scale * (cp * cx - sp * cy),
                          from.y / 2 + to.y / 2 + scale * (sp * cx + cp * cy)};
    arc->u = (Point){scale * rx * cp, scale * rx * sp};
    arc->v = (Point){-scale * ry * sp, scale * ry * cp};
    if (svg_point_valid(arc->center) && svg_point_valid(arc->u) && svg_point_valid(arc->v) &&
        isfinite(arc->angle) && isfinite(arc->sweep) && arc->sweep != 0 &&
        isfinite(arc->numerical_error))
        return true;
range:
    c->why = SVG_LIMIT;
    return false;
}
static Point arc_point(const Arc *a, double theta)
{
    double cs = cos(theta), sn = sin(theta);
    return (Point){a->center.x + a->u.x * cs + a->v.x * sn,
                   a->center.y + a->u.y * cs + a->v.y * sn};
}
static Point arc_derivative(const Arc *a, double theta)
{
    double cs = cos(theta), sn = sin(theta);
    return (Point){-a->u.x * sn + a->v.x * cs, -a->u.y * sn + a->v.y * cs};
}
static bool lower_arc(Context *c, svg_geometry_t *g, Point from, const svg_command_t *cmd,
                      const Map *map, double tolerance)
{
    Arc arc;
    if (!center_arc(c, from, cmd, &arc))
        return false;
    /* For each device axis, the fourth derivative of an ellipse coordinate
     * is bounded by |mapped u|+|mapped v|. Cubic Hermite interpolation has
     * scalar positional remainder <= M * step^4 / 384. Reserve half the
     * requested error for numerical conversion; check a conservative binary64
     * roundoff allowance separately. This is not a tangent/stroke-error bound. */
    double mx =
        fabs(map->a * arc.u.x + map->c * arc.u.y) + fabs(map->a * arc.v.x + map->c * arc.v.y);
    double my =
        fabs(map->b * arc.u.x + map->d * arc.u.y) + fabs(map->b * arc.v.x + map->d * arc.v.y);
    double bound = fmax(mx, my);
    double magnitude = fabs(arc.center.x) + fabs(arc.center.y) + fabs(arc.u.x) + fabs(arc.u.y) +
                       fabs(arc.v.x) + fabs(arc.v.y) + fabs(from.x) + fabs(from.y) +
                       fabs(cmd->p[0].x) + fabs(cmd->p[0].y);
    double stretch = fmax(fabs(map->a) + fabs(map->c), fabs(map->b) + fabs(map->d));
    double roundoff = (magnitude * (512 * DBL_EPSILON) + arc.numerical_error) * stretch;
    if (!isfinite(bound) || !isfinite(roundoff) || roundoff > tolerance / 2) {
        c->why = SVG_LIMIT;
        return false;
    }
    size_t pieces = (size_t)ceil(fabs(arc.sweep) / (PI / 2));
    if (!pieces)
        pieces = 1;
    double step = arc.sweep / (double)pieces;
    while (bound * (step * step) * (step * step) / 384 > tolerance / 2) {
        if (!svg_spend(c, 1))
            return false;
        if (pieces > OS64_GEOMETRY_COMMAND_MAX / 2) {
            c->why = SVG_LIMIT;
            return false;
        }
        pieces *= 2;
        step = arc.sweep / (double)pieces;
    }
    for (size_t i = 0; i < pieces; ++i) {
        if (!svg_spend(c, 1))
            return false;
        double a0 = arc.angle + step * (double)i;
        double a1 = arc.angle + step * (double)(i + 1);
        Point end = i + 1 == pieces ? cmd->p[0] : arc_point(&arc, a1);
        Point d0 = arc_derivative(&arc, a0), d1 = arc_derivative(&arc, a1);
        os64_geometry_command_t out = {.verb = OS64_GEOMETRY_CUBIC,
                                       .p = {{from.x + d0.x * step / 3, from.y + d0.y * step / 3},
                                             {end.x - d1.x * step / 3, end.y - d1.y * step / 3},
                                             end}};
        if (!emit(c, g, out))
            return false;
        from = end;
    }
    return true;
}
Status svg_path_lower(Context *c, const svg_path_t *p, const Map *map, double tolerance,
                      svg_geometry_t **out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    if (!c || !p || p->owner != c || !svg_map_valid(map) || !isfinite(tolerance) ||
        tolerance <= 0 || tolerance > 1)
        return SVG_BAD_ARGUMENT;
    Status status = svg_enter(c);
    if (status != SVG_OK)
        return status;
    return svg_leave(c, svg_lower(c, p, map, tolerance, out));
}
Status svg_lower(Context *c, const svg_path_t *p, const Map *map, double tolerance,
                 svg_geometry_t **out)
{
    *out = NULL;
    svg_geometry_t *g = svg_alloc(c, sizeof(*g));
    if (!g)
        return c->why;
    *g = (svg_geometry_t){.owner = c};
    Point current = {0, 0}, start = {0, 0};
    bool closed = false;
    for (size_t i = 0; i < p->count; ++i) {
        if (!svg_spend(c, 1))
            break;
        const svg_command_t *cmd = &p->commands[i];
        if (cmd->verb == SVG_ARC && cmd->p[0].x == current.x && cmd->p[0].y == current.y)
            continue;
        if (closed && cmd->verb != SVG_MOVE && cmd->verb != SVG_CLOSE) {
            if (!simple(c, g, OS64_GEOMETRY_MOVE, current))
                break;
            closed = false;
        }
        os64_geometry_command_t converted = {0};
        switch (cmd->verb) {
        case SVG_MOVE:
            converted.verb = OS64_GEOMETRY_MOVE;
            start = current = cmd->p[0];
            closed = false;
            break;
        case SVG_LINE:
            converted.verb = OS64_GEOMETRY_LINE;
            current = cmd->p[0];
            break;
        case SVG_QUADRATIC:
            converted.verb = OS64_GEOMETRY_QUADRATIC;
            current = cmd->p[1];
            break;
        case SVG_CUBIC:
            converted.verb = OS64_GEOMETRY_CUBIC;
            current = cmd->p[2];
            break;
        case SVG_CLOSE:
            if (closed)
                continue;
            converted.verb = OS64_GEOMETRY_CLOSE;
            current = start;
            closed = true;
            break;
        case SVG_ARC:
            if (cmd->rx == 0 || cmd->ry == 0)
                simple(c, g, OS64_GEOMETRY_LINE, cmd->p[0]);
            else
                lower_arc(c, g, current, cmd, map, tolerance);
            current = cmd->p[0];
            continue;
        }
        for (size_t j = 0; j < 3; ++j)
            converted.p[j] = cmd->p[j];
        if (!emit(c, g, converted))
            break;
    }
    if (c->why == SVG_OK) {
        ++c->stats.geometries;
        *out = g;
    } else {
        svg_free(c, g->commands);
        svg_free(c, g);
    }
    return c->why;
}
const os64_geometry_command_t *svg_geometry_commands(const svg_geometry_t *g, size_t *count)
{
    if (count)
        *count = g ? g->count : 0;
    return g ? g->commands : NULL;
}
Status svg_geometry_release(Context *c, svg_geometry_t *g)
{
    if (!c || !g || g->owner != c)
        return SVG_BAD_ARGUMENT;
    if (c->busy)
        return SVG_BUSY;
    c->busy = true;
    svg_free_geometry(c, g);
    return svg_leave(c, SVG_OK);
}

void svg_free_geometry(Context *c, svg_geometry_t *g)
{
    if (!g)
        return;
    svg_free(c, g->commands);
    svg_free(c, g);
    --c->stats.geometries;
}
