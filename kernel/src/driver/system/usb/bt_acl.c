// bt_acl.c — the controller's ACL data buffers, counted. bt_acl.h is the
// contract; Bluetooth Core Vol 4 Part E § 4.1.1 (the pools) and § 7.7.19
// (Number Of Completed Packets) are the rules.
#include "driver/system/usb/bt_acl.h"

void bt_acl_init(bt_acl_t *a)
{
    uint8_t *raw=(uint8_t *)a;
    for(size_t i=0;i<sizeof(*a);i++) raw[i]=0;
}

void bt_acl_sizes(bt_acl_t *a,uint16_t classic_size,uint16_t classic_total,
                  uint16_t le_size,uint16_t le_total)
{
    a->classic_size=classic_size; a->classic_total=classic_total;
    // Half an LE pool is no LE pool.
    bool own=le_size && le_total;
    a->le_size=own?le_size:0; a->le_total=own?le_total:0;
    a->known=true;
}

bool bt_acl_shared(const bt_acl_t *a) { return a->le_total==0; }

static bt_acl_link_t *acl_find(bt_acl_t *a,uint16_t handle)
{
    for(unsigned i=0;i<BT_ACL_LINKS;i++)
        if(a->link[i].live && a->link[i].handle==handle) return &a->link[i];
    return NULL;
}
static const bt_acl_link_t *acl_find_const(const bt_acl_t *a,uint16_t handle)
{
    for(unsigned i=0;i<BT_ACL_LINKS;i++)
        if(a->link[i].live && a->link[i].handle==handle) return &a->link[i];
    return NULL;
}

bool bt_acl_link_up(bt_acl_t *a,uint16_t handle,bool le)
{
    if(acl_find(a,handle)) return false;
    for(unsigned i=0;i<BT_ACL_LINKS;i++) if(!a->link[i].live) {
        a->link[i]=(bt_acl_link_t){.handle=handle,.le=le,.live=true};
        return true;
    }
    return false;
}

void bt_acl_link_down(bt_acl_t *a,uint16_t handle)
{
    bt_acl_link_t *l=acl_find(a,handle);
    if(l) *l=(bt_acl_link_t){0};
}

uint16_t bt_acl_packet_size(const bt_acl_t *a,bool le)
{
    return le && !bt_acl_shared(a) ? a->le_size : a->classic_size;
}

// Packets out on the links that draw on one pool. A row with no link in it
// is all zeroes (bt_acl_link_down), so it adds nothing.
static unsigned acl_out(const bt_acl_t *a,bool want_le,bool want_classic)
{
    unsigned n=0;
    for(unsigned i=0;i<BT_ACL_LINKS;i++) {
        const bt_acl_link_t *l=&a->link[i];
        if(l->le?want_le:want_classic) n+=l->out;
    }
    return n;
}

uint16_t bt_acl_reserved_for_le(const bt_acl_t *a)
{
    if(!bt_acl_shared(a) || !a->classic_total) return 0;
    unsigned le=0;
    for(unsigned i=0;i<BT_ACL_LINKS;i++) if(a->link[i].live && a->link[i].le) le++;
    // A Classic link is always left one buffer.
    return (uint16_t)(le<a->classic_total ? le : a->classic_total-1u);
}

bool bt_acl_may_send(const bt_acl_t *a,uint16_t handle)
{
    const bt_acl_link_t *l=acl_find_const(a,handle);
    if(!l) return false;   // and before the sizes are known every pool holds nothing
    if(!bt_acl_shared(a))
        return l->le ? acl_out(a,true,false)<a->le_total : acl_out(a,false,true)<a->classic_total;
    if(l->le) return acl_out(a,true,true)<a->classic_total;
    // Classic, in a shared pool: its own packets may not reach into what
    // is kept for LE, and the pool as a whole may not be overrun.
    return acl_out(a,false,true)+bt_acl_reserved_for_le(a)<a->classic_total &&
           acl_out(a,true,true)<a->classic_total;
}

bool bt_acl_sent(bt_acl_t *a,uint16_t handle)
{
    bt_acl_link_t *l=acl_find(a,handle);
    if(!l) return false;
    l->out++;
    return true;
}

bt_acl_verdict_t bt_acl_completed(bt_acl_t *a,const uint8_t *e,size_t n,uint16_t *link)
{
    // Event 0x13: a count of entries, then that many (handle, packets).
    if(n<3 || e[0]!=0x13 || n!=2u+e[1] || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;
    bt_acl_verdict_t verdict=BT_ACL_COUNTED;
    for(unsigned i=0;i<e[2];i++) {
        const uint8_t *p=e+3+4*i;
        uint16_t handle=(uint16_t)((p[0]|p[1]<<8)&0xfff);
        uint16_t count=(uint16_t)(p[2]|p[3]<<8);
        bt_acl_link_t *l=acl_find(a,handle);
        if(!l || !count) { a->strays++; continue; }
        if(count>l->out) {
            // The first such link is the one reported; every one is zeroed.
            if(verdict==BT_ACL_COUNTED) *link=handle;
            verdict=BT_ACL_OVERCOUNT; l->out=0;
            continue;
        }
        l->out-=count;
    }
    return verdict;
}

uint16_t bt_acl_outstanding(const bt_acl_t *a,uint16_t handle)
{
    const bt_acl_link_t *l=acl_find_const(a,handle);
    return l?l->out:0;
}

uint16_t bt_acl_in_use(const bt_acl_t *a,bool le_pool)
{
    if(bt_acl_shared(a)) return (uint16_t)acl_out(a,true,true);
    return (uint16_t)(le_pool?acl_out(a,true,false):acl_out(a,false,true));
}
