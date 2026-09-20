#include "text_internal.h"
#include "os64/text_draw.h"

os64_font_status_t os64_text_draw(const os64_text_run_t *r, os64_gui_surface_t *s,
    os64_gui_rect_t clip, int32_t bx, int32_t by, uint32_t color)
{
    if (!r || !s || !s->pixels || !s->width || !s->height ||
        s->width>INT32_MAX || s->height>INT32_MAX || s->pitch_px<s->width ||
        (size_t)s->pitch_px>SIZE_MAX/sizeof(uint32_t)/s->height) return OS64_FONT_BAD_ARGUMENT;
    int64_t x0=clip.x>0?clip.x:0,y0=clip.y>0?clip.y:0;
    int64_t x1=(int64_t)clip.x+clip.w,y1=(int64_t)clip.y+clip.h;
    if (x1>s->width) x1=s->width;
    if (y1>s->height) y1=s->height;
    if (clip.w<=0 || clip.h<=0 || x0>=x1 || y0>=y1) return OS64_FONT_OK;
    /* Validate the complete translation before touching the first pixel. */
    for (size_t n=0;n<r->view.glyph_count;n++) {
        const os64_text_placement_t *p=&r->view.glyphs[n];
        const os64_font_glyph_view_t *g=&r->images[n]->view;
        int64_t x=(int64_t)bx+text_pixel(p->x)+g->left;
        int64_t y=(int64_t)by+text_pixel(p->y)+g->top;
        if (x<INT32_MIN || y<INT32_MIN || x+g->width>INT32_MAX || y+g->height>INT32_MAX)
            return OS64_FONT_LIMIT;
        if (r->cell_advance && ((int64_t)bx+text_pixel((int64_t)p->x+r->cell_advance)>INT32_MAX ||
            (int64_t)by+text_floor(-(int64_t)r->cell_ascent,64)<INT32_MIN ||
            (int64_t)by-text_floor(-((int64_t)r->cell_height-r->cell_ascent),64)>INT32_MAX))
            return OS64_FONT_LIMIT;
    }
    for (size_t n=0;n<r->view.glyph_count;n++) {
        const os64_text_placement_t *p=&r->view.glyphs[n];
        const os64_font_glyph_view_t *g=&r->images[n]->view;
        int64_t gx=(int64_t)bx+text_pixel(p->x)+g->left;
        int64_t gy=(int64_t)by+text_pixel(p->y)+g->top;
        int64_t left=gx>x0?gx:x0,top=gy>y0?gy:y0;
        int64_t right=gx+g->width<x1?gx+g->width:x1,bottom=gy+g->height<y1?gy+g->height:y1;
        if (r->cell_advance) {
            int64_t a=(int64_t)bx+text_pixel(p->x),b=(int64_t)bx+text_pixel((int64_t)p->x+r->cell_advance);
            int64_t t=(int64_t)by+text_floor(-(int64_t)r->cell_ascent,64);
            int64_t u=(int64_t)by-text_floor(-((int64_t)r->cell_height-r->cell_ascent),64);
            if (left<a) left=a;
            if (right>b) right=b;
            if (top<t) top=t;
            if (bottom>u) bottom=u;
        }
        for (int64_t y=top;y<bottom;y++) for (int64_t x=left;x<right;x++) {
            uint32_t a=g->coverage[(size_t)(y-gy)*g->stride+(size_t)(x-gx)];
            if (!a) continue;
            uint32_t *pixel=&s->pixels[(size_t)y*s->pitch_px+(size_t)x];
            uint32_t dest=*pixel,value=0xff000000u;
            for (unsigned shift=0;shift<24;shift+=8)
                value|=((a*((color>>shift)&255)+(255-a)*((dest>>shift)&255)+127)/255)<<shift;
            *pixel=value;
        }
    }
    return OS64_FONT_OK;
}
