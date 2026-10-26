// A one-line choice whose list drops from it (DROPDOWN.md § Step 2): the
// closed box is this widget; the open list is the window's menu session's
// (os64_ui_menu_open_list), which owns the popup, its keys and dismissal.
// Labels and flags stay with the caller and are asked when the box paints
// or the list opens; the session keeps its own copy while the list is open.
#include "ui_internal.h"
#include "os64/ui_menu.h"
#include "os64/str.h"
#include "os64/mem.h"

static const char *label_of(const os64_ui_dropdown_t *d, size_t index, size_t *len)
{
    const char *label = d->label && index < d->count ? d->label(index, d->list_user) : "";
    if (!label) label = "";
    *len = os64_strlen(label);
    return label;
}

static bool enabled_at(const os64_ui_dropdown_t *d, size_t index)
{
    return index < d->count && (!d->enabled || d->enabled(index, d->list_user));
}

// The value changes, and the caller hears of it, only when it is new.
static void choose(os64_ui_t *ui, os64_ui_dropdown_t *d, size_t index)
{
    if (index >= d->count || (int)index == d->selected) return;
    d->selected = (int)index;
    os64_ui_mark_dirty(ui, &d->w);
    if (d->on_change) d->on_change(d, d->list_user);
}

// The session's pick, after the list has closed: a row that is already the
// value is a commit and not a change (Quinn's review, 4).
static void picked(void *user, size_t row)
{
    os64_ui_dropdown_t *d = user;
    choose(d->w.ui, d, row);
}

// The list, in the window's session: the rows as they are now, copied by
// the session at the call, so the arrays here live only for it.
static void open_list(os64_ui_t *ui, os64_ui_dropdown_t *d)
{
    os64_ui_menu_session_t *session = ui->menu;
    if (!session || !d->count) return;
    const char **labels = os64_malloc(d->count * sizeof(*labels));
    bool *enabled = os64_malloc(d->count * sizeof(*enabled));
    if (labels && enabled) {
        for (size_t i = 0; i < d->count; ++i) {
            size_t len;
            labels[i] = label_of(d, i, &len);
            enabled[i] = enabled_at(d, i);
        }
        os64_ui_menu_list_t list = {
            .labels = labels, .enabled = enabled, .count = d->count,
            .selected = d->selected >= 0 ? (size_t)d->selected : d->count,
            .min_width = d->w.bounds.w, .widget = &d->w, .pick = picked, .user = d,
        };
        (void)os64_ui_menu_open_list(session, &list, d->w.bounds);
    }
    os64_free(labels);
    os64_free(enabled);
}

// Up and Down on the closed box: the next row it may choose, or none.
static void step(os64_ui_t *ui, os64_ui_dropdown_t *d, int delta)
{
    int64_t at = d->selected;
    for (;;) {
        at += delta;
        if (at < 0 || at >= (int64_t)d->count) return;
        if (enabled_at(d, (size_t)at)) break;
    }
    choose(ui, d, (size_t)at);
}

static void dropdown_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    if (w->bounds.w < 4 || w->bounds.h < 4) return;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->field_bg);
    os64_draw_rect(&ctx->surf, w->bounds, w->focused ? t->focus_ring : t->field_border);
    // The arrow, a small down-pointing triangle at the right.
    int32_t ax = w->bounds.x + w->bounds.w - 12, ay = w->bounds.y + w->bounds.h / 2 - 2;
    for (int32_t i = 0; i < 4; ++i)
        os64_draw_hline(&ctx->surf, ax - 3 + i, ay + i, 7 - 2 * i, t->field_fg);
    if (d->selected < 0) return;
    size_t len = 0;
    const char *label = label_of(d, (size_t)d->selected, &len);
    os64_ui_t *ui = os64_ui_of(w);
    os64_font_role_t role = ui_role(w, OS64_FONT_ROLE_UI);
    os64_gui_rect_t clip = {w->bounds.x + 2, w->bounds.y + 2, w->bounds.w - 24, w->bounds.h - 4};
    if (clip.w <= 0 || clip.h <= 0) return;
    int32_t y = w->bounds.y + (w->bounds.h - os64_ui_font_row_height(ui, role)) / 2;
    os64_ui_draw_text(ui, &d->run, role, &ctx->surf, clip, clip.x + 4, y, label, len, t->field_fg, t->field_bg);
}

// A key with no character: a HID keyboard's arrow or Enter arrives as a
// scancode, not as the ESC [ burst a PS/2 one sends (which
// os64_ui_decode_key reads), so it is matched as the menu session matches
// its named keys.
static ui_key_t named_key(const os64_gui_event_t *ev)
{
    static const struct { uint16_t key; ui_key_t decoded; } kNamed[] = {
        {OS64_UI_KEY_UP, K_UP}, {OS64_UI_KEY_DOWN, K_DOWN}, {OS64_UI_KEY_ENTER, K_ENTER},
    };
    uint8_t mods = ev->key.modifiers & (OS64_GUI_MOD_SHIFT | OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT);
    if (ev->key.ascii) return K_NONE;
    for (size_t i = 0; i < sizeof(kNamed) / sizeof(kNamed[0]); ++i)
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){kNamed[i].key, mods}, ev)) return kNamed[i].decoded;
    return K_NONE;
}

static void dropdown_cancel(os64_ui_widget_t *w)
{
    ((os64_ui_dropdown_t *)w)->seq = 0;
    w->pressed = false;
}

static bool dropdown_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT) return false;
        os64_ui_set_focus(ui, w);
        open_list(ui, d);
        return true;
    case OS64_GUI_EVENT_KEY_DOWN: {
        if (ui->grab || (ev->key.modifiers & OS64_GUI_MOD_CTRL)) {
            d->seq = 0;
            return false;
        }
        char ch = 0;
        ui_key_t key = os64_ui_decode_key(&d->seq, ev, &ch);
        if (key == K_NONE && !d->seq) key = named_key(ev);
        if (ev->key.modifiers & OS64_GUI_MOD_ALT) {
            if (key == K_NONE) return true;     // a sequence still arriving
            if (key != K_DOWN) return false;
            open_list(ui, d);
            return true;
        }
        switch (key) {
        case K_UP: step(ui, d, -1); return true;
        case K_DOWN: step(ui, d, 1); return true;
        case K_ENTER: open_list(ui, d); return true;
        case K_CHAR:
            if (ch != ' ') return false;
            open_list(ui, d);
            return true;
        default: return key == K_NONE;
        }
    }
    default: return false;
    }
}

static os64_font_status_t dropdown_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    os64_ui_run_release(d->run_staged);
    d->run_staged = NULL;
    if (d->selected < 0) return OS64_FONT_OK;
    size_t len = 0;
    const char *label = label_of(d, (size_t)d->selected, &len);
    return os64_ui_run_layout(ui, ui_role(w, OS64_FONT_ROLE_UI), label, len, &d->run_staged);
}

static void dropdown_commit(os64_ui_widget_t *w)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    os64_ui_run_release(d->run);
    d->run = d->run_staged;
    d->run_staged = NULL;
}

static void dropdown_discard(os64_ui_widget_t *w)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    os64_ui_run_release(d->run_staged);
    d->run_staged = NULL;
}

// The list first: detached, no pick can reach this widget, which may be
// freed the moment this returns (DROPDOWN.md, contract 1); then its runs.
static void dropdown_destroy(os64_ui_widget_t *w)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    if (w->ui && w->ui->menu) os64_ui_menu_list_detach(w->ui->menu, d);
    os64_ui_run_release(d->run);
    os64_ui_run_release(d->run_staged);
    d->run = d->run_staged = NULL;
}

static void dropdown_reface(os64_ui_widget_t *w)
{
    os64_ui_dropdown_t *d = (os64_ui_dropdown_t *)w;
    os64_ui_run_release(d->run);
    os64_ui_run_release(d->run_staged);
    d->run = d->run_staged = NULL;
}

static const os64_ui_class_t kDropdownClass = {"dropdown", dropdown_paint, dropdown_event, dropdown_cancel,
                                              dropdown_prepare, dropdown_commit, dropdown_discard,
                                              dropdown_destroy, NULL, dropdown_reface, false};

void os64_ui_dropdown(os64_ui_dropdown_t *d, size_t count, const char *(*label)(size_t, void *),
                      void (*on_change)(os64_ui_dropdown_t *, void *), void *user)
{
    *d = (os64_ui_dropdown_t){0};
    d->w.cls = &kDropdownClass;
    d->w.focusable = true;
    d->count = count;
    d->selected = -1;
    d->label = label;
    d->on_change = on_change;
    d->list_user = user;
}

void os64_ui_dropdown_set(os64_ui_t *ui, os64_ui_dropdown_t *d, size_t count, int selected)
{
    int value = selected >= 0 && (size_t)selected < count ? selected : -1;
    if (count == d->count && value == d->selected) return;
    if (ui && ui->menu) os64_ui_menu_list_detach(ui->menu, d);
    d->count = count;
    d->selected = value;
    if (ui) os64_ui_mark_dirty(ui, &d->w);
}
