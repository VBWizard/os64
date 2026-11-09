#include "os64/decoration_prepare.h"
#include "os64/arena.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "text_internal.h"

void os64_decor_defaults(os64_decor_header_t *h)
{
    if (!h) return;
    *h=(os64_decor_header_t){.magic=OS64_DECOR_MAGIC,.version=OS64_DECOR_VERSION,
        .border=1,.padding_x=6,.padding_y=3,.align=0,
        .active_face=0xff203b59,.inactive_face=0xff343c46,
        .active_text=0xfff2eee6,.inactive_text=0xffc4cbd4,
        .active_border=0xffd5a650,.inactive_border=0xff59616b,
        .active_face2=0xff50759a,.inactive_face2=0xff5c6672,
        .active_border2=0xff8c642f,.inactive_border2=0xff303943,
        .button_count=3,.button_size=24,.button_gap=4,.scale=1,.seed=17,
        .buttons={{OS64_DECOR_MINIMIZE,1,OS64_DECOR_BARE,0},
                  {OS64_DECOR_MAXIMIZE,1,OS64_DECOR_BARE,0},
                  {OS64_DECOR_CLOSE,1,OS64_DECOR_SQUARE,0}}};
}

/* Tile preparation stays in userland. The shared painter samples finished
 * pixels; gradient tiles stretch across their surface, patterns repeat. */
static uint32_t mix_color(uint32_t first,uint32_t second,unsigned weight,unsigned total)
{
    uint32_t out=0xff000000;
    for (unsigned shift=0;shift<24;shift+=8) {
        unsigned a=(first>>shift)&255,b=(second>>shift)&255;
        unsigned n=(a*(total-weight)+b*weight+total/2)/total;
        out|=(uint32_t)n<<shift;
    }
    return out;
}
static void prepare_tiles(os64_decor_header_t *h)
{
    uint32_t *out=(void *)((uint8_t *)h+h->tile_offset);
    uint32_t colors[4]={h->active_face,h->inactive_face,h->active_border,h->inactive_border};
    uint32_t seconds[4]={h->active_face2,h->inactive_face2,h->active_border2,h->inactive_border2};
    for(unsigned tile=0;tile<4;++tile) for(uint32_t y=0;y<32;++y) for(uint32_t x=0;x<32;++x) {
        uint32_t kind=tile<2 || h->match_border?h->finish:h->border_finish;
        uint32_t xx=x/h->scale,yy=y/h->scale;
        uint32_t hash=h->seed ^ (xx*0x9e3779b9u) ^ (yy*0x85ebca6bu);
        hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
        unsigned weight=0,total=64;
        if(kind==OS64_DECOR_GRADIENT){weight=h->direction?x:y;total=31;}
        if(kind==OS64_DECOR_GRAIN){weight=(hash&63)*h->strength;total=63*64;}
        if(kind==OS64_DECOR_STRIPES)weight=((h->direction?xx:yy)&1)?h->strength:0;
        if(kind==OS64_DECOR_STIPPLE)weight=(xx%2==0 && yy%2==0)?h->strength:0;
        *out++=mix_color(colors[tile],seconds[tile],weight,total);
    }
}
static bool finish_parameters_valid(const os64_decor_header_t *h)
{
    return h->strength<=64 && h->finish<=OS64_DECOR_STIPPLE && h->border_finish<=OS64_DECOR_STIPPLE &&
        (h->scale==1 || h->scale==2 || h->scale==4 || h->scale==8) && h->direction<=1 && h->relief<=3;
}
os64_font_status_t os64_decor_restyle(const void *bytes,size_t length,const os64_decor_header_t *style,
    void **out,size_t *out_length)
{
    if(out)*out=NULL;
    if(out_length)*out_length=0;
    os64_decor_view_t view;
    if(!out || !out_length || !style || !finish_parameters_valid(style) || !os64_decor_validate(bytes,length,&view))
        return OS64_FONT_BAD_ARGUMENT;
    const os64_decor_header_t *old=view.header;
    size_t delta=sizeof(os64_decor_header_t)-old->glyph_offset;
    if(length>OS64_DECOR_BYTES_MAX-delta)return OS64_FONT_LIMIT;
    size_t size=length+delta;
    os64_decor_header_t *h=os64_malloc(size);
    if(!h)return OS64_FONT_NO_MEMORY;
    *h=*style;
    os64_memcpy((uint8_t *)h+sizeof(*h),(const uint8_t *)bytes+old->glyph_offset,length-old->glyph_offset);
    h->magic=old->magic;h->version=OS64_DECOR_VERSION;h->bytes=(uint32_t)size;
    h->glyph_count=old->glyph_count;h->pair_count=old->pair_count;
    h->glyph_offset=old->glyph_offset+(uint32_t)delta;h->pair_offset=old->pair_offset+(uint32_t)delta;
    h->mask_offset=old->mask_offset+(uint32_t)delta;h->mask_bytes=old->mask_bytes;
    h->line_height=old->line_height;h->baseline=old->baseline;
    h->tile_offset=old->tile_offset+(uint32_t)delta;h->tile_bytes=old->tile_bytes;
    prepare_tiles(h);
    if(!os64_decor_validate(h,size,&view)){os64_free(h);return OS64_FONT_BAD_ARGUMENT;}
    *out=h;*out_length=size;return OS64_FONT_OK;
}

typedef struct {
    os64_decor_glyph_t glyph;
    const uint8_t *mask;
    os64_text_font_t *font;
    uint32_t index;
} prepared_t;

os64_font_status_t os64_decor_prepare(const os64_font_role_view_t *role,
    const os64_decor_header_t *style, void **bytes, size_t *length)
{
    if (bytes) *bytes=NULL;
    if (length) *length=0;
    if (style && !finish_parameters_valid(style)) return OS64_FONT_BAD_ARGUMENT;
    if (!role || !role->text || !role->fonts || !role->font_count || !style || !bytes || !length)
        return OS64_FONT_BAD_ARGUMENT;
    os64_arena_t *arena=os64_arena_create(4096,16u*1024u*1024u);
    if (!arena) return OS64_FONT_NO_MEMORY;
    prepared_t *glyphs=os64_arena_calloc(arena,OS64_DECOR_GLYPHS_MAX,sizeof(*glyphs));
    os64_decor_pair_t *pairs=os64_arena_alloc(arena,OS64_DECOR_PAIRS_MAX*sizeof(*pairs));
    os64_font_status_t status=OS64_FONT_NO_MEMORY;
    size_t count=0,pair_count=0,mask_bytes=0;
    int64_t top=-(int64_t)role->baseline_px,bottom=(int64_t)role->row_height_px-role->baseline_px;
    if (!glyphs || !pairs) goto done;
    os64_text_layout_t layout={.encoding=OS64_TEXT_UTF8_WESTERN_V1,
        .fonts=role->fonts,.font_count=role->font_count,.tab_interval=8*64};
    for (uint32_t scalar=0x20;scalar<=0x25a0;++scalar) {
        bool marker=scalar==0x25a0;
        if (!marker && !text_w1_supported(scalar)) continue;
        if (count==OS64_DECOR_GLYPHS_MAX) {status=OS64_FONT_LIMIT;goto done;}
        char encoded[4];
        size_t encoded_length=os64_utf8_encode(marker?0xfffd:scalar,encoded);
        os64_text_run_t *run=NULL;
        status=os64_text_layout(role->text,(const uint8_t *)encoded,encoded_length,&layout,&run);
        if (status!=OS64_FONT_OK) goto done;
        if (run->view.glyph_count!=1) {
            os64_text_run_release(run);status=OS64_FONT_BAD_ARGUMENT;goto done;
        }
        const os64_font_glyph_view_t *g=&run->images[0]->view;
        prepared_t *p=&glyphs[count];
        p->glyph=(os64_decor_glyph_t){.scalar=marker?UINT32_MAX:scalar,.offset=(uint32_t)mask_bytes,
            .width=g->width,.height=g->height,.left=g->left,.top=g->top,.advance=g->advance_x};
        size_t area=(size_t)g->width*g->height;
        if (g->width>OS64_DECOR_MASK_MAX || g->height>OS64_DECOR_MASK_MAX ||
            area>OS64_DECOR_BYTES_MAX-mask_bytes) {
            os64_text_run_release(run);status=OS64_FONT_LIMIT;goto done;
        }
        if (area) {
            uint8_t *mask=os64_arena_alloc(arena,area);
            if (!mask) {os64_text_run_release(run);status=OS64_FONT_NO_MEMORY;goto done;}
            for (uint32_t y=0;y<g->height;++y)
                os64_memcpy(mask+(size_t)y*g->width,g->coverage+(size_t)y*g->stride,g->width);
            p->mask=mask;
        }
        mask_bytes+=area;
        if (g->top<top) top=g->top;
        if ((int64_t)g->top+g->height>bottom) bottom=(int64_t)g->top+g->height;
        const os64_text_placement_t *place=&run->view.glyphs[0];
        for (size_t i=0;i<role->font_count;++i)
            if (role->fonts[i]->identity==place->font_identity) p->font=role->fonts[i];
        p->index=place->glyph_index;
        os64_text_run_release(run);
        ++count;
    }
    for (size_t i=0;i<count;++i) for (size_t j=0;j<count;++j) {
        if (!glyphs[i].font || glyphs[i].font!=glyphs[j].font || !glyphs[i].font->face ||
            !glyphs[i].index || !glyphs[j].index) continue;
        int32_t delta=0;
        status=role->text->backend->pair_adjust(glyphs[i].font->face,glyphs[i].index,glyphs[j].index,&delta);
        if (status!=OS64_FONT_OK) goto done;
        if (!delta) continue;
        if (pair_count==OS64_DECOR_PAIRS_MAX) {status=OS64_FONT_LIMIT;goto done;}
        pairs[pair_count++]=(os64_decor_pair_t){(uint32_t)((i<<16)|j),delta};
    }
    size_t glyph_offset=sizeof(os64_decor_header_t);
    size_t pair_offset=glyph_offset+count*sizeof(os64_decor_glyph_t);
    size_t mask_offset=pair_offset+pair_count*sizeof(os64_decor_pair_t);
    size_t tile_offset=(mask_offset+mask_bytes+3u)&~(size_t)3u;
    size_t total=tile_offset+OS64_DECOR_TILE_BYTES;
    if (top>0 || bottom<0 || (int64_t)bottom-top>256 || total>OS64_DECOR_BYTES_MAX) {
        status=OS64_FONT_LIMIT;goto done;
    }
    uint8_t *result=os64_malloc(total);
    if (!result) {status=OS64_FONT_NO_MEMORY;goto done;}
    os64_decor_header_t *h=(void *)result;
    *h=*style;
    h->magic=OS64_DECOR_MAGIC;h->version=OS64_DECOR_VERSION;h->bytes=(uint32_t)total;
    h->glyph_count=(uint32_t)count;h->pair_count=(uint32_t)pair_count;
    h->glyph_offset=(uint32_t)glyph_offset;h->pair_offset=(uint32_t)pair_offset;
    h->mask_offset=(uint32_t)mask_offset;h->mask_bytes=(uint32_t)mask_bytes;
    h->tile_offset=(uint32_t)tile_offset;h->tile_bytes=OS64_DECOR_TILE_BYTES;
    os64_memset(result+mask_offset+mask_bytes,0,tile_offset-mask_offset-mask_bytes);
    prepare_tiles(h);
    h->line_height=(uint32_t)(bottom-top);h->baseline=(uint32_t)-top;
    for (size_t i=0;i<count;++i) {
        ((os64_decor_glyph_t *)(result+glyph_offset))[i]=glyphs[i].glyph;
        size_t area=(size_t)glyphs[i].glyph.width*glyphs[i].glyph.height;
        if (area) os64_memcpy(result+mask_offset+glyphs[i].glyph.offset,glyphs[i].mask,area);
    }
    os64_memcpy(result+pair_offset,pairs,pair_count*sizeof(*pairs));
    os64_decor_view_t view;
    if (!os64_decor_validate(result,total,&view)) {
        os64_free(result);status=OS64_FONT_LIMIT;goto done;
    }
    *bytes=result;*length=total;status=OS64_FONT_OK;
done:
    os64_arena_destroy(arena);
    return status;
}
