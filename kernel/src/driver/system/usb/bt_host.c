#include "driver/system/usb/bt_host.h"
#include "strings/sprintf.h"

static uint16_t host_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static bool host_setup(const bt_host_t *h)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        const bt_le_t *s=&h->peers[i].le;
        if(!bt_le_quiescent(s) && !bt_le_input_active(s)) return true;
    }
    return false;
}
void bt_host_init(bt_host_t *h,uint64_t now)
{
    *h=(bt_host_t){.credits=1};
    for(unsigned i=0;i<BT_HOST_PEERS;i++) h->peers[i].le.shared_controller=true;
    bt_scan_start_le(&h->scan,now);
}
void bt_host_event(bt_host_t *h,const uint8_t *e,size_t n)
{
    if(n<2 || n!=2u+e[1]) { h->failed=true; return; }
    if(e[0]==0x0e || e[0]==0x0f) {
        bool status=e[0]==0x0f;
        if(n<(status?6u:5u) || (status && n!=6)) { h->failed=true; return; }
        h->credits=e[status?3:2];
        uint16_t op=host_u16(e+(status?4:3));
        if(h->owner==1) {
            if(op==h->scan.opcode) h->command_done=true;
            bt_scan_event(&h->scan,e,n);
        } else if(h->owner>=2 && h->owner<BT_HOST_PEERS+2) {
            bt_le_t *s=&h->peers[h->owner-2].le;
            if(op==s->opcode) h->command_done=true;
            bt_le_event(s,e,n);
        }
        return;
    }
    if(e[0]==0x10) { h->failed=true; return; }
    if(e[0]==0x3e && n>=3 && e[2]==2) { bt_scan_event(&h->scan,e,n); return; }
    if(e[0]==1 || e[0]==2 || e[0]==0x22 || e[0]==0x2f) {
        bt_scan_event(&h->scan,e,n); return;
    }
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        bt_le_t *s=&h->peers[i].le;
        // Initiation is serialized. A completion belongs to its pending peer,
        // including a success racing cancellation; established peers ignore it.
        if(e[0]==0x3e && n>=3 && e[2]==1 &&
           !(s->link_may_active && !s->connected)) continue;
        bt_le_event(s,e,n);
    }
}
typedef struct {
    bt_host_t *host;
    unsigned peer;
    bt_scan_send_t command;
    bt_le_acl_send_t acl;
    bt_host_report_t report;
    void *context;
} host_call_t;
static void host_report(void *context,const bt_le_input_t *input)
{
    host_call_t *c=context; c->report(c->context,c->peer,input);
}
void bt_host_receive(bt_host_t *h,const uint8_t *p,size_t n,bt_host_report_t report,void *ctx)
{
    // Assemble USB fragments once, then route complete ACL packets by handle.
    // Per-peer L2CAP reassembly allows interleaved continuation fragments.
    while(n && !h->failed) {
        if(h->wire_used<4) {
            h->wire[h->wire_used++]=*p++; n--;
            if(h->wire_used<4) continue;
            h->wire_need=4u+host_u16(h->wire+2);
            if(h->wire_need>sizeof(h->wire)) { h->failed=true; return; }
        }
        while(n && h->wire_used<h->wire_need) { h->wire[h->wire_used++]=*p++; n--; }
        if(h->wire_used<h->wire_need) return;
        uint16_t handle=host_u16(h->wire)&0xfff;
        for(unsigned i=0;i<BT_HOST_PEERS;i++) {
            bt_le_t *s=&h->peers[i].le;
            if(!s->connected || s->handle!=handle) continue;
            host_call_t call={.host=h,.peer=i,.report=report,.context=ctx};
            bt_le_receive(s,h->wire,h->wire_need,host_report,&call);
            break;
        }
        volatile uint8_t *wipe=h->wire;
        for(size_t i=0;i<h->wire_used;i++) wipe[i]=0;
        h->wire_used=h->wire_need=0;
    }
}
static void host_command(void *context,uint16_t op,const uint8_t *p,uint8_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    h->owner=c->peer==BT_HOST_PEERS?1:c->peer+2;
    h->credits=0; h->command_done=false;
    c->command(c->context,op,p,n);
}
static void host_acl(void *context,const uint8_t *p,size_t n)
{
    host_call_t *c=context; c->host->tx_used=true;
    c->host->poll_peer=(c->peer+1)%BT_HOST_PEERS;
    c->acl(c->context,p,n);
}
static bool host_control(const bt_host_t *h,unsigned owner,bool usb_done)
{ return usb_done && (h->owner==owner || (!h->owner && h->credits)); }
static void host_retire(bt_host_t *h,bool usb_done)
{
    bool pending=h->owner==1?h->scan.pending:
        h->owner>=2?h->peers[h->owner-2].le.pending:false;
    if(usb_done && h->command_done && !pending) h->owner=0;
}
void bt_host_policy(bt_host_t *h,uint64_t now)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) bt_manager_observe(&h->peers[i].manager,&h->peers[i].le,now);
    if(!h->initialized || h->failed || host_setup(h)) return;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        bt_host_peer_t *p=&h->peers[i];
        if(p->manager.scan_owned) {
            bt_manager_step(&p->manager,&p->le,&h->scan,now);
            return;
        }
    }
    if(!bt_scan_quiescent(&h->scan)) return;
    for(unsigned count=0;count<BT_HOST_PEERS;count++) {
        unsigned i=h->next_peer; h->next_peer=(i+1)%BT_HOST_PEERS;
        bt_host_peer_t *p=&h->peers[i];
        bt_manager_step(&p->manager,&p->le,&h->scan,now);
        if(p->manager.scan_owned) break;
    }
}
void bt_host_tick(bt_host_t *h,uint64_t now,bool usb_done,bool bulk_done,bool failed,
                  bt_scan_send_t command,bt_le_acl_send_t acl,bt_host_report_t report,void *ctx)
{
    h->failed|=failed;
    host_retire(h,usb_done);
    h->tx_used=!bulk_done;
    for(unsigned i=0;i<BT_HOST_PEERS;i++)
        if(h->peers[i].le.outstanding) h->tx_used=true;
    host_call_t call={.host=h,.peer=BT_HOST_PEERS,.command=command,.acl=acl,.report=report,.context=ctx};
    if(!h->initialized && !h->failed && h->scan.phase==BT_SCAN_STOPPED) {
        if(!h->init_retry_at) h->init_retry_at=now+2000;
        if(now>=h->init_retry_at) { bt_scan_start_le(&h->scan,now); h->init_retry_at=0; }
    }
    if(!bt_scan_quiescent(&h->scan)) {
        h->scan.credits=h->credits;
        bt_scan_tick(&h->scan,now,host_control(h,1,usb_done),h->failed,host_command,&call);
        host_retire(h,usb_done);
        if(h->scan.phase==BT_SCAN_FAILED) h->failed=true;
        if(!h->initialized && h->scan.phase==BT_SCAN_DONE) {
            h->initialized=true; h->scan.shared_controller=true;
        }
    }
    unsigned first=h->poll_peer;
    for(unsigned count=0;count<BT_HOST_PEERS;count++) {
        unsigned i=(first+count)%BT_HOST_PEERS;
        bt_le_t *s=&h->peers[i].le; call.peer=i; s->credits=h->credits;
        bt_le_tick(s,now,host_control(h,i+2,usb_done),
                   s->phase==BT_LE_CLEANUP?bulk_done:!h->tx_used,
                   h->failed,host_command,host_acl,host_report,&call);
        host_retire(h,usb_done);
        if(s->phase==BT_LE_FAILED) h->failed=true;
    }
    // A controller failure affects every connection, even if detected by the
    // last peer in this pass. Deliver releases before returning to input code.
    if(h->failed) {
        volatile uint8_t *wipe=h->wire;
        for(size_t i=0;i<sizeof(h->wire);i++) wipe[i]=0;
        h->wire_used=h->wire_need=0;
    }
    if(h->failed) for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        call.peer=i;
        bt_le_tick(&h->peers[i].le,now,false,false,true,host_command,host_acl,host_report,&call);
    }
    bt_host_policy(h,now);
}
bool bt_host_scan(bt_host_t *h,uint64_t now)
{
    if(!h->initialized || h->failed || host_setup(h) || !bt_scan_start_le(&h->scan,now)) return false;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        h->peers[i].manager.scan_owned=false;
        h->peers[i].manager.next_attempt=now+60000;
    }
    return true;
}
static bool host_address_equal(uint8_t a_type,const uint8_t a[6],uint8_t b_type,const uint8_t b[6])
{
    if(a_type!=b_type) return false;
    for(unsigned i=0;i<6;i++) if(a[i]!=b[i]) return false;
    return true;
}
bool bt_host_command(bt_host_t *h,const char *p,size_t n,uint64_t now)
{
    h->duplicate_slot=0;
    while(n && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) { p++; n--; }
    unsigned slot=0;
    if(n>=7 && p[0]=='s' && p[1]=='l' && p[2]=='o' && p[3]=='t' && p[4]==' ') {
        if(p[5]<'0' || p[5]>='0'+BT_HOST_PEERS || p[6]!=' ') return false;
        slot=p[5]-'0'; p+=7; n-=7;
    }
    bt_host_peer_t *peer=&h->peers[slot];
    bool connect=n>=4 && ((p[0]=='c' && p[1]=='o') || (p[0]=='b' && p[1]=='o') ||
                         (p[0]=='r' && p[1]=='e'));
    if(connect && (!h->initialized || h->failed || host_setup(h))) return false;
    if(connect) {
        // Validate on scratch state so a duplicate request cannot mutate the
        // selected slot's reconnect policy or retained failure diagnostics.
        bt_le_t candidate={.shared_controller=true,.bond=peer->le.bond};
        bool valid=bt_le_request(&candidate,p,n,now);
        for(unsigned i=0;i<BT_HOST_PEERS && valid;i++) if(i!=slot) {
            const bt_le_t *other=&h->peers[i].le;
            const bt_le_bond_t *bond=&other->bond;
            // A sleeping peer still owns its saved identity. Without an IRK,
            // retain ownership of its last air address as well.
            bool duplicate=other->connected && host_address_equal(other->address_type,other->peer,
                candidate.address_type,candidate.peer);
            if(bond->valid) duplicate|=host_address_equal(bond->address_type,bond->peer,
                candidate.address_type,candidate.peer) || (!bond->has_irk &&
                host_address_equal(bond->last_address_type,bond->last_peer,candidate.address_type,candidate.peer));
            if(duplicate) { valid=false; h->duplicate_slot=i+1; }
        }
        volatile uint8_t *wipe=(volatile uint8_t *)&candidate;
        for(size_t i=0;i<sizeof(candidate);i++) wipe[i]=0;
        if(!valid) return false;
    }
    return bt_manager_command(&peer->manager,&peer->le,&h->scan,p,n,now);
}
size_t bt_host_status(const bt_host_t *h,char *out,size_t cap)
{
    if(!cap) return 0;
    int n=snprintf(out,cap,"controller: %s\ncommands: slot N COMMAND (default slot 0)\n",
        h->failed?"failed; reboot required":h->initialized?"ready":"initializing");
    if(n<0) return 0;
    size_t used=(size_t)n<cap?(size_t)n:cap-1;
    if(h->duplicate_slot && used<cap-1) {
        n=snprintf(out+used,cap-used,"last command refused: peer belongs to slot %u\n",h->duplicate_slot-1);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    for(unsigned i=0;i<BT_HOST_PEERS && used<cap-1;i++) {
        n=snprintf(out+used,cap-used,"\nslot %u:\n",i);
        if(n<0) break;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
        used+=bt_le_status(&h->peers[i].le,out+used,cap-used);
        used+=bt_manager_status(&h->peers[i].manager,out+used,cap-used);
    }
    return used;
}
