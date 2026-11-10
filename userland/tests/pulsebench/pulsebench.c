// Native timing fixture around Pulse's production event loop. The measurement
// loop does not save settings; leave the controls alone during the run. Results
// use this task's CPU ledger, excluding the compositor and load task, and
// elapsed monotonic time. Keep the window visible.
#include "os64/ui_dispatcher.h"
static int64_t benchmark_turn(os64_ui_dispatcher_t *,bool);
#define os64_ui_dispatcher_turn benchmark_turn
#define main pulse_app_main
#include "../../apps/pulse/pulse.c"
#undef main
#undef os64_ui_dispatcher_turn

static unsigned phase;
static int64_t phase_start,measure_start,load_task;
static uint64_t cpu_start;
static bool measuring,started;
static const char *const phase_name[]={"Classic idle","Instruments idle","Compact idle",
    "Classic loaded","Instruments loaded","Compact loaded","Compact details loaded"};
static void end_load(void)
{
    if(load_task<=0)return;
    char path[64];os64_snprintf(path,sizeof(path),"/proc/%ld/ctl",load_task);
    int64_t fd=os64_open(path,"w");
    if(fd>=0){(void)os64_write((int32_t)fd,"kill",4);(void)os64_close((int32_t)fd);}
    int32_t status;(void)os64_wait(load_task,&status);load_task=0;
}
static bool begin_phase(int64_t now)
{
    pulse_config_t c=g.config;unsigned mode=phase<6?phase%3:2;
    c.view=mode?PULSE_INSTRUMENTS:PULSE_CLASSIC;c.instruments_compact=mode==2;
    c.font_size=24;c.refresh_ms=1000;c.stay_on_top=true;
    for(unsigned i=0;i<PULSE_METERS;++i)c.show[i]=true;
    c.size[0]=(pulse_size_t){420,500};c.size[1]=(pulse_size_t){600,610};c.size[2]=(pulse_size_t){360,390};
    pulse_instruments_destroy(g.renderer);g.renderer=NULL;
    if(!change_view(&c))return false;
    if(phase==3){
        char *const argv[]={"/tests/spin_test",NULL};load_task=os64_spawn(argv[0],argv);
        if(load_task<=0)return false;
    }
    if(phase==6)pulse_overlay_focus(&g.overlay,PULSE_DIAL_CPU);
    __atomic_store_n(&g.refresh_ms,1000,__ATOMIC_RELAXED);
    phase_start=now;measuring=false;return true;
}
static int64_t benchmark_turn(os64_ui_dispatcher_t *dispatcher,bool wait)
{
    int64_t rc=os64_ui_dispatcher_turn(dispatcher,wait),now=os64_micros();
    if(rc<0 || now<0)return rc;
    if(!started){started=true;if(!begin_phase(now)){g.running=false;return -1;}}
    if(!measuring && now-phase_start>=5000000){
        os64_proc_info_t info;
        if(os64_proc_read(os64_taskid(),&info)){g.running=false;return -1;}
        cpu_start=info.runtime_us;measure_start=now;measuring=true;
    }else if(measuring && now-measure_start>=30000000){
        os64_proc_info_t info;
        if(os64_proc_read(os64_taskid(),&info)){g.running=false;return -1;}
        uint64_t elapsed=(uint64_t)(now-measure_start),cpu=info.runtime_us-cpu_start;
        uint64_t permille_percent=cpu*100000/elapsed;char line[256];
        os64_snprintf(line,sizeof(line),"pulsebench: %s elapsed_us=%lu cpu_us=%lu cpu=%lu.%03lu%% drawing_live=%lu drawing_peak=%lu",
            phase_name[phase],elapsed,cpu,permille_percent/1000,permille_percent%1000,
            (uint64_t)pulse_instruments_live_bytes(g.renderer),(uint64_t)pulse_instruments_peak_bytes(g.renderer));
        os64_debug_log(line);os64_puts(line);
        if(++phase==sizeof(phase_name)/sizeof(phase_name[0]))g.running=false;
        else if(!begin_phase(now)){g.running=false;return -1;}
    }
    return rc;
}
int main(void)
{
    int result=pulse_app_main();end_load();
    os64_debug_log(phase==7 && !result?"pulsebench: PASS":"pulsebench: incomplete");
    return phase==7?result:1;
}
