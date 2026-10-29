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

static uint16_t add(uint16_t a, uint16_t b)
{ uint32_t n=(uint32_t)a+b; return n>65535?65535:(uint16_t)n; }
static draw2d_pixel_t sum(draw2d_pixel_t a, draw2d_pixel_t b)
{ return (draw2d_pixel_t){add(a.a,b.a),add(a.r,b.r),add(a.g,b.g),add(a.b,b.b)}; }
static draw2d_pixel_t composite_pixel(draw2d_pixel_t s, draw2d_pixel_t d, draw2d_operator_t op)
{
    uint16_t fs=65535,fd=65535;
    switch(op) {
    case DRAW2D_SOURCE_OVER: return draw2d_over(s,d);
    case DRAW2D_DESTINATION_OUT: fs=0;fd=65535-s.a;break;
    case DRAW2D_SOURCE_IN: fs=d.a;fd=0;break;
    case DRAW2D_SOURCE_OUT: fs=65535-d.a;fd=0;break;
    case DRAW2D_SOURCE_ATOP: fs=d.a;fd=65535-s.a;break;
    case DRAW2D_DESTINATION_OVER: fs=65535-d.a;break;
    case DRAW2D_DESTINATION_IN: fs=0;fd=s.a;break;
    case DRAW2D_DESTINATION_ATOP: fs=65535-d.a;fd=s.a;break;
    case DRAW2D_COPY: fd=0;break;
    case DRAW2D_XOR: fs=65535-d.a;fd=65535-s.a;break;
    case DRAW2D_LIGHTER: break;
    default: return d;
    }
    return sum(draw2d_scale(s,fs),draw2d_scale(d,fd));
}
static draw2d_pixel_t image_pixel(const draw2d_surface_t *s, double x, double y)
{
    uint32_t px=x<0?0:x>=s->width?s->width-1:(uint32_t)x;
    uint32_t py=y<0?0:y>=s->height?s->height-1:(uint32_t)y;
    const unsigned char *row=(const unsigned char *)s->pixels+(size_t)py*s->stride;
    if(s->format==DRAW2D_ARGB16_PREMULTIPLIED)return ((const draw2d_pixel_t *)row)[px];
    uint32_t p=((const uint32_t *)row)[px];
    return s->format==DRAW2D_ARGB8_STRAIGHT?draw2d_color(p,65535,65535):draw2d_unpack(p);
}
static draw2d_pixel_t interpolate(draw2d_pixel_t a,draw2d_pixel_t b,double t)
{
    return (draw2d_pixel_t){a.a+(b.a-a.a)*t+.5,a.r+(b.r-a.r)*t+.5,
                            a.g+(b.g-a.g)*t+.5,a.b+(b.b-a.b)*t+.5};
}
static draw2d_pixel_t sample(const draw2d_image_t *image,double x,double y)
{
    const os64_geometry_map_t *m=&image->device_to_source;
    double sx=m->a*x+m->c*y+m->tx,sy=m->b*x+m->d*y+m->ty;
    /* Map admission bounds the complete target, including uncovered pixels. */
    if(!image->smoothing)return image_pixel(&image->surface,floor(sx),floor(sy));
    sx-=.5;sy-=.5;
    double ix=floor(sx),iy=floor(sy);
    draw2d_pixel_t top=interpolate(image_pixel(&image->surface,ix,iy),image_pixel(&image->surface,ix+1,iy),sx-ix);
    draw2d_pixel_t bottom=interpolate(image_pixel(&image->surface,ix,iy+1),image_pixel(&image->surface,ix+1,iy+1),sx-ix);
    return interpolate(top,bottom,sy-iy);
}
static draw2d_status_t paint_mask(const draw2d_surface_t *s,const uint8_t *coverage,
    const uint8_t *clip,size_t length,draw2d_solid_t source,draw2d_composite_t op,
    uint64_t max_pixels,draw2d_damage_t *damage,const draw2d_image_t *image)
{
    if(damage)*damage=(draw2d_damage_t){0};
    draw2d_status_t status=draw2d_surface_validate(s);
    if(status!=DRAW2D_OK)return status;
    if(s->format==DRAW2D_ARGB8_STRAIGHT || op.operation<DRAW2D_SOURCE_OVER || op.operation>DRAW2D_LIGHTER)
        return DRAW2D_UNSUPPORTED;
    uint64_t count=(uint64_t)s->width*s->height;
    if(count>length || count>max_pixels)return DRAW2D_LIMIT;
    if(count && !coverage)return DRAW2D_BAD_ARGUMENT;
    for(uint32_t y=0;y<s->height;++y) {
        unsigned char *row=(unsigned char *)s->pixels+(size_t)y*s->stride;
        for(uint32_t x=0;x<s->width;++x) {
            size_t at=(size_t)y*s->width+x;
            uint16_t cv=(uint16_t)coverage[at]*257,cl=clip?(uint16_t)clip[at]*257:65535;
            if(!cl)continue;
            draw2d_pixel_t d=s->format==DRAW2D_ARGB16_PREMULTIPLIED?((draw2d_pixel_t *)row)[x]:draw2d_unpack(((uint32_t *)row)[x]);
            draw2d_pixel_t r=op.operation==DRAW2D_CLEAR?draw2d_scale(d,65535-cv):
                composite_pixel(image ? draw2d_scale(draw2d_scale(sample(image,x+.5,y+.5),op.opacity),cv) :
                    draw2d_color(source.argb,op.opacity,cv),d,op.operation);
            if(cl!=65535)r=sum(draw2d_scale(r,cl),draw2d_scale(d,65535-cl));
            if(s->format==DRAW2D_ARGB16_PREMULTIPLIED)((draw2d_pixel_t *)row)[x]=r;
            else ((uint32_t *)row)[x]=draw2d_pack(r);
        }
    }
    if(damage && count)*damage=(draw2d_damage_t){0,0,s->width,s->height};
    return DRAW2D_OK;
}

draw2d_status_t draw2d_paint_mask(const draw2d_surface_t *s,const uint8_t *coverage,
    const uint8_t *clip,size_t length,draw2d_solid_t source,draw2d_composite_t op,
    uint64_t max_pixels,draw2d_damage_t *damage)
{ return paint_mask(s,coverage,clip,length,source,op,max_pixels,damage,NULL); }
draw2d_status_t draw2d_paint_image(const draw2d_surface_t *s,const uint8_t *coverage,
    const uint8_t *clip,size_t length,const draw2d_image_t *image,draw2d_composite_t op,
    uint64_t max_pixels,draw2d_damage_t *damage)
{
    if(damage)*damage=(draw2d_damage_t){0};
    if(!s||!image)return DRAW2D_BAD_ARGUMENT;
    draw2d_status_t status=draw2d_surface_validate(&image->surface);
    if(status!=DRAW2D_OK)return status;
    if(!image->surface.width||!image->surface.height)return DRAW2D_BAD_ARGUMENT;
    const os64_geometry_map_t *m=&image->device_to_source;
    if(!isfinite(m->a)||!isfinite(m->b)||!isfinite(m->c)||!isfinite(m->d)||
       !isfinite(m->tx)||!isfinite(m->ty))return DRAW2D_BAD_ARGUMENT;
    double bx=fabs(m->a)*s->width+fabs(m->c)*s->height+fabs(m->tx);
    double by=fabs(m->b)*s->width+fabs(m->d)*s->height+fabs(m->ty);
    if(!isfinite(bx)||!isfinite(by))return DRAW2D_LIMIT;
    return paint_mask(s,coverage,clip,length,(draw2d_solid_t){0},op,max_pixels,damage,image);
}
