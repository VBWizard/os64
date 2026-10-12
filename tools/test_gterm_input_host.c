// A release queued before overflow must cancel the terminal's private drag.
#include <assert.h>
#include <stdio.h>
#include "gui/event_queue.h"
#define main gterm_unused_main
#include "../userland/apps/gterm/gterm.c"
#undef main

void gterm_input_loss_contracts(void)
{
    os64_ui_init(&gUi, NULL);
    os64_ui_widget_t widget;
    os64_ui_panel(&widget);
    gUi.grab = &widget;
    gUi.grab_button = OS64_GUI_MOUSE_LEFT;
    gDragging = gSelLive = true;
    gui_event_queue_t queue = {0};
    input_event_t event = {.type = INPUT_EVENT_MOUSE_BUTTON_UP};
    assert(gui_event_queue_push(&queue, &event) == 1);
    event.type = INPUT_EVENT_MOUSE_MOVE;
    for (unsigned i = 1; i < GUI_WINDOW_EVENTS_MAX - 1; ++i)
        assert(gui_event_queue_push(&queue, &event) == 1);
    assert(gui_event_queue_push(&queue, &event) == 0);
    assert(gui_event_queue_pop(&queue, &event) && event.type == INPUT_EVENT_INPUT_LOST);
    os64_gui_event_t lost = {.type = event.type};
    interrupt_gestures(&lost);
    assert(!gDragging && !gUi.grab && gSelLive);
    assert(!gui_event_queue_pop(&queue, &event));
    os64_ui_font_release(&gUi);
    puts("gterm input: overflow cancels selection drag and toolkit grab without copying");
}
