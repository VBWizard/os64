// HCI discovery wire formats follow Bluetooth Core, Vol 4 Part E, and
// https://github.com/torvalds/linux/blob/v6.12/include/net/bluetooth/hci.h .
#include "driver/system/usb/bt_scan.h"
#include "strings/sprintf.h"

static uint16_t scan_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void scan_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d=dst; const uint8_t *s=src;
    while (n--) *d++=*s++;
}
static bool scan_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    while (n--) if (*a++!=*b++) return false;
    return true;
}

static bool scan_space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
bool bt_scan_request_valid(const char *data, size_t bytes)
{
    while (bytes && scan_space(*data)) { data++; bytes--; }
    while (bytes && scan_space(data[bytes-1])) bytes--;
    return bytes==4 && data[0]=='s' && data[1]=='c' && data[2]=='a' && data[3]=='n';
}

bool bt_scan_start(bt_scan_t *s, uint64_t now)
{
    if (s->phase!=BT_SCAN_IDLE && s->phase!=BT_SCAN_DONE) return false;
    *s=(bt_scan_t){.phase=BT_SCAN_RESET,.credits=1,.total_deadline=now+40000};
    return true;
}

// Validate the complete AD/EIR chain before changing a cached name. A zero
// length ends padded EIR data; names are printable ASCII in this text view.
static bool scan_name(bt_scan_device_t *d, const uint8_t *p, size_t n)
{
    for (size_t off=0; off<n && p[off]; off+=1u+p[off])
        if (p[off]>n-off-1) return false;
    for (size_t off=0; off<n && p[off]; off+=1u+p[off]) {
        uint8_t kind=p[off+1];
        if ((kind!=8 && kind!=9) || p[off]<2 || (d->complete_name && kind==8)) continue;
        size_t len=p[off]-1;
        if (len>=sizeof(d->name)) len=sizeof(d->name)-1;
        for (size_t i=0;i<len;i++) {
            uint8_t c=p[off+2+i];
            d->name[i]=c>=32 && c<127 ? (char)c : '?';
        }
        d->name[len]=0;
        d->complete_name=kind==9;
    }
    return true;
}

static void scan_record(bt_scan_t *s, const bt_scan_device_t *d)
{
    for (unsigned i=0;i<s->count;i++) {
        bt_scan_device_t *old=&s->devices[i];
        if (old->le!=d->le || old->address_type!=d->address_type ||
            !scan_equal(old->address,d->address,6)) continue;
        old->rssi=d->rssi;
        if (d->name[0] && (!old->complete_name || d->complete_name)) {
            scan_copy(old->name,d->name,sizeof(old->name));
            old->complete_name=d->complete_name;
        }
        return;
    }
    if (s->count==BT_SCAN_DEVICES) { s->dropped++; return; }
    s->devices[s->count++]=*d;
}

static bool scan_reports(bt_scan_t *s, const uint8_t *e, size_t n)
{
    if (e[0]==0x3e) {
        if (n<3) return false;
        if (e[2]!=2) return true; // Legacy LE advertising report subevent.
        if (s->phase!=BT_SCAN_LE_ENABLE && s->phase!=BT_SCAN_LE_WAIT &&
            s->phase!=BT_SCAN_LE_DISABLE) return true;
        if (n<4 || !e[3]) return false;
        size_t off=4;
        // A multi-report event is accepted atomically after checking its layout.
        for (unsigned i=0;i<e[3];i++) {
            if (n-off<10 || e[off]>4 || e[off+1]>1 || e[off+8]>31 ||
                (size_t)e[off+8]+10>n-off) return false;
            bt_scan_device_t scratch={0};
            if (!scan_name(&scratch,e+off+9,e[off+8])) return false;
            off+=10u+e[off+8];
        }
        if (off!=n) return false;
        off=4;
        for (unsigned i=0;i<e[3];i++) {
            bt_scan_device_t d={.le=true,.address_type=e[off+1],
                .rssi=(int8_t)e[off+9+e[off+8]]};
            scan_copy(d.address,e+off+2,6);
            scan_name(&d,e+off+9,e[off+8]);
            scan_record(s,&d);
            off+=10u+e[off+8];
        }
        return true;
    }
    if (e[0]!=2 && e[0]!=0x22 && e[0]!=0x2f) return true;
    if (s->phase!=BT_SCAN_INQUIRY && s->phase!=BT_SCAN_CLASSIC_WAIT) return true;
    if (n<3 || !e[2]) return false;
    size_t width=e[0]==0x2f ? 254 : 14;
    if (e[0]==0x22 && n==3u+15u*e[2]) width=15;
    if (n!=3+width*e[2] || (e[0]==0x2f && e[2]!=1)) return false;
    for (unsigned i=0;i<e[2];i++) {
        const uint8_t *p=e+3+i*width;
        bt_scan_device_t d={.rssi=127};
        scan_copy(d.address,p,6);
        scan_copy(d.device_class,p+((e[0]==2 || width==15) ? 9 : 8),3);
        if (e[0]!=2) d.rssi=(int8_t)p[width==15 ? 14 : 13];
        if (e[0]==0x2f && !scan_name(&d,p+14,240)) return false;
        scan_record(s,&d);
    }
    return true;
}

void bt_scan_event(void *context, const uint8_t *e, size_t n)
{
    bt_scan_t *s=context;
    if (n<2 || n!=2u+e[1]) { s->bad_reply=true; return; }
    if (e[0]==0x0e || e[0]==0x0f) {
        bool status=e[0]==0x0f;
        if (n<(status ? 6u : 5u)) { s->bad_reply=true; return; }
        s->credits=e[status ? 3 : 2];
        if (!s->pending || scan_u16(e+(status ? 4 : 3))!=s->opcode) return;
        if (s->command_done || n<6 || (status && n!=6)) { s->bad_reply=true; return; }
        // Inquiry is asynchronous: Command Status accepts it; Inquiry Complete
        // ends the radio procedure. Other commands here use Command Complete.
        if (status && !e[2] && s->opcode!=0x0401) return;
        s->command_done=true;
        s->status=e[status ? 2 : 5];
        if (s->status) return;
        if (s->opcode==0x0401 && !status) { s->bad_reply=true; return; }
        if (s->opcode==0x1003) {
            if (n!=14) { s->bad_reply=true; return; }
            scan_copy(s->features,e+6,8);
        } else if (s->opcode==0x1009) {
            if (n!=12) { s->bad_reply=true; return; }
            scan_copy(s->address,e+6,6);
        } else if (n!=6) s->bad_reply=true;
    } else if (e[0]==1 && (s->phase==BT_SCAN_INQUIRY || s->phase==BT_SCAN_CLASSIC_WAIT)) {
        if (n!=3) { s->bad_reply=true; return; }
        s->inquiry_done=true; s->inquiry_status=e[2];
    } else if (e[0]==0x10) {
        if (n!=3) s->bad_reply=true;
        else { s->hardware_error=true; s->status=e[2]; }
    } else if (!scan_reports(s,e,n)) s->malformed_reports++;
}

static void scan_fail(bt_scan_t *s, const char *reason, uint64_t now)
{
    if (!s->error) {
        s->error=reason; s->error_opcode=s->opcode; s->error_status=s->status;
    }
    s->phase=BT_SCAN_CLEANUP;
    s->pending=false; s->bad_reply=false; s->hardware_error=false;
    s->deadline=now+2000;
}

void bt_scan_tick(bt_scan_t *s, uint64_t now, bool usb_done, bool usb_failed,
                  bt_scan_send_t send, void *context)
{
    if (usb_failed) {
        if (!s->error) { s->error="USB transport failed; reboot required"; s->error_opcode=s->opcode; }
        s->phase=BT_SCAN_FAILED;
        return;
    }
    if (s->phase==BT_SCAN_FAILED) return;
    if (s->hardware_error && s->phase!=BT_SCAN_CLEANUP) {
        scan_fail(s,"controller hardware error",now);
        return;
    }
    if (s->phase==BT_SCAN_IDLE || s->phase==BT_SCAN_DONE) return;
    if (s->phase!=BT_SCAN_CLEANUP && (s->bad_reply || now>=s->total_deadline)) {
        scan_fail(s,s->bad_reply ? "malformed HCI reply" : "scan deadline expired",now);
        return;
    }
    if (s->pending) {
        if (now>=s->deadline || (s->command_done && s->status) || s->bad_reply || s->hardware_error) {
            if (s->phase==BT_SCAN_CLEANUP) { s->phase=BT_SCAN_FAILED; return; }
            scan_fail(s,now>=s->deadline ? "HCI command timed out" : "HCI command rejected",now);
            return;
        }
        if (!usb_done || !s->command_done) return;
        s->pending=false;
        if (s->phase==BT_SCAN_CLEANUP) {
            s->radio_active=false; s->phase=BT_SCAN_FAILED; return;
        }
        if (s->phase==BT_SCAN_INQUIRY) s->deadline=now+12000;
        if (s->phase==BT_SCAN_LE_ENABLE) s->deadline=now+10000;
        if (s->phase==BT_SCAN_RESET || s->phase==BT_SCAN_LE_DISABLE) s->radio_active=false;
        s->phase++;
    }
    if (s->phase==BT_SCAN_CLASSIC_WAIT) {
        if (!s->inquiry_done) {
            if (now>=s->deadline) scan_fail(s,"Classic inquiry timed out",now);
            return;
        }
        if (s->inquiry_status) { s->status=s->inquiry_status; scan_fail(s,"Classic inquiry failed",now); return; }
        s->radio_active=false; s->phase=BT_SCAN_LE_HOST;
    }
    if (s->phase==BT_SCAN_LE_WAIT) {
        if (now<s->deadline) return;
        s->phase=BT_SCAN_LE_DISABLE;
    }
    if (s->phase==BT_SCAN_INQUIRY_MODE && (s->features[4]&0x20)) s->phase=BT_SCAN_LE_HOST;
    if (s->phase==BT_SCAN_LE_HOST && !(s->features[4]&0x40)) s->phase=BT_SCAN_DONE;
    if (s->phase==BT_SCAN_DONE) return;
    // A failed command may still own the USB DMA buffer. Wait for its status
    // stage before a best-effort HCI Reset; never overwrite an in-flight TD.
    if (!usb_done || (!s->credits && s->phase!=BT_SCAN_CLEANUP)) {
        if (s->phase==BT_SCAN_CLEANUP && now>=s->deadline) s->phase=BT_SCAN_FAILED;
        return;
    }
    uint8_t p[8]={0}, length=0;
    uint16_t op=0;
    switch (s->phase) {
    case BT_SCAN_RESET: case BT_SCAN_CLEANUP: op=0x0c03; break;
    case BT_SCAN_FEATURES: op=0x1003; break;
    case BT_SCAN_ADDRESS: op=0x1009; break;
    case BT_SCAN_MASK:
        op=0x0c01; length=8;
        // Inquiry, command replies, Hardware Error, RSSI/EIR and LE Meta.
        p[0]=3; p[1]=0xe0; p[4]=2; p[5]=0x40; p[7]=0x20;
        break;
    case BT_SCAN_INQUIRY_MODE:
        op=0x0c45; length=1;
        p[0]=(s->features[6]&1) ? 2 : (s->features[3]&0x40) ? 1 : 0;
        break;
    case BT_SCAN_INQUIRY:
        op=0x0401; length=5; p[0]=0x33; p[1]=0x8b; p[2]=0x9e; p[3]=8;
        s->radio_active=true; break; // GIAC, 8 * 1.28 seconds, unlimited responses.
    case BT_SCAN_LE_HOST: op=0x0c6d; length=2; p[0]=1; break;
    case BT_SCAN_LE_MASK: op=0x2001; length=8; p[0]=2; break;
    case BT_SCAN_LE_PARAMS:
        op=0x200b; length=7; p[0]=1; p[1]=0x60; p[3]=0x30;
        break; // Active scan, 60ms interval / 30ms window, public own address, all advertisers.
    case BT_SCAN_LE_ENABLE: op=0x200c; length=2; p[0]=1; p[1]=1; s->radio_active=true; break;
    case BT_SCAN_LE_DISABLE: op=0x200c; length=2; break;
    default: return;
    }
    s->opcode=op; s->status=0; s->command_done=false; s->bad_reply=false;
    s->pending=true; s->credits=0; s->deadline=now+2000;
    send(context,op,p,length);
}

size_t bt_scan_status(const bt_scan_t *s, char *out, size_t cap)
{
    const char *state=s->phase==BT_SCAN_IDLE ? "idle" : s->phase==BT_SCAN_DONE ? "complete" :
        s->phase==BT_SCAN_FAILED ? "failed" : s->phase==BT_SCAN_CLEANUP ? "stopping after error" :
        s->phase<=BT_SCAN_INQUIRY ? "initializing" : s->phase==BT_SCAN_CLASSIC_WAIT ? "Classic inquiry" : "LE scan";
    int n=snprintf(out,cap,"state: %s\ncontroller: %02x:%02x:%02x:%02x:%02x:%02x\n"
        "devices: %u\nreports beyond capacity: %u\nmalformed reports: %u\n"
        "error: %s (opcode %04x status %02x)\n"
        "discovery may be active: %s\nwrite scan to start Classic then LE discovery (about 21 seconds)\n",
        state,s->address[5],s->address[4],s->address[3],s->address[2],s->address[1],s->address[0],
        s->count,s->dropped,s->malformed_reports,s->error ? s->error : "none",
        s->error_opcode,s->error_status,s->radio_active ? "yes" : "no");
    return n<0 || !cap ? 0 : (size_t)n<cap ? (size_t)n : cap-1;
}

size_t bt_scan_devices(const bt_scan_t *s, char *out, size_t cap)
{
    size_t used=0;
    for (unsigned i=0;i<s->count && used<cap;i++) {
        const bt_scan_device_t *d=&s->devices[i];
        char rssi[16];
        if (d->rssi==127) snprintf(rssi,sizeof(rssi),"unknown");
        else snprintf(rssi,sizeof(rssi),"%d dBm",(int)d->rssi);
        int n=snprintf(out+used,cap-used,"%s %02x:%02x:%02x:%02x:%02x:%02x %s RSSI %s class %02x%02x%02x name %s\n",
            d->le ? "LE" : "Classic",d->address[5],d->address[4],d->address[3],d->address[2],d->address[1],d->address[0],
            d->le && d->address_type ? "random" : "public",rssi,
            d->device_class[2],d->device_class[1],d->device_class[0],d->name[0] ? d->name : "(not advertised)");
        if (n<0) break;
        if ((size_t)n>=cap-used) { used=cap-1; break; }
        used+=(size_t)n;
    }
    if (!used && cap) out[0]=0;
    return used;
}
