#include "os64/ui_dispatcher.h"
#include "os64/str.h"
static int64_t native_poll(void *u, int64_t h, os64_gui_event_t *e)
{ (void)u; return os64_gui_event_poll(h,e); }
static int64_t native_wait(void *u, int64_t *h)
{ (void)u; return os64_gui_event_wait_any(h); }
static int64_t native_destroy(void *u, int64_t h)
{ (void)u; return os64_gui_window_destroy(h); }
static int64_t native_create(void *u, int64_t h, os64_gui_rect_t r)
{ (void)u; return os64_gui_popup_create("Menu",h,r.x-1,r.y-1,r.w,r.h); }
static int64_t native_rect(void *u, int64_t h, os64_gui_rect_t *r)
{ (void)u; return os64_gui_window_get_content_rect(h,r); }
static int64_t native_surface(void *u, int64_t h, os64_gui_surface_t *s)
{ (void)u; return os64_gui_window_get_surface(h,s); }
static int64_t native_screen(void *u, uint32_t *w, uint32_t *h)
{ (void)u; return os64_gui_screen_info(w,h); }
static void native_publish(void *u, int64_t h, os64_gui_rect_t r)
{ (void)u; (void)os64_gui_window_publish(h,&r); }
void os64_ui_dispatcher_init(os64_ui_dispatcher_t *d, const os64_ui_window_backend_t *b)
{
    *d=(os64_ui_dispatcher_t){0};
    d->native=b?*b:(os64_ui_window_backend_t){NULL,native_poll,native_wait,
        native_destroy,native_create,native_rect,native_surface,native_screen,native_publish};
}
bool os64_ui_dispatcher_can_create(const os64_ui_dispatcher_t *d) { return d&&!d->dead_count; }
bool os64_ui_dispatcher_live(const os64_ui_dispatcher_t *d,int64_t h)
{
    if(!d||h<=0) return false;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) if(d->entries[i].handle==h) return true;
    return false;
}
os64_ui_status_code_t os64_ui_dispatcher_add(os64_ui_dispatcher_t *d,int64_t h,
    os64_ui_t *ui,void (*event)(void *,const os64_gui_event_t *),void (*paint)(void *),void *u)
{
    if(!d||h<=0) return OS64_UI_INVALID;
    if(d->dead_count) return OS64_UI_BUSY;
    if(os64_ui_dispatcher_live(d,h)) return OS64_UI_DUPLICATE;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) if(!d->entries[i].handle&&!d->entries[i].boundary) {
        d->entries[i]=(os64_ui_dispatch_entry_t){.handle=h,.ui=ui,.user=u,.event=event,.paint=paint};
        return OS64_UI_OK;
    }
    return OS64_UI_LIMIT;
}
int64_t os64_ui_dispatcher_destroy(os64_ui_dispatcher_t *d,int64_t h)
{
    if(!d||!os64_ui_dispatcher_live(d,h)) return OS64_GUI_ERR_INVALID_HANDLE;
    int64_t rc=d->native.destroy(d->native.user,h);
    if(rc<0) return rc;
    d->tombstones[d->dead_count++]=h;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) {
        if(d->entries[i].handle==h) d->entries[i]=(os64_ui_dispatch_entry_t){0};
        d->entries[i].drained=false;
    }
    return rc;
}
int64_t os64_ui_dispatcher_turn(os64_ui_dispatcher_t *d,bool wait)
{
    if(!d||d->dispatching) return OS64_GUI_ERR_BUSY;
    if(wait&&!d->dead_count) {
        int64_t h=0,rc=d->native.wait(d->native.user,&h);
        if(rc!=1) return rc;
        if(!os64_ui_dispatcher_live(d,h)) return OS64_GUI_ERR_INVALID_HANDLE;
    }
    d->dispatching=true;
    size_t events=0,idle=0;
    bool empty=false;
    while(events<OS64_UI_DISPATCH_BUDGET && idle<OS64_UI_DISPATCH_WINDOWS) {
        size_t i=d->cursor++%OS64_UI_DISPATCH_WINDOWS;
        os64_ui_dispatch_entry_t entry=d->entries[i];
        if(!entry.handle) { ++idle; continue; }
        os64_gui_event_t e;
        int64_t rc=d->native.poll(d->native.user,entry.handle,&e);
        if(rc<0) { d->dispatching=false; return rc; }
        if(rc==0) { d->entries[i].drained=true; ++idle; continue; }
        idle=0; ++events;
        if(d->observe) d->observe(d->observe_user,entry.handle,&e);
        bool consumed=entry.ui && e.type==OS64_GUI_EVENT_WINDOW_PLACEMENT &&
            entry.ui->placement_event && entry.ui->placement_event(entry.ui,&e);
        if(!consumed) consumed=entry.ui&&entry.ui->menu_event&&entry.ui->menu_event(entry.ui,&e);
        if(!consumed) {
            if(entry.event) entry.event(entry.user,&e);
            else if(entry.ui) (void)os64_ui_dispatch(entry.ui,&e);
        }
    }
    empty=idle==OS64_UI_DISPATCH_WINDOWS;
    bool drained=true;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i)
        if(d->entries[i].handle&&!d->entries[i].drained) drained=false;
    if(drained) { d->dead_count=0; os64_memset(d->tombstones,0,sizeof(d->tombstones)); }
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) {
        os64_ui_dispatch_entry_t entry=d->entries[i];
        if(entry.boundary) entry.boundary(entry.boundary_user,empty);
    }
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) {
        os64_ui_dispatch_entry_t entry=d->entries[i];
        if(entry.handle&&entry.paint) entry.paint(entry.user);
        else if(entry.handle&&entry.ui) os64_ui_paint(entry.ui);
    }
    d->dispatching=false;
    return (int64_t)events;
}
