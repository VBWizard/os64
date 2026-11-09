#ifndef OS64_UI_BROWSER_H
#define OS64_UI_BROWSER_H

#include "os64/browser.h"
#include "os64/ui_status.h"

#define OS64_UI_BROWSER_CRUMBS 12u

typedef enum {
    OS64_UI_BROWSER_TOOLBAR,
    OS64_UI_BROWSER_ADDRESS,
    OS64_UI_BROWSER_BREADCRUMBS,
    OS64_UI_BROWSER_FILTER,
    OS64_UI_BROWSER_SIDEBAR,
    OS64_UI_BROWSER_STATUS,
    OS64_UI_BROWSER_PARTS
} os64_ui_browser_part_t;

typedef struct os64_ui_browser {
    os64_ui_t *ui;
    os64_browser_t *browser;
    os64_ui_widget_t *parent;
    os64_ui_widget_t toolbar, back, forward, up, refresh;
    os64_ui_widget_t address_row, address_label;
    os64_ui_textfield_t address;
    char address_text[OS64_DIRECTORY_PATH];
    os64_ui_widget_t crumb_row, overflow, crumbs[OS64_UI_BROWSER_CRUMBS];
    char crumb_names[OS64_UI_BROWSER_CRUMBS][OS64_DIRECTORY_PATH];
    char crumb_paths[OS64_UI_BROWSER_CRUMBS][OS64_DIRECTORY_PATH];
    size_t crumb_count;
    os64_ui_widget_t filter_row, filter_label;
    os64_ui_textfield_t filter;
    char filter_text[OS64_DIRECTORY_FILTER];
    os64_ui_widget_t sidebar, favorites_label, mounts_label;
    os64_ui_listbox_t favorites, mounts;
    os64_ui_dataview_t details;
    os64_ui_status_t status;
    uint64_t notice_token, help_token, seen_revision;
    char status_text[OS64_UI_STATUS_TEXT_MAX];
    char seen_location[OS64_DIRECTORY_PATH];
    char seen_requested[OS64_DIRECTORY_PATH];
    bool visible[OS64_UI_BROWSER_PARTS];
    bool layout_pending;
} os64_ui_browser_t;

/* The parent and widget storage remain owned by the caller. Bind the browser
 * after creating it with &part->details, then sync at dispatcher boundaries. */
void os64_ui_browser_init(os64_ui_browser_t *part, os64_ui_t *ui,
    os64_ui_widget_t *parent);
void os64_ui_browser_bind(os64_ui_browser_t *part, os64_browser_t *browser);
/* A failed live layout is retried by sync; Details may block input until
 * a replacement frame can be prepared. Staged layout follows font adoption. */
os64_font_status_t os64_ui_browser_layout(os64_ui_browser_t *part, bool staged);
void os64_ui_browser_sync(os64_ui_browser_t *part);
void os64_ui_browser_show(os64_ui_browser_t *part, os64_ui_browser_part_t part_id,
    bool visible);
bool os64_ui_browser_visible(const os64_ui_browser_t *part,
    os64_ui_browser_part_t part_id);
void os64_ui_browser_focus_address(os64_ui_browser_t *part);
void os64_ui_browser_focus_filter(os64_ui_browser_t *part);
void os64_ui_browser_message(os64_ui_browser_t *part, const char *message);

#endif
