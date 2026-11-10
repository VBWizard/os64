#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pulse_data.h"
#include "os64/slurp.h"
#include "os64/conf.h"
// Include the production placement path with persistence redirected to a
// deterministic store; the real parser still validates and reads the text.
static char saved_conf[1024];
static int read_error,write_error,write_calls;
static char saved_keys[2][40],saved_values[2][100];
static size_t saved_count;
static int64_t placement_read(const char *name,os64_conf_fn fn,void *user,char *path,size_t cap)
{
    (void)path;(void)cap;assert(!strcmp(name,"pulse.conf"));
    if(read_error)return read_error;
    return os64_conf_parse(saved_conf,strlen(saved_conf),fn,user);
}
static int64_t placement_write(const char *name,const os64_conf_pair_t *pairs,size_t n,
    bool (*validate)(const char *,size_t,void *),void *user)
{
    assert(!strcmp(name,"pulse.conf"));assert(n<=2);++write_calls;saved_count=n;
    const char *good="geometry = 1,2,3,400,500\n";assert(validate(good,strlen(good),user));
    const char *bad="this is not a setting\n";assert(!validate(bad,strlen(bad),user));
    for(size_t i=0;i<n;++i){strcpy(saved_keys[i],pairs[i].key);strcpy(saved_values[i],pairs[i].value);}
    return write_error;
}
#define os64_conf_find_read placement_read
#define os64_conf_write_checked placement_write
#include "../userland/apps/pulse/pulse_placement.c"
#undef os64_conf_find_read
#undef os64_conf_write_checked
static void placement(void)
{
    pulse_config_t c;pulse_config_defaults(&c);
    strcpy(saved_conf,"geometry = 1,90,80,420,450\ninstruments_size = 620,630\ninstruments_compact_size = 370,380\n");
    os64_gui_event_t e={.type=OS64_GUI_EVENT_WINDOW_RESIZE};
    assert(!pulse_record_placement(&c,&e) && !write_calls);
    e.type=OS64_GUI_EVENT_WINDOW_PLACEMENT;e.placement.changed=OS64_GUI_PLACEMENT_WIDTH;
    e.placement.width=500;assert(!pulse_record_placement(&c,&e));
    assert(c.size[0].w==500 && saved_count==1 && !strcmp(saved_values[0],"1,90,80,500,450"));
    c.view=PULSE_INSTRUMENTS;e.placement.width=700;assert(!pulse_record_placement(&c,&e));
    assert(c.size[1].w==700 && saved_count==1 && !strcmp(saved_keys[0],"instruments_size") && !strcmp(saved_values[0],"700,630"));
    c.instruments_compact=true;e.placement.changed=OS64_GUI_PLACEMENT_ALL;
    e.placement.x=-5;e.placement.y=60;e.placement.width=390;e.placement.height=400;
    assert(!pulse_record_placement(&c,&e));
    assert(saved_count==2 && !strcmp(saved_values[0],"1,-5,60,420,450"));
    assert(!strcmp(saved_keys[1],"instruments_compact_size") && !strcmp(saved_values[1],"390,400"));
    assert(c.size[0].w==500 && c.size[1].w==700 && c.size[2].w==390);
    int calls=write_calls;e.placement.width=0;
    assert(pulse_record_placement(&c,&e)==OS64_CONF_BAD_SETTING && write_calls==calls);
    e.placement.width=410;read_error=OS64_CONF_IO_ERROR;
    assert(pulse_record_placement(&c,&e)==read_error && write_calls==calls && c.size[2].w==410);
    read_error=0;write_error=OS64_CONF_IO_ERROR;e.placement.width=420;
    assert(pulse_record_placement(&c,&e)==write_error && c.size[2].w==420);
    write_error=0;read_error=OS64_CONF_NO_FILE;
    assert(!pulse_record_placement(&c,&e));read_error=0;
}
static os64_monitor_snapshot_t sys;
static os64_monitor_interface_t interfaces[3];
static os64_monitor_io_snapshot_t net={.interfaces=interfaces,.interface_count=2};
static int system_fail,network_fail;
static int64_t clock_us;static unsigned clock_reads;
int64_t os64_micros(void){++clock_reads;return clock_us;}
static const char *mount_report="/ ext2 nvme 1 root rw 512 1 1000 400 - -\n";
int32_t os64_monitor_sample_system(os64_monitor_t *m,const os64_monitor_snapshot_t **out)
{ (void)m;*out=&sys;return system_fail?-1:0; }
int32_t os64_monitor_io_sample(os64_monitor_io_t *m,const os64_monitor_io_snapshot_t **out)
{ (void)m;*out=&net;return network_fail?-1:0; }
os64_slurp_status_t os64_slurp(const char *path,size_t cap,uint8_t **out,size_t *len)
{
    assert(!strcmp(path,"/sys/mounts"));*len=strlen(mount_report);assert(*len<cap);
    *out=malloc(*len+1);assert(*out);memcpy(*out,mount_report,*len+1);return OS64_SLURP_OK;
}
void *os64_malloc(size_t n){return malloc(n);}
void os64_free(void *p){free(p);}
int64_t os64_write(int32_t h,const void *s,size_t n){(void)h;(void)s;(void)n;abort();}
static void disk(void)
{
    uint64_t total=0,available=0;
    char good[]="# header\n/ ext2 nvme 1 root rw 512 1 1000 400 - -\n/mnt/my\\x20disk fat disk 1 name rw 512 1 20 10 - -\n";
    assert(pulse_mount_space(good,"/mnt/my disk",&total,&available));assert(total==20 && available==10);
    char absent[]="/sys sysfs - - - rw - - - - - -\n";
    assert(!pulse_mount_space(absent,"/sys",&total,&available));
    char bad[]="/ ext2 disk 1 name rw 512 1 10 11 - -\n";
    assert(!pulse_mount_space(bad,"/",&total,&available));
    char duplicate[]="/ ext2 disk 1 name rw 512 1 10 5 - -\n/ ext2 disk 1 name rw 512 1 20 10 - -\n";
    assert(!pulse_mount_space(duplicate,"/",&total,&available));
    char shortrow[]="/ ext2 10 5\n";assert(!pulse_mount_space(shortrow,"/",&total,&available));
    char missing[]="/ ext2 disk 1 name rw 512 1 10 5 - -";assert(!pulse_mount_space(missing,"/home",&total,&available));
}
static void network(void)
{
    strcpy(interfaces[0].name,"eth0");strcpy(interfaces[1].name,"eth1");
    for(unsigned i=0;i<2;++i){interfaces[i].status.available=true;interfaces[i].status.rate_valid=true;}
    interfaces[0].per_second.rx_bytes=100;interfaces[0].per_second.tx_bytes=5;
    interfaces[1].per_second.rx_bytes=200;interfaces[1].per_second.tx_bytes=7;
    uint64_t down,up;
    assert(pulse_network_rate(&net,"all",&down,&up) && down==300 && up==12);
    assert(pulse_network_rate(&net,"eth1",&down,&up) && down==200 && up==7);
    assert(!pulse_network_rate(&net,"missing",&down,&up));
    interfaces[1].status.rate_valid=false;assert(!pulse_network_rate(&net,"all",&down,&up));
    interfaces[1].status.rate_valid=true;
    strcpy(interfaces[1].name,"lo");assert(pulse_network_rate(&net,"all",&down,&up) && down==100);
    strcpy(interfaces[1].name,"eth1");
    interfaces[1].per_second.rx_bytes=UINT64_MAX;assert(!pulse_network_rate(&net,"all",&down,&up));
    interfaces[1].per_second.rx_bytes=200;
    net.partial=OS64_MONITOR_IO_INTERFACES_PARTIAL;assert(!pulse_network_rate(&net,"all",&down,&up));net.partial=0;
}
static void formatting(void)
{
    for(uint64_t total=1;total<1000;++total)
        for(uint64_t used=0;used<=total;++used) assert(pulse_percent(used,total)==used*100/total);
    assert(pulse_percent(UINT64_MAX-1,UINT64_MAX)==99);
    assert(pulse_percent(UINT64_MAX,UINT64_MAX)==100);
    char text[64];
    pulse_format_rate(text,sizeof(text),1048576,PULSE_MIB);assert(!strcmp(text,"1.0 MiB/s"));
    pulse_format_rate(text,sizeof(text),125000,PULSE_MBIT);assert(!strcmp(text,"1.0 Mbit/s"));
    pulse_format_rate(text,sizeof(text),1536,PULSE_KIB);assert(!strcmp(text,"1.5 KiB/s"));
    pulse_format_rate(text,sizeof(text),UINT64_MAX,PULSE_BITS);assert(!strcmp(text,"rate too large"));
    pulse_format_rate(text,sizeof(text),0,PULSE_BYTES);assert(!strcmp(text,"0.0 B/s"));
}
static void scales(void)
{
    pulse_history_t h={0};pulse_sample_t s={.cpu_valid=true,.cpu_percent=70,
        .ram_valid=true,.ram_used=40,.ram_total=100,.net_valid=true,.download=1500,.upload=500};
    assert(pulse_history_update(&h,0,&s));assert(s.network_scale==2048);
    assert(s.peak_valid[PULSE_DIAL_CPU] && s.peak[PULSE_DIAL_CPU]==70);
    assert(s.peak[PULSE_DIAL_RAM]==40 && s.peak[PULSE_DIAL_DOWN]==1500 && s.peak[PULSE_DIAL_UP]==500);
    s.cpu_percent=20;s.download=0;s.upload=800;
    assert(pulse_history_update(&h,500000,&s));assert(s.peak[PULSE_DIAL_CPU]==70 && s.peak[PULSE_DIAL_UP]==800);
    pulse_history_t before=h;
    assert(!pulse_history_update(&h,499999,&s));assert(!memcmp(&h,&before,sizeof(h)) && !s.peak_valid[0]);
    assert(!pulse_history_update(&h,-1,&s));assert(!memcmp(&h,&before,sizeof(h)));
    s=(pulse_sample_t){0};assert(pulse_history_update(&h,59000000,&s));
    assert(s.peak_valid[0] && s.peak[0]==70 && s.network_scale==2048);
    assert(pulse_history_update(&h,60000000,&s));assert(!s.peak_valid[0] && s.network_scale==1024);
    s=(pulse_sample_t){.cpu_valid=true,.net_valid=true,.download=UINT64_MAX};
    assert(pulse_history_update(&h,61000000,&s));assert(s.peak_valid[0] && s.peak[0]==0 && s.network_scale==UINT64_MAX);
    pulse_history_clear_network(&h);s.net_valid=false;
    assert(pulse_history_update(&h,62000000,&s));assert(s.peak_valid[0] && !s.peak_valid[PULSE_DIAL_DOWN] && s.network_scale==1024);
    s.cpu_valid=false;assert(pulse_history_update(&h,122000000,&s));assert(!s.peak_valid[0]);
}
static void configuration(void)
{
    pulse_config_t c;pulse_config_defaults(&c);assert(c.view==PULSE_CLASSIC && !c.instruments_compact);
    pulse_config_line("view","instruments",&c);pulse_config_line("instruments_compact","on",&c);
    assert(pulse_presentation(&c)==PULSE_COMPACT_SIZE && pulse_views[c.view].compact);
    pulse_config_line("view","classic",&c);assert(c.instruments_compact && pulse_presentation(&c)==0);
    pulse_config_line("geometry","1,-20,40,430,560",&c);assert(c.x==-20 && c.y==40 && c.size[0].w==430);
    pulse_config_line("instruments_size","600,640",&c);pulse_config_line("instruments_compact_size","350,390",&c);
    assert(c.size[1].h==640 && c.size[2].w==350);
    pulse_config_line("instruments_size","4294967297,50",&c);assert(c.size[1].w==600);
    pulse_config_line("geometry","1,2147483648,0,400,400",&c);assert(c.x==-20);
    pulse_config_line("geometry","1,-2147483648,0,400,400",&c);assert(c.x==INT32_MIN);
    pulse_config_line("view","unknown",&c);pulse_config_line("instruments_compact","unknown",&c);
    assert(c.view==PULSE_CLASSIC && !c.instruments_compact);
}
static void sampling(void)
{
    pulse_config_t c;pulse_config_defaults(&c);
    assert(c.stay_on_top);
    pulse_config_line("stay_on_top","off",&c);assert(!c.stay_on_top);
    pulse_config_line("stay_on_top","bad",&c);assert(!c.stay_on_top);
    assert(c.font_size==24);
    pulse_config_line("font_size","32",&c);assert(c.font_size==32);
    pulse_config_line("font_size","0",&c);assert(c.font_size==32);
    pulse_config_line("font_size","49",&c);assert(c.font_size==32);
    pulse_config_line("font_size","16",&c);assert(c.font_size==16);
    pulse_config_line("font_size","48",&c);assert(c.font_size==48);
    assert(c.refresh_ms==1000 && c.units==PULSE_MIB && c.show[PULSE_NET]);
    pulse_config_line("refresh_ms","0",&c);assert(c.refresh_ms==1000);
    pulse_config_line("refresh_ms","250",&c);assert(c.refresh_ms==250);
    pulse_config_line("cpu","off",&c);assert(!c.show[PULSE_CPU]);
    pulse_config_line("network_units","Mbit/s",&c);assert(c.units==PULSE_MBIT);
    pulse_config_line("mount","relative",&c);assert(!strcmp(c.mount,"/"));
    pulse_sampler_t sampler={0};pulse_sample_t s;
    sys.summary_valid=true;sys.memory_valid=true;sys.machine_percent[1]=75;
    sys.memory.used=100;sys.memory.available=900;sys.memory.usable=1000;
    pulse_sample(&sampler,&c,&s);
    assert(s.cpu_valid && s.cpu_percent==25 && s.ram_valid && s.ram_used==100 && s.ram_available==900);
    assert(s.disk_valid && s.disk_used==600 && s.disk_available==400 && s.net_valid && s.download==300);
    assert(clock_reads==1 && s.peak_valid[PULSE_DIAL_CPU]);
    clock_us=60000000;system_fail=network_fail=1;pulse_sample(&sampler,&c,&s);
    assert(clock_reads==2 && !s.peak_valid[PULSE_DIAL_CPU]);
    clock_us=-1;pulse_sample(&sampler,&c,&s);assert(!s.peak_valid[PULSE_DIAL_RAM]);
    assert(!s.cpu_valid && !s.ram_valid && !s.net_valid && s.disk_valid);
    c.show[PULSE_DISK]=false;pulse_sample(&sampler,&c,&s);assert(!s.disk_valid);
}
int main(void)
{
    disk();network();formatting();scales();configuration();sampling();placement();
    puts("pulse: disk parsing, network validity, unit conversion, peaks, sampling and placement persistence PASS");
}
