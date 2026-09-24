#ifndef GUI_EVENT_QUEUE_H
#define GUI_EVENT_QUEUE_H
#include "gui/input.h"

#define GUI_WINDOW_EVENTS_MAX 64
// Protected by kGuiLock. Input keeps ring order; pointer snapshots coalesce
// outside the ring and follow its drain, so old motions cannot undo a leave.
// Appearance generations coalesce separately and precede input to avoid
// starvation while a client continuously receives pointer motion.
typedef struct gui_event_queue {
    input_event_t events[GUI_WINDOW_EVENTS_MAX];
    uint32_t head, tail;
    input_event_t pointer;
    bool pointer_pending;
    input_event_t appearance;
    bool appearance_pending;
} gui_event_queue_t;

// 0 = dropped, 1 = stored, 2 = stored after evicting the oldest input.
int gui_event_queue_push(gui_event_queue_t *q, const input_event_t *ev);
bool gui_event_queue_pop(gui_event_queue_t *q, input_event_t *out);
static inline bool gui_event_queue_pending(const gui_event_queue_t *q)
{
    return q->head != q->tail || q->pointer_pending || q->appearance_pending;
}
#endif
