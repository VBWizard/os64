#include "text_internal.h"
#include "os64/str.h"
#include "os64/charset.h"

static void include_metrics(os64_text_run_t *r, const os64_font_face_info_t *info)
{
    if (info->ascent>r->view.ascent) r->view.ascent=info->ascent;
    if (info->descent>r->view.descent) r->view.descent=info->descent;
    if (info->line_height>r->view.line_height) r->view.line_height=info->line_height;
}
/* Whether `f` answers for `scalar`: every font does that was opened with
 * no ranges. */
static bool covers(const os64_text_font_t *f, uint32_t scalar)
{
    if (!f->range_count) return true;
    size_t lo=0,hi=f->range_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (f->ranges[mid].last<scalar) lo=mid+1;else hi=mid;
    }
    return lo<f->range_count && f->ranges[lo].first<=scalar;
}
static os64_font_status_t resolve(os64_text_run_t *r, uint32_t scalar,
    os64_text_font_t **chosen, uint32_t *index, text_image **image)
{
    os64_text_context_t *c=r->context;
    os64_text_font_t *primary=r->fonts[0];
    uint32_t w=8,h=16;int32_t top=-12;
    if (r->cell_advance) {
        int64_t cw=((int64_t)r->cell_advance+63)/64;
        int64_t ch=((int64_t)r->cell_height+63)/64;
        if (cw>OS64_FONT_MASK_DIM_MAX || ch>OS64_FONT_MASK_DIM_MAX || !ch)
            return OS64_FONT_LIMIT;
        w=(uint32_t)cw;h=(uint32_t)ch;top=(int32_t)text_pixel(-(int64_t)r->cell_ascent);
    }
    if (scalar!=TEXT_MARKER) {
        for (size_t n=0;n<r->font_count;n++) {
            os64_text_font_t *f=r->fonts[n];
            os64_font_status_t status;
            if (!covers(f,scalar)) {
                status=OS64_FONT_MISSING;
            } else if (f->face) {
                for (unsigned attempt=0;;attempt++) {
                    c->refusal=OS64_FONT_OK;
                    status=c->backend->lookup(f->face,scalar,index);
                    if (!attempt && status!=OS64_FONT_OK && text_retry(c,status)) continue;
                    status=text_status(c,status);break;
                }
                if (status==OS64_FONT_OK) status=text_image_get(f,*index,0,0,0,image);
            } else {
                *index=scalar;
                if (r->encoding==OS64_TEXT_CP437) {
                    for (uint32_t b=0;b<256;b++)
                        if (os64_cp437_codepoint((uint8_t)b)==scalar) {*index=TEXT_CP437_BASE+b;break;}
                }
                if (r->cell_advance && scalar>=0x2500 && scalar<=0x259f && !text_bitmap_rows(scalar))
                    status=OS64_FONT_MISSING;
                else status=text_bitmap_rows(*index)?text_image_get(f,*index,8,16,-12,image):OS64_FONT_MISSING;
            }
            if (status==OS64_FONT_OK) {
                if ((*image)->view.advance_x>0 || r->cell_advance) {*chosen=f;return status;}
                /* Standalone zero advance has no invertible caret interval. */
                text_image_unpin(*image);*image=NULL;scalar=TEXT_MARKER;break;
            }
            if (status!=OS64_FONT_MISSING && status!=OS64_FONT_UNSUPPORTED) return text_status(c,status);
            if (n==0 && r->cell_advance && scalar>=0x2500 && scalar<=0x259f && covers(primary,scalar)) {
                *chosen=primary;*index=0;
                return text_image_get(primary,scalar,w,h,top,image);
            }
        }
    }
    *chosen=NULL;*index=0;
    return text_image_get(primary,TEXT_MARKER,w,h,top,image);
}
static os64_font_status_t add_glyph(os64_text_run_t *r, uint32_t scalar,
    size_t begin, size_t end, int64_t *pen, size_t caret_index,
    os64_text_font_t **previous, uint32_t *previous_index)
{
    os64_text_context_t *c=r->context;
    text_image *image=NULL;os64_text_font_t *font=NULL;uint32_t index=0;
    os64_font_status_t status=resolve(r,scalar,&font,&index,&image);
    if (status!=OS64_FONT_OK) return status;
    os64_font_pos_t advance=r->cell_advance?r->cell_advance:image->view.advance_x;
    if (!r->cell_advance && font && font->face && font==*previous && *previous_index && index) {
        os64_font_pos_t delta=0;
        for (unsigned attempt=0;;attempt++) {
            c->refusal=OS64_FONT_OK;
            status=c->backend->pair_adjust(font->face,*previous_index,index,&delta);
            if (!attempt && status!=OS64_FONT_OK && text_retry(c,status)) continue;
            status=text_status(c,status);break;
        }
        if (status!=OS64_FONT_OK) {text_image_unpin(image);return status;}
        os64_text_caret_t *carets=(os64_text_caret_t *)r->view.carets;
        if (caret_index && *pen+delta>carets[caret_index-1].x) {
            *pen+=delta;
            if (*pen>INT32_MAX) {text_image_unpin(image);return OS64_FONT_LIMIT;}
            carets[caret_index].x=(int32_t)*pen;
        }
    }
    int64_t next=*pen+advance;
    if (*pen<0 || next>INT32_MAX || next<=*pen) {
        text_image_unpin(image);return OS64_FONT_LIMIT;
    }
    os64_font_rect_t ink={0};
    if (image->view.width && image->view.height) {
        int64_t x0=*pen+image->view.ink.x0,x1=*pen+image->view.ink.x1;
        if (x0<INT32_MIN || x1>INT32_MAX) {text_image_unpin(image);return OS64_FONT_LIMIT;}
        ink=(os64_font_rect_t){(int32_t)x0,image->view.ink.y0,(int32_t)x1,image->view.ink.y1};
        if (!r->view.ink.x0 && !r->view.ink.y0 && !r->view.ink.x1 && !r->view.ink.y1) r->view.ink=ink;
        else {
            if (ink.x0<r->view.ink.x0) r->view.ink.x0=ink.x0;
            if (ink.y0<r->view.ink.y0) r->view.ink.y0=ink.y0;
            if (ink.x1>r->view.ink.x1) r->view.ink.x1=ink.x1;
            if (ink.y1>r->view.ink.y1) r->view.ink.y1=ink.y1;
        }
    }
    size_t at=r->view.glyph_count++;
    os64_text_placement_t *glyphs=(os64_text_placement_t *)r->view.glyphs;
    glyphs[at]=(os64_text_placement_t){.byte_begin=begin,.byte_end=end,
        .font_identity=font?font->identity:0,.glyph_index=index,
        .x=(int32_t)*pen,.advance_x=advance,.ink=ink};
    r->images[at]=image;
    if (font) include_metrics(r,&font->info);
    else {
        os64_font_face_info_t marker={.ascent=768,.descent=256,.line_height=1024};
        if (r->cell_advance) marker= r->fonts[0]->info;
        include_metrics(r,&marker);
    }
    *previous=font;*previous_index=index;*pen=next;return OS64_FONT_OK;
}
os64_font_status_t os64_text_layout(os64_text_context_t *c, const uint8_t *bytes,
    size_t length, const os64_text_layout_t *o, os64_text_run_t **out)
{
    if (out) *out=NULL;
    if (!c || !out || !o || (!bytes && length) || !o->fonts || !o->font_count ||
        o->font_count>OS64_TEXT_FALLBACK_MAX || o->encoding>OS64_TEXT_CP437 ||
        o->encoding<OS64_TEXT_UTF8_WESTERN_V1 || o->tab_interval<=0 || o->cell_advance<0 ||
        (o->cell_advance && o->encoding==OS64_TEXT_UTF8_WESTERN_V1) ||
        (o->cell_advance && o->letter_spacing)) return OS64_FONT_BAD_ARGUMENT;
    if (length>OS64_TEXT_BYTES_MAX) return OS64_FONT_LIMIT;
    for (size_t n=0;n<o->font_count;n++) {
        if (!o->fonts[n] || o->fonts[n]->context!=c) return OS64_FONT_BAD_ARGUMENT;
        for (size_t j=0;j<n;j++) if (o->fonts[n]==o->fonts[j]) return OS64_FONT_BAD_ARGUMENT;
    }
    for (size_t n=0;n<length;n++) if (bytes[n]==10) return OS64_FONT_BAD_ARGUMENT;
    size_t clusters=0,count=0;
    for (size_t at=0;at<length;) {
        text_cluster d=text_decode(bytes,length,at,o->encoding,o->cell_advance!=0);
        count+=(d.scalar==TEXT_TAB?0:1)+(d.extra_marker?1:0);clusters++;at=d.end;
        if (count>OS64_TEXT_GLYPHS_MAX) return OS64_FONT_LIMIT;
    }
    if (count>SIZE_MAX/sizeof(os64_text_placement_t) || count>SIZE_MAX/sizeof(text_image *) ||
        clusters>=SIZE_MAX/sizeof(os64_text_caret_t)) return OS64_FONT_LIMIT;
    if (o->cell_advance && count>(size_t)INT32_MAX/(uint32_t)o->cell_advance)
        return OS64_FONT_LIMIT;
    // The run and these buffers share one lifetime, including retained runs.
    // One allocation avoids repeated heap searches on large page rebuilds.
    _Static_assert(sizeof(os64_text_run_t)%_Alignof(os64_text_placement_t)==0 &&
        sizeof(os64_text_placement_t)%_Alignof(text_image *)==0 &&
        sizeof(text_image *)%_Alignof(os64_text_caret_t)==0,
        "run buffer boundaries must preserve alignment");
    size_t glyph_bytes=count*sizeof(os64_text_placement_t);
    size_t image_bytes=count*sizeof(text_image *);
    size_t caret_bytes=(clusters+1)*sizeof(os64_text_caret_t);
    size_t total=sizeof(os64_text_run_t);
    if (glyph_bytes>SIZE_MAX-total) return OS64_FONT_LIMIT;
    total+=glyph_bytes;
    if (image_bytes>SIZE_MAX-total) return OS64_FONT_LIMIT;
    total+=image_bytes;
    if (caret_bytes>SIZE_MAX-total) return OS64_FONT_LIMIT;
    total+=caret_bytes;
    if (length>SIZE_MAX-total) return OS64_FONT_LIMIT;
    total+=length;
    os64_text_run_t *r=text_alloc(c,total);
    if (!r) return c->refusal;
    *r=(os64_text_run_t){.context=c,.refs=1,.encoding=o->encoding,.cell_advance=o->cell_advance,
        .cell_ascent=o->fonts[0]->info.ascent,.cell_height=o->fonts[0]->info.line_height};
    uint8_t *storage=(uint8_t *)(r+1);
    r->view.glyphs=count?(const void *)storage:NULL;storage+=glyph_bytes;
    r->images=count?(void *)storage:NULL;storage+=image_bytes;
    r->view.carets=(const void *)storage;storage+=caret_bytes;
    r->view.bytes=length?storage:NULL;
    c->runs++;
    os64_font_status_t status=OS64_FONT_OK;
    for (size_t n=0;n<o->font_count;n++) {r->fonts[n]=o->fonts[n];r->fonts[n]->refs++;r->font_count++;}
    include_metrics(r,&r->fonts[0]->info);
    if (length) os64_memcpy((void *)r->view.bytes,bytes,length);
    r->view.byte_count=length;
    os64_text_caret_t *carets=(os64_text_caret_t *)r->view.carets;
    carets[0]=(os64_text_caret_t){0,0};r->view.caret_count=1;
    int64_t pen=0;os64_text_font_t *previous=NULL;uint32_t previous_index=0;
    for (size_t at=0;at<length;) {
        text_cluster d=text_decode(bytes,length,at,o->encoding,o->cell_advance!=0);
        if (d.scalar==TEXT_TAB) {
            pen=(int64_t)o->tab_origin+(text_floor(pen-o->tab_origin,o->tab_interval)+1)*o->tab_interval;
            if (pen>INT32_MAX) {status=OS64_FONT_LIMIT;goto fail;}
            previous=NULL;previous_index=0;
        } else {
            status=add_glyph(r,d.scalar,at,d.end,&pen,r->view.caret_count-1,&previous,&previous_index);
            if (status!=OS64_FONT_OK) goto fail;
            if (d.extra_marker) {
                previous=NULL;previous_index=0;
                status=add_glyph(r,TEXT_MARKER,at,d.end,&pen,r->view.caret_count-1,&previous,&previous_index);
                if (status!=OS64_FONT_OK) goto fail;
            }
            if (o->letter_spacing) {
                int64_t from=carets[r->view.caret_count-1].x;
                pen+=o->letter_spacing;
                if (pen<from) pen=from;
                if (pen>INT32_MAX) {status=OS64_FONT_LIMIT;goto fail;}
            }
        }
        carets[r->view.caret_count++]=(os64_text_caret_t){d.end,(int32_t)pen};at=d.end;
    }
    if ((int64_t)r->view.ascent+r->view.descent>INT32_MAX) {status=OS64_FONT_LIMIT;goto fail;}
    if (r->view.line_height<r->view.ascent+r->view.descent)
        r->view.line_height=r->view.ascent+r->view.descent;
    r->view.advance_x=(int32_t)pen;*out=r;text_trim(c);return OS64_FONT_OK;
fail:
    os64_text_run_release(r);return status;
}
os64_font_status_t os64_text_run_view(const os64_text_run_t *r, os64_text_run_view_t *out)
{
    if (out) *out=(os64_text_run_view_t){0};
    if (!r || !out) return OS64_FONT_BAD_ARGUMENT;
    *out=r->view;return OS64_FONT_OK;
}
os64_font_status_t os64_text_run_retain(os64_text_run_t *r)
{
    if (!r) return OS64_FONT_BAD_ARGUMENT;
    if (r->refs==SIZE_MAX) return OS64_FONT_LIMIT;
    r->refs++;return OS64_FONT_OK;
}
void os64_text_run_release(os64_text_run_t *r)
{
    if (!r || --r->refs) return;
    os64_text_context_t *c=r->context;
    for (size_t n=0;n<r->view.glyph_count;n++) text_image_unpin(r->images[n]);
    for (size_t n=0;n<r->font_count;n++) os64_text_font_release(r->fonts[n]);
    c->runs--;text_free(c,r);text_trim(c);
}
static size_t boundary(const os64_text_run_t *r, size_t byte)
{
    size_t lo=0,hi=r->view.caret_count-1;
    while (lo<hi) {size_t mid=lo+(hi-lo)/2;if (r->view.carets[mid].byte_offset<byte) lo=mid+1;else hi=mid;}
    return lo;
}
os64_font_status_t os64_text_caret(const os64_text_run_t *r, size_t byte,
    os64_text_bias_t bias, os64_text_caret_t *out)
{
    if (out) *out=(os64_text_caret_t){0};
    if (!r || !out || byte>r->view.byte_count || (bias!=OS64_TEXT_BEFORE && bias!=OS64_TEXT_AFTER))
        return OS64_FONT_BAD_ARGUMENT;
    size_t n=boundary(r,byte);
    if (r->view.carets[n].byte_offset!=byte && bias==OS64_TEXT_BEFORE) n--;
    *out=r->view.carets[n];return OS64_FONT_OK;
}
os64_font_status_t os64_text_hit(const os64_text_run_t *r, os64_font_pos_t x, os64_text_caret_t *out)
{
    if (out) *out=(os64_text_caret_t){0};
    if (!r || !out) return OS64_FONT_BAD_ARGUMENT;
    size_t lo=0,hi=r->view.caret_count-1;
    while (lo<hi) {size_t mid=lo+(hi-lo)/2;if (r->view.carets[mid].x<x) lo=mid+1;else hi=mid;}
    if (lo && (int64_t)x-r->view.carets[lo-1].x<(int64_t)r->view.carets[lo].x-x) lo--;
    *out=r->view.carets[lo];return OS64_FONT_OK;
}
os64_font_status_t os64_text_fit(const os64_text_run_t *r, os64_font_pos_t width, size_t *out)
{
    if (out) *out=0;
    if (!r || !out || width<0) return OS64_FONT_BAD_ARGUMENT;
    size_t lo=0,hi=r->view.caret_count-1;
    while (lo<hi) {size_t mid=lo+(hi-lo+1)/2;if (r->view.carets[mid].x<=width) lo=mid;else hi=mid-1;}
    *out=r->view.carets[lo].byte_offset;return OS64_FONT_OK;
}
os64_font_status_t os64_text_selection(const os64_text_run_t *r, size_t begin,
    size_t end, os64_font_rect_t *out)
{
    if (out) *out=(os64_font_rect_t){0};
    if (!r || !out || begin>end || end>r->view.byte_count) return OS64_FONT_BAD_ARGUMENT;
    size_t a=boundary(r,begin),b=boundary(r,end);
    if (r->view.carets[a].byte_offset!=begin || r->view.carets[b].byte_offset!=end)
        return OS64_FONT_BAD_ARGUMENT;
    if (begin!=end) *out=(os64_font_rect_t){r->view.carets[a].x,-r->view.ascent,
        r->view.carets[b].x,r->view.line_height-r->view.ascent};
    return OS64_FONT_OK;
}

/* Both markup and script clients consume positioned contours through this
 * boundary; glyph identity, fallback and run lifetime stay in the text layer. */
typedef struct {
    os64_font_outline_emit_t emit;
    void *context;
    size_t count,limit;
    double x,y;
} TextOutline;
static os64_font_status_t text_outline_emit(void *opaque,const os64_geometry_command_t *input)
{
    TextOutline *v=opaque;
    if(v->count==v->limit)return OS64_FONT_LIMIT;
    ++v->count;
    os64_geometry_command_t command=*input;
    unsigned n=command.verb==OS64_GEOMETRY_CLOSE?0:command.verb==OS64_GEOMETRY_CUBIC?3:command.verb==OS64_GEOMETRY_QUADRATIC?2:1;
    for(unsigned i=0;i<n;++i){command.p[i].x+=v->x;command.p[i].y+=v->y;}
    return v->emit?v->emit(v->context,&command):OS64_FONT_OK;
}
static void text_bounds_union(os64_geometry_bounds_t *a,os64_geometry_bounds_t b)
{
    if(b.empty)return;
    if(a->empty){*a=b;return;}
    if(b.x_min<a->x_min)a->x_min=b.x_min;
    if(b.y_min<a->y_min)a->y_min=b.y_min;
    if(b.x_max>a->x_max)a->x_max=b.x_max;
    if(b.y_max>a->y_max)a->y_max=b.y_max;
}
os64_font_status_t os64_text_run_outline(const os64_text_run_t *run,size_t limit,
    os64_font_outline_emit_t emit,void *opaque,os64_geometry_bounds_t *out)
{
    if(out)*out=(os64_geometry_bounds_t){.empty=true};
    if(!run||!out)return OS64_FONT_BAD_ARGUMENT;
    TextOutline v={.emit=emit,.context=opaque,.limit=limit};
    os64_geometry_bounds_t bounds={.empty=true};
    const os64_font_backend_t *backend=run->context->backend;
    for(size_t i=0;i<run->view.glyph_count;++i) {
        const os64_text_placement_t *g=&run->view.glyphs[i];
        v.x=g->x/64.0;v.y=g->y/64.0;
        os64_text_font_t *font=NULL;
        for(size_t f=0;f<run->font_count;++f)if(run->fonts[f]->identity==g->font_identity){font=run->fonts[f];break;}
        if(font&&font->face) {
            if(backend->struct_size<offsetof(os64_font_backend_t,outline)+sizeof(backend->outline)||!backend->outline)return OS64_FONT_UNSUPPORTED;
            os64_geometry_bounds_t b;
            os64_font_status_t status=backend->outline(font->face,g->glyph_index,limit-v.count,text_outline_emit,&v,&b);
            if(status!=OS64_FONT_OK)return status;
            b.x_min+=v.x;b.x_max+=v.x;b.y_min+=v.y;b.y_max+=v.y;text_bounds_union(&bounds,b);
        } else {
            const os64_font_glyph_view_t *image=&run->images[i]->view;
            for(uint32_t y=0;y<image->height;++y)for(uint32_t x=0;x<image->width;) {
                if(!image->coverage[(size_t)y*image->stride+x]){++x;continue;}
                uint32_t start=x++;
                while(x<image->width&&image->coverage[(size_t)y*image->stride+x])++x;
                /* Widen signed bearings before adding unsigned row offsets. */
                double x0=(double)image->left+start,x1=(double)image->left+x,y0=(double)image->top+y;
                os64_geometry_command_t commands[]={
                    {.verb=OS64_GEOMETRY_MOVE,.p={{x0,y0}}},{.verb=OS64_GEOMETRY_LINE,.p={{x1,y0}}},
                    {.verb=OS64_GEOMETRY_LINE,.p={{x1,y0+1}}},{.verb=OS64_GEOMETRY_LINE,.p={{x0,y0+1}}},
                    {.verb=OS64_GEOMETRY_CLOSE}};
                for(unsigned k=0;k<5;++k){os64_font_status_t status=text_outline_emit(&v,&commands[k]);if(status!=OS64_FONT_OK)return status;}
                text_bounds_union(&bounds,(os64_geometry_bounds_t){false,x0+v.x,y0+v.y,x1+v.x,y0+1+v.y});
            }
        }
    }
    *out=bounds;return OS64_FONT_OK;
}
