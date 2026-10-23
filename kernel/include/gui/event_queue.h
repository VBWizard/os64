#ifndef GUI_EVENT_QUEUE_H
#define GUI_EVENT_QUEUE_H
#include "gui/input.h"

#define GUI_WINDOW_EVENTS_MAX 64
// Protected by kGuiLock. Input keeps ring order; pointer snapshots coalesce
// outside the ring and follow its drain, so old motions cannot undo a leave.
// Appearance generations coalesce separately and precede input to avoid
// starvation while a client continuously receives pointer motion.
// Doorbells and explicit placement edits coalesce outside the ring. A service
// cursor shares turns with ring/loss; appearance priority and pointer-after-
// drain are ordering constraints, not participants in that rotation.
typedef struct gui_event_queue {
    input_event_t events[GUI_WINDOW_EVENTS_MAX];
    uint32_t head, tail;
    input_event_t pointer;
    bool pointer_pending;
    input_event_t appearance;
    bool appearance_pending;
    uint32_t doorbell_mask;
    uint64_t doorbell_tick;
    bool doorbell_pending;
    input_event_t placement;
    bool placement_pending;
    uint8_t service_next; // doorbell=0, placement=1, ring/loss=2
    // Pending loss purges queued gestures whose transitions may straddle
    // the gap. Window/control records survive and get turns between notices.
    bool lost_pending;
    // After reporting loss, yield to a retained ring record before reporting
    // another loss; repeated overflow must not starve close/focus/resize.
    bool lost_yield_control;
} gui_event_queue_t;

// 0 = dropped, 1 = stored, 2 = stored after evicting the oldest input.
int gui_event_queue_push(gui_event_queue_t *q, const input_event_t *ev);
bool gui_event_queue_pop(gui_event_queue_t *q, input_event_t *out);
static inline bool gui_event_queue_pending(const gui_event_queue_t *q)
{
    return q->head != q->tail || q->pointer_pending || q->appearance_pending ||
           q->doorbell_pending || q->placement_pending || q->lost_pending;
}
#endif
