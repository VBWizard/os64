#ifndef OS64_UI_STATUS_H
#define OS64_UI_STATUS_H
#include "os64/ui.h"
#include "os64/ui_result.h"
#define OS64_UI_STATUS_TEXT_MAX 512u
/* Storage belongs to the app, as for other retained widgets. Slot fields are
 * toolkit-owned; use setters, including before attach. Source text is copied.
 * Counters/tokens are scoped to this widget and not reused after clearing. */
typedef struct {
    char *text;
    void *run, *staged;
    uint64_t token;
} os64_ui_status_slot_t;
typedef struct {
    os64_ui_widget_t w;
    os64_ui_status_slot_t base, help, notice;
    uint64_t generation;
} os64_ui_status_t;
extern const os64_ui_class_t os64_ui_status_class;
void os64_ui_status_init(os64_ui_status_t *);
os64_ui_status_code_t os64_ui_status_base(os64_ui_status_t *, const char *);
os64_ui_status_code_t os64_ui_status_help(os64_ui_status_t *, const char *, uint64_t *token);
os64_ui_status_code_t os64_ui_status_clear_help(os64_ui_status_t *, uint64_t token);
os64_ui_status_code_t os64_ui_status_notice(os64_ui_status_t *, const char *, uint64_t *generation);
os64_ui_status_code_t os64_ui_status_clear_notice(os64_ui_status_t *, uint64_t generation);
/* Release copied messages and font runs, also done by tree font teardown.
 * Reface releases runs while retaining messages and token ownership. */
void os64_ui_status_destroy(os64_ui_status_t *);
#endif
