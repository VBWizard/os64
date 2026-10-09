#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/system/usb/bt_scan.h"

static uint16_t sent_op;
static uint8_t sent_params[8], sent_length;
static unsigned sends;
static void send_command(void *ctx,uint16_t op,const uint8_t *p,uint8_t n)
{
    (void)ctx; sent_op=op; sent_length=n; sends++;
    assert(n<=sizeof(sent_params)); memcpy(sent_params,p,n);
    if(op==0x2001) assert(n==8 && p[0]==7); // Include Connection Update Complete.
}
static void complete(bt_scan_t *s,uint8_t status)
{
    uint8_t e[14]={0x0e,4,1,s->opcode&255,s->opcode>>8,status};
    size_t n=6;
    if (!status && s->opcode==0x1003) { n=14;e[1]=12;e[10]=0x40;e[12]=1; }
    if (!status && s->opcode==0x1009) { n=12;e[1]=10;e[6]=0x11;e[11]=0xaa; }
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
        bt_hci_feed(&stream,bt_scan_event,&s,adv,split);
        bt_hci_feed(&stream,bt_scan_event,&s,adv+split,sizeof(adv)-split);
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
            bt_hci_feed(&stream,bt_scan_event,&classic,eir+off,n);
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
    reports(); late_completion();
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
    s.shared_controller=true;
    uint8_t address[6],features[8]; memcpy(address,s.address,6); memcpy(features,s.features,8);
    assert(bt_scan_start_le(&s,3000) && s.phase==BT_SCAN_LE_PARAMS);
    assert(!memcmp(s.address,address,6) && !memcmp(s.features,features,8));
    bt_scan_status(&s,out,sizeof(out));
    assert(strstr(out,"controller: aa:00:00:00:00:11") && strstr(out,"LE discovery (about 2 seconds)"));
    assert(!strstr(out,"Classic then"));
    s.phase=BT_SCAN_DONE; s.shared_controller=false;
    assert(bt_scan_start(&s,4000) && !s.le_only);
    bt_scan_status(&s,out,sizeof(out)); assert(strstr(out,"Classic then LE discovery"));
    puts("test_bt_scan_host: command sequencing, deadlines, cleanup, Classic/LE reports and bounded names passed");
}
