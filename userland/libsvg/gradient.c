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
uint32_t svg_linear_gradient_sample(const svg_linear_sampler_t *s, double x, double y)
{
    if (!s || !s->count || !isfinite(x) || !isfinite(y))
        return 0;
    if (s->constant)
        return s->stops[s->count - 1].argb;
    double t = s->x * x + s->y * y + s->bias;
    if (!isfinite(t))
        return 0;
    if (s->spread == SVG_SPREAD_REPEAT)
        t -= floor(t);
    else if (s->spread == SVG_SPREAD_REFLECT) {
        t = fmod(t, 2);
        if (t < 0)
            t += 2;
        if (t > 1)
            t = 2 - t;
    }
    if (t < s->stops[0].offset)
        return s->stops[0].argb;
    if (t >= s->stops[s->count - 1].offset)
        return s->stops[s->count - 1].argb;
    // Upper bound makes the last stop at a duplicate offset own that point.
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->stops[mid].offset <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    const svg_gradient_stop_t *a = &s->stops[lo - 1], *b = &s->stops[lo];
    double f = (t - a->offset) / (b->offset - a->offset);
    uint32_t color = 0;
    // SVG interpolates unpremultiplied sRGB and alpha independently.
    for (unsigned shift = 0; shift < 32; shift += 8) {
        double av = (a->argb >> shift) & 255, bv = (b->argb >> shift) & 255;
        color |= (uint32_t)(av + (bv - av) * f + .5) << shift;
    }
    return color;
}
