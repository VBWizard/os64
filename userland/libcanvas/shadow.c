#include "internal.h"
#include <draw2d/blur.h>

typedef struct { double x,y; uint32_t w,h; } Area;
typedef struct { Area area; draw2d_pixel_t *pixels; size_t bytes; } Patch;
typedef struct { const draw2d_shader_t *source; double x,y; } Shift;
static uint32_t shifted_color(void *arg,double x,double y)
{
    Shift *s=arg;
    return s->source->sample(s->source->context,x+s->x,y+s->y);
}
static canvas_status_t area(canvas_context_t *c,os64_geometry_bounds_t bounds,
                             double x0,double y0,double x1,double y1,Area *out)
{
    *out=(Area){0};
    if(bounds.empty)return CANVAS_OK;
    if(!isfinite(bounds.x_min)||!isfinite(bounds.y_min)||!isfinite(bounds.x_max)||!isfinite(bounds.y_max))
        return CANVAS_LIMIT;
    x0=floor(fmax(x0,bounds.x_min-2));y0=floor(fmax(y0,bounds.y_min-2));
    x1=ceil(fmin(x1,bounds.x_max+2));y1=ceil(fmin(y1,bounds.y_max+2));
    if(x1<=x0||y1<=y0)return CANVAS_OK;
    double w=x1-x0,h=y1-y0;
    if(!isfinite(w)||!isfinite(h)||w>UINT32_MAX||h>UINT32_MAX||w*h>c->options.max_draw_pixels)
        return CANVAS_LIMIT;
    *out=(Area){x0,y0,(uint32_t)w,(uint32_t)h};return CANVAS_OK;
}
/* Rasterize before clipping, in the original device pixel grid. Source alpha
 * includes paint/image alpha and globalAlpha, including off-bitmap geometry. */
static canvas_status_t patch(canvas_context_t *c,os64_geometry_context_t *geometry,
    const os64_geometry_outline_t *outline,const os64_geometry_map_t *map,bool even_odd,
    Area a,draw2d_solid_t solid,const draw2d_shader_t *shader,const draw2d_image_t *image,Patch *out)
{
    *out=(Patch){.area=a};
    size_t n=(size_t)a.w*a.h;if(!n)return CANVAS_OK;
    if(n>SIZE_MAX/sizeof(draw2d_pixel_t))return CANVAS_LIMIT;
    uint8_t *coverage=NULL;
    canvas_status_t status=c_allocate(c,n,(void **)&coverage);
    if(status!=CANVAS_OK)return status;
    out->bytes=n*sizeof(draw2d_pixel_t);
    status=c_allocate(c,out->bytes,(void **)&out->pixels);
    if(status==CANVAS_OK) {
        for(size_t i=0;i<n;i++){coverage[i]=0;out->pixels[i]=(draw2d_pixel_t){0};}
        os64_geometry_target_t target={a.x,a.y,a.w,a.h,a.w,n,coverage,even_odd};
        if(outline)status=c_geometry_status(c->options.geometry->raster(geometry,outline,map,&target));
    }
    if(status==CANVAS_OK) {
        draw2d_surface_t surface={out->pixels,a.w,a.h,(size_t)a.w*sizeof(draw2d_pixel_t),out->bytes,DRAW2D_ARGB16_PREMULTIPLIED};
        draw2d_composite_t op={DRAW2D_SOURCE_OVER,(uint16_t)(c->state.global_alpha*65535+.5)};
        draw2d_status_t result;
        if(image) {
            draw2d_image_t source=*image;
            source.device_to_source.tx+=a.x*source.device_to_source.a+a.y*source.device_to_source.c;
            source.device_to_source.ty+=a.x*source.device_to_source.b+a.y*source.device_to_source.d;
            result=draw2d_paint_image(&surface,coverage,NULL,n,&source,op,c->options.max_draw_pixels,NULL);
        } else if(shader) {
            Shift shift={shader,a.x,a.y};draw2d_shader_t source={&shift,shifted_color};
            result=draw2d_paint_shader(&surface,coverage,NULL,n,&source,op,c->options.max_draw_pixels,NULL);
        } else result=draw2d_paint_mask(&surface,coverage,NULL,n,solid,op,c->options.max_draw_pixels,NULL);
        status=result==DRAW2D_OK?CANVAS_OK:result==DRAW2D_LIMIT?CANVAS_LIMIT:CANVAS_BAD_ARGUMENT;
    }
    c_release(c,coverage,n);return status;
}
static uint16_t alpha_at(const uint16_t *mask,Area a,double x,double y)
{
    if(x<0||y<0||x>=a.w||y>=a.h)return 0;
    return mask[(size_t)y*a.w+(size_t)x];
}
static uint16_t shifted_alpha(const uint16_t *mask,Area a,double x,double y)
{
    if(!mask)return 0;
    x-=a.x;y-=a.y;
    double ix=floor(x),iy=floor(y),fx=x-ix,fy=y-iy;
    double top=alpha_at(mask,a,ix,iy)*(1-fx)+alpha_at(mask,a,ix+1,iy)*fx;
    double bottom=alpha_at(mask,a,ix,iy+1)*(1-fx)+alpha_at(mask,a,ix+1,iy+1)*fx;
    return (uint16_t)(top*(1-fy)+bottom*fy+.5);
}
static draw2d_pixel_t source_at(const Patch *p,uint32_t x,uint32_t y)
{
    double px=x-p->area.x,py=y-p->area.y;
    if(px<0||py<0||px>=p->area.w||py>=p->area.h)return (draw2d_pixel_t){0};
    return p->pixels[(size_t)py*p->area.w+(size_t)px];
}

canvas_status_t c_shadow_paint(canvas_context_t *c,os64_geometry_context_t *geometry,
    const os64_geometry_outline_t *outline,const os64_geometry_map_t *map,bool even_odd,
    draw2d_solid_t solid,const draw2d_shader_t *shader,const draw2d_image_t *image)
{
    const os64_geometry_backend_t *b=c->options.geometry;
    if(!b->bounds)return CANVAS_UNSUPPORTED;
    os64_geometry_bounds_t bounds={.empty=true};
    canvas_status_t status=outline?c_geometry_status(b->bounds(geometry,outline,map,0,0,&bounds)):CANVAS_OK;
    if(status!=CANVAS_OK)return status;
    double dx=c->state.shadow_x,dy=c->state.shadow_y;
    /* Three radius-r box passes have variance r*(r+1). Interpolate adjacent
     * results to match sigma squared, including fractional blur. This bounded
     * Gaussian approximation shares CSS's integer kernel; sigma is capped at
     * 64 device pixels while the saved shadowBlur value remains unchanged. */
    double sigma=fmin(c->state.shadow_blur/2,64);
    uint32_t radius=(uint32_t)floor((sqrt(1+4*sigma*sigma)-1)/2);
    double mix=(sigma*sigma-radius*(radius+1))/(2*(radius+1));
    double reach=3*(radius+(mix>0))+1;
    Area visible,behind;
    status=area(c,bounds,0,0,c->surface.width,c->surface.height,&visible);
    if(status==CANVAS_OK)status=area(c,bounds,-dx-reach,-dy-reach,
        c->surface.width-dx+reach,c->surface.height-dy+reach,&behind);
    if(status!=CANVAS_OK)return status;
    // The blur needs transparent padding beyond the shape, not just its
    // original alpha bounds. Keep it bounded by the inverse visible region.
    if(behind.w&&behind.h) {
        double x0=floor(fmax(-dx-reach,behind.x-reach));
        double y0=floor(fmax(-dy-reach,behind.y-reach));
        double x1=ceil(fmin(c->surface.width-dx+reach,behind.x+behind.w+reach));
        double y1=ceil(fmin(c->surface.height-dy+reach,behind.y+behind.h+reach));
        double w=x1-x0,h=y1-y0;
        if(w>UINT32_MAX||h>UINT32_MAX||w*h>c->options.max_draw_pixels)return CANVAS_LIMIT;
        behind=(Area){x0,y0,(uint32_t)w,(uint32_t)h};
    }
    Patch foreground={0},cast={0};uint16_t *mask=NULL,*upper=NULL,*scratch=NULL;
    size_t n=(size_t)behind.w*behind.h,bytes=n*sizeof(uint16_t);
    size_t scratch_bytes=(size_t)(behind.w>behind.h?behind.w:behind.h)*sizeof(uint16_t);
    if(n>SIZE_MAX/sizeof(uint16_t))return CANVAS_LIMIT;
    status=patch(c,geometry,outline,map,even_odd,visible,solid,shader,image,&foreground);
    if(status==CANVAS_OK&&n)status=patch(c,geometry,outline,map,even_odd,behind,solid,shader,image,&cast);
    if(status==CANVAS_OK&&n)status=c_allocate(c,bytes,(void **)&mask);
    if(status==CANVAS_OK&&n)for(size_t i=0;i<n;i++)mask[i]=cast.pixels[i].a;
    c_release(c,cast.pixels,cast.bytes);
    if(status==CANVAS_OK&&n&&(radius||mix>0)) {
        status=c_allocate(c,scratch_bytes,(void **)&scratch);
        if(status==CANVAS_OK&&mix>0)status=c_allocate(c,bytes,(void **)&upper);
        if(status==CANVAS_OK) {
            if(upper)for(size_t i=0;i<n;i++)upper[i]=mask[i];
            draw2d_mask16_t m={mask,behind.w,behind.h,(size_t)behind.w*2,bytes};
            if(draw2d_blur_mask16(&m,radius,0,scratch,scratch_bytes,c->options.max_draw_pixels)!=DRAW2D_OK)status=CANVAS_LIMIT;
            if(status==CANVAS_OK&&upper) {
                m.pixels=upper;
                if(draw2d_blur_mask16(&m,radius+1,0,scratch,scratch_bytes,c->options.max_draw_pixels)!=DRAW2D_OK)status=CANVAS_LIMIT;
                else for(size_t i=0;i<n;i++)mask[i]=(uint16_t)(mask[i]*(1-mix)+upper[i]*mix+.5);
            }
        }
    }
    if(status==CANVAS_OK) {
        // Allocation, rasterization and source sampling have completed.
        // Composite shadow then source through the final clip, rounding at
        // each write to the unorm8 bitmap. No fallible operation follows.
        draw2d_damage_t damage={c->surface.width,c->surface.height,0,0};
        uint16_t opacity=(uint16_t)(c->state.shadow_alpha*65535+.5);
        for(uint32_t y=0;y<c->surface.height;y++)for(uint32_t x=0;x<c->surface.width;x++) {
            size_t at=(size_t)y*c->surface.width+x;
            uint16_t clip=c->clip?c->clip->pixels[at]*257:65535;if(!clip)continue;
            uint32_t *pixel=(uint32_t *)((uint8_t *)c->surface.pixels+(size_t)y*c->surface.stride)+x;
            draw2d_pixel_t shadow=draw2d_color(0xff000000|(c->state.shadow_argb&0xffffff),opacity,
                shifted_alpha(mask,behind,x-dx,y-dy));
            uint32_t v=draw2d_pack(draw2d_blend_pixel(shadow,draw2d_unpack(*pixel),c->state.composite,clip));
            v=draw2d_pack(draw2d_blend_pixel(source_at(&foreground,x,y),draw2d_unpack(v),c->state.composite,clip));
            if(v==*pixel)continue;
            *pixel=v;if(x<damage.x0)damage.x0=x;if(y<damage.y0)damage.y0=y;
            if(x+1>damage.x1)damage.x1=x+1;
            if(y+1>damage.y1)damage.y1=y+1;
        }
        if(damage.x1>damage.x0&&damage.y1>damage.y0)c_damage_union(c,damage);
    }
    c_release(c,scratch,scratch_bytes);c_release(c,upper,bytes);c_release(c,mask,bytes);
    c_release(c,foreground.pixels,foreground.bytes);
    return status;
}
