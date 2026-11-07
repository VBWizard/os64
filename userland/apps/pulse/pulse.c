#include "pulse_data.h"
#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/ui_window.h"
#include "os64/ui_settings.h"
#include "os64/ui_menu.h"
#include "os64/conf.h"
#include "os64/thread.h"

static struct {
    os64_ui_t ui;
    os64_draw_ctx_t ctx;
    int64_t window, timer;
    os64_ui_dispatcher_t dispatcher;
    os64_ui_widget_t root, settings_button, meters[PULSE_METERS];
    os64_ui_widget_t titles[PULSE_METERS], details[PULSE_METERS];
    os64_ui_class_t meter_class;
    char title[PULSE_METERS][96], detail[PULSE_METERS][96];
    pulse_config_t config;
    pulse_sample_t sample;
    pulse_sampler_t sampler;
    bool running, want_settings;
    unsigned stop_timer, refresh_ms;
    os64_ui_settings_t settings;
    os64_ui_menu_session_t *settings_menu;
    os64_ui_checkbox_t show[PULSE_METERS];
    os64_ui_dropdown_t units;
    os64_ui_widget_t labels[7];
    os64_ui_textfield_t mount, interface, refresh;
    char mount_text[256], interface_text[64], refresh_text[16];
} g;
static const char *const names[PULSE_METERS]={"CPU","RAM","Disk","Network"};

static void place(os64_ui_widget_t *w,os64_gui_rect_t box,bool stage)
{ if(stage) os64_ui_widget_stage_bounds(w,box);else w->bounds=box; }
static unsigned visible_count(const pulse_config_t *c)
{ unsigned n=0;for(unsigned i=0;i<PULSE_METERS;++i) if(c->show[i]) ++n;return n; }

static int32_t needed_height(os64_ui_t *ui,const pulse_config_t *c)
{
    int32_t row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI),pad=ui->theme.pad;
    return 2*pad+os64_ui_control_min_height(ui)+(int32_t)visible_count(c)*(2*row+3*pad+8);
}
static bool arrange(os64_ui_t *ui,bool stage)
{
    os64_gui_rect_t b=os64_ui_widget_planned_bounds(&g.root);
    int32_t pad=ui->theme.pad,row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI);
    int32_t bh=os64_ui_control_min_height(ui),width=b.w-2*pad;
    if(width<1 || b.h<needed_height(ui,&g.config)) return false;
    int32_t caption;
    if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"Settings",8,&caption) || caption+2*pad>width) return false;
    place(&g.settings_button,(os64_gui_rect_t){pad,pad,width,bh},stage);
    int32_t y=2*pad+bh,h=2*row+2*pad+8;
    for(unsigned i=0;i<PULSE_METERS;++i) if(g.config.show[i]) {
        place(&g.meters[i],(os64_gui_rect_t){pad,y,width,h},stage);
        place(&g.titles[i],(os64_gui_rect_t){pad,y,width,row},stage);
        place(&g.details[i],(os64_gui_rect_t){pad,y+row,width,row},stage);
        y+=h+pad;
    }
    return true;
}
static void layout(os64_ui_t *ui)
{
    g.root.bounds=(os64_gui_rect_t){0,0,(int32_t)g.ctx.surf.width,(int32_t)g.ctx.surf.height};
    (void)arrange(ui,false);
}
static os64_font_status_t plan_font(os64_ui_t *ui,void *user,void **out)
{ (void)user;*out=NULL;return arrange(ui,true)?OS64_FONT_OK:OS64_FONT_LIMIT; }
static void discard_font(os64_ui_t *ui,void *user,void *plan)
{ (void)ui;(void)user;(void)plan; }
static void commit_font(os64_ui_t *ui,void *user,void *plan)
{
    (void)user;(void)plan;
    // Keep later manual resizes within the newly adopted font's envelope.
    (void)os64_gui_window_set_min_size(g.window,280,(uint32_t)needed_height(ui,&g.config));
}

static void meter_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->panel_bg);
    unsigned i=(unsigned)(w-g.meters),pct=0;
    bool valid=false;
    if(i==PULSE_CPU){valid=g.sample.cpu_valid;pct=g.sample.cpu_percent;}
    else if(i==PULSE_RAM){valid=g.sample.ram_valid;pct=pulse_percent(g.sample.ram_used,g.sample.ram_total);}
    else if(i==PULSE_DISK){valid=g.sample.disk_valid;pct=pulse_percent(g.sample.disk_used,g.sample.disk_total);}
    else return; // Network has rates, not a percentage of an invented capacity.
    os64_gui_rect_t bar={w->bounds.x,w->bounds.y+w->bounds.h-8,w->bounds.w,8};
    os64_draw_fill_rect(&ctx->surf,bar,t->scroll_track);
    if(valid){bar.w=(int32_t)((int64_t)bar.w*pct/100);os64_draw_fill_rect(&ctx->surf,bar,t->text_sel_bg);}
}
static void text_changed(unsigned i)
{
    os64_ui_run_release(g.titles[i].run);g.titles[i].run=NULL;
    os64_ui_run_release(g.details[i].run);g.details[i].run=NULL;
    os64_ui_mark_dirty(&g.ui,&g.meters[i]);
}
static void render_sample(void)
{
    char a[32],b[32];
    if(g.sample.cpu_valid) os64_snprintf(g.title[0],sizeof(g.title[0]),"CPU: %u%%",g.sample.cpu_percent);
    else os64_strcopy(g.title[0],sizeof(g.title[0]),"CPU: waiting for a sample");
    os64_strcopy(g.detail[0],sizeof(g.detail[0]),"Overall utilization");
    for(unsigned i=PULSE_RAM;i<=PULSE_DISK;++i){
        bool valid=i==PULSE_RAM?g.sample.ram_valid:g.sample.disk_valid;
        if(valid){
            os64_format_binary_size(i==PULSE_RAM?g.sample.ram_used:g.sample.disk_used,a,sizeof(a));
            os64_format_binary_size(i==PULSE_RAM?g.sample.ram_available:g.sample.disk_available,b,sizeof(b));
            os64_snprintf(g.title[i],sizeof(g.title[i]),"%s: %s used",names[i],a);
            os64_snprintf(g.detail[i],sizeof(g.detail[i]),"%s available",b);
        }else{
            os64_snprintf(g.title[i],sizeof(g.title[i]),"%s: unavailable",names[i]);
            os64_strcopy(g.detail[i],sizeof(g.detail[i]),i==PULSE_DISK?g.config.mount:"");
        }
    }
    if(g.sample.net_valid){
        pulse_format_rate(a,sizeof(a),g.sample.download,g.config.units);
        pulse_format_rate(b,sizeof(b),g.sample.upload,g.config.units);
        os64_snprintf(g.title[3],sizeof(g.title[3]),"Down: %s",a);
        os64_snprintf(g.detail[3],sizeof(g.detail[3]),"Up: %s",b);
    }else{
        os64_strcopy(g.title[3],sizeof(g.title[3]),"Network: waiting for a sample");
        os64_strcopy(g.detail[3],sizeof(g.detail[3]),g.config.interface);
    }
    for(unsigned i=0;i<PULSE_METERS;++i) text_changed(i);
}
static void sample_now(void)
{
    pulse_sample(&g.sampler,&g.config,&g.sample);
    render_sample();
}
static void ask_settings(os64_ui_widget_t *w,void *user)
{ (void)w;(void)user;g.want_settings=true; }
static void close_window(os64_ui_t *ui)
{ (void)ui;g.running=false; }
static void window_event(void *user,const os64_gui_event_t *ev)
{
    (void)user;
    if(ev->type==OS64_GUI_EVENT_SETTINGS) g.want_settings=true;
    else if(ev->type==OS64_GUI_EVENT_DOORBELL) sample_now();
    else os64_ui_dispatch(&g.ui,ev);
}
static void window_paint(void *user)
{ (void)user;os64_ui_paint(&g.ui); }

// The timer touches no widgets or samplers. Join it before releasing the
// handle it rings; main owns sampling and the two window lifetimes.
static int64_t timer_main(void *user)
{
    (void)user;
    while(!__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)){
        unsigned remaining=__atomic_load_n(&g.refresh_ms,__ATOMIC_RELAXED);
        while(remaining && !__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)){
            unsigned step=remaining>250?250:remaining;
            (void)os64_sleep(step);remaining-=step;
        }
        if(!__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)) (void)os64_gui_event_ring(g.window,1);
    }
    return 0;
}

static const char *unit_label(size_t i,void *user)
{ (void)user;return i<PULSE_UNITS?pulse_unit_names[i]:""; }
static bool settings_arrange(os64_ui_settings_t *d,os64_gui_rect_t b,int32_t row,bool stage)
{
    int32_t gap=d->ui.theme.gap,half=(b.w-gap)/2;
    if(half<100)return false;
    for(unsigned i=0;i<PULSE_METERS;++i)
        place(&g.show[i].w,(os64_gui_rect_t){b.x+(int32_t)(i%2)*(half+gap),b.y+(int32_t)(i/2)*row,half,row},stage);
    os64_ui_widget_t *fields[]={&g.units.w,&g.mount.w,&g.interface.w,&g.refresh.w};
    for(unsigned i=0;i<4;++i){
        place(&g.labels[i],(os64_gui_rect_t){b.x,b.y+(int32_t)(i+2)*row,half,row},stage);
        place(fields[i],(os64_gui_rect_t){b.x+half+gap,b.y+(int32_t)(i+2)*row,half,row},stage);
    }
    for(unsigned i=4;i<7;++i)
        place(&g.labels[i],(os64_gui_rect_t){b.x,b.y+(int32_t)(i+2)*row,b.w,row},stage);
    return true;
}
static void settings_apply(os64_ui_settings_t *d,bool save)
{
    pulse_config_t c=g.config;
    uint64_t interval;
    if(!os64_parse_u64(g.refresh.buf,&interval) || interval<250 || interval>10000){
        os64_ui_settings_report(d,"Refresh must be 250 to 10000 milliseconds.");return;
    }
    if(g.mount.buf[0]!='/' || !g.interface.buf[0] || g.units.selected<0 || g.units.selected>=PULSE_UNITS){
        os64_ui_settings_report(d,"Choose units, an absolute mount path, and an interface or all.");return;
    }
    c.refresh_ms=(unsigned)interval;c.units=(unsigned)g.units.selected;
    os64_strcopy(c.mount,sizeof(c.mount),g.mount.buf);
    os64_strcopy(c.interface,sizeof(c.interface),g.interface.buf);
    for(unsigned i=0;i<PULSE_METERS;++i)c.show[i]=g.show[i].checked;
    if(os64_gui_window_set_min_size(g.window,280,(uint32_t)needed_height(&g.ui,&c))<0 ||
       os64_draw_ctx_refresh(&g.ctx)<0){os64_ui_settings_report(d,"The window could not fit those meters.");return;}
    g.config=c;
    __atomic_store_n(&g.refresh_ms,c.refresh_ms,__ATOMIC_RELAXED);
    for(unsigned i=0;i<PULSE_METERS;++i) os64_ui_set_hidden(&g.ui,&g.meters[i],!c.show[i]);
    layout(&g.ui);sample_now();os64_ui_mark_dirty(&g.ui,&g.root);
    if(save){
        char refresh[16];os64_snprintf(refresh,sizeof(refresh),"%u",c.refresh_ms);
        os64_conf_pair_t pairs[]={
            {"cpu",c.show[0]?"on":"off"},{"ram",c.show[1]?"on":"off"},
            {"disk",c.show[2]?"on":"off"},{"net",c.show[3]?"on":"off"},
            {"network_units",pulse_unit_names[c.units]},{"mount",c.mount},
            {"interface",c.interface},{"refresh_ms",refresh}};
        if(os64_conf_write("pulse.conf",pairs,sizeof(pairs)/sizeof(*pairs))){
            os64_ui_settings_report(d,"Applied here, but settings could not be saved.");return;
        }
    }
    os64_ui_settings_report(d,save?"Applied and saved.":"Applied to this window.");
}
static void settings_event(void *user,const os64_gui_event_t *ev)
{ (void)user;os64_ui_settings_event(&g.settings,ev); }
static void settings_paint(void *user)
{ (void)user;os64_ui_paint(&g.settings.ui); }
static bool close_settings(void)
{
    if(os64_ui_menu_session_dispose(g.settings_menu)!=OS64_UI_OK)return false;
    g.settings_menu=NULL;
    os64_ui_settings_close(&g.settings);
    return !g.settings.window;
}
static void build_settings_widgets(void)
{
    for(unsigned i=0;i<PULSE_METERS;++i){
        os64_ui_checkbox(&g.show[i],names[i],g.config.show[i],NULL,NULL);
        os64_ui_add_child(&g.settings.body,&g.show[i].w);
    }
    const char *labels[]={"Network units","Disk mount","Network interface","Refresh (milliseconds)",
        "Use all to combine network interfaces.","A mount such as / selects its filesystem space.",
        "Hide meters here; resize the panel to your liking."};
    for(unsigned i=0;i<7;++i){os64_ui_label(&g.labels[i],labels[i]);os64_ui_add_child(&g.settings.body,&g.labels[i]);}
    os64_ui_dropdown(&g.units,PULSE_UNITS,unit_label,NULL,NULL);
    os64_ui_add_child(&g.settings.body,&g.units.w);
    os64_ui_dropdown_set(&g.settings.ui,&g.units,PULSE_UNITS,(int)g.config.units);
    os64_ui_textfield(&g.mount,g.mount_text,sizeof(g.mount_text),NULL,NULL,NULL);
    os64_ui_textfield(&g.interface,g.interface_text,sizeof(g.interface_text),NULL,NULL,NULL);
    os64_ui_textfield(&g.refresh,g.refresh_text,sizeof(g.refresh_text),NULL,NULL,NULL);
    os64_ui_add_child(&g.settings.body,&g.mount.w);os64_ui_add_child(&g.settings.body,&g.interface.w);
    os64_ui_add_child(&g.settings.body,&g.refresh.w);
    os64_ui_textfield_set(&g.settings.ui,&g.mount,g.config.mount);
    os64_ui_textfield_set(&g.settings.ui,&g.interface,g.config.interface);
    char value[16];os64_snprintf(value,sizeof(value),"%u",g.config.refresh_ms);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,value);
}
static void open_settings(void)
{
    g.want_settings=false;
    if(!os64_ui_settings_open(&g.settings,g.window,"Pulse Settings",9,settings_arrange,settings_apply,NULL))return;
    build_settings_widgets();
    os64_ui_settings_ready(&g.settings);
    if(os64_ui_dispatcher_add(&g.dispatcher,g.settings.window,&g.settings.ui,settings_event,settings_paint,NULL)!=OS64_UI_OK){
        os64_ui_settings_close(&g.settings);return;
    }
    g.settings.dispatcher=&g.dispatcher;
    if(os64_ui_menu_session_create(&g.dispatcher,&g.settings.ui,g.settings.window,NULL,NULL,NULL,&g.settings_menu)!=OS64_UI_OK)
        (void)close_settings();
}

static void build_widgets(void)
{
    os64_ui_panel(&g.root);os64_ui_set_root(&g.ui,&g.root);
    os64_ui_button(&g.settings_button,"Settings",ask_settings,NULL);os64_ui_add_child(&g.root,&g.settings_button);
    for(unsigned i=0;i<PULSE_METERS;++i){
        os64_ui_panel(&g.meters[i]);
        if(!i){g.meter_class=*g.meters[i].cls;g.meter_class.paint=meter_paint;}
        g.meters[i].cls=&g.meter_class;g.meters[i].hidden=!g.config.show[i];
        os64_ui_add_child(&g.root,&g.meters[i]);
        os64_ui_label(&g.titles[i],g.title[i]);os64_ui_add_child(&g.meters[i],&g.titles[i]);
        os64_ui_label(&g.details[i],g.detail[i]);os64_ui_add_child(&g.meters[i],&g.details[i]);
    }
}

int main(void)
{
    pulse_config_defaults(&g.config);
    int64_t rc=os64_conf_find_read("pulse.conf",pulse_config_line,&g.config,NULL,0);
    if(rc<0 && rc!=OS64_CONF_NO_FILE)os64_complain("pulse: configuration could not be fully read; using available settings");
    if(!pulse_sampler_init(&g.sampler)){os64_complain("pulse: cannot allocate samplers");return 1;}
    os64_ui_window_options_t options={.title="Pulse",.x=30,.y=40,.content_width=320,.content_height=420,
        .flags=OS64_GUI_WINDOW_PINNED|OS64_GUI_WINDOW_START_UNFOCUSED|OS64_GUI_WINDOW_HAS_SETTINGS,
        .remember={"pulse.conf",OS64_WINDOW_REMEMBER_POSITION_AND_SIZE}};
    if(os64_ui_window_open(&g.ui,&g.ctx,&g.window,&options)!=OS64_UI_OK){
        if(g.window>0)(void)os64_gui_window_destroy(g.window);
        pulse_sampler_destroy(&g.sampler);return 1;
    }
    build_widgets();
    g.ui.on_resize=layout;g.ui.on_close=close_window;
    (void)os64_ui_font_planner(&g.ui,plan_font,commit_font,discard_font,NULL);
    (void)os64_gui_window_set_min_size(g.window,280,(uint32_t)needed_height(&g.ui,&g.config));
    (void)os64_draw_ctx_refresh(&g.ctx);layout(&g.ui);
    (void)os64_ui_font_follow(&g.ui);
    os64_ui_dispatcher_init(&g.dispatcher,NULL);
    if(os64_ui_dispatcher_add(&g.dispatcher,g.window,&g.ui,window_event,window_paint,NULL)!=OS64_UI_OK){
        os64_ui_font_release(&g.ui);os64_gui_window_destroy(g.window);pulse_sampler_destroy(&g.sampler);return 1;
    }
    g.running=true;g.refresh_ms=g.config.refresh_ms;
    sample_now();os64_ui_mark_dirty(&g.ui,&g.root);os64_ui_paint(&g.ui);
    g.timer=os64_thread(timer_main,NULL);
    if(g.timer<0){os64_complain("pulse: cannot start refresh timer");g.running=false;}
    while(g.running && !g.ui.quit){
        if(os64_ui_dispatcher_turn(&g.dispatcher,true)<0)break;
        if(g.settings.window && g.settings.ui.quit)(void)close_settings();
        if(g.want_settings && os64_ui_dispatcher_can_create(&g.dispatcher))open_settings();
    }
    __atomic_store_n(&g.stop_timer,1,__ATOMIC_RELEASE);
    if(g.timer>=0 && os64_thread_join((int32_t)g.timer,NULL)<0){
        os64_complain("pulse: refresh timer did not join");return 1;
    }
    if(g.timer>=0)(void)os64_close((int32_t)g.timer);
    if(!close_settings()){os64_complain("pulse: settings cleanup refused");return 1;}
    (void)os64_ui_font_release(&g.ui);
    (void)os64_ui_dispatcher_destroy(&g.dispatcher,g.window);
    pulse_sampler_destroy(&g.sampler);
    return 0;
}
