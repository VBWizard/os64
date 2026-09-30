// Public sampling contract against changing /proc and /sys fixtures.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/monitor.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/mem.h"

int64_t os64_write(int32_t h, const void *p, size_t n)
{ (void)h; (void)p; (void)n; assert(!"unexpected output"); return -1; }

static os64_proc_info_t tasks[4];
static os64_thread_info_t threads[4];
static os64_monitor_time_t cores[4];
static size_t ntasks, nthreads, ncores, proc_pos, core_pos, thread_pos, line_pos;
static uint32_t core_ids[4], reading_core;
static uint64_t thread_pid;
static os64_ticks_t clock_now;
static bool fail_proc, fail_directory_read, fail_clock, fail_memory, fail_core;
static bool malformed_core, duplicate_core, missing_core, fail_core_read, fail_core_close;
static bool reverse_order;
static unsigned allocations, fail_allocation, live_allocations, open_handles;

void *os64_malloc(size_t size)
{
    if (++allocations == fail_allocation) return NULL;
    void *p = malloc(size); if (p) live_allocations++; return p;
}
void os64_free(void *p) { if (p) { assert(live_allocations); live_allocations--; free(p); } }
int64_t os64_ticks(os64_ticks_t *out) { *out = clock_now; return fail_clock ? -1 : 0; }
int64_t os64_memory(os64_memory_t *out)
{ *out = (os64_memory_t){.total=1000,.usable=900,.free=400,.used=500,.available=400}; return fail_memory ? -1 : 0; }
int64_t os64_opendir(const char *path)
{
    if (!strcmp(path,"/proc")) {
        if (fail_proc) return -1;
        proc_pos=0; open_handles++; return 3;
    }
    if (!strcmp(path,"/sys/cpu")) { core_pos=0; open_handles++; return 4; }
    unsigned long pid; char tail;
    assert(sscanf(path,"/proc/%lu/thread%c",&pid,&tail)==1);
    thread_pid=pid; thread_pos=0; open_handles++; return 5;
}
int64_t os64_readdir(int32_t h, os64_dirent_t *out)
{
    memset(out,0,sizeof(*out));
    if (h==3) {
        if (proc_pos==ntasks) return fail_directory_read ? -1 : 0;
        size_t i=reverse_order ? ntasks-1-proc_pos : proc_pos;
        snprintf(out->name,sizeof(out->name),"%lu",tasks[i].pid); proc_pos++; return 1;
    }
    if (h==4) {
        if (core_pos==ncores) return 0;
        snprintf(out->name,sizeof(out->name),"%u",core_ids[core_pos++]); return 1;
    }
    assert(h==5);
    while (thread_pos<nthreads) {
        os64_thread_info_t *t=&threads[thread_pos++];
        if (t->pid==thread_pid) { snprintf(out->name,sizeof(out->name),"%lu",t->tid); return 1; }
    }
    return 0;
}
int32_t os64_proc_read(uint64_t pid, os64_proc_info_t *out)
{
    for (size_t i=0;i<ntasks;i++) if (tasks[i].pid==pid) {
        if (!tasks[i].name[0]) return -1; // exited after enumeration
        *out=tasks[i]; return 0;
    }
    return -1;
}
int32_t os64_proc_read_thread(uint64_t pid,uint64_t tid,os64_thread_info_t *out)
{
    for(size_t i=0;i<nthreads;i++) if(threads[i].pid==pid && threads[i].tid==tid) { *out=threads[i];return 0; }
    return -1;
}
int64_t os64_open(const char *path,const char *mode)
{
    (void)mode; unsigned id; char tail;
    assert(sscanf(path,"/sys/cpu/%u/time%c",&id,&tail)==1);
    if (fail_core && id==0) return -1;
    for(size_t i=0;i<ncores;i++) if(core_ids[i]==id) {reading_core=i;line_pos=0;open_handles++;return 6;}
    return -1;
}
int64_t os64_readline(int32_t h,char *out,size_t cap)
{
    assert(h==6);
    if(fail_core_read) return -1;
    os64_monitor_time_t *c=&cores[reading_core];
    // Deliberately reordered, with an unknown field between known ones.
    switch(line_pos++) {
    case 0: snprintf(out,cap,"idle_us: %lu",c->idle);return 1;
    case 1: snprintf(out,cap,"future: 777");return 1;
    case 2: snprintf(out,cap,"sched_us: %lu",c->sched);return 1;
    case 3: snprintf(out,cap,"busy_us: %lu%s",c->busy,malformed_core?"x":"");return 1;
    case 4: if(missing_core)return 0;snprintf(out,cap,"total_us: %lu",c->total);return 1;
    case 5: if(duplicate_core){snprintf(out,cap,"total_us: %lu",c->total);return 1;}return 0;
    default:return 0;
    }
}
int64_t os64_close(int32_t h)
{ assert(h>=3 && h<=6 && open_handles);open_handles--;return h==6 && fail_core_close ? -1 : 0; }

static void setup(void)
{
    assert(!live_allocations && !open_handles);
    memset(tasks,0,sizeof(tasks));memset(threads,0,sizeof(threads));memset(cores,0,sizeof(cores));
    tasks[0]=(os64_proc_info_t){.pid=1,.ppid=0,.runtime_us=500000,.threads=2,.state=OS64_PROC_RUNNING,.name="worker"};
    tasks[1]=(os64_proc_info_t){.pid=2,.ppid=1,.runtime_us=123,.threads=1,.state=OS64_PROC_ZOMBIE,.name="zombie"};
    threads[0]=(os64_thread_info_t){.pid=1,.tid=1,.runtime_us=200000};
    threads[1]=(os64_thread_info_t){.pid=1,.tid=2,.runtime_us=300000};
    cores[0]=(os64_monitor_time_t){1000000,500000,450000,50000};
    cores[1]=cores[0];core_ids[0]=0;core_ids[1]=7;
    ntasks=nthreads=ncores=2;clock_now=(os64_ticks_t){100,100};
    fail_proc=fail_directory_read=fail_clock=fail_memory=fail_core=false;
    malformed_core=duplicate_core=missing_core=fail_core_read=fail_core_close=reverse_order=false;
    allocations=fail_allocation=0;
}
static void advance(void)
{
    clock_now.ticks+=100;
    for(size_t i=0;i<ncores;i++) {
        cores[i].total+=1000000;cores[i].busy+=500000;cores[i].idle+=450000;cores[i].sched+=50000;
    }
    tasks[0].runtime_us+=1500000;
    threads[0].runtime_us+=500000;threads[1].runtime_us+=1000000;
}
static const os64_monitor_snapshot_t *sample(os64_monitor_t *m,bool th)
{
    const os64_monitor_snapshot_t *s=NULL;
    assert(os64_monitor_sample(m,th,&s)==0 && s && !open_handles);
    return s;
}
static const os64_monitor_task_t *task(const os64_monitor_snapshot_t *s,uint64_t pid)
{ for(size_t i=0;i<s->task_count;i++)if(s->tasks[i].info.pid==pid)return &s->tasks[i];assert(0);return NULL; }

static void independent_consumers(void)
{
    setup();os64_monitor_t *a=os64_monitor_create(4,4,4),*b=os64_monitor_create(4,4,4);assert(a && b);
    const os64_monitor_snapshot_t *s=sample(a,true),*held=sample(b,true);
    assert(!task(s,1)->cpu.valid && !s->summary_valid && s->zombies==1 && s->thread_count==2);
    advance();s=sample(a,true);
    assert(s->summary_valid && s->interval_us==1000000 && s->cores[1].id==7);
    assert(task(s,1)->cpu.percent==150 && s->threads[0].cpu.percent==50 && s->threads[1].cpu.percent==100);
    assert(s->machine_percent[0]==50 && s->machine_percent[1]==45 && s->machine_percent[2]==5);
    assert(held->sequence==1 && held->tasks[0].info.runtime_us==500000);
    assert(s->memory_valid && s->memory.free==400 && s->skew_valid && !s->skew_tenths);
    advance();tasks[0].runtime_us-=1000000; reverse_order=true;
    s=sample(a,true);assert(task(s,1)->cpu.percent==50);
    s=sample(b,true);assert(s->interval_us==2000000 && task(s,1)->cpu.percent==100);
    os64_monitor_destroy(a);os64_monitor_destroy(b);
}
static void gaps_and_resets(void)
{
    setup();os64_monitor_t *m=os64_monitor_create(4,4,4);assert(m);sample(m,true);
    advance();sample(m,false);advance();const os64_monitor_snapshot_t *s=sample(m,true);
    assert(!s->threads[0].cpu.valid && task(s,1)->cpu.valid);
    advance();s=sample(m,true);assert(s->threads[0].cpu.percent==50);
    tasks[0].runtime_us=1;advance();s=sample(m,true);assert(!task(s,1)->cpu.valid);
    advance();s=sample(m,true);assert(task(s,1)->cpu.percent==150);
    tasks[0].name[0]=0;advance();s=sample(m,true);assert(s->task_count==1);
    strcpy(tasks[0].name,"worker");advance();s=sample(m,true);assert(!task(s,1)->cpu.valid);
    // Reusing a thread ID under a different owner must not reuse its history.
    tasks[1].threads=2;threads[0].pid=2;advance();s=sample(m,true);
    bool found=false;for(size_t i=0;i<s->thread_count;i++)if(s->threads[i].info.pid==2){found=true;assert(!s->threads[i].cpu.valid);}assert(found);
    fail_proc=true;s=(void*)1;assert(os64_monitor_sample(m,true,&s)<0 && !s && !open_handles);
    fail_proc=false;advance();s=sample(m,true);assert(!task(s,1)->cpu.valid && !s->summary_valid);
    advance();s=sample(m,true);assert(task(s,1)->cpu.valid);
    fail_directory_read=true;assert(os64_monitor_sample(m,true,&s)<0 && !s && !open_handles);
    fail_directory_read=false;fail_clock=true;assert(os64_monitor_sample(m,true,&s)<0 && !s && !open_handles);
    fail_clock=false;sample(m,true);advance();fail_memory=true;s=sample(m,true);assert(!s->memory_valid && task(s,1)->cpu.valid);
    os64_monitor_destroy(m);
}
static void optional_sources(void)
{
    setup();os64_monitor_t *m=os64_monitor_create(4,4,4);sample(m,true);
    advance();cores[1]=(os64_monitor_time_t){1000000,500000,450000,50000};
    const os64_monitor_snapshot_t *s=sample(m,true);assert(s->summary_valid && s->parked==1 && s->cores[1].parked);
    assert(s->machine_percent[0]==25 && s->machine_percent[1]==73 && s->machine_percent[2]==2);
    fail_core=true;advance();s=sample(m,true);assert(s->core_count==1 && !s->summary_valid && !s->ledger_interval_us && s->interval_us==1000000);
    fail_core=false;advance();s=sample(m,true);assert(!s->summary_valid && !s->cores[0].valid);
    advance();s=sample(m,true);assert(s->summary_valid);
    bool *bad[]={&malformed_core,&duplicate_core,&missing_core,&fail_core_read,&fail_core_close};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        *bad[i]=true;advance();s=sample(m,true);assert(!s->core_count && (s->partial & OS64_MONITOR_CORES_PARTIAL));*bad[i]=false;
        advance();s=sample(m,true);assert(!s->summary_valid);advance();assert(sample(m,true)->summary_valid);
    }
    os64_monitor_destroy(m);
    setup();m=os64_monitor_create(1,1,1);s=sample(m,true);
    assert(s->task_count==1 && s->thread_count==1 && s->core_count==1 && s->partial==7);os64_monitor_destroy(m);
    setup();m=os64_monitor_create(4,0,4);s=sample(m,false);assert(!s->partial && !s->threads_sampled);
    s=sample(m,true);assert(s->partial==OS64_MONITOR_THREADS_PARTIAL);os64_monitor_destroy(m);
}
static void rounding_and_clocks(void)
{
    setup();ncores=1;os64_monitor_t *m=os64_monitor_create(4,4,4);sample(m,false);
    advance();clock_now.ticks+=10;const os64_monitor_snapshot_t *s=sample(m,false);
    assert(s->interval_us==1000000 && s->skew_tenths==100 && !s->skew_negative);
    advance();clock_now.ticks-=10;s=sample(m,false);assert(s->skew_negative && s->skew_tenths==100);
    advance();clock_now.ticks+=900;s=sample(m,false);assert(s->interval_us==10000000 && task(s,1)->cpu.percent==15);
    // Counter reset invalidates the affected core for one sample.
    cores[0]=(os64_monitor_time_t){0};advance();s=sample(m,false);assert(!s->summary_valid);
    unsigned seed=17;
    for(unsigned i=0;i<1000;i++) {
        uint64_t old=tasks[0].runtime_us;
        seed=seed*1664525u+1013904223u;uint64_t busy=seed%1000001;
        seed=seed*1664525u+1013904223u;uint64_t idle=seed%(1000001-busy);
        clock_now.ticks+=100;cores[0].total+=1000000;cores[0].busy+=busy;cores[0].idle+=idle;cores[0].sched+=1000000-busy-idle;
        tasks[0].runtime_us+=seed%4000000;
        s=sample(m,false);assert(s->summary_valid);
        assert(s->machine_percent[0]+s->machine_percent[1]+s->machine_percent[2]==100);
        assert(task(s,1)->cpu.percent==((tasks[0].runtime_us-old)*100+500000)/1000000);
    }
    os64_monitor_destroy(m);
    setup();ncores=1;cores[0]=(os64_monitor_time_t){0};tasks[0].runtime_us=0;clock_now.ticks=0;
    m=os64_monitor_create(4,0,4);sample(m,false);
    uint64_t n=UINT64_MAX-100;
    cores[0]=(os64_monitor_time_t){n,n/2,n-n/2,0};tasks[0].runtime_us=n;
    clock_now.ticks=n/10000;s=sample(m,false);
    assert(s->summary_valid && task(s,1)->cpu.percent==100 && s->machine_percent[0]==50);
    os64_monitor_destroy(m);
}
static void allocation_failures(void)
{
    setup();assert(!os64_monitor_create(0,1,1));assert(!os64_monitor_create(1,1,0));
    assert(!os64_monitor_create(SIZE_MAX,1,1));assert(!live_allocations);
    for(unsigned i=1;i<=7;i++) {
        allocations=0;fail_allocation=i;assert(!os64_monitor_create(4,4,4));assert(!live_allocations);
    }
    fail_allocation=0;os64_monitor_destroy(NULL);
    const os64_monitor_snapshot_t *s=(void*)1;assert(os64_monitor_sample(NULL,false,&s)<0 && !s);
}
int main(void)
{
    independent_consumers();gaps_and_resets();optional_sources();rounding_and_clocks();allocation_failures();
    assert(!live_allocations && !open_handles);puts("monitor: independent consumers, lifecycle, clocks, parsing, bounds and 1000 rounding intervals PASS");
}
