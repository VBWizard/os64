#ifndef OS64_DRAW2D_PAINT_H
#define OS64_DRAW2D_PAINT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Numeric sRGB paint, independent of markup, paths and fonts. Channels in
 * this intermediate are premultiplied; RGB must not exceed alpha. */
typedef struct { uint16_t a, r, g, b; } draw2d_pixel_t;
static inline uint16_t draw2d_multiply(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a * b + 32767) / 65535);
}
static inline draw2d_pixel_t draw2d_scale(draw2d_pixel_t p, uint16_t a)
{
    return (draw2d_pixel_t){draw2d_multiply(p.a, a), draw2d_multiply(p.r, a),
                            draw2d_multiply(p.g, a), draw2d_multiply(p.b, a)};
}
static inline draw2d_pixel_t draw2d_over(draw2d_pixel_t s, draw2d_pixel_t d)
{
    uint16_t inverse = 65535 - s.a;
    return (draw2d_pixel_t){s.a + draw2d_multiply(d.a, inverse),
                            s.r + draw2d_multiply(d.r, inverse),
                            s.g + draw2d_multiply(d.g, inverse),
                            s.b + draw2d_multiply(d.b, inverse)};
}
/* Preserve SVG's rounding order: straight color alpha, opacity, coverage,
 * then premultiplication. Coverage is independent of paint opacity. */
static inline draw2d_pixel_t draw2d_color(uint32_t argb, uint16_t opacity, uint16_t coverage)
{
    uint16_t a = draw2d_multiply((uint16_t)((argb >> 24) * 257), opacity);
    a = draw2d_multiply(a, coverage);
    return (draw2d_pixel_t){a, draw2d_multiply((uint16_t)(((argb >> 16) & 255) * 257), a),
                            draw2d_multiply((uint16_t)(((argb >> 8) & 255) * 257), a),
                            draw2d_multiply((uint16_t)((argb & 255) * 257), a)};
}
static inline draw2d_pixel_t draw2d_unpack(uint32_t premultiplied)
{
    return (draw2d_pixel_t){(uint16_t)((premultiplied >> 24) * 257),
                            (uint16_t)(((premultiplied >> 16) & 255) * 257),
                            (uint16_t)(((premultiplied >> 8) & 255) * 257),
                            (uint16_t)((premultiplied & 255) * 257)};
}
static inline uint32_t draw2d_pack(draw2d_pixel_t p)
{
    return ((uint32_t)((p.a + 128u) / 257) << 24) |
           ((uint32_t)((p.r + 128u) / 257) << 16) |
           ((uint32_t)((p.g + 128u) / 257) << 8) | ((p.b + 128u) / 257);
}
typedef enum { DRAW2D_OK, DRAW2D_BAD_ARGUMENT, DRAW2D_LIMIT, DRAW2D_UNSUPPORTED } draw2d_status_t;
typedef enum {
    DRAW2D_ARGB8_PREMULTIPLIED, DRAW2D_ARGB16_PREMULTIPLIED, DRAW2D_ARGB8_STRAIGHT
} draw2d_format_t;
typedef struct {
    void *pixels;
    uint32_t width, height;
    size_t stride, capacity; /* Bytes; capacity need not include final-row padding. */
    draw2d_format_t format;
} draw2d_surface_t;
typedef struct { uint32_t x0, y0, x1, y1; } draw2d_damage_t;
typedef struct { uint32_t argb; } draw2d_solid_t; /* Straight-alpha sRGB source. */
typedef enum { DRAW2D_SOURCE_OVER, DRAW2D_CLEAR } draw2d_operator_t;
typedef struct {
    draw2d_operator_t operation;
    uint16_t opacity;
} draw2d_composite_t;

#pragma GCC visibility push(default)
/* Empty surfaces may have NULL storage. Nonempty surfaces require aligned
 * storage/stride, nonoverflowing row bounds and sufficient capacity. */
draw2d_status_t draw2d_surface_validate(const draw2d_surface_t *);
/* Axis-aligned device-space rectangle with analytic pixel-area coverage.
 * Negative extents normalize; nonfinite inputs and zero extents draw nothing.
 * Paint targets are premultiplied (straight surfaces can only be validated).
 * CLEAR ignores source and opacity. Admission validates the whole operation
 * and pixel work cap before writing. No allocations/callbacks or partial
 * failure; padding is untouched. damage is optional, empty on no-op/failure.
 * Surface descriptor and damage output must not alias writable pixel storage
 * or each other. Input pixel channels must obey premultiplication. */
draw2d_status_t draw2d_paint_rect(const draw2d_surface_t *, double x, double y,
                                 double width, double height, draw2d_solid_t,
                                 draw2d_composite_t, uint64_t max_pixels,
                                 draw2d_damage_t *damage);
#pragma GCC visibility pop
#endif
