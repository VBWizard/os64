/* Native-window contract fixture; run in an isolated GUI guest. */
#include "os64/os64.h"
static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; os64_printf("popuptest: FAIL %d: %s\n", __LINE__, #c); } } while (0)
static void drain(int64_t w)
{ os64_gui_event_t e; while (os64_gui_event_poll(w,&e)==1) {} }
static bool focus_edge(int64_t w, bool gained, int64_t other)
{
    os64_gui_event_t e; bool found=false;
    while(os64_gui_event_poll(w,&e)==1)
        if(e.type==OS64_GUI_EVENT_WINDOW_FOCUS && e.focus.gained==gained &&
           e.focus.sibling && e.focus.other_handle==other)found=true;
    return found;
}
static int64_t create_owner(void)
{ return os64_gui_window_create("Popup contract",30,30,250,180,0); }
static int64_t number(const char *s)
{ int64_t n=0;while(*s)n=n*10+(*s++-'0');return n; }
typedef struct {
    int64_t first, second, target;
    uint32_t mask;
    bool any, entered, done, saw_noise, require_noise;
    int64_t rc, ready;
} wait_job;

// Readiness names a queue, not a particular notification. Consume unrelated
// GUI events and check the target handle and doorbell mask together.
static int64_t wait_for_bell(wait_job *j)
{
    bool received=false;
    for (;;) {
        int64_t ready=j->first;
        int64_t rc=j->any?os64_gui_event_wait_any(&ready):os64_gui_event_wait(ready,NULL);
        if(rc!=1)return rc;
        if(ready!=j->first && ready!=j->second){
            os64_printf("popuptest: wait returned unexpected handle %ld\n",ready);
            return -100;
        }
        os64_gui_event_t e;
        rc=os64_gui_event_poll(ready,&e);
        if(rc!=1)return rc;
        if(e.type!=OS64_GUI_EVENT_DOORBELL)continue;
        if(ready==j->first && (e.doorbell.mask&0x40))j->saw_noise=true;
        if(ready==j->target && (e.doorbell.mask&j->mask)){
            j->ready=ready;
            received=true;
        }
        if(received && (!j->require_noise || j->saw_noise))return 1;
    }
}
static int64_t wait_worker(void *arg)
{
    wait_job *j=arg;
    __atomic_store_n(&j->entered,true,__ATOMIC_RELEASE);
    j->rc=wait_for_bell(j);
    __atomic_store_n(&j->done,true,__ATOMIC_RELEASE);
    return 0;
}

// Bound these blocking regressions even if a notification never arrives.
// This guard is scoped to waits(); it must not compete for the signal-handler
// delivery exercised later by teardown_wait().
static int64_t wait_watchdog(void *arg)
{
    wait_job *j=arg;
    for(unsigned i=0;i<500;++i){
        if(__atomic_load_n(&j->done,__ATOMIC_ACQUIRE))return 0;
        os64_sleep(10);
    }
    os64_printf("popuptest: event-wait watchdog expired\n");
    os64_gui_window_destroy(j->second);
    os64_gui_window_destroy(j->first);
    return -1;
}
static int64_t wake_worker(void *arg)
{
    wait_job *j=arg;os64_sleep(150);
    return os64_gui_event_ring(j->second,0x80);
}
static void join(int64_t t)
{
    int64_t rc=-1;
    CHECK(t>=0);
    if(t>=0){CHECK(os64_thread_join((int32_t)t,&rc)==0 && rc==0);CHECK(os64_close((int32_t)t)==0);}
}
static void waits(int64_t a,int64_t b)
{
    drain(a);drain(b);
    int64_t ready=0;
    CHECK(os64_gui_event_wait_any(NULL)==OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_event_wait_any((int64_t *)1)==OS64_GUI_ERR_BAD_ARGS);
    wait_job guard={.first=a,.second=b};
    int64_t watchdog=os64_thread(wait_watchdog,&guard);
    CHECK(watchdog>=0);if(watchdog<0)return;
    CHECK(os64_gui_event_ring(a,1)==0 && os64_gui_event_ring(b,2)==0);
    CHECK(os64_gui_event_wait_any(&ready)==1);int64_t first=ready;
    CHECK(first==a || first==b);
    CHECK(os64_gui_event_wait_any(&ready)==1 && ready!=first);
    wait_job first_bell={.first=a,.second=b,.target=a,.mask=1};
    wait_job second_bell={.first=b,.second=a,.target=b,.mask=2};
    CHECK(wait_for_bell(&first_bell)==1);
    CHECK(wait_for_bell(&second_bell)==1);
    wait_job wake={.first=a,.second=b,.target=b,.mask=0x80,.any=true,.require_noise=true};
    int64_t t=os64_thread(wake_worker,&wake);
    if(t<0){CHECK(false);goto finish;}
    // A deterministic unrelated notification reproduces the same condition as
    // delayed pointer/visibility events, without depending on compositor timing.
    CHECK(os64_gui_event_ring(a,0x40)==0);
    CHECK(wait_for_bell(&wake)==1 && wake.ready==b && wake.saw_noise);
    join(t);drain(a);drain(b);
    // A single-window waiter and an any-window waiter must not replace one
    // another. A ready second window keeps the probe bounded if startup races.
    wait_job j={.first=a,.second=b,.target=a,.mask=8};
    t=os64_thread(wait_worker,&j);
    if(t<0){CHECK(false);goto finish;}
    while(!__atomic_load_n(&j.entered,__ATOMIC_ACQUIRE))os64_sleep(1);
    int64_t rc=0;
    for(unsigned i=0;i<100 && rc!=OS64_GUI_ERR_BUSY;++i){
        CHECK(os64_gui_event_ring(b,4)==0);
        rc=os64_gui_event_wait_any(&ready);os64_sleep(5);
    }
    CHECK(rc==OS64_GUI_ERR_BUSY);
    CHECK(os64_gui_event_ring(a,8)==0);join(t);CHECK(j.rc==1);
    drain(a);drain(b);
    rc=0;
    for(unsigned attempt=0;attempt<5 && rc!=OS64_GUI_ERR_BUSY;++attempt){
        j=(wait_job){.first=a,.second=b,.target=a,.mask=0x80,.any=true};
        t=os64_thread(wait_worker,&j);
        if(t<0){CHECK(false);goto finish;}
        while(!__atomic_load_n(&j.entered,__ATOMIC_ACQUIRE))os64_sleep(1);
        os64_sleep(10);
        // Entered is not proof of registration. A delayed bell releases the
        // winner even if our probe wins the waiter slot between worker calls.
        // Retry that scheduling outcome; still require an observed BUSY.
        wait_job release={.second=a};
        int64_t waker=os64_thread(wake_worker,&release);
        CHECK(waker>=0);
        if(waker<0){
            os64_gui_event_ring(a,0x80);join(t);goto finish;
        }
        rc=os64_gui_event_wait(a,NULL);
        CHECK(rc==1 || rc==OS64_GUI_ERR_BUSY);
        join(waker);join(t);
        CHECK((j.rc==1 && j.ready==a) ||
              (rc==1 && j.rc==OS64_GUI_ERR_BUSY));
        drain(a);drain(b);
    }
    CHECK(rc==OS64_GUI_ERR_BUSY);
finish:
    __atomic_store_n(&guard.done,true,__ATOMIC_RELEASE);
    join(watchdog);
}

// Signal/teardown tests likewise allow ordinary readiness before their terminal
// result. Draining one queued event per pass keeps waiting on the actual syscall.
static int64_t wait_until_terminal(void)
{
    for (;;) {
        int64_t ready=0;
        int64_t rc=os64_gui_event_wait_any(&ready);
        if(rc!=1)return rc;
        os64_gui_event_t e;
        rc=os64_gui_event_poll(ready,&e);
        if(rc<0 && rc!=OS64_GUI_ERR_INVALID_HANDLE)return rc;
    }
}
static volatile unsigned caught;
static void interrupt_handler(int signo) { caught=(unsigned)signo; }
static int interrupt_task(int64_t target)
{
    os64_sleep(150);
    char path[64];os64_snprintf(path,sizeof(path),"/proc/%ld/ctl",target);
    int64_t fd=os64_open(path,"w");if(fd<0)return -1;
    int64_t n=os64_write((int32_t)fd,"interrupt",9);os64_close((int32_t)fd);
    return n==9?0:-1;
}
static int64_t destroy_worker(void *arg)
{ os64_sleep(150);return os64_gui_window_destroy(*(int64_t *)arg); }
static void teardown_wait(void)
{
    int64_t w=create_owner();CHECK(w>0);if(w<=0)return;
    drain(w);
    CHECK(os64_signal_set_handler(OS64_SIGINT,interrupt_handler)>=0);
    // Signal delivery is task-wide: a sibling sender can claim the handler
    // itself. Use another process so the parked thread must handle the signal.
    char id[24];os64_snprintf(id,sizeof(id),"%ld",os64_taskid());
    char *args[]={"/tests/popuptest","--interrupt",id,NULL};
    int64_t sender=os64_spawn(args[0],args);CHECK(sender>0);
    if(sender>0){
        CHECK(wait_until_terminal()==OS64_GUI_ERR_INTERRUPTED);
        int32_t code=-1;CHECK(os64_wait(sender,&code)==sender && code==0);
        CHECK(caught==OS64_SIGINT);
    }
    CHECK(os64_signal_set_handler(OS64_SIGINT,OS64_SIG_DEFAULT)>=0);
    drain(w);
    int64_t worker=os64_thread(destroy_worker,&w);CHECK(worker>=0);
    if(worker>=0){CHECK(wait_until_terminal()==OS64_GUI_ERR_INVALID_HANDLE);join(worker);}
    else os64_gui_window_destroy(w);
}
static void full_table(void)
{
    for(unsigned pass=0;pass<2;++pass){
        int64_t windows[32],parent=0;unsigned n=0;
        for(;n<32;++n){
            int64_t next=os64_gui_popup_create("Capacity",parent,100,100,40,20);
            if(next<0){CHECK(next==OS64_GUI_ERR_NO_RESOURCES);break;}
            windows[n]=parent=next;
        }
        CHECK(n>0);
        if(n==32)CHECK(os64_gui_popup_create("Full",parent,100,100,40,20)==OS64_GUI_ERR_NO_RESOURCES);
        while(n)CHECK(os64_gui_window_destroy(windows[--n])==0);
    }
}
static void paint(int64_t w,uint32_t color,const char *text)
{
    os64_gui_surface_t s;if(os64_gui_window_get_surface(w,&s)<0)return;
    os64_draw_fill_rect(&s,(os64_gui_rect_t){0,0,(int32_t)s.width,(int32_t)s.height},color);
    os64_draw_text(&s,12,12,text,os64_strlen(text),0xfff5f5f5,color);
    os64_gui_window_publish(w,NULL);
}
static int show(void)
{
    int64_t b=os64_gui_window_create("External window",650,80,300,180,0);
    int64_t a=os64_gui_window_create("Popup owner",60,80,300,180,0);
    int64_t p=os64_gui_popup_create("First panel",a,300,160,180,100);
    int64_t c=os64_gui_popup_create("Second panel",p,460,210,180,100);
    if(a<=0 || b<=0 || p<=0 || c<=0)return 1;
    paint(a,0xff20384d,"Owner: active chrome");paint(b,0xff493d50,"Other: click or Alt+Tab");
    paint(p,0xff245d68,"Native popup");paint(c,0xff42673c,"Native child");
    os64_printf("show: owner=%ld other=%ld popup=%ld child=%ld; Escape closes deepest; Q exits\n",a,b,p,c);
    bool quit=false;
    while(!quit){
        int64_t ready;if(os64_gui_event_wait_any(&ready)!=1)break;
        os64_gui_event_t e;
        while(os64_gui_event_poll(ready,&e)==1){
            if(e.type==OS64_GUI_EVENT_WINDOW_FOCUS)
                os64_printf("focus: window=%ld gained=%u sibling=%u other=%u\n",ready,e.focus.gained,e.focus.sibling,e.focus.other_handle);
            if(e.type==OS64_GUI_EVENT_KEY_DOWN && (e.key.ascii=='q' || e.key.ascii=='Q'))quit=true;
            if(e.type==OS64_GUI_EVENT_KEY_DOWN && e.key.ascii==27){
                if(c>0){os64_gui_window_destroy(c);c=0;}
                else if(p>0){os64_gui_window_destroy(p);p=0;}
            }
            if(e.type==OS64_GUI_EVENT_WINDOW_CLOSE)quit=true;
        }
    }
    if(c>0)os64_gui_window_destroy(c);
    if(p>0)os64_gui_window_destroy(p);
    os64_gui_window_destroy(b);os64_gui_window_destroy(a);return 0;
}
int main(int argc,char **argv)
{
    if(argc==2 && os64_streq(argv[1],"--show"))return show();
    if(argc==3 && os64_streq(argv[1],"--interrupt"))return interrupt_task(number(argv[2]));
    if(argc==3 && os64_streq(argv[1],"--foreign")){
        int64_t w=number(argv[2]);os64_gui_rect_t rect;
        return os64_gui_popup_create("Foreign",w,0,0,50,20)==OS64_GUI_ERR_NOT_OWNER &&
               os64_gui_window_get_content_rect(w,&rect)==OS64_GUI_ERR_NOT_OWNER?0:1;
    }
    if(argc==2 && os64_streq(argv[1],"--exit-family")){
        int64_t w=create_owner();
        int64_t p=os64_gui_popup_create("Exit parent",w,240,60,50,20);
        int64_t c=os64_gui_popup_create("Exit child",p,280,80,50,20);
        return w>0 && p>0 && c>0 && os64_gui_window_destroy(w)==OS64_GUI_ERR_BUSY?0:1;
    }
    uint32_t sw,sh;
    if(os64_gui_screen_info(&sw,&sh)<0){os64_printf("popuptest: SKIP GUI required\n");return 0;}
    os64_printf("popuptest: automated window/focus/wait checks; windows will open and close\n");
    int64_t ready=-1;
    CHECK(os64_gui_event_wait_any(&ready)==OS64_GUI_ERR_INVALID_HANDLE);
    int64_t a=create_owner(),b=os64_gui_window_create("Other",350,30,200,140,0);
    CHECK(a>0 && b>0);if(a<=0 || b<=0)return 1;
    CHECK(os64_gui_popup_create("Unfocused",a,240,60,50,20)==OS64_GUI_ERR_NOT_FOCUSED);
    CHECK(os64_gui_popup_create("Invalid",0x10000,0,0,50,20)==OS64_GUI_ERR_INVALID_HANDLE);
    CHECK(os64_gui_window_focus(a)==0);drain(a);drain(b);
    os64_gui_rect_t ar;os64_gui_surface_t surface;
    CHECK(os64_gui_window_get_content_rect(a,&ar)==0);
    CHECK(os64_gui_window_get_surface(a,&surface)==0 && ar.w==(int32_t)surface.width && ar.h==(int32_t)surface.height);
    int64_t p=os64_gui_popup_create("Parent level",a,240,60,50,20);
    CHECK(p>0);if(p<=0)return 1;
    CHECK(focus_edge(a,false,p));CHECK(focus_edge(p,true,a));
    CHECK(os64_gui_window_get_surface(p,&surface)==0 && surface.width==50 && surface.height==20 && surface.pitch_px==50);
    CHECK(os64_gui_window_set_min_size(p,0,0)==0);
    CHECK(os64_gui_window_set_min_size(p,51,20)==OS64_GUI_ERR_BAD_ARGS);
    os64_gui_window_state_t state;
    CHECK(os64_gui_window_get_state(p,&state)==0);
    CHECK(os64_gui_window_get_content_rect(p,&ar)==0 && ar.x==state.x+1 && ar.y==state.y+1 && ar.w==50 && ar.h==20);
    CHECK(os64_gui_window_destroy(a)==OS64_GUI_ERR_BUSY);
    int64_t c=os64_gui_popup_create("Child level",p,280,80,70,30);
    CHECK(c>0);if(c<=0)return 1;
    CHECK(focus_edge(p,false,c));drain(c);
    CHECK(os64_gui_window_destroy(p)==OS64_GUI_ERR_BUSY);
    CHECK(os64_gui_window_destroy(c)==0 && focus_edge(p,true,c));
    CHECK(os64_gui_window_destroy(p)==0 && focus_edge(a,true,p));
    p=os64_gui_popup_create("Outside focus",a,240,60,50,20);CHECK(p>0);
    CHECK(os64_gui_window_focus(b)==0);drain(a);drain(b);
    CHECK(os64_gui_window_destroy(p)==0);
    // If destroy stole focus, a child of B would now be refused.
    p=os64_gui_popup_create("Still B",b,520,60,50,20);CHECK(p>0);
    CHECK(os64_gui_window_destroy(p)==0);
    char id[24];os64_snprintf(id,sizeof(id),"%ld",a);
    char *foreign[]={"/tests/popuptest","--foreign",id,NULL};
    int64_t task=os64_spawn(foreign[0],foreign);int32_t code=-1;
    CHECK(task>0 && os64_wait(task,&code)==task && code==0);
    for(unsigned i=0;i<12;++i){
        char *exit_args[]={"/tests/popuptest","--exit-family",NULL};
        task=os64_spawn(exit_args[0],exit_args);code=-1;
        CHECK(task>0 && os64_wait(task,&code)==task && code==0);
    }
    waits(a,b);
    CHECK(os64_gui_window_destroy(b)==0 && os64_gui_window_destroy(a)==0);
    CHECK(os64_gui_event_wait_any(&ready)==OS64_GUI_ERR_INVALID_HANDLE);
    teardown_wait();full_table();
    os64_printf("popuptest: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
