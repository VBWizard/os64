#include "os64/appearance.h"
#include "os64/decoration.h"
#include "os64/text_profile.h"
#include <limits.h>

static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t pixel(int32_t a)
{
    int64_t n = (int64_t)a + 32;
    return (int32_t)(n >= 0 ? n / 64 : -((-n + 63) / 64));
}

bool os64_decor_validate(const void *bytes, size_t length, os64_decor_view_t *out)
{
    if (out) *out = (os64_decor_view_t){0};
    if (!out || !bytes || ((uintptr_t)bytes & 3u) ||
        length < OS64_DECOR_LEGACY_HEADER_BYTES || length > OS64_DECOR_BYTES_MAX) return false;
    const os64_decor_header_t *h = bytes;
    if (h->magic != OS64_DECOR_MAGIC || (h->version != OS64_DECOR_VERSION && h->version != 5u && h->version != 4u) ||
        h->bytes != length || h->edge != OS64_DECOR_EDGE_TOP ||
        !h->glyph_count || h->glyph_count > OS64_DECOR_GLYPHS_MAX ||
        h->pair_count > OS64_DECOR_PAIRS_MAX || !h->line_height || h->line_height > 256 ||
        h->baseline > h->line_height || h->border < 1 || h->border > 16 ||
        h->padding_x > 32 || h->padding_y > 32 || h->align > 2 ||
        h->reserved[0] || h->reserved[1] || h->button_reserved ||
        h->button_count>OS64_DECOR_BUTTONS_MAX || h->button_size<16 ||
        h->button_size>48 || h->button_gap>16 || h->finish>OS64_DECOR_STIPPLE ||
        h->border_finish>OS64_DECOR_STIPPLE || h->strength>64 ||
        (h->scale!=1 && h->scale!=2 && h->scale!=4 && h->scale!=8) ||
        h->direction>1 || h->relief>3 || h->match_border>1) return false;
    size_t header_bytes=h->version>=6?sizeof(*h):OS64_DECOR_LEGACY_HEADER_BYTES;
    if(length<header_bytes)return false;
    if(h->version>=6)for(unsigned i=0;i<4;++i){
        if((h->symbols[i].active && (h->symbols[i].active>>24)!=255) ||
            (h->symbols[i].inactive && (h->symbols[i].inactive>>24)!=255))return false;
    }
    uint32_t actions=0;
    for (uint32_t i=0;i<OS64_DECOR_BUTTONS_MAX;++i) {
        const os64_decor_button_t *b=&h->buttons[i];
        if (i>=h->button_count) {
            if (b->action || b->group || b->shape || b->face) return false;
            continue;
        }
        if (b->action>OS64_DECOR_PIN || b->group>1 || b->shape>OS64_DECOR_BARE ||
            (h->version==4u && b->face) ||
            (b->face && (b->face>>24)!=255 && (b->face>>24)!=1)) return false;
        if (b->action && (actions & (1u<<b->action))) return false;
        if (b->action) actions |= 1u<<b->action;
    }
    size_t gend = header_bytes + h->glyph_count * sizeof(os64_decor_glyph_t);
    size_t pend = gend + h->pair_count * sizeof(os64_decor_pair_t);
    if (h->glyph_offset != header_bytes || h->pair_offset != gend ||
        h->mask_offset != pend || pend > length || h->mask_bytes > length-pend ||
        h->tile_offset != ((pend+h->mask_bytes+3u)&~(size_t)3u) ||
        h->tile_offset>length || h->tile_bytes!=OS64_DECOR_TILE_BYTES ||
        h->tile_bytes!=length-h->tile_offset) return false;
    const uint8_t *b = bytes;
    const os64_decor_glyph_t *glyphs = (const void *)(b + h->glyph_offset);
    size_t end = 0;
    for (size_t n = 0; n < h->glyph_count; ++n) {
        const os64_decor_glyph_t *g = &glyphs[n];
        if ((n && g->scalar <= glyphs[n-1].scalar) || g->reserved ||
            (g->scalar != OS64_W1_MARKER && !os64_w1_supported(g->scalar)) ||
            g->width > OS64_DECOR_MASK_MAX || g->height > OS64_DECOR_MASK_MAX ||
            g->left < -256 || g->left > 256 || g->top < -(int32_t)h->baseline ||
            (int64_t)h->baseline + g->top + g->height > h->line_height ||
            g->advance < 1 || g->advance > 256 * 64 || g->offset != end) return false;
        end += (size_t)g->width * g->height;
        if (end > h->mask_bytes) return false;
    }
    if (end != h->mask_bytes || glyphs[h->glyph_count-1].scalar != OS64_W1_MARKER) return false;
    const os64_decor_pair_t *pairs = (const void *)(b + h->pair_offset);
    for (size_t n = 0; n < h->pair_count; ++n) {
        const os64_decor_pair_t *p = &pairs[n];
        if ((n && p->key <= pairs[n-1].key) || (p->key >> 16) >= h->glyph_count ||
            (p->key & 65535u) >= h->glyph_count || p->adjustment < -256 * 64 ||
            p->adjustment > 256 * 64) return false;
    }
    const uint32_t *tiles=(const void *)(b+h->tile_offset);
    for (size_t i=0;i<OS64_DECOR_TILE_BYTES/4;++i)
        if ((tiles[i]>>24)!=255) return false;
    *out = (os64_decor_view_t){h, glyphs, pairs, b + pend,tiles};
    return true;
}

void os64_decor_header_copy(const os64_decor_header_t *h,os64_decor_header_t *out)
{
    *out=(os64_decor_header_t){0};
    size_t size=h->version>=6?sizeof(*out):OS64_DECOR_LEGACY_HEADER_BYTES;
    const uint8_t *source=(const void *)h;uint8_t *target=(void *)out;
    for(size_t i=0;i<size;++i)target[i]=source[i];
    out->version=OS64_DECOR_VERSION;
}

os64_decor_insets_t os64_decor_insets(const os64_decor_header_t *h, bool titlebar)
{
    int32_t b = (int32_t)h->border;
    return (os64_decor_insets_t){b, titlebar ? b + maximum((int32_t)h->line_height,h->button_count?(int32_t)h->button_size:0) + (int32_t)(2*h->padding_y) : b, b, b};
}

bool os64_decor_layout(const os64_decor_header_t *h, int32_t width, int32_t height,
    bool titlebar, os64_decor_layout_t *out)
{
    if (!h || !out) return false;
    *out = (os64_decor_layout_t){0};
    os64_decor_insets_t i = os64_decor_insets(h, titlebar);
    if (titlebar && width<(int32_t)os64_decor_min_width(h)) return false;
    if (width > 65536 || height > 65536 || width <= i.left+i.right || height <= i.top+i.bottom) return false;
    out->insets = i;
    if (titlebar) {
        out->title = (os64_decor_rect_t){i.left, (int32_t)h->border, width-i.left-i.right,
            i.top-(int32_t)h->border};
        int32_t counts[2]={0},extent[2]={0};
        for (uint32_t n=0;n<h->button_count;++n) ++counts[h->buttons[n].group];
        for (unsigned g=0;g<2;++g) if (counts[g])
            extent[g]=counts[g]*(int32_t)h->button_size+(counts[g]-1)*(int32_t)h->button_gap;
        int32_t left=i.left+(int32_t)h->padding_x;
        int32_t right=width-i.right-(int32_t)h->padding_x;
        int32_t cursor[2]={left,right-extent[1]};
        for (uint32_t n=0;n<h->button_count;++n) {
            unsigned group=h->buttons[n].group;
            out->buttons[n]=(os64_decor_rect_t){cursor[group],out->title.y+(out->title.h-(int32_t)h->button_size)/2,
                (int32_t)h->button_size,(int32_t)h->button_size};
            cursor[group]+=(int32_t)(h->button_size+h->button_gap);
        }
        left+=extent[0]+(counts[0]?4:0);right-=extent[1]+(counts[1]?4:0);
        out->text=(os64_decor_rect_t){left,out->title.y+(out->title.h-(int32_t)h->line_height)/2,
            maximum(0,right-left),(int32_t)h->line_height};
        out->baseline = out->text.y + (int32_t)h->baseline;
    }
    return true;
}

uint32_t os64_decor_min_width(const os64_decor_header_t *h)
{
    uint32_t count[2]={0};
    for (uint32_t n=0;n<h->button_count;++n) ++count[h->buttons[n].group];
    uint32_t width=2*(h->border+h->padding_x)+24;
    for (unsigned g=0;g<2;++g) if (count[g])
        width+=count[g]*h->button_size+(count[g]-1)*h->button_gap+4;
    return width;
}

uint32_t os64_decor_hit(const os64_decor_header_t *h, const os64_decor_layout_t *l, int32_t x, int32_t y)
{
    for (uint32_t i=0;i<h->button_count;++i) {
        os64_decor_rect_t r=l->buttons[i];
        if (x>=r.x && y>=r.y && (int64_t)x< (int64_t)r.x+r.w && (int64_t)y<(int64_t)r.y+r.h)
            return h->buttons[i].action;
    }
    return 0;
}
void os64_decor_capture_cancel(os64_decor_capture_t *c)
{ c->window=0;c->action=0;c->armed=false; }
void os64_decor_capture_begin(os64_decor_capture_t *c, uint32_t window, uint32_t action, uint32_t buttons)
{ *c=(os64_decor_capture_t){window,action,buttons,true}; }
uint32_t os64_decor_capture_step(os64_decor_capture_t *c, uint32_t event, uint32_t button,
    uint32_t hit_window, uint32_t hit_action)
{
    if (!c->buttons || button>7) return 0;
    c->armed=c->window && c->window==hit_window && c->action==hit_action;
    uint32_t action=0;
    if (event==OS64_DECOR_POINTER_DOWN) c->buttons|=1u<<button;
    if (event==OS64_DECOR_POINTER_UP) {
        if (button==0) {if (c->armed) action=c->action;os64_decor_capture_cancel(c);}
        c->buttons &= ~(1u<<button);
    }
    return action;
}

static bool intersection(os64_decor_rect_t a, os64_decor_rect_t b, os64_decor_rect_t *out)
{
    int64_t x = maximum(a.x,b.x), y = maximum(a.y,b.y);
    int64_t r = (int64_t)a.x+a.w, br = (int64_t)b.x+b.w;
    int64_t u = (int64_t)a.y+a.h, bu = (int64_t)b.y+b.h;
    if (br < r) r = br;
    if (bu < u) u = bu;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || r <= x || u <= y) return false;
    *out = (os64_decor_rect_t){(int32_t)x,(int32_t)y,(int32_t)(r-x),(int32_t)(u-y)};
    return true;
}

static void fill(os64_decor_surface_t *s, os64_decor_rect_t clip,
    os64_decor_rect_t rect, uint32_t color)
{
    os64_decor_rect_t r;
    if (!intersection(clip, rect, &r)) return;
    for (int32_t y=r.y; y<r.y+r.h; ++y)
        for (int32_t x=r.x; x<r.x+r.w; ++x) s->pixels[(size_t)y*s->pitch+x] = color;
}

static void finish_fill(const os64_decor_view_t *v,os64_decor_surface_t *s,
    os64_decor_rect_t clip,os64_decor_rect_t rect,os64_decor_rect_t anchor,unsigned tile,bool gradient)
{
    os64_decor_rect_t r;
    if (!intersection(clip,rect,&r)) return;
    const uint32_t *pixels=v->tiles+tile*OS64_DECOR_TILE_SIDE*OS64_DECOR_TILE_SIDE;
    for (int32_t y=r.y;y<r.y+r.h;++y) for (int32_t x=r.x;x<r.x+r.w;++x) {
        uint32_t tx=(uint32_t)(x-anchor.x)%OS64_DECOR_TILE_SIDE;
        uint32_t ty=(uint32_t)(y-anchor.y)%OS64_DECOR_TILE_SIDE;
        if (gradient) {
            tx=(uint32_t)((int64_t)(x-anchor.x)*31/maximum(1,anchor.w-1));
            ty=(uint32_t)((int64_t)(y-anchor.y)*31/maximum(1,anchor.h-1));
        }
        s->pixels[(size_t)y*s->pitch+x]=pixels[ty*OS64_DECOR_TILE_SIDE+tx];
    }
}

static uint32_t ordinal(const os64_decor_view_t *v, uint32_t scalar)
{
    uint32_t lo=0,hi=v->header->glyph_count;
    while (lo<hi) {
        uint32_t m=lo+(hi-lo)/2;
        if (v->glyphs[m].scalar < scalar) lo=m+1; else hi=m;
    }
    return lo<v->header->glyph_count && v->glyphs[lo].scalar==scalar ? lo : v->header->glyph_count-1;
}

static int32_t adjustment(const os64_decor_view_t *v, uint32_t left, uint32_t right)
{
    if (left == UINT32_MAX) return 0;
    uint32_t key=(left<<16)|right,lo=0,hi=v->header->pair_count;
    while (lo<hi) {
        uint32_t m=lo+(hi-lo)/2;
        if (v->pairs[m].key < key) lo=m+1; else hi=m;
    }
    return lo<v->header->pair_count && v->pairs[lo].key==key ? v->pairs[lo].adjustment : 0;
}

typedef struct { uint32_t glyph; int32_t x; size_t cluster; } placed_t;

static void glyph_paint(const os64_decor_view_t *v, os64_decor_surface_t *s,
    os64_decor_rect_t clip, uint32_t glyph, int32_t x, int32_t baseline, uint32_t color)
{
    const os64_decor_glyph_t *g=&v->glyphs[glyph];
    os64_decor_rect_t r={x+g->left,baseline+g->top,(int32_t)g->width,(int32_t)g->height}, c;
    if (!intersection(r,clip,&c)) return;
    for (int32_t y=c.y;y<c.y+c.h;++y) for (int32_t xx=c.x;xx<c.x+c.w;++xx) {
        uint32_t a=v->masks[g->offset+(size_t)(y-r.y)*g->width+(xx-r.x)];
        uint32_t *p=&s->pixels[(size_t)y*s->pitch+xx],value=0xff000000u;
        for (uint32_t shift=0;shift<24;shift+=8)
            value|=((a*((color>>shift)&255)+(255-a)*((*p>>shift)&255)+127)/255)<<shift;
        *p=value;
    }
}

static uint32_t mix(uint32_t a,uint32_t b)
{ return 0xff000000u | (((a&0xfefefeu)+(b&0xfefefeu))>>1); }

static void button_paint(const os64_decor_header_t *h, os64_decor_surface_t *s,
    os64_decor_rect_t clip, os64_decor_rect_t r, const os64_decor_button_t *b,
    bool active, bool pinned, const os64_decor_state_t *state)
{
    if (!b->action) return;
    uint32_t face=active?h->active_face:h->inactive_face;
    uint32_t ink=active?h->active_text:h->inactive_text;
    bool disabled=state && (state->disabled & (1u<<b->action));
    bool pressed=!disabled && state && state->pressed==b->action;
    bool hover=!disabled && state && state->hover==b->action;
    uint32_t housing=pressed?mix(face,0xff000000u):hover?mix(face,ink):mix(face,0xff606060u);
    if (b->face) {
        uint32_t custom=0xff000000u | (b->face&0xffffffu);
        housing=pressed?mix(custom,0xff000000u):hover?mix(custom,0xffffffffu):custom;
        if (disabled) housing=mix(housing,face);
    }
    /* Housing uses title ink; a symbol override must not recolor its hover. */
    uint32_t custom_ink=h->version>=6?(active?h->symbols[b->action-1].active:h->symbols[b->action-1].inactive):0;
    if(custom_ink)ink=custom_ink;
    else if(disabled)ink=mix(face,ink);
    os64_decor_rect_t c;
    if (!intersection(r,clip,&c)) return;
    int32_t mid=r.w/2, radius=r.w/2;
    for (int32_t y=c.y;y<c.y+c.h;++y) for (int32_t x=c.x;x<c.x+c.w;++x) {
        int32_t dx=x-r.x-mid,dy=y-r.y-mid;
        bool inside=b->shape!=OS64_DECOR_ROUND || dx*dx+dy*dy<radius*radius;
        if (inside && (b->face>>24)!=1 && (b->shape!=OS64_DECOR_BARE || hover || pressed))
            s->pixels[(size_t)y*s->pitch+x]=housing;
    }
    int32_t k=maximum(3,r.w/5),cx=r.x+mid+(pressed?1:0),cy=r.y+mid+(pressed?1:0);
    for (int32_t y=-k-2;y<=k+2;++y) for (int32_t x=-k-2;x<=k+2;++x) {
        bool mark=false;
        if (b->action==OS64_DECOR_CLOSE) mark=x>=-k && x<=k && (y==x || y==-x);
        if (b->action==OS64_DECOR_MINIMIZE) mark=x>=-k && x<=k && y==k;
        if (b->action==OS64_DECOR_MAXIMIZE) {
            mark=((y==-k || y==k) && x>=-k && x<=k) || ((x==-k || x==k) && y>=-k && y<=k);
            if (state && state->maximized)
                mark=(((y==-k+2 || y==k+2) && x>=-k && x<=k) || ((x==-k || x==k) && y>=-k+2 && y<=k+2)) ||
                    ((y==-k && x>=-k+2 && x<=k+2) || (x==k+2 && y>=-k && y<=k));
        }
        if (b->action==OS64_DECOR_PIN)
            mark=(y==-k && x>=-k/2 && x<=k/2) || (y>=-k && y<=0 && (x==-k/2 || x==k/2 || pinned)) ||
                (y==0 && x>=-k && x<=k) || (x==0 && y>=0 && y<=k);
        if (mark) {
            int32_t px=cx+x,py=cy+y;
            uint32_t color=ink;
            if(custom_ink && disabled && px>=clip.x && py>=clip.y &&
                px<clip.x+clip.w && py<clip.y+clip.h)
                color=mix(s->pixels[(size_t)py*s->pitch+px],ink);
            fill(s,clip,(os64_decor_rect_t){px,py,1,1},color);
        }
    }
}

bool os64_decor_paint(const os64_decor_view_t *v, os64_decor_surface_t *s,
    os64_decor_rect_t frame, os64_decor_rect_t damage, bool titlebar, bool active,
    bool pinned, const char *title, size_t length, bool legacy_title, const os64_decor_state_t *state)
{
    if (!v || !v->header || !s || !s->pixels || s->width>INT32_MAX || s->height>INT32_MAX ||
        s->pitch<s->width || (s->height && s->pitch>SIZE_MAX/4/s->height) ||
        length>OS64_DECOR_TITLE_MAX || (!title && length) || frame.x < INT32_MIN+512 ||
        frame.y<INT32_MIN+512 || (int64_t)frame.x+frame.w>INT32_MAX-512 ||
        (int64_t)frame.y+frame.h>INT32_MAX-512) return false;
    os64_decor_layout_t layout;
    if (!os64_decor_layout(v->header,frame.w,frame.h,titlebar,&layout)) return false;
    os64_decor_rect_t clip;
    if (!intersection(damage,(os64_decor_rect_t){0,0,(int32_t)s->width,(int32_t)s->height},&clip)) return true;
    const os64_decor_header_t *h=v->header;
    int32_t b=(int32_t)h->border;
    unsigned border_tile=active?2:3;
    bool border_gradient=(h->match_border?h->finish:h->border_finish)==OS64_DECOR_GRADIENT;
    finish_fill(v,s,clip,(os64_decor_rect_t){frame.x,frame.y,frame.w,b},frame,border_tile,border_gradient);
    finish_fill(v,s,clip,(os64_decor_rect_t){frame.x,frame.y+frame.h-b,frame.w,b},frame,border_tile,border_gradient);
    finish_fill(v,s,clip,(os64_decor_rect_t){frame.x,frame.y,b,frame.h},frame,border_tile,border_gradient);
    finish_fill(v,s,clip,(os64_decor_rect_t){frame.x+frame.w-b,frame.y,b,frame.h},frame,border_tile,border_gradient);
    if (!titlebar) return true;
    os64_decor_rect_t bar=layout.title;
    bar.x+=frame.x;bar.y+=frame.y;
    finish_fill(v,s,clip,bar,bar,active?0:1,h->finish==OS64_DECOR_GRADIENT);
    uint32_t face=active?h->active_face:h->inactive_face;
    for (uint32_t n=0;n<h->relief;++n) {
        fill(s,clip,(os64_decor_rect_t){bar.x+(int32_t)n,bar.y+(int32_t)n,bar.w-2*(int32_t)n,1},mix(face,0xffffffff));
        fill(s,clip,(os64_decor_rect_t){bar.x+(int32_t)n,bar.y+bar.h-1-(int32_t)n,bar.w-2*(int32_t)n,1},mix(face,0xff000000));
    }
    bool pin_button=false;
    for (uint32_t n=0;n<h->button_count;++n) {
        os64_decor_rect_t r=layout.buttons[n];r.x+=frame.x;r.y+=frame.y;
        button_paint(h,s,clip,r,&h->buttons[n],active,pinned,state);
        if (h->buttons[n].action==OS64_DECOR_PIN) pin_button=true;
    }
    os64_decor_rect_t text=layout.text;
    text.x+=frame.x;text.y+=frame.y;
    uint32_t color=active?h->active_text:h->inactive_text;
    if (pinned && !pin_button && text.w>=14) {
        fill(s,clip,(os64_decor_rect_t){text.x+text.w-8,text.y+(text.h-8)/2,8,8},color);
        text.w-=14;
    }
    os64_decor_rect_t tc;
    if (!intersection(clip,text,&tc)) return true;
    placed_t placed[OS64_DECOR_TITLE_MAX*2];
    size_t count=0,at=0,cluster=0;
    int32_t pen=0,last_start=-1;uint32_t previous=UINT32_MAX;
    while (at<length) {
        os64_w1_cluster_t d=os64_w1_decode((const uint8_t *)title,length,at,legacy_title);
        for (unsigned extra=0;extra<(d.extra_marker?2u:1u);++extra) {
            uint32_t g=ordinal(v,extra?OS64_W1_MARKER:d.scalar);
            int32_t delta=adjustment(v,previous,g);
            if (previous!=UINT32_MAX && pen+delta>last_start) pen+=delta;
            placed[count++]=(placed_t){g,pen,cluster};
            last_start=pen;pen+=v->glyphs[g].advance;previous=g;
        }
        at=d.end;++cluster;
    }
    bool shortened=pen>(int64_t)text.w*64;
    uint32_t ellipsis=ordinal(v,0x2026);
    int32_t ellipsis_x=pen;
    if (shortened) {
        int32_t room=text.w*64-v->glyphs[ellipsis].advance;
        while (count) {
            size_t start=count-1;
            while (start && placed[start-1].cluster==placed[count-1].cluster) --start;
            int32_t end=placed[count-1].x+v->glyphs[placed[count-1].glyph].advance;
            if (end<=room) break;
            count=start;
        }
        ellipsis_x=count?placed[count-1].x+v->glyphs[placed[count-1].glyph].advance:0;
        pen=ellipsis_x+v->glyphs[ellipsis].advance;
    }
    int32_t width=pixel(pen),x=text.x;
    if (h->align==1) x=maximum(text.x,minimum(frame.x+frame.w/2-width/2,text.x+text.w-width));
    if (h->align==2) x=maximum(text.x,text.x+text.w-width);
    for (size_t n=0;n<count;++n)
        glyph_paint(v,s,tc,placed[n].glyph,x+pixel(placed[n].x),frame.y+layout.baseline,color);
    if (shortened) glyph_paint(v,s,tc,ellipsis,x+pixel(ellipsis_x),frame.y+layout.baseline,color);
    return true;
}

uint64_t os64_decor_fingerprint(const void *bytes,size_t length)
{
    const uint8_t *p=bytes;
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<length;++i)hash=(hash^p[i])*UINT64_C(1099511628211);
    return hash;
}

size_t os64_decor_status_write(char *out,const os64_decor_status_t *status)
{
    const char prefix[]="fingerprint = fnv1a64-v1:";
    const char digits[]="0123456789abcdef";
    size_t used=os64_appearance_header_write(out,status->generation);
    for(size_t i=0;i<sizeof(prefix)-1;++i)out[used++]=prefix[i];
    for(int shift=60;shift>=0;shift-=4)out[used++]=digits[(status->fingerprint>>shift)&15];
    out[used++]=':';
    for(int shift=28;shift>=0;shift-=4)out[used++]=digits[(status->bytes>>shift)&15];
    out[used++]='\n';
    return used;
}

bool os64_decor_status_read(const char *text,size_t length,os64_decor_status_t *out)
{
    os64_decor_status_t next={0};size_t body;
    if(!out || !os64_appearance_header_read(text,length,&next.generation,&body))return false;
    if(body==length){*out=next;return true;}
    const char prefix[]="fingerprint = fnv1a64-v1:";
    if(length-body!=sizeof(prefix)-1+16+1+8+1)return false;
    for(size_t i=0;i<sizeof(prefix)-1;++i)if(text[body++]!=prefix[i])return false;
    uint64_t fields[2]={0};
    for(unsigned f=0;f<2;++f){
        for(unsigned i=0;i<(f?8u:16u);++i){
            unsigned char c=text[body++];
            unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;
            if(d==16)return false;
            fields[f]=(fields[f]<<4)|d;
        }
        if(text[body++]!=(f?'\n':':'))return false;
    }
    next.fingerprint=fields[0];next.bytes=(uint32_t)fields[1];
    if(next.bytes>OS64_DECOR_BYTES_MAX ||
        (next.bytes && (!next.generation || next.bytes<OS64_DECOR_LEGACY_HEADER_BYTES)) ||
        (!next.bytes && next.fingerprint))return false;
    *out=next;return true;
}
