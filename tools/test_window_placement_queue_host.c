#include <assert.h>
#include <stdio.h>
#include "gui/event_queue.h"

static void push(gui_event_queue_t *q, unsigned type)
{
    input_event_t e = {.type=type, .doorbell={.mask=1}};
    if (type == INPUT_EVENT_WINDOW_PLACEMENT)
        e.placement.changed = OS64_GUI_PLACEMENT_X;
    (void)gui_event_queue_push(q, &e);
}
static input_event_t pop(gui_event_queue_t *q, unsigned type)
{
    input_event_t e;
    assert(gui_event_queue_pop(q, &e));
    assert(e.type == type);
    return e;
}
static void masked_overflow(void)
{
    gui_event_queue_t q = {0};
    for (unsigned i=0; i<GUI_WINDOW_EVENTS_MAX; ++i) push(&q, INPUT_EVENT_MOUSE_MOVE);
    input_event_t e = {.type=INPUT_EVENT_WINDOW_PLACEMENT,
        .placement={.width=1800,.height=900,.changed=12},.tick=1};
    gui_event_queue_push(&q, &e);
    e.placement.x=42; e.placement.y=-18;
    e.placement.width=800; e.placement.height=600; e.placement.changed=3; e.tick=2;
    gui_event_queue_push(&q, &e);
    e=pop(&q, INPUT_EVENT_WINDOW_PLACEMENT);
    assert(e.placement.changed==15 && e.placement.width==1800 && e.placement.height==900);
    assert(e.placement.x==42 && e.placement.y==-18 && e.tick==2);
    pop(&q, INPUT_EVENT_INPUT_LOST);
    assert(!gui_event_queue_pending(&q));
    e.placement.changed=OS64_GUI_PLACEMENT_HEIGHT; e.placement.height=777;
    gui_event_queue_push(&q, &e);
    assert(gui_event_queue_pending(&q));
    e=pop(&q, INPUT_EVENT_WINDOW_PLACEMENT);
    assert(e.placement.changed==OS64_GUI_PLACEMENT_HEIGHT && e.placement.height==777);
    assert(!gui_event_queue_pending(&q));
    e.placement.changed=0; gui_event_queue_push(&q, &e);
    assert(!gui_event_queue_pending(&q));
}
static void replenishment(void)
{
    gui_event_queue_t q = {0};
    const unsigned types[]={INPUT_EVENT_DOORBELL,INPUT_EVENT_WINDOW_PLACEMENT,INPUT_EVENT_WINDOW_CLOSE};
    for (unsigned j=0;j<3;++j) push(&q,types[j]);
    for (unsigned i=0;i<300;++i) {
        pop(&q,types[i%3]); push(&q,types[i%3]);
    }
    q=(gui_event_queue_t){0};
    push(&q,INPUT_EVENT_DOORBELL); push(&q,INPUT_EVENT_WINDOW_PLACEMENT);
    for (unsigned i=0;i<100;++i) {
        pop(&q,types[i%2]); push(&q,types[i%2]);
    }
}
static void ordering(void)
{
    gui_event_queue_t q = {0};
    push(&q,INPUT_EVENT_MOUSE_MOVE); push(&q,INPUT_EVENT_MOUSE_MOVE);
    push(&q,INPUT_EVENT_POINTER_STATE); push(&q,INPUT_EVENT_DOORBELL);
    pop(&q,INPUT_EVENT_DOORBELL); pop(&q,INPUT_EVENT_MOUSE_MOVE);
    push(&q,INPUT_EVENT_DOORBELL);
    pop(&q,INPUT_EVENT_DOORBELL); pop(&q,INPUT_EVENT_MOUSE_MOVE);
    pop(&q,INPUT_EVENT_POINTER_STATE);
    push(&q,INPUT_EVENT_DOORBELL); push(&q,INPUT_EVENT_WINDOW_PLACEMENT);
    push(&q,INPUT_EVENT_WINDOW_CLOSE); push(&q,INPUT_EVENT_POINTER_STATE);
    for (unsigned i=0;i<100;++i) {
        push(&q,INPUT_EVENT_APPEARANCE); pop(&q,INPUT_EVENT_APPEARANCE);
    }
    pop(&q,INPUT_EVENT_DOORBELL); pop(&q,INPUT_EVENT_WINDOW_PLACEMENT);
    pop(&q,INPUT_EVENT_WINDOW_CLOSE); pop(&q,INPUT_EVENT_POINTER_STATE);
}
static void repeated_loss(void)
{
    gui_event_queue_t q = {0};
    for (unsigned i=0;i<GUI_WINDOW_EVENTS_MAX;++i) push(&q,INPUT_EVENT_WINDOW_CLOSE);
    push(&q,INPUT_EVENT_DOORBELL); push(&q,INPUT_EVENT_WINDOW_PLACEMENT);
    pop(&q,INPUT_EVENT_DOORBELL); pop(&q,INPUT_EVENT_WINDOW_PLACEMENT);
    pop(&q,INPUT_EVENT_INPUT_LOST);
    for (unsigned i=0;i<100;++i) {
        // Refill/overflow before the control turn: loss must not starve close.
        push(&q,INPUT_EVENT_WINDOW_CLOSE); push(&q,INPUT_EVENT_WINDOW_CLOSE);
        pop(&q,INPUT_EVENT_WINDOW_CLOSE);
        push(&q,INPUT_EVENT_DOORBELL); push(&q,INPUT_EVENT_WINDOW_PLACEMENT);
        pop(&q,INPUT_EVENT_DOORBELL); pop(&q,INPUT_EVENT_WINDOW_PLACEMENT);
        pop(&q,INPUT_EVENT_INPUT_LOST);
    }
}
static void legacy_empty_ring_loss(void)
{
    gui_event_queue_t q={0};
    push(&q,INPUT_EVENT_WINDOW_CLOSE);
    for (unsigned i=0;i<GUI_WINDOW_EVENTS_MAX;++i) push(&q,INPUT_EVENT_MOUSE_MOVE);
    pop(&q,INPUT_EVENT_INPUT_LOST);
    for (unsigned i=0;i<GUI_WINDOW_EVENTS_MAX;++i) push(&q,INPUT_EVENT_MOUSE_MOVE);
    pop(&q,INPUT_EVENT_WINDOW_CLOSE);
    assert(q.head==q.tail && q.lost_pending);
    // With no placement, preserve the existing empty-ring doorbell priority.
    for (unsigned i=0;i<10;++i) {
        push(&q,INPUT_EVENT_DOORBELL);pop(&q,INPUT_EVENT_DOORBELL);
    }
    pop(&q,INPUT_EVENT_INPUT_LOST);
    assert(!gui_event_queue_pending(&q));
}
int main(void)
{
    _Static_assert(sizeof(input_event_t)==32, "kernel ABI");
    _Static_assert(offsetof(input_event_t,placement.changed)==20, "mask ABI");
    _Static_assert(offsetof(input_event_t,tick)==24, "tick ABI");
    masked_overflow(); replenishment(); ordering(); repeated_loss(); legacy_empty_ring_loss();
    puts("placement queue: masked overflow, pending, rotation, priority, pointer drain and loss progress passed");
}
