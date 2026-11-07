#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pulse_data.h"
#include "os64/slurp.h"
#include "os64/conf.h"
static os64_monitor_snapshot_t sys;
static os64_monitor_interface_t interfaces[3];
static os64_monitor_io_snapshot_t net={.interfaces=interfaces,.interface_count=2};
static int system_fail,network_fail;
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
static void sampling(void)
{
    pulse_config_t c;pulse_config_defaults(&c);
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
    system_fail=network_fail=1;pulse_sample(&sampler,&c,&s);
    assert(!s.cpu_valid && !s.ram_valid && !s.net_valid && s.disk_valid);
    c.show[PULSE_DISK]=false;pulse_sample(&sampler,&c,&s);assert(!s.disk_valid);
}
int main(void)
{
    disk();network();formatting();sampling();
    puts("pulse: disk parsing, network validity, unit conversion, percentages and sampling PASS");
}
