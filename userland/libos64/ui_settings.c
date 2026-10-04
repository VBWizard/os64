#include "os64/ui_settings.h"
#include "os64/str.h"
#include "os64/mem.h"

static const os64_ui_class_t settings_body={.name="settings_body"};

static void place(os64_ui_widget_t *w,os64_gui_rect_t r,bool staged)
{
    if(staged)os64_ui_widget_stage_bounds(w,r);else w->bounds=r;
}
static bool arrange(os64_ui_settings_t *d,bool staged)
{
    os64_ui_t *ui=&d->ui;
    int32_t row=os64_ui_control_min_height(ui),pad=ui->theme.pad+8;
    int32_t w=(int32_t)d->ctx.surf.width,h=(int32_t)d->ctx.surf.height;
    int32_t bw=(w-4*pad)/3;
    if(row*(int32_t)(d->body_rows+2)+4*pad>h || bw<1)return false;
    for(unsigned i=0;i<3;++i){
        int32_t width;
        const char *text=d->actions[i].text;
        if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,text,os64_strlen(text),&width) ||
            width+2*ui->theme.pad>bw)return false;
    }
    place(&d->root,(os64_gui_rect_t){0,0,w,h},staged);
    os64_gui_rect_t body={pad,pad,w-2*pad,(int32_t)d->body_rows*row};
    if(d->arrange && !d->arrange(d,body,row,staged))return false;
    place(&d->body,body,staged);
    place(&d->status,(os64_gui_rect_t){pad,h-2*row-2*pad,w-2*pad,row},staged);
    for(unsigned i=0;i<3;++i)
        place(&d->actions[i],(os64_gui_rect_t){pad+(int32_t)i*(bw+pad),h-row-pad,bw,row},staged);
    return true;
}
static os64_font_status_t plan(os64_ui_t *ui,void *user,void **out)
{
    (void)ui;*out=NULL;
    return arrange(user,true)?OS64_FONT_OK:OS64_FONT_LIMIT;
}
static void planned(os64_ui_t *ui,void *user,void *p)
{(void)ui;(void)user;(void)p;}
static void resize(os64_ui_t *ui)
{(void)arrange((os64_ui_settings_t *)ui,false);}
static void clicked(os64_ui_widget_t *w,void *user)
{
    os64_ui_settings_t *d=user;
    if(w==&d->actions[2])d->ui.quit=true;
    else if(d->apply)d->apply(d,w==&d->actions[1]);
}
bool os64_ui_settings_open(os64_ui_settings_t *d,int64_t parent,const char *title,
    unsigned rows,bool (*layout)(os64_ui_settings_t *,os64_gui_rect_t,int32_t,bool),
    void (*apply)(os64_ui_settings_t *,bool),void *user)
{
    if(d->window>0){(void)os64_gui_window_focus(d->window);return false;}
    os64_memset(d,0,sizeof(*d));
    os64_gui_window_state_t state;
    int32_t x=180,y=160;
    if(!os64_gui_window_get_state(parent,&state)){x=state.x+40;y=state.y+50;}
    // Tall enough for the body's rows, the status and the actions at a row
    // of 30 px, room left for a larger font; never shorter than 420.
    int32_t height=(int32_t)(rows+2)*30+90;
    if(height<420)height=420;
    d->window=os64_gui_window_create_content(title,x,y,760,height,OS64_GUI_CREATE_FIT_SCREEN);
    if(d->window<=0)return false;
    if(os64_draw_ctx_init(&d->ctx,d->window)){os64_ui_settings_close(d);return false;}
    d->body_rows=rows;d->arrange=layout;d->apply=apply;d->user=user;
    os64_ui_init(&d->ui,&d->ctx);
    os64_ui_panel(&d->root);d->body=(os64_ui_widget_t){.cls=&settings_body};
    os64_ui_set_root(&d->ui,&d->root);os64_ui_add_child(&d->root,&d->body);
    os64_ui_label(&d->status,d->message);os64_ui_add_child(&d->root,&d->status);
    const char *captions[]={"Apply","Save as default","Close"};
    for(unsigned i=0;i<3;++i){
        os64_ui_button(&d->actions[i],captions[i],clicked,d);
        os64_ui_add_child(&d->root,&d->actions[i]);
    }
    d->ui.on_resize=resize;
    (void)os64_ui_font_planner(&d->ui,plan,planned,planned,d);
    return true;
}
void os64_ui_settings_ready(os64_ui_settings_t *d)
{
    if(d->window<=0)return;
    (void)arrange(d,false);
    (void)os64_ui_font_follow(&d->ui);
    // Pin the initial usable content envelope; fonts that cannot fit a live
    // window are refused by the planner without disturbing the current layout.
    (void)os64_gui_window_set_min_size(d->window,d->ctx.surf.width,d->ctx.surf.height);
    os64_ui_mark_dirty(&d->ui,&d->root);
}
void os64_ui_settings_report(os64_ui_settings_t *d,const char *message)
{
    os64_strcopy(d->message,sizeof(d->message),message);
    os64_ui_mark_dirty(&d->ui,&d->status);
}
void os64_ui_settings_close(os64_ui_settings_t *d)
{
    if(d->window>0){
        (void)os64_ui_font_release(&d->ui);
        (void)os64_gui_window_destroy(d->window);
    }
    d->window=0;
}
void os64_ui_settings_pump(os64_ui_settings_t *d)
{
    if(d->window<=0)return;
    os64_gui_event_t ev;int64_t rc;
    while((rc=os64_gui_event_poll(d->window,&ev))==1){
        if(ev.type==OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii==27 &&
            (ev.key.scancode==0x01 || ev.key.scancode==0x29))d->ui.quit=true;
        else os64_ui_dispatch(&d->ui,&ev);
        if(d->ui.quit)break;
    }
    if(rc<0 || d->ui.quit){os64_ui_settings_close(d);return;}
    os64_ui_paint(&d->ui);
}
