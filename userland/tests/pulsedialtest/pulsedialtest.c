// Deterministic preview of Pulse's production Instruments renderer.
// Values are examples: no sampling or configuration writes.
#include "../../apps/pulse/pulse_instruments.h"
#include "os64/os64.h"
#include "os64/ui_window.h"
#include "os64/slurp.h"
#include "os64/font_adopt.h"

static os64_ui_t ui;
static os64_draw_ctx_t ctx;
static os64_ui_widget_t root;
static os64_ui_class_t canvas_class;
static pulse_instruments_t *renderer;
static pulse_config_t config;
static pulse_instrument_readings_t readings;
static bool compact, unavailable, paint_failed;

static void example(void)
{
    readings=(pulse_instrument_readings_t){.current={
        .cpu_valid=true,.ram_valid=true,.disk_valid=true,.net_valid=true,
        .cpu_percent=37,.ram_used=3ULL<<30,.ram_available=5ULL<<30,.ram_total=8ULL<<30,
        .disk_used=120ULL<<30,.disk_available=380ULL<<30,.disk_total=500ULL<<30,
        .download=1572864,.upload=262144,.network_scale=2097152},
        .peak_valid={true,true,false,true,true},.peak={82,61,0,1900000,900000}};
    if(unavailable)readings.current=(pulse_sample_t){.network_scale=2097152};
}
static bool font(unsigned px)
{
    uint8_t *bytes=NULL;size_t length=0;
    if(os64_slurp("/etc/fonts/DejaVuSans.ttf",4u*1024*1024,&bytes,&length)!=OS64_SLURP_OK)return false;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT]={0};
    for(unsigned i=0;i<OS64_FONT_ROLE_COUNT;++i)specs[i].pixel_height=16;
    specs[OS64_FONT_ROLE_UI]=(os64_font_role_spec_t){.pixel_height=px,
        .primary={OS64_FONT_SOURCE_OUTLINE,bytes,length}};
    specs[OS64_FONT_ROLE_DOCUMENT]=(os64_font_role_spec_t){.pixel_height=px*2/3<16?16:px*2/3,
        .primary={OS64_FONT_SOURCE_OUTLINE,bytes,length}};
    os64_font_set_t *candidate=NULL;
    os64_font_status_t rc=os64_font_set_prepare(os64_ui_font_context(&ui),specs,&candidate);
    os64_free(bytes);if(rc)return false;
    os64_font_consumer_t consumer;os64_ui_font_consumer(&ui,&consumer);
    rc=os64_font_adopt(candidate,&consumer,1,NULL);os64_font_set_release(candidate);return rc==OS64_FONT_OK;
}
static void paint(os64_ui_widget_t *w,os64_draw_ctx_t *draw,const os64_ui_theme_t *theme)
{
    (void)draw;(void)theme;
    pulse_instrument_layout_t layout;
    bool ok=pulse_instruments_layout(&ui,&config,compact,w->bounds,&layout) &&
        pulse_instruments_paint(renderer,&ctx.surf,&layout,&config,&readings);
    if(!ok){
        os64_draw_fill_rect(&ctx.surf,w->bounds,0xff402030);
        os64_draw_text_clipped(&ctx.surf,w->bounds,8,12,"Preview cannot fit or draw",25,0xffffffff,0xff402030);
        if(!paint_failed)os64_printf("pulsedialtest: rendering refused at %ux%u\n",ctx.surf.width,ctx.surf.height);
    }
    paint_failed=!ok;
}
static void resized(os64_ui_t *u)
{
    root.bounds=(os64_gui_rect_t){0,0,(int32_t)ctx.surf.width,(int32_t)ctx.surf.height};
    os64_ui_mark_dirty(u,&root);
}
static bool apply_view(const pulse_config_t *next,bool next_compact)
{
    os64_gui_rect_t minimum;
    if(!pulse_instruments_min_size(&ui,next,next_compact,&minimum) ||
       os64_gui_window_set_min_size(ctx.win,(uint32_t)minimum.w,(uint32_t)minimum.h)){
        os64_complain("pulsedialtest: could not install the content minimum; keeping the current view");
        return false;
    }
    // Raising the minimum can grow the native surface immediately. Refresh
    // before changing the view or drawing through the previous surface.
    if(os64_draw_ctx_refresh(&ctx)){
        os64_complain("pulsedialtest: surface refresh failed after setting minimum");
        ui.quit=true;return false;
    }
    config=*next;compact=next_compact;resized(&ui);
    return true;
}
int main(int argc,char **argv)
{
    unsigned px=24;
    for(int i=1;i<argc;++i){
        if(!os64_strcmp(argv[i],"--compact"))compact=true;
        else if(!os64_strcmp(argv[i],"--26"))px=26;
        else {os64_printf("Usage: pulsedialtest [--compact] [--26]\nKeys: c compact, 1-4 toggle meters, u unavailable, n units, q quit. Ctrl+Alt+right-drag to resize.\n");return 1;}
    }
    pulse_config_defaults(&config);example();
    int64_t window=0;
    os64_ui_window_options_t options={.title="Pulse preview (example data)",.x=80,.y=80,
        .content_width=compact?(px==26?360:320):600,.content_height=compact?(px==26?360:320):560,
        .flags=OS64_GUI_WINDOW_PINNED};
    if(os64_ui_window_open(&ui,&ctx,&window,&options)!=OS64_UI_OK){
        os64_complain("pulsedialtest: window creation failed (%ld)",(long)window);
        if(window>0)(void)os64_gui_window_destroy(window);
        return 1;
    }
    if(!font(px)){os64_complain("pulsedialtest: could not prepare font");goto fail;}
    renderer=pulse_instruments_create(&ui);
    if(!renderer){os64_complain("pulsedialtest: renderer allocation failed");goto fail;}
    os64_ui_panel(&root);canvas_class=*root.cls;canvas_class.paint=paint;root.cls=&canvas_class;
    os64_ui_set_root(&ui,&root);ui.on_resize=resized;
    if(!apply_view(&config,compact))goto fail;
    char message[160];
    os64_snprintf(message,sizeof(message),"pulsedialtest: %s %ux%u, %upx; c compact, 1-4 meters, u unavailable, n units, q quit\n",
        compact?"compact":"normal",ctx.surf.width,ctx.surf.height,px);
    os64_printf("%s",message);os64_debug_log(message);
    os64_ui_paint(&ui);
    while(!ui.quit){
        os64_gui_event_t ev;if(os64_gui_event_wait(window,&ev)!=1)break;
        do {
            if(ev.type==OS64_GUI_EVENT_KEY_DOWN){
                char key=ev.key.ascii;
                if(key=='q')ui.quit=true;
                else if(key=='c')(void)apply_view(&config,!compact);
                else if(key>='1' && key<='4'){
                    pulse_config_t next=config;next.show[key-'1']=!next.show[key-'1'];
                    (void)apply_view(&next,compact);
                }
                else if(key=='u'){unavailable=!unavailable;example();}
                else if(key=='n')config.units=(config.units+1)%PULSE_UNITS;
                os64_ui_mark_dirty(&ui,&root);
            }
            if(ui.quit)break;
            os64_ui_dispatch(&ui,&ev);
        }while(!ui.quit && os64_gui_event_poll(window,&ev)==1);
        if(!ui.quit)os64_ui_paint(&ui);
    }
    os64_snprintf(message,sizeof(message),"pulsedialtest: closed; peak drawing allocation %lu bytes",(unsigned long)pulse_instruments_peak_bytes(renderer));
    os64_printf("%s\n",message);os64_debug_log(message);
    pulse_instruments_destroy(renderer);os64_ui_font_release(&ui);os64_gui_window_destroy(window);return 0;
fail:
    pulse_instruments_destroy(renderer);os64_ui_font_release(&ui);os64_gui_window_destroy(window);return 1;
}
