#include "pulse_data.h"
#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/conf.h"

const char *const pulse_unit_names[PULSE_UNITS] = {"B/s", "KiB/s", "MiB/s", "bit/s", "kbit/s", "Mbit/s"};
void pulse_config_defaults(pulse_config_t *c)
{
    *c = (pulse_config_t){.show={true,true,true,true},.units=PULSE_MIB,.refresh_ms=1000,.font_size=24,.stay_on_top=true};
    os64_strcopy(c->mount,sizeof(c->mount),"/");
    os64_strcopy(c->interface,sizeof(c->interface),"all");
}

bool pulse_config_line(const char *key,const char *value,void *user)
{
    pulse_config_t *c=user;
    if (!key) return true;
    const char *names[]={"cpu","ram","disk","net"};
    for(unsigned i=0;i<PULSE_METERS;++i)
        if(os64_streq_nocase(key,names[i])) { (void)os64_conf_get_bool(value,&c->show[i]); return true; }
    if(os64_streq_nocase(key,"stay_on_top")){(void)os64_conf_get_bool(value,&c->stay_on_top);return true;}
    uint64_t n;
    if(os64_streq_nocase(key,"refresh_ms")) {
        if(os64_parse_u64(value,&n) && n>=250 && n<=10000) c->refresh_ms=(unsigned)n;
    } else if(os64_streq_nocase(key,"font_size")) {
        if(os64_parse_u64(value,&n) && n>=16 && n<=48) c->font_size=(unsigned)n;
    } else if(os64_streq_nocase(key,"network_units")) {
        for(unsigned i=0;i<PULSE_UNITS;++i) if(os64_streq(value,pulse_unit_names[i])) c->units=i;
    } else if(os64_streq_nocase(key,"mount")) {
        if(value[0]=='/' && os64_strlen(value)<sizeof(c->mount)) os64_strcopy(c->mount,sizeof(c->mount),value);
    } else if(os64_streq_nocase(key,"interface")) {
        if(value[0] && os64_strlen(value)<sizeof(c->interface)) os64_strcopy(c->interface,sizeof(c->interface),value);
    }
    return true;
}

unsigned pulse_percent(uint64_t used,uint64_t total)
{
    if(!total) return 0;
    if(used>=total) return 100;
    // Comparison thresholds avoid overflowing a byte count times 100.
    unsigned p=0;
    while(p<100) {
        unsigned next=p+1;
        uint64_t threshold=(total/100)*next + ((total%100)*next+99)/100;
        if(used<threshold) break;
        p=next;
    }
    return p;
}

void pulse_format_rate(char *out,size_t cap,uint64_t bytes,unsigned unit)
{
    static const uint32_t divisor[PULSE_UNITS]={1,1024,1048576,1,1000,1000000};
    if(unit>=PULSE_UNITS) unit=PULSE_BYTES;
    uint64_t d=divisor[unit],scale=unit>=PULSE_BITS?8:1;
    uint64_t whole=bytes/d,part=bytes%d;
    if(whole>UINT64_MAX/scale) { os64_strcopy(out,cap,"rate too large"); return; }
    whole*=scale; part*=scale;
    if(whole>UINT64_MAX-part/d) { os64_strcopy(out,cap,"rate too large"); return; }
    whole+=part/d;
    unsigned decimal=(unsigned)((part%d)*10/d);
    os64_snprintf(out,cap,"%lu.%u %s",whole,decimal,pulse_unit_names[unit]);
}

bool pulse_mount_space(char *text,const char *wanted,uint64_t *total,uint64_t *available)
{
    bool found=false;
    for(char *line=text;*line;) {
        char *next=line;while(*next && *next!='\n') ++next;
        if(*next) *next++=0;
        char *p=line;while(*p==' ' || *p=='\t' || *p=='\r') ++p;
        if(*p && *p!='#') {
            char *fields[12];unsigned count=0;
            while(*p) {
                if(count==12) return false;
                fields[count++]=p;
                while(*p && *p!=' ' && *p!='\t' && *p!='\r') ++p;
                if(*p) *p++=0;
                while(*p==' ' || *p=='\t' || *p=='\r') ++p;
            }
            if(count!=12) return false;
            os64_unescape_field(fields[0]);
            if(os64_streq(fields[0],wanted)) {
                if(found || !os64_parse_u64(fields[8],total) ||
                   !os64_parse_u64(fields[9],available) || *available>*total) return false;
                found=true;
            }
        }
        line=next;
    }
    return found;
}

bool pulse_network_rate(const os64_monitor_io_snapshot_t *s,const char *name,uint64_t *down,uint64_t *up)
{
    *down=*up=0;
    if(!s || (s->partial & OS64_MONITOR_IO_INTERFACES_PARTIAL)) return false;
    bool all=os64_streq(name,"all"),found=false;
    for(size_t i=0;i<s->interface_count;++i) {
        const os64_monitor_interface_t *n=&s->interfaces[i];
        if(all ? os64_streq(n->name,"lo") : !os64_streq(name,n->name)) continue;
        found=true;
        if(!n->status.available || !n->status.rate_valid ||
           *down>UINT64_MAX-n->per_second.rx_bytes || *up>UINT64_MAX-n->per_second.tx_bytes) return false;
        *down+=n->per_second.rx_bytes;*up+=n->per_second.tx_bytes;
    }
    return found;
}

uint64_t pulse_rate_scale(pulse_rate_scale_t *s,uint64_t second,uint64_t down,uint64_t up,bool valid)
{
    if(!s->initialized || second<s->last_second)*s=(pulse_rate_scale_t){.initialized=true};
    s->last_second=second;
    unsigned slot=(unsigned)(second%60);
    if(s->seconds[slot]!=second){s->seconds[slot]=second;s->peaks[slot]=0;}
    uint64_t rate=down>up?down:up;
    if(valid && rate>s->peaks[slot])s->peaks[slot]=rate;
    uint64_t peak=0;
    for(unsigned i=0;i<60;++i)
        if(s->seconds[i]<=second && second-s->seconds[i]<60 && s->peaks[i]>peak)peak=s->peaks[i];
    uint64_t ceiling=1024;
    while(ceiling<peak){
        if(ceiling>UINT64_MAX/2)return UINT64_MAX;
        ceiling*=2;
    }
    return ceiling;
}

bool pulse_sampler_init(pulse_sampler_t *s)
{
    *s=(pulse_sampler_t){0};
    s->system=os64_monitor_create(1,0,256);
    s->network=os64_monitor_io_create(OS64_MONITOR_IO_NETWORK,32);
    if(s->system && s->network) return true;
    pulse_sampler_destroy(s);return false;
}
void pulse_sampler_destroy(pulse_sampler_t *s)
{
    os64_monitor_destroy(s->system);os64_monitor_io_destroy(s->network);
    *s=(pulse_sampler_t){0};
}
void pulse_sample(pulse_sampler_t *sampler,const pulse_config_t *c,pulse_sample_t *out)
{
    *out=(pulse_sample_t){0};
    // Baselines continue while a meter is hidden, so showing it does not
    // turn a long gap into a misleading first rate.
    const os64_monitor_snapshot_t *sys=NULL;
    if(os64_monitor_sample_system(sampler->system,&sys)==0) {
        out->cpu_valid=sys->summary_valid;
        out->cpu_percent=100-sys->machine_percent[1]; // work plus scheduler time
        out->ram_valid=sys->memory_valid;
        out->ram_used=sys->memory.used;out->ram_available=sys->memory.available;out->ram_total=sys->memory.usable;
    }
    if(!os64_streq(sampler->scale_interface,c->interface)){
        sampler->scale=(pulse_rate_scale_t){0};
        os64_strcopy(sampler->scale_interface,sizeof(sampler->scale_interface),c->interface);
    }
    const os64_monitor_io_snapshot_t *net=NULL;
    if(os64_monitor_io_sample(sampler->network,&net)==0)
        out->net_valid=pulse_network_rate(net,c->interface,&out->download,&out->upload);
    uint64_t second=sys && sys->ticks.per_second?sys->ticks.ticks/sys->ticks.per_second:sampler->scale.last_second;
    out->network_scale=pulse_rate_scale(&sampler->scale,second,out->download,out->upload,out->net_valid);
    if(c->show[PULSE_DISK]) {
        uint8_t *text=NULL;size_t len=0;
        if(os64_slurp("/sys/mounts",8192,&text,&len)==OS64_SLURP_OK) {
            out->disk_valid=pulse_mount_space((char *)text,c->mount,&out->disk_total,&out->disk_available);
            if(out->disk_valid) out->disk_used=out->disk_total-out->disk_available;
        }
        os64_free(text);
    }
}
