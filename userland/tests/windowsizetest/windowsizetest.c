// Exact content-size ABI probe. Temporary windows; no configuration writes.
#include "os64/os64.h"
#include "os64/gui.h"
static unsigned failures;
#define CHECK(x) do {if(!(x)){os64_complain("windowsizetest: line %u failed: %s",(unsigned)__LINE__,#x);++failures;}}while(0)
static unsigned resize_events(int64_t window)
{
    unsigned count=0;os64_gui_event_t e;
    while(os64_gui_event_poll(window,&e)==1){
        CHECK(e.type!=OS64_GUI_EVENT_WINDOW_PLACEMENT);
        if(e.type==OS64_GUI_EVENT_WINDOW_RESIZE)++count;
    }return count;
}
int main(int argc,char **argv)
{
    if(argc==3 && os64_streq(argv[1],"--foreign")){
        uint64_t handle;if(!os64_parse_u64(argv[2],&handle))return 2;
        return os64_gui_window_set_content_size((int64_t)handle,400,260)==OS64_GUI_ERR_NOT_OWNER?0:1;
    }
    uint32_t sw,sh;CHECK(!os64_gui_screen_info(&sw,&sh));
    int64_t w=os64_gui_window_create_content("Size test",40,40,400,260,OS64_GUI_WINDOW_START_UNFOCUSED|OS64_GUI_WINDOW_PINNED);
    if(w<=0){os64_complain("windowsizetest: desktop required (%ld)",w);return 1;}
    CHECK(!os64_gui_window_set_min_size(w,120,90));(void)resize_events(w);
    os64_gui_window_state_t before,after;CHECK(!os64_gui_window_get_state(w,&before));
    CHECK(!os64_gui_window_set_content_size(w,400,260));CHECK(!resize_events(w));
    CHECK(!os64_gui_window_set_content_size(w,450,300));
    os64_gui_rect_t r;CHECK(!os64_gui_window_get_content_rect(w,&r));CHECK(r.w==450 && r.h==300);
    CHECK(!os64_gui_window_get_state(w,&after));CHECK(after.x==before.x && after.y==before.y && after.flags==before.flags);
    CHECK(resize_events(w)==1);before=after;
    CHECK(os64_gui_window_set_content_size(w,100,80)==OS64_GUI_ERR_BELOW_MINIMUM);
    CHECK(os64_gui_window_set_content_size(w,sw+1,100)==OS64_GUI_ERR_ABOVE_CAPACITY);
    CHECK(os64_gui_window_set_content_size(w,sw,100)==OS64_GUI_ERR_SCREEN_FIT);
    CHECK((int64_t)os64_syscall3(SYSCALL_GUI_WINDOW_SET_CONTENT_SIZE,w,UINT64_C(1)<<32,100)==OS64_GUI_ERR_BAD_ARGS);
    CHECK(!os64_gui_window_get_state(w,&after));CHECK(!os64_memcmp(&before,&after,sizeof(before)));CHECK(!resize_events(w));
    char handle[24];os64_snprintf(handle,sizeof(handle),"%ld",w);
    char *const child_args[]={argv[0],"--foreign",handle,NULL};int64_t child=os64_spawn(argv[0],child_args);CHECK(child>0);
    if(child>0){int32_t status=-1;CHECK(os64_wait(child,&status)==child && status==0);}
    CHECK(!os64_gui_window_destroy(w));
    w=os64_gui_window_create_content("Size edge",(int32_t)sw-410,(int32_t)sh-300,400,260,OS64_GUI_WINDOW_START_UNFOCUSED);
    CHECK(w>0);
    if(w>0){
        CHECK(!os64_gui_window_set_content_size(w,500,350));CHECK(!os64_gui_window_get_state(w,&after));
        CHECK(after.x==(int32_t)(sw-after.width) && after.y==(int32_t)(sh-after.height));
        CHECK(!os64_gui_window_destroy(w));
    }
    w=os64_gui_window_create_content("Size maximized",40,40,400,260,OS64_GUI_CREATE_MAXIMIZED|OS64_GUI_WINDOW_START_UNFOCUSED);
    CHECK(w>0);
    if(w>0){
        CHECK(!os64_gui_window_get_state(w,&before));(void)resize_events(w);
        CHECK(!os64_gui_window_set_content_size(w,450,300));CHECK(!os64_gui_window_get_state(w,&after));
        CHECK(!os64_memcmp(&before,&after,sizeof(before)) && !resize_events(w));
        CHECK(!os64_gui_window_destroy(w));
    }
    w=os64_gui_popup_create("Size popup",0,40,40,180,90);CHECK(w>0);
    if(w>0){CHECK(os64_gui_window_set_content_size(w,180,90)==OS64_GUI_ERR_BAD_ARGS);CHECK(!os64_gui_window_destroy(w));}
    CHECK(os64_gui_window_set_content_size(-1,400,260)==OS64_GUI_ERR_INVALID_HANDLE);
    char verdict[80];os64_snprintf(verdict,sizeof(verdict),"windowsizetest: %u failures",failures);
    os64_debug_log(verdict);os64_printf("%s\n",verdict);return failures?1:0;
}
