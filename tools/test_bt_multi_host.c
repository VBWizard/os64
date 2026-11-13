// Reuse the independently calculated SMP peer and paginated GATT server.
#define main single_peer_suite
#include "test_bt_le_host.c"
#undef main
#include "../kernel/src/driver/system/usb/bt_acl.c"
#include "../kernel/src/driver/system/usb/bt_classic.c"
#include "../kernel/src/driver/system/usb/bt_l2cap.c"
#include "../kernel/src/driver/system/usb/bt_sdp.c"
#include "../kernel/src/driver/system/usb/bt_host.c"
#include "hid_mouse_fixtures.h"

static bt_host_t host;
static unsigned input_count[BT_HOST_PEERS], release_count[BT_HOST_PEERS], reset_count;
static bt_le_input_t input_last[BT_HOST_PEERS];
static void multi_report(void *ctx,unsigned slot,const bt_le_input_t *in)
{
    (void)ctx; assert(slot<BT_HOST_PEERS); input_last[slot]=*in; input_count[slot]++;
    if(in->mouse?!in->pointer.buttons:!in->keys[0] && !in->keys[2]) release_count[slot]++;
}
// The Classic model's state (below): every command the host sends, in
// order, and which of them is still owed its reply.
typedef struct { uint64_t at; uint8_t b[40]; size_t n; } cm_event_t;
static struct {
    cm_event_t q[64]; unsigned qn;
    uint16_t ops[4096]; uint64_t at[4096]; unsigned n;
    bool link[4096];        // sent by the Classic link (it held the slot)
    uint16_t owed;          // the opcode sent and not yet answered, 0 when none
    unsigned classic;       // commands the Classic model was given
    // The controller and the headphones behind it.
    bool present, hold_auth, silent, they_secure, has_key, live;
    uint8_t page_status, scan, key[16], new_key_type;
    uint16_t handle;
    unsigned scan_writes;
    // ACL data on the Classic link: packets the host sent, and how many
    // the controller has not yet completed (Number Of Completed Packets
    // delivered, or the link gone), which the host's count must equal.
    unsigned acl_packets;
    uint16_t acl_out;
    // The controller holding back Number Of Completed Packets for the
    // Classic link, and how many it has held back.
    bool withhold;
    uint16_t withheld;
} CM;
// The headphones' L2CAP and SDP (headphones_acl, below): their end of the
// channel we open to their SDP server, and of the one they open to ours.
typedef struct { uint64_t at; uint8_t b[1100]; size_t n; } hp_packet_t;
static struct {
    hp_packet_t q[32]; unsigned qn;      // ACL packets on their way to the host
    uint8_t ident;
    uint16_t mtu;                        // what their Configuration Requests state
    uint16_t refuse;                     // our Connection Request's answer, 0: accepted
    bool silent;                         // their SDP server answers nothing
    // Ours to theirs: the host's CID, theirs is HP_SERVER_CID.
    uint16_t host_out;
    bool out_open, out_closed;
    unsigned conn_reqs, sdp_requests, disc_reqs;
    uint16_t served;                     // record bytes served for the query in progress
    // Theirs to ours: the host's CID (0 until it accepts), and the result.
    uint16_t host_in, in_result;
    bool in_answered;
    unsigned in_conf_rsps;
    // Their questions to our SDP, and the answers: each answer is compared
    // with bt_sdp_respond's for the same request.
    uint8_t asked[8][64]; size_t asked_n[8]; uint16_t asked_tid[8]; unsigned asks;
    uint16_t answered_tid[8]; unsigned answers;
    uint8_t lists[2048]; size_t lists_n;  // attribute bytes our answers carried
    bool follow;                          // ask for the continuation an answer carries
    unsigned echo_rsps, conn_rsps;
    uint16_t last_conn_result;
    unsigned other;                       // anything else they were sent
} HP;
#define HP_SERVER_CID 0x0070
#define HP_CLIENT_CID 0x0080
#define HP_AVDTP_CID  0x0090
// The transport's send slots: each packet completes `delay` ms after it was
// sent, oldest first, unless `hold` keeps them all.
static struct {
    uint16_t handle[BT_HOST_TX_SLOTS]; uint64_t done[BT_HOST_TX_SLOTS];
    unsigned head, n;
    uint64_t delay;
    bool hold;
    // Per tick: the ACL packets sent, in order, by handle.
    uint16_t sent[8]; unsigned sent_n;
} TX;
static void multi_event(const uint8_t *p,size_t n)
{
    if(n>=6 && (p[0]==0x0e || p[0]==0x0f)) {
        uint16_t op=p[0]==0x0e?le_u16(p+3):le_u16(p+4);
        if(op==CM.owed) CM.owed=0;
    }
    if(p[0]==0x13 && n>=3 && n==3u+4u*p[2])
        for(unsigned i=0;i<p[2];i++)
            if((le_u16(p+3+4*i)&0xfff)==CM.handle && host.classic.connected) {
                // More than is out is a bad count: the host zeroes the link's.
                uint16_t k=le_u16(p+5+4*i);
                CM.acl_out=k<=CM.acl_out?(uint16_t)(CM.acl_out-k):0;
            }
    if(p[0]==0x05 && n==6 && le_u16(p+3)==CM.handle) CM.acl_out=0;
    bt_host_event(&host,p,n);
}
static void multi_wire(const uint8_t *p,size_t n)
{ bt_host_receive(&host,p,n,0,multi_report,NULL); }
static void classic_model(uint16_t op,const uint8_t *p,uint8_t n);
// What the Classic link sends (it holds the slot: host_classic_command set
// the owner before this is called), and the scan's inquiry, go to the
// Classic model; everything else to the LE model, as before.
static bool inquiry_op(uint16_t op) { return op==0x0401 || op==0x0402 || op==0x0c45; }
static void multi_command(void *ctx,uint16_t op,const uint8_t *p,uint8_t n)
{
    bool link=host.owner==BT_HOST_OWNER_CLASSIC;
    if(CM.n<4096) { CM.at[CM.n]=now; CM.link[CM.n]=link; CM.ops[CM.n++]=op; }
    if(op==0x0c03) reset_count++;
    if(link || inquiry_op(op)) {
        // One command at a time: nothing the Classic model takes is sent
        // over another's reply.
        assert(!CM.owed); CM.owed=op;
        classic_model(op,p,n); return;
    }
    if(op!=0x0c03) CM.owed=op;
    if(op==0x0c13) {
        // Write Local Name: 248 bytes, more than the LE model keeps. "os64"
        // and zeroes; only its opcode is kept for whoever answers it.
        assert(n==248 && !memcmp(p,"os64",4));
        for(unsigned i=4;i<n;i++) assert(!p[i]);
        assert(!cmd_n); cmd[0]=(uint8_t)op; cmd[1]=(uint8_t)(op>>8); cmd[2]=n; cmd_n=3; commands++;
        return;
    }
    command(ctx,op,p,n);
}
// The controller-wide count (bt_acl.h) must agree with each peer's own at
// every step of every scenario here: a link for each connected peer, under
// its handle, with as many packets out as the peer believes it has, and
// one for a connected Classic link, with as many out as the controller's
// model has not completed.
static void acl_agrees(void)
{
    unsigned links=0;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        const bt_le_t *s=&host.peers[i].le;
        if(!s->connected) { assert(!host.acl_link[i]); continue; }
        links++;
        assert(host.acl_link[i]==s->handle+1u);
        assert(bt_acl_outstanding(&host.acl,s->handle)==s->outstanding);
    }
    if(host.classic.connected) {
        links++;
        assert(host.acl_classic==host.classic.handle+1u);
        bool found=false;
        for(unsigned i=0;i<BT_ACL_LINKS;i++)
            found|=host.acl.link[i].live && host.acl.link[i].handle==host.classic.handle && !host.acl.link[i].le;
        assert(found && bt_acl_outstanding(&host.acl,host.classic.handle)==CM.acl_out);
    } else assert(!host.acl_classic);
    unsigned live=0;
    for(unsigned i=0;i<BT_ACL_LINKS;i++) live+=host.acl.link[i].live;
    assert(live==links);
}
// What the Classic model owes the host is delivered at the start of a
// step, as the event drain precedes the host's tick in kusb's pass.
static void classic_deliver(void)
{
    for(;;) {
        unsigned best=CM.qn;
        for(unsigned i=0;i<CM.qn;i++) if(CM.q[i].at<=now && (best==CM.qn || CM.q[i].at<CM.q[best].at)) best=i;
        if(best==CM.qn) return;
        cm_event_t e=CM.q[best];
        memmove(&CM.q[best],&CM.q[best+1],(CM.qn-best-1)*sizeof(cm_event_t)); CM.qn--;
        multi_event(e.b,e.n);
    }
}
// What leaves for the controller as ACL data: the Classic link's packets
// go to the headphones' model, the LE peers' to the LE model, as before.
// A Classic packet is one whole L2CAP PDU, first automatically flushable.
// Every packet takes a send slot: one sent with none free is what fails the
// real transport.
static void headphones_acl(const uint8_t *p,size_t n);
static void multi_acl(void *ctx,const uint8_t *p,size_t n)
{
    assert(n>=4 && n==4u+le_u16(p+2));
    uint16_t handle=le_u16(p)&0xfff;
    assert(TX.n<BT_HOST_TX_SLOTS && n<=BT_LE_ACL_MAX+4);
    unsigned slot=(TX.head+TX.n++)%BT_HOST_TX_SLOTS;
    TX.handle[slot]=handle; TX.done[slot]=now+TX.delay;
    if(TX.sent_n<8) TX.sent[TX.sent_n++]=handle;
    if(host.classic.connected && handle==host.classic.handle) {
        assert((le_u16(p)>>12)==2 && n>=8 && le_u16(p+4)==n-8);
        headphones_acl(p,n); return;
    }
    acl(ctx,p,n);
}
// The oldest slots whose transfers have finished, gone.
static void tx_settle(void)
{
    while(TX.n && !TX.hold && TX.done[TX.head]<=now) { TX.head=(TX.head+1)%BT_HOST_TX_SLOTS; TX.n--; }
}
static unsigned tx_holding(uint16_t handle)
{
    unsigned k=0;
    for(unsigned i=0;i<TX.n;i++) k+=TX.handle[(TX.head+i)%BT_HOST_TX_SLOTS]==handle;
    return k;
}
static void headphones_deliver(void);
static void step(void)
{
    ++now; classic_deliver(); headphones_deliver(); tx_settle(); TX.sent_n=0;
    bt_host_tick(&host,now,true,TX.n,false,multi_command,multi_acl,multi_report,NULL); acl_agrees();
}
static void notify_keyboard(void)
{
    const uint8_t a[]={12,0x20,15,0,11,0,4,0,0x1b,10,0,2,0,4,0,0,0,0,0};
    multi_wire(a,sizeof(a));
}
// With `classic_on`, every scenario's host also has its Classic slot, with
// no bond (declared absent, as the transport does once it has read the
// disk), and its one Write Scan Enable(0) already answered: what an LE
// keyboard and mouse share the controller with on a machine that has
// never bonded headphones.
static bool classic_on;
static const uint8_t CM_LOCAL[6]={0x11,0x22,0x33,0x44,0x55,0x66};
static void classic_slot(const bt_classic_bond_t *bond,bool boot_page);
static void setup(void)
{
    begin(REPORT_PROTOCOL); cmd_n=tx_n=0;
    event_sink=multi_event; wire_sink=multi_wire; mouse_map=packed; mouse_map_bytes=sizeof(packed);
    host=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        host.peers[i].le.shared_controller=true; host.peers[i].manager.loaded=true;
        input_count[i]=release_count[i]=0;
    }
    host.peers[0].le=(bt_le_t){.shared_controller=true,.connected=true,.encrypted=true,
        .phase=BT_LE_READY,.handle=12,.input_value=10};
    host.peers[0].le.peer[0]=0x44;
    reset_count=0;
    memset(&CM,0,sizeof(CM)); CM.present=true;
    memset(&HP,0,sizeof(HP)); HP.mtu=48; HP.ident=0x80; HP.follow=true;
    memset(&TX,0,sizeof(TX));
    if(classic_on) {
        bt_classic_bond_t none={0};
        classic_slot(&none,false);
        now=0;
    }
}
static void pump_mouse(void)
{
    for(unsigned i=0;i<1000 && !bt_le_input_active(&host.peers[1].le);i++) {
        notify_keyboard(); step();
        assert(host.peers[0].le.phase==BT_LE_READY && !release_count[0]);
        assert(!host.failed && host.peers[1].le.phase<BT_LE_CLEANUP);
        session=host.peers[1].le; controller();
        session=host.peers[1].le; peripheral();
    }
    assert(bt_le_input_active(&host.peers[1].le) && host.peers[1].le.mouse);
    assert(!reset_count && input_count[0]>20 && !release_count[0]);
}
static void mouse_notification(void)
{
    const uint8_t p[]={0x1b,10,0,3,0xff,0x7f,0x80,0xff,0,0,0};
    incoming(4,p,sizeof(p));
    assert(input_last[1].mouse && input_last[1].pointer.buttons==3);
    assert(input_last[1].pointer.x==-1 && input_last[1].pointer.y==-2041 && input_last[1].pointer.wheel==-1);
}
static void reply(uint16_t op,uint8_t status)
{
    assert(cmd_n && le_u16(cmd)==op); cmd_n=0;
    const uint8_t e[]={0x0e,4,1,op,op>>8,status}; multi_event(e,sizeof(e));
}
static void disconnected(unsigned slot,uint8_t reason)
{
    uint16_t handle=host.peers[slot].le.handle;
    const uint8_t e[]={5,4,0,handle,handle>>8,reason}; multi_event(e,sizeof(e));
}
static void pairing_and_reconnect(void)
{
    setup(); peer_bonding=true; peer_key_delivery=2; peer_pair_response[6]=1;
    const char *request="slot 1 bond-justworks random f8:2c:fe:ff:f0:1a";
    assert(bt_host_command(&host,request,strlen(request),now));
    pump_mouse(); mouse_notification();
    assert(host.peers[1].le.bond.valid && !host.peers[0].le.bond.valid);
    // An unrelated report handle cannot change held buttons.
    unsigned count=input_count[1]; const uint8_t other[]={0x1b,16,0,0,0}; incoming(4,other,sizeof(other));
    assert(input_count[1]==count && input_last[1].pointer.buttons==3);
    // Mouse power loss releases its buttons, then schedules discovery while
    // keyboard notifications keep flowing. Scan commands must not reset it.
    disconnected(1,8); step(); notify_keyboard();
    assert(release_count[1]==1 && !release_count[0]);
    assert(host.peers[1].manager.scan_owned && host.scan.phase==BT_SCAN_LE_PARAMS);
    step(); reply(0x200b,0); step(); reply(0x200c,0);
    step(); now+=2001; step(); reply(0x200c,0);
    host.scan.devices[0]=(bt_scan_device_t){.le=true,.address_type=1};
    memcpy(host.scan.devices[0].address,peer_air,6); host.scan.count=1;
    step(); assert(host.peers[1].le.bond_reused);
    peer_reconnecting=true; peer_bonding=false; peer_key_delivery=0;
    pump_mouse(); mouse_notification();
    // Explicit disconnect addresses just the mouse handle, with no Reset.
    assert(bt_host_command(&host,"slot 1 disconnect",17,now)); step();
    assert(cmd_n && le_u16(cmd)==0x0406 && le_u16(cmd+3)==11);
    const uint8_t ok[]={0x0f,4,0,1,6,4}; cmd_n=0; multi_event(ok,sizeof(ok));
    disconnected(1,0x16); step(); notify_keyboard();
    assert(host.peers[1].le.phase==BT_LE_IDLE && !release_count[0] && !reset_count);
    assert(host.peers[1].manager.suppressed);
    assert(bt_host_command(&host,"slot 1 forget",13,now));
    assert(!host.peers[1].le.bond.valid && !host.peers[1].manager.automatic);
    puts("PASS: mouse bonding, packed motion/buttons/wheel, background saved-key reconnect and disconnect while keyboard input stays live");
}
static void cancel_races(void)
{
    for(unsigned race=0;race<4;race++) {
        setup(); bt_le_t *s=&host.peers[1].le;
        *s=(bt_le_t){.shared_controller=true,.phase=BT_LE_CONNECTING,.link_may_active=true,
            .address_type=1,.deadline=now+20000,.total_deadline=now+30000};
        memcpy(s->peer,peer_air,6);
        assert(bt_host_command(&host,"slot 1 disconnect",17,now)); step();
        assert(cmd_n && le_u16(cmd)==0x200e);
        if(race==2) {
            now+=3001; step(); assert(host.failed && release_count[0]==1); continue;
        }
        reply(0x200e,race==3?0x0c:0);
        uint8_t conn[]={0x3e,19,1,race?0:2,11,0,0,1,0,0,0,0,0,0,24,0,0,0,0x90,1,0};
        memcpy(conn+8,peer_air,6); multi_event(conn,sizeof(conn)); step();
        if(race) {
            assert(cmd_n && le_u16(cmd)==0x0406);
            const uint8_t ok[]={0x0f,4,0,1,6,4}; cmd_n=0; multi_event(ok,sizeof(ok));
            disconnected(1,0x16); step();
        }
        assert(!host.failed && s->phase==BT_LE_IDLE && !reset_count && !release_count[0]);
        notify_keyboard();
    }
    puts("PASS: initiation cancellation, connection racing cancel, and unconfirmed cleanup fails closed");
}
static void credits_and_faults(void)
{
    setup();
    // The first command owns its DMA buffer through both HCI and USB completion.
    host.peers[1].le.phase=BT_LE_ADDRESS; host.peers[1].le.total_deadline=10000;
    step(); assert(host.owner==3 && cmd_n);
    const uint8_t ok[]={0x0e,10,1,9,0x10,0,1,2,3,4,5,6};
    cmd_n=0; multi_event(ok,sizeof(ok));
    bt_host_tick(&host,++now,false,0,false,multi_command,multi_acl,multi_report,NULL);
    assert(!cmd_n && host.owner==3 && host.peers[1].le.pending);
    step(); assert(cmd_n && le_u16(cmd)==0x2002);
    // A truncated mouse report must release that source without inventing motion.
    setup(); host.peers[1].le=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,
        .connected=true,.encrypted=true,.handle=11,.input_value=10,.mouse=true};
    assert(hid_mouse_parse(packed,sizeof(packed),&host.peers[1].le.mouse_layout));
    const uint8_t short_report[]={0x1b,10,0,3}; incoming(4,short_report,sizeof(short_report));
    assert(!input_count[1] && host.peers[1].le.phase==BT_LE_CLEANUP);
    step(); assert(release_count[1]==1 && !release_count[0]);
    // HCI ACL credit is global even with two independent ATT queues.
    setup();
    host.peers[1].le=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,
        .connected=true,.encrypted=true,.handle=11,.input_value=10};
    const uint8_t ack[]={0x1e};
    assert(le_queue(&host.peers[0].le,4,ack,1)); assert(le_queue(&host.peers[1].le,4,ack,1));
    step(); assert(tx_n && host.peers[0].le.outstanding && !host.peers[1].le.outstanding);
    tx_n=0; step(); assert(!tx_n);
    const uint8_t complete[]={0x13,5,1,12,0,1,0}; multi_event(complete,sizeof(complete));
    step(); assert(tx_n && host.peers[1].le.outstanding);
    puts("PASS: shared command ownership, USB lifetime, ACL credits and malformed mouse report isolation");
}
static void rejected_create_and_fragmentation(void)
{
    setup();
    const char *request="slot 1 connect-justworks random f8:2c:fe:ff:f0:1a";
    assert(bt_host_command(&host,request,strlen(request),now));
    step(); session=host.peers[1].le; controller();
    step(); session=host.peers[1].le; controller();
    step(); assert(cmd_n && le_u16(cmd)==0x200d); cmd_n=0;
    const uint8_t reject[]={0x0f,4,0x0c,1,0x0d,0x20}; multi_event(reject,sizeof(reject));
    step(); step(); notify_keyboard();
    assert(!cmd_n && !host.failed && host.peers[1].le.phase==BT_LE_STOPPED);
    assert(!release_count[0] && !reset_count);
    // Duplicate requests are rejected before changing policy or session state.
    setup(); memcpy(host.peers[0].le.peer,peer_air,6); host.peers[0].le.address_type=1;
    host.peers[1].manager.suppressed=true;
    assert(!bt_host_command(&host,request,strlen(request),now));
    assert(host.peers[1].le.phase==BT_LE_IDLE && host.peers[1].manager.suppressed);
    // L2CAP fragments from separate handles can interleave, and USB can split
    // an ACL header at any octet. Both inputs must still reach their own source.
    setup(); host.peers[1].le=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,
        .connected=true,.encrypted=true,.handle=11,.input_value=10,.mouse=true};
    assert(hid_mouse_parse(packed,sizeof(packed),&host.peers[1].le.mouse_layout));
    const uint8_t first[]={11,0x20,6,0,11,0,4,0,0x1b,10};
    const uint8_t last[]={11,0x10,9,0,0,3,0xff,0x7f,0x80,0xff,0,0,0};
    for(size_t i=0;i<sizeof(first);i++) multi_wire(first+i,1);
    notify_keyboard(); assert(input_count[0]==1 && !input_count[1]);
    for(size_t i=0;i<sizeof(last);i++) multi_wire(last+i,1);
    assert(input_count[1]==1 && input_last[1].pointer.x==-1 && input_last[1].pointer.buttons==3);
    // Slot-zero keyboard loss is symmetric: mouse movement remains live.
    disconnected(0,8); step(); mouse_notification();
    assert(release_count[0]==1 && !release_count[1] && !host.failed && !reset_count);
    puts("PASS: rejected initiation and duplicate requests preserve the other peer; interleaved fragments and keyboard loss preserve mouse input");
}
static void saved_duplicate_requests(void)
{
    const char *verbs[]={"bond","bond-justworks","connect","connect-justworks","reconnect"};
    for(unsigned owner=0;owner<BT_HOST_PEERS;owner++) for(unsigned type=0;type<2;type++)
    for(unsigned alias=0;alias<2;alias++) for(unsigned v=0;v<sizeof(verbs)/sizeof(verbs[0]);v++) {
        setup();
        for(unsigned i=0;i<BT_HOST_PEERS;i++) {
            host.peers[i].le=(bt_le_t){.shared_controller=true};
            host.peers[i].manager.suppressed=true;
        }
        unsigned target=1-owner;
        bt_le_bond_t *bond=&host.peers[owner].le.bond;
        *bond=(bt_le_bond_t){.valid=true,.has_irk=!alias,.address_type=type,.last_address_type=type};
        memcpy(bond->peer,peer_air,6); memcpy(bond->last_peer,peer_air,6);
        if(alias) bond->peer[0]^=1;
        // Reconnect also checks the other slot before touching its own saved key.
        host.peers[target].le.bond=(bt_le_bond_t){.valid=true,.address_type=type};
        memcpy(host.peers[target].le.bond.peer,peer_air,6);
        bt_host_peer_t before[BT_HOST_PEERS]; memcpy(before,host.peers,sizeof(before));
        char request[96];
        if(v==4) snprintf(request,sizeof(request),"slot %u reconnect",target);
        else snprintf(request,sizeof(request),"slot %u %s %s f8:2c:fe:ff:f0:1a",target,verbs[v],type?"random":"public");
        assert(!bt_host_command(&host,request,strlen(request),now));
        assert(host.duplicate_slot==owner+1 && !memcmp(before,host.peers,sizeof(before)) && !cmd_n);
        char output[4096],diagnostic[80]; bt_host_status(&host,output,sizeof(output));
        snprintf(diagnostic,sizeof(diagnostic),"last command refused: peer belongs to slot %u",owner);
        assert(strstr(output,diagnostic));
        // Equal bytes with a different address type identify a different peer.
        bond->address_type=1-type; bond->last_address_type=1-type;
        assert(bt_host_command(&host,request,strlen(request),now) && !host.duplicate_slot);
    }
    // Default slot zero must not overwrite an asleep slot-one mouse's bond.
    setup(); host.peers[0].le=(bt_le_t){.shared_controller=true};
    host.peers[1].le.bond=(bt_le_bond_t){.valid=true,.address_type=1};
    memcpy(host.peers[1].le.bond.peer,peer_air,6);
    const char *request="bond random f8:2c:fe:ff:f0:1a";
    assert(!bt_host_command(&host,request,strlen(request),now) && host.duplicate_slot==2);
    assert(bt_host_command(&host,"slot 1 forget",13,now));
    assert(bt_host_command(&host,request,strlen(request),now) && !host.duplicate_slot);
    puts("PASS: offline bond identity/air-address duplicates refuse without mutation, identify the owning slot and allow explicit forget");
}
static void scan_recovery(void)
{
    setup(); assert(bt_host_scan(&host,now)); step();
    reply(0x200b,0x0c); step(); step();
    assert(cmd_n && le_u16(cmd)==0x200c && !cmd[3]); reply(0x200c,0); step();
    assert(host.scan.phase==BT_SCAN_STOPPED && !host.failed && !reset_count);
    notify_keyboard(); assert(!release_count[0]);
    // Opcode zero can replenish command credits without a return-status byte.
    const uint8_t credit[]={0x0e,3,1,0,0}; multi_event(credit,sizeof(credit));
    assert(!host.failed && host.credits==1);
    host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}};
    step(); assert(host.init_retry_at);
    now=host.init_retry_at; step(); assert(cmd_n && le_u16(cmd)==0x0c03);
    puts("PASS: shared scan cleanup disables scanning without Reset; initial reset-confirmed setup failures remain retryable");
}
// The start-up round's reset and the reset that cleans up after it can both
// go unanswered (the P5 after a warm reboot). That is not the boot: the
// round is run again, with the command credit no reply gave back, and the
// host is failed only when BT_HOST_INIT_ROUNDS have ended that way.
static void start_up_rounds(void)
{
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}}; reset_count=0;
    for(unsigned round=1;round<=BT_HOST_INIT_ROUNDS;round++) {
        step(); assert(host.init_retry_at && !host.failed);
        now=host.init_retry_at; cmd_n=0; step(); assert(cmd_n && le_u16(cmd)==0x0c03);
        assert(reset_count==2*round-1);
        now+=2000; step();                       // unanswered: the round turns to cleaning up
        assert(host.scan.phase==BT_SCAN_CLEANUP && !host.failed);
        cmd_n=0; step(); assert(reset_count==2*round);    // whose own reset goes out
        now+=2000; step();                       // and is unanswered too
        assert(host.init_failed_rounds==round);
        assert(host.failed==(round==BT_HOST_INIT_ROUNDS));
        if(!host.failed) assert(host.scan.phase==BT_SCAN_STOPPED && host.credits==1);
    }
    char text[4096]; bt_host_status(&host,text,sizeof(text));
    assert(strstr(text,"controller: failed; reboot required\nstart-up rounds failed: 3 of 3\n"));

    // A round that fails once and is answered the second time goes on.
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}}; reset_count=0;
    step(); now=host.init_retry_at; cmd_n=0; step();
    now+=2000; step(); cmd_n=0; step(); now+=2000; step();
    assert(host.init_failed_rounds==1 && !host.failed);
    step(); now=host.init_retry_at; cmd_n=0; step(); assert(cmd_n && le_u16(cmd)==0x0c03);
    reply(0x0c03,0); cmd_n=0; step();
    assert(!host.failed && host.scan.phase>BT_SCAN_RESET && host.scan.phase<BT_SCAN_CLEANUP);
    assert(cmd_n && le_u16(cmd)==0x1003);        // the round moved on to its next command

    // What a failed round died of outlives the round that follows it.
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}};
    step(); now=host.init_retry_at; cmd_n=0; step();
    now+=2000; step(); cmd_n=0; step(); now+=2000; step();
    step(); now=host.init_retry_at; cmd_n=0; step();     // round two is under way
    assert(!host.scan.error && host.init_failed_rounds==1);
    bt_host_status(&host,text,sizeof(text));
    assert(strstr(text,"last failed start-up round: HCI command timed out (opcode 0c03 status 00); the USB transfer completed, no reply came\n"));

    // An event that cannot be right, during start-up, fails the ROUND: the
    // assembler can deliver one while it is finding its place after a tail.
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}};
    step(); now=host.init_retry_at; cmd_n=0; step(); assert(cmd_n && le_u16(cmd)==0x0c03);
    const uint8_t hardware[]={0x10,1,0x42}, runt[]={0x0e,1,1}, wrong[]={0x0f,3,0,1,3};
    multi_event(hardware,sizeof(hardware)); assert(!host.failed && host.scan.hardware_error);
    host.scan.hardware_error=false;
    multi_event(runt,sizeof(runt)); assert(!host.failed && host.scan.bad_reply);
    host.scan.bad_reply=false;
    multi_event(wrong,sizeof(wrong)); assert(!host.failed && host.scan.bad_reply);
    step(); assert(host.scan.phase==BT_SCAN_CLEANUP && !host.failed);     // the round fails, the host does not
    // Between rounds there is nothing for it to belong to.
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}};
    multi_event(hardware,sizeof(hardware)); assert(!host.failed && !host.scan.hardware_error);
    // Once the controller is up, the same event is the end of the host.
    setup(); multi_event(hardware,sizeof(hardware)); assert(host.failed);
    setup(); multi_event(runt,sizeof(runt)); assert(host.failed);

    // A failed transport is not a round to repeat.
    setup(); host=(bt_host_t){.credits=1,.scan={.phase=BT_SCAN_STOPPED}};
    step(); now=host.init_retry_at; cmd_n=0; step();
    bt_host_tick(&host,++now,true,0,true,multi_command,multi_acl,multi_report,NULL);
    assert(host.failed && host.init_failed_rounds==0);
    puts("PASS: an unanswered start-up round is run again with its credit restored, three failures fail the host, a failed transport is not retried");
}
// The ACL assembler gets back in step at transfer boundaries when it is fed
// packets, and a header that cannot be real costs its transfer, not the host.
static void acl_resync(void)
{
    enum { MPS=64 };
    setup();
    const uint8_t report[]={12,0x20,15,0,11,0,4,0,0x1b,10,0,2,0,4,0,0,0,0,0};
    // A tail left over from before this host took the adapter: three bytes.
    const uint8_t tail[]={0x55,0x66,0x77};
    bt_host_receive(&host,tail,sizeof(tail),MPS,multi_report,NULL);
    assert(host.wire_used==0 && host.wire_resyncs==1 && !host.failed);
    bt_host_receive(&host,report,sizeof(report),MPS,multi_report,NULL);
    assert(input_count[0]==1 && host.wire_resyncs==1 && !host.failed);
    // A tail long enough to read as a whole header with a believable
    // length: the transfer still ended, inside what it called a body.
    const uint8_t longer[]={0x55,0x66,0x10,0x00,0x77};
    setup();
    bt_host_receive(&host,longer,sizeof(longer),MPS,multi_report,NULL);
    assert(host.wire_used==0 && host.wire_need==0 && host.wire_resyncs==1 && !host.failed);
    bt_host_receive(&host,report,sizeof(report),MPS,multi_report,NULL);
    assert(input_count[0]==1);
    // The byte-stream form would have read the tail as part of a header.
    setup();
    bt_host_receive(&host,tail,sizeof(tail),0,multi_report,NULL);
    bt_host_receive(&host,report,sizeof(report),0,multi_report,NULL);
    assert(input_count[0]==0);
    // A length that cannot be real: the stream form fails the host, the
    // packet form skips to the end of the transfer and carries on.
    uint8_t wild[MPS]; memset(wild,0xff,sizeof(wild));
    setup(); bt_host_receive(&host,wild,sizeof(wild),0,multi_report,NULL); assert(host.failed);
    setup(); bt_host_receive(&host,wild,sizeof(wild),MPS,multi_report,NULL);
    assert(!host.failed && host.wire_skip && host.wire_resyncs==1 && host.wire_used==0);
    bt_host_receive(&host,wild,sizeof(wild),MPS,multi_report,NULL);     // still that transfer
    assert(host.wire_skip && host.wire_resyncs==1);
    bt_host_receive(&host,wild,5,MPS,multi_report,NULL);                // its short last packet
    assert(!host.wire_skip && !host.failed);
    bt_host_receive(&host,report,sizeof(report),MPS,multi_report,NULL);
    assert(input_count[0]==1);
    // A wild length in a short packet is a whole transfer already.
    setup(); bt_host_receive(&host,wild,8,MPS,multi_report,NULL);
    assert(!host.failed && !host.wire_skip && host.wire_resyncs==1);
    // The packet form drops nothing the stream form would deliver: two ACL
    // packets in one USB packet are both routed.
    uint8_t two[2*sizeof(report)]; memcpy(two,report,sizeof(report)); memcpy(two+sizeof(report),report,sizeof(report));
    setup(); bt_host_receive(&host,two,sizeof(two),MPS,multi_report,NULL);
    assert(input_count[0]==2 && host.wire_resyncs==0 && host.wire_used==0);
    // A packet longer than one USB packet still assembles: full, then short.
    uint8_t big[4+4+70]={12,0x20,74,0,70,0,4,0,0x1b,10,0,2,0,4};
    setup();
    bt_host_receive(&host,big,MPS,MPS,multi_report,NULL);
    assert(host.wire_used==MPS && host.wire_resyncs==0);
    bt_host_receive(&host,big+MPS,sizeof(big)-MPS,MPS,multi_report,NULL);
    assert(host.wire_used==0 && host.wire_resyncs==0 && !host.failed);
    char text[4096]; bt_host_status(&host,text,sizeof(text));
    assert(strstr(text,"ACL framing resyncs: 0\n"));
    puts("PASS: ACL assembly resyncs at transfer boundaries, skips an impossible length, and still spans packets");
}
static void peer_att_requests(void)
{
    setup(); bt_le_t *s=&host.peers[1].le;
    *s=(bt_le_t){.shared_controller=true,.phase=BT_LE_SERVICES,.connected=true,.encrypted=true,
        .handle=11,.cursor=1,.att_pending=true,.att_opcode=0x10,.deadline=1234};
    const uint8_t request[]={0x10,1,0,0xff,0xff,0,0x28};
    incoming(4,request,sizeof(request));
    assert(s->phase==BT_LE_SERVICES && !s->malformed && s->att_pending && s->deadline==1234);
    assert(s->queue_count==1);
    const uint8_t error[]={1,0x10,0,0,6};
    assert(s->queue_bytes[s->queue_head]==13 && !memcmp(s->queue[s->queue_head]+8,error,sizeof(error)));
    notify_keyboard(); assert(input_count[0]==1 && !release_count[0]);
    const uint8_t services[]={0x11,6,1,0,30,0,0x12,0x18};
    incoming(4,services,sizeof(services));
    assert(s->phase==BT_LE_CHARACTERISTICS && !s->att_pending && s->service_start==1);
    // Server requests while idle and commands without responses must not consume
    // a client transaction or manufacture a disconnect.
    const uint8_t read[]={0x0a,1,0}; incoming(4,read,sizeof(read));
    assert(s->queue_count==2 && !s->malformed && !s->att_pending);
    const uint8_t invalid_handle[]={1,0x0a,1,0,1};
    assert(!memcmp(s->queue[(s->queue_head+1)%BT_LE_QUEUE]+8,invalid_handle,sizeof(invalid_handle)));
    const uint8_t write_command[]={0x52,1,0,0xaa}; incoming(4,write_command,sizeof(write_command));
    assert(s->queue_count==2 && !s->malformed);
    const uint8_t find[]={0x04,1,0,0xff,0xff};
    const uint8_t absent[]={1,0x04,1,0,0x0a};
    s->queue_count=0; incoming(4,find,sizeof(find));
    assert(s->queue_count==1 && !memcmp(s->queue[s->queue_head]+8,absent,sizeof(absent)));
    const uint8_t bad_range[]={0x04,2,0,1,0};
    const uint8_t bad_range_error[]={1,0x04,2,0,1};
    s->queue_count=0; incoming(4,bad_range,sizeof(bad_range));
    assert(s->queue_count==1 && !memcmp(s->queue[s->queue_head]+8,bad_range_error,sizeof(bad_range_error)));
    const uint8_t bad_length[]={1,0x0a,0,0,4};
    s->queue_count=0; incoming(4,read,1);
    assert(s->queue_count==1 && !memcmp(s->queue[s->queue_head]+8,bad_length,sizeof(bad_length)));
    assert(s->phase==BT_LE_CHARACTERISTICS && !s->malformed && !s->att_pending);
    puts("PASS: peer ATT requests receive a server error without consuming host discovery or disturbing keyboard input");
}
static void mouse_interval(void)
{
    for(unsigned failure=0;failure<4;failure++) {
        setup(); bt_le_t *s=&host.peers[1].le;
        host.peers[0].le.interval=36;
        *s=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,.connected=true,.encrypted=true,
            .mouse=true,.handle=11,.input_value=10,.interval=36,.supervision_timeout=400};
        assert(hid_mouse_parse(packed,sizeof(packed),&s->mouse_layout));
        host.credits=0; step(); assert(!cmd_n && !s->mouse_update_sent);
        host.credits=1; step();
        assert(cmd_n && le_u16(cmd)==0x2013 && cmd[2]==14);
        assert(le_u16(cmd+3)==11 && le_u16(cmd+5)==6 && le_u16(cmd+7)==9);
        assert(le_u16(cmd+9)==0 && le_u16(cmd+11)==400);
        assert(!le_u16(cmd+13) && !le_u16(cmd+15));
        notify_keyboard(); mouse_notification();
        assert(!release_count[0] && !release_count[1] && !reset_count);
        cmd_n=0;
        uint8_t status[]={0x0f,4,failure==1?0x0c:0,1,0x13,0x20}; multi_event(status,sizeof(status));
        uint8_t updated[]={0x3e,10,3,failure==2?0x3b:0,11,0,9,0,0,0,0x90,1};
        if(failure!=1 && failure!=3) multi_event(updated,sizeof(updated));
        // Command status can precede USB completion and an inspect request.
        assert(bt_host_command(&host,"slot 1 inspect",14,now));
        bt_host_tick(&host,++now,false,0,false,multi_command,multi_acl,multi_report,NULL);
        assert(s->pending && host.owner==3 && !cmd_n);
        s->phase=BT_LE_READY; step();
        assert(!s->pending && !host.owner && !cmd_n && !s->error && !host.failed);
        assert(s->interval==(failure?36:9) && s->connection_updates==(!failure || failure==2));
        assert(s->update_status==(failure==1?0x0c:failure==2?0x3b:0));
        assert(host.peers[0].le.interval==36 && !host.peers[0].le.connection_updates);
        updated[4]=13; multi_event(updated,sizeof(updated));
        assert(host.peers[0].le.interval==36 && s->interval==(failure?36:9));
        now+=30000; step(); assert(!cmd_n && s->phase==BT_LE_READY);
        notify_keyboard(); mouse_notification();
        assert(!release_count[0] && !release_count[1]);
        char output[4096]; bt_le_status(s,output,sizeof(output));
        assert(strstr(output,"timing update: mouse requested=yes"));
    }
    puts("PASS: mouse-only short interval request, per-handle completion, USB ownership and nonfatal timing refusal preserve both inputs");
}
// ── The Classic link beside the LE peers (AUDIO_CLASSIC.md part L) ─────────
//
// A model controller and headphones answer the Classic link's commands
// through the same host the keyboard and mouse use: their events arrive
// through bt_host_event, their commands leave through the one command slot.

static const uint8_t PH[6]={0xa1,0xb2,0xc3,0x5d,0x87,0x4c};    // 4c:87:5d:c3:b2:a1
static const char PH_TEXT[]="4c:87:5d:c3:b2:a1";
static const uint8_t PH_KEY[16]={0x9a,0x7e,0x31,0xc4,0x58,0xe2,0x0f,0xb6,0x73,0xd9,0x2c,0x84,0xf1,0x46,0xab,0x1d};
static const uint8_t PH_NEW_KEY[16]={0x5e,0xc7,0x02,0x9b,0xe4,0x38,0x71,0xad,0x16,0xf3,0x8c,0x4f,0xd0,0x67,0xba,0x29};
static void cm_push(uint64_t delay,const uint8_t *b,size_t n)
{
    assert(CM.qn<64 && n<=sizeof(CM.q[0].b));
    cm_event_t *e=&CM.q[CM.qn++]; e->at=now+delay; e->n=n; memcpy(e->b,b,n);
}
#define CM_PUSH(delay,...) do { const uint8_t b_[]={__VA_ARGS__}; cm_push((delay),b_,sizeof b_); } while(0)
static void cm_cs(uint64_t d,uint8_t status,uint16_t op) { CM_PUSH(d,0x0f,4,status,1,(uint8_t)op,(uint8_t)(op>>8)); }
static void cm_cc_addr(uint64_t d,uint16_t op,const uint8_t *a)
{ CM_PUSH(d,0x0e,10,1,(uint8_t)op,(uint8_t)(op>>8),0,a[0],a[1],a[2],a[3],a[4],a[5]); }
static void cm_cc(uint64_t d,uint16_t op,uint8_t status) { CM_PUSH(d,0x0e,4,1,(uint8_t)op,(uint8_t)(op>>8),status); }
static void cm_conn(uint64_t d,uint8_t status,uint16_t h,const uint8_t *a)
{ CM_PUSH(d,0x03,11,status,(uint8_t)h,(uint8_t)(h>>8),a[0],a[1],a[2],a[3],a[4],a[5],1,0); }
static void cm_addr_event(uint64_t d,uint8_t code,const uint8_t *a) { CM_PUSH(d,code,6,a[0],a[1],a[2],a[3],a[4],a[5]); }
static void cm_request(uint64_t d,const uint8_t *a)
{ CM_PUSH(d,0x04,10,a[0],a[1],a[2],a[3],a[4],a[5],0x18,0x04,0x24,1); }
static void cm_auth(uint64_t d,uint8_t status) { CM_PUSH(d,0x06,3,status,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8)); }
static void cm_enc(uint64_t d) { CM_PUSH(d,0x08,4,0,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),1); }
static void cm_disconn(uint64_t d,uint8_t reason)
{ CM_PUSH(d,0x05,4,0,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),reason); }
static bool cm_ours(const uint8_t *p,uint8_t n) { return n>=6 && !memcmp(p,PH,6); }
static void classic_model(uint16_t op,const uint8_t *p,uint8_t n)
{
    CM.classic++;
    // Who may send which: the Classic link's own commands only while it
    // holds the slot, the scan's inquiry only while the scan does.
    if(inquiry_op(op)) assert(host.owner==BT_HOST_OWNER_SCAN);
    else assert(host.owner==BT_HOST_OWNER_CLASSIC && op!=0x0c03);
    if(CM.silent) return;
    switch(op) {
    case 0x0c1a: assert(n==1); CM.scan=p[0]; CM.scan_writes++; cm_cc(1,op,0); break;
    case 0x0405:
        assert(n==13 && cm_ours(p,n)); cm_cs(1,0,op);
        if(CM.present) { CM.live=true; CM.handle=0x40; cm_conn(30,0,CM.handle,PH); }
        else cm_conn(100,CM.page_status?CM.page_status:0x04,0,PH);
        break;
    case 0x0408: cm_cc_addr(1,op,p); break;
    case 0x0406:
        assert(n==3); cm_cs(1,0,op);
        if(CM.live && le_u16(p)==CM.handle) { cm_disconn(10,0x16); CM.live=false; }
        break;
    case 0x0409:
        assert(n==7 && cm_ours(p,n)); cm_cs(1,0,op);
        CM.live=true; CM.handle=0x41; cm_conn(20,0,CM.handle,PH);
        CM_PUSH(25,0x12,8,0,PH[0],PH[1],PH[2],PH[3],PH[4],PH[5],1);
        if(CM.they_secure) cm_addr_event(30,0x17,PH);
        break;
    case 0x040a: case 0x042a:
        assert(n==7); cm_cs(1,0,op); CM_PUSH(20,0x03,11,p[6],0,0,p[0],p[1],p[2],p[3],p[4],p[5],op==0x040a?1:0,0); break;
    case 0x040b:
        assert(n==22); cm_cc_addr(1,op,p);
        if(!cm_ours(p,n)) break;
        if(CM.has_key && !memcmp(p+6,CM.key,16)) { if(CM.they_secure) cm_enc(20); else cm_auth(20,0); }
        else cm_auth(20,0x06);
        break;
    case 0x040c: cm_cc_addr(1,op,p); if(cm_ours(p,n)) cm_addr_event(10,0x31,PH); break;
    case 0x042b:
        assert(n==9 && p[6]==0x03); cm_cc_addr(1,op,p);
        CM_PUSH(5,0x32,9,PH[0],PH[1],PH[2],PH[3],PH[4],PH[5],0x03,0x00,0x04);
        CM_PUSH(10,0x33,10,PH[0],PH[1],PH[2],PH[3],PH[4],PH[5],0x40,0xe2,0x01,0x00);
        break;
    case 0x042c: {
        cm_cc_addr(1,op,p);
        CM_PUSH(10,0x36,7,0,PH[0],PH[1],PH[2],PH[3],PH[4],PH[5]);
        uint8_t k[25]={0x18,23}; memcpy(k+2,PH,6); memcpy(k+8,PH_NEW_KEY,16); k[24]=CM.new_key_type?CM.new_key_type:4;
        cm_push(12,k,sizeof k);
        CM.has_key=true; memcpy(CM.key,PH_NEW_KEY,16);
        if(CM.they_secure) cm_enc(20); else cm_auth(20,0);
        break;
    }
    case 0x040e: case 0x042d: case 0x0434: cm_cc_addr(1,op,p); break;
    case 0x0411: assert(n==2); cm_cs(1,0,op); if(!CM.hold_auth) cm_addr_event(10,0x17,PH); break;
    case 0x0413: assert(n==3 && p[2]==1); cm_cs(1,0,op); cm_enc(10); break;
    case 0x1408: CM_PUSH(1,0x0e,7,1,0x08,0x14,0,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),16); break;
    case 0x0c45: cm_cc(1,op,0); break;
    case 0x0401:
        cm_cs(1,0,op);
        CM_PUSH(50,2,15,1,PH[0],PH[1],PH[2],PH[3],PH[4],PH[5],1,0,0,0x04,0x04,0x24,0,0);
        CM_PUSH(100,1,1,0);
        break;
    case 0x0402: cm_cc(1,op,0); break;
    default: printf("unexpected Classic command %04x\n",op); assert(0);
    }
}
// ── The headphones' L2CAP and SDP ───────────────────────────────────────────
//
// Written here from the specification, not shared with bt_l2cap.c's or
// bt_sdp.c's own suites (their peers keep their state in globals named for
// those suites): Connection, Configuration and Disconnection on both kinds
// of channel, Echo, an SDP server holding a canned Audio Sink record served
// in parts with continuation, and a client asking our SDP.

// Their Audio Sink record, as AttributeLists: one record, 68 bytes.
static const uint8_t HP_RECORD[]={
    0x35,0x42, 0x35,0x40,
    0x09,0x00,0x00, 0x0a,0x00,0x01,0x00,0x03,                          // ServiceRecordHandle
    0x09,0x00,0x01, 0x35,0x03,0x19,0x11,0x0b,                          // ServiceClassIDList: Audio Sink
    0x09,0x00,0x04, 0x35,0x10, 0x35,0x06,0x19,0x01,0x00,0x09,0x00,0x19,  // L2CAP, PSM 0x0019
                    0x35,0x06,0x19,0x00,0x19,0x09,0x01,0x03,           // AVDTP 1.3
    0x09,0x00,0x05, 0x35,0x03,0x19,0x10,0x02,                          // BrowseGroupList
    0x09,0x00,0x09, 0x35,0x08,0x35,0x06,0x19,0x11,0x0d,0x09,0x01,0x03, // A2DP 1.3
    0x09,0x03,0x11, 0x09,0x00,0x03,                                    // SupportedFeatures
};
_Static_assert(sizeof(HP_RECORD)==68,"the record's lengths are right");
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0]<<8 | p[1]); }
// One L2CAP PDU from the headphones, as one ACL packet, delivered at the
// next step's start (the drain precedes the tick).
static void hp_pdu(uint16_t cid,const uint8_t *d,size_t n)
{
    assert(HP.qn<32 && 8+n<=sizeof(HP.q[0].b));
    hp_packet_t *q=&HP.q[HP.qn++];
    uint16_t h=CM.handle|0x2000;
    q->b[0]=(uint8_t)h; q->b[1]=(uint8_t)(h>>8); q->b[2]=(uint8_t)(n+4); q->b[3]=(uint8_t)((n+4)>>8);
    q->b[4]=(uint8_t)n; q->b[5]=(uint8_t)(n>>8); q->b[6]=(uint8_t)cid; q->b[7]=(uint8_t)(cid>>8);
    memcpy(q->b+8,d,n); q->n=8+n; q->at=now+1;
}
static void hp_cmd(uint8_t code,uint8_t ident,const uint8_t *d,size_t n)
{
    uint8_t c[1100]={code,ident,(uint8_t)n,(uint8_t)(n>>8)};
    memcpy(c+4,d,n); hp_pdu(1,c,4+n);
}
#define HP_CMD(code,ident,...) do { const uint8_t d_[]={__VA_ARGS__}; hp_cmd((code),(ident),d_,sizeof d_); } while(0)
#define W16(x) (uint8_t)(x),(uint8_t)((x)>>8)
static void hp_config(uint16_t host_cid) { HP_CMD(4,HP.ident++,W16(host_cid),0,0,1,2,W16(HP.mtu)); }
static void headphones_deliver(void)
{
    unsigned kept=0;
    for(unsigned i=0;i<HP.qn;i++) {
        if(HP.q[i].at<=now) multi_wire(HP.q[i].b,HP.q[i].n);
        else HP.q[kept++]=HP.q[i];
    }
    HP.qn=kept;
}
// They open a channel to our SDP, or to AVDTP.
static void hp_connect(uint16_t psm,uint16_t cid) { HP_CMD(2,HP.ident++,W16(psm),W16(cid)); }
// Their question to our SDP: Service Search Attribute for `uuid`, every
// attribute, at most `max` bytes, with a continuation state.
static void hp_ask(uint16_t tid,uint16_t uuid,uint16_t max,const uint8_t *cont,uint8_t cont_n)
{
    assert(HP.asks<8 && HP.host_in);
    uint8_t *q=HP.asked[HP.asks];
    size_t k=0;
    q[k++]=0x06; q[k++]=(uint8_t)(tid>>8); q[k++]=(uint8_t)tid; q[k++]=0; q[k++]=0;
    q[k++]=0x35; q[k++]=3; q[k++]=0x19; q[k++]=(uint8_t)(uuid>>8); q[k++]=(uint8_t)uuid;
    q[k++]=(uint8_t)(max>>8); q[k++]=(uint8_t)max;
    q[k++]=0x35; q[k++]=5; q[k++]=0x0a; q[k++]=0; q[k++]=0; q[k++]=0xff; q[k++]=0xff;
    q[k++]=cont_n; if(cont_n) memcpy(q+k,cont,cont_n);
    k+=cont_n;
    q[4]=(uint8_t)(k-5);
    HP.asked_n[HP.asks]=k; HP.asked_tid[HP.asks]=tid; HP.asks++;
    hp_pdu(HP.host_in,q,k);
}
// Our client's request on the channel to their server: checked field by
// field, answered with the next part of the record.
static void hp_serve(const uint8_t *q,size_t n)
{
    HP.sdp_requests++;
    assert(n>=20 && q[0]==0x06 && be16(q+3)==n-5);
    static const uint8_t middle[]={0x35,0x03,0x19,0x11,0x0b};
    static const uint8_t range[]={0x35,0x05,0x0a,0x00,0x00,0xff,0xff};
    assert(!memcmp(q+5,middle,5) && !memcmp(q+12,range,7));
    uint16_t max=be16(q+10);
    uint8_t cont=q[19];
    assert(n==20u+cont && (cont==0 || cont==2));
    uint16_t from=cont?be16(q+20):0;
    assert(from==HP.served);
    if(HP.silent) return;
    uint16_t count=(uint16_t)(sizeof(HP_RECORD)-from); if(count>max) count=max;
    HP.served=(uint16_t)(from+count);
    bool more=HP.served<sizeof(HP_RECORD);
    uint8_t r[1100]; size_t k=0;
    r[k++]=0x07; r[k++]=q[1]; r[k++]=q[2]; k+=2;
    r[k++]=(uint8_t)(count>>8); r[k++]=(uint8_t)count;
    memcpy(r+k,HP_RECORD+from,count); k+=count;
    if(more) { r[k++]=2; r[k++]=(uint8_t)(HP.served>>8); r[k++]=(uint8_t)HP.served; } else r[k++]=0;
    r[3]=(uint8_t)((k-5)>>8); r[4]=(uint8_t)(k-5);
    assert(k<=BT_L2CAP_MTU);
    hp_pdu(HP.host_out,r,k);
}
// Our answer on the channel they opened to our SDP: byte for byte what
// bt_sdp_respond gives for the question it answers, within their MTU;
// its attribute bytes kept, and the continuation asked for.
static void hp_answered(const uint8_t *r,size_t n)
{
    assert(n>=5 && HP.answers<HP.asks && HP.answers<8);
    unsigned a=HP.answers++;
    uint8_t want[1100];
    size_t room=HP.mtu<BT_L2CAP_MTU?HP.mtu:BT_L2CAP_MTU;
    size_t w=bt_sdp_respond(HP.asked[a],HP.asked_n[a],want,room);
    assert(w==n && !memcmp(want,r,n) && n<=HP.mtu);
    HP.answered_tid[a]=be16(r+1);
    if(r[0]!=0x07) return;
    uint16_t count=be16(r+5);
    assert(HP.lists_n+count<=sizeof(HP.lists));
    memcpy(HP.lists+HP.lists_n,r+7,count); HP.lists_n+=count;
    uint8_t cont=r[7+count];
    if(cont && HP.follow) hp_ask((uint16_t)(be16(r+1)+1),be16(HP.asked[a]+8),be16(HP.asked[a]+10),r+8+count,cont);
}
// What the host sends them on the Classic link.
static void headphones_acl(const uint8_t *p,size_t n)
{
    CM.acl_packets++; CM.acl_out++;
    // The controller completes it a step later, unless it is told not to.
    if(CM.withhold) CM.withheld++;
    else CM_PUSH(1,0x13,5,1,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),1,0);
    uint16_t cid=le_u16(p+6);
    const uint8_t *d=p+8;
    size_t len=n-8;
    if(cid==HP_SERVER_CID) { hp_serve(d,len); return; }
    if(cid==HP_CLIENT_CID) { hp_answered(d,len); return; }
    assert(cid==1);
    while(len) {
        assert(len>=4 && le_u16(d+2)<=len-4);
        uint8_t code=d[0], ident=d[1];
        uint16_t m=le_u16(d+2);
        const uint8_t *x=d+4;
        switch(code) {
        case 2:     // our Connection Request: to their SDP, the one PSM this part asks for
            assert(m==4 && le_u16(x)==BT_L2CAP_PSM_SDP);
            HP.conn_reqs++; HP.host_out=le_u16(x+2); HP.served=0; HP.out_open=HP.out_closed=false;
            if(HP.refuse) { HP_CMD(3,ident,0,0,W16(HP.host_out),W16(HP.refuse),0,0); break; }
            HP_CMD(3,ident,W16(HP_SERVER_CID),W16(HP.host_out),0,0,0,0);
            hp_config(HP.host_out);
            break;
        case 3:     // the answer to their Connection Request
            assert(m==8);
            HP.conn_rsps++; HP.last_conn_result=le_u16(x+4);
            if(le_u16(x+2)==HP_CLIENT_CID) {
                HP.in_result=le_u16(x+4); HP.in_answered=true;
                if(!HP.in_result) { HP.host_in=le_u16(x); hp_config(HP.host_in); }
            }
            break;
        case 4: {   // our Configuration Request: our MTU, accepted
            assert(m==8 && x[4]==1 && x[5]==2 && le_u16(x+6)==BT_L2CAP_MTU);
            uint16_t theirs=le_u16(x);
            uint16_t ours=theirs==HP_SERVER_CID?HP.host_out:HP.host_in;
            HP_CMD(5,ident,W16(ours),0,0,0,0);
            break;
        }
        case 5:     // the answer to theirs
            assert(m>=6 && !le_u16(x+4));
            if(le_u16(x)==HP_SERVER_CID) HP.out_open=true; else HP.in_conf_rsps++;
            break;
        case 6:     // our Disconnection Request
            assert(m==4);
            HP.disc_reqs++;
            if(le_u16(x)==HP_SERVER_CID) HP.out_closed=true;
            hp_cmd(7,ident,x,4);
            break;
        case 9: HP.echo_rsps++; break;
        default: HP.other++; break;
        }
        d+=4u+m; len-=4u+m;
    }
}
// The LE model answers what it knows; the scan's two commands are
// answered here, as scan_recovery does.
static void answer_le(void)
{
    if(!cmd_n) return;
    uint16_t op=le_u16(cmd);
    if(op==0x200b || op==0x200c) reply(op,0); else controller();
}
static unsigned classic_ops_since(unsigned from)
{
    unsigned k=0;
    for(unsigned i=from;i<CM.n;i++) k+=CM.link[i];
    return k;
}
static int op_index(uint16_t op,unsigned from)
{
    for(unsigned i=from;i<CM.n;i++) if(CM.ops[i]==op) return (int)i;
    return -1;
}
// The host as the start-up round leaves it, with its Classic slot, and
// the bond the transport read (`bond->valid` false: none); stepped until
// the link's page scan write is answered. Without `boot_page` the page a
// loaded bond is owed is taken back first.
static bool shared_pool;
static void classic_slot(const bt_classic_bond_t *bond,bool boot_page)
{
    memcpy(host.scan.address,CM_LOCAL,6); host.scan.features[4]=0x40;
    // The AX210's two pools (AUDIO_CLASSIC.md § T, as built), or one pool
    // of four that both kinds share.
    if(shared_pool) bt_acl_sizes(&host.acl,1021,4,0,0);
    else bt_acl_sizes(&host.acl,1021,4,251,6);
    bt_classic_init(&host.classic,CM_LOCAL);
    host.classic_available=true; host.classic_unavailable=NULL;
    assert(bt_host_classic_bond(&host,bond,now));
    if(!boot_page) host.classic_page_at=0;
    for(unsigned i=0;i<20 && (CM.owed || !bt_classic_quiescent(&host.classic) || host.owner);i++) {
        if(host.classic.phase!=BT_CLASSIC_IDLE) break;   // the boot page has begun
        step();
    }
    assert(CM.scan_writes==1 && CM.scan==(host.classic.bond.valid?2:0));
}
// One step, watched: whether the command slot was free for its tick (no
// holder, or the holder's reply in and nothing of its own pending), whether
// the Classic link had an answer waiting, and where its commands begin.
static bool w_free, w_urgent;
static unsigned w_before;
static void step_watched(void)
{
    ++now; classic_deliver(); headphones_deliver(); tx_settle(); TX.sent_n=0;
    w_free=!host.owner || (host.command_done && !host_owner_pending(&host));
    w_urgent=bt_classic_urgent(&host.classic);
    w_before=CM.n;
    bt_host_tick(&host,now,true,TX.n,false,multi_command,multi_acl,multi_report,NULL); acl_agrees();
}
static bt_classic_bond_t ph_bond(void)
{
    bt_classic_bond_t b={.valid=true,.key_type=4};
    memcpy(b.peer,PH,6); memcpy(b.local,CM_LOCAL,6); memcpy(b.key,PH_KEY,16);
    strcpy(b.name,"SoundLink II");
    return b;
}
static bool status_has(const char *want)
{
    static char text[BT_SCAN_TEXT_BYTES];
    bt_host_status(&host,text,sizeof(text));
    if(strstr(text,want)) return true;
    printf("status lacks \"%s\":\n%s\n",want,text);
    return false;
}
// The status text in any spelling of a key: three bytes of it in hex.
static bool status_shows_key(const uint8_t key[16])
{
    static char text[BT_SCAN_TEXT_BYTES],flat[BT_SCAN_TEXT_BYTES];
    bt_host_status(&host,text,sizeof(text));
    size_t m=0;
    for(size_t i=0;text[i] && m+1<sizeof(flat);i++) if(text[i]!=':' && text[i]!=' ') flat[m++]=text[i];
    flat[m]=0;
    for(int i=0;i+3<=16;i++) {
        char fwd[7],rev[7];
        snprintf(fwd,sizeof fwd,"%02x%02x%02x",key[i],key[i+1],key[i+2]);
        snprintf(rev,sizeof rev,"%02x%02x%02x",key[i+2],key[i+1],key[i]);
        if(strstr(flat,fwd) || strstr(flat,rev)) return true;
    }
    return false;
}
static bool classic_cmd(const char *text) { return bt_host_command(&host,text,strlen(text),now); }

// No bond and nobody typing: ten thousand ticks of keyboard traffic, and
// the Classic link sends nothing but its one Write Scan Enable(0). Then
// every scenario above again, with the Classic slot present.
static void classic_quiet(void)
{
    classic_on=true;
    setup();
    unsigned from=CM.n;
    assert(CM.scan_writes==1 && !CM.scan);
    for(unsigned i=0;i<10000;i++) {
        notify_keyboard(); step();
        if(i%1000==999) { const uint8_t ack[]={0x1e}; assert(le_queue(&host.peers[0].le,4,ack,1)); }
        if(tx_n) { tx_n=0; const uint8_t complete[]={0x13,5,1,12,0,1,0}; multi_event(complete,sizeof(complete)); }
        assert(!host.failed && host.peers[0].le.phase==BT_LE_READY);
    }
    assert(input_count[0]==10000 && !release_count[0]);
    assert(classic_ops_since(from)==0 && CM.classic==1 && CM.scan_writes==1);
    // Not one byte of L2CAP: no link, so no L2CAP to send it.
    assert(!CM.acl_packets && !host.l2cap.up);
    assert(host.classic.phase==BT_CLASSIC_IDLE && bt_classic_quiescent(&host.classic) && !host.classic.malformed);
    assert(!host.owner && !host.classic_auto && !host.classic_page_at);
    mouse_interval();
    peer_att_requests();
    pairing_and_reconnect(); cancel_races(); credits_and_faults();
    rejected_create_and_fragmentation(); scan_recovery(); acl_resync();
    saved_duplicate_requests();
    classic_on=false;
    puts("PASS: with no Classic bond nobody typing, the Classic slot sends one Write Scan Enable(0) and every LE scenario holds");
}

// What the Classic link may take from the LE peers: nothing. With no bond
// and no session it takes only the BR/EDR-only event codes, whatever is in
// them; Disconnection Complete, Encryption Change and Key Refresh for an
// LE handle are left for the LE peers, live Classic link or not; and the
// keyboard's own events still reach it through the host.
static void classic_takes_only_its_own(void)
{
    static const uint8_t classic_only[]={0x03,0x04,0x06,0x12,0x14,0x16,0x17,0x18,0x31,0x32,0x33,0x36};
    for(unsigned live=0;live<2;live++) {
        setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
        if(live) { host.classic.connected=true; host.classic.handle=0x40; }
        for(unsigned code=0;code<256;code++) for(unsigned params=0;params<=40;params++) {
            uint8_t e[42]={(uint8_t)code,(uint8_t)params};
            // Status 0, the keyboard's handle (12), its address bytes after.
            if(params>=3) { e[3]=12; e[4]=0; }
            for(unsigned i=5;i<2+params;i++) e[i]=(uint8_t)(i*7);
            bt_classic_t copy=host.classic;
            bool took=bt_classic_event(&copy,e,2+params);
            bool mine=false;
            for(unsigned i=0;i<sizeof(classic_only);i++) mine|=classic_only[i]==code;
            if(took) assert(mine);
            // The shared ones are never taken for the keyboard's handle.
            if(code==0x05 || code==0x08 || code==0x30) assert(!took);
        }
        // Through the host: the keyboard's encryption refresh and its loss
        // still reach slot 0.
        unsigned before=release_count[0];
        const uint8_t refresh[]={0x30,3,0,12,0}; multi_event(refresh,sizeof(refresh));
        assert(host.peers[0].le.phase==BT_LE_READY);
        disconnected(0,0x08); step();
        assert(release_count[0]==before+1 && !host.peers[0].le.connected);
        assert(host.classic.connected==(live==1) && host.classic.phase==BT_CLASSIC_IDLE);
        if(live) { host.classic.connected=false; host.classic.handle=0; step(); }
    }
    puts("PASS: the Classic link takes only BR/EDR events, and none for an LE handle");
}

// `slot 2 bond` from end to end while the keyboard types: pairing,
// encryption, the key size, the bond marked to save, page scan on; the
// keyboard's packets flow at every step and the ACL count agrees.
static void classic_bond_while_typing(void)
{
    setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
    char text[64]; snprintf(text,sizeof text,"slot 2 bond %s",PH_TEXT);
    // The scan heard it as Classic: its name goes to the bond.
    host.scan.count=1; host.scan.devices[0]=(bt_scan_device_t){.le=false,.rssi=-40};
    memcpy(host.scan.devices[0].address,PH,6); strcpy(host.scan.devices[0].name,"SoundLink II");
    unsigned from=CM.n, typed=input_count[0];
    assert(classic_cmd(text));
    assert(host.classic.phase==BT_CLASSIC_PAGING && host.classic_auto && !host.refused_why);
    for(unsigned i=0;i<3000 && host.classic.phase!=BT_CLASSIC_READY;i++) {
        notify_keyboard(); step();
        assert(input_count[0]==typed+i+1 && !release_count[0] && !host.failed);
    }
    assert(host.classic.phase==BT_CLASSIC_READY && host.classic.encrypted && host.classic.key_size==16);
    static const uint16_t want[]={0x0405,0x0411,0x040c,0x042b,0x042c,0x0413,0x1408};
    for(unsigned i=0,at=from;i<7;i++) { int k=op_index(want[i],at); assert(k>=0); at=(unsigned)k+1; }
    assert(host.classic.bond.valid && host.classic.bond_dirty && !memcmp(host.classic.bond.key,PH_NEW_KEY,16));
    assert(!strcmp(host.classic.bond.name,"SoundLink II"));
    for(unsigned i=0;i<50;i++) { notify_keyboard(); step(); }
    assert(CM.scan==2 && CM.scan_writes==2);
    assert(host.acl_classic==CM.handle+1u);
    assert(status_has("\nslot 2:\nkind: classic\nstate: ready\n") && status_has("automatic: yes; connected\n"));
    assert(!status_shows_key(PH_NEW_KEY));
    // A packet on the Classic handle is its L2CAP's (a signalling frame too
    // short to hold a command: counted malformed); the keyboard's still arrive.
    const uint8_t classic_packet[]={0x40,0x20,5,0,1,0,1,0,0x02};
    unsigned before=input_count[0], malformed=host.l2cap.malformed;
    multi_wire(classic_packet,sizeof(classic_packet)); notify_keyboard();
    assert(host.l2cap.malformed==malformed+1 && input_count[0]==before+1);
    assert(status_has("\nl2cap: link up, encrypted\n"));
    // Its completions are the count's; the keyboard's handle is untouched.
    const uint8_t stray[]={0x13,5,1,0x40,0,1,0}; multi_event(stray,sizeof(stray));
    assert(host.acl_bad_completions==1);
    // The headphones go: the link is down, the count lets it go.
    cm_disconn(0,0x13); step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && !host.classic.connected && !host.acl_classic);
    puts("PASS: slot 2 bond pairs, encrypts and keeps the bond while the keyboard types; ACL counted, Classic data goes to its L2CAP");
}

// The controller asks the Classic link something (Link Key Request) while
// an LE peer holds the command slot through its pairing: the answer goes at
// the first turn the slot is free, ahead of the LE peer's next command.
static void classic_urgent_order(void)
{
    setup(); bt_classic_bond_t b=ph_bond(); classic_slot(&b,true);
    CM.has_key=true; memcpy(CM.key,PH_KEY,16); CM.hold_auth=true;
    // The boot page: paged, linked, Authentication Requested out and held.
    for(unsigned i=0;i<300 && host.classic.phase!=BT_CLASSIC_AUTHENTICATING;i++) step();
    for(unsigned i=0;i<10;i++) step();
    assert(host.classic.phase==BT_CLASSIC_AUTHENTICATING && !host.owner && !CM.owed);
    // The mouse pairs on slot 1.
    peer_bonding=true; peer_key_delivery=2; peer_pair_response[6]=1;
    const char *request="slot 1 bond-justworks random f8:2c:fe:ff:f0:1a";
    assert(bt_host_command(&host,request,strlen(request),now));
    bool injected=false, answered=false;
    unsigned injected_at=0;
    for(unsigned i=0;i<1000 && (!bt_le_input_active(&host.peers[1].le) || !answered);i++) {
        notify_keyboard();
        step_watched();
        if(injected && !answered && w_free && w_urgent) {
            // The first command of the first free turn is the answer.
            assert(CM.n>w_before && CM.ops[w_before]==0x040b);
            answered=true;
        }
        if(!injected && host.owner==BT_HOST_OWNER_PEER0+1 && cmd_n) {
            cm_addr_event(0,0x17,PH); injected=true; injected_at=CM.n;
        }
        session=host.peers[1].le; controller();
        session=host.peers[1].le; peripheral();
        assert(!host.failed && !release_count[0]);
    }
    assert(injected && answered && bt_le_input_active(&host.peers[1].le));
    // Between the injection and the answer, nothing but the LE command in
    // flight's own chain: the answer was not made to wait for a free turn.
    int answer=op_index(0x040b,injected_at); assert(answer>=0);
    for(unsigned i=0;i<200 && host.classic.phase!=BT_CLASSIC_READY;i++) { notify_keyboard(); step(); }
    assert(host.classic.phase==BT_CLASSIC_READY && !host.classic.bond_dirty);
    puts("PASS: a Link Key Request during an LE pairing is answered at the first free turn, ahead of the LE peer");
}

// The bonded headphones call while the scan holds the slot: the accept is
// latched, sent at the first free turn, and the link comes up; the scan
// still finishes.
static void classic_inbound_during_scan(void)
{
    setup(); bt_classic_bond_t b=ph_bond(); classic_slot(&b,false);   // no boot page: they call us
    assert(bt_classic_quiescent(&host.classic));
    CM.has_key=true; memcpy(CM.key,PH_KEY,16); CM.they_secure=true;
    assert(bt_host_scan(&host,now)); step();
    assert(cmd_n && le_u16(cmd)==0x200b && host.owner==BT_HOST_OWNER_SCAN);
    cm_request(0,PH);
    step();
    assert(host.classic.phase==BT_CLASSIC_ACCEPTING && bt_classic_urgent(&host.classic));
    assert(host.owner==BT_HOST_OWNER_SCAN && op_index(0x0409,0)<0);
    unsigned before=CM.n;
    reply(0x200b,0);
    // The scan chains its commands while it holds the slot; the first turn
    // the slot is free is the accept's, ahead of whatever the scan sends next.
    bool accepted=false;
    for(unsigned i=0;i<5000 && (host.classic.phase!=BT_CLASSIC_READY || host.scan.phase!=BT_SCAN_DONE);i++) {
        notify_keyboard(); step_watched();
        if(!accepted && w_free && w_urgent) { assert(CM.n>w_before && CM.ops[w_before]==0x0409); accepted=true; }
        answer_le();
        assert(!host.failed);
    }
    assert(accepted);
    assert(host.classic.phase==BT_CLASSIC_READY && host.classic.session==BT_CLASSIC_SESSION_INBOUND);
    assert(host.scan.phase==BT_SCAN_DONE && !host.scan.error);
    assert(op_index(0x040b,before)>=0 && op_index(0x0411,before)<0);
    puts("PASS: the bonded headphones calling during a scan are accepted at the first free turn; the scan finishes");
}

// Every owner value through the command reply's routing and host_retire:
// the Classic owner's reply reaches the Classic link, an LE owner's its
// peer, and no value indexes past the LE peers (UBSan's bounds check).
static void classic_owner_bounds(void)
{
    for(unsigned owner=0;owner<=BT_HOST_OWNER_CLASSIC+2;owner++) for(unsigned match=0;match<2;match++) {
        setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
        host.owner=(uint8_t)owner; host.credits=0; host.command_done=false;
        uint16_t op=0x0c1a;
        host.scan.opcode=op; host.scan.pending=true;
        for(unsigned i=0;i<BT_HOST_PEERS;i++) { host.peers[i].le.opcode=op; host.peers[i].le.pending=true; }
        host.classic.opcode=op; host.classic.pending=true; host.classic.command_done=false;
        uint16_t sent=match?op:0x0c1b;
        const uint8_t e[]={0x0e,4,1,(uint8_t)sent,(uint8_t)(sent>>8),0};
        bt_host_event(&host,e,sizeof(e));
        bool known=owner>=BT_HOST_OWNER_SCAN && owner<=BT_HOST_OWNER_CLASSIC;
        assert(host.credits==1 && host.command_done==(known && match));
        assert(host.classic.command_done==(owner==BT_HOST_OWNER_CLASSIC && match));
        // Retire: a holder whose command is still pending keeps the slot.
        host_retire(&host,true);
        assert(host.owner==owner);
        host.scan.pending=false; host.classic.pending=false;
        for(unsigned i=0;i<BT_HOST_PEERS;i++) host.peers[i].le.pending=false;
        host.command_done=true; host_retire(&host,true);
        assert(host.owner==BT_HOST_OWNER_NONE);
        assert(host_owner_pending(&host)==false);
    }
    // A Classic command the controller never answers: the link gives up at
    // its deadline, the slot stays held, and the controller is failed three
    // seconds later rather than leaving the keyboard without a slot.
    setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
    CM.silent=true;
    char text[64]; snprintf(text,sizeof text,"slot 2 connect %s",PH_TEXT);
    assert(classic_cmd(text)); step();
    assert(host.owner==BT_HOST_OWNER_CLASSIC && host.classic.pending);
    for(unsigned i=0;i<2100;i++) { step(); assert(!host.failed); }
    assert(!host.classic.pending && host.owner==BT_HOST_OWNER_CLASSIC && host.classic.error);
    unsigned sent=CM.classic;
    for(unsigned i=0;i<2800;i++) { step(); assert(!host.failed && CM.classic==sent); }
    for(unsigned i=0;i<200 && !host.failed;i++) step();
    assert(host.failed && host.classic.phase==BT_CLASSIC_FAILED);
    // A late reply instead frees the slot.
    setup(); classic_slot(&none,false);
    CM.silent=true;
    assert(classic_cmd(text)); step();
    for(unsigned i=0;i<2100;i++) step();
    assert(host.owner==BT_HOST_OWNER_CLASSIC && !host.classic.pending);
    unsigned before=CM.n;
    cm_cs(0,0,0x0405); step();
    // Freed: the closing link's page cancel is the next command.
    assert(!host.failed && CM.n==before+1 && CM.ops[before]==0x0408);
    // Answered, but the USB transfer that carried the command never reports
    // done: the slot cannot be retired, and that too fails the controller
    // rather than holding the slot for ever.
    setup(); classic_slot(&none,false);
    assert(classic_cmd(text)); step();
    assert(host.owner==BT_HOST_OWNER_CLASSIC && !host.command_done);
    uint64_t sent_at=now;
    for(unsigned i=0;i<4900;i++) {
        ++now; classic_deliver();   // the model's reply arrives; the transfer never completes
        bt_host_tick(&host,now,false,0,false,multi_command,multi_acl,multi_report,NULL);
        assert(!host.failed && host.owner==BT_HOST_OWNER_CLASSIC);
        if(i==100) assert(host.command_done);
    }
    for(unsigned i=0;i<300 && !host.failed;i++) {
        ++now; classic_deliver();
        bt_host_tick(&host,now,false,0,false,multi_command,multi_acl,multi_report,NULL);
    }
    assert(host.failed && now>=sent_at+5000 && now<sent_at+5200);
    puts("PASS: every command-slot owner is bounded in routing and retirement; a Classic command unanswered, or answered with its transfer never done, fails the controller; a late reply frees the slot");
}

// Run a start-up round on the host, answering every command (`refuse` with
// 0x0c), on a controller whose features byte 4 is `features4`.
static void startup_round(uint8_t features4,uint16_t refuse)
{
    setup(); host=(bt_host_t){0}; bt_host_init(&host,now); memset(&CM,0,sizeof(CM));
    for(unsigned i=0;i<BT_HOST_PEERS;i++) host.peers[i].manager.loaded=true;
    for(unsigned i=0;i<6000 && !host.initialized;i++) {
        step();
        if(!cmd_n) continue;
        uint16_t op=le_u16(cmd); cmd_n=0;
        uint8_t e[20]={0x0e,4,1,(uint8_t)op,(uint8_t)(op>>8),op==refuse?0x0c:0}; size_t n=6;
        if(op==0x1003) { n=14; e[10]=features4; e[12]=1; }
        if(op==0x1009) { n=12; memcpy(e+6,CM_LOCAL,6); }
        if(op==0x1005) { n=13; e[6]=0xfd; e[7]=3; e[9]=4; }
        if(op==0x2002) { n=9; e[6]=251; e[8]=6; }
        if(op==refuse) n=6;
        e[1]=(uint8_t)(n-2); multi_event(e,n);
        if(host.scan.phase==BT_SCAN_LE_WAIT) now+=2000;
    }
    assert(host.initialized && !host.failed);
}
static bool sent_op(uint16_t op) { return op_index(op,0)>=0; }

// No Classic slot: a setup command refused, or a controller with no BR/EDR.
// Slot 2 is refused with the reason, nothing Classic is ever sent, and LE
// comes up as it always has.
static void classic_unavailable(void)
{
    // Every setup command answered: the slot is there.
    startup_round(0x40,0);
    assert(host.classic_available && !host.classic_unavailable && !host.classic_setup_refused);
    assert(!memcmp(host.classic.local,CM_LOCAL,6) && host.classic.phase==BT_CLASSIC_IDLE);
    assert(sent_op(0x0c56) && sent_op(0x0c13) && sent_op(0x0c24) && sent_op(0x080f) && sent_op(0x0c18) && sent_op(0x0c16));
    static const uint16_t refusable[]={0x0c56,0x0c13,0x0c24,0x080f,0x0c18,0x0c16};
    for(unsigned r=0;r<7;r++) {
        if(r<6) startup_round(0x40,refusable[r]);
        else startup_round(0x60,0);
        assert(host.initialized && !host.failed && host.scan.phase==BT_SCAN_DONE && host.acl.known);
        assert(!host.classic_available && host.classic_unavailable);
        if(r<6) {
            assert(host.classic_setup_refused && host.classic_refused_opcode==refusable[r] && host.classic_refused_status==0x0c);
            char want[96]; snprintf(want,sizeof want,"unavailable: the controller refused a Classic setup command (opcode %04x status 0c)\n",refusable[r]);
            assert(status_has(want));
        } else {
            assert(!sent_op(0x0c56) && !sent_op(0x0c13));
            assert(status_has("\nslot 2:\nkind: classic\nunavailable: the controller has no Classic radio\n"));
        }
        unsigned from=CM.n;
        char text[64]; snprintf(text,sizeof text,"slot 2 bond %s",PH_TEXT);
        assert(!classic_cmd(text));
        assert(status_has(r<6?"last command refused: no Classic slot: the controller refused a Classic setup command\n":
                               "last command refused: no Classic slot: the controller has no Classic radio\n"));
        assert(!classic_cmd("slot 2 forget") && !classic_cmd("slot 2 reconnect"));
        assert(!bt_host_classic_bond(&host,&(bt_classic_bond_t){0},now));
        // Calls and pairing asks the controller might raise go to nobody.
        cm_request(0,PH); cm_addr_event(0,0x17,PH);
        for(unsigned i=0;i<3000;i++) step();
        assert(classic_ops_since(from)==0 && !host.failed);
        if(r==6) assert(!bt_host_scan_classic(&host,now));
        // The LE scan still runs.
        assert(bt_host_scan(&host,now));
    }
    puts("PASS: a refused Classic setup command or no BR/EDR costs only slot 2: LE starts, slot 2 says why, nothing Classic is sent");
}

// Reconnection: a page once at boot; a drop the headphones may come back
// from, and a page they did not answer, is paged after 5 s, then 10, 20,
// 40, 60, 60; READY starts the ladder again; forget stops it; a drop they
// will not come back from waits.
static void classic_reconnection(void)
{
    bt_classic_bond_t b=ph_bond();
    // At boot, with the headphones off: the page is not answered, and it
    // is asked again on the ladder; switched on, they are found by the
    // next one.
    setup(); CM.present=false; classic_slot(&b,true);
    for(unsigned i=0;i<200;i++) step();
    assert(op_index(0x0405,0)>=0 && host.classic.phase==BT_CLASSIC_STOPPED && host.classic.retryable);
    assert(host.classic.error_status==0x04 && host.classic_page_at>now && host.classic_page_at<=now+5000);
    unsigned pages=0; for(unsigned i=0;i<CM.n;i++) pages+=CM.ops[i]==0x0405;
    assert(pages==1);
    for(unsigned i=0;i<200000;i++) step();
    pages=0; for(unsigned i=0;i<CM.n;i++) pages+=CM.ops[i]==0x0405;
    assert(pages>=5 && pages<=8);     // 5, 10, 20, 40 s, then a minute apart
    CM.present=true; CM.has_key=true; memcpy(CM.key,PH_KEY,16);
    for(unsigned i=0;i<61000 && host.classic.phase!=BT_CLASSIC_READY;i++) step();
    assert(host.classic.phase==BT_CLASSIC_READY && !host.classic_backoff_ms);
    // A page the controller ends for a reason of its own waits for them.
    setup(); CM.present=false; CM.page_status=0x0d; classic_slot(&b,true);
    for(unsigned i=0;i<70000;i++) step();
    pages=0; for(unsigned i=0;i<CM.n;i++) pages+=CM.ops[i]==0x0405;
    assert(pages==1 && !host.classic_page_at && !host.classic.retryable);
    assert(status_has("automatic: yes; waiting for the headphones to call\n"));
    // A page held while an LE peer sets up (connecting, nothing to send),
    // and then while a scan runs.
    setup();
    host.peers[1].le=(bt_le_t){.shared_controller=true,.phase=BT_LE_CONNECTING,.link_may_active=true,
        .address_type=1,.deadline=now+100000,.total_deadline=now+100000};
    CM.has_key=true; memcpy(CM.key,PH_KEY,16);
    classic_slot(&b,true);
    for(unsigned i=0;i<500;i++) step();
    assert(host.classic_page_at && host.classic.phase==BT_CLASSIC_IDLE && op_index(0x0405,0)<0);
    host.peers[1].le=(bt_le_t){.shared_controller=true};
    assert(bt_host_scan(&host,now));
    for(unsigned i=0;i<4000 && !bt_scan_quiescent(&host.scan);i++) {
        step(); answer_le();
        assert((host.classic.phase==BT_CLASSIC_IDLE || bt_scan_quiescent(&host.scan)) && op_index(0x0405,0)<0);
    }
    assert(host.scan.phase==BT_SCAN_DONE);
    for(unsigned i=0;i<4000 && host.classic.phase!=BT_CLASSIC_READY;i++) { step(); answer_le(); }
    assert(host.classic.phase==BT_CLASSIC_READY && host.classic.session==BT_CLASSIC_SESSION_RECONNECT);
    assert(!host.classic_page_at && !host.classic_backoff_ms);
    // Dropped by the far end (connection timeout): paged again after the
    // ladder's waits, each page failing the same retryable way.
    CM.present=false; CM.page_status=0x08;
    cm_disconn(0,0x08); step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && host.classic.retryable);
    static const unsigned ladder[]={5000,10000,20000,40000,60000,60000};
    uint64_t stopped_at=now;
    for(unsigned k=0;k<6;k++) {
        assert(host.classic_page_at==stopped_at+ladder[k]);
        char want[64]; snprintf(want,sizeof want,"automatic: yes; next page in %u ms\n",ladder[k]-1);
        step(); assert(status_has(want));
        unsigned from=CM.n;
        while(now<stopped_at+ladder[k]-1) { step(); assert(op_index(0x0405,from)<0); }
        // Due: the page is asked for at the end of this tick, sent at the next.
        step(); assert(host.classic.phase==BT_CLASSIC_PAGING && !host.classic_page_at);
        step(); assert(CM.ops[CM.n-1]==0x0405 && CM.at[CM.n-1]==stopped_at+ladder[k]+1);
        for(unsigned i=0;i<200 && host.classic.phase!=BT_CLASSIC_STOPPED;i++) step();
        assert(host.classic.phase==BT_CLASSIC_STOPPED && host.classic.retryable);
        stopped_at=now;
    }
    // Back: READY starts the ladder again.
    CM.present=true; CM.page_status=0;
    while(host.classic.phase!=BT_CLASSIC_READY && now<stopped_at+70000) step();
    assert(host.classic.phase==BT_CLASSIC_READY && !host.classic_backoff_ms);
    cm_disconn(0,0x08); step(); stopped_at=now;
    assert(host.classic_page_at==stopped_at+5000);
    // Forget: nothing more, and the bond is gone.
    assert(classic_cmd("slot 2 forget"));
    assert(!host.classic_auto && !host.classic_page_at && !host.classic.bond.valid && host.classic.bond_dirty);
    unsigned from=CM.n;
    for(unsigned i=0;i<70000;i++) step();
    assert(op_index(0x0405,from)<0);
    assert(status_has("automatic: no\n"));
    // A drop the headphones will not come back from (authentication): no page.
    setup(); classic_slot(&b,true);
    CM.has_key=true; memcpy(CM.key,PH_KEY,16);
    for(unsigned i=0;i<500 && host.classic.phase!=BT_CLASSIC_READY;i++) step();
    assert(host.classic.phase==BT_CLASSIC_READY);
    cm_disconn(0,0x05); step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && !host.classic.retryable);
    from=CM.n;
    for(unsigned i=0;i<70000;i++) step();
    assert(op_index(0x0405,from)<0 && host.classic_auto && !host.classic_page_at);
    // Disconnect by hand: no page either, even from a link already down.
    setup(); classic_slot(&b,true);
    CM.has_key=true; memcpy(CM.key,PH_KEY,16);
    for(unsigned i=0;i<500 && host.classic.phase!=BT_CLASSIC_READY;i++) step();
    assert(classic_cmd("slot 2 disconnect") && !host.classic_auto);
    from=CM.n;
    for(unsigned i=0;i<70000;i++) step();
    assert(op_index(0x0405,from)<0);
    // A page is owed after an unanswered one, and they call meanwhile with
    // a key that is not ours: their call takes the owed page's place, and
    // its end (authentication, not to be paged after) stands.
    setup(); CM.present=false; classic_slot(&b,true);
    for(unsigned i=0;i<200;i++) step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && host.classic.retryable && host.classic_page_at);
    CM.has_key=true; memcpy(CM.key,PH_NEW_KEY,16); CM.they_secure=true;
    from=CM.n;
    cm_request(0,PH); step(); step();
    assert(host.classic.phase!=BT_CLASSIC_STOPPED && !host.classic_page_at);
    for(unsigned i=0;i<3000 && host.classic.phase!=BT_CLASSIC_STOPPED;i++) step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && !host.classic.retryable);
    for(unsigned i=0;i<70000;i++) step();
    assert(op_index(0x0405,from)<0 && host.classic_auto && !host.classic_page_at);
    // A bond made as another adapter is no bond: nothing is paged.
    setup(); bt_classic_bond_t other=b; other.local[0]^=1; classic_slot(&other,true);
    assert(!host.classic.bond.valid && !host.classic_auto && !host.classic_page_at && CM.scan==0);
    puts("PASS: Classic reconnection pages once at boot, backs off 5 to 60 s after retryable drops, restarts at READY, stops at forget");
}

// Which slot takes which address, and what each command is refused for.
static void classic_commands(void)
{
    setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
    static const uint8_t LE_ONLY[6]={1,2,3,4,5,6}, DUAL[6]={7,8,9,10,11,12};
    host.scan.count=4;
    host.scan.devices[0]=(bt_scan_device_t){.le=false}; memcpy(host.scan.devices[0].address,PH,6);
    host.scan.devices[1]=(bt_scan_device_t){.le=true}; memcpy(host.scan.devices[1].address,LE_ONLY,6);
    host.scan.devices[2]=(bt_scan_device_t){.le=true}; memcpy(host.scan.devices[2].address,DUAL,6);
    host.scan.devices[3]=(bt_scan_device_t){.le=false}; memcpy(host.scan.devices[3].address,DUAL,6);
    strcpy(host.scan.devices[3].name,"Dual");
    char text[96];
    // A Classic-only address on an LE slot.
    for(unsigned slot=0;slot<2;slot++) {
        snprintf(text,sizeof text,"slot %u bond public %s",slot,PH_TEXT);
        assert(!classic_cmd(text) && host.peers[slot].le.phase==(slot?BT_LE_IDLE:BT_LE_READY));
        assert(status_has("last command refused: that is a Classic address: use slot 2\n"));
    }
    // An LE-only address on slot 2.
    assert(!classic_cmd("slot 2 bond 06:05:04:03:02:01"));
    assert(status_has("last command refused: the scan heard that address as LE; give it to slot 0 or 1\n"));
    // A dual-mode address: each slot takes its own kind; slot 2 is given the Classic entry.
    assert(classic_cmd("slot 2 connect 0c:0b:0a:09:08:07") && !strcmp(host.classic.peer_name,"Dual"));
    assert(!host.refused_why);
    assert(classic_cmd("slot 2 disconnect"));
    for(unsigned i=0;i<300 && !bt_classic_quiescent(&host.classic);i++) step();
    assert(classic_cmd("slot 1 connect public 0c:0b:0a:09:08:07") && !host.refused_why);
    host.peers[1].le=(bt_le_t){.shared_controller=true}; host.peers[1].manager=(bt_manager_t){.loaded=true};
    // Not ready, failed, busy.
    host.initialized=false;
    assert(!classic_cmd("slot 2 reconnect") && status_has("last command refused: the controller is not ready\n"));
    assert(status_has("\nslot 2:\nkind: classic\nunavailable: the controller is not ready\n"));
    host.initialized=true;
    bt_le_t le1=host.peers[1].le; host.peers[1].le.phase=BT_LE_ADDRESS;
    snprintf(text,sizeof text,"slot 2 bond %s",PH_TEXT);
    assert(!classic_cmd(text) && status_has("last command refused: busy: an LE slot is setting up\n"));
    assert(!classic_cmd("slot 2 reconnect") && !classic_cmd("slot 2 connect 4c:87:5d:c3:b2:a1"));
    // Forget and disconnect are never busy.
    assert(classic_cmd("slot 2 forget"));
    assert(!classic_cmd("slot 2 disconnect") && status_has("last command refused: not connected\n"));
    host.peers[1].le=le1;
    // Two crossing words: the duplicate-slot line is slot 0's and 1's alone.
    assert(!classic_cmd("slot 2 bond zz"));
    assert(status_has("last command refused: write bond|connect XX:XX:XX:XX:XX:XX, reconnect, forget or disconnect\n"));
    assert(!classic_cmd("slot 3 forget") && !classic_cmd("slot 2") && !classic_cmd("slot 2forget"));
    // `scan classic`: refused while the link pages, taken when it is quiet or READY.
    CM.silent=true;
    assert(classic_cmd(text)); step();
    assert(!bt_host_scan_classic(&host,now));
    CM.silent=false;
    host.failed=true;
    assert(!classic_cmd(text) && status_has("last command refused: the Bluetooth controller failed; reboot required\n"));
    assert(classic_cmd("slot 2 forget"));   // the bond file stays the user's to clear
    setup(); classic_slot(&none,false);
    assert(bt_host_scan_classic(&host,now) && host.scan.phase==BT_SCAN_INQUIRY_MODE && !host.scan.le_only);
    for(unsigned i=0;i<12000 && host.scan.phase!=BT_SCAN_DONE;i++) { step(); answer_le(); }
    assert(host.scan.phase==BT_SCAN_DONE && host.scan.count==1 && !host.scan.devices[0].le);
    assert(!memcmp(host.scan.devices[0].address,PH,6));
    assert(sent_op(0x0c45) && sent_op(0x0401) && !sent_op(0x0c56));
    // The scan's inquiry leaves the Classic link alone, and LE untouched.
    assert(host.classic.phase==BT_CLASSIC_IDLE && host.peers[0].le.phase==BT_LE_READY);
    puts("PASS: slot 2 commands: address kinds both ways, refusals with reasons, busy rules, scan classic");
}

// The status text for each state, and never a key.
static void classic_status(void)
{
    setup();
    host.initialized=false;
    // Not ready: the slot says so, and nothing of its services is shown.
    static char text[BT_SCAN_TEXT_BYTES];
    bt_host_status(&host,text,sizeof(text));
    static const char not_ready[]="\nslot 2:\nkind: classic\nunavailable: the controller is not ready\n";
    assert(strlen(text)>=strlen(not_ready) && !strcmp(text+strlen(text)-strlen(not_ready),not_ready));
    host.initialized=true;
    bt_classic_bond_t b=ph_bond(); classic_slot(&b,true);
    assert(status_has("\nslot 2:\nkind: classic\nstate: "));
    assert(status_has("bond: 4c:87:5d:c3:b2:a1 key type 04 name SoundLink II (to save: no)\n"));
    assert(!status_shows_key(PH_KEY));
    host.classic_auto=false; assert(status_has("automatic: no\n"));
    // After the LE slots, and the commands line names slot N.
    bt_host_status(&host,text,sizeof(text));
    char *s0=strstr(text,"\nslot 0:\n"),*s1=strstr(text,"\nslot 1:\n"),*s2=strstr(text,"\nslot 2:\n");
    assert(s0 && s1 && s2 && s0<s1 && s1<s2);
    // In a buffer too small for it, the text stays a string.
    char small[64];
    for(size_t cap=1;cap<sizeof(small);cap++) { size_t n=bt_host_status(&host,small,cap); assert(n<cap && !small[n]); }
    size_t whole=bt_host_status(&host,text,sizeof(text));
    for(size_t cap=whole-200;cap<=whole+1;cap++) { size_t n=bt_host_status(&host,text,cap); assert(n<cap && !text[n]); }
    puts("PASS: slot 2's status: not ready, unavailable with the reason, the link, automatic connection, the services; no key");
}

// ── Slot 2's services: L2CAP and SDP (AUDIO_CLASSIC.md part S) ─────────────

// The host with its Classic slot, bonded to the headphones with `slot 2
// bond` and stepped to READY while the keyboard types. The query begins in
// the tick that reaches READY, so the headphones' answer to our channel
// (`refuse`: 0 accepts) is set before.
static void classic_bonded_refusing(uint16_t refuse)
{
    setup(); bt_classic_bond_t none={0}; classic_slot(&none,false);
    HP.refuse=refuse;
    host.scan.count=1; host.scan.devices[0]=(bt_scan_device_t){.le=false};
    memcpy(host.scan.devices[0].address,PH,6);
    char text[64]; snprintf(text,sizeof text,"slot 2 bond %s",PH_TEXT);
    assert(classic_cmd(text));
    for(unsigned i=0;i<3000 && host.classic.phase!=BT_CLASSIC_READY;i++) { notify_keyboard(); step(); }
    assert(host.classic.phase==BT_CLASSIC_READY && host.l2cap.up && host.l2cap.encrypted);
}
static void classic_bonded(void) { classic_bonded_refusing(0); }
// Steps until this connection's query has ended, the keyboard typing.
static void sdp_settle(void)
{
    unsigned ended=host.sdp_ended;
    for(unsigned i=0;i<8000 && host.sdp_ended==ended;i++) { notify_keyboard(); step(); }
    assert(host.sdp_ended==ended+1);
    for(unsigned i=0;i<50;i++) { notify_keyboard(); step(); }
}
static bool l2cap_all_free(void)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(!bt_l2cap_is_free(&host.l2cap,i)) return false;
    return true;
}
static const char SDP_SINK_LINE[]="sdp: audio sink, avdtp 1.3, a2dp 1.3, features 0x0003, psm 0019 (complete)\n";
// The keyboard's own ATT traffic: one packet queued whenever it has none,
// and each completed by the controller a step after it is sent.
static void keyboard_sends(void)
{
    bt_le_t *k=&host.peers[0].le;
    if(!k->queue_count && !k->outstanding) { const uint8_t ack[]={0x1e}; assert(le_queue(k,4,ack,1)); }
}
static void keyboard_completes(void)
{
    if(!tx_n) return;
    tx_n=0; CM_PUSH(1,0x13,5,1,12,0,1,0);
}

// READY, and with no typing: we open the SDP channel, it is configured both
// ways, the query goes out and is answered in three parts, the channel is
// closed, and nothing more is asked on that connection.
static void classic_sdp_query(void)
{
    classic_bonded();
    sdp_settle();
    assert(HP.conn_reqs==1 && HP.out_open && HP.sdp_requests==3 && HP.served==sizeof(HP_RECORD));
    assert(HP.disc_reqs==1 && HP.out_closed && l2cap_all_free());
    assert(status_has(SDP_SINK_LINE) && status_has("sdp requests answered: 0 dropped: 0\n"));
    assert(!strcmp(host.sdp_line,SDP_SINK_LINE) && host.sdp_ended==1);
    const bt_sdp_sink_t *r=&host.sdp_result;
    assert(host.sdp_result_valid && host.sdp_result_current && r->found && r->complete);
    assert(r->avdtp_version==0x0103 && r->a2dp_version==0x0103 && r->features==3 && r->l2cap_psm==0x0019);
    assert(!host.l2cap.malformed && !host.l2cap.strays && !host.l2cap.closed_cid && !HP.other);
    unsigned packets=CM.acl_packets;
    for(unsigned i=0;i<20000;i++) { notify_keyboard(); step(); }
    assert(HP.conn_reqs==1 && HP.sdp_requests==3 && CM.acl_packets==packets && host.sdp_ended==1);
    assert(host.classic.phase==BT_CLASSIC_READY && !host.failed);
    puts("PASS: slot 2's SDP query: our channel, configured both ways, three parts with continuation, closed, once a connection");
}

// The keyboard types every tick and sends every seventh, while the
// headphones keep the Classic link busy on a transport whose transfers take
// 5 ms: an LE packet that may go (queued, nothing of LE's in flight) goes
// in that very tick, ahead of any Classic packet, however many slots the
// Classic link has filled; and the count agrees at every step. Then an LE
// peer still holding a packet it could not send holds the Classic link.
static void classic_le_first(void)
{
    classic_bonded(); sdp_settle();
    // Transfers of 5 ms keep the Classic link at the most slots it may
    // take; then 1 ms ones leave room for both kinds in one tick.
    unsigned eligible_ticks=0, both=0, classic_sent=0, full=0;
    for(unsigned i=0;i<6000;i++) {
        TX.delay=i<3000?5:1;
        notify_keyboard();
        if(i%7==0) keyboard_sends();
        HP_CMD(8,HP.ident++,(uint8_t)i);          // an Echo Request: the answer is Classic traffic
        ++now; classic_deliver(); headphones_deliver(); tx_settle(); TX.sent_n=0;
        const bt_le_t *k=&host.peers[0].le;
        bool eligible=k->queue_count && !k->outstanding && !tx_holding(12);
        bt_host_tick(&host,now,true,TX.n,false,multi_command,multi_acl,multi_report,NULL); acl_agrees();
        int le_at=-1, classic_at=-1;
        for(unsigned s=0;s<TX.sent_n;s++) {
            if(TX.sent[s]==12 && le_at<0) le_at=(int)s;
            if(TX.sent[s]==CM.handle && classic_at<0) classic_at=(int)s;
        }
        // It would fail if the Classic link went first: in the order of the
        // sends, or by taking the slot the keyboard needed.
        if(eligible) { assert(le_at>=0); eligible_ticks++; }
        if(le_at>=0 && classic_at>=0) { assert(le_at<classic_at); both++; }
        classic_sent+=classic_at>=0;
        full+=tx_holding(CM.handle)==BT_HOST_TX_SLOTS-1;
        assert(TX.n<=BT_HOST_TX_SLOTS && !host.failed && host.peers[0].le.phase==BT_LE_READY);
        keyboard_completes();
    }
    assert(eligible_ticks>800 && both>300 && classic_sent>3000 && full>1000 && !release_count[0]);
    // The keyboard's packet waits on its own uncompleted one: the Classic
    // link waits too, and goes again once the keyboard has sent.
    TX.delay=0;
    for(unsigned i=0;i<20;i++) step();
    bt_le_t *k=&host.peers[0].le;
    keyboard_sends(); step();
    assert(tx_n && k->outstanding); tx_n=0;
    const uint8_t ack[]={0x1e}; assert(le_queue(k,4,ack,1));
    unsigned classic=CM.acl_packets;
    for(unsigned i=0;i<50;i++) {
        HP_CMD(8,HP.ident++,(uint8_t)i); step();
        assert(CM.acl_packets==classic && k->queue_count && !tx_n);
    }
    CM_PUSH(0,0x13,5,1,12,0,1,0); step();
    assert(tx_n && !k->queue_count); tx_n=0;
    CM_PUSH(0,0x13,5,1,12,0,1,0);
    for(unsigned i=0;i<20;i++) step();
    assert(CM.acl_packets>classic);
    puts("PASS: LE goes first: the keyboard's packets leave in the tick they may, ahead of the Classic link's, on a slow transport kept busy");
}

// The controller holds back the Classic link's completions: L2CAP stops at
// the pool's four, LE is untouched, and the rest go when the four come back.
static void classic_pool_exhausted(void)
{
    classic_bonded(); sdp_settle();
    CM.withhold=true;
    unsigned before=CM.acl_packets, echoes=HP.echo_rsps, le=packets;
    for(unsigned i=0;i<6;i++) HP_CMD(8,HP.ident++,0xaa);
    for(unsigned i=0;i<300;i++) { notify_keyboard(); keyboard_sends(); step(); keyboard_completes(); }
    assert(CM.acl_packets==before+4 && HP.echo_rsps==echoes+4 && CM.withheld==4);
    assert(bt_acl_outstanding(&host.acl,CM.handle)==4 && bt_l2cap_wants_send(&host.l2cap));
    assert(packets>le+100 && !host.failed && !release_count[0]);
    CM.withhold=false;
    CM_PUSH(0,0x13,5,1,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),4,0); CM.withheld=0;
    for(unsigned i=0;i<20;i++) { notify_keyboard(); step(); }
    assert(CM.acl_packets==before+6 && HP.echo_rsps==echoes+6 && !bt_l2cap_wants_send(&host.l2cap));
    puts("PASS: the Classic pool exhausted: L2CAP stops at four, the keyboard carries on, the rest go on completion");
}

// One pool for both kinds: the Classic link stops with a buffer left for
// each LE slot the host has, so a peer that connects while its packets are
// out, and does not ask the count, cannot overrun the controller.
static void classic_shared_pool(void)
{
    shared_pool=true; classic_bonded(); sdp_settle(); shared_pool=false;
    assert(bt_acl_shared(&host.acl));
    CM.withhold=true;
    unsigned before=CM.acl_packets;
    for(unsigned i=0;i<6;i++) HP_CMD(8,HP.ident++,0xaa);
    for(unsigned i=0;i<300;i++) {
        notify_keyboard(); keyboard_sends(); step(); keyboard_completes();
        assert(bt_acl_in_use(&host.acl,false)<=4);
    }
    assert(CM.acl_packets==before+4-BT_HOST_PEERS && bt_acl_outstanding(&host.acl,CM.handle)==4-BT_HOST_PEERS);
    assert(bt_l2cap_wants_send(&host.l2cap) && !host.failed);
    CM.withhold=false;
    CM_PUSH(0,0x13,5,1,(uint8_t)CM.handle,(uint8_t)(CM.handle>>8),(uint8_t)(4-BT_HOST_PEERS),0); CM.withheld=0;
    for(unsigned i=0;i<60;i++) { notify_keyboard(); step(); }
    assert(CM.acl_packets==before+6 && !bt_l2cap_wants_send(&host.l2cap));
    puts("PASS: a shared pool: the Classic link leaves a buffer for each LE slot, connected or not");
}

// They ask our SDP: Service Search Attribute for Audio Source with a small
// maximum, followed through every continuation, each answer byte for byte
// what bt_sdp_respond gives; then three at once with the payload slot
// taken: the first two answered in order, the third dropped and counted.
// And AVDTP is not admitted in this part.
static void classic_sdp_asked(void)
{
    classic_bonded(); sdp_settle();
    hp_connect(BT_L2CAP_PSM_SDP,HP_CLIENT_CID);
    for(unsigned i=0;i<20 && HP.in_conf_rsps<1;i++) step();
    assert(HP.in_answered && !HP.in_result && HP.host_in && HP.in_conf_rsps==1);
    step(); step();
    hp_ask(1,0x110A,16,NULL,0);
    for(unsigned i=0;i<200 && (HP.answers<HP.asks || !HP.answers);i++) { notify_keyboard(); step(); }
    assert(HP.answers==HP.asks && HP.answers>=4);
    bt_sdp_sink_t ours={0};
    assert(bt_sdp_parse_attribute_lists(HP.lists,HP.lists_n,0x110A,true,&ours)==BT_SDP_PARSED);
    assert(ours.found && ours.l2cap_psm==0x0019 && ours.avdtp_version==0x0103 && ours.a2dp_version==0x0103 && ours.features==1);
    assert(host.sdp_answered==HP.answers && !host.sdp_dropped);
    // Three at once.
    HP.follow=false;
    unsigned answers=HP.answers;
    hp_ask(10,0x110A,600,NULL,0); hp_ask(11,0x110A,600,NULL,0); hp_ask(12,0x110A,600,NULL,0);
    step();
    assert(host.sdp_dropped==1 && host.sdp_answer_bytes);
    for(unsigned i=0;i<20;i++) step();
    assert(HP.answers==answers+2 && HP.answered_tid[answers]==10 && HP.answered_tid[answers+1]==11);
    assert(!host.sdp_answer_bytes && host.sdp_answered==answers+2);
    char want[64]; snprintf(want,sizeof want,"sdp requests answered: %u dropped: 1\n",answers+2);
    assert(status_has(want));
    // AVDTP: not supported in this part.
    unsigned rsps=HP.conn_rsps;
    hp_connect(BT_L2CAP_PSM_AVDTP,HP_AVDTP_CID);
    for(unsigned i=0;i<10;i++) step();
    assert(HP.conn_rsps==rsps+1 && HP.last_conn_result==0x0002);
    assert(host.classic.phase==BT_CLASSIC_READY && !host.failed && !host.l2cap.malformed);
    puts("PASS: our SDP answered across continuations byte for byte; a request while one answer is held is dropped and counted; AVDTP not supported");
}

// The link drops mid-query: everything of that connection goes, and the
// next connection asks again, once; then a drop after an answer leaves it
// shown as the earlier connection's.
static void classic_sdp_drop(void)
{
    classic_bonded();
    HP.silent=true;
    for(unsigned i=0;i<100 && !HP.sdp_requests;i++) step();
    assert(HP.sdp_requests==1 && bt_sdp_client_state(&host.sdp)==BT_SDP_ASKING);
    cm_disconn(0,0x08); step();
    assert(host.classic.phase==BT_CLASSIC_STOPPED && !host.l2cap.up && l2cap_all_free());
    assert(bt_sdp_client_state(&host.sdp)==BT_SDP_IDLE && !host.sdp_asked && !host.sdp_channel && !host.sdp_result_valid);
    assert(status_has("l2cap: link down, not encrypted\n") && status_has("sdp: not asked\n"));
    HP.silent=false;
    // Paged again on the ladder, READY, and asked once more.
    for(unsigned i=0;i<20000 && !host.sdp_result_current;i++) { notify_keyboard(); step(); }
    assert(host.sdp_result_current && HP.conn_reqs==2 && status_has(SDP_SINK_LINE));
    for(unsigned i=0;i<5000;i++) step();
    assert(HP.conn_reqs==2 && host.sdp_ended==1);
    // Gone again: the answer is kept as the earlier connection's.
    cm_disconn(0,0x08); step();
    assert(host.sdp_result_valid && !host.sdp_result_current && host.sdp_result.l2cap_psm==0x0019);
    char want[160]; snprintf(want,sizeof want,"sdp: not asked\nearlier connection's %s",SDP_SINK_LINE);
    assert(status_has(want));
    puts("PASS: the link dropping mid-query resets everything; the next connection asks once; an answer outlives its connection, marked");
}

// Our channel refused: the query is recorded as not made, with the result,
// it is not asked again on that connection, and the link stays READY.
static void classic_sdp_refused(void)
{
    classic_bonded_refusing(0x0002);
    for(unsigned i=0;i<200 && !host.sdp_ended;i++) step();
    assert(HP.conn_reqs==1 && host.sdp_ended==1 && host.sdp_refused==0x0002 && host.l2cap_refusals==1);
    assert(status_has("sdp: not made: the headphones refused the SDP channel\n"));
    assert(status_has("last refusal: PSM not supported (0002) psm 0001\n"));
    assert(!strcmp(host.sdp_line,"sdp: not made: the headphones refused the SDP channel\n"));
    for(unsigned i=0;i<20000;i++) { notify_keyboard(); step(); }
    assert(HP.conn_reqs==1 && host.sdp_ended==1 && host.classic.phase==BT_CLASSIC_READY && !host.failed);
    puts("PASS: a refused SDP channel is recorded, not retried, and the link stays ready");
}

// A full-size Classic ACL packet (an L2CAP PDU of BT_L2CAP_MTU + 4: an Echo
// Request as large as our signalling MTU) between two keyboard packets, as
// one stream and as one transfer of 64-byte USB packets: each is delivered.
static void classic_full_packet(void)
{
    enum { MPS=64 };
    classic_bonded(); sdp_settle();
    static const uint8_t key[]={12,0x20,15,0,11,0,4,0,0x1b,10,0,2,0,4,0,0,0,0,0};
    static uint8_t stream[2*sizeof(key)+4+4+BT_L2CAP_MTU];
    size_t k=0;
    memcpy(stream,key,sizeof key); k+=sizeof key;
    uint16_t h=CM.handle|0x2000, acl_len=4+BT_L2CAP_MTU;
    stream[k++]=(uint8_t)h; stream[k++]=(uint8_t)(h>>8); stream[k++]=(uint8_t)acl_len; stream[k++]=(uint8_t)(acl_len>>8);
    stream[k++]=(uint8_t)BT_L2CAP_MTU; stream[k++]=(uint8_t)(BT_L2CAP_MTU>>8); stream[k++]=1; stream[k++]=0;
    stream[k++]=8; stream[k++]=0x77; stream[k++]=(uint8_t)(BT_L2CAP_MTU-4); stream[k++]=(uint8_t)((BT_L2CAP_MTU-4)>>8);
    for(unsigned i=0;i<BT_L2CAP_MTU-4;i++) stream[k++]=(uint8_t)i;
    memcpy(stream+k,key,sizeof key); k+=sizeof key;
    assert(k==sizeof stream && 4+acl_len==4+4+BT_L2CAP_MTU);
    for(unsigned form=0;form<2;form++) {
        unsigned typed=input_count[0], echoes=HP.echo_rsps;
        if(!form) bt_host_receive(&host,stream,k,0,multi_report,NULL);
        else for(size_t at=0;at<k;at+=MPS) bt_host_receive(&host,stream+at,k-at<MPS?k-at:MPS,MPS,multi_report,NULL);
        assert(input_count[0]==typed+2 && !host.wire_resyncs && !host.wire_used && !host.failed);
        assert(!host.l2cap.oversized && !host.l2cap.malformed && !host.l2cap.unfinished);
        for(unsigned i=0;i<5;i++) step();
        assert(HP.echo_rsps==echoes+1);
    }
    puts("PASS: a full-size Classic ACL packet between keyboard packets: assembled whole and answered, both forms, no resync");
}

// An LE peer cleaning up waits for its own packets to leave the send slots,
// not the Classic link's: Classic ahead of it in the slots, and behind it.
static void classic_cleanup_in_flight(void)
{
    for(unsigned order=0;order<2;order++) {
        classic_bonded(); sdp_settle();
        bt_le_t *m=&host.peers[1].le;
        *m=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,.connected=true,.encrypted=true,
            .handle=11,.input_value=10,.mouse=true};
        assert(hid_mouse_parse(packed,sizeof(packed),&m->mouse_layout));
        step();
        TX.hold=true;
        const uint8_t ack[]={0x1e};
        if(order==0) {      // Classic first, the mouse's behind it
            HP_CMD(8,HP.ident++,1); step(); step();
            assert(tx_holding(CM.handle)==1);
            assert(le_queue(m,4,ack,1)); step(); tx_n=0;
        } else {            // the mouse's first, Classic behind it
            assert(le_queue(m,4,ack,1)); step(); tx_n=0;
            HP_CMD(8,HP.ident++,1); HP_CMD(8,HP.ident++,2); step(); step(); step();
            assert(tx_holding(CM.handle)==2);
        }
        assert(tx_holding(11)==1);
        cmd_n=0;
        assert(bt_host_command(&host,"slot 1 disconnect",17,now));
        for(unsigned i=0;i<100;i++) { step(); assert(!cmd_n && m->phase==BT_LE_CLEANUP); }
        if(order==0) {
            // The Classic packet completes; the mouse's is still in its slot.
            TX.head=(TX.head+1)%BT_HOST_TX_SLOTS; TX.n--;
            for(unsigned i=0;i<100;i++) { step(); assert(!cmd_n && m->phase==BT_LE_CLEANUP); }
        }
        // The mouse's completes; with the Classic link's still held, or
        // none, the Disconnect goes at once.
        assert(TX.handle[TX.head]==11);
        TX.head=(TX.head+1)%BT_HOST_TX_SLOTS; TX.n--;
        step();
        assert(cmd_n && le_u16(cmd)==0x0406 && le_u16(cmd+3)==11);
        assert(tx_holding(CM.handle)==(order==0?0:2));
        const uint8_t ok[]={0x0f,4,0,1,6,4}; cmd_n=0; multi_event(ok,sizeof(ok));
        disconnected(1,0x16); step();
        assert(m->phase==BT_LE_IDLE && !host.failed);
        // The Classic link's packets held past the three seconds a cleanup
        // may take cost nothing.
        for(unsigned i=0;i<3500;i++) { notify_keyboard(); step(); assert(!host.failed); }
        assert(host.peers[0].le.phase==BT_LE_READY && !release_count[0]);
        TX.hold=false;
    }
    puts("PASS: an LE peer's cleanup waits for its own packets in the send slots, not the Classic link's, in both orders");
}

int main(void)
{
    (void)basic;
    mouse_interval();
    peer_att_requests();
    pairing_and_reconnect(); cancel_races(); credits_and_faults();
    rejected_create_and_fragmentation(); scan_recovery(); start_up_rounds(); acl_resync();
    saved_duplicate_requests();
    classic_quiet();
    classic_takes_only_its_own();
    classic_bond_while_typing();
    classic_urgent_order();
    classic_inbound_during_scan();
    classic_owner_bounds();
    classic_unavailable();
    classic_reconnection();
    classic_commands();
    classic_status();
    classic_sdp_query();
    classic_le_first();
    classic_pool_exhausted();
    classic_shared_pool();
    classic_sdp_asked();
    classic_sdp_drop();
    classic_sdp_refused();
    classic_full_packet();
    classic_cleanup_in_flight();
    puts("Bluetooth multi-peer host tests passed");
}
