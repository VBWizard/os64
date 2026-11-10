#include "pulse_data.h"
#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/conf.h"

const pulse_view_info_t pulse_views[PULSE_VIEWS]={{"Classic",false},{"Instruments",true}};
unsigned pulse_presentation(const pulse_config_t *c)
{return c->view==PULSE_INSTRUMENTS?(c->instruments_compact?PULSE_COMPACT_SIZE:PULSE_INSTRUMENTS_SIZE):PULSE_CLASSIC_SIZE;}
const char *const pulse_unit_names[PULSE_UNITS] = {"B/s", "KiB/s", "MiB/s", "bit/s", "kbit/s", "Mbit/s"};
void pulse_config_defaults(pulse_config_t *c)
{
    *c = (pulse_config_t){.show={true,true,true,true},.units=PULSE_MIB,.refresh_ms=1000,.font_size=24,.stay_on_top=true,
        .x=30,.y=40,.size={{320,420},{600,610},{360,390}}};
    os64_strcopy(c->mount,sizeof(c->mount),"/");
    os64_strcopy(c->interface,sizeof(c->interface),"all");
}

static bool geometry_numbers(const char *text,int64_t *values,unsigned count)
{
    const char *p=text;
    for(unsigned i=0;i<count;++i){
        bool negative=*p=='-';if(negative)++p;
        if(*p<'0' || *p>'9')return false;
        uint64_t n=0;
        while(*p>='0' && *p<='9'){
            unsigned digit=(unsigned)(*p++-'0');
            if(n>((uint64_t)INT32_MAX+negative-digit)/10)return false;
            n=n*10+digit;
        }
        values[i]=negative?-(int64_t)n:(int64_t)n;
        if(i+1<count){if(*p++!=',')return false;}
    }
    return *p==0;
}
static bool size_valid(int64_t w,int64_t h)
{return w>0 && h>0 && w<=32767 && h<=32767;}
bool pulse_config_line(const char *key,const char *value,void *user)
{
    pulse_config_t *c=user;
    if (!key) return true;
    const char *names[]={"cpu","ram","disk","net"};
    for(unsigned i=0;i<PULSE_METERS;++i)
        if(os64_streq_nocase(key,names[i])) { (void)os64_conf_get_bool(value,&c->show[i]); return true; }
    if(os64_streq_nocase(key,"stay_on_top")){(void)os64_conf_get_bool(value,&c->stay_on_top);return true;}
    if(os64_streq_nocase(key,"view")){
        c->view=os64_streq_nocase(value,"instruments")?PULSE_INSTRUMENTS:PULSE_CLASSIC;return true;
    }
    if(os64_streq_nocase(key,"instruments_compact")){
        c->instruments_compact=false;(void)os64_conf_get_bool(value,&c->instruments_compact);return true;
    }
    if(os64_streq_nocase(key,"geometry")){
        int64_t n[5];if(geometry_numbers(value,n,5) && n[0]==1 && size_valid(n[3],n[4])){
            c->x=(int32_t)n[1];c->y=(int32_t)n[2];c->size[0]=(pulse_size_t){(uint32_t)n[3],(uint32_t)n[4]};
        }return true;
    }
    const char *size_keys[]={"instruments_size","instruments_compact_size"};
    for(unsigned i=0;i<2;++i)if(os64_streq_nocase(key,size_keys[i])){
        int64_t n[2];if(geometry_numbers(value,n,2) && size_valid(n[0],n[1]))c->size[i+1]=(pulse_size_t){(uint32_t)n[0],(uint32_t)n[1]};
        return true;
    }
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

static uint64_t rate_ceiling(uint64_t peak)
{
    uint64_t ceiling=1024;
    while(ceiling<peak){if(ceiling>UINT64_MAX/2)return UINT64_MAX;ceiling*=2;}
    return ceiling;
}
void pulse_history_clear_network(pulse_history_t *h)
{for(unsigned i=0;i<PULSE_PEAK_SECONDS;++i)h->valid[i]&=3;}
bool pulse_history_update(pulse_history_t *h,int64_t micros,pulse_sample_t *s)
{
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){s->peak_valid[i]=false;s->peak[i]=0;}
    s->network_scale=rate_ceiling(s->net_valid?(s->download>s->upload?s->download:s->upload):0);
    if(micros<0)return false;
    uint64_t second=(uint64_t)micros/1000000;
    if(h->initialized && (uint64_t)micros<h->last_micros)return false;
    h->initialized=true;h->last_micros=(uint64_t)micros;
    unsigned slot=(unsigned)(second%PULSE_PEAK_SECONDS);
    if(h->seconds[slot]!=second){h->seconds[slot]=second;h->valid[slot]=0;}
    const unsigned dial[]={PULSE_DIAL_CPU,PULSE_DIAL_RAM,PULSE_DIAL_DOWN,PULSE_DIAL_UP};
    const uint64_t value[]={s->cpu_percent,pulse_percent(s->ram_used,s->ram_total),s->download,s->upload};
    const bool valid[]={s->cpu_valid,s->ram_valid,s->net_valid,s->net_valid};
    for(unsigned n=0;n<4;++n){
        uint8_t bit=(uint8_t)(1u<<n);unsigned d=dial[n];
        if(valid[n]){
            if(!(h->valid[slot]&bit) || value[n]>h->values[slot][n])h->values[slot][n]=value[n];
            h->valid[slot]|=bit;
        }
        for(unsigned i=0;i<PULSE_PEAK_SECONDS;++i)
            if((h->valid[i]&bit) && h->seconds[i]<=second && second-h->seconds[i]<PULSE_PEAK_SECONDS){
                s->peak_valid[d]=true;if(h->values[i][n]>s->peak[d])s->peak[d]=h->values[i][n];
            }
    }
    uint64_t down=s->peak[PULSE_DIAL_DOWN],up=s->peak[PULSE_DIAL_UP];
    s->network_scale=rate_ceiling(down>up?down:up);return true;
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
    int64_t micros=os64_micros();
    // Baselines continue while a meter is hidden, so showing it does not
    // turn a long gap into a misleading first rate.
    const os64_monitor_snapshot_t *sys=NULL;
    if(os64_monitor_sample_system(sampler->system,&sys)==0) {
        out->cpu_valid=sys->summary_valid;
        out->cpu_percent=100-sys->machine_percent[1]; // work plus scheduler time
        out->ram_valid=sys->memory_valid;
        out->ram_used=sys->memory.used;out->ram_available=sys->memory.available;out->ram_total=sys->memory.usable;
    }
    if(!os64_streq(sampler->history_interface,c->interface)){
        pulse_history_clear_network(&sampler->history);
        os64_strcopy(sampler->history_interface,sizeof(sampler->history_interface),c->interface);
    }
    const os64_monitor_io_snapshot_t *net=NULL;
    if(os64_monitor_io_sample(sampler->network,&net)==0)
        out->net_valid=pulse_network_rate(net,c->interface,&out->download,&out->upload);
    (void)pulse_history_update(&sampler->history,micros,out);
    if(c->show[PULSE_DISK]) {
        uint8_t *text=NULL;size_t len=0;
        if(os64_slurp("/sys/mounts",8192,&text,&len)==OS64_SLURP_OK) {
            out->disk_valid=pulse_mount_space((char *)text,c->mount,&out->disk_total,&out->disk_available);
            if(out->disk_valid) out->disk_used=out->disk_total-out->disk_available;
        }
        os64_free(text);
    }
}
