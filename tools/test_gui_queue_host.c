/* Loss must not replay queued downs after resetting an armed consumer. */
#include <assert.h>
#include <stdio.h>
#include "gui/event_queue.h"
int main(void)
{
    for(unsigned offset=0;offset<GUI_WINDOW_EVENTS_MAX;++offset){
        gui_event_queue_t q={.head=offset,.tail=offset};input_event_t out;
        input_event_t down={.type=INPUT_EVENT_MOUSE_BUTTON_DOWN};
        input_event_t up={.type=INPUT_EVENT_MOUSE_BUTTON_UP};
        input_event_t move={.type=INPUT_EVENT_MOUSE_MOVE};
        // The first down has not reached the consumer when its release drops.
        assert(gui_event_queue_push(&q,&down)==1);
        for(unsigned i=1;i<GUI_WINDOW_EVENTS_MAX-1;++i)assert(gui_event_queue_push(&q,&move)==1);
        assert(gui_event_queue_push(&q,&up)==0);
        assert(gui_event_queue_push(&q,&down)==0);
        assert(gui_event_queue_pending(&q));
        assert(gui_event_queue_pop(&q,&out) && out.type==INPUT_EVENT_INPUT_LOST);
        assert(!gui_event_queue_pop(&q,&out));
        assert(gui_event_queue_push(&q,&up)==1);
        assert(gui_event_queue_pop(&q,&out) && out.type==INPUT_EVENT_MOUSE_BUTTON_UP);
        // Focus eviction also latches loss, while remaining control records
        // retain their order through wrapped ring compaction.
        input_event_t focus={.type=INPUT_EVENT_WINDOW_FOCUS};
        for(unsigned i=0;i<GUI_WINDOW_EVENTS_MAX-1;++i){
            input_event_t e=(i%2)?move:focus;e.focus.other_handle=(uint16_t)i;
            assert(gui_event_queue_push(&q,&e)==1);
        }
        focus.focus.other_handle=100;
        assert(gui_event_queue_push(&q,&focus)==2);
        assert(gui_event_queue_pop(&q,&out) && out.type==INPUT_EVENT_INPUT_LOST);
        for(unsigned i=2;i<GUI_WINDOW_EVENTS_MAX-1;i+=2)
            assert(gui_event_queue_pop(&q,&out) && out.type==INPUT_EVENT_WINDOW_FOCUS && out.focus.other_handle==i);
        assert(gui_event_queue_pop(&q,&out) && out.focus.other_handle==100);
        assert(!gui_event_queue_pending(&q));
    }
    puts("gui queue: 64 wrap positions; drop/eviction reset and retained control order passed");
    return 0;
}
