#include "font_page.h"
#include "os64/os64.h"
#include "os64/font_settings.h"
#include "os64/conf.h"

static os64_ui_t *editor, *preview;
static void (*report)(const char *);
static os64_font_config_t draft, baseline, previous;
static bool loaded, have_undo;
static unsigned role;
static os64_font_catalog_t *catalog;
static os64_ui_widget_t heading, roles[3], selected, size_label, refresh_button;
static os64_ui_widget_t install_label, install_button, specimen;
static os64_ui_listbox_t fonts;
static os64_ui_scrollbar_t scroll;
static os64_ui_slider_t size_slider;
static os64_ui_textfield_t install_field;
static char install_path[256], size_text[32], selected_text[256], message[256];
static char labels[OS64_FONT_DISCOVERY_MAX][256];
static void *sample_runs[3], *sample_staged[3];
static const char *const samples[] = {
    "The quick brown fox - 0123456789", "+--- Terminal 0123456789 ---+",
    "Caf\xc3\xa9, na\xc3\xafve, r\xc3\xa9sum\xc3\xa9 - Aa Bb Gg"
};

static bool same_choices(const os64_font_config_t *a, const os64_font_config_t *b)
{
    for (size_t r = 0; r < 3; ++r) {
        if (a->roles[r].size != b->roles[r].size) return false;
        for (size_t s = 0; s < 3; ++s)
            if (!os64_streq(a->roles[r].face[s], b->roles[r].face[s])) return false;
    }
    return true;
}

bool font_page_can_undo(void) { return have_undo; }

bool font_page_dirty(void) { return loaded && !same_choices(&draft, &baseline); }

static void failure(const char *action, const os64_font_config_error_t *error)
{
    const char *reason = os64_font_config_status_name(error->status);
    if (error->status == OS64_FONT_CONFIG_FACE) {
        if (error->font_status == OS64_FONT_NO_MEMORY) reason = "not enough memory";
        else if (error->font_status == OS64_FONT_LIMIT) reason = "font exceeds resource limits";
        else if (error->font_status == OS64_FONT_UNSUPPORTED && error->role == OS64_FONT_ROLE_TERMINAL)
            reason = "Terminal needs fixed-width text at this size";
        else if (error->font_status == OS64_FONT_UNSUPPORTED) reason = "unsupported font format";
        else reason = "invalid font file";
    }
    os64_snprintf(message, sizeof(message), "%s: %s", action, reason);
    report(message);
}

static bool show_candidate(const os64_font_config_t *candidate)
{
    os64_font_set_t *set = NULL;
    os64_font_config_error_t error;
    if (os64_font_config_prepare(os64_ui_font_context(preview), candidate, &set, &error)) {
        failure("Preview kept", &error); return false;
    }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(preview, &consumer);
    os64_font_status_t status = os64_font_adopt(set, &consumer, 1, NULL);
    os64_font_set_release(set);
    if (status) { report("Preview cannot fit this font; previous choice kept"); return false; }
    return true;
}

static const char *font_label(size_t i, void *user)
{ (void)user; return catalog && i < catalog->count ? labels[i] : ""; }

static void controls(void)
{
    int chosen = -1;
    for (size_t i = 0; catalog && i < catalog->count; ++i)
        if (os64_streq(catalog->entries[i].path, draft.roles[role].face[0])) chosen = (int)i;
    os64_ui_listbox_set(editor, &fonts, catalog ? catalog->count : 0, chosen);
    os64_ui_slider_set(editor, &size_slider, (int32_t)draft.roles[role].size);
    os64_ui_set_enabled(editor, &size_slider.w, !os64_streq(draft.roles[role].face[0], "builtin"));
    os64_snprintf(size_text, sizeof(size_text), "%u pixels", draft.roles[role].size);
    os64_strcopy(selected_text, sizeof(selected_text), draft.roles[role].face[0]);
    static const char *const names[] = {"Interface", "Terminal", "Document"};
    static const char *const active[] = {"Interface *", "Terminal *", "Document *"};
    for (size_t r = 0; r < 3; ++r) roles[r].text = role == r ? active[r] : names[r];
    os64_ui_scrollbar_set(editor, &scroll, catalog ? (int64_t)catalog->count : 0,
                          os64_ui_listbox_rows(&fonts, &editor->theme), (int64_t)fonts.top);
    os64_ui_mark_dirty(editor, editor->root);
    os64_ui_mark_dirty(preview, preview->root);
}

static void refresh(void)
{
    os64_font_catalog_t *next = NULL;
    os64_font_config_status_t status = os64_font_config_discover(os64_ui_font_context(preview), &draft, &next);
    if (status) { report("Cannot refresh fonts; previous list kept"); return; }
    os64_font_catalog_release(catalog); catalog = next;
    for (size_t i = 0; i < catalog->count; ++i) {
        const os64_font_catalog_entry_t *entry = &catalog->entries[i];
        const char *base = entry->path;
        for (const char *p = base; *p; ++p) if (*p == '/') base = p + 1;
        if (entry->status) os64_snprintf(labels[i], sizeof(labels[i]), "%s [%s]", base,
                                        os64_font_config_status_name(entry->status));
        else os64_snprintf(labels[i], sizeof(labels[i]), "%s %s - %s",
                            entry->info.family, entry->info.style, base);
    }
    controls();
    if (catalog->limited) report("Font list reached its limit; selected files are included");
    else if (catalog->directory_unavailable) report("Font folder unavailable; selected files are included");
    else report("Font list refreshed; preview kept");
}

void font_page_activate(void)
{
    if (!loaded) {
        uint64_t generation;
        if (os64_font_settings_current(&draft, &generation)) os64_font_config_defaults(&draft);
        baseline = draft; previous = draft; loaded = true;
        (void)show_candidate(&draft);
        refresh();
    }
    controls();
}

static void select_role(os64_ui_widget_t *w, void *user)
{ (void)w; role = (unsigned)(uintptr_t)user; controls(); }
static void select_font(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    if (!catalog || list->selected < 0 || (size_t)list->selected >= catalog->count) return;
    os64_font_catalog_entry_t *entry = &catalog->entries[list->selected];
    if (entry->status) { report("This file is not a usable font"); controls(); return; }
    os64_font_config_t candidate = draft;
    os64_strcopy(candidate.roles[role].face[0], OS64_FONT_PATH_CAP, entry->path);
    if (os64_streq(entry->path,"builtin")) candidate.roles[role].size = 16;
    if (show_candidate(&candidate)) {
        previous = draft; have_undo = true; draft = candidate;
        report("Font changed in preview");
    }
    controls();
}
static void change_size(os64_ui_slider_t *slider, void *user)
{
    (void)user;
    os64_font_config_t candidate = draft;
    candidate.roles[role].size = (uint32_t)slider->value;
    if (show_candidate(&candidate)) {
        previous = draft; have_undo = true; draft = candidate;
        report("Size changed in preview");
    }
    controls();
}
static void refresh_click(os64_ui_widget_t *w, void *user) { (void)w; (void)user; refresh(); }
static void scrolled(os64_ui_scrollbar_t *bar, void *user)
{ (void)user; os64_ui_listbox_scroll_to(editor, &fonts, (size_t)bar->pos); }

/* Installation validates the staged bytes and the complete role set before
 * a no-replace publish. The provider decides format from contents. */
static void use_file(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (install_path[0] != '/') { report("Enter the font file's full path"); return; }
    os64_font_config_t candidate = draft;
    os64_strcopy(candidate.roles[role].face[0], OS64_FONT_PATH_CAP, install_path);
    char installed[OS64_FONT_PATH_CAP];
    os64_font_config_error_t error = {0};
    os64_font_config_status_t status = os64_font_config_install(os64_ui_font_context(preview),
        &candidate, (os64_font_role_t)role, installed, &error);
    if (status) {
        error.status = status; failure("Install refused", &error); return;
    }
    os64_strcopy(candidate.roles[role].face[0], OS64_FONT_PATH_CAP, installed);
    if (show_candidate(&candidate)) {
        previous = draft; have_undo = true; draft = candidate;
        refresh(); report("Font installed; Save fonts keeps this choice");
    }
    controls();
}
static void file_submit(os64_ui_textfield_t *field, void *user)
{ (void)field; use_file(NULL, user); }

static void specimen_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *theme)
{
    os64_draw_fill_rect(&ctx->surf, w->bounds, theme->text_bg);
    int32_t band = w->bounds.h / 3;
    for (size_t r = 0; r < 3; ++r) {
        os64_gui_rect_t clip = {w->bounds.x + 12, w->bounds.y + (int32_t)r * band + 8,
                               w->bounds.w - 24, band - 16};
        os64_ui_draw_text(preview, &sample_runs[r], (os64_font_role_t)r, &ctx->surf, clip,
                          clip.x, clip.y, samples[r], os64_strlen(samples[r]), theme->text_fg, theme->text_bg);
    }
}
static os64_font_status_t specimen_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
    (void)w;
    for (size_t r = 0; r < 3; ++r) {
        os64_font_status_t status = os64_ui_run_layout(ui, (os64_font_role_t)r,
            samples[r], os64_strlen(samples[r]), &sample_staged[r]);
        if (status) return status;
    }
    return OS64_FONT_OK;
}
static void specimen_commit(os64_ui_widget_t *w)
{
    (void)w;
    for (size_t r = 0; r < 3; ++r) {
        os64_ui_run_release(sample_runs[r]); sample_runs[r] = sample_staged[r];
        sample_staged[r] = NULL;
    }
}
static void specimen_discard(os64_ui_widget_t *w)
{
    (void)w;
    for (size_t r = 0; r < 3; ++r) {
        os64_ui_run_release(sample_staged[r]); sample_staged[r] = NULL;
    }
}
static void specimen_destroy(os64_ui_widget_t *w)
{
    specimen_discard(w);
    for (size_t r = 0; r < 3; ++r) {
        os64_ui_run_release(sample_runs[r]); sample_runs[r] = NULL;
    }
}
static const os64_ui_class_t specimen_class = {
    .name = "font specimen", .paint = specimen_paint, .prepare = specimen_prepare,
    .commit = specimen_commit, .discard = specimen_discard, .destroy = specimen_destroy
};

static void add(os64_ui_widget_t *page, os64_ui_widget_t *widget)
{ os64_ui_add_child(page, widget); }
void font_page_init(os64_ui_t *ui, os64_ui_t *sample, os64_ui_widget_t *page,
                     void (*status)(const char *))
{
    editor = ui; preview = sample; report = status;
    os64_ui_label(&heading, "FONTS FOR YOUR WORK"); add(page, &heading);
    for (size_t r = 0; r < 3; ++r) {
        os64_ui_button(&roles[r], "", select_role, (void *)(uintptr_t)r); add(page, &roles[r]);
    }
    os64_ui_listbox(&fonts, 0, font_label, select_font, NULL); add(page, &fonts.w);
    os64_ui_scrollbar(&scroll, scrolled, NULL); add(page, &scroll.w);
    os64_ui_label(&selected, selected_text); add(page, &selected);
    os64_ui_label(&size_label, size_text); add(page, &size_label);
    os64_ui_slider(&size_slider, 8, 96, 1, 16, change_size, NULL); add(page, &size_slider.w);
    os64_ui_label(&install_label, "Install a font file (full path)"); add(page, &install_label);
    os64_ui_textfield(&install_field, install_path, sizeof(install_path), file_submit, NULL, NULL);
    add(page, &install_field.w);
    os64_ui_button(&install_button, "Install font", use_file, NULL); add(page, &install_button);
    os64_ui_button(&refresh_button, "Refresh fonts", refresh_click, NULL); add(page, &refresh_button);
    specimen = (os64_ui_widget_t){.cls = &specimen_class, .hidden = true};
    add(preview->root, &specimen);
}
static void place(os64_ui_widget_t *w, int x, int y, int width, int height)
{ w->bounds = (os64_gui_rect_t){x,y,width,height}; }
void font_page_layout(bool visible)
{
    place(&heading,40,138,412,22);
    for (size_t r = 0; r < 3; ++r) place(&roles[r],40+(int)r*140,172,132,32);
    place(&fonts.w,40,218,390,140); place(&scroll.w,436,218,16,140);
    place(&selected,40,369,412,22); place(&size_label,40,405,112,24);
    place(&size_slider.w,160,405,292,24); place(&install_label,40,446,412,20);
    place(&install_field.w,40,474,412,30); place(&install_button,40,518,194,32);
    place(&refresh_button,246,518,206,32);
    if (visible) {
        for (os64_ui_widget_t *w = preview->root->first_child; w; w = w->next_sibling)
            w->hidden = w != &specimen;
    } else {
        for (os64_ui_widget_t *w = preview->root->first_child; w; w = w->next_sibling)
            w->hidden = w == &specimen;
    }
    specimen.bounds = preview->root->bounds;
    if (loaded) controls();
}
void font_page_apply(void)
{
    if (!loaded) font_page_activate();
    uint64_t generation;
    os64_font_config_error_t error = {0};
    int result = os64_font_settings_apply(os64_ui_font_context(preview), &draft, &generation, &error);
    if (!result) report("Fonts published to session; apps may retain a refused change");
    else if (result == OS64_UI_APPLY_CONFLICT) report("Changed elsewhere; Apply again to retry");
    else if (error.status) failure("Apply kept current fonts", &error);
    else report("Cannot publish fonts; previous session kept");
}
void font_page_save(void)
{
    if (!loaded) font_page_activate();
    os64_font_config_error_t error = {0};
    if (os64_font_settings_save(&draft, &error)) {
        if (error.status) failure("Save refused", &error);
        else report("Save failed; previous startup fonts kept");
        return;
    }
    baseline = draft;
    (void)os64_conf_target("fonts.conf", draft.path, sizeof(draft.path));
    report("Fonts saved for startup; current session unchanged");
}
void font_page_reset(void)
{
    if (loaded && show_candidate(&baseline)) { previous = draft; have_undo = true; draft = baseline; controls(); }
}
void font_page_undo(void)
{
    if (have_undo && show_candidate(&previous)) { draft = previous; have_undo = false; controls(); }
}
void font_page_close(void)
{
    specimen_destroy(&specimen);
    os64_font_catalog_release(catalog); catalog = NULL;
}
