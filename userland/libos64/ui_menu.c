#include "ui_menu_internal.h"
#include "ui_internal.h"
#include "os64/mem.h"
#include "os64/str.h"

static void panel_event(void *,const os64_gui_event_t *);
static void panel_paint(void *);
static void boundary(void *,bool);
static bool owner_event(os64_ui_t *,const os64_gui_event_t *);
static bool panel_select(os64_ui_menu_session_t *,size_t,size_t);
static int64_t menu_turn(os64_ui_t *ui)
{ os64_ui_menu_session_t *s=ui->menu; return os64_ui_dispatcher_turn(s->dispatcher,true); }
static bool before_font(os64_ui_t *ui) { return os64_ui_menu_close(ui->menu,OS64_UI_MENU_FONT)==OS64_UI_OK; }
static char lower(char c) { return c>='A'&&c<='Z'?(char)(c+32):c; }
static bool is_key(const os64_gui_event_t *e,uint16_t key,uint8_t mods)
{
    os64_gui_event_t down=*e; down.type=OS64_GUI_EVENT_KEY_DOWN;
    return os64_ui_shortcut_match((os64_ui_shortcut_t){key,mods},&down);
}
static unsigned identity(const os64_gui_event_t *e)
{ return e->key.scancode+((e->key.modifiers&OS64_GUI_MOD_HID)?256u:0); }
static bool left_alt(const os64_gui_event_t *e)
{ return e->key.scancode==((e->key.modifiers&OS64_GUI_MOD_HID)?0xe2:0x38); }
static void cancel_press(os64_ui_menu_session_t *s)
{ s->pressed_key=0; s->pressed_row=MENU_NONE; s->pressed_panel=MENU_NONE; }
static void reset_input(os64_ui_menu_session_t *s)
{
    cancel_press(s);
    os64_memset(s->held,0,sizeof(s->held));
    s->f10_held=false; s->alt_armed=false; s->decoder=0;
    s->suppress_pending=false; s->suppress_button=0;
}
static void publish_bar(os64_ui_menu_session_t *s,bool target_changed)
{
    if(!s->bar) return;
    os64_ui_menu_model_t *m=s->bar->model;
    uint64_t enabled=0;
    for(size_t i=0;m&&i<m->count;++i)
        if(menu_model_enabled(m->entries[i].child,s->bar_view)) enabled|=UINT64_C(1)<<i;
    /* Applications may refresh the same view before passing it back to us.
     * Compare published state so their paint callback does not dirty an idle bar. */
    if(target_changed||!s->bar_snapshot||s->bar_snapshot_model!=m||s->bar_enabled!=enabled)
        os64_ui_mark_dirty(s->ui,&s->bar->w);
    s->bar_snapshot_model=m; s->bar_enabled=enabled; s->bar_snapshot=true;
}
static void clear_help(os64_ui_menu_session_t *s)
{
    if(s->status&&s->help_token) (void)os64_ui_status_clear_help(s->status,s->help_token);
    s->help_token=0;
}
static void help(os64_ui_menu_session_t *s,os64_ui_popup_t *p)
{
    clear_help(s);
    if(!s->status||p->selected==MENU_NONE||p->list) return;
    menu_row *r=&p->rows[p->selected];
    if(r->spec.help&&*r->spec.help) (void)os64_ui_status_help(s->status,r->spec.help,&s->help_token);
}
static bool family(os64_ui_menu_session_t *s,int64_t h)
{
    if(s->owner&&h==s->owner) return true;
    for(size_t i=0;i<s->depth;++i) if(h==s->panels[i].handle) return true;
    return false;
}
static void release_pending(os64_ui_menu_session_t *s)
{ os64_ui_menu_model_release(s->pending); s->pending=NULL; }
static os64_ui_status_code_t close_to(os64_ui_menu_session_t *s,size_t depth)
{
    os64_ui_status_code_t rc=OS64_UI_OK;
    if(s->pending&&s->pending_depth>=depth) release_pending(s);
    if(s->pressed_panel>=depth) cancel_press(s);
    while(s->depth>depth) {
        menu_panel *p=&s->panels[s->depth-1];
        /* Native children must disappear while their parent registration and
         * font runs are alive. Successful removal quarantines the handle. */
        if(os64_ui_dispatcher_destroy(s->dispatcher,p->handle)<0) { rc=OS64_UI_ERROR; break; }
        --s->depth; os64_ui_popup_destroy(p->widget); *p=(menu_panel){0};
    }
    clear_help(s);
    if(s->depth) help(s,s->panels[s->depth-1].widget);
    return rc;
}
os64_ui_status_code_t os64_ui_menu_close(os64_ui_menu_session_t *s,os64_ui_menu_close_t reason)
{
    if(!s) return OS64_UI_OK;
    if(reason==OS64_UI_MENU_LOST) reset_input(s);
    bool restore_focus=s->active;
    cancel_press(s);
    release_pending(s);
    if(reason!=OS64_UI_MENU_DISMISSED) { s->suppress_pending=false; s->suppress_button=0; }
    s->decoder=0; s->alt_armed=false;
    s->failed_panel=MENU_NONE; s->failed_row=MENU_NONE;
    /* A failed native destroy leaves its registration borrowing this session,
     * popup and font context. Keep them alive so the caller can retry. */
    os64_ui_status_code_t rc=close_to(s,0);
    if(rc) return rc;
    s->active=false; s->dismissed=reason==OS64_UI_MENU_DISMISSED;
    if(s->view) { os64_ui_action_view_release(s->view); s->view=NULL; }
    os64_free(s->list); s->list=NULL; s->list_pending=false; s->close_retry=false;
    if(s->bar&&s->bar->temporary) {
        s->bar->temporary=false; os64_ui_set_hidden(s->ui,&s->bar->w,true);
        if(s->layout) s->layout(s->user);
    }
    if(s->bar) os64_ui_mark_dirty(s->ui,&s->bar->w);
    /* set_focus verifies membership and eligibility without a native focus
     * syscall. Generic libui keeps the logical editor across family blur. */
    os64_ui_widget_t *restore=s->saved_focus,*root=restore;
    while(root&&root->parent) root=root->parent;
    if(!restore||root!=s->ui->root||!restore->focusable||!os64_ui_widget_enabled(restore)) restore=s->fallback;
    if(restore_focus&&restore) os64_ui_set_focus(s->ui,restore);
    s->saved_focus=NULL;
    return OS64_UI_OK;
}
void os64_ui_menu_focus_fallback(os64_ui_menu_session_t *s,os64_ui_widget_t *w) { if(s) s->fallback=w; }
bool os64_ui_menu_active(const os64_ui_menu_session_t *s) { return s&&s->active; }
os64_ui_status_code_t os64_ui_menu_place(os64_gui_rect_t a,int32_t w,int32_t h,
    uint32_t sw,uint32_t sh,bool cascade,os64_gui_rect_t *out)
{
    if(!out||sw>INT32_MAX||sh>INT32_MAX||w<OS64_GUI_MIN_CONTENT||h<OS64_GUI_MIN_CONTENT||
        (int64_t)w+2>sw||(int64_t)h+2>sh||a.w<0||a.h<0) return OS64_UI_LIMIT;
    int64_t x=cascade?(int64_t)a.x+a.w:a.x,y=cascade?a.y:(int64_t)a.y+a.h;
    if(x+w+1>sw) x=cascade?(int64_t)a.x-w:(int64_t)a.x+a.w-w;
    if(y+h+1>sh) y=cascade?(int64_t)sh-h-1:(int64_t)a.y-h;
    if(x<1) x=1;
    if(y<1) y=1;
    if(x+w+1>sw) x=(int64_t)sw-w-1;
    if(y+h+1>sh) y=(int64_t)sh-h-1;
    *out=(os64_gui_rect_t){(int32_t)x,(int32_t)y,w,h}; return OS64_UI_OK;
}
/* A NULL model opens the session's list, which is always level 0. */
static os64_ui_status_code_t open_panel(os64_ui_menu_session_t *s,os64_ui_menu_model_t *model,size_t level)
{
    os64_ui_dispatcher_t *d=s->dispatcher;
    if(level>=OS64_UI_MENU_DEPTH) return OS64_UI_LIMIT;
    if(level>s->depth||(level&&(!s->panels[level-1].widget||!s->panels[level-1].handle))) return OS64_UI_INVALID;
    if(!os64_ui_dispatcher_can_create(d)) return OS64_UI_BUSY;
    uint32_t sw=0,sh=0;
    os64_gui_rect_t parent={0},a=s->anchor;
    int64_t owner=level?s->panels[level-1].handle:s->owner;
    if(d->native.screen(d->native.user,&sw,&sh)<0||sw>INT32_MAX||sh>INT32_MAX||sw<10||sh<10||
       (owner&&d->native.rect(d->native.user,owner,&parent)<0)) return OS64_UI_ERROR;
    if(level) {
        os64_ui_popup_t *p=s->panels[level-1].widget;
        a=os64_ui_popup_row(p,p->selected);
        if(!a.h) return OS64_UI_REFUSED;
    }
    int64_t x=(int64_t)parent.x+a.x,y=(int64_t)parent.y+a.y;
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX) return OS64_UI_LIMIT;
    a.x=(int32_t)x; a.y=(int32_t)y;
    os64_ui_popup_t *widget=NULL;
    os64_ui_status_code_t rc=model
        ? os64_ui_popup_create(s->ui,model,s->view,s->provider,(int32_t)sw-2,(int32_t)sh-2,&widget)
        : menu_popup_create_list(s->ui,s->list,(int32_t)sw-2,(int32_t)sh-2,&widget);
    if(rc) return rc;
    os64_gui_rect_t placed;
    rc=os64_ui_menu_place(a,widget->w.bounds.w,widget->w.bounds.h,sw,sh,level!=0,&placed);
    if(rc) { os64_ui_popup_destroy(widget); return rc; }
    bool slot=false;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) if(!d->entries[i].handle&&!d->entries[i].boundary) slot=true;
    if(!slot) { os64_ui_popup_destroy(widget); return OS64_UI_LIMIT; }
    int64_t handle=d->native.create(d->native.user,owner,placed);
    if(handle<=0) { os64_ui_popup_destroy(widget); return handle==OS64_GUI_ERR_NO_RESOURCES?OS64_UI_LIMIT:OS64_UI_ERROR; }
    menu_panel *p=&s->panels[level];
    *p=(menu_panel){.session=s,.widget=widget,.handle=handle,.index=level,.draw={.win=handle}};
    rc=os64_ui_dispatcher_add(d,handle,NULL,panel_event,panel_paint,p);
    if(rc) { /* Registration space is checked by the shared 32-window bound. */
        (void)d->native.destroy(d->native.user,handle);
        d->tombstones[d->dead_count++]=handle;
        for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) d->entries[i].drained=false;
        os64_ui_popup_destroy(widget); *p=(menu_panel){0}; return rc;
    }
    ++s->depth;
    if(d->native.surface(d->native.user,handle,&p->draw.surf)<0||
        p->draw.surf.width!=(uint32_t)widget->w.bounds.w||p->draw.surf.height!=(uint32_t)widget->w.bounds.h) {
        close_to(s,level); return OS64_UI_ERROR;
    }
    /* A list opens on its value, a menu (and a list whose value is not a
     * row it may choose) on the first row it may. */
    if(!model&&menu_selectable(widget,s->list->value)) os64_ui_popup_select(widget,s->list->value);
    else for(size_t i=0;i<menu_count(widget);++i) if(menu_selectable(widget,i)) { os64_ui_popup_select(widget,i); break; }
    help(s,widget); panel_paint(p); return OS64_UI_OK;
}
static void report(os64_ui_menu_session_t *s,os64_ui_status_code_t rc)
{ if(s->error&&rc!=OS64_UI_OK&&rc!=OS64_UI_BUSY&&rc!=OS64_UI_EMPTY) s->error(s->user,rc); }
static os64_ui_status_code_t close_report(os64_ui_menu_session_t *s,os64_ui_menu_close_t reason)
{
    os64_ui_status_code_t rc=os64_ui_menu_close(s,reason);
    report(s,rc); return rc;
}
static os64_ui_status_code_t close_after_failure(os64_ui_menu_session_t *s,os64_ui_status_code_t failure)
{
    /* Report after cleanup: a surviving registration makes teardown failure
     * the result the application must handle before releasing borrowed state. */
    os64_ui_status_code_t closed=os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
    return closed?closed:failure;
}
static os64_ui_status_code_t request_panel(os64_ui_menu_session_t *s,os64_ui_menu_model_t *m,size_t level)
{
    release_pending(s);
    os64_ui_status_code_t rc=close_to(s,level);
    if(rc) return rc;
    rc=open_panel(s,m,level);
    if(rc==OS64_UI_BUSY) {
        rc=os64_ui_menu_model_retain(m);
        if(!rc) { s->pending=m; s->pending_depth=level; rc=OS64_UI_BUSY; }
    }
    if(level&&rc!=OS64_UI_OK&&rc!=OS64_UI_BUSY) { s->failed_panel=level-1; s->failed_row=s->panels[level-1].widget->selected; }
    return rc;
}
os64_ui_status_code_t os64_ui_menu_open(os64_ui_menu_session_t *s,os64_ui_menu_model_t *m,
    os64_ui_action_view_t *v,os64_gui_rect_t anchor,os64_ui_menu_invocation_t kind)
{
    if(!s||!m||!v||!os64_ui_action_view_registry(v,m->actions)||anchor.w<0||anchor.h<0) return OS64_UI_INVALID;
    os64_ui_status_code_t rc=os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
    if(rc) return rc;
    if(!m->count) return OS64_UI_EMPTY;
    rc=os64_ui_action_view_retain(v);
    if(rc) return rc;
    rc=os64_ui_actions_refresh(v);
    if(rc) { os64_ui_action_view_release(v); return rc; }
    s->view=v; s->anchor=anchor; s->kind=kind; s->active=true;
    publish_bar(s,false);
    if(s->bar) os64_ui_mark_dirty(s->ui,&s->bar->w);
    s->saved_focus=s->ui->focus; os64_ui_cancel_gestures(s->ui);
    rc=request_panel(s,m,0);
    if(rc!=OS64_UI_OK&&rc!=OS64_UI_BUSY) { rc=close_after_failure(s,rc); report(s,rc); }
    return rc;
}
/* The session's copy of a list, in one block: the label pointers, the
 * flags, then the labels. A label is kept to 127 bytes, cut at a character
 * boundary, which is what a row lays out (menu_row.label). */
static os64_ui_status_code_t list_copy(const os64_ui_menu_list_t *l,menu_list **out)
{
    size_t text=0;
    for(size_t i=0;i<l->count;++i) {
        size_t n=l->labels[i]?os64_strlen(l->labels[i]):0;
        text+=(n>127?127:n)+1;
    }
    if(l->count>(SIZE_MAX-sizeof(menu_list)-text)/(sizeof(char *)+sizeof(bool))) return OS64_UI_LIMIT;
    menu_list *c=os64_malloc(sizeof(*c)+l->count*(sizeof(char *)+sizeof(bool))+text);
    if(!c) return OS64_UI_NO_MEMORY;
    *c=(menu_list){.count=l->count,.value=l->selected<l->count?l->selected:MENU_NONE,.min_width=l->min_width,
        .pick=l->pick,.user=l->user,.widget=l->widget,.widget_bounds=l->widget?l->widget->bounds:(os64_gui_rect_t){0}};
    c->labels=(const char **)(c+1);
    c->enabled=(bool *)(c->labels+l->count);
    char *at=(char *)(c->enabled+l->count);
    for(size_t i=0;i<l->count;++i) {
        const char *s=l->labels[i]?l->labels[i]:"";
        size_t n=os64_strlen(s);
        if(n>127) { n=127; while(n&&((unsigned char)s[n]&0xC0)==0x80) --n; }
        os64_memcpy(at,s,n); at[n]=0;
        c->labels[i]=at; at+=n+1;
        c->enabled[i]=l->enabled[i];
    }
    *out=c; return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_menu_open_list(os64_ui_menu_session_t *s,const os64_ui_menu_list_t *l,
    os64_gui_rect_t anchor)
{
    if(!s||!l||!l->labels||!l->enabled||anchor.w<0||anchor.h<0) return OS64_UI_INVALID;
    if(!l->count) return OS64_UI_EMPTY;
    menu_list *copy=NULL;
    os64_ui_status_code_t rc=list_copy(l,&copy);
    if(rc) return rc;
    rc=os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
    if(rc) { os64_free(copy); return rc; }
    s->list=copy; s->anchor=anchor; s->kind=OS64_UI_MENU_CONTEXT; s->active=true;
    if(s->bar) os64_ui_mark_dirty(s->ui,&s->bar->w);
    s->saved_focus=s->ui->focus; os64_ui_cancel_gestures(s->ui);
    rc=open_panel(s,NULL,0);
    if(rc==OS64_UI_BUSY) s->list_pending=true;
    else if(rc) { rc=close_after_failure(s,rc); report(s,rc); }
    return rc;
}
void os64_ui_menu_list_detach(os64_ui_menu_session_t *s,void *user)
{
    if(!s||!s->list||s->list->user!=user) return;
    /* The detaching caller may be freeing the list's widget: the close
     * must not give focus back to it, now or on a retry. */
    if(s->list->widget&&s->saved_focus==s->list->widget) s->saved_focus=NULL;
    s->list->pick=NULL; s->list->user=NULL; s->list->widget=NULL;
    /* With no native window yet (pending) the close cannot be refused. */
    if(os64_ui_menu_close(s,OS64_UI_MENU_GEOMETRY)!=OS64_UI_OK) s->close_retry=true;
}
os64_ui_status_code_t os64_ui_menu_list_relabel(os64_ui_menu_session_t *s,void *user,
    const char *const *labels,const bool *enabled,size_t count)
{
    if(!s||!user||!labels||!enabled) return OS64_UI_INVALID;
    menu_list *old=s->list;
    if(!s->active||!old||old->user!=user) return OS64_UI_STALE;
    if(count!=old->count) return OS64_UI_INVALID;
    os64_ui_menu_list_t l={.labels=labels,.enabled=enabled,.count=count,.pick=old->pick,.user=old->user};
    menu_list *copy=NULL;
    os64_ui_status_code_t rc=list_copy(&l,&copy);
    if(rc) return rc;
    copy->value=old->value; copy->min_width=old->min_width;
    copy->widget=old->widget; copy->widget_bounds=old->widget_bounds;
    s->list=copy;
    /* A list is always level 0; a pending one has no popup yet. */
    if(s->depth&&s->panels[0].widget&&s->panels[0].widget->list==old) menu_popup_relist(s->panels[0].widget,copy);
    os64_free(old);
    return OS64_UI_OK;
}
static bool same_rect(os64_gui_rect_t a,os64_gui_rect_t b)
{ return a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h; }
static void boundary(void *user,bool empty)
{
    os64_ui_menu_session_t *s=user;
    /* A focus-only transition does not authorize swallowing a future click.
     * Budget exhaustion retains the intent until the existing queues drain. */
    if(empty) s->suppress_pending=false;
    if(s->pending&&s->active&&os64_ui_dispatcher_can_create(s->dispatcher)) {
        os64_ui_menu_model_t *m=s->pending; s->pending=NULL;
        os64_ui_status_code_t rc=open_panel(s,m,s->pending_depth);
        os64_ui_menu_model_release(m);
        if(rc) report(s,close_after_failure(s,rc));
    }
    if(s->list_pending&&s->active&&os64_ui_dispatcher_can_create(s->dispatcher)) {
        s->list_pending=false;
        os64_ui_status_code_t rc=open_panel(s,NULL,0);
        if(rc==OS64_UI_BUSY) s->list_pending=true;
        else if(rc) report(s,close_after_failure(s,rc));
    }
    /* A detached list whose close was refused is asked again, quietly: its
     * popup holds only the session's copy and calls no one meanwhile. A list
     * whose box was hidden or moved since it opened closes. */
    if(s->close_retry) {
        if(os64_ui_menu_close(s,OS64_UI_MENU_GEOMETRY)!=OS64_UI_OK) s->close_retry=true;
    } else if(s->active&&s->list&&s->list->widget&&
              (s->list->widget->hidden||!same_rect(s->list->widget->bounds,s->list->widget_bounds)))
        (void)close_report(s,OS64_UI_MENU_GEOMETRY);
}
os64_ui_status_code_t os64_ui_menu_session_create(os64_ui_dispatcher_t *d,os64_ui_t *ui,int64_t owner,
    os64_ui_menubar_t *b,os64_ui_status_t *status,const os64_ui_icon_provider_t *provider,os64_ui_menu_session_t **out)
{
    if(!out) return OS64_UI_INVALID;
    *out=NULL;
    if(!d||!ui||ui->menu||owner<0||(owner&&!os64_ui_dispatcher_live(d,owner))||
       (b&&(b->session||b->w.ui!=ui))||(!owner&&b)) return OS64_UI_INVALID;
    if(!owner) for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i)
        if(d->entries[i].handle&&d->entries[i].ui==ui) return OS64_UI_INVALID;
    size_t participant=OS64_UI_DISPATCH_WINDOWS;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i)
        if(owner?d->entries[i].handle==owner:(!d->entries[i].handle&&!d->entries[i].boundary)) { participant=i; break; }
    if(participant==OS64_UI_DISPATCH_WINDOWS) return OS64_UI_LIMIT;
    if(owner&&(d->entries[participant].ui!=ui||d->entries[participant].boundary)) return OS64_UI_INVALID;
    os64_ui_menu_session_t *s=os64_malloc(sizeof(*s));
    if(!s) return OS64_UI_NO_MEMORY;
    *s=(os64_ui_menu_session_t){.dispatcher=d,.ui=ui,.owner=owner,.bar=b,.status=status,.provider=provider};
    cancel_press(s);
    ui->menu=s; ui->menu_event=owner_event; ui->menu_before_font=before_font; ui->menu_turn=menu_turn;
    if(b) b->session=s;
    d->entries[participant].boundary=boundary; d->entries[participant].boundary_user=s;
    *out=s; return OS64_UI_OK;
}
void os64_ui_menu_hooks(os64_ui_menu_session_t *s,void (*layout)(void *),
    void (*error)(void *,os64_ui_status_code_t),void *user)
{ if(s) { s->layout=layout; s->error=error; s->user=user; } }
os64_ui_status_code_t os64_ui_menu_session_dispose(os64_ui_menu_session_t *s)
{
    if(!s) return OS64_UI_OK;
    os64_ui_status_code_t rc=os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
    if(rc) return rc;
    for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) if(s->dispatcher->entries[i].boundary_user==s) {
        s->dispatcher->entries[i].boundary=NULL; s->dispatcher->entries[i].boundary_user=NULL;
    }
    s->ui->menu=NULL; s->ui->menu_event=NULL; s->ui->menu_before_font=NULL; s->ui->menu_turn=NULL;
    if(s->bar) { s->bar->session=NULL; s->bar->view=NULL; }
    os64_ui_action_view_release(s->bar_view); os64_free(s);
    return OS64_UI_OK;
}
static bool normalize_bar_selection(os64_ui_menu_session_t *s)
{
    os64_ui_menubar_t *b=s->bar;
    size_t visible=menu_bar_visible(b),count=b&&b->model?
        visible+(visible<b->model->count?1:0):0;
    size_t selected=b?b->selected:MENU_NONE;
    if(!b||b->w.disabled) return false;
    if(selected>=count||!menu_bar_enabled(b,selected)) {
        selected=MENU_NONE;
        for(size_t i=0;i<count;++i) if(menu_bar_enabled(b,i)) { selected=i; break; }
    }
    if(b->selected!=selected) { b->selected=selected; os64_ui_mark_dirty(s->ui,&b->w); }
    return selected!=MENU_NONE;
}
static os64_ui_status_code_t sync_bar_mode(os64_ui_menu_session_t *s)
{
    /* Live panels retain their anchor until path teardown. An invalidated
     * pending root is cancelled before normalizing its displayed heading. */
    if(!s->active||s->kind!=OS64_UI_MENU_BAR||s->depth) return OS64_UI_OK;
    if(s->pending) {
        if(menu_model_enabled(s->pending,s->view)) return OS64_UI_OK;
        release_pending(s); cancel_press(s);
    }
    return normalize_bar_selection(s)?OS64_UI_OK:os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
}
static os64_ui_status_code_t reconcile_selection(os64_ui_menu_session_t *s)
{
    os64_ui_status_code_t rc=sync_bar_mode(s);
    if(rc) return rc;
    for(size_t i=0;i<s->depth;++i) {
        os64_ui_popup_t *p=s->panels[i].widget;
        if(!menu_selectable(p,p->selected)) {
            release_pending(s); cancel_press(s);
            /* Selection owns the native child's anchor. Use the same commit
             * point as pointer/keyboard changes when destruction is refused. */
            return panel_select(s,i,MENU_NONE)?OS64_UI_OK:OS64_UI_ERROR;
        }
    }
    if(s->depth) help(s,s->panels[s->depth-1].widget);
    return rc;
}
os64_ui_status_code_t os64_ui_menu_bar_view(os64_ui_menu_session_t *s,os64_ui_action_view_t *v)
{
    if(!s||!v||(s->bar&&s->bar->model&&!os64_ui_action_view_registry(v,s->bar->model->actions))) return OS64_UI_INVALID;
    if(s->bar_view==v) { publish_bar(s,false); return sync_bar_mode(s); }
    os64_ui_status_code_t rc=os64_ui_action_view_retain(v);
    if(!rc) { os64_ui_action_view_release(s->bar_view); s->bar_view=v; if(s->bar) s->bar->view=v; publish_bar(s,true); }
    return rc?rc:sync_bar_mode(s);
}
os64_ui_status_code_t os64_ui_menu_refresh(os64_ui_menu_session_t *s)
{
    if(!s||!s->view) return OS64_UI_INVALID;
    os64_ui_status_code_t rc=os64_ui_actions_refresh(s->view);
    publish_bar(s,false);
    if(rc) {
        return close_after_failure(s,rc);
    }
    return reconcile_selection(s);
}
static void invoke(os64_ui_menu_session_t *s,uint32_t id)
{
    os64_ui_action_view_t *v=s->view;
    if(!v) return;
    os64_ui_status_code_t rc=os64_ui_action_view_retain(v);
    if(rc) { report(s,rc); return; }
    if(close_report(s,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK) (void)os64_ui_action_invoke(v,id);
    /* The callback may invalidate the tree; only the independently retained
     * view is touched here. Application destruction waits until dispatch ends. */
    os64_ui_action_view_release(v);
}
static void child(os64_ui_menu_session_t *s,size_t level,bool explicit)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    if(p->list||p->selected==MENU_NONE||!menu_selectable(p,p->selected)) return;
    if(!explicit&&s->failed_panel==level&&s->failed_row==p->selected) return;
    s->failed_panel=MENU_NONE; s->failed_row=MENU_NONE;
    os64_ui_menu_entry_t *e=&p->model->entries[p->selected];
    if(e->child) {
        if(s->depth>level+1&&s->panels[level+1].widget->model==e->child) return;
        report(s,request_panel(s,e->child,level+1));
    }
}
static bool panel_select(os64_ui_menu_session_t *s,size_t level,size_t row)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    if(!menu_selectable(p,row)) row=MENU_NONE;
    if(row!=p->selected) {
        if(s->pending&&s->pending_depth>level) release_pending(s);
        cancel_press(s);
        os64_ui_status_code_t rc=close_to(s,level+1);
        if(rc) { report(s,rc); return false; }
        s->failed_panel=MENU_NONE; s->failed_row=MENU_NONE;
        os64_ui_popup_select(p,row);
    }
    help(s,p); return true;
}
/* A list's commit: the family closes first, as invoke closes before the
 * command runs, then the row goes to whoever opened the list, unless they
 * detached. A close the native destroy refuses commits nothing. */
static void commit(os64_ui_menu_session_t *s,size_t row)
{
    void (*pick)(void *,size_t)=s->list->pick;
    void *user=s->list->user;
    if(close_report(s,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK&&pick) pick(user,row);
}
static void activate(os64_ui_menu_session_t *s,size_t level,size_t row)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    if(!menu_selectable(p,row)) return;
    if(!panel_select(s,level,row)) return;
    if(p->list) { commit(s,row); return; }
    os64_ui_menu_entry_t e=p->model->entries[row];
    if(e.child) child(s,level,true); else invoke(s,e.action);
}
static void select_step(os64_ui_menu_session_t *s,size_t level,int delta,bool edge)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    size_t n=menu_count(p),i=p->selected;
    if(edge) i=delta>0?n-1:0;
    else if(i==MENU_NONE) i=delta>0?(p->top?p->top-1:n-1):(p->bottom<n?p->bottom:0);
    for(size_t k=0;k<n;++k) {
        i=delta>0?(i+1)%n:(i+n-1)%n;
        if(menu_selectable(p,i)) { (void)panel_select(s,level,i); return; }
    }
}
static void select_page(os64_ui_menu_session_t *s,size_t level,int delta)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    size_t span=p->bottom-p->top;
    if(!span) return;
    int64_t base=p->selected==MENU_NONE?(int64_t)(delta>0?p->top:p->bottom-1):(int64_t)p->selected;
    int64_t target=base+delta*(int64_t)span,n=(int64_t)menu_count(p);
    if(target<0) target=0;
    if(target>=n) target=n-1;
    /* Count model rows once, then adjust for an enabled landing entry. Sparse
     * commands must not multiply a page into several enabled-entry steps. */
    for(int pass=0;pass<2;++pass) {
        int step=pass?-delta:delta;
        for(int64_t at=target;at>=0&&at<n;at+=step)
            if(menu_selectable(p,(size_t)at)) { (void)panel_select(s,level,(size_t)at); return; }
    }
}
static int bar_hit(os64_ui_menubar_t *b,int32_t x,int32_t y)
{
    if(!b||!b->model||b->w.hidden||y<b->w.bounds.y||y>=b->w.bounds.y+b->w.bounds.h||
        x<b->w.bounds.x||x>=b->w.bounds.x+b->w.bounds.w) return -1;
    size_t visible=menu_bar_visible(b);
    int32_t at=b->w.bounds.x;
    for(size_t i=0;i<=visible&&i<b->model->count;++i) {
        int32_t width=i==visible?b->overflow_width:b->widths[i]+2*MENU_GAP;
        if(x<at+width) return (int)i;
        at+=width;
    }
    return -1;
}
static void reveal(os64_ui_menu_session_t *s)
{
    if(s->bar&&s->bar->w.hidden) {
        s->bar->temporary=true; os64_ui_cancel_gestures(s->ui);
        os64_ui_set_hidden(s->ui,&s->bar->w,false);
        if(s->layout) s->layout(s->user);
    }
}
static os64_ui_status_code_t prepare_bar_view(os64_ui_menu_session_t *s,os64_ui_action_view_t **out)
{
    *out=NULL;
    os64_ui_action_view_t *view=s->bar_view;
    os64_ui_status_code_t rc=os64_ui_action_view_retain(view);
    if(rc) return rc;
    rc=os64_ui_actions_refresh(view);
    publish_bar(s,false);
    if(rc) { os64_ui_action_view_release(view); return rc; }
    *out=view;
    return OS64_UI_OK;
}
static void bar_mode(os64_ui_menu_session_t *s)
{
    if(s->active) { (void)close_report(s,OS64_UI_MENU_EXPLICIT); return; }
    if(!s->bar||s->bar->w.disabled||!s->bar->model||!s->bar->model->count||!s->bar_view) return;
    os64_ui_action_view_t *view=NULL;
    os64_ui_status_code_t rc=prepare_bar_view(s,&view);
    if(rc) { report(s,rc); return; }
    s->view=view;
    reveal(s);
    if(!normalize_bar_selection(s)) {
        (void)os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT); return;
    }
    s->saved_focus=s->ui->focus; os64_ui_cancel_gestures(s->ui);
    s->active=true; s->kind=OS64_UI_MENU_BAR;
    os64_ui_mark_dirty(s->ui,&s->bar->w);
}
/* Consume a retained, refreshed candidate. The existing session keeps its
 * saved target until native teardown succeeds. */
static void bar_open_prepared(os64_ui_menu_session_t *s,size_t index,bool direct,os64_ui_action_view_t *view)
{
    os64_ui_menubar_t *b=s->bar;
    if(!b||b->w.disabled||!b->model||!b->model->count) { os64_ui_action_view_release(view); return; }
    if(index>b->model->count-1) index=0;
    bool enabled=direct?menu_model_enabled(b->model->entries[index].child,view):menu_bar_enabled_view(b,index,view);
    if(!enabled) {
        bool current=s->view==view;
        os64_ui_action_view_release(view);
        if(current) (void)reconcile_selection(s);
        else report(s,sync_bar_mode(s));
        return;
    }
    reveal(s);
    size_t visible=menu_bar_visible(b);
    /* Reveal reflows the bar; decide against the resulting affordance using
     * the same retained snapshot, rather than the app's future bar target. */
    if(!direct&&!menu_bar_enabled_view(b,index,view)) {
        os64_ui_action_view_release(view); (void)close_report(s,OS64_UI_MENU_EXPLICIT); return;
    }
    os64_ui_menu_model_t *m=NULL;
    os64_ui_status_code_t rc=OS64_UI_OK;
    int32_t x=b->w.bounds.x;
    if(!direct&&index>=visible&&visible<b->model->count) {
        index=visible;
        rc=os64_ui_menu_model_create(b->model->actions,b->model->entries+visible,b->model->count-visible,&m);
    } else { m=b->model->entries[index].child; rc=os64_ui_menu_model_retain(m); }
    if(rc) { os64_ui_action_view_release(view); report(s,close_after_failure(s,rc)); return; }
    for(size_t i=0;i<index&&i<visible;++i) x+=b->widths[i]+2*MENU_GAP;
    /* Preserve temporary reveal across replacement; open normally collapses
     * an older context session, so reveal after its close and reflow first. */
    bool temporary=b->temporary;
    rc=os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);
    if(rc) { os64_ui_menu_model_release(m); os64_ui_action_view_release(view); report(s,rc); return; }
    if(temporary) reveal(s);
    s->view=view;
    s->saved_focus=s->ui->focus; os64_ui_cancel_gestures(s->ui);
    s->active=true; s->kind=OS64_UI_MENU_BAR;
    /* Hidden mnemonic targets share the visible overflow affordance; model
     * choice stays in the retained panel rather than the bar's hit-test index. */
    b->selected=index>=visible?visible:index;
    s->anchor=(os64_gui_rect_t){x,b->w.bounds.y,index>=visible?b->overflow_width:b->widths[index]+2*MENU_GAP,b->w.bounds.h};
    os64_ui_cancel_gestures(s->ui);
    rc=request_panel(s,m,0); os64_ui_menu_model_release(m);
    if(rc!=OS64_UI_OK&&rc!=OS64_UI_BUSY) { report(s,close_after_failure(s,rc)); return; }
    os64_ui_mark_dirty(s->ui,&b->w);
}
static void bar_open_target(os64_ui_menu_session_t *s,size_t index,bool direct)
{
    if(!s->bar||s->bar->w.disabled||!s->bar->model||!s->bar->model->count||!s->bar_view) return;
    os64_ui_action_view_t *view=NULL;
    os64_ui_status_code_t rc=prepare_bar_view(s,&view);
    if(rc) { report(s,close_after_failure(s,rc)); return; }
    bar_open_prepared(s,index,direct,view);
}
static void bar_open(os64_ui_menu_session_t *s,size_t index) { bar_open_target(s,index,false); }
static void bar_step(os64_ui_menu_session_t *s,int delta,bool open)
{
    if(!s->bar||!s->bar->model) return;
    os64_ui_action_view_t *view=NULL;
    os64_ui_status_code_t rc=prepare_bar_view(s,&view);
    if(rc) { report(s,close_after_failure(s,rc)); return; }
    size_t v=menu_bar_visible(s->bar),n=v+(v<s->bar->model->count?1:0);
    size_t i=s->bar->selected;
    if(i==MENU_NONE||i>=n) i=delta>0?n-1:0;
    bool enabled=false;
    for(size_t k=0;k<n;++k) {
        i=delta>0?(i+1)%n:(i+n-1)%n;
        if(menu_bar_enabled_view(s->bar,i,view)) { enabled=true; break; }
    }
    if(!enabled) { os64_ui_action_view_release(view); (void)close_report(s,OS64_UI_MENU_EXPLICIT); return; }
    if(open) bar_open_prepared(s,i,false,view);
    else { s->bar->selected=i; os64_ui_mark_dirty(s->ui,&s->bar->w); os64_ui_action_view_release(view); }
}
static bool key_event(os64_ui_menu_session_t *s,const os64_gui_event_t *e)
{
    bool down=e->type==OS64_GUI_EVENT_KEY_DOWN;
    uint8_t mods=e->key.modifiers&(OS64_GUI_MOD_SHIFT|OS64_GUI_MOD_CTRL|OS64_GUI_MOD_ALT);
    if(left_alt(e)) {
        if(!s->active&&(!s->bar||!s->bar_view||s->bar->w.disabled||!s->bar->model||!s->bar->model->count)) { s->alt_armed=false; return false; }
        if(down) { if(mods==OS64_GUI_MOD_ALT&&!s->alt_armed) s->alt_armed=true; }
        else if(s->alt_armed) { s->alt_armed=false; bar_mode(s); }
        return true;
    }
    s->alt_armed=false;
    if(is_key(e,OS64_UI_KEY_F10,0)||is_key(e,OS64_UI_KEY_F10,OS64_GUI_MOD_SHIFT)) {
        if(!down) { bool held=s->f10_held; s->f10_held=false; return held||s->active; }
        if(s->f10_held) return true;
        if(mods&OS64_GUI_MOD_SHIFT) {
            if(close_report(s,OS64_UI_MENU_EXPLICIT)!=OS64_UI_OK) return true;
            bool handled=os64_ui_context(s->ui,e,true);
            s->f10_held=handled;
            return handled;
        }
        if(!s->active&&(!s->bar||s->bar->w.disabled||!s->bar->model||!s->bar->model->count||!s->bar_view)) return false;
        s->f10_held=true; bar_mode(s); return true;
    }
    unsigned id=identity(e),mask=1u<<(id%8);
    bool held=s->held[id/8]&mask;
    if(!down) {
        s->held[id/8]&=(uint8_t)~mask;
        if(s->active&&s->pressed_key==id+1&&s->pressed_panel<s->depth) {
            size_t level=s->pressed_panel,row=s->pressed_row; cancel_press(s);
            if(row==s->panels[level].widget->selected) activate(s,level,row);
        }
        return s->active||held;
    }
    if(held) return s->active||held;
    if(mods==OS64_GUI_MOD_ALT&&s->bar&&s->bar->model&&e->key.ascii) {
        for(size_t i=0;i<s->bar->model->count;++i)
            if(lower(s->bar->model->entries[i].mnemonic)==lower(e->key.ascii)) {
                s->held[id/8]|=(uint8_t)mask; bar_open_target(s,i,true); return true;
            }
    }
    if(!s->active) return false;
    if(mods&(OS64_GUI_MOD_CTRL|OS64_GUI_MOD_ALT)) {
        uint32_t shortcut=os64_ui_action_shortcut_id(s->view,e);
        if(shortcut) { s->held[id/8]|=(uint8_t)mask; invoke(s,shortcut); }
        return true;
    }
    char ch=0; ui_key_t k=os64_ui_decode_key(&s->decoder,e,&ch);
    if(!e->key.ascii) {
        const uint16_t named[]={OS64_UI_KEY_UP,OS64_UI_KEY_DOWN,OS64_UI_KEY_LEFT,OS64_UI_KEY_RIGHT,
            OS64_UI_KEY_HOME,OS64_UI_KEY_END,OS64_UI_KEY_PAGE_UP,OS64_UI_KEY_PAGE_DOWN,
            OS64_UI_KEY_ENTER,OS64_UI_KEY_TAB,OS64_UI_KEY_ESCAPE};
        const ui_key_t decoded[]={K_UP,K_DOWN,K_LEFT,K_RIGHT,K_HOME,K_END,K_PGUP,K_PGDN,K_ENTER,K_TAB,K_ESC};
        for(size_t i=0;i<sizeof(named)/sizeof(*named);++i)
            if(is_key(e,named[i],mods)) { k=decoded[i]; break; }
    }
    if(k==K_NONE) return true;
    size_t count=0,match=MENU_NONE,next=MENU_NONE;
    if(k==K_CHAR&&!mods&&s->depth) {
        os64_ui_popup_t *p=s->panels[s->depth-1].widget;
        for(size_t i=0;i<menu_count(p);++i) {
            char mnemonic=menu_mnemonic(p,i);
            if(mnemonic&&lower(mnemonic)==lower(ch)&&menu_selectable(p,i)) {
                ++count; if(match==MENU_NONE) match=i;
                if(next==MENU_NONE&&(p->selected==MENU_NONE||i>p->selected)) next=i;
            }
        }
    }
    bool menu_key=k==K_ESC||k==K_TAB||k==K_ENTER||k==K_UP||k==K_DOWN||k==K_LEFT||k==K_RIGHT||
        k==K_HOME||k==K_END||k==K_PGUP||k==K_PGDN||(k==K_CHAR&&ch==' ')||count;
    if(!menu_key) {
        uint32_t shortcut=os64_ui_action_shortcut_id(s->view,e);
        if(shortcut) { s->held[id/8]|=(uint8_t)mask; invoke(s,shortcut); return true; }
    }
    /* PS/2 extended navigation is a press-only escape burst. Let decoded
     * navigation repeat; activation/mode keys have release edges and retain
     * their guard. An ESC prefix cannot hold the identity before its suffix. */
    if(k==K_ENTER||k==K_CHAR||k==K_ESC||k==K_TAB) s->held[id/8]|=(uint8_t)mask;
    if(k==K_ESC) {
        if(s->depth>1) report(s,close_to(s,s->depth-1)); else (void)close_report(s,OS64_UI_MENU_EXPLICIT);
        return true;
    }
    if(k==K_TAB) {
        if(close_report(s,OS64_UI_MENU_EXPLICIT)!=OS64_UI_OK) return true;
        (void)os64_ui_focus_next(s->ui,mods&OS64_GUI_MOD_SHIFT,true); return true;
    }
    if(!s->depth) {
        if(s->pending||s->kind!=OS64_UI_MENU_BAR||!s->bar) return true;
        if(k==K_LEFT||k==K_RIGHT) bar_step(s,k==K_RIGHT?1:-1,false);
        else if(k==K_DOWN||k==K_ENTER) bar_open(s,s->bar->selected);
        return true;
    }
    size_t level=s->depth-1;
    os64_ui_popup_t *p=s->panels[level].widget;
    if(k==K_UP||k==K_DOWN) select_step(s,level,k==K_DOWN?1:-1,false);
    else if(k==K_HOME||k==K_END) {
        select_step(s,level,k==K_HOME?1:-1,true);
    } else if(k==K_PGUP||k==K_PGDN) {
        select_page(s,level,k==K_PGDN?1:-1);
    } else if(k==K_RIGHT) {
        if(!p->list&&p->selected!=MENU_NONE&&p->model->entries[p->selected].child) child(s,level,true);
        else if(s->kind==OS64_UI_MENU_BAR&&level==0) bar_step(s,1,true);
    } else if(k==K_LEFT) {
        if(level) report(s,close_to(s,level)); else if(s->kind==OS64_UI_MENU_BAR) bar_step(s,-1,true);
    } else if(k==K_ENTER||(k==K_CHAR&&ch==' ')) {
        if(p->selected!=MENU_NONE) {
            s->pressed_key=(uint16_t)(id+1); s->pressed_panel=level; s->pressed_row=p->selected;
        }
    } else if(k==K_CHAR&&!mods) {
        /* A menu's unique mnemonic runs its command; in a list a letter only
         * moves the highlight, a unique match included: the value changes
         * when it is committed (DROPDOWN.md, contract 2). */
        if(count==1&&!p->list) activate(s,level,match);
        else if(count) (void)panel_select(s,level,next==MENU_NONE?match:next);
    }
    return true;
}
static bool focus_event(os64_ui_menu_session_t *s,int64_t handle,const os64_gui_event_t *e)
{
    if(e->type!=OS64_GUI_EVENT_WINDOW_FOCUS) return false;
    s->alt_armed=false;
    int64_t other=e->focus.other_handle;
    bool retired=false;
    for(size_t i=0;i<s->dispatcher->dead_count;++i)
        if(s->dispatcher->tombstones[i]==other) retired=true;
    if(retired) return false;
    if(s->active) {
        if(s->owner&&((handle==s->owner&&e->focus.gained&&other&&other!=s->owner&&!retired)||
            (handle!=s->owner&&!e->focus.gained&&other==s->owner))) {
            s->suppress_pending=true;
            (void)close_report(s,OS64_UI_MENU_DISMISSED);
        }
    }
    /* A release routed outside the family cannot clear our guards, including
     * after a shortcut already closed the session on its down edge. */
    if(!e->focus.gained&&!family(s,other)) {
        (void)close_report(s,OS64_UI_MENU_LOST);
    }
    return false;
}
static bool owner_event(os64_ui_t *ui,const os64_gui_event_t *e)
{
    os64_ui_menu_session_t *s=ui->menu;
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN||e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP||
        e->type==OS64_GUI_EVENT_MOUSE_MOVE||e->type==OS64_GUI_EVENT_MOUSE_WHEEL) s->alt_armed=false;
    if(e->type==OS64_GUI_EVENT_INPUT_LOST) {
        (void)close_report(s,OS64_UI_MENU_LOST); os64_ui_cancel_gestures(ui); return false;
    }
    if(e->type==OS64_GUI_EVENT_WINDOW_COVERED) reset_input(s);
    if(e->type==OS64_GUI_EVENT_WINDOW_RESIZE||e->type==OS64_GUI_EVENT_WINDOW_CLOSE) {
        return close_report(s,OS64_UI_MENU_GEOMETRY)!=OS64_UI_OK;
    }
    if(e->type==OS64_GUI_EVENT_APPEARANCE) {
        /* Font session preparation has its own hook; color-only adoption can
         * leave live menus intact. The font follower closes before resolving. */
        return false;
    }
    (void)focus_event(s,s->owner,e);
    if(e->type==OS64_GUI_EVENT_MOUSE_WHEEL&&s->active) return true;
    if(e->type==OS64_GUI_EVENT_KEY_DOWN||e->type==OS64_GUI_EVENT_KEY_UP) return key_event(s,e);
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP&&s->suppress_button==(uint8_t)(e->mouse.button+1)) {
        s->suppress_button=0; return true;
    }
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN||e->type==OS64_GUI_EVENT_MOUSE_MOVE) {
        s->alt_armed=false;
        int i=bar_hit(s->bar,e->mouse.x,e->mouse.y);
        if(i>=0&&(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN&&e->mouse.button==OS64_GUI_MOUSE_LEFT)) {
            bool toggle=s->active&&s->kind==OS64_UI_MENU_BAR&&s->bar->selected==(size_t)i;
            if(toggle) (void)close_report(s,OS64_UI_MENU_EXPLICIT); else bar_open(s,(size_t)i);
            s->suppress_button=OS64_GUI_MOUSE_LEFT+1; return true;
        }
        if(i>=0&&s->active&&s->kind==OS64_UI_MENU_BAR&&s->bar->selected!=(size_t)i) { bar_open(s,(size_t)i); return true; }
        if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN) {
            s->suppress_button=0; /* A lost release cannot pair with this down. */
            if(s->active||s->suppress_pending) {
                (void)close_report(s,OS64_UI_MENU_DISMISSED);
                s->suppress_pending=false; s->suppress_button=(uint8_t)(e->mouse.button+1); return true;
            }
        }
    }
    return false;
}
static void panel_paint(void *user)
{
    menu_panel *p=user;
    if(!p->handle||!p->widget) return;
    p->widget->w.cls->paint(&p->widget->w,&p->draw,&p->session->ui->theme);
    p->session->dispatcher->native.publish(p->session->dispatcher->native.user,p->handle,p->widget->w.bounds);
}
static void panel_scroll(os64_ui_menu_session_t *s,size_t level,int32_t rows)
{
    os64_ui_popup_t *p=s->panels[level].widget;
    if(menu_popup_scroll_top(p,rows)==p->top) return;
    cancel_press(s);
    if(s->pending&&s->pending_depth>level) release_pending(s);
    os64_ui_status_code_t rc=close_to(s,level+1);
    if(rc) { report(s,rc); return; }
    os64_ui_popup_scroll(p,rows); help(s,p);
}
static void panel_event(void *user,const os64_gui_event_t *e)
{
    menu_panel *panel=user;
    os64_ui_menu_session_t *s=panel->session;
    if(!s||!s->active) return;
    size_t level=panel->index;
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN||e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP||
        e->type==OS64_GUI_EVENT_MOUSE_MOVE||e->type==OS64_GUI_EVENT_MOUSE_WHEEL) s->alt_armed=false;
    if(e->type==OS64_GUI_EVENT_INPUT_LOST) {
        (void)close_report(s,OS64_UI_MENU_LOST); os64_ui_cancel_gestures(s->ui); return;
    }
    if(e->type==OS64_GUI_EVENT_WINDOW_COVERED) reset_input(s);
    if(e->type==OS64_GUI_EVENT_WINDOW_CLOSE||e->type==OS64_GUI_EVENT_WINDOW_RESIZE) {
        (void)close_report(s,OS64_UI_MENU_GEOMETRY); return;
    }
    if(e->type==OS64_GUI_EVENT_APPEARANCE) {
        /* Route to the owner even when the popup's coalesced slot wins. */
        (void)os64_ui_dispatch(s->ui,e); return;
    }
    (void)focus_event(s,panel->handle,e);
    if(!s->active) return;
    if(e->type==OS64_GUI_EVENT_KEY_DOWN||e->type==OS64_GUI_EVENT_KEY_UP) { (void)key_event(s,e); return; }
    os64_ui_popup_t *p=panel->widget;
    if(e->type==OS64_GUI_EVENT_MOUSE_WHEEL) {
        panel_scroll(s,level,e->mouse.dy>64?192:e->mouse.dy< -64?-192:e->mouse.dy*3); return;
    }
    if(e->type==OS64_GUI_EVENT_MOUSE_MOVE||e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN||e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP) {
        s->alt_armed=false;
        int row=os64_ui_popup_hit(p,e->mouse.x,e->mouse.y);
        if(e->type==OS64_GUI_EVENT_MOUSE_MOVE) {
            if(s->pressed_panel==level&&s->pressed_row!=(size_t)row) cancel_press(s);
            if(row>=0) {
                if(!panel_select(s,level,(size_t)row)) return;
                if(!p->list&&p->selected!=MENU_NONE&&p->model->entries[p->selected].child) child(s,level,false);
                else { release_pending(s); report(s,close_to(s,level+1)); }
            }
        } else if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN&&e->mouse.button==OS64_GUI_MOUSE_LEFT) {
            if(row==-2||row==-3) panel_scroll(s,level,row==-2?-1:1);
            else if(row>=0&&menu_selectable(p,(size_t)row)) {
                if(!panel_select(s,level,(size_t)row)) return;
                s->pressed_panel=level; s->pressed_row=(size_t)row; s->pressed_key=0;
            }
        } else if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP&&e->mouse.button==OS64_GUI_MOUSE_LEFT) {
            bool match=s->pressed_panel==level&&s->pressed_row==(size_t)row&&!s->pressed_key;
            cancel_press(s);
            if(match&&row>=0) activate(s,level,(size_t)row);
        }
    }
}
