#include "internal.h"

static Map compose(Map a, Map b)
{
    return (Map){a.a * b.a + a.c * b.b,          a.b * b.a + a.d * b.b,
                 a.a * b.c + a.c * b.d,          a.b * b.c + a.d * b.d,
                 a.a * b.tx + a.c * b.ty + a.tx, a.b * b.tx + a.d * b.ty + a.ty};
}
Status svg_linear_gradient_prepare(const svg_linear_gradient_t *g, const Map *map,
                                   const svg_bounds_t *box, svg_linear_sampler_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (svg_linear_sampler_t){0};
    if (!g || !svg_map_valid(map) || !svg_map_valid(&g->transform) || g->spread < SVG_SPREAD_PAD ||
        g->spread > SVG_SPREAD_REFLECT || !isfinite(g->x1) || !isfinite(g->y1) ||
        !isfinite(g->x2) || !isfinite(g->y2) || (g->count && !g->stops))
        return SVG_BAD_ARGUMENT;
    if (g->count > SVG_GRADIENT_STOPS_MAX)
        return SVG_LIMIT;
    double previous = 0;
    for (size_t i = 0; i < g->count; ++i) {
        double offset = g->stops[i].offset;
        if (!isfinite(offset) || offset < previous || offset > 1)
            return SVG_BAD_ARGUMENT;
        previous = offset;
    }
    Map units = {1, 0, 0, 1, 0, 0};
    if (g->object_bbox) {
        if (!box || !isfinite(box->min_x) || !isfinite(box->min_y) || !isfinite(box->max_x) ||
            !isfinite(box->max_y) || box->max_x < box->min_x || box->max_y < box->min_y)
            return SVG_BAD_ARGUMENT;
        if (box->empty || box->min_x == box->max_x || box->min_y == box->max_y)
            return SVG_OK;
        units =
            (Map){box->max_x - box->min_x, 0, 0, box->max_y - box->min_y, box->min_x, box->min_y};
    }
    Map m = compose(*map, compose(units, g->transform));
    if (!svg_map_valid(&m))
        return SVG_LIMIT;
    double det = m.a * m.d - m.b * m.c;
    if (!isfinite(det))
        return SVG_LIMIT;
    if (!det || !g->count)
        return SVG_OK;
    double dx = g->x2 - g->x1, dy = g->y2 - g->y1, length = hypot(dx, dy);
    if (!isfinite(length))
        return SVG_LIMIT;
    svg_linear_sampler_t sampler = {.stops = g->stops,
                                    .count = g->count,
                                    .spread = g->spread,
                                    .constant = length == 0 || g->count == 1};
    if (!sampler.constant) {
        Map inverse = {m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
        inverse.tx = -inverse.a * m.tx - inverse.c * m.ty;
        inverse.ty = -inverse.b * m.tx - inverse.d * m.ty;
        double vx = (dx / length) / length, vy = (dy / length) / length;
        sampler.x = vx * inverse.a + vy * inverse.b;
        sampler.y = vx * inverse.c + vy * inverse.d;
        sampler.bias = vx * (inverse.tx - g->x1) + vy * (inverse.ty - g->y1);
        if (!svg_map_valid(&inverse) || !isfinite(sampler.x) || !isfinite(sampler.y) ||
            !isfinite(sampler.bias))
            return SVG_LIMIT;
    }
    *out = sampler;
    return SVG_OK;
}
static uint32_t stops_sample(const svg_gradient_stop_t *stops, size_t count, svg_spread_t spread,
                             double t)
{
    if (spread == SVG_SPREAD_REPEAT)
        t -= floor(t);
    else if (spread == SVG_SPREAD_REFLECT) {
        t = fmod(t, 2);
        if (t < 0)
            t += 2;
        if (t > 1)
            t = 2 - t;
    }
    if (t < stops[0].offset)
        return stops[0].argb;
    if (t >= stops[count - 1].offset)
        return stops[count - 1].argb;
    // Upper bound makes the last stop at a duplicate offset own that point.
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (stops[mid].offset <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    const svg_gradient_stop_t *a = &stops[lo - 1], *b = &stops[lo];
    double f = (t - a->offset) / (b->offset - a->offset);
    uint32_t color = 0;
    // SVG interpolates unpremultiplied sRGB and alpha independently.
    for (unsigned shift = 0; shift < 32; shift += 8) {
        double av = (a->argb >> shift) & 255, bv = (b->argb >> shift) & 255;
        color |= (uint32_t)(av + (bv - av) * f + .5) << shift;
    }
    return color;
}
uint32_t svg_linear_gradient_sample(const svg_linear_sampler_t *s, double x, double y)
{
    if (!s || !s->count || !isfinite(x) || !isfinite(y))
        return 0;
    if (s->constant)
        return s->stops[s->count - 1].argb;
    double t = s->x * x + s->y * y + s->bias;
    if (!isfinite(t))
        return 0;
    return stops_sample(s->stops, s->count, s->spread, t);
}

static uint32_t stops_average(const svg_gradient_stop_t *stops, size_t count)
{
    uint32_t color = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        double integral = stops[0].offset * ((stops[0].argb >> shift) & 255);
        for (size_t i = 1; i < count; ++i)
            integral += (stops[i].offset - stops[i - 1].offset) *
                        (((stops[i].argb >> shift) & 255) + ((stops[i - 1].argb >> shift) & 255)) *
                        .5;
        integral += (1 - stops[count - 1].offset) * ((stops[count - 1].argb >> shift) & 255);
        color |= (uint32_t)(integral + .5) << shift;
    }
    return color;
}
Status svg_radial_gradient_prepare(const svg_radial_gradient_t *g, const Map *map,
                                   const svg_bounds_t *box, svg_radial_sampler_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (svg_radial_sampler_t){0};
    if (!g || !isfinite(g->cx) || !isfinite(g->cy) || !isfinite(g->r) || !isfinite(g->fx) ||
        !isfinite(g->fy) || !isfinite(g->fr) || g->r < 0 || g->fr < 0)
        return SVG_BAD_ARGUMENT;
    // Reuse stop/unit/map validation. A nondegenerate numeric linear program
    // also distinguishes an empty object box or singular map from valid paint.
    svg_linear_gradient_t validation = {.x2 = 1,
                                        .transform = g->transform,
                                        .object_bbox = g->object_bbox,
                                        .spread = g->spread,
                                        .stops = g->stops,
                                        .count = g->count};
    svg_linear_sampler_t checked;
    Status status = svg_linear_gradient_prepare(&validation, map, box, &checked);
    if (status != SVG_OK || !checked.count)
        return status;
    double dx = g->cx - g->fx, dy = g->cy - g->fy, dr = g->r - g->fr;
    double distance = hypot(dx, dy), scale = fmax(distance, fmax(g->r, g->fr));
    if (!isfinite(distance) || !isfinite(dr) || !isfinite(scale))
        return SVG_LIMIT;
    if (!distance && !dr)
        return SVG_OK;
    Map units = {1, 0, 0, 1, 0, 0};
    if (g->object_bbox)
        units =
            (Map){box->max_x - box->min_x, 0, 0, box->max_y - box->min_y, box->min_x, box->min_y};
    Map m = compose(*map, compose(units, g->transform));
    double det = m.a * m.d - m.b * m.c;
    Map inverse = {m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
    inverse.tx = -inverse.a * m.tx - inverse.c * m.ty;
    inverse.ty = -inverse.b * m.tx - inverse.d * m.ty;
    if (!svg_map_valid(&inverse))
        return SVG_LIMIT;
    svg_radial_sampler_t s = {.stops = g->stops,
                              .count = g->count,
                              .sample_to_gradient = inverse,
                              .fx = g->fx,
                              .fy = g->fy,
                              .dx = dx / scale,
                              .dy = dy / scale,
                              .fr = g->fr / scale,
                              .dr = dr / scale,
                              .scale = scale,
                              .spread = g->spread,
                              .constant = g->count == 1,
                              .tangent = fabs(distance - fabs(dr)) <=
                                         8 * DBL_EPSILON * fmax(distance, fabs(dr))};
    s.quadratic = s.dx * s.dx + s.dy * s.dy - s.dr * s.dr;
    // Subtracting independently rounded centers/radii can perturb tangency.
    // Collapse only the rounding-sized neighborhood, not visibly narrow cones.
    if (s.tangent)
        s.quadratic = 0;
    s.average = stops_average(g->stops, g->count);
    *out = s;
    return SVG_OK;
}
bool svg_radial_gradient_position(const svg_radial_sampler_t *s, double x, double y,
                                  double *position)
{
    if (!position)
        return false;
    *position = 0;
    if (!s || !s->count || !isfinite(x) || !isfinite(y))
        return false;
    const Map *m = &s->sample_to_gradient;
    double px = (m->a * x + m->c * y + m->tx - s->fx) / s->scale;
    double py = (m->b * x + m->d * y + m->ty - s->fy) / s->scale;
    // |p - t*d|^2 = (fr + t*dr)^2. Choose the last nonnegative-radius
    // circle covering p; there may be two roots or none outside the cone.
    double a = s->quadratic, b = -2 * (px * s->dx + py * s->dy + s->fr * s->dr);
    double c = px * px + py * py - s->fr * s->fr;
    if (!isfinite(b) || !isfinite(c))
        return false;
    double roots[2];
    size_t count = 0;
    bool double_root = false;
    if (!a) {
        if (b)
            roots[count++] = -c / b;
    } else {
        double bb = b * b, ac = 4 * a * c;
        double discriminant = bb - ac;
        if (!isfinite(discriminant))
            return false;
        // Cancellation between rounded products can move a double root to
        // either side of zero. Scale the error by the products, without an absolute
        // epsilon that would fill genuinely uncovered small cones.
        double error = 8 * DBL_EPSILON * fmax(bb, fabs(ac));
        double_root = fabs(discriminant) <= error;
        if (double_root)
            discriminant = 0;
        if (discriminant >= 0) {
            double q = -.5 * (b + (b < 0 ? -sqrt(discriminant) : sqrt(discriminant)));
            roots[count++] = q / a;
            if (q)
                roots[count++] = c / q;
        }
    }
    bool found = false;
    double t = 0;
    for (size_t i = 0; i < count; ++i) {
        double delta = roots[i] * s->dr, radius = s->fr + delta;
        // A zero-radius double root also subtracts nearly equal terms here.
        // Ordinary roots retain the strict nonnegative-radius test.
        double error = double_root ? 8 * DBL_EPSILON * fmax(s->fr, fabs(delta)) : 0;
        if (isfinite(roots[i]) && isfinite(radius) && radius >= -error &&
            (!found || roots[i] > t)) {
            found = true;
            t = roots[i];
        }
    }
    if (found)
        *position = t;
    return found;
}
uint32_t svg_radial_gradient_sample(const svg_radial_sampler_t *s, double x, double y)
{
    if (!s || !s->count || !isfinite(x) || !isfinite(y))
        return 0;
    if (s->constant)
        return s->stops[s->count - 1].argb;
    double t;
    if (!svg_radial_gradient_position(s, x, y, &t)) {
        if (!s->tangent)
            return 0;
        // SVG extends the collapsed side of a tangent cone. Canvas leaves it
        // transparent; this branch is an SVG policy, not quadratic geometry.
        if (s->spread != SVG_SPREAD_PAD)
            return s->average;
        return s->stops[s->dr > 0 ? s->count - 1 : 0].argb;
    }
    return stops_sample(s->stops, s->count, s->spread, t);
}
