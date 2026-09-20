// Appearance Workshop: preview palettes, treatments and independent font
// roles, then explicitly choose session or startup use. Named compositions
// preserve unowned font/future settings alongside their editable theme.
// Design and persistence contracts: APPEARANCE.md.
#include "os64/os64.h"
#include "os64/ui.h"
#include "font_page.h"

// Keep the WM constraint and the layout fallback in the same content units.
enum { WORKSHOP_MIN_WIDTH = 958, WORKSHOP_MIN_HEIGHT = 696 };

static os64_draw_ctx_t gCtx;
static os64_ui_t gEditor, gPreview;
static os64_ui_t *gInput;
static os64_ui_widget_t gRoot, gHeading, gIntro, gPaletteLabel, gStyleLabel;
static os64_ui_widget_t gStyles[2], gFooter, gSmall, gApply;
static os64_ui_widget_t gPages[4], gTabs[4], gCorners[2], gCornerLabel, gTreatmentHint;
static os64_ui_widget_t gUndo, gResetComponent, gColorTitle, gHexLabel, gSetHex, gFollow;
static os64_ui_widget_t gColorHint, gStates[4], gMenuSample;
static os64_ui_listbox_t gColors;
static os64_ui_scrollbar_t gColorScroll;
static os64_ui_checkbox_t gIndividual;
static os64_ui_textfield_t gHexField;
static os64_ui_colorpicker_t gPicker;
static char gHex[8], gSelectedColor[64];
static unsigned gPage = 1;
static os64_ui_theme_t gUndoThemes[32];
static size_t gUndoCount;
static bool gPickerEditing;
static void sync_color(void);
static void remember_edit(void);
static void refresh_composition(void);
static os64_ui_widget_t gCanvas, gPreviewTitle, gFieldLabel, gAction, gReset;
static os64_ui_widget_t gCount, gTextLabel;
static os64_ui_widget_t gLevelLabel, gCollectionLabel, gNameLabel, gStartupLabel;
static os64_ui_widget_t gLoad, gRefresh, gSave, gStartup;
static os64_ui_listbox_t gThemes, gPalettes;
static os64_ui_scrollbar_t gThemeScroll;
static os64_ui_textfield_t gNameField;
static char gName[OS64_UI_THEME_NAME_MAX + 1], gLoadedName[OS64_UI_THEME_NAME_MAX + 1];
static os64_ui_theme_t gBaseline, gStartupTheme;
static os64_ui_theme_entry_t gSaved[128];
static size_t gSavedCount;
static char gLoadedEnvelope[4097];
static size_t gLoadedEnvelopeLength;
static os64_ui_t gDialog;
static os64_ui_widget_t gDialogRoot, gDialogTitle, gDialogBody, gConfirm, gCancel;
static int gPendingLoad;
static enum { CONFIRM_NONE, CONFIRM_LOAD, CONFIRM_CLOSE, CONFIRM_REPLACE } gConfirmation;
static void layout(void);
static os64_font_status_t dialog_layout(os64_ui_t *ui, bool staged);
static void refresh_collection(void);
static void load_selected(void);
static os64_ui_checkbox_t gEnable, gCheck;
static os64_ui_slider_t gLevel;
static os64_ui_textfield_t gField;
static os64_ui_textview_t gText;
static os64_ui_scrollbar_t gScroll;
static char gFieldText[96];
static char gCountText[48], gComposition[160];
static const char *gApplyStatus = "Preview";
static char gLevelText[32];
static uint32_t gClicks;
static unsigned gPalette, gStyle;
static uint32_t gChanged = OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
static bool gCompact;
static void font_status(const char *text) { gApplyStatus = text; refresh_composition(); }

static const char *const kNames[] = {
    "Midnight Workshop", "Paper & Graphite", "Electric Workstation", "Current palette"
};
static const char *const kLines[] = {
    "A small workshop, after dark.",
    "",
    "Warm light. Crisp edges. Room to think.",
    "",
    "Try selecting a few words with the mouse.",
    "The scrollbar moves this real text view.",
    "Type into the field above, then press Enter.",
    "",
    "A palette is one part of the composition.",
    "Raised buttons can wear any of these colors.",
    "Your text stays put while the look changes.",
    "",
    "Modern, classic, or something of your own.",
    "",
    "Built for os64, with room to grow.",
    "End of sample."
};

static size_t line_count(void *user)
{
    (void)user;
    return sizeof(kLines) / sizeof(kLines[0]);
}

static const char *line_at(void *user, size_t index, size_t *len)
{
    const char *s = index < line_count(user) ? kLines[index] : "";
    *len = os64_strlen(s);
    return s;
}

static const os64_ui_textbuf_t kText = {
    .line_count = line_count, .line = line_at
};

static bool same_theme(const os64_ui_theme_t *a, const os64_ui_theme_t *b)
{
    const unsigned char *aa = (const unsigned char *)a;
    const unsigned char *bb = (const unsigned char *)b;
    for (size_t i = 0; i < sizeof(*a); ++i)
        if (aa[i] != bb[i]) return false;
    return true;
}

static bool theme_dirty(void)
{ return !same_theme(&gPreview.theme, &gBaseline) || !os64_streq(gName, gLoadedName); }
static bool draft_dirty(void)
{ return font_page_dirty() || theme_dirty(); }

static void describe_palette(void)
{
    gStyle = gPreview.theme.button_bevel >= 2;
    gPalette = 3;
    for (unsigned i = 0; i < 3; ++i) {
        os64_ui_theme_t named = gPreview.theme;
        os64_ui_theme_palette(&named, (os64_ui_palette_t)i);
        if (same_theme(&named, &gPreview.theme)) { gPalette = i; break; }
    }
    os64_ui_listbox_set(&gEditor, &gPalettes, 3, gPalette < 3 ? (int)gPalette : -1);
    sync_color();
}

static void refresh_composition(void)
{
    os64_snprintf(gComposition, sizeof(gComposition), "%s | %s", draft_dirty() ? "Unsaved changes" : "Saved / unchanged", gApplyStatus);
    gStyles[0].text = gStyle ? "Flat" : "Flat *";
    gStyles[1].text = gStyle ? "Raised *" : "Raised";
    gCorners[0].text = gPreview.theme.control_radius ? "Square" : "Square *";
    gCorners[1].text = gPreview.theme.control_radius ? "Gently rounded *" : "Gently rounded";
    os64_ui_set_enabled(&gEditor, &gUndo, gPage == 3 ? font_page_can_undo() : gUndoCount != 0);
    gStartupLabel.text = same_theme(&gPreview.theme, &gStartupTheme) ?
        "Startup: this composition" : "Startup: another appearance";
    os64_ui_mark_dirty(&gEditor, &gRoot);
    os64_ui_mark_dirty(&gPreview, &gCanvas);
}

static void palette_click(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    os64_ui_theme_t next = gPreview.theme;
    os64_ui_theme_palette(&next, (os64_ui_palette_t)list->selected);
    if (same_theme(&next, &gPreview.theme)) return;
    remember_edit();
    gPreview.theme = next;
    gApplyStatus = "Preview changed";
    gChanged |= OS64_UI_COMPONENT_PALETTE;
    gPalette = (unsigned)list->selected;
    sync_color();
    refresh_composition();
}

static void style_click(os64_ui_widget_t *w, void *user)
{
    (void)w;
    os64_ui_theme_t next = gPreview.theme;
    next.button_bevel = user ? 3 : 0;
    if (same_theme(&next, &gPreview.theme)) return;
    remember_edit();
    gPreview.theme = next;
    gApplyStatus = "Preview changed";
    gChanged |= OS64_UI_COMPONENT_TREATMENT;
    gStyle = (unsigned)(uintptr_t)user;
    refresh_composition();
}


static void remember_edit(void)
{
    if (gUndoCount == 32) {
        for (size_t i = 1; i < 32; ++i) gUndoThemes[i - 1] = gUndoThemes[i];
        --gUndoCount;
    }
    gUndoThemes[gUndoCount++] = gPreview.theme;
}

static uint32_t selected_color(void)
{
    size_t i = gColors.selected >= 0 ? (size_t)gColors.selected : 0;
    return gIndividual.checked ? os64_ui_theme_color_get(&gPreview.theme, i) :
        os64_ui_palette_role_get(&gPreview.theme, i);
}

static void sync_hex(void)
{
    char value[8];
    os64_snprintf(value, sizeof(value), "%06X", selected_color() & 0xffffffu);
    os64_ui_textfield_set(&gEditor, &gHexField, value);
}

static void sync_color(void)
{
    size_t i = gColors.selected >= 0 ? (size_t)gColors.selected : 0;
    os64_snprintf(gSelectedColor, sizeof(gSelectedColor), "%s",
        gIndividual.checked ? os64_ui_theme_color_label(i) : os64_ui_palette_role_name(i));
    os64_ui_colorpicker_set(&gEditor, &gPicker, selected_color());
    sync_hex();
    os64_ui_theme_t probe = gPreview.theme;
    os64_ui_set_enabled(&gEditor, &gFollow, gIndividual.checked &&
        os64_ui_palette_color_follow(&probe, i));
    os64_ui_mark_dirty(&gEditor, &gRoot);
}

static void edit_color(uint32_t color)
{
    size_t i = gColors.selected >= 0 ? (size_t)gColors.selected : 0;
    if (gIndividual.checked) os64_ui_theme_color_set(&gPreview.theme, i, color);
    else os64_ui_palette_role_set(&gPreview.theme, i, color);
    gChanged |= OS64_UI_COMPONENT_PALETTE;
    gPalette = 3;
    os64_ui_listbox_set(&gEditor, &gPalettes, 3, -1);
    gApplyStatus = "Color changed in preview";
    sync_hex();
    refresh_composition();
}

static void picker_changed(os64_ui_colorpicker_t *picker, void *user)
{
    (void)user;
    if (!gPickerEditing) remember_edit();
    gPickerEditing = picker->w.pressed;
    edit_color(picker->color);
}

static void hex_submit(os64_ui_textfield_t *field, void *user)
{
    (void)user;
    uint32_t color = 0;
    if (field->len != 6) goto invalid;
    for (size_t i = 0; i < 6; ++i) {
        char c = field->buf[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A') + 10;
        else goto invalid;
        color = (color << 4) | digit;
    }
    color |= 0xff000000u;
    if (color != selected_color()) { remember_edit(); edit_color(color); }
    sync_color();
    return;
invalid:
    gApplyStatus = "Enter six hex digits, such as 437DE0";
    refresh_composition();
}

static void hex_click(os64_ui_widget_t *w, void *user)
{ (void)w; hex_submit(&gHexField, user); }

static const char *color_label(size_t index, void *user)
{
    (void)user;
    return gIndividual.checked ? os64_ui_theme_color_label(index) : os64_ui_palette_role_name(index);
}

static uint32_t color_swatch(size_t index, void *user)
{
    (void)user;
    return gIndividual.checked ? os64_ui_theme_color_get(&gPreview.theme, index) :
        os64_ui_palette_role_get(&gPreview.theme, index);
}

static void color_selection(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    os64_ui_scrollbar_set(&gEditor, &gColorScroll, (int64_t)list->count,
        os64_ui_listbox_rows(list, &gEditor.theme), (int64_t)list->top);
    sync_color();
}

static void colors_scrolled(os64_ui_scrollbar_t *scroll, void *user)
{
    (void)user;
    os64_ui_listbox_scroll_to(&gEditor, &gColors, (size_t)scroll->pos);
}

static void individual_changed(os64_ui_checkbox_t *cb, void *user)
{
    (void)user;
    os64_ui_listbox_set(&gEditor, &gColors, cb->checked ?
        os64_ui_theme_color_count() : OS64_UI_PALETTE_ROLE_COUNT, 0);
    gColorHint.text = cb->checked ? "Fine-tune a control color." : "Shared roles keep colors together.";
    color_selection(&gColors, NULL);
}

static void follow_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    os64_ui_theme_t next = gPreview.theme;
    if (gColors.selected < 0 || !os64_ui_palette_color_follow(&next, (size_t)gColors.selected) ||
        same_theme(&next, &gPreview.theme)) return;
    remember_edit();
    gPreview.theme = next;
    gChanged |= OS64_UI_COMPONENT_PALETTE;
    describe_palette();
    gApplyStatus = "Color follows its shared role";
    refresh_composition();
}

static void undo_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gPage == 3) { font_page_undo(); return; }
    if (!gUndoCount) return;
    gPreview.theme = gUndoThemes[--gUndoCount];
    gChanged = OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
    describe_palette();
    gApplyStatus = "Edit undone in preview";
    refresh_composition();
}

static void reset_component_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gPage == 3) { font_page_reset(); return; }
    uint32_t component = gPage == 2 ? OS64_UI_COMPONENT_TREATMENT : OS64_UI_COMPONENT_PALETTE;
    os64_ui_theme_t next = gPreview.theme;
    os64_ui_theme_merge(&next, &gBaseline, component);
    if (same_theme(&next, &gPreview.theme)) return;
    remember_edit();
    gPreview.theme = next;
    gChanged |= component;
    describe_palette();
    gApplyStatus = "Component reset to loaded / saved values";
    refresh_composition();
}

static void corner_click(os64_ui_widget_t *w, void *user)
{
    (void)w;
    os64_ui_theme_t next = gPreview.theme;
    next.control_radius = user ? 6 : 0;
    if (same_theme(&next, &gPreview.theme)) return;
    remember_edit();
    gPreview.theme = next;
    gChanged |= OS64_UI_COMPONENT_TREATMENT;
    gApplyStatus = "Corners changed in preview";
    refresh_composition();
}

static void tab_click(os64_ui_widget_t *w, void *user)
{
    (void)w;
    os64_ui_cancel_interaction(&gEditor);
    gPickerEditing = false;
    gPage = (unsigned)(uintptr_t)user;
    if (gPage == 3) font_page_activate();
    layout();
    os64_ui_set_focus(&gEditor, &gTabs[gPage]);
}

// These specimens hold visual states so palette changes can be compared
// together. The normal button, field, checkbox, slider and text view interact.
static void state_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    // Initialize through libui so the non-PIE application does not import a
    // shared-library data symbol via an unsupported ELF COPY relocation.
    os64_ui_widget_t copy;
    os64_ui_button(&copy, w->text, NULL, NULL);
    copy.bounds = w->bounds;
    copy.hovered = (uintptr_t)w->user == 0;
    copy.pressed = (uintptr_t)w->user == 1;
    copy.focused = (uintptr_t)w->user == 2;
    copy.cls->paint(&copy, ctx, t);
}
static const os64_ui_class_t kStateClass = {"state sample", state_paint, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static void menu_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_gui_rect_t r = w->bounds;
    os64_draw_fill_rect(&ctx->surf, r, t->menu_bg);
    os64_draw_rect(&ctx->surf, r, t->menu_sep);
    os64_draw_text_clipped(&ctx->surf, r, r.x + 10, r.y + 7,
        "File   Edit   View", 18, t->menu_fg, t->menu_bg);
    os64_draw_hline(&ctx->surf, r.x + 6, r.y + 28, r.w - 12, t->menu_sep);
    os64_gui_rect_t selected = {r.x + 6, r.y + 33, r.w - 12, 24};
    os64_draw_fill_rect(&ctx->surf, selected, t->menu_hi_bg);
    os64_draw_text_clipped(&ctx->surf, selected, r.x + 10, r.y + 37,
        "Selected menu item", 18, t->menu_hi_fg, t->menu_hi_bg);
}
static const os64_ui_class_t kMenuClass = {"menu sample", menu_paint, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static void apply_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gPage == 3) { font_page_apply(); return; }
    uint64_t generation;
    // After an Apply, only edited components replace the latest session.
    // An unchanged preview can still be explicitly reapplied as a composition.
    uint32_t components = gChanged ? gChanged :
        OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
    int result = os64_ui_theme_apply(&gPreview.theme, components, &generation);
    if (result == 0) {
        gApplyStatus = "Applied to this session";
        os64_ui_theme_session(&gEditor.theme, &gEditor.appearance_generation, generation);
        // Show the merged result, including components another publisher owned.
        uint64_t installed = 0;
        os64_ui_theme_session(&gPreview.theme, &installed, generation);
        describe_palette();
        gChanged = 0;
    } else if (result == OS64_UI_APPLY_CONFLICT) {
        gApplyStatus = "Changed elsewhere; Apply again to retry";
    } else if (result == OS64_UI_APPLY_INVALID) {
        gApplyStatus = "Cannot apply this appearance";
    } else if (result == OS64_UI_APPLY_EXHAUSTED) {
        gApplyStatus = "Session limit reached; restart to apply";
    } else {
        gApplyStatus = "Apply failed; your preview is unchanged";
    }
    if (result) os64_complain("appearance: Apply failed (%d)\n", result);
    refresh_composition();
}

static const char *theme_label(size_t index, void *user)
{
    (void)user;
    static const char *const presets[] = {
        "Midnight Workshop [preset]", "Paper & Graphite [preset]", "Electric Workstation [preset]"
    };
    return index < 3 ? presets[index] : gSaved[index - 3].name;
}

static const char *palette_label(size_t index, void *user)
{
    (void)user;
    return kNames[index];
}

static void theme_selection(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    os64_ui_scrollbar_set(&gEditor, &gThemeScroll, (int64_t)list->count,
        os64_ui_listbox_rows(list, &gEditor.theme), (int64_t)list->top);
}

static void themes_scrolled(os64_ui_scrollbar_t *scroll, void *user)
{
    (void)user;
    os64_ui_listbox_scroll_to(&gEditor, &gThemes, (size_t)scroll->pos);
}

static void refresh_collection(void)
{
    os64_ui_theme_startup(&gStartupTheme);
    int count = os64_ui_theme_list(gSaved, sizeof(gSaved) / sizeof(gSaved[0]));
    gSavedCount = count >= 0 ? (size_t)count : 0;
    if (count < 0) gApplyStatus = count == OS64_UI_THEME_LIMIT ?
        "Collection exceeds 128 saved themes" : "Cannot read saved themes";
    os64_ui_listbox_set(&gEditor, &gThemes, gSavedCount + 3, -1);
    theme_selection(&gThemes, NULL);
}

static void refresh_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    gApplyStatus = "Collection refreshed; preview kept";
    refresh_collection();
    refresh_composition();
}

static void ask_confirmation(int kind)
{
    gConfirmation = kind;
    os64_ui_cancel_interaction(&gEditor);
    os64_ui_cancel_interaction(&gPreview);
    gDialog.theme = gEditor.theme;
    gDialogTitle.text = kind == CONFIRM_REPLACE ? "Replace saved theme?" : "Discard unsaved changes?";
    gDialogBody.text = kind == CONFIRM_REPLACE ? gName : "Keep editing to save your composition first.";
    gConfirm.text = kind == CONFIRM_REPLACE ? "Replace" : "Discard changes";
    dialog_layout(&gDialog,false);
    os64_ui_set_focus(&gDialog, &gCancel);
    os64_ui_mark_dirty(&gDialog, &gDialogRoot);
    os64_ui_mark_dirty(&gEditor, &gRoot);
}

static void load_selected(void)
{
    int index = gPendingLoad;
    if (index < 0 || (size_t)index >= gSavedCount + 3) return;
    os64_ui_theme_t theme;
    os64_ui_theme_defaults(&theme);
    const char *name;
    char envelope[4097];
    size_t envelope_length = 0;
    if (index < 3) {
        os64_ui_theme_palette(&theme, (os64_ui_palette_t)index);
        name = kNames[index];
    } else {
        name = gSaved[index - 3].name;
        int result = os64_ui_theme_load_snapshot(name, &theme, envelope, sizeof(envelope), &envelope_length);
        if (result) {
            gApplyStatus = result == OS64_UI_THEME_INVALID ?
                "Invalid saved theme; preview kept" : "Cannot load theme; preview kept";
            refresh_composition();
            return;
        }
    }
    if (envelope_length) os64_memcpy(gLoadedEnvelope, envelope, envelope_length);
    gLoadedEnvelopeLength = envelope_length;
    gPreview.theme = theme;
    gUndoCount = 0;
    gBaseline = theme;
    os64_strcopy(gLoadedName, sizeof(gLoadedName), name);
    os64_ui_textfield_set(&gEditor, &gNameField, name);
    gChanged = OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
    describe_palette();
    gApplyStatus = "Loaded into preview";
    refresh_composition();
}

static void load_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gThemes.selected < 0) {
        gApplyStatus = "Select a theme to load";
        refresh_composition();
        return;
    }
    gPendingLoad = gThemes.selected;
    if (theme_dirty()) ask_confirmation(CONFIRM_LOAD);
    else load_selected();
}

static void save_draft(bool replace)
{
    int result = os64_ui_theme_save_snapshot(gName, &gPreview.theme,
        gLoadedEnvelopeLength ? gLoadedEnvelope : NULL, gLoadedEnvelopeLength, replace);
    if (result == OS64_UI_THEME_EXISTS) { ask_confirmation(CONFIRM_REPLACE); return; }
    if (!result) {
        gBaseline = gPreview.theme;
        os64_strcopy(gLoadedName, sizeof(gLoadedName), gName);
        gApplyStatus = "Saved; session unchanged";
        refresh_collection();
        for (size_t i = 0; i < gSavedCount; ++i)
            if (os64_streq(gSaved[i].name, gName))
                os64_ui_listbox_set(&gEditor, &gThemes, gSavedCount + 3, (int)i + 3);
        theme_selection(&gThemes, NULL);
    } else if (result == OS64_UI_THEME_INVALID) {
        gApplyStatus = "Name: 1-40 letters, digits, spaces, - _ & ( )";
    } else {
        gApplyStatus = "Save failed; preview kept (disk or atomic replace unavailable)";
    }
    refresh_composition();
}

static void save_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gPage == 3) font_page_save();
    else save_draft(false);
}

static void startup_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (draft_dirty()) gApplyStatus = "Save your changes before choosing startup";
    else if (os64_ui_theme_set_startup(&gPreview.theme) != 0)
        gApplyStatus = "Startup save failed; previous choice kept";
    else {
        gStartupTheme = gPreview.theme;
        os64_ui_theme_session(&gEditor.theme, &gEditor.appearance_generation, 0);
        gApplyStatus = "Chosen for startup; session unchanged";
    }
    refresh_composition();
}

static void confirm_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    int kind = gConfirmation;
    gConfirmation = CONFIRM_NONE;
    os64_ui_cancel_interaction(&gDialog);
    if (kind == CONFIRM_CLOSE) gEditor.quit = true;
    else if (kind == CONFIRM_LOAD) load_selected();
    else if (kind == CONFIRM_REPLACE) save_draft(true);
    refresh_composition();
}

static void cancel_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    gConfirmation = CONFIRM_NONE;
    os64_ui_cancel_interaction(&gDialog);
    refresh_composition();
}

static void count_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    os64_snprintf(gCountText, sizeof(gCountText), "Button presses: %u", ++gClicks);
    os64_ui_mark_dirty(&gPreview, &gCount);
}

static void level_changed(os64_ui_slider_t *sl, void *user)
{
    (void)user;
    os64_snprintf(gLevelText, sizeof(gLevelText), "Sample level: %d", sl->value);
    os64_ui_mark_dirty(&gPreview, &gLevelLabel);
}

static void enabled_changed(os64_ui_checkbox_t *cb, void *user)
{
    (void)user;
    os64_ui_set_enabled(&gPreview, &gField.w, cb->checked);
    os64_ui_set_enabled(&gPreview, &gAction, cb->checked);
    os64_ui_set_enabled(&gPreview, &gCheck.w, cb->checked);
    os64_ui_set_enabled(&gPreview, &gLevel.w, cb->checked);
}

static void reset_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    gClicks = 0;
    os64_strcopy(gCountText, sizeof(gCountText), "Button presses: 0");
    os64_ui_textfield_set(&gPreview, &gField, "A place to make it yours");
    os64_ui_textview_goto(&gPreview, &gText, 0, 0, false);
    os64_ui_checkbox_set(&gPreview, &gEnable, true);
    os64_ui_checkbox_set(&gPreview, &gCheck, true);
    enabled_changed(&gEnable, NULL);
    os64_ui_slider_set(&gPreview, &gLevel, 60);
    level_changed(&gLevel, NULL);
    os64_ui_mark_dirty(&gPreview, &gCanvas);
}

static void field_submit(os64_ui_textfield_t *field, void *user)
{
    (void)field;
    count_click(NULL, user);
}

static void view_changed(os64_ui_textview_t *tv, void *user)
{
    (void)user;
    os64_ui_scrollbar_set(&gPreview, &gScroll, (int64_t)line_count(NULL),
                         os64_ui_textview_rows(tv, &gPreview.theme),
                         (int64_t)tv->top);
}

static void scroll_changed(os64_ui_scrollbar_t *sb, void *user)
{
    (void)user;
    os64_ui_textview_scroll_to(&gPreview, &gText, (size_t)sb->pos);
}

typedef struct {
    int row, scale, button, field, check, top, footer, min_w, min_h;
    int width, height;
} workshop_layout_t;
static os64_ui_t *gStagingLayout;
static workshop_layout_t gLayout;
static bool gHaveLayout;

static int larger(int a, int b) { return a > b ? a : b; }
static int scaled(const workshop_layout_t *m, int value)
{ return (value * m->scale + 19) / 20; }
static void place(os64_ui_widget_t *w, int x, int y, int width, int height)
{
    os64_gui_rect_t r = {x,y,width,height};
    if (gStagingLayout) {
        if (w->ui == gStagingLayout) os64_ui_widget_stage_bounds(w, r);
    } else w->bounds = r;
}

static os64_font_status_t measure_layout(os64_ui_t *ui, workshop_layout_t *m)
{
    *m = (workshop_layout_t){0};
    m->row = larger(20, os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI));
    m->scale = m->row;
    /* Row height supplies a comfortable starting width; measured captions
     * also protect wide faces whose advances grow faster than their rows. */
    static const struct { const char *text; int width; } captions[] = {
        {"Apply to session",168}, {"Reset component",196}, {"Save",80},
        {"Interface *",132}, {"Terminal *",132}, {"Document *",132},
        {"Install font",194}, {"Refresh fonts",206}, {"Controls *",106},
        {"Follow palette",186}, {"Set",66}, {"Use at startup",412},
        {"Undo",96}, {"Load theme",192}, {"Refresh",208},
        {"Composition name",144}, {"Reset sample",116},
        {"Keep editing",210}, {"Discard changes",210}
    };
    for (size_t i = 0; i < sizeof(captions)/sizeof(captions[0]); ++i) {
        int32_t width;
        os64_font_status_t status = os64_ui_text_measure(ui, OS64_FONT_ROLE_UI,
            captions[i].text, os64_strlen(captions[i].text), &width);
        if (status) return status;
        m->scale = larger(m->scale, ((width + 12) * 20 + captions[i].width - 1) / captions[i].width);
    }
    m->button = larger(32, m->row + 12);
    m->field = larger(30, m->row + 8);
    m->check = larger(26, m->row + 4);
    m->top = 16 + 2*m->row + 8 + 14 + m->button + 12;
    m->footer = 2*m->field + 20 + m->row + 6;
    int page_min = 266 + 5*m->row + 2*m->check + m->field;
    m->min_w = larger(WORKSHOP_MIN_WIDTH, 68 + scaled(m,444) + scaled(m,446));
    m->min_h = larger(WORKSHOP_MIN_HEIGHT, m->top + page_min + 16 + m->footer);
    m->width = larger((int)gCtx.surf.width, m->min_w);
    m->height = larger((int)gCtx.surf.height, m->min_h);
    return OS64_FONT_OK;
}

static os64_font_status_t fit_window(workshop_layout_t *m)
{
    if (m->width == (int)gCtx.surf.width && m->height == (int)gCtx.surf.height)
        return OS64_FONT_OK;
    uint32_t sw, sh;
    os64_gui_window_state_t state;
    if (gCtx.win <= 0 || os64_gui_screen_info(&sw,&sh) ||
        os64_gui_window_get_state(gCtx.win,&state)) return OS64_FONT_LIMIT;
    int64_t right = larger(0,state.x) + (int64_t)m->width + state.width - gCtx.surf.width;
    int64_t bottom = larger(0,state.y) + (int64_t)m->height + state.height - gCtx.surf.height;
    return right <= sw && bottom <= sh ? OS64_FONT_OK : OS64_FONT_LIMIT;
}

static os64_font_status_t dialog_layout(os64_ui_t *ui, bool staged)
{
    int row = larger(20, os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI));
    int button = larger(36,row+12), dw = 520;
    const char *body = "Keep editing to save your composition first.";
    int32_t width = 0;
    os64_font_status_t status = os64_ui_text_measure(ui, OS64_FONT_ROLE_UI,
        body, os64_strlen(body), &width);
    if (status && staged) return status;
    dw = larger(dw, width + 40);
    int dh = 60 + 2*row + 16 + button;
    if (staged && (dw > (int)gCtx.surf.width || dh > (int)gCtx.surf.height))
        return OS64_FONT_LIMIT;
    dw = dw < (int)gCtx.surf.width ? dw : (int)gCtx.surf.width;
    int dx = ((int)gCtx.surf.width-dw)/2, dy = ((int)gCtx.surf.height-dh)/2;
    gStagingLayout = staged ? ui : NULL;
    place(&gDialogRoot,dx,dy,dw,dh);
    place(&gDialogTitle,dx+20,dy+20,dw-40,row);
    place(&gDialogBody,dx+20,dy+28+row,dw-40,row);
    place(&gConfirm,dx+20,dy+44+2*row,(dw-60)/2,button);
    place(&gCancel,dx+(dw+20)/2,dy+44+2*row,(dw-60)/2,button);
    gStagingLayout = NULL;
    return OS64_FONT_OK;
}

static void arrange(const workshop_layout_t *m, bool staged)
{
    int width=m->width, height=m->height, row=m->row, bh=m->button;
    int left=scaled(m,444), px=24+left+20, pw=width-px-24;
    int footer=height-m->footer, page_h=footer-16-m->top;
    gStagingLayout = staged ? &gEditor : NULL;
    place(&gRoot,0,0,width,height);
    place(&gSmall,0,0,width,height);
    place(&gHeading,24,16,width-48,row);
    place(&gIntro,24,24+row,width-48,row);
    static const char *const tabs[]={"Themes","Palette","Controls","Fonts"};
    static const char *const active[]={"Themes *","Palette *","Controls *","Fonts *"};
    for (unsigned i=0;i<4;++i) {
        place(&gTabs[i],24+(int)i*scaled(m,112),m->top-12-bh,scaled(m,106),bh);
        place(&gPages[i],24,m->top,left,page_h);
        if (!staged) {
            gTabs[i].text=gPage==i?active[i]:tabs[i];
            os64_ui_set_hidden(&gEditor,&gPages[i],i!=gPage);
        }
    }
    int x=40, inner=left-32, y=m->top+16;
    place(&gCollectionLabel,x,y,inner,row);
    int list_y=y+row+10, bottom=m->top+page_h-16;
    int startup_y=bottom-row-8-bh;
    int load_y=startup_y-12-bh;
    place(&gThemes.w,x,list_y,inner-18,load_y-12-list_y);
    place(&gThemeScroll.w,x+inner-16,list_y,16,load_y-12-list_y);
    place(&gLoad,x,load_y,(inner-12)/2,bh);
    place(&gRefresh,x+(inner+12)/2,load_y,(inner-12)/2,bh);
    place(&gStartup,x,startup_y,inner,bh);
    place(&gStartupLabel,x,bottom-row,inner,row);

    place(&gPaletteLabel,x,y,inner,row); y+=row+8;
    place(&gPalettes.w,x,y,inner,2*row+8); y+=2*row+18;
    place(&gIndividual.w,x,y,inner,m->check); y+=m->check+12;
    int right=scaled(m,186), rx=x+inner-right;
    place(&gColors.w,x,y,rx-x-28,bottom-row-12-y);
    place(&gColorScroll.w,rx-24,y,14,bottom-row-12-y);
    place(&gColorTitle,rx,y,right,row); y+=row+8;
    place(&gPicker.w,rx,y,right,156); y+=168;
    int hex=scaled(m,92), hash=scaled(m,16), set=scaled(m,66);
    place(&gHexLabel,rx,y,hash,m->field);
    place(&gHexField.w,rx+hash+4,y,hex,m->field);
    place(&gSetHex,rx+right-set,y,set,m->field); y+=m->field+8;
    place(&gFollow,rx,y,right,m->check);
    place(&gColorHint,x,bottom-row,inner,row);

    y=m->top+16;
    place(&gStyleLabel,x,y,inner,row); y+=row+12;
    for(unsigned i=0;i<2;++i) place(&gStyles[i],x+(int)i*(inner+12)/2,y,(inner-12)/2,bh);
    y+=bh+20; place(&gCornerLabel,x,y,inner,row); y+=row+12;
    for(unsigned i=0;i<2;++i) place(&gCorners[i],x+(int)i*(inner+12)/2,y,(inner-12)/2,bh);
    y+=bh+20; place(&gTreatmentHint,x,y,inner,row);

    place(&gNameLabel,24,footer,scaled(m,144),m->field);
    place(&gNameField.w,24+scaled(m,152),footer,scaled(m,344),m->field);
    place(&gSave,width-24-scaled(m,256),footer,scaled(m,80),m->field);
    place(&gApply,width-24-scaled(m,168),footer,scaled(m,168),m->field);
    place(&gUndo,24,footer+m->field+10,scaled(m,96),m->field);
    place(&gResetComponent,24+scaled(m,108),footer+m->field+10,scaled(m,196),m->field);
    place(&gFooter,24,height-row-6,width-48,row);

    /* The specimen is a separate font consumer. Its viewport follows the
     * committed editor layout, as it does on an ordinary window resize. */
    if (!staged) {
        place(&gCanvas,px,m->top,pw,page_h);
        int py=m->top+16, bx=px+16, iw=pw-32;
        place(&gPreviewTitle,bx,py,iw,row); py+=row+8;
        place(&gMenuSample,bx,py,iw,larger(64,row+16)); py+=larger(64,row+16)+8;
        place(&gFieldLabel,bx,py,iw,row); py+=row+8;
        place(&gField.w,bx,py,iw,bh); py+=bh+8;
        int bw=(iw-16)/5;
        place(&gAction,bx,py,bw,bh);
        for(int i=0;i<4;++i) place(&gStates[i],bx+(i+1)*(bw+4),py,bw,bh);
        py+=bh+8;
        place(&gEnable.w,bx,py,scaled(m,204),m->check);
        place(&gCheck.w,bx+scaled(m,224),py,iw-scaled(m,224),m->check); py+=m->check+8;
        place(&gLevelLabel,bx,py,scaled(m,144),larger(28,row));
        place(&gLevel.w,bx+scaled(m,164),py,iw-scaled(m,164),larger(28,row)); py+=larger(28,row)+8;
        place(&gTextLabel,bx,py,iw,row); py+=row+8;
        int count_y=m->top+page_h-16-m->field;
        place(&gText.w,bx,py,iw-18,count_y-12-py);
        place(&gScroll.w,bx+iw-16,py,16,count_y-12-py);
        place(&gCount,bx,count_y,iw-scaled(m,132),m->field);
        place(&gReset,bx+iw-scaled(m,116),count_y,scaled(m,116),m->field);
    }
    gStagingLayout = NULL;
    font_page_layout(gPage==3,(os64_gui_rect_t){24,m->top,left,page_h},row,staged);
    if (!staged) {
        gCompact=false; gSmall.hidden=true; gCanvas.hidden=false;
        os64_ui_set_hidden(&gEditor,&gResetComponent,gPage==0);
        os64_ui_set_hidden(&gEditor,&gNameField.w,gPage==3);
        os64_ui_set_hidden(&gEditor,&gNameLabel,gPage==3);
        os64_ui_scrollbar_set(&gEditor,&gColorScroll,(int64_t)gColors.count,
            os64_ui_listbox_rows(&gColors,&gEditor.theme),(int64_t)gColors.top);
        theme_selection(&gThemes,NULL); view_changed(&gText,NULL); refresh_composition();
    }
}

static void layout(void)
{
    if (!gHaveLayout) {
        if (measure_layout(&gEditor,&gLayout)) return;
        gHaveLayout=true;
    }
    workshop_layout_t m=gLayout;
    /* Resize events use the actual surface. The WM enforces the accepted
     * minimum; no hidden-controls fallback is used for a valid font layout. */
    m.width=(int)gCtx.surf.width; m.height=(int)gCtx.surf.height;
    arrange(&m,false);
    dialog_layout(&gDialog,false);
}

static os64_font_status_t plan_workshop(os64_ui_t *ui, void *user, void **out)
{
    (void)user; *out=NULL;
    workshop_layout_t *m=os64_malloc(sizeof(*m));
    if (!m) return OS64_FONT_NO_MEMORY;
    os64_font_status_t status=measure_layout(ui,m);
    if (!status) status=fit_window(m);
    if (status) { os64_free(m); return status; }
    arrange(m,true); *out=m; return OS64_FONT_OK;
}
static void discard_workshop(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; os64_free(plan); }
static void commit_workshop(os64_ui_t *ui, void *user, void *plan)
{
    (void)user;
    workshop_layout_t *m=plan;
    /* The plan fits the reserved canvas. Setting its minimum allocates no
     * memory; a live owned window can accept it. Refresh after any growth. */
    if (gCtx.win>0 && (os64_gui_window_set_min_size(gCtx.win,m->min_w,m->min_h) ||
                      os64_draw_ctx_refresh(&gCtx))) {
        os64_complain("appearance: cannot update window minimum"); ui->quit=true;
    } else {
        gLayout=*m; gHaveLayout=true;
        m->width=(int)gCtx.surf.width; m->height=(int)gCtx.surf.height;
        arrange(m,false);
        char line[128];
        os64_snprintf(line, sizeof(line), "appearance: interface row %d, minimum %dx%d",
                      m->row, m->min_w, m->min_h);
        os64_debug_log(line);
    }
    os64_free(m);
}
static os64_font_status_t plan_dialog(os64_ui_t *ui, void *user, void **out)
{
    (void)user; *out=NULL;
    return dialog_layout(ui, true);
}
static void finish_dialog(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }

static void editor_label(os64_ui_widget_t *w, const char *text)
{
    os64_ui_label(w, text);
    os64_ui_add_child(&gRoot, w);
}

static void page_label(unsigned page, os64_ui_widget_t *w, const char *text)
{
    os64_ui_label(w, text);
    os64_ui_add_child(&gPages[page], w);
}

static void preview_label(os64_ui_widget_t *w, const char *text)
{
    os64_ui_label(w, text);
    os64_ui_add_child(&gCanvas, w);
}

static void setup(void)
{
    os64_ui_init(&gEditor, &gCtx);
    os64_ui_init(&gPreview, &gCtx);
    os64_ui_init(&gDialog, &gCtx);
    gDialog.follow_session = false;
    gPreview.follow_session = false;
    os64_ui_theme_defaults(&gEditor.theme);
    os64_ui_theme_palette(&gEditor.theme, OS64_UI_PALETTE_MIDNIGHT);
    gEditor.appearance_generation = 0;
    os64_ui_theme_current(&gEditor.theme, &gEditor.appearance_generation);
    // The editor follows startup and session appearance; the specimen is
    // independently editable and adopts a collection entry only on Load.
    os64_ui_theme_startup(&gStartupTheme);
    os64_ui_theme_defaults(&gPreview.theme);
    os64_ui_theme_palette(&gPreview.theme, OS64_UI_PALETTE_MIDNIGHT);
    os64_ui_panel(&gRoot);
    os64_ui_panel(&gCanvas);
    os64_ui_set_root(&gEditor, &gRoot);
    os64_ui_set_root(&gPreview, &gCanvas);
    for (unsigned i = 0; i < 4; ++i) {
        os64_ui_panel(&gPages[i]);
        os64_ui_add_child(&gRoot, &gPages[i]);
        os64_ui_button(&gTabs[i], "", tab_click, (void *)(uintptr_t)i);
        os64_ui_add_child(&gRoot, &gTabs[i]);
    }
    editor_label(&gHeading, "APPEARANCE WORKSHOP");
    editor_label(&gIntro, "Choose the pieces. Make something yours.");
    page_label(0, &gCollectionLabel, "THEME COLLECTION");
    page_label(1, &gPaletteLabel, "START WITH A PALETTE");
    page_label(2, &gStyleLabel, "SURFACE TREATMENT");
    os64_ui_button(&gApply, "Apply to session", apply_click, NULL);
    os64_ui_add_child(&gRoot, &gApply);
    editor_label(&gFooter, gComposition);
    editor_label(&gSmall, "Enlarge the window to explore the gallery.");
    os64_ui_listbox(&gThemes, 3, theme_label, theme_selection, NULL);
    os64_ui_listbox(&gPalettes, 3, palette_label, palette_click, NULL);
    os64_ui_scrollbar(&gThemeScroll, themes_scrolled, NULL);
    os64_ui_add_child(&gPages[0], &gThemes.w);
    os64_ui_add_child(&gPages[0], &gThemeScroll.w);
    os64_ui_add_child(&gPages[1], &gPalettes.w);
    os64_ui_button(&gLoad, "Load theme", load_click, NULL);
    os64_ui_button(&gRefresh, "Refresh", refresh_click, NULL);
    os64_ui_button(&gSave, "Save", save_click, NULL);
    os64_ui_button(&gStartup, "Use at startup", startup_click, NULL);
    os64_ui_add_child(&gPages[0], &gLoad);
    os64_ui_add_child(&gPages[0], &gRefresh);
    editor_label(&gNameLabel, "Composition name");
    os64_ui_textfield(&gNameField, gName, sizeof(gName), NULL, NULL, NULL);
    os64_ui_add_child(&gRoot, &gNameField.w);
    os64_ui_textfield_set(&gEditor, &gNameField, kNames[0]);
    os64_strcopy(gLoadedName, sizeof(gLoadedName), kNames[0]);
    gBaseline = gPreview.theme;
    os64_ui_add_child(&gRoot, &gSave);
    os64_ui_add_child(&gPages[0], &gStartup);
    page_label(0, &gStartupLabel, "");
    os64_ui_panel(&gDialogRoot);
    os64_ui_set_root(&gDialog, &gDialogRoot);
    os64_ui_label(&gDialogTitle, "");
    os64_ui_label(&gDialogBody, "");
    os64_ui_button(&gConfirm, "", confirm_click, NULL);
    os64_ui_button(&gCancel, "Keep editing", cancel_click, NULL);
    os64_ui_add_child(&gDialogRoot, &gDialogTitle);
    os64_ui_add_child(&gDialogRoot, &gDialogBody);
    os64_ui_add_child(&gDialogRoot, &gConfirm);
    os64_ui_add_child(&gDialogRoot, &gCancel);
    for (unsigned i = 0; i < 2; ++i) {
        os64_ui_button(&gStyles[i], i ? "Raised" : "Flat", style_click,
                       (void *)(uintptr_t)i);
        os64_ui_add_child(&gPages[2], &gStyles[i]);
    }
    os64_ui_button(&gUndo, "Undo", undo_click, NULL);
    os64_ui_button(&gResetComponent, "Reset component", reset_component_click, NULL);
    os64_ui_add_child(&gRoot, &gUndo);
    os64_ui_add_child(&gRoot, &gResetComponent);
    page_label(2, &gCornerLabel, "CORNERS");
    page_label(2, &gTreatmentHint, "Try the states in the live preview.");
    for (unsigned i = 0; i < 2; ++i) {
        os64_ui_button(&gCorners[i], "", corner_click, (void *)(uintptr_t)i);
        os64_ui_add_child(&gPages[2], &gCorners[i]);
    }
    os64_ui_checkbox(&gIndividual, "Individual control colors", false, individual_changed, NULL);
    os64_ui_add_child(&gPages[1], &gIndividual.w);
    os64_ui_listbox(&gColors, OS64_UI_PALETTE_ROLE_COUNT, color_label, color_selection, NULL);
    gColors.swatch = color_swatch;
    os64_ui_listbox_set(&gEditor, &gColors, OS64_UI_PALETTE_ROLE_COUNT, 3);
    os64_ui_scrollbar(&gColorScroll, colors_scrolled, NULL);
    os64_ui_colorpicker(&gPicker, gPreview.theme.button_face, picker_changed, NULL);
    os64_ui_textfield(&gHexField, gHex, sizeof(gHex), hex_submit, NULL, NULL);
    os64_ui_button(&gSetHex, "Set", hex_click, NULL);
    os64_ui_button(&gFollow, "Follow palette", follow_click, NULL);
    os64_ui_add_child(&gPages[1], &gColors.w);
    os64_ui_add_child(&gPages[1], &gColorScroll.w);
    page_label(1, &gColorTitle, gSelectedColor);
    os64_ui_add_child(&gPages[1], &gPicker.w);
    page_label(1, &gHexLabel, "#");
    os64_ui_add_child(&gPages[1], &gHexField.w);
    os64_ui_add_child(&gPages[1], &gSetHex);
    os64_ui_add_child(&gPages[1], &gFollow);
    page_label(1, &gColorHint, "Shared roles keep colors together.");
    preview_label(&gPreviewTitle, "LIVE PREVIEW");
    gMenuSample.cls = &kMenuClass;
    os64_ui_add_child(&gCanvas, &gMenuSample);
    static const char *const states[] = {"Hover", "Pressed", "Focus", "Disabled"};
    for (unsigned i = 0; i < 4; ++i) {
        os64_ui_button(&gStates[i], states[i], NULL, (void *)(uintptr_t)i);
        gStates[i].cls = &kStateClass;
        gStates[i].focusable = false;
        gStates[i].disabled = i == 3;
        os64_ui_add_child(&gCanvas, &gStates[i]);
    }
    preview_label(&gFieldLabel, "Sample text field");
    os64_ui_textfield(&gField, gFieldText, sizeof(gFieldText), field_submit, NULL, NULL);
    os64_ui_textfield_set(&gPreview, &gField, "A place to make it yours");
    os64_ui_add_child(&gCanvas, &gField.w);
    os64_ui_button(&gAction, "Normal", count_click, NULL);
    os64_ui_button(&gReset, "Reset sample", reset_click, NULL);
    os64_ui_add_child(&gCanvas, &gAction);
    os64_ui_add_child(&gCanvas, &gReset);
    os64_strcopy(gCountText, sizeof(gCountText), "Button presses: 0");
    preview_label(&gCount, gCountText);
    os64_ui_checkbox(&gEnable, "Enable controls", true, enabled_changed, NULL);
    os64_ui_checkbox(&gCheck, "Checked", true, NULL, NULL);
    os64_ui_add_child(&gCanvas, &gEnable.w);
    os64_ui_add_child(&gCanvas, &gCheck.w);
    preview_label(&gLevelLabel, gLevelText);
    os64_ui_slider(&gLevel, 0, 100, 5, 60, level_changed, NULL);
    os64_ui_add_child(&gCanvas, &gLevel.w);
    level_changed(&gLevel, NULL);
    preview_label(&gTextLabel, "Paper, ink & selection");
    os64_ui_textview(&gText, &kText, NULL, view_changed, NULL);

    os64_ui_scrollbar(&gScroll, scroll_changed, NULL);
    os64_ui_add_child(&gCanvas, &gText.w);
    os64_ui_add_child(&gCanvas, &gScroll.w);
    gInput = &gEditor;
    refresh_collection();
    describe_palette();
    font_page_init(&gEditor, &gPreview, &gPages[3], font_status);
    (void)os64_ui_font_planner(&gEditor,plan_workshop,commit_workshop,discard_workshop,NULL);
    (void)os64_ui_font_planner(&gDialog,plan_dialog,finish_dialog,finish_dialog,NULL);
    layout();
    (void)os64_ui_font_follow(&gEditor);
    (void)os64_ui_font_follow(&gDialog);
    dialog_layout(&gDialog,false);
    sync_color();
    os64_ui_textview_goto(&gPreview, &gText, 0, 2, false);
    os64_ui_textview_goto(&gPreview, &gText, 0, 16, true);
}

static void paint(void)
{
    if (!gEditor.any_dirty && !gPreview.any_dirty && !(gConfirmation && gDialog.any_dirty))
        return;
    // Repainting the editor's root overwrites the specimen's canvas area.
    // Render both complete trees before the single publish to avoid a blank
    // specimen frame between their paints.
    os64_ui_mark_dirty(&gEditor, &gRoot);
    os64_ui_render(&gEditor, NULL);
    if (!gCompact) {
        os64_ui_mark_dirty(&gPreview, &gCanvas);
        os64_ui_render(&gPreview, NULL);
    }
    if (gConfirmation) {
        os64_ui_mark_dirty(&gDialog, &gDialogRoot);
        os64_ui_render(&gDialog, NULL);
    }
    os64_draw_publish(&gCtx, &gRoot.bounds);
}

static void dispatch(const os64_gui_event_t *ev)
{
    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN ||
        ev->type == OS64_GUI_EVENT_WINDOW_FOCUS ||
        ev->type == OS64_GUI_EVENT_WINDOW_RESIZE) gPickerEditing = false;
    if (ev->type == OS64_GUI_EVENT_WINDOW_CLOSE) {
        if (draft_dirty()) ask_confirmation(CONFIRM_CLOSE);
        else gEditor.quit = true;
        return;
    }
    if (ev->type == OS64_GUI_EVENT_WINDOW_RESIZE) {
        // Cancel gestures across changed bounds while retaining the typing target.
        // A later release must not activate a control that moved underneath it.
        os64_ui_cancel_gestures(&gEditor);
        os64_ui_cancel_gestures(&gPreview);
        os64_ui_cancel_gestures(&gDialog);
        os64_draw_ctx_refresh(&gCtx);
        layout();
        return;
    }
    if ((ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN ||
         ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) &&
        ev->mouse.button != OS64_GUI_MOUSE_LEFT)
        return;
    if (ev->type == OS64_GUI_EVENT_APPEARANCE) {
        uint64_t before = gEditor.appearance_generation;
        os64_ui_dispatch(&gEditor, ev);
        (void)os64_ui_font_follow(&gDialog);
        dialog_layout(&gDialog,false);
        if (gEditor.appearance_generation > before) {
            gApplyStatus = gEditor.font_settings_result ? "Session updated; Workshop kept its fonts and preview" : "Session updated; preview kept";
            gDialog.theme = gEditor.theme;
            refresh_composition();
        }
        return;
    }
    if (gConfirmation) {
        if (ev->type == OS64_GUI_EVENT_KEY_DOWN && ev->key.ascii == 27 &&
            (ev->key.scancode == 1 || ((ev->key.modifiers & OS64_GUI_MOD_HID) &&
                                      ev->key.scancode == 0x29))) cancel_click(NULL, NULL);
        else os64_ui_dispatch(&gDialog, ev);
        return;
    }
    if (ev->type == OS64_GUI_EVENT_WINDOW_FOCUS ||
        ev->type == OS64_GUI_EVENT_WINDOW_COVERED) {
        os64_ui_dispatch(&gEditor, ev);
        os64_ui_dispatch(&gPreview, ev);
        return;
    }
    if (ev->type == OS64_GUI_EVENT_POINTER_STATE ||
        ev->type == OS64_GUI_EVENT_MOUSE_MOVE) {
        // Both trees reconcile hover; only the tree owning a grab receives
        // motion as a widget gesture. Their roots overlap on the canvas.
        if (gEditor.grab || gPreview.grab) {
            os64_ui_t *owner = gEditor.grab ? &gEditor : &gPreview;
            os64_ui_clear_hover(owner == &gEditor ? &gPreview : &gEditor);
            os64_ui_dispatch(owner, ev);
        } else {
            os64_ui_dispatch(&gEditor, ev);
            os64_ui_dispatch(&gPreview, ev);
        }
        return;
    }
    if ((ev->type == OS64_GUI_EVENT_KEY_DOWN || ev->type == OS64_GUI_EVENT_KEY_UP) &&
        ev->key.ascii == '\t' && !(ev->key.modifiers & OS64_GUI_MOD_ALT)) {
        if (ev->type == OS64_GUI_EVENT_KEY_DOWN) {
            bool reverse = ev->key.modifiers & OS64_GUI_MOD_SHIFT;
            if (!os64_ui_focus_next(gInput, reverse, false)) {
                gInput = gInput == &gEditor ? &gPreview : &gEditor;
                if (!os64_ui_focus_next(gInput, reverse, false)) {
                    gInput = gInput == &gEditor ? &gPreview : &gEditor;
                    os64_ui_focus_next(gInput, reverse, true);
                }
            }
        }
        return;
    }
    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN && !gInput->grab) {
        os64_gui_rect_t r = gCanvas.bounds;
        bool inside = !gCompact && ev->mouse.x >= r.x && ev->mouse.y >= r.y &&
                      ev->mouse.x < r.x + r.w && ev->mouse.y < r.y + r.h;
        gInput = inside ? &gPreview : &gEditor;
        os64_ui_set_focus(inside ? &gEditor : &gPreview, NULL);
    }
    os64_ui_dispatch(gInput, ev);
    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) gPickerEditing = false;
    if (gInput == &gEditor && gEditor.focus == &gNameField.w) refresh_composition();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t win = os64_gui_window_create("Appearance Workshop", 32, 24, 960, 728, 0);
    if (win <= 0) {
        os64_complain("appearance: cannot create window (%ld)\n", (long)win);
        return 1;
    }
    int64_t rc = os64_gui_window_set_min_size(win, WORKSHOP_MIN_WIDTH, WORKSHOP_MIN_HEIGHT);
    if (rc != 0) {
        os64_complain("appearance: cannot set minimum window size (%ld)\n", (long)rc);
        os64_gui_window_destroy(win);
        return 1;
    }
    if (os64_draw_ctx_init(&gCtx, win) != 0) {
        os64_gui_window_destroy(win);
        return 1;
    }
    setup();
    paint();
    while (!gEditor.quit) {
        os64_gui_event_t ev;
        int64_t rc = os64_gui_event_wait(win, &ev);
        if (rc == OS64_INTERRUPTED) continue;
        if (rc != 1) break;
        dispatch(&ev);
        while (!gEditor.quit && os64_gui_event_poll(win, &ev) == 1)
            dispatch(&ev);
        paint();
    }
    font_page_close();
    os64_ui_font_release(&gPreview);
    os64_ui_font_release(&gEditor);
    os64_ui_font_release(&gDialog);
    os64_gui_window_destroy(win);
    return 0;
}
