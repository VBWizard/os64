// Reuse the independently calculated SMP peer and paginated GATT server.
#define main single_peer_suite
#include "test_bt_le_host.c"
#undef main
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
static void multi_event(const uint8_t *p,size_t n) { bt_host_event(&host,p,n); }
static void multi_wire(const uint8_t *p,size_t n)
{ bt_host_receive(&host,p,n,multi_report,NULL); }
static void multi_command(void *ctx,uint16_t op,const uint8_t *p,uint8_t n)
{ if(op==0x0c03) reset_count++; command(ctx,op,p,n); }
static void step(void)
{ bt_host_tick(&host,++now,true,true,false,multi_command,acl,multi_report,NULL); }
static void notify_keyboard(void)
{
    const uint8_t a[]={12,0x20,15,0,11,0,4,0,0x1b,10,0,2,0,4,0,0,0,0,0};
    multi_wire(a,sizeof(a));
}
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
    bt_host_tick(&host,++now,false,true,false,multi_command,acl,multi_report,NULL);
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
        bt_host_tick(&host,++now,false,true,false,multi_command,acl,multi_report,NULL);
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
int main(void)
{
    (void)basic;
    mouse_interval();
    peer_att_requests();
    pairing_and_reconnect(); cancel_races(); credits_and_faults();
    rejected_create_and_fragmentation(); scan_recovery();
    puts("Bluetooth multi-peer host tests passed");
}
