#include "os64/ui_browser.h"
#include "os64/ui_menu.h"
#include "os64/ui_action.h"
#include "os64/ui_dispatcher.h"
#include "os64/proc.h"
#include "os64/fmt.h"
#include "os64/str.h"
#include "image/icon_theme.h"

enum {
    A_OPEN=1,A_QUIT,A_SELECT_ALL,A_CLEAR_SELECTION,A_BACK,A_FORWARD,A_UP,
    A_REFRESH,A_ROOT,A_HOME,A_FOCUS_ADDRESS,A_FOCUS_FILTER,A_HIDDEN,A_HELP,
    A_COLUMNS,A_COLUMN_LEFT,A_COLUMN_RIGHT,A_SHOW_MENU,A_SHOW_TOOLBAR,
    A_SHOW_ADDRESS,A_SHOW_CRUMBS,A_SHOW_FILTER,A_SHOW_SIDEBAR,A_SHOW_STATUS,
    A_COLUMN_1=100
};
enum { M_FILE,M_EDIT,M_VIEW,M_GO,M_HELP,M_TOP,M_HEADER,M_ROW,M_BACKGROUND,M_COUNT };

typedef struct {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root, browser_area;
    os64_ui_browser_t browser_ui;
    os64_ui_menubar_t bar;
    os64_browser_t *browser;
    os64_icon_theme_t *icons;
    os64_ui_dispatcher_t dispatcher;
    os64_ui_menu_session_t *menus;
    os64_ui_actions_t *actions;
    os64_ui_action_view_t *action_view;
    os64_ui_menu_model_t *models[M_COUNT];
    os64_ui_shortcut_router_t shortcuts;
    int64_t window;
    uint64_t context_domain, context_id, pending_domain, pending_id;
    uint32_t context_column;
    bool initialized, menu_visible;
} roam_t;

static roam_t app;
static bool app_retain(void *cookie,uint64_t generation)
{ (void)cookie; return generation==1; }
static void app_release(void *cookie,uint64_t generation)
{ (void)cookie; (void)generation; }
static bool app_valid(void *cookie,uint64_t generation)
{ (void)cookie; return generation==1; }

static bool target_valid(const roam_t *a)
{
    const os64_directory_t *dir=os64_browser_directory(a->browser);
    return dir && a->context_id && a->context_domain==os64_directory_domain(dir) &&
        os64_directory_entry(dir,a->context_id);
}

static int32_t nonnegative(int32_t n) { return n>0?n:0; }
static os64_font_status_t layout(roam_t *a,bool staged)
{
    int32_t pad=a->ui.theme.pad, gap=a->ui.theme.gap;
    if(pad<0) pad=0;
    if(gap<0) gap=0;
    int32_t line=os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI)+6;
    int32_t width=nonnegative(a->root.bounds.w-2*pad);
    int32_t bar_h=a->bar.w.hidden?0:line;
    os64_gui_rect_t bar={pad,pad,width,bar_h};
    os64_gui_rect_t area={0,bar_h+pad+(bar_h?gap:0),a->root.bounds.w,
        nonnegative(a->root.bounds.h-bar_h-pad-(bar_h?gap:0))};
    if(staged) {
        os64_ui_widget_stage_bounds(&a->bar.w,bar);
        os64_ui_widget_stage_bounds(&a->browser_area,area);
    } else { a->bar.w.bounds=bar; a->browser_area.bounds=area; }
    return os64_ui_browser_layout(&a->browser_ui,staged);
}

static void resize(os64_ui_t *ui)
{ roam_t *a=ui->root->user; layout(a,false); }
static os64_font_status_t plan(os64_ui_t *ui,void *user,void **out)
{ (void)ui; *out=NULL; return layout(user,true); }
static void plan_done(os64_ui_t *ui,void *user,void *plan_data)
{ (void)ui; (void)user; (void)plan_data; }
static void menu_layout(void *user)
{ roam_t *a=user; layout(a,false); os64_ui_mark_dirty(&a->ui,&a->root); }
static void menu_error(void *user,os64_ui_status_code_t status)
{ roam_t *a=user; (void)status; os64_ui_browser_message(&a->browser_ui,"Menu unavailable."); }

static void result_message(roam_t *a,os64_ui_status_code_t rc)
{
    if(rc!=OS64_UI_OK && rc!=OS64_UI_BUSY)
        os64_ui_browser_message(&a->browser_ui,"Could not open that location.");
}
static void activate(roam_t *a,uint64_t domain,os64_ui_data_id_t id)
{
    const os64_directory_t *dir=os64_browser_directory(a->browser);
    const os64_directory_entry_t *entry=dir && domain==os64_directory_domain(dir)?
        os64_directory_entry(dir,id):NULL;
    if(!entry) return;
    if(!(entry->entry.flags&OS64_DE_DIR)) {
        os64_ui_browser_message(&a->browser_ui,"Opening files is not configured.");
        return;
    }
    result_message(a,os64_browser_activate(a->browser,domain,id));
}
static void changed(os64_ui_dataview_t *view,
    const os64_ui_dataview_change_t *change,void *user)
{
    roam_t *a=user; (void)view;
    if(change->activated) {
        a->pending_domain=change->domain;
        a->pending_id=change->activated;
    }
}
static void view_error(os64_ui_dataview_t *view,
    os64_ui_dataview_operation_t operation,os64_ui_status_code_t status,void *user)
{ (void)view; (void)operation; (void)status; (void)user; }

static bool context(os64_ui_widget_t *w,const os64_gui_event_t *event,void *user)
{
    roam_t *a=user;
    if(!a->menus) return false;
    bool keyboard=event->type==OS64_GUI_EVENT_KEY_DOWN;
    os64_gui_rect_t anchor=keyboard?w->bounds:
        (os64_gui_rect_t){event->mouse.x,event->mouse.y,0,0};
    os64_ui_menu_model_t *model=a->models[M_BACKGROUND];
    a->context_id=0; a->context_domain=0; a->context_column=0;
    if(w==&a->browser_ui.details.w) {
        if(!keyboard) a->context_column=os64_ui_dataview_header_at(
            &a->browser_ui.details,event->mouse.x,event->mouse.y);
        if(a->context_column) model=a->models[M_HEADER];
        else {
            os64_ui_data_id_t target=0;
            if(os64_ui_dataview_context_target(&a->browser_ui.details,event,&target)==OS64_UI_OK && target) {
                const os64_directory_t *dir=os64_browser_directory(a->browser);
                a->context_id=target;
                a->context_domain=dir?os64_directory_domain(dir):0;
                model=a->models[M_ROW];
            }
        }
    }
    os64_ui_status_code_t rc=os64_ui_menu_open(a->menus,model,a->action_view,
        anchor,OS64_UI_MENU_CONTEXT);
    if(rc!=OS64_UI_OK && rc!=OS64_UI_BUSY) menu_error(a,rc);
    return true;
}

static bool column_state(roam_t *a,uint32_t id,os64_ui_dataview_column_t *out,size_t *index)
{
    for(size_t i=0;i<OS64_UI_DATA_COLUMNS;++i) {
        os64_ui_dataview_column_t col;
        if(!os64_ui_dataview_column(&a->browser_ui.details,i,&col)) break;
        if(col.id==id) { if(out) *out=col; if(index) *index=i; return true; }
    }
    return false;
}
static void query(void *user,const os64_ui_action_context_t *ctx,uint32_t id,
    os64_ui_action_state_t *state)
{
    roam_t *a=user; (void)ctx;
    state->enabled=true; state->checked=false;
    if(id==A_OPEN) state->enabled=target_valid(a);
    if(id==A_BACK) state->enabled=os64_browser_can_back(a->browser);
    if(id==A_FORWARD) state->enabled=os64_browser_can_forward(a->browser);
    if(id==A_HIDDEN) state->checked=os64_browser_hidden(a->browser);
    if(id==A_SHOW_MENU) state->checked=a->menu_visible;
    if(id>=A_SHOW_TOOLBAR && id<=A_SHOW_STATUS)
        state->checked=os64_ui_browser_visible(&a->browser_ui,
            (os64_ui_browser_part_t)(id-A_SHOW_TOOLBAR));
    if(id==A_COLUMN_LEFT || id==A_COLUMN_RIGHT) {
        size_t position=0;
        os64_ui_dataview_column_t neighbor;
        state->enabled=column_state(a,a->context_column,NULL,&position) &&
            (id==A_COLUMN_LEFT?position>0:
                os64_ui_dataview_column(&a->browser_ui.details,position+1,&neighbor));
    }
    if(id>=A_COLUMN_1 && id<A_COLUMN_1+4) {
        os64_ui_dataview_column_t col={0};
        bool present=column_state(a,id-A_COLUMN_1+1,&col,NULL);
        state->enabled=present && col.id!=1;
        state->checked=present && col.visible;
    }
}
static os64_ui_action_result_t invoke(void *user,const os64_ui_action_context_t *ctx,uint32_t id)
{
    roam_t *a=user; (void)ctx;
    os64_ui_status_code_t rc=OS64_UI_OK;
    switch(id) {
    case A_OPEN: if(target_valid(a)) activate(a,a->context_domain,a->context_id); else rc=OS64_UI_STALE; break;
    case A_QUIT: a->ui.quit=true; break;
    case A_SELECT_ALL: rc=os64_ui_dataview_command(&a->browser_ui.details,
        OS64_UI_SELECTION_ALL,0,false); break;
    case A_CLEAR_SELECTION: rc=os64_ui_dataview_command(&a->browser_ui.details,
        OS64_UI_SELECTION_CLEAR,0,false); break;
    case A_BACK: rc=os64_browser_back(a->browser); break;
    case A_FORWARD: rc=os64_browser_forward(a->browser); break;
    case A_UP: rc=os64_browser_up(a->browser); break;
    case A_REFRESH: rc=os64_browser_refresh(a->browser); break;
    case A_ROOT: rc=os64_browser_navigate(a->browser,"/"); break;
    case A_HOME: {
        char home[OS64_DIRECTORY_PATH];
        rc=os64_directory_home(home);
        if(rc==OS64_UI_OK) rc=os64_browser_navigate(a->browser,home);
        break;
    }
    case A_FOCUS_ADDRESS: os64_ui_browser_focus_address(&a->browser_ui); break;
    case A_FOCUS_FILTER: os64_ui_browser_focus_filter(&a->browser_ui); break;
    case A_HIDDEN: rc=os64_browser_filter(a->browser,
        os64_browser_filter_text(a->browser),!os64_browser_hidden(a->browser)); break;
    case A_HELP: os64_ui_browser_message(&a->browser_ui,
        "Enter opens folders. Use the address or filter to find a location."); break;
    case A_COLUMNS: rc=os64_ui_menu_open(a->menus,a->models[M_HEADER],a->action_view,
        a->bar.w.bounds,OS64_UI_MENU_CONTEXT); break;
    case A_COLUMN_LEFT: case A_COLUMN_RIGHT: {
        os64_ui_dataview_column_t col; size_t index;
        if(!column_state(a,a->context_column,&col,&index)) rc=OS64_UI_STALE;
        else rc=os64_ui_dataview_column_set(&a->browser_ui.details,col.id,col.width,
            col.visible,id==A_COLUMN_LEFT?index-1:index+1);
        break;
    }
    case A_SHOW_MENU:
        if(a->bar.temporary) { a->bar.temporary=false; a->bar.w.hidden=true; }
        a->menu_visible=!a->menu_visible;
        os64_ui_set_hidden(&a->ui,&a->bar.w,!a->menu_visible);
        layout(a,false); break;
    case A_SHOW_TOOLBAR: case A_SHOW_ADDRESS: case A_SHOW_CRUMBS:
    case A_SHOW_FILTER: case A_SHOW_SIDEBAR: case A_SHOW_STATUS: {
        os64_ui_browser_part_t part=(os64_ui_browser_part_t)(id-A_SHOW_TOOLBAR);
        os64_ui_browser_show(&a->browser_ui,part,
            !os64_ui_browser_visible(&a->browser_ui,part));
        break;
    }
    default:
        if(id>=A_COLUMN_1 && id<A_COLUMN_1+4) {
            os64_ui_dataview_column_t col; size_t index;
            if(!column_state(a,id-A_COLUMN_1+1,&col,&index) || col.id==1)
                rc=OS64_UI_REFUSED;
            else rc=os64_ui_dataview_column_set(&a->browser_ui.details,col.id,
                col.width,!col.visible,index);
        } else rc=OS64_UI_INVALID;
        break;
    }
    if(rc!=OS64_UI_OK && rc!=OS64_UI_BUSY) result_message(a,rc);
    return (os64_ui_action_result_t){rc==OS64_UI_OK?OS64_UI_OK:
        rc==OS64_UI_BUSY?OS64_UI_REFUSED:rc,rc};
}

static bool create_actions(roam_t *a)
{
    os64_ui_action_spec_t specs[]={
        {A_OPEN,OS64_UI_ACTION_COMMAND,0,"Open","Open the selected folder",NULL,'o',{0,0}},
        {A_QUIT,OS64_UI_ACTION_COMMAND,0,"Close Roam","Close this window",NULL,'q',{'q',OS64_GUI_MOD_CTRL}},
        {A_SELECT_ALL,OS64_UI_ACTION_COMMAND,0,"Select All","Select visible rows",NULL,'a',{'a',OS64_GUI_MOD_CTRL}},
        {A_CLEAR_SELECTION,OS64_UI_ACTION_COMMAND,0,"Clear Selection","Clear selection",NULL,'c',{0,0}},
        {A_BACK,OS64_UI_ACTION_COMMAND,0,"Back","Previous location",NULL,'b',{'[',OS64_GUI_MOD_CTRL}},
        {A_FORWARD,OS64_UI_ACTION_COMMAND,0,"Forward","Next location",NULL,'f',{']',OS64_GUI_MOD_CTRL}},
        {A_UP,OS64_UI_ACTION_COMMAND,0,"Up","Parent folder",NULL,'u',{OS64_UI_KEY_UP,OS64_GUI_MOD_ALT}},
        {A_REFRESH,OS64_UI_ACTION_COMMAND,0,"Refresh","Read this location again",NULL,'r',{OS64_UI_KEY_F5,0}},
        {A_ROOT,OS64_UI_ACTION_COMMAND,0,"Root","Filesystem root",NULL,'r',{0,0}},
        {A_HOME,OS64_UI_ACTION_COMMAND,0,"Home","Home folder",NULL,'h',{0,0}},
        {A_FOCUS_ADDRESS,OS64_UI_ACTION_COMMAND,0,"Location","Edit the current location",NULL,'l',{'l',OS64_GUI_MOD_CTRL}},
        {A_FOCUS_FILTER,OS64_UI_ACTION_COMMAND,0,"Quick Filter","Filter this folder",NULL,'f',{'f',OS64_GUI_MOD_CTRL}},
        {A_HIDDEN,OS64_UI_ACTION_TOGGLE,0,"Show Hidden Files","Include dot names",NULL,'h',{'h',OS64_GUI_MOD_CTRL}},
        {A_HELP,OS64_UI_ACTION_COMMAND,0,"Using Roam","Navigation help",NULL,'u',{OS64_UI_KEY_F1,0}},
        {A_COLUMNS,OS64_UI_ACTION_COMMAND,0,"Columns","Show column options",NULL,'c',{0,0}},
        {A_COLUMN_LEFT,OS64_UI_ACTION_COMMAND,0,"Move Column Left","Reorder the header column",NULL,'l',{0,0}},
        {A_COLUMN_RIGHT,OS64_UI_ACTION_COMMAND,0,"Move Column Right","Reorder the header column",NULL,'r',{0,0}},
        {A_SHOW_MENU,OS64_UI_ACTION_TOGGLE,0,"Show Menu Bar","Alt or F10 reveals the menu",NULL,'m',{0,0}},
        {A_SHOW_TOOLBAR,OS64_UI_ACTION_TOGGLE,0,"Show Toolbar","Navigation buttons",NULL,'t',{0,0}},
        {A_SHOW_ADDRESS,OS64_UI_ACTION_TOGGLE,0,"Show Location","Editable address",NULL,'l',{0,0}},
        {A_SHOW_CRUMBS,OS64_UI_ACTION_TOGGLE,0,"Show Breadcrumbs","Path components",NULL,'b',{0,0}},
        {A_SHOW_FILTER,OS64_UI_ACTION_TOGGLE,0,"Show Quick Filter","Name filter",NULL,'f',{0,0}},
        {A_SHOW_SIDEBAR,OS64_UI_ACTION_TOGGLE,0,"Show Places","Favorites and mounts",NULL,'p',{0,0}},
        {A_SHOW_STATUS,OS64_UI_ACTION_TOGGLE,0,"Show Status","Location and loading status",NULL,'s',{0,0}},
        {A_COLUMN_1,OS64_UI_ACTION_TOGGLE,0,"Name","Required column",NULL,'n',{0,0}},
        {A_COLUMN_1+1,OS64_UI_ACTION_TOGGLE,0,"Size","File size",NULL,'s',{0,0}},
        {A_COLUMN_1+2,OS64_UI_ACTION_TOGGLE,0,"Type","Display type",NULL,'t',{0,0}},
        {A_COLUMN_1+3,OS64_UI_ACTION_TOGGLE,0,"Modified","Modification time",NULL,'m',{0,0}}
    };
    const os64_ui_action_callbacks_t callbacks={query,invoke};
    os64_ui_status_code_t rc=os64_ui_actions_create(specs,
        sizeof(specs)/sizeof(*specs),&callbacks,a,&a->actions);
    if(rc) { os64_printf("roam: action registry refused (%u)\n",rc); return false; }
    os64_ui_action_context_t ctx={a,1,app_retain,app_release,app_valid};
    rc=os64_ui_action_view_create(a->actions,&ctx,&a->action_view);
    if(rc) os64_printf("roam: action view refused (%u)\n",rc);
    return rc==OS64_UI_OK;
}

static bool create_menus(roam_t *a)
{
    os64_ui_menu_entry_t file[]={{.action=A_FOCUS_ADDRESS},{.action=A_REFRESH},
        {.kind=OS64_UI_MENU_SEPARATOR},{.action=A_QUIT}};
    os64_ui_menu_entry_t edit[]={{.action=A_SELECT_ALL},{.action=A_CLEAR_SELECTION}};
    os64_ui_menu_entry_t columns[]={{.action=A_COLUMN_1},{.action=A_COLUMN_1+1},
        {.action=A_COLUMN_1+2},{.action=A_COLUMN_1+3},
        {.kind=OS64_UI_MENU_SEPARATOR},{.action=A_COLUMN_LEFT},{.action=A_COLUMN_RIGHT}};
    os64_ui_menu_entry_t go[]={{.action=A_BACK},{.action=A_FORWARD},{.action=A_UP},
        {.action=A_ROOT},{.action=A_HOME},{.action=A_REFRESH}};
    os64_ui_menu_entry_t help[]={{.action=A_HELP}};
    os64_ui_menu_entry_t row[]={{.action=A_OPEN},{.kind=OS64_UI_MENU_SEPARATOR},
        {.action=A_FOCUS_ADDRESS},{.action=A_SHOW_MENU}};
    os64_ui_menu_entry_t background[]={{.action=A_SHOW_MENU},{.action=A_REFRESH},
        {.action=A_FOCUS_ADDRESS},{.action=A_FOCUS_FILTER}};
    if(os64_ui_menu_model_create(a->actions,file,4,&a->models[M_FILE]) ||
       os64_ui_menu_model_create(a->actions,edit,2,&a->models[M_EDIT]) ||
       os64_ui_menu_model_create(a->actions,columns,7,&a->models[M_HEADER]) ||
       os64_ui_menu_model_create(a->actions,go,6,&a->models[M_GO]) ||
       os64_ui_menu_model_create(a->actions,help,1,&a->models[M_HELP]) ||
       os64_ui_menu_model_create(a->actions,row,4,&a->models[M_ROW]) ||
       os64_ui_menu_model_create(a->actions,background,4,&a->models[M_BACKGROUND])) {
        os64_printf("roam: base menu creation refused\n"); return false;
    }
    os64_ui_menu_entry_t view[]={{.kind=OS64_UI_MENU_SUBMENU,.label="Columns",
        .mnemonic='c',.child=a->models[M_HEADER]},{.action=A_HIDDEN},
        {.kind=OS64_UI_MENU_SEPARATOR},{.action=A_SHOW_MENU},{.action=A_SHOW_TOOLBAR},
        {.action=A_SHOW_ADDRESS},{.action=A_SHOW_CRUMBS},{.action=A_SHOW_FILTER},
        {.action=A_SHOW_SIDEBAR},{.action=A_SHOW_STATUS}};
    if(os64_ui_menu_model_create(a->actions,view,10,&a->models[M_VIEW])) {
        os64_printf("roam: View menu creation refused\n"); return false;
    }
    os64_ui_menu_entry_t top[]={{.kind=OS64_UI_MENU_SUBMENU,.label="File",.mnemonic='f',.child=a->models[M_FILE]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Edit",.mnemonic='e',.child=a->models[M_EDIT]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="View",.mnemonic='v',.child=a->models[M_VIEW]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Go",.mnemonic='g',.child=a->models[M_GO]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Help",.mnemonic='h',.child=a->models[M_HELP]}};
    os64_ui_status_code_t rc=os64_ui_menu_model_create(a->actions,top,5,&a->models[M_TOP]);
    if(!rc) rc=os64_ui_menubar_bind(&a->bar,a->models[M_TOP]);
    if(rc) os64_printf("roam: menu bar creation refused (%u)\n",rc);
    return rc==OS64_UI_OK;
}

static void commands(void *user,const os64_gui_event_t *event)
{
    roam_t *a=user;
    bool editing=a->ui.focus==&a->browser_ui.address.w ||
        a->ui.focus==&a->browser_ui.filter.w;
    if(!editing && a->ui.focus==&a->browser_ui.details.w &&
       os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_BACKSPACE,0},event)) {
        result_message(a,os64_browser_up(a->browser)); return;
    }
    uint32_t shortcut=os64_ui_action_shortcut_id(a->action_view,event);
    bool allowed=!editing || shortcut==A_FOCUS_ADDRESS ||
        shortcut==A_FOCUS_FILTER || shortcut==A_HIDDEN || shortcut==A_QUIT ||
        shortcut==A_REFRESH || shortcut==A_BACK || shortcut==A_FORWARD ||
        shortcut==A_UP || event->type==OS64_GUI_EVENT_KEY_UP ||
        event->type==OS64_GUI_EVENT_WINDOW_FOCUS ||
        event->type==OS64_GUI_EVENT_WINDOW_COVERED ||
        event->type==OS64_GUI_EVENT_INPUT_LOST;
    if(!allowed || !os64_ui_shortcut_route(&a->shortcuts,a->action_view,event,NULL))
        (void)os64_ui_dispatch(&a->ui,event);
}
static void prepare_turn(roam_t *a)
{
    if(a->ui.quit) return;
    if(!os64_ui_menu_active(a->menus)) {
        if(a->pending_id) {
            uint64_t domain=a->pending_domain,id=a->pending_id;
            a->pending_id=0; a->pending_domain=0;
            activate(a,domain,id);
        }
        (void)os64_browser_poll(a->browser);
    }
    os64_ui_browser_sync(&a->browser_ui);
}
static void paint(void *user)
{
    roam_t *a=user;
    prepare_turn(a);
    (void)os64_ui_actions_refresh(a->action_view);
    os64_ui_paint(&a->ui);
}

static bool initialize(roam_t *a,const char *initial)
{
    a->window=os64_gui_window_create("Roam",72,48,880,630,0);
    if(a->window<=0) {
        os64_printf("roam: window unavailable (%ld)\n",(long)a->window); return false;
    }
    if(os64_draw_ctx_init(&a->draw,a->window)!=0) {
        os64_printf("roam: window surface unavailable\n"); return false;
    }
    os64_ui_init(&a->ui,&a->draw);
    os64_ui_panel(&a->root);
    a->root.bounds=(os64_gui_rect_t){0,0,(int32_t)a->draw.surf.width,(int32_t)a->draw.surf.height};
    a->root.user=a;
    os64_ui_set_root(&a->ui,&a->root);
    os64_ui_menubar_init(&a->bar); os64_ui_add_child(&a->root,&a->bar.w);
    os64_ui_panel(&a->browser_area); os64_ui_add_child(&a->root,&a->browser_area);
    os64_ui_browser_init(&a->browser_ui,&a->ui,&a->browser_area);
    a->browser_ui.details.on_change=changed;
    a->browser_ui.details.on_error=view_error;
    a->browser_ui.details.callback_user=a;
    a->browser_ui.details.w.on_context=context;
    a->browser_ui.details.w.context_user=a;
    a->root.on_context=context; a->root.context_user=a;
    a->browser_area.on_context=context; a->browser_area.context_user=a;
    a->menu_visible=true;
    a->ui.on_resize=resize;
    if(os64_ui_font_planner(&a->ui,plan,plan_done,plan_done,a)!=OS64_FONT_OK) {
        os64_printf("roam: font planner unavailable\n"); return false;
    }
    layout(a,false);
    (void)os64_icon_theme_create(&a->icons);
    os64_ui_status_code_t rc=os64_browser_create(&a->browser_ui.details,
        a->icons?os64_icon_theme_provider(a->icons):NULL,
        a->window,0x200u,&a->browser);
    if(rc!=OS64_UI_OK) {
        os64_printf("roam: directory controller refused (%u)\n",rc); return false;
    }
    os64_ui_browser_bind(&a->browser_ui,a->browser);
    if(!create_actions(a) || !create_menus(a)) return false;
    os64_ui_dispatcher_init(&a->dispatcher,NULL);
    rc=os64_ui_dispatcher_add(&a->dispatcher,a->window,&a->ui,commands,paint,a);
    if(rc!=OS64_UI_OK) {
        os64_printf("roam: event dispatcher refused (%u)\n",rc); return false;
    }
    rc=os64_ui_menu_session_create(&a->dispatcher,&a->ui,a->window,&a->bar,
        &a->browser_ui.status,a->icons?os64_icon_theme_provider(a->icons):NULL,
        &a->menus);
    if(rc!=OS64_UI_OK) {
        os64_printf("roam: native menu session refused (%u)\n",rc); return false;
    }
    os64_ui_menu_hooks(a->menus,menu_layout,menu_error,a);
    os64_ui_menu_focus_fallback(a->menus,&a->browser_ui.details.w);
    rc=os64_ui_menu_bar_view(a->menus,a->action_view);
    if(rc!=OS64_UI_OK) {
        os64_printf("roam: menu action view refused (%u)\n",rc); return false;
    }
    os64_ui_shortcut_reset(&a->shortcuts);
    (void)os64_ui_font_follow(&a->ui);
    layout(a,false);
    os64_ui_set_focus(&a->ui,&a->browser_ui.details.w);
    result_message(a,os64_browser_navigate(a->browser,initial));
    os64_ui_browser_sync(&a->browser_ui);
    a->initialized=true;
    return true;
}
static bool cleanup(roam_t *a)
{
    if(a->menus && os64_ui_menu_session_dispose(a->menus)!=OS64_UI_OK)
        return false;
    a->menus=NULL;
    if(a->browser) { os64_browser_destroy(a->browser); a->browser=NULL; }
    os64_ui_menubar_destroy(&a->bar);
    for(size_t i=0;i<M_COUNT;++i) {
        os64_ui_menu_model_release(a->models[i]); a->models[i]=NULL;
    }
    if(a->action_view) {
        (void)os64_ui_action_view_destroy(a->action_view); a->action_view=NULL;
    }
    if(a->actions) { (void)os64_ui_actions_destroy(a->actions); a->actions=NULL; }
    if(os64_ui_font_release(&a->ui)!=OS64_FONT_OK) return false;
    if(a->icons && os64_icon_theme_destroy(a->icons)!=OS64_UI_OK) return false;
    a->icons=NULL;
    return true;
}

int main(int argc,char **argv)
{
    char cwd[OS64_DIRECTORY_PATH], normalized[OS64_DIRECTORY_PATH];
    const char *initial="/";
    if(os64_getcwd(cwd,sizeof(cwd))>=0) initial=cwd;
    bool invalid_start=false;
    if(argc>1) {
        if(os64_directory_path(initial,argv[1],normalized)==OS64_UI_OK)
            initial=normalized;
        else invalid_start=true;
    }
    if(!initialize(&app,initial)) {
        os64_printf("roam: could not initialize browser\n");
        (void)cleanup(&app);
        if(app.window>0) (void)os64_gui_window_destroy(app.window);
        return 1;
    }
    if(invalid_start)
        os64_ui_browser_message(&app.browser_ui,"Could not open that location.");
    while(!app.ui.quit)
        if(os64_ui_dispatcher_turn(&app.dispatcher,true)<0) break;
    while(!cleanup(&app)) {
        app.ui.quit=false;
        if(os64_ui_dispatcher_turn(&app.dispatcher,true)<0) return 1;
        app.ui.quit=true;
    }
    (void)os64_ui_dispatcher_destroy(&app.dispatcher,app.window);
    return 0;
}
