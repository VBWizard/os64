// A simulated controller and keyboard use OpenSSL only on the host side.
// Production asks the AX210 for random numbers and AES; it links no TLS code.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "../kernel/src/driver/system/usb/bt_le.c"
static bt_le_t session;
static uint8_t cmd[35],tx[31],last_report[8],peer_nonce[16],pair_request[7];
static size_t cmd_n,tx_n;
static uint64_t now;
static unsigned commands,packets,keys,releases,rand_calls;
static int scenario;
enum { NORMAL, WRONG_CONFIRM, NO_BOOT, JUST_WORKS, REJECT_COMMAND, SHARED_BUFFER, GATT_ERROR };
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
    case 0x2017: aes(cmd+3,cmd+19,e+6); n+=16; break;
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
            uint8_t rsp[7]; memcpy(rsp,pair_response,7);
            if(scenario==JUST_WORKS) rsp[1]=3;
            incoming(6,rsp,7);
        } else if(p[0]==3) {
            uint8_t expected[16];
            confirm(session.tk,session.random,pair_request,pair_response,0,1,local_addr,peer_addr,expected);
            assert(n==17 && !memcmp(p+1,expected,16));
            uint8_t response[17]={3};
            confirm(session.tk,peer_nonce,pair_request,pair_response,0,1,local_addr,peer_addr,response+1);
            if(scenario==WRONG_CONFIRM) response[1]^=1;
            incoming(6,response,17);
        } else if(p[0]==4) {
            assert(n==17 && !memcmp(p+1,session.random,16));
            uint8_t response[17]={4}; memcpy(response+1,peer_nonce,16); incoming(6,response,17);
        } else assert(!"unexpected SMP request");
    } else if(le_u16(wire+6)==4) {
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
    const char *request="connect random f8:2c:fe:ff:f0:1a";
    assert(bt_le_request(&session,request,strlen(request),0));
    assert(!bt_le_request(&session,request,strlen(request),0));
}
static void run(void)
{
    for(unsigned i=0;i<1000 && session.phase!=BT_LE_READY && session.phase!=BT_LE_FAILED;i++) {
        tick(true,true); controller(); peripheral();
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
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10};
        keys=0;
        bt_le_receive(&session,wire,split,report,NULL);
        bt_le_receive(&session,wire+split,sizeof(wire)-split,report,NULL);
        assert(keys==1 && !session.malformed);
    }
    // HCI continuation fragments, themselves split across USB packets.
    for(unsigned split=4;split<15;split++) {
        uint8_t a[24]={0x0b,0x20},b[24]={0x0b,0x10};
        a[2]=split; memcpy(a+4,wire+4,split); b[2]=15-split; memcpy(b+4,wire+4+split,15-split);
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10}; keys=0;
        bt_le_receive(&session,a,split+4,report,NULL); bt_le_receive(&session,b,19-split,report,NULL);
        assert(keys==1);
    }
    unsigned r=0x1234;
    for(unsigned i=0;i<10000;i++) {
        uint8_t noise[80]; for(unsigned j=0;j<sizeof(noise);j++) { r=r*1664525+1013904223; noise[j]=r>>24; }
        session=(bt_le_t){.phase=BT_LE_READY,.connected=true,.encrypted=true,.handle=11,.boot_value=10};
        bt_le_receive(&session,noise,i%sizeof(noise),report,NULL);
        bt_le_event(&session,noise,i%sizeof(noise));
    }
    puts("PASS: USB/HCI fragmentation and hostile frame bounds under ASan/UBSan");
}
int main(void)
{
    vectors(); success(); failures(); framing();
    bt_le_t s={0};
    assert(!bt_le_request(&s,"connect random zz:00:00:00:00:00",32,0));
    assert(!bt_le_request(&s,"disconnect junk",15,0));
    char one[1]; assert(bt_le_status(&s,one,1)==0 && !one[0]);
    puts("Bluetooth LE host tests passed"); return 0;
}
