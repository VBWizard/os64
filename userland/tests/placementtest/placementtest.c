// Interactive compositor evidence in an isolated GUI guest. Log is synced after
// each event so the host can inspect it. a/s select wait-any/single-window wait;
// d destroys and recreates mid-gesture; m raises the minimum; q exits. --fit
// grows the initial minimum like application layout during window creation.
#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

int main(int argc, char **argv)
{
    (void)argv;
    int64_t log=os64_open("/home/placement-events.log","w");
    if (log<0) return 1;
    int64_t win=0;
    bool any=true, done=false;
    unsigned generation=0;
    while (!done) {
        if (!win) {
            win=os64_gui_window_create_content("Placement evidence",
                120,100,500,300,OS64_GUI_CREATE_FIT_SCREEN);
            if (win<=0) {
                os64_hprintf((int32_t)log,"CREATE FAILED %ld\n",(long)win);
                break;
            }
            ++generation;
            if (argc>1) (void)os64_gui_window_set_min_size(win,550,320);
            os64_draw_ctx_t draw;
            if (os64_draw_ctx_init(&draw,win)==0) {
                os64_draw_fill_rect(&draw.surf,(os64_gui_rect_t){0,0,(int32_t)draw.surf.width,
                    (int32_t)draw.surf.height},0x00306090);
                os64_draw_publish(&draw,NULL);
            }
            os64_hprintf((int32_t)log,"CREATE %u %ld\n",generation,(long)win);
        }
        os64_hprintf((int32_t)log,"WAIT %s\n",any ? "any" : "single");
        (void)os64_sync((int32_t)log);
        os64_gui_event_t e;
        int64_t rc;
        if (any) {
            int64_t ready=0;
            rc=os64_gui_event_wait_any(&ready);
            if (rc==1) rc=os64_gui_event_poll(ready,&e);
        } else rc=os64_gui_event_wait(win,&e);
        if (rc!=1) { if (rc==OS64_INTERRUPTED) continue; break; }
        os64_hprintf((int32_t)log,"EVENT %u %u",generation,e.type);
        if (e.type==OS64_GUI_EVENT_WINDOW_PLACEMENT)
            os64_hprintf((int32_t)log," mask=%u x=%d y=%d w=%u h=%u",e.placement.changed,
                e.placement.x,e.placement.y,e.placement.width,e.placement.height);
        os64_hprintf((int32_t)log,"\n");
        if (e.type==OS64_GUI_EVENT_WINDOW_CLOSE) done=true;
        if (e.type==OS64_GUI_EVENT_KEY_DOWN) {
            if (e.key.ascii=='a') any=true;
            if (e.key.ascii=='s') any=false;
            if (e.key.ascii=='m') (void)os64_gui_window_set_min_size(win,650,400);
            if (e.key.ascii=='q') done=true;
            if (e.key.ascii=='d' && os64_gui_window_destroy(win)==0) win=0;
        }
    }
    if (win>0) (void)os64_gui_window_destroy(win);
    (void)os64_sync((int32_t)log);(void)os64_close((int32_t)log);
    return win<0 ? 1 : 0;
}
