// bt_classic.c — one Classic Bluetooth link. bt_classic.h is the contract;
// docs/design/pending/AUDIO_CLASSIC.md § L3 is the state table. HCI wire
// formats follow Bluetooth Core Vol 4 Part E §§ 7.1, 7.3.18, 7.5.7 and 7.7,
// and https://github.com/torvalds/linux/blob/v6.12/include/net/bluetooth/hci.h .
#include "driver/system/usb/bt_classic.h"
#include "strings/sprintf.h"

#define CL_COMMAND_MS 2000
#define CL_PAGE_MS 6000          // the controller's page timeout is 5.12 s
#define CL_ACCEPT_MS 6000
#define CL_INBOUND_MS 2000       // a peer that called us has this long to start securing the link
// The controller bounds each pairing step with its own 30 s LMP response
// timeout and reports it in Authentication Complete; this bound is past it.
#define CL_AUTH_MS 40000
#define CL_ENCRYPT_MS 10000
#define CL_CLOSE_MS 2000
#define CL_ANSWER_MS 25000

static const char cl_stale[]="the headphones no longer hold this bond; forget and bond again";

static uint16_t cl_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static void cl_put(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void cl_copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; }
static bool cl_equal(const uint8_t *a, const uint8_t *b, size_t n)
{ while(n--) if(*a++!=*b++) return false; return true; }
static void cl_wipe(void *p, size_t n)
{ volatile uint8_t *v=p; while(n--) *v++=0; }

// The phases in which the link has a peer it is paging, accepting or
// linked to. CLOSING is not one: what the peer asks while the link is
// being taken down is refused.
static bool cl_active(const bt_classic_t *c)
{ return c->phase>=BT_CLASSIC_PAGING && c->phase<=BT_CLASSIC_READY; }
static bool cl_ours(const bt_classic_t *c, const uint8_t *address)
{ return cl_active(c) && cl_equal(address,c->peer,6); }
static bool cl_our_handle(const bt_classic_t *c, const uint8_t *p)
{ return c->connected && cl_u16(p)==c->handle; }
// The far end went away and may come back: connection timeout, remote
// user ended it, remote low resources, remote power off, LMP response
// timeout. Authentication and key trouble and an end we asked for are not.
static bool cl_far_end_gone(uint8_t reason)
{ return reason==0x08 || reason==0x13 || reason==0x14 || reason==0x15 || reason==0x22; }

static void cl_key_wipe(bt_classic_t *c)
{ cl_wipe(c->new_key,sizeof(c->new_key)); c->new_key_type=0; c->have_key=false; }
static void cl_answers_clear(bt_classic_t *c)
{ for(unsigned i=0;i<BT_CLASSIC_ANSWERS;i++) c->answer[i].set=false; }
static void cl_name(char out[32], const char *name)
{
    unsigned i=0;
    for(;name && i<31 && name[i];i++) out[i]=name[i]>=32 && name[i]<127 ? name[i] : '?';
    for(;i<32;i++) out[i]=0;
}
static void cl_want_scan(bt_classic_t *c)
{ c->scan_enable_wanted=c->bond.valid ? 0x02 : 0x00; }
// Write Scan Enable is sent when what is wanted is not what was last sent:
// a write the controller refused, or never answered, is not repeated until
// there is something else to write (or a load makes it unknown again).
static bool cl_scan_owed(const bt_classic_t *c)
{ return c->bond_loaded && c->scan_enable_wanted!=c->scan_enable_tried; }

static void cl_error(bt_classic_t *c, const char *why, uint16_t op, uint8_t status, bool retry)
{
    if(c->error) return;
    c->error=why; c->error_opcode=op; c->error_status=status; c->retryable=retry;
}
static void cl_latch(bt_classic_t *c, unsigned kind)
{
    // The controller's patience runs from its first ask.
    if(!c->answer[kind].set) { c->answer[kind].set=true; c->answer[kind].since=c->now; }
}
static void cl_refuse(bt_classic_t *c, uint16_t op, const uint8_t *address, uint8_t reason)
{
    if(c->refusal_count==BT_CLASSIC_REFUSALS) { c->refusals_dropped++; return; }
    bt_classic_refusal_t *r=&c->refusal[c->refusal_count++];
    r->opcode=op; cl_copy(r->address,address,6); r->reason=reason; r->since=c->now;
}

// End the link. A live handle is disconnected; a page still owed its
// Connection Complete is cancelled (a crossing call accepted beside it
// still owes one, which CLOSING takes as it comes); an accepted call cannot
// be cancelled, so its Connection Complete is waited for; an accept not yet
// sent becomes a reject. With none of these it is over at once.
static void cl_close(bt_classic_t *c, uint8_t reason)
{
    if(c->answer[BT_CLASSIC_ANSWER_ACCEPT].set) cl_refuse(c,0x040a,c->peer,0x0f);
    c->crossing=c->crossing && !c->answer[BT_CLASSIC_ANSWER_ACCEPT].set;   // a crossing call refused is owed nothing
    cl_answers_clear(c);
    cl_key_wipe(c);
    c->next_op=0; c->waiting=false; c->close_reason=reason;
    if(c->connected) { c->phase=BT_CLASSIC_CLOSING; c->next_op=0x0406; }
    else if(c->link_may_active) {
        c->phase=BT_CLASSIC_CLOSING;
        if(c->session!=BT_CLASSIC_SESSION_INBOUND) c->next_op=0x0408;
        else { c->waiting=true; c->deadline=c->now+CL_CLOSE_MS; }
    } else c->phase=BT_CLASSIC_STOPPED;
}
static void cl_fail(bt_classic_t *c, const char *why, uint16_t op, uint8_t status, bool retry, uint8_t reason)
{
    if(!cl_active(c)) return;
    cl_error(c,why,op,status,retry);
    cl_close(c,reason);
}
// The controller would not, or did not, confirm the end of the link:
// STOPPED regardless, the handle forgotten.
static void cl_give_up(bt_classic_t *c, const char *why, uint16_t op, uint8_t status)
{
    cl_error(c,why,op,status,false);
    c->connected=false; c->encrypted=false; c->link_may_active=false; c->crossing=false;
    c->next_op=0; c->waiting=false; c->phase=BT_CLASSIC_STOPPED;
}

// A new attempt: everything the last one learned is cleared, except the
// bond, page scan, the refusals still owed, any command in flight and the
// malformed count.
static void cl_session(bt_classic_t *c, bt_classic_session_t kind, const uint8_t peer[6], const char *name)
{
    c->attempt++;
    c->session=kind; cl_copy(c->peer,peer,6); cl_name(c->peer_name,name);
    c->error=NULL; c->error_opcode=0; c->error_status=0; c->retryable=false;
    c->timed_out=c->timeout_usb_done=c->timeout_reply=false;
    c->disconnect_reason=0; c->connected=false; c->encrypted=false; c->handle=0;
    c->link_may_active=false; c->crossing=false; c->disconnect_sent=false; c->key_given=false;
    c->peer_authenticating=false; c->close_reason=0; c->debug_key=false;
    c->role_known=c->mode_known=c->peer_io_known=c->ssp_done=false;
    c->role=c->mode=c->key_size=c->refresh_status=0; c->mode_interval=0;
    c->peer_io[0]=c->peer_io[1]=c->peer_io[2]=0;
    cl_answers_clear(c); cl_key_wipe(c);
    c->waiting=false; c->next_op=0;
}

void bt_classic_init(bt_classic_t *c, const uint8_t local[6])
{
    cl_wipe(c,sizeof(*c));
    cl_copy(c->local,local,6);
    c->scan_enable_set=c->scan_enable_tried=0xff;
}

void bt_classic_set_bond(bt_classic_t *c, const bt_classic_bond_t *bond)
{
    cl_wipe(&c->bond,sizeof(c->bond));
    if(bond && bond->valid) {
        cl_copy(&c->bond,bond,sizeof(c->bond));
        c->bond.name[31]=0;
    }
    c->bond_dirty=false; c->bond_loaded=true;
    cl_want_scan(c);
    // What the controller holds is not known (a reset clears it), so the
    // first turn after a load writes it.
    c->scan_enable_set=c->scan_enable_tried=0xff;
}

static bool cl_space(char ch) { return ch==' ' || ch=='\t' || ch=='\r' || ch=='\n'; }
static int cl_hex(char ch)
{
    if(ch>='0' && ch<='9') return ch-'0';
    if(ch>='a' && ch<='f') return ch-'a'+10;
    if(ch>='A' && ch<='F') return ch-'A'+10;
    return -1;
}
// "aa:bb:cc:dd:ee:ff", most significant byte first, as people write it.
static bool cl_address(const char *p, uint8_t out[6])
{
    uint8_t any=0;
    for(unsigned i=0;i<6;i++) {
        int hi=cl_hex(p[3*i]),lo=cl_hex(p[3*i+1]);
        if(hi<0 || lo<0 || (i<5 && p[3*i+2]!=':')) return false;
        out[5-i]=(uint8_t)(hi<<4 | lo); any|=out[5-i];
    }
    return any!=0;
}
static bool cl_word(const char *p, size_t n, const char *word)
{
    size_t i=0;
    for(;i<n && word[i];i++) if(p[i]!=word[i]) return false;
    return i==n && !word[i];
}

bool bt_classic_request(bt_classic_t *c, const char *p, size_t n,
                        const bt_scan_device_t *hint, uint64_t now, const char **refusal)
{
    const char *unused;
    if(!refusal) refusal=&unused;
    *refusal=NULL; c->now=now;
    while(n && cl_space(*p)) { p++; n--; }
    while(n && cl_space(p[n-1])) n--;
    if(cl_word(p,n,"forget")) {
        cl_wipe(&c->bond,sizeof(c->bond));
        c->bond_dirty=true; cl_want_scan(c);
        if(cl_active(c)) cl_close(c,0x13);
        return true;
    }
    if(cl_word(p,n,"disconnect")) {
        if(c->phase==BT_CLASSIC_FAILED) { *refusal="the Bluetooth controller failed; reboot required"; return false; }
        if(c->phase==BT_CLASSIC_CLOSING) return true;
        if(!cl_active(c)) { *refusal="not connected"; return false; }
        cl_close(c,0x13);
        return true;
    }
    bt_classic_session_t kind;
    uint8_t address[6];
    if(cl_word(p,n,"reconnect")) kind=BT_CLASSIC_SESSION_RECONNECT;
    else if(n==22 && cl_word(p,5,"bond ") && cl_address(p+5,address)) kind=BT_CLASSIC_SESSION_BOND;
    else if(n==25 && cl_word(p,8,"connect ") && cl_address(p+8,address)) kind=BT_CLASSIC_SESSION_CONNECT;
    else { *refusal="write bond|connect XX:XX:XX:XX:XX:XX, reconnect, forget or disconnect"; return false; }
    if(c->phase==BT_CLASSIC_FAILED) { *refusal="the Bluetooth controller failed; reboot required"; return false; }
    if(c->phase!=BT_CLASSIC_IDLE && c->phase!=BT_CLASSIC_STOPPED) { *refusal="busy: the Classic link is in use"; return false; }
    const char *name=NULL;
    if(kind==BT_CLASSIC_SESSION_RECONNECT) {
        if(!c->bond.valid) { *refusal="no bond to reconnect; bond first"; return false; }
        cl_copy(address,c->bond.peer,6); name=c->bond.name;
    }
    // A hint for another address says nothing about this one.
    if(hint && cl_equal(hint->address,address,6)) {
        if(hint->le) { *refusal="the scan heard that address as LE; give it to slot 0 or 1"; return false; }
        if(kind!=BT_CLASSIC_SESSION_RECONNECT) name=hint->name;
    }
    cl_session(c,kind,address,name);
    c->phase=BT_CLASSIC_PAGING; c->next_op=0x0405;
    return true;
}

// Parameter bytes of each event this link reads. `shared`: LE links
// produce it too, so it is this link's only for its live handle.
static const struct { uint8_t code, params; bool shared; } cl_events[]={
    {0x03,11,false}, {0x04,10,false}, {0x05,4,true}, {0x06,3,false}, {0x08,4,true},
    {0x12,8,false}, {0x14,6,false}, {0x16,6,false}, {0x17,6,false}, {0x18,23,false},
    {0x30,3,true}, {0x31,6,false}, {0x32,9,false}, {0x33,10,false}, {0x36,7,false},
};

// What a command's reply carries. Asynchronous commands are accepted by
// Command Status and finished by an event; the rest answer with Command
// Complete: the address back (pairing answers, refusals, the page
// cancel), the handle and key size, or the status alone.
enum { CL_ASYNC, CL_ADDRESS, CL_KEY_SIZE, CL_STATUS };
static unsigned cl_reply_kind(uint16_t op)
{
    switch(op) {
    case 0x0405: case 0x0406: case 0x0409: case 0x040a: case 0x0411: case 0x0413: case 0x042a:
        return CL_ASYNC;
    case 0x1408: return CL_KEY_SIZE;
    case 0x0c1a: return CL_STATUS;
    default: return CL_ADDRESS;
    }
}

static bool cl_reply(bt_classic_t *c, const uint8_t *e, size_t n)
{
    bool cs=e[0]==0x0f;
    if(n<(cs ? 6u : 5u)) { c->malformed++; return false; }
    if(!c->pending || cl_u16(e+(cs ? 4 : 3))!=c->opcode) return false;
    if(c->command_done) { c->malformed++; return true; }
    unsigned kind=cl_reply_kind(c->opcode);
    if(cs) {
        // A Command Status with an error refuses any command; without one
        // it accepts only an asynchronous command.
        if(n!=6 || (kind!=CL_ASYNC && !e[2])) { c->malformed++; return true; }
        c->status=e[2]; c->command_done=true;
        return true;
    }
    size_t want=kind==CL_ADDRESS ? 12 : kind==CL_KEY_SIZE ? 9 : 6;
    if(kind==CL_ASYNC || n!=want) { c->malformed++; return true; }
    if(!e[5]) {
        if(kind==CL_ADDRESS && !cl_equal(e+6,c->command_address,6)) { c->malformed++; return true; }
        if(kind==CL_KEY_SIZE) {
            if(cl_u16(e+6)!=c->command_handle || e[8]>16) { c->malformed++; return true; }
            c->key_size=e[8];
        }
    }
    c->status=e[5]; c->command_done=true;
    return true;
}

bool bt_classic_event(bt_classic_t *c, const uint8_t *e, size_t n)
{
    if(n<2 || n!=2u+e[1]) { c->malformed++; return false; }
    if(e[0]==0x0e || e[0]==0x0f) return cl_reply(c,e,n);
    unsigned i=0, count=sizeof(cl_events)/sizeof(cl_events[0]);
    while(i<count && cl_events[i].code!=e[0]) i++;
    if(i==count) return false;
    if(e[1]!=cl_events[i].params) { c->malformed++; return !cl_events[i].shared; }
    if(c->phase==BT_CLASSIC_FAILED) return !cl_events[i].shared;
    switch(e[0]) {
    case 0x03: {   // Connection Complete: status, handle, address, link type, encryption
        uint8_t status=e[2];
        if(e[11]!=1 || !cl_equal(e+5,c->peer,6)) return true;
        if(c->phase!=BT_CLASSIC_PAGING && c->phase!=BT_CLASSIC_ACCEPTING &&
           !(c->phase==BT_CLASSIC_CLOSING && c->link_may_active)) return true;
        if(!status && cl_u16(e+3)>0x0eff) { c->malformed++; return true; }
        if(status && c->crossing) { c->crossing=false; return true; }   // the other call is still owed
        // A crossing call not yet answered is now the link made, or ended.
        if(c->crossing) c->answer[BT_CLASSIC_ANSWER_ACCEPT].set=false;
        c->link_may_active=false; c->crossing=false;
        if(status) {
            // The controller has ended the call itself; an accept still
            // latched has nobody to answer. A page nobody answered (page
            // timeout, 0x04) is headphones switched off or out of reach,
            // which is the far end gone: headphones do not always call a
            // host back, so page scan alone does not bring them home.
            c->answer[BT_CLASSIC_ANSWER_ACCEPT].set=false;
            cl_fail(c,"the connection was not made",0,status,status==0x04 || cl_far_end_gone(status),0x13);
            return true;
        }
        c->connected=true; c->handle=cl_u16(e+3); c->encrypted=false; c->disconnect_sent=false;
        if(c->phase==BT_CLASSIC_CLOSING) return true;
        c->waiting=false;
        if(c->session==BT_CLASSIC_SESSION_INBOUND) {
            c->phase=BT_CLASSIC_LINKED; c->waiting=true; c->deadline=c->now+CL_INBOUND_MS;
        } else { c->phase=BT_CLASSIC_AUTHENTICATING; c->next_op=0x0411; }
        return true;
    }
    case 0x04: {   // Connection Request: address, class, link type
        const uint8_t *a=e+2;
        if(e[11]>2) { c->malformed++; return true; }
        if(e[11]!=1) { cl_refuse(c,0x042a,a,0x0d); return true; }   // SCO or eSCO: no voice links
        bool bonded=c->bond.valid && cl_equal(a,c->bond.peer,6);
        if(bonded && (c->phase==BT_CLASSIC_IDLE || c->phase==BT_CLASSIC_STOPPED)) {
            cl_session(c,BT_CLASSIC_SESSION_INBOUND,a,c->bond.name);
            c->phase=BT_CLASSIC_ACCEPTING; cl_latch(c,BT_CLASSIC_ANSWER_ACCEPT);
            return true;
        }
        // The address being paged calls us at the same moment: its call
        // crosses ours, and whichever the controller completes is the link.
        if(c->phase==BT_CLASSIC_PAGING && !c->crossing && cl_equal(a,c->peer,6)) {
            c->crossing=true; cl_latch(c,BT_CLASSIC_ANSWER_ACCEPT);
            return true;
        }
        // Busy is the truth for the bonded peer; unacceptable for anyone else.
        cl_refuse(c,0x040a,a,bonded ? 0x0d : 0x0f);
        return true;
    }
    case 0x05:     // Disconnection Complete: status, handle, reason
        if(!cl_our_handle(c,e+3)) return false;
        if(e[2]) {
            if(c->phase==BT_CLASSIC_CLOSING) cl_give_up(c,"the controller could not disconnect",0,e[2]);
            return true;
        }
        c->connected=false; c->encrypted=false; c->disconnect_reason=e[5];
        cl_fail(c,"the peer disconnected",0,e[5],cl_far_end_gone(e[5]),0x13);
        return true;
    case 0x06:     // Authentication Complete: status, handle
        if(!cl_our_handle(c,e+3)) return true;
        if(!e[2]) {
            if(c->phase==BT_CLASSIC_AUTHENTICATING) {
                c->phase=BT_CLASSIC_ENCRYPTING; c->next_op=0x0413; c->waiting=false;
            }
            return true;
        }
        if(e[2]==0x06 && c->key_given) cl_fail(c,cl_stale,0,e[2],false,0x05);
        else cl_fail(c,"authentication failed",0,e[2],cl_far_end_gone(e[2]),0x05);
        return true;
    case 0x08: {   // Encryption Change: status, handle, enabled (1 E0, 2 AES-CCM)
        if(!cl_our_handle(c,e+3)) return false;
        bool on=!e[2] && (e[5]==1 || e[5]==2);
        c->encrypted=on;
        if(!on) { cl_fail(c,"encryption failed or was turned off",0,e[2],cl_far_end_gone(e[2]),0x05); return true; }
        if(c->phase==BT_CLASSIC_LINKED || c->phase==BT_CLASSIC_AUTHENTICATING || c->phase==BT_CLASSIC_ENCRYPTING) {
            c->phase=BT_CLASSIC_CHECKING; c->next_op=0x1408; c->waiting=false;
        }
        return true;
    }
    case 0x12:     // Role Change: status, address, new role
        if(cl_ours(c,e+3) && !e[2]) { c->role_known=true; c->role=e[9]; }
        return true;
    case 0x14:     // Mode Change: status, handle, mode, interval
        if(cl_our_handle(c,e+3) && !e[2]) { c->mode_known=true; c->mode=e[5]; c->mode_interval=cl_u16(e+6); }
        return true;
    case 0x16:     // PIN Code Request: legacy pairing is not offered
        if(cl_ours(c,e+2)) cl_latch(c,BT_CLASSIC_ANSWER_PIN);
        else cl_refuse(c,0x040e,e+2,0);
        return true;
    case 0x17:     // Link Key Request
    case 0x31:     // IO Capability Request
        if(!cl_ours(c,e+2)) {
            if(e[0]==0x17) cl_refuse(c,0x040c,e+2,0);
            else cl_refuse(c,0x0434,e+2,0x18);   // pairing not allowed
            return true;
        }
        cl_latch(c,e[0]==0x17 ? BT_CLASSIC_ANSWER_LINK_KEY : BT_CLASSIC_ANSWER_IO_CAPABILITY);
        if(c->phase==BT_CLASSIC_LINKED) {
            // They called and are securing the link themselves; a second
            // authentication started from our side would collide with it.
            c->peer_authenticating=true; c->waiting=true; c->deadline=c->now+CL_AUTH_MS;
        }
        return true;
    case 0x18:     // Link Key Notification: address, key, key type
        if(cl_ours(c,e+2) && c->session==BT_CLASSIC_SESSION_BOND) {
            cl_copy(c->new_key,e+8,16); c->new_key_type=e[24]; c->have_key=true;
        }
        return true;
    case 0x30:     // Encryption Key Refresh Complete: status, handle
        if(!cl_our_handle(c,e+3)) return false;
        c->refresh_status=e[2];
        return true;
    case 0x32:     // IO Capability Response: address, IO capability, OOB, authentication
        if(cl_ours(c,e+2)) { cl_copy(c->peer_io,e+8,3); c->peer_io_known=true; }
        return true;
    case 0x33:     // User Confirmation Request: address, numeric value (Just Works shows none)
        if(cl_ours(c,e+2)) cl_latch(c,BT_CLASSIC_ANSWER_CONFIRM);
        else cl_refuse(c,0x042d,e+2,0);
        return true;
    case 0x36:     // Simple Pairing Complete: status, address
        if(!cl_ours(c,e+3)) return true;
        if(e[2]) cl_fail(c,"simple pairing failed",0,e[2],cl_far_end_gone(e[2]),0x05);
        else c->ssp_done=true;
        return true;
    }
    return true;
}

static void cl_ready(bt_classic_t *c)
{
    c->phase=BT_CLASSIC_READY; c->waiting=false;
    if(c->session==BT_CLASSIC_SESSION_BOND && c->have_key) {
        // A debug key is one anyone listening can compute: used for this
        // session, never kept.
        if(c->new_key_type==0x03) c->debug_key=true;
        else {
            cl_wipe(&c->bond,sizeof(c->bond));
            c->bond.valid=true;
            cl_copy(c->bond.peer,c->peer,6); cl_copy(c->bond.local,c->local,6);
            cl_copy(c->bond.key,c->new_key,16); c->bond.key_type=c->new_key_type;
            cl_copy(c->bond.name,c->peer_name,sizeof(c->bond.name));
            c->bond_dirty=true; cl_want_scan(c);
        }
    }
    cl_key_wipe(c);
}

// The command in flight has its reply and its USB transfer is done.
static void cl_finish(bt_classic_t *c)
{
    uint8_t st=c->status;
    uint16_t op=c->opcode;
    if(op==0x0c1a) {
        if(!st) c->scan_enable_set=c->command_value;
        c->scan_enable_status=st;
        return;
    }
    if(!c->command_link || c->command_attempt!=c->attempt) return;
    if(op==0x0408) {
        // Unknown Connection: no page was outstanding (an accepted
        // crossing call may still be). Anything else (Connection Already
        // Exists among it) leaves the Connection Complete to say how the
        // page ended.
        if(st==0x02) {
            if(c->crossing) c->crossing=false; else c->link_may_active=false;
        }
        return;
    }
    if(op==0x0406) {
        if(st==0x02) { c->connected=false; c->encrypted=false; }   // the controller holds no such link
        else if(st) cl_give_up(c,"the controller refused to disconnect",op,st);
        return;
    }
    // A page or accept refused while two calls cross is one of the two
    // ending: the other is still owed, or is already the link.
    if(st && (op==0x0405 || op==0x0409) && (c->crossing || c->connected)) { c->crossing=false; return; }
    if(st) {
        if(op==0x0405 || op==0x0409) c->link_may_active=false;
        cl_fail(c,"HCI command rejected",op,st,false,0x13);
        return;
    }
    if(op==0x1408 && c->phase==BT_CLASSIC_CHECKING) {
        // Anything short of the full 16 bytes is the key-size negotiation attack.
        if(c->key_size<16) cl_fail(c,"weak encryption refused",op,0,false,0x05);
        else cl_ready(c);
    }
}

static void cl_timeout(bt_classic_t *c)
{
    uint16_t op=c->opcode;
    if(op==0x0c1a) { c->scan_enable_status=0xff; return; }
    if(!c->command_link || c->command_attempt!=c->attempt) return;
    if(op==0x0406 || op==0x0408) {
        if(c->phase==BT_CLASSIC_CLOSING) cl_give_up(c,"the controller did not answer the disconnection",op,0);
        return;
    }
    cl_fail(c,"HCI command timed out",op,0,false,0x13);
}

void bt_classic_tick(bt_classic_t *c, uint64_t now, bool control, bool failed,
                     bt_scan_send_t send, void *context)
{
    c->now=now;
    if(c->phase==BT_CLASSIC_FAILED) return;
    if(failed) {
        cl_error(c,"USB transport failed; reboot required",c->opcode,0,false);
        c->phase=BT_CLASSIC_FAILED; c->pending=false; c->next_op=0; c->waiting=false;
        cl_answers_clear(c); c->refusal_count=0; cl_key_wipe(c);
        return;
    }
    // A command given up on this tick still holds the controller's command
    // credit, which comes back only with its reply: nothing more is sent
    // until a later tick's `control` says the slot is free again.
    bool gave_up=false;
    if(c->pending) {
        if(c->command_done && control) { c->pending=false; cl_finish(c); }
        else if(now>=c->command_deadline) {
            bool first=!c->error;
            bool usb_done=control, reply=c->command_done;
            c->pending=false; cl_timeout(c); gave_up=true;
            if(first && c->error) { c->timed_out=true; c->timeout_usb_done=usb_done; c->timeout_reply=reply; }
        }
    }
    bool expired=false;
    for(unsigned i=0;i<BT_CLASSIC_ANSWERS;i++)
        if(c->answer[i].set && now>=c->answer[i].since+CL_ANSWER_MS) expired=true;
    if(expired) {
        cl_answers_clear(c);
        cl_fail(c,"an answer to the peer waited too long for the command slot",0,0,false,0x13);
    }
    unsigned keep=0;
    for(unsigned i=0;i<c->refusal_count;i++)
        if(now<c->refusal[i].since+CL_ANSWER_MS) c->refusal[keep++]=c->refusal[i];
    c->refusal_count=keep;
    if(c->waiting && now>=c->deadline) {
        c->waiting=false;
        if(c->phase==BT_CLASSIC_LINKED) { c->phase=BT_CLASSIC_AUTHENTICATING; c->next_op=0x0411; }
        else if(c->phase==BT_CLASSIC_CLOSING) cl_give_up(c,"the controller did not confirm the link was closed",0,0);
        else if(c->phase==BT_CLASSIC_PAGING) cl_fail(c,"the page was not answered",0x0405,0,false,0x13);
        else cl_fail(c,"the peer did not answer in time",0,0,false,0x13);
    }
    if(c->phase==BT_CLASSIC_CLOSING) {
        // A cancel queued for a page that has since ended is not sent; a
        // link that page brought up is disconnected instead.
        if(c->next_op==0x0408 && !c->link_may_active) c->next_op=0;
        if(c->connected && !c->disconnect_sent && !c->next_op) c->next_op=0x0406;
        if(!c->connected && !c->link_may_active) { c->next_op=0; c->waiting=false; c->phase=BT_CLASSIC_STOPPED; }
    }
    if(c->pending || !control || gave_up) return;
    uint8_t p[22]={0}, len=0;
    uint16_t op=0;
    bool link=true;
    unsigned k=0;
    while(k<BT_CLASSIC_ANSWERS && !c->answer[k].set) k++;
    if(k<BT_CLASSIC_ANSWERS) {
        c->answer[k].set=false;
        cl_copy(p,c->peer,6); len=6;
        switch(k) {
        case BT_CLASSIC_ANSWER_ACCEPT:
            op=0x0409; p[len++]=0x01;    // remain the peripheral: the caller chose to be central
            c->link_may_active=true; c->waiting=true; c->deadline=now+CL_ACCEPT_MS;
            break;
        case BT_CLASSIC_ANSWER_LINK_KEY:
            if(c->bond.valid && cl_equal(c->bond.peer,c->peer,6)) {
                op=0x040b; cl_copy(p+6,c->bond.key,16); len=22; c->key_given=true;
            } else op=0x040c;            // no key: the controller pairs
            break;
        case BT_CLASSIC_ANSWER_IO_CAPABILITY:
            // NoInputNoOutput, no OOB data; general bonding without MITM
            // protection unless this session keeps nothing.
            op=0x042b; p[len++]=0x03; p[len++]=0x00;
            p[len++]=c->session==BT_CLASSIC_SESSION_CONNECT ? 0x00 : 0x04;
            break;
        case BT_CLASSIC_ANSWER_CONFIRM: op=0x042c; break;   // Just Works
        default: op=0x040e; break;
        }
    } else if(c->refusal_count) {
        bt_classic_refusal_t *r=&c->refusal[0];
        op=r->opcode; cl_copy(p,r->address,6); len=6;
        if(op==0x040a || op==0x042a || op==0x0434) p[len++]=r->reason;
        link=false;
        c->refusal_count--;
        for(unsigned i=0;i<c->refusal_count;i++) c->refusal[i]=c->refusal[i+1];
    } else if(c->next_op) {
        op=c->next_op; c->next_op=0;
        switch(op) {
        case 0x0405:
            // DM1/DH1/DM3/DH3/DM5/DH5, page scan repetition mode R2, no
            // clock offset (the scan does not keep one), role switch allowed.
            cl_copy(p,c->peer,6); cl_put(p+6,0xcc18); p[8]=0x02; p[9]=0; cl_put(p+10,0); p[12]=1; len=13;
            c->link_may_active=true; c->waiting=true; c->deadline=now+CL_PAGE_MS;
            break;
        case 0x0408:
            cl_copy(p,c->peer,6); len=6;
            c->waiting=true; c->deadline=now+CL_CLOSE_MS;
            break;
        case 0x0406:
            cl_put(p,c->handle); p[2]=c->close_reason; len=3; c->disconnect_sent=true;
            c->waiting=true; c->deadline=now+CL_CLOSE_MS;
            break;
        case 0x0411:
            cl_put(p,c->handle); len=2;
            c->waiting=true; c->deadline=now+CL_AUTH_MS;
            break;
        case 0x0413:
            cl_put(p,c->handle); p[2]=1; len=3;
            c->waiting=true; c->deadline=now+CL_ENCRYPT_MS;
            break;
        default:   // 0x1408: the command's own deadline is the wait
            cl_put(p,c->handle); len=2;
            break;
        }
    } else if(cl_scan_owed(c)) {
        op=0x0c1a; p[0]=c->scan_enable_wanted; len=1; link=false;
        c->scan_enable_tried=p[0]; c->command_value=p[0];
    } else return;
    c->opcode=op; c->status=0; c->command_done=false; c->pending=true;
    c->command_deadline=now+CL_COMMAND_MS;
    c->command_link=link; c->command_attempt=c->attempt; c->command_handle=c->handle;
    cl_copy(c->command_address,p,6);
    send(context,op,p,len);
    cl_wipe(p,sizeof(p));
}

bool bt_classic_urgent(const bt_classic_t *c)
{
    if(c->refusal_count) return true;
    for(unsigned i=0;i<BT_CLASSIC_ANSWERS;i++) if(c->answer[i].set) return true;
    return false;
}

bool bt_classic_quiescent(const bt_classic_t *c)
{
    return (c->phase==BT_CLASSIC_IDLE || c->phase==BT_CLASSIC_STOPPED) && !c->pending &&
        !c->next_op && !bt_classic_urgent(c) && !cl_scan_owed(c);
}

static const char *cl_phase_text(bt_classic_phase_t phase)
{
    switch(phase) {
    case BT_CLASSIC_IDLE: return "idle";
    case BT_CLASSIC_PAGING: return "paging";
    case BT_CLASSIC_ACCEPTING: return "accepting a call";
    case BT_CLASSIC_LINKED: return "linked";
    case BT_CLASSIC_AUTHENTICATING: return "authenticating";
    case BT_CLASSIC_ENCRYPTING: return "encrypting";
    case BT_CLASSIC_CHECKING: return "checking the key size";
    case BT_CLASSIC_READY: return "ready";
    case BT_CLASSIC_CLOSING: return "closing";
    case BT_CLASSIC_STOPPED: return "stopped";
    default: return "failed (reboot required)";
    }
}

size_t bt_classic_status(const bt_classic_t *c, char *out, size_t cap)
{
    static const char *const sessions[]={"bond","session only (nothing kept)","reconnect to the bond","called by the bonded peer"};
    static const char *const modes[]={"active","hold","sniff"};
    const bt_classic_bond_t *b=&c->bond;
    char held[96], bond[160], io[64], role[40];
    if(b->valid)
        snprintf(held,sizeof(held),"%02x:%02x:%02x:%02x:%02x:%02x key type %02x name %s",
                 b->peer[5],b->peer[4],b->peer[3],b->peer[2],b->peer[1],b->peer[0],b->key_type,
                 b->name[0] ? b->name : "(none)");
    else snprintf(held,sizeof(held),"none");
    if(c->debug_key) snprintf(bond,sizeof(bond),"not kept (debug link key); held: %s",held);
    else snprintf(bond,sizeof(bond),"%s",held);
    if(c->peer_io_known) snprintf(io,sizeof(io),"IO %02x OOB %02x authentication %02x",c->peer_io[0],c->peer_io[1],c->peer_io[2]);
    else snprintf(io,sizeof(io),"unknown");
    if(c->mode_known) snprintf(role,sizeof(role),"%s interval %u",c->mode<3 ? modes[c->mode] : "unknown",(unsigned)c->mode_interval);
    else snprintf(role,sizeof(role),"unknown");
    const char *timeout=!c->timed_out ? "" : !c->timeout_usb_done ? "; the USB transfer had not completed" :
        "; the USB transfer completed, no reply came";
    unsigned waiting=0;
    for(unsigned i=0;i<BT_CLASSIC_ANSWERS;i++) waiting+=c->answer[i].set;
    int n=snprintf(out,cap,"state: %s\nsession: %s\npeer: %02x:%02x:%02x:%02x:%02x:%02x name %s\n"
        "connected: %s handle %04x encrypted: %s key size: %u\n"
        "role: %s mode: %s\npeer pairing capabilities: %s simple pairing: %s\n"
        "bond: %s (to save: %s)\npage scan: wanted %02x confirmed %02x last write status %02x\n"
        "answers waiting: %u refusals waiting: %u refusals dropped: %u\n"
        "error: %s (opcode %04x status %02x)%s\ndisconnect reason: %02x retryable: %s\n"
        "malformed events: %u\n"
        "write bond|connect XX:XX:XX:XX:XX:XX, reconnect, forget or disconnect\n",
        cl_phase_text(c->phase),sessions[c->session],
        c->peer[5],c->peer[4],c->peer[3],c->peer[2],c->peer[1],c->peer[0],c->peer_name[0] ? c->peer_name : "(none)",
        c->connected ? "yes" : "no",(unsigned)c->handle,c->encrypted ? "yes" : "no",(unsigned)c->key_size,
        !c->role_known ? "unknown" : c->role ? "peripheral" : "central",role,io,c->ssp_done ? "complete" : "not done",
        bond,c->bond_dirty ? "yes" : "no",c->scan_enable_wanted,c->scan_enable_set,c->scan_enable_status,
        waiting,c->refusal_count,(unsigned)c->refusals_dropped,
        c->error ? c->error : "none",c->error_opcode,c->error_status,timeout,
        c->disconnect_reason,c->retryable ? "yes" : "no",(unsigned)c->malformed);
    return n<0 || !cap ? 0 : (size_t)n<cap ? (size_t)n : cap-1;
}

// The bond's check: bt_bond.c's CRC-32.
static uint32_t cl_crc(const uint8_t *p, size_t n)
{
    uint32_t crc=0xffffffff;
    while(n--) {
        crc^=*p++;
        for(unsigned i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}

void bt_classic_bond_encode(uint8_t out[BT_CLASSIC_BOND_RECORD_BYTES], const bt_classic_bond_t *b)
{
    cl_wipe(out,BT_CLASSIC_BOND_RECORD_BYTES);
    cl_copy(out,"OS64BC01",8);
    if(b->valid) {
        out[8]=1; out[9]=b->key_type;
        cl_copy(out+12,b->peer,6); cl_copy(out+18,b->local,6); cl_copy(out+24,b->key,16);
        cl_name((char *)out+40,b->name);
    }
    uint32_t crc=cl_crc(out,76);
    for(unsigned i=0;i<4;i++) out[76+i]=(uint8_t)(crc>>(8*i));
}

bool bt_classic_bond_decode(bt_classic_bond_t *b, const uint8_t *p, size_t n)
{
    cl_wipe(b,sizeof(*b));
    if(n!=BT_CLASSIC_BOND_RECORD_BYTES || !cl_equal(p,(const uint8_t *)"OS64BC01",8)) return false;
    uint32_t crc=cl_crc(p,76);
    for(unsigned i=0;i<4;i++) if(p[76+i]!=(uint8_t)(crc>>(8*i))) return false;
    if(p[8]>1 || p[10] || p[11] || p[72] || p[73] || p[74] || p[75]) return false;
    if(!p[8]) {
        for(unsigned i=8;i<76;i++) if(p[i]) return false;
        return true;
    }
    unsigned end=0;
    while(end<32 && p[40+end]) {
        if(p[40+end]<32 || p[40+end]>126) return false;
        end++;
    }
    if(end==32) return false;
    for(unsigned i=end;i<32;i++) if(p[40+i]) return false;
    b->valid=true; b->key_type=p[9];
    cl_copy(b->peer,p+12,6); cl_copy(b->local,p+18,6); cl_copy(b->key,p+24,16);
    cl_copy(b->name,p+40,32);
    return true;
}
