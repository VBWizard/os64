#include "os64/ui_file_dialog.h"
#include "os64/ui_action.h"
#include "os64/io.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/conf.h"

static void file_report(os64_ui_file_dialog_t *d, const char *text)
{
    if (!!*d->message_text != !!*text) d->footer_dirty = true;
    os64_strcopy(d->message_text, sizeof(d->message_text), text);
    os64_ui_mark_dirty(&d->ui, &d->message);
}

static void file_place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{
    if (staged) os64_ui_widget_stage_bounds(w, r);
    else w->bounds = r;
}

static os64_font_status_t file_layout(os64_ui_file_dialog_t *d, bool staged)
{
    int32_t pad = d->ui.theme.pad + 4, gap = d->ui.theme.gap;
    int32_t row = os64_ui_control_min_height(&d->ui);
    int32_t width = d->root.bounds.w - 2 * pad;
    int32_t button = 0, label = 0, hidden = 0;
    const char *captions[] = {"Save", "Open", "Cancel", "New Folder", "Replace", "Keep File", "Create"};
    for (size_t i = 0; i < sizeof(captions)/sizeof(*captions); ++i) {
        int32_t n;
        os64_font_status_t rc = os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI,
            captions[i], os64_strlen(captions[i]), &n);
        if (rc) return rc;
        if (n > button) button = n;
    }
    if (os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, "File name", 9, &label) ||
        os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, "Show hidden files", 17, &hidden))
        return OS64_FONT_NO_MEMORY;
    button += 2 * pad; label += pad;
    hidden += d->ui.theme.checkbox_size + gap;
    int32_t minimum = hidden + button + gap;
    if (2 * button + gap > minimum) minimum = 2 * button + gap;
    if (label + 4 * row > minimum) minimum = label + 4 * row;
    bool wrap = width < hidden + 3 * button + 3 * gap;
    bool message = *d->message_text != 0;
    int32_t rows = 2 + wrap + message;
    int32_t footer = rows * row + (rows - 1) * gap + 2 * pad;
    /* Reserve the minimum for wrapped controls and a message. The actual
     * footer only uses those rows when needed, leaving the rest to browsing. */
    int32_t min_height = 7 * row + 3 * gap + 2 * pad;
    if (staged) {
        d->staged_min_width = minimum + 2 * pad;
        d->staged_min_height = min_height;
    } else {
        d->min_width = minimum + 2 * pad;
        d->min_height = min_height;
    }
    if (width < minimum || d->root.bounds.h < min_height || row <= 0)
        return OS64_FONT_LIMIT;
    int32_t y = d->root.bounds.h - footer;
    os64_gui_rect_t area = {0, 0, d->root.bounds.w, y};
    os64_gui_rect_t old = d->browser_area.bounds;
    file_place(&d->browser_area, area, staged);
    os64_font_status_t rc = os64_ui_browser_layout(&d->browser_ui, staged);
    if (rc) {
        if (!staged) d->browser_area.bounds = old;
        return rc;
    }
    file_place(&d->form, (os64_gui_rect_t){pad,y,width,footer}, staged);
    file_place(&d->name_label, (os64_gui_rect_t){pad,y,label,row}, staged);
    file_place(&d->name.w, (os64_gui_rect_t){pad+label,y,width-label,row}, staged);
    file_place(&d->folder_label, (os64_gui_rect_t){pad,y,label,row}, staged);
    file_place(&d->folder.w, (os64_gui_rect_t){pad+label,y,width-label,row}, staged);
    y += row + gap;
    file_place(&d->message, (os64_gui_rect_t){pad,y,width,message ? row : 0}, staged);
    if (message) y += row + gap;
    file_place(&d->hidden.w, (os64_gui_rect_t){pad,y,hidden,row}, staged);
    file_place(&d->new_folder, (os64_gui_rect_t){pad+hidden+gap,y,button,row}, staged);
    if (wrap) y += row + gap;
    os64_gui_rect_t secondary = {pad+width-2*button-gap,y,button,row};
    os64_gui_rect_t primary = {pad+width-button,y,button,row};
    file_place(&d->accept, primary, staged); file_place(&d->cancel, secondary, staged);
    file_place(&d->replace, primary, staged); file_place(&d->keep, secondary, staged);
    file_place(&d->folder_create, primary, staged); file_place(&d->folder_cancel, secondary, staged);
    if (!staged) {
        os64_ui_set_hidden(&d->ui, &d->message, !message);
        d->footer_dirty = false;
        os64_ui_mark_dirty(&d->ui, &d->root);
    }
    return OS64_FONT_OK;
}

static void file_resize(os64_ui_t *ui)
{
    os64_ui_file_dialog_t *d = ui->root->user;
    d->layout_pending = file_layout(d, false) != OS64_FONT_OK;
}
static os64_font_status_t file_plan(os64_ui_t *ui, void *user, void **out)
{ (void)ui; *out = NULL; return file_layout(user, true); }
static void file_committed(os64_ui_t *ui, void *user, void *p)
{
    (void)ui; (void)p;
    os64_ui_file_dialog_t *d = user;
    d->min_width = d->staged_min_width; d->min_height = d->staged_min_height;
}
static void file_discarded(os64_ui_t *ui, void *user, void *p)
{ (void)ui; (void)user; (void)p; }

/* The prompt disables the underlying controls and moves focus to the safe
 * response. A second Enter from activating a file cannot authorize Replace. */
static void file_controls(os64_ui_file_dialog_t *d)
{
    bool ordinary = !d->confirm && !d->making_folder;
    os64_ui_cancel_interaction(&d->ui);
    os64_ui_set_enabled(&d->ui, &d->browser_area, ordinary);
    os64_ui_set_enabled(&d->ui, &d->name.w, ordinary);
    os64_ui_set_enabled(&d->ui, &d->hidden.w, ordinary);
    os64_ui_set_enabled(&d->ui, &d->new_folder, ordinary);
    os64_ui_set_hidden(&d->ui, &d->name.w, d->making_folder);
    os64_ui_set_hidden(&d->ui, &d->name_label, d->making_folder);
    os64_ui_set_hidden(&d->ui, &d->accept, !ordinary);
    os64_ui_set_hidden(&d->ui, &d->cancel, !ordinary);
    os64_ui_set_hidden(&d->ui, &d->replace, !d->confirm);
    os64_ui_set_hidden(&d->ui, &d->keep, !d->confirm);
    os64_ui_set_hidden(&d->ui, &d->folder.w, !d->making_folder);
    os64_ui_set_hidden(&d->ui, &d->folder_label, !d->making_folder);
    os64_ui_set_hidden(&d->ui, &d->folder_create, !d->making_folder);
    os64_ui_set_hidden(&d->ui, &d->folder_cancel, !d->making_folder);
    os64_ui_set_focus(&d->ui, d->confirm ? &d->keep :
        d->making_folder ? &d->folder.w : &d->name.w);
}

static void file_cancel_prompt(os64_ui_file_dialog_t *d)
{
    d->confirm = d->making_folder = false;
    d->pending_path[0] = 0;
    file_controls(d);
    file_report(d, "");
}
static void file_accept_path(os64_ui_file_dialog_t *d, const char *path, bool replace)
{
    os64_strcopy(d->path, sizeof(d->path), path);
    d->replace_existing = replace;
    d->result = OS64_UI_FILE_ACCEPTED;
}
static void file_navigate(os64_ui_file_dialog_t *d, const char *path)
{
    os64_ui_status_code_t rc = os64_browser_navigate(d->browser, path);
    file_report(d, rc == OS64_UI_OK || rc == OS64_UI_BUSY ? "" : "Could not open that folder.");
}
static bool file_clean_text(const char *s)
{
    if (!s || !*s) return false;
    for (size_t i = 0; s[i]; ++i) {
        if (i >= OS64_DIRECTORY_PATH - 1 || (unsigned char)s[i] < 32 || s[i] == 127)
            return false;
    }
    return true;
}
static void file_submit(os64_ui_file_dialog_t *d)
{
    if (d->result != OS64_UI_FILE_PENDING || d->confirm || d->making_folder) return;
    if (d->layout_pending || os64_browser_loading(d->browser)) {
        file_report(d, "Wait for the folder to finish opening."); return;
    }
    if (d->selected_folder) {
        const os64_directory_t *dir = os64_browser_directory(d->browser);
        const os64_directory_entry_t *e = dir && d->selected_domain == os64_directory_domain(dir) ?
            os64_directory_entry(dir, d->selected_folder) : NULL;
        uint64_t id = d->selected_folder;
        d->selected_folder = 0;
        if (e && (e->entry.flags & OS64_DE_DIR) && os64_ui_selection_contains(
            os64_ui_dataview_selection(&d->browser_ui.details), id)) {
            (void)os64_browser_activate(d->browser, d->selected_domain, id); return;
        }
    }
    const char *base = os64_browser_location(d->browser);
    char path[OS64_DIRECTORY_PATH];
    if (!file_clean_text(d->name.buf) || !base || !*base ||
        os64_directory_path(base, d->name.buf, path) != OS64_UI_OK) {
        file_report(d, "Enter a valid file name or path."); return;
    }
    os64_dirent_t entry;
    int64_t exists = os64_stat(path, &entry);
    if (exists == 0 && (entry.flags & OS64_DE_DIR)) {
        file_navigate(d, path); return;
    }
    /* A trailing slash or dot component expresses a directory, even if the
     * normalized spelling happens to name a regular file. */
    const char *leaf = d->name.buf;
    for (const char *s = leaf; *s; ++s) if (*s == '/') leaf = s + 1;
    if (!*leaf || os64_streq(leaf, ".") || os64_streq(leaf, "..")) {
        file_report(d, "Enter a file name after the folder path."); return;
    }
    if (d->mode == OS64_UI_FILE_OPEN) {
        if (exists < 0) { file_report(d, "That file could not be found or inspected."); return; }
        file_accept_path(d, path, false); return;
    }
    char parent[OS64_DIRECTORY_PATH];
    os64_strcopy(parent, sizeof(parent), path);
    size_t n = os64_strlen(parent);
    while (n > 1 && parent[n-1] != '/') --n;
    parent[n > 1 ? n-1 : 1] = 0;
    os64_dirent_t folder;
    if (os64_stat(parent, &folder) < 0 || !(folder.flags & OS64_DE_DIR)) {
        file_report(d, "The destination folder could not be found or inspected."); return;
    }
    if (exists == 0) {
        os64_strcopy(d->pending_path, sizeof(d->pending_path), path);
        d->confirm = true;
        file_controls(d);
        char message[sizeof(d->message_text)];
        os64_snprintf(message, sizeof(message), "Replace the existing file: %s?", path);
        file_report(d, message);
    } else file_accept_path(d, path, false);
}
static void file_name_submit(os64_ui_textfield_t *tf, void *user)
{ (void)tf; file_submit(user); }
static void file_name_changed(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    os64_ui_file_dialog_t *d = user;
    d->selected_folder = 0;
    file_report(d, "");
}
static void file_create_folder(os64_ui_file_dialog_t *d)
{
    char path[OS64_DIRECTORY_PATH];
    if (!file_clean_text(d->folder.buf) || os64_directory_join(
        os64_browser_location(d->browser), d->folder.buf, path) != OS64_UI_OK) {
        file_report(d, "Enter a single folder name, without slashes."); return;
    }
    if (os64_mkdir(path) < 0) {
        file_report(d, "Could not create the folder. Check its name and destination."); return;
    }
    file_cancel_prompt(d);
    file_navigate(d, path);
}
static void file_folder_submit(os64_ui_textfield_t *tf, void *user)
{ (void)tf; file_create_folder(user); }
static void file_clicked(os64_ui_widget_t *w, void *user)
{
    os64_ui_file_dialog_t *d = user;
    if (w == &d->accept) file_submit(d);
    else if (w == &d->cancel) d->result = OS64_UI_FILE_CANCELLED;
    else if (w == &d->keep || w == &d->folder_cancel) file_cancel_prompt(d);
    else if (w == &d->folder_create) file_create_folder(d);
    else if (w == &d->new_folder) {
        if (os64_browser_loading(d->browser) || !*os64_browser_location(d->browser)) {
            file_report(d, "Open a folder before creating one."); return;
        }
        d->making_folder = true;
        os64_ui_textfield_set(&d->ui, &d->folder, "");
        file_controls(d); file_report(d, "");
    } else if (w == &d->replace && d->confirm) {
        os64_dirent_t entry;
        if (os64_stat(d->pending_path, &entry) < 0 || (entry.flags & OS64_DE_DIR)) {
            file_cancel_prompt(d);
            file_report(d, "The destination file changed. Choose it again.");
        } else file_accept_path(d, d->pending_path, true);
    }
}
static void file_hidden_changed(os64_ui_checkbox_t *cb, void *user)
{
    os64_ui_file_dialog_t *d = user;
    os64_ui_status_code_t rc = os64_browser_filter(d->browser,
        os64_browser_filter_text(d->browser), cb->checked);
    if (rc != OS64_UI_OK) {
        os64_ui_checkbox_set(&d->ui, cb, os64_browser_hidden(d->browser));
        file_report(d, "Could not change hidden-file visibility.");
    }
}
static void file_activate_pending(os64_ui_file_dialog_t *d);

static void file_changed(os64_ui_dataview_t *view,
    const os64_ui_dataview_change_t *change, void *user)
{
    os64_ui_file_dialog_t *d = user;
    if (change->activated) {
        d->pending_domain = change->domain;
        d->pending_id = change->activated;
    }
    if (!(change->flags & (OS64_UI_DATAVIEW_SELECTION | OS64_UI_DATAVIEW_FOCUS))) return;
    os64_ui_selection_t *selection = os64_ui_dataview_selection(view);
    const os64_directory_t *dir = os64_browser_directory(d->browser);
    if (!dir || os64_directory_domain(dir) != change->domain) return;
    uint64_t id = os64_ui_selection_focus(selection);
    const os64_directory_entry_t *e = os64_directory_entry(dir, id);
    d->selected_folder = 0;
    if (e && os64_ui_selection_contains(selection, id)) {
        if (e->entry.flags & OS64_DE_DIR) {
            d->selected_folder = id; d->selected_domain = change->domain;
        } else os64_ui_textfield_set(&d->ui, &d->name, e->entry.name);
    }
}

/* Width and height are one value so concurrent applications publish a whole
 * pair. Read at every open; closing an older dialog never writes it back. */
static bool file_size_parse(const char *text, uint32_t *width, uint32_t *height)
{
    uint32_t size[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            size[i] = size[i] * 10 + (uint32_t)(*text++ - '0');
            /* Bound arithmetic before native creation. Open clamps to the
             * screen; FIT_SCREEN then accounts for its decorations. */
            if (size[i] > 32767) return false;
        }
        if (!size[i] || (i == 0 ? *text++ != ',' : *text != 0)) return false;
    }
    *width = size[0]; *height = size[1];
    return true;
}
static void file_size_baseline(os64_ui_file_dialog_t *d)
{
    d->size_width = d->draw.surf.width;
    d->size_height = d->draw.surf.height;
}
static void file_size_remember(os64_ui_file_dialog_t *d)
{
    bool changed = d->size_width != d->draw.surf.width ||
        d->size_height != d->draw.surf.height;
    os64_gui_window_state_t state;
    if (os64_gui_window_get_state(d->window, &state) != 0) return;
    bool suspended = (state.flags & (OS64_GUI_WINDOW_MAXIMIZED | OS64_GUI_WINDOW_MINIMIZED)) != 0;
    /* Keep the last normal dimensions through maximize/minimize. A restore
     * matches them; a manual resize out of maximized state is a new choice. */
    if (suspended) return;
    file_size_baseline(d);
    if (!d->size_ready || !changed || d->layout_pending) return;
    char value[32];
    os64_snprintf(value, sizeof(value), "%u,%u", d->size_width, d->size_height);
    os64_conf_pair_t pair = {"size", value};
    /* The WM emits resize on completion of a drag. Persistence is best effort:
     * a read-only configuration destination must not prevent choosing a file.
     * The checked writer atomically replaces the pair, preserving the old file
     * on failure; this generated decimal value needs no extra validator. */
    (void)os64_conf_write_checked("file-dialog.conf", &pair, 1, NULL, NULL);
}

void os64_ui_file_dialog_event(os64_ui_file_dialog_t *d, const os64_gui_event_t *event)
{
    if (!d || d->window <= 0 || !event || d->result != OS64_UI_FILE_PENDING) return;
    if (os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_ESCAPE,0}, event)) {
        if (d->confirm || d->making_folder) file_cancel_prompt(d);
        else d->result = OS64_UI_FILE_CANCELLED;
        return;
    }
    if (!d->confirm && !d->making_folder) {
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){'l',OS64_GUI_MOD_CTRL}, event)) {
            os64_ui_browser_focus_address(&d->browser_ui); return;
        }
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){'f',OS64_GUI_MOD_CTRL}, event)) {
            os64_ui_browser_focus_filter(&d->browser_ui); return;
        }
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){'h',OS64_GUI_MOD_CTRL}, event)) {
            os64_ui_checkbox_set(&d->ui, &d->hidden, !d->hidden.checked);
            file_hidden_changed(&d->hidden, d); return;
        }
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_UP,OS64_GUI_MOD_ALT}, event) ||
            (d->ui.focus == &d->browser_ui.details.w &&
             os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_BACKSPACE,0}, event))) {
            (void)os64_browser_up(d->browser); return;
        }
        if (os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_F5,0}, event)) {
            (void)os64_browser_refresh(d->browser); return;
        }
    }
    (void)os64_ui_dispatch(&d->ui, event);
    if (event->type == OS64_GUI_EVENT_WINDOW_RESIZE) file_size_remember(d);
    if (d->ui.quit) d->result = OS64_UI_FILE_CANCELLED;
    else file_activate_pending(d);
}

static void file_activate_pending(os64_ui_file_dialog_t *d)
{
    if (d->result != OS64_UI_FILE_PENDING) return;
    os64_ui_selection_t *selection = os64_ui_dataview_selection(&d->browser_ui.details);
    if (os64_ui_selection_count(selection) > 1) {
        os64_ui_status_code_t rc = os64_ui_dataview_command(&d->browser_ui.details,
            OS64_UI_SELECTION_CHOOSE, os64_ui_selection_focus(selection), false);
        if (rc != OS64_UI_OK) {
            d->pending_id = 0;
            file_report(d, "Choose one file."); return;
        }
    }
    if (d->pending_id) {
        uint64_t id = d->pending_id, domain = d->pending_domain;
        d->pending_id = d->pending_domain = 0;
        const os64_directory_t *dir = os64_browser_directory(d->browser);
        const os64_directory_entry_t *e = dir && domain == os64_directory_domain(dir) ?
            os64_directory_entry(dir, id) : NULL;
        if (e && !d->confirm && !d->making_folder && !os64_browser_loading(d->browser)) {
            d->selected_folder = 0;
            if (e->entry.flags & OS64_DE_DIR)
                (void)os64_browser_activate(d->browser, domain, id);
            else {
                os64_ui_textfield_set(&d->ui, &d->name, e->entry.name);
                file_submit(d);
            }
        }
    }
}

void os64_ui_file_dialog_update(os64_ui_file_dialog_t *d)
{
    if (!d || d->window <= 0 || d->result != OS64_UI_FILE_PENDING) return;
    file_activate_pending(d);
    if (!d->confirm && !d->making_folder) (void)os64_browser_poll(d->browser);
    const char *location = os64_browser_location(d->browser);
    if (!os64_streq(d->location, location)) {
        d->selected_folder = 0;
        /* Save carries a proposed name through navigation. Open discards the
         * old folder's file name, except for the initial suggested name. */
        if (*d->location && d->mode == OS64_UI_FILE_OPEN)
            os64_ui_textfield_set(&d->ui, &d->name, "");
        os64_strcopy(d->location, sizeof(d->location), location);
    }
    if (d->selected_folder) {
        const os64_directory_t *dir = os64_browser_directory(d->browser);
        if (!dir || d->selected_domain != os64_directory_domain(dir) ||
            !os64_ui_selection_contains(os64_ui_dataview_selection(&d->browser_ui.details),
                d->selected_folder)) d->selected_folder = 0;
    }
    os64_ui_browser_sync(&d->browser_ui);
    os64_ui_checkbox_set(&d->ui, &d->hidden, os64_browser_hidden(d->browser));
    if (d->layout_pending || d->footer_dirty) file_resize(&d->ui);
    os64_ui_set_enabled(&d->ui, &d->accept, !d->layout_pending &&
        !os64_browser_loading(d->browser) && (*d->name.buf || d->selected_folder));
    if (d->min_width != d->applied_min_width || d->min_height != d->applied_min_height) {
        if (os64_gui_window_set_min_size(d->window, d->min_width, d->min_height) == 0) {
            d->applied_min_width = d->min_width; d->applied_min_height = d->min_height;
            if (os64_draw_ctx_refresh(&d->draw) != 0) {
                d->result = OS64_UI_FILE_CANCELLED; return;
            }
            d->root.bounds.w = (int32_t)d->draw.surf.width;
            d->root.bounds.h = (int32_t)d->draw.surf.height;
            file_resize(&d->ui);
            file_size_baseline(d);
        }
    }
    os64_ui_paint(&d->ui);
}
static void file_event_callback(void *user, const os64_gui_event_t *event)
{ os64_ui_file_dialog_event(user, event); }
static void file_paint_callback(void *user)
{ os64_ui_file_dialog_update(user); }

/* Composition is separate from native creation so host tests can exercise
 * the same widgets, layout, prompts and selection policy on a real canvas. */
static os64_ui_status_code_t file_compose(os64_ui_file_dialog_t *d,
    const os64_ui_file_options_t *options)
{
    os64_ui_init(&d->ui, &d->draw);
    os64_ui_panel(&d->root);
    d->root.bounds = (os64_gui_rect_t){0,0,(int32_t)d->draw.surf.width,(int32_t)d->draw.surf.height};
    d->root.user = d;
    os64_ui_set_root(&d->ui, &d->root);
    os64_ui_panel(&d->browser_area); os64_ui_add_child(&d->root, &d->browser_area);
    os64_ui_browser_init(&d->browser_ui, &d->ui, &d->browser_area);
    d->browser_ui.details.on_change = file_changed;
    d->browser_ui.details.callback_user = d;
    os64_ui_panel(&d->form); os64_ui_add_child(&d->root, &d->form);
    os64_ui_label(&d->name_label, "File name");
    os64_ui_textfield(&d->name, d->name_text, sizeof(d->name_text), file_name_submit, NULL, d);
    d->name.on_change = file_name_changed;
    os64_ui_label(&d->message, d->message_text);
    os64_ui_checkbox(&d->hidden, "Show hidden files", false, file_hidden_changed, d);
    os64_ui_label(&d->folder_label, "Folder");
    os64_ui_textfield(&d->folder, d->folder_text, sizeof(d->folder_text), file_folder_submit, NULL, d);
    os64_ui_widget_t *buttons[] = {&d->accept,&d->cancel,&d->new_folder,&d->replace,
        &d->keep,&d->folder_create,&d->folder_cancel};
    const char *labels[] = {d->mode == OS64_UI_FILE_SAVE ? "Save" : "Open", "Cancel",
        "New Folder", "Replace", "Keep File", "Create", "Cancel"};
    for (size_t i = 0; i < sizeof(buttons)/sizeof(*buttons); ++i)
        os64_ui_button(buttons[i], labels[i], file_clicked, d);
    os64_ui_widget_t *children[] = {&d->name_label,&d->name.w,&d->hidden.w,&d->new_folder,
        &d->message,&d->accept,&d->cancel,&d->replace,&d->keep,
        &d->folder_label,&d->folder.w,&d->folder_create,&d->folder_cancel};
    for (size_t i = 0; i < sizeof(children)/sizeof(*children); ++i)
        os64_ui_add_child(&d->form, children[i]);
    d->ui.on_resize = file_resize;
    if (os64_ui_font_planner(&d->ui, file_plan, file_committed, file_discarded, d) != OS64_FONT_OK)
        return OS64_UI_NO_MEMORY;
    os64_font_status_t font_rc = file_layout(d, false);
    if (font_rc == OS64_FONT_LIMIT &&
        os64_gui_window_set_min_size(d->window, d->min_width, d->min_height) == 0) {
        d->applied_min_width = d->min_width; d->applied_min_height = d->min_height;
        if (os64_draw_ctx_refresh(&d->draw) != 0) return OS64_UI_ERROR;
        d->root.bounds.w = (int32_t)d->draw.surf.width;
        d->root.bounds.h = (int32_t)d->draw.surf.height;
        font_rc = file_layout(d, false);
    }
    if (font_rc != OS64_FONT_OK)
        return font_rc == OS64_FONT_NO_MEMORY ? OS64_UI_NO_MEMORY : OS64_UI_LIMIT;
    /* Follow only after the tree and planner can reject an unfit font. A
     * tiny saved window may grow for a valid candidate, but a refused native
     * minimum leaves the fallback font usable on the smaller display. */
    d->staged_min_width = d->staged_min_height = 0;
    if (os64_ui_font_follow(&d->ui) != 0 &&
        (d->staged_min_width > d->root.bounds.w || d->staged_min_height > d->root.bounds.h) &&
        os64_gui_window_set_min_size(d->window, d->staged_min_width, d->staged_min_height) == 0) {
        d->applied_min_width = d->staged_min_width; d->applied_min_height = d->staged_min_height;
        if (os64_draw_ctx_refresh(&d->draw) != 0) return OS64_UI_ERROR;
        d->root.bounds.w = (int32_t)d->draw.surf.width;
        d->root.bounds.h = (int32_t)d->draw.surf.height;
        (void)os64_ui_font_follow(&d->ui);
    }
    font_rc = file_layout(d, false);
    if (font_rc != OS64_FONT_OK)
        return font_rc == OS64_FONT_NO_MEMORY ? OS64_UI_NO_MEMORY : OS64_UI_LIMIT;
    os64_ui_status_code_t rc = os64_browser_create(&d->browser_ui.details,
        options->icons, d->window, 0x200u, &d->browser);
    if (rc) return rc;
    os64_ui_browser_bind(&d->browser_ui, d->browser);
    file_controls(d);
    os64_ui_textfield_set(&d->ui, &d->name, options->filename ? options->filename : "");
    d->name.anchor = 0; d->name.selected = d->name.len != 0;
    return OS64_UI_OK;
}

os64_ui_status_code_t os64_ui_file_dialog_open(os64_ui_file_dialog_t *d,
    os64_ui_dispatcher_t *dispatcher, int64_t parent, const os64_ui_file_options_t *options)
{
    if (!d || !options || (options->mode != OS64_UI_FILE_SAVE && options->mode != OS64_UI_FILE_OPEN))
        return OS64_UI_INVALID;
    if (d->window > 0) { (void)os64_gui_window_focus(d->window); return OS64_UI_BUSY; }
    char initial[OS64_DIRECTORY_PATH], check[OS64_DIRECTORY_PATH];
    if (options->directory) {
        if (options->directory[0] != '/' || os64_directory_path("/", options->directory, initial))
            return OS64_UI_INVALID;
    } else (void)os64_directory_home(initial);
    if (options->filename && *options->filename &&
        (!file_clean_text(options->filename) || os64_directory_join("/", options->filename, check)))
        return OS64_UI_INVALID;
    if (dispatcher && !os64_ui_dispatcher_can_create(dispatcher)) return OS64_UI_BUSY;
    *d = (os64_ui_file_dialog_t){.mode=options->mode,.dispatcher=dispatcher};
    os64_gui_window_state_t state;
    int32_t x = 100, y = 80;
    if (parent > 0 && os64_gui_window_get_state(parent, &state) == 0) {
        x = state.x + 32; y = state.y + 40;
    }
    const char *title = options->title ? options->title :
        options->mode == OS64_UI_FILE_SAVE ? "Save As" : "Open";
    uint32_t width = 880, height = 700;
    char value[32];
    if (os64_conf_get("file-dialog.conf", "size", value, sizeof(value)) == 0)
        (void)file_size_parse(value, &width, &height);
    /* The kernel checks its dimension ceiling before FIT_SCREEN. Bound the
     * request first; FIT_SCREEN then subtracts the current frame's insets.
     * Without screen info, the ABI ceiling is safe on supported displays.
     * These opening adjustments must not overwrite the saved preference. */
    uint32_t screen_width, screen_height;
    if (os64_gui_screen_info(&screen_width, &screen_height) != 0)
        screen_width = screen_height = OS64_GUI_WINDOW_DIM_MAX;
    if (width > screen_width) width = screen_width;
    if (height > screen_height) height = screen_height;
    /* Composition raises the native floor to the measured controls. */
    if (width < OS64_GUI_MIN_CONTENT) width = OS64_GUI_MIN_CONTENT;
    if (height < OS64_GUI_MIN_CONTENT) height = OS64_GUI_MIN_CONTENT;
    d->window = os64_gui_window_create_content(title,x,y,width,height,OS64_GUI_CREATE_FIT_SCREEN);
    if (d->window <= 0) { d->window = 0; return OS64_UI_ERROR; }
    os64_ui_status_code_t rc = OS64_UI_ERROR;
    if (os64_draw_ctx_init(&d->draw, d->window) != 0) goto fail;
    rc = file_compose(d, options);
    if (rc) goto fail;
    file_resize(&d->ui);
    rc = os64_browser_navigate(d->browser, initial);
    if (rc != OS64_UI_OK && rc != OS64_UI_BUSY) goto fail;
    if (dispatcher) {
        rc = os64_ui_dispatcher_add(dispatcher,d->window,&d->ui,file_event_callback,file_paint_callback,d);
        if (rc) goto fail;
        d->registered = true;
    }
    os64_ui_file_dialog_update(d);
    file_size_baseline(d);
    d->size_ready = true;
    return OS64_UI_OK;
fail:
    (void)os64_ui_file_dialog_close(d);
    return rc;
}
void os64_ui_file_dialog_pump(os64_ui_file_dialog_t *d)
{
    if (!d || d->window <= 0 || d->registered) return;
    for (size_t i = 0; i < OS64_UI_DISPATCH_BUDGET && d->result == OS64_UI_FILE_PENDING; ++i) {
        os64_gui_event_t event;
        int64_t rc = os64_gui_event_poll(d->window, &event);
        if (rc < 0) { d->result = OS64_UI_FILE_CANCELLED; break; }
        if (!rc) break;
        os64_ui_file_dialog_event(d, &event);
    }
    os64_ui_file_dialog_update(d);
}
os64_ui_status_code_t os64_ui_file_dialog_close(os64_ui_file_dialog_t *d)
{
    if (!d) return OS64_UI_INVALID;
    if (d->window <= 0) return OS64_UI_OK;
    if (d->result == OS64_UI_FILE_PENDING) d->result = OS64_UI_FILE_CANCELLED;
    /* Worker pools ring this window; stop them before releasing its handle. */
    if (d->browser) { os64_browser_destroy(d->browser); d->browser = NULL; }
    if (os64_ui_font_release(&d->ui) != OS64_FONT_OK) return OS64_UI_BUSY;
    int64_t rc = d->registered ? os64_ui_dispatcher_destroy(d->dispatcher, d->window) :
        os64_gui_window_destroy(d->window);
    if (rc < 0) return OS64_UI_ERROR;
    d->window = 0; d->registered = false;
    return OS64_UI_OK;
}
