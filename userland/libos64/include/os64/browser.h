#ifndef OS64_BROWSER_H
#define OS64_BROWSER_H
#include "os64/directory.h"
#include "os64/ui_dataview.h"
#define OS64_BROWSER_HISTORY 32u
#define OS64_BROWSER_CACHE_BYTES (64u * 1024u * 1024u)
typedef struct os64_browser os64_browser_t;
/* View is initialized and belongs to its UI. Browser creates/owns its selection,
 * attaches it, and disposes the view before freeing that selection. Pool/window
 * lifetime belongs to the browser/app respectively: destroy browser first. */
os64_ui_status_code_t os64_browser_create(os64_ui_dataview_t *view,const os64_ui_icon_provider_t *icons,
    int64_t window,uint32_t doorbell_bit,os64_browser_t **out);
void os64_browser_destroy(os64_browser_t *);
os64_ui_status_code_t os64_browser_navigate(os64_browser_t *,const char *path);
os64_ui_status_code_t os64_browser_back(os64_browser_t *);
os64_ui_status_code_t os64_browser_forward(os64_browser_t *);
os64_ui_status_code_t os64_browser_up(os64_browser_t *);
os64_ui_status_code_t os64_browser_refresh(os64_browser_t *);
void os64_browser_cancel(os64_browser_t *);
/* Drain on doorbell/boundary outside menu sessions and data-view callbacks.
 * Reap stale jobs without publication. Returns last publication/refusal status. */
os64_ui_status_code_t os64_browser_poll(os64_browser_t *);
os64_ui_status_code_t os64_browser_filter(os64_browser_t *,const char *filter,bool hidden);
/* Directory IDs navigate. Files return REFUSED; association policy is E5. */
os64_ui_status_code_t os64_browser_activate(os64_browser_t *,uint64_t domain,os64_ui_data_id_t);
const char *os64_browser_location(const os64_browser_t *);
const char *os64_browser_requested(const os64_browser_t *);
const char *os64_browser_filter_text(const os64_browser_t *);
bool os64_browser_hidden(const os64_browser_t *);
bool os64_browser_loading(const os64_browser_t *);
bool os64_browser_can_back(const os64_browser_t *);
bool os64_browser_can_forward(const os64_browser_t *);
const os64_directory_t *os64_browser_directory(const os64_browser_t *);
const os64_directory_places_t *os64_browser_places(const os64_browser_t *);
os64_ui_status_code_t os64_browser_status(const os64_browser_t *,int64_t *io_error);
uint64_t os64_browser_revision(const os64_browser_t *);
#endif
