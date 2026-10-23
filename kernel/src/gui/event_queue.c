#include "gui/event_queue.h"

int gui_event_queue_push(gui_event_queue_t *q, const input_event_t *ev)
{
    if (ev->type == INPUT_EVENT_WINDOW_PLACEMENT) {
        uint32_t mask = ev->placement.changed & OS64_GUI_PLACEMENT_ALL;
        if (!mask) return 1;
        if (mask & OS64_GUI_PLACEMENT_X) q->placement.placement.x = ev->placement.x;
        if (mask & OS64_GUI_PLACEMENT_Y) q->placement.placement.y = ev->placement.y;
        if (mask & OS64_GUI_PLACEMENT_WIDTH) q->placement.placement.width = ev->placement.width;
        if (mask & OS64_GUI_PLACEMENT_HEIGHT) q->placement.placement.height = ev->placement.height;
        q->placement.placement.changed |= mask;
        q->placement.type = INPUT_EVENT_WINDOW_PLACEMENT;
        q->placement.tick = ev->tick;
        q->placement_pending = true;
        return 1;
    }
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
        // Admit new focus even when full, but report the lost history: the
        // evicted record can itself be focus. Other records drop newest.
        q->lost_pending = true;
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
    // Rotate eligible doorbell/placement/ring-loss turns: a member that stays
    // eligible is served before another gets a second turn. Appearance above
    // has priority, and pointer below waits for the ring's drain. A loss plus
    // its retained control form one ring turn, so repeated overflow cannot
    // postpone that control. Preserve the legacy empty-ring doorbell priority
    // when there is no placement; a loss alone then waits behind doorbells.
    unsigned source = 3;
    if (q->doorbell_pending && q->head == q->tail && !q->placement_pending)
        source = 0;
    else for (unsigned i = 0; i < 3; ++i) {
        unsigned candidate = (q->service_next + i) % 3;
        if ((candidate == 0 && q->doorbell_pending) ||
            (candidate == 1 && q->placement_pending) ||
            (candidate == 2 && (q->lost_pending || q->head != q->tail))) {
            source = candidate;
            break;
        }
    }
    if (source == 0) {
        *out = (input_event_t){.type = INPUT_EVENT_DOORBELL,
            .doorbell = {.mask = q->doorbell_mask}, .tick = q->doorbell_tick};
        q->doorbell_mask = 0;
        q->doorbell_pending = false;
        q->service_next = 1;
        return true;
    }
    if (source == 1) {
        *out = q->placement;
        q->placement = (input_event_t){0};
        q->placement_pending = false;
        q->service_next = 2;
        return true;
    }
    if (q->lost_pending) {
        // Compact in ring order. Removing queued downs as well as ups keeps
        // a pre-loss gesture from re-arming a client after INPUT_LOST.
        uint32_t read = q->tail, write = q->tail;
        while (read != q->head) {
            input_event_t ev = q->events[read];
            read = (read + 1) % GUI_WINDOW_EVENTS_MAX;
            if ((ev.type >= INPUT_EVENT_KEY_DOWN && ev.type <= INPUT_EVENT_MOUSE_BUTTON_UP) ||
                ev.type == INPUT_EVENT_MOUSE_WHEEL) continue;
            q->events[write] = ev;
            write = (write + 1) % GUI_WINDOW_EVENTS_MAX;
        }
        q->head = write;
        // A retained control must get a turn between loss notifications.
        // Still purge gestures above on every new loss, before that turn.
        if (!q->lost_yield_control || q->head == q->tail) {
            q->lost_pending = false;
            q->lost_yield_control = q->head != q->tail;
            q->service_next = q->head != q->tail ? 2 : 0;
            *out = (input_event_t){.type = INPUT_EVENT_INPUT_LOST};
            return true;
        }
    }
    if (q->head != q->tail) {
        *out = q->events[q->tail];
        q->tail = (q->tail + 1) % GUI_WINDOW_EVENTS_MAX;
        q->service_next = 0;
        q->lost_yield_control = false;
        return true;
    }
    if (!q->pointer_pending) return false;
    *out = q->pointer;
    q->pointer_pending = false;
    return true;
}
