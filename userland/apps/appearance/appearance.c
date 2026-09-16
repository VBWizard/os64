// Appearance Workshop: compose palettes and button treatments in a local
// preview, save named snapshots, and explicitly choose session/startup use.
// Design and persistence contracts: APPEARANCE.md.
#include "os64/os64.h"
#include "os64/ui.h"

static os64_draw_ctx_t gCtx;
static os64_ui_t gEditor, gPreview;
static os64_ui_t *gInput;
static os64_ui_widget_t gRoot, gHeading, gIntro, gPaletteLabel, gStyleLabel;
static os64_ui_widget_t gStyles[2], gFooter, gSmall, gApply;
static os64_ui_widget_t gCanvas, gPreviewTitle, gFieldLabel, gAction, gReset;
static os64_ui_widget_t gCount, gTextLabel, gSelectionHint;
static os64_ui_widget_t gLevelLabel, gCollectionLabel, gNameLabel, gStartupLabel;
static os64_ui_widget_t gLoad, gRefresh, gSave, gStartup;
static os64_ui_listbox_t gThemes, gPalettes;
static os64_ui_scrollbar_t gThemeScroll;
static os64_ui_textfield_t gNameField;
static char gName[OS64_UI_THEME_NAME_MAX + 1], gLoadedName[OS64_UI_THEME_NAME_MAX + 1];
static os64_ui_theme_t gBaseline, gStartupTheme;
static os64_ui_theme_entry_t gSaved[128];
static size_t gSavedCount;
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

static bool draft_dirty(void)
{
    return !same_theme(&gPreview.theme, &gBaseline) || !os64_streq(gName, gLoadedName);
}

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
}

static void refresh_composition(void)
{
    os64_snprintf(gComposition, sizeof(gComposition), "%s / %s | %s | %s",
                  kNames[gPalette], gStyle ? "raised" : "flat",
                  draft_dirty() ? "Unsaved" : "Unchanged", gApplyStatus);
    gStyles[0].text = gStyle ? "Flat" : "Flat *";
    gStyles[1].text = gStyle ? "Raised *" : "Raised";
    gStartupLabel.text = same_theme(&gPreview.theme, &gStartupTheme) ?
        "Startup: this composition" : "Startup: another appearance";
    os64_ui_mark_dirty(&gEditor, &gRoot);
    os64_ui_mark_dirty(&gPreview, &gCanvas);
}

static void palette_click(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    gApplyStatus = "Preview changed";
    gChanged |= OS64_UI_COMPONENT_PALETTE;
    gPalette = (unsigned)list->selected;
    os64_ui_theme_palette(&gPreview.theme, (os64_ui_palette_t)gPalette);
    refresh_composition();
}

static void style_click(os64_ui_widget_t *w, void *user)
{
    (void)w;
    gApplyStatus = "Preview changed";
    gChanged |= OS64_UI_COMPONENT_TREATMENT;
    gStyle = (unsigned)(uintptr_t)user;
    gPreview.theme.button_bevel = gStyle ? 3 : 0;
    refresh_composition();
}

static void apply_click(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
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
    if (index < 3) {
        os64_ui_theme_palette(&theme, (os64_ui_palette_t)index);
        name = kNames[index];
    } else {
        name = gSaved[index - 3].name;
        int result = os64_ui_theme_load(name, &theme);
        if (result) {
            gApplyStatus = result == OS64_UI_THEME_INVALID ?
                "Invalid saved theme; preview kept" : "Cannot load theme; preview kept";
            refresh_composition();
            return;
        }
    }
    gPreview.theme = theme;
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
    if (draft_dirty()) ask_confirmation(CONFIRM_LOAD);
    else load_selected();
}

static void save_draft(bool replace)
{
    int result = os64_ui_theme_save(gName, &gPreview.theme, replace);
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
    save_draft(false);
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
    gCompact = width < 880 || height < 696;
    for (os64_ui_widget_t *w = gRoot.first_child; w; w = w->next_sibling)
        w->hidden = gCompact;
    gSmall.hidden = !gCompact;
    place(&gSmall, 0, 0, width, height);
    gCanvas.hidden = gCompact;
    if (gCompact) {
        os64_ui_cancel_interaction(&gEditor);
        os64_ui_cancel_interaction(&gPreview);
        gPreview.any_dirty = false;
        os64_ui_mark_dirty(&gEditor, &gRoot);
        return;
    }
    place(&gHeading, 24, 20, width - 48, 24);
    place(&gIntro, 24, 49, width - 48, 20);
    place(&gCollectionLabel, 24, 86, 280, 20);
    place(&gThemes.w, 24, 112, 268, 124);
    place(&gThemeScroll.w, 294, 112, 14, 124);
    place(&gLoad, 24, 244, 174, 30);
    place(&gRefresh, 206, 244, 102, 30);
    place(&gPaletteLabel, 24, 292, 284, 20);
    place(&gPalettes.w, 24, 318, 284, 76);
    place(&gStyleLabel, 24, 408, 284, 20);
    for (int i = 0; i < 2; ++i)
        place(&gStyles[i], 24 + i * 146, 434, 138, 30);
    place(&gNameLabel, 24, 482, 284, 20);
    place(&gNameField.w, 24, 508, 284, 30);
    place(&gSave, 24, 548, 108, 34);
    place(&gApply, 140, 548, 168, 34);
    place(&gStartup, 24, 596, 284, 30);
    place(&gStartupLabel, 24, 634, 284, 20);
    place(&gFooter, 24, height - 30, width - 48, 20);
    place(&gDialogRoot, (width - 500) / 2, (height - 180) / 2, 500, 180);
    int dx = gDialogRoot.bounds.x, dy = gDialogRoot.bounds.y;
    place(&gDialogTitle, dx + 20, dy + 20, 460, 24);
    place(&gDialogBody, dx + 20, dy + 58, 460, 24);
    place(&gConfirm, dx + 20, dy + 116, 216, 36);
    place(&gCancel, dx + 256, dy + 116, 224, 36);
    theme_selection(&gThemes, NULL);

    int x = 332, y = 86, pw = width - x - 24;
    place(&gCanvas, x, y, pw, height - y - 48);
    place(&gPreviewTitle, x + 18, y + 16, pw - 36, 20);
    place(&gFieldLabel, x + 18, y + 52, pw - 36, 20);
    place(&gField.w, x + 18, y + 78, pw - 36, 32);
    place(&gAction, x + 18, y + 127, 144, 34);
    place(&gReset, x + 174, y + 127, 144, 34);
    place(&gCount, x + 18, y + 172, pw - 36, 20);
    place(&gEnable.w, x + 18, y + 209, 230, 28);
    place(&gCheck.w, x + 252, y + 209, pw - 270, 28);
    place(&gLevelLabel, x + 18, y + 255, 168, 28);
    place(&gLevel.w, x + 192, y + 255, pw - 210, 28);
    place(&gTextLabel, x + 18, y + 294, pw - 36, 20);
    int text_y = y + 322, text_h = height - 114 - text_y;
    place(&gText.w, x + 18, text_y, pw - 36 - 18, text_h);
    place(&gScroll.w, x + pw - 32, text_y, 14, text_h);
    place(&gSelectionHint, x + 18, height - 89, pw - 36, 20);
    view_changed(&gText, NULL);
    refresh_composition();
}

static void editor_label(os64_ui_widget_t *w, const char *text)
{
    os64_ui_label(w, text);
    os64_ui_add_child(&gRoot, w);
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
    editor_label(&gHeading, "APPEARANCE WORKSHOP");
    editor_label(&gIntro, "Choose the pieces. Make something yours.");
    editor_label(&gCollectionLabel, "01  THEME COLLECTION");
    editor_label(&gPaletteLabel, "02  PALETTE");
    editor_label(&gStyleLabel, "03  BUTTON TREATMENT");
    os64_ui_button(&gApply, "Apply to session", apply_click, NULL);
    os64_ui_add_child(&gRoot, &gApply);
    editor_label(&gFooter, gComposition);
    editor_label(&gSmall, "Enlarge the window to explore the gallery.");
    os64_ui_listbox(&gThemes, 3, theme_label, theme_selection, NULL);
    os64_ui_listbox(&gPalettes, 3, palette_label, palette_click, NULL);
    os64_ui_scrollbar(&gThemeScroll, themes_scrolled, NULL);
    os64_ui_add_child(&gRoot, &gThemes.w);
    os64_ui_add_child(&gRoot, &gThemeScroll.w);
    os64_ui_add_child(&gRoot, &gPalettes.w);
    os64_ui_button(&gLoad, "Load theme", load_click, NULL);
    os64_ui_button(&gRefresh, "Refresh", refresh_click, NULL);
    os64_ui_button(&gSave, "Save", save_click, NULL);
    os64_ui_button(&gStartup, "Use at startup", startup_click, NULL);
    os64_ui_add_child(&gRoot, &gLoad);
    os64_ui_add_child(&gRoot, &gRefresh);
    editor_label(&gNameLabel, "COMPOSITION NAME");
    os64_ui_textfield(&gNameField, gName, sizeof(gName), NULL, NULL, NULL);
    os64_ui_add_child(&gRoot, &gNameField.w);
    os64_ui_textfield_set(&gEditor, &gNameField, kNames[0]);
    os64_strcopy(gLoadedName, sizeof(gLoadedName), kNames[0]);
    gBaseline = gPreview.theme;
    os64_ui_add_child(&gRoot, &gSave);
    os64_ui_add_child(&gRoot, &gStartup);
    editor_label(&gStartupLabel, "");
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
        os64_ui_add_child(&gRoot, &gStyles[i]);
    }
    preview_label(&gPreviewTitle, "LIVE WIDGET GALLERY");
    preview_label(&gFieldLabel, "Sample text field");
    os64_ui_textfield(&gField, gFieldText, sizeof(gFieldText), field_submit, NULL, NULL);
    os64_ui_textfield_set(&gPreview, &gField, "A place to make it yours");
    os64_ui_add_child(&gCanvas, &gField.w);
    os64_ui_button(&gAction, "Try a button", count_click, NULL);
    os64_ui_button(&gReset, "Reset sample", reset_click, NULL);
    os64_ui_add_child(&gCanvas, &gAction);
    os64_ui_add_child(&gCanvas, &gReset);
    os64_strcopy(gCountText, sizeof(gCountText), "Button presses: 0");
    preview_label(&gCount, gCountText);
    os64_ui_checkbox(&gEnable, "Enable sample controls", true, enabled_changed, NULL);
    os64_ui_checkbox(&gCheck, "Sample choice", true, NULL, NULL);
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
    preview_label(&gSelectionHint, "Preview your choices, then Apply to session.");
    gInput = &gEditor;
    refresh_collection();
    describe_palette();
    layout();
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
        if (gEditor.appearance_generation > before) {
            gApplyStatus = "Session changed; preview kept";
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
    os64_gui_window_destroy(win);
    return 0;
}
