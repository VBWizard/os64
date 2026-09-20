#include "text_internal.h"
#include "os64/str.h"
#include "os64/charset.h"

static void include_metrics(os64_text_run_t *r, const os64_font_face_info_t *info)
{
    if (info->ascent>r->view.ascent) r->view.ascent=info->ascent;
    if (info->descent>r->view.descent) r->view.descent=info->descent;
    if (info->line_height>r->view.line_height) r->view.line_height=info->line_height;
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
            if (f->face) {
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
            if (n==0 && r->cell_advance && scalar>=0x2500 && scalar<=0x259f) {
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
        (o->cell_advance && o->encoding==OS64_TEXT_UTF8_WESTERN_V1)) return OS64_FONT_BAD_ARGUMENT;
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
    os64_text_run_t *r=text_alloc(c,sizeof(*r));
    if (!r) return c->refusal;
    *r=(os64_text_run_t){.context=c,.encoding=o->encoding,.cell_advance=o->cell_advance,
        .cell_ascent=o->fonts[0]->info.ascent,.cell_height=o->fonts[0]->info.line_height};
    c->runs++;
    os64_font_status_t status=OS64_FONT_OK;
    for (size_t n=0;n<o->font_count;n++) {r->fonts[n]=o->fonts[n];r->fonts[n]->refs++;r->font_count++;}
    include_metrics(r,&r->fonts[0]->info);
    if (length) {
        r->view.bytes=text_alloc(c,length);
        if (!r->view.bytes) goto allocation_failed;
        os64_memcpy((void *)r->view.bytes,bytes,length);
    }
    r->view.byte_count=length;
    if (count) {
        r->view.glyphs=text_alloc(c,count*sizeof(os64_text_placement_t));
        if (!r->view.glyphs) goto allocation_failed;
        r->images=text_alloc(c,count*sizeof(text_image *));
        if (!r->images) goto allocation_failed;
    }
    r->view.carets=text_alloc(c,(clusters+1)*sizeof(os64_text_caret_t));
    if (!r->view.carets) goto allocation_failed;
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
        }
        carets[r->view.caret_count++]=(os64_text_caret_t){d.end,(int32_t)pen};at=d.end;
    }
    if ((int64_t)r->view.ascent+r->view.descent>INT32_MAX) {status=OS64_FONT_LIMIT;goto fail;}
    if (r->view.line_height<r->view.ascent+r->view.descent)
        r->view.line_height=r->view.ascent+r->view.descent;
    r->view.advance_x=(int32_t)pen;*out=r;text_trim(c);return OS64_FONT_OK;
allocation_failed:
    status=c->refusal;
fail:
    os64_text_run_release(r);return status;
}
os64_font_status_t os64_text_run_view(const os64_text_run_t *r, os64_text_run_view_t *out)
{
    if (out) *out=(os64_text_run_view_t){0};
    if (!r || !out) return OS64_FONT_BAD_ARGUMENT;
    *out=r->view;return OS64_FONT_OK;
}
void os64_text_run_release(os64_text_run_t *r)
{
    if (!r) return;
    os64_text_context_t *c=r->context;
    for (size_t n=0;n<r->view.glyph_count;n++) text_image_unpin(r->images[n]);
    for (size_t n=0;n<r->font_count;n++) os64_text_font_release(r->fonts[n]);
    text_free(c,(void *)r->view.bytes);text_free(c,(void *)r->view.glyphs);
    text_free(c,(void *)r->view.carets);text_free(c,r->images);
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
