#include "os64/ui_toolbar.h"
#include "os64/str.h"
#include "ui_internal.h"
#include "ui_message.h"

static void click(os64_ui_widget_t *w, void *user)
{
    os64_ui_toolbar_t *bar = user;
    for (size_t i=0; i<bar->count; ++i)
        if (w == &bar->buttons[i].w && bar->command) {
            bar->command(w, bar->specs[i].id, bar->user);
            return;
        }
}
static void paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *theme)
{ os64_ui_panel_class.paint(w, ctx, theme); }
static void destroy(os64_ui_widget_t *w)
{ os64_ui_toolbar_destroy((os64_ui_toolbar_t *)w); }
/* Font reface drops child caption runs, not the command registry or images. */
static void reface(os64_ui_widget_t *w) { (void)w; }
static const os64_ui_class_t toolbar_class = {
    .name="toolbar", .paint=paint, .destroy=destroy, .reface=reface, .paints_part=true
};
void os64_ui_toolbar_init(os64_ui_toolbar_t *bar)
{
    *bar = (os64_ui_toolbar_t){0};
    bar->w.cls = &toolbar_class;
}
os64_ui_status_code_t os64_ui_toolbar_bind(os64_ui_toolbar_t *bar,
    os64_ui_actions_t *actions, const os64_ui_toolbar_entry_t *entries, size_t count,
    void (*command)(os64_ui_widget_t *, uint32_t, void *), void *user)
{
    if (!bar || !actions || (count && (!entries || !command))) return OS64_UI_INVALID;
    if (bar->actions || bar->w.first_child) return OS64_UI_DUPLICATE;
    if (count > OS64_UI_TOOLBAR_MAX) return OS64_UI_LIMIT;
    /* Validate before retaining anything or adding children to the tree. */
    for (size_t i=0; i<count; ++i) {
        os64_ui_action_spec_t spec;
        if (os64_ui_action_spec(actions, entries[i].action, &spec) != OS64_UI_OK ||
            spec.kind != OS64_UI_ACTION_COMMAND) return OS64_UI_INVALID;
        for (size_t j=0; j<i; ++j)
            if (entries[j].action == entries[i].action) return OS64_UI_DUPLICATE;
        size_t length;
        os64_ui_status_code_t rc = ui_message_length(
            entries[i].label ? entries[i].label : spec.label, sizeof(bar->labels[i]), &length);
        if (rc != OS64_UI_OK) return rc;
        if (!length) return OS64_UI_INVALID;
    }
    os64_ui_status_code_t rc = os64_ui_actions_retain(actions);
    if (rc != OS64_UI_OK) return rc;
    bar->actions = actions; bar->command = command; bar->user = user;
    bar->count = count;
    for (size_t i=0; i<count; ++i) {
        (void)os64_ui_action_spec(actions, entries[i].action, &bar->specs[i]);
        os64_strcopy(bar->labels[i], sizeof(bar->labels[i]),
            entries[i].label ? entries[i].label : bar->specs[i].label);
        bar->groups[i] = entries[i].group;
        os64_ui_icon_button(&bar->buttons[i], bar->labels[i], click, bar);
        os64_ui_add_child(&bar->w, &bar->buttons[i].w);
    }
    return OS64_UI_OK;
}
void os64_ui_toolbar_destroy(os64_ui_toolbar_t *bar)
{
    if (!bar) return;
    for (size_t i=0; i<bar->count; ++i) {
        os64_ui_icon_button_clear(&bar->buttons[i]);
        bar->buttons[i].w.on_click = NULL;
    }
    if (bar->actions) os64_ui_actions_release(bar->actions);
    bar->actions = NULL; bar->command = NULL;
    bar->count = 0;
}
os64_ui_icon_status_t os64_ui_toolbar_icons(os64_ui_toolbar_t *bar,
    const os64_ui_icon_provider_t *provider)
{
    if (!bar || !bar->actions) return OS64_UI_ICON_INVALID;
    os64_ui_icon_status_t result = OS64_UI_ICON_OK;
    for (size_t i=0; i<bar->count; ++i) {
        if (!bar->specs[i].icon) continue;
        os64_ui_icon_status_t rc = os64_ui_icon_button_image(&bar->buttons[i], provider,
                                                           bar->specs[i].icon);
        if (result == OS64_UI_ICON_OK) result = rc;
    }
    return result;
}
os64_ui_status_code_t os64_ui_toolbar_refresh(os64_ui_toolbar_t *bar,
    os64_ui_action_view_t *view)
{
    if (!bar || !bar->actions) return OS64_UI_INVALID;
    os64_ui_status_code_t rc = os64_ui_action_view_registry(view, bar->actions) ?
        os64_ui_actions_refresh(view) : OS64_UI_INVALID;
    for (size_t i=0; i<bar->count; ++i) {
        os64_ui_action_state_t state = {0};
        bool enabled = rc == OS64_UI_OK &&
            os64_ui_action_snapshot(view, bar->specs[i].id, &state) == OS64_UI_OK && state.enabled;
        if (bar->w.ui) os64_ui_set_enabled(bar->w.ui, &bar->buttons[i].w, enabled);
        else bar->buttons[i].w.disabled = !enabled;
    }
    return rc;
}
uint32_t os64_ui_toolbar_help(const os64_ui_toolbar_t *bar, const char **text)
{
    if (text) *text = NULL;
    if (!bar || !bar->actions || !bar->w.ui || !os64_ui_widget_enabled(&bar->w)) return 0;
    size_t picked = bar->count;
    for (size_t i=0; i<bar->count; ++i)
        if (bar->w.ui->focus == &bar->buttons[i].w) picked = i;
    for (size_t i=0; i<bar->count; ++i)
        if (bar->w.ui->hover == &bar->buttons[i].w) picked = i;
    if (picked == bar->count || !os64_ui_widget_enabled(&bar->buttons[picked].w)) return 0;
    if (text) *text = bar->specs[picked].help;
    return bar->specs[picked].id;
}
os64_font_status_t os64_ui_toolbar_measure(os64_ui_toolbar_t *bar, os64_ui_t *ui,
    int32_t left, int32_t top, int32_t width, os64_ui_toolbar_plan_t *out)
{
    if (!bar || !ui || !out) return OS64_FONT_BAD_ARGUMENT;
    if (width <= 0 || (int64_t)left + width > INT32_MAX || ui->theme.gap < 0)
        return OS64_FONT_LIMIT;
    os64_ui_toolbar_plan_t plan = {.owner=bar, .count=bar->count,
        .bounds={left,top,width,0}};
    int64_t x=left, y=top, right=(int64_t)left+width, row_height=0;
    int32_t gap=ui->theme.gap;
    for (size_t i=0; i<bar->count; ++i) {
        os64_ui_widget_t *w = &bar->buttons[i].w;
        os64_font_role_t role = ui_role(w, OS64_FONT_ROLE_UI);
        int32_t caption=0;
        os64_font_status_t rc = os64_ui_text_measure(ui, role, w->text,
            os64_strlen(w->text), &caption);
        if (rc != OS64_FONT_OK) return rc;
        int64_t bw=(int64_t)caption+12;
        if (bw < 48) bw=48;
        if (bw > width) return OS64_FONT_LIMIT;
        int64_t bh=24+(int64_t)os64_ui_font_row_height(ui, role)+10;
        int64_t separation=bar->groups[i] ? 2*(int64_t)gap : gap;
        if (i && x+separation+bw > right) {
            x=left; y+=row_height+gap; row_height=0;
        } else if (i) x+=separation;
        if (bh <= 0 || y+bh > INT32_MAX || y+bh-top > INT32_MAX) return OS64_FONT_LIMIT;
        plan.buttons[i]=(os64_gui_rect_t){(int32_t)x,(int32_t)y,(int32_t)bw,(int32_t)bh};
        if (bh > row_height) row_height=bh;
        x+=bw;
    }
    if (bar->count) plan.bounds.h=(int32_t)(y+row_height-top);
    *out=plan;
    return OS64_FONT_OK;
}
void os64_ui_toolbar_apply(os64_ui_toolbar_t *bar, const os64_ui_toolbar_plan_t *plan,
    bool staged)
{
    if (!bar || !plan || plan->owner != bar || plan->count != bar->count) return;
    if (staged) {
        os64_ui_widget_stage_bounds(&bar->w, plan->bounds);
        for (size_t i=0; i<bar->count; ++i)
            os64_ui_widget_stage_bounds(&bar->buttons[i].w, plan->buttons[i]);
    } else {
        if (bar->w.ui) os64_ui_mark_dirty(bar->w.ui, &bar->w);
        bar->w.bounds=plan->bounds;
        for (size_t i=0; i<bar->count; ++i) bar->buttons[i].w.bounds=plan->buttons[i];
        if (bar->w.ui) os64_ui_mark_dirty(bar->w.ui, &bar->w);
    }
}
