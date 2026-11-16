#ifndef PULSE_OVERLAY_H
#define PULSE_OVERLAY_H
#include "os64/ui.h"
// Local overlay helper: target IDs belong to the owner, and time is supplied
// by it. The helper neither samples data nor creates a native window.
typedef struct {
    int target, shown;
    int64_t deadline;
    bool keyboard;
    void *runs[10];
} pulse_overlay_t;
void pulse_overlay_init(pulse_overlay_t *);
void pulse_overlay_cancel(pulse_overlay_t *);
void pulse_overlay_release(pulse_overlay_t *);
void pulse_overlay_hover(pulse_overlay_t *,int target,int64_t now);
void pulse_overlay_focus(pulse_overlay_t *,int target);
bool pulse_overlay_due(pulse_overlay_t *,int64_t now);
// Wrap bounded text into the available rectangle, with an ellipsis if the
// last visible line cannot hold the remainder. Drawing stays inside bounds.
bool pulse_overlay_paint(pulse_overlay_t *,os64_ui_t *,os64_gui_surface_t *,
    os64_gui_rect_t bounds,os64_gui_rect_t anchor,const char *text);
#endif
