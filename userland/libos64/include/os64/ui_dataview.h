#ifndef OS64_UI_DATAVIEW_H
#define OS64_UI_DATAVIEW_H
#include "os64/ui.h"
#include "os64/ui_data.h"
#include "os64/ui_icon.h"
#define OS64_UI_DATAVIEW_RUNS 16384u
#define OS64_UI_DATAVIEW_SELECTION 1u
#define OS64_UI_DATAVIEW_FOCUS 2u
#define OS64_UI_DATAVIEW_VIEWPORT 4u
#define OS64_UI_DATAVIEW_COLUMNS 8u
#define OS64_UI_DATAVIEW_MODEL 16u
typedef struct {
    uint32_t id, width;
    bool visible;
} os64_ui_dataview_column_t;
typedef enum { OS64_UI_DATAVIEW_ATTACH, OS64_UI_DATAVIEW_BIND,
    OS64_UI_DATAVIEW_LAYOUT, OS64_UI_DATAVIEW_SCROLL, OS64_UI_DATAVIEW_SORT,
    OS64_UI_DATAVIEW_COLUMN, OS64_UI_DATAVIEW_COMMAND, OS64_UI_DATAVIEW_FONT,
    OS64_UI_DATAVIEW_CONTEXT, OS64_UI_DATAVIEW_SEARCH, OS64_UI_DATAVIEW_DISPOSE } os64_ui_dataview_operation_t;
typedef struct {
    uint64_t revision;
    uint32_t flags;
    uint64_t domain, activated;
} os64_ui_dataview_change_t;
typedef struct {
    size_t metadata_bytes, peak_metadata_bytes, runs, run_slots, icon_leases, icon_failures;
    size_t painted_rows, prepared_rows;
    /* Availability describes the last query; queried is false until timed
     * input uses the clock, and resets when the clock provider changes. */
    bool clock_queried, clock_available;
} os64_ui_dataview_diagnostics_t;
typedef struct os64_ui_dataview os64_ui_dataview_t;
struct os64_ui_dataview {
    os64_ui_widget_t w;
    /* Scrollbar children belong to the view; their positions are committed by
     * its transactions. Applications use the view setters rather than these. */
    os64_ui_scrollbar_t vertical, horizontal;
    void *state;
    /* Toolkit-owned diagnostics live here so a failed attach is inspectable. */
    os64_ui_status_code_t last_status;
    os64_ui_dataview_operation_t last_operation;
    bool notifying;
    void (*on_change)(os64_ui_dataview_t *, const os64_ui_dataview_change_t *, void *);
    void (*on_error)(os64_ui_dataview_t *, os64_ui_dataview_operation_t, os64_ui_status_code_t, void *);
    void *callback_user;
};
extern const os64_ui_class_t os64_ui_dataview_class;
void os64_ui_dataview_init(os64_ui_dataview_t *);
/* One view borrows one already-bound selection and an optional provider.
 * Generic font teardown detaches it and releases resources; disposal is safe
 * after that. App-owned selection/snapshot/provider lifetimes remain separate. */
os64_ui_status_code_t os64_ui_dataview_attach(os64_ui_dataview_t *, os64_ui_selection_t *, const os64_ui_icon_provider_t *);
os64_ui_status_code_t os64_ui_dataview_dispose(os64_ui_dataview_t *);
os64_ui_status_code_t os64_ui_dataview_bind(os64_ui_dataview_t *, os64_ui_data_projection_t *, os64_ui_data_bind_t);
os64_ui_status_code_t os64_ui_dataview_layout(os64_ui_dataview_t *);
os64_ui_status_code_t os64_ui_dataview_scroll(os64_ui_dataview_t *, size_t top, int64_t left);
os64_ui_status_code_t os64_ui_dataview_sort(os64_ui_dataview_t *, os64_ui_data_sort_t);
os64_ui_status_code_t os64_ui_dataview_column_set(os64_ui_dataview_t *, uint32_t id, uint32_t width, bool visible, size_t position);
os64_ui_status_code_t os64_ui_dataview_command(os64_ui_dataview_t *, os64_ui_selection_command_t, os64_ui_data_id_t, bool reveal);
os64_ui_status_code_t os64_ui_dataview_context_target(os64_ui_dataview_t *, const os64_gui_event_t *, os64_ui_data_id_t *out);
uint32_t os64_ui_dataview_header_at(const os64_ui_dataview_t *, int32_t x, int32_t y);
os64_ui_selection_t *os64_ui_dataview_selection(const os64_ui_dataview_t *);
uint64_t os64_ui_dataview_revision(const os64_ui_dataview_t *);
size_t os64_ui_dataview_top(const os64_ui_dataview_t *);
int64_t os64_ui_dataview_left(const os64_ui_dataview_t *);
size_t os64_ui_dataview_full_rows(const os64_ui_dataview_t *);
bool os64_ui_dataview_blocked(const os64_ui_dataview_t *);
os64_ui_status_code_t os64_ui_dataview_status(const os64_ui_dataview_t *, os64_ui_dataview_operation_t *);
bool os64_ui_dataview_column(const os64_ui_dataview_t *, size_t position, os64_ui_dataview_column_t *);
os64_ui_dataview_diagnostics_t os64_ui_dataview_diagnostics(const os64_ui_dataview_t *);
/* Injectable monotonic clock; NULL restores os64_micros. No allocations. */
os64_ui_status_code_t os64_ui_dataview_clock(os64_ui_dataview_t *, int64_t (*)(void *), void *);
#endif
