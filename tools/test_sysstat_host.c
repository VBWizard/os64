// Exercise the real command and formatter against controlled clocks and samples.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/apps/sysstat/sysstat.h"

static char stdout_text[1024 * 1024], stderr_text[4096];
static size_t stdout_size, stderr_size, write_limit;
static uint64_t now_us, sample_end, oversleep, write_delay;
static unsigned calls, sleeps, live, fail_sample, slow_sample;
static bool bad_clock, backwards_clock, bad_sleep, bad_write, zero_write;
static bool interrupt_write, interrupt_sleep, partial, missing_task, single_thread, no_split;
static os64_monitor_task_t tasks[4];
static os64_monitor_thread_t threads[2];
static os64_monitor_core_t cores[SYSSTAT_CORES];
static os64_monitor_snapshot_t snapshot;

int64_t os64_write(int32_t h, const void *data, size_t n)
{
    assert(h == OS64_STDOUT || h == OS64_STDERR);
    if (h == OS64_STDOUT) {
        if (interrupt_write) { interrupt_write=false;return OS64_INTERRUPTED; }
        if (bad_write) return -1;
        if (zero_write) return 0;
        if (write_limit && n > write_limit) n=write_limit;
        assert(stdout_size+n<sizeof(stdout_text));
        memcpy(stdout_text+stdout_size,data,n);stdout_size+=n;stdout_text[stdout_size]=0;
        now_us+=write_delay;
    } else {
        assert(stderr_size+n<sizeof(stderr_text));
        memcpy(stderr_text+stderr_size,data,n);stderr_size+=n;stderr_text[stderr_size]=0;
    }
    return (int64_t)n;
}
int64_t os64_micros(void)
{
    if (bad_clock) return -1;
    if (backwards_clock && calls>=2) return 0;
    return (int64_t)now_us;
}
int64_t os64_sleep(uint64_t ms)
{
    sleeps++;
    if (bad_sleep) return -1;
    if (interrupt_sleep) {interrupt_sleep=false;now_us+=1000;return OS64_INTERRUPTED;}
    now_us+=ms*1000+oversleep;oversleep=0;
    return 0;
}
os64_monitor_t *os64_monitor_create(size_t task_cap,size_t thread_cap,size_t core_cap)
{
    assert(task_cap==SYSSTAT_TASKS && core_cap==SYSSTAT_CORES);
    assert(thread_cap==0 || thread_cap==SYSSTAT_THREADS);
    live++;return (os64_monitor_t *)malloc(1);
}
void os64_monitor_destroy(os64_monitor_t *m) {assert(live && m);live--;free(m);}
int32_t os64_monitor_sample(os64_monitor_t *m,bool th,const os64_monitor_snapshot_t **out)
{
    assert(m);calls++;
    if (calls==fail_sample) {*out=NULL;return -1;}
    now_us+=calls==slow_sample?2500000:2000;
    snapshot.interval_us=now_us-sample_end;sample_end=now_us;
    snapshot.tick_interval_us=snapshot.interval_us;
    snapshot.ledger_interval_us=snapshot.interval_us;
    snapshot.sequence=calls;
    snapshot.summary_valid=calls>1;
    snapshot.memory_valid=true;
    snapshot.memory.used=1000000+calls*4096;
    snapshot.memory.available=9000000-calls*4096;
    snapshot.threads_sampled=th;
    snapshot.thread_count=th&&!single_thread?2:0;
    tasks[0].info.threads=single_thread?1:2;
    for (unsigned i=0;i<4;i++) tasks[i].cpu.valid=calls>1;
    for (unsigned i=0;i<2;i++) threads[i].cpu.valid=calls>1;
    if (partial && calls==2) {
        snapshot.partial=7;snapshot.memory_valid=false;snapshot.summary_valid=false;
        cores[0].valid=false;
    } else {snapshot.partial=0;cores[0].valid=true;}
    if (missing_task && calls>=2) tasks[0].info.pid=777;
    *out=&snapshot;return 0;
}
static unsigned io_live, io_calls;
static bool fail_io_allocation, io_unavailable;
static os64_monitor_io_snapshot_t io_snapshot;
static os64_monitor_interface_t interface;
os64_monitor_io_t *os64_monitor_io_create(uint32_t sources, size_t capacity)
{
    assert(sources && capacity == 16);
    if (fail_io_allocation) return NULL;
    io_live++; io_snapshot.sources = sources;
    return (os64_monitor_io_t *)malloc(1);
}
void os64_monitor_io_destroy(os64_monitor_io_t *m)
{ if (m) { assert(io_live); io_live--; free(m); } }
int32_t os64_monitor_io_sample(os64_monitor_io_t *m, const os64_monitor_io_snapshot_t **out)
{
    assert(m); io_calls++;
    os64_monitor_io_status_t status = {.available = !io_unavailable, .clock_valid = !io_unavailable,
        .delta_valid = !io_unavailable && io_calls > 1, .rate_valid = !io_unavailable && io_calls > 1,
        .sampled_at_us = now_us, .interval_us = 1000000};
    interface = (os64_monitor_interface_t){.name = "eth0", .mac = "00:11:22:33:44:55", .link_up = true,
        .status = status, .total = {.rx_bytes = 5000}, .delta = {.rx_bytes = 1000}, .per_second = {.rx_bytes = 1000}};
    io_snapshot.interfaces = &interface; io_snapshot.interface_count = 1;
    io_snapshot.tcp = (os64_monitor_tcp_t){.status = status, .delta = {.retransmits = 2}, .per_second = {.retransmits = 2}};
    io_snapshot.cache = (os64_monitor_cache_t){.status = status, .enabled = true, .bytes = 4096, .capacity = 8192,
        .hit_percent_valid = !io_unavailable, .hit_percent = 80, .delta = {.hits = 8, .misses = 2},
        .per_second = {.hits = 8, .misses = 2}};
    io_snapshot.partial = io_unavailable ? 7 : 0;
    *out = &io_snapshot; return 0;
}
bool os64_monitor_machine_split(const os64_monitor_t *m,os64_monitor_core_split_t *out)
{
    assert(m);*out=(os64_monitor_core_split_t){.valid=!no_split,.percent={40,10,45,5}};return true;
}
bool os64_monitor_core_split(const os64_monitor_t *m,uint32_t id,os64_monitor_core_split_t *out)
{
    assert(m);*out=(os64_monitor_core_split_t){.valid=!no_split&&id==0,.percent={40,10,45,5}};return true;
}
bool os64_monitor_task_split(const os64_monitor_t *m,uint64_t id,os64_monitor_split_t *out)
{
    assert(m);
    *out=(os64_monitor_split_t){.idle=id==1,.user={.percent=100,.valid=!no_split},.system={.percent=50,.valid=!no_split}};
    return true;
}
bool os64_monitor_thread_split(const os64_monitor_t *m,uint64_t pid,uint64_t tid,os64_monitor_split_t *out)
{
    assert(m&&pid==42&&(tid==7||tid==8));
    *out=(os64_monitor_split_t){.user={.percent=50,.valid=!no_split},.system={.percent=25,.valid=!no_split}};
    return true;
}
bool os64_monitor_task_events(const os64_monitor_t *m,uint64_t pid,os64_monitor_task_events_t *out)
{
    assert(m&&pid==42);*out=(os64_monitor_task_events_t){.valid=calls>1,.switches=17,.minor_faults=2};return true;
}
const char *os64_proc_state_name(os64_proc_state_t state)
{ return state==OS64_PROC_ZOMBIE?"zombie":state==OS64_PROC_RUNNING?"running":"runnable"; }

#define main sysstat_main
#include "../userland/apps/sysstat/main.c"
#undef main

static void setup(void)
{
    assert(!live && !io_live);
    io_calls = 0; fail_io_allocation = io_unavailable = false;
    stdout_size=stderr_size=0;stdout_text[0]=stderr_text[0]=0;
    write_limit=write_delay=oversleep=sample_end=0;now_us=1000000;
    calls=sleeps=fail_sample=slow_sample=0;
    bad_clock=backwards_clock=bad_sleep=bad_write=zero_write=false;
    interrupt_write=interrupt_sleep=partial=missing_task=single_thread=no_split=false;
    memset(tasks,0,sizeof(tasks));memset(threads,0,sizeof(threads));memset(cores,0,sizeof(cores));
    tasks[0]=(os64_monitor_task_t){.info={.pid=42,.threads=2,.name="worker",.heap=16384,.state=OS64_PROC_RUNNING},.cpu={.percent=150,.delta_us=1500000}};
    tasks[1]=(os64_monitor_task_t){.info={.pid=8,.threads=1,.name="busy",.state=OS64_PROC_RUNNABLE},.cpu={.percent=200,.delta_us=2000000}};
    tasks[2]=(os64_monitor_task_t){.info={.pid=1,.threads=1,.name="idle0",.kernel=true,.state=OS64_PROC_RUNNING},.cpu={.percent=100,.delta_us=1000000}};
    tasks[3]=(os64_monitor_task_t){.info={.pid=99,.threads=1,.name="zombie",.state=OS64_PROC_ZOMBIE},.cpu={.percent=0,.delta_us=0}};
    for (unsigned i=0;i<2;i++) {
        threads[i]=(os64_monitor_thread_t){.info={.pid=42,.tid=7+i,.core=i?7:0,.state=OS64_PROC_RUNNABLE},.cpu={.percent=75,.delta_us=750000}};
        cores[i]=(os64_monitor_core_t){.id=i?7:0,.valid=true,.parked=i!=0,.percent={50,45,5}};
    }
    snapshot=(os64_monitor_snapshot_t){.tasks=tasks,.threads=threads,.cores=cores,.task_count=4,.core_count=2,
        .zombies=1,.parked=1,.machine_percent={50,45,5},.skew_valid=true,.skew_negative=true,.skew_tenths=12,
        .memory={.total=12000000,.usable=10000000,.largest_free_extent=4000000}};
}
static int run(char **argv)
{
    int argc=0;while(argv[argc])argc++;
    int rc=sysstat_main(argc,argv);assert(!live && !io_live);return rc;
}
static void save(const char *directory,const char *name)
{
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s.jsonl",directory,name);
    assert(n>0&&(size_t)n<sizeof(path));FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(stdout_text,1,stdout_size,f)==stdout_size);assert(!fclose(f));
}
static void parser(void)
{
    const char *bad[][3]={{"--count","0"},{"--count","-1"},{"--count","4294967296"},
        {"--count","18446744073709551616"},{"--interval-ms","99"},{"--interval-ms","60001"},
        {"--top","513"},{"--tid","abc"},{"--tid",""},{"--interval-ms","1.5"},{"--unknown",NULL},
        {"--count",NULL},{"extra",NULL}};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        setup();char *args[]={"sysstat",(char*)bad[i][0],(char*)bad[i][1],NULL};
        assert(run(args)==2 && !calls && !stdout_size && stderr_size);
    }
    setup();char *help[]={"sysstat","--help",NULL};assert(!run(help)&&!calls&&strstr(stdout_text,"--interval-ms"));
    setup();char *maximum[]={"sysstat","-n","4294967295","-i","60000","--tid","18446744073709551615",NULL};
    sysstat_options_t opt;assert(!options(7,maximum,&opt)&&opt.count==UINT32_MAX&&opt.task_id==UINT64_MAX);
}
static void commands(const char *directory)
{
    char *full[]={"sysstat","-n","3","-jc","--tid","42","--top","3",NULL};
    setup();strcpy(tasks[0].info.name,"worker\"\\\n\033 caf\xc3\xa9 \xff");
    write_limit=7;interrupt_write=interrupt_sleep=true;
    assert(!run(full)&&calls==4&&!stderr_size);save(directory,"full");
    setup();partial=true;assert(!run(full)&&calls==4);save(directory,"partial");
    setup();missing_task=true;assert(!run(full));save(directory,"missing");
    setup();single_thread=true;assert(!run(full));save(directory,"single");
    setup();no_split=true;assert(!run(full));save(directory,"nosplit");
    setup();oversleep=250000;assert(!run(full));save(directory,"late");
    setup();slow_sample=2;assert(!run(full));save(directory,"slow");
    char *simple[]={"sysstat","-n","2","-j",NULL};
    setup();write_delay=1500000;assert(!run(simple));save(directory,"slow_output");
    setup();char *text[]={"sysstat","-n","2","-c","--tid","42","--top","3",NULL};
    strcpy(tasks[0].info.name,"bad\033[2J\nname");
    assert(!run(text)&&!strchr(stdout_text,27)&&strstr(stdout_text,"Summary: 2/2")&&strstr(stdout_text,"thread 7"));
    setup();char *one[]={"sysstat","-n","1","-i","250","-j",NULL};
    assert(!run(one)&&calls==2&&sleeps==1);save(directory,"one");
    setup();partial=true;assert(!run(one));save(directory,"unavailable");
    setup();no_split=true;snapshot.machine_percent[0]=0;snapshot.machine_percent[1]=95;
    assert(!run(one));save(directory,"zero");
    setup();char *large[]={"sysstat","-n","1","-jc",NULL};
    snapshot.core_count=SYSSTAT_CORES;
    for (unsigned i=2;i<SYSSTAT_CORES;i++) cores[i].id=i+10;
    write_limit=13;assert(!run(large)&&stdout_size>4096);save(directory,"large");
    setup();char *io_args[]={"sysstat","-n","2","-j","--network","--cache",NULL};
    write_limit=11; assert(!run(io_args) && io_calls==3);save(directory,"io");
    setup();io_unavailable=true;assert(!run(io_args));save(directory,"io_unavailable");
    setup();fail_io_allocation=true;assert(run(io_args)==1 && !calls && !io_calls);
    setup();fail_sample=1;assert(run(io_args)==1 && !io_calls);
    setup();char *io_text[]={"sysstat","-n","1","-NC",NULL};
    assert(!run(io_text) && strstr(stdout_text,"rx_bytes=1000") && strstr(stdout_text,"hit_percent=80"));
    setup();char *defaults[]={"sysstat",NULL};assert(!run(defaults)&&calls==11&&!io_calls&&strstr(stdout_text,"Summary: 10/10"));
}
static void failures(const char *directory)
{
    char *args[]={"sysstat","-n","3","-j",NULL};
    setup();fail_sample=1;assert(run(args)==1&&calls==1&&!stdout_size);
    setup();fail_sample=3;assert(run(args)==1&&calls==3);save(directory,"failed");
    setup();bad_clock=true;assert(run(args)==1&&!stdout_size);
    setup();backwards_clock=true;assert(run(args)==1&&calls==2&&!stdout_size);
    setup();bad_sleep=true;assert(run(args)==1&&calls==1&&!stdout_size);
    setup();bad_write=true;assert(run(args)==1&&calls==2&&!stdout_size);
    setup();zero_write=true;assert(run(args)==1&&calls==2&&!stdout_size);
    setup();now_us=INT64_MAX-3000;assert(run(args)==1&&!stdout_size);
}
int main(int argc,char **argv)
{
    assert(argc==2);parser();commands(argv[1]);failures(argv[1]);
    puts("sysstat: options, timing, selected tasks, output, short writes and failures PASS");
    return 0;
}
