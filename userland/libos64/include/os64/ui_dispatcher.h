#ifndef OS64_UI_DISPATCHER_H
#define OS64_UI_DISPATCHER_H
#include "os64/ui.h"
#include "os64/ui_result.h"
#define OS64_UI_DISPATCH_WINDOWS 32u
#define OS64_UI_DISPATCH_BUDGET 64u
/* One UI/lifecycle thread. Register application siblings here too. Native
 * creation must wait for can_create(); destroy through destroy(). The backend
 * seam supports deterministic queue/lifetime tests, with NULL selecting E1K. */
typedef struct {
    void *user;
    int64_t (*poll)(void *, int64_t, os64_gui_event_t *);
    int64_t (*wait)(void *, int64_t *);
    int64_t (*destroy)(void *, int64_t);
    int64_t (*create)(void *, int64_t owner, os64_gui_rect_t content);
    int64_t (*rect)(void *, int64_t, os64_gui_rect_t *);
    int64_t (*surface)(void *, int64_t, os64_gui_surface_t *);
    int64_t (*screen)(void *, uint32_t *, uint32_t *);
    void (*publish)(void *, int64_t, os64_gui_rect_t);
} os64_ui_window_backend_t;
typedef struct {
    int64_t handle;
    os64_ui_t *ui;
    void *user;
    void (*event)(void *, const os64_gui_event_t *);
    void (*paint)(void *);
    void (*boundary)(void *, bool queues_empty);
    void *boundary_user;
    bool drained;
} os64_ui_dispatch_entry_t;
typedef struct os64_ui_dispatcher {
    os64_ui_window_backend_t native;
    os64_ui_dispatch_entry_t entries[OS64_UI_DISPATCH_WINDOWS];
    int64_t tombstones[OS64_UI_DISPATCH_WINDOWS];
    size_t cursor, dead_count;
    bool dispatching;
    // Optional read-only diagnostic observer, before routing each event.
    void (*observe)(void *, int64_t, const os64_gui_event_t *);
    void *observe_user;
} os64_ui_dispatcher_t;
void os64_ui_dispatcher_init(os64_ui_dispatcher_t *, const os64_ui_window_backend_t *);
os64_ui_status_code_t os64_ui_dispatcher_add(os64_ui_dispatcher_t *, int64_t,
    os64_ui_t *, void (*commands)(void *, const os64_gui_event_t *),
    void (*paint)(void *), void *);
/* Remove only after native destruction. Failed destroy preserves registration.
 * Actual application teardown is deferred until its event callback returns. */
int64_t os64_ui_dispatcher_destroy(os64_ui_dispatcher_t *, int64_t);
bool os64_ui_dispatcher_can_create(const os64_ui_dispatcher_t *);
bool os64_ui_dispatcher_live(const os64_ui_dispatcher_t *, int64_t);
/* One bounded round-robin turn, followed by boundary hooks and paints. wait
 * sleeps only when no queue is ready. Returns event count or native error. */
int64_t os64_ui_dispatcher_turn(os64_ui_dispatcher_t *, bool wait);
#endif
