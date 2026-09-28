#ifndef OS64_UI_SETTINGS_H
#define OS64_UI_SETTINGS_H
#include "os64/ui.h"

/* App-owned modeless dialog. Build children in body after open, then call
 * ready. Pump from the parent's loop; close before releasing app widgets.
 * Reopening a live dialog restores/focuses it without resetting its draft. */
typedef struct os64_ui_settings os64_ui_settings_t;
struct os64_ui_settings {
    os64_ui_t ui; /* first, for libui resize callbacks */
    os64_draw_ctx_t ctx;
    os64_ui_widget_t root, body, status, actions[3];
    int64_t window;
    unsigned body_rows;
    char message[160];
    void *user;
    bool (*arrange)(os64_ui_settings_t *, os64_gui_rect_t, int32_t row, bool staged);
    /* App validates and applies; save=true also persists. Report errors/status
     * with os64_ui_settings_report. The helper never writes configuration. */
    void (*apply)(os64_ui_settings_t *, bool save);
};
bool os64_ui_settings_open(os64_ui_settings_t *, int64_t parent,const char *title,
    unsigned body_rows, bool (*arrange)(os64_ui_settings_t *,os64_gui_rect_t,int32_t,bool),
    void (*apply)(os64_ui_settings_t *,bool),void *user);
void os64_ui_settings_ready(os64_ui_settings_t *);
void os64_ui_settings_pump(os64_ui_settings_t *);
void os64_ui_settings_close(os64_ui_settings_t *);
void os64_ui_settings_report(os64_ui_settings_t *,const char *);
#endif
