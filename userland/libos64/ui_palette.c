// Palette building blocks. Geometry and paint treatment belong to separate
// choices, so selecting colors preserves them.
#include "os64/ui.h"
#include "os64/str.h"

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

typedef struct {
    const char *name;
    const char *members[6];
} palette_role_t;

// The first property supplies the role color. Other members follow while
// equal to it; a different value is an override, including after a reload.
static const palette_role_t kRoles[OS64_UI_PALETTE_ROLE_COUNT] = {
    {"Surface", {"panel.bg", "disabled.bg", "menu.bg"}},
    {"Text & focus", {"label.fg", "text.fg", "field.fg", "menu.fg", "focus.ring"}},
    {"Field & paper", {"field.bg", "text.bg", "scroll.track"}},
    {"Accent", {"button.face", "text.sel.bg", "text.caret", "field.border.focus", "menu.hi.bg"}},
    {"Accent text", {"button.fg", "text.sel.fg", "menu.hi.fg"}},
    {"Borders & muted", {"panel.border", "field.border", "scroll.thumb", "menu.sep", "disabled.fg"}},
    {"Hover & light", {"button.highlight", "button.face.hover", "hover.border"}},
    {"Pressed face", {"button.face.pressed"}},
    {"Shadow", {"button.shadow", "button.border"}},
};

static size_t color_index(const char *name)
{
    size_t count = os64_ui_theme_color_count();
    for (size_t i = 0; i < count; ++i)
        if (os64_streq(name, os64_ui_theme_color_name(i))) return i;
    return count;
}

const char *os64_ui_palette_role_name(size_t role)
{
    return role < OS64_UI_PALETTE_ROLE_COUNT ? kRoles[role].name : "";
}

uint32_t os64_ui_palette_role_get(const os64_ui_theme_t *t, size_t role)
{
    return role < OS64_UI_PALETTE_ROLE_COUNT ?
        os64_ui_theme_color_get(t, color_index(kRoles[role].members[0])) : 0xff000000u;
}

void os64_ui_palette_role_set(os64_ui_theme_t *t, size_t role, uint32_t color)
{
    if (role >= OS64_UI_PALETTE_ROLE_COUNT || (color >> 24) != 255) return;
    uint32_t previous = os64_ui_palette_role_get(t, role);
    for (size_t i = 0; i < 6 && kRoles[role].members[i]; ++i) {
        size_t index = color_index(kRoles[role].members[i]);
        if (i == 0 || os64_ui_theme_color_get(t, index) == previous)
            os64_ui_theme_color_set(t, index, color);
    }
}

bool os64_ui_palette_color_follow(os64_ui_theme_t *t, size_t index)
{
    const char *name = os64_ui_theme_color_name(index);
    for (size_t r = 0; r < OS64_UI_PALETTE_ROLE_COUNT; ++r)
        for (size_t i = 1; i < 6 && kRoles[r].members[i]; ++i)
            if (os64_streq(name, kRoles[r].members[i]))
                return os64_ui_theme_color_set(t, index, os64_ui_palette_role_get(t, r));
    return false;
}
