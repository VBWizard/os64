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

static void place(os64_ui_widget_t *w, int x, int y, int width, int height)
{
    w->bounds = (os64_gui_rect_t){x, y, width, height};
}

static void layout(void)
{
    int width = (int)gCtx.surf.width, height = (int)gCtx.surf.height;
    place(&gRoot, 0, 0, width, height);
    gCompact = width < WORKSHOP_MIN_WIDTH || height < WORKSHOP_MIN_HEIGHT;
    for (os64_ui_widget_t *w = gRoot.first_child; w; w = w->next_sibling)
        w->hidden = gCompact;
    gSmall.hidden = !gCompact;
    place(&gSmall, 0, 0, width, height);
    gCanvas.hidden = gCompact;
    // Confirmation remains available even if the window is shrunk mid-prompt.
    int dw = width < 520 ? width : 520, dh = height < 180 ? height : 180;
    place(&gDialogRoot, (width - dw) / 2, (height - dh) / 2, dw, dh);
    int dx = gDialogRoot.bounds.x, dy = gDialogRoot.bounds.y;
    place(&gDialogTitle, dx + 20, dy + 20, dw - 40, 24);
    place(&gDialogBody, dx + 20, dy + 58, dw - 40, 24);
    place(&gConfirm, dx + 20, dy + 116, (dw - 60) / 2, 36);
    place(&gCancel, dx + (dw + 20) / 2, dy + 116, (dw - 60) / 2, 36);
    if (gCompact) {
        os64_ui_cancel_interaction(&gEditor);
        os64_ui_cancel_interaction(&gPreview);
        gPreview.any_dirty = false;
        os64_ui_mark_dirty(&gEditor, &gRoot);
        return;
    }
    int left = 444, footer = height - 106;
    place(&gHeading, 24, 16, width - 48, 24);
    place(&gIntro, 24, 44, width - 48, 20);
    static const char *const tabs[] = {"Themes", "Palette", "Controls", "Fonts"};
    static const char *const active[] = {"Themes *", "Palette *", "Controls *", "Fonts *"};
    for (unsigned i = 0; i < 4; ++i) {
        place(&gTabs[i], 24 + (int)i * 112, 78, 106, 32);
        gTabs[i].text = gPage == i ? active[i] : tabs[i];
        place(&gPages[i], 24, 122, left, footer - 138);
        os64_ui_set_hidden(&gEditor, &gPages[i], i != gPage);
    }
    place(&gCollectionLabel, 40, 138, left - 32, 20);
    place(&gThemes.w, 40, 168, left - 54, 244);
    place(&gThemeScroll.w, 436, 168, 16, 244);
    place(&gLoad, 40, 426, 192, 32);
    place(&gRefresh, 244, 426, 208, 32);
    place(&gStartup, 40, 476, 412, 32);
    place(&gStartupLabel, 40, 520, 412, 20);

    place(&gPaletteLabel, 40, 136, 400, 20);
    place(&gPalettes.w, 40, 164, 412, 76);
    place(&gIndividual.w, 40, 250, 412, 26);
    place(&gColors.w, 40, 290, 198, 244);
    place(&gColorScroll.w, 240, 290, 14, 244);
    place(&gColorTitle, 266, 282, 186, 28);
    place(&gPicker.w, 266, 314, 186, 156);
    place(&gHexLabel, 266, 476, 16, 30);
    place(&gHexField.w, 286, 476, 92, 30);
    place(&gSetHex, 386, 476, 66, 30);
    place(&gFollow, 266, 514, 186, 28);
    place(&gColorHint, 40, 546, 412, 20);
    // Layout changes the scroll range, not the user's unsubmitted hex edit.
    os64_ui_scrollbar_set(&gEditor, &gColorScroll, (int64_t)gColors.count,
        os64_ui_listbox_rows(&gColors, &gEditor.theme), (int64_t)gColors.top);

    place(&gStyleLabel, 40, 142, 412, 20);
    for (unsigned i = 0; i < 2; ++i) {
        place(&gStyles[i], 40 + (int)i * 212, 176, 200, 36);
        place(&gCorners[i], 40 + (int)i * 212, 268, 200, 36);
    }
    place(&gCornerLabel, 40, 234, 412, 20);
    place(&gTreatmentHint, 40, 328, 412, 20);

    place(&gNameLabel, 24, footer, 144, 30);
    place(&gNameField.w, 176, footer, 344, 30);
    place(&gSave, width - 280, footer, 80, 30);
    place(&gApply, width - 192, footer, 168, 30);
    place(&gUndo, 24, footer + 40, 96, 30);
    place(&gResetComponent, 132, footer + 40, 196, 30);
    os64_ui_set_hidden(&gEditor, &gResetComponent, gPage == 0);
    place(&gFooter, 24, height - 26, width - 48, 20);
    theme_selection(&gThemes, NULL);

    int x = 488, pw = width - x - 24;
    place(&gCanvas, x, 122, pw, footer - 138);
    place(&gPreviewTitle, x + 16, 138, pw - 32, 20);
    place(&gMenuSample, x + 16, 166, pw - 32, 64);
    place(&gFieldLabel, x + 16, 236, pw - 32, 20);
    place(&gField.w, x + 16, 260, pw - 32, 30);
    int bw = (pw - 48) / 5;
    place(&gAction, x + 16, 306, bw, 32);
    for (int i = 0; i < 4; ++i)
        place(&gStates[i], x + 16 + (i + 1) * (bw + 4), 306, bw, 32);
    place(&gEnable.w, x + 16, 350, 204, 28);
    place(&gCheck.w, x + 224, 350, pw - 240, 28);
    place(&gLevelLabel, x + 16, 390, 144, 28);
    place(&gLevel.w, x + 164, 390, pw - 180, 28);
    place(&gTextLabel, x + 16, 430, pw - 32, 20);
    place(&gText.w, x + 16, 456, pw - 50, footer - 500);
    place(&gScroll.w, x + pw - 30, 456, 14, footer - 500);
    place(&gCount, x + 16, footer - 38, pw - 160, 20);
    place(&gReset, x + pw - 132, footer - 40, 116, 24);
    view_changed(&gText, NULL);
    refresh_composition();
    font_page_layout(gPage == 3);
    os64_ui_set_hidden(&gEditor, &gNameField.w, gPage == 3);
    os64_ui_set_hidden(&gEditor, &gNameLabel, gPage == 3);
}

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
    layout();
    (void)os64_ui_font_follow(&gEditor);
    (void)os64_ui_font_follow(&gDialog);
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
        if (gEditor.appearance_generation > before) {
            gApplyStatus = gEditor.font_settings_result ? "Font does not fit editor; current font and preview kept" : "Session changed; preview kept";
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
