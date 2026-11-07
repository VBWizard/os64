/* Drive Pulse's real widgets, layout and font planner on a host surface.
 * The shared font harness supplies the allocator and real face loader. */
#include "os64/ui.h"
static uint64_t pulse_test_parent_flags, pulse_test_create_flags;
static bool pulse_test_state_failure;
static int64_t pulse_test_state(int64_t window,os64_gui_window_state_t *state)
{
    (void)window;*state=(os64_gui_window_state_t){.flags=pulse_test_parent_flags,.x=10,.y=20};
    return pulse_test_state_failure?-1:0;
}
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
#define os64_gui_window_set_min_size ui_test_set_min_size
#define os64_draw_ctx_refresh ui_test_ctx_refresh
#define pulse_sample pulse_test_sample
#define main pulse_test_guest_main
#include "../userland/apps/pulse/pulse.c"
#undef main
#undef pulse_sample
#undef os64_draw_ctx_refresh
#undef os64_gui_window_set_min_size

bool os64_ui_theme_session(os64_ui_theme_t *t,uint64_t *installed,uint64_t hint)
{ (void)t;(void)installed;(void)hint;return false; }
int64_t os64_open(const char *p,const char *mode){(void)p;(void)mode;return -1;}
int64_t os64_read(int32_t fd,void *buf,size_t n){(void)fd;(void)buf;(void)n;return -1;}
int64_t os64_write(int32_t fd,const void *buf,size_t n){(void)fd;(void)buf;(void)n;return -1;}
int64_t os64_close(int32_t fd){(void)fd;return 0;}

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
    CHECK(!strcmp(name,"pulse.conf"));CHECK(count==8);CHECK(!strcmp(pairs[7].key,"refresh_ms"));
    ++pulse_test_saves;return pulse_test_save_failure?-1:0;
}
static void pulse_test_settings(void)
{
    current="settings Apply and Save";
    g.settings.ui.ctx=&g.settings.ctx;g.settings.ui.theme=g.ui.theme;
    g.settings.ctx.surf.width=760;g.settings.ctx.surf.height=420;
    os64_ui_panel(&g.settings.root);os64_ui_panel(&g.settings.body);
    os64_ui_set_root(&g.settings.ui,&g.settings.root);os64_ui_add_child(&g.settings.root,&g.settings.body);
    os64_ui_label(&g.settings.status,g.settings.message);os64_ui_add_child(&g.settings.root,&g.settings.status);
    build_settings_widgets();
    CHECK(settings_arrange(&g.settings,(os64_gui_rect_t){16,16,728,288},32,false));
    CHECK(g.units.selected==PULSE_MIB && g.show[0].checked);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,"1");settings_apply(&g.settings,true);
    CHECK(g.config.refresh_ms==1000 && !pulse_test_saves);CHECK(strstr(g.settings.message,"Refresh")!=NULL);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,"500");
    os64_ui_textfield_set(&g.settings.ui,&g.mount,"relative");settings_apply(&g.settings,true);
    CHECK(!pulse_test_saves && !strcmp(g.config.mount,"/"));
    os64_ui_textfield_set(&g.settings.ui,&g.mount,"/");
    for(unsigned i=0;i<PULSE_METERS;++i)os64_ui_checkbox_set(&g.settings.ui,&g.show[i],false);
    os64_ui_dropdown_set(&g.settings.ui,&g.units,PULSE_UNITS,PULSE_MBIT);
    settings_apply(&g.settings,false);
    CHECK(g.config.refresh_ms==500 && g.config.units==PULSE_MBIT && !pulse_test_saves);
    CHECK(visible_count(&g.config)==0 && !g.settings_button.hidden);
    for(unsigned i=0;i<PULSE_METERS;++i)CHECK(g.meters[i].hidden);
    os64_ui_checkbox_set(&g.settings.ui,&g.show[PULSE_CPU],true);
    pulse_test_save_failure=true;settings_apply(&g.settings,true);
    CHECK(pulse_test_saves==1 && !g.meters[PULSE_CPU].hidden);
    CHECK(strstr(g.settings.message,"could not be saved")!=NULL);
    pulse_test_save_failure=false;settings_apply(&g.settings,true);
    CHECK(pulse_test_saves==2 && !strcmp(g.settings.message,"Applied and saved."));
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

    current="meter selection and layout";
    pulse_config_defaults(&g.config);g.ui.ctx=&g.ctx;ui_test_view_theme(&g.ui.theme);
    g.ui.theme.panel_bg=0xff202833;g.ui.theme.label_fg=0xffedf3fa;
    g.ui.theme.button_face=0xff315e85;g.ui.theme.button_fg=0xffedf3fa;
    g.ui.theme.scroll_track=0xff394553;g.ui.theme.text_sel_bg=0xff62bad7;
    g.ctx.surf.width=320;g.ctx.surf.height=420;g.ctx.surf.pitch_px=320;
    g.ctx.surf.pixels=calloc(320*800,4);CHECK(g.ctx.surf.pixels!=NULL);
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
        .download=1572864,.upload=262144};
    render_sample();CHECK(!strcmp(g.title[0],"CPU: 37%"));CHECK(strstr(g.title[3],"1.5 MiB/s")!=NULL);
    os64_gui_rect_t damage;CHECK(os64_ui_render(&g.ui,&damage));
    CHECK(g.titles[0].run!=NULL);
    g.sample.cpu_percent=75;render_sample();CHECK(g.titles[0].run==NULL);
    CHECK(os64_ui_render(&g.ui,&damage));CHECK(!strcmp(g.title[0],"CPU: 75%"));
#ifdef UI_TEXT_REAL
    current="font adoption and rollback";
    const char *fixtures=argc>1?argv[1]:"userland/libfreetype/fixtures";
    CHECK(ui_test_adopt(&g.ui,fixtures,16)==OS64_FONT_OK);
    CHECK(ui_test_min_h==(uint32_t)needed_height(&g.ui,&g.config));
    os64_gui_rect_t before=g.details[3].bounds;
    CHECK(ui_test_adopt(&g.ui,fixtures,96)==OS64_FONT_LIMIT);
    CHECK(!memcmp(&before,&g.details[3].bounds,sizeof(before)));
#endif
    if(argc>2)pulse_test_preview(argv[2]);
    g.sample=(pulse_sample_t){0};render_sample();
    CHECK(strstr(g.title[0],"waiting")!=NULL && strstr(g.title[1],"unavailable")!=NULL);
    pulse_test_settings();
    CHECK(os64_ui_font_release(&g.ui)==OS64_FONT_OK);free(g.ctx.surf.pixels);
    printf("pulse UI: %lu checks, %d failures\n",checks,failures);
    return failures?1:0;
}
