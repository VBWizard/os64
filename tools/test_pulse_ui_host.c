/* Drive Pulse's real widgets, layout and font planner on a host surface.
 * The shared font harness supplies the allocator and real face loader. */
#include "os64/ui.h"
static uint64_t pulse_test_parent_flags, pulse_test_create_flags, pulse_test_dialog_flags;
static bool pulse_test_pin_failure;
static unsigned pulse_test_focuses;
static bool pulse_test_state_failure;
static os64_draw_ctx_t *pulse_test_live;
static int64_t pulse_test_exact_error;
static unsigned pulse_test_exact_failures,pulse_test_exact_calls;
static int64_t pulse_test_state(int64_t window,os64_gui_window_state_t *state)
{
    *state=(os64_gui_window_state_t){.flags=window==2?pulse_test_dialog_flags:pulse_test_parent_flags,.x=10,.y=20,.width=pulse_test_live?pulse_test_live->surf.width+2:322,
        .height=pulse_test_live?pulse_test_live->surf.height+21:441};
    return pulse_test_state_failure?-1:0;
}
static int64_t pulse_test_pin(int64_t window,bool pinned)
{
    if(pulse_test_pin_failure)return -1;
    uint64_t *flags=window==2?&pulse_test_dialog_flags:&pulse_test_parent_flags;
    if(pinned)*flags|=OS64_GUI_WINDOW_PINNED;else *flags&=~OS64_GUI_WINDOW_PINNED;
    return 0;
}
static int64_t pulse_test_focus(int64_t window)
{ (void)window;++pulse_test_focuses;return 0; }
static int64_t pulse_test_create(const char *title,int32_t x,int32_t y,uint32_t w,uint32_t h,uint64_t flags)
{
    (void)title;(void)x;(void)y;(void)w;(void)h;pulse_test_create_flags=flags;
    return -1; /* Capture creation policy without opening a guest window. */
}
#define os64_gui_window_get_state pulse_test_state
#define os64_gui_window_create_content pulse_test_create
#define arrange pulse_test_settings_arrange
#define main pulse_test_font_suite_main
#include "test_ui_text_host.c"
#undef main
#undef arrange
#undef os64_gui_window_create_content
#undef os64_gui_window_get_state
static bool pulse_test_resize_failure, pulse_test_font_missing;
static uint32_t pulse_test_min_w;
static const char *pulse_test_fixtures;
static int64_t pulse_test_min_size(int64_t window,uint32_t width,uint32_t height)
{
    (void)window;
    if(pulse_test_resize_failure || width>1600 || height>1200)return -1;
    pulse_test_min_w=width;ui_test_min_h=height;
    if(pulse_test_live){
        if(pulse_test_live->surf.width<width)pulse_test_live->surf.width=width;
        if(pulse_test_live->surf.height<height)pulse_test_live->surf.height=height;
    }return 0;
}
static int64_t pulse_test_refresh(os64_draw_ctx_t *ctx)
{
    if(ctx->surf.width<pulse_test_min_w)ctx->surf.width=pulse_test_min_w;
    return ui_test_ctx_refresh(ctx);
}
static int64_t pulse_test_content(int64_t window,os64_gui_rect_t *out)
{(void)window;*out=(os64_gui_rect_t){11,40,(int32_t)pulse_test_live->surf.width,(int32_t)pulse_test_live->surf.height};return 0;}
static int64_t pulse_test_screen(uint32_t *w,uint32_t *h){*w=1600;*h=1200;return 0;}
static int64_t pulse_test_exact(int64_t window,uint32_t w,uint32_t h)
{
    (void)window;++pulse_test_exact_calls;
    if(pulse_test_exact_failures){--pulse_test_exact_failures;return pulse_test_exact_error;}
    if(w<pulse_test_min_w || h<ui_test_min_h)return OS64_GUI_ERR_BELOW_MINIMUM;
    if(!(pulse_test_parent_flags&OS64_GUI_WINDOW_MAXIMIZED)){pulse_test_live->surf.width=w;pulse_test_live->surf.height=h;}
    return 0;
}
#define os64_gui_window_get_content_rect pulse_test_content
#define os64_gui_screen_info pulse_test_screen
#define os64_gui_window_set_content_size pulse_test_exact
#define pulse_record_placement pulse_test_placement
#define os64_gui_window_set_min_size pulse_test_min_size
#define os64_draw_ctx_refresh pulse_test_refresh
#define pulse_sample pulse_test_sample
#define os64_gui_window_get_state pulse_test_state
#define os64_gui_window_set_pinned pulse_test_pin
#define os64_gui_window_focus pulse_test_focus
#define main pulse_test_guest_main
#include "../userland/apps/pulse/pulse.c"
#undef main
#undef os64_gui_window_get_state
#undef os64_gui_window_set_pinned
#undef os64_gui_window_focus
#undef pulse_sample
#undef os64_draw_ctx_refresh
#undef os64_gui_window_set_min_size
#undef os64_gui_window_get_content_rect
#undef os64_gui_screen_info
#undef os64_gui_window_set_content_size
#undef pulse_record_placement

void os64_complain(const char *fmt,...){(void)fmt;}
int64_t pulse_test_placement(pulse_config_t *c,const os64_gui_event_t *e){(void)c;(void)e;return 0;}
bool os64_ui_theme_session(os64_ui_theme_t *t,uint64_t *installed,uint64_t hint)
{ (void)t;(void)installed;(void)hint;return false; }
int64_t os64_open(const char *p,const char *mode){(void)p;(void)mode;return -1;}
int64_t os64_read(int32_t fd,void *buf,size_t n){(void)fd;(void)buf;(void)n;return -1;}
int64_t os64_write(int32_t fd,const void *buf,size_t n){(void)fd;(void)buf;(void)n;return -1;}
int64_t os64_close(int32_t fd){(void)fd;return 0;}

os64_slurp_status_t os64_slurp(const char *path,size_t cap,uint8_t **out,size_t *length)
{
    CHECK(!strcmp(path,"/etc/fonts/DejaVuSans.ttf"));*out=NULL;*length=0;
    if(pulse_test_font_missing)return OS64_SLURP_NO_FILE;
    *out=slurp(pulse_test_fixtures,"DejaVuSans.ttf",length);
    CHECK(*length<cap);return *out?OS64_SLURP_OK:OS64_SLURP_IO_ERROR;
}

void pulse_test_sample(pulse_sampler_t *sampler,const pulse_config_t *config,pulse_sample_t *sample)
{ (void)sampler;(void)config;*sample=(pulse_sample_t){0}; }
/* Popup ownership is covered by test_menu_apps_host; this harness drives
 * dropdown values and Apply without opening a native popup. */
os64_ui_status_code_t os64_ui_menu_open_list(os64_ui_menu_session_t *s,const os64_ui_menu_list_t *list,os64_gui_rect_t anchor)
{ (void)s;(void)list;(void)anchor;return OS64_UI_ERROR; }
void os64_ui_menu_list_detach(os64_ui_menu_session_t *s,void *user)
{ (void)s;(void)user; }

static unsigned pulse_test_saves;
static bool pulse_test_save_failure;
int64_t os64_conf_write(const char *name,const os64_conf_pair_t *pairs,size_t count)
{
    CHECK(!strcmp(name,"pulse.conf"));CHECK(count==12);CHECK(!strcmp(pairs[9].key,"stay_on_top"));CHECK(!strcmp(pairs[8].key,"font_size"));CHECK(!strcmp(pairs[7].key,"refresh_ms"));
    ++pulse_test_saves;return pulse_test_save_failure?-1:0;
}
static unsigned visible_count(const pulse_config_t *c)
{ unsigned n=0;for(unsigned i=0;i<PULSE_METERS;++i)if(c->show[i])++n;return n; }
static void pulse_test_settings(void)
{
    current="settings Apply and Save";
    g.settings.ui.ctx=&g.settings.ctx;g.settings.ui.theme=g.ui.theme;
    g.settings.ctx.surf.width=760;g.settings.ctx.surf.height=560;
    os64_ui_panel(&g.settings.root);os64_ui_panel(&g.settings.body);
    os64_ui_set_root(&g.settings.ui,&g.settings.root);os64_ui_add_child(&g.settings.root,&g.settings.body);
    os64_ui_label(&g.settings.status,g.settings.message);os64_ui_add_child(&g.settings.root,&g.settings.status);
    build_settings_widgets();
    CHECK(settings_arrange(&g.settings,(os64_gui_rect_t){16,16,728,416},32,false));
    CHECK(g.units.selected==PULSE_MIB && g.show[0].checked);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,"1");settings_apply(&g.settings,true);
    CHECK(g.config.refresh_ms==1000 && !pulse_test_saves);CHECK(strstr(g.settings.message,"Refresh")!=NULL);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,"500");
    os64_ui_textfield_set(&g.settings.ui,&g.font_size,"99");settings_apply(&g.settings,true);
    CHECK(g.config.font_size==24 && !pulse_test_saves);CHECK(strstr(g.settings.message,"Text size")!=NULL);
    os64_ui_textfield_set(&g.settings.ui,&g.font_size,"32");
    os64_ui_textfield_set(&g.settings.ui,&g.mount,"relative");settings_apply(&g.settings,true);
    CHECK(!pulse_test_saves && !strcmp(g.config.mount,"/"));
    os64_ui_textfield_set(&g.settings.ui,&g.mount,"/");
    for(unsigned i=0;i<PULSE_METERS;++i)os64_ui_checkbox_set(&g.settings.ui,&g.show[i],false);
    os64_ui_dropdown_set(&g.settings.ui,&g.units,PULSE_UNITS,PULSE_MBIT);
    pulse_test_resize_failure=true;settings_apply(&g.settings,false);
    CHECK(g.config.font_size==24 && visible_count(&g.config)==4);
    pulse_test_resize_failure=false;
    pulse_test_font_missing=true;settings_apply(&g.settings,false);
    CHECK(g.config.font_size==24 && visible_count(&g.config)==4);
    pulse_test_font_missing=false;
    settings_apply(&g.settings,false);
    CHECK(g.config.font_size==32);
    CHECK(g.config.refresh_ms==500 && g.config.units==PULSE_MBIT && !pulse_test_saves);
    CHECK(visible_count(&g.config)==0 && !g.settings_button.hidden);
    for(unsigned i=0;i<PULSE_METERS;++i)CHECK(g.meters[i].hidden);
    os64_ui_checkbox_set(&g.settings.ui,&g.show[PULSE_CPU],true);
    pulse_test_save_failure=true;settings_apply(&g.settings,true);
    CHECK(pulse_test_saves==1 && !g.meters[PULSE_CPU].hidden);
    CHECK(strstr(g.settings.message,"could not be saved")!=NULL);
    pulse_test_save_failure=false;settings_apply(&g.settings,true);
    CHECK(pulse_test_saves==2 && !strcmp(g.settings.message,"Applied and saved."));
    current="frame pin and Settings share native state";
    g.settings.window=2;pulse_test_parent_flags=pulse_test_dialog_flags=OS64_GUI_WINDOW_PINNED;
    sync_pin_state();CHECK(g.stay_on_top.checked);
    g.stay_on_top.checked=false;pin_changed(&g.stay_on_top,NULL);
    CHECK(!g.config.stay_on_top && !g.stay_on_top.checked);
    CHECK(!pulse_test_parent_flags && !pulse_test_dialog_flags && pulse_test_focuses==1);
    pulse_test_parent_flags=OS64_GUI_WINDOW_PINNED;
    os64_gui_event_t pin={.type=OS64_GUI_EVENT_WINDOW_PIN_CHANGED};window_event(NULL,&pin);
    CHECK(g.config.stay_on_top && g.stay_on_top.checked && pulse_test_dialog_flags==OS64_GUI_WINDOW_PINNED);
    pulse_test_pin_failure=true;g.stay_on_top.checked=false;pin_changed(&g.stay_on_top,NULL);
    CHECK(g.stay_on_top.checked && g.config.stay_on_top && strstr(g.settings.message,"Could not change")!=NULL);
    pulse_test_pin_failure=false;
    pulse_test_parent_flags=0;pin.type=OS64_GUI_EVENT_INPUT_LOST;window_event(NULL,&pin);
    CHECK(!g.stay_on_top.checked && !g.config.stay_on_top && !pulse_test_dialog_flags);
    g.settings.window=0;
    CHECK(os64_ui_font_release(&g.settings.ui)==OS64_FONT_OK);
}

static void pulse_test_preview(const char *path)
{
    os64_gui_rect_t damage;
    os64_ui_mark_dirty(&g.ui,&g.root);CHECK(os64_ui_render(&g.ui,&damage));
    FILE *out=fopen(path,"wb");CHECK(out!=NULL);if(!out)return;
    fprintf(out,"P6\n%u %u\n255\n",g.ctx.surf.width,g.ctx.surf.height);
    const uint32_t *pixels=g.ctx.surf.pixels;
    bool wrote=true;
    for(unsigned y=0;y<g.ctx.surf.height;++y)for(unsigned x=0;x<g.ctx.surf.width;++x){
        uint32_t p=pixels[y*g.ctx.surf.pitch_px+x];unsigned char rgb[]={p>>16,p>>8,p};
        if(fwrite(rgb,1,3,out)!=3)wrote=false;
    }
    CHECK(wrote);CHECK(fclose(out)==0);
}
static void pulse_test_views(void)
{
    current="live Instruments and preferred sizes";
    pulse_config_t c=g.config;c.view=PULSE_INSTRUMENTS;c.font_size=26;
    for(unsigned i=0;i<PULSE_METERS;++i)c.show[i]=true;
    g.sample=(pulse_sample_t){.cpu_valid=true,.cpu_percent=45,.peak_valid={true},.peak={80},.network_scale=1024};
    CHECK(change_view(&c));CHECK(g.renderer && !g.instruments.hidden && g.settings_button.hidden);
    CHECK(g.ctx.surf.width==600 && g.ctx.surf.height==610);
    CHECK(g.sample.cpu_percent==45 && g.sample.peak[0]==80);
    os64_gui_rect_t damage;CHECK(os64_ui_render(&g.ui,&damage));CHECK(!g.paint_failed);
    c=g.config;c.instruments_compact=true;CHECK(change_view(&c));
    CHECK(g.ctx.surf.width==360 && g.ctx.surf.height==390);CHECK(os64_ui_render(&g.ui,&damage));CHECK(!g.paint_failed);
    pulse_size_t compact_size={g.ctx.surf.width,g.ctx.surf.height},minimum=g.minimum;
    current="minimum growth followed by exact-size refusal rolls back";
    pulse_test_exact_error=OS64_GUI_ERR_SCREEN_FIT;pulse_test_exact_failures=1;
    c=g.config;c.instruments_compact=false;CHECK(!change_view(&c));
    CHECK(g.config.instruments_compact && g.ctx.surf.width==compact_size.w && g.ctx.surf.height==compact_size.h);
    CHECK(pulse_test_min_w==minimum.w && ui_test_min_h==minimum.h);
    CHECK(g.transition_error==OS64_GUI_ERR_SCREEN_FIT);
    current="failed rollback is reported and actual surface retained";
    pulse_test_exact_failures=2;CHECK(!change_view(&c));
    CHECK(strstr(g.transition_message,"restoring")!=NULL && g.config.instruments_compact);
    CHECK(g.root.bounds.w==(int32_t)g.ctx.surf.width && g.root.bounds.h==(int32_t)g.ctx.surf.height);
    CHECK(os64_ui_render(&g.ui,&damage));CHECK(!g.paint_failed);
    CHECK(change_view(&c));c=g.config;c.instruments_compact=true;CHECK(change_view(&c));
    current="older kernel cannot grow a view through minimum side effects";
    g.resize_supported=false;unsigned calls=pulse_test_exact_calls;
    minimum=g.minimum;c=g.config;c.instruments_compact=false;
    CHECK(!change_view(&c));CHECK(g.config.instruments_compact && strstr(g.transition_message,"kernel")!=NULL);
    CHECK(pulse_test_exact_calls==calls && g.minimum.w==minimum.w && g.minimum.h==minimum.h);
    // The same mode change is usable after the owner manually makes room.
    g.ctx.surf.width=700;g.ctx.surf.height=700;layout(&g.ui);
    CHECK(change_view(&c));CHECK(g.resize_warning && g.ctx.surf.width==700 && g.ctx.surf.height==700);
    CHECK(pulse_test_exact_calls==calls);g.resize_supported=true;
    current="view-specific compact preference survives Classic";
    c=g.config;c.instruments_compact=true;CHECK(change_view(&c));c=g.config;c.view=PULSE_CLASSIC;
    CHECK(change_view(&c));CHECK(g.config.instruments_compact && g.instruments.hidden && !g.settings_button.hidden);
    c=g.config;c.view=PULSE_INSTRUMENTS;CHECK(change_view(&c));CHECK(g.ctx.surf.width==360 && g.ctx.surf.height==390);
    current="maximized transition changes normal target, not visible size";
    pulse_test_parent_flags|=OS64_GUI_WINDOW_MAXIMIZED;g.ctx.surf.width=1000;g.ctx.surf.height=900;
    c=g.config;c.instruments_compact=false;CHECK(change_view(&c));
    CHECK(g.ctx.surf.width==1000 && g.ctx.surf.height==900 && g.normal_size.w==600 && g.normal_size.h==610);
    pulse_test_parent_flags&=~OS64_GUI_WINDOW_MAXIMIZED;
}
int main(int argc,char **argv)
{
    current="settings inherits pinned band";
    os64_ui_settings_t d={0};
    pulse_test_parent_flags=OS64_GUI_WINDOW_PINNED|OS64_GUI_WINDOW_START_UNFOCUSED;
    CHECK(!os64_ui_settings_open(&d,1,"test",1,NULL,NULL,NULL));
    CHECK(pulse_test_create_flags==(OS64_GUI_CREATE_FIT_SCREEN|OS64_GUI_WINDOW_PINNED));
    d.window=0;pulse_test_parent_flags=0;
    CHECK(!os64_ui_settings_open(&d,1,"test",1,NULL,NULL,NULL));
    CHECK(pulse_test_create_flags==OS64_GUI_CREATE_FIT_SCREEN);
    d.window=0;pulse_test_state_failure=true;pulse_test_parent_flags=OS64_GUI_WINDOW_PINNED;
    CHECK(!os64_ui_settings_open(&d,1,"test",1,NULL,NULL,NULL));
    CHECK(pulse_test_create_flags==OS64_GUI_CREATE_FIT_SCREEN);

    pulse_test_state_failure=false;pulse_test_parent_flags=OS64_GUI_WINDOW_PINNED;
    current="meter selection and layout";
    pulse_config_defaults(&g.config);g.ui.ctx=&g.ctx;ui_test_view_theme(&g.ui.theme);
    pulse_test_live=&g.ctx;g.resize_supported=true;g.minimum=(pulse_size_t){64,32};
    g.ui.theme.panel_bg=0xff202833;g.ui.theme.label_fg=0xffedf3fa;
    g.ui.theme.button_face=0xff315e85;g.ui.theme.button_fg=0xffedf3fa;
    g.ui.theme.scroll_track=0xff394553;g.ui.theme.text_sel_bg=0xff62bad7;
    g.ctx.surf.width=320;g.ctx.surf.height=420;g.ctx.surf.pitch_px=1600;
    g.ctx.surf.pixels=calloc(1600*1200,4);CHECK(g.ctx.surf.pixels!=NULL);
    build_widgets();g.ui.on_resize=layout;
    CHECK(os64_ui_font_planner(&g.ui,plan_font,commit_font,discard_font,NULL)==OS64_FONT_OK);
    for(unsigned mask=0;mask<16;++mask){
        for(unsigned i=0;i<PULSE_METERS;++i){g.config.show[i]=(mask&(1u<<i))!=0;os64_ui_set_hidden(&g.ui,&g.meters[i],!g.config.show[i]);}
        g.ctx.surf.height=(unsigned)needed_height(&g.ui,&g.config);layout(&g.ui);
        CHECK(arrange(&g.ui,false));CHECK(!g.settings_button.hidden && g.settings_button.bounds.h>0);
        int32_t last=g.settings_button.bounds.y+g.settings_button.bounds.h;
        for(unsigned i=0;i<PULSE_METERS;++i)if(g.config.show[i]){
            CHECK(g.meters[i].bounds.y>=last);last=g.meters[i].bounds.y+g.meters[i].bounds.h;
            CHECK(last<=(int32_t)g.ctx.surf.height);
            CHECK(g.details[i].bounds.y+g.details[i].bounds.h<=last-8);
        }
    }
    g.ctx.surf.height=420;layout(&g.ui);
    g.sample=(pulse_sample_t){.cpu_valid=true,.ram_valid=true,.disk_valid=true,.net_valid=true,
        .cpu_percent=37,.ram_used=3ULL<<30,.ram_available=5ULL<<30,.ram_total=8ULL<<30,
        .disk_used=120ULL<<30,.disk_available=380ULL<<30,.disk_total=500ULL<<30,
        .download=1572864,.upload=262144,.network_scale=2097152};
    render_sample();CHECK(!strcmp(g.title[0],"CPU: 37%"));CHECK(strstr(g.title[3],"1.5 MiB/s")!=NULL);
    os64_gui_rect_t damage;CHECK(os64_ui_render(&g.ui,&damage));
    CHECK(g.titles[0].run!=NULL);
    int32_t net_row=os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI),pad=g.ui.theme.pad;
    int32_t y=g.meters[PULSE_NET].bounds.y+net_row+pad/2;
    uint32_t *pixels=g.ctx.surf.pixels;
    int32_t x=g.meters[PULSE_NET].bounds.x;
    CHECK(pixels[y*g.ctx.surf.pitch_px+x]==0xff409bff);
    CHECK(pixels[(y+net_row+pad+8)*g.ctx.surf.pitch_px+x]==0xffed6868);
    CHECK(strstr(g.scale_text,"2.0 MiB/s")!=NULL);
    g.sample.cpu_percent=75;render_sample();CHECK(g.titles[0].run==NULL);
    CHECK(os64_ui_render(&g.ui,&damage));CHECK(!strcmp(g.title[0],"CPU: 75%"));
#ifdef UI_TEXT_REAL
    current="independent gadget font, growth and rollback";
    pulse_test_fixtures=argc>1?argv[1]:"userland/libfreetype/fixtures";
    CHECK(apply_font(&g.config));layout(&g.ui);
    CHECK(ui_test_min_h==(uint32_t)needed_height(&g.ui,&g.config));
    CHECK(g.ctx.surf.width>320);
    int32_t row=os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI);
    CHECK(row>16 && !g.ui.font_session);
    g.ui.follow_session=true;
    os64_gui_event_t appearance={.type=OS64_GUI_EVENT_APPEARANCE};
    os64_ui_dispatch(&g.ui,&appearance);
    CHECK(os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI)==row);
    os64_gui_rect_t before=g.details[3].bounds;
    pulse_config_t larger=g.config;larger.font_size=48;
    pulse_test_resize_failure=true;CHECK(!apply_font(&larger));pulse_test_resize_failure=false;
    CHECK(!memcmp(&before,&g.details[3].bounds,sizeof(before)));
    CHECK(os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI)==row);
    CHECK(apply_font(&larger));
    CHECK(g.ctx.surf.height>420 && g.details[3].bounds.y+g.details[3].bounds.h<=(int32_t)g.ctx.surf.height);
    CHECK(os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI)>row);
    CHECK(apply_font(&g.config));
    // A normal user resize after reducing text size can restore compactness.
    g.ctx.surf.height=ui_test_min_h;g.ctx.surf.width=pulse_test_min_w;layout(&g.ui);
#endif
    g.sample.network_scale=1024;render_sample();CHECK(!strcmp(g.scale_text,"Scale: 1.0 KiB/s"));
    g.config.units=PULSE_MBIT;render_sample();CHECK(!strcmp(g.scale_text,"Scale: 8.1 kbit/s"));
    g.config.units=PULSE_MIB;g.sample.network_scale=2097152;render_sample();
    if(argc>2)pulse_test_preview(argv[2]);
    g.sample=(pulse_sample_t){0};render_sample();
    CHECK(strstr(g.title[0],"waiting")!=NULL && strstr(g.title[1],"unavailable")!=NULL);
    pulse_test_settings();pulse_test_views();
    pulse_instruments_destroy(g.renderer);g.renderer=NULL;
    CHECK(os64_ui_font_release(&g.ui)==OS64_FONT_OK);free(g.ctx.surf.pixels);
    printf("pulse UI: %lu checks, %d failures\n",checks,failures);
    return failures?1:0;
}
