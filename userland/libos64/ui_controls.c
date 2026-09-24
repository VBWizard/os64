// Checkbox and integer slider: shared interaction, theme-owned paint.
#include "ui_internal.h"

static size_t caption_length(const char *s)
{
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void checkbox_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                           const os64_ui_theme_t *t)
{
    os64_ui_checkbox_t *cb = (os64_ui_checkbox_t *)w;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
    int32_t size = t->checkbox_size;
    if (size > w->bounds.w) size = w->bounds.w;
    if (size > w->bounds.h) size = w->bounds.h;
    if (size <= 0) return;
    os64_ui_widget_t box = *w;
    box.text = "";
    box.bounds = (os64_gui_rect_t){w->bounds.x,
        w->bounds.y + (w->bounds.h - size) / 2, size, size};
    os64_ui_button_class.paint(&box, ctx, t);
    if (cb->checked && size >= 8) {
        // A block check remains legible in the bitmap renderer at small sizes.
        int32_t x = box.bounds.x + 3, y = box.bounds.y + size / 2;
        os64_draw_fill_rect(&ctx->surf, (os64_gui_rect_t){x, y, 2, 3}, t->button_fg);
        for (int32_t i = 0; i < size - 6; ++i)
            os64_draw_fill_rect(&ctx->surf,
                (os64_gui_rect_t){x + 2 + i, y + 1 - i / 2, 1, 2}, t->button_fg);
    }
    os64_ui_t *ui = os64_ui_of(w);
    os64_ui_draw_text(ui, &w->run, OS64_FONT_ROLE_UI, &ctx->surf, w->bounds,
        w->bounds.x + size + t->gap,
        w->bounds.y + (w->bounds.h - os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI)) / 2,
        w->text ? w->text : "", caption_length(w->text), t->label_fg, t->panel_bg);
}

static void checkbox_click(os64_ui_widget_t *w, void *user)
{
    (void)user;
    os64_ui_checkbox_t *cb = (os64_ui_checkbox_t *)w;
    cb->checked = !cb->checked;
    if (cb->on_change) cb->on_change(cb, cb->check_user);
}

static bool checkbox_event(os64_ui_widget_t *w, os64_ui_t *ui,
                           const os64_gui_event_t *ev)
{
    return os64_ui_button_class.event(w, ui, ev);
}

// The box and its caption share a row, so the control is one row tall plus
// the theme's padding — the same arithmetic a button uses.
static void checkbox_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
    w->natural_h = os64_ui_control_min_height(ui);
}

const os64_ui_class_t os64_ui_checkbox_class = {
    "checkbox", checkbox_paint, checkbox_event, NULL,
    os64_ui_stage_caption, os64_ui_commit_caption, os64_ui_discard_caption,
    NULL, checkbox_metrics
};

void os64_ui_checkbox(os64_ui_checkbox_t *cb, const char *text, bool checked,
                     void (*on_change)(os64_ui_checkbox_t *, void *), void *user)
{
    *cb = (os64_ui_checkbox_t){0};
    os64_ui_button(&cb->w, text, checkbox_click, NULL);
    cb->w.cls = &os64_ui_checkbox_class;
    cb->checked = checked;
    cb->on_change = on_change;
    cb->check_user = user;
}

void os64_ui_checkbox_set(os64_ui_t *ui, os64_ui_checkbox_t *cb, bool checked)
{
    if (cb->checked == checked) return;
    cb->checked = checked;
    os64_ui_mark_dirty(ui, &cb->w);
}

static int32_t slider_clamp(const os64_ui_slider_t *sl, int64_t value)
{
    if (value < sl->min) return sl->min;
    if (value > sl->max) return sl->max;
    return (int32_t)value;
}

static int32_t thumb_width(const os64_ui_slider_t *sl, const os64_ui_theme_t *t)
{
    int32_t width = t->scroll_w;
    if (width < 4) width = 4;
    if (width > 40) width = 40;
    if (width > sl->w.bounds.w) width = sl->w.bounds.w;
    return width > 0 ? width : 0;
}

static int32_t thumb_pos(const os64_ui_slider_t *sl, int32_t travel)
{
    int64_t range = (int64_t)sl->max - sl->min;
    return range > 0 && travel > 0 ?
        (int32_t)(((int64_t)sl->value - sl->min) * travel / range) : 0;
}

static void slider_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                         const os64_ui_theme_t *t)
{
    os64_ui_slider_t *sl = (os64_ui_slider_t *)w;
    if (w->bounds.w <= 0 || w->bounds.h <= 0) return;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
    int32_t h = t->slider_track_h;
    if (h < 0) h = 0;
    if (h > w->bounds.h) h = w->bounds.h;
    os64_gui_rect_t track = {w->bounds.x,
        w->bounds.y + (w->bounds.h - h) / 2, w->bounds.w, h};
    os64_draw_fill_rect(&ctx->surf, track, t->scroll_track);
    // Disabled paint shares the panel background; retain the track's shape
    // with the same muted outline used by the disabled controls.
    if (!os64_ui_widget_enabled(w))
        os64_draw_rect(&ctx->surf, track, t->disabled_fg);
    os64_ui_widget_t thumb = *w;
    thumb.text = "";
    thumb.bounds.w = thumb_width(sl, t);
    thumb.bounds.x += thumb_pos(sl, w->bounds.w - thumb.bounds.w);
    os64_ui_button_class.paint(&thumb, ctx, t);
}

void os64_ui_slider_set(os64_ui_t *ui, os64_ui_slider_t *sl, int32_t value)
{
    value = slider_clamp(sl, value);
    if (sl->value == value) return;
    sl->value = value;
    os64_ui_mark_dirty(ui, &sl->w);
}

static void slider_change(os64_ui_t *ui, os64_ui_slider_t *sl, int64_t value)
{
    int32_t clamped = slider_clamp(sl, value);
    if (sl->value == clamped) return;
    os64_ui_slider_set(ui, sl, clamped);
    if (sl->on_change) sl->on_change(sl, sl->slider_user);
}

static void slider_pointer(os64_ui_t *ui, os64_ui_slider_t *sl, int32_t x)
{
    int32_t travel = sl->w.bounds.w - thumb_width(sl, &ui->theme);
    if (travel <= 0) return;
    int64_t pos = (int64_t)x - sl->w.bounds.x - sl->drag_offset;
    if (pos < 0) pos = 0;
    if (pos > travel) pos = travel;
    int64_t range = (int64_t)sl->max - sl->min;
    slider_change(ui, sl, sl->min + (pos * range + travel / 2) / travel);
}

static void slider_cancel(os64_ui_widget_t *w)
{
    os64_ui_slider_t *sl = (os64_ui_slider_t *)w;
    sl->drag_offset = -1;
    sl->seq = 0;
}

static bool slider_event(os64_ui_widget_t *w, os64_ui_t *ui,
                         const os64_gui_event_t *ev)
{
    os64_ui_slider_t *sl = (os64_ui_slider_t *)w;
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT) return false;
        os64_ui_set_focus(ui, w);
        int32_t width = thumb_width(sl, &ui->theme);
        int64_t offset = (int64_t)ev->mouse.x - w->bounds.x -
                         thumb_pos(sl, w->bounds.w - width);
        sl->drag_offset = offset >= 0 && offset < width ? (int32_t)offset : width / 2;
        sl->drag_x = ev->mouse.x;
        w->pressed = true;
        os64_ui_mark_dirty(ui, w);
        if (offset < 0 || offset >= width) slider_pointer(ui, sl, ev->mouse.x);
        return true;
    }
    case OS64_GUI_EVENT_MOUSE_MOVE:
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
        if (sl->drag_offset < 0) return false;
        if (sl->drag_x != ev->mouse.x) {
            sl->drag_x = ev->mouse.x;
            slider_pointer(ui, sl, ev->mouse.x);
        }
        if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) {
            sl->drag_offset = -1;
            w->pressed = false;
            os64_ui_mark_dirty(ui, w);
        }
        return true;
    case OS64_GUI_EVENT_KEY_DOWN: {
        if (ui->grab || (ev->key.modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT))) {
            sl->seq = 0;
            return false;
        }
        char ch = 0;
        ui_key_t key = os64_ui_decode_key(&sl->seq, ev, &ch);
        switch (key) {
        case K_LEFT: case K_DOWN: slider_change(ui, sl, (int64_t)sl->value - sl->step); break;
        case K_RIGHT: case K_UP: slider_change(ui, sl, (int64_t)sl->value + sl->step); break;
        case K_HOME: slider_change(ui, sl, sl->min); break;
        case K_END: slider_change(ui, sl, sl->max); break;
        default: return key == K_NONE;
        }
        return true;
    }
    default: return false;
    }
}

const os64_ui_class_t os64_ui_slider_class = {
    "slider", slider_paint, slider_event, slider_cancel, NULL, NULL, NULL, NULL, NULL
};

void os64_ui_slider(os64_ui_slider_t *sl, int32_t min, int32_t max,
                   int32_t step, int32_t value,
                   void (*on_change)(os64_ui_slider_t *, void *), void *user)
{
    *sl = (os64_ui_slider_t){0};
    sl->w.cls = &os64_ui_slider_class;
    sl->w.focusable = true;
    sl->min = min;
    sl->max = max < min ? min : max;
    sl->step = step < 1 ? 1 : step;
    sl->value = slider_clamp(sl, value);
    sl->drag_offset = -1;
    sl->on_change = on_change;
    sl->slider_user = user;
}
