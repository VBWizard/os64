/* Shared menu/action/status test gallery: app-owned targets and visibility policy,
 * ordinary libui controls and the configured Interface font. */
#include "os64/ui.h"
#include "os64/ui_action.h"
#include "os64/ui_status.h"
#include "os64/ui_menu.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

enum { RECORD=1, PANEL, STATUS, HELP, CLEAR_HELP, CLEAR_NOTICE, QUIT, BAR, HEADING, PANE_A, PANE_B, RADIO_A, RADIO_B };
typedef struct { uint64_t generation; unsigned refs, hits; bool enabled; } gallery_target;
typedef struct {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root, heading, instructions, content, panel;
    os64_ui_widget_t select[2], record[2], enable[2], invalidate[2], titles[2], controls[6];
    os64_ui_status_t status;
    os64_ui_menubar_t bar;
    os64_ui_menu_model_t *models[8];
    os64_ui_menu_session_t *menus;
    os64_ui_dispatcher_t dispatcher;
    os64_ui_textfield_t editor;
    char editor_text[256];
    unsigned radio;
    bool trace;
    size_t trace_windows;
    os64_ui_actions_t *actions;
    os64_ui_action_view_t *views[3];
    gallery_target targets[2];
    os64_ui_shortcut_router_t router;
    unsigned selected, help_count;
    uint64_t help_token, previous_help, notice_token;
} gallery;
static gallery g;
static bool app_retain(void *cookie, uint64_t gen)
{ (void)cookie; (void)gen; return true; }
static void app_release(void *cookie, uint64_t gen)
{ (void)cookie; (void)gen; }
static bool app_valid(void *cookie, uint64_t gen)
{ (void)cookie; return gen == 1; }
static bool target_retain(void *cookie, uint64_t gen)
{ (void)gen; ++((gallery_target *)cookie)->refs; return true; }
static void target_release(void *cookie, uint64_t gen)
{ (void)gen; --((gallery_target *)cookie)->refs; }
static bool target_valid(void *cookie, uint64_t gen)
{ return ((gallery_target *)cookie)->generation == gen; }
static void gallery_query(void *app, const os64_ui_action_context_t *context, uint32_t id,
                  os64_ui_action_state_t *state)
{
    gallery *a = app;
    gallery_target *t = context->cookie;
    state->enabled = id == RECORD ? (context->cookie != a && t->enabled) : true;
    if (id == PANEL) state->checked = !a->panel.hidden;
    if (id == STATUS) state->checked = !a->status.w.hidden;
    if (id == BAR) state->checked = !a->bar.w.hidden || a->bar.temporary;
    if (id == HEADING) state->checked = !a->heading.hidden;
    if (id == PANE_A) state->checked = !a->record[0].hidden;
    if (id == PANE_B) state->checked = !a->record[1].hidden;
    if (id == RADIO_A || id == RADIO_B) state->checked = a->radio == id;
    if (id >= 100) state->enabled = id % 7 != 0;
}

/* Both live resize and font adoption use the candidate's row height. */
static void gallery_layout(gallery *a, bool staged)
{
    int32_t pad = a->ui.theme.pad, gap = a->ui.theme.gap;
    int32_t row = os64_ui_control_min_height(&a->ui);
    int32_t text_row = os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI);
    int32_t width = a->root.bounds.w - 2 * pad;
    if (width < 0) width = 0;
    int32_t col = width > gap ? (width-gap)/2 : 0;
    os64_ui_widget_t *nodes[27];
    os64_gui_rect_t rects[27];
    size_t n = 0;
#define PLACE(w_, x_, y_, width_, height_) do { nodes[n]=(w_); \
    rects[n++]=(os64_gui_rect_t){(x_), (y_), (width_), (height_)}; } while (0)
    int32_t y = pad;
    PLACE(&a->bar.w, pad, y, width, os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI)+6);
    if (!a->bar.w.hidden) y += os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI)+6+gap;
    PLACE(&a->heading, pad, y, width, text_row); if (!a->heading.hidden) y += text_row + gap;
    PLACE(&a->instructions, pad, y, width, text_row); y += text_row + gap;
    for (unsigned i = 0; i < 2; ++i)
        PLACE(&a->select[i], pad+(int32_t)i*(col+gap), y, col, row);
    y += row + gap;
    PLACE(&a->content, pad, y, col, row);
    PLACE(&a->editor.w, pad+col+gap, y, col, row); y += row + gap;
    int32_t panel_height = 4*row+3*gap;
    PLACE(&a->panel, pad, y, width, panel_height);
    for (unsigned i = 0; i < 2; ++i) {
        int32_t x = pad+(int32_t)i*(col+gap), py = y;
        PLACE(&a->titles[i], x, py, col, row); py += row+gap;
        PLACE(&a->record[i], x, py, col, row); py += row+gap;
        PLACE(&a->enable[i], x, py, col, row); py += row+gap;
        PLACE(&a->invalidate[i], x, py, col, row);
    }
    if (!a->panel.hidden) y += panel_height + gap;
    int32_t control_width = width > 2*gap ? (width-2*gap)/3 : 0;
    for (unsigned i = 0; i < 6; ++i) {
        PLACE(&a->controls[i], pad+(int32_t)(i%3)*(control_width+gap),
            y+(int32_t)(i/3)*(row+gap), control_width, row);
    }
    y += 2*(row+gap);
    PLACE(&a->status.w, pad, y, width, row);
    for (size_t i = 0; i < n; ++i) {
        if (staged) os64_ui_widget_stage_bounds(nodes[i], rects[i]);
        else nodes[i]->bounds = rects[i];
    }
#undef PLACE
}
static void gallery_resize(os64_ui_t *ui) { (void)ui; gallery_layout(&g, false); }
static os64_font_status_t gallery_plan(os64_ui_t *ui, void *user, void **out)
{
    gallery *a=user;
    /* Hidden widgets still stage their runs, but consume no visible rows. */
    int32_t rows=6+(a->panel.hidden?0:4)+(a->status.w.hidden?0:1)-(a->heading.hidden?1:0);
    int64_t need=(int64_t)rows*os64_ui_control_min_height(ui) +
        (int64_t)(rows-1)*ui->theme.gap + 2*ui->theme.pad +
        (a->bar.w.hidden?0:os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI)+6+ui->theme.gap) -
        (1+(a->heading.hidden?0:1))*(os64_ui_control_min_height(ui)-os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI));
    *out=NULL;
    if(need>a->root.bounds.h) return OS64_FONT_LIMIT;
    gallery_layout(a, true);
    return OS64_FONT_OK;
}
static void gallery_plan_done(os64_ui_t *ui, void *user, void *p) { (void)ui; (void)user; (void)p; }
static void gallery_hide(gallery *a, os64_ui_widget_t *w)
{
    if (!w->hidden) {
        for (os64_ui_widget_t *f = a->ui.focus; f; f = f->parent)
            if (f == w) { os64_ui_set_focus(&a->ui, &a->content); break; }
    }
    os64_ui_set_hidden(&a->ui, w, !w->hidden);
    gallery_layout(a, false);
    os64_ui_mark_dirty(&a->ui, &a->root);
}
static os64_ui_action_result_t gallery_invoke(void *app,
    const os64_ui_action_context_t *context, uint32_t id)
{
    gallery *a = app;
    gallery_target *t = context->cookie;
    os64_ui_status_code_t rc = OS64_UI_OK;
    if(a->trace) os64_printf("gallery invoke=%u menus=%u target=%u\n",id,os64_ui_menu_active(a->menus),context->cookie==a?2u:context->cookie==&a->targets[0]?0u:1u);
    switch (id) {
    case RECORD: {
        ++t->hits;
        char text[128];
        os64_snprintf(text, sizeof(text), "Target A: %u calls   Target B: %u calls",
                      a->targets[0].hits, a->targets[1].hits);
        rc = os64_ui_status_base(&a->status, text);
        break;
    }
    case PANEL: gallery_hide(a, &a->panel); break;
    case STATUS: gallery_hide(a, &a->status.w); break;
    case BAR:
        /* A temporarily revealed bar still represents a persistently hidden bar. */
        if(a->bar.temporary) { a->bar.temporary=false; a->bar.w.hidden=true; }
        gallery_hide(a,&a->bar.w); break;
    case HEADING: gallery_hide(a,&a->heading); break;
    case PANE_A: gallery_hide(a,&a->record[0]); break;
    case PANE_B: gallery_hide(a,&a->record[1]); break;
    case RADIO_A: case RADIO_B: a->radio=id; break;
    case HELP: {
        a->previous_help = a->help_token;
        char text[128];
        os64_snprintf(text, sizeof(text), "Action help %u - copied, token-owned", ++a->help_count);
        rc = os64_ui_status_help(&a->status, text, &a->help_token);
        break;
    }
    case CLEAR_HELP:
        /* The old producer cannot erase the latest producer's help. */
        rc = os64_ui_status_clear_help(&a->status, a->previous_help); break;
    case CLEAR_NOTICE: rc = os64_ui_status_clear_notice(&a->status, a->notice_token); break;
    case QUIT: a->ui.quit=true; break;
    default:
        if(id>=100) { char text[80]; os64_snprintf(text,sizeof(text),"Tall-menu command %u invoked",id-100); rc=os64_ui_status_base(&a->status,text); }
        else rc=OS64_UI_INVALID;
        break;
    }
    return (os64_ui_action_result_t){rc==OS64_UI_OK ? OS64_UI_OK :
        rc==OS64_UI_STALE ? OS64_UI_REFUSED : OS64_UI_ERROR, rc};
}
static void gallery_select_target(os64_ui_widget_t *w, void *user)
{
    gallery *a=user;
    a->selected=w==&a->select[1];
    a->content.text=a->selected ? "Action target: B" : "Action target: A";
    os64_ui_mark_dirty(&a->ui, &a->content);
}
static void gallery_change_target(os64_ui_widget_t *w, void *user)
{
    gallery *a=user;
    for (unsigned i=0;i<2;++i) {
        if (w==&a->enable[i]) a->targets[i].enabled=!a->targets[i].enabled;
        if (w==&a->invalidate[i]) ++a->targets[i].generation;
        (void)os64_ui_actions_refresh(a->views[i]);
    }
}
static void gallery_add(gallery *a, os64_ui_widget_t *parent, os64_ui_widget_t *w)
{ (void)a; os64_ui_add_child(parent, w); }
static bool gallery_build(gallery *a)
{
    os64_ui_init(&a->ui, &a->draw);
    os64_ui_panel(&a->root); a->root.bounds=(os64_gui_rect_t){0,0,(int32_t)a->draw.surf.width,(int32_t)a->draw.surf.height};
    os64_ui_set_root(&a->ui, &a->root);
    os64_ui_menubar_init(&a->bar); gallery_add(a,&a->root,&a->bar.w);
    os64_ui_label(&a->heading, "Shared menus, actions & status");
    os64_ui_label(&a->instructions, "Alt/F10 menus   Shift+F10 context   Ctrl+D target");
    os64_ui_button(&a->content,"Action target: A",NULL,NULL);
    gallery_add(a,&a->root,&a->heading); gallery_add(a,&a->root,&a->instructions);
    for(unsigned i=0;i<2;++i) {
        os64_ui_button(&a->select[i],i?"Shortcut target B":"Shortcut target A",gallery_select_target,a);
        gallery_add(a,&a->root,&a->select[i]);
    }
    gallery_add(a,&a->root,&a->content);
    os64_ui_textfield(&a->editor,a->editor_text,sizeof(a->editor_text),NULL,NULL,NULL);
    os64_ui_textfield_set(&a->ui,&a->editor,"Editor owns Ctrl+C/X/V/A");
    gallery_add(a,&a->root,&a->editor.w);
    os64_ui_panel(&a->panel); gallery_add(a,&a->root,&a->panel);
    for(unsigned i=0;i<2;++i) {
        a->targets[i]=(gallery_target){.generation=1,.enabled=i==0};
        os64_ui_label(&a->titles[i],i?"Target B":"Target A");
        os64_ui_button(&a->record[i],"",NULL,NULL);
        os64_ui_button(&a->enable[i],"Enable/disable target",gallery_change_target,a);
        os64_ui_button(&a->invalidate[i],"Make target stale",gallery_change_target,a);
        gallery_add(a,&a->panel,&a->titles[i]); gallery_add(a,&a->panel,&a->record[i]);
        gallery_add(a,&a->panel,&a->enable[i]); gallery_add(a,&a->panel,&a->invalidate[i]);
    }
    os64_ui_status_init(&a->status); gallery_add(a,&a->root,&a->status.w);
    os64_ui_action_spec_t specs[63]={
        {RECORD,OS64_UI_ACTION_COMMAND,0,"Record target", "Invoke the selected target","synthetic-command",'r',{'d',OS64_GUI_MOD_CTRL}},
        {PANEL,OS64_UI_ACTION_TOGGLE,0,"Target panel", "Keep target state while hidden","synthetic-toggle",'t',{'p',OS64_GUI_MOD_CTRL}},
        {STATUS,OS64_UI_ACTION_TOGGLE,0,"Status bar", "Keep messages while hidden",NULL,'s',{'b',OS64_GUI_MOD_CTRL}},
        {HELP,OS64_UI_ACTION_COMMAND,0,"New help", "Replace help with a new token",NULL,0,{'h',OS64_GUI_MOD_CTRL}},
        {CLEAR_HELP,OS64_UI_ACTION_COMMAND,0,"Clear older help", "Stale clear keeps newer help",NULL,0,{0,0}},
        {CLEAR_NOTICE,OS64_UI_ACTION_COMMAND,0,"Clear notice", "Expose base information",NULL,0,{'n',OS64_GUI_MOD_CTRL}},
        {QUIT,OS64_UI_ACTION_COMMAND,0,"Close gallery", "Defer teardown until dispatch returns",NULL,'q',{'q',OS64_GUI_MOD_CTRL}},
        {BAR,OS64_UI_ACTION_TOGGLE,0,"Show Menu Bar","Recover with Alt/F10 or context menu",NULL,'m',{0,0}},
        {HEADING,OS64_UI_ACTION_TOGGLE,0,"Show Heading","Reclaim the heading row",NULL,'h',{0,0}},
        {PANE_A,OS64_UI_ACTION_TOGGLE,0,"Show Pane A control","Retain pane A model",NULL,'a',{0,0}},
        {PANE_B,OS64_UI_ACTION_TOGGLE,0,"Show Pane B control","Retain pane B model",NULL,'b',{0,0}},
        {RADIO_A,OS64_UI_ACTION_RADIO,1,"First radio choice","Application owns radio state",NULL,'f',{0,0}},
        {RADIO_B,OS64_UI_ACTION_RADIO,1,"Second radio choice","Application owns radio state",NULL,'f',{0,0}}
    };
    char tall_labels[50][96];
    for(unsigned i=0;i<50;++i) {
        os64_snprintf(tall_labels[i],sizeof(tall_labels[i]),"Row %02u - Caf\xc3\xa9, a deliberately long UTF-8 menu label",i);
        specs[13+i]=(os64_ui_action_spec_t){.id=100+i,.label=tall_labels[i],.help="Wheel, Page Down or End reaches the last command"};
    }
    const os64_ui_action_callbacks_t cb={gallery_query,gallery_invoke};
    if(os64_ui_actions_create(specs,sizeof(specs)/sizeof(*specs),&cb,a,&a->actions)!=OS64_UI_OK) return false;
    for(unsigned i=0;i<2;++i) {
        os64_ui_action_context_t c={&a->targets[i],1,target_retain,target_release,target_valid};
        if(os64_ui_action_view_create(a->actions,&c,&a->views[i])!=OS64_UI_OK ||
           os64_ui_action_bind(&a->record[i],a->views[i],RECORD)!=OS64_UI_OK) return false;
    }
    os64_ui_action_context_t app_context={a,1,app_retain,app_release,app_valid};
    if(os64_ui_action_view_create(a->actions,&app_context,&a->views[2])!=OS64_UI_OK) return false;
    const uint32_t ids[]={PANEL,STATUS,HELP,CLEAR_HELP,CLEAR_NOTICE,QUIT};
    for(unsigned i=0;i<6;++i) {
        os64_ui_button(&a->controls[i],"",NULL,NULL); gallery_add(a,&a->root,&a->controls[i]);
        if(os64_ui_action_bind(&a->controls[i],a->views[2],ids[i])!=OS64_UI_OK) return false;
    }
    if(os64_ui_status_base(&a->status,"Target A: 0 calls   Target B: 0 calls")!=OS64_UI_OK ||
       os64_ui_status_notice(&a->status,"Persistent notice - stays while help changes",&a->notice_token)!=OS64_UI_OK)
        return false;
    os64_ui_menu_entry_t commands[]={{.action=RECORD},{.action=HELP},{.action=CLEAR_HELP},{.kind=OS64_UI_MENU_SEPARATOR},{.action=QUIT}};
    os64_ui_menu_entry_t visibility[]={{.action=BAR},{.action=PANEL},{.action=STATUS},{.action=HEADING},{.action=PANE_A},{.action=PANE_B}};
    os64_ui_menu_entry_t radios[]={{.action=RADIO_A},{.action=RADIO_B}};
    os64_ui_menu_entry_t tall[50];
    for(unsigned i=0;i<50;++i) tall[i]=(os64_ui_menu_entry_t){.action=100+i};
    if(os64_ui_menu_model_create(a->actions,commands,5,&a->models[0]) ||
       os64_ui_menu_model_create(a->actions,visibility,6,&a->models[1]) ||
       os64_ui_menu_model_create(a->actions,radios,2,&a->models[2]) ||
       os64_ui_menu_model_create(a->actions,tall,50,&a->models[3])) return false;
    os64_ui_menu_entry_t nested[]={{.kind=OS64_UI_MENU_SUBMENU,.label="Duplicate mnemonics",.mnemonic='d',.child=a->models[2]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Tall UTF-8 commands",.mnemonic='t',.child=a->models[3]}};
    if(os64_ui_menu_model_create(a->actions,nested,2,&a->models[4])) return false;
    os64_ui_menu_entry_t tops[]={{.kind=OS64_UI_MENU_SUBMENU,.label="Actions",.mnemonic='a',.child=a->models[0]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="View",.mnemonic='v',.child=a->models[1]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Fixtures",.mnemonic='f',.child=a->models[4]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Long overflow heading",.mnemonic='l',.child=a->models[3]}};
    if(os64_ui_menu_model_create(a->actions,tops,4,&a->models[5]) || os64_ui_menubar_bind(&a->bar,a->models[5])) return false;
    os64_ui_menu_entry_t context[]={{.action=RECORD},{.kind=OS64_UI_MENU_SEPARATOR},{.action=BAR},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Show",.mnemonic='s',.child=a->models[1]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Fixtures",.mnemonic='f',.child=a->models[4]}};
    if(os64_ui_menu_model_create(a->actions,context,5,&a->models[6])) return false;
    os64_ui_shortcut_reset(&a->router);
    a->ui.on_resize=gallery_resize;
    if(os64_ui_font_planner(&a->ui,gallery_plan,gallery_plan_done,gallery_plan_done,a)!=OS64_FONT_OK) return false;
    gallery_layout(a,false); os64_ui_set_focus(&a->ui,&a->content);
    return true;
}
static bool gallery_dispose(gallery *a)
{
    if(os64_ui_menu_session_dispose(a->menus)!=OS64_UI_OK) return false;
    a->menus=NULL;
    os64_ui_cancel_interaction(&a->ui);
    for(unsigned i=0;i<2;++i) (void)os64_ui_action_unbind(&a->record[i]);
    for(unsigned i=0;i<6;++i) (void)os64_ui_action_unbind(&a->controls[i]);
    os64_ui_menubar_destroy(&a->bar);
    for(unsigned i=0;i<8;++i) { os64_ui_menu_model_release(a->models[i]); a->models[i]=NULL; }
    for(unsigned i=0;i<3;++i) if(a->views[i]) (void)os64_ui_action_view_destroy(a->views[i]);
    if(a->actions) (void)os64_ui_actions_destroy(a->actions);
    (void)os64_ui_font_release(&a->ui);
    return true;
}
static bool gallery_route(gallery *a, const os64_gui_event_t *ev, os64_ui_action_result_t *out)
{
    bool record = os64_ui_shortcut_match((os64_ui_shortcut_t){'d',OS64_GUI_MOD_CTRL},ev);
    return os64_ui_shortcut_route(&a->router,a->views[record?a->selected:2],ev,out);
}

static void gallery_menu_layout(void *u) { gallery *a=u; gallery_layout(a,false); os64_ui_mark_dirty(&a->ui,&a->root); }
static void gallery_menu_error(void *u,os64_ui_status_code_t rc)
{ gallery *a=u; char text[80]; os64_snprintf(text,sizeof(text),"Menu refused: toolkit result %u",rc); (void)os64_ui_status_notice(&a->status,text,&a->notice_token); }
static bool gallery_context(os64_ui_widget_t *w,const os64_gui_event_t *e,void *u)
{
    gallery *a=u;
    if(!a->menus) return false;
    bool keyboard=e->type==OS64_GUI_EVENT_KEY_DOWN;
    os64_gui_rect_t anchor=keyboard?w->bounds:(os64_gui_rect_t){e->mouse.x,e->mouse.y,0,0};
    unsigned target=2;
    for(unsigned i=0;i<2;++i) if(w==&a->record[i]||w==&a->titles[i]) target=i;
    os64_ui_status_code_t rc=os64_ui_menu_open(a->menus,a->models[6],a->views[target],anchor,OS64_UI_MENU_CONTEXT);
    if(rc==OS64_UI_STALE||rc==OS64_UI_INVALID) gallery_menu_error(a,rc);
    return true;
}
static uint32_t gallery_icon_pixels[32*32];
static os64_ui_icon_status_t gallery_icon_acquire(void *u,const char *key,uint32_t size,os64_ui_icon_lease_t *out)
{
    (void)u;(void)key;
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
        gallery_icon_pixels[y*32+x]=(x>=2&&y>=2&&x<14&&y<14)?0xc040c0e0:0;
    *out=(os64_ui_icon_lease_t){.pixels=gallery_icon_pixels,.width=size<32?size:32,.height=size<32?size:32,.pitch=32,.generation=1};
    return OS64_UI_ICON_OK;
}
static void gallery_icon_release(void *u,os64_ui_icon_lease_t *l) { (void)u;(void)l; }
static const os64_ui_icon_provider_t gallery_icons={NULL,gallery_icon_acquire,gallery_icon_release};
static void gallery_commands(void *u,const os64_gui_event_t *e)
{
    gallery *a=u;
    if(!gallery_route(a,e,NULL)) (void)os64_ui_dispatch(&a->ui,e);
}
static void gallery_observe(void *u,int64_t handle,const os64_gui_event_t *e)
{
    gallery *a=u;
    if(!a->trace) return;
    if(e->type==OS64_GUI_EVENT_KEY_DOWN||e->type==OS64_GUI_EVENT_KEY_UP)
        os64_printf("gallery key handle=%ld edge=%u ascii=%u scan=%u mods=%u\n",(long)handle,e->type,(unsigned char)e->key.ascii,e->key.scancode,e->key.modifiers);
    if(e->type==OS64_GUI_EVENT_WINDOW_FOCUS)
        os64_printf("gallery focus handle=%ld gained=%u other=%u\n",(long)handle,e->focus.gained,e->focus.other_handle);
}
static void gallery_paint(void *u)
{
    gallery *a=u;
    if(a->ui.quit) return;
    for(unsigned i=0;i<3;++i) (void)os64_ui_actions_refresh(a->views[i]);
    (void)os64_ui_menu_bar_view(a->menus,a->views[a->selected]);
    if(a->trace) {
        size_t windows=0;
        for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) if(a->dispatcher.entries[i].handle) ++windows;
        if(windows!=a->trace_windows) {
            a->trace_windows=windows;
            os64_printf("gallery windows=%lu font-bytes=%lu target-hits=%u,%u\n",(unsigned long)windows,(unsigned long)os64_ui_font_live_bytes(&a->ui),a->targets[0].hits,a->targets[1].hits);
            for(size_t i=0;i<OS64_UI_DISPATCH_WINDOWS;++i) {
                int64_t h=a->dispatcher.entries[i].handle;
                if(h) { os64_gui_surface_t surface;
                    if(a->dispatcher.native.surface(a->dispatcher.native.user,h,&surface)==0)
                        os64_printf("gallery surface handle=%ld width=%u height=%u pitch=%u\n",(long)h,surface.width,surface.height,surface.pitch_px);
                }
            }
        }
    }
    os64_ui_paint(&a->ui);
}
static bool __attribute__((unused)) gallery_attach_menus(gallery *a,int64_t win,const os64_ui_window_backend_t *backend,bool icons)
{
    os64_ui_dispatcher_init(&a->dispatcher,backend);
    a->dispatcher.observe=gallery_observe; a->dispatcher.observe_user=a;
    if(os64_ui_dispatcher_add(&a->dispatcher,win,&a->ui,gallery_commands,gallery_paint,a) ||
       os64_ui_menu_session_create(&a->dispatcher,&a->ui,win,&a->bar,&a->status,icons?&gallery_icons:NULL,&a->menus)) return false;
    os64_ui_menu_hooks(a->menus,gallery_menu_layout,gallery_menu_error,a);
    os64_ui_menu_focus_fallback(a->menus,&a->content);
    if(os64_ui_menu_bar_view(a->menus,a->views[a->selected])) return false;
    a->root.on_context=gallery_context; a->root.context_user=a;
    a->content.on_context=gallery_context; a->content.context_user=a;
    a->editor.w.on_context=gallery_context; a->editor.w.context_user=a;
    for(unsigned i=0;i<2;++i) { a->record[i].on_context=gallery_context; a->record[i].context_user=a; }
    return true;
}

#ifndef UI_ACTION_HOST
int main(int argc,char **argv)
{
    bool icons=true; int32_t window_width=880;
    for(int i=1;i<argc;++i) { if(os64_streq(argv[i],"--no-icons")) icons=false; if(os64_streq(argv[i],"--trace")) g.trace=true; if(os64_streq(argv[i],"--narrow")) window_width=240; }
    int64_t win=os64_gui_window_create("Shared menu toolkit gallery",40,10,window_width,740,0);
    if(win<=0) { os64_printf("actiongallery: GUI unavailable (%ld)\n",(long)win); return 1; }
    if(os64_draw_ctx_init(&g.draw,win)!=0) { os64_gui_window_destroy(win); return 1; }
    if(!gallery_build(&g)) { os64_printf("actiongallery: initialization refused\n"); gallery_dispose(&g); os64_gui_window_destroy(win); return 1; }
    if(!gallery_attach_menus(&g,win,NULL,icons)) { gallery_dispose(&g); os64_gui_window_destroy(win); return 1; }
    (void)os64_ui_font_follow(&g.ui);
    os64_ui_paint(&g.ui);
    while(!g.ui.quit) if(os64_ui_dispatcher_turn(&g.dispatcher,true)<0) break;
    if(!gallery_dispose(&g)) {
        os64_printf("actiongallery: native popup destruction refused; exiting\n");
        /* Process exit releases native windows. Keep borrowed toolkit state
         * intact rather than freeing it underneath a surviving registration. */
        return 1;
    }
    if(g.trace) os64_printf("gallery exit font-bytes=%lu refs=%u,%u hits=%u,%u\n",(unsigned long)os64_ui_font_live_bytes(&g.ui),g.targets[0].refs,g.targets[1].refs,g.targets[0].hits,g.targets[1].hits);
    (void)os64_ui_dispatcher_destroy(&g.dispatcher,win);
    return 0;
}
#endif
