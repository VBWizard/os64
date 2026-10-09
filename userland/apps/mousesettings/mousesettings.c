#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/mouse_settings.h"

static os64_ui_t ui;
static os64_draw_ctx_t draw;
static os64_ui_widget_t root,title,device,connection,speed_label,hint,status;
static os64_ui_widget_t previous,next,refresh,defaults,apply,save,forget;
static os64_ui_slider_t speed;
static os64_ui_checkbox_t primary;
static os64_mouse_snapshot_t snapshot;
static unsigned selected;
static char device_text[96],connection_text[64],speed_text[64],status_text[128];
static const char *hint_text="Apply for this session. Save to keep after restart.";

typedef struct { int row,button,width,height,min_width,min_height; } mouse_layout_t;
static mouse_layout_t layout_state;
static int maximum(int a,int b) { return a>b?a:b; }
static void place(os64_ui_widget_t *w,int x,int y,int width,int height,bool staged)
{
    os64_gui_rect_t r={x,y,width,height};
    if(staged) os64_ui_widget_stage_bounds(w,r); else w->bounds=r;
}
static void arrange(const mouse_layout_t *m,bool staged)
{
    int width=m->width-40,row=m->row,button=m->button,y=20;
    place(&root,0,0,m->width,m->height,staged);
    place(&title,20,y,width,row,staged); y+=row+16;
    place(&device,20,y,width,row,staged); y+=row+4;
    place(&connection,20,y,width,row,staged); y+=row+10;
    int third=(width-16)/3;
    place(&previous,20,y,third,button,staged);
    place(&next,28+third,y,third,button,staged);
    place(&refresh,36+2*third,y,width-2*third-16,button,staged); y+=button+20;
    place(&speed_label,20,y,width,row,staged); y+=row+8;
    place(&speed.w,20,y,width,button,staged); y+=button+16;
    place(&primary.w,20,y,width,button,staged); y+=button+20;
    int fourth=(width-24)/4;
    place(&defaults,20,y,fourth,button,staged);
    place(&apply,28+fourth,y,fourth,button,staged);
    place(&save,36+2*fourth,y,fourth,button,staged);
    place(&forget,44+3*fourth,y,width-3*fourth-24,button,staged); y+=button+16;
    place(&hint,20,y,width,row,staged); y+=row+6;
    place(&status,20,y,width,row,staged);
    if(!staged) os64_ui_mark_dirty(&ui,&root);
}
static os64_font_status_t measure(os64_ui_t *u,mouse_layout_t *m)
{
    m->row=maximum(20,os64_ui_font_row_height(u,OS64_FONT_ROLE_UI));
    m->button=maximum(34,m->row+12);
    m->min_width=520;
    const char *labels[]={hint_text,"Use right button as primary","Applied; saving failed. Preferences remain active.",
        "Bluetooth mouse (de:52:33:bd:ac:a6)","Forgotten. Move the new mouse, then choose Refresh.",
        "Saved preferences removed; refresh to check live settings."};
    for(unsigned i=0;i<sizeof(labels)/sizeof(labels[0]);i++) {
        int32_t width=0;
        os64_font_status_t result=os64_ui_text_measure(u,OS64_FONT_ROLE_UI,labels[i],os64_strlen(labels[i]),&width);
        if(result) return result;
        m->min_width=maximum(m->min_width,width+40);
    }
    m->min_height=6*m->row+4*m->button+156;
    m->width=maximum(m->min_width,(int)draw.surf.width);
    m->height=maximum(m->min_height,(int)draw.surf.height);
    return OS64_FONT_OK;
}
static void resized(os64_ui_t *u)
{
    if(u->grab) u->grab->pressed=false;
    u->grab=NULL;
    layout_state.width=(int)draw.surf.width; layout_state.height=(int)draw.surf.height;
    arrange(&layout_state,false);
}
static os64_font_status_t plan_font(os64_ui_t *u,void *ctx,void **out)
{
    (void)ctx; *out=NULL;
    mouse_layout_t *m=os64_malloc(sizeof(*m));
    if(!m) return OS64_FONT_NO_MEMORY;
    os64_font_status_t result=measure(u,m);
    if(!result && (m->width>(int)draw.surf.width || m->height>(int)draw.surf.height)) {
        uint32_t sw,sh; os64_gui_window_state_t state;
        if(draw.win<=0 || os64_gui_screen_info(&sw,&sh) || os64_gui_window_get_state(draw.win,&state)) result=OS64_FONT_LIMIT;
        else if(maximum(0,state.x)+(int64_t)m->width+state.width-draw.surf.width>sw ||
                maximum(0,state.y)+(int64_t)m->height+state.height-draw.surf.height>sh) result=OS64_FONT_LIMIT;
    }
    if(result) { os64_free(m); return result; }
    arrange(m,true); *out=m; return OS64_FONT_OK;
}
static void discard_font(os64_ui_t *u,void *ctx,void *plan)
{ (void)u; (void)ctx; os64_free(plan); }
static void commit_font(os64_ui_t *u,void *ctx,void *plan)
{
    (void)ctx; mouse_layout_t *m=plan;
    if(draw.win>0 && (os64_gui_window_set_min_size(draw.win,m->min_width,m->min_height) || os64_draw_ctx_refresh(&draw)))
        u->quit=true;
    else { layout_state=*m; resized(u); }
    os64_free(m);
}
static void message(const char *text)
{
    os64_strcopy(status_text,sizeof(status_text),text);
    os64_ui_mark_dirty(&ui,&status);
}
static void speed_changed(os64_ui_slider_t *slider,void *ctx)
{
    (void)ctx;
    os64_snprintf(speed_text,sizeof(speed_text),"Pointer speed: %d.%02dx",slider->value/100,slider->value%100);
    os64_ui_mark_dirty(&ui,&speed_label);
    message("Changed. Choose Apply or Save.");
}
static void primary_changed(os64_ui_checkbox_t *box,void *ctx)
{ (void)box; (void)ctx; message("Changed. Choose Apply or Save."); }
static void remember_draft(void)
{
    if(selected<snapshot.count) {
        snapshot.devices[selected].setting.speed=speed.value;
        snapshot.devices[selected].setting.right_primary=primary.checked;
    }
}
static void show_device(void)
{
    bool have=selected<snapshot.count;
    if(have) {
        const os64_mouse_device_t *d=&snapshot.devices[selected];
        os64_strcopy(device_text,sizeof(device_text),d->name);
        os64_snprintf(connection_text,sizeof(connection_text),"Device %u of %u - %s",selected+1,snapshot.count,
            d->connected?"Connected":"Disconnected");
        os64_ui_slider_set(&ui,&speed,d->setting.speed);
        os64_ui_checkbox_set(&ui,&primary,d->setting.right_primary!=0);
    } else {
        os64_strcopy(device_text,sizeof(device_text),"No mice detected");
        os64_strcopy(connection_text,sizeof(connection_text),"Connect or wake a mouse, then choose Refresh.");
    }
    os64_snprintf(speed_text,sizeof(speed_text),"Pointer speed: %d.%02dx",speed.value/100,speed.value%100);
    os64_ui_set_enabled(&ui,&speed.w,have); os64_ui_set_enabled(&ui,&primary.w,have);
    os64_ui_set_enabled(&ui,&defaults,have); os64_ui_set_enabled(&ui,&apply,have); os64_ui_set_enabled(&ui,&save,have);
    os64_ui_set_enabled(&ui,&forget,have && !snapshot.devices[selected].connected);
    os64_ui_set_enabled(&ui,&previous,have && selected>0);
    os64_ui_set_enabled(&ui,&next,have && selected+1<snapshot.count);
    os64_ui_mark_dirty(&ui,&root);
}
static void refresh_devices(os64_ui_widget_t *w,void *ctx)
{
    (void)w; (void)ctx;
    os64_mouse_snapshot_t fresh;
    if(os64_mouse_read(&fresh)) { message("Cannot read mouse settings. Check the kernel version."); return; }
    char key[OS64_MOUSE_KEY]={0};
    if(selected<snapshot.count) os64_strcopy(key,sizeof(key),snapshot.devices[selected].setting.key);
    snapshot=fresh; selected=0;
    for(unsigned i=0;i<snapshot.count;i++) if(!os64_strcmp(key,snapshot.devices[i].setting.key)) selected=i;
    show_device(); message("Current settings loaded.");
}
static void select_device(os64_ui_widget_t *w,void *ctx)
{
    (void)ctx; remember_draft();
    if(w==&previous && selected) selected--;
    if(w==&next && selected+1<snapshot.count) selected++;
    show_device(); message(selected<snapshot.count && !snapshot.devices[selected].connected?
        "Forget removes this mouse's saved preferences.":"Preferences apply to the selected mouse.");
}
static void reset_defaults(os64_ui_widget_t *w,void *ctx)
{
    (void)w; (void)ctx;
    os64_ui_slider_set(&ui,&speed,100); os64_ui_checkbox_set(&ui,&primary,false);
    speed_changed(&speed,NULL);
}
static void publish(os64_ui_widget_t *w,void *ctx)
{
    (void)ctx;
    if(selected>=snapshot.count) return;
    remember_draft();
    os64_mouse_command_t command={.version=OS64_MOUSE_VERSION,.count=1,
        .expected_generation=snapshot.generation};
    command.settings[0]=snapshot.devices[selected].setting;
    if(os64_mouse_apply(&command)) { message("Apply failed. Refresh to load the current settings."); return; }
    snapshot.generation++;
    if(w==&save) message(os64_mouse_save(&command.settings[0])<0?
        "Applied; saving failed. Preferences remain active.":"Applied and saved for restart.");
    else message("Applied for this session.");
}
static void forget_device(os64_ui_widget_t *w,void *ctx)
{
    (void)w; (void)ctx;
    if(selected>=snapshot.count || snapshot.devices[selected].connected) return;
    int result=os64_mouse_forget(&snapshot.devices[selected].setting,snapshot.generation);
    if(result<0) { message("Forget failed. Refresh and try again."); return; }
    refresh_devices(NULL,NULL);
    message(result==OS64_MOUSE_FORGET_SAVED_ONLY?
        "Saved preferences removed; refresh to check live settings.":
        "Forgotten. Move the new mouse, then choose Refresh.");
}
static void widgets_init(void)
{
    os64_ui_panel(&root); os64_ui_set_root(&ui,&root);
    os64_ui_label(&title,"Mouse Settings"); os64_ui_label(&device,device_text);
    os64_ui_label(&connection,connection_text); os64_ui_label(&speed_label,speed_text);
    os64_ui_label(&hint,hint_text); os64_ui_label(&status,status_text);
    os64_ui_button(&previous,"Previous",select_device,NULL); os64_ui_button(&next,"Next",select_device,NULL);
    os64_ui_button(&refresh,"Refresh",refresh_devices,NULL); os64_ui_button(&defaults,"Defaults",reset_defaults,NULL);
    os64_ui_button(&apply,"Apply",publish,NULL); os64_ui_button(&save,"Save",publish,NULL);
    os64_ui_button(&forget,"Forget",forget_device,NULL);
    os64_ui_slider(&speed,OS64_MOUSE_MIN_SPEED,OS64_MOUSE_MAX_SPEED,25,100,speed_changed,NULL);
    os64_ui_checkbox(&primary,"Use right button as primary",false,primary_changed,NULL);
    os64_ui_widget_t *widgets[]={&title,&device,&connection,&previous,&next,&refresh,&speed_label,&speed.w,
        &primary.w,&defaults,&apply,&save,&forget,&hint,&status};
    for(unsigned i=0;i<sizeof(widgets)/sizeof(widgets[0]);i++) os64_ui_add_child(&root,widgets[i]);
}
int main(int argc,char **argv)
{
    (void)argc; (void)argv;
    int64_t win=os64_gui_window_create("Mouse Settings",80,70,600,440,0);
    if(win<=0) return 1;
    if(os64_draw_ctx_init(&draw,win)) { os64_gui_window_destroy(win); return 1; }
    os64_ui_init(&ui,&draw); os64_ui_theme_defaults(&ui.theme);
    os64_ui_theme_palette(&ui.theme,OS64_UI_PALETTE_MIDNIGHT);
    os64_ui_theme_current(&ui.theme,&ui.appearance_generation); ui.on_resize=resized;
    widgets_init();
    if(measure(&ui,&layout_state) || os64_gui_window_set_min_size(win,layout_state.min_width,layout_state.min_height) ||
       os64_draw_ctx_refresh(&draw)) { os64_ui_font_release(&ui); os64_gui_window_destroy(win); return 1; }
    resized(&ui); show_device(); refresh_devices(NULL,NULL);
    (void)os64_ui_font_planner(&ui,plan_font,commit_font,discard_font,NULL);
    os64_ui_run(&ui,win,NULL);
    os64_ui_font_release(&ui); os64_gui_window_destroy(win); return 0;
}
