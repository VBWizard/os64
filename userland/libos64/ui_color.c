// Native opaque-RGB picker. Integer HSV keeps this control independent of
// floating-point support and exposes its exact selected color to the caller.
#include "ui_internal.h"

static int clamp(int value, int max)
{
    return value < 0 ? 0 : value > max ? max : value;
}

uint32_t os64_ui_color_from_hsv(int hue, int saturation, int value)
{
    hue = clamp(hue, 359);
    saturation = clamp(saturation, 255);
    value = clamp(value, 255);
    int sector = hue / 60, f = hue % 60;
    int p = (value * (255 - saturation) + 127) / 255;
    int q = (value * (255 * 60 - saturation * f) + 7650) / (255 * 60);
    int t = (value * (255 * 60 - saturation * (60 - f)) + 7650) / (255 * 60);
    int r = 0, g = 0, b = 0;
    switch (sector) {
    case 0: r = value; g = t; b = p; break;
    case 1: r = q; g = value; b = p; break;
    case 2: r = p; g = value; b = t; break;
    case 3: r = p; g = q; b = value; break;
    case 4: r = t; g = p; b = value; break;
    default: r = value; g = p; b = q; break;
    }
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void os64_ui_colorpicker_set(os64_ui_t *ui, os64_ui_colorpicker_t *p, uint32_t color)
{
    p->color = color | 0xff000000u;
    int r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    int max = r > g ? r : g; if (b > max) max = b;
    int min = r < g ? r : g; if (b < min) min = b;
    int delta = max - min;
    p->value = max;
    p->saturation = max ? (delta * 255 + max / 2) / max : 0;
    // Preserve the hue of an achromatic color so dragging up from gray or
    // black does not unexpectedly reset the user's chosen hue.
    if (delta) {
        int h = max == r ? 60 * (g - b) / delta :
                max == g ? 120 + 60 * (b - r) / delta : 240 + 60 * (r - g) / delta;
        p->hue = h < 0 ? h + 360 : h;
    }
    if (ui) os64_ui_mark_dirty(ui, &p->w);
}

static bool areas(const os64_ui_widget_t *w, os64_gui_rect_t *sv, os64_gui_rect_t *hue)
{
    if (w->bounds.w < 12 || w->bounds.h < 40) return false;
    *sv = (os64_gui_rect_t){w->bounds.x + 2, w->bounds.y + 2, w->bounds.w - 4, w->bounds.h - 26};
    *hue = (os64_gui_rect_t){w->bounds.x + 2, w->bounds.y + w->bounds.h - 18, w->bounds.w - 4, 16};
    return true;
}

static void picker_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_ui_colorpicker_t *p = (os64_ui_colorpicker_t *)w;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
    os64_draw_rect(&ctx->surf, w->bounds, w->focused ? t->focus_ring : t->field_border);
    os64_gui_rect_t sv, hue, clip;
    if (!areas(w, &sv, &hue)) return;
    os64_gui_rect_t surface = {0, 0, (int32_t)ctx->surf.width, (int32_t)ctx->surf.height};
    for (int part = 0; part < 2; ++part) {
        os64_gui_rect_t r = part ? hue : sv;
        if (!os64_rect_intersect(r, surface, &clip)) continue;
        for (int y = clip.y; y < clip.y + clip.h; ++y) {
            uint32_t *row = ctx->surf.pixels + (size_t)y * ctx->surf.pitch_px;
            for (int x = clip.x; x < clip.x + clip.w; ++x) {
                int fraction = (int)((int64_t)(x - r.x) * (part ? 359 : 255) / (r.w - 1));
                row[x] = part ? os64_ui_color_from_hsv(fraction, 255, 255) :
                    os64_ui_color_from_hsv(p->hue, fraction,
                        255 - (int)((int64_t)(y - r.y) * 255 / (r.h - 1)));
            }
        }
    }
    int x = sv.x + p->saturation * (sv.w - 1) / 255;
    int y = sv.y + (255 - p->value) * (sv.h - 1) / 255;
    // Marker bars intersect their own area, including at exact endpoints.
    for (int i = -3; i <= 3; ++i) {
        os64_gui_rect_t h = {x - 3, y + i, 7, 1}, c;
        if (os64_rect_intersect(h, sv, &c)) {
            uint32_t color = i == -3 || i == 3 ? 0xff000000u : 0xffffffffu;
            if (i == -3 || i == 3) os64_draw_fill_rect(&ctx->surf, c, color);
            else {
                os64_gui_rect_t l = {x - 3, y + i, 1, 1}, rr = {x + 3, y + i, 1, 1};
                if (os64_rect_intersect(l, sv, &c)) os64_draw_fill_rect(&ctx->surf, c, color);
                if (os64_rect_intersect(rr, sv, &c)) os64_draw_fill_rect(&ctx->surf, c, color);
            }
        }
    }
    x = hue.x + p->hue * (hue.w - 1) / 359;
    os64_draw_vline(&ctx->surf, x, hue.y, hue.h, 0xff000000u);
    if (x + 1 < hue.x + hue.w) os64_draw_vline(&ctx->surf, x + 1, hue.y, hue.h, 0xffffffffu);
}

static void notify_color(os64_ui_t *ui, os64_ui_colorpicker_t *p)
{
    uint32_t color = os64_ui_color_from_hsv(p->hue, p->saturation, p->value);
    if (color == p->color) { os64_ui_mark_dirty(ui, &p->w); return; }
    p->color = color;
    os64_ui_mark_dirty(ui, &p->w);
    if (p->on_change) p->on_change(p, p->color_user);
}

static void picker_pointer(os64_ui_t *ui, os64_ui_colorpicker_t *p, int x, int y)
{
    os64_gui_rect_t sv, hue;
    if (!areas(&p->w, &sv, &hue)) return;
    if (p->drag_part == 2)
        p->hue = (int)((int64_t)clamp(x - hue.x, hue.w - 1) * 359 / (hue.w - 1));
    else {
        p->saturation = (int)((int64_t)clamp(x - sv.x, sv.w - 1) * 255 / (sv.w - 1));
        p->value = 255 - (int)((int64_t)clamp(y - sv.y, sv.h - 1) * 255 / (sv.h - 1));
    }
    notify_color(ui, p);
}

static void picker_cancel(os64_ui_widget_t *w)
{
    os64_ui_colorpicker_t *p = (os64_ui_colorpicker_t *)w;
    p->drag_part = 0; p->seq = 0; w->pressed = false;
}

static bool picker_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    os64_ui_colorpicker_t *p = (os64_ui_colorpicker_t *)w;
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
        os64_gui_rect_t sv, hue;
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT || !areas(w, &sv, &hue)) return false;
        os64_ui_set_focus(ui, w);
        if (ev->mouse.y >= hue.y) p->drag_part = 2;
        else if (ev->mouse.y < sv.y + sv.h) p->drag_part = 1;
        else return true;
        w->pressed = true;
        picker_pointer(ui, p, ev->mouse.x, ev->mouse.y);
        return true;
    }
    case OS64_GUI_EVENT_MOUSE_MOVE:
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
        if (!p->drag_part) return false;
        picker_pointer(ui, p, ev->mouse.x, ev->mouse.y);
        if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) picker_cancel(w);
        return true;
    case OS64_GUI_EVENT_KEY_DOWN: {
        if (ui->grab || (ev->key.modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT))) {
            p->seq = 0; return false;
        }
        char ch = 0;
        ui_key_t key = os64_ui_decode_key(&p->seq, ev, &ch);
        bool shift = ev->key.modifiers & OS64_GUI_MOD_SHIFT;
        switch (key) {
        case K_LEFT: if (shift) p->hue = (p->hue + 359) % 360; else p->saturation = clamp(p->saturation - 1, 255); break;
        case K_RIGHT: if (shift) p->hue = (p->hue + 1) % 360; else p->saturation = clamp(p->saturation + 1, 255); break;
        case K_UP: p->value = clamp(p->value + 1, 255); break;
        case K_DOWN: p->value = clamp(p->value - 1, 255); break;
        default: return key == K_NONE;
        }
        notify_color(ui, p);
        return true;
    }
    default: return false;
    }
}

static const os64_ui_class_t kPickerClass = {"colorpicker", picker_paint, picker_event, picker_cancel};

void os64_ui_colorpicker(os64_ui_colorpicker_t *p, uint32_t color,
    void (*on_change)(os64_ui_colorpicker_t *, void *), void *user)
{
    *p = (os64_ui_colorpicker_t){0};
    p->w.cls = &kPickerClass; p->w.focusable = true;
    p->on_change = on_change; p->color_user = user;
    os64_ui_colorpicker_set(NULL, p, color);
}
