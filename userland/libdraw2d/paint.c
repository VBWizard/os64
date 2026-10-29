#include <draw2d/paint.h>
#include <math.h>

static size_t pixel_size(draw2d_format_t format)
{
    return format == DRAW2D_ARGB16_PREMULTIPLIED ? sizeof(draw2d_pixel_t) : sizeof(uint32_t);
}
draw2d_status_t draw2d_surface_validate(const draw2d_surface_t *s)
{
    if (!s)
        return DRAW2D_BAD_ARGUMENT;
    if (s->format != DRAW2D_ARGB8_PREMULTIPLIED && s->format != DRAW2D_ARGB16_PREMULTIPLIED &&
        s->format != DRAW2D_ARGB8_STRAIGHT)
        return DRAW2D_UNSUPPORTED;
    if (!s->width || !s->height)
        return DRAW2D_OK;
    size_t unit = pixel_size(s->format);
    size_t alignment = s->format == DRAW2D_ARGB16_PREMULTIPLIED ? _Alignof(draw2d_pixel_t)
                                                             : _Alignof(uint32_t);
    if (!s->pixels || (uintptr_t)s->pixels % alignment || s->stride % alignment)
        return DRAW2D_BAD_ARGUMENT;
    if (s->width > SIZE_MAX / unit)
        return DRAW2D_LIMIT;
    size_t row = (size_t)s->width * unit;
    if (s->stride < row)
        return DRAW2D_BAD_ARGUMENT;
    if (s->height > 1 && s->stride > (SIZE_MAX - row) / (s->height - 1))
        return DRAW2D_LIMIT;
    if ((size_t)(s->height - 1) * s->stride + row > s->capacity)
        return DRAW2D_LIMIT;
    return DRAW2D_OK;
}
static double min(double a, double b) { return a < b ? a : b; }
static double max(double a, double b) { return a > b ? a : b; }

draw2d_status_t draw2d_paint_rect(const draw2d_surface_t *s, double x, double y,
                                 double width, double height, draw2d_solid_t source,
                                 draw2d_composite_t composite, uint64_t max_pixels,
                                 draw2d_damage_t *damage)
{
    if (damage)
        *damage = (draw2d_damage_t){0};
    draw2d_status_t status = draw2d_surface_validate(s);
    if (status != DRAW2D_OK)
        return status;
    if (s->format == DRAW2D_ARGB8_STRAIGHT ||
        (composite.operation != DRAW2D_SOURCE_OVER && composite.operation != DRAW2D_CLEAR))
        return DRAW2D_UNSUPPORTED;
    if (!s->width || !s->height || !isfinite(x) || !isfinite(y) || !isfinite(width) ||
        !isfinite(height) || !width || !height)
        return DRAW2D_OK;
    if (composite.operation == DRAW2D_SOURCE_OVER && (!composite.opacity || !(source.argb >> 24)))
        return DRAW2D_OK;
    /* Finite endpoints may overflow to infinity; clipping precedes integer
     * conversion, so enormous rectangles can still cover a bounded bitmap. */
    double right = x + width, bottom = y + height;
    double left = max(0, min(x, right)), top = max(0, min(y, bottom));
    right = min(s->width, max(x, right));
    bottom = min(s->height, max(y, bottom));
    if (right <= left || bottom <= top)
        return DRAW2D_OK;
    uint32_t x0 = (uint32_t)left, y0 = (uint32_t)top;
    uint32_t x1 = (uint32_t)right, y1 = (uint32_t)bottom;
    if (right > x1) ++x1;
    if (bottom > y1) ++y1;
    if ((uint64_t)(x1 - x0) * (y1 - y0) > max_pixels)
        return DRAW2D_LIMIT;
    for (uint32_t py = y0; py < y1; ++py) {
        unsigned char *row = (unsigned char *)s->pixels + (size_t)py * s->stride;
        double cy = min(bottom, (double)py + 1) - max(top, py);
        for (uint32_t px = x0; px < x1; ++px) {
            double cx = min(right, (double)px + 1) - max(left, px);
            uint16_t coverage = (uint16_t)(cx * cy * 65535 + .5);
            draw2d_pixel_t dest = s->format == DRAW2D_ARGB16_PREMULTIPLIED
                                      ? ((draw2d_pixel_t *)row)[px]
                                      : draw2d_unpack(((uint32_t *)row)[px]);
            draw2d_pixel_t result = composite.operation == DRAW2D_CLEAR
                ? draw2d_scale(dest, 65535 - coverage)
                : draw2d_over(draw2d_color(source.argb, composite.opacity, coverage), dest);
            if (s->format == DRAW2D_ARGB16_PREMULTIPLIED)
                ((draw2d_pixel_t *)row)[px] = result;
            else
                ((uint32_t *)row)[px] = draw2d_pack(result);
        }
    }
    if (damage)
        *damage = (draw2d_damage_t){x0, y0, x1, y1};
    return DRAW2D_OK;
}
