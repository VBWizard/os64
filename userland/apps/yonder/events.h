#ifndef YONDER_EVENTS_H
#define YONDER_EVENTS_H
#include <stdbool.h>
#include <stdint.h>
#include <os64/gui.h>

/* P5 testing: Million Dollar Homepage's description popup waited for motion
 * to stop. Hover edits can rebuild a large page, so input must yield to paint.
 * Bound a turn's input work so rendering, page arrival and timers get a turn
 * while the input queue is busy. Idle motion uses absolute positions; merge
 * consecutive samples before dispatch, stopping at buttons/modifiers or a
 * different event. Drags, clicks, keys and doorbells keep their order. */
#define YONDER_EVENT_BATCH_MAX 32u

typedef int64_t (*YonderEventPoll)(void *opaque, os64_gui_event_t *event);
typedef struct {
    unsigned consumed;
    bool held, drained;
    os64_gui_event_t pending;
} YonderEventBatch;

static inline bool yonder_event_batch_next(YonderEventBatch *batch,
    os64_gui_event_t *event, YonderEventPoll poll, void *opaque)
{
    if (batch->consumed >= YONDER_EVENT_BATCH_MAX || batch->drained) return false;
    if (batch->held) {
        *event = batch->pending;
        batch->held = false;
    } else if (poll(opaque,event) != 1) {
        batch->drained = true;
        return false;
    }
    batch->consumed++;
    if (event->type != OS64_GUI_EVENT_MOUSE_MOVE || event->mouse.buttons != 0) return true;
    while (batch->consumed < YONDER_EVENT_BATCH_MAX) {
        os64_gui_event_t next;
        if (poll(opaque,&next) != 1) { batch->drained = true;break; }
        if (next.type != OS64_GUI_EVENT_MOUSE_MOVE || next.mouse.buttons != 0 ||
            next.mouse.modifiers != event->mouse.modifiers) {
            batch->pending = next;
            batch->held = true;
            break;
        }
        *event = next;
        batch->consumed++;
    }
    return true;
}
#endif
