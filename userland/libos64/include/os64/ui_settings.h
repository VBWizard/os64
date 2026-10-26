#ifndef OS64_UI_SETTINGS_H
#define OS64_UI_SETTINGS_H
#include "os64/ui.h"
#include "os64/ui_dispatcher.h"

/* App-owned modeless dialog. Build children in body after open, then call
 * ready. Its events reach it one of two ways: pumped from the parent's loop,
 * or, attached to the parent's dispatcher, delivered by its turns; attached,
 * the dialog closes itself when asked (Close, Escape). Close before
 * releasing app widgets. Reopening a live dialog restores/focuses it without
 * resetting its draft. */
typedef struct os64_ui_settings os64_ui_settings_t;
struct os64_ui_settings {
    os64_ui_t ui; /* first, for libui resize callbacks */
    os64_draw_ctx_t ctx;
    os64_ui_widget_t root, body, status, actions[3];
    int64_t window;
    unsigned body_rows;
    /* ready() is adopting the window's first font: the planner may grow the
     * window to the row that font makes, where a live window refuses a font
     * it cannot hold. */
    bool opening;
    char message[160];
    void *user;
    bool (*arrange)(os64_ui_settings_t *, os64_gui_rect_t, int32_t row, bool staged);
    /* App validates and applies; save=true also persists. Report errors/status
     * with os64_ui_settings_report. The helper never writes configuration. */
    void (*apply)(os64_ui_settings_t *, bool save);
    /* The dispatcher the window is registered with, NULL when pumped. */
    os64_ui_dispatcher_t *dispatcher;
};
bool os64_ui_settings_open(os64_ui_settings_t *, int64_t parent,const char *title,
    unsigned body_rows, bool (*arrange)(os64_ui_settings_t *,os64_gui_rect_t,int32_t,bool),
    void (*apply)(os64_ui_settings_t *,bool),void *user);
void os64_ui_settings_ready(os64_ui_settings_t *);
void os64_ui_settings_pump(os64_ui_settings_t *);
/* One of the dialog's events; Escape asks it to close (ui.quit). */
void os64_ui_settings_event(os64_ui_settings_t *, const os64_gui_event_t *);
/* Registers the open dialog's window with `dispatcher`, after ready. From
 * then on its turns deliver the dialog's events and paint it, and a close,
 * asked or called, destroys the window through it. False, and the dialog
 * still pumped, when the dispatcher refuses it. */
bool os64_ui_settings_attach(os64_ui_settings_t *, os64_ui_dispatcher_t *dispatcher);
void os64_ui_settings_close(os64_ui_settings_t *);
void os64_ui_settings_report(os64_ui_settings_t *,const char *);
#endif
