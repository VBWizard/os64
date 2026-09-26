#include "gui/event_queue.h"

int gui_event_queue_push(gui_event_queue_t *q, const input_event_t *ev)
{
    if (ev->type == INPUT_EVENT_DOORBELL) {
        q->doorbell_mask |= ev->doorbell.mask;
        q->doorbell_tick = ev->tick;
        q->doorbell_pending = true;
        return 1;
    }
    if (ev->type == INPUT_EVENT_APPEARANCE) {
        q->appearance = *ev;
        q->appearance_pending = true;
        return 1;
    }
    if (ev->type == INPUT_EVENT_POINTER_STATE) {
        q->pointer = *ev;
        q->pointer_pending = true;
        return 1;
    }
    uint32_t next = (q->head + 1) % GUI_WINDOW_EVENTS_MAX;
    int result = 1;
    if (next == q->tail) {
        // Focus changes must reach popup clients even under pressure. The
        // oldest queued event is sacrificed; other input keeps drop-newest.
        if (ev->type != INPUT_EVENT_WINDOW_FOCUS) return 0;
        q->tail = (q->tail + 1) % GUI_WINDOW_EVENTS_MAX;
        result = 2;
    }
    q->events[q->head] = *ev;
    q->head = next;
    return result;
}

bool gui_event_queue_pop(gui_event_queue_t *q, input_event_t *out)
{
    if (q->appearance_pending) {
        *out = q->appearance;
        q->appearance_pending = false;
        return true;
    }
    if (q->doorbell_pending &&
        (!q->doorbell_yield_input || q->head == q->tail)) {
        *out = (input_event_t){.type = INPUT_EVENT_DOORBELL,
            .doorbell = {.mask = q->doorbell_mask}, .tick = q->doorbell_tick};
        q->doorbell_mask = 0;
        q->doorbell_pending = false;
        q->doorbell_yield_input = true;
        return true;
    }
    if (q->head != q->tail) {
        *out = q->events[q->tail];
        q->tail = (q->tail + 1) % GUI_WINDOW_EVENTS_MAX;
        q->doorbell_yield_input = false;
        return true;
    }
    if (!q->pointer_pending) return false;
    *out = q->pointer;
    q->pointer_pending = false;
    return true;
}
