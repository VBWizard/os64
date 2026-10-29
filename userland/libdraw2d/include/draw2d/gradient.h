#ifndef DRAW2D_GRADIENT_H
#define DRAW2D_GRADIENT_H
#include <draw2d/paint.h>
/* Numeric sRGB gradient programs. No markup, allocation, or reference lookup.
 * Stops are borrowed and must remain immutable while a prepared sampler is used. */
typedef struct { bool empty; double min_x,min_y,max_x,max_y; } draw2d_gradient_bounds_t;
#define DRAW2D_GRADIENT_STOPS_MAX 256u
typedef enum { DRAW2D_SPREAD_PAD, DRAW2D_SPREAD_REPEAT, DRAW2D_SPREAD_REFLECT } draw2d_spread_t;
typedef struct {
    double offset; /* Normalized, nondecreasing [0,1]; duplicate offsets retain order. */
    uint32_t argb; /* Straight-alpha sRGB. */
} draw2d_gradient_stop_t;
typedef struct {
    double x1, y1, x2, y2;
    os64_geometry_map_t transform;
    bool object_bbox;
    draw2d_spread_t spread;
    const draw2d_gradient_stop_t *stops;
    size_t count;
} draw2d_linear_gradient_t;
/* Prepared numeric sampler. Stop storage is borrowed and must remain immutable
 * until sampling ends. Initialize with draw2d_linear_gradient_prepare; sample
 * positions use the coordinate space selected by that call's map. */
typedef struct {
    const draw2d_gradient_stop_t *stops;
    size_t count;
    double x, y, bias;
    draw2d_spread_t spread;
    bool constant;
} draw2d_linear_sampler_t;
typedef struct {
    double cx, cy, r, fx, fy, fr;
    os64_geometry_map_t transform;
    bool object_bbox;
    draw2d_spread_t spread;
    const draw2d_gradient_stop_t *stops;
    size_t count;
} draw2d_radial_gradient_t;
/* Prepared radial sampler borrows immutable stops, like the linear sampler.
 * Initialize through draw2d_radial_gradient_prepare before sampling. */
typedef struct {
    const draw2d_gradient_stop_t *stops;
    size_t count;
    os64_geometry_map_t sample_to_gradient;
    double fx, fy, dx, dy, fr, dr, scale, quadratic;
    draw2d_spread_t spread;
    uint32_t average;
    bool constant, tangent;
} draw2d_radial_sampler_t;

#pragma GCC visibility push(default)
draw2d_status_t draw2d_linear_gradient_prepare(const draw2d_linear_gradient_t *,const os64_geometry_map_t *,const draw2d_gradient_bounds_t *,draw2d_linear_sampler_t *);
uint32_t draw2d_linear_gradient_sample(const draw2d_linear_sampler_t *,double,double);
draw2d_status_t draw2d_radial_gradient_prepare(const draw2d_radial_gradient_t *,const os64_geometry_map_t *,const draw2d_gradient_bounds_t *,draw2d_radial_sampler_t *);
bool draw2d_radial_gradient_position(const draw2d_radial_sampler_t *,double,double,double *);
/* Canvas cone policy: points outside the cone remain transparent. */
uint32_t draw2d_radial_gradient_sample(const draw2d_radial_sampler_t *,double,double);
/* Direct stop sampling requires a valid spread and at most STOPS_MAX sorted
 * stops satisfying the descriptor contract above. No stop storage is copied. */
uint32_t draw2d_gradient_stops_sample(const draw2d_gradient_stop_t *,size_t,draw2d_spread_t,double);
#pragma GCC visibility pop
#endif
