// A scrollable selection control; labels and collection lifetime stay with
// the caller. Selection changes do not imply loading or applying a document.
#include "ui_internal.h"
#include "os64/str.h"
#include "os64/mem.h"

// The row's own pitch: the UI face's line box plus the padding that has
// always separated rows. It is stamped on the listbox rather than derived
// here because the questions that ask for it are handed a THEME, which
// cannot carry a font; an unstamped listbox reads as the bitmap cell, which
// is the same answer the builtin set gives.
static int row_height(const os64_ui_listbox_t *list, const os64_ui_theme_t *theme)
{
    return (list->row_h > 0 ? list->row_h : theme->font_h) + 8;
}

static void list_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
    ((os64_ui_listbox_t *)w)->row_h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
}

// ── staging the visible rows ────────────────────────────────────────────────
// A caption is one string a widget owns; a list's rows are as many strings as
// fit, fetched from the application every time anyone asks. So the run array
// is sized by what the box can SHOW, and each slot is checked against the
// label it is supposed to be before it is trusted.

static const char *row_text(const os64_ui_listbox_t *list, size_t index, size_t *len)
{
    const char *label = list->label ? list->label(index, list->list_user) : "";
    if (!label) label = "";
    *len = os64_strlen(label);
    return label;
}

static void free_runs(void ***runs, size_t *count)
{
    for (size_t i = 0; i < *count; ++i)
        os64_ui_run_release((*runs)[i]);
    os64_free(*runs);
    *runs = NULL;
    *count = 0;
}

static os64_font_status_t list_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    // Size the staging from the candidate's pitch, not the installed one:
    // a taller face shows fewer rows, and staging the old count would
    // either over-allocate or leave the first paint short.
    int32_t pitch = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) + 8;
    int32_t fits = pitch > 0 ? (w->bounds.h - 4) / pitch : 0;
    size_t visible = fits > 0 ? (size_t)fits : 0;
    if (visible > list->count - (list->top < list->count ? list->top : list->count))
        visible = list->count - (list->top < list->count ? list->top : list->count);

    free_runs(&list->row_runs_staged, &list->row_runs_staged_count);
    if (!visible)
        return OS64_FONT_OK;

    void **runs = os64_malloc(visible * sizeof(*runs));
    if (!runs)
        return OS64_FONT_NO_MEMORY;
    for (size_t i = 0; i < visible; ++i)
        runs[i] = NULL;

    for (size_t i = 0; i < visible; ++i) {
        size_t len = 0;
        const char *label = row_text(list, list->top + i, &len);
        os64_font_status_t status =
            os64_ui_run_layout(ui, OS64_FONT_ROLE_UI, label, len, &runs[i]);
        if (status != OS64_FONT_OK) {
            for (size_t j = 0; j < i; ++j)
                os64_ui_run_release(runs[j]);
            os64_free(runs);
            return status;
        }
    }
    list->row_runs_staged = runs;
    list->row_runs_staged_count = visible;
    return OS64_FONT_OK;
}

static void list_commit(os64_ui_widget_t *w)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    free_runs(&list->row_runs, &list->row_run_count);
    list->row_runs = list->row_runs_staged;
    list->row_run_count = list->row_runs_staged_count;
    list->row_runs_staged = NULL;
    list->row_runs_staged_count = 0;
}

static void list_discard(os64_ui_widget_t *w)
{
    os64_ui_listbox_t *list = (os64_ui_listbox_t *)w;
    free_runs(&((os64_ui_listbox_t *)w)->row_runs_staged,
              &list->row_runs_staged_count);
}

int os64_ui_listbox_rows(const os64_ui_listbox_t *list, const os64_ui_theme_t *theme)
{
    int rows = (list->w.bounds.h - 4) / row_height(list, theme);
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
    int rows = os64_ui_listbox_rows(list, t), height = row_height(list, t);
    for (int row = 0; row < rows && list->top + (size_t)row < list->count; ++row) {
        size_t index = list->top + (size_t)row;
        bool selected = list->selected >= 0 && index == (size_t)list->selected;
        uint32_t bg = selected ? t->text_sel_bg : t->field_bg;
        uint32_t fg = selected ? t->text_sel_fg : t->field_fg;
        os64_gui_rect_t rect = {w->bounds.x + 2, w->bounds.y + 2 + row * height,
                                w->bounds.w - 4, height};
        os64_draw_fill_rect(&ctx->surf, rect, bg);
        size_t label_len = 0;
        const char *label = row_text(list, index, &label_len);
        int inset = 6;
        if (list->swatch && rect.w >= 28) {
            os64_gui_rect_t chip = {rect.x + 6, rect.y + 5, 16, 14};
            os64_draw_fill_rect(&ctx->surf, chip, list->swatch(index, list->list_user));
            os64_draw_rect(&ctx->surf, chip, fg);
            inset = 28;
        }
        // The staged run for this slot when it still says what the row
        // says; the draw re-lays-out anything that has changed since.
        void **slot = (size_t)row < list->row_run_count ? &list->row_runs[row] : NULL;
        os64_ui_draw_text(os64_ui_of(w), slot, OS64_FONT_ROLE_UI, &ctx->surf, rect,
                          rect.x + inset, rect.y + 4, label, label_len, fg, bg);
    }
}

static int pointer_index(const os64_ui_listbox_t *list, const os64_ui_theme_t *t,
                          int x, int y)
{
    os64_gui_rect_t r = list->w.bounds;
    if (x < r.x + 2 || x >= r.x + r.w - 2 || y < r.y + 2) return -1;
    int row = (y - r.y - 2) / row_height(list, t);
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

static const os64_ui_class_t kListClass = {"listbox", list_paint, list_event, list_cancel,
                                          list_prepare, list_commit, list_discard,
                                          list_metrics};

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
