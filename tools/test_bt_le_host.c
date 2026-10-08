// A simulated controller and keyboard use OpenSSL only on the host side.
// Production asks the AX210 for random numbers and AES; it links no TLS code.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "../kernel/src/driver/system/usb/bt_le.c"
#include "hid_keyboard_fixtures.h"
static bt_le_t session;
static uint8_t cmd[35],tx[31],last_report[8],peer_nonce[16],pair_request[7];
static size_t cmd_n,tx_n;
static uint8_t peer_pair_response[7];
static uint64_t now;
static unsigned commands,packets,keys,releases,rand_calls;
static int scenario;
enum { NORMAL, WRONG_CONFIRM, NO_BOOT, JUST_WORKS, REJECT_COMMAND, SHARED_BUFFER, GATT_ERROR, JUST_WORKS_OPT_IN, JUST_WORKS_BAD_CONFIRM, JUST_WORKS_MITM, JUST_WORKS_INVALID_IO, REPORT_PROTOCOL, REPORT_PROTOCOL_NO_MODE, REPORT_BAD_REF, REPORT_BAD_MAP, REPORT_MAP_TOO_LONG, REPORT_MAP_EXACT };
static const uint8_t peer_addr[]={0x1a,0xf0,0xff,0xfe,0x2c,0xf8};
static const uint8_t local_addr[]={0x49,0x4e,0x35,0x80,0x2f,0x6c};
static const uint8_t pair_response[]={2,2,0,4,16,0,0};
static void report(void *ctx,const uint8_t p[8])
{ (void)ctx; memcpy(last_report,p,8); if(p[2] || p[0]) keys++; else releases++; }
static void command(void *ctx,uint16_t op,const uint8_t *p,uint8_t n)
{
    (void)ctx; assert(!cmd_n && n<=32); cmd[0]=op; cmd[1]=op>>8; cmd[2]=n;
    if(n) memcpy(cmd+3,p,n);
    cmd_n=n+3; commands++;
}
static void acl(void *ctx,const uint8_t *p,size_t n)
{ (void)ctx; assert(!tx_n && n<=31); memcpy(tx,p,n); tx_n=n; packets++; }
static void aes(const uint8_t key_le[16],const uint8_t plain_le[16],uint8_t out_le[16])
{
    uint8_t k[16],p[16],out[32];
    for(unsigned i=0;i<16;i++) { k[i]=key_le[15-i]; p[i]=plain_le[15-i]; }
    EVP_CIPHER_CTX *c=EVP_CIPHER_CTX_new(); assert(c);
    int n=0,last=0;
    assert(EVP_EncryptInit_ex(c,EVP_aes_128_ecb(),NULL,k,NULL)==1);
    assert(EVP_CIPHER_CTX_set_padding(c,0)==1);
    assert(EVP_EncryptUpdate(c,out,&n,p,16)==1 && n==16);
    assert(EVP_EncryptFinal_ex(c,out+n,&last)==1 && last==0);
    EVP_CIPHER_CTX_free(c);
    for(unsigned i=0;i<16;i++) out_le[i]=out[15-i];
}
// Independent, big-endian construction of the Core's c1 inputs.
static void confirm(const uint8_t key[16],const uint8_t random[16],
                    const uint8_t req[7],const uint8_t rsp[7],uint8_t iat,uint8_t rat,
                    const uint8_t ia[6],const uint8_t ra[6],uint8_t out[16])
{
    uint8_t big[16],p[16],tmp[16];
    for(unsigned i=0;i<7;i++) { big[i]=rsp[6-i]; big[7+i]=req[6-i]; }
    big[14]=rat; big[15]=iat;
    for(unsigned i=0;i<16;i++) p[i]=big[15-i]^random[i];
    aes(key,p,tmp);
    memset(big,0,4);
    for(unsigned i=0;i<6;i++) { big[4+i]=ia[5-i]; big[10+i]=ra[5-i]; }
    for(unsigned i=0;i<16;i++) p[i]=big[15-i]^tmp[i];
    aes(key,p,out);
}
static void fromhex(uint8_t *out,const char *hex,size_t n)
{ for(size_t i=0;i<n;i++) { unsigned x; assert(sscanf(hex+2*i,"%2x",&x)==1); out[n-1-i]=x; } }
static void vectors(void)
{
    uint8_t k[16]={0},r[16],req[7],rsp[7],ia[6],ra[6],expected[16],out[16];
    fromhex(r,"5783d52156ad6f0e6388274ec6702ee0",16);
    fromhex(req,"07071000000101",7); fromhex(rsp,"05000800000302",7);
    fromhex(ia,"a1a2a3a4a5a6",6); fromhex(ra,"b1b2b3b4b5b6",6);
    fromhex(expected,"1e1e3fef878988ead2a74dc5bef13b86",16);
    confirm(k,r,req,rsp,1,0,ia,ra,out); assert(!memcmp(out,expected,16));
    bt_le_t s={0}; uint8_t p[32];
    memcpy(s.request,req,7); memcpy(s.response,rsp,7); memcpy(s.local,ia,6); memcpy(s.peer,ra,6);
    le_c1_first(&s,r,p); p[16]^=1; // Published vector uses iat=1; this host uses public iat=0.
    aes(p,p+16,s.crypto); le_c1_second(&s,p); aes(p,p+16,out);
    assert(!memcmp(out,expected,16));
    fromhex(p,"112233445566778899aabbccddeeff00",16);
    fromhex(expected,"9a1fe1f0e8b0f49b5b4216ae796da062",16);
    aes(k,p,out); assert(!memcmp(out,expected,16));
}
static void event(const uint8_t *p,size_t n) { bt_le_event(&session,p,n); }
static void incoming(uint16_t cid,const uint8_t *p,size_t n)
{
    uint8_t a[80]={0x0b,0x20}; assert(n<=68);
    a[2]=n+4; a[4]=n; a[6]=cid; memcpy(a+8,p,n);
    // Deliberately split across every header and most payload boundaries.
    for(size_t i=0;i<n+8;i++) bt_le_receive(&session,a+i,1,report,NULL);
}
static void complete_acl(void)
{ const uint8_t e[]={0x13,5,1,0x0b,0,1,0}; event(e,sizeof(e)); }
static void controller(void)
{
    if(!cmd_n) return;
    uint16_t op=le_u16(cmd); uint8_t e[22]={0x0e,4,1,cmd[0],cmd[1],0}; size_t n=6;
    cmd_n=0;
    if(scenario==REJECT_COMMAND && op==0x2002) { e[5]=0x0c; event(e,n); return; }
    switch(op) {
    case 0x1009: memcpy(e+6,local_addr,6); n+=6; break;
    case 0x2002: e[6]=27; e[8]=scenario==SHARED_BUFFER?0:4; n+=3; break;
    case 0x1005: e[6]=27; e[9]=8; n+=7; break;
    case 0x200d: {
        assert(cmd[2]==25 && cmd[8]==1 && !memcmp(cmd+9,peer_addr,6));
        const uint8_t status[]={0x0f,4,0,1,0x0d,0x20}; event(status,sizeof(status));
        uint8_t conn[]={0x3e,19,1,0,0x0b,0,0,1,0,0,0,0,0,0,24,0,0,0,0x90,1,0};
        memcpy(conn+8,peer_addr,6); event(conn,sizeof(conn)); return;
    }
    case 0x2018:
        for(unsigned i=0;i<8;i++) e[6+i]=rand_calls*17+i+1;
        rand_calls++; n+=8; break;
    case 0x2017:
        if(scenario>=JUST_WORKS_OPT_IN) for(unsigned i=0;i<16;i++) assert(cmd[3+i]==0);
        aes(cmd+3,cmd+19,e+6); n+=16; break;
    case 0x2019: {
        uint8_t block[16],expected[16];
        memcpy(block,session.random,8); memcpy(block+8,peer_nonce,8);
        aes(session.tk,block,expected);
        assert(!memcmp(cmd+15,expected,16));
        for(unsigned i=5;i<15;i++) assert(!cmd[i]); // zero Rand and EDIV with the STK.
        const uint8_t status[]={0x0f,4,0,1,0x19,0x20}; event(status,sizeof(status));
        const uint8_t enc[]={8,4,0,0x0b,0,1}; event(enc,sizeof(enc)); return;
    }
    case 0x0c03: case 0x0c01: case 0x0c6d: case 0x2001: break;
    default: assert(!"unexpected command");
    }
    e[1]=n-2; event(e,n);
}
static void report_peripheral(const uint8_t *p,size_t n)
{
    unsigned handle=n>=3?le_u16(p+1):0;
    if(p[0]==0x10) {
        const uint8_t rsp[]={0x11,6,6,0,30,0,0x12,0x18}; incoming(4,rsp,sizeof(rsp));
    } else if(p[0]==8) {
        if(handle==6) {
            const uint8_t rsp[]={9,7,7,0,2,8,0,0x4b,0x2a,9,0,0x12,10,0,0x4d,0x2a};
            incoming(4,rsp,sizeof(rsp));
        } else if(handle==11) {
            const uint8_t rsp[]={9,7,15,0,0x12,16,0,0x4d,0x2a,21,0,4,22,0,0x4e,0x2a};
            incoming(4,rsp,scenario==REPORT_PROTOCOL_NO_MODE?9:sizeof(rsp));
        } else {
            uint8_t rsp[]={1,8,handle,handle>>8,0x0a}; incoming(4,rsp,sizeof(rsp));
        }
    } else if(p[0]==4) {
        assert(handle==11 || handle==13 || handle==17 || handle==19);
        if(handle==13 || handle==19) {
            uint8_t err[]={1,4,handle,0,0x0a}; incoming(4,err,sizeof(err));
        } else {
            uint8_t rsp[]={5,1,handle,0,2,0x29,handle+1,0,8,0x29}; incoming(4,rsp,sizeof(rsp));
        }
    } else if(p[0]==0x0a || p[0]==0x0c) {
        uint8_t rsp[23]={p[0]==0x0a?0x0b:0x0d};
        if(handle==8) {
            unsigned offset=p[0]==0x0c?le_u16(p+3):0;
            uint8_t map[HID_KEYBOARD_MAP_BYTES+44]; memset(map,0x64,sizeof(map));
            memcpy(map,keyboard_composite_map,sizeof(keyboard_composite_map));
            unsigned length=sizeof(keyboard_composite_map);
            if(scenario==REPORT_BAD_MAP) length--;
            if(scenario==REPORT_MAP_TOO_LONG) length=sizeof(map);
            if(scenario==REPORT_MAP_EXACT) length=((length+21)/22)*22;
            assert(offset<=length);
            unsigned part=length-offset; if(part>22) part=22;
            memcpy(rsp+1,map+offset,part); incoming(4,rsp,part+1);
        } else {
            assert(handle==12 || handle==18);
            rsp[1]=handle==12?2:scenario==REPORT_BAD_REF?99:7; rsp[2]=1; incoming(4,rsp,3);
        }
    } else if(p[0]==0x52) {
        assert(scenario!=REPORT_PROTOCOL_NO_MODE && handle==22 && n==4 && p[3]==1);
    } else if(p[0]==0x12) {
        assert(handle==17 && n==5 && le_u16(p+3)==1);
        uint8_t ok=0x13; incoming(4,&ok,1);
    } else assert(!"unexpected report protocol request");
}

static void peripheral(void)
{
    if(!tx_n) return;
    uint8_t wire[31]; memcpy(wire,tx,tx_n); size_t n=tx_n-8; tx_n=0;
    assert(le_u16(wire)==0x0b && le_u16(wire+2)==n+4 && le_u16(wire+4)==n);
    const uint8_t *p=wire+8;
    complete_acl();
    if(le_u16(wire+6)==6) {
        if(p[0]==1) {
            assert(n==7); memcpy(pair_request,p,7);
            if(scenario>=JUST_WORKS_OPT_IN) {
                const uint8_t expected[]={1,3,0,0,16,0,0};
                assert(!memcmp(p,expected,7));
                assert(!session.passkey_visible && !session.passkey);
            }
            incoming(6,peer_pair_response,7);
        } else if(p[0]==3) {
            uint8_t expected[16];
            confirm(session.tk,session.random,pair_request,peer_pair_response,0,1,local_addr,peer_addr,expected);
            assert(n==17 && !memcmp(p+1,expected,16));
            uint8_t response[17]={3};
            confirm(session.tk,peer_nonce,pair_request,peer_pair_response,0,1,local_addr,peer_addr,response+1);
            if(scenario==WRONG_CONFIRM || scenario==JUST_WORKS_BAD_CONFIRM) response[1]^=1;
            incoming(6,response,17);
        } else if(p[0]==4) {
            assert(n==17 && !memcmp(p+1,session.random,16));
            uint8_t response[17]={4}; memcpy(response+1,peer_nonce,16); incoming(6,response,17);
        } else assert(!"unexpected SMP request");
    } else if(le_u16(wire+6)==4) {
        if(scenario>=REPORT_PROTOCOL) { report_peripheral(p,n); return; }
        if(p[0]==0x10) {
            const uint8_t rsp[]={0x11,6,1,0,5,0,0,0x18,6,0,30,0,0x12,0x18};
            incoming(4,rsp,sizeof(rsp));
        } else if(p[0]==8) {
            if(le_u16(p+1)==6) {
                uint8_t rsp[]={9,7,7,0,4,8,0,0x4e,0x2a,9,0,0x12,10,0,0x22,0x2a};
                if(scenario==NO_BOOT) rsp[14]=0x4d;
                incoming(4,rsp,sizeof(rsp));
            } else if(le_u16(p+1)==11) {
                const uint8_t rsp[]={9,7,15,0,2,16,0,0x4a,0x2a}; incoming(4,rsp,sizeof(rsp));
            } else {
                const uint8_t rsp[]={1,8,17,0,0x0a}; incoming(4,rsp,sizeof(rsp));
            }
        } else if(p[0]==4) {
            assert(le_u16(p+1)==11 && le_u16(p+3)==14);
            const uint8_t rsp[]={5,1,11,0,2,0x29}; incoming(4,rsp,sizeof(rsp));
        } else if(p[0]==0x52) {
            assert(n==4 && le_u16(p+1)==8 && p[3]==0);
        } else if(p[0]==0x12) {
            assert(n==5 && le_u16(p+1)==11 && le_u16(p+3)==1);
            if(scenario==GATT_ERROR) {
                const uint8_t err[]={1,0x12,11,0,5}; incoming(4,err,sizeof(err));
            } else { uint8_t ok=0x13; incoming(4,&ok,1); }
        } else if(p[0]==3 || p[0]==0x1e) { /* server replies */ }
        else assert(!"unexpected ATT request");
    } else assert(le_u16(wire+6)==5);
}
static void tick(bool usb,bool bulk)
{ bt_le_tick(&session,++now,usb,bulk,false,command,acl,report,NULL); }
static void begin(int which)
{
    session=(bt_le_t){0}; cmd_n=tx_n=0; now=0; commands=packets=keys=releases=rand_calls=0;
    scenario=which; for(unsigned i=0;i<16;i++) peer_nonce[i]=0xf0-i;
    memcpy(peer_pair_response,pair_response,7);
    if(which==JUST_WORKS) peer_pair_response[1]=3;
    if(which>=JUST_WORKS_OPT_IN) {
        const uint8_t p5[]={2,3,0,1,16,0,0}; memcpy(peer_pair_response,p5,7);
        if(which==JUST_WORKS_MITM) peer_pair_response[3]|=4;
        if(which==JUST_WORKS_INVALID_IO) peer_pair_response[1]=5;
    }
    const char *request=which>=JUST_WORKS_OPT_IN ?
        "connect-justworks random f8:2c:fe:ff:f0:1a" : "connect random f8:2c:fe:ff:f0:1a";
    assert(bt_le_request(&session,request,strlen(request),0));
    assert(!bt_le_request(&session,request,strlen(request),0));
}
static void run(void)
{
    for(unsigned i=0;i<1000 && session.phase!=BT_LE_READY && session.phase!=BT_LE_FAILED;i++) {
        tick(true,true); controller(); peripheral();
        if(scenario>=JUST_WORKS_OPT_IN) assert(!session.passkey_visible && !session.passkey);
    }
}
static void success(void)
{
    begin(NORMAL); run(); assert(session.phase==BT_LE_READY && session.encrypted);
    assert(!session.passkey_visible && !session.passkey);
    for(unsigned i=0;i<16;i++) assert(!session.tk[i] && !session.crypto[i] && !session.random[i]);
    const uint8_t key[]={0x1b,10,0,2,0,4,0,0,0,0,0}; incoming(4,key,sizeof(key));
    assert(keys==1 && last_report[0]==2 && last_report[2]==4 && session.reports==1);
    uint8_t mtu[]={2,0,2}; incoming(4,mtu,sizeof(mtu)); tick(true,true); peripheral();
    assert(session.phase==BT_LE_READY);
    assert(bt_le_request(&session,"disconnect\n",11,now));
    for(unsigned i=0;i<10 && session.phase!=BT_LE_IDLE;i++) { tick(true,true); controller(); }
    assert(session.phase==BT_LE_IDLE && releases==1);
    assert(bt_le_request(&session,"connect public 01:02:03:04:05:06",32,now));
    puts("PASS: encrypted passkey pairing, paginated HID discovery, notification, release and reconnect");
}
static void just_works(void)
{
    begin(JUST_WORKS_OPT_IN); run();
    assert(session.phase==BT_LE_READY && session.encrypted && rand_calls==2);
    assert(!session.passkey_visible && !session.passkey);
    char status[1024]; bt_le_status(&session,status,sizeof(status));
    assert(strstr(status,"legacy Just Works (unauthenticated)") && !strstr(status,"type on the Bluetooth keyboard"));
    const uint8_t key[]={0x1b,10,0,2,0,4,0,0,0,0,0}; incoming(4,key,sizeof(key));
    assert(keys==1 && session.reports==1);
    assert(bt_le_request(&session,"disconnect",10,now));
    for(unsigned i=0;i<10 && session.phase!=BT_LE_IDLE;i++) { tick(true,true); controller(); }
    assert(session.phase==BT_LE_IDLE && releases==1);
    assert(bt_le_request(&session,"connect random f8:2c:fe:ff:f0:1a",32,now));
    assert(!session.just_works); // The next request chooses its own association mode.
    for(int which=JUST_WORKS_BAD_CONFIRM;which<=JUST_WORKS_INVALID_IO;which++) {
        begin(which); run();
        assert(session.phase==BT_LE_FAILED && !session.encrypted && !session.connected && !keys);
        if(which==JUST_WORKS_BAD_CONFIRM) assert(!strcmp(session.error,"pairing confirm mismatch"));
        if(which==JUST_WORKS_MITM) assert(!strcmp(session.error,"peer requires authenticated pairing"));
        if(which==JUST_WORKS_INVALID_IO) assert(!strcmp(session.error,"peer IO capability unsupported"));
    }
    bt_le_t s={0};
    const char *bad[]={"connect-justwork random f8:2c:fe:ff:f0:1a", "connect-justworks random f8:2c:fe:ff:f0:1a x",
        "connect-justworks random f8:2c:fe:ff:f0:zz", "connect-justworks", "connect-justworks  random f8:2c:fe:ff:f0:1a"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!bt_le_request(&s,bad[i],strlen(bad[i]),0));
    const char *ok="\tconnect-justworks public 01:02:03:04:05:06\n";
    assert(bt_le_request(&s,ok,strlen(ok),0));
    puts("PASS: explicit Just Works uses zero TK, retains confirm/encryption checks, and delivers keys without a passkey");
}

static void report_protocol(void)
{
    for(int which=REPORT_PROTOCOL;which<=REPORT_MAP_EXACT;which++) {
        begin(which); run();
        if(which==REPORT_BAD_REF || which==REPORT_BAD_MAP || which==REPORT_MAP_TOO_LONG) {
            assert(session.phase==BT_LE_FAILED && !keys);
            char status[8192]; size_t length=bt_le_status(&session,status,sizeof(status));
            assert(length==strlen(status) && strstr(status,"report map 0000: 05 01"));
            for(size_t cap=0;cap<sizeof(status);cap+=17) {
                memset(status,0xa5,sizeof(status));
                length=bt_le_status(&session,status,cap);
                assert((unsigned char)status[cap]==0xa5);
                if(cap) assert(length<cap && status[length]==0); else assert(!length);
            }
            continue;
        }
        assert(session.phase==BT_LE_READY && session.encrypted && session.report_protocol);
        assert(session.input_value==16 && session.ccc==17 && session.report_layout.report_id==7);
        assert(session.report_count==2 && session.report_layout.bytes==8);
        const uint8_t consumer[]={0x1b,10,0,0xe9,0}; incoming(4,consumer,sizeof(consumer)); assert(!keys);
        const uint8_t key[]={0x1b,16,0,2,0,4,0,0,0,0,0}; incoming(4,key,sizeof(key));
        assert(keys==1 && last_report[0]==2 && last_report[2]==4);
        const uint8_t wrong[]={0x1b,16,0,7,2,0,4,0,0,0,0,0}; incoming(4,wrong,sizeof(wrong));
        assert(session.phase==BT_LE_CLEANUP && keys==1);
        tick(true,true); assert(releases==1);
    }
    puts("PASS: report-only GATT keyboard, blob map reads, media filtering, optional Protocol Mode and malformed maps/references");
}

static void receive_diagnostics(void)
{
    begin(REPORT_PROTOCOL); run();
    assert(session.phase==BT_LE_READY && session.ready_ms && !session.rx_after_ready);
    assert(session.rx_bytes && session.rx_acl && !session.rx_unmatched_acl);
    assert(session.interval==24 && session.latency==0 && session.supervision_timeout==400);
    const uint8_t media[]={0x1b,10,0,0xe9,0}; incoming(4,media,sizeof(media));
    assert(session.rx_notifications==1 && session.rx_ignored_notifications==1 && !keys);
    assert(session.last_notification_handle==10 && session.last_notification_bytes==2);
    const uint8_t update[]={0x12,1,8,0,12,0,24,0,4,0,0x90,1};
    incoming(5,update,sizeof(update));
    assert(session.parameter_requests==1 && session.last_signal_opcode==0x12);
    assert(session.requested_parameters[0]==12 && session.requested_parameters[1]==24 &&
           session.requested_parameters[2]==4 && session.requested_parameters[3]==400);
    assert(session.queue_count==1 && session.queue[session.queue_head][8]==0x13 &&
           session.queue[session.queue_head][12]==1); // Existing rejection policy is unchanged.
    // A complete ACL frame for a different connection must not deliver a key.
    uint8_t other[]={0x0c,0x20,15,0,11,0,4,0,0x1b,16,0,0,0,4,0,0,0,0,0};
    bt_le_receive(&session,other,sizeof(other),report,NULL);
    assert(session.rx_unmatched_acl==1 && session.rx_after_ready==3 && !keys);
    unsigned acl_count=session.rx_acl;
    now+=64000; session.now=now;
    const uint8_t lost[]={5,4,0,0x0b,0,8}; event(lost,sizeof(lost));
    tick(true,true); controller(); tick(true,true);
    assert(session.phase==BT_LE_FAILED && session.disconnect_reason==8 && session.disconnected_ms);
    assert(session.rx_acl==acl_count && session.rx_notifications==1 && session.parameter_requests==1);
    char status[4096]; bt_le_status(&session,status,sizeof(status));
    assert(strstr(status,"disconnect reason: 08") && strstr(status,"notifications: total=1 ignored=1"));
    puts("PASS: receive metadata distinguishes ignored reports and connection updates, survives timeout cleanup");
}

static void failures(void)
{
    for(int kind=WRONG_CONFIRM;kind<=GATT_ERROR;kind++) {
        begin(kind); run();
        if(kind==SHARED_BUFFER) assert(session.phase==BT_LE_READY);
        else { assert(session.phase==BT_LE_FAILED && !session.connected && !keys); assert(session.error); }
    }
    begin(NORMAL); tick(true,true); unsigned sent=commands;
    controller(); tick(false,true); assert(commands==sent); // HCI reply cannot reuse DMA before USB completion.
    tick(true,true); assert(commands==sent+1);
    begin(NORMAL); tick(true,true); now=3000; tick(false,true);
    assert(session.phase==BT_LE_CLEANUP); sent=commands; tick(false,true); assert(commands==sent);
    now+=4000; tick(false,true); assert(session.phase==BT_LE_FAILED);
    begin(NORMAL); run();
    const uint8_t overcredit[]={0x13,5,1,0x0b,0,2,0}; event(overcredit,sizeof(overcredit));
    assert(session.phase==BT_LE_CLEANUP);
    begin(NORMAL); run();
    const uint8_t disconnected[]={5,4,0,0x0b,0,0x13}; event(disconnected,sizeof(disconnected));
    tick(true,true); assert(releases==1 && !session.encrypted);
    begin(NORMAL); run();
    bt_le_tick(&session,++now,true,true,true,command,acl,report,NULL);
    assert(session.phase==BT_LE_FAILED && releases==1 && session.link_may_active);
    puts("PASS: confirm mismatch, unsupported keyboard, Just Works refusal, command/GATT failure, credits and DMA ordering");
}
static void framing(void)
{
    const uint8_t wire[]={0x0b,0x20,15,0,11,0,4,0,0x1b,10,0,0,0,4,0,0,0,0,0};
    for(size_t split=1;split<sizeof(wire);split++) {
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10,.input_value=10};
        keys=0;
        bt_le_receive(&session,wire,split,report,NULL);
        bt_le_receive(&session,wire+split,sizeof(wire)-split,report,NULL);
        assert(keys==1 && !session.malformed);
    }
    // HCI continuation fragments, themselves split across USB packets.
    for(unsigned split=4;split<15;split++) {
        uint8_t a[24]={0x0b,0x20},b[24]={0x0b,0x10};
        a[2]=split; memcpy(a+4,wire+4,split); b[2]=15-split; memcpy(b+4,wire+4+split,15-split);
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10,.input_value=10}; keys=0;
        bt_le_receive(&session,a,split+4,report,NULL); bt_le_receive(&session,b,19-split,report,NULL);
        assert(keys==1);
    }
    unsigned r=0x1234;
    for(unsigned i=0;i<10000;i++) {
        uint8_t noise[80]; for(unsigned j=0;j<sizeof(noise);j++) { r=r*1664525+1013904223; noise[j]=r>>24; }
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10,.input_value=10};
        bt_le_receive(&session,noise,i%sizeof(noise),report,NULL);
        bt_le_event(&session,noise,i%sizeof(noise));
    }
    puts("PASS: USB/HCI fragmentation and hostile frame bounds under ASan/UBSan");
}
static void pairing_diagnostics(void)
{
    // P5 keyboard's actual response: NoInputNoOutput, bonding flag, 16-byte
    // key, no key distribution. A passkey request must not silently downgrade.
    const uint8_t p5_response[]={2,3,0,1,16,0,0};
    session=(bt_le_t){.phase=BT_LE_PAIR_WAIT,.connected=true,.handle=11,.now=1};
    incoming(6,p5_response,sizeof(p5_response));
    assert(session.phase==BT_LE_CLEANUP && !session.passkey_visible);
    assert(!strcmp(session.error,"peer cannot enter a displayed passkey"));
    assert(!memcmp(session.response,p5_response,sizeof(p5_response)));
    const uint8_t changed_field[]={1,2,4,5,6,3};
    const uint8_t changed_value[]={3,1,12,1,1,0xc0};
    const char *reasons[]={"peer cannot enter a displayed passkey", "peer OOB pairing flag unsupported",
        "peer encryption key size unsupported", "peer requested unsupported key distribution",
        "peer requested unsupported key distribution", "peer authentication flags unsupported"};
    for(unsigned i=0;i<sizeof(changed_field);i++) {
        uint8_t response[7]; memcpy(response,pair_response,7);
        response[changed_field[i]]=changed_value[i];
        session=(bt_le_t){.phase=BT_LE_PAIR_WAIT,.connected=true,.handle=11,.now=1};
        memset(session.tk,0x5a,16); cmd_n=tx_n=0; now=1;
        incoming(6,response,sizeof(response));
        assert(session.phase==BT_LE_CLEANUP);
        assert(!memcmp(session.response,response,7));
        assert(!strcmp(session.error,reasons[i]));
        tick(true,true); controller(); tick(true,true);
        assert(session.phase==BT_LE_FAILED && !memcmp(session.response,response,7));
        char status[1024]; bt_le_status(&session,status,sizeof(status));
        assert(strstr(status,"pairing response: 02 ") && strstr(status,"peer capabilities: IO=") && strstr(status,reasons[i]));
        for(unsigned j=0;j<16;j++) assert(session.tk[j]==0);
    }
    puts("PASS: rejected SMP capabilities survive cleanup and identify the unsupported field");
}

int main(void)
{
    vectors(); success(); failures(); framing(); pairing_diagnostics(); just_works(); report_protocol(); receive_diagnostics();
    bt_le_t s={0};
    assert(!bt_le_request(&s,"connect random zz:00:00:00:00:00",32,0));
    assert(!bt_le_request(&s,"disconnect junk",15,0));
    char one[1]; assert(bt_le_status(&s,one,1)==0 && !one[0]);
    puts("Bluetooth LE host tests passed"); return 0;
}
