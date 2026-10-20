#ifndef OS64_UI_FILE_DIALOG_H
#define OS64_UI_FILE_DIALOG_H

#include "os64/ui_browser.h"
#include "os64/ui_dispatcher.h"

typedef enum { OS64_UI_FILE_SAVE, OS64_UI_FILE_OPEN } os64_ui_file_mode_t;
typedef enum {
    OS64_UI_FILE_PENDING, OS64_UI_FILE_ACCEPTED, OS64_UI_FILE_CANCELLED
} os64_ui_file_result_t;

typedef struct {
    os64_ui_file_mode_t mode;
    const char *title;             /* NULL selects Save As / Open. */
    const char *directory;         /* Absolute; NULL selects HOME. */
    const char *filename;          /* Suggested leaf name, copied at open. */
    const os64_ui_icon_provider_t *icons; /* Borrowed until close. */
} os64_ui_file_options_t;

/* App-owned, zero-initialized storage. Use one dialog per pending operation.
 * All calls are on the UI thread, outside data-view callbacks. The dialog
 * chooses one path; it does not open, truncate or write the chosen file.
 *
 * With a dispatcher, events and painting are registered by open. Without
 * one, wait on window and call pump (or relay that window's readiness to
 * the parent's event loop). Polling only when the parent gets input is not
 * sufficient. This is a separate window, not a native modal owner lock.
 *
 * Inspect result after a dispatcher turn/pump, copy path, then close before
 * destroying the parent. Accepted/cancelled storage stays alive until close.
 * Close failures preserve storage for retry; do not free a live dialog.
 * A failed open can also leave window live if cleanup was refused; close it.
 *
 * replace_existing records explicit replacement consent. When false, Save
 * callers must create exclusively (mode "x") or publish with NOREPLACE:
 * metadata failure cannot establish absence, and a file can appear after
 * selection. I/O errors and atomic publication remain the caller's policy. */
typedef struct os64_ui_file_dialog {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root, browser_area, form, name_label, message;
    os64_ui_widget_t accept, cancel, new_folder, replace, keep;
    os64_ui_widget_t folder_label, folder_create, folder_cancel;
    os64_ui_textfield_t name, folder;
    os64_ui_checkbox_t hidden;
    os64_ui_browser_t browser_ui;
    os64_browser_t *browser;
    os64_ui_dispatcher_t *dispatcher;
    int64_t window;
    os64_ui_file_mode_t mode;
    os64_ui_file_result_t result;
    char path[OS64_DIRECTORY_PATH];
    bool replace_existing;
    /* Private interaction state; callers read window/result/path above. */
    char name_text[OS64_DIRECTORY_PATH], folder_text[OS64_DIRECTORY_PATH];
    char message_text[OS64_DIRECTORY_PATH + 80];
    char location[OS64_DIRECTORY_PATH], pending_path[OS64_DIRECTORY_PATH];
    uint64_t pending_domain, pending_id, selected_domain, selected_folder;
    int32_t min_width, min_height, staged_min_width, staged_min_height;
    int32_t applied_min_width, applied_min_height;
    bool confirm, making_folder, layout_pending, registered;
} os64_ui_file_dialog_t;

os64_ui_status_code_t os64_ui_file_dialog_open(os64_ui_file_dialog_t *,
    os64_ui_dispatcher_t *, int64_t parent, const os64_ui_file_options_t *);
/* Manual-loop entry points. Do not also pump a dispatcher-owned dialog. */
void os64_ui_file_dialog_event(os64_ui_file_dialog_t *, const os64_gui_event_t *);
void os64_ui_file_dialog_update(os64_ui_file_dialog_t *);
void os64_ui_file_dialog_pump(os64_ui_file_dialog_t *);
os64_ui_status_code_t os64_ui_file_dialog_close(os64_ui_file_dialog_t *);

#endif
