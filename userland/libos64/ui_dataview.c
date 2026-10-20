#include "os64/ui_dataview.h"
#include "os64/ui_action.h"
#include "ui_internal.h"
#include "ui_data_internal.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/proc.h"

typedef struct {
    os64_gui_rect_t bounds, body, header, vertical, horizontal;
    os64_ui_t *font_ui;
    bool app_face;
    size_t top, full, shown, columns, slots, bytes;
    int32_t left, content, pitch, header_pitch, icon_size;
    os64_ui_dataview_column_t column[OS64_UI_DATA_COLUMNS];
    void **runs;
    int32_t *advances;
    os64_ui_icon_lease_t *icons;
    size_t icon_failures;
} view_frame;
typedef struct {
    os64_ui_selection_t *selection;
    const os64_ui_icon_provider_t *provider;
    view_frame *live, *staged;
    uint64_t revision, staged_revision;
    size_t state_bytes;
    uint32_t change_flags;
    bool busy, blocked;
    os64_ui_dataview_diagnostics_t diagnostics;
    uint8_t seq, modifiers;
    uint16_t held_enter, held_space;
    uint64_t pressed_id, pressed_revision, last_click;
    int64_t click_time, prefix_time;
    int32_t press_x, press_y, guide_x;
    uint32_t pressed_header, resize_column;
    bool pointer, moved;
    char prefix[65];
    size_t prefix_length;
    int64_t (*clock)(void *);
    void *clock_user;
} view_state;
static view_state *vs(const os64_ui_dataview_t *v) { return v ? v->state : NULL; }
static os64_ui_data_projection_t *projection(view_state *s) { return os64_ui_selection_projection(s->selection); }
static os64_ui_data_t *snapshot(view_state *s) { return os64_ui_data_projection_snapshot(projection(s)); }
static bool same_rect(os64_gui_rect_t a, os64_gui_rect_t b)
{ return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h; }
static void dirty(os64_ui_dataview_t *v)
{ if (v->w.ui) os64_ui_mark_dirty(v->w.ui,&v->w); }
static void clear_click(view_state *s) { s->last_click=0; s->click_time=0; }
static void clear_prefix(view_state *s) { s->prefix_length=0; s->prefix[0]=0; s->prefix_time=0; }
static void view_cancel(os64_ui_widget_t *w)
{
    os64_ui_dataview_t *v=(void *)w;
    view_state *s=vs(v);
    w->pressed=false;
    if (!s) return;
    s->pointer=s->moved=false; s->pressed_id=s->pressed_header=s->resize_column=0;
    s->seq=0; s->held_enter=s->held_space=0;
    clear_prefix(s); clear_click(s);
    v->vertical.drag_grab=v->horizontal.drag_grab=-1;
    dirty(v);
}
static void end_gesture(os64_ui_dataview_t *v)
{
    view_state *s=vs(v);
    uint16_t enter=s?s->held_enter:0, space=s?s->held_space:0;
    view_cancel(&v->w);
    /* A synchronous model/layout change ends pointer intent, but a held key
     * still belongs to its original press until release or input loss. */
    if (s) { s->held_enter=enter; s->held_space=space; }
    os64_ui_t *ui=v->w.ui;
    if (ui && (ui->grab==&v->w || ui->grab==&v->vertical.w || ui->grab==&v->horizontal.w)) {
        ui->grab=NULL; ui->grab_button=0;
    }
}
static void frame_free(view_state *s, view_frame *f)
{
    if (!f) return;
    for (size_t i=0;i<f->slots;++i) os64_ui_run_release(f->runs[i]);
    for (size_t i=0;i<f->shown;++i) os64_ui_icon_release(s->provider,&f->icons[i]);
    os64_free(f);
}
static os64_ui_status_code_t font_status(os64_font_status_t rc)
{
    switch (rc) {
    case OS64_FONT_OK: return OS64_UI_OK;
    case OS64_FONT_NO_MEMORY: return OS64_UI_NO_MEMORY;
    case OS64_FONT_LIMIT: return OS64_UI_LIMIT;
    case OS64_FONT_BUSY: return OS64_UI_BUSY;
    case OS64_FONT_BAD_ARGUMENT: return OS64_UI_INVALID;
    default: return OS64_UI_ERROR;
    }
}
static os64_font_status_t to_font(os64_ui_status_code_t rc)
{
    switch (rc) {
    case OS64_UI_OK: return OS64_FONT_OK;
    case OS64_UI_NO_MEMORY: return OS64_FONT_NO_MEMORY;
    case OS64_UI_LIMIT: return OS64_FONT_LIMIT;
    case OS64_UI_BUSY: return OS64_FONT_BUSY;
    case OS64_UI_INVALID: return OS64_FONT_BAD_ARGUMENT;
    default: return OS64_FONT_ENGINE_ERROR;
    }
}
static os64_ui_status_code_t result(os64_ui_dataview_t *v, os64_ui_dataview_operation_t op, os64_ui_status_code_t rc)
{
    view_state *s=vs(v);
    if (!v || v->notifying || (s && s->busy)) return rc;
    v->last_status=rc; v->last_operation=op;
    if (rc && v->on_error) {
        v->notifying=true;
        if (s) s->busy=true;
        v->on_error(v,op,rc,v->callback_user);
        if (s) s->busy=false;
        v->notifying=false;
    }
    return rc;
}
static os64_ui_status_code_t ready(os64_ui_dataview_t *v)
{
    if (!v) return OS64_UI_INVALID;
    if (v->notifying) return OS64_UI_BUSY;
    return !vs(v)?OS64_UI_INVALID:vs(v)->busy || vs(v)->staged?OS64_UI_BUSY:vs(v)->revision==UINT64_MAX?OS64_UI_LIMIT:OS64_UI_OK;
}
static size_t selection_metadata(const os64_ui_selection_t *s)
{ return os64_ui_selection_bytes(s)+os64_ui_data_projection_bytes(os64_ui_selection_projection(s)); }
static size_t metadata(view_state *s, const view_frame *candidate, const os64_ui_selection_t *sel)
{
    size_t n=s->state_bytes+sizeof(os64_ui_dataview_t)+selection_metadata(s->selection);
    if (s->live) n+=s->live->bytes;
    if (candidate) n+=candidate->bytes;
    if (sel && sel != s->selection) {
        n+=os64_ui_selection_bytes(sel);
        if (os64_ui_selection_projection(sel) != projection(s)) n+=os64_ui_data_projection_bytes(os64_ui_selection_projection(sel));
    }
    return n;
}
static void record_peak(view_state *s,size_t bytes)
{ if (bytes>s->diagnostics.peak_metadata_bytes) s->diagnostics.peak_metadata_bytes=bytes; }
static os64_ui_status_code_t prepare(os64_ui_dataview_t *v, const os64_ui_selection_t *sel,
    const view_frame *presentation, os64_gui_rect_t bounds, size_t top, int64_t left, view_frame **out)
{
    *out=NULL;
    view_state *s=vs(v);
    os64_ui_data_projection_t *p=os64_ui_selection_projection(sel);
    os64_ui_data_t *d=os64_ui_data_projection_snapshot(p);
    if (!p || bounds.w<0 || bounds.h<0 || bounds.x<0 || bounds.y<0 ||
        (int64_t)bounds.x+bounds.w>INT32_MAX-65536 || (int64_t)bounds.y+bounds.h>INT32_MAX-65536) return OS64_UI_INVALID;
    if (v->w.ui && !os64_ui_font_context(v->w.ui)) return OS64_UI_NO_MEMORY;
    view_frame plan={0};
    plan.bounds=bounds; plan.font_ui=v->w.ui; plan.app_face=v->w.app_face;
    int32_t line=os64_ui_font_row_height(v->w.ui,ui_role(&v->w,OS64_FONT_ROLE_UI));
    if (line<=0 || line>4096) return OS64_UI_LIMIT;
    plan.icon_size=(int32_t)os64_ui_icon_menu_size(line);
    plan.pitch=(line>plan.icon_size?line:plan.icon_size)+8; plan.header_pitch=line+12;
    int32_t strip=v->w.ui?v->w.ui->theme.scroll_w:16;
    if (strip<8) strip=16;
    if (strip>64) strip=64;
    int32_t bw=bounds.w>strip+2?bounds.w-strip-2:0;
    int32_t hh=bounds.h>2?bounds.h-2:0;
    if (hh>plan.header_pitch) hh=plan.header_pitch;
    int32_t bh=bounds.h>hh+strip+2?bounds.h-hh-strip-2:0;
    plan.header=(os64_gui_rect_t){bounds.x+1,bounds.y+1,bw,hh};
    plan.body=(os64_gui_rect_t){bounds.x+1,bounds.y+1+hh,bw,bh};
    plan.vertical=(os64_gui_rect_t){bounds.x+1+bw,plan.body.y,bounds.w>bw+2?bounds.w-bw-2:0,bh};
    plan.horizontal=(os64_gui_rect_t){bounds.x+1,plan.body.y+bh,bw,bounds.h>hh+bh+2?bounds.h-hh-bh-2:0};
    plan.columns=os64_ui_data_columns(d);
    for (size_t c=0;c<plan.columns;++c) {
        const os64_ui_data_column_t *col=os64_ui_data_column(d,c);
        plan.column[c]=presentation?presentation->column[c]:(os64_ui_dataview_column_t){col->id,col->width,true};
        if (plan.column[c].visible) plan.content+=(int32_t)plan.column[c].width;
    }
    plan.full=(size_t)(bh/plan.pitch);
    size_t count=os64_ui_data_projection_count(p), page=plan.full?plan.full:1;
    size_t maximum=count>page?count-page:0;
    plan.top=top<maximum?top:maximum;
    int64_t maxleft=plan.content>bw?plan.content-bw:0;
    plan.left=(int32_t)(left<0?0:left>maxleft?maxleft:left);
    plan.shown=bh && bw?(size_t)((bh+plan.pitch-1)/plan.pitch):0;
    if (plan.shown>count-plan.top) plan.shown=count-plan.top;
    plan.slots=(plan.shown+1)*plan.columns;
    if (plan.slots>OS64_UI_DATAVIEW_RUNS) return OS64_UI_LIMIT;
    size_t advance_slots=(plan.slots+1)&~(size_t)1;
    plan.bytes=sizeof(view_frame)+plan.slots*sizeof(void *)+advance_slots*sizeof(int32_t)+plan.shown*sizeof(os64_ui_icon_lease_t);
    size_t bytes=metadata(s,&plan,sel);
    if (bytes>OS64_UI_DATA_VIEW_BYTES) return OS64_UI_LIMIT;
    record_peak(s,bytes);
    view_frame *f=os64_malloc(plan.bytes);
    if (!f) return OS64_UI_NO_MEMORY;
    os64_memset(f,0,plan.bytes); *f=plan;
    size_t capacity=os64_malloc_size(f);
    if (capacity>OS64_UI_DATA_VIEW_BYTES-16) { os64_free(f); return OS64_UI_LIMIT; }
    f->bytes=capacity+16;
    bytes=metadata(s,f,sel);
    if (bytes>OS64_UI_DATA_VIEW_BYTES) { os64_free(f); return OS64_UI_LIMIT; }
    record_peak(s,bytes);
    f->runs=(void *)(f+1); f->advances=(void *)(f->runs+f->slots); f->icons=(void *)(f->advances+advance_slots);
    for (size_t r=0;r<=f->shown;++r) {
        const os64_ui_data_row_t *row=r?os64_ui_data_row(d,os64_ui_data_projection_id(p,f->top+r-1)):NULL;
        for (size_t c=0;c<f->columns;++c) {
            if (!f->column[c].visible) continue;
            size_t schema=0;
            while (os64_ui_data_column(d,schema)->id!=f->column[c].id) ++schema;
            os64_ui_data_text_t text=row?row->cells[schema].display:os64_ui_data_column(d,schema)->title;
            size_t slot=r*f->columns+c;
            os64_ui_status_code_t rc=font_status(os64_ui_run_layout(v->w.ui,ui_role(&v->w,OS64_FONT_ROLE_UI),text.bytes,text.length,&f->runs[slot]));
            if (!rc && v->w.ui && !f->runs[slot]) rc=font_status(os64_ui_font_status(v->w.ui));
            if (!rc) rc=font_status(os64_ui_run_width(v->w.ui,&f->runs[slot],ui_role(&v->w,OS64_FONT_ROLE_UI),text.bytes,text.length,&f->advances[slot]));
            if (rc) { frame_free(s,f); return rc; }
        }
        if (row && row->icon.length) {
            os64_ui_icon_status_t rc=os64_ui_icon_acquire(s->provider,row->icon.bytes,(uint32_t)f->icon_size,&f->icons[r-1]);
            if (rc!=OS64_UI_ICON_OK) ++f->icon_failures;
        }
    }
    *out=f;
    return OS64_UI_OK;
}
static void sync_bars(os64_ui_dataview_t *v)
{
    view_state *s=vs(v);
    if (!s || !s->live) return;
    view_frame *f=s->live;
    v->vertical.w.bounds=f->vertical; v->horizontal.w.bounds=f->horizontal;
    v->vertical.w.hidden=v->horizontal.w.hidden=s->blocked;
    v->vertical.w.disabled=v->horizontal.w.disabled=s->blocked;
    v->vertical.total=(int64_t)os64_ui_data_projection_count(projection(s));
    v->vertical.visible=(int64_t)(f->full?f->full:1); v->vertical.pos=(int64_t)f->top;
    v->horizontal.total=f->content; v->horizontal.visible=f->body.w; v->horizontal.pos=f->left;
}
static uint32_t selection_flags(const os64_ui_selection_t *a,const os64_ui_selection_t *b)
{
    uint32_t flags=0;
    if (os64_ui_selection_focus(a)!=os64_ui_selection_focus(b) ||
        os64_ui_selection_anchor(a)!=os64_ui_selection_anchor(b)) flags|=OS64_UI_DATAVIEW_FOCUS;
    bool different=os64_ui_selection_count(a)!=os64_ui_selection_count(b);
    ui_selection_state *x=a?a->state:NULL, *y=b?b->state:NULL;
    if (!different && x && y) {
        if (x->projection==y->projection) different=os64_memcmp(x->selected,y->selected,os64_ui_data_projection_count(x->projection))!=0;
        else for (size_t i=0;i<os64_ui_data_projection_count(x->projection) && !different;++i)
            if (x->selected[i] && !os64_ui_selection_contains(b,os64_ui_data_projection_id(x->projection,i))) different=true;
    }
    if (different) flags|=OS64_UI_DATAVIEW_SELECTION;
    return flags;
}
static void publish(os64_ui_dataview_t *v,view_frame *f,os64_ui_selection_t *sel,bool cancel)
{
    view_state *s=vs(v);
    s->change_flags=sel?selection_flags(s->selection,sel):0;
    if (sel && os64_ui_selection_projection(sel)!=projection(s)) s->change_flags|=OS64_UI_DATAVIEW_MODEL;
    if (!s->live || s->live->top!=f->top || s->live->left!=f->left ||
        !same_rect(s->live->bounds,f->bounds) || s->live->pitch!=f->pitch) s->change_flags|=OS64_UI_DATAVIEW_VIEWPORT;
    if (!s->live || s->live->columns!=f->columns) s->change_flags|=OS64_UI_DATAVIEW_COLUMNS;
    else for (size_t i=0;i<f->columns;++i) {
        os64_ui_dataview_column_t a=s->live->column[i], b=f->column[i];
        if (a.id!=b.id || a.width!=b.width || a.visible!=b.visible) s->change_flags|=OS64_UI_DATAVIEW_COLUMNS;
    }
    if (cancel) end_gesture(v);
    if (sel) ui_selection_publish(s->selection,sel,v);
    view_frame *old=s->live; s->live=f; s->blocked=false; ++s->revision;
    frame_free(s,old); sync_bars(v); dirty(v);
    s->diagnostics.metadata_bytes=metadata(s,NULL,NULL);
    s->diagnostics.prepared_rows=f->shown;
    s->diagnostics.run_slots=f->slots; s->diagnostics.runs=0;
    for (size_t i=0;i<f->slots;++i) s->diagnostics.runs+=f->runs[i]!=NULL;
    s->diagnostics.icon_failures=f->icon_failures;
    s->diagnostics.icon_leases=0;
    for (size_t i=0;i<f->shown;++i) s->diagnostics.icon_leases+=f->icons[i].pixels!=NULL;
}
static void block(os64_ui_dataview_t *v)
{
    view_state *s=vs(v);
    if (!s) return;
    s->blocked=true; end_gesture(v); sync_bars(v); dirty(v);
}
static bool usable(os64_ui_dataview_t *v)
{
    view_state *s=vs(v);
    if (!s || !s->live) return false;
    if (!s->blocked && (!same_rect(v->w.bounds,s->live->bounds) || s->live->font_ui!=v->w.ui || s->live->app_face!=v->w.app_face)) block(v);
    return !s->blocked;
}
static void notify(os64_ui_dataview_t *v,uint32_t flags,uint64_t activated)
{
    view_state *s=vs(v);
    if (!s || !v->on_change) return;
    flags=flags?s->change_flags:0;
    if (!flags && !activated) return;
    os64_ui_dataview_change_t change={s->revision,flags,os64_ui_data_domain(snapshot(s)),activated};
    s->busy=true; v->notifying=true; v->on_change(v,&change,v->callback_user); v->notifying=false; s->busy=false;
}
os64_ui_selection_t *os64_ui_dataview_selection(const os64_ui_dataview_t *v) { return vs(v)?vs(v)->selection:NULL; }
uint64_t os64_ui_dataview_revision(const os64_ui_dataview_t *v) { return vs(v)?vs(v)->revision:0; }
size_t os64_ui_dataview_top(const os64_ui_dataview_t *v) { return vs(v)&&vs(v)->live?vs(v)->live->top:0; }
int64_t os64_ui_dataview_left(const os64_ui_dataview_t *v) { return vs(v)&&vs(v)->live?vs(v)->live->left:0; }
size_t os64_ui_dataview_full_rows(const os64_ui_dataview_t *v) { return vs(v)&&vs(v)->live?vs(v)->live->full:0; }
bool os64_ui_dataview_blocked(const os64_ui_dataview_t *v)
{ return !vs(v) || !vs(v)->live || vs(v)->blocked || !same_rect(v->w.bounds,vs(v)->live->bounds) || vs(v)->live->font_ui!=v->w.ui || vs(v)->live->app_face!=v->w.app_face; }
os64_ui_status_code_t os64_ui_dataview_status(const os64_ui_dataview_t *v,os64_ui_dataview_operation_t *op)
{ if (op) *op=v?v->last_operation:OS64_UI_DATAVIEW_ATTACH; return v?v->last_status:OS64_UI_INVALID; }
bool os64_ui_dataview_column(const os64_ui_dataview_t *v,size_t at,os64_ui_dataview_column_t *out)
{
    if (!vs(v) || !vs(v)->live || !out || at>=vs(v)->live->columns) return false;
    *out=vs(v)->live->column[at]; return true;
}
os64_ui_dataview_diagnostics_t os64_ui_dataview_diagnostics(const os64_ui_dataview_t *v)
{ return vs(v)?vs(v)->diagnostics:(os64_ui_dataview_diagnostics_t){0}; }
os64_ui_status_code_t os64_ui_dataview_attach(os64_ui_dataview_t *v,os64_ui_selection_t *sel,const os64_ui_icon_provider_t *provider)
{
    if (!v || !sel || !sel->state) return result(v,OS64_UI_DATAVIEW_ATTACH,OS64_UI_INVALID);
    if (v->notifying || v->state || ((ui_selection_state *)sel->state)->owner) return result(v,OS64_UI_DATAVIEW_ATTACH,OS64_UI_BUSY);
    view_state *s=os64_malloc(sizeof(*s));
    if (!s) return result(v,OS64_UI_DATAVIEW_ATTACH,OS64_UI_NO_MEMORY);
    os64_memset(s,0,sizeof(*s)); s->state_bytes=os64_malloc_size(s)+16; s->selection=sel; s->provider=provider; v->state=s;
    view_frame *f=NULL;
    s->busy=true;
    os64_ui_status_code_t rc=prepare(v,sel,NULL,v->w.bounds,0,0,&f);
    s->busy=false;
    if (rc) { os64_free(s); v->state=NULL; return result(v,OS64_UI_DATAVIEW_ATTACH,rc); }
    ((ui_selection_state *)sel->state)->owner=v;
    publish(v,f,NULL,true);
    return result(v,OS64_UI_DATAVIEW_ATTACH,OS64_UI_OK);
}
os64_ui_status_code_t os64_ui_dataview_dispose(os64_ui_dataview_t *v)
{
    if (!v) return OS64_UI_INVALID;
    view_state *s=vs(v);
    if (v->notifying || (s && (s->busy || s->staged))) return result(v,OS64_UI_DATAVIEW_DISPOSE,OS64_UI_BUSY);
    if (!s) return result(v,OS64_UI_DATAVIEW_DISPOSE,OS64_UI_OK);
    end_gesture(v);
    s->busy=true;
    frame_free(s,s->live); frame_free(s,s->staged);
    if (s->selection && s->selection->state) ((ui_selection_state *)s->selection->state)->owner=NULL;
    os64_free(s); v->state=NULL;
    v->vertical.w.hidden=v->horizontal.w.hidden=true;
    return result(v,OS64_UI_DATAVIEW_DISPOSE,OS64_UI_OK);
}
static os64_ui_status_code_t bind_impl(os64_ui_dataview_t *v,os64_ui_data_projection_t *p,os64_ui_data_bind_t mode)
{
    view_state *s=vs(v);
    if (!p || mode<OS64_UI_DATA_PRESERVE || mode>OS64_UI_DATA_RESET) return OS64_UI_INVALID;
    os64_ui_selection_t sel={0};
    os64_ui_status_code_t rc=ui_selection_copy(s->selection,&sel);
    if (rc) return rc;
    size_t transient=metadata(s,NULL,&sel)+os64_ui_selection_bytes(&sel)+(p==projection(s)?0:os64_ui_data_projection_bytes(p));
    if (transient>OS64_UI_DATA_VIEW_BYTES) { os64_ui_selection_dispose(&sel); return OS64_UI_LIMIT; }
    record_peak(s,transient);
    rc=os64_ui_selection_bind(&sel,p,mode);
    view_frame *f=NULL;
    if (!rc) {
        bool preserve=mode==OS64_UI_DATA_PRESERVE && os64_ui_data_domain(snapshot(s))==os64_ui_data_domain(os64_ui_data_projection_snapshot(p));
        size_t top=0;
        if (preserve && s->live) {
            top=s->live->top;
            uint64_t id=os64_ui_data_projection_id(projection(s),top);
            os64_ui_data_projection_position(p,id,&top);
        }
        rc=prepare(v,&sel,preserve?s->live:NULL,v->w.bounds,top,preserve&&s->live?s->live->left:0,&f);
    }
    if (!rc) publish(v,f,&sel,true);
    os64_ui_selection_dispose(&sel);
    return rc;
}
os64_ui_status_code_t os64_ui_dataview_bind(os64_ui_dataview_t *v,os64_ui_data_projection_t *p,os64_ui_data_bind_t mode)
{
    os64_ui_status_code_t rc=ready(v);
    if (!rc) { vs(v)->busy=true; rc=bind_impl(v,p,mode); vs(v)->busy=false; }
    return result(v,OS64_UI_DATAVIEW_BIND,rc);
}
os64_ui_status_code_t os64_ui_dataview_restore(os64_ui_dataview_t *v,
    const os64_ui_selection_t *saved,size_t top,int64_t left)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_BIND,rc);
    if (!saved || !saved->state) return result(v,OS64_UI_DATAVIEW_BIND,OS64_UI_INVALID);
    view_state *s=vs(v);
    if (os64_ui_selection_revision(s->selection)==UINT64_MAX)
        return result(v,OS64_UI_DATAVIEW_BIND,OS64_UI_LIMIT);
    os64_ui_selection_t candidate={0};
    view_frame *f=NULL;
    s->busy=true;
    rc=os64_ui_selection_clone(saved,&candidate);
    if (!rc) {
        /* Imported bookmarks can have older revisions. Enumeration tokens
         * belong to this live selection, not to the source of its contents. */
        ((ui_selection_state *)candidate.state)->revision=os64_ui_selection_revision(s->selection)+1;
        os64_ui_data_t *data=os64_ui_data_projection_snapshot(os64_ui_selection_projection(&candidate));
        const view_frame *columns=os64_ui_data_schema_equal(snapshot(s),data)?s->live:NULL;
        rc=prepare(v,&candidate,columns,v->w.bounds,top,left,&f);
    }
    if (!rc) publish(v,f,&candidate,true);
    os64_ui_selection_dispose(&candidate);
    s->busy=false;
    return result(v,OS64_UI_DATAVIEW_BIND,rc);
}
os64_ui_status_code_t os64_ui_dataview_layout(os64_ui_dataview_t *v)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_LAYOUT,rc);
    view_state *s=vs(v); view_frame *f=NULL;
    s->busy=true;
    rc=prepare(v,s->selection,s->live,v->w.bounds,os64_ui_dataview_top(v),os64_ui_dataview_left(v),&f);
    if (!rc) publish(v,f,NULL,true);
    else if (!s->live || (!same_rect(v->w.bounds,s->live->bounds) || s->live->font_ui!=v->w.ui || s->live->app_face!=v->w.app_face)) block(v);
    s->busy=false;
    return result(v,OS64_UI_DATAVIEW_LAYOUT,rc);
}
os64_ui_status_code_t os64_ui_dataview_scroll(os64_ui_dataview_t *v,size_t top,int64_t left)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_SCROLL,rc);
    if (!usable(v)) return result(v,OS64_UI_DATAVIEW_SCROLL,OS64_UI_REFUSED);
    view_state *s=vs(v); view_frame *f=NULL; s->busy=true;
    rc=prepare(v,s->selection,s->live,v->w.bounds,top,left,&f);
    if (!rc) { clear_click(s); publish(v,f,NULL,false); }
    s->busy=false; sync_bars(v);
    return result(v,OS64_UI_DATAVIEW_SCROLL,rc);
}
os64_ui_status_code_t os64_ui_dataview_sort(os64_ui_dataview_t *v,os64_ui_data_sort_t order)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_SORT,rc);
    view_state *s=vs(v); os64_ui_data_projection_t *p=NULL; s->busy=true;
    rc=os64_ui_data_projection_sort(projection(s),order,&p);
    if (!rc) rc=bind_impl(v,p,OS64_UI_DATA_PRESERVE);
    os64_ui_data_projection_release(p); s->busy=false;
    return result(v,OS64_UI_DATAVIEW_SORT,rc);
}
os64_ui_status_code_t os64_ui_dataview_column_set(os64_ui_dataview_t *v,uint32_t id,uint32_t width,bool visible,size_t position)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_COLUMN,rc);
    view_state *s=vs(v);
    if (!s->live || width<48 || width>4096 || position>=s->live->columns) return result(v,OS64_UI_DATAVIEW_COLUMN,OS64_UI_INVALID);
    view_frame presentation=*s->live;
    size_t at=0, schema=0;
    while (at<presentation.columns && presentation.column[at].id!=id) ++at;
    while (schema<presentation.columns && os64_ui_data_column(snapshot(s),schema)->id!=id) ++schema;
    if (at==presentation.columns || (!visible && os64_ui_data_column(snapshot(s),schema)->required)) return result(v,OS64_UI_DATAVIEW_COLUMN,OS64_UI_INVALID);
    os64_ui_dataview_column_t column={id,width,visible};
    if (at<position) for (size_t i=at;i<position;++i) presentation.column[i]=presentation.column[i+1];
    else for (size_t i=at;i>position;--i) presentation.column[i]=presentation.column[i-1];
    presentation.column[position]=column;
    view_frame *f=NULL; s->busy=true;
    rc=prepare(v,s->selection,&presentation,v->w.bounds,presentation.top,presentation.left,&f);
    if (!rc) publish(v,f,NULL,true);
    s->busy=false;
    return result(v,OS64_UI_DATAVIEW_COLUMN,rc);
}
static os64_ui_status_code_t command_impl(os64_ui_dataview_t *v,os64_ui_selection_command_t cmd,uint64_t id,bool reveal)
{
    view_state *s=vs(v); os64_ui_selection_t sel={0};
    os64_ui_status_code_t rc=ui_selection_copy(s->selection,&sel);
    if (!rc && metadata(s,NULL,&sel)>OS64_UI_DATA_VIEW_BYTES) rc=OS64_UI_LIMIT;
    if (!rc) record_peak(s,metadata(s,NULL,&sel));
    if (!rc) rc=os64_ui_selection_command(&sel,cmd,id);
    if (!rc) {
        size_t top=s->live->top, at;
        if (reveal && os64_ui_data_projection_position(projection(s),os64_ui_selection_focus(&sel),&at)) {
            size_t page=s->live->full?s->live->full:1;
            if (at<top) top=at; else if (at>=top+page) top=at-page+1;
        }
        if (top!=s->live->top) {
            view_frame *f=NULL;
            rc=prepare(v,&sel,s->live,v->w.bounds,top,s->live->left,&f);
            if (!rc) publish(v,f,&sel,false);
        } else {
            s->change_flags=selection_flags(s->selection,&sel);
            if (s->change_flags) {
                ui_selection_publish(s->selection,&sel,v); ++s->revision; dirty(v);
                s->diagnostics.metadata_bytes=metadata(s,NULL,NULL);
            }
        }
    }
    os64_ui_selection_dispose(&sel);
    return rc;
}
os64_ui_status_code_t os64_ui_dataview_command(os64_ui_dataview_t *v,os64_ui_selection_command_t cmd,uint64_t id,bool reveal)
{
    os64_ui_status_code_t rc=ready(v);
    if (!rc && !usable(v)) rc=OS64_UI_REFUSED;
    if (!rc) {
        vs(v)->busy=true; rc=command_impl(v,cmd,id,reveal); vs(v)->busy=false;
        if (!rc) { clear_click(vs(v)); clear_prefix(vs(v)); }
    }
    return result(v,OS64_UI_DATAVIEW_COMMAND,rc);
}
static bool inside(os64_gui_rect_t r,int32_t x,int32_t y)
{ return r.w>0 && r.h>0 && x>=r.x && (int64_t)x< (int64_t)r.x+r.w && y>=r.y && (int64_t)y<(int64_t)r.y+r.h; }
static uint64_t row_at(os64_ui_dataview_t *v,int32_t x,int32_t y)
{
    if (!usable(v)) return 0;
    view_state *s=vs(v); view_frame *f=s->live;
    if (!inside(f->body,x,y)) return 0;
    return os64_ui_data_projection_id(projection(s),f->top+(size_t)((y-f->body.y)/f->pitch));
}
uint32_t os64_ui_dataview_header_at(const os64_ui_dataview_t *v,int32_t x,int32_t y)
{
    if (os64_ui_dataview_blocked(v)) return 0;
    view_frame *f=vs(v)->live;
    if (!inside(f->header,x,y)) return 0;
    int64_t start=(int64_t)f->header.x-f->left;
    for (size_t c=0;c<f->columns;++c) if (f->column[c].visible) {
        if (x>=start && x<start+f->column[c].width) return f->column[c].id;
        start+=f->column[c].width;
    }
    return 0;
}
os64_ui_status_code_t os64_ui_dataview_context_target(os64_ui_dataview_t *v,const os64_gui_event_t *ev,uint64_t *out)
{
    if (out) *out=0;
    os64_ui_status_code_t rc=ready(v);
    if (rc) return result(v,OS64_UI_DATAVIEW_CONTEXT,rc);
    if (!ev || !out) return result(v,OS64_UI_DATAVIEW_CONTEXT,OS64_UI_INVALID);
    if (!usable(v)) return result(v,OS64_UI_DATAVIEW_CONTEXT,OS64_UI_REFUSED);
    view_state *s=vs(v); end_gesture(v);
    uint64_t id;
    if (ev->type==OS64_GUI_EVENT_KEY_DOWN) id=os64_ui_selection_focus(s->selection);
    else {
        if (!inside(s->live->body,ev->mouse.x,ev->mouse.y)) return result(v,OS64_UI_DATAVIEW_CONTEXT,OS64_UI_INVALID);
        id=row_at(v,ev->mouse.x,ev->mouse.y);
        s->busy=true;
        rc=command_impl(v,id?OS64_UI_SELECTION_CONTEXT:OS64_UI_SELECTION_CLEAR,id,false);
        s->busy=false;
        if (!rc) notify(v,OS64_UI_DATAVIEW_SELECTION|OS64_UI_DATAVIEW_FOCUS,0);
    }
    if (!rc) *out=id;
    return result(v,OS64_UI_DATAVIEW_CONTEXT,rc);
}
os64_ui_status_code_t os64_ui_dataview_clock(os64_ui_dataview_t *v,int64_t (*clock)(void *),void *user)
{
    os64_ui_status_code_t rc=ready(v);
    if (rc) return rc;
    vs(v)->clock=clock; vs(v)->clock_user=user; clear_prefix(vs(v)); clear_click(vs(v));
    vs(v)->diagnostics.clock_queried=false; vs(v)->diagnostics.clock_available=false;
    return OS64_UI_OK;
}
static int64_t now(view_state *s)
{
    int64_t t=s->clock?s->clock(s->clock_user):os64_micros();
    s->diagnostics.clock_queried=true;
    s->diagnostics.clock_available=t>=0;
    return t;
}
static unsigned char fold(unsigned char c) { return c>='A' && c<='Z'?c+32:c; }
static bool prefix_match(os64_ui_data_text_t text,const char *prefix,size_t n)
{
    if (n>text.length) return false;
    for (size_t i=0;i<n;++i) if (fold((unsigned char)text.bytes[i])!=fold((unsigned char)prefix[i])) return false;
    return true;
}
static os64_ui_status_code_t search(os64_ui_dataview_t *v,char ch,bool backspace)
{
    view_state *s=vs(v);
    char prefix[65]; os64_memcpy(prefix,s->prefix,sizeof(prefix));
    size_t n=s->prefix_length; int64_t t=now(s);
    bool fresh=t<0 || !n || t<s->prefix_time || t-s->prefix_time>=1000000;
    if (fresh) n=0;
    bool cycle=!backspace && n==1 && fold((unsigned char)prefix[0])==fold((unsigned char)ch);
    if (backspace) { if (n) --n; }
    else if (!cycle) { if (n==64) return OS64_UI_OK; prefix[n++]=ch; }
    prefix[n]=0;
    if (n) {
        os64_ui_data_projection_t *p=projection(s);
        size_t count=os64_ui_data_projection_count(p), at=0;
        bool focused=os64_ui_data_projection_position(p,os64_ui_selection_focus(s->selection),&at);
        size_t start=focused?(fresh||cycle?(at+1)%count:at):0;
        for (size_t i=0;i<count;++i) {
            uint64_t id=os64_ui_data_projection_id(p,(start+i)%count);
            const os64_ui_data_row_t *row=os64_ui_data_row(snapshot(s),id);
            if (prefix_match(row->search,prefix,n)) {
                s->busy=true; os64_ui_status_code_t rc=command_impl(v,OS64_UI_SELECTION_CHOOSE,id,true); s->busy=false;
                if (rc) return rc;
                clear_click(s);
                os64_memcpy(s->prefix,prefix,sizeof(prefix)); s->prefix_length=n; s->prefix_time=t;
                notify(v,OS64_UI_DATAVIEW_SELECTION|OS64_UI_DATAVIEW_FOCUS|OS64_UI_DATAVIEW_VIEWPORT,0);
                return OS64_UI_OK;
            }
        }
    }
    os64_memcpy(s->prefix,prefix,sizeof(prefix)); s->prefix_length=n; s->prefix_time=t;
    return OS64_UI_OK;
}
static os64_ui_selection_command_t choose_command(uint8_t mods,bool navigation)
{
    if (mods&OS64_GUI_MOD_SHIFT) return mods&OS64_GUI_MOD_CTRL?OS64_UI_SELECTION_UNION:OS64_UI_SELECTION_RANGE;
    if (mods&OS64_GUI_MOD_CTRL) return navigation?OS64_UI_SELECTION_FOCUS:OS64_UI_SELECTION_TOGGLE;
    return OS64_UI_SELECTION_CHOOSE;
}
static void user_command(os64_ui_dataview_t *v,os64_ui_selection_command_t cmd,uint64_t id,bool reveal)
{
    if (!os64_ui_dataview_command(v,cmd,id,reveal))
        notify(v,OS64_UI_DATAVIEW_SELECTION|OS64_UI_DATAVIEW_FOCUS|(reveal?OS64_UI_DATAVIEW_VIEWPORT:0),0);
}
static bool view_event(os64_ui_widget_t *w,os64_ui_t *ui,const os64_gui_event_t *ev)
{
    os64_ui_dataview_t *v=(void *)w; view_state *s=vs(v);
    if (!s || s->busy) return false;
    if (ev->type==OS64_GUI_EVENT_KEY_UP) {
        uint16_t physical=(uint16_t)(ev->key.scancode+1)|((ev->key.modifiers&OS64_GUI_MOD_HID)?256:0);
        if (physical==s->held_enter) s->held_enter=0;
        if (physical==s->held_space) s->held_space=0;
        return true;
    }
    if (!usable(v)) return false;
    view_frame *f=s->live;
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_WHEEL:
        if (ui->grab) return true;
        if (!ev->mouse.dy && !ev->mouse.dx) return false;
        if (!os64_ui_dataview_scroll(v,ui_scroll_rows(f->top,(int32_t)ev->mouse.dy*3),(int64_t)f->left+(int64_t)ev->mouse.dx*32)) notify(v,OS64_UI_DATAVIEW_VIEWPORT,0);
        return true;
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev->mouse.button!=OS64_GUI_MOUSE_LEFT) return false;
        os64_ui_set_focus(ui,w);
        s->modifiers=ev->mouse.modifiers; s->pointer=true; s->moved=false; w->pressed=true;
        s->press_x=ev->mouse.x; s->press_y=ev->mouse.y; s->pressed_revision=s->revision;
        s->pressed_id=row_at(v,ev->mouse.x,ev->mouse.y);
        s->pressed_header=os64_ui_dataview_header_at(v,ev->mouse.x,ev->mouse.y);
        int32_t x=f->header.x-f->left;
        if (inside(f->header,ev->mouse.x,ev->mouse.y)) for (size_t c=0;c<f->columns;++c) if (f->column[c].visible) {
            x+=(int32_t)f->column[c].width;
            if (ev->mouse.x>=x-4 && ev->mouse.x<=x+4) { s->resize_column=f->column[c].id; s->guide_x=x; break; }
        }
        return true;
    }
    case OS64_GUI_EVENT_MOUSE_MOVE: {
        if (!s->pointer) return false;
        int64_t dx=(int64_t)ev->mouse.x-s->press_x, dy=(int64_t)ev->mouse.y-s->press_y;
        if (dx>4 || dx< -4 || dy>4 || dy< -4) { s->moved=true; clear_click(s); }
        if (s->resize_column) {
            int32_t start=f->header.x-f->left;
            for (size_t c=0;c<f->columns;++c) if (f->column[c].visible) {
                if (f->column[c].id==s->resize_column) {
                    int64_t width=(int64_t)f->column[c].width+dx;
                    if (width<48) width=48;
                    if (width>4096) width=4096;
                    s->guide_x=start+(int32_t)width; break;
                }
                start+=(int32_t)f->column[c].width;
            }
            dirty(v);
        }
        return true;
    }
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
        if (ev->mouse.button!=OS64_GUI_MOUSE_LEFT || !s->pointer) return false;
        s->pointer=false; w->pressed=false;
        if (s->pressed_revision!=s->revision) { view_cancel(w); return true; }
        uint32_t resize=s->resize_column; s->resize_column=0;
        if (resize) {
            for (size_t c=0;c<f->columns;++c) if (f->column[c].id==resize) {
                int64_t width=(int64_t)f->column[c].width+ev->mouse.x-s->press_x;
                if (width<48) width=48;
                if (width>4096) width=4096;
                if (!os64_ui_dataview_column_set(v,resize,(uint32_t)width,true,c)) notify(v,OS64_UI_DATAVIEW_COLUMNS|OS64_UI_DATAVIEW_VIEWPORT,0);
                break;
            }
            return true;
        }
        if (s->moved || (int64_t)ev->mouse.x-s->press_x>4 || (int64_t)ev->mouse.x-s->press_x< -4 ||
            (int64_t)ev->mouse.y-s->press_y>4 || (int64_t)ev->mouse.y-s->press_y< -4) { clear_click(s); return true; }
        if (s->pressed_header) {
            uint32_t col=s->pressed_header;
            if (os64_ui_dataview_header_at(v,ev->mouse.x,ev->mouse.y)==col) {
                os64_ui_data_sort_t order=os64_ui_data_projection_order(projection(s));
                order=(os64_ui_data_sort_t){col,order.column==col?!order.descending:false};
                for (size_t c=0;c<os64_ui_data_columns(snapshot(s));++c)
                    if (os64_ui_data_column(snapshot(s),c)->id==col && os64_ui_data_column(snapshot(s),c)->sortable) {
                        if (!os64_ui_dataview_sort(v,order)) notify(v,OS64_UI_DATAVIEW_MODEL|OS64_UI_DATAVIEW_VIEWPORT,0);
                    }
            }
            clear_click(s); return true;
        }
        uint64_t id=row_at(v,ev->mouse.x,ev->mouse.y);
        if (id!=s->pressed_id || !inside(f->body,ev->mouse.x,ev->mouse.y)) { clear_click(s); return true; }
        uint8_t mods=s->modifiers & (OS64_GUI_MOD_CTRL|OS64_GUI_MOD_SHIFT|OS64_GUI_MOD_ALT);
        if (mods&OS64_GUI_MOD_ALT) return true;
        if (!id && mods) return true;
        int64_t t=now(s); bool activate=id && !mods && s->last_click==id && t>=0 && t>=s->click_time && t-s->click_time<=500000;
        os64_ui_status_code_t rc=os64_ui_dataview_command(v,id?choose_command(mods,false):OS64_UI_SELECTION_CLEAR,id,false);
        if (!rc) {
            if (!mods && id && !activate) { s->last_click=id; s->click_time=t; }
            notify(v,OS64_UI_DATAVIEW_SELECTION|OS64_UI_DATAVIEW_FOCUS,activate?id:0);
        }
        return true;
    }
    case OS64_GUI_EVENT_KEY_DOWN: {
        if (os64_ui_key_is_esc(ev)) { os64_ui_cancel_gestures(ui); return true; }
        if (ui->grab) return false;
        bool hid=(ev->key.modifiers&OS64_GUI_MOD_HID)!=0;
        if (ev->key.scancode==(hid?0x3b:0x3c)) { s->seq=0; return false; }
        if (ev->key.modifiers&OS64_GUI_MOD_ALT) { s->seq=0; return false; }
        char ch=0; ui_key_t key=os64_ui_decode_key(&s->seq,ev,&ch);
        uint8_t chord=ev->key.modifiers&(OS64_GUI_MOD_CTRL|OS64_GUI_MOD_SHIFT|OS64_GUI_MOD_ALT);
        if (key==K_NONE && !ev->key.ascii) {
            const uint16_t named[]={OS64_UI_KEY_UP,OS64_UI_KEY_DOWN,OS64_UI_KEY_LEFT,OS64_UI_KEY_RIGHT,
                OS64_UI_KEY_HOME,OS64_UI_KEY_END,OS64_UI_KEY_PAGE_UP,OS64_UI_KEY_PAGE_DOWN};
            const ui_key_t decoded[]={K_UP,K_DOWN,K_LEFT,K_RIGHT,K_HOME,K_END,K_PGUP,K_PGDN};
            for (size_t i=0;i<8;++i)
                if (os64_ui_shortcut_match((os64_ui_shortcut_t){named[i],chord},ev)) { key=decoded[i]; break; }
        }
        /* Physical burst stamps reject orphaned bytes after input cancellation,
         * while printable keypad input keeps the shared shortcut rules. */
        if (key==K_CHAR && !os64_ui_shortcut_match((os64_ui_shortcut_t){(unsigned char)ch,chord},ev)) key=K_NONE;
        size_t count=os64_ui_data_projection_count(projection(s));
        uint64_t focus=os64_ui_selection_focus(s->selection);
        uint8_t mods=ev->key.modifiers;
        uint16_t physical=(uint16_t)(ev->key.scancode+1)|((mods&OS64_GUI_MOD_HID)?256:0);
        if ((mods&OS64_GUI_MOD_CTRL) && !s->seq &&
            ((ev->key.ascii==1) || (key==K_CHAR && fold((unsigned char)ch)=='a'))) {
            clear_prefix(s); if (count) user_command(v,OS64_UI_SELECTION_ALL,0,false); return true;
        }
        if (key==K_ENTER) {
            if (s->held_enter) return true;
            s->held_enter=physical; clear_prefix(s); clear_click(s);
            if (focus) notify(v,0,focus);
            return true;
        }
        if (key==K_CHAR && ch==' ') {
            if (s->held_space) return true;
            s->held_space=physical; clear_prefix(s); clear_click(s);
            if (focus) user_command(v,choose_command(mods,false),focus,false);
            return true;
        }
        if (key==K_LEFT || key==K_RIGHT) {
            clear_prefix(s);
            if (!os64_ui_dataview_scroll(v,f->top,(int64_t)f->left+(key==K_LEFT?-32:32))) notify(v,OS64_UI_DATAVIEW_VIEWPORT,0);
            return true;
        }
        if (key==K_UP || key==K_DOWN || key==K_HOME || key==K_END || key==K_PGUP || key==K_PGDN) {
            clear_prefix(s); clear_click(s);
            if (!count) return true;
            size_t at=0; bool has=os64_ui_data_projection_position(projection(s),focus,&at);
            size_t step=f->full?f->full:1;
            if (key==K_HOME) at=0;
            else if (key==K_END) at=count-1;
            else if (!has) at=key==K_UP || key==K_PGUP?count-1:0;
            else if (key==K_UP || key==K_PGUP) { size_t n=key==K_UP?1:step; at=at>n?at-n:0; }
            else { size_t n=key==K_DOWN?1:step; at=n>count-1-at?count-1:at+n; }
            user_command(v,choose_command(mods,true),os64_ui_data_projection_id(projection(s),at),true);
            return true;
        }
        if (!(mods&OS64_GUI_MOD_CTRL) && (key==K_BACKSPACE || (key==K_CHAR && ch>' '))) {
            result(v,OS64_UI_DATAVIEW_SEARCH,search(v,ch,key==K_BACKSPACE)); return true;
        }
        return key==K_NONE;
    }
    default: return false;
    }
}
static void clipped_icon(os64_gui_surface_t *surface,os64_gui_rect_t slot,os64_gui_rect_t clip,const os64_ui_icon_lease_t *lease)
{
    os64_gui_rect_t r;
    if (!os64_rect_intersect(clip,(os64_gui_rect_t){0,0,(int32_t)surface->width,(int32_t)surface->height},&r)) return;
    os64_gui_surface_t sub=*surface;
    sub.pixels+=r.y*surface->pitch_px+r.x; sub.width=(uint32_t)r.w; sub.height=(uint32_t)r.h;
    slot.x-=r.x; slot.y-=r.y;
    os64_ui_icon_draw(&sub,slot,lease,false);
}
static void view_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_ui_dataview_t *v=(void *)w; view_state *s=vs(v);
    if (s) s->diagnostics.painted_rows=0;
    if (w->bounds.w<=0 || w->bounds.h<=0) return;
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->field_bg);
    os64_draw_rect(&ctx->surf,w->bounds,t->field_border);
    if (!s) return;
    if (!usable(v)) return;
    view_frame *f=s->live;
    os64_ui_data_t *d=snapshot(s);
    os64_ui_data_sort_t order=os64_ui_data_projection_order(projection(s));
    for (size_t r=0;r<=f->shown;++r) {
        uint64_t id=r?os64_ui_data_projection_id(projection(s),f->top+r-1):0;
        const os64_ui_data_row_t *row=r?os64_ui_data_row(d,id):NULL;
        os64_gui_rect_t area=r?(os64_gui_rect_t){f->body.x,f->body.y+(int32_t)(r-1)*f->pitch,f->body.w,f->pitch}:f->header;
        os64_gui_rect_t clip;
        if (!os64_rect_intersect(area,r?f->body:f->header,&clip)) continue;
        bool selected=r && os64_ui_selection_contains(s->selection,id);
        uint32_t bg=r?(selected?t->text_sel_bg:t->field_bg):t->button_face;
        uint32_t fg=r?(selected?t->text_sel_fg:t->field_fg):t->button_fg;
        os64_draw_fill_rect(&ctx->surf,clip,bg);
        if (r) ++s->diagnostics.painted_rows;
        int32_t x=area.x-f->left;
        for (size_t c=0;c<f->columns;++c) {
            if (!f->column[c].visible) continue;
            os64_gui_rect_t cell={x,area.y,(int32_t)f->column[c].width,area.h};
            x+=cell.w;
            os64_gui_rect_t cell_clip;
            if (!os64_rect_intersect(cell,clip,&cell_clip)) continue;
            size_t schema=0;
            while (os64_ui_data_column(d,schema)->id!=f->column[c].id) ++schema;
            const os64_ui_data_column_t *col=os64_ui_data_column(d,schema);
            os64_ui_data_text_t text=row?row->cells[schema].display:col->title;
            size_t slot=r*f->columns+c;
            int32_t text_x=cell.x+6;
            if (r && col->search) {
                os64_gui_rect_t icon={cell.x+4,area.y+4,f->icon_size,f->icon_size};
                clipped_icon(&ctx->surf,icon,cell_clip,&f->icons[r-1]); text_x+=f->icon_size+6;
            } else if (r && col->right) text_x=cell.x+cell.w-6-f->advances[slot];
            os64_ui_draw_run(w->ui,f->runs[slot],ui_role(w,OS64_FONT_ROLE_UI),&ctx->surf,cell_clip,
                text_x,area.y+4,text.bytes,text.length,fg,bg);
            if (!r && order.column==col->id) {
                for (int32_t a=0;a<4;++a) {
                    os64_gui_rect_t mark={cell.x+cell.w-14+a,area.y+7+(order.descending?a:3-a),7-2*a,1}, clipped;
                    if (os64_rect_intersect(mark,cell_clip,&clipped)) os64_draw_fill_rect(&ctx->surf,clipped,fg);
                }
            }
        }
        if (r && w->focused && id==os64_ui_selection_focus(s->selection)) os64_draw_rect(&ctx->surf,clip,t->focus_ring);
    }
    if (s->resize_column && s->pointer) {
        os64_gui_rect_t guide={s->guide_x,w->bounds.y+1,1,w->bounds.h>2?w->bounds.h-2:0}, clipped;
        if (os64_rect_intersect(guide,w->bounds,&clipped)) os64_draw_fill_rect(&ctx->surf,clipped,t->focus_ring);
    }
}
static os64_font_status_t view_prepare(os64_ui_widget_t *w,os64_ui_t *ui)
{
    (void)ui; os64_ui_dataview_t *v=(void *)w; view_state *s=vs(v);
    if (!s) return OS64_FONT_OK;
    os64_ui_status_code_t rc=ready(v);
    if (rc) return to_font(rc);
    s->busy=true;
    rc=prepare(v,s->selection,s->live,os64_ui_widget_planned_bounds(w),os64_ui_dataview_top(v),os64_ui_dataview_left(v),&s->staged);
    s->busy=false;
    if (!rc) s->staged_revision=s->revision;
    result(v,OS64_UI_DATAVIEW_FONT,rc);
    return to_font(rc);
}
static void view_commit(os64_ui_widget_t *w)
{
    os64_ui_dataview_t *v=(void *)w; view_state *s=vs(v);
    if (!s || !s->staged) return;
    view_frame *f=s->staged; s->staged=NULL;
    if (s->staged_revision!=s->revision) { frame_free(s,f); block(v); return; }
    s->busy=true; publish(v,f,NULL,true); s->busy=false;
}
static void view_discard(os64_ui_widget_t *w)
{
    view_state *s=vs((void *)w);
    if (s) { s->busy=true; frame_free(s,s->staged); s->staged=NULL; s->busy=false; }
}
static void view_destroy(os64_ui_widget_t *w) { (void)os64_ui_dataview_dispose((void *)w); }
static void view_reface(os64_ui_widget_t *w)
{
    os64_ui_dataview_t *v=(void *)w; view_state *s=vs(v);
    if (!s) return;
    s->busy=true;
    if (s->live) {
        for (size_t i=0;i<s->live->slots;++i) {
            os64_ui_run_release(s->live->runs[i]); s->live->runs[i]=NULL;
        }
        for (size_t i=0;i<s->live->shown;++i) os64_ui_icon_release(s->provider,&s->live->icons[i]);
    }
    frame_free(s,s->staged); s->staged=NULL; s->busy=false;
    s->diagnostics.runs=s->diagnostics.icon_leases=0;
    block(v);
}
static void bar_scroll(os64_ui_scrollbar_t *bar,void *user)
{
    os64_ui_dataview_t *v=user; view_state *s=vs(v);
    if (!s || !usable(v)) { sync_bars(v); return; }
    size_t top=bar==&v->vertical?(size_t)bar->pos:s->live->top;
    int64_t left=bar==&v->horizontal?bar->pos:s->live->left;
    if (!os64_ui_dataview_scroll(v,top,left)) notify(v,OS64_UI_DATAVIEW_VIEWPORT,0);
    sync_bars(v);
}
const os64_ui_class_t os64_ui_dataview_class={"details",view_paint,view_event,view_cancel,
    view_prepare,view_commit,view_discard,view_destroy,NULL,view_reface,false};
void os64_ui_dataview_init(os64_ui_dataview_t *v)
{
    *v=(os64_ui_dataview_t){0}; v->w.cls=&os64_ui_dataview_class; v->w.focusable=true;
    os64_ui_scrollbar(&v->vertical,bar_scroll,v); os64_ui_scrollbar(&v->horizontal,bar_scroll,v);
    v->horizontal.horizontal=true; v->vertical.w.hidden=v->horizontal.w.hidden=true;
    os64_ui_add_child(&v->w,&v->vertical.w); os64_ui_add_child(&v->w,&v->horizontal.w);
}
