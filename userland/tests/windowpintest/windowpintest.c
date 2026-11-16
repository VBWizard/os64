// Guest ABI probe: run with the desktop active. Windows are temporary.
#include "os64/os64.h"
#include "os64/gui.h"
static unsigned failures;
#define CHECK(x) do { if(!(x)){os64_hprintf(OS64_STDERR,"windowpintest: line %u failed: %s\n",(unsigned)__LINE__,#x);++failures;} } while(0)
int main(void)
{
    int64_t window=os64_gui_window_create_content("Pin test",40,40,180,80,OS64_GUI_WINDOW_START_UNFOCUSED);
    if(window<=0){os64_hprintf(OS64_STDERR,"windowpintest: desktop required (%ld)\n",window);return 1;}
    os64_gui_window_state_t state;
    CHECK(os64_gui_window_set_pinned(window,true)==0);
    CHECK(os64_gui_window_get_state(window,&state)==0 && (state.flags&OS64_GUI_WINDOW_PINNED));
    CHECK(os64_gui_window_set_pinned(window,false)==0);
    CHECK(os64_gui_window_get_state(window,&state)==0 && !(state.flags&OS64_GUI_WINDOW_PINNED));
    CHECK((int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_SET_PINNED,window,UINT64_C(1)<<32)==OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_pinned(-1,true)==OS64_GUI_ERR_INVALID_HANDLE);
    bool notified=false;os64_gui_event_t event;
    while(os64_gui_event_poll(window,&event)==1)
        if(event.type==OS64_GUI_EVENT_WINDOW_PIN_CHANGED)notified=true;
    CHECK(notified);
    int64_t popup=os64_gui_window_create_content("Pin popup probe",40,40,100,50,
        OS64_GUI_WINDOW_NO_DECORATIONS|OS64_GUI_WINDOW_PINNED|OS64_GUI_WINDOW_START_UNFOCUSED);
    CHECK(popup>0);
    if(popup>0){
        CHECK(os64_gui_window_set_pinned(popup,false)==OS64_GUI_ERR_BAD_ARGS);
        CHECK(os64_gui_window_get_state(popup,&state)==0 && (state.flags&OS64_GUI_WINDOW_PINNED));
        CHECK(os64_gui_window_destroy(popup)==0);
    }
    CHECK(os64_gui_window_destroy(window)==0);
    char verdict[80];os64_snprintf(verdict,sizeof(verdict),"windowpintest: %u failures",failures);
    os64_debug_log(verdict);os64_hprintf(OS64_STDOUT,"%s\n",verdict);
    return failures?1:0;
}
