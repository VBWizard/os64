#include "pulse_details.h"
#include "os64/os64.h"
void pulse_details(char *out,size_t capacity,unsigned dial,const pulse_config_t *c,const pulse_sample_t *s)
{
    char a[64],b[64],d[64],peak[80];peak[0]=0;
    if(dial<PULSE_DIAL_COUNT && dial!=PULSE_DIAL_DISK){
        if(!s->peak_valid[dial])os64_strcopy(peak,sizeof(peak),"Peak, last 60s: unavailable");
        else{
            if(dial==PULSE_DIAL_CPU || dial==PULSE_DIAL_RAM)os64_snprintf(a,sizeof(a),"%lu%%",s->peak[dial]);
            else pulse_format_rate(a,sizeof(a),s->peak[dial],c->units);
            os64_snprintf(peak,sizeof(peak),"Peak, last 60s: %s",a);
        }
    }
    if(dial==PULSE_DIAL_CPU){
        if(s->cpu_valid)os64_snprintf(a,sizeof(a),"Current: %u%%",s->cpu_percent);
        else os64_strcopy(a,sizeof(a),"Waiting for a valid CPU interval");
        os64_snprintf(out,capacity,"CPU - overall utilization\n%s\nNormalized across cores\n%s",a,peak);
    }else if(dial==PULSE_DIAL_RAM || dial==PULSE_DIAL_DISK){
        bool disk=dial==PULSE_DIAL_DISK,valid=disk?s->disk_valid:s->ram_valid;
        os64_format_binary_size(disk?s->disk_used:s->ram_used,a,sizeof(a));
        os64_format_binary_size(disk?s->disk_available:s->ram_available,b,sizeof(b));
        os64_format_binary_size(disk?s->disk_total:s->ram_total,d,sizeof(d));
        if(disk){
            if(valid)os64_snprintf(out,capacity,"Disk - filesystem capacity\nMount: %s\nUsed: %s\nAvailable: %s\nTotal: %s\nSpace usage, not disk activity",c->mount,a,b,d);
            else os64_snprintf(out,capacity,"Disk - filesystem capacity\nMount: %s\nSpace data unavailable\nNot a disk activity indicator",c->mount);
        }else if(valid)os64_snprintf(out,capacity,"Memory\nCurrent: %u%%\nUsed: %s\nAvailable: %s\nTotal: %s\n%s",pulse_percent(s->ram_used,s->ram_total),a,b,d,peak);
        else os64_snprintf(out,capacity,"Memory\nMemory data unavailable\n%s",peak);
    }else if(dial==PULSE_DIAL_DOWN || dial==PULSE_DIAL_UP){
        pulse_format_rate(a,sizeof(a),dial==PULSE_DIAL_DOWN?s->download:s->upload,c->units);
        pulse_format_rate(b,sizeof(b),s->network_scale,c->units);
        if(s->network_scale && b[0]=='0' && b[1]=='.' && b[2]=='0' && b[3]==' ')
            pulse_format_rate(b,sizeof(b),s->network_scale,c->units>=PULSE_BITS?PULSE_KBIT:PULSE_KIB);
        os64_snprintf(out,capacity,"%s\n%s: %s\n%s%s\n%s\nShared scale: %s\nRecent traffic, not link capacity",dial==PULSE_DIAL_DOWN?"Download":"Upload",
            os64_streq(c->interface,"all")?"Interfaces":"Interface",os64_streq(c->interface,"all")?"all (excluding loopback)":c->interface,
            s->net_valid?"Current: ":"Waiting for a valid network interval",s->net_valid?a:"",peak,b);
    }else if(capacity)out[0]=0;
}
