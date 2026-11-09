// HID 1.11 short items, keyboard usage page 7. Input bit offsets are independent
// of Output/Feature offsets, and of other Report IDs in a composite device.
#include "driver/system/hid_keyboard_map.h"
#define KMAP_STACK 8
#define KMAP_USAGES 32

typedef struct { uint32_t page,size,count; int32_t min,max; uint8_t id; } kmap_global_t;
typedef struct {
    uint32_t usages[KMAP_USAGES],min,max;
    unsigned count;
    bool have_min,have_max;
} kmap_local_t;
static int32_t kmap_signed(uint32_t v,unsigned n)
{ return n==1?(int8_t)v:n==2?(int16_t)v:(int32_t)v; }
static uint32_t kmap_usage(const kmap_local_t *l,unsigned i)
{
    if(i<l->count) return l->usages[i];
    if(l->have_min && l->have_max) {
        i-=l->count; return l->min+(i<l->max-l->min?i:l->max-l->min);
    }
    return l->count?l->usages[l->count-1]:0;
}
bool hid_keyboard_parse_map(const uint8_t *d,size_t n,uint8_t id,hid_keyboard_layout_t *out)
{
    if(!d || !out || !n || n>HID_KEYBOARD_MAP_BYTES) return false;
    kmap_global_t g={0},stack[KMAP_STACK]; kmap_local_t l={0};
    bool collections[KMAP_STACK],ids=false,have_keys=false;
    unsigned depth=0,pushed=0;
    uint16_t offsets[256]={0};
    hid_keyboard_layout_t m={.report_id=id};
    for(size_t pos=0;pos<n;) {
        uint8_t prefix=d[pos++];
        if(prefix==0xfe) return false;
        unsigned size=prefix&3; if(size==3) size=4;
        if(size>n-pos) return false;
        uint32_t v=0; for(unsigned i=0;i<size;i++) v|=(uint32_t)d[pos++]<<(8*i);
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1) {
            switch(tag) {
            case 0: if(v>0xffff) return false; g.page=v; break;
            case 1: g.min=kmap_signed(v,size); break;
            case 2:
                if(g.min>=0 && v>0x7fffffff) return false;
                g.max=g.min<0?kmap_signed(v,size):(int32_t)v; break;
            case 3: case 4: case 5: case 6: break;
            case 7: g.size=v; break;
            case 8: if(!v || v>255) return false; ids=true; g.id=v; break;
            case 9: g.count=v; break;
            case 10: if(size || pushed==KMAP_STACK) return false; stack[pushed++]=g; break;
            case 11: if(size || !pushed) return false; g=stack[--pushed]; break;
            default: return false;
            }
        } else if(type==2) {
            uint32_t u=size==4?v:(g.page<<16)|v;
            switch(tag) {
            case 0: if(l.count==KMAP_USAGES) return false; l.usages[l.count++]=u; break;
            case 1: l.min=u; l.have_min=true; break;
            case 2: l.max=u; l.have_max=true; break;
            case 3: case 4: case 5: case 7: case 8: case 9: break;
            default: return false;
            }
        } else if(type==0) {
            if(l.have_min!=l.have_max || (l.have_min && (l.max<l.min || l.min>>16!=l.max>>16))) return false;
            if(tag==10) {
                if(depth==KMAP_STACK || size!=1) return false;
                uint32_t u=kmap_usage(&l,0);
                bool keyboard=v==1?(u==0x10006 || u==0x10007):(depth && collections[depth-1]);
                collections[depth++]=keyboard;
            } else if(tag==12) {
                if(!depth || size) return false;
                depth--;
            } else if(tag==8) {
                if(!depth || !size || !g.size || !g.count || g.size>512 || g.count>512 ||
                   g.size*g.count>512u-offsets[g.id]) return false;
                uint16_t bit=offsets[g.id]; offsets[g.id]+=g.size*g.count;
                if(g.id==id && collections[depth-1] && !(v&1)) {
                    if(v&~3u) return false; // Absolute data with supported array/variable semantics.
                    if(v&2) {
                        for(unsigned i=0;i<g.count;i++) {
                            uint32_t u=kmap_usage(&l,i);
                            if(u>>16!=7) continue;
                            u&=0xffff;
                            if(u>255 || g.size!=1 || g.min!=0 || g.max!=1 || m.usage_bits[u]) return false;
                            m.usage_bits[u]=bit+i+1;
                            if(u>=4 && u<0xe0) have_keys=true;
                        }
                    } else {
                        if(g.page!=7 || !l.have_min || l.count || l.min>>16!=7 ||
                           (l.max&0xffff)>255 || m.array_count || g.size>8 || g.count>32 ||
                           g.min<0 || g.max<g.min || g.max>255 || (uint32_t)g.max>=(1u<<g.size) ||
                           (uint32_t)(g.max-g.min)!=l.max-l.min) return false;
                        m.array_bit=bit; m.array_size=g.size; m.array_count=g.count;
                        m.logical_min=g.min; m.logical_max=g.max; m.usage_min=l.min&255;
                        have_keys=true;
                    }
                }
            } else if(tag!=9 && tag!=11) return false;
            l=(kmap_local_t){0};
        } else return false;
    }
    if(depth || pushed || (ids && offsets[0]) || (!ids && id) || !have_keys) return false;
    m.bytes=(offsets[id]+7)/8;
    if(!m.bytes || m.bytes>HID_KEYBOARD_INPUT_BYTES) return false;
    *out=m; return true;
}
static unsigned kmap_read(const uint8_t *p,unsigned bit,unsigned size)
{
    unsigned v=0;
    for(unsigned i=0;i<size;i++) v|=((p[(bit+i)/8]>>((bit+i)%8))&1u)<<i;
    return v;
}
bool hid_keyboard_decode_map(const hid_keyboard_layout_t *m,const uint8_t *p,size_t n,uint8_t out[8])
{
    if(!m || !p || !out || !m->bytes || n!=m->bytes || n>HID_KEYBOARD_INPUT_BYTES) return false;
    bool held[256]={0}; uint8_t result[8]={0}; unsigned count=0;
    for(unsigned u=0;u<256;u++) if(m->usage_bits[u]) {
        if(m->usage_bits[u]>n*8) return false;
        held[u]=kmap_read(p,m->usage_bits[u]-1,1)!=0;
    }
    if(m->array_count) {
        if(!m->array_size || m->array_size>8 || m->array_count>32 ||
           m->array_bit+(size_t)m->array_size*m->array_count>n*8) return false;
        for(unsigned i=0;i<m->array_count;i++) {
            unsigned v=kmap_read(p,m->array_bit+i*m->array_size,m->array_size);
            if(v<m->logical_min || v>m->logical_max) return false;
            unsigned u=v-m->logical_min+m->usage_min;
            if(u>255) return false;
            held[u]=true;
        }
    }
    for(unsigned u=0xe0;u<=0xe7;u++) if(held[u]) result[0]|=1u<<(u-0xe0);
    bool rollover=held[1] || held[2] || held[3];
    for(unsigned u=4;u<256;u++) {
        if(!held[u] || (u>=0xe0 && u<=0xe7)) continue;
        if(count<6) result[2+count]=u;
        count++;
    }
    if(rollover || count>6) for(unsigned i=2;i<8;i++) result[i]=1;
    for(unsigned i=0;i<8;i++) out[i]=result[i];
    return true;
}
