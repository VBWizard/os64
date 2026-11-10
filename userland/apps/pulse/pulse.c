#include "pulse_data.h"
#include "pulse_instruments.h"
#include "pulse_placement.h"
#include "pulse_overlay.h"
#include "pulse_details.h"
#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/ui_window.h"
#include "os64/ui_settings.h"
#include "os64/ui_menu.h"
#include "os64/conf.h"
#include "os64/thread.h"
#include "os64/slurp.h"

static struct {
    os64_ui_t ui;
    os64_draw_ctx_t ctx;
    int64_t window, timer;
    os64_ui_dispatcher_t dispatcher;
    os64_ui_widget_t root, settings_button, meters[PULSE_METERS], instruments;
    os64_ui_widget_t toolbar_compact,toolbar_settings,view_label;
    os64_ui_dropdown_t toolbar_view,view;
    os64_ui_checkbox_t compact;
    os64_ui_class_t instruments_class,toolbar_button_class,toolbar_dropdown_class;
    pulse_instruments_t *renderer;
    os64_ui_widget_t dials[PULSE_DIAL_COUNT];
    pulse_overlay_t overlay;
    int64_t hover_deadline;
    bool overlay_failed;

    os64_ui_menu_session_t *main_menu;
    pulse_size_t target_size,normal_size,minimum;
    bool startup,resize_supported,restore_size,resize_warning,paint_failed;
    int64_t transition_error;
    char transition_message[160];
    os64_ui_widget_t titles[PULSE_METERS], details[PULSE_METERS];
    os64_ui_class_t meter_class;
    os64_ui_widget_t scale_label;
    char scale_text[64];
    char title[PULSE_METERS][96], detail[PULSE_METERS][96];
    pulse_config_t config;
    const pulse_config_t *font_candidate;
    uint32_t font_min_w, font_min_h;
    pulse_sample_t sample;
    pulse_sampler_t sampler;
    bool running, want_settings;
    unsigned stop_timer, refresh_ms;
    os64_ui_settings_t settings;
    os64_ui_menu_session_t *settings_menu;
    os64_ui_checkbox_t show[PULSE_METERS], stay_on_top;
    os64_ui_dropdown_t units;
    os64_ui_widget_t labels[8];
    os64_ui_textfield_t mount, interface, refresh, font_size;
    char mount_text[256], interface_text[64], refresh_text[16], font_size_text[16];
} g;
enum { PULSE_NOTIFY_SAMPLE=1, PULSE_NOTIFY_HOVER=2 };
static void cancel_details(void)
{
    pulse_overlay_cancel(&g.overlay);
    __atomic_store_n(&g.hover_deadline,0,__ATOMIC_RELEASE);
    os64_ui_mark_dirty(&g.ui,&g.instruments);
}
static void publish_hover(void)
{__atomic_store_n(&g.hover_deadline,g.overlay.deadline,__ATOMIC_RELEASE);}
static int focused_dial(void)
{
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)
        if(g.ui.focus==&g.dials[i] && !g.dials[i].hidden && !g.ui.window_blurred)return (int)i;
    return -1;
}
static bool change_view(const pulse_config_t *);
static void sync_view_widgets(void);
static const char *view_label(size_t i,void *user)
{(void)user;return i<PULSE_VIEWS?pulse_views[i].name:"";}
static const char *const names[PULSE_METERS]={"CPU","RAM","Disk","Network"};

static void place(os64_ui_widget_t *w,os64_gui_rect_t box,bool stage)
{ if(stage) os64_ui_widget_stage_bounds(w,box);else w->bounds=box; }

static int32_t meter_height(os64_ui_t *ui,unsigned meter)
{
    int32_t row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI),pad=ui->theme.pad;
    return meter==PULSE_NET?3*row+3*pad+16:2*row+2*pad+8;
}
static int32_t toolbar_height(os64_ui_t *ui)
{return os64_ui_font_row_height(ui,OS64_FONT_ROLE_DOCUMENT)+12;}
static bool toolbar_widths(os64_ui_t *ui,int32_t *view,int32_t *compact,int32_t *settings)
{
    if(os64_ui_text_measure(ui,OS64_FONT_ROLE_DOCUMENT,"Instruments",11,view) ||
       os64_ui_text_measure(ui,OS64_FONT_ROLE_DOCUMENT,"Compact",7,compact) ||
       os64_ui_text_measure(ui,OS64_FONT_ROLE_DOCUMENT,"Settings",8,settings))return false;
    *view+=32;*compact+=16;*settings+=16;return true;
}
static int32_t needed_height(os64_ui_t *ui,const pulse_config_t *c)
{
    int32_t height=2*ui->theme.pad+os64_ui_control_min_height(ui);
    for(unsigned i=0;i<PULSE_METERS;++i)if(c->show[i])height+=meter_height(ui,i)+ui->theme.pad;
    return height;
}
static bool arrange(os64_ui_t *ui,bool stage)
{
    const pulse_config_t *c=g.font_candidate?g.font_candidate:&g.config;
    os64_gui_rect_t b=os64_ui_widget_planned_bounds(&g.root);
    int32_t pad=ui->theme.pad,row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI);
    if(c->view==PULSE_INSTRUMENTS){
        int32_t a,d,e,h=toolbar_height(ui);
        if(!toolbar_widths(ui,&a,&d,&e) || b.w<a+d+e+4*pad)return false;
        os64_gui_rect_t body={0,h+2*pad,b.w,b.h-h-2*pad};
        pulse_instrument_layout_t plan;
        if(!pulse_instruments_layout(ui,c,c->instruments_compact,body,&plan))return false;
        place(&g.toolbar_view.w,(os64_gui_rect_t){pad,pad,a,h},stage);
        place(&g.toolbar_compact,(os64_gui_rect_t){2*pad+a,pad,d,h},stage);
        place(&g.toolbar_settings,(os64_gui_rect_t){b.w-pad-e,pad,e,h},stage);
        place(&g.instruments,body,stage);
        for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)place(&g.dials[i],plan.dials[i],stage);
        return true;
    }
    int32_t bh=os64_ui_control_min_height(ui),width=b.w-2*pad;
    if(width<1 || b.h<needed_height(ui,c)) return false;
    int32_t caption;
    if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"Settings",8,&caption) || caption+2*pad>width) return false;
    place(&g.settings_button,(os64_gui_rect_t){pad,pad,width,bh},stage);
    int32_t y=2*pad+bh;
    for(unsigned i=0;i<PULSE_METERS;++i) if(c->show[i]) {
        int32_t h=meter_height(ui,i);
        place(&g.meters[i],(os64_gui_rect_t){pad,y,width,h},stage);
        place(&g.titles[i],(os64_gui_rect_t){pad,y,width,row},stage);
        int32_t detail_y=y+row+(i==PULSE_NET?pad+8:0);
        place(&g.details[i],(os64_gui_rect_t){pad,detail_y,width,row},stage);
        if(i==PULSE_NET)place(&g.scale_label,(os64_gui_rect_t){pad,y+2*row+2*pad+16,width,row},stage);
        y+=h+pad;
    }
    return true;
}
static void layout(os64_ui_t *ui)
{
    cancel_details();
    g.root.bounds=(os64_gui_rect_t){0,0,(int32_t)g.ctx.surf.width,(int32_t)g.ctx.surf.height};
    (void)arrange(ui,false);
    os64_gui_window_state_t state;
    if(!os64_gui_window_get_state(g.window,&state) && !(state.flags&OS64_GUI_WINDOW_MAXIMIZED))
        g.normal_size=(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height};
}
// Plan fonts and layout before the barrier mutates native geometry.
static os64_font_status_t plan_font(os64_ui_t *ui,void *user,void **out)
{
    (void)user;*out=NULL;
    const pulse_config_t *c=g.font_candidate?g.font_candidate:&g.config;
    if(c->view==PULSE_INSTRUMENTS){
        os64_gui_rect_t floor;int32_t a,d,e;
        if(!pulse_instruments_min_size(ui,c,c->instruments_compact,&floor) || !toolbar_widths(ui,&a,&d,&e))return OS64_FONT_LIMIT;
        int32_t toolbar=a+d+e+4*ui->theme.pad;
        g.font_min_w=(uint32_t)(floor.w>toolbar?floor.w:toolbar);
        g.font_min_h=(uint32_t)(floor.h+toolbar_height(ui)+2*ui->theme.pad);
    }else{
        const char *captions[]={"CPU: waiting for a sample","RAM: 999.9G used",
            "Disk: 999.9G used","Network: waiting for a sample"};
        int32_t width=0,measured;
        if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"Settings",8,&width))return OS64_FONT_LIMIT;
        for(unsigned i=0;i<PULSE_METERS;++i)if(c->show[i]){
            const char *rows[]={captions[i],g.title[i],g.detail[i]};
            for(unsigned j=0;j<3;++j){
                if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,rows[j],os64_strlen(rows[j]),&measured))return OS64_FONT_LIMIT;
                if(measured>width)width=measured;
            }
        }
        width+=4*ui->theme.pad;g.font_min_w=(uint32_t)(width<280?280:width);
        g.font_min_h=(uint32_t)needed_height(ui,c);
    }
    pulse_size_t target=g.restore_size?c->size[pulse_presentation(c)]:(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height};
    os64_gui_window_state_t state;os64_gui_rect_t content;uint32_t sw,sh;
    if(os64_gui_window_get_state(g.window,&state) || os64_gui_window_get_content_rect(g.window,&content) ||
       os64_gui_screen_info(&sw,&sh) || content.w<=0 || content.h<=0)return OS64_FONT_LIMIT;
    uint32_t chrome_w=state.width-(uint32_t)content.w,chrome_h=state.height-(uint32_t)content.h;
    if(sw<=chrome_w || sh<=chrome_h || g.font_min_w>sw-chrome_w || g.font_min_h>sh-chrome_h){
        os64_strcopy(g.transition_message,sizeof(g.transition_message),"That view and text size cannot fit this screen.");return OS64_FONT_LIMIT;
    }
    if((state.flags&OS64_GUI_WINDOW_MAXIMIZED) && !g.restore_size)target=g.normal_size;
    if(target.w<g.font_min_w)target.w=g.font_min_w;
    if(target.h<g.font_min_h)target.h=g.font_min_h;
    if(target.w>sw-chrome_w)target.w=sw-chrome_w;
    if(target.h>sh-chrome_h)target.h=sh-chrome_h;
    if(!g.resize_supported && g.restore_size && !g.startup){
        if(g.font_min_w>g.ctx.surf.width || g.font_min_h>g.ctx.surf.height){
            os64_strcopy(g.transition_message,sizeof(g.transition_message),"Automatic sizing unavailable. Enlarge Pulse manually or update the kernel, then retry.");return OS64_FONT_LIMIT;
        }
        target=(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height};g.resize_warning=true;
    }
    g.target_size=target;
    pulse_size_t visible=(state.flags&OS64_GUI_WINDOW_MAXIMIZED)?(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height}:target;
    os64_ui_widget_stage_bounds(&g.root,(os64_gui_rect_t){0,0,(int32_t)visible.w,(int32_t)visible.h});
    return arrange(ui,true)?OS64_FONT_OK:OS64_FONT_LIMIT;
}
static void discard_font(os64_ui_t *ui,void *user,void *plan)
{(void)ui;(void)user;(void)plan;}
static os64_font_status_t resize_for_font(void *user,void *plan)
{
    (void)user;(void)plan;
    pulse_size_t old_min=g.minimum,old_size=g.normal_size;
    os64_gui_window_state_t state;
    if(os64_gui_window_get_state(g.window,&state))return OS64_FONT_LIMIT;
    if(!(state.flags&OS64_GUI_WINDOW_MAXIMIZED))old_size=(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height};
    int64_t rc=os64_gui_window_set_min_size(g.window,g.font_min_w,g.font_min_h);
    if(rc){g.transition_error=rc;return OS64_FONT_LIMIT;}
    if(g.resize_supported)rc=os64_gui_window_set_content_size(g.window,g.target_size.w,g.target_size.h);
    if(rc){
        g.transition_error=rc;
        int64_t floor_rc=os64_gui_window_set_min_size(g.window,old_min.w,old_min.h);
        int64_t size_rc=os64_gui_window_set_content_size(g.window,old_size.w,old_size.h);
        if(floor_rc)g.minimum=(pulse_size_t){g.font_min_w,g.font_min_h};
        if(os64_draw_ctx_refresh(&g.ctx))g.ui.quit=true;
        os64_gui_window_state_t restored;
        bool readback_failed=os64_gui_window_get_state(g.window,&restored)!=0;
        bool moved=!readback_failed && (restored.x!=state.x || restored.y!=state.y);
        bool wrong_size=!(state.flags&OS64_GUI_WINDOW_MAXIMIZED) &&
            (g.ctx.surf.width!=old_size.w || g.ctx.surf.height!=old_size.h);
        if(floor_rc || size_rc || readback_failed || moved || wrong_size)
            os64_strcopy(g.transition_message,sizeof(g.transition_message),"Size change failed; restoring the previous geometry also failed. Current view retained.");
        return OS64_FONT_LIMIT;
    }
    g.minimum=(pulse_size_t){g.font_min_w,g.font_min_h};
    g.normal_size=g.target_size;return OS64_FONT_OK;
}
static void commit_font(os64_ui_t *ui,void *user,void *plan)
{
    (void)user;(void)plan;
    pulse_overlay_release(&g.overlay);
    pulse_instruments_invalidate(g.renderer);
    if(os64_draw_ctx_refresh(&g.ctx)<0)ui->quit=true;
}
static bool apply_font(const pulse_config_t *config)
{
    g.transition_error=0;g.transition_message[0]=0;g.resize_warning=false;
    g.restore_size=!g.startup && pulse_presentation(config)!=pulse_presentation(&g.config);
    bool created=false;
    if(config->view==PULSE_INSTRUMENTS && !g.renderer){
        g.renderer=pulse_instruments_create(&g.ui);if(!g.renderer)return false;created=true;
    }
    uint8_t *bytes=NULL;size_t length=0;
    if(os64_slurp("/etc/fonts/DejaVuSans.ttf",4*1024*1024,&bytes,&length)!=OS64_SLURP_OK){
        if(created){pulse_instruments_destroy(g.renderer);g.renderer=NULL;}return false;
    }
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT]={0};
    specs[OS64_FONT_ROLE_UI].pixel_height=config->font_size;
    specs[OS64_FONT_ROLE_UI].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,bytes,length};
    specs[OS64_FONT_ROLE_DOCUMENT].pixel_height=config->font_size*2/3<16?16:config->font_size*2/3;
    specs[OS64_FONT_ROLE_DOCUMENT].primary=specs[OS64_FONT_ROLE_UI].primary;
    os64_font_set_t *set=NULL;
    os64_font_status_t status=os64_font_set_prepare(os64_ui_font_context(&g.ui),specs,&set);
    os64_free(bytes);
    if(status){if(created){pulse_instruments_destroy(g.renderer);g.renderer=NULL;}return false;}
    os64_font_consumer_t consumer;os64_ui_font_consumer(&g.ui,&consumer);
    consumer.barrier=resize_for_font;
    g.font_candidate=config;
    status=os64_font_adopt(set,&consumer,1,NULL);
    g.font_candidate=NULL;
    os64_font_set_release(set);
    if(status){
        if(created){pulse_instruments_destroy(g.renderer);g.renderer=NULL;}
        layout(&g.ui);
    }
    return status==OS64_FONT_OK && !g.ui.quit;
}

// Toolbar controls retain toolkit input behavior but use the smaller caption
// role, so primary dial numbers do not force a wide toolbar.
static void toolbar_metrics(os64_ui_widget_t *w,os64_ui_t *ui)
{w->natural_h=toolbar_height(ui);}
static os64_font_status_t toolbar_prepare(os64_ui_widget_t *w,os64_ui_t *ui)
{
    os64_ui_run_release(w->run_staged);w->run_staged=NULL;
    return os64_ui_run_layout(ui,OS64_FONT_ROLE_DOCUMENT,w->text,os64_strlen(w->text),&w->run_staged);
}
static void toolbar_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    bool selected=w==&g.toolbar_compact && g.config.instruments_compact;
    uint32_t bg=w->pressed||selected?t->text_sel_bg:t->button_face;
    os64_draw_fill_rect(&ctx->surf,w->bounds,bg);
    os64_draw_rect(&ctx->surf,w->bounds,w->focused?t->focus_ring:t->field_border);
    int32_t width=0;
    if(os64_ui_run_width(&g.ui,&w->run,OS64_FONT_ROLE_DOCUMENT,w->text,os64_strlen(w->text),&width))return;
    os64_ui_draw_run(&g.ui,w->run,OS64_FONT_ROLE_DOCUMENT,&ctx->surf,w->bounds,
        w->bounds.x+(w->bounds.w-width)/2,w->bounds.y+6,w->text,os64_strlen(w->text),t->button_fg,bg);
}
static os64_font_status_t toolbar_dropdown_prepare(os64_ui_widget_t *w,os64_ui_t *ui)
{
    os64_ui_dropdown_t *d=(os64_ui_dropdown_t *)w;const char *text=view_label((size_t)d->selected,NULL);
    os64_ui_run_release(d->run_staged);d->run_staged=NULL;
    return os64_ui_run_layout(ui,OS64_FONT_ROLE_DOCUMENT,text,os64_strlen(text),&d->run_staged);
}
static void toolbar_dropdown_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_ui_dropdown_t *d=(os64_ui_dropdown_t *)w;const char *text=view_label((size_t)d->selected,NULL);
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->field_bg);
    os64_draw_rect(&ctx->surf,w->bounds,w->focused?t->focus_ring:t->field_border);
    int32_t ax=w->bounds.x+w->bounds.w-12,ay=w->bounds.y+w->bounds.h/2-2;
    for(int32_t i=0;i<4;++i)os64_draw_hline(&ctx->surf,ax-3+i,ay+i,7-2*i,t->field_fg);
    os64_gui_rect_t clip={w->bounds.x+4,w->bounds.y+2,w->bounds.w-24,w->bounds.h-4};
    os64_ui_draw_text(&g.ui,&d->run,OS64_FONT_ROLE_DOCUMENT,&ctx->surf,clip,clip.x,w->bounds.y+6,
        text,os64_strlen(text),t->field_fg,t->field_bg);
}
static void instruments_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    (void)t;pulse_instrument_layout_t plan;
    pulse_instrument_readings_t readings={.current=g.sample};
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){readings.peak_valid[i]=g.sample.peak_valid[i];readings.peak[i]=g.sample.peak[i];}
    bool ok=pulse_instruments_layout(&g.ui,&g.config,g.config.instruments_compact,w->bounds,&plan) &&
        pulse_instruments_paint(g.renderer,&ctx->surf,&plan,&g.config,&readings);
    if(!ok){
        os64_draw_fill_rect(&ctx->surf,w->bounds,0xff402030);
        const char *error="Could not draw instruments";
        os64_draw_text_clipped(&ctx->surf,w->bounds,w->bounds.x+8,w->bounds.y+8,error,os64_strlen(error),0xffffffff,0xff402030);
        if(!g.paint_failed)os64_complain("pulse: instrument rendering failed");
    }
    g.paint_failed=!ok;
    if(ok){
        int focused=focused_dial();
        if(focused>=0)os64_draw_rect(&ctx->surf,plan.dials[focused],0xff87b9e9);
        int shown=g.overlay.shown;
        if(shown>=0 && shown<PULSE_DIAL_COUNT && plan.visible[shown]){
            char text[768];pulse_details(text,sizeof(text),(unsigned)shown,&g.config,&g.sample);
            bool painted=pulse_overlay_paint(&g.overlay,&g.ui,&ctx->surf,w->bounds,plan.dials[shown],text);
            if(!painted && !g.overlay_failed)os64_complain("pulse: could not draw detail card");
            g.overlay_failed=!painted;
        }
    }
}
static void meter_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->panel_bg);
    unsigned i=(unsigned)(w-g.meters),pct=0;
    bool valid=false;
    if(i==PULSE_CPU){valid=g.sample.cpu_valid;pct=g.sample.cpu_percent;}
    else if(i==PULSE_RAM){valid=g.sample.ram_valid;pct=pulse_percent(g.sample.ram_used,g.sample.ram_total);}
    else if(i==PULSE_DISK){valid=g.sample.disk_valid;pct=pulse_percent(g.sample.disk_used,g.sample.disk_total);}
    else {
        // Both directions use the same recent-peak ceiling, not link capacity.
        int32_t row=os64_ui_font_row_height(&g.ui,OS64_FONT_ROLE_UI);
        uint64_t rates[]={g.sample.download,g.sample.upload};
        uint32_t colors[]={0xff409bff,0xffed6868};
        for(unsigned n=0;n<2;++n){
            os64_gui_rect_t bar={w->bounds.x,w->bounds.y+(int32_t)n*(row+t->pad+8)+row+t->pad/2,w->bounds.w,8};
            os64_draw_fill_rect(&ctx->surf,bar,t->scroll_track);
            if(g.sample.net_valid && rates[n] && g.sample.network_scale){
                bar.w=(int32_t)((int64_t)bar.w*pulse_percent(rates[n],g.sample.network_scale)/100);
                if(bar.w<1)bar.w=1;
                os64_draw_fill_rect(&ctx->surf,bar,colors[n]);
            }
        }
        return;
    }
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
    if(g.config.view==PULSE_INSTRUMENTS){os64_ui_mark_dirty(&g.ui,&g.instruments);return;}
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
    pulse_format_rate(a,sizeof(a),g.sample.network_scale,g.config.units);
    // A small ceiling must not round to a displayed zero in large units.
    if(g.sample.network_scale && a[0]=='0' && a[1]=='.' && a[2]=='0' && a[3]==' ')
        pulse_format_rate(a,sizeof(a),g.sample.network_scale,g.config.units>=PULSE_BITS?PULSE_KBIT:PULSE_KIB);
    os64_snprintf(g.scale_text,sizeof(g.scale_text),"Scale: %s",a);
    os64_ui_run_release(g.scale_label.run);g.scale_label.run=NULL;
    for(unsigned i=0;i<PULSE_METERS;++i) text_changed(i);
}
static void sample_now(void)
{
    pulse_sample(&g.sampler,&g.config,&g.sample);
    render_sample();
}
static void sync_view_widgets(void)
{
    bool graphical=g.config.view==PULSE_INSTRUMENTS;
    const unsigned meter[]={PULSE_CPU,PULSE_RAM,PULSE_DISK,PULSE_NET,PULSE_NET};
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)
        os64_ui_set_hidden(&g.ui,&g.dials[i],!graphical || !g.config.show[meter[i]]);
    os64_ui_set_hidden(&g.ui,&g.settings_button,graphical);
    os64_ui_set_hidden(&g.ui,&g.instruments,!graphical);
    os64_ui_set_hidden(&g.ui,&g.toolbar_view.w,!graphical);
    os64_ui_set_hidden(&g.ui,&g.toolbar_compact,!graphical);
    os64_ui_set_hidden(&g.ui,&g.toolbar_settings,!graphical);
    for(unsigned i=0;i<PULSE_METERS;++i)os64_ui_set_hidden(&g.ui,&g.meters[i],graphical || !g.config.show[i]);
    os64_ui_dropdown_set(&g.ui,&g.toolbar_view,PULSE_VIEWS,(int)g.config.view);
    if(g.settings.window){
        os64_ui_dropdown_set(&g.settings.ui,&g.view,PULSE_VIEWS,(int)g.config.view);
        os64_ui_checkbox_set(&g.settings.ui,&g.compact,g.config.instruments_compact);
        os64_ui_set_enabled(&g.settings.ui,&g.compact.w,pulse_views[g.config.view].compact);
    }
}
static const char *transition_failure(void)
{
    if(*g.transition_message)return g.transition_message;
    switch(g.transition_error){
    case OS64_GUI_ERR_BELOW_MINIMUM:return "Saved size is below the window frame's minimum; view unchanged.";
    case OS64_GUI_ERR_ABOVE_CAPACITY:return "Requested size exceeds this window's capacity; view unchanged.";
    case OS64_GUI_ERR_SCREEN_FIT:return "Requested size cannot fit this screen; view unchanged.";
    default:return "Could not prepare the font or window size; view unchanged.";
    }
}
static bool change_view(const pulse_config_t *c)
{
    cancel_details();
    if(!apply_font(c)){sync_view_widgets();return false;}
    g.config=*c;sync_view_widgets();layout(&g.ui);render_sample();os64_ui_mark_dirty(&g.ui,&g.root);
    return true;
}
static void toolbar_changed(os64_ui_dropdown_t *d,void *user)
{
    (void)user;pulse_config_t c=g.config;c.view=(unsigned)d->selected;
    if(!change_view(&c)){
        os64_strcopy(g.transition_message,sizeof(g.transition_message),transition_failure());g.want_settings=true;
    }else if(g.resize_warning){
        os64_strcopy(g.transition_message,sizeof(g.transition_message),"View changed. Automatic size restoration is unavailable; update the kernel to enable it.");g.want_settings=true;
    }
}
static void compact_clicked(os64_ui_widget_t *w,void *user)
{
    (void)w;(void)user;pulse_config_t c=g.config;c.instruments_compact=!c.instruments_compact;
    if(!change_view(&c)){
        os64_strcopy(g.transition_message,sizeof(g.transition_message),transition_failure());g.want_settings=true;
    }else if(g.resize_warning){
        os64_strcopy(g.transition_message,sizeof(g.transition_message),"Compact changed. Automatic size restoration is unavailable; update the kernel to enable it.");g.want_settings=true;
    }
}
static void ask_settings(os64_ui_widget_t *w,void *user)
{ (void)w;(void)user;cancel_details();g.want_settings=true; }
static void close_window(os64_ui_t *ui)
{ (void)ui;cancel_details();g.running=false; }
// Native state is authoritative, including changes from frame compositions
// and the keyboard. Input-loss and periodic reconciliation cover dropped hints.
static void sync_pin_state(void)
{
    os64_gui_window_state_t state;
    if(os64_gui_window_get_state(g.window,&state)<0)return;
    if((state.flags&(OS64_GUI_WINDOW_COVERED|OS64_GUI_WINDOW_MINIMIZED)) &&
       (g.overlay.shown>=0 || g.overlay.deadline))cancel_details();
    g.config.stay_on_top=(state.flags & OS64_GUI_WINDOW_PINNED)!=0;
    if(g.settings.window>0){
        os64_ui_checkbox_set(&g.settings.ui,&g.stay_on_top,g.config.stay_on_top);
        os64_gui_window_state_t dialog;
        if(!os64_gui_window_get_state(g.settings.window,&dialog) &&
           ((dialog.flags & OS64_GUI_WINDOW_PINNED)!=0)!=g.config.stay_on_top)
            (void)os64_gui_window_set_pinned(g.settings.window,g.config.stay_on_top);
    }
}
static void pin_changed(os64_ui_checkbox_t *box,void *user)
{
    (void)user;
    if(os64_gui_window_set_pinned(g.window,box->checked)<0){
        os64_ui_settings_report(&g.settings,"Could not change Stay on top; check that the kernel is updated.");
        os64_ui_checkbox_set(&g.settings.ui,box,g.config.stay_on_top);
        sync_pin_state();return;
    }
    sync_pin_state();
    // Re-raising the parent into its band must not cover the dialog used here.
    if(g.settings.window>0)(void)os64_gui_window_focus(g.settings.window);
    os64_ui_settings_report(&g.settings,"Stay on top changed. Save as default to remember it.");
}
static void window_event(void *user,const os64_gui_event_t *ev)
{
    (void)user;
    sync_pin_state();
    if(ev->type==OS64_GUI_EVENT_WINDOW_PLACEMENT){
        if(pulse_record_placement(&g.config,ev))os64_complain("pulse: could not save window placement");
    }
    else if(ev->type==OS64_GUI_EVENT_SETTINGS){cancel_details();g.want_settings=true;}
    else if(ev->type==OS64_GUI_EVENT_DOORBELL){
        if(ev->doorbell.mask&PULSE_NOTIFY_SAMPLE)sample_now();
        if((ev->doorbell.mask&PULSE_NOTIFY_HOVER) && pulse_overlay_due(&g.overlay,os64_micros())){
            publish_hover();os64_ui_mark_dirty(&g.ui,&g.instruments);
        }
    }else{
        bool pointer=ev->type==OS64_GUI_EVENT_POINTER_STATE;
        bool keyboard=ev->type==OS64_GUI_EVENT_KEY_DOWN;
        if(ev->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN || ev->type==OS64_GUI_EVENT_INPUT_LOST ||
           ev->type==OS64_GUI_EVENT_WINDOW_COVERED || ev->type==OS64_GUI_EVENT_WINDOW_RESIZE ||
           ev->type==OS64_GUI_EVENT_WINDOW_FOCUS || (keyboard && ev->key.ascii==27))cancel_details();
        os64_ui_dispatch(&g.ui,ev);
        if(g.config.view==PULSE_INSTRUMENTS && !g.settings.window && !g.want_settings){
            if(pointer){
                int hit=-1;pulse_instrument_layout_t plan;
                if(ev->pointer.inside && pulse_instruments_layout(&g.ui,&g.config,g.config.instruments_compact,g.instruments.bounds,&plan))
                    hit=pulse_instruments_hit(&plan,ev->pointer.x,ev->pointer.y);
                int before=g.overlay.shown;
                pulse_overlay_hover(&g.overlay,hit,os64_micros());publish_hover();
                if(before!=g.overlay.shown)os64_ui_mark_dirty(&g.ui,&g.instruments);
            }else if((keyboard && ev->key.ascii=='\t') || (ev->type==OS64_GUI_EVENT_WINDOW_FOCUS && ev->focus.gained)){
                pulse_overlay_focus(&g.overlay,focused_dial());publish_hover();os64_ui_mark_dirty(&g.ui,&g.instruments);
            }
        }
    }
}
static void window_paint(void *user)
{ (void)user;os64_ui_paint(&g.ui); }

// The timer touches no widgets or samplers. Join it before releasing the
// handle it rings; main owns sampling and the two window lifetimes.
static int64_t timer_main(void *user)
{
    (void)user;
    int64_t notified_deadline=0;
    while(!__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)){
        unsigned remaining=__atomic_load_n(&g.refresh_ms,__ATOMIC_RELAXED);
        while(remaining && !__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)){
            unsigned step=remaining>250?250:remaining;
            (void)os64_sleep(step);remaining-=step;
            int64_t deadline=__atomic_load_n(&g.hover_deadline,__ATOMIC_ACQUIRE);
            if(deadline && deadline!=notified_deadline && os64_micros()>=deadline){
                (void)os64_gui_event_ring(g.window,PULSE_NOTIFY_HOVER);notified_deadline=deadline;
            }
        }
        if(!__atomic_load_n(&g.stop_timer,__ATOMIC_ACQUIRE)) (void)os64_gui_event_ring(g.window,PULSE_NOTIFY_SAMPLE);
    }
    return 0;
}

static const char *unit_label(size_t i,void *user)
{ (void)user;return i<PULSE_UNITS?pulse_unit_names[i]:""; }
static void settings_view_changed(os64_ui_dropdown_t *d,void *user)
{
    (void)user;bool compact=d->selected>=0 && d->selected<PULSE_VIEWS && pulse_views[d->selected].compact;
    os64_ui_set_enabled(&g.settings.ui,&g.compact.w,compact);
}
static bool settings_arrange(os64_ui_settings_t *d,os64_gui_rect_t b,int32_t row,bool stage)
{
    int32_t gap=d->ui.theme.gap,half=(b.w-gap)/2;
    if(half<100)return false;
    place(&g.view_label,(os64_gui_rect_t){b.x,b.y,half,row},stage);
    place(&g.view.w,(os64_gui_rect_t){b.x+half+gap,b.y,half,row},stage);
    place(&g.compact.w,(os64_gui_rect_t){b.x,b.y+row,b.w,row},stage);
    for(unsigned i=0;i<PULSE_METERS;++i)
        place(&g.show[i].w,(os64_gui_rect_t){b.x+(int32_t)(i%2)*(half+gap),b.y+(int32_t)(i/2+2)*row,half,row},stage);
    place(&g.stay_on_top.w,(os64_gui_rect_t){b.x,b.y+4*row,b.w,row},stage);
    os64_ui_widget_t *fields[]={&g.units.w,&g.mount.w,&g.interface.w,&g.refresh.w,&g.font_size.w};
    for(unsigned i=0;i<5;++i){
        place(&g.labels[i],(os64_gui_rect_t){b.x,b.y+(int32_t)(i+5)*row,half,row},stage);
        place(fields[i],(os64_gui_rect_t){b.x+half+gap,b.y+(int32_t)(i+5)*row,half,row},stage);
    }
    for(unsigned i=5;i<8;++i)
        place(&g.labels[i],(os64_gui_rect_t){b.x,b.y+(int32_t)(i+5)*row,b.w,row},stage);
    return true;
}
static void settings_apply(os64_ui_settings_t *d,bool save)
{
    pulse_config_t c=g.config;
    uint64_t interval,size;
    if(!os64_parse_u64(g.refresh.buf,&interval) || interval<250 || interval>10000){
        os64_ui_settings_report(d,"Refresh must be 250 to 10000 milliseconds.");return;
    }
    if(!os64_parse_u64(g.font_size.buf,&size) || size<16 || size>48){
        os64_ui_settings_report(d,"Text size must be 16 to 48 pixels.");return;
    }
    c.font_size=(unsigned)size;
    if(g.view.selected<0 || g.view.selected>=PULSE_VIEWS){os64_ui_settings_report(d,"Choose a view.");return;}
    c.view=(unsigned)g.view.selected;c.instruments_compact=g.compact.checked;
    if(g.mount.buf[0]!='/' || !g.interface.buf[0] || g.units.selected<0 || g.units.selected>=PULSE_UNITS){
        os64_ui_settings_report(d,"Choose units, an absolute mount path, and an interface or all.");return;
    }
    c.refresh_ms=(unsigned)interval;c.units=(unsigned)g.units.selected;
    os64_strcopy(c.mount,sizeof(c.mount),g.mount.buf);
    os64_strcopy(c.interface,sizeof(c.interface),g.interface.buf);
    for(unsigned i=0;i<PULSE_METERS;++i)c.show[i]=g.show[i].checked;
    bool source_changed=!os64_streq(c.interface,g.config.interface);
    if(!change_view(&c)){os64_ui_settings_report(d,transition_failure());return;}
    __atomic_store_n(&g.refresh_ms,c.refresh_ms,__ATOMIC_RELAXED);
    if(source_changed)sample_now();
    if(save){
        char refresh[16],font_size[16];os64_snprintf(refresh,sizeof(refresh),"%u",c.refresh_ms);
        os64_snprintf(font_size,sizeof(font_size),"%u",c.font_size);
        os64_conf_pair_t pairs[]={
            {"cpu",c.show[0]?"on":"off"},{"ram",c.show[1]?"on":"off"},
            {"disk",c.show[2]?"on":"off"},{"net",c.show[3]?"on":"off"},
            {"network_units",pulse_unit_names[c.units]},{"mount",c.mount},
            {"interface",c.interface},{"refresh_ms",refresh},{"font_size",font_size},{"stay_on_top",c.stay_on_top?"on":"off"},
            {"view",c.view==PULSE_INSTRUMENTS?"instruments":"classic"},
            {"instruments_compact",c.instruments_compact?"on":"off"}};
        if(os64_conf_write("pulse.conf",pairs,sizeof(pairs)/sizeof(*pairs))){
            os64_ui_settings_report(d,"Applied here, but settings could not be saved.");return;
        }
    }
    os64_ui_settings_report(d,g.resize_warning?
        "Applied; automatic size restoration is unavailable. Update the kernel to enable it.":
        save?"Applied and saved.":"Applied to this window.");
}
static void settings_event(void *user,const os64_gui_event_t *ev)
{ (void)user;sync_pin_state();os64_ui_settings_event(&g.settings,ev); }
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
    os64_ui_label(&g.view_label,"View");os64_ui_add_child(&g.settings.body,&g.view_label);
    os64_ui_dropdown(&g.view,PULSE_VIEWS,view_label,settings_view_changed,NULL);
    os64_ui_add_child(&g.settings.body,&g.view.w);
    os64_ui_dropdown_set(&g.settings.ui,&g.view,PULSE_VIEWS,(int)g.config.view);
    os64_ui_checkbox(&g.compact,"Compact (Instruments only)",g.config.instruments_compact,NULL,NULL);
    os64_ui_add_child(&g.settings.body,&g.compact.w);settings_view_changed(&g.view,NULL);
    for(unsigned i=0;i<PULSE_METERS;++i){
        os64_ui_checkbox(&g.show[i],names[i],g.config.show[i],NULL,NULL);
        os64_ui_add_child(&g.settings.body,&g.show[i].w);
    }
    os64_ui_checkbox(&g.stay_on_top,"Stay on top",g.config.stay_on_top,pin_changed,NULL);
    os64_ui_add_child(&g.settings.body,&g.stay_on_top.w);
    const char *labels[]={"Network units","Disk mount","Network interface","Refresh (milliseconds)","Text size (pixels)",
        "Use all to combine network interfaces.","A mount such as / selects its filesystem space.",
        "Hide meters here; resize the panel to your liking."};
    for(unsigned i=0;i<8;++i){os64_ui_label(&g.labels[i],labels[i]);os64_ui_add_child(&g.settings.body,&g.labels[i]);}
    os64_ui_dropdown(&g.units,PULSE_UNITS,unit_label,NULL,NULL);
    os64_ui_add_child(&g.settings.body,&g.units.w);
    os64_ui_dropdown_set(&g.settings.ui,&g.units,PULSE_UNITS,(int)g.config.units);
    os64_ui_textfield(&g.mount,g.mount_text,sizeof(g.mount_text),NULL,NULL,NULL);
    os64_ui_textfield(&g.interface,g.interface_text,sizeof(g.interface_text),NULL,NULL,NULL);
    os64_ui_textfield(&g.refresh,g.refresh_text,sizeof(g.refresh_text),NULL,NULL,NULL);
    os64_ui_textfield(&g.font_size,g.font_size_text,sizeof(g.font_size_text),NULL,NULL,NULL);
    os64_ui_add_child(&g.settings.body,&g.font_size.w);
    os64_ui_add_child(&g.settings.body,&g.mount.w);os64_ui_add_child(&g.settings.body,&g.interface.w);
    os64_ui_add_child(&g.settings.body,&g.refresh.w);
    os64_ui_textfield_set(&g.settings.ui,&g.mount,g.config.mount);
    os64_ui_textfield_set(&g.settings.ui,&g.interface,g.config.interface);
    char value[16];os64_snprintf(value,sizeof(value),"%u",g.config.refresh_ms);
    os64_ui_textfield_set(&g.settings.ui,&g.refresh,value);
    os64_snprintf(value,sizeof(value),"%u",g.config.font_size);
    os64_ui_textfield_set(&g.settings.ui,&g.font_size,value);
}
static void open_settings(void)
{
    cancel_details();
    g.want_settings=false;
    sync_pin_state();
    if(!os64_ui_settings_open(&g.settings,g.window,"Pulse Settings",13,settings_arrange,settings_apply,NULL))return;
    build_settings_widgets();
    os64_ui_settings_ready(&g.settings);
    if(*g.transition_message){os64_ui_settings_report(&g.settings,g.transition_message);g.transition_message[0]=0;}
    if(os64_ui_dispatcher_add(&g.dispatcher,g.settings.window,&g.settings.ui,settings_event,settings_paint,NULL)!=OS64_UI_OK){
        os64_ui_settings_close(&g.settings);return;
    }
    g.settings.dispatcher=&g.dispatcher;
    if(os64_ui_menu_session_create(&g.dispatcher,&g.settings.ui,g.settings.window,NULL,NULL,NULL,&g.settings_menu)!=OS64_UI_OK)
        (void)close_settings();
}

static void build_widgets(void)
{
    pulse_overlay_init(&g.overlay);
    os64_ui_panel(&g.root);os64_ui_set_root(&g.ui,&g.root);
    os64_ui_panel(&g.instruments);g.instruments_class=*g.instruments.cls;
    g.instruments_class.paint=instruments_paint;g.instruments.cls=&g.instruments_class;
    os64_ui_add_child(&g.root,&g.instruments);
    static const os64_ui_class_t dial_class={.name="Pulse dial"};
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){
        g.dials[i].cls=&dial_class;g.dials[i].focusable=true;
        os64_ui_add_child(&g.instruments,&g.dials[i]);
    }
    os64_ui_dropdown(&g.toolbar_view,PULSE_VIEWS,view_label,toolbar_changed,NULL);
    g.toolbar_dropdown_class=*g.toolbar_view.w.cls;
    g.toolbar_dropdown_class.paint=toolbar_dropdown_paint;g.toolbar_dropdown_class.prepare=toolbar_dropdown_prepare;
    g.toolbar_dropdown_class.metrics=toolbar_metrics;g.toolbar_view.w.cls=&g.toolbar_dropdown_class;
    os64_ui_add_child(&g.root,&g.toolbar_view.w);
    os64_ui_button(&g.toolbar_compact,"Compact",compact_clicked,NULL);
    os64_ui_button(&g.toolbar_settings,"Settings",ask_settings,NULL);
    g.toolbar_button_class=*g.toolbar_compact.cls;g.toolbar_button_class.paint=toolbar_paint;
    g.toolbar_button_class.prepare=toolbar_prepare;g.toolbar_button_class.metrics=toolbar_metrics;
    g.toolbar_compact.cls=g.toolbar_settings.cls=&g.toolbar_button_class;
    os64_ui_add_child(&g.root,&g.toolbar_compact);os64_ui_add_child(&g.root,&g.toolbar_settings);
    os64_ui_button(&g.settings_button,"Settings",ask_settings,NULL);os64_ui_add_child(&g.root,&g.settings_button);
    for(unsigned i=0;i<PULSE_METERS;++i){
        os64_ui_panel(&g.meters[i]);
        if(!i){g.meter_class=*g.meters[i].cls;g.meter_class.paint=meter_paint;}
        g.meters[i].cls=&g.meter_class;g.meters[i].hidden=!g.config.show[i];
        os64_ui_add_child(&g.root,&g.meters[i]);
        os64_ui_label(&g.titles[i],g.title[i]);os64_ui_add_child(&g.meters[i],&g.titles[i]);
        os64_ui_label(&g.details[i],g.detail[i]);os64_ui_add_child(&g.meters[i],&g.details[i]);
    }
    os64_ui_label(&g.scale_label,g.scale_text);os64_ui_add_child(&g.meters[PULSE_NET],&g.scale_label);
    sync_view_widgets();
}

int main(void)
{
    pulse_config_defaults(&g.config);
    int64_t rc=os64_conf_find_read("pulse.conf",pulse_config_line,&g.config,NULL,0);
    if(rc<0 && rc!=OS64_CONF_NO_FILE)os64_complain("pulse: configuration could not be fully read; using available settings");
    if(!pulse_sampler_init(&g.sampler)){os64_complain("pulse: cannot allocate samplers");return 1;}
    pulse_size_t initial=g.config.size[pulse_presentation(&g.config)];
    os64_ui_window_options_t options={.title="Pulse",.x=g.config.x,.y=g.config.y,.content_width=initial.w,.content_height=initial.h,
        .flags=(g.config.stay_on_top?OS64_GUI_WINDOW_PINNED:0)|OS64_GUI_WINDOW_START_UNFOCUSED|OS64_GUI_WINDOW_HAS_SETTINGS};
    if(os64_ui_window_open(&g.ui,&g.ctx,&g.window,&options)!=OS64_UI_OK){
        if(g.window>0)(void)os64_gui_window_destroy(g.window);
        pulse_sampler_destroy(&g.sampler);return 1;
    }
    build_widgets();
    g.ui.on_resize=layout;g.ui.on_close=close_window;
    (void)os64_ui_font_planner(&g.ui,plan_font,commit_font,discard_font,NULL);
    g.minimum=(pulse_size_t){64,32};g.normal_size=(pulse_size_t){g.ctx.surf.width,g.ctx.surf.height};
    int64_t probe=os64_gui_window_set_content_size(g.window,g.ctx.surf.width,g.ctx.surf.height);
    os64_gui_window_state_t owned;
    if(os64_gui_window_get_state(g.window,&owned)){
        os64_complain("pulse: window readback failed");os64_ui_font_release(&g.ui);
        os64_gui_window_destroy(g.window);pulse_sampler_destroy(&g.sampler);return 1;
    }
    g.resize_supported=probe!=OS64_GUI_ERR_INVALID_HANDLE;
    g.startup=true;layout(&g.ui);
    // The gadget has its own face/size; session colours can still follow.
    if(!apply_font(&g.config)){
        os64_complain("pulse: cannot load or fit the gadget font");
        pulse_instruments_destroy(g.renderer);os64_ui_font_release(&g.ui);os64_gui_window_destroy(g.window);pulse_sampler_destroy(&g.sampler);return 1;
    }
    g.startup=false;sync_view_widgets();layout(&g.ui);
    os64_ui_dispatcher_init(&g.dispatcher,NULL);
    if(os64_ui_dispatcher_add(&g.dispatcher,g.window,&g.ui,window_event,window_paint,NULL)!=OS64_UI_OK){
        pulse_instruments_destroy(g.renderer);os64_ui_font_release(&g.ui);os64_gui_window_destroy(g.window);pulse_sampler_destroy(&g.sampler);return 1;
    }
    if(os64_ui_menu_session_create(&g.dispatcher,&g.ui,g.window,NULL,NULL,NULL,&g.main_menu)!=OS64_UI_OK){
        os64_complain("pulse: cannot create view menu");pulse_instruments_destroy(g.renderer);
        os64_ui_font_release(&g.ui);os64_ui_dispatcher_destroy(&g.dispatcher,g.window);pulse_sampler_destroy(&g.sampler);return 1;
    }
    g.running=true;g.refresh_ms=g.config.refresh_ms;
    sample_now();os64_ui_mark_dirty(&g.ui,&g.root);os64_ui_paint(&g.ui);
    g.timer=os64_thread(timer_main,NULL);
    if(g.timer<0){os64_complain("pulse: cannot start refresh timer");g.running=false;}
    while(g.running && !g.ui.quit){
        if(os64_ui_dispatcher_turn(&g.dispatcher,true)<0)break;
        sync_pin_state();
        if(g.settings.window && g.settings.ui.quit)(void)close_settings();
        if(g.want_settings && os64_ui_dispatcher_can_create(&g.dispatcher))open_settings();
    }
    cancel_details();
    __atomic_store_n(&g.stop_timer,1,__ATOMIC_RELEASE);
    if(g.timer>=0 && os64_thread_join((int32_t)g.timer,NULL)<0){
        os64_complain("pulse: refresh timer did not join");return 1;
    }
    if(g.timer>=0)(void)os64_close((int32_t)g.timer);
    if(!close_settings()){os64_complain("pulse: settings cleanup refused");return 1;}
    if(os64_ui_menu_session_dispose(g.main_menu)!=OS64_UI_OK){os64_complain("pulse: view menu cleanup refused");return 1;}
    pulse_overlay_release(&g.overlay);
    pulse_instruments_destroy(g.renderer);
    (void)os64_ui_font_release(&g.ui);
    (void)os64_ui_dispatcher_destroy(&g.dispatcher,g.window);
    pulse_sampler_destroy(&g.sampler);
    return 0;
}
