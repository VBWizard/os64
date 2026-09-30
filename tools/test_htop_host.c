// Model, terminal drawing and real event-loop contracts with synthetic OS data.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/apps/htop/htop.h"
#include "os64/signal.h"

static os64_monitor_task_t tasks[HT_TASKS];
static os64_monitor_thread_t threads[HT_THREADS];
static os64_monitor_core_t cores[HT_CORES];
static os64_monitor_snapshot_t snapshot;
static ht_view_t view;
static ht_cell_t grid[HT_WIDTH * HT_HEIGHT];
static unsigned writes, closes, allocations, live, reads, samples, raw_calls;
static bool fail_open, short_write, fail_close, raw_mode, run_loop;
static char verb[16], path_written[80], output_text[2 * 1024 * 1024];
static size_t output_used;
static uint64_t milliseconds;
static unsigned tty_cols=80, tty_rows=25;
static const char *scheduled;
static uint64_t key_time[128];
static size_t key_pos, key_count;

void *os64_malloc(size_t n) { void *p=malloc(n);if(p){allocations++;live++;}return p; }
void os64_free(void *p) {if(p){assert(live);live--;free(p);} }
int64_t os64_write(int32_t h,const void *p,size_t n)
{
    if(h==7){writes++;assert(n<sizeof(verb));memcpy(verb,p,n);verb[n]=0;return short_write?(int64_t)n-1:(int64_t)n;}
    assert(h==OS64_STDOUT || h==OS64_STDERR);
    assert(output_used+n<sizeof(output_text));memcpy(output_text+output_used,p,n);output_used+=n;output_text[output_used]=0;return (int64_t)n;
}
int64_t os64_open(const char *p,const char *mode)
{assert(mode && !strcmp(mode,"w"));snprintf(path_written,sizeof(path_written),"%s",p);return fail_open?-1:7;}
int64_t os64_close(int32_t h) {assert(h==7 || h==3);closes++;return fail_close && h==7?-1:0;}
int32_t os64_proc_read(uint64_t pid,os64_proc_info_t *out)
{
    reads++;
    for(size_t i=0;i<snapshot.task_count;i++)if(tasks[i].info.pid==pid){*out=tasks[i].info;return 0;}
    return -1;
}
int32_t os64_proc_command(uint64_t pid,char **out)
{
    os64_proc_info_t p;if(os64_proc_read(pid,&p)<0){*out=NULL;return -1;}
    size_t n=strlen(p.command)+1;*out=os64_malloc(n);if(!*out)return -1;memcpy(*out,p.command,n);return 0;
}
char os64_proc_state_letter(os64_proc_state_t state) {return state==OS64_PROC_RUNNING?'R':state==OS64_PROC_ZOMBIE?'Z':'I';}
const char *os64_proc_state_name(os64_proc_state_t state) {return state==OS64_PROC_RUNNING?"running":state==OS64_PROC_ZOMBIE?"zombie":"isleep";}
static void setup(void)
{
    assert(!live);memset(tasks,0,sizeof(tasks));memset(threads,0,sizeof(threads));memset(cores,0,sizeof(cores));
    tasks[0].info=(os64_proc_info_t){.pid=10,.threads=2,.name="worker",.command="worker --serve",.state=OS64_PROC_RUNNING};
    tasks[1].info=(os64_proc_info_t){.pid=20,.ppid=10,.threads=1,.name="child",.command="child",.state=OS64_PROC_ISLEEP};
    tasks[2].info=(os64_proc_info_t){.pid=30,.kernel=true,.name="idle0",.command="idle0",.state=OS64_PROC_RUNNING};
    tasks[3].info=(os64_proc_info_t){.pid=40,.name="ended",.command="ended",.state=OS64_PROC_ZOMBIE};
    for(unsigned i=0;i<4;i++)tasks[i].cpu=(os64_monitor_usage_t){1000-i*100,10-i,true};
    threads[0].info=(os64_thread_info_t){.pid=10,.tid=20};threads[1].info=(os64_thread_info_t){.pid=10,.tid=21};
    for(unsigned i=0;i<12;i++){cores[i].id=i;cores[i].valid=true;cores[i].percent[0]=i;cores[i].percent[1]=100-i;}
    snapshot=(os64_monitor_snapshot_t){.tasks=tasks,.threads=threads,.cores=cores,.task_count=4,.thread_count=2,.core_count=12,
        .zombies=1,.memory_valid=true,.summary_valid=true,.ticks={1000,100},.machine_percent={20,75,5},
        .memory={.total=1000,.usable=900,.used=500,.free=400,.available=400}};
    writes=closes=reads=samples=raw_calls=allocations=0;fail_open=short_write=fail_close=raw_mode=run_loop=false;
    verb[0]=path_written[0]=0;output_used=0;output_text[0]=0;milliseconds=0;
    tty_cols=80;tty_rows=25;scheduled="q";key_count=1;key_pos=0;key_time[0]=200;
    ht_init(&view,999);ht_rebuild(&view,&snapshot,true);
}
static void navigation(void)
{
    setup();assert(view.count==2 && view.identity.pid==10);
    ht_key(&view,HT_DOWN);assert(view.identity.pid==20);
    tasks[1].cpu.delta_us=2000;ht_rebuild(&view,&snapshot,true);
    assert(view.selected==0 && view.identity.pid==20);
    ht_key(&view,'s');ht_key(&view,'S');assert(view.identity.pid==20);
    ht_key(&view,'/');for(const char *p="worker";*p;p++)ht_key(&view,*p);ht_key(&view,'\n');
    assert(view.count==1 && view.identity.pid==10);ht_key(&view,27);assert(view.count==2);
    ht_key(&view,'i');ht_key(&view,'z');assert(view.count==4);
    ht_key(&view,'t');assert(view.count==6);
    bool task20=false,thread20=false;
    for(size_t i=0;i<view.count;i++){
        view.selected=i;ht_key(&view,HT_UP);ht_key(&view,HT_DOWN);
        // Test identities directly via selection movement, including equal IDs.
        ht_key(&view,HT_HOME);for(size_t j=0;j<i;j++)ht_key(&view,HT_DOWN);
        if(view.identity.is_thread && view.identity.tid==20)thread20=true;
        if(!view.identity.is_thread && view.identity.pid==20)task20=true;
    }
    assert(task20 && thread20);
    ht_key(&view,HT_END);assert(view.selected==view.count-1);ht_key(&view,HT_PGUP);assert(view.selected<view.count);
    ht_key(&view,' ');assert(view.paused);ht_key(&view,' ');assert(!view.paused);
}
static void tree_and_disappearance(void)
{
    setup();ht_key(&view,'v');assert(view.count==2);
    assert(tasks[view.rows[0].task].info.pid==10 && view.rows[1].depth==1);
    tasks[0].info.ppid=20;ht_rebuild(&view,&snapshot,false);assert(view.count==2); // parent cycle
    tasks[1].info.ppid=777;ht_rebuild(&view,&snapshot,false);assert(view.count==2); // orphan
    ht_key(&view,HT_HOME);assert(ht_key(&view,'\n')==HT_LOAD_DETAILS);
    uint64_t selected=view.identity.pid;
    for(size_t i=0;i<snapshot.task_count;i++)if(tasks[i].info.pid==selected)tasks[i].info.state=OS64_PROC_ZOMBIE;
    ht_rebuild(&view,&snapshot,true);assert(view.mode==HT_LIST && view.identity.pid!=selected);
    ht_rebuild(&view,NULL,false);assert(!view.count && !view.identity.valid);
}
static void actions(void)
{
    setup();ht_key(&view,'X');assert(view.mode==HT_CONFIRM && view.action_pid==10);
    assert(ht_key(&view,'y')==0 && view.mode==HT_CONFIRM && !writes);
    tasks[1].cpu.delta_us=99999;ht_rebuild(&view,&snapshot,true);
    view.confirm_drawn=true;assert(ht_key(&view,'y')==HT_SEND_SIGNAL);
    assert(ht_send_signal(&view)==0 && writes==1 && !strcmp(path_written,"/proc/10/ctl") && !strcmp(verb,"kill"));
    view.action_kill=false;assert(!ht_send_signal(&view) && !strcmp(verb,"interrupt"));
    unsigned before=writes;tasks[0].info.kernel=true;assert(ht_send_signal(&view)<0 && writes==before);
    tasks[0].info.kernel=false;tasks[0].info.state=OS64_PROC_ZOMBIE;assert(ht_send_signal(&view)<0 && writes==before);
    tasks[0].info.state=OS64_PROC_RUNNING;strcpy(tasks[0].info.name,"new-program");assert(ht_send_signal(&view)<0 && writes==before);
    strcpy(tasks[0].info.name,"worker");view.self=10;assert(ht_send_signal(&view)<0 && writes==before);view.self=999;
    short_write=true;assert(ht_send_signal(&view)<0 && writes==before+1);short_write=false;
    fail_close=true;assert(ht_send_signal(&view)<0);fail_close=false;
    fail_open=true;before=writes;assert(ht_send_signal(&view)<0 && writes==before);fail_open=false;
    ht_key(&view,'x');assert(view.mode==HT_CONFIRM);tasks[0].info.state=OS64_PROC_ZOMBIE;
    ht_rebuild(&view,&snapshot,true);assert(view.mode==HT_LIST);
}
static void drawing(void)
{
    setup();strcpy(tasks[0].info.command,"worker\033[2J\nspoof");
    unsigned widths[]={0,1,20,59,60,80,100,240,1000}, heights[]={0,1,12,18,25,48,100,200};
    for(size_t i=0;i<sizeof(widths)/sizeof(widths[0]);i++)for(size_t j=0;j<sizeof(heights)/sizeof(heights[0]);j++)
        for(unsigned mode=HT_LIST;mode<=HT_CONFIRM;mode++) {
            ht_layout(&view,widths[i],heights[j]);view.mode=(ht_mode_t)mode;ht_draw(&view,grid);
            for(unsigned y=0;y<HT_HEIGHT;y++)for(unsigned x=0;x<HT_WIDTH;x++) {
                ht_cell_t c=grid[y*HT_WIDTH+x];assert(c.ch>=32 && c.ch<=126 && c.style<HT_STYLE_COUNT);
                if(x>=view.cols || y>=view.lines)assert(c.ch==' ');
            }
        }
    view.mode=HT_LIST;ht_layout(&view,80,18);view.core_page=0;
    ht_key(&view,']');assert(view.core_page==1);ht_draw(&view,grid);ht_key(&view,']');assert(view.core_page==2);
    ht_layout(&view,80,25);ht_draw(&view,grid);assert(view.core_page==0); // narrower bank is now out of range
    ht_layout(&view,80,25);view.mode=HT_DETAILS;view.command="A\033[31mB\nC";view.detail_line=1000000;
    ht_draw(&view,grid);assert(view.detail_line==0);
    snapshot.memory.used=UINT64_MAX;snapshot.memory.usable=UINT64_MAX;view.mode=HT_LIST;ht_draw(&view,grid);
}
static void decoding(void)
{
    ht_input_t d={.pending=-1};assert(ht_decode(&d,27)==0);assert(ht_decode(&d,'[')==0);assert(ht_decode(&d,'A')==HT_UP);
    assert(ht_decode(&d,27)==0 && ht_escape_timeout(&d)==27);
    ht_decode(&d,27);assert(ht_decode(&d,'q')==27 && d.pending=='q');d.pending=-1;
    const char *seq="\033[6~";int key=0;while(*seq)key=ht_decode(&d,(unsigned char)*seq++);assert(key==HT_PGDN);
    seq="\033[1;5D";while(*seq)key=ht_decode(&d,(unsigned char)*seq++);assert(key==HT_LEFT);
    seq="\033OP";while(*seq)key=ht_decode(&d,(unsigned char)*seq++);assert(key=='?');
    ht_decode(&d,27);ht_decode(&d,'[');for(unsigned i=0;i<100;i++)assert(!ht_decode(&d,'9'));assert(!ht_decode(&d,'A'));
    ht_decode(&d,27);ht_decode(&d,'[');assert(!ht_escape_timeout(&d));assert(ht_decode(&d,'q')=='q');
}

// Run the actual event loop with deterministic time and terminal I/O. Keep
// signal registration on the host side instead of issuing a guest syscall.
static int64_t host_signal(int signo,os64_signal_fn fn) {(void)signo;(void)fn;return 0;}
#define os64_signal_set_handler host_signal
#define main htop_main
#include "../userland/apps/htop/htop.c"
#undef main
#undef os64_signal_set_handler
int64_t os64_micros(void) {return (int64_t)milliseconds*1000;}
int64_t os64_ticks(os64_ticks_t *out) {*out=(os64_ticks_t){milliseconds/10,100};return 0;}
uint64_t os64_taskid(void) {return 999;}
int64_t os64_tty_handle(void) {return 3;}
int32_t os64_tty_read(os64_tty_info_t *out) {*out=(os64_tty_info_t){.cols=tty_cols,.rows=tty_rows,.live=true,.fg_task=999};return 0;}
int32_t os64_tty_mode(bool *raw,uint64_t *owner) {if(raw)*raw=raw_mode;if(owner)*owner=0;return 0;}
int32_t os64_tty_set_raw(bool raw) {raw_mode=raw;raw_calls++;return 0;}
int64_t os64_read_for(int32_t h,void *out,size_t cap,uint64_t timeout)
{
    assert(h==3 && run_loop);milliseconds+=timeout;
    if(key_pos==key_count){assert(milliseconds<10000);return OS64_ERR_TIMEOUT;}
    size_t n=0;while(key_pos<key_count && n<cap && milliseconds>=key_time[key_pos])((char*)out)[n++]=scheduled[key_pos++];
    return n?(int64_t)n:OS64_ERR_TIMEOUT;
}
os64_monitor_t *os64_monitor_create(size_t a,size_t b,size_t c) {(void)a;(void)b;(void)c;return os64_malloc(1);}
void os64_monitor_destroy(os64_monitor_t *m) {os64_free(m);}
int32_t os64_monitor_sample(os64_monitor_t *m,bool th,const os64_monitor_snapshot_t **out)
{
    assert(m);samples++;snapshot.sequence=samples;snapshot.threads_sampled=th;snapshot.thread_count=th?2:0;
    *out=&snapshot;return 0;
}
static void loop_test(const char *keys,const uint64_t *times,size_t n,unsigned expected_writes,int expected_status)
{
    setup();scheduled=keys;key_count=n;for(size_t i=0;i<n;i++)key_time[i]=times[i];
    run_loop=true;char *argv[]={"htop",NULL};assert(htop_main(1,argv)==expected_status);
    assert(!live && !raw_mode && raw_calls==2 && writes==expected_writes && samples>=1);
    assert(strstr(output_text,"htop / os64") && strstr(output_text,"\033[0m\033[2J\033[H"));
}
int main(void)
{
    navigation();tree_and_disappearance();actions();drawing();decoding();
    uint64_t queued[]={200,200,500,700};loop_test("Xynq",queued,4,0,0);
    uint64_t accepted[]={200,500,700};loop_test("Xyq",accepted,3,1,0);
    uint64_t interactive[]={200,300,400,500,600,700,800,900,1200,1500,1700,1900};
    loop_test("/work\n\033st q",interactive,12,0,0);
    uint64_t details_keys[]={200,300,400,500};loop_test("\njqq",details_keys,4,0,0);
    uint64_t interrupt_key[]={200};loop_test("\003",interrupt_key,1,0,130);
    puts("htop: model, identities, tree cycles, drawing bounds, input, actions and real event loop PASS");
}
