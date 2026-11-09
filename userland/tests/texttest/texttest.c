/* Public F2 API integration in the guest. Geometry/failure oracles live in
 * test_text_host; this test checks real loading, retained runs and painting. */
#include "os64/os64.h"
#include "os64/text.h"
#include "os64/text_draw.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/draw.h"
#include "os64/proc.h"

typedef union {max_align_t align;size_t size;} header;
static size_t live,peak;
static void *alloc(void *unused,size_t bytes)
{
    (void)unused;if(bytes>SIZE_MAX-sizeof(header))return NULL;
    header *h=os64_malloc(bytes+sizeof(*h));if(!h)return NULL;
    h->size=bytes;live+=bytes;if(live>peak)peak=live;return h+1;
}
static void release(void *unused,void *p,size_t bytes)
{
    (void)unused;header *h=(header *)p-1;
    if(h->size!=bytes){os64_printf("texttest: wrong free size\n");os64_exit(10);}
    live-=bytes;os64_free(h);
}
#define REQUIRE(x) do {if(!(x)){os64_printf("texttest: FAIL line %d\n",__LINE__);return 2;}}while(0)
int main(void)
{
    const char *names[]={"DejaVuSans.ttf","DejaVuSansMono.ttf","SourceSans3-Regular.otf","SourceCodePro-Regular.otf"};
    os64_text_options_t options={.memory={NULL,alloc,release},.backend=os64_freetype_backend_v1(),.cache_cap=4096};
    os64_text_context_t *context=NULL;REQUIRE(os64_text_create(&options,&context)==OS64_FONT_OK);
    os64_text_run_t *runs[8]={0};
    const uint8_t sample[]="AVATAR  iWiW  caf\xc3\xa9 / cafe\xcc\x81  q\xcc\x81";
    for(size_t f=0;f<4;f++) {
        char path[256];os64_snprintf(path,sizeof(path),"/tests/fonts/%s",names[f]);
        uint8_t *bytes=NULL;size_t length=0;
        REQUIRE(os64_slurp(path,OS64_FONT_FILE_MAX,&bytes,&length)==OS64_SLURP_OK);
        for(size_t s=0;s<2;s++) {
            os64_font_face_options_t fo={s?28:18,OS64_FONT_HINT_NORMAL};os64_text_font_t *font=NULL;
            REQUIRE(os64_text_font_open(context,bytes,length,&fo,&font)==OS64_FONT_OK);
            os64_text_layout_t lo={.fonts=&font,.font_count=1,.tab_interval=128*64};
            REQUIRE(os64_text_layout(context,sample,sizeof(sample)-1,&lo,&runs[f*2+s])==OS64_FONT_OK);
            os64_text_run_view_t v;REQUIRE(os64_text_run_view(runs[f*2+s],&v)==OS64_FONT_OK);
            REQUIRE(v.byte_count==sizeof(sample)-1 && v.caret_count<v.byte_count);
            os64_text_caret_t caret;REQUIRE(os64_text_caret(runs[f*2+s],5,OS64_TEXT_BEFORE,&caret)==OS64_FONT_OK);
            os64_printf("texttest: %s %upx advance %d/64, glyphs %lu, carets %lu\n",names[f],fo.pixel_height,v.advance_x,(unsigned long)v.glyph_count,(unsigned long)v.caret_count);
            os64_text_font_release(font);
        }
        os64_free(bytes);
    }
    REQUIRE(os64_text_destroy(context)==OS64_FONT_BUSY);
    uint32_t sw=0,sh=0;
    if(os64_gui_screen_info(&sw,&sh)==0) {
        int64_t win=os64_gui_window_create_content("F2 text runs",30,35,940,550,0);REQUIRE(win>0);
        os64_gui_surface_t surface;REQUIRE(os64_gui_window_get_surface(win,&surface)==0);
        os64_gui_rect_t clip={0,0,(int32_t)surface.width,(int32_t)surface.height};
        os64_draw_fill_rect(&surface,clip,0xfff6f3ec);
        for(size_t f=0;f<4;f++) {
            int32_t top=10+(int32_t)f*132;
            os64_draw_text(&surface,12,top,names[f],os64_strlen(names[f]),0xff335577,0xfff6f3ec);
            for(size_t s=0;s<2;s++) {
                os64_text_run_t *r=runs[f*2+s];os64_text_run_view_t v;
                REQUIRE(os64_text_run_view(r,&v)==OS64_FONT_OK);
                int32_t baseline=top+48+(int32_t)s*49;
                os64_font_rect_t selection;REQUIRE(os64_text_selection(r,0,6,&selection)==OS64_FONT_OK);
                os64_draw_fill_rect(&surface,(os64_gui_rect_t){12,baseline-(v.ascent+63)/64,
                    (selection.x1+63)/64,(v.line_height+63)/64},0xffd3e2f6);
                REQUIRE(os64_text_draw(r,&surface,clip,12,baseline,0xff202431)==OS64_FONT_OK);
                /* Baseline and caret guides use the retained run coordinates. */
                os64_draw_hline(&surface,12,baseline+2,(v.advance_x+63)/64,0xffa6afb8);
                for(size_t n=0;n<v.caret_count;n++)
                    os64_draw_vline(&surface,12+(v.carets[n].x+32)/64,baseline+2,3,0xffc35555);
            }
        }
        REQUIRE(os64_gui_window_publish(win,NULL)==0);
        os64_printf("texttest: specimen published\n");
        for(unsigned n=0;n<150;n++) os64_sleep(100);
        REQUIRE(os64_gui_window_destroy(win)==0);
    } else os64_printf("texttest: SKIP specimen (text boot)\n");
    for(size_t n=0;n<8;n++)os64_text_run_release(runs[n]);
    REQUIRE(os64_text_destroy(context)==OS64_FONT_OK && live==0);
    os64_printf("texttest: PASS peak %lu bytes\n",(unsigned long)peak);return 0;
}
