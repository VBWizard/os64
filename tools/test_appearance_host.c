#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "os64/ui.h"
#include "gui/event_queue.h"

extern bool fail_alloc;
void appearance_session_contracts(void);
void appearance_customizer_contracts(void);
void controlcenter_layout_contracts(void);

// Diagnostics use the kernel log in the guest; this fixture has no log sink.
void os64_debug_log(const char *line) { assert(line && *line); }

// Kernel surface refresh is outside this host test; retain its supplied size.
int64_t __wrap_os64_draw_ctx_refresh(os64_draw_ctx_t *ctx)
{
    assert(ctx);
    return 0;
}

static void theme_schema_contracts(void)
{
    os64_ui_theme_t base, draft, decoded;
    os64_ui_theme_defaults(&base);
    draft = base;
    os64_ui_theme_palette(&draft, OS64_UI_PALETTE_ELECTRIC);
    draft.button_bevel = 3;
    char text[4096];
    int64_t n = os64_ui_theme_encode_session(&draft, text, sizeof(text));
    assert(n > 0 && n < (int64_t)sizeof(text));
    decoded = base; decoded.pad = 17; decoded.gap = 13;
    assert(os64_ui_theme_parse(&decoded, text, (size_t)n, true));
    assert(decoded.button_face == draft.button_face && decoded.button_bevel == 3);
    assert(decoded.pad == 17 && decoded.gap == 13);
    assert(os64_ui_theme_encode_session(&draft, text, 8) < 0);
    const char *bad[] = {"button.face = ffffff\nunknown = 1\n", "pad = 99999\n",
        "font.w = 9\n", "button.bevel = 5\n", "button.face = 1234567\n",
        "button.face ffffff\n", "slider.track.h = 0\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        decoded = base;
        assert(!os64_ui_theme_parse(&decoded, bad[i], strlen(bad[i]), false));
        assert(memcmp(&decoded, &base, sizeof(base)) == 0);
    }
    const char *partial = "button.face = 123456\n";
    assert(os64_ui_theme_parse(&decoded, partial, strlen(partial), false));
    assert(decoded.button_face == 0xff123456);
    assert(!os64_ui_theme_parse(&decoded, partial, strlen(partial), true));
    assert(!os64_ui_theme_parse(&decoded, NULL, 32, true));
    const char *overlay = "inherit = startup\nbutton.face = 123456\n";
    decoded = base;
    assert(os64_ui_theme_parse(&decoded, overlay, strlen(overlay), true));
    assert(decoded.button_face == 0xff123456 && decoded.panel_bg == base.panel_bg);
    const char *bad_overlay[] = {"inherit = startup\npad = 17\n",
        "inherit = startup\nunknown = 1\n", "inherit = startup\nbutton.face = xyz\n",
        "inherit = other\n", "inherit = startup\ninherit = startup\n"};
    for (size_t i = 0; i < sizeof(bad_overlay) / sizeof(*bad_overlay); ++i) {
        decoded = base;
        assert(!os64_ui_theme_parse(&decoded, bad_overlay[i], strlen(bad_overlay[i]), true));
        assert(!memcmp(&decoded, &base, sizeof(base)));
    }
    const char nul[] = "button.face = 123456\n\0junk";
    assert(!os64_ui_theme_parse(&decoded, nul, sizeof(nul) - 1, false));
    fail_alloc = true;
    assert(!os64_ui_theme_parse(&decoded, partial, strlen(partial), false));
    fail_alloc = false;
    decoded = base;
    os64_ui_theme_merge(&decoded, &draft, OS64_UI_COMPONENT_PALETTE);
    assert(decoded.button_face == draft.button_face && decoded.button_bevel == 0);
    os64_ui_theme_merge(&decoded, &draft, OS64_UI_COMPONENT_TREATMENT);
    assert(decoded.button_bevel == 3 && decoded.pad == base.pad);
    draft.font_h = 17;
    assert(!os64_ui_theme_valid(&draft));
    assert(os64_ui_theme_encode_session(&draft, text, sizeof(text)) < 0);
}

static void palettes_preserve_composition(void)
{
    os64_ui_theme_t t;
    os64_ui_theme_defaults(&t);
    t.pad = 13;
    t.gap = 5;
    t.button_h = 37;
    t.scroll_w = 19;
    t.button_bevel = 3;
    for (unsigned p = 0; p < 3; ++p) {
        os64_ui_theme_palette(&t, (os64_ui_palette_t)p);
        assert(t.pad == 13 && t.gap == 5 && t.button_h == 37);
        assert(t.scroll_w == 19 && t.button_bevel == 3);
        assert(t.font_w == 8 && t.font_h == 16);
        assert(t.text_bg != t.text_fg && t.text_sel_bg != t.text_sel_fg);
        assert(t.button_face != t.button_fg);
        os64_ui_theme_t before = t;
        os64_ui_theme_palette(&t, (os64_ui_palette_t)99);
        assert(memcmp(&before, &t, sizeof(t)) == 0);
    }
}

static void bevel_stays_inside_button(void)
{
    uint32_t pixels[32 * 32];
    os64_draw_ctx_t ctx = { .surf = { .pixels = pixels, .width = 32,
        .height = 32, .pitch_px = 32 } };
    os64_ui_widget_t button;
    os64_ui_button(&button, "", NULL, NULL);
    os64_ui_theme_t t;
    os64_ui_theme_defaults(&t);
    os64_ui_theme_palette(&t, OS64_UI_PALETTE_MIDNIGHT);
    t.button_bevel = INT_MAX;
    for (int height = 1; height <= 16; ++height) {
        for (int width = 1; width <= 16; ++width) {
            for (size_t i = 0; i < 32 * 32; ++i) pixels[i] = 0x12345678;
            button.bounds = (os64_gui_rect_t){4, 5, width, height};
            button.cls->paint(&button, &ctx, &t);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    if (x < 4 || x >= 4 + width || y < 5 || y >= 5 + height)
                        assert(pixels[y * 32 + x] == 0x12345678);
        }
    }
    button.bounds = (os64_gui_rect_t){4, 5, 16, 16};
    button.cls->paint(&button, &ctx, &t);
    assert(pixels[6 * 32 + 5] == t.button_highlight);
    button.pressed = true;
    button.cls->paint(&button, &ctx, &t);
    assert(pixels[6 * 32 + 5] == t.button_shadow);
}

static void controls_clip_captions(void)
{
    uint32_t pixels[32 * 32];
    os64_draw_ctx_t ctx = { .surf = { .pixels = pixels, .width = 32,
        .height = 32, .pitch_px = 32 } };
    os64_ui_theme_t t;
    os64_ui_theme_defaults(&t);
    t.button_bevel = INT_MAX;
    os64_ui_widget_t button, label;
    os64_ui_checkbox_t cb;
    os64_ui_slider_t sl;
    os64_ui_button(&button, "Caption longer than its control", NULL, NULL);
    os64_ui_label(&label, button.text);
    os64_ui_checkbox(&cb, button.text, true, NULL, NULL);
    os64_ui_slider(&sl, INT_MIN, INT_MAX, INT_MAX, 0, NULL, NULL);
    os64_ui_widget_t *widgets[] = {&button, &label, &cb.w, &sl.w};
    for (size_t k = 0; k < sizeof(widgets) / sizeof(widgets[0]); ++k)
        for (int h = 0; h <= 18; ++h)
            for (int w = 0; w <= 18; ++w) {
                for (size_t i = 0; i < 32 * 32; ++i) pixels[i] = 0x12345678;
                widgets[k]->bounds = (os64_gui_rect_t){4, 5, w, h};
                widgets[k]->focused = widgets[k]->hovered = true;
                widgets[k]->cls->paint(widgets[k], &ctx, &t);
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x)
                        if (x < 4 || x >= 4 + w || y < 5 || y >= 5 + h)
                            assert(pixels[y * 32 + x] == 0x12345678);
            }
}

static int clicks, changes;
static void clicked(os64_ui_widget_t *w, void *u)
{
    (void)w; (void)u; ++clicks;
}
static void checked(os64_ui_checkbox_t *cb, void *u)
{
    (void)cb; (void)u; ++changes;
}
static void slid(os64_ui_slider_t *sl, void *u)
{
    (void)sl; (void)u; ++changes;
}
static void mouse(os64_ui_t *ui, uint8_t type, int x, int y, uint8_t button)
{
    os64_gui_event_t ev = {.type = type,
        .mouse = {.x = x, .y = y, .button = button}};
    os64_ui_dispatch(ui, &ev);
}
static void key(os64_ui_t *ui, uint8_t type, char ascii, uint8_t sc, uint8_t mods)
{
    os64_gui_event_t ev = {.type = type,
        .key = {.ascii = ascii, .scancode = sc, .modifiers = mods}};
    os64_ui_dispatch(ui, &ev);
}
static void burst(os64_ui_t *ui, const char *s)
{
    for (; *s; ++s) key(ui, OS64_GUI_EVENT_KEY_DOWN, *s, 0x4d, 0);
}

static void hide_focus_on_resize(os64_ui_t *ui)
{
    assert(ui->focus);
    ui->focus->hidden = true;
}

static void interaction_contracts(void)
{
    os64_ui_t ui = {0};
    os64_ui_theme_defaults(&ui.theme);
    os64_ui_widget_t root, group, a, b, foreign;
    os64_ui_checkbox_t cb;
    os64_ui_slider_t sl;
    os64_ui_panel(&root); os64_ui_panel(&group);
    root.bounds = (os64_gui_rect_t){0, 0, 400, 200};
    group.bounds = root.bounds;
    os64_ui_button(&a, "A", clicked, NULL);
    os64_ui_button(&b, "B", clicked, NULL);
    os64_ui_button(&foreign, "outside tree", clicked, NULL);
    os64_ui_checkbox(&cb, "Check", false, checked, NULL);
    os64_ui_slider(&sl, INT_MIN, INT_MAX, INT_MAX, 0, slid, NULL);
    a.bounds = (os64_gui_rect_t){10, 10, 70, 30};
    b.bounds = (os64_gui_rect_t){90, 10, 70, 30};
    cb.w.bounds = (os64_gui_rect_t){10, 50, 150, 30};
    sl.w.bounds = (os64_gui_rect_t){10, 100, 300, 25};
    os64_ui_add_child(&root, &group);
    os64_ui_add_child(&group, &a); os64_ui_add_child(&group, &b);
    os64_ui_add_child(&root, &cb.w); os64_ui_add_child(&root, &sl.w);
    os64_ui_set_root(&ui, &root);
    os64_ui_set_focus(&ui, &foreign); assert(!ui.focus);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, 0); assert(ui.focus == &a);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, OS64_GUI_MOD_SHIFT);
    assert(ui.focus == &sl.w);
    assert(!os64_ui_focus_next(&ui, false, false) && !ui.focus);
    os64_ui_set_enabled(&ui, &group, false);
    assert(os64_ui_focus_next(&ui, false, true) && ui.focus == &cb.w);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 20, 20, OS64_GUI_MOUSE_LEFT);
    assert(!ui.grab && !a.pressed);
    os64_ui_set_enabled(&ui, &group, true);
    os64_ui_set_focus(&ui, &a);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, ' ', 57, 0);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, ' ', 57, 0);
    assert(a.pressed && clicks == 0);
    key(&ui, OS64_GUI_EVENT_KEY_UP, '\n', 28, 0); assert(a.pressed);
    key(&ui, OS64_GUI_EVENT_KEY_UP, ' ', 57, OS64_GUI_MOD_SHIFT);
    assert(!a.pressed && clicks == 1);
    key(&ui, OS64_GUI_EVENT_KEY_UP, ' ', 57, 0); assert(clicks == 1);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\n', 0x28, OS64_GUI_MOD_HID);
    os64_ui_set_hidden(&ui, &a, true);
    assert(!ui.focus && !a.pressed && !a.activation_key);
    os64_ui_set_hidden(&ui, &a, false); os64_ui_set_focus(&ui, &a);
    key(&ui, OS64_GUI_EVENT_KEY_UP, '\n', 0x28, OS64_GUI_MOD_HID);
    assert(clicks == 1);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 20, 20, OS64_GUI_MOUSE_RIGHT);
    assert(!ui.grab);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 20, 20, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 100, 20, OS64_GUI_MOUSE_RIGHT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 100, 20, OS64_GUI_MOUSE_RIGHT);
    assert(ui.grab == &a && a.pressed);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, 100, 20, 0);
    assert(!a.pressed && !b.hovered);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 100, 20, OS64_GUI_MOUSE_LEFT);
    assert(!ui.grab && clicks == 1);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 20, 20, OS64_GUI_MOUSE_LEFT);
    os64_ui_set_enabled(&ui, &group, false);
    assert(!ui.grab && !ui.focus && !a.pressed && !a.hovered);
    os64_ui_set_enabled(&ui, &group, true);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 20, 20, OS64_GUI_MOUSE_LEFT);
    assert(clicks == 1);
    os64_ui_set_focus(&ui, &cb.w);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, ' ', 57, 0);
    key(&ui, OS64_GUI_EVENT_KEY_UP, ' ', 57, 0);
    assert(cb.checked && changes == 1);
    os64_ui_checkbox_set(&ui, &cb, false); assert(!cb.checked && changes == 1);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, 20, 20, 0); assert(a.hovered);
    os64_gui_event_t leave = {.type = OS64_GUI_EVENT_POINTER_STATE};
    os64_ui_dispatch(&ui, &leave); assert(!a.hovered && !ui.hover);
    os64_ui_set_focus(&ui, &a);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, 20, 20, 0);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, ' ', 57, 0);
    os64_gui_event_t blur = {.type = OS64_GUI_EVENT_WINDOW_FOCUS};
    os64_ui_dispatch(&ui, &blur);
    assert(ui.focus == &a && !a.focused && !a.pressed);
    assert(a.hovered); // keyboard focus can leave while the pointer stays here
    key(&ui, OS64_GUI_EVENT_KEY_UP, ' ', 57, 0); assert(clicks == 1);
    blur.focus.gained = 1;
    os64_ui_dispatch(&ui, &blur);
    assert(ui.focus == &a && a.focused && !a.pressed);

    os64_ui_set_focus(&ui, &sl.w);
    burst(&ui, "\033[F"); assert(sl.value == INT_MAX);
    burst(&ui, "\033[C"); assert(sl.value == INT_MAX);
    burst(&ui, "\033[D"); assert(sl.value == 0);
    burst(&ui, "\033[H"); assert(sl.value == INT_MIN);
    burst(&ui, "\033[D"); assert(sl.value == INT_MIN);
    os64_ui_slider_set(&ui, &sl, 0);
    int before = changes;
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 154, 110, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 154, 110, OS64_GUI_MOUSE_LEFT);
    assert(sl.value == 0 && changes == before); // stationary thumb click
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 154, 110, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, INT_MAX, 110, 0);
    assert(sl.value == INT_MAX);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, INT_MIN, 110, 0);
    assert(sl.value == INT_MIN);
    os64_ui_set_enabled(&ui, &sl.w, false);
    assert(sl.drag_offset == -1 && !ui.grab && !ui.focus);

    // Resizing keeps keyboard focus but a pre-resize release cannot activate.
    os64_draw_ctx_t ctx = {.surf = {.width = 400, .height = 400}};
    ui.ctx = &ctx;
    os64_gui_event_t resize = {.type = OS64_GUI_EVENT_WINDOW_RESIZE};
    os64_ui_set_focus(&ui, &a);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, ' ', 57, 0);
    before = clicks;
    os64_ui_dispatch(&ui, &resize);
    assert(ui.focus == &a && a.focused && !a.pressed && !a.activation_key);
    key(&ui, OS64_GUI_EVENT_KEY_UP, ' ', 57, 0);
    assert(clicks == before);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 20, 20, OS64_GUI_MOUSE_LEFT);
    assert(ui.grab == &a && a.pressed);
    os64_ui_dispatch(&ui, &resize);
    assert(ui.focus == &a && !ui.grab && !ui.hover && !a.hovered && !a.pressed);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 20, 20, OS64_GUI_MOUSE_LEFT);
    assert(clicks == before);
    blur.focus.gained = 0;
    os64_ui_dispatch(&ui, &blur);
    os64_ui_dispatch(&ui, &resize);
    assert(ui.focus == &a && !a.focused);
    blur.focus.gained = 1;
    os64_ui_dispatch(&ui, &blur);
    assert(ui.focus == &a && a.focused);
    os64_ui_cancel_interaction(&ui);
    assert(!ui.focus && !a.focused);
    os64_ui_set_focus(&ui, &a);
    ui.on_resize = hide_focus_on_resize;
    os64_ui_dispatch(&ui, &resize);
    assert(!ui.focus && !a.focused && a.hidden);
    os64_ui_set_enabled(&ui, &sl.w, true);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, INT_MAX, 110, OS64_GUI_MOUSE_LEFT);
    assert(sl.value == INT_MIN);
    os64_ui_slider(&sl, 7, -5, 0, INT_MAX, NULL, NULL);
    assert(sl.min == 7 && sl.max == 7 && sl.step == 1 && sl.value == 7);
    // Widgets retain runs now — a textview keeps one for the caret's line,
    // a listbox one per visible row — so a leak-checked harness has to hand
    // them back the way an application would.
    os64_ui_font_release(&ui);
}

static void queue_preserves_final_pointer(void)
{
    gui_event_queue_t q = {0};
    input_event_t ev = {.type = INPUT_EVENT_MOUSE_MOVE};
    for (uint32_t i = 0; i < GUI_WINDOW_EVENTS_MAX - 1; ++i) {
        ev.mouse.x = (int)i;
        assert(gui_event_queue_push(&q, &ev) == 1);
    }
    assert(gui_event_queue_push(&q, &ev) == 0);
    ev = (input_event_t){.type = INPUT_EVENT_POINTER_STATE, .pointer = {.inside = 1}};
    assert(gui_event_queue_push(&q, &ev) == 1);
    ev.pointer.inside = 0;
    assert(gui_event_queue_push(&q, &ev) == 1);
    input_event_t out;
    for (uint32_t i = 0; i < GUI_WINDOW_EVENTS_MAX - 1; ++i) {
        assert(gui_event_queue_pop(&q, &out));
        assert(out.type == INPUT_EVENT_MOUSE_MOVE && out.mouse.x == (int)i);
    }
    assert(gui_event_queue_pending(&q));
    assert(gui_event_queue_pop(&q, &out));
    assert(out.type == INPUT_EVENT_POINTER_STATE && !out.pointer.inside);
    assert(!gui_event_queue_pending(&q) && !gui_event_queue_pop(&q, &out));
    // A later entry supersedes an unread leave, including its coordinates.
    gui_event_queue_push(&q, &ev);
    ev.pointer.inside = 1; ev.pointer.x = 27; ev.pointer.y = 83;
    gui_event_queue_push(&q, &ev);
    assert(gui_event_queue_pop(&q, &out) && out.pointer.inside);
    assert(out.pointer.x == 27 && out.pointer.y == 83);
    // Preserve focus's existing overflow policy through ring wrap-around.
    ev.type = INPUT_EVENT_KEY_DOWN;
    for (uint32_t i = 0; i < GUI_WINDOW_EVENTS_MAX - 1; ++i)
        assert(gui_event_queue_push(&q, &ev) == 1);
    ev.type = INPUT_EVENT_WINDOW_FOCUS;
    assert(gui_event_queue_push(&q, &ev) == 2);
    for (uint32_t i = 0; i < GUI_WINDOW_EVENTS_MAX - 1; ++i)
        assert(gui_event_queue_pop(&q, &out));
    assert(out.type == INPUT_EVENT_WINDOW_FOCUS && !gui_event_queue_pending(&q));
}

static char sample_line[32];
static size_t sample_lines(void *u) { (void)u; return 1; }
static const char *sample_get(void *u, size_t i, size_t *len)
{
    (void)u; (void)i; *len = strlen(sample_line); return sample_line;
}
static bool sample_insert(void *u, size_t line, size_t col, const char *s, size_t n)
{
    (void)u; assert(line == 0 && col <= strlen(sample_line));
    assert(strlen(sample_line) + n < sizeof(sample_line));
    memmove(sample_line + col + n, sample_line + col, strlen(sample_line) - col + 1);
    memcpy(sample_line + col, s, n);
    return true;
}

static void text_focus_preserves_literal_tabs(void)
{
    os64_ui_t ui = {0};
    os64_ui_theme_defaults(&ui.theme);
    os64_ui_widget_t root, button;
    os64_ui_textview_t edit, read;
    os64_ui_textfield_t field;
    char field_text[32];
    os64_ui_textbuf_t editable = {.line_count = sample_lines, .line = sample_get,
        .insert = sample_insert};
    os64_ui_textbuf_t readonly = {.line_count = sample_lines, .line = sample_get};
    os64_ui_panel(&root);
    os64_ui_textview(&edit, &editable, NULL, NULL, NULL);
    os64_ui_textview(&read, &readonly, NULL, NULL, NULL);
    os64_ui_textfield(&field, field_text, sizeof(field_text), NULL, NULL, NULL);
    os64_ui_button(&button, "next", NULL, NULL);
    root.bounds = (os64_gui_rect_t){0, 0, 400, 400};
    edit.w.bounds = (os64_gui_rect_t){0, 0, 300, 100};
    read.w.bounds = (os64_gui_rect_t){0, 100, 300, 100};
    field.w.bounds = (os64_gui_rect_t){0, 200, 300, 32};
    os64_ui_add_child(&root, &edit.w); os64_ui_add_child(&root, &read.w);
    os64_ui_add_child(&root, &field.w); os64_ui_add_child(&root, &button);
    os64_ui_set_root(&ui, &root);
    assert(edit.w.accepts_tab && !read.w.accepts_tab);
    os64_ui_set_focus(&ui, &edit.w);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, 0);
    assert(ui.focus == &edit.w && strcmp(sample_line, "\t") == 0);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, OS64_GUI_MOD_CTRL);
    assert(ui.focus == &read.w && strcmp(sample_line, "\t") == 0);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, 0);
    assert(ui.focus == &field.w);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, 'a', 30, 0);
    assert(strcmp(field_text, "a") == 0);
    os64_draw_ctx_t ctx = {.surf = {.width = 400, .height = 400}};
    ui.ctx = &ctx;
    os64_gui_event_t resize = {.type = OS64_GUI_EVENT_WINDOW_RESIZE};
    os64_ui_dispatch(&ui, &resize);
    assert(ui.focus == &field.w && field.w.focused);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, 'b', 48, 0);
    assert(strcmp(field_text, "ab") == 0);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, 0);
    assert(ui.focus == &button && strcmp(field_text, "ab") == 0);
    os64_ui_set_focus(&ui, &edit.w);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, '\t', 15, OS64_GUI_MOD_CTRL | OS64_GUI_MOD_SHIFT);
    assert(ui.focus == &button);
    os64_ui_set_focus(&ui, &edit.w);
    os64_ui_dispatch(&ui, &resize);
    key(&ui, OS64_GUI_EVENT_KEY_DOWN, 'z', 44, 0);
    assert(ui.focus == &edit.w && strcmp(sample_line, "\tz") == 0);
    os64_ui_set_focus(&ui, &read.w);
    size_t before = read.cur_col;
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, 100, 110, 0);
    assert(read.cur_col == before); // hover must not become a selection drag
    os64_ui_set_enabled(&ui, &field.w, false);
    os64_ui_set_focus(&ui, &field.w); assert(ui.focus == &read.w);
    // Widgets retain runs now — a textview keeps one for the caret's line,
    // a listbox one per visible row — so a leak-checked harness has to hand
    // them back the way an application would.
    os64_ui_font_release(&ui);
}

static void render_composes_independent_trees(void)
{
    uint32_t pixels[16 * 16] = {0};
    os64_draw_ctx_t ctx = { .surf = { .pixels = pixels, .width = 16,
        .height = 16, .pitch_px = 16 } };
    os64_ui_t outer = {.ctx = &ctx}, inner = {.ctx = &ctx};
    os64_ui_theme_defaults(&outer.theme);
    os64_ui_theme_defaults(&inner.theme);
    outer.theme.panel_bg = 0xff112233;
    inner.theme.panel_bg = 0xffaabbcc;
    os64_ui_widget_t root, specimen;
    os64_ui_panel(&root);
    os64_ui_panel(&specimen);
    root.bounds = (os64_gui_rect_t){0, 0, 16, 16};
    specimen.bounds = (os64_gui_rect_t){4, 4, 8, 8};
    os64_ui_set_root(&outer, &root);
    os64_ui_set_root(&inner, &specimen);
    os64_gui_rect_t damage;
    assert(os64_ui_render(&outer, &damage));
    assert(damage.w == 16 && damage.h == 16);
    assert(os64_ui_render(&inner, &damage));
    assert(damage.x == 4 && damage.y == 4 && damage.w == 8 && damage.h == 8);
    assert(pixels[2 * 16 + 2] == outer.theme.panel_bg);
    assert(pixels[6 * 16 + 6] == inner.theme.panel_bg);
    assert(!os64_ui_render(&inner, NULL));
    // Widgets retain runs now — a textview keeps one for the caret's line,
    // a listbox one per visible row — so a leak-checked harness has to hand
    // them back the way an application would.
    os64_ui_font_release(&outer);
    os64_ui_font_release(&inner);
}

static const char *list_label(size_t index, void *user)
{
    (void)index; (void)user; return "A long list row label";
}
static int list_changes;
static void list_changed(os64_ui_listbox_t *list, void *user)
{ (void)list; (void)user; ++list_changes; }
static void list_contracts(void)
{
    os64_ui_t ui = {0}; os64_ui_theme_defaults(&ui.theme);
    os64_ui_widget_t root; os64_ui_panel(&root);
    root.bounds = (os64_gui_rect_t){0, 0, 120, 90};
    os64_ui_listbox_t list;
    os64_ui_listbox(&list, 12, list_label, list_changed, NULL);
    list.w.bounds = (os64_gui_rect_t){4, 4, 100, 52};
    os64_ui_add_child(&root, &list.w); os64_ui_set_root(&ui, &root);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 12, 12, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 115, 80, OS64_GUI_MOUSE_LEFT);
    assert(list.selected == -1 && !list_changes);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 12, 36, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 12, 36, OS64_GUI_MOUSE_LEFT);
    assert(list.selected == 1 && list_changes == 1);
    burst(&ui, "\033[B"); assert(list.selected == 2 && list.top == 1);
    burst(&ui, "\033[F"); assert(list.selected == 11 && list.top == 10);
    os64_ui_listbox_scroll_to(&ui, &list, 0); assert(list.top == 0 && list.selected == 11);
    os64_ui_listbox_set(&ui, &list, 1, 0); assert(list.top == 0 && list.selected == 0);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 12, 12, OS64_GUI_MOUSE_LEFT);
    os64_ui_cancel_interaction(&ui);
    int before = list_changes;
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 12, 12, OS64_GUI_MOUSE_LEFT);
    assert(list_changes == before && list.pressed_index == -1);
    os64_ui_listbox_set(&ui, &list, 0, 0);
    burst(&ui, "\033[B"); assert(list.selected == -1 && list.top == 0);
    uint32_t pixels[32 * 32];
    os64_draw_ctx_t ctx = {.surf = {.pixels = pixels, .width = 32, .height = 32, .pitch_px = 32}};
    list.count = 2; list.selected = 0;
    for (int h = 0; h < 28; ++h) for (int w = 0; w < 28; ++w) {
        for (size_t i = 0; i < 32 * 32; ++i) pixels[i] = 0x12345678;
        list.w.bounds = (os64_gui_rect_t){2, 2, w, h};
        list.w.cls->paint(&list.w, &ctx, &ui.theme);
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
            if (x < 2 || x >= 2 + w || y < 2 || y >= 2 + h)
                assert(pixels[y * 32 + x] == 0x12345678);
    }
    // Widgets retain runs now — a textview keeps one for the caret's line,
    // a listbox one per visible row — so a leak-checked harness has to hand
    // them back the way an application would.
    os64_ui_font_release(&ui);
}

static void rounded_and_color_contracts(void)
{
    os64_ui_theme_t theme, copy, before;
    os64_ui_theme_defaults(&theme);
    os64_ui_theme_palette(&theme, OS64_UI_PALETTE_ELECTRIC);
    theme.control_radius = 6;
    theme.button_bevel = 3;
    char bytes[4096];
    for (int session = 0; session < 2; ++session) {
        int64_t n = session ? os64_ui_theme_encode_session(&theme, bytes, sizeof(bytes)) :
            os64_ui_theme_encode(&theme, bytes, sizeof(bytes));
        assert(n > 0);
        copy = theme; copy.control_radius = 0;
        assert(session ? os64_ui_theme_parse(&copy, bytes, (size_t)n, true) :
            os64_ui_theme_parse_saved(&copy, bytes, (size_t)n));
        assert(copy.control_radius == 6);
        char *radius = strstr(bytes, "control.radius = 6\n");
        assert(radius);
        memmove(radius, radius + strlen("control.radius = 6\n"), strlen(radius + strlen("control.radius = 6\n")) + 1);
        // A pre-customizer snapshot loads as square even into a rounded draft.
        assert(session ? os64_ui_theme_parse(&copy, bytes, strlen(bytes), true) :
            os64_ui_theme_parse_saved(&copy, bytes, strlen(bytes)));
        assert(copy.control_radius == 0 && copy.button_face == theme.button_face);
    }
    before = theme;
    assert(!os64_ui_theme_parse(&theme, "control.radius = 9\n", 19, false));
    assert(!memcmp(&theme, &before, sizeof(theme)));
    copy = theme; copy.control_radius = 0;
    os64_ui_theme_merge(&copy, &theme, OS64_UI_COMPONENT_PALETTE);
    assert(copy.control_radius == 0);
    os64_ui_theme_merge(&copy, &theme, OS64_UI_COMPONENT_TREATMENT);
    assert(copy.control_radius == 6);

    // Shared colors move together, a differing override survives, and it
    // can explicitly rejoin the role after a serialized round trip.
    theme.text_sel_bg = 0xffaa1177;
    os64_ui_palette_role_set(&theme, 3, 0xff123456);
    assert(theme.button_face == 0xff123456 && theme.menu_hi_bg == 0xff123456);
    assert(theme.text_sel_bg == 0xffaa1177 && theme.control_radius == 6);
    int64_t n = os64_ui_theme_encode(&theme, bytes, sizeof(bytes));
    assert(os64_ui_theme_parse_saved(&copy, bytes, (size_t)n));
    size_t sel = 0;
    while (sel < os64_ui_theme_color_count() && strcmp(os64_ui_theme_color_name(sel), "text.sel.bg")) ++sel;
    assert(os64_ui_palette_color_follow(&copy, sel));
    assert(copy.text_sel_bg == copy.button_face);
    before = theme;
    assert(!os64_ui_theme_color_set(&theme, os64_ui_theme_color_count(), 0xffffffff));
    assert(!memcmp(&theme, &before, sizeof(theme)));
    // Each compiled palette's properties participate in a role.
    for (unsigned preset = 0; preset < 3; ++preset) {
        os64_ui_theme_palette(&theme, (os64_ui_palette_t)preset);
        for (size_t role = 0; role < OS64_UI_PALETTE_ROLE_COUNT; ++role)
            os64_ui_palette_role_set(&theme, role, 0xff123456);
        for (size_t i = 0; i < os64_ui_theme_color_count(); ++i)
            assert(os64_ui_theme_color_get(&theme, i) == 0xff123456 &&
                "theme color must belong to a palette role and follow it in each preset");
    }

    uint32_t pixels[48 * 48];
    os64_draw_ctx_t ctx = {.surf = {.pixels = pixels, .width = 48, .height = 48, .pitch_px = 48}};
    os64_ui_widget_t button; os64_ui_button(&button, "Long caption", NULL, NULL);
    os64_ui_theme_palette(&theme, OS64_UI_PALETTE_ELECTRIC);
    theme.control_radius = 8;
    for (int h = 0; h < 30; ++h) for (int w = 0; w < 30; ++w) {
        for (size_t i = 0; i < 48 * 48; ++i) pixels[i] = 0x12345678;
        button.bounds = (os64_gui_rect_t){6, 7, w, h};
        button.cls->paint(&button, &ctx, &theme);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 48; ++x)
            if (x < 6 || x >= 6 + w || y < 7 || y >= 7 + h)
                assert(pixels[y * 48 + x] == 0x12345678);
    }
    os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 0, 48, 48}, 0x12345678);
    os64_draw_fill_round_rect(&ctx.surf, (os64_gui_rect_t){4, 4, 24, 24}, 6, 0xffabcdef);
    assert(pixels[4 * 48 + 4] == 0x12345678);
    assert(pixels[4 * 48 + 16] == 0xffabcdef && pixels[16 * 48 + 4] == 0xffabcdef);
    os64_draw_round_rect(&ctx.surf, (os64_gui_rect_t){INT_MIN, INT_MIN, INT_MAX, INT_MAX}, INT_MAX, 1, 2);
    os64_draw_fill_round_rect(&ctx.surf, (os64_gui_rect_t){INT_MAX, INT_MAX, INT_MAX, INT_MAX}, INT_MAX, 1);
    assert(pixels[4 * 48 + 4] == 0x12345678);
}

static int color_changes;
static void picked(os64_ui_colorpicker_t *p, void *user)
{ (void)p; (void)user; ++color_changes; }

static void picker_contracts(void)
{
    assert(os64_ui_color_from_hsv(0, 255, 255) == 0xffff0000);
    assert(os64_ui_color_from_hsv(120, 255, 255) == 0xff00ff00);
    assert(os64_ui_color_from_hsv(240, 255, 255) == 0xff0000ff);
    assert(os64_ui_color_from_hsv(200, 0, 255) == 0xffffffff);
    assert(os64_ui_color_from_hsv(200, 255, 0) == 0xff000000);
    os64_ui_t ui = {0}; os64_ui_theme_defaults(&ui.theme);
    os64_ui_widget_t root; os64_ui_panel(&root);
    root.bounds = (os64_gui_rect_t){0, 0, 120, 120};
    os64_ui_colorpicker_t picker;
    os64_ui_colorpicker(&picker, 0xffff0000, picked, NULL);
    picker.w.bounds = (os64_gui_rect_t){4, 4, 104, 100};
    os64_ui_add_child(&root, &picker.w); os64_ui_set_root(&ui, &root);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 6, 6, OS64_GUI_MOUSE_LEFT);
    assert(picker.color == 0xffffffff && color_changes == 1);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_MOVE, 200, 200, 0);
    assert(picker.color == 0xff000000);
    os64_ui_cancel_interaction(&ui);
    int count = color_changes;
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 50, 50, OS64_GUI_MOUSE_LEFT);
    assert(color_changes == count && !picker.drag_part);
    os64_ui_colorpicker_set(&ui, &picker, 0xff437de0);
    assert(picker.color == 0xff437de0 && color_changes == count);
    os64_ui_set_focus(&ui, &picker.w);
    int saturation = picker.saturation, value = picker.value;
    burst(&ui, "\033[D"); assert(picker.saturation == saturation - 1);
    burst(&ui, "\033[A"); assert(picker.value == value + 1);
    os64_ui_set_enabled(&ui, &picker.w, false);
    count = color_changes;
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_DOWN, 50, 50, OS64_GUI_MOUSE_LEFT);
    mouse(&ui, OS64_GUI_EVENT_MOUSE_BUTTON_UP, 50, 50, OS64_GUI_MOUSE_LEFT);
    assert(color_changes == count);
    uint32_t pixels[48 * 48];
    os64_draw_ctx_t ctx = {.surf = {.pixels = pixels, .width = 48, .height = 48, .pitch_px = 48}};
    for (int h = 0; h < 44; ++h) for (int w = 0; w < 40; ++w) {
        for (size_t i = 0; i < 48 * 48; ++i) pixels[i] = 0x12345678;
        picker.w.bounds = (os64_gui_rect_t){2, 2, w, h};
        picker.w.cls->paint(&picker.w, &ctx, &ui.theme);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 48; ++x)
            if (x < 2 || x >= 2 + w || y < 2 || y >= 2 + h)
                assert(pixels[y * 48 + x] == 0x12345678);
    }
    // Widgets retain runs now — a textview keeps one for the caret's line,
    // a listbox one per visible row — so a leak-checked harness has to hand
    // them back the way an application would.
    os64_ui_font_release(&ui);
}

int main(void)
{
    rounded_and_color_contracts();
    picker_contracts();
    list_contracts();
    theme_schema_contracts();
    appearance_session_contracts();
    appearance_customizer_contracts();
    controlcenter_layout_contracts();
    palettes_preserve_composition();
    bevel_stays_inside_button();
    render_composes_independent_trees();
    controls_clip_captions();
    interaction_contracts();
    queue_preserves_final_pointer();
    text_focus_preserves_literal_tabs();
    puts("appearance: palettes, legacy themes, rounded paint, color picker, interaction and pointer queue contracts passed");
}
