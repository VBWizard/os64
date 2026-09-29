#include "os64/os64.h"

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; os64_printf("windowfocustest: FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static unsigned focus_events(int64_t win)
{
    unsigned n=0; os64_gui_event_t ev;
    while(os64_gui_event_poll(win,&ev)==1)
        if(ev.type==OS64_GUI_EVENT_WINDOW_FOCUS)++n;
    return n;
}
int main(int argc,char **argv)
{
    if(argc==3 && os64_streq(argv[1],"--foreign")){
        int64_t win=0;
        for(const char *p=argv[2];*p;++p)win=win*10+(*p-'0');
        return os64_gui_window_focus(win)==OS64_GUI_ERR_NOT_OWNER?0:1;
    }
    uint32_t w,h;
    if(os64_gui_screen_info(&w,&h)<0){os64_printf("windowfocustest: SKIP (GUI required)\n");return 0;}
    int64_t first=os64_gui_window_create("Focus contract",40,40,240,160,0);
    if(argc==2 && os64_streq(argv[1],"--restore")){
        CHECK(first>0);
        os64_gui_window_state_t state={0};bool minimized=false;
        // The QEMU driver sends Ctrl+Alt+N while this window owns focus.
        for(unsigned i=0;i<150 && !minimized;++i){
            os64_sleep(100);
            if(os64_gui_window_get_state(first,&state)==0)
                minimized=(state.flags&OS64_GUI_WINDOW_MINIMIZED)!=0;
        }
        CHECK(minimized);
        CHECK(os64_gui_window_focus(first)==0);
        CHECK(os64_gui_window_get_state(first,&state)==0 &&
              !(state.flags&OS64_GUI_WINDOW_MINIMIZED));
        os64_gui_window_destroy(first);
        os64_printf("windowfocustest restore: %u checks, %u failures\n",checks,failures);
        return failures?1:0;
    }
    int64_t second=os64_gui_window_create("Other window",80,80,240,160,0);
    CHECK(first>0 && second>0);
    if(first<=0 || second<=0)return 1;
    focus_events(first);focus_events(second);
    CHECK(os64_gui_window_focus(first)==0);
    CHECK(focus_events(first)==1 && focus_events(second)==1);
    CHECK(os64_gui_window_focus(first)==0 && focus_events(first)==0);
    CHECK(os64_gui_window_focus(0)==OS64_GUI_ERR_INVALID_HANDLE);
    char number[24];os64_snprintf(number,sizeof(number),"%ld",first);
    char *args[]={"/tests/windowfocustest","--foreign",number,NULL};
    int64_t child=os64_spawn(args[0],args);int32_t code=-1;
    CHECK(child>0 && os64_wait(child,&code)==child && code==0);
    uint32_t flags[]={OS64_GUI_WINDOW_DESKTOP,OS64_GUI_WINDOW_NO_DECORATIONS|OS64_GUI_WINDOW_PINNED};
    for(unsigned i=0;i<2;++i){
        int64_t special=os64_gui_window_create("Nonfocusable",0,0,160,100,flags[i]);
        CHECK(special>0);
        if(special>0){CHECK(os64_gui_window_focus(special)==OS64_GUI_ERR_BAD_ARGS);os64_gui_window_destroy(special);}
    }
    os64_gui_window_destroy(second);os64_gui_window_destroy(first);
    CHECK(os64_gui_window_focus(first)==OS64_GUI_ERR_INVALID_HANDLE);
    os64_printf("windowfocustest: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
