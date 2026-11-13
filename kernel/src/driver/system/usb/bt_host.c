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
    bt_acl_init(&h->acl);
    for(unsigned i=0;i<BT_HOST_PEERS;i++) h->peers[i].le.shared_controller=true;
    bt_scan_start_le(&h->scan,now);
}
// Bring the count's links in line with the peers: one for each LE peer that
// is connected and one for a connected Classic link, under its handle.
// Called wherever a peer may have connected or let go (its events, its
// tick), before the count is asked or told anything.
static void host_acl_link(bt_host_t *h,uint16_t *counted,bool connected,uint16_t handle,bool le)
{
    uint16_t now=connected?(uint16_t)((handle&0xfff)+1):0;   // the handle plus one (bt_host.h)
    if(now==*counted) return;
    if(*counted) bt_acl_link_down(&h->acl,(uint16_t)(*counted-1));
    if(now) (void)bt_acl_link_up(&h->acl,(uint16_t)(now-1),le);
    *counted=now;
}
static void host_acl_links(bt_host_t *h)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        const bt_le_t *s=&h->peers[i].le;
        host_acl_link(h,&h->acl_link[i],s->connected,s->handle,true);
    }
    host_acl_link(h,&h->acl_classic,h->classic.connected,h->classic.handle,false);
}
// An event that cannot be right. Once the controller is known to be up that
// is the end of the host. During start-up it is the end of the ROUND, which
// bt_host_tick may run again: an adapter taken over while it was running
// can hand this host the tail of an old event first, and the assembler
// delivers whatever that tail happens to frame before it is back in step.
static void host_bad_event(bt_host_t *h,bool hardware)
{
    if(h->initialized) { h->failed=true; return; }
    if(bt_scan_quiescent(&h->scan)) return;    // between rounds: nothing it could belong to
    if(hardware) h->scan.hardware_error=true; else h->scan.bad_reply=true;
}
void bt_host_event(bt_host_t *h,const uint8_t *e,size_t n)
{
    if(n<2 || n!=2u+e[1]) { host_bad_event(h,false); return; }
    if(e[0]==0x0e || e[0]==0x0f) {
        bool status=e[0]==0x0f;
        if(n<(status?6u:5u) || (status && n!=6)) { host_bad_event(h,false); return; }
        h->credits=e[status?3:2];
        uint16_t op=host_u16(e+(status?4:3));
        if(h->owner==BT_HOST_OWNER_SCAN) {
            if(op==h->scan.opcode) h->command_done=true;
            bt_scan_event(&h->scan,e,n);
        } else if(h->owner>=BT_HOST_OWNER_PEER0 && h->owner<BT_HOST_OWNER_PEER0+BT_HOST_PEERS) {
            bt_le_t *s=&h->peers[h->owner-BT_HOST_OWNER_PEER0].le;
            if(op==s->opcode) h->command_done=true;
            bt_le_event(s,e,n);
        } else if(h->owner==BT_HOST_OWNER_CLASSIC) {
            if(op==h->classic.opcode) h->command_done=true;
            (void)bt_classic_event(&h->classic,e,n);
        }
        return;
    }
    if(e[0]==0x10) { host_bad_event(h,true); return; }
    if(e[0]==0x13) {
        // Counted here for the controller as a whole; each LE peer below
        // still takes its own entries, and judges them, as it always has.
        uint16_t link;
        if(bt_acl_completed(&h->acl,e,n,&link)!=BT_ACL_COUNTED) h->acl_bad_completions++;
    }
    // The Classic link is offered every other event first. It takes the
    // BR/EDR-only events, and Disconnection Complete, Encryption Change and
    // Key Refresh only for its own live handle (bt_classic.h), so nothing
    // an LE peer or the scan reads is taken from them.
    if(h->initialized && h->classic_available && bt_classic_event(&h->classic,e,n)) {
        host_acl_links(h); return;
    }
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
    host_acl_links(h);
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
static void host_wire_empty(bt_host_t *h)
{
    volatile uint8_t *wipe=h->wire;
    for(size_t i=0;i<h->wire_used;i++) wipe[i]=0;
    h->wire_used=h->wire_need=0;
}
void bt_host_receive(bt_host_t *h,const uint8_t *p,size_t n,size_t max_packet,
                     bt_host_report_t report,void *ctx)
{
    // A packet shorter than the endpoint's maximum ends its transfer.
    bool ends_transfer=max_packet!=0 && n<max_packet;
    if(h->wire_skip) {
        // The rest of a transfer whose header could not be real.
        if(ends_transfer) h->wire_skip=false;
        return;
    }
    // Assemble USB fragments once, then route complete ACL packets by handle.
    // Per-peer L2CAP reassembly allows interleaved continuation fragments.
    while(n && !h->failed) {
        if(h->wire_used<4) {
            h->wire[h->wire_used++]=*p++; n--;
            if(h->wire_used<4) continue;
            h->wire_need=4u+host_u16(h->wire+2);
            if(h->wire_need>sizeof(h->wire)) {
                if(!max_packet) { h->failed=true; return; }
                // Not a header: the assembler was out of step with the
                // transfers (bt_host.h). Drop to the end of this one.
                host_wire_empty(h); h->wire_resyncs++;
                h->wire_skip=!ends_transfer;
                return;
            }
        }
        while(n && h->wire_used<h->wire_need) { h->wire[h->wire_used++]=*p++; n--; }
        if(h->wire_used<h->wire_need) break;   // out of bytes: the end-of-transfer check below decides
        uint16_t handle=host_u16(h->wire)&0xfff;
        bool routed=false;
        for(unsigned i=0;i<BT_HOST_PEERS && !routed;i++) {
            bt_le_t *s=&h->peers[i].le;
            if(!s->connected || s->handle!=handle) continue;
            host_call_t call={.host=h,.peer=i,.report=report,.context=ctx};
            bt_le_receive(s,h->wire,h->wire_need,host_report,&call);
            routed=true;
        }
        // The Classic link's packets have no reader yet: counted and dropped,
        // as is a packet for a handle nobody holds.
        if(!routed && h->classic.connected && h->classic.handle==handle) h->classic_rx_unread++;
        host_wire_empty(h);
    }
    // The transfer is over and the packet is not: what is held was never
    // the start of one.
    if(ends_transfer && h->wire_used) { host_wire_empty(h); h->wire_resyncs++; }
}
static void host_command(void *context,uint16_t op,const uint8_t *p,uint8_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    h->owner=c->peer==BT_HOST_PEERS?BT_HOST_OWNER_SCAN:BT_HOST_OWNER_PEER0+c->peer;
    h->credits=0; h->command_done=false;
    c->command(c->context,op,p,n);
}
static void host_classic_command(void *context,uint16_t op,const uint8_t *p,uint8_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    h->owner=BT_HOST_OWNER_CLASSIC;
    h->credits=0; h->command_done=false;
    c->command(c->context,op,p,n);
}
static void host_acl(void *context,const uint8_t *p,size_t n)
{
    host_call_t *c=context; c->host->tx_used=true;
    c->host->poll_peer=(c->peer+1)%BT_HOST_PEERS;
    if(n>=2) (void)bt_acl_sent(&c->host->acl,host_u16(p)&0xfff);
    c->acl(c->context,p,n);
}
static bool host_control(const bt_host_t *h,unsigned owner,bool usb_done)
{ return usb_done && (h->owner==owner || (!h->owner && h->credits)); }
// The Classic link counts no credits of its own: holding the slot is leave
// to finish the command in flight, and to send the next only with a credit
// the controller has given back. After a command it gave up waiting for it
// holds the slot with none, and sends nothing until some reply brings one.
static bool host_classic_control(const bt_host_t *h,bool usb_done)
{
    if(h->owner!=BT_HOST_OWNER_CLASSIC) return host_control(h,BT_HOST_OWNER_CLASSIC,usb_done);
    return usb_done && (h->credits || (h->classic.pending && !h->command_done));
}
static bool host_owner_pending(const bt_host_t *h)
{
    if(h->owner==BT_HOST_OWNER_SCAN) return h->scan.pending;
    if(h->owner>=BT_HOST_OWNER_PEER0 && h->owner<BT_HOST_OWNER_PEER0+BT_HOST_PEERS)
        return h->peers[h->owner-BT_HOST_OWNER_PEER0].le.pending;
    if(h->owner==BT_HOST_OWNER_CLASSIC) return h->classic.pending;
    return false;
}
static void host_retire(bt_host_t *h,bool usb_done)
{
    if(usb_done && h->command_done && !host_owner_pending(h)) h->owner=BT_HOST_OWNER_NONE;
}
// Reconnection (bt_host.h): one page at classic_page_at, when the link is
// quiescent and nothing else is setting up or scanning.
static void host_classic_policy(bt_host_t *h,uint64_t now)
{
    bt_classic_t *c=&h->classic;
    if(!h->initialized || h->failed || !h->classic_available) return;
    if(c->phase==BT_CLASSIC_READY) { h->classic_backoff_ms=0; h->classic_page_at=0; return; }
    // An attempt under way, theirs or ours, takes the place of a page that
    // was owed: how IT ends decides whether another is. Without this, a
    // page armed by an unanswered one fired after their call had ended in
    // a way nothing should be paged after (a stale key).
    if(c->phase!=BT_CLASSIC_IDLE && c->phase!=BT_CLASSIC_STOPPED) { h->classic_page_at=0; return; }
    if(!h->classic_auto || !c->bond.valid || !bt_classic_quiescent(c)) return;
    if(!h->classic_page_at) {
        // Only an end the far end may come back from is paged again.
        if(c->phase!=BT_CLASSIC_STOPPED || !c->retryable) return;
        if(!h->classic_backoff_ms) h->classic_backoff_ms=5000;
        h->classic_page_at=now+h->classic_backoff_ms;
        h->classic_backoff_ms=h->classic_backoff_ms>=30000?60000:h->classic_backoff_ms*2;
        return;
    }
    if(now<h->classic_page_at || host_setup(h) || !bt_scan_quiescent(&h->scan)) return;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) if(h->peers[i].manager.scan_owned) return;
    h->classic_page_at=0;
    (void)bt_classic_request(c,"reconnect",9,NULL,now,NULL);
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
static void host_classic_tick(bt_host_t *h,uint64_t now,bool usb_done,host_call_t *call)
{
    bt_classic_tick(&h->classic,now,host_classic_control(h,usb_done),h->failed,host_classic_command,call);
    host_retire(h,usb_done);
    host_acl_links(h);
}
// The start-up round has reached DONE: the Classic link begins, if the
// controller can carry it.
static void host_classic_begin(bt_host_t *h)
{
    const bt_scan_t *s=&h->scan;
    h->classic_setup_refused=s->classic_setup_refused;
    h->classic_refused_opcode=s->classic_refused_opcode;
    h->classic_refused_status=s->classic_refused_status;
    bt_classic_init(&h->classic,s->address);
    // LMP features: bit 37 is "BR/EDR Not Supported"; without LE (bit 38)
    // the round ends before its Classic setup.
    h->classic_unavailable=(s->features[4]&0x20)?"the controller has no Classic radio":
        !(s->features[4]&0x40)?"the start-up round ended before the Classic setup":
        s->classic_setup_refused?"the controller refused a Classic setup command":NULL;
    h->classic_available=!h->classic_unavailable;
}
// The Classic link still owning the command slot this long after its
// command's deadline, with nothing of its own in flight: the reply never
// came, or it came and the USB transfer that carried the command never
// reported done. Either way the slot would be held for ever and the LE
// peers starved in silence; the controller is failed instead, as it is
// when an LE command goes unanswered.
#define BT_HOST_CLASSIC_REPLY_GRACE_MS 3000
void bt_host_tick(bt_host_t *h,uint64_t now,bool usb_done,bool bulk_done,bool failed,
                  bt_scan_send_t command,bt_le_acl_send_t acl,bt_host_report_t report,void *ctx)
{
    h->failed|=failed;
    host_retire(h,usb_done);
    host_acl_links(h);   // before any peer below may send
    h->tx_used=!bulk_done;
    for(unsigned i=0;i<BT_HOST_PEERS;i++)
        if(h->peers[i].le.outstanding) h->tx_used=true;
    host_call_t call={.host=h,.peer=BT_HOST_PEERS,.command=command,.acl=acl,.report=report,.context=ctx};
    if(!h->initialized && !h->failed && h->scan.phase==BT_SCAN_STOPPED) {
        if(!h->init_retry_at) h->init_retry_at=now+2000;
        if(now>=h->init_retry_at) { bt_scan_start_le(&h->scan,now); h->init_retry_at=0; }
    }
    // The Classic link takes the first free turn when the controller is
    // waiting on one of its answers, and the last otherwise.
    bool classic=h->initialized && h->classic_available;
    bool classic_first=classic && bt_classic_urgent(&h->classic);
    if(classic_first) host_classic_tick(h,now,usb_done,&call);
    if(!bt_scan_quiescent(&h->scan)) {
        h->scan.credits=h->credits;
        bt_scan_tick(&h->scan,now,host_control(h,BT_HOST_OWNER_SCAN,usb_done),h->failed,host_command,&call);
        host_retire(h,usb_done);
        if(h->scan.phase==BT_SCAN_FAILED) {
            // A start-up round that failed outright is run again a few
            // times before the host is called failed (bt_host.h). STOPPED
            // is the state the retry above starts from. A failed transport
            // is not a round to repeat.
            if(!h->initialized && !h->failed) {
                // The next round rebuilds the scan from nothing, so what
                // this one died of is kept here.
                h->round_error=h->scan.error; h->round_opcode=h->scan.error_opcode;
                h->round_status=h->scan.error_status;
                h->round_timeout=bt_scan_timeout_text(&h->scan);
            }
            if(!h->initialized && !h->failed && ++h->init_failed_rounds<BT_HOST_INIT_ROUNDS) {
                h->scan.phase=BT_SCAN_STOPPED;
                // No reply came, so no reply gave the command credit back.
                // The next round opens with a reset, which a host may send
                // whatever it believes about credit.
                h->credits=1;
            }
            else h->failed=true;
        }
        if(!h->initialized && h->scan.phase==BT_SCAN_DONE) {
            h->initialized=true; h->scan.shared_controller=true;
            // The next scan clears what this round heard.
            if(h->scan.buffers_known)
                bt_acl_sizes(&h->acl,h->scan.acl_size,h->scan.acl_total,
                             h->scan.le_acl_size,h->scan.le_acl_total);
            host_classic_begin(h);
        }
    }
    unsigned first=h->poll_peer;
    for(unsigned count=0;count<BT_HOST_PEERS;count++) {
        unsigned i=(first+count)%BT_HOST_PEERS;
        bt_le_t *s=&h->peers[i].le; call.peer=i; s->credits=h->credits;
        bt_le_tick(s,now,host_control(h,BT_HOST_OWNER_PEER0+i,usb_done),
                   s->phase==BT_LE_CLEANUP?bulk_done:!h->tx_used,
                   h->failed,host_command,host_acl,host_report,&call);
        host_retire(h,usb_done);
        host_acl_links(h);
        if(s->phase==BT_LE_FAILED) h->failed=true;
    }
    if(classic && !classic_first) host_classic_tick(h,now,usb_done,&call);
    if(classic && h->owner==BT_HOST_OWNER_CLASSIC && !h->classic.pending &&
       now>=h->classic.command_deadline+BT_HOST_CLASSIC_REPLY_GRACE_MS) h->failed=true;
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
    if(h->failed && classic) host_classic_tick(h,now,usb_done,&call);
    bt_host_policy(h,now);
    host_classic_policy(h,now);
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
bool bt_host_scan_classic(bt_host_t *h,uint64_t now)
{
    if(!h->initialized || h->failed || host_setup(h)) return false;
    if(!bt_classic_quiescent(&h->classic) && h->classic.phase!=BT_CLASSIC_READY) return false;
    if(!bt_scan_start_classic(&h->scan,now)) return false;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        h->peers[i].manager.scan_owned=false;
        h->peers[i].manager.next_attempt=now+60000;
    }
    return true;
}
bool bt_host_classic_bond(bt_host_t *h,const bt_classic_bond_t *bond,uint64_t now)
{
    if(!h->initialized || !h->classic_available) return false;
    bool ours=bond && bond->valid;
    for(unsigned i=0;ours && i<6;i++) ours=bond->local[i]==h->classic.local[i];
    bt_classic_set_bond(&h->classic,ours?bond:NULL);
    h->classic_auto=ours;
    h->classic_page_at=ours?(now?now:1):0;   // 0 means none owed
    return true;
}
static bool host_address_equal(uint8_t a_type,const uint8_t a[6],uint8_t b_type,const uint8_t b[6])
{
    if(a_type!=b_type) return false;
    for(unsigned i=0;i<6;i++) if(a[i]!=b[i]) return false;
    return true;
}
static bool host_space(char ch) { return ch==' ' || ch=='\t' || ch=='\n' || ch=='\r'; }
static bool host_word(const char *p,size_t n,const char *word)
{
    size_t i=0;
    for(;word[i];i++) if(i==n || p[i]!=word[i]) return false;
    return i==n || host_space(p[i]);
}
static int host_hex(char ch)
{
    return ch>='0' && ch<='9'?ch-'0':ch>='a' && ch<='f'?ch-'a'+10:ch>='A' && ch<='F'?ch-'A'+10:-1;
}
// The address in a command, the first "aa:bb:cc:dd:ee:ff" in it, least
// significant byte first as the scan keeps it.
static bool host_find_address(const char *p,size_t n,uint8_t out[6])
{
    for(size_t at=0;at+17<=n;at++) {
        bool good=true;
        for(unsigned i=0;i<6 && good;i++) {
            int hi=host_hex(p[at+3*i]),lo=host_hex(p[at+3*i+1]);
            good=hi>=0 && lo>=0 && (i==5 || p[at+3*i+2]==':');
            if(good) out[5-i]=(uint8_t)(hi<<4|lo);
        }
        if(good) return true;
    }
    return false;
}
// What the last scan heard at this address: its first Classic entry and
// its first LE entry, each NULL when there is none.
static void host_heard(const bt_host_t *h,const uint8_t address[6],
                       const bt_scan_device_t **classic,const bt_scan_device_t **le)
{
    *classic=*le=NULL;
    for(unsigned i=0;i<h->scan.count && i<BT_SCAN_DEVICES;i++) {
        const bt_scan_device_t *d=&h->scan.devices[i];
        bool same=true;
        for(unsigned j=0;j<6 && same;j++) same=d->address[j]==address[j];
        if(!same) continue;
        if(!d->le && !*classic) *classic=d;
        if(d->le && !*le) *le=d;
    }
}
static const char host_no_classic[]="no Classic slot";
// "slot 2 COMMAND": the Classic link's words, refused with a reason the
// status file keeps.
static bool host_classic_command_text(bt_host_t *h,const char *p,size_t n,uint64_t now)
{
    while(n && host_space(*p)) { p++; n--; }
    while(n && host_space(p[n-1])) n--;
    bool forget=host_word(p,n,"forget"), disconnect=host_word(p,n,"disconnect");
    bool pages=host_word(p,n,"bond") || host_word(p,n,"connect") || host_word(p,n,"reconnect");
    // Forgetting stays possible on a failed controller, as it does for LE:
    // the bond file is the user's to clear whatever the adapter is doing.
    if(!h->initialized) { h->refused_why="the controller is not ready"; return false; }
    if(h->failed && !forget) { h->refused_why="the Bluetooth controller failed; reboot required"; return false; }
    if(!h->classic_available) { h->refused_why=host_no_classic; return false; }
    // Until the bond file has been read the link does not know what it
    // holds, and the read, when it lands, would overwrite whatever a
    // command had done meanwhile: a bond just made, or one just forgotten.
    if(!h->classic.bond_loaded) { h->refused_why="the saved bond has not been read yet; try again in a moment"; return false; }
    if(pages && host_setup(h)) { h->refused_why="busy: an LE slot is setting up"; return false; }
    // A dual-mode device is heard at one address as both kinds; the
    // Classic entry is the one this slot is about.
    uint8_t address[6];
    const bt_scan_device_t *classic=NULL,*le=NULL;
    if(host_find_address(p,n,address)) host_heard(h,address,&classic,&le);
    const char *why=NULL;
    bool took=bt_classic_request(&h->classic,p,n,classic?classic:le,now,&why);
    // Forget and disconnect end automatic connection whatever the link
    // says to them (disconnecting a link that is down is refused, and the
    // person still meant "stop").
    if(forget || disconnect) { h->classic_auto=false; h->classic_page_at=0; }
    if(!took) { h->refused_why=why; return false; }
    if(host_word(p,n,"bond") || host_word(p,n,"reconnect")) { h->classic_auto=true; h->classic_page_at=0; }
    return true;
}
bool bt_host_command(bt_host_t *h,const char *p,size_t n,uint64_t now)
{
    h->duplicate_slot=0; h->refused_why=NULL;
    while(n && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) { p++; n--; }
    unsigned slot=0;
    if(n>=7 && p[0]=='s' && p[1]=='l' && p[2]=='o' && p[3]=='t' && p[4]==' ' &&
       p[5]=='0'+BT_HOST_CLASSIC_SLOT && p[6]==' ')
        return host_classic_command_text(h,p+7,n-7,now);
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
        // An address the last scan heard only as Classic belongs to slot 2.
        const bt_scan_device_t *classic=NULL,*le=NULL;
        uint8_t address[6];
        if(valid && host_find_address(p,n,address)) host_heard(h,address,&classic,&le);
        if(classic && !le) { valid=false; h->refused_why="that is a Classic address: use slot 2"; }
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
    int n=snprintf(out,cap,"controller: %s\nstart-up rounds failed: %u of %u\n"
        "last failed start-up round: %s (opcode %04x status %02x)%s\n"
        "ACL framing resyncs: %u\ncommands: slot N COMMAND (default slot 0)\n",
        h->failed?"failed; reboot required":h->initialized?"ready":"initializing",
        h->init_failed_rounds,BT_HOST_INIT_ROUNDS,
        h->round_error?h->round_error:"none",h->round_opcode,h->round_status,
        h->round_timeout?h->round_timeout:"",h->wire_resyncs);
    if(n<0) return 0;
    size_t used=(size_t)n<cap?(size_t)n:cap-1;
    if(used<cap-1) {
        const bt_acl_t *a=&h->acl;
        if(!a->known) n=snprintf(out+used,cap-used,"acl pools: not yet read\n");
        else if(bt_acl_shared(a)) n=snprintf(out+used,cap-used,
            "acl pools: one, classic and le together, %u x %u bytes; in use %u, kept for le %u; "
            "stray completions %u, bad completions %u\n",
            a->classic_total,a->classic_size,bt_acl_in_use(a,false),
            bt_acl_reserved_for_le(a),a->strays,h->acl_bad_completions);
        else n=snprintf(out+used,cap-used,
            "acl pools: classic %u x %u bytes, in use %u; le %u x %u bytes, in use %u; "
            "stray completions %u, bad completions %u\n",
            a->classic_total,a->classic_size,bt_acl_in_use(a,false),
            a->le_total,a->le_size,bt_acl_in_use(a,true),a->strays,h->acl_bad_completions);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->duplicate_slot && used<cap-1) {
        n=snprintf(out+used,cap-used,"last command refused: peer belongs to slot %u\n",h->duplicate_slot-1);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->refused_why && used<cap-1) {
        n=snprintf(out+used,cap-used,"last command refused: %s%s%s\n",h->refused_why,
                   h->refused_why==host_no_classic?": ":"",
                   h->refused_why==host_no_classic && h->classic_unavailable?h->classic_unavailable:"");
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
    if(used>=cap-1) return used;
    // Slot 2. The link's own text never shows a key (bt_classic.h).
    const bt_classic_t *c=&h->classic;
    if(!h->initialized)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: the controller is not ready\n",
                   BT_HOST_CLASSIC_SLOT);
    else if(!h->classic_available && h->classic_setup_refused)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: %s (opcode %04x status %02x)\n",
                   BT_HOST_CLASSIC_SLOT,h->classic_unavailable,h->classic_refused_opcode,h->classic_refused_status);
    else if(!h->classic_available)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: %s\n",
                   BT_HOST_CLASSIC_SLOT,h->classic_unavailable?h->classic_unavailable:"unknown");
    else n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\n",BT_HOST_CLASSIC_SLOT);
    if(n<0) return used;
    used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    if(h->initialized && h->classic_available && used<cap-1) {
        used+=bt_classic_status(c,out+used,cap-used);
        if(used>=cap-1) return used;
        // The link's clock: the time of its last tick.
        uint64_t wait=h->classic_page_at>c->now?h->classic_page_at-c->now:0;
        if(!h->classic_auto) n=snprintf(out+used,cap-used,"automatic: no\n");
        else if(c->phase==BT_CLASSIC_READY) n=snprintf(out+used,cap-used,"automatic: yes; connected\n");
        else if(h->classic_page_at) n=snprintf(out+used,cap-used,"automatic: yes; next page in %lu ms\n",(unsigned long)wait);
        else n=snprintf(out+used,cap-used,"automatic: yes; waiting for the headphones to call\n");
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->classic_storage && used<cap-1) {
        n=snprintf(out+used,cap-used,"bond storage: %s\n",h->classic_storage);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(used<cap-1) {
        n=snprintf(out+used,cap-used,"acl packets received and not read: %u\n",(unsigned)h->classic_rx_unread);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    return used;
}
