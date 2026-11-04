#include "internal.h"
/* SVG selects object-box units and owns its tangent-cone extension policy;
 * the numeric programs and root solving are shared with Canvas. */
static Status result(draw2d_status_t s)
{ return s==DRAW2D_OK?SVG_OK:s==DRAW2D_LIMIT?SVG_LIMIT:SVG_BAD_ARGUMENT; }
static draw2d_gradient_bounds_t bounds(const svg_bounds_t *b)
{ return b?(draw2d_gradient_bounds_t){b->empty,b->min_x,b->min_y,b->max_x,b->max_y}:(draw2d_gradient_bounds_t){0}; }
Status svg_linear_gradient_prepare(const svg_linear_gradient_t *g,const Map *m,const svg_bounds_t *b,svg_linear_sampler_t *out)
{ draw2d_gradient_bounds_t box=bounds(b);return result(draw2d_linear_gradient_prepare(g,m,b?&box:NULL,out)); }
uint32_t svg_linear_gradient_sample(const svg_linear_sampler_t *s,double x,double y)
{ return draw2d_linear_gradient_sample(s,x,y); }
Status svg_radial_gradient_prepare(const svg_radial_gradient_t *g,const Map *m,const svg_bounds_t *b,svg_radial_sampler_t *out)
{ draw2d_gradient_bounds_t box=bounds(b);return result(draw2d_radial_gradient_prepare(g,m,b?&box:NULL,out)); }
bool svg_radial_gradient_position(const svg_radial_sampler_t *s,double x,double y,double *t)
{ return draw2d_radial_gradient_position(s,x,y,t); }
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
    return draw2d_gradient_stops_sample(s->stops, s->count, s->spread, t);
}
