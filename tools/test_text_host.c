#include "text_internal.h"
#include "os64/text_draw.h"
#include "os64/draw.h"
#include "os64/charset.h"
#include "fake_backend.h"
#include "os64/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks,live,allocations,deny;
static const char *current="setup";
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d [%s]: %s\n",__FILE__,__LINE__,current,#x);exit(1);}}while(0)
typedef union {max_align_t align;size_t bytes;} block;
static void *alloc(void *ctx,size_t bytes)
{
    (void)ctx;allocations++;
    if (deny && allocations==deny) return NULL;
    block *b=malloc(sizeof(*b)+bytes);if(!b)return NULL;
    b->bytes=bytes;live+=bytes;return b+1;
}
static void release(void *ctx,void *p,size_t bytes)
{
    (void)ctx;block *b=(block *)p-1;CHECK(b->bytes==bytes);live-=bytes;free(b);
}
static os64_text_context_t *c;
static os64_text_context_t *create(size_t total,size_t cache)
{
    os64_text_options_t o={.memory={NULL,alloc,release},.backend=os64_fake_font_backend(),.memory_cap=total,.cache_cap=cache};
    os64_text_context_t *context=NULL;CHECK(os64_text_create(&o,&context)==OS64_FONT_OK);return context;
}
static os64_text_font_t *open_font(os64_text_context_t *ctx,uint8_t name)
{
    os64_text_font_t *f=NULL;os64_font_face_options_t o={16,OS64_FONT_HINT_NORMAL};
    CHECK(os64_text_font_open(ctx,&name,1,&o,&f)==OS64_FONT_OK);return f;
}
static bool rect_eq(os64_font_rect_t a,os64_font_rect_t b)
{return a.x0==b.x0 && a.y0==b.y0 && a.x1==b.x1 && a.y1==b.y1;}
#include "text_vectors.h"

static os64_text_run_t *layout(os64_text_context_t *ctx,os64_text_font_t *f,
    const uint8_t *bytes,size_t length,os64_text_encoding_t encoding,int32_t cell)
{
    os64_text_layout_t o={.fonts=&f,.font_count=1,.encoding=encoding,.tab_interval=512,.cell_advance=cell};
    os64_text_run_t *r=NULL;CHECK(os64_text_layout(ctx,bytes,length,&o,&r)==OS64_FONT_OK);return r;
}
static void lifetimes(void)
{
    current="cache ownership";c=create(0,1);os64_text_font_t *f=open_font(c,'P');
    os64_text_run_t *a=layout(c,f,(const uint8_t *)"iW",2,0,0);
    os64_text_run_t *b=layout(c,f,(const uint8_t *)"i",1,0,0);
    CHECK(a->images[0]==b->images[0] && c->cache_bytes>c->cache_cap);
    text_image *image=a->images[0];uint8_t saved=image->view.coverage[0];
    CHECK(text_evict(c)==0 && image->view.coverage[0]==saved);
    CHECK(os64_text_destroy(c)==OS64_FONT_BUSY);
    os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_BUSY);
    os64_text_run_release(a);CHECK(b->images[0]->view.coverage[0]==saved);
    os64_text_run_release(b);CHECK(c->cache_bytes==0 && c->fonts==NULL);
    CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    current="reload identity";c=create(0,0);
    uint8_t source='P';os64_font_face_options_t o={16,0};
    CHECK(os64_text_font_open(c,&source,1,&o,&f)==OS64_FONT_OK);
    source='M';os64_text_font_t *g=NULL;CHECK(os64_text_font_open(c,&source,1,&o,&g)==OS64_FONT_OK);
    a=layout(c,f,(uint8_t *)"i",1,0,0);b=layout(c,g,(uint8_t *)"i",1,0,0);
    CHECK(a->view.advance_x==192 && b->view.advance_x==512);
    CHECK(a->view.glyphs[0].font_identity!=b->view.glyphs[0].font_identity);
    CHECK(a->images[0]!=b->images[0]);
    os64_text_run_release(a);os64_text_run_release(b);os64_text_font_release(f);os64_text_font_release(g);
    CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
}
static void failures(void)
{
    current="allocation-denial cleanup";
    for(size_t point=1;point<30;point++) {
        c=create(0,0);os64_text_font_t *f=open_font(c,'P');
        os64_text_run_t *held=layout(c,f,(uint8_t *)"W",1,0,0);
        size_t baseline=allocations;deny=baseline+point;
        os64_text_layout_t o={.fonts=&f,.font_count=1,.tab_interval=512};
        os64_text_run_t *r=(void *)1;
        os64_font_status_t status=os64_text_layout(c,(uint8_t *)"AVeWi",5,&o,&r);
        CHECK(status==OS64_FONT_OK || (status==OS64_FONT_NO_MEMORY && !r));
        CHECK(held->view.advance_x==576 && held->images[0]->view.coverage[0]==34);
        deny=0;os64_text_run_release(r);os64_text_run_release(held);os64_text_font_release(f);
        CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    }
    current="context cap with pinned run";c=create(0,0);os64_text_font_t *f=open_font(c,'P');
    os64_text_run_t *held=layout(c,f,(uint8_t *)"W",1,0,0);
    c->cap=c->live;size_t before=allocations;
    os64_text_layout_t o={.fonts=&f,.font_count=1,.tab_interval=512};os64_text_run_t *r=(void *)1;
    CHECK(os64_text_layout(c,(uint8_t *)"i",1,&o,&r)==OS64_FONT_LIMIT && !r);
    CHECK(allocations==before && held->view.advance_x==576);
    c->cap=OS64_TEXT_MEMORY_DEFAULT;r=layout(c,f,(uint8_t *)"i",1,0,0);
    os64_text_run_release(r);os64_text_run_release(held);os64_text_font_release(f);
    CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    current="cap recovers by evicting unpinned entries";c=create(0,0);f=open_font(c,'P');
    r=layout(c,f,(uint8_t *)"AVeWi",5,0,0);os64_text_run_release(r);
    CHECK(c->cache_bytes>0);c->cap=c->live+256;
    r=layout(c,f,(uint8_t *)"j",1,0,0);CHECK(r->view.advance_x==256);
    os64_text_run_release(r);os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
}

static os64_font_status_t small_engine(const os64_font_engine_options_t *options,os64_font_engine_t **out)
{
    os64_font_engine_options_t o=*options;o.memory_cap=400;
    return os64_fake_font_backend()->engine_create(&o,out);
}
static os64_font_status_t reverse_pair(os64_font_face_t *face,uint32_t left,uint32_t right,os64_font_pos_t *out)
{
    (void)face;(void)left;(void)right;*out=-10000;return OS64_FONT_OK;
}
static unsigned lookup_refusals,lookup_calls;
static os64_font_status_t refusing_lookup(os64_font_face_t *face,uint32_t scalar,uint32_t *out)
{
    lookup_calls++;
    if(lookup_refusals) {lookup_refusals--;*out=0;return OS64_FONT_LIMIT;}
    return os64_fake_font_backend()->lookup(face,scalar,out);
}
static void backend_limits(void)
{
    current="backend cap distinct from context cap";
    os64_font_backend_t backend=*os64_fake_font_backend();backend.engine_create=small_engine;
    os64_text_options_t options={.memory={NULL,alloc,release},.backend=&backend};
    CHECK(os64_text_create(&options,&c)==OS64_FONT_OK);
    os64_text_font_t *f=open_font(c,'P');os64_text_run_t *held=layout(c,f,(uint8_t *)"W",1,0,0),*r=NULL;
    os64_text_layout_t o={.fonts=&f,.font_count=1,.tab_interval=512};
    os64_font_status_t result=os64_text_layout(c,(uint8_t *)"AV",2,&o,&r);
    CHECK(result==OS64_FONT_LIMIT && !r);
    CHECK(c->refusal==OS64_FONT_OK && c->live<c->cap && held->view.advance_x==576);
    os64_text_run_release(held);
    r=layout(c,f,(uint8_t *)"AV",2,0,0);CHECK(r->view.advance_x==832);
    os64_text_run_release(r);os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    current="lookup cap retry requires eviction and is bounded";
    backend=*os64_fake_font_backend();backend.lookup=refusing_lookup;options.backend=&backend;
    CHECK(os64_text_create(&options,&c)==OS64_FONT_OK);f=open_font(c,'P');
    held=layout(c,f,(uint8_t *)"W",1,0,0);os64_text_run_release(held);
    lookup_calls=0;lookup_refusals=1;
    r=layout(c,f,(uint8_t *)"A",1,0,0);CHECK(lookup_calls==2);
    lookup_calls=0;lookup_refusals=1;
    CHECK(os64_text_layout(c,(uint8_t *)"V",1,&o,&held)==OS64_FONT_LIMIT && !held && lookup_calls==1);
    os64_text_run_release(r);lookup_calls=0;lookup_refusals=2;
    CHECK(os64_text_layout(c,(uint8_t *)"V",1,&o,&r)==OS64_FONT_LIMIT && !r && lookup_calls==2);
    lookup_refusals=0;
    os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    current="kerning cannot reverse insertion boundaries";
    backend=*os64_fake_font_backend();backend.pair_adjust=reverse_pair;options.backend=&backend;
    CHECK(os64_text_create(&options,&c)==OS64_FONT_OK);f=open_font(c,'P');
    r=layout(c,f,(uint8_t *)"AV",2,0,0);
    CHECK(r->view.advance_x==896 && r->view.carets[1].x==448);
    os64_text_run_release(r);os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
}
static void geometry(void)
{
    current="clips, blending and atomic paint failure";c=create(0,0);os64_text_font_t *f=open_font(c,'P');
    os64_text_layout_t overflow={.fonts=&f,.font_count=1,.tab_interval=512,.encoding=OS64_TEXT_LATIN1,.cell_advance=INT32_MAX};
    os64_text_run_t *rejected=NULL;size_t before=allocations;
    CHECK(os64_text_layout(c,(uint8_t *)"WW",2,&overflow,&rejected)==OS64_FONT_LIMIT && !rejected);
    CHECK(allocations==before);

    os64_text_run_t *r=layout(c,f,(uint8_t *)"iW",2,0,0);
    uint32_t pixels[20*20];for(size_t n=0;n<400;n++)pixels[n]=0xff000000;
    os64_gui_surface_t s={pixels,16,20,20};
    CHECK(os64_text_draw(r,&s,(os64_gui_rect_t){1,1,5,9},0,10,0xffffff)==OS64_FONT_OK);
    CHECK(pixels[1*20+1]==0xff111111 && pixels[1*20+3]==0xff222222);
    CHECK(pixels[0]==0xff000000 && pixels[1*20+6]==0xff000000 && pixels[19]==0xff000000);
    uint32_t copy[400];memcpy(copy,pixels,sizeof(copy));
    CHECK(os64_text_draw(r,&s,(os64_gui_rect_t){0,0,16,20},INT32_MAX,10,0xffffff)==OS64_FONT_LIMIT);
    CHECK(memcmp(copy,pixels,sizeof(copy))==0);
    CHECK(os64_text_draw(r,&s,(os64_gui_rect_t){0,0,0,20},INT32_MAX,10,0xffffff)==OS64_FONT_OK);
    os64_text_run_release(r);
    r=layout(c,f,(uint8_t *)"WW",2,OS64_TEXT_LATIN1,256);
    for(size_t n=0;n<400;n++)pixels[n]=0xff000000;
    CHECK(os64_text_draw(r,&s,(os64_gui_rect_t){0,0,16,20},0,12,0xffffff)==OS64_FONT_OK);
    CHECK(pixels[3*20+7]==0xff222222 && pixels[3*20+8]==0xff000000);
    os64_text_run_release(r);
    os64_text_layout_t o={.fonts=&f,.font_count=1,.tab_origin=100,.tab_interval=512};
    CHECK(os64_text_layout(c,(uint8_t *)"\t",1,&o,&r)==OS64_FONT_OK && r->view.advance_x==100);
    os64_text_run_release(r);
    o.tab_origin=INT32_MAX;o.tab_interval=INT32_MAX;
    CHECK(os64_text_layout(c,(uint8_t *)"i\tW",3,&o,&r)==OS64_FONT_LIMIT && !r);
    CHECK(os64_text_layout(c,(uint8_t *)"\n",1,&o,&r)==OS64_FONT_BAD_ARGUMENT && !r);
    CHECK(os64_text_layout(c,NULL,1,&o,&r)==OS64_FONT_BAD_ARGUMENT && !r);
    CHECK(os64_text_layout(c,(uint8_t *)"x",OS64_TEXT_BYTES_MAX+1u,&o,&r)==OS64_FONT_LIMIT && !r);
    os64_text_font_t *dups[]={f,f};o.fonts=dups;o.font_count=2;
    CHECK(os64_text_layout(c,NULL,0,&o,&r)==OS64_FONT_BAD_ARGUMENT && !r);
    os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
}
static void unicode_profile(void)
{
    current="complete W1 composition table";
    /* Independently selected expected rows generated from pinned source by the
     * Python runner; the test encodes the original source and checks decoding. */
    #include "text_composition_tests.h"
    const uint8_t invalid[]={0xc0,0x80,0xed,0xa0,0x80,0xf4,0x90,0x80,0x80};
    for(size_t n=0;n<sizeof(invalid);n++) {
        text_cluster d=text_decode(invalid,sizeof(invalid),n,0,false);
        CHECK(d.end==n+1 && d.scalar==TEXT_MARKER);
    }
    const uint8_t nonletter[]={0xc3,0x97,0xcc,0x81};
    text_cluster d=text_decode(nonletter,4,0,0,false);CHECK(d.end==2 && !d.extra_marker);
    d=text_decode(nonletter,4,2,0,false);CHECK(d.end==4 && d.scalar==TEXT_MARKER);
    uint8_t *long_marks=malloc(OS64_TEXT_BYTES_MAX);CHECK(long_marks!=NULL);
    long_marks[0]='q';for(size_t n=1;n+1<OS64_TEXT_BYTES_MAX;n+=2){long_marks[n]=0xcc;long_marks[n+1]=0x81;}
    d=text_decode(long_marks,OS64_TEXT_BYTES_MAX-1,0,0,false);
    CHECK(d.end==OS64_TEXT_BYTES_MAX-1 && d.scalar=='q' && d.extra_marker);free(long_marks);
}
static void bitmap(void)
{
    current="bitmap and scalable cell graphics";c=create(0,0);os64_text_font_t *f=NULL;
    CHECK(os64_text_font_bitmap(c,&f)==OS64_FONT_OK);
    os64_text_run_t *r=layout(c,f,(uint8_t *)"iW",2,OS64_TEXT_LATIN1,0);
    CHECK(r->view.advance_x==1024);os64_text_run_release(r);
    for(unsigned charset=0;charset<2;charset++) for(unsigned byte=0;byte<256;byte++) {
        if(byte==10)continue;
        uint32_t old_pixels[128],new_pixels[128];
        for(size_t n=0;n<128;n++)old_pixels[n]=new_pixels[n]=0xff223344;
        os64_gui_surface_t old_surface={old_pixels,8,16,8},new_surface={new_pixels,8,16,8};
        uint8_t b=(uint8_t)byte;
        os64_draw_text_charset(&old_surface,0,0,(char *)&b,1,0xffddeeff,0xff223344,
            charset?OS64_CHARSET_CP437:OS64_CHARSET_LATIN1);
        r=layout(c,f,&b,1,charset?OS64_TEXT_CP437:OS64_TEXT_LATIN1,512);
        CHECK(os64_text_draw(r,&new_surface,(os64_gui_rect_t){0,0,8,16},0,12,0xffddeeff)==OS64_FONT_OK);
        /* Missing box characters gain procedural coverage by the F2 contract;
         * the old API remains the oracle for the compatibility glyphs. */
        uint32_t cp=charset?os64_cp437_codepoint(b):b;
        if(!(charset && cp>=0x2500 && cp<=0x259f && !text_bitmap_rows(cp)))
            CHECK(memcmp(old_pixels,new_pixels,sizeof(old_pixels))==0);
        os64_text_run_release(r);
    }

    os64_text_font_release(f);f=open_font(c,'P');
    const uint8_t lines[]={0xc4,0xb3,0xdb,0xdc,0xb0};
    r=layout(c,f,lines,sizeof(lines),OS64_TEXT_CP437,20*64);
    CHECK(r->view.advance_x==100*64 && r->view.glyph_count==5);
    const os64_font_glyph_view_t *a=&r->images[0]->view,*b=&r->images[1]->view;
    CHECK(a->width==20 && a->height==16 && a->coverage[8*20]==255 && a->coverage[8*20+19]==255);
    CHECK(b->coverage[10]==255 && b->coverage[15*20+10]==255);
    CHECK(r->images[2]->view.coverage[0]==255 && r->images[2]->view.coverage[319]==255);
    CHECK(r->images[3]->view.coverage[0]==0 && r->images[3]->view.coverage[319]==255);
    os64_text_run_release(r);os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    /* The outer and inner rails each turn through the top-left double corner. */
    CHECK(text_box_pixel(0x2554,8,12,24,32)==255);
    CHECK(text_box_pixel(0x2554,14,18,24,32)==255);
    CHECK(text_box_pixel(0x2554,12,16,24,32)==0);
    CHECK(text_box_pixel(0x2554,8,11,24,32)==0);
    CHECK(text_box_pixel(0x250c,11,15,24,32)==255);
    CHECK(text_box_pixel(0x250c,11,14,24,32)==0);
    CHECK(text_box_pixel(0x250c,10,15,24,32)==0);
    /* Vertical double rules must survive cells narrower than their rail gap. */
    for(uint32_t width=1;width<=8;width++) {
        size_t upper=0,lower=0;
        for(uint32_t x=0;x<width;x++) {
            upper+=text_box_pixel(0x2551,x,0,width,16)!=0;
            lower+=text_box_pixel(0x2551,x,15,width,16)!=0;
        }
        CHECK(upper>0 && upper==lower);
    }
    for(uint32_t cp=0x2500;cp<=0x259f;cp++) {
        size_t ink=0;for(uint32_t y=0;y<32;y++)for(uint32_t x=0;x<24;x++)ink+=text_box_pixel(cp,x,y,24,32)!=0;
        CHECK(ink>0 && ink<=24*32);
    }
}

#ifdef TEXT_REAL
static void real_fonts(const char *dir)
{
    current="real FreeType integration";
    const char *names[]={"DejaVuSans.ttf","DejaVuSansMono.ttf","SourceSans3-Regular.otf","SourceCodePro-Regular.otf"};
    for(size_t n=0;n<4;n++) {
        char path[1024];snprintf(path,sizeof(path),"%s/%s",dir,names[n]);
        FILE *file=fopen(path,"rb");CHECK(file!=NULL);CHECK(fseek(file,0,SEEK_END)==0);
        long length=ftell(file);CHECK(length>0);rewind(file);
        uint8_t *source=malloc((size_t)length);CHECK(source!=NULL);
        CHECK(fread(source,1,(size_t)length,file)==(size_t)length);fclose(file);
        os64_text_options_t options={.memory={NULL,alloc,release},.backend=os64_freetype_backend_v1(),.cache_cap=1024};
        CHECK(os64_text_create(&options,&c)==OS64_FONT_OK);
        os64_font_face_options_t fo={32,OS64_FONT_HINT_NORMAL};os64_text_font_t *f=NULL;
        CHECK(os64_text_font_open(c,source,(size_t)length,&fo,&f)==OS64_FONT_OK);free(source);
        const uint8_t composed[]={0xc3,0xa9},decomposed[]={'e',0xcc,0x81};
        os64_text_run_t *a=layout(c,f,composed,sizeof(composed),0,0),*b=layout(c,f,decomposed,sizeof(decomposed),0,0);
        CHECK(a->view.glyph_count==1 && b->view.glyph_count==1 && a->view.advance_x==b->view.advance_x);
        CHECK(a->images[0]==b->images[0] && b->view.carets[1].byte_offset==3);
        uint32_t p[128*64],q[128*64];memset(p,0,sizeof(p));memset(q,0,sizeof(q));
        os64_gui_surface_t ps={p,128,64,128},qs={q,128,64,128};
        CHECK(os64_text_draw(a,&ps,(os64_gui_rect_t){0,0,128,64},4,40,0xffffff)==OS64_FONT_OK);
        CHECK(os64_text_draw(b,&qs,(os64_gui_rect_t){0,0,128,64},4,40,0xffffff)==OS64_FONT_OK);
        CHECK(memcmp(p,q,sizeof(p))==0);
        size_t ink=0;for(size_t j=0;j<128*64;j++)ink+=p[j]!=0;CHECK(ink>0);
        os64_text_run_release(a);os64_text_run_release(b);
        a=layout(c,f,(uint8_t *)"AV",2,0,0);
        CHECK(a->view.caret_count==3 && a->view.carets[1].x==a->view.glyphs[1].x);
        printf("real %s: AV advance %d/64, V origin %d/64, ink pixels %zu\n",names[n],a->view.advance_x,a->view.glyphs[1].x,ink);
        os64_text_font_release(f);CHECK(os64_text_destroy(c)==OS64_FONT_BUSY);
        CHECK(os64_text_draw(a,&ps,(os64_gui_rect_t){0,0,128,64},4,40,0xffffff)==OS64_FONT_OK);
        os64_text_run_release(a);CHECK(os64_text_destroy(c)==OS64_FONT_OK && live==0);
    }
}
#endif
int main(int argc,char **argv)
{
    (void)argc;(void)argv;
    c=create(0,0);frozen_vectors();CHECK(os64_text_destroy(c)==OS64_FONT_OK);CHECK(live==0);
    lifetimes();failures();backend_limits();geometry();unicode_profile();bitmap();
#ifdef TEXT_REAL
    CHECK(argc==2);real_fonts(argv[1]);
#endif
    printf("PASS: %zu checks, F2 vectors, ownership, failure, drawing and profile\n",checks);return 0;
}
