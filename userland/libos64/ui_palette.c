// Palette building blocks. Geometry and paint treatment belong to separate
// choices, so selecting colors preserves them.
#include "os64/ui.h"

void os64_ui_theme_palette(os64_ui_theme_t *t, os64_ui_palette_t palette)
{
    uint32_t surface, border, ink, field, accent, accent_ink, pressed, light, dark;
    switch (palette) {
    case OS64_UI_PALETTE_MIDNIGHT:
        surface = 0xff202a36; border = 0xff465362; ink = 0xffeee9df;
        field = 0xff141e29; accent = 0xffdfae65; accent_ink = 0xff201b15;
        pressed = 0xffbd8842; light = 0xfff5d09a; dark = 0xff8a602d;
        break;
    case OS64_UI_PALETTE_PAPER:
        surface = 0xffe6e3db; border = 0xff999f99; ink = 0xff303a3a;
        field = 0xfffaf7ef; accent = 0xff377e7b; accent_ink = 0xfffaf7ef;
        pressed = 0xff285d5b; light = 0xff73aaa5; dark = 0xff204c4a;
        break;
    case OS64_UI_PALETTE_ELECTRIC:
        surface = 0xff293442; border = 0xff617084; ink = 0xffe8eef6;
        field = 0xff192330; accent = 0xff437de0; accent_ink = 0xffffffff;
        pressed = 0xff2a55a2; light = 0xff85aef1; dark = 0xff233e72;
        break;
    default:
        return;
    }
    t->panel_bg = surface;
    t->panel_border = border;
    t->label_fg = ink;
    t->button_face = accent;
    t->button_face_pressed = pressed;
    t->button_border = dark;
    t->button_fg = accent_ink;
    t->button_highlight = light;
    t->button_shadow = dark;
    t->button_face_hover = light;
    t->hover_border = light;
    t->focus_ring = ink;
    t->disabled_bg = surface;
    t->disabled_fg = border;
    t->text_bg = field;
    t->text_fg = ink;
    t->text_sel_bg = accent;
    t->text_sel_fg = accent_ink;
    t->text_caret = accent;
    t->field_bg = field;
    t->field_fg = ink;
    t->field_border = border;
    t->field_border_focus = accent;
    t->scroll_track = field;
    t->scroll_thumb = border;
    t->menu_bg = surface;
    t->menu_fg = ink;
    t->menu_hi_bg = accent;
    t->menu_hi_fg = accent_ink;
    t->menu_sep = border;
}
