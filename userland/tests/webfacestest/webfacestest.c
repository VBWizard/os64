/* Exercise installed family resolution and paint the same faces a page uses.
 * Each borrowed list is consumed before the next family request. */
#include "os64/os64.h"
#include "os64/font_config.h"
#include "os64/text_draw.h"
#include "os64/draw.h"
#include "os64/str.h"

static void *allocate(void *user, size_t bytes) { (void)user; return os64_malloc(bytes); }
static void release(void *user, void *ptr, size_t bytes) { (void)user; (void)bytes; os64_free(ptr); }
static int sample(os64_font_family_cache_t *cache, os64_font_family_t family,
    unsigned style, uint32_t size, const char *text, os64_gui_surface_t *surface,
    int32_t x, int32_t y, int32_t *advance)
{
    os64_font_family_list_t list = {.generic=family};
    os64_font_role_view_t view;
    os64_font_status_t status = os64_font_family_open(cache,&list,style&1,style&2,size,&view);
    if (status) return status;
    os64_text_layout_t layout = {.fonts=view.fonts,.font_count=view.font_count,.tab_interval=1024};
    os64_text_run_t *run=NULL;
    status=os64_text_layout(view.text,(const uint8_t *)text,os64_strlen(text),&layout,&run);
    if (status) return status;
    os64_text_run_view_t measured;
    status=os64_text_run_view(run,&measured);
    if (!status && advance) *advance=(measured.advance_x+63)/64;
    if (!status && surface) {
        os64_gui_rect_t clip={0,0,(int32_t)surface->width,(int32_t)surface->height};
        status=os64_text_draw(run,surface,clip,x,y,0xff202838);
    }
    os64_text_run_release(run);
    return status;
}
int main(void)
{
    os64_text_options_t options={.memory={NULL,allocate,release},
        .memory_cap=128u*1024u*1024u,.cache_cap=8u*1024u*1024u};
    os64_text_context_t *text=NULL;
    os64_font_family_cache_t *cache=NULL;
    os64_font_config_t config;
    os64_font_config_error_t error;
    int result=1;
    int64_t window=-1;
    if (os64_font_context_create(&options,&text)) goto done;
    if (os64_font_config_read(&config,&error) ||
        os64_font_config_family_prepare(text,&config,&cache,&error)) {
        os64_printf("webfacestest: config %s line %lu font status %u\n",
            os64_font_config_status_name(error.status),(unsigned long)error.line,(unsigned)error.font_status);
        goto done;
    }
    uint32_t width,height;
    os64_gui_surface_t surface, *canvas=NULL;
    if (!os64_gui_screen_info(&width,&height)) {
        window=os64_gui_window_create_content("Web faces: serif / sans / mono",25,30,960,640,0);
        if (window<0 || os64_gui_window_get_surface(window,&surface)) goto done;
        canvas=&surface;
        os64_draw_fill_rect(canvas,(os64_gui_rect_t){0,0,(int32_t)surface.width,(int32_t)surface.height},0xfff6f3ec);
    }
    const char *families[]={"SERIF","SANS","MONO"};
    const char *styles[]={"Regular","Bold","Italic / Oblique","Bold + Italic"};
    const uint32_t sizes[]={12,18,26};
    for (unsigned f=0;f<3;++f) {
        int32_t x=16+(int32_t)f*316;
        if (canvas) os64_draw_text(canvas,x,12,families[f],os64_strlen(families[f]),0xff335577,0xfff6f3ec);
        for (unsigned s=0;s<4;++s) {
            int32_t y=45+(int32_t)s*130;
            if (canvas) os64_draw_text(canvas,x,y,styles[s],os64_strlen(styles[s]),0xff657080,0xfff6f3ec);
            for (unsigned n=0;n<3;++n)
                if (sample(cache,(os64_font_family_t)f,s,sizes[n],"Yonder AV 0123",canvas,x,y+34+(int32_t)n*31,NULL)) goto done;
        }
        /* The style sequence models <b>bold <i>both</i></b><i> italic</i>. */
        const unsigned nested_styles[]={0,1,3,1,0,2};
        const char *segments[]={"plain ","bold ","both ","bold ","plain ","italic"};
        for (unsigned n=0;n<6;++n) {
            int32_t advance;
            if (sample(cache,(os64_font_family_t)f,nested_styles[n],12,segments[n],canvas,x,603,&advance)) goto done;
            x+=advance;
        }
    }
    if (canvas) {
        if (os64_gui_window_publish(window,NULL)) goto done;
        os64_printf("webfacestest: specimen ready, 12 faces x 3 sizes plus nested styles\n");
        os64_sleep(30000);
    }
    result=0;
done:
    if (window>=0) os64_gui_window_destroy(window);
    os64_font_family_cache_destroy(cache);
    if (os64_text_destroy(text)) result=1;
    os64_printf("webfacestest: %s\n",result ? "FAIL" : "PASS");
    return result;
}
