// bt_avrcp.c — AVCTP and AVRCP, both roles. bt_avrcp.h is the contract;
// docs/design/pending/AUDIO_REMOTE.md § Part R the design. The wire is
// AVCTP 1.3 and AVRCP 1.4 as BlueZ (profiles/audio/avctp.c, avrcp.c) and
// Android's stack (stack/avct, stack/avrc, profile/avrcp) read and write it.
// AVCTP lives here beside AVRCP: what it adds is a three-byte header and
// the reassembly of a fragmented message, AVRCP is the one profile it
// carries, and a file of its own would need a seam as large as itself.
//
// AVCTP, one L2CAP payload:
//   byte 0: transaction label (bits 7-4), packet type (3-2), C/R (bit 1,
//           set in a response), IPID (bit 0, set in the answer to a message
//           for a profile nobody here speaks)
//   single: bytes 1-2 the profile identifier; then the AV/C frame
//   start:  byte 1 the number of packets, the start included; bytes 2-3
//           the profile identifier; then the frame
//   continue, end: byte 0 alone, then more of the frame
// The AV/C frame: byte 0 the command or response type (low four bits),
// byte 1 the subunit (type in bits 7-3, id in 2-0: the panel is 0x48),
// byte 2 the opcode, then its operands. A vendor-dependent frame's
// operands are the company (three bytes), the PDU, the packet type (low
// two bits), the parameters' length, then the parameters. Every multi-byte
// field is big-endian.
#include "driver/system/usb/bt_avrcp.h"
#include "strings/sprintf.h"

#define PKT_SINGLE    0
#define PKT_START     1
#define PKT_CONTINUE  2
#define PKT_END       3
#define HDR_RESPONSE  0x02
#define HDR_IPID      0x01

#define CT_CONTROL         0x0
#define CT_STATUS          0x1
#define CT_NOTIFY          0x3
#define CT_NOT_IMPLEMENTED 0x8
#define CT_ACCEPTED        0x9
#define CT_REJECTED        0xa
#define CT_STABLE          0xc
#define CT_CHANGED         0xd
#define CT_INTERIM         0xf

#define SUBUNIT_PANEL      0x48
#define OP_VENDOR          0x00
#define OP_UNIT_INFO       0x30
#define OP_SUBUNIT_INFO    0x31
#define OP_PASS_THROUGH    0x7c
#define PASS_RELEASED      0x80

#define PDU_GET_CAPABILITIES       0x10
#define PDU_GET_ELEMENT_ATTRIBUTES 0x20
#define PDU_GET_PLAY_STATUS        0x30
#define PDU_REGISTER_NOTIFICATION  0x31
#define PDU_SET_ABSOLUTE_VOLUME    0x50
#define CAP_COMPANY        0x02
#define CAP_EVENTS         0x03
#define EVENT_PLAYBACK     0x01
#define EVENT_TRACK        0x02
#define EVENT_VOLUME       0x0d
#define STATUS_INVALID_COMMAND   0x00
#define STATUS_INVALID_PARAMETER 0x01

// What we build, in bytes with AVCTP's header, each within
// BT_AVRCP_LONGEST and so within the least MTU an AVCTP channel may have.
#define HEADERS          6                    // AVCTP's three, AV/C's three
#define VENDOR           (HEADERS + 7)
#define REGISTER_BYTES   (VENDOR + 5)         // our Register Notification
#define PLAY_STATUS_BYTES (VENDOR + 9)        // Get Play Status' answer; the track's INTERIM is as long
#define INFO_BYTES       (HEADERS + 5)        // Unit Info's and Subunit Info's answers
_Static_assert(REGISTER_BYTES <= BT_AVRCP_LONGEST, "our longest command fits the buffer");
_Static_assert(PLAY_STATUS_BYTES <= BT_AVRCP_LONGEST, "our longest vendor answer fits the buffer");
_Static_assert(INFO_BYTES <= BT_AVRCP_LONGEST, "the info answers fit the buffer");
_Static_assert(HEADERS + BT_AVRCP_ECHO == BT_AVRCP_LONGEST, "an echoed answer fits the buffer");
_Static_assert(BT_AVRCP_LONGEST <= BT_AVRCP_LEAST_MTU, "nothing we send is fragmented");

static const uint8_t ar_ops[7]={BT_AVRCP_OP_VOLUME_UP,BT_AVRCP_OP_VOLUME_DOWN,BT_AVRCP_OP_PLAY,BT_AVRCP_OP_STOP,
                                BT_AVRCP_OP_PAUSE,BT_AVRCP_OP_FORWARD,BT_AVRCP_OP_BACKWARD};
static const char *const ar_op_names[7]={"volume up","volume down","play","stop","pause","forward","backward"};

static void ar_wipe(void *p, size_t n) { uint8_t *v=p; while(n--) *v++=0; }
static void ar_copy(void *d, const void *s, size_t n)
{ uint8_t *x=d; const uint8_t *y=s; while(n--) *x++=*y++; }
static uint16_t ar_u16(const uint8_t *p) { return (uint16_t)(p[0]<<8 | p[1]); }

static void ar_put(bt_avrcp_msg_t *m, uint8_t v) { if(m->bytes<BT_AVRCP_LONGEST) m->pdu[m->bytes++]=v; }
static void ar_put16(bt_avrcp_msg_t *m, uint16_t v) { ar_put(m,(uint8_t)(v>>8)); ar_put(m,(uint8_t)v); }

// A message's id: never 0, never one given before in the life of the
// struct (it wraps after four billion messages).
static uint32_t ar_id(bt_avrcp_t *a)
{
    if(++a->next_id==0) a->next_id=1;
    return a->next_id;
}

// AVCTP's header for a single packet under our profile, and the AV/C header.
static void ar_begin(bt_avrcp_msg_t *m, uint8_t label, bool response, uint8_t ctype, uint8_t subunit, uint8_t opcode)
{
    m->bytes=0;
    ar_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | (response ? HDR_RESPONSE : 0)));
    ar_put16(m,BT_AVRCP_PROFILE);
    ar_put(m,ctype); ar_put(m,subunit); ar_put(m,opcode);
}

static void ar_company(bt_avrcp_msg_t *m) { ar_put(m,0x00); ar_put(m,0x19); ar_put(m,0x58); }

// The vendor-dependent header under the Bluetooth SIG's company: the PDU,
// a single packet, the parameters' length.
static void ar_vendor(bt_avrcp_msg_t *m, uint8_t pdu, uint16_t params)
{
    ar_company(m);
    ar_put(m,pdu); ar_put(m,PKT_SINGLE); ar_put16(m,params);
}

static int ar_op_index(uint8_t op)
{
    for(int i=0;i<7;i++) if(ar_ops[i]==op) return i;
    return -1;
}

// ── Their presses ───────────────────────────────────────────────────────────

static bool ar_ring_push(bt_avrcp_t *a, uint8_t op, bool pressed)
{
    if(a->ring_count>=BT_AVRCP_PRESSES) return false;
    a->ring[(a->ring_head+a->ring_count)%BT_AVRCP_PRESSES]=(bt_avrcp_press_t){ .operation=op, .pressed=pressed };
    a->ring_count++;
    return true;
}

// The press held ends: its release reaches the ring when its press did.
static void ar_let_go(bt_avrcp_t *a)
{
    if(!a->held) return;
    if(a->held_reported) ar_ring_push(a,a->held,false);
    a->held=0; a->held_reported=false;
}

// A press. The same operation again with no release between is their
// repeat while the button is down: one hold, its two seconds begun again.
// Another operation releases the one held first. A press reaches the ring
// only with room for its release beside it, and nothing else reaches the
// ring until that release (a new press lets go of the old first), so a
// press the host reads is always followed by its release.
static void ar_pressed(bt_avrcp_t *a, uint8_t op, int which)
{
    if(a->held==op) { a->held_until=a->now+BT_AVRCP_HOLD_MS; return; }
    ar_let_go(a);
    a->presses[which]++;
    a->held=op;
    a->held_until=a->now+BT_AVRCP_HOLD_MS;
    a->held_reported=a->ring_count+2u<=BT_AVRCP_PRESSES && ar_ring_push(a,op,true);
    if(!a->held_reported) a->presses_lost++;
}

// A release ends the hold of its own operation; one of an operation not
// held is answered and counted, and goes no further.
static void ar_released(bt_avrcp_t *a, uint8_t op)
{
    if(a->held!=op) { a->unmatched_releases++; return; }
    ar_let_go(a);
}

// ── The target: what we answer ──────────────────────────────────────────────

// An answer that is their frame with the response type in place of theirs:
// the AV/C headers and as many of their operands as BT_AVRCP_ECHO allows.
static void ar_echo(bt_avrcp_msg_t *m, uint8_t label, uint8_t ctype, const uint8_t *f, size_t n)
{
    ar_begin(m,label,true,ctype,f[1],f[2]);
    size_t k=n-3<BT_AVRCP_ECHO ? n-3 : BT_AVRCP_ECHO;
    for(size_t i=0;i<k;i++) ar_put(m,f[3+i]);
}

static void ar_reject(bt_avrcp_t *a, bt_avrcp_msg_t *m, uint8_t label, uint8_t subunit, uint8_t pdu, uint8_t status)
{
    ar_begin(m,label,true,CT_REJECTED,subunit,OP_VENDOR);
    ar_vendor(m,pdu,1);
    ar_put(m,status);
    a->rejected++;
}

// Pass through: one of the seven operations taken is ACCEPTED and its
// press or release goes to the host; another is NOT IMPLEMENTED. Not a
// control command to the panel, or no operation byte and length: REJECTED.
// The length byte is not held against them: the operation is unambiguous.
static void ar_pass_through(bt_avrcp_t *a, bt_avrcp_msg_t *m, uint8_t label, const uint8_t *f, size_t n)
{
    if((f[0]&0x0f)!=CT_CONTROL || f[1]!=SUBUNIT_PANEL || n<5) {
        ar_echo(m,label,CT_REJECTED,f,n); a->rejected++;
        return;
    }
    uint8_t op=f[3]&0x7f;
    int which=ar_op_index(op);
    if(which<0) { ar_echo(m,label,CT_NOT_IMPLEMENTED,f,n); a->not_taken++; a->not_implemented++; return; }
    ar_echo(m,label,CT_ACCEPTED,f,n);
    a->answered++;
    if(f[3]&PASS_RELEASED) ar_released(a,op);
    else ar_pressed(a,op,which);
}

// Vendor dependent, the AVRCP PDUs: each answered as AUDIO_REMOTE.md's
// table has it, or rejected with the status its fault calls for (a PDU we
// do not take, or one sent as the wrong command type or in AVRCP's own
// fragments, is an invalid command; a parameter we do not offer, or a
// length that is not the PDU's, an invalid parameter).
static void ar_vendor_in(bt_avrcp_t *a, bt_avrcp_msg_t *m, uint8_t label, const uint8_t *f, size_t n)
{
    uint8_t ctype=f[0]&0x0f, subunit=f[1];
    const uint8_t *o=f+3;
    size_t on=n-3;
    if(on<3 || o[0]!=0x00 || o[1]!=0x19 || o[2]!=0x58) { ar_echo(m,label,CT_NOT_IMPLEMENTED,f,n); a->not_implemented++; return; }
    uint8_t pdu=on>3 ? o[3] : 0;
    if(on<7 || (o[4]&3)!=PKT_SINGLE) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }
    const uint8_t *p=o+7;
    size_t pn=on-7;
    if(ar_u16(o+5)!=pn) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }
    switch(pdu) {
    case PDU_GET_CAPABILITIES:
        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }
        if(pn!=1 || (p[0]!=CAP_COMPANY && p[0]!=CAP_EVENTS)) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }
        ar_begin(m,label,true,CT_STABLE,subunit,OP_VENDOR);
        if(p[0]==CAP_COMPANY) {
            ar_vendor(m,pdu,5);
            ar_put(m,CAP_COMPANY); ar_put(m,1); ar_company(m);
        } else {
            ar_vendor(m,pdu,4);
            ar_put(m,CAP_EVENTS); ar_put(m,2); ar_put(m,EVENT_PLAYBACK); ar_put(m,EVENT_TRACK);
        }
        break;
    case PDU_REGISTER_NOTIFICATION:
        if(ctype!=CT_NOTIFY) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }
        if(pn!=5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK)) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }
        ar_begin(m,label,true,CT_INTERIM,subunit,OP_VENDOR);
        if(p[0]==EVENT_PLAYBACK) {
            // Its INTERIM, and the CHANGED it may come to owe, each need a
            // slot; a registration standing already had its CHANGED's.
            if(!a->play_registered && a->answers+2u>BT_AVRCP_ANSWERS) { m->bytes=0; a->overrun++; return; }
            a->play_registered=true; a->play_label=label; a->play_subunit=subunit;
            ar_vendor(m,pdu,2);
            ar_put(m,EVENT_PLAYBACK); ar_put(m,a->playback);
        } else {
            // While we play, the identifier 0: "a track is playing", as a
            // target without browsing says it (Android's does); otherwise
            // all ones, "no track selected". We have no notion of a track
            // changing, so the registration is never spent.
            a->track_registered=true;
            ar_vendor(m,pdu,9);
            ar_put(m,EVENT_TRACK);
            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0x00 : 0xff;
            for(int i=0;i<8;i++) ar_put(m,track);
        }
        break;
    case PDU_GET_PLAY_STATUS:
        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }
        if(pn) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }
        // The song's length and position are not known: 0xFFFFFFFF each.
        ar_begin(m,label,true,CT_STABLE,subunit,OP_VENDOR);
        ar_vendor(m,pdu,9);
        for(int i=0;i<8;i++) ar_put(m,0xff);
        ar_put(m,a->playback);
        break;
    case PDU_GET_ELEMENT_ATTRIBUTES: {
        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }
        // The element is the one playing (identifier zero), then a count
        // of four-byte attribute ids: answered with no attributes.
        bool playing=pn>=9;
        for(size_t i=0;playing && i<8;i++) if(p[i]) playing=false;
        if(!playing || pn!=9u+4u*p[8]) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }
        ar_begin(m,label,true,CT_STABLE,subunit,OP_VENDOR);
        ar_vendor(m,pdu,1);
        ar_put(m,0);
        break;
    }
    default:
        ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND);
        return;
    }
    a->answered++;
}

// One command of theirs. With no room for its answer it is dropped
// unanswered and counted, and nothing it asked for is done; the room kept
// for a standing registration's CHANGED is not theirs to take. A message
// for another profile is answered with AVCTP's header alone, IPID set.
static void ar_command_in(bt_avrcp_t *a, uint8_t header, uint16_t profile, const uint8_t *f, size_t n)
{
    uint8_t label=header>>4;
    if(header&HDR_IPID) { a->malformed++; return; }
    unsigned keep=a->play_registered ? 1 : 0;
    if(a->answers+1u+keep>BT_AVRCP_ANSWERS) { a->overrun++; return; }
    bt_avrcp_msg_t m={0};
    if(profile!=BT_AVRCP_PROFILE) {
        ar_put(&m,(uint8_t)(label<<4 | PKT_SINGLE<<2 | HDR_RESPONSE | HDR_IPID));
        ar_put16(&m,profile);
        a->invalid_profile++;
    } else if(n<3) {
        a->malformed++;              // no AV/C header to answer under
        return;
    } else switch(f[2]) {
    case OP_UNIT_INFO: case OP_SUBUNIT_INFO:
        if((f[0]&0x0f)!=CT_STATUS) { ar_echo(&m,label,CT_REJECTED,f,n); a->rejected++; break; }
        // The unit is a panel: Unit Info names it and the SIG's company,
        // Subunit Info its page (theirs, kept) and the panel, then padding.
        ar_begin(&m,label,true,CT_STABLE,f[1],f[2]);
        if(f[2]==OP_UNIT_INFO) { ar_put(&m,0x07); ar_put(&m,SUBUNIT_PANEL); ar_company(&m); }
        else { ar_put(&m,n>3 ? f[3] : 0x07); ar_put(&m,SUBUNIT_PANEL); ar_put(&m,0xff); ar_put(&m,0xff); ar_put(&m,0xff); }
        a->answered++;
        break;
    case OP_PASS_THROUGH:
        ar_pass_through(a,&m,label,f,n);
        break;
    case OP_VENDOR:
        ar_vendor_in(a,&m,label,f,n);
        break;
    default:
        ar_echo(&m,label,CT_NOT_IMPLEMENTED,f,n);
        a->not_implemented++;
        break;
    }
    if(!m.bytes) return;
    m.id=ar_id(a);
    a->answer[a->answers++]=m;
}

// ── The controller: their volume ────────────────────────────────────────────

// A label of ours that our other command does not hold, searched from
// where the last search stopped.
static uint8_t ar_label(bt_avrcp_t *a, const bt_avrcp_command_t *other)
{
    uint8_t l=a->next_label;
    if(other->live && other->label==l) l=(uint8_t)((l+1)&15);
    a->next_label=(uint8_t)((l+1)&15);
    return l;
}

// A command of ours, new in its slot: a label of its own, queued.
static bt_avrcp_msg_t *ar_command(bt_avrcp_t *a, bt_avrcp_command_t *c, uint8_t ctype, uint8_t pdu, uint16_t params)
{
    const bt_avrcp_command_t *other=c==&a->reg ? &a->set : &a->reg;
    c->live=true; c->sent=false; c->waiting=false; c->tries=0;
    c->pdu_id=pdu;
    c->label=ar_label(a,other);
    ar_begin(&c->msg,c->label,false,ctype,SUBUNIT_PANEL,OP_VENDOR);
    ar_vendor(&c->msg,pdu,params);
    c->msg.id=ar_id(a);
    return &c->msg;
}

static void ar_ask(bt_avrcp_t *a)
{
    a->state=BT_AVRCP_ASKING;
    bt_avrcp_msg_t *m=ar_command(a,&a->reg,CT_STATUS,PDU_GET_CAPABILITIES,1);
    ar_put(m,CAP_EVENTS);
}

// Register Notification for their volume; the interval is zero, as it is
// for every event but the playback position.
static void ar_register(bt_avrcp_t *a)
{
    bt_avrcp_msg_t *m=ar_command(a,&a->reg,CT_NOTIFY,PDU_REGISTER_NOTIFICATION,5);
    ar_put(m,EVENT_VOLUME);
    ar_put(m,0); ar_put(m,0); ar_put(m,0); ar_put(m,0);
    a->registrations++;
}

static void ar_send_set(bt_avrcp_t *a, uint8_t level)
{
    bt_avrcp_msg_t *m=ar_command(a,&a->set,CT_CONTROL,PDU_SET_ABSOLUTE_VOLUME,1);
    ar_put(m,level&0x7f);
}

// PLAIN, FAILED or IDLE: absolute volume is over for this channel; our
// commands go, and leaving READY is their level's going.
static void ar_controller_ends(bt_avrcp_t *a, bt_avrcp_state_t state)
{
    a->reg.live=false; a->set.live=false; a->set_pending=false;
    a->state=state;
}

// The Set out is finished; the newest level asked meanwhile goes next.
static void ar_set_done(bt_avrcp_t *a)
{
    a->set.live=false;
    if(a->set_pending) { a->set_pending=false; ar_send_set(a,a->set_wanted); }
}

// They told us a level. The top bit is reserved.
static void ar_level(bt_avrcp_t *a, uint8_t v)
{
    a->volume=v&0x7f;
    a->volume_serial++;
}

// Our command was refused: rejected, not implemented, or answered as a
// profile they do not speak. A refused Set is counted and changes
// nothing; a refused question or registration means no absolute volume.
static void ar_refused(bt_avrcp_t *a, bt_avrcp_command_t *c)
{
    if(c==&a->set) { a->sets_rejected++; ar_set_done(a); return; }
    ar_controller_ends(a,BT_AVRCP_PLAIN);
}

// Get Capabilities (events), answered STABLE: the capability, a count and
// that many events.
static void ar_capabilities(bt_avrcp_t *a, uint8_t ctype, const uint8_t *p, size_t n)
{
    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n!=2u+p[1]) { a->malformed++; return; }
    a->reg.live=false;
    for(size_t i=0;i<p[1];i++)
        if(p[2+i]==EVENT_VOLUME) { a->state=BT_AVRCP_REGISTERING; ar_register(a); return; }
    ar_controller_ends(a,BT_AVRCP_PLAIN);
}

// Our volume registration: INTERIM gives their level and it stands; CHANGED
// gives the new level, spends it, and it is made again (a CHANGED before
// any INTERIM is taken the same way). A second INTERIM on a standing
// registration (the answer to its second transmission) is taken as their
// level too: it left them after the first, so it is the newer.
static void ar_registration(bt_avrcp_t *a, uint8_t ctype, const uint8_t *p, size_t n)
{
    if(n!=2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED)) { a->malformed++; return; }
    if(ctype==CT_INTERIM) {
        a->reg.waiting=false;
        ar_level(a,p[1]);
        a->state=BT_AVRCP_READY;
        return;
    }
    ar_level(a,p[1]);
    a->state=BT_AVRCP_READY;
    ar_register(a);
}

// Our Set: ACCEPTED carries the level they took. INTERIM means they will
// answer later: its timer begins again.
static void ar_set_answer(bt_avrcp_t *a, uint8_t ctype, const uint8_t *p, size_t n)
{
    if(ctype==CT_INTERIM) { a->sets_interim++; a->set.deadline=a->now+BT_AVRCP_REPLY_MS; return; }
    if(ctype!=CT_ACCEPTED || n!=1) { a->malformed++; return; }
    a->sets_accepted++;
    ar_level(a,p[0]);
    ar_set_done(a);
}

// A response is taken only when it carries the label of a command of ours
// that has been sent; anything else is a stray. One that carries the
// label and another PDU is a stray too, and one we cannot read is
// malformed: either way the command goes on waiting, its timer deciding
// (a registration that stands goes on standing).
// NOT IMPLEMENTED may echo our command or carry less of it; what PDU it
// carries must be ours.
static void ar_response(bt_avrcp_t *a, uint8_t header, const uint8_t *f, size_t n)
{
    uint8_t label=header>>4;
    bt_avrcp_command_t *c=a->reg.live && a->reg.sent && a->reg.label==label ? &a->reg
                         : a->set.live && a->set.sent && a->set.label==label ? &a->set : 0;
    if(!c) { a->strays++; return; }
    if(header&HDR_IPID) { a->invalid_profile++; ar_refused(a,c); return; }
    if(n<3) { a->malformed++; return; }
    if(f[2]!=OP_VENDOR) { a->strays++; return; }
    uint8_t ctype=f[0]&0x0f;
    const uint8_t *o=f+3;
    size_t on=n-3;
    bool sig=on>=3 && o[0]==0x00 && o[1]==0x19 && o[2]==0x58;
    if(ctype==CT_NOT_IMPLEMENTED) {
        if(sig && on>=4 && o[3]!=c->pdu_id) { a->strays++; return; }
        ar_refused(a,c);
        return;
    }
    if(!sig || on<7) { a->malformed++; return; }
    if(o[3]!=c->pdu_id) { a->strays++; return; }
    if((o[4]&3)!=PKT_SINGLE || ar_u16(o+5)!=on-7) { a->malformed++; return; }
    const uint8_t *p=o+7;
    size_t pn=on-7;
    if(ctype==CT_REJECTED) { ar_refused(a,c); return; }
    if(c==&a->set) ar_set_answer(a,ctype,p,pn);
    else if(c->pdu_id==PDU_GET_CAPABILITIES) ar_capabilities(a,ctype,p,pn);
    else ar_registration(a,ctype,p,pn);
}

// One whole message: its AVCTP header byte and profile, then the frame.
// A response for another profile that does not say so is a stray.
static void ar_message(bt_avrcp_t *a, uint8_t header, uint16_t profile, const uint8_t *f, size_t n)
{
    if(!(header&HDR_RESPONSE)) { ar_command_in(a,header,profile,f,n); return; }
    if(profile!=BT_AVRCP_PROFILE && !(header&HDR_IPID)) { a->strays++; return; }
    ar_response(a,header,f,n);
}

// ── The contract ────────────────────────────────────────────────────────────

void bt_avrcp_init(bt_avrcp_t *a) { ar_wipe(a,sizeof(*a)); a->state=BT_AVRCP_IDLE; a->playback=BT_AVRCP_STOPPED; }

void bt_avrcp_reset(bt_avrcp_t *a)
{
    ar_let_go(a);
    bt_avrcp_press_t ring[BT_AVRCP_PRESSES];
    ar_copy(ring,a->ring,sizeof(ring));
    uint8_t head=a->ring_head, count=a->ring_count;
    bt_avrcp_init(a);
    ar_copy(a->ring,ring,sizeof(ring));
    a->ring_head=head; a->ring_count=count;
}

void bt_avrcp_channel_open(bt_avrcp_t *a, uint64_t now)
{
    a->now=now;
    if(a->state!=BT_AVRCP_IDLE) return;
    ar_ask(a);
}

void bt_avrcp_channel_closed(bt_avrcp_t *a, uint64_t now)
{
    a->now=now;
    ar_let_go(a);
    ar_wipe(a->answer,sizeof(a->answer));
    a->answers=0;
    ar_controller_ends(a,BT_AVRCP_IDLE);
    a->play_registered=false; a->track_registered=false;
    a->restarts=0;
    a->rx_active=false;
}

void bt_avrcp_receive(bt_avrcp_t *a, const uint8_t *p, size_t n, uint64_t now)
{
    a->now=now;
    if(a->state==BT_AVRCP_IDLE) { a->ignored++; return; }
    if(!n) { a->malformed++; return; }
    uint8_t type=(p[0]>>2)&3;
    if(type==PKT_SINGLE || type==PKT_START) {
        // A new message ends one still being gathered, which is dropped.
        if(a->rx_active) { a->fragments_dropped++; a->rx_active=false; }
        if(type==PKT_SINGLE) {
            if(n<3) { a->malformed++; return; }
            ar_message(a,p[0],ar_u16(p+1),p+3,n-3);
            return;
        }
        if(n<4 || p[1]<2) { a->fragments_dropped++; return; }
        if(n-4>sizeof(a->rx)) { a->too_long++; return; }
        a->rx_header=p[0]; a->rx_left=(uint8_t)(p[1]-1); a->rx_profile=ar_u16(p+2);
        ar_copy(a->rx,p+4,n-4);
        a->rx_have=n-4;
        a->rx_active=true;
        return;
    }
    // A continue or an end: it must follow a start, carry the start's
    // label, C/R and IPID, and come where the count says (an end only as
    // the last packet), within BT_AVRCP_MESSAGE.
    if(!a->rx_active) { a->fragments_dropped++; return; }
    bool end=type==PKT_END;
    if((p[0]&0xf3)!=(a->rx_header&0xf3) || (end ? a->rx_left!=1 : a->rx_left<=1)) {
        a->fragments_dropped++; a->rx_active=false;
        return;
    }
    if(n-1>sizeof(a->rx)-a->rx_have) { a->too_long++; a->rx_active=false; return; }
    ar_copy(a->rx+a->rx_have,p+1,n-1);
    a->rx_have+=n-1;
    a->rx_left--;
    if(end) { a->rx_active=false; ar_message(a,a->rx_header,a->rx_profile,a->rx,a->rx_have); }
}

// A command of ours unanswered BT_AVRCP_REPLY_MS after it was sent has a
// second period; unanswered again, the controller has FAILED. A question
// or registration is queued again for its second period, the same label
// and bytes under a new serial (BlueZ's one retry), since asking twice
// gets the same answer. A Set is not: sent twice, its second copy would
// put back a level they had since changed with their own button, and say
// so only in an answer we could no longer take. It waits the second
// period out.
static void ar_timer(bt_avrcp_t *a, bt_avrcp_command_t *c)
{
    if(!c->live || !c->waiting || a->now<c->deadline) return;
    a->silences++;
    if(c->tries) { ar_controller_ends(a,BT_AVRCP_FAILED); return; }
    c->tries=1;
    if(c==&a->set) { c->deadline=a->now+BT_AVRCP_REPLY_MS; return; }
    c->sent=false; c->waiting=false;
    c->msg.id=ar_id(a);
}

void bt_avrcp_tick(bt_avrcp_t *a, uint64_t now)
{
    a->now=now;
    if(a->held && now>=a->held_until) { a->auto_released++; ar_let_go(a); }
    ar_timer(a,&a->reg);
    ar_timer(a,&a->set);
}

// The message to send now: the oldest answer owed, else a command of ours
// not yet sent; NULL when there is none. *c says which command, NULL for
// an answer.
static bt_avrcp_msg_t *ar_head(bt_avrcp_t *a, bt_avrcp_command_t **c)
{
    *c=0;
    if(a->answers) return &a->answer[0];
    if(a->reg.live && !a->reg.sent) { *c=&a->reg; return &a->reg.msg; }
    if(a->set.live && !a->set.sent) { *c=&a->set; return &a->set.msg; }
    return 0;
}

// The head is gone to the controller (or dropped as too long for the
// channel): an answer leaves the owed list; a command is sent and its
// timer starts. A Set is counted when it leaves (it leaves once), so one
// replaced while queued is not.
static void ar_commit(bt_avrcp_t *a, bt_avrcp_command_t *c)
{
    if(c) {
        if(c==&a->set) a->sets++;
        c->sent=true; c->waiting=true; c->deadline=a->now+BT_AVRCP_REPLY_MS;
        return;
    }
    for(unsigned i=1;i<a->answers;i++) a->answer[i-1]=a->answer[i];
    a->answers--;
    ar_wipe(&a->answer[a->answers],sizeof(a->answer[0]));
}

size_t bt_avrcp_next(bt_avrcp_t *a, uint8_t *out, size_t cap, uint16_t mtu, uint64_t now, uint32_t *serial)
{
    a->now=now;
    *serial=0;
    // Each pass sends or drops one message, and there are at most the
    // answers owed and our two commands.
    for(unsigned pass=0;pass<BT_AVRCP_ANSWERS+2u;pass++) {
        bt_avrcp_command_t *c;
        bt_avrcp_msg_t *m=ar_head(a,&c);
        if(!m) return 0;
        size_t n=m->bytes;
        if(n>mtu) { a->unsendable++; ar_commit(a,c); continue; }
        if(cap<n) return 0;
        ar_copy(out,m->pdu,n);
        *serial=m->id;
        return n;
    }
    return 0;
}

bool bt_avrcp_sent(bt_avrcp_t *a, uint32_t serial, uint64_t now)
{
    a->now=now;
    bt_avrcp_command_t *c;
    bt_avrcp_msg_t *m=ar_head(a,&c);
    if(!m || !serial || m->id!=serial) { a->stale_sends++; return false; }
    ar_commit(a,c);
    return true;
}

bt_avrcp_state_t bt_avrcp_state(const bt_avrcp_t *a) { return a->state; }

bool bt_avrcp_volume(const bt_avrcp_t *a, uint8_t *raw)
{
    if(a->state!=BT_AVRCP_READY) return false;
    *raw=a->volume;
    return true;
}

uint32_t bt_avrcp_volume_serial(const bt_avrcp_t *a) { return a->volume_serial; }

bool bt_avrcp_set_volume(bt_avrcp_t *a, uint8_t raw, uint64_t now)
{
    a->now=now;
    if(a->state!=BT_AVRCP_READY) return false;
    if(a->set.live && a->set.sent) { a->set_pending=true; a->set_wanted=raw; return true; }
    ar_send_set(a,raw);              // a Set still queued is replaced, under a label of its own
    return true;
}

bool bt_avrcp_press(bt_avrcp_t *a, bt_avrcp_press_t *out)
{
    if(!a->ring_count) return false;
    *out=a->ring[a->ring_head];
    a->ring_head=(uint8_t)((a->ring_head+1)%BT_AVRCP_PRESSES);
    a->ring_count--;
    return true;
}

// A change of status spends a standing registration with CHANGED, under
// the registration's label, in the slot kept for it.
void bt_avrcp_playback(bt_avrcp_t *a, uint8_t status, uint64_t now)
{
    a->now=now;
    if(status>BT_AVRCP_PAUSED || status==a->playback) return;
    a->playback=status;
    // Headphones may hold back their volume registration's answer until
    // something plays, and so be FAILED for silence alone: playing begins
    // the controller again, a bounded number of times a channel. PLAIN is
    // their own answer and stays.
    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {
        a->restarts++;
        ar_ask(a);
    }
    if(!a->play_registered) return;
    a->play_registered=false;
    if(a->answers>=BT_AVRCP_ANSWERS) { a->overrun++; return; }   // the kept slot makes this a guard, never a path
    bt_avrcp_msg_t *m=&a->answer[a->answers++];
    ar_begin(m,a->play_label,true,CT_CHANGED,a->play_subunit,OP_VENDOR);
    ar_vendor(m,PDU_REGISTER_NOTIFICATION,2);
    ar_put(m,EVENT_PLAYBACK); ar_put(m,status);
    m->id=ar_id(a);
}

const char *bt_avrcp_state_name(bt_avrcp_state_t s)
{
    static const char *const names[]={"idle","asking","registering","ready","plain","failed"};
    return (unsigned)s<sizeof(names)/sizeof(names[0]) ? names[s] : "?";
}

static const char *ar_playback_name(uint8_t s)
{
    return s==BT_AVRCP_PLAYING ? "playing" : s==BT_AVRCP_PAUSED ? "paused" : "stopped";
}

size_t bt_avrcp_status(const bt_avrcp_t *a, char *out, size_t cap)
{
    if(!cap) return 0;
    size_t n=0;
    int w;
    if(a->state==BT_AVRCP_READY)
        w=snprintf(out,cap,"avrcp: %s, their volume %u/127 (told %u times)\n",bt_avrcp_state_name(a->state),
                   (unsigned)a->volume,(unsigned)a->volume_serial);
    else w=snprintf(out,cap,"avrcp: %s, their volume not known\n",bt_avrcp_state_name(a->state));
    if(w>0) n+=(size_t)w;
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avrcp: sets %u (accepted %u, interim %u, rejected %u)%s, registrations %u, silences %u, restarts %u of %u\n",
                   (unsigned)a->sets,(unsigned)a->sets_accepted,(unsigned)a->sets_interim,(unsigned)a->sets_rejected,
                   a->set_pending ? ", one waiting" : "",(unsigned)a->registrations,(unsigned)a->silences,
                   (unsigned)a->restarts,(unsigned)BT_AVRCP_RESTARTS);
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avrcp: presses:");
        if(w>0) n+=(size_t)w;
        for(int i=0;i<7 && n<cap;i++) {
            w=snprintf(out+n,cap-n,"%s %s %u",i ? "," : "",ar_op_names[i],(unsigned)a->presses[i]);
            if(w>0) n+=(size_t)w;
        }
        if(n<cap) {
            w=snprintf(out+n,cap-n,"; held %s; auto-released %u, lost %u, unmatched releases %u, not taken %u\n",
                       a->held ? ar_op_names[ar_op_index(a->held)] : "nothing",(unsigned)a->auto_released,
                       (unsigned)a->presses_lost,(unsigned)a->unmatched_releases,(unsigned)a->not_taken);
            if(w>0) n+=(size_t)w;
        }
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avrcp: playback %s; they registered for playback %s, track %s; answered %u, rejected %u, not implemented %u\n",
                   ar_playback_name(a->playback),a->play_registered ? "yes" : "no",a->track_registered ? "yes" : "no",
                   (unsigned)a->answered,(unsigned)a->rejected,(unsigned)a->not_implemented);
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avrcp: strays %u, malformed %u, fragments dropped %u, too long %u, unsendable %u, stale sends %u, overrun %u, ignored %u, invalid profile %u\n",
                   (unsigned)a->strays,(unsigned)a->malformed,(unsigned)a->fragments_dropped,(unsigned)a->too_long,
                   (unsigned)a->unsendable,(unsigned)a->stale_sends,(unsigned)a->overrun,(unsigned)a->ignored,
                   (unsigned)a->invalid_profile);
        if(w>0) n+=(size_t)w;
    }
    return n<cap ? n : cap-1;
}
