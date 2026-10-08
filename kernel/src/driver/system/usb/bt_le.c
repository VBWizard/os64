// Bluetooth Core Vol 3 Parts A/F/H and Vol 4 Part E: single-peer LE central,
// legacy SMP Passkey Entry/explicit Just Works, and HID keyboard services.
#include "driver/system/usb/bt_le.h"
#include "strings/sprintf.h"

static uint16_t le_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static void le_put(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void le_copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; }
static bool le_equal(const uint8_t *a,const uint8_t *b,size_t n)
{ uint8_t x=0; while(n--) x|=*a++ ^ *b++; return !x; }
static void le_wipe(void *p,size_t n)
{ volatile uint8_t *v=p; while(n--) *v++=0; }
static void le_secrets_clear(bt_le_t *s)
{
    le_wipe(s->tk,16); le_wipe(s->crypto,16); le_wipe(s->random,16);
    le_wipe(s->peer_random,16); le_wipe(s->peer_confirm,16); le_wipe(s->reply,16);
    s->passkey=0; s->passkey_visible=false;
}
static void le_fail(bt_le_t *s,const char *why)
{
    if (s->phase==BT_LE_CLEANUP || s->phase==BT_LE_FAILED) return;
    if (!s->error) { s->error=why; s->error_opcode=s->opcode; s->error_status=s->status; }
    s->phase=BT_LE_CLEANUP; s->deadline=s->now+3000;
    s->queue_count=0; le_wipe(s->queue,sizeof(s->queue));
    s->att_pending=false; s->release_pending=true;
    le_secrets_clear(s);
}
static void le_bad(bt_le_t *s) { s->malformed++; le_fail(s,"malformed peer/controller packet"); }
static bool le_space(char c) { return c==' ' || c=='\n' || c=='\r' || c=='\t'; }
static int le_hex(char c)
{
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
bool bt_le_input_active(const bt_le_t *s)
{
    return s->connected && s->encrypted && (s->phase==BT_LE_READY ||
        (s->ready_ms && (s->phase==BT_LE_VERIFY_CCC || s->phase==BT_LE_VERIFY_PROTOCOL)));
}
bool bt_le_request(bt_le_t *s,const char *p,size_t n,uint64_t now)
{
    while(n && le_space(*p)) { p++; n--; }
    while(n && le_space(p[n-1])) n--;
    if(n==10 && le_equal((const uint8_t *)p,(const uint8_t *)"disconnect",10)) {
        if(s->phase==BT_LE_IDLE || s->phase==BT_LE_FAILED || s->phase==BT_LE_CLEANUP) return false;
        s->stop_requested=true; return true;
    }
    if(n==7 && le_equal((const uint8_t *)p,(const uint8_t *)"inspect",7)) {
        if(s->phase!=BT_LE_READY || !bt_le_input_active(s) || s->att_pending) return false;
        s->ccc_read=false; s->protocol_read=false;
        s->verified_ccc=0; s->verified_protocol=0;
        s->phase=BT_LE_VERIFY_CCC; s->now=now; s->total_deadline=now+10000;
        return true;
    }
    if(s->phase!=BT_LE_IDLE) return false;
    bool just_works=false;
    if(n==32 && le_equal((const uint8_t *)p,(const uint8_t *)"connect ",8)) p+=8;
    else if(n==42 && le_equal((const uint8_t *)p,(const uint8_t *)"connect-justworks ",18)) {
        p+=18; just_works=true;
    } else return false;
    if(p[6]!=' ') return false;
    uint8_t type,addr[6];
    if(le_equal((const uint8_t *)p,(const uint8_t *)"public",6)) type=0;
    else if(le_equal((const uint8_t *)p,(const uint8_t *)"random",6)) type=1;
    else return false;
    for(unsigned i=0;i<6;i++) {
        int hi=le_hex(p[7+3*i]),lo=le_hex(p[8+3*i]);
        if(hi<0 || lo<0 || (i<5 && p[9+3*i]!=':')) return false;
        addr[5-i]=(hi<<4)|lo;
    }
    *s=(bt_le_t){.phase=BT_LE_RESET,.now=now,.deadline=now+3000,
        .total_deadline=now+150000,.credits=1,.address_type=type,.just_works=just_works};
    le_copy(s->peer,addr,6);
    return true;
}

static bool le_queue(bt_le_t *s,uint16_t cid,const uint8_t *p,size_t n)
{
    if(n>23 || s->queue_count==BT_LE_QUEUE) { le_fail(s,"outgoing protocol queue full"); return false; }
    unsigned i=(s->queue_head+s->queue_count)%BT_LE_QUEUE;
    uint8_t *q=s->queue[i];
    le_put(q,s->handle); // PB=00: first, non-automatically-flushable ACL fragment.
    le_put(q+2,n+4); le_put(q+4,n); le_put(q+6,cid); le_copy(q+8,p,n);
    s->queue_bytes[i]=n+8; s->queue_count++;
    return true;
}
static void le_smp_send(bt_le_t *s,uint8_t op,const uint8_t data[16])
{ uint8_t p[17]={op}; le_copy(p+1,data,16); le_queue(s,6,p,17); }

// SMP c1 is e(TK, e(TK, random XOR p1) XOR p2). The controller's LE Encrypt
// command accepts the same least-significant-octet-first representation as SMP.
static void le_c1_first(bt_le_t *s,const uint8_t random[16],uint8_t p[32])
{
    le_copy(p,s->tk,16); p[16]=0; p[17]=s->address_type;
    le_copy(p+18,s->request,7); le_copy(p+25,s->response,7);
    for(unsigned i=0;i<16;i++) p[16+i]^=random[i];
}
static void le_c1_second(bt_le_t *s,uint8_t p[32])
{
    le_copy(p,s->tk,16); le_copy(p+16,s->crypto,16);
    for(unsigned i=0;i<6;i++) { p[16+i]^=s->peer[i]; p[22+i]^=s->local[i]; }
}

void bt_le_event(void *context,const uint8_t *e,size_t n)
{
    bt_le_t *s=context;
    if(s->phase==BT_LE_IDLE || s->phase==BT_LE_FAILED) return;
    if(n<2 || n!=2u+e[1]) { le_bad(s); return; }
    if(e[0]==0x0e || e[0]==0x0f) {
        bool cs=e[0]==0x0f;
        if(n<(cs?6u:5u) || (cs && n!=6)) { le_bad(s); return; }
        s->credits=e[cs?3:2];
        if(!s->pending || le_u16(e+(cs?4:3))!=s->opcode) return;
        if(s->command_done || n<6) { le_bad(s); return; }
        bool async=s->opcode==0x200d || s->opcode==0x2019;
        if(cs!=async) { le_bad(s); return; }
        s->status=e[cs?2:5]; s->command_done=true; s->reply_bytes=0;
        if(s->status) return;
        size_t wanted=0;
        switch(s->opcode) {
        case 0x1009: wanted=6; break;
        case 0x2002: wanted=3; break;
        case 0x1005: wanted=7; break;
        case 0x2018: wanted=8; break;
        case 0x2017: wanted=16; break;
        }
        if(n!=6+wanted) { le_bad(s); return; }
        le_copy(s->reply,e+6,wanted); s->reply_bytes=wanted;
    } else if(e[0]==0x3e && n>=3 && e[2]==1) {
        if(s->phase!=BT_LE_CREATE && s->phase!=BT_LE_CONNECTING) { le_bad(s); return; }
        if(n!=21) { le_bad(s); return; }
        s->status=e[3];
        if(s->status) { le_fail(s,"LE connection failed"); return; }
        if(e[6]!=0 || e[7]!=s->address_type || !le_equal(e+8,s->peer,6) || le_u16(e+4)>0x0eff) {
            le_fail(s,"unexpected LE connection identity/role"); return;
        }
        s->handle=le_u16(e+4); s->connected=true;
        s->interval=le_u16(e+14); s->latency=le_u16(e+16); s->supervision_timeout=le_u16(e+18);
    } else if(e[0]==0x13) {
        if(n<3 || n!=3u+4u*e[2]) { le_bad(s); return; }
        for(unsigned i=0;i<e[2];i++) {
            if(le_u16(e+3+4*i)!=s->handle || !s->connected) continue;
            unsigned count=le_u16(e+5+4*i);
            if(count>s->outstanding) { le_bad(s); return; }
            s->outstanding-=count;
        }
    } else if(e[0]==5) {
        if(n!=6) { le_bad(s); return; }
        if(s->connected && le_u16(e+3)==s->handle) {
            s->disconnect_reason=e[5]; s->disconnected_ms=s->now;
            s->status=e[5]; s->connected=false; s->encrypted=false; s->link_may_active=false;
            le_fail(s,"peer disconnected");
        }
    } else if(e[0]==8) {
        if(n!=6) { le_bad(s); return; }
        if(!s->connected || le_u16(e+3)!=s->handle) return;
        s->status=e[2];
        if(s->status || e[5]!=1 || (s->phase!=BT_LE_ENCRYPT && s->phase!=BT_LE_ENCRYPT_WAIT)) {
            s->encrypted=false; le_fail(s,"link encryption failed or changed unexpectedly"); return;
        }
        s->encrypted=true;
    } else if(e[0]==0x10) le_fail(s,"controller hardware error");
}

static void le_smp(bt_le_t *s,const uint8_t *p,size_t n)
{
    if(!n) { le_bad(s); return; }
    if(p[0]==0x0b && n==2) return; // Security Request: our explicit request owns pairing.
    if(p[0]==5 && n==2) { s->status=p[1]; le_fail(s,"peer rejected pairing"); return; }
    if(p[0]==2 && s->phase==BT_LE_PAIR_WAIT) {
        if(n!=7) { le_bad(s); return; }
        // Capability bytes are public negotiation data, not keys. Preserve
        // rejected responses too, so status identifies the incompatible field.
        le_copy(s->response,p,7);
        // The command selects the association policy. A passkey request does
        // not fall back to Just Works; explicit Just Works advertises no MITM
        // capability and refuses a peer requiring authenticated pairing.
        const char *unsupported=NULL;
        if(s->just_works && p[1]>4) unsupported="peer IO capability unsupported";
        else if(!s->just_works && p[1]!=2 && p[1]!=4) unsupported="peer cannot enter a displayed passkey";
        else if(s->just_works && (p[3]&4)) unsupported="peer requires authenticated pairing";
        else if(p[2]) unsupported="peer OOB pairing flag unsupported";
        else if(p[4]!=16) unsupported="peer encryption key size unsupported";
        else if(p[5] || p[6]) unsupported="peer requested unsupported key distribution";
        else if((p[3]&3)>1 || (p[3]&0xc0)) unsupported="peer authentication flags unsupported";
        if(unsupported) { le_fail(s,unsupported); return; }
        s->phase=BT_LE_CONFIRM_LOW;
        s->passkey_visible=!s->just_works; s->deadline=s->now+60000;
    } else if(p[0]==3 && s->phase==BT_LE_CONFIRM_WAIT) {
        if(n!=17) { le_bad(s); return; }
        le_copy(s->peer_confirm,p+1,16);
        le_smp_send(s,4,s->random);
        if(s->phase!=BT_LE_CLEANUP) { s->phase=BT_LE_RANDOM_WAIT; s->deadline=s->now+30000; }
    } else if(p[0]==4 && s->phase==BT_LE_RANDOM_WAIT) {
        if(n!=17) { le_bad(s); return; }
        le_copy(s->peer_random,p+1,16); s->phase=BT_LE_VERIFY_LOW;
        s->passkey_visible=false;
    } else le_fail(s,"unexpected SMP message");
}

static void le_att_send(bt_le_t *s,const uint8_t *p,size_t n)
{
    if(le_queue(s,4,p,n)) { s->att_pending=true; s->att_opcode=p[0]; s->deadline=s->now+5000; }
}
static void le_report_next(bt_le_t *s)
{
    while(s->report_index<s->report_count) {
        bt_le_report_char_t *r=&s->report_chars[s->report_index];
        if(r->value<r->end) {
            s->cursor=r->value+1; s->phase=BT_LE_REPORT_DESCRIPTORS; return;
        }
        s->report_index++;
    }
    le_fail(s,"no supported keyboard input Report Reference/map");
}
static void le_map_done(bt_le_t *s)
{
    if(!s->report_map_bytes) { le_fail(s,"empty HID Report Map"); return; }
    s->report_index=0; le_report_next(s);
}
static void le_report_descriptors_done(bt_le_t *s)
{
    bt_le_report_char_t *r=&s->report_chars[s->report_index];
    if(r->ccc && r->reference) s->phase=BT_LE_REPORT_REFERENCE;
    else { s->report_index++; le_report_next(s); }
}
static void le_chars_done(bt_le_t *s)
{
    if(s->boot_value && s->protocol && (s->boot_properties&0x10) && (s->protocol_properties&4)) {
        if(s->boot_value>=s->boot_end) { le_fail(s,"boot keyboard CCC descriptor missing"); return; }
        s->input_value=s->boot_value; s->cursor=s->boot_value+1; s->phase=BT_LE_DESCRIPTORS;
    } else if(s->report_map_handle && s->report_count) {
        if(s->protocol && !(s->protocol_properties&4)) {
            le_fail(s,"Protocol Mode is not writable without response"); return;
        }
        s->report_protocol=true; s->phase=BT_LE_REPORT_MAP;
    } else le_fail(s,"HID keyboard needs boot input or readable Report Map and notifying Report");
}
static void le_inspection_done(bt_le_t *s)
{
    s->inspection_ms=s->now; s->inspections++;
    if(!s->ready_ms) s->ready_ms=s->now;
    s->phase=BT_LE_READY;
}
static void le_att(bt_le_t *s,const uint8_t *p,size_t n,bt_le_report_t report,void *ctx)
{
    if(!n || n>23) { le_bad(s); return; }
    s->last_att_opcode=p[0];
    if(p[0]==2) {
        if(n!=3 || le_u16(p+1)<23) { le_bad(s); return; }
        uint8_t reply[]={3,23,0}; le_queue(s,4,reply,3); return;
    }
    if(p[0]==0x1b) {
        if(n<3) { le_bad(s); return; }
        s->rx_notifications++; s->last_notification_handle=le_u16(p+1);
        s->last_notification_bytes=n-3;
        if(bt_le_input_active(s) && s->last_notification_handle==s->input_value) {
            uint8_t keys[8];
            if(s->report_protocol) {
                if(!hid_keyboard_decode_map(&s->report_layout,p+3,n-3,keys)) {
                    le_fail(s,"keyboard report does not match Report Map"); return;
                }
            } else {
                if(n!=11) { le_bad(s); return; }
                le_copy(keys,p+3,8);
            }
            s->reports++; report(ctx,keys);
        } else s->rx_ignored_notifications++;
        return;
    }
    if(p[0]==0x1d) { // Acknowledge an indication even if its value is not used.
        if(n<3) { le_bad(s); return; }
        s->rx_indications++;
        uint8_t ack=0x1e; le_queue(s,4,&ack,1); return;
    }
    if(!s->att_pending) { le_fail(s,"unexpected ATT response"); return; }
    s->att_pending=false;
    if(p[0]==1) {
        if(n!=5 || p[1]!=s->att_opcode) { le_bad(s); return; }
        s->status=p[4];
        if(p[4]==0x0a && s->phase==BT_LE_CHARACTERISTICS) { le_chars_done(s); return; }
        if(p[4]==0x0a && s->phase==BT_LE_REPORT_DESCRIPTORS && le_u16(p+2)==s->cursor) {
            le_report_descriptors_done(s); return;
        }
        if(s->phase==BT_LE_REPORT_MAP && s->att_opcode==0x0c &&
           le_u16(p+2)==s->report_map_handle && s->report_map_bytes &&
           (p[4]==7 || (p[4]==0x0b && s->report_map_bytes==22))) { le_map_done(s); return; }
        le_fail(s,p[4]==0x0a ? "required HID service/descriptor not found" : "GATT request rejected"); return;
    }
    if(s->phase==BT_LE_SERVICES && p[0]==0x11) {
        if(n<2 || (p[1]!=6 && p[1]!=20) || n<2u+p[1] || (n-2)%p[1]) { le_bad(s); return; }
        uint16_t previous=s->cursor-1;
        for(size_t i=2;i<n;i+=p[1]) {
            uint16_t start=le_u16(p+i),end=le_u16(p+i+2);
            if(start<=previous || end<start) { le_bad(s); return; }
            previous=end;
            if(p[1]==6 && le_u16(p+i+4)==0x1812 && !s->service_start) {
                s->service_start=start; s->service_end=end;
            }
        }
        if(s->service_start) {
            s->phase=BT_LE_CHARACTERISTICS; s->cursor=s->service_start; s->boot_end=s->service_end;
        } else if(previous==0xffff) le_fail(s,"HID service not found");
        else s->cursor=previous+1;
    } else if(s->phase==BT_LE_CHARACTERISTICS && p[0]==9) {
        if(n<2 || (p[1]!=7 && p[1]!=21) || n<2u+p[1] || (n-2)%p[1]) { le_bad(s); return; }
        uint16_t previous=s->cursor-1;
        for(size_t i=2;i<n;i+=p[1]) {
            uint16_t decl=le_u16(p+i),value=le_u16(p+i+3);
            if(decl<=previous || value<=decl || value>s->service_end) { le_bad(s); return; }
            previous=value;
            if(s->boot_value && decl>s->boot_value && s->boot_end==s->service_end) s->boot_end=decl-1;
            for(unsigned j=0;j<s->report_count;j++) {
                bt_le_report_char_t *r=&s->report_chars[j];
                if(decl>r->value && r->end==s->service_end) r->end=decl-1;
            }
            if(p[1]==7 && le_u16(p+i+5)==0x2a4b && (p[i+2]&2)) {
                if(s->report_map_handle) { le_fail(s,"ambiguous HID Report Map"); return; }
                s->report_map_handle=value;
            }
            if(p[1]==7 && le_u16(p+i+5)==0x2a4d && (p[i+2]&0x10)) {
                if(s->report_count==BT_LE_REPORTS) { le_fail(s,"too many HID input reports"); return; }
                s->report_chars[s->report_count++]=(bt_le_report_char_t){.value=value,.end=s->service_end};
            }
            if(p[1]==7 && le_u16(p+i+5)==0x2a22) {
                if(s->boot_value) { le_fail(s,"ambiguous boot keyboard characteristic"); return; }
                s->boot_value=value; s->boot_properties=p[i+2];
            }
            if(p[1]==7 && le_u16(p+i+5)==0x2a4e) {
                s->protocol=value; s->protocol_properties=p[i+2];
            }
        }
        if(previous>=s->service_end) le_chars_done(s);
        else s->cursor=previous+1;
    } else if(s->phase==BT_LE_REPORT_MAP && p[0]==(s->att_opcode==0x0a?0x0b:0x0d)) {
        if(n-1>sizeof(s->report_map)-s->report_map_bytes) { le_fail(s,"HID Report Map exceeds limit"); return; }
        le_copy(s->report_map+s->report_map_bytes,p+1,n-1); s->report_map_bytes+=n-1;
        if(n<23) le_map_done(s);
    } else if(s->phase==BT_LE_REPORT_REFERENCE && p[0]==0x0b) {
        if(n!=3 || p[2]<1 || p[2]>3) { le_bad(s); return; }
        bt_le_report_char_t *r=&s->report_chars[s->report_index]; r->id=p[1]; r->type=p[2];
        if(r->type==1 && hid_keyboard_parse_map(s->report_map,s->report_map_bytes,r->id,&s->report_layout) &&
           s->report_layout.bytes<=20) {
            s->input_value=r->value; s->ccc=r->ccc;
            s->phase=s->protocol?BT_LE_PROTOCOL:BT_LE_SUBSCRIBE;
        } else { s->report_index++; le_report_next(s); }
    } else if((s->phase==BT_LE_DESCRIPTORS || s->phase==BT_LE_REPORT_DESCRIPTORS) && p[0]==5) {
        if(n<2 || (p[1]!=1 && p[1]!=2)) { le_bad(s); return; }
        size_t width=p[1]==1 ? 4:18;
        if(n<2+width || (n-2)%width) { le_bad(s); return; }
        bool reports=s->phase==BT_LE_REPORT_DESCRIPTORS;
        bt_le_report_char_t *r=reports?&s->report_chars[s->report_index]:NULL;
        uint16_t end=reports?r->end:s->boot_end;
        uint16_t previous=s->cursor-1;
        for(size_t i=2;i<n;i+=width) {
            uint16_t handle=le_u16(p+i);
            if(handle<=previous || handle>end) { le_bad(s); return; }
            previous=handle;
            if(width==4 && le_u16(p+i+2)==0x2902) {
                uint16_t *ccc=reports?&r->ccc:&s->ccc;
                if(*ccc) { le_bad(s); return; }
                *ccc=handle;
            }
            if(reports && width==4 && le_u16(p+i+2)==0x2908) {
                if(r->reference) { le_bad(s); return; }
                r->reference=handle;
            }
        }
        if(reports) {
            if(previous>=end) le_report_descriptors_done(s);
            else s->cursor=previous+1;
            return;
        }
        if(s->ccc) s->phase=BT_LE_PROTOCOL;
        else if(previous>=s->boot_end) le_fail(s,"boot keyboard CCC descriptor missing");
        else s->cursor=previous+1;
    } else if(s->phase==BT_LE_SUBSCRIBE && p[0]==0x13 && n==1) {
        s->phase=BT_LE_VERIFY_CCC;
    } else if(s->phase==BT_LE_VERIFY_CCC && p[0]==0x0b) {
        if(n!=3) { le_bad(s); return; }
        s->verified_ccc=le_u16(p+1); s->ccc_read=true;
        if(s->verified_ccc!=1) { le_fail(s,"keyboard notification setting readback mismatch"); return; }
        if(s->protocol && (s->protocol_properties&2)) s->phase=BT_LE_VERIFY_PROTOCOL;
        else le_inspection_done(s);
    } else if(s->phase==BT_LE_VERIFY_PROTOCOL && p[0]==0x0b) {
        if(n!=2) { le_bad(s); return; }
        s->verified_protocol=p[1]; s->protocol_read=true;
        if(s->verified_protocol!=(s->report_protocol?1:0)) {
            le_fail(s,"keyboard Protocol Mode readback mismatch"); return;
        }
        le_inspection_done(s);
    } else le_bad(s);
}

static void le_l2cap(bt_le_t *s,bt_le_report_t report,void *ctx)
{
    uint16_t cid=le_u16(s->l2cap+2); s->last_cid=cid;
    const uint8_t *p=s->l2cap+4; size_t n=s->l2cap_need-4;
    if(cid==6) le_smp(s,p,n);
    else if(cid==4) le_att(s,p,n,report,ctx);
    else if(cid==5) {
        size_t off=0;
        while(off<n) {
            if(n-off<4 || !p[off+1] || le_u16(p+off+2)>n-off-4) { le_bad(s); return; }
            uint16_t len=le_u16(p+off+2);
            s->last_signal_opcode=p[off];
            // Keep the centrally chosen connection parameters. Reject the
            // peripheral's optional update rather than promising an update.
            uint8_t reply[]={1,p[off+1],2,0,0,0};
            if(p[off]==0x12 && len==8) {
                s->parameter_requests++;
                for(unsigned i=0;i<4;i++) s->requested_parameters[i]=le_u16(p+off+4+2*i);
                reply[0]=0x13; reply[4]=1;
            }
            if(p[off]!=1) le_queue(s,5,reply,6);
            off+=4u+len;
        }
    }
}
static void le_acl_packet(bt_le_t *s,bt_le_report_t report,void *ctx)
{
    uint16_t flags=le_u16(s->wire),handle=flags&0xfff;
    size_t n=s->wire_need-4;
    s->rx_acl++;
    if(bt_le_input_active(s)) s->rx_after_ready++;
    if(!s->connected || handle!=s->handle) { s->rx_unmatched_acl++; return; }
    if(flags&0xc000) { le_bad(s); return; }
    unsigned pb=(flags>>12)&3;
    if(pb==0 || pb==2) {
        if(s->l2cap_used || n<4) { le_bad(s); return; }
        s->l2cap_need=4u+le_u16(s->wire+4);
        if(s->l2cap_need>sizeof(s->l2cap)) { le_bad(s); return; }
    } else if(pb!=1 || !s->l2cap_used) { le_bad(s); return; }
    if(n>s->l2cap_need-s->l2cap_used) { le_bad(s); return; }
    le_copy(s->l2cap+s->l2cap_used,s->wire+4,n); s->l2cap_used+=n;
    if(s->l2cap_used==s->l2cap_need) {
        le_l2cap(s,report,ctx); le_wipe(s->l2cap,sizeof(s->l2cap)); s->l2cap_used=0; s->l2cap_need=0;
    }
}
void bt_le_receive(bt_le_t *s,const uint8_t *p,size_t n,bt_le_report_t report,void *ctx)
{
    if(s->phase==BT_LE_IDLE || s->phase>=BT_LE_CLEANUP) return;
    s->rx_bytes+=n;
    while(n && s->phase<BT_LE_CLEANUP) {
        size_t target=s->wire_used<4 ? 4:s->wire_need;
        size_t take=target-s->wire_used; if(take>n) take=n;
        le_copy(s->wire+s->wire_used,p,take); s->wire_used+=take; p+=take; n-=take;
        if(s->wire_used==4 && target==4) {
            s->wire_need=4u+le_u16(s->wire+2);
            if(s->wire_need>sizeof(s->wire) || s->wire_need==4) { le_bad(s); return; }
        }
        if(s->wire_used==s->wire_need) {
            le_acl_packet(s,report,ctx); le_wipe(s->wire,sizeof(s->wire)); s->wire_used=0; s->wire_need=0;
        }
    }
}

static void le_command_finished(bt_le_t *s)
{
    switch(s->phase) {
    case BT_LE_RESET: s->phase=BT_LE_MASK; break;
    case BT_LE_MASK: s->phase=BT_LE_HOST; break;
    case BT_LE_HOST: s->phase=BT_LE_EVENTS; break;
    case BT_LE_EVENTS: s->phase=BT_LE_ADDRESS; break;
    case BT_LE_ADDRESS: le_copy(s->local,s->reply,6); s->phase=BT_LE_BUFFER; break;
    case BT_LE_BUFFER:
        s->acl_size=le_u16(s->reply);
        if(!s->reply[2]) s->phase=BT_LE_SHARED_BUFFER;
        else if(s->acl_size<27) le_fail(s,"controller ACL buffer too small");
        else s->phase=BT_LE_CREATE;
        break;
    case BT_LE_SHARED_BUFFER:
        s->acl_size=le_u16(s->reply);
        if(s->acl_size<27 || !le_u16(s->reply+3)) le_fail(s,"controller has no usable ACL buffers");
        else s->phase=BT_LE_CREATE;
        break;
    case BT_LE_CREATE: s->phase=BT_LE_CONNECTING; s->deadline=s->now+20000; break;
    case BT_LE_PASSKEY: {
        uint32_t r=le_u16(s->reply) | (uint32_t)le_u16(s->reply+2)<<16;
        if(r>=4294000000u) break; // Rejection sampling keeps six-digit codes uniform.
        s->passkey=r%1000000; le_put(s->tk,s->passkey); le_put(s->tk+2,s->passkey>>16);
        s->phase=BT_LE_RANDOM_LOW; break;
    }
    case BT_LE_RANDOM_LOW: le_copy(s->random,s->reply,8); s->phase=BT_LE_RANDOM_HIGH; break;
    case BT_LE_RANDOM_HIGH: le_copy(s->random+8,s->reply,8); s->phase=BT_LE_PAIR; break;
    case BT_LE_CONFIRM_LOW: le_copy(s->crypto,s->reply,16); s->phase=BT_LE_CONFIRM_HIGH; break;
    case BT_LE_CONFIRM_HIGH:
        le_smp_send(s,3,s->reply);
        if(s->phase!=BT_LE_CLEANUP) { s->phase=BT_LE_CONFIRM_WAIT; s->deadline=s->now+60000; }
        break;
    case BT_LE_VERIFY_LOW: le_copy(s->crypto,s->reply,16); s->phase=BT_LE_VERIFY_HIGH; break;
    case BT_LE_VERIFY_HIGH:
        if(!le_equal(s->reply,s->peer_confirm,16)) le_fail(s,"pairing confirm mismatch");
        else s->phase=BT_LE_STK;
        break;
    case BT_LE_STK: le_copy(s->crypto,s->reply,16); s->phase=BT_LE_ENCRYPT; break;
    case BT_LE_ENCRYPT: s->phase=BT_LE_ENCRYPT_WAIT; s->deadline=s->now+10000; break;
    default: le_bad(s); break;
    }
}
void bt_le_tick(bt_le_t *s,uint64_t now,bool control_done,bool bulk_done,bool usb_failed,
                bt_scan_send_t command,bt_le_acl_send_t acl,bt_le_report_t report,void *ctx)
{
    s->now=now;
    if(s->phase==BT_LE_IDLE || s->phase==BT_LE_FAILED) return;
    if(usb_failed) {
        le_fail(s,"USB transport failed; reboot required");
        s->phase=BT_LE_FAILED;
    }
    if(s->stop_requested && s->phase!=BT_LE_CLEANUP && s->phase!=BT_LE_FAILED)
        le_fail(s,"disconnected by request");
    if(s->release_pending) { uint8_t empty[8]={0}; report(ctx,empty); s->release_pending=false; }
    if(s->phase==BT_LE_FAILED) return;
    if(s->phase==BT_LE_CLEANUP) {
        // HCI Reset cancels initiation and disconnects an established link.
        // USB owns the buffers until both TDs finish, regardless of HCI state.
        if(now>=s->deadline) { s->phase=BT_LE_FAILED; return; }
        if(!control_done || !bulk_done) return;
        if(s->pending && s->opcode==0x0c03) {
            if(!s->command_done) return;
            s->pending=false;
            if(!s->status) {
                s->connected=false; s->encrypted=false; s->outstanding=0; s->link_may_active=false;
                if(s->stop_requested) { *s=(bt_le_t){0}; return; }
            }
            s->phase=BT_LE_FAILED; return;
        }
        s->pending=true; s->command_done=false; s->status=0; s->opcode=0x0c03;
        s->credits=0; command(ctx,0x0c03,NULL,0); return;
    }
    if(s->phase!=BT_LE_READY && now>=s->total_deadline) {
        le_fail(s,"connection/pairing deadline expired"); return;
    }
    if(s->outstanding && now>=s->acl_deadline) { le_fail(s,"ACL completion timed out"); return; }
    if(s->pending) {
        if(now>=s->command_deadline || (s->command_done && s->status)) {
            le_fail(s,now>=s->command_deadline ? "HCI command timed out":"HCI command rejected"); return;
        }
        if(!control_done || !s->command_done) return;
        s->pending=false; le_command_finished(s); le_wipe(s->reply,sizeof(s->reply));
        if(s->phase>=BT_LE_CLEANUP) return;
    }
    if(s->phase==BT_LE_CONNECTING && s->connected)
        // Just Works uses TK=0; the zeroed session keeps that value while both
        // modes still draw a fresh nonce and verify the peer's c1 confirmation.
        s->phase=s->just_works ? BT_LE_RANDOM_LOW : BT_LE_PASSKEY;
    if(s->phase==BT_LE_ENCRYPT_WAIT && s->encrypted) {
        le_secrets_clear(s); s->phase=BT_LE_SERVICES; s->cursor=1;
    }
    if(s->queue_count && bulk_done && !s->outstanding) {
        unsigned i=s->queue_head;
        s->outstanding=1; s->acl_deadline=now+5000;
        acl(ctx,s->queue[i],s->queue_bytes[i]);
        le_wipe(s->queue[i],sizeof(s->queue[i]));
        s->queue_head=(i+1)%BT_LE_QUEUE; s->queue_count--;
    }
    if(s->att_pending || s->phase==BT_LE_CONNECTING || s->phase==BT_LE_PAIR_WAIT ||
       s->phase==BT_LE_CONFIRM_WAIT || s->phase==BT_LE_RANDOM_WAIT || s->phase==BT_LE_ENCRYPT_WAIT) {
        if(now>=s->deadline) le_fail(s,"peer response timed out");
        return;
    }
    if(s->phase==BT_LE_PAIR) {
        // DisplayOnly/MITM for Passkey Entry; NoInputNoOutput/no MITM for
        // explicit Just Works. Both request a 16-byte session without bonding.
        const uint8_t req[]={1,s->just_works?3:0,0,s->just_works?0:4,16,0,0};
        le_copy(s->request,req,7);
        if(le_queue(s,6,req,7)) { s->phase=BT_LE_PAIR_WAIT; s->deadline=now+30000; }
        return;
    }
    if(s->phase>=BT_LE_SERVICES) {
        uint8_t p[7]={0};
        switch(s->phase) {
        case BT_LE_SERVICES:
            p[0]=0x10; le_put(p+1,s->cursor); le_put(p+3,0xffff); le_put(p+5,0x2800);
            le_att_send(s,p,7); break;
        case BT_LE_CHARACTERISTICS:
            p[0]=8; le_put(p+1,s->cursor); le_put(p+3,s->service_end); le_put(p+5,0x2803);
            le_att_send(s,p,7); break;
        case BT_LE_DESCRIPTORS:
            p[0]=4; le_put(p+1,s->cursor); le_put(p+3,s->boot_end); le_att_send(s,p,5); break;
        case BT_LE_REPORT_MAP:
            p[0]=s->report_map_bytes?0x0c:0x0a; le_put(p+1,s->report_map_handle);
            le_put(p+3,s->report_map_bytes); le_att_send(s,p,s->report_map_bytes?5:3); break;
        case BT_LE_REPORT_DESCRIPTORS:
            p[0]=4; le_put(p+1,s->cursor); le_put(p+3,s->report_chars[s->report_index].end);
            le_att_send(s,p,5); break;
        case BT_LE_REPORT_REFERENCE:
            p[0]=0x0a; le_put(p+1,s->report_chars[s->report_index].reference); le_att_send(s,p,3); break;
        case BT_LE_PROTOCOL:
            p[0]=0x52; le_put(p+1,s->protocol); p[3]=s->report_protocol?1:0;
            if(le_queue(s,4,p,4)) s->phase=BT_LE_SUBSCRIBE;
            break;
        case BT_LE_VERIFY_CCC: case BT_LE_VERIFY_PROTOCOL:
            p[0]=0x0a; le_put(p+1,s->phase==BT_LE_VERIFY_CCC?s->ccc:s->protocol);
            le_att_send(s,p,3); break;
        case BT_LE_SUBSCRIBE:
            p[0]=0x12; le_put(p+1,s->ccc); p[3]=1; le_att_send(s,p,5); break;
        default: break;
        }
        return;
    }
    if(!control_done || !s->credits) return;
    uint8_t p[32]={0},len=0; uint16_t op=0;
    switch(s->phase) {
    case BT_LE_RESET: op=0x0c03; break;
    case BT_LE_MASK:
        op=0x0c01; len=8; p[0]=0x90; p[1]=0xe0; p[2]=4; p[7]=0x20;
        break; // Disconnect, Encryption Change, command replies, Hardware Error, completed ACL, LE.
    case BT_LE_HOST: op=0x0c6d; len=2; p[0]=1; break;
    case BT_LE_EVENTS: op=0x2001; len=8; p[0]=1; break; // Legacy Connection Complete.
    case BT_LE_ADDRESS: op=0x1009; break;
    case BT_LE_BUFFER: op=0x2002; break;
    case BT_LE_SHARED_BUFFER: op=0x1005; break;
    case BT_LE_CREATE:
        s->link_may_active=true; op=0x200d; len=25; le_put(p,0x60); le_put(p+2,0x30);
        p[5]=s->address_type; le_copy(p+6,s->peer,6);
        le_put(p+13,24); le_put(p+15,40); le_put(p+19,400);
        break; // 30–50ms connection interval, zero latency, 4s supervision timeout.
    case BT_LE_PASSKEY: case BT_LE_RANDOM_LOW: case BT_LE_RANDOM_HIGH: op=0x2018; break;
    case BT_LE_CONFIRM_LOW: case BT_LE_VERIFY_LOW:
        op=0x2017; len=32; le_c1_first(s,s->phase==BT_LE_CONFIRM_LOW?s->random:s->peer_random,p); break;
    case BT_LE_CONFIRM_HIGH: case BT_LE_VERIFY_HIGH:
        op=0x2017; len=32; le_c1_second(s,p); break;
    case BT_LE_STK:
        op=0x2017; len=32; le_copy(p,s->tk,16);
        le_copy(p+16,s->random,8); le_copy(p+24,s->peer_random,8); break;
    case BT_LE_ENCRYPT:
        op=0x2019; len=28; le_put(p,s->handle); le_copy(p+12,s->crypto,16); break;
    default: return;
    }
    s->opcode=op; s->status=0; s->pending=true; s->command_done=false;
    s->credits=0; s->command_deadline=now+2000;
    command(ctx,op,p,len); le_wipe(p,sizeof(p));
}
size_t bt_le_status(const bt_le_t *s,char *out,size_t cap)
{
    const char *state=s->phase==BT_LE_IDLE?"idle":s->phase==BT_LE_READY?
        (s->reports?"keyboard ready":"HID configured; awaiting input"):
        s->ready_ms && (s->phase==BT_LE_VERIFY_CCC || s->phase==BT_LE_VERIFY_PROTOCOL)?"inspecting HID settings":
        s->phase==BT_LE_FAILED?"failed (reboot required)":s->phase==BT_LE_CLEANUP?"stopping":
        s->phase<=BT_LE_CREATE?"initializing":s->phase==BT_LE_CONNECTING?"connecting":
        s->phase<=BT_LE_ENCRYPT_WAIT?"pairing": "discovering HID service";
    char code[80]={0},capabilities[224]={0};
    if(s->response[0]==2) {
        const uint8_t *r=s->response;
        snprintf(capabilities,sizeof(capabilities),
            "pairing response: %02x %02x %02x %02x %02x %02x %02x\n"
            "peer capabilities: IO=%u OOB=%u auth=%02x key_bytes=%u initiator_keys=%02x responder_keys=%02x\n",
            r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[1],r[2],r[3],r[4],r[5],r[6]);
    }
    if(s->passkey_visible) snprintf(code,sizeof(code),"type on the Bluetooth keyboard, then Enter: %06u\n",s->passkey);
    int n=snprintf(out,cap,"state: %s\npeer: %02x:%02x:%02x:%02x:%02x:%02x %s\n"
        "link may be active: %s\nencrypted: %s\nmode: %s; session only (no saved bond)\n%s"
        "key reports: %u\nmalformed packets: %u\nHID service: %04x-%04x input: %04x CCC: %04x\n"
        "HID mode: %s map handle: %04x bytes: %u input candidates: %u report ID: %u\n"
        "%serror: %s (last command %04x status/reason %02x)\n"
        "write connect|connect-justworks public|random XX:XX:XX:XX:XX:XX, or inspect|disconnect\n",
        state,s->peer[5],s->peer[4],s->peer[3],s->peer[2],s->peer[1],s->peer[0],s->address_type?"random":"public",
        s->link_may_active?"yes":"no",
        s->encrypted?"yes":"no",s->just_works?"legacy Just Works (unauthenticated)":"legacy passkey",code,s->reports,s->malformed,s->service_start,s->service_end,s->input_value,s->ccc,
        s->report_protocol?"report":"boot",s->report_map_handle,s->report_map_bytes,s->report_count,s->report_layout.report_id,
        capabilities,s->error?s->error:"none",s->error_opcode,s->error_status);
    if(n<0 || !cap) return 0;
    size_t used=(size_t)n<cap?(size_t)n:cap-1;
    if(used<cap-1) {
        n=snprintf(out+used,cap-used,
            "receive: bytes=%u ACL=%u unmatched=%u after_ready=%u last_CID=%04x last_ATT=%02x\n"
            "notifications: total=%u ignored=%u last_handle=%04x last_bytes=%u indications=%u\n"
            "initial connection: interval=%u latency=%u timeout=%u (1.25ms/events/10ms)\n"
            "parameter requests: %u last_signal=%02x min=%u max=%u latency=%u timeout=%u\n"
            "ready at ms: %lu disconnected at ms: %lu disconnect reason: %02x\n"
            "HID Protocol Mode handle: %04x\n"
            "readback: CCC=%04x read=%s Protocol_Mode=%u read=%s completed=%u last_ms=%lu\n",
            s->rx_bytes,s->rx_acl,s->rx_unmatched_acl,s->rx_after_ready,s->last_cid,s->last_att_opcode,
            s->rx_notifications,s->rx_ignored_notifications,s->last_notification_handle,s->last_notification_bytes,s->rx_indications,
            s->interval,s->latency,s->supervision_timeout,s->parameter_requests,s->last_signal_opcode,
            s->requested_parameters[0],s->requested_parameters[1],s->requested_parameters[2],s->requested_parameters[3],
            s->ready_ms,s->disconnected_ms,s->disconnect_reason,s->protocol,
            s->verified_ccc,s->ccc_read?"yes":"no",s->verified_protocol,s->protocol_read?"yes":"no",
            s->inspections,s->inspection_ms);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    for(unsigned i=0;i<s->report_count && used<cap-1;i++) {
        const bt_le_report_char_t *r=&s->report_chars[i];
        n=snprintf(out+used,cap-used,"report candidate: value=%04x CCC=%04x reference=%04x ID=%u type=%u\n",
                   r->value,r->ccc,r->reference,r->id,r->type);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    // Keep the public descriptor after cleanup so unsupported hardware can be
    // diagnosed without another kernel build. Pairing secrets are not printed.
    if(s->phase==BT_LE_FAILED && s->report_map_bytes) {
        for(unsigned i=0;i<s->report_map_bytes && used<cap-1;i++) {
            if(!(i%16)) {
                n=snprintf(out+used,cap-used,"report map %04x:",i);
                if(n<0) return used;
                used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
            }
            n=snprintf(out+used,cap-used," %02x%s",s->report_map[i],
                       i%16==15 || i+1==s->report_map_bytes?"\n":"");
            if(n<0) return used;
            used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
        }
    }
    return used;
}
