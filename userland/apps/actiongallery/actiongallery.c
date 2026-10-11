/* Shared-action/status gallery: app-owned targets and visibility policy,
 * ordinary libui controls and the configured Interface font. */
#include "os64/ui.h"
#include "os64/ui_action.h"
#include "os64/ui_status.h"
#include "os64/fmt.h"
#include "os64/mem.h"

enum { RECORD=1, PANEL, STATUS, HELP, CLEAR_HELP, CLEAR_NOTICE, QUIT };
typedef struct { uint64_t generation; unsigned refs, hits; bool enabled; } gallery_target;
typedef struct {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root, heading, instructions, content, panel;
    os64_ui_widget_t select[2], record[2], enable[2], invalidate[2], titles[2], controls[6];
    os64_ui_status_t status;
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
}

/* Both live resize and font adoption use the candidate's row height. */
static void gallery_layout(gallery *a, bool staged)
{
    int32_t pad = a->ui.theme.pad, gap = a->ui.theme.gap;
    int32_t row = os64_ui_control_min_height(&a->ui);
    int32_t width = a->root.bounds.w - 2 * pad;
    if (width < 0) width = 0;
    int32_t col = width > gap ? (width-gap)/2 : 0;
    os64_ui_widget_t *nodes[24];
    os64_gui_rect_t rects[24];
    size_t n = 0;
#define PLACE(w_, x_, y_, width_, height_) do { nodes[n]=(w_); \
    rects[n++]=(os64_gui_rect_t){(x_), (y_), (width_), (height_)}; } while (0)
    int32_t y = pad;
    PLACE(&a->heading, pad, y, width, row); y += row + gap;
    PLACE(&a->instructions, pad, y, width, row); y += row + gap;
    for (unsigned i = 0; i < 2; ++i)
        PLACE(&a->select[i], pad+(int32_t)i*(col+gap), y, col, row);
    y += row + gap;
    PLACE(&a->content, pad, y, width, row); y += row + gap;
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
    int32_t rows=a->panel.hidden?7:11;
    int64_t need=(int64_t)rows*os64_ui_control_min_height(ui) +
        (int64_t)(rows-1)*ui->theme.gap + 2*ui->theme.pad;
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
    default: rc=OS64_UI_INVALID; break;
    }
    return (os64_ui_action_result_t){rc==OS64_UI_OK ? OS64_UI_OK :
        rc==OS64_UI_STALE ? OS64_UI_REFUSED : OS64_UI_ERROR, rc};
}
static void gallery_select_target(os64_ui_widget_t *w, void *user)
{
    gallery *a=user;
    a->selected=w==&a->select[1];
    a->content.text=a->selected ? "Shortcut target: B (Ctrl+D records)" : "Shortcut target: A (Ctrl+D records)";
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
    os64_ui_label(&a->heading, "Actions & status - shared toolkit gallery");
    os64_ui_label(&a->instructions, "Ctrl+D record   Ctrl+P panel   Ctrl+B status");
    os64_ui_button(&a->content,"Shortcut target: A (Ctrl+D records)",NULL,NULL);
    gallery_add(a,&a->root,&a->heading); gallery_add(a,&a->root,&a->instructions);
    for(unsigned i=0;i<2;++i) {
        os64_ui_button(&a->select[i],i?"Shortcut target B":"Shortcut target A",gallery_select_target,a);
        gallery_add(a,&a->root,&a->select[i]);
    }
    gallery_add(a,&a->root,&a->content);
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
    const os64_ui_action_spec_t specs[]={
        {RECORD,OS64_UI_ACTION_COMMAND,0,"Record target", "Invoke the selected target",NULL,0,{'d',OS64_GUI_MOD_CTRL}},
        {PANEL,OS64_UI_ACTION_TOGGLE,0,"Target panel", "Keep target state while hidden",NULL,0,{'p',OS64_GUI_MOD_CTRL}},
        {STATUS,OS64_UI_ACTION_TOGGLE,0,"Status bar", "Keep messages while hidden",NULL,0,{'b',OS64_GUI_MOD_CTRL}},
        {HELP,OS64_UI_ACTION_COMMAND,0,"New help", "Replace help with a new token",NULL,0,{'h',OS64_GUI_MOD_CTRL}},
        {CLEAR_HELP,OS64_UI_ACTION_COMMAND,0,"Clear older help", "Stale clear keeps newer help",NULL,0,{0,0}},
        {CLEAR_NOTICE,OS64_UI_ACTION_COMMAND,0,"Clear notice", "Expose base information",NULL,0,{'n',OS64_GUI_MOD_CTRL}},
        {QUIT,OS64_UI_ACTION_COMMAND,0,"Close gallery", "Defer teardown until dispatch returns",NULL,0,{'q',OS64_GUI_MOD_CTRL}}
    };
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
    os64_ui_shortcut_reset(&a->router);
    a->ui.on_resize=gallery_resize;
    if(os64_ui_font_planner(&a->ui,gallery_plan,gallery_plan_done,gallery_plan_done,a)!=OS64_FONT_OK) return false;
    gallery_layout(a,false); os64_ui_set_focus(&a->ui,&a->content);
    return true;
}
static void gallery_dispose(gallery *a)
{
    os64_ui_cancel_interaction(&a->ui);
    for(unsigned i=0;i<2;++i) (void)os64_ui_action_unbind(&a->record[i]);
    for(unsigned i=0;i<6;++i) (void)os64_ui_action_unbind(&a->controls[i]);
    for(unsigned i=0;i<3;++i) if(a->views[i]) (void)os64_ui_action_view_destroy(a->views[i]);
    if(a->actions) (void)os64_ui_actions_destroy(a->actions);
    (void)os64_ui_font_release(&a->ui);
}
static bool gallery_route(gallery *a, const os64_gui_event_t *ev, os64_ui_action_result_t *out)
{
    bool record = os64_ui_shortcut_match((os64_ui_shortcut_t){'d',OS64_GUI_MOD_CTRL},ev);
    return os64_ui_shortcut_route(&a->router,a->views[record?a->selected:2],ev,out);
}
#ifndef UI_ACTION_HOST
int main(int argc,char **argv)
{
    (void)argc; (void)argv;
    int64_t win=os64_gui_window_create("Actions and status",40,10,880,740,0);
    if(win<=0) { os64_printf("actiongallery: GUI unavailable (%ld)\n",(long)win); return 1; }
    if(os64_draw_ctx_init(&g.draw,win)!=0) { os64_gui_window_destroy(win); return 1; }
    if(!gallery_build(&g)) { os64_printf("actiongallery: initialization refused\n"); gallery_dispose(&g); os64_gui_window_destroy(win); return 1; }
    (void)os64_ui_font_follow(&g.ui);
    os64_ui_paint(&g.ui);
    while(!g.ui.quit) {
        os64_gui_event_t ev;
        if(os64_gui_event_wait(win,&ev)!=1) break;
        os64_ui_action_result_t result;
        if(!gallery_route(&g,&ev,&result))
            os64_ui_dispatch(&g.ui,&ev);
        if(g.ui.quit) break;
        for(unsigned i=0;i<3;++i) (void)os64_ui_actions_refresh(g.views[i]);
        os64_ui_paint(&g.ui);
    }
    gallery_dispose(&g); os64_gui_window_destroy(win);
    return 0;
}
#endif
