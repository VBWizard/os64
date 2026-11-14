#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/system/usb/bt_scan.h"

static uint16_t sent_op;
static uint8_t sent_params[255], sent_length;
static unsigned sends;
// Every command sent, in order, with the parameters of the last of each
// of a few opcodes the tests read back.
static uint16_t sent_log[256];
static unsigned sent_logged;
static uint8_t mask_params[8], name_params[248];
static unsigned name_length;
static void send_command(void *ctx,uint16_t op,const uint8_t *p,uint8_t n)
{
    (void)ctx; sent_op=op; sent_length=n; sends++;
    memcpy(sent_params,p,n);   // a uint8_t length always fits
    if(sent_logged<256) sent_log[sent_logged++]=op;
    if(op==0x2001) assert(n==8 && p[0]==7); // Include Connection Update Complete.
    if(op==0x0c01) { assert(n==8); memcpy(mask_params,p,8); }
    if(op==0x0c13) { assert(n<=sizeof(name_params)); memcpy(name_params,p,n); name_length=n; }
}
// LMP features byte 4 the model controller reports: LE supported (0x40);
// BR/EDR Not Supported (0x20) when a test says so.
static uint8_t features4=0x40;
static void complete(bt_scan_t *s,uint8_t status)
{
    uint8_t e[14]={0x0e,4,1,(uint8_t)(s->opcode&255),(uint8_t)(s->opcode>>8),status};
    size_t n=6;
    if (!status && s->opcode==0x1003) { n=14;e[1]=12;e[10]=features4;e[12]=1; }
    if (!status && s->opcode==0x1009) { n=12;e[1]=10;e[6]=0x11;e[11]=0xaa; }
    // Read Buffer Size: 1021-byte ACL packets, 8 of them. LE Read Buffer Size: 251 bytes, 3.
    if (!status && s->opcode==0x1005) { n=13;e[1]=11;e[6]=0xfd;e[7]=3;e[8]=64;e[9]=8;e[11]=8; }
    if (!status && s->opcode==0x2002) { n=9;e[1]=7;e[6]=251;e[8]=3; }
    if (s->opcode==0x0401) {
        const uint8_t accepted[]={0x0f,4,status,1,1,4};
        bt_scan_event(s,accepted,sizeof(accepted));
    } else bt_scan_event(s,e,n);
}
static void tick(bt_scan_t *s,uint64_t now) { bt_scan_tick(s,now,true,false,send_command,NULL); }
static void until(bt_scan_t *s,bt_scan_phase_t phase,uint64_t now)
{
    for(unsigned i=0;i<20 && s->phase!=phase;i++) {
        if(s->pending) complete(s,0);
        tick(s,now+i);
    }
    assert(s->phase==phase);
}
static void reports(void)
{
    const uint8_t adv[]={0x3e,19,2,1,0,1,1,2,3,4,5,6,7,6,9,'B','o','a','r','d',0xd6};
    for(unsigned split=0;split<=sizeof(adv);split++) {
        bt_scan_t s={.phase=BT_SCAN_LE_WAIT}; bt_hci_stream_t stream={0};
        bt_hci_feed(&stream,bt_scan_event,&s,adv,split,0);
        bt_hci_feed(&stream,bt_scan_event,&s,adv+split,sizeof(adv)-split,0);
        assert(s.count==1 && s.devices[0].le && s.devices[0].address_type==1);
        assert(s.devices[0].rssi==-42 && !strcmp(s.devices[0].name,"Board"));
        bt_scan_event(&s,adv,sizeof(adv)); assert(s.count==1);
        uint8_t short_name[sizeof(adv)]; memcpy(short_name,adv,sizeof(adv));
        short_name[14]=8;short_name[15]='X';
        bt_scan_event(&s,short_name,sizeof(short_name));
        assert(!strcmp(s.devices[0].name,"Board"));
        short_name[7]++; short_name[15]=27;
        bt_scan_event(&s,short_name,sizeof(short_name));
        assert(s.count==2 && s.devices[1].name[0]=='?');
        char out[BT_SCAN_TEXT_BYTES];
        assert(bt_scan_devices(&s,out,sizeof(out))>0);
        assert(strstr(out,"06:05:04:03:02:01 random RSSI -42 dBm"));
        char tiny[2]={0}; assert(bt_scan_devices(&s,tiny,sizeof(tiny))==1 && !tiny[1]);
    }
    // Scan response fills in a previously unnamed advertisement.
    bt_scan_t s={.phase=BT_SCAN_LE_WAIT};
    uint8_t unnamed[]={0x3e,12,2,1,0,1,1,2,3,4,5,6,0,0xff};
    bt_scan_event(&s,unnamed,sizeof(unnamed)); assert(s.count==1 && !s.devices[0].name[0]);
    uint8_t scan_response[sizeof(adv)];memcpy(scan_response,adv,sizeof(adv));scan_response[4]=4;
    bt_scan_event(&s,scan_response,sizeof(scan_response));assert(s.count==1 && s.devices[0].name[0]);
    scan_response[5]=0;bt_scan_event(&s,scan_response,sizeof(scan_response));assert(s.count==2);
    // Malformed later report cannot publish an earlier report from the same event.
    uint8_t multi[sizeof(adv)+10];memcpy(multi,adv,sizeof(adv));
    multi[1]=sizeof(multi)-2;multi[3]=2;memset(multi+sizeof(adv),0,10);multi[sizeof(adv)+8]=31;
    unsigned count=s.count;bt_scan_event(&s,multi,sizeof(multi));assert(s.count==count && s.malformed_reports==1);
    for(unsigned i=0;i<100;i++) { scan_response[6]=i;bt_scan_event(&s,scan_response,sizeof(scan_response)); }
    assert(s.count==BT_SCAN_DEVICES && s.dropped);
    bt_scan_t classic={.phase=BT_SCAN_CLASSIC_WAIT};
    uint8_t eir[257]={0x2f,255,1,1,2,3,4,5,6,1,0,0x40,0x25,0,0,0,0xce,6,9,'M','o','u','s','e'};
    for(unsigned chunk=1;chunk<=64;chunk++) {
        classic=(bt_scan_t){.phase=BT_SCAN_CLASSIC_WAIT};bt_hci_stream_t stream={0};
        for(size_t off=0;off<sizeof(eir);off+=chunk) {
            size_t n=sizeof(eir)-off;if(n>chunk)n=chunk;
            bt_hci_feed(&stream,bt_scan_event,&classic,eir+off,n,0);
        }
        assert(classic.count==1 && !classic.devices[0].le && classic.devices[0].rssi==-50);
        assert(!strcmp(classic.devices[0].name,"Mouse") && classic.devices[0].device_class[0]==0x40);
    }
    const uint8_t basic[]={2,15,1,9,8,7,6,5,4,1,0,0,1,2,3,0,0};
    bt_scan_event(&classic,basic,sizeof(basic));assert(classic.count==2 && classic.devices[1].rssi==127);
    const uint8_t rssi[]={0x22,15,1,3,4,5,6,7,8,1,0,1,2,3,0,0,0xc0};
    bt_scan_event(&classic,rssi,sizeof(rssi));assert(classic.count==3 && classic.devices[2].rssi==-64);
    // Every advertised length and truncation is bounded even for hostile bytes.
    uint32_t random=7;
    for(unsigned i=0;i<5000;i++) {
        uint8_t e[257];for(unsigned j=0;j<sizeof(e);j++) {random=random*1664525+1013904223;e[j]=random>>24;}
        size_t n=2+(random%256);e[1]=n-2;e[0]=(i&1)?0x3e:0x2f;
        bt_scan_event(&s,e,n);bt_scan_event(&classic,e,n);
    }
}
// The two buffer-size replies: taken whole, refused at any other length,
// and never asked for by a scan on a controller already in service.
static void buffers(void)
{
    // LE only, so the round does not stop for a twelve-second inquiry.
    bt_scan_t s={0}; assert(bt_scan_start_le(&s,0));
    until(&s,BT_SCAN_BUFFER,1);
    assert(!s.buffers_known);
    tick(&s,30); assert(s.pending && sent_op==0x1005 && sent_length==0);
    complete(&s,0); tick(&s,31);
    assert(s.phase==BT_SCAN_LE_BUFFER && sent_op==0x2002 && !s.buffers_known);
    assert(s.acl_size==1021 && s.acl_total==8);
    complete(&s,0); tick(&s,32);
    // On to the Classic setup, which follows the buffers.
    assert(s.phase==BT_SCAN_SSP && sent_op==0x0c56 && s.buffers_known && s.le_acl_size==251 && s.le_acl_total==3);
    char out[512]; bt_scan_status(&s,out,sizeof(out));
    for(int which=0;which<2;which++) for(int delta=-1;delta<=1;delta+=2) {
        s=(bt_scan_t){0}; assert(bt_scan_start_le(&s,0));
        until(&s,which?BT_SCAN_LE_BUFFER:BT_SCAN_BUFFER,1); tick(&s,30);
        assert(s.pending && s.opcode==(which?0x2002:0x1005));
        uint8_t e[16]={0x0e,0,1,(uint8_t)s.opcode,(uint8_t)(s.opcode>>8),0};
        size_t n=(size_t)((which?9:13)+delta); e[1]=(uint8_t)(n-2);
        bt_scan_event(&s,e,n); tick(&s,31);
        assert(s.error && !strcmp(s.error,"malformed HCI reply") && !s.buffers_known);
    }
    // In service: straight to the scan itself.
    s=(bt_scan_t){.phase=BT_SCAN_DONE,.shared_controller=true};
    unsigned before=sends;
    assert(bt_scan_start_le(&s,0)); tick(&s,1);
    assert(sends==before+1 && sent_op==0x200b);
    s=(bt_scan_t){.phase=BT_SCAN_BUFFER}; bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"state: initializing"));
    s.phase=BT_SCAN_LE_BUFFER; bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"state: initializing"));
    s.phase=BT_SCAN_LE_PARAMS; bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"state: LE scan"));
}
// Run a start-up round (LE only, as the host starts it) to its end,
// answering every command with success except `refuse` (answered with
// `status`), or leaving `silent` unanswered. Returns where it stopped.
static bt_scan_phase_t startup(bt_scan_t *s,uint16_t refuse,uint8_t status,uint16_t silent)
{
    *s=(bt_scan_t){0}; assert(bt_scan_start_le(s,0));
    sent_logged=0; name_length=0; memset(mask_params,0,sizeof(mask_params));
    uint64_t now=0;
    for(unsigned i=0;i<200 && s->phase!=BT_SCAN_DONE && s->phase!=BT_SCAN_STOPPED && s->phase!=BT_SCAN_FAILED;i++) {
        tick(s,now);
        if(s->pending && s->opcode!=silent) complete(s,s->opcode==refuse?status:0);
        now+=s->phase==BT_SCAN_LE_WAIT?2000:10;
    }
    return s->phase;
}
static bool logged(uint16_t op)
{
    for(unsigned i=0;i<sent_logged;i++) if(sent_log[i]==op) return true;
    return false;
}
static const uint16_t classic_setup_ops[6]={0x0c56,0x0c13,0x0c24,0x080f,0x0c18,0x0c16};

// The start-up round's Classic setup (AUDIO_CLASSIC.md § L2): one widened
// event mask, six commands after the buffers, a refusal that costs only
// the Classic slot, and none of it on a controller with no BR/EDR.
static void classic_setup(void)
{
    bt_scan_t s;
    features4=0x40;
    assert(startup(&s,0,0,0)==BT_SCAN_DONE && !s.classic_setup_refused);
    // Bit n of the mask is event code n+1: the LE mask plus 0x03 0x04 0x06,
    // 0x12 0x14 0x16 0x17 0x18, 0x30, 0x31 0x32 0x33 0x36.
    static const uint8_t classic_mask[8]={0xbf,0xe0,0xee,0x00,0x02,0xc0,0x27,0x20};
    assert(!memcmp(mask_params,classic_mask,8));
    // The order: buffers, the six, then the LE scan.
    unsigned at=0;
    while(at<sent_logged && sent_log[at]!=0x2002) at++;
    assert(at+7<sent_logged);
    for(unsigned i=0;i<6;i++) assert(sent_log[at+1+i]==classic_setup_ops[i]);
    assert(sent_log[at+7]==0x200b);
    // Their parameters, exactly.
    for(unsigned i=0;i<6;i++) {
        bt_scan_t t={0}; assert(bt_scan_start_le(&t,0));
        for(unsigned k=0;k<40 && !(t.pending && t.opcode==classic_setup_ops[i]);k++) {
            if(t.pending) complete(&t,0);
            tick(&t,k);
        }
        assert(t.pending && t.opcode==classic_setup_ops[i] && sent_op==classic_setup_ops[i]);
        switch(i) {
        case 0: assert(sent_length==1 && sent_params[0]==1); break;
        case 1:
            assert(sent_length==248 && name_length==248 && !memcmp(name_params,"os64",4));
            for(unsigned b=4;b<248;b++) assert(!name_params[b]);
            break;
        case 2: assert(sent_length==3 && sent_params[0]==0x04 && sent_params[1]==0x01 && sent_params[2]==0x20); break;
        case 3: assert(sent_length==2 && sent_params[0]==0x05 && sent_params[1]==0x00); break;
        case 4: assert(sent_length==2 && sent_params[0]==0x00 && sent_params[1]==0x20); break;
        case 5: assert(sent_length==2 && sent_params[0]==0xa0 && sent_params[1]==0x1f); break;
        }
    }
    // Each refused: the round still reaches DONE, every later setup command
    // is still sent, and the refusal is recorded.
    for(unsigned i=0;i<6;i++) {
        assert(startup(&s,classic_setup_ops[i],0x0c,0)==BT_SCAN_DONE);
        assert(!s.error && s.classic_setup_refused);
        assert(s.classic_refused_opcode==classic_setup_ops[i] && s.classic_refused_status==0x0c);
        for(unsigned k=0;k<6;k++) assert(logged(classic_setup_ops[k]));
        assert(logged(0x200b) && logged(0x200c));
        assert(s.buffers_known);
    }
    // Two refused: the first is the one recorded.
    {
        bt_scan_t t={0}; assert(bt_scan_start_le(&t,0));
        for(unsigned k=0;k<200 && t.phase!=BT_SCAN_DONE;k++) {
            tick(&t,k*10+(t.phase==BT_SCAN_LE_WAIT?3000:0));
            if(t.pending) complete(&t,t.opcode==0x0c13?0x12:t.opcode==0x080f?0x0c:0);
        }
        assert(t.phase==BT_SCAN_DONE && t.classic_refused_opcode==0x0c13 && t.classic_refused_status==0x12);
    }
    // One unanswered fails the round as any command does.
    for(unsigned i=0;i<6;i++) {
        bt_scan_t t={0}; assert(bt_scan_start_le(&t,0));
        uint64_t now=0;
        for(unsigned k=0;k<400 && t.phase!=BT_SCAN_CLEANUP;k++) {
            tick(&t,now);
            if(t.pending && t.opcode!=classic_setup_ops[i]) complete(&t,0);
            now+=10;
        }
        assert(t.phase==BT_SCAN_CLEANUP && t.error && !strcmp(t.error,"HCI command timed out"));
        assert(t.error_opcode==classic_setup_ops[i] && !t.classic_setup_refused);
    }
    // A failing status on any other start-up command still fails the round.
    assert(startup(&s,0x1005,0x0c,0)==BT_SCAN_STOPPED);
    assert(s.error && !strcmp(s.error,"HCI command rejected") && !s.classic_setup_refused);
    // No BR/EDR: none of the six, and the mask is byte for byte the LE one.
    features4=0x60;
    assert(startup(&s,0,0,0)==BT_SCAN_DONE && !s.classic_setup_refused);
    for(unsigned k=0;k<6;k++) assert(!logged(classic_setup_ops[k]));
    static const uint8_t le_mask[8]={0x93,0xe0,0x04,0x00,0x02,0x40,0x00,0x20};
    assert(!memcmp(mask_params,le_mask,8));
    unsigned at2=0;
    while(at2<sent_logged && sent_log[at2]!=0x2002) at2++;
    assert(at2+1<sent_logged && sent_log[at2+1]==0x200b);
    features4=0x40;
    char out[1024];
    s=(bt_scan_t){.phase=BT_SCAN_POLICY}; bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"state: initializing"));
}

// `scan classic`: an inquiry on a controller in service, then the LE scan,
// and nothing of the start-up round.
static void classic_scan(void)
{
    bt_scan_t s;
    features4=0x40;
    assert(startup(&s,0,0,0)==BT_SCAN_DONE);
    // Not in service yet.
    assert(!bt_scan_start_classic(&s,100));
    s.shared_controller=true;
    sent_logged=0;
    assert(bt_scan_start_classic(&s,100) && s.phase==BT_SCAN_INQUIRY_MODE && !s.le_only);
    // Busy.
    assert(!bt_scan_start_classic(&s,100) && !bt_scan_start_le(&s,100));
    char out[1024];
    tick(&s,100); assert(sent_op==0x0c45 && sent_length==1 && sent_params[0]==2);
    bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"state: Classic inquiry"));
    complete(&s,0); tick(&s,101);
    assert(s.phase==BT_SCAN_INQUIRY && sent_op==0x0401 && s.inquiry_active && s.radio_active);
    complete(&s,0); tick(&s,102); assert(s.phase==BT_SCAN_CLASSIC_WAIT);
    const uint8_t found[]={2,15,1,9,8,7,6,5,4,1,0,0,1,2,3,0,0};
    bt_scan_event(&s,found,sizeof(found)); assert(s.count==1 && !s.devices[0].le);
    const uint8_t done[]={1,1,0}; bt_scan_event(&s,done,sizeof(done)); assert(!s.inquiry_active);
    tick(&s,5000); assert(s.phase==BT_SCAN_LE_PARAMS && sent_op==0x200b);
    complete(&s,0); tick(&s,5001); assert(s.phase==BT_SCAN_LE_ENABLE);
    complete(&s,0); tick(&s,5002); assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,14000); assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,15002); assert(s.phase==BT_SCAN_LE_DISABLE);
    complete(&s,0); tick(&s,15003); assert(s.phase==BT_SCAN_DONE && !s.error && s.count==1);
    static const uint16_t want[]={0x0c45,0x0401,0x200b,0x200c,0x200c};
    assert(sent_logged==5 && !memcmp(sent_log,want,sizeof(want)));
    // No BR/EDR: refused.
    s.features[4]|=0x20; assert(!bt_scan_start_classic(&s,20000)); s.features[4]&=~0x20;
    // A failed inquiry on a controller in service is cancelled, then the LE
    // scan is stopped; the controller is never reset.
    for(unsigned refuse=0;refuse<2;refuse++) {
        assert(bt_scan_start_classic(&s,20000));
        sent_logged=0;
        tick(&s,20000); complete(&s,0); tick(&s,20001); complete(&s,0); tick(&s,20002);
        assert(s.phase==BT_SCAN_CLASSIC_WAIT && s.inquiry_active);
        tick(&s,32002); assert(s.phase==BT_SCAN_CLEANUP && !strcmp(s.error,"Classic inquiry timed out"));
        tick(&s,32003); assert(sent_op==0x0402 && sent_length==0);
        // Refused: no inquiry was running after all. Still an answer.
        complete(&s,refuse?0x0c:0); tick(&s,32004);
        // The LE half never began, so there is no LE scan to disable: a
        // disable for a scan that is not running is a command a controller
        // may refuse, and a refusal in cleanup would fail the host.
        assert(!s.inquiry_active && !s.le_scan_on);
        tick(&s,32005);
        assert(s.phase==BT_SCAN_STOPPED && !s.radio_active && !s.pending);
        static const uint16_t cleanup[]={0x0c45,0x0401,0x0402};
        assert(sent_logged==3 && !memcmp(sent_log,cleanup,sizeof(cleanup)));
    }
    // The cancel unanswered: the cleanup fails as any cleanup does.
    assert(bt_scan_start_classic(&s,40000));
    tick(&s,40000); complete(&s,0); tick(&s,40001); complete(&s,0); tick(&s,40002);
    tick(&s,52002); tick(&s,52003); assert(sent_op==0x0402);
    tick(&s,54003); assert(s.phase==BT_SCAN_FAILED);
    // A refused inquiry: cleaned up the same way.
    s=(bt_scan_t){.phase=BT_SCAN_DONE,.shared_controller=true,.features={[4]=0x40}};
    assert(bt_scan_start_classic(&s,0));
    sent_logged=0;
    tick(&s,0); complete(&s,0); tick(&s,1); complete(&s,0x0c); tick(&s,2);
    assert(s.phase==BT_SCAN_CLEANUP && !strcmp(s.error,"HCI command rejected"));
    tick(&s,3); assert(sent_op==0x0402);
    complete(&s,0x0c); tick(&s,4); tick(&s,5);
    assert(s.phase==BT_SCAN_STOPPED && sent_op==0x0402);   // the cancel was the last thing sent
    // A Classic scan that fails once its LE half HAS begun stops that too.
    s=(bt_scan_t){.phase=BT_SCAN_DONE,.shared_controller=true,.features={[4]=0x40}};
    assert(bt_scan_start_classic(&s,0));
    tick(&s,0); complete(&s,0); tick(&s,1); complete(&s,0); tick(&s,2);
    { const uint8_t done[]={1,1,0}; bt_scan_event(&s,done,sizeof(done)); }
    tick(&s,3); assert(s.phase==BT_SCAN_LE_PARAMS && !s.inquiry_active && sent_op==0x200b);
    complete(&s,0); tick(&s,4); assert(sent_op==0x200c && sent_params[0]==1 && s.le_scan_on);
    complete(&s,0); tick(&s,5); assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,40001); assert(s.phase==BT_SCAN_CLEANUP && !strcmp(s.error,"scan deadline expired"));
    tick(&s,40002); assert(sent_op==0x200c && sent_length==2 && !sent_params[0]);
    complete(&s,0); tick(&s,40003); assert(s.phase==BT_SCAN_STOPPED);
    // ...and one that completes switches its LE scan off and says so.
    s=(bt_scan_t){.phase=BT_SCAN_DONE,.shared_controller=true,.features={[4]=0x40}};
    assert(bt_scan_start_classic(&s,0));
    tick(&s,0); complete(&s,0); tick(&s,1); complete(&s,0); tick(&s,2);
    { const uint8_t done[]={1,1,0}; bt_scan_event(&s,done,sizeof(done)); }
    tick(&s,3); complete(&s,0); tick(&s,4); complete(&s,0); tick(&s,5);
    tick(&s,10006); assert(s.phase==BT_SCAN_LE_DISABLE && s.le_scan_on);
    complete(&s,0); tick(&s,10007); assert(s.phase==BT_SCAN_DONE && !s.le_scan_on);
    // An LE scan's cleanup is what it was: the LE scan stopped, nothing else.
    s=(bt_scan_t){.phase=BT_SCAN_DONE,.shared_controller=true,.features={[4]=0x40}};
    assert(bt_scan_start_le(&s,0));
    sent_logged=0;
    tick(&s,0); complete(&s,0x12); tick(&s,1);
    assert(s.phase==BT_SCAN_CLEANUP); tick(&s,2);
    complete(&s,0); tick(&s,3); assert(s.phase==BT_SCAN_STOPPED);
    static const uint16_t le_cleanup[]={0x200b,0x200c};
    assert(sent_logged==2 && !memcmp(sent_log,le_cleanup,sizeof(le_cleanup)));
    // A Classic scan on an exclusive controller (the start-up round) still
    // resets in its cleanup, and goes on to the LE setup after the inquiry.
    s=(bt_scan_t){0}; assert(bt_scan_start(&s,0));
    sent_logged=0;
    for(unsigned k=0;k<40 && s.phase!=BT_SCAN_CLASSIC_WAIT;k++) { if(s.pending) complete(&s,0); tick(&s,k); }
    tick(&s,13000); assert(s.phase==BT_SCAN_CLEANUP); tick(&s,13001); assert(sent_op==0x0c03);
}

static void late_completion(void)
{
    bt_scan_t s={0}; assert(bt_scan_start(&s,0)); tick(&s,0);
    complete(&s,0); tick(&s,2001);
    assert(s.phase==BT_SCAN_FEATURES && !s.error && sent_op==0x1003);
    // A reply alone does not release DMA, and a USB completion is not an HCI reply.
    for(unsigned kind=0;kind<5;kind++) {
        s=(bt_scan_t){0}; assert(bt_scan_start(&s,0)); tick(&s,0);
        if(kind!=1) complete(&s,kind==2?0x0c:0);
        if(kind==3) s.bad_reply=true;
        unsigned before=sends;
        bt_scan_tick(&s,2001,kind!=0,kind==4,send_command,NULL);
        assert(s.phase==(kind==4?BT_SCAN_FAILED:BT_SCAN_CLEANUP) && s.error);
        assert(sends==before);
        if(kind==2) assert(s.error_status==0x0c && !strcmp(s.error,"HCI command rejected"));
    }
    // An unfinished scan still has its overall limit, even with a command reply.
    s=(bt_scan_t){0}; assert(bt_scan_start(&s,0)); tick(&s,0); complete(&s,0);
    tick(&s,40000); assert(s.phase==BT_SCAN_CLEANUP && !strcmp(s.error,"scan deadline expired"));
    // Cleanup accepts a completed Reset seen late, retaining the original error.
    s=(bt_scan_t){0}; assert(bt_scan_start(&s,0)); tick(&s,0); tick(&s,2000);
    tick(&s,2001); complete(&s,0); tick(&s,4002);
    assert(bt_scan_quiescent(&s) && !s.radio_active && !strcmp(s.error,"HCI command timed out"));
    char out[512]; bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"stopped (retry available)"));
    // The error says which half of the timed-out command was missing: here
    // the USB transfer had completed and the controller never replied.
    assert(s.timed_out && s.timeout_usb_done && !s.timeout_reply);
    assert(strstr(out,"HCI command timed out (opcode 0c03 status 00); the USB transfer completed, no reply came"));
    // The other half: the reply is there and the transfer is not.
    { bt_scan_t u={0}; assert(bt_scan_start(&u,0)); tick(&u,0); complete(&u,0);
      bt_scan_tick(&u,2000,false,false,send_command,NULL);
      assert(u.timed_out && !u.timeout_usb_done && u.timeout_reply);
      char text[512]; bt_scan_status(&u,text,sizeof(text));
      assert(strstr(text,"; the USB transfer had not completed")); }
    // A timeout that is NOT the recorded error is not printed beside it: a
    // rejected command, then a cleanup reset that goes unanswered.
    { bt_scan_t r={0}; assert(bt_scan_start(&r,0)); tick(&r,0); complete(&r,0x0c);
      tick(&r,1); assert(r.phase==BT_SCAN_CLEANUP && !strcmp(r.error,"HCI command rejected"));
      tick(&r,2); tick(&r,2002); assert(r.phase==BT_SCAN_FAILED);
      char text[512]; bt_scan_status(&r,text,sizeof(text));
      assert(!r.timed_out && strstr(text,"HCI command rejected (opcode 0c03 status 0c)\n")); }
    // Nothing timed out: nothing is said.
    { bt_scan_t ok={0}; assert(bt_scan_start(&ok,0)); tick(&ok,0);
      char text[512]; bt_scan_status(&ok,text,sizeof(text));
      assert(!ok.timed_out && strstr(text,"error: none (opcode 0000 status 00)\n")); }
    unsigned before=sends; tick(&s,9000); assert(sends==before && s.error);
    assert(bt_scan_start(&s,9001) && !s.error);
    for(unsigned kind=0;kind<3;kind++) {
        s=(bt_scan_t){.phase=BT_SCAN_CLASSIC_WAIT,.deadline=10,.total_deadline=40000,.radio_active=true};
        tick(&s,10); tick(&s,11);
        if(kind!=1) complete(&s,kind==2?0x0c:0);
        bt_scan_tick(&s,2012,kind!=0,false,send_command,NULL);
        assert(s.phase==BT_SCAN_FAILED && s.radio_active && !bt_scan_start(&s,2013));
    }
    // An expired cleanup may not start a new Reset merely because USB finally finished.
    s=(bt_scan_t){0}; assert(bt_scan_start(&s,0)); tick(&s,0); tick(&s,2000);
    before=sends; tick(&s,4000); assert(s.phase==BT_SCAN_FAILED && sends==before);
    assert(!bt_scan_start(&s,4001));
}
int main(void)
{
    assert(bt_scan_request_valid("scan\n",5) && bt_scan_request_valid(" \tscan\r\n",8));
    assert(!bt_scan_request_valid("scan x",6) && !bt_scan_request_valid("scan\0",5));
    assert(!bt_scan_request_valid("",0));
    assert(bt_scan_request_valid("scan classic\n",13) && bt_scan_request_classic(" scan classic\r\n",15));
    assert(!bt_scan_request_classic("scan\n",5) && !bt_scan_request_classic("scan  classic",13));
    assert(!bt_scan_request_valid("scan classicx",13) && !bt_scan_request_valid("scan classi",11));
    assert(!bt_scan_request_valid("scan  classic",13) && !bt_scan_request_classic("scan classic\0",13));
    reports(); buffers(); late_completion(); classic_setup(); classic_scan();
    bt_scan_t s={0};assert(bt_scan_start(&s,0));assert(!bt_scan_start(&s,0));
    tick(&s,0);assert(sent_op==0x0c03);
    complete(&s,0);unsigned before=sends;
    bt_scan_tick(&s,1,false,false,send_command,NULL);assert(sends==before);
    tick(&s,2);assert(sent_op==0x1003);
    const uint8_t stale[]={0x0e,4,1,9,0xfc,0};bt_scan_event(&s,stale,sizeof(stale));assert(!s.command_done);
    until(&s,BT_SCAN_INQUIRY,5);
    assert(sent_op==0x0401 && sent_length==5 && sent_params[0]==0x33 && sent_params[3]==8);
    complete(&s,0);tick(&s,20);assert(s.phase==BT_SCAN_CLASSIC_WAIT);
    const uint8_t done[]={1,1,0};bt_scan_event(&s,done,sizeof(done));
    until(&s,BT_SCAN_LE_ENABLE,21);assert(sent_op==0x200c && sent_params[0]==1);
    complete(&s,0);tick(&s,30);assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,10029);assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,10030);assert(s.phase==BT_SCAN_LE_DISABLE && sent_op==0x200c && !sent_params[0]);
    complete(&s,0);tick(&s,10031);assert(s.phase==BT_SCAN_DONE && !s.radio_active);
    assert(bt_scan_start(&s,20000));assert(s.count==0);tick(&s,20000);complete(&s,0x0c);
    tick(&s,20001);assert(s.phase==BT_SCAN_CLEANUP);tick(&s,20002);assert(sent_op==0x0c03);
    complete(&s,0);tick(&s,20003);assert(s.phase==BT_SCAN_STOPPED && !s.radio_active);
    assert(s.error_status==0x0c && bt_scan_start(&s,20004));
    // No command response: cleanup is bounded, and preserves DMA ownership.
    s=(bt_scan_t){0};bt_scan_start(&s,0);tick(&s,0);tick(&s,2000);
    assert(s.phase==BT_SCAN_CLEANUP);before=sends;
    bt_scan_tick(&s,2001,false,false,send_command,NULL);assert(sends==before);
    bt_scan_tick(&s,4000,false,false,send_command,NULL);assert(s.phase==BT_SCAN_FAILED);
    s=(bt_scan_t){.phase=BT_SCAN_CLASSIC_WAIT,.deadline=10,.total_deadline=40000,.radio_active=true};
    tick(&s,10);assert(s.phase==BT_SCAN_CLEANUP);tick(&s,11);complete(&s,0);tick(&s,12);
    assert(s.phase==BT_SCAN_STOPPED && !s.radio_active);
    s=(bt_scan_t){.phase=BT_SCAN_LE_WAIT,.radio_active=true};
    bt_scan_tick(&s,0,true,true,send_command,NULL);assert(s.phase==BT_SCAN_FAILED && s.radio_active);
    char out[512];bt_scan_status(&s,out,sizeof(out));assert(strstr(out,"USB transport failed"));
    s=(bt_scan_t){0};
    const uint8_t hardware_error[]={0x10,1,0x42};
    bt_scan_event(&s,hardware_error,sizeof(hardware_error));tick(&s,0);
    assert(s.phase==BT_SCAN_CLEANUP && s.error_status==0x42);
    tick(&s,1);complete(&s,0);tick(&s,2);assert(s.phase==BT_SCAN_STOPPED);
    s=(bt_scan_t){0};bt_scan_start(&s,0);tick(&s,0);complete(&s,0);s.credits=0;
    before=sends;tick(&s,1);assert(sends==before && !s.pending);
    const uint8_t credit[]={0x0e,3,1,0,0};bt_scan_event(&s,credit,sizeof(credit));
    tick(&s,2);assert(sends==before+1 && sent_op==0x1003);
    s=(bt_scan_t){0}; assert(bt_scan_start_le(&s,0));
    for(unsigned t=0;t<20 && s.phase!=BT_SCAN_LE_ENABLE;t++) {
        if(s.pending) complete(&s,0);
        tick(&s,t); assert(sent_op!=0x0401 && sent_op!=0x0c45);
    }
    assert(s.phase==BT_SCAN_LE_ENABLE && s.le_only);
    complete(&s,0); tick(&s,30);
    tick(&s,2029); assert(s.phase==BT_SCAN_LE_WAIT);
    tick(&s,2030); assert(s.phase==BT_SCAN_LE_DISABLE && !sent_params[0]);
    complete(&s,0); tick(&s,2031); assert(s.phase==BT_SCAN_DONE && !s.radio_active);
    // The start-up round asked the controller for its ACL buffers, once.
    assert(s.buffers_known && s.acl_size==1021 && s.acl_total==8 && s.le_acl_size==251 && s.le_acl_total==3);
    s.shared_controller=true;
    uint8_t address[6],features[8]; memcpy(address,s.address,6); memcpy(features,s.features,8);
    assert(bt_scan_start_le(&s,3000) && s.phase==BT_SCAN_LE_PARAMS);
    assert(!memcmp(s.address,address,6) && !memcmp(s.features,features,8));
    bt_scan_status(&s,out,sizeof(out));
    // The last line promises what each spelling of a write starts.
    assert(strstr(out,"controller: aa:00:00:00:00:11"));
    assert(strstr(out,"\nwrite scan to start LE discovery (about 2 seconds), or scan classic to start "
                      "Classic then LE discovery (about 21 seconds)\n"));
    s.phase=BT_SCAN_DONE; s.shared_controller=false;
    assert(bt_scan_start(&s,4000) && !s.le_only);
    bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"or scan classic to start Classic then LE discovery"));
    s.features[4]|=0x20; bt_scan_status(&s,out,sizeof(out));
    assert(strstr(out,"\nwrite scan to start LE discovery (about 2 seconds); this controller has no Classic radio\n"));
    assert(!strstr(out,"scan classic"));
    puts("test_bt_scan_host: command sequencing, deadlines, cleanup, Classic/LE reports and bounded names passed");
}
