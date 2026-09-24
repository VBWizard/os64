#include "os64/decoration_prepare.h"
#include "os64/text_draw.h"
#include "os64/mem.h"
#include "fake_backend.h"
#include "../userland/apps/framestudio/model.h"
#include "../userland/apps/framestudio/storage.h"
void test_frame_storage(const frame_draft_t *,const void *,size_t);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "decoration_legacy.h"

static size_t live, allocations, deny;
void frame_test_deny_next_alloc(void) {deny=allocations+1;}
void *os64_malloc(size_t bytes)
{
    if (++allocations==deny) return NULL;
    void *p=malloc(bytes);
    if (p) ++live;
    return p;
}
void os64_free(void *p) { if (p) {assert(live);--live;free(p);} }
static void *allocate(void *u,size_t n) {(void)u;return os64_malloc(n);}
static void release(void *u,void *p,size_t n) {(void)u;(void)n;os64_free(p);}
const os64_font_backend_t *os64_freetype_backend_v1(void) {return os64_fake_font_backend();}

static void two_colors(const void *bytes,size_t length)
{
    const uint32_t first[]={0xff102030,0xff305070,0xff90b0d0,0xffa08060};
    const uint32_t second[]={0xffe0a060,0xffc09060,0xff203040,0xff305090};
    os64_decor_header_t recipe;os64_decor_defaults(&recipe);
    recipe.active_face=first[0];recipe.inactive_face=first[1];
    recipe.active_border=first[2];recipe.inactive_border=first[3];
    recipe.active_face2=second[0];recipe.inactive_face2=second[1];
    recipe.active_border2=second[2];recipe.inactive_border2=second[3];
    for(unsigned kind=0;kind<=OS64_DECOR_STIPPLE;++kind)
    for(unsigned direction=0;direction<2;++direction)
    for(unsigned strength=0;strength<=64;strength+=32)
    for(unsigned match=0;match<2;++match) {
        recipe.finish=kind;recipe.direction=direction;recipe.strength=strength;
        recipe.border_finish=OS64_DECOR_SOLID;recipe.match_border=match;
        void *out=NULL;size_t size=0;os64_decor_view_t view;
        assert(os64_decor_restyle(bytes,length,&recipe,&out,&size)==OS64_FONT_OK);
        assert(os64_decor_validate(out,size,&view));
        for(unsigned tile=0;tile<4;++tile) {
            unsigned finish=tile<2 || match?kind:OS64_DECOR_SOLID;
            const uint32_t *pixels=view.tiles+tile*32*32;
            if(finish==OS64_DECOR_GRADIENT) {
                /* Both edges reach the chosen endpoints, even at Strength 0. */
                for(unsigned i=0;i<32;++i) {
                    assert(pixels[direction?i*32:i]==first[tile]);
                    assert(pixels[direction?i*32+31:31*32+i]==second[tile]);
                }
            }
            bool saw_first=false,saw_second=false;
            for(unsigned i=0;i<32*32;++i) {
                uint32_t pixel=pixels[i];
                if(pixel==first[tile])saw_first=true;
                if(pixel==second[tile])saw_second=true;
                assert((pixel>>24)==255);
                if(finish==OS64_DECOR_SOLID || (!strength && finish!=OS64_DECOR_GRADIENT))assert(pixel==first[tile]);
                if(strength==64 && finish>=OS64_DECOR_STRIPES)assert(pixel==first[tile] || pixel==second[tile]);
                for(unsigned shift=0;shift<24;shift+=8) {
                    unsigned a=(first[tile]>>shift)&255,b=(second[tile]>>shift)&255,p=(pixel>>shift)&255;
                    unsigned low=a<b?a:b,high=a>b?a:b;
                    assert(p>=low && p<=high);
                    if(strength==32 && finish>=OS64_DECOR_GRAIN) {
                        unsigned delta=p>a?p-a:a-p,range=high-low;
                        assert(delta<=(range+1)/2);
                    }
                }
            }
            if(strength==64 && finish!=OS64_DECOR_SOLID)assert(saw_first && saw_second);
        }
        os64_free(out);
    }
    /* Equal endpoints produce one color, regardless of finish or Strength. */
    recipe.active_face2=recipe.active_face;recipe.inactive_face2=recipe.inactive_face;
    recipe.active_border2=recipe.active_border;recipe.inactive_border2=recipe.inactive_border;
    for(unsigned kind=0;kind<=OS64_DECOR_STIPPLE;++kind) {
        recipe.finish=kind;recipe.strength=64;recipe.match_border=1;
        void *out=NULL;size_t size=0;os64_decor_view_t view;
        assert(os64_decor_restyle(bytes,length,&recipe,&out,&size)==OS64_FONT_OK);
        assert(os64_decor_validate(out,size,&view));
        for(unsigned tile=0;tile<4;++tile)for(unsigned i=0;i<32*32;++i)assert(view.tiles[tile*32*32+i]==first[tile]);
        os64_free(out);
    }
}

static void button_colors(const void *bytes,size_t length)
{
    os64_decor_header_t style;os64_decor_defaults(&style);
    style.finish=OS64_DECOR_STRIPES;style.strength=64;style.relief=0;
    style.button_count=3;
    const uint32_t colors[]={0xffff5f57,0xffffbd2e,0xff28c840};
    for(unsigned i=0;i<3;++i)style.buttons[i]=(os64_decor_button_t){i+1,1,OS64_DECOR_ROUND,colors[i]};
    void *out;size_t size;os64_decor_view_t v;os64_decor_layout_t layout;
    assert(os64_decor_restyle(bytes,length,&style,&out,&size)==OS64_FONT_OK);
    assert(os64_decor_validate(out,size,&v));
    assert(os64_decor_layout(v.header,400,180,true,&layout));
    uint32_t full[400*180]={0},split[400*180]={0};
    os64_decor_surface_t a={full,400,180,400},b={split,400,180,400};
    os64_decor_rect_t frame={0,0,400,180};
    for(unsigned active=0;active<2;++active){
        assert(os64_decor_paint(&v,&a,frame,frame,true,active,false,"",0,false,NULL));
        for(unsigned i=0;i<3;++i){os64_decor_rect_t r=layout.buttons[i];
            assert(full[(r.y+2)*400+r.x+r.w/2]==colors[i]);
        }
        for(int y=0;y<180;y+=7)for(int x=0;x<400;x+=11)
            assert(os64_decor_paint(&v,&b,frame,(os64_decor_rect_t){x,y,11,7},true,active,false,"",0,false,NULL));
        assert(!memcmp(full,split,sizeof(full)));
    }
    frame_draft_t draft={.style=style};draft.font.size=16;strcpy(draft.font.face[0],"builtin");
    void *file,*decoded;size_t file_size,decoded_size;frame_draft_t loaded;
    assert(frame_encode(&draft,out,size,&file,&file_size)==FRAME_STORE_OK);
    assert(frame_decode(file,file_size,&loaded,&decoded,&decoded_size)==FRAME_STORE_OK);
    assert(frame_same(&draft,&loaded) && !memcmp(out,decoded,size));
    os64_free(file);os64_free(decoded);
    os64_decor_header_t *header=out;
    header->buttons[0].face=0x02000000;assert(!os64_decor_validate(out,size,&v));
    header->buttons[0].face=colors[0];
    size_t old_size;void *old=legacy_bundle(out,size,4,&old_size);
    os64_free(out);out=old;size=old_size;header=out;
    assert(!os64_decor_validate(out,size,&v));
    for(unsigned i=0;i<3;++i)header->buttons[i].face=style.buttons[i].face=0;
    assert(os64_decor_validate(out,size,&v));
    draft.style=style;
    assert(frame_encode(&draft,out,size,&file,&file_size)==FRAME_STORE_OK);
    assert(frame_decode(file,file_size,&loaded,&decoded,&decoded_size)==FRAME_STORE_OK);
    assert(loaded.style.version==OS64_DECOR_VERSION && frame_same(&loaded,&draft));
    void *upgraded;size_t upgraded_size;
    loaded.style.buttons[0].face=colors[0];
    assert(os64_decor_restyle(decoded,decoded_size,&loaded.style,&upgraded,&upgraded_size)==OS64_FONT_OK);
    assert(((os64_decor_header_t *)upgraded)->version==OS64_DECOR_VERSION);
    os64_free(upgraded);os64_free(file);os64_free(decoded);os64_free(out);
    /* Transparent retains the textured surface in every interaction state. */
    for(unsigned i=0;i<3;++i)style.buttons[i].face=OS64_DECOR_FACE_TRANSPARENT | (colors[i]&0xffffff);
    assert(os64_decor_restyle(bytes,length,&style,&out,&size)==OS64_FONT_OK);
    assert(os64_decor_validate(out,size,&v));
    for(unsigned state=0;state<4;++state){
        os64_decor_state_t st={0};if(state==1)st.hover=1;if(state==2)st.pressed=1;if(state==3)st.disabled=2;
        assert(os64_decor_paint(&v,&a,frame,frame,true,true,false,"",0,false,&st));
        os64_decor_rect_t r=layout.buttons[0];int x=r.x+r.w/2,y=r.y+2;
        assert(full[y*400+x]==v.tiles[((y-layout.title.y)%32)*32+(x-layout.title.x)%32]);
        assert(os64_decor_hit(v.header,&layout,x,y)==OS64_DECOR_CLOSE);
    }
    os64_free(out);
    puts("button colors: PASS custom/inactive colors, transparency/states/hits, split damage, saved V4 compatibility and V6 round trip");
}

static uint32_t symbol_mix(uint32_t a,uint32_t b)
{return 0xff000000u | (((a&0xfefefeu)+(b&0xfefefeu))>>1);}
static void symbol_colors(const void *bytes,size_t length)
{
    frame_draft_t draft={0};frame_preset(&draft,0);draft.font.size=24;
    strcpy(draft.font.face[0],"/missing/source.ttf");
    os64_decor_header_t *h=&draft.style;
    const uint32_t fills[]={0xffff4040,0xffffcc22,0xff22dd55,0xff203b59};
    for(unsigned i=0;i<4;++i)h->buttons[i]=(os64_decor_button_t){i+1,i==3?0:1,OS64_DECOR_ROUND,fills[i]};
    uint32_t painted[400*180],inherited[400*180],split[400*180],background[400*180];
    os64_decor_surface_t a={painted,400,180,400},b={inherited,400,180,400},c={split,400,180,400},d={background,400,180,400};
    os64_decor_rect_t frame={0,0,400,180};
    for(unsigned fill=0;fill<4;++fill){
        for(unsigned i=0;i<4;++i){
            h->buttons[i].face=fill==0 || fill==3?0:fill==1?fills[i]:OS64_DECOR_FACE_TRANSPARENT;
            h->buttons[i].shape=fill==3?OS64_DECOR_BARE:OS64_DECOR_ROUND;
            h->symbols[i]=(os64_decor_symbol_t){0};
        }
        void *base;size_t base_size;os64_decor_view_t base_view;
        assert(os64_decor_restyle(bytes,length,h,&base,&base_size)==OS64_FONT_OK);
        assert(os64_decor_validate(base,base_size,&base_view));
        for(unsigned version=4;version<=5;++version){
            if(version==4 && fill!=0 && fill!=3)continue;
            size_t old_size;void *old=legacy_bundle(base,base_size,version,&old_size);
            os64_decor_view_t old_view;assert(os64_decor_validate(old,old_size,&old_view));
            for(unsigned active=0;active<2;++active)for(unsigned state=0;state<4;++state){
                os64_decor_state_t st={state==1?1:0,state==2?1:0,state==3?30:0,false};
                memset(painted,0,sizeof(painted));memset(inherited,0,sizeof(inherited));
                assert(os64_decor_paint(&base_view,&a,frame,frame,true,active,false,"Title",5,false,&st));
                assert(os64_decor_paint(&old_view,&b,frame,frame,true,active,false,"Title",5,false,&st));
                assert(!memcmp(painted,inherited,sizeof(painted)));
            }
            void *upgraded;size_t upgraded_size;
            assert(os64_decor_restyle(old,old_size,h,&upgraded,&upgraded_size)==OS64_FONT_OK);
            assert(upgraded_size==base_size && !memcmp(upgraded,base,base_size));
            os64_free(upgraded);os64_free(old);
        }
        for(unsigned i=0;i<4;++i)h->symbols[i]=(os64_decor_symbol_t){i==3?0xffeefaff:0xff000000,i==3?0xffaaccee:0xff223344};
        void *custom;size_t size;os64_decor_view_t v;
        assert(os64_decor_restyle(bytes,length,h,&custom,&size)==OS64_FONT_OK);
        assert(os64_decor_validate(custom,size,&v));
        os64_decor_layout_t layout;assert(os64_decor_layout(v.header,400,180,true,&layout));
        os64_decor_header_t blank=*h;for(unsigned i=0;i<4;++i)blank.buttons[i].action=0;
        void *empty;size_t empty_size;os64_decor_view_t empty_view;
        assert(os64_decor_restyle(bytes,length,&blank,&empty,&empty_size)==OS64_FONT_OK);
        assert(os64_decor_validate(empty,empty_size,&empty_view));
        for(unsigned active=0;active<2;++active)for(unsigned state=0;state<4;++state){
            os64_decor_state_t st={state==1?1:0,state==2?1:0,state==3?30:0,false};
            memset(painted,0,sizeof(painted));memset(inherited,0,sizeof(inherited));
            memset(split,0,sizeof(split));memset(background,0,sizeof(background));
            assert(os64_decor_paint(&v,&a,frame,frame,true,active,false,"Title",5,false,&st));
            assert(os64_decor_paint(&base_view,&b,frame,frame,true,active,false,"Title",5,false,&st));
            assert(os64_decor_paint(&empty_view,&d,frame,frame,true,active,false,"Title",5,false,&st));
            for(int y=0;y<180;y+=7)for(int x=0;x<400;x+=11)
                assert(os64_decor_paint(&v,&c,frame,(os64_decor_rect_t){x,y,11,7},true,active,false,"Title",5,false,&st));
            assert(!memcmp(painted,split,sizeof(painted)));
            for(unsigned i=0;i<4;++i){
                os64_decor_rect_t r=layout.buttons[i];int k=r.w/5;
                int x=r.x+r.w/2,y=r.y+r.h/2+(i==1?k:i==2?-k:0);
                if(state==2 && i==0){++x;++y;}
                uint32_t ink=active?h->symbols[i].active:h->symbols[i].inactive;
                if(state==3){
                    uint32_t face=active?h->active_face:h->inactive_face;
                    uint32_t under=fill==0?symbol_mix(face,0xff606060):fill==1?symbol_mix(fills[i],face):background[y*400+x];
                    ink=symbol_mix(under,ink);
                }
                assert(painted[y*400+x]==ink);
                assert(painted[(r.y+2)*400+r.x+r.w/2]==inherited[(r.y+2)*400+r.x+r.w/2]);
                assert(os64_decor_hit(v.header,&layout,r.x+r.w/2,r.y+r.h/2)==i+1);
            }
            /* Ink changes are confined to symbols inside the control rectangles. */
            for(int y=0;y<180;++y)for(int x=0;x<400;++x)if(painted[y*400+x]!=inherited[y*400+x]){
                bool inside=false;
                for(unsigned i=0;i<4;++i){os64_decor_rect_t r=layout.buttons[i];if(x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h)inside=true;}
                assert(inside);
            }
        }
        void *file,*decoded;size_t file_size,decoded_size;frame_draft_t loaded;
        assert(frame_encode(&draft,custom,size,&file,&file_size)==FRAME_STORE_OK);
        assert(frame_decode(file,file_size,&loaded,&decoded,&decoded_size)==FRAME_STORE_OK);
        assert(frame_same(&loaded,&draft) && decoded_size==size && !memcmp(decoded,custom,size));
        os64_free(file);os64_free(decoded);
        uint64_t fingerprint=os64_decor_fingerprint(custom,size);
        frame_test_deny_next_alloc();void *failed=(void *)1;size_t failed_size=1;
        assert(os64_decor_restyle(custom,size,h,&failed,&failed_size)==OS64_FONT_NO_MEMORY && !failed && !failed_size);
        assert(os64_decor_fingerprint(custom,size)==fingerprint);
        ((os64_decor_header_t *)custom)->symbols[0].active=0x01020304;
        assert(!os64_decor_validate(custom,size,&v));
        os64_free(empty);os64_free(custom);os64_free(base);
    }
    puts("symbol colors: PASS per-action active/inactive ink, inherited V4/V5 pixel parity and migration, housing isolation, disabled backgrounds, split damage, hits, save/load, allocation refusal");
}

static void exercise(const os64_font_role_view_t *role)
{
    os64_decor_header_t style;
    os64_decor_defaults(&style);
    style.button_count=0;memset(style.buttons,0,sizeof(style.buttons));
    void *bytes=NULL;size_t length=0;
    assert(os64_decor_prepare(role,&style,&bytes,&length)==OS64_FONT_OK);
    os64_decor_view_t view;
    assert(os64_decor_validate(bytes,length,&view));
    two_colors(bytes,length);
    button_colors(bytes,length);
    symbol_colors(bytes,length);
    frame_draft_t saved={.style=style};saved.font.size=24;
    strcpy(saved.font.face[0],"/missing/source.ttf");
    test_frame_storage(&saved,bytes,length);
    void *encoded=NULL,*decoded=(void *)1;size_t encoded_size=0,decoded_size=1;
    assert(frame_encode(&saved,bytes,length,&encoded,&encoded_size)==FRAME_STORE_OK);
    frame_draft_t kept=saved;deny=allocations+1;
    assert(frame_decode(encoded,encoded_size,&kept,&decoded,&decoded_size)==FRAME_STORE_MEMORY);
    assert(!decoded && !decoded_size && frame_same(&saved,&kept));
    deny=0;os64_free(encoded);
    assert(view.header->glyph_count>400);
    os64_decor_layout_t layout;
    assert(os64_decor_layout(view.header,400,150,true,&layout));
    assert(layout.insets.top>=role->row_height_px+7);
    uint32_t full[400*150],split[400*150],expected[400*150];
    for (size_t i=0;i<400*150;++i) full[i]=split[i]=expected[i]=0xff123456;
    os64_decor_surface_t a={full,400,150,400},b={split,400,150,400};
    os64_decor_rect_t frame={0,0,400,150},all={0,0,400,150};
    const char *title="AV iW Cafe\xcc\x81";
    assert(os64_decor_paint(&view,&a,frame,all,true,true,false,title,strlen(title),false,NULL));
    for (int x=0;x<400;x+=17) for (int y=0;y<150;y+=11)
        assert(os64_decor_paint(&view,&b,frame,(os64_decor_rect_t){x,y,17,11},
            true,true,false,title,strlen(title),false,NULL));
    assert(!memcmp(full,split,sizeof(full)));
    /* The client interior is outside the decoration painter's ownership. */
    assert(full[100*400+100]==0xff123456);
    memcpy(expected,full,sizeof(full));
    for (int y=layout.text.y;y<layout.text.y+layout.text.h;++y)
        for (int x=layout.text.x;x<layout.text.x+layout.text.w;++x)
            expected[y*400+x]=style.active_face;
    os64_text_layout_t text_layout={.encoding=OS64_TEXT_UTF8_WESTERN_V1,
        .fonts=role->fonts,.font_count=role->font_count,.tab_interval=512};
    os64_text_run_t *run=NULL;
    assert(os64_text_layout(role->text,(const uint8_t *)title,strlen(title),&text_layout,&run)==OS64_FONT_OK);
    os64_gui_surface_t surface={.pixels=expected,.width=400,.height=150,.pitch_px=400};
    assert(os64_text_draw(run,&surface,(os64_gui_rect_t){layout.text.x,layout.text.y,layout.text.w,layout.text.h},
        layout.text.x,layout.baseline,style.active_text)==OS64_FONT_OK);
    os64_text_run_release(run);
    assert(!memcmp(full,expected,sizeof(full)));
    /* Truncation, negative origins, and extreme clip inputs stay bounded. */
    char long_title[OS64_DECOR_TITLE_MAX];memset(long_title,'W',sizeof(long_title));
    assert(os64_decor_paint(&view,&a,(os64_decor_rect_t){-15,-10,60,100},all,
        true,false,true,long_title,sizeof(long_title),false,NULL));
    assert(os64_decor_paint(&view,&a,frame,(os64_decor_rect_t){INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX},
        true,true,false,title,strlen(title),false,NULL));
    assert(!os64_decor_paint(&view,&a,(os64_decor_rect_t){0,0,INT32_MAX,150},all,
        true,true,false,title,strlen(title),false,NULL));
    uint8_t *copy=malloc(length);assert(copy);
    for (size_t n=0;n<length;n+=length/31+1) assert(!os64_decor_validate(bytes,n,&view));
    for (size_t n=0;n<sizeof(os64_decor_header_t)/4;++n) {
        memcpy(copy,bytes,length);((uint32_t *)copy)[n]=UINT32_MAX;
        bool colors=(n>=16 && n<=21) || n==offsetof(os64_decor_header_t,seed)/4 ||
            (n>=offsetof(os64_decor_header_t,active_face2)/4 && n<=offsetof(os64_decor_header_t,inactive_border2)/4) ||
            n>=offsetof(os64_decor_header_t,symbols)/4;
        assert(os64_decor_validate(copy,length,&view)==colors);
    }
    memcpy(copy,bytes,length);
    os64_decor_header_t *h=(void *)copy;
    h->version=3;assert(!os64_decor_validate(copy,length,&view));
    h->version=OS64_DECOR_VERSION;
    os64_decor_glyph_t *g=(void *)(copy+h->glyph_offset);
    g[0].offset=UINT32_MAX;assert(!os64_decor_validate(copy,length,&view));
    memcpy(copy,bytes,length);g[1].scalar=g[0].scalar;assert(!os64_decor_validate(copy,length,&view));
    memcpy(copy,bytes,length);g[0].top=INT32_MIN;assert(!os64_decor_validate(copy,length,&view));
    memcpy(copy,bytes,length);
    h->button_count=3;h->button_size=28;h->button_gap=5;
    h->buttons[0]=(os64_decor_button_t){OS64_DECOR_PIN,0,OS64_DECOR_ROUND,0};
    h->buttons[1]=(os64_decor_button_t){OS64_DECOR_MAXIMIZE,1,OS64_DECOR_BARE,0};
    h->buttons[2]=(os64_decor_button_t){OS64_DECOR_CLOSE,1,OS64_DECOR_SQUARE,0};
    assert(os64_decor_validate(copy,length,&view));
    for (unsigned state=0;state<4;++state) {
        os64_decor_state_t visual={OS64_DECOR_CLOSE,state==1?OS64_DECOR_CLOSE:0,
            state==2?(1u<<OS64_DECOR_CLOSE):0,state==3};
        memset(full,0,sizeof(full));memset(split,0,sizeof(split));
        assert(os64_decor_paint(&view,&a,frame,all,true,state!=2,true,title,strlen(title),false,&visual));
        for (int x=0;x<400;x+=17) for (int y=0;y<150;y+=11)
            assert(os64_decor_paint(&view,&b,frame,(os64_decor_rect_t){x,y,17,11},
                true,state!=2,true,title,strlen(title),false,&visual));
        assert(!memcmp(full,split,sizeof(full)));
    }
    h->buttons[0].action=OS64_DECOR_CLOSE;assert(!os64_decor_validate(copy,length,&view));
    h->buttons[0].action=OS64_DECOR_PIN;h->buttons[0].group=2;assert(!os64_decor_validate(copy,length,&view));
    h->buttons[0].group=0;h->buttons[0].shape=3;assert(!os64_decor_validate(copy,length,&view));
    os64_decor_header_t recipe;os64_decor_defaults(&recipe);
    for(unsigned kind=0;kind<=OS64_DECOR_STIPPLE;++kind) for(unsigned direction=0;direction<2;++direction) {
        recipe.finish=kind;recipe.border_finish=OS64_DECOR_GRAIN;recipe.strength=24;
        recipe.scale=2;recipe.direction=direction;recipe.relief=2;
        void *finished=NULL,*again=NULL;size_t finished_length=0,again_length=0;
        assert(os64_decor_restyle(bytes,length,&recipe,&finished,&finished_length)==OS64_FONT_OK);
        assert(os64_decor_restyle(bytes,length,&recipe,&again,&again_length)==OS64_FONT_OK);
        assert(finished_length==again_length && !memcmp(finished,again,finished_length));
        assert(os64_decor_validate(finished,finished_length,&view));
        assert(view.header->baseline==((os64_decor_header_t *)bytes)->baseline);
        assert(!memcmp((uint8_t *)finished+view.header->glyph_offset,
            (uint8_t *)bytes+view.header->glyph_offset,view.header->tile_offset-view.header->glyph_offset));
        memset(full,0,sizeof(full));memset(split,0,sizeof(split));
        assert(os64_decor_paint(&view,&a,frame,all,true,true,false,title,strlen(title),false,NULL));
        for(int x=0;x<400;x+=17)for(int y=0;y<150;y+=11)
            assert(os64_decor_paint(&view,&b,frame,(os64_decor_rect_t){x,y,17,11},true,true,false,title,strlen(title),false,NULL));
        assert(!memcmp(full,split,sizeof(full)));
        /* Translation leaves a finish anchored to the frame, not the screen. */
        memset(split,0,sizeof(split));
        assert(os64_decor_paint(&view,&b,(os64_decor_rect_t){17,13,400,150},all,true,true,false,title,strlen(title),false,NULL));
        for(int y=0;y<120;++y)for(int x=0;x<360;++x)assert(full[y*400+x]==split[(y+13)*400+x+17]);
        os64_free(finished);os64_free(again);
    }
    memcpy(copy,bytes,length);
    h=(void *)copy;((uint32_t *)(copy+h->tile_offset))[0]&=0x00ffffff;
    assert(!os64_decor_validate(copy,length,&view));
    recipe.scale=0;void *refused=(void *)1;size_t refused_length=1;
    assert(os64_decor_restyle(bytes,length,&recipe,&refused,&refused_length)!=OS64_FONT_OK && !refused && !refused_length);
    recipe.scale=1;deny=allocations+1;
    assert(os64_decor_restyle(bytes,length,&recipe,&refused,&refused_length)==OS64_FONT_NO_MEMORY && !refused && !refused_length);
    deny=0;
    free(copy);os64_free(bytes);
}

static void controls(void)
{
    os64_decor_header_t h;os64_decor_defaults(&h);
    h.line_height=29;h.baseline=23;
    os64_decor_layout_t l;
    assert(os64_decor_layout(&h,500,200,true,&l));
    assert(os64_decor_hit(&h,&l,l.buttons[2].x,l.buttons[2].y)==OS64_DECOR_CLOSE);
    assert(os64_decor_hit(&h,&l,l.buttons[2].x+l.buttons[2].w,l.buttons[2].y)==0);
    assert(l.text.x+l.text.w<=l.buttons[0].x);
    uint32_t min=os64_decor_min_width(&h);
    assert(os64_decor_layout(&h,min,200,true,&l) && l.text.w>=24);
    assert(!os64_decor_layout(&h,min-1,200,true,&l));
    h.buttons[0]=(os64_decor_button_t){OS64_DECOR_CLOSE,0,OS64_DECOR_ROUND,0};
    h.buttons[1]=(os64_decor_button_t){OS64_DECOR_SPACER,0,0,0};
    h.buttons[2]=(os64_decor_button_t){OS64_DECOR_MINIMIZE,1,OS64_DECOR_BARE,0};
    assert(os64_decor_layout(&h,500,200,true,&l));
    assert(l.buttons[0].x<l.text.x && l.buttons[2].x>=l.text.x+l.text.w);
    assert(!os64_decor_hit(&h,&l,l.buttons[1].x,l.buttons[1].y));
    h.button_count=0;memset(h.buttons,0,sizeof(h.buttons));
    assert(os64_decor_layout(&h,500,200,true,&l));
    assert(!os64_decor_hit(&h,&l,480,10));
    os64_decor_capture_t c;
    os64_decor_capture_begin(&c,7,OS64_DECOR_CLOSE,1);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_MOVE,0,8,OS64_DECOR_CLOSE) && !c.armed);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_MOVE,0,7,OS64_DECOR_CLOSE) && c.armed);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_DOWN,1,7,OS64_DECOR_CLOSE));
    assert(os64_decor_capture_step(&c,OS64_DECOR_POINTER_UP,0,7,OS64_DECOR_CLOSE)==OS64_DECOR_CLOSE);
    assert(c.buttons==2 && !c.window);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_UP,1,7,OS64_DECOR_CLOSE) && !c.buttons);
    os64_decor_capture_begin(&c,7,OS64_DECOR_CLOSE,1);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_UP,0,7,OS64_DECOR_MINIMIZE));
    os64_decor_capture_begin(&c,7,OS64_DECOR_CLOSE,1);
    os64_decor_capture_cancel(&c);
    assert(c.buttons==1);
    assert(!os64_decor_capture_step(&c,OS64_DECOR_POINTER_UP,0,7,OS64_DECOR_CLOSE) && !c.buttons);
    puts("controls: PASS group geometry, minimum width, empty/spacer hits, capture re-entry and cancellation");
}

static void fingerprint_status(void)
{
    assert(os64_decor_fingerprint("",0)==UINT64_C(0xcbf29ce484222325));
    assert(os64_decor_fingerprint("a",1)==UINT64_C(0xaf63dc4c8601ec8c));
    assert(os64_decor_fingerprint("foobar",6)==UINT64_C(0x85944171f73967e8));
    char text[OS64_DECOR_STATUS_MAX];
    os64_decor_status_t original={UINT64_MAX,UINT64_MAX,OS64_DECOR_BYTES_MAX},out={0};
    size_t n=os64_decor_status_write(text,&original);
    assert(n<sizeof(text) && os64_decor_status_read(text,n,&out));
    assert(out.generation==original.generation && out.fingerprint==original.fingerprint && out.bytes==original.bytes);
    for(size_t i=0;i<n;++i){
        /* A complete generation-only header remains a supported legacy reply. */
        bool header_end=i && text[i-1]=='\n';
        assert(os64_decor_status_read(text,i,&out)==header_end);
    }
    text[n]='x';assert(!os64_decor_status_read(text,n+1,&out));
    text[n-2]='g';assert(!os64_decor_status_read(text,n,&out));
    original=(os64_decor_status_t){0};n=os64_decor_status_write(text,&original);
    assert(os64_decor_status_read(text,n,&out) && !out.generation && !out.bytes);
    original.bytes=sizeof(os64_decor_header_t);n=os64_decor_status_write(text,&original);
    assert(!os64_decor_status_read(text,n,&out));
    puts("decoration fingerprint: PASS FNV vectors, status bounds, legacy framing, truncation and invalid fields");
}
int main(void)
{
    fingerprint_status();
    controls();
    frame_draft_t draft={0},copy={0};
    strcpy(draft.font.face[0],"/home/fonts/chosen.ttf");draft.font.size=28;
    for(unsigned i=0;i<3;++i){frame_preset(&draft,i);assert(draft.font.size==28 && !strcmp(draft.font.face[0],"/home/fonts/chosen.ttf"));}
    copy=draft;assert(frame_same(&copy,&draft));copy.style.seed++;assert(!frame_same(&copy,&draft));
    copy=draft;copy.font.size++;assert(!frame_same(&copy,&draft));
    copy=draft;copy.style.active_face2^=0xffffff;assert(!frame_same(&copy,&draft));
    copy=draft;copy.style.inactive_border2^=0xffffff;assert(!frame_same(&copy,&draft));

    os64_text_context_t *text=NULL;
    os64_text_options_t options={.memory={NULL,allocate,release},.backend=os64_fake_font_backend()};
    assert(os64_font_context_create(&options,&text)==OS64_FONT_OK);
    os64_font_set_t *set=NULL;
    assert(os64_font_set_prepare(text,NULL,&set)==OS64_FONT_OK);
    os64_font_role_view_t role;
    assert(os64_font_set_view(set,OS64_FONT_ROLE_UI,&role)==OS64_FONT_OK);
    exercise(&role);
    os64_decor_header_t style;os64_decor_defaults(&style);
    for (size_t n=1;n<=24;++n) {
        void *bytes=(void *)1;size_t length=1;
        deny=allocations+n;
        os64_font_status_t result=os64_decor_prepare(&role,&style,&bytes,&length);
        deny=0;
        if (result==OS64_FONT_OK) os64_free(bytes);
        else assert(!bytes && !length);
    }
    os64_font_set_release(set);
    uint8_t source='P';os64_font_role_spec_t specs[3]={0};
    specs[0].pixel_height=24;specs[0].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,&source,1};
    assert(os64_font_set_prepare(text,specs,&set)==OS64_FONT_OK);
    assert(os64_font_set_view(set,OS64_FONT_ROLE_UI,&role)==OS64_FONT_OK);
    exercise(&role);
    os64_font_set_release(set);
    assert(os64_text_destroy(text)==OS64_FONT_OK);
    assert(live==0);
    puts("decoration: PASS lifetime, refusal, F2 parity, two-color endpoints and strength, finishes, restyle, split damage, translation, presets, bounded format");
    return 0;
}
