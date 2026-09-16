// A scrollable selection control; labels and collection lifetime stay with
// the caller. Selection changes do not imply loading or applying a document.
#include "ui_internal.h"
#include "os64/str.h"

static int row_height(const os64_ui_theme_t *theme)
{
    return theme->font_h + 8;
}

int os64_ui_listbox_rows(const os64_ui_listbox_t *list, const os64_ui_theme_t *theme)
{
    int rows = (list->w.bounds.h - 4) / row_height(theme);
    return rows > 0 ? rows : 0;
}

void os64_ui_listbox_scroll_to(os64_ui_t *ui, os64_ui_listbox_t *list, size_t top)
{
    size_t rows = (size_t)os64_ui_listbox_rows(list, &ui->theme);
    size_t max = list->count > rows ? list->count - rows : 0;
    list->top = top < max ? top : max;
    os64_ui_mark_dirty(ui, &list->w);
}

void os64_ui_listbox_set(os64_ui_t *ui, os64_ui_listbox_t *list, size_t count, int selected)
{
    list->count = count;
    list->selected = selected >= 0 && (size_t)selected < count ? selected : -1;
    size_t top = list->top;
    size_t rows = (size_t)os64_ui_listbox_rows(list, &ui->theme);
    if (list->selected >= 0) {
        size_t index = (size_t)list->selected;
        if (index < top) top = index;
        else if (rows && index >= top + rows) top = index - rows + 1;
    }
    os64_ui_listbox_scroll_to(ui, list, top);
}

static void list_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    if (w->bounds.w < 4 || w->bounds.h < 4) return;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->field_bg);
    os64_draw_rect(&ctx->surf, w->bounds, w->focused ? t->focus_ring : t->field_border);
    int rows = os64_ui_listbox_rows(list, t), height = row_height(t);
    for (int row = 0; row < rows && list->top + (size_t)row < list->count; ++row) {
        size_t index = list->top + (size_t)row;
        bool selected = list->selected >= 0 && index == (size_t)list->selected;
        uint32_t bg = selected ? t->text_sel_bg : t->field_bg;
        uint32_t fg = selected ? t->text_sel_fg : t->field_fg;
        os64_gui_rect_t rect = {w->bounds.x + 2, w->bounds.y + 2 + row * height,
                                w->bounds.w - 4, height};
        os64_draw_fill_rect(&ctx->surf, rect, bg);
        const char *label = list->label ? list->label(index, list->list_user) : "";
        if (!label) label = "";
        os64_draw_text_clipped(&ctx->surf, rect, rect.x + 6, rect.y + 4,
                               label, os64_strlen(label), fg, bg);
    }
}

static int pointer_index(const os64_ui_listbox_t *list, const os64_ui_theme_t *t,
                          int x, int y)
{
    os64_gui_rect_t r = list->w.bounds;
    if (x < r.x + 2 || x >= r.x + r.w - 2 || y < r.y + 2) return -1;
    int row = (y - r.y - 2) / row_height(t);
    size_t index = list->top + (size_t)row;
    return row < os64_ui_listbox_rows(list, t) && index < list->count ? (int)index : -1;
}

static void choose(os64_ui_t *ui, os64_ui_listbox_t *list, int index)
{
    if (index < 0 || (size_t)index >= list->count || index == list->selected) return;
    os64_ui_listbox_set(ui, list, list->count, index);
    if (list->on_change) list->on_change(list, list->list_user);
}

static void list_cancel(os64_ui_widget_t *w)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    list->pressed_index = -1;
    list->seq = 0;
    w->pressed = false;
}

static bool list_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT) return false;
        os64_ui_set_focus(ui, w);
        list->pressed_index = pointer_index(list, &ui->theme, ev->mouse.x, ev->mouse.y);
        w->pressed = true;
        return true;
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT || !w->pressed) return false;
        int index = pointer_index(list, &ui->theme, ev->mouse.x, ev->mouse.y);
        if (index == list->pressed_index) choose(ui, list, index);
        list_cancel(w);
        return true;
    }
    case OS64_GUI_EVENT_KEY_DOWN: {
        if (ui->grab || (ev->key.modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT))) {
            list->seq = 0;
            return false;
        }
        char ch;
        ui_key_t key = os64_ui_decode_key(&list->seq, ev, &ch);
        int index = list->selected;
        int rows = os64_ui_listbox_rows(list, &ui->theme);
        switch (key) {
        case K_UP: --index; break;
        case K_DOWN: ++index; break;
        case K_HOME: index = 0; break;
        case K_END: index = (int)list->count - 1; break;
        case K_PGUP: index -= rows; break;
        case K_PGDN: index += rows; break;
        default: return key == K_NONE;
        }
        if (index < 0) index = 0;
        if ((size_t)index >= list->count) index = (int)list->count - 1;
        choose(ui, list, index);
        return true;
    }
    default: return false;
    }
}

static const os64_ui_class_t kListClass = {"listbox", list_paint, list_event, list_cancel};

void os64_ui_listbox(os64_ui_listbox_t *list, size_t count,
                     const char *(*label)(size_t, void *),
                     void (*on_change)(os64_ui_listbox_t *, void *), void *user)
{
    *list = (os64_ui_listbox_t){0};
    list->w.cls = &kListClass;
    list->w.focusable = true;
    list->count = count;
    list->selected = list->pressed_index = -1;
    list->label = label;
    list->on_change = on_change;
    list->list_user = user;
}
