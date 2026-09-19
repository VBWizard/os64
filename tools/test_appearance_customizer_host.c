// Exercise the application's real page layout and draft history. Collection
// I/O has a separate production-filesystem suite; this fixture supplies an
// empty collection so these checks can drive the editor deterministically.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define main appearance_guest_main
#include "../userland/apps/appearance/appearance.c"
#undef main

void os64_complain(const char *fmt, ...) { (void)fmt; }

int os64_ui_theme_list(os64_ui_theme_entry_t *entries, size_t cap)
{ (void)entries; (void)cap; return 0; }
int os64_ui_theme_load(const char *name, os64_ui_theme_t *theme)
{ (void)name; (void)theme; return OS64_UI_THEME_IO; }
int os64_ui_theme_save(const char *name, const os64_ui_theme_t *theme, bool replace)
{ (void)name; (void)theme; (void)replace; return OS64_UI_THEME_IO; }

static void contained(const os64_ui_widget_t *w)
{
    if (w->hidden) return;
    for (const os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling) {
        if (c->hidden) continue;
        assert(c->bounds.x >= w->bounds.x && c->bounds.y >= w->bounds.y);
        assert(c->bounds.x + c->bounds.w <= w->bounds.x + w->bounds.w);
        assert(c->bounds.y + c->bounds.h <= w->bounds.y + w->bounds.h);
        contained(c);
    }
}

void appearance_customizer_contracts(void)
{
    uint32_t *pixels = os64_malloc(958 * 706 * sizeof(uint32_t));
    assert(pixels);
    gCtx.surf = (os64_gui_surface_t){.pixels = pixels, .width = 958, .height = 706, .pitch_px = 958};
    setup();
    for (unsigned i = 0; i < 3; ++i) {
        tab_click(NULL, (void *)(uintptr_t)i);
        assert(!gCompact && !gCanvas.hidden);
        contained(&gRoot); contained(&gCanvas);
        os64_ui_render(&gEditor, NULL); os64_ui_render(&gPreview, NULL);
        for (unsigned j = 0; j < 3; ++j) assert(gPages[j].hidden == (i != j));
    }
    assert(gText.left_px == 0 && gText.sel);
    // The minimum supported native content area still contains its children.
    gCtx.surf.height = 696; layout();
    contained(&gRoot); contained(&gCanvas);
    gCtx.surf.height = 706; layout();
    os64_ui_theme_t old = gPreview.theme;
    uint32_t unchanged_components = gChanged;
    const char *unchanged_status = gApplyStatus;
    // Repeated unchanged choices must not enable Undo or spend its history.
    for (unsigned i = 0; i < 40; ++i) {
        corner_click(NULL, NULL);
        style_click(NULL, (void *)(uintptr_t)gStyle);
        palette_click(&gPalettes, NULL);
        reset_component_click(NULL, NULL);
    }
    assert(same_theme(&gPreview.theme, &old) && !gUndoCount && gUndo.disabled);
    assert(gChanged == unchanged_components && gApplyStatus == unchanged_status);
    corner_click(NULL, (void *)1);
    assert(gPreview.theme.control_radius == 6 && gUndoCount == 1 && draft_dirty());
    for (unsigned i = 0; i < 40; ++i) corner_click(NULL, (void *)1);
    assert(gUndoCount == 1);
    undo_click(NULL, NULL);
    assert(same_theme(&gPreview.theme, &old) && !gUndoCount && !draft_dirty());
    tab_click(NULL, (void *)1);
    reset_component_click(NULL, NULL);
    assert(!gUndoCount && gUndo.disabled);
    os64_ui_listbox_set(&gEditor, &gColors, OS64_UI_PALETTE_ROLE_COUNT, 3);
    color_selection(&gColors, NULL);
    os64_ui_textfield_set(&gEditor, &gHexField, "12ZZ44");
    hex_submit(&gHexField, NULL);
    assert(same_theme(&gPreview.theme, &old) && !gUndoCount);
    os64_ui_textfield_set(&gEditor, &gHexField, "123456");
    hex_submit(&gHexField, NULL);
    assert(gPreview.theme.button_face == 0xff123456 && gUndoCount == 1);
    assert(gPreview.theme.menu_hi_bg == 0xff123456);
    assert(same_theme(&gBaseline, &old));
    reset_component_click(NULL, NULL);
    assert(same_theme(&gPreview.theme, &old));
    undo_click(NULL, NULL);
    assert(gPreview.theme.button_face == 0xff123456);
    gPickerEditing = false;
    size_t count = gUndoCount;
    gPicker.w.pressed = true; gPicker.color = 0xff224466;
    picker_changed(&gPicker, NULL);
    gPicker.color = 0xff335577; picker_changed(&gPicker, NULL);
    assert(gUndoCount == count + 1);
    gPicker.w.pressed = false; gPickerEditing = false;
    undo_click(NULL, NULL);
    assert(gPreview.theme.button_face == 0xff123456);
    count = gUndoCount;
    os64_ui_set_focus(&gEditor, &gPicker.w);
    uint32_t first_key_color = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const char *arrow = "\033[A";
        while (*arrow) {
            os64_gui_event_t ev = {.type = OS64_GUI_EVENT_KEY_DOWN,
                .key = {.ascii = (unsigned char)*arrow++}};
            dispatch(&ev);
        }
        if (!i) first_key_color = gPreview.theme.button_face;
    }
    assert(first_key_color != 0xff123456);
    assert(gPreview.theme.button_face != first_key_color);
    assert(gUndoCount == count + 2);
    undo_click(NULL, NULL);
    assert(gPreview.theme.button_face == first_key_color);
    undo_click(NULL, NULL);
    assert(gPreview.theme.button_face == 0xff123456);
    // Changing pages cancels the picker's drag and hidden controls lose focus.
    os64_ui_set_focus(&gEditor, &gPicker.w);
    gEditor.grab = &gPicker.w; gPicker.drag_part = 1; gPicker.w.pressed = true;
    tab_click(NULL, (void *)0);
    assert(!gEditor.grab && !gPicker.drag_part && gEditor.focus == &gTabs[0]);
    // The application's multi-tree resize path preserves its active text field.
    os64_ui_set_focus(&gEditor, &gNameField.w);
    os64_gui_event_t resize = {.type = OS64_GUI_EVENT_WINDOW_RESIZE};
    dispatch(&resize);
    assert(gEditor.focus == &gNameField.w && gNameField.w.focused);
    size_t name_length = strlen(gName);
    os64_gui_event_t key = {.type = OS64_GUI_EVENT_KEY_DOWN, .key = {.ascii = 'x'}};
    dispatch(&key);
    assert(strlen(gName) == name_length + 1);
    tab_click(NULL, (void *)1);
    os64_ui_textfield_set(&gEditor, &gHexField, "12AB");
    os64_ui_set_focus(&gEditor, &gHexField.w);
    dispatch(&resize);
    assert(gEditor.focus == &gHexField.w && !strcmp(gHex, "12AB"));
    os64_free(pixels);
    gCtx.surf.pixels = NULL;
    // The widgets measured text, so each of these windows is holding a text
    // context and its builtin role set. A program just exits; a leak-checked
    // harness has to hand them back.
    os64_ui_font_release(&gEditor);
    os64_ui_font_release(&gPreview);
    puts("appearance customizer: native layout, palette edits, hex validation, Undo and page cancellation passed");
}
