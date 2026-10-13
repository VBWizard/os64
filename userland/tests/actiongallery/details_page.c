/* Synthetic Details diagnostic. Filesystem identity and commands belong to
 * Roam's producer; this page deliberately keeps its rows in memory. */
#include "os64/ui_dataview.h"
#include "os64/ui_action.h"
#include "os64/ui_menu.h"
#include "os64/ui_status.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "image/icon_theme.h"
#include "details_page.h"

enum { D_EMPTY=1,D_ONE,D_TENK,D_MAX,D_FILTER,D_REFRESH,D_DOMAIN,D_SECOND,
    D_RETRY,D_DIAGNOSTICS,D_COLUMNS,D_LEFT,D_RIGHT,D_TARGET,D_CLOSE,D_SHOW=100,D_MOVE_LEFT=110,D_MOVE_RIGHT=120 };
typedef struct {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root, heading;
    os64_ui_menubar_t bar;
    os64_ui_status_t status, diagnostics;
    os64_ui_dataview_t views[2];
    os64_ui_selection_t selections[2];
    os64_ui_data_t *data[2];
    os64_icon_theme_t *icons;
    os64_ui_actions_t *actions;
    os64_ui_action_view_t *action_view;
    os64_ui_menu_model_t *models[6];
    os64_ui_menu_session_t *menus;
    os64_ui_dispatcher_t dispatcher;
    os64_ui_shortcut_router_t shortcuts;
    uint64_t notice_token, domain[2], next_domain, last_activation, context_id, context_domain;
    uint32_t context_column;
    unsigned active, refresh[2];
    bool second, filtered[2], report_pending, error_pending;
    os64_ui_status_code_t pending_error;
    size_t error_rows;
    int64_t operation_time, event_start, dispatch_start, samples[100];
    size_t sample_count, ordinary_events;
} details_page;
static os64_ui_data_text_t dt(const char *text)
{ size_t n=0; while (text[n]) ++n; return (os64_ui_data_text_t){text,n}; }
/* Source buffers are released immediately after the toolkit copies them. */
static os64_ui_status_code_t details_fixture(size_t count,uint64_t domain,unsigned refresh,
    uint64_t remove_a,uint64_t remove_b,os64_ui_data_t **out)
{
    *out=NULL;
    if (count>OS64_UI_DATA_ROWS) return OS64_UI_LIMIT;
    os64_ui_data_column_t columns[]={
        {.id=1,.title={"Name",4},.type=OS64_UI_DATA_TEXT_KEY,.width=260,.sortable=true,.required=true,.search=true},
        {.id=2,.title={"Size",4},.type=OS64_UI_DATA_UNSIGNED,.width=100,.sortable=true,.right=true},
        {.id=3,.title={"Type",4},.type=OS64_UI_DATA_TEXT_KEY,.width=140,.sortable=true},
        {.id=4,.title={"Modified",8},.type=OS64_UI_DATA_SIGNED,.width=180,.sortable=true}
    };
    typedef struct { os64_ui_data_row_t row; os64_ui_data_cell_t cells[4]; char name[180],size[32],date[40]; } source_row;
    source_row *source=count?os64_malloc(count*sizeof(*source)):NULL;
    os64_ui_data_row_t *rows=count?os64_malloc(count*sizeof(*rows)):NULL;
    if (count && (!source || !rows)) { os64_free(source); os64_free(rows); return OS64_UI_NO_MEMORY; }
    for (size_t i=0;i<count;++i) {
        source_row *r=&source[i]; *r=(source_row){0};
        bool folder=i%9==0;
        static const char *prefixes[]={"Alpha","Alpine","Beta","Bravo","Caf\xc3\xa9","Delta","Echo","\xe6\x96\x87\xe4\xbb\xb6"};
        os64_snprintf(r->name,sizeof(r->name),"%s %06lu%s",prefixes[i%8],(unsigned long)i,
            i%13==0?" — a deliberately long synthetic label that clips inside its column":"");
        uint64_t size=i==1?10:i==2?2:i*971;
        os64_snprintf(r->size,sizeof(r->size),"%lu bytes",(unsigned long)size);
        os64_snprintf(r->date,sizeof(r->date),"2026-10-%02u 09:%02u",1+(unsigned)(i%28),(unsigned)(i%60));
        const char *type=folder?"Folder":i%2?"Text document":"Data file";
        uint64_t id=UINT64_C(9000000000)+i*17;
        if (refresh && (i%5==0 || id==remove_a || id==remove_b)) id=UINT64_C(100000000000)+(uint64_t)refresh*100001+i;
        r->row=(os64_ui_data_row_t){id,folder?0:1,dt(r->name),dt(folder?"folder":"file"),r->cells};
        r->cells[0]=(os64_ui_data_cell_t){dt(r->name),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text=dt(r->name)}};
        r->cells[1]=(os64_ui_data_cell_t){dt(folder?"":r->size),{.present=!folder && i%17!=0,.type=OS64_UI_DATA_UNSIGNED,.value.u=size}};
        r->cells[2]=(os64_ui_data_cell_t){dt(type),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text=dt(type)}};
        r->cells[3]=(os64_ui_data_cell_t){dt(i%11==0?"":r->date),{.present=i%11!=0,.type=OS64_UI_DATA_SIGNED,.value.i=(int64_t)i-20000}};
        rows[i]=r->row;
    }
    os64_ui_status_code_t rc=os64_ui_data_create(domain,columns,4,rows,count,out);
    os64_free(rows); os64_free(source);
    return rc;
}
static void details_report(details_page *a)
{
    os64_ui_dataview_t *v=&a->views[a->active];
    os64_ui_selection_t *sel=&a->selections[a->active];
    os64_ui_dataview_diagnostics_t d=os64_ui_dataview_diagnostics(v);
    char text[360];
    os64_snprintf(text,sizeof(text),"Selected %lu | Focus %lu | Anchor %lu | Act %lu%s",
        (unsigned long)os64_ui_selection_count(sel),(unsigned long)os64_ui_selection_focus(sel),
        (unsigned long)os64_ui_selection_anchor(sel),(unsigned long)a->last_activation,
        os64_ui_dataview_blocked(v)?"   LAYOUT BLOCKED":"");
    (void)os64_ui_status_base(&a->status,text);
    os64_snprintf(text,sizeof(text),"Rows %lu View %u Top %lu Left %ld Metadata %lu/%lu Runs %lu Icons %lu (%lu missing)%s",
        (unsigned long)os64_ui_data_projection_count(os64_ui_selection_projection(sel)),a->active+1,(unsigned long)os64_ui_dataview_top(v),(long)os64_ui_dataview_left(v),
        (unsigned long)d.metadata_bytes,(unsigned long)d.peak_metadata_bytes,(unsigned long)d.runs,
        (unsigned long)d.icon_leases,(unsigned long)d.icon_failures,d.clock_available?"":"   clock unavailable/unqueried");
    (void)os64_ui_status_base(&a->diagnostics,text);
}
static void details_error(void *user,os64_ui_status_code_t rc)
{
    details_page *a=user; char message[120];
    os64_snprintf(message,sizeof(message),"Last failure (%u): kept %lu projection rows",rc,
        (unsigned long)os64_ui_data_projection_count(os64_ui_selection_projection(os64_ui_dataview_selection(&a->views[a->active]))));
    (void)os64_ui_status_notice(&a->status,message,&a->notice_token);
    os64_printf("Details: %s\n",message);
}
static void details_view_error(os64_ui_dataview_t *v,os64_ui_dataview_operation_t op,os64_ui_status_code_t rc,void *user)
{
    (void)op; details_page *a=user;
    /* Font preparation calls this while measuring the candidate face. Queue
     * status text until the dispatch boundary so it uses committed fonts. */
    a->pending_error=rc; a->error_pending=true; a->report_pending=true;
    a->error_rows=os64_ui_data_projection_count(os64_ui_selection_projection(os64_ui_dataview_selection(v)));
}
static void details_changed(os64_ui_dataview_t *v,const os64_ui_dataview_change_t *change,void *user)
{
    details_page *a=user; a->active=v==&a->views[1]?1:0;
    if (change->activated) {
        a->last_activation=change->activated;
        os64_printf("Details activation domain=%lu ID=%lu (synthetic; no file action)\n",(unsigned long)change->domain,(unsigned long)change->activated);
    }
    if ((change->flags&OS64_UI_DATAVIEW_MODEL) && a->dispatch_start>=0)
        os64_printf("Details native sort/publication: elapsed-us=%ld\n",(long)(os64_micros()-a->dispatch_start));
    a->report_pending=true;
}
static void details_layout(details_page *a,bool staged)
{
    int32_t line=os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI);
    int32_t width=a->root.bounds.w>12?a->root.bounds.w-12:0;
    int32_t y=6;
    os64_gui_rect_t bar={6,y,width,line+6}; y+=line+12;
    os64_gui_rect_t heading={6,y,width,line}; y+=line+6;
    int32_t height=a->root.bounds.h>y+2*(line+8)+12?a->root.bounds.h-y-2*(line+8)-12:0;
    int32_t first=a->second?(width>6?(width-6)/2:0):width;
    os64_gui_rect_t view[2]={{6,y,first,height},{12+first,y,a->second?first:0,height}};
    y+=height+6;
    os64_gui_rect_t status={6,y,width,line+8}; y+=line+8;
    os64_gui_rect_t diagnostic={6,y,width,line+8};
    os64_ui_widget_t *nodes[]={&a->bar.w,&a->heading,&a->views[0].w,&a->views[1].w,&a->status.w,&a->diagnostics.w};
    os64_gui_rect_t rects[]={bar,heading,view[0],view[1],status,diagnostic};
    for (size_t i=0;i<6;++i) {
        if (staged) os64_ui_widget_stage_bounds(nodes[i],rects[i]); else nodes[i]->bounds=rects[i];
    }
    if (!staged) {
        for (unsigned i=0;i<(a->second?2u:1u);++i)
            if (a->views[i].state) (void)os64_ui_dataview_layout(&a->views[i]);
    }
}
static os64_font_status_t details_plan(os64_ui_t *ui,void *user,void **out)
{ (void)ui; *out=NULL; details_layout(user,true); return OS64_FONT_OK; }
static void details_plan_done(os64_ui_t *ui,void *user,void *plan)
{ (void)ui; (void)plan; ((details_page *)user)->report_pending=true; }
static void details_resize(os64_ui_t *ui)
{
    details_page *a=ui->root->user; details_layout(a,false); details_report(a);
}
static void details_menu_layout(void *user)
{ details_page *a=user; details_layout(a,false); os64_ui_mark_dirty(&a->ui,&a->root); }
static os64_ui_status_code_t details_projection(os64_ui_data_t *d,bool filtered,os64_ui_data_sort_t order,os64_ui_data_projection_t **out)
{
    *out=NULL; os64_ui_data_projection_t *p=NULL;
    os64_ui_status_code_t rc;
    if (filtered) {
        size_t count=os64_ui_data_count(d), n=0;
        uint64_t *ids=count?os64_malloc(count*sizeof(*ids)):NULL;
        if (count && !ids) return OS64_UI_NO_MEMORY;
        for (size_t i=0;i<count;++i) if (i%3==0) ids[n++]=os64_ui_data_row_at(d,i)->id;
        rc=os64_ui_data_projection_create(d,ids,n,&p); os64_free(ids);
    } else rc=os64_ui_data_projection_identity(d,&p);
    if (!rc && order.column) {
        rc=os64_ui_data_projection_sort(p,order,out); os64_ui_data_projection_release(p);
    } else if (!rc) *out=p;
    return rc;
}
static os64_ui_status_code_t details_replace(details_page *a,size_t count,bool reset,bool refresh)
{
    os64_ui_dataview_t *v=&a->views[a->active];
    os64_ui_selection_t *sel=&a->selections[a->active];
    os64_ui_data_t *d=NULL; os64_ui_data_projection_t *p=NULL;
    if (reset && a->next_domain==UINT64_MAX) return OS64_UI_LIMIT;
    uint64_t domain=reset?a->next_domain+1:a->domain[a->active];
    unsigned generation=refresh?a->refresh[a->active]+1:0;
    int64_t before=os64_micros();
    os64_ui_status_code_t rc=details_fixture(count,domain,generation,
        refresh?os64_ui_selection_focus(sel):0,refresh?os64_ui_selection_anchor(sel):0,&d);
    if (!rc) rc=details_projection(d,a->filtered[a->active] && !reset,reset?(os64_ui_data_sort_t){0}:os64_ui_data_projection_order(os64_ui_selection_projection(sel)),&p);
    if (!rc) rc=os64_ui_dataview_bind(v,p,reset?OS64_UI_DATA_RESET:OS64_UI_DATA_PRESERVE);
    if (!rc) {
        os64_ui_data_release(a->data[a->active]); a->data[a->active]=d; d=NULL; a->domain[a->active]=domain; a->refresh[a->active]=generation;
        if (reset) { a->filtered[a->active]=false; a->next_domain=domain; }
        a->operation_time=os64_micros()-before;
        os64_printf("Details construction/publication: rows=%lu snapshot-bytes=%lu elapsed-us=%ld\n",
            (unsigned long)count,(unsigned long)os64_ui_data_bytes(a->data[a->active]),(long)a->operation_time);
    }
    os64_ui_data_projection_release(p); os64_ui_data_release(d);
    return rc;
}
static bool details_context(os64_ui_widget_t *w,const os64_gui_event_t *ev,void *user)
{
    details_page *a=user; unsigned which=w==&a->views[1].w?1:0; a->active=which;
    os64_ui_dataview_t *v=&a->views[which];
    bool keyboard=ev->type==OS64_GUI_EVENT_KEY_DOWN;
    a->context_column=keyboard?0:os64_ui_dataview_header_at(v,ev->mouse.x,ev->mouse.y);
    os64_gui_rect_t anchor=keyboard?w->bounds:(os64_gui_rect_t){ev->mouse.x,ev->mouse.y,0,0};
    os64_ui_status_code_t rc=OS64_UI_OK;
    if (!a->context_column) {
        rc=os64_ui_dataview_context_target(v,ev,&a->context_id);
        a->context_domain=os64_ui_data_domain(os64_ui_data_projection_snapshot(os64_ui_selection_projection(&a->selections[which])));
    }
    if (!rc) rc=os64_ui_menu_open(a->menus,a->models[a->context_column?1:4],a->action_view,anchor,OS64_UI_MENU_CONTEXT);
    if (rc && rc!=OS64_UI_BUSY) details_error(a,rc);
    return true;
}
static bool details_retain(void *user,uint64_t generation) { (void)user; return generation==1; }
static void details_release(void *user,uint64_t generation) { (void)user; (void)generation; }
static bool details_valid(void *user,uint64_t generation) { (void)user; return generation==1; }
static void details_query(void *app,const os64_ui_action_context_t *context,uint32_t id,os64_ui_action_state_t *out)
{
    (void)context; details_page *a=app; out->enabled=true;
    os64_ui_dataview_t *v=&a->views[a->active];
    if (id==D_FILTER) out->checked=a->filtered[a->active];
    if (id==D_SECOND) out->checked=a->second;
    if (id==D_TARGET) out->enabled=a->context_id && !os64_ui_dataview_blocked(v) &&
        a->context_domain==os64_ui_data_domain(os64_ui_data_projection_snapshot(os64_ui_selection_projection(&a->selections[a->active]))) &&
        os64_ui_data_projection_position(os64_ui_selection_projection(&a->selections[a->active]),a->context_id,NULL);
    if (id==D_LEFT || id==D_RIGHT) out->enabled=false;
    for (size_t i=0;i<4;++i) {
        os64_ui_dataview_column_t col;
        if (!os64_ui_dataview_column(v,i,&col)) continue;
        if (id==D_MOVE_LEFT+col.id) out->enabled=i>0;
        if (id==D_MOVE_RIGHT+col.id) out->enabled=i<3;
        if (id==D_SHOW+col.id) { out->checked=col.visible; out->enabled=col.id!=1; }
        if (col.id==a->context_column && id==D_LEFT) out->enabled=i>0;
        if (col.id==a->context_column && id==D_RIGHT) out->enabled=i<3;
    }
}
static void details_performance(details_page *a)
{
    os64_ui_dataview_t *v=&a->views[a->active];
    os64_ui_dataview_diagnostics_t d=os64_ui_dataview_diagnostics(v);
    int64_t sorted[100];
    for (size_t i=0;i<a->sample_count;++i) {
        size_t at=i;
        while (at && sorted[at-1]>a->samples[i]) { sorted[at]=sorted[at-1]; --at; }
        sorted[at]=a->samples[i];
    }
    os64_printf("Details native diagnostics: snapshot=%lu metadata=%lu peak=%lu font-engine=%lu font-peak=%lu runs=%lu leases=%lu painted-rows=%lu prepared-rows=%lu construction-us=%ld\n",
        (unsigned long)os64_ui_data_bytes(a->data[a->active]),(unsigned long)d.metadata_bytes,(unsigned long)d.peak_metadata_bytes,
        (unsigned long)os64_ui_font_live_bytes(&a->ui),(unsigned long)os64_ui_font_peak_bytes(&a->ui),(unsigned long)d.runs,(unsigned long)d.icon_leases,
        (unsigned long)d.painted_rows,(unsigned long)d.prepared_rows,(long)a->operation_time);
    if (a->sample_count) os64_printf("Details ordinary native event-to-paint: samples=%lu after-10-warmups median-us=%ld p95-us=%ld max-us=%ld (environment must be identified by tester)\n",
        (unsigned long)a->sample_count,(long)sorted[a->sample_count/2],(long)sorted[(a->sample_count-1)*95/100],(long)sorted[a->sample_count-1]);
    os64_printf("Details shared icon pixel cache: %lu bytes; theme routing metadata and decoder scratch excluded.\n",(unsigned long)os64_icon_theme_cache_bytes(a->icons));
    details_report(a);
}
static os64_ui_action_result_t details_invoke(void *app,const os64_ui_action_context_t *context,uint32_t id)
{
    (void)context; details_page *a=app; os64_ui_dataview_t *v=&a->views[a->active];
    os64_ui_status_code_t rc=OS64_UI_OK;
    switch (id) {
    case D_EMPTY: case D_ONE: case D_TENK: case D_MAX:
        rc=details_replace(a,id==D_EMPTY?0:id==D_ONE?1:id==D_TENK?10000:100000,true,false); break;
    case D_REFRESH: rc=details_replace(a,os64_ui_data_count(a->data[a->active]),false,true); break;
    case D_DOMAIN: rc=details_replace(a,os64_ui_data_count(a->data[a->active]),true,false); break;
    case D_FILTER: {
        os64_ui_data_projection_t *p=NULL;
        rc=details_projection(a->data[a->active],!a->filtered[a->active],os64_ui_data_projection_order(os64_ui_selection_projection(&a->selections[a->active])),&p);
        if (!rc) rc=os64_ui_dataview_bind(v,p,OS64_UI_DATA_PRESERVE);
        if (!rc) a->filtered[a->active]=!a->filtered[a->active];
        os64_ui_data_projection_release(p); break;
    }
    case D_SECOND:
        if (a->second) {
            os64_ui_set_focus(&a->ui,&a->views[0].w);
            rc=os64_ui_dataview_dispose(&a->views[1]);
            if (!rc) { os64_ui_selection_dispose(&a->selections[1]); a->second=false; os64_ui_data_release(a->data[1]); a->data[1]=NULL; a->active=0; os64_ui_set_hidden(&a->ui,&a->views[1].w,true); }
        } else {
            rc=os64_ui_selection_bind(&a->selections[1],os64_ui_selection_projection(&a->selections[0]),OS64_UI_DATA_RESET);
            if (!rc) {
                a->second=true;
                a->data[1]=a->data[0]; os64_ui_data_retain(a->data[1]);
                a->domain[1]=a->domain[0]; a->refresh[1]=a->refresh[0]; a->filtered[1]=a->filtered[0];
                details_layout(a,false);
                rc=os64_ui_dataview_attach(&a->views[1],&a->selections[1],a->icons?os64_icon_theme_provider(a->icons):NULL);
                if (!rc) os64_ui_set_hidden(&a->ui,&a->views[1].w,false);
                else { a->second=false; os64_ui_selection_dispose(&a->selections[1]); os64_ui_data_release(a->data[1]); a->data[1]=NULL; }
            }
        }
        details_layout(a,false); break;
    case D_RETRY: rc=os64_ui_dataview_layout(v); break;
    case D_DIAGNOSTICS: details_performance(a); break;
    case D_COLUMNS:
        a->context_column=1;
        rc=os64_ui_menu_open(a->menus,a->models[1],a->action_view,a->bar.w.bounds,OS64_UI_MENU_CONTEXT); break;
    case D_LEFT: case D_RIGHT:
        for (size_t i=0;i<4;++i) {
            os64_ui_dataview_column_t col;
            if (os64_ui_dataview_column(v,i,&col) && col.id==a->context_column) {
                rc=os64_ui_dataview_column_set(v,col.id,col.width,col.visible,id==D_LEFT?i-1:i+1); break;
            }
        }
        break;
    case D_TARGET:
        os64_printf("Details context domain=%lu ID=%lu selected-count=%lu\n",(unsigned long)a->context_domain,(unsigned long)a->context_id,
            (unsigned long)os64_ui_selection_count(&a->selections[a->active])); break;
    case D_CLOSE: a->ui.quit=true; break;
    default:
        if ((id>D_MOVE_LEFT && id<=D_MOVE_LEFT+4) || (id>D_MOVE_RIGHT && id<=D_MOVE_RIGHT+4)) {
            bool right=id>D_MOVE_RIGHT;
            uint32_t column=id-(right?D_MOVE_RIGHT:D_MOVE_LEFT);
            for (size_t i=0;i<4;++i) {
                os64_ui_dataview_column_t col;
                if (os64_ui_dataview_column(v,i,&col) && col.id==column) {
                    rc=os64_ui_dataview_column_set(v,col.id,col.width,col.visible,right?i+1:i-1); break;
                }
            }
        } else if (id>D_SHOW && id<=D_SHOW+4) for (size_t i=0;i<4;++i) {
            os64_ui_dataview_column_t col;
            if (os64_ui_dataview_column(v,i,&col) && col.id==id-D_SHOW) {
                a->context_column=col.id;
                rc=os64_ui_dataview_column_set(v,col.id,col.width,!col.visible,i); break;
            }
        }
        else rc=OS64_UI_INVALID;
    }
    if (rc && rc!=OS64_UI_BUSY) details_error(a,rc);
    details_report(a);
    os64_printf("Details command=%u view=%u result=%u rows=%lu second=%u filter=%u\n",id,a->active+1,rc,
        (unsigned long)os64_ui_data_projection_count(os64_ui_selection_projection(&a->selections[a->active])),a->second,a->filtered[a->active]);
    return (os64_ui_action_result_t){rc,0};
}
static bool details_setup_error(const char *stage,unsigned status)
{
    os64_printf("Details setup refused: %s (result %u)\n",stage,status);
    return false;
}
static bool details_setup(details_page *a)
{
    a->domain[0]=a->next_domain=1;
    os64_ui_init(&a->ui,&a->draw); os64_ui_panel(&a->root); a->root.user=a;
    a->root.bounds=(os64_gui_rect_t){0,0,(int32_t)a->draw.surf.width,(int32_t)a->draw.surf.height};
    os64_ui_menubar_init(&a->bar); os64_ui_add_child(&a->root,&a->bar.w);
    os64_ui_label(&a->heading,"Details - synthetic data. Click/Ctrl/Shift; arrows; type a prefix; Enter/double click.");
    os64_ui_add_child(&a->root,&a->heading);
    os64_ui_status_init(&a->status); os64_ui_add_child(&a->root,&a->status.w);
    os64_ui_status_init(&a->diagnostics); os64_ui_add_child(&a->root,&a->diagnostics.w);
    for (unsigned i=0;i<2;++i) {
        os64_ui_dataview_init(&a->views[i]); os64_ui_add_child(&a->root,&a->views[i].w);
        a->views[i].on_change=details_changed; a->views[i].on_error=details_view_error; a->views[i].callback_user=a;
        a->views[i].w.on_context=details_context; a->views[i].w.context_user=a;
    }
    a->views[1].w.hidden=true;
    os64_ui_set_root(&a->ui,&a->root); a->ui.on_resize=details_resize;
    os64_font_status_t font_rc=os64_ui_font_planner(&a->ui,details_plan,details_plan_done,details_plan_done,a);
    if (font_rc) return details_setup_error("font planner",font_rc);
    details_layout(a,false);
    os64_ui_data_projection_t *p=NULL;
    int64_t construction_start=os64_micros();
    os64_ui_status_code_t rc=details_fixture(10000,1,0,0,0,&a->data[a->active]);
    if (rc) return details_setup_error("snapshot",rc);
    rc=os64_ui_data_projection_identity(a->data[a->active],&p);
    if (rc) return details_setup_error("projection",rc);
    rc=os64_ui_selection_bind(&a->selections[0],p,OS64_UI_DATA_RESET);
    os64_ui_data_projection_release(p);
    if (rc) return details_setup_error("selection",rc);
    rc=os64_ui_dataview_attach(&a->views[0],&a->selections[0],a->icons?os64_icon_theme_provider(a->icons):NULL);
    if (rc) return details_setup_error("Details attach",rc);
    a->operation_time=os64_micros()-construction_start;
    os64_ui_action_spec_t specs[]={
        {D_EMPTY,OS64_UI_ACTION_COMMAND,0,"0 rows","Empty fixture",NULL,0,{0,0}},
        {D_ONE,OS64_UI_ACTION_COMMAND,0,"1 row","One-row fixture",NULL,0,{0,0}},
        {D_TENK,OS64_UI_ACTION_COMMAND,0,"10,000 rows","Normal Details fixture",NULL,0,{0,0}},
        {D_MAX,OS64_UI_ACTION_COMMAND,0,"100,000 rows","Toolkit limit fixture",NULL,0,{0,0}},
        {D_FILTER,OS64_UI_ACTION_TOGGLE,0,"Subset filter","Keep source positions divisible by three",NULL,'f',{0,0}},
        {D_REFRESH,OS64_UI_ACTION_COMMAND,0,"Refresh / remove focus and anchor","Retain other matching IDs",NULL,'r',{0,0}},
        {D_DOMAIN,OS64_UI_ACTION_COMMAND,0,"New identity domain","Reuse numeric IDs with cleared state",NULL,'n',{0,0}},
        {D_SECOND,OS64_UI_ACTION_TOGGLE,0,"Second independent view","Share immutable rows",NULL,'s',{0,0}},
        {D_RETRY,OS64_UI_ACTION_COMMAND,0,"Retry Layout","Retry after layout refusal",NULL,'r',{0,0}},
        {D_DIAGNOSTICS,OS64_UI_ACTION_COMMAND,0,"Report timing and memory","Write measurements to stdout",NULL,'d',{0,0}},
        {D_COLUMNS,OS64_UI_ACTION_COMMAND,0,"Columns","Keyboard-accessible header menu",NULL,'c',{0,0}},
        {D_LEFT,OS64_UI_ACTION_COMMAND,0,"Move target column left","Column order belongs to this view",NULL,'l',{0,0}},
        {D_RIGHT,OS64_UI_ACTION_COMMAND,0,"Move target column right","Column order belongs to this view",NULL,'r',{0,0}},
        {D_TARGET,OS64_UI_ACTION_COMMAND,0,"Report context target IDs","No real files are opened",NULL,'t',{0,0}},
        {D_CLOSE,OS64_UI_ACTION_COMMAND,0,"Close","Close the diagnostic",NULL,'c',{'q',OS64_GUI_MOD_CTRL}},
        {D_MOVE_LEFT+1,OS64_UI_ACTION_COMMAND,0,"Move Name left","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_RIGHT+1,OS64_UI_ACTION_COMMAND,0,"Move Name right","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_LEFT+2,OS64_UI_ACTION_COMMAND,0,"Move Size left","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_RIGHT+2,OS64_UI_ACTION_COMMAND,0,"Move Size right","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_LEFT+3,OS64_UI_ACTION_COMMAND,0,"Move Type left","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_RIGHT+3,OS64_UI_ACTION_COMMAND,0,"Move Type right","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_LEFT+4,OS64_UI_ACTION_COMMAND,0,"Move Modified left","Reorder this column",NULL,0,{0,0}},
        {D_MOVE_RIGHT+4,OS64_UI_ACTION_COMMAND,0,"Move Modified right","Reorder this column",NULL,0,{0,0}},
        {D_SHOW+1,OS64_UI_ACTION_TOGGLE,0,"Name (required)","Required-visible column",NULL,'n',{0,0}},
        {D_SHOW+2,OS64_UI_ACTION_TOGGLE,0,"Size","Show or hide Size",NULL,'s',{0,0}},
        {D_SHOW+3,OS64_UI_ACTION_TOGGLE,0,"Type","Show or hide Type",NULL,'t',{0,0}},
        {D_SHOW+4,OS64_UI_ACTION_TOGGLE,0,"Modified","Show or hide Modified",NULL,'m',{0,0}}
    };
    const os64_ui_action_callbacks_t callbacks={details_query,details_invoke};
    rc=os64_ui_actions_create(specs,sizeof(specs)/sizeof(*specs),&callbacks,a,&a->actions);
    if (rc) return details_setup_error("actions",rc);
    os64_ui_action_context_t context={a,1,details_retain,details_release,details_valid};
    rc=os64_ui_action_view_create(a->actions,&context,&a->action_view);
    if (rc) return details_setup_error("action view",rc);
    os64_ui_menu_entry_t data[]={{.action=D_EMPTY},{.action=D_ONE},{.action=D_TENK},{.action=D_MAX},{.kind=OS64_UI_MENU_SEPARATOR},
        {.action=D_FILTER},{.action=D_REFRESH},{.action=D_DOMAIN},{.action=D_CLOSE}};
    os64_ui_menu_entry_t columns[]={{.action=D_SHOW+1},{.action=D_SHOW+2},{.action=D_SHOW+3},{.action=D_SHOW+4},
        {.kind=OS64_UI_MENU_SEPARATOR},{.action=D_LEFT},{.action=D_RIGHT},
        {.action=D_MOVE_LEFT+1},{.action=D_MOVE_RIGHT+1},{.action=D_MOVE_LEFT+2},{.action=D_MOVE_RIGHT+2},{.action=D_MOVE_LEFT+3},{.action=D_MOVE_RIGHT+3},{.action=D_MOVE_LEFT+4},{.action=D_MOVE_RIGHT+4}};
    os64_ui_menu_entry_t view[]={{.kind=OS64_UI_MENU_SUBMENU,.label="Columns",.mnemonic='c',.child=NULL},
        {.action=D_SECOND},{.action=D_RETRY},{.action=D_DIAGNOSTICS}};
    os64_ui_menu_entry_t context_menu[]={{.action=D_TARGET},{.action=D_COLUMNS},{.action=D_DIAGNOSTICS}};
    if ((rc=os64_ui_menu_model_create(a->actions,data,sizeof(data)/sizeof(*data),&a->models[0])) ||
        (rc=os64_ui_menu_model_create(a->actions,columns,sizeof(columns)/sizeof(*columns),&a->models[1])) ||
        (rc=os64_ui_menu_model_create(a->actions,context_menu,3,&a->models[4]))) return details_setup_error("menu models",rc);
    view[0].child=a->models[1];
    rc=os64_ui_menu_model_create(a->actions,view,4,&a->models[2]);
    if (rc) return details_setup_error("View menu",rc);
    os64_ui_menu_entry_t top[]={{.kind=OS64_UI_MENU_SUBMENU,.label="Data",.mnemonic='d',.child=a->models[0]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="View",.mnemonic='v',.child=a->models[2]}};
    rc=os64_ui_menu_model_create(a->actions,top,2,&a->models[3]);
    if (!rc) rc=os64_ui_menubar_bind(&a->bar,a->models[3]);
    if (rc) return details_setup_error("menu bar",rc);
    os64_ui_set_focus(&a->ui,&a->views[0].w);
    details_report(a); return true;
}
static bool details_cleanup(details_page *a)
{
    if (a->menus && os64_ui_menu_session_dispose(a->menus)) return false;
    a->menus=NULL;
    for (unsigned i=0;i<2;++i) { (void)os64_ui_dataview_dispose(&a->views[i]); (void)os64_ui_selection_dispose(&a->selections[i]); }
    for (unsigned i=0;i<2;++i) { os64_ui_data_release(a->data[i]); a->data[i]=NULL; }
    os64_ui_menubar_destroy(&a->bar);
    for (size_t i=0;i<6;++i) { os64_ui_menu_model_release(a->models[i]); a->models[i]=NULL; }
    if (a->action_view) { (void)os64_ui_action_view_destroy(a->action_view); a->action_view=NULL; }
    if (a->actions) { (void)os64_ui_actions_destroy(a->actions); a->actions=NULL; }
    if (os64_ui_font_release(&a->ui)) return false;
    if (a->icons && os64_icon_theme_destroy(a->icons)) return false;
    a->icons=NULL; return true;
}
static void details_commands(void *user,const os64_gui_event_t *event)
{
    details_page *a=user;
    if (a->ui.focus==&a->views[1].w) a->active=1;
    else if (a->ui.focus==&a->views[0].w) a->active=0;
    bool ordinary=event->type==OS64_GUI_EVENT_MOUSE_WHEEL;
    if (event->type==OS64_GUI_EVENT_KEY_DOWN) {
        /* Burst navigation is stamped with the physical arrow's code. */
        uint8_t sc=event->key.scancode;
        ordinary=(event->key.modifiers&OS64_GUI_MOD_HID)?sc>=0x4a && sc<=0x52:sc==0x48 || sc==0x50 || sc==0x47 || sc==0x4f || sc==0x49 || sc==0x51;
    }
    a->dispatch_start=os64_micros();
    a->event_start=ordinary && !os64_ui_menu_active(a->menus)?os64_micros():-1;
    /* The dispatcher has already routed this delivery through the menu. */
    if (!os64_ui_shortcut_route(&a->shortcuts,a->action_view,event,NULL)) (void)os64_ui_dispatch(&a->ui,event);
    if (!ordinary && event->type==OS64_GUI_EVENT_KEY_DOWN && event->key.ascii>' ' && event->key.ascii<127 &&
        !(event->key.modifiers&(OS64_GUI_MOD_CTRL|OS64_GUI_MOD_ALT)) && !os64_ui_menu_active(a->menus))
        os64_printf("Details native prefix input: ascii=%u elapsed-us=%ld focus=%lu status=%u\n",
            (unsigned char)event->key.ascii,(long)(os64_micros()-a->dispatch_start),
            (unsigned long)os64_ui_selection_focus(&a->selections[a->active]),os64_ui_dataview_status(&a->views[a->active],NULL));
}
static void details_boundary(void *user,bool queues_empty)
{
    (void)queues_empty; details_page *a=user;
    if (a->error_pending) {
        char message[120];
        os64_snprintf(message,sizeof(message),"Last failure (%u): kept %lu projection rows",a->pending_error,(unsigned long)a->error_rows);
        (void)os64_ui_status_notice(&a->status,message,&a->notice_token);
        os64_printf("Details: %s\n",message); a->error_pending=false;
    }
    if (a->report_pending) { details_report(a); a->report_pending=false; }
}
static void details_paint(void *user)
{
    details_page *a=user;
    (void)os64_ui_actions_refresh(a->action_view);
    os64_ui_paint(&a->ui);
    if (a->event_start>=0) {
        int64_t end=os64_micros();
        if (end>=a->event_start && ++a->ordinary_events>10 && a->sample_count<100)
            a->samples[a->sample_count++]=end-a->event_start;
    }
    a->event_start=-1;
}
int gallery_details_page(bool dark,bool icons)
{
    details_page a={0}; a.event_start=-1;
    os64_printf("Details gallery: reusable virtualized table, synthetic rows and E2 icons; no filesystem commands.\n");
    int64_t window=os64_gui_window_create("Details gallery",24,12,1000,720,0);
    if (window<0) { os64_printf("Details window creation refused (%ld)\n",(long)window); return 1; }
    os64_draw_ctx_init(&a.draw,window);
    if (icons) (void)os64_icon_theme_create(&a.icons);
    if (!details_setup(&a)) { (void)details_cleanup(&a); (void)os64_gui_window_destroy(window); return 1; }
    if (dark) os64_ui_theme_palette(&a.ui.theme,OS64_UI_PALETTE_MIDNIGHT);
    os64_ui_dispatcher_init(&a.dispatcher,NULL);
    os64_ui_status_code_t rc=os64_ui_dispatcher_add(&a.dispatcher,window,&a.ui,details_commands,details_paint,&a);
    if (!rc) rc=os64_ui_menu_session_create(&a.dispatcher,&a.ui,window,&a.bar,&a.status,
            a.icons?os64_icon_theme_provider(a.icons):NULL,&a.menus);
    if (rc) {
        (void)details_setup_error("dispatcher/menu session",rc);
        (void)details_cleanup(&a); (void)os64_gui_window_destroy(window); return 1;
    }
    a.dispatcher.entries[0].boundary=details_boundary;
    a.dispatcher.entries[0].boundary_user=&a;
    os64_ui_menu_hooks(a.menus,details_menu_layout,details_error,&a);
    os64_ui_menu_focus_fallback(a.menus,&a.views[0].w);
    (void)os64_ui_menu_bar_view(a.menus,a.action_view);
    (void)os64_ui_font_follow(&a.ui);
    details_layout(&a,false); details_boundary(&a,true); details_paint(&a);
    while (!a.ui.quit) if (os64_ui_dispatcher_turn(&a.dispatcher,true)<0) break;
    details_performance(&a);
    while (!details_cleanup(&a)) { a.ui.quit=false; if (os64_ui_dispatcher_turn(&a.dispatcher,true)<0) return 1; a.ui.quit=true; }
    (void)os64_ui_dispatcher_destroy(&a.dispatcher,window);
    return 0;
}
