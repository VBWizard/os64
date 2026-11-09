#ifndef OS64_UI_WINDOW_H
#define OS64_UI_WINDOW_H
#include "os64/ui.h"
#include "os64/ui_result.h"
typedef struct {
    const char *title;
    int32_t x, y;
    uint32_t content_width, content_height;
    uint64_t flags;
    os64_window_remember_t remember;
} os64_ui_window_options_t;

// Open into unused caller-owned storage. Initializes UI once; compose widgets
// afterward. No dispatcher registration. Omit remember.config to opt out.
// Outcome                          | ownership through *out_window
// Invalid options / create failure | no positive handle held
// Post-create failure, destroyed   | zero; no handle held
// Post-create failure, destroy busy| positive; retain storage and retry cleanup
// Success                         | positive; caller owns window and storage
// A negative creation result is retained for diagnostics (e.g. NOT_RUNNING).
// Do not reinitialize storage while it still owns a positive native handle.
os64_ui_status_code_t os64_ui_window_open(os64_ui_t *, os64_draw_ctx_t *,
    int64_t *out_window, const os64_ui_window_options_t *);
#endif
