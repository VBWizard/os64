// bt_sdp.c — the SDP client and responder. bt_sdp.h is the contract;
// docs/design/pending/AUDIO_CLASSIC.md §§ S2, S3 the design; the wire
// formats are Bluetooth Core Vol 3 Part B §§ 3 (data elements), 4.2 to 4.7
// (PDUs, continuation) and 5.1 (the universal attributes), and the A2DP
// specification's SDP record. Every multi-byte field here is BIG-endian:
// PDU lengths, transaction IDs, data-element sizes, and the UUIDs and
// integers inside data elements.
#include "driver/system/usb/bt_sdp.h"
#include "strings/sprintf.h"

#define SDP_ERROR_RSP      0x01
#define SDP_SEARCH_REQ     0x02
#define SDP_SEARCH_RSP     0x03
#define SDP_ATTR_REQ       0x04
#define SDP_ATTR_RSP       0x05
#define SDP_SEARCH_ATTR_REQ 0x06
#define SDP_SEARCH_ATTR_RSP 0x07

#define SDP_BAD_HANDLE     0x0002
#define SDP_BAD_SYNTAX     0x0003
#define SDP_BAD_SIZE       0x0004
#define SDP_BAD_CONTINUATION 0x0005

// The smallest MaximumAttributeByteCount a request may state (§ 4.6.1).
#define SDP_MIN_ATTR_BYTES 7
#define SDP_MAX_PATTERN    12       // UUIDs in a ServiceSearchPattern (§ 4.5.1)
#define SDP_NOT_16         0xffffffffu

enum { T_NIL, T_UINT, T_INT, T_UUID, T_TEXT, T_BOOL, T_SEQ, T_ALT, T_URL };

static uint16_t sdp_u16(const uint8_t *p) { return (uint16_t)(p[0]<<8 | p[1]); }
static uint32_t sdp_u32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void sdp_put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void sdp_copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; }
static void sdp_wipe(void *p, size_t n)
{ uint8_t *v=p; while(n--) *v++=0; }

// ── Data elements ───────────────────────────────────────────────────────────

typedef enum { EL_OK, EL_SHORT, EL_BAD } sdp_el_result_t;
typedef struct { uint8_t type, index; size_t data, size; } sdp_el_t;

// One element's header at p[at], read from bytes before `lim`: EL_SHORT
// when the header does not fit, EL_BAD for a descriptor § 3.2 and § 3.3 do
// not allow (a type with a size index it cannot have, or a reserved type).
// Whether its DATA fits is the caller's question: e->size may be anything.
static sdp_el_result_t sdp_header(const uint8_t *p, size_t at, size_t lim, sdp_el_t *e)
{
    if(at>=lim) return EL_SHORT;
    uint8_t type=p[at]>>3, index=p[at]&7;
    bool ok;
    switch(type) {
    case T_NIL: case T_BOOL: ok=index==0; break;
    case T_UINT: case T_INT: ok=index<=4; break;
    case T_UUID: ok=index==1 || index==2 || index==4; break;
    case T_TEXT: case T_SEQ: case T_ALT: case T_URL: ok=index>=5; break;
    default: ok=false; break;
    }
    if(!ok) return EL_BAD;
    size_t head=1, size;
    if(index<5) size=type==T_NIL ? 0 : (size_t)1<<index;
    else {
        size_t bytes=(size_t)1<<(index-5);
        if(lim-at-1<bytes) return EL_SHORT;
        size=0;
        for(size_t i=0;i<bytes;i++) size=size<<8 | p[at+1+i];
        head+=bytes;
    }
    if(head>lim-at) return EL_SHORT;
    e->type=type; e->index=index; e->data=at+head; e->size=size;
    return EL_OK;
}
// A whole element, header and data, before `lim`.
static bool sdp_element(const uint8_t *p, size_t at, size_t lim, sdp_el_t *e)
{ return sdp_header(p,at,lim,e)==EL_OK && e->size<=lim-e->data; }

static bool sdp_is_u16(const sdp_el_t *e) { return e->type==T_UINT && e->index==1; }

// A UUID as its 16-bit form, or SDP_NOT_16: a 32-bit UUID is a 16-bit one
// when its top half is zero, a 128-bit one when it is 0000xxxx on the
// Bluetooth base, 0000xxxx-0000-1000-8000-00805F9B34FB (§ 2.5.1).
static uint32_t sdp_uuid16(const uint8_t *p, const sdp_el_t *e)
{
    static const uint8_t base[12]={0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb};
    const uint8_t *v=p+e->data;
    if(e->size==2) return sdp_u16(v);
    if(e->size==4) return v[0] || v[1] ? SDP_NOT_16 : sdp_u16(v+2);
    if(v[0] || v[1]) return SDP_NOT_16;
    for(unsigned i=0;i<12;i++) if(v[4+i]!=base[i]) return SDP_NOT_16;
    return sdp_u16(v+2);
}

// ── The walker ──────────────────────────────────────────────────────────────

// What one record has shown so far.
typedef struct { bool match; uint16_t avdtp, a2dp, features, psm; bool features_present; } sdp_record_t;

static void sdp_commit(bt_sdp_sink_t *out, const sdp_record_t *r)
{
    if(out->found || !r->match) return;
    out->found=true;
    out->avdtp_version=r->avdtp; out->a2dp_version=r->a2dp; out->l2cap_psm=r->psm;
    out->features=r->features; out->features_present=r->features_present;
}

// Each element is read against the end its container declared (`end`) and
// the end of the bytes held (`bytes`). Running past the first is
// malformed; running past only the second is the cut a bound made. The
// open containers are an explicit stack of BT_SDP_DEPTH; a sequence or
// alternative met with the stack full is stepped over by its size. An
// element's meaning comes from where it sits: depth 0 is the list of records, 1 a
// record, 2 its attribute ID and value pairs, and below that the value of
// the attribute being read, in which a sequence whose first element is a
// UUID and whose second a uint16 is a descriptor (protocol, version or PSM).
bt_sdp_parse_t bt_sdp_parse_attribute_lists(const uint8_t *p, size_t bytes, uint16_t wanted,
                                            bool complete, bt_sdp_sink_t *out)
{
    size_t end[BT_SDP_DEPTH], at=0;
    unsigned count[BT_SDP_DEPTH];
    uint32_t first[BT_SDP_DEPTH];
    unsigned depth=0;
    bool outer_seen=false;
    uint16_t attribute=0;
    sdp_record_t rec={0};
    bt_sdp_parse_t verdict=BT_SDP_PARSED;
    sdp_wipe(out,sizeof(*out));
    for(;;) {
        while(depth && at==end[depth-1]) depth--;
        if(at>=bytes) {
            if(at>bytes || depth || !outer_seen) verdict=BT_SDP_TRUNCATED;
            break;
        }
        if(!depth && outer_seen) { verdict=BT_SDP_MALFORMED; break; }   // bytes after the list
        // The list itself is bounded by the bytes held when they are the
        // whole answer, and by nothing when they are a prefix of it.
        size_t lim=depth ? end[depth-1] : complete ? bytes : (size_t)-1;
        size_t avail=lim<bytes ? lim : bytes;
        sdp_el_t e;
        sdp_el_result_t r=sdp_header(p,at,avail,&e);
        if(r==EL_BAD || (r==EL_SHORT && avail==lim)) { verdict=BT_SDP_MALFORMED; break; }
        if(r==EL_SHORT) { verdict=BT_SDP_TRUNCATED; break; }
        if(e.size>lim-e.data) { verdict=BT_SDP_MALFORMED; break; }     // past its container
        bool container=e.type==T_SEQ || e.type==T_ALT;
        if(!container && e.size>bytes-e.data) { verdict=BT_SDP_TRUNCATED; break; }
        unsigned index=depth ? count[depth-1] : 0;
        if(depth) count[depth-1]++;
        if(depth==0 && e.type!=T_SEQ) { verdict=BT_SDP_MALFORMED; break; }
        if(depth==1) {
            if(e.type!=T_SEQ) { verdict=BT_SDP_MALFORMED; break; }
            sdp_commit(out,&rec);
            rec=(sdp_record_t){0};
        }
        if(depth==2 && !(index&1)) {
            if(!sdp_is_u16(&e)) { verdict=BT_SDP_MALFORMED; break; }
            attribute=sdp_u16(p+e.data);
        } else if(depth==2 && attribute==0x0311 && sdp_is_u16(&e)) {
            rec.features=sdp_u16(p+e.data); rec.features_present=true;
        } else if(depth>=3 && e.type==T_UUID) {
            uint32_t u=sdp_uuid16(p,&e);
            if(index==0) first[depth-1]=u;
            if(attribute==0x0001 && depth==3 && u==wanted) rec.match=true;
        } else if(depth>=3 && index==1 && sdp_is_u16(&e)) {
            uint16_t v=sdp_u16(p+e.data);
            if(attribute==0x0004 && first[depth-1]==0x0100 && !rec.psm) rec.psm=v;
            if(attribute==0x0004 && first[depth-1]==0x0019 && !rec.avdtp) rec.avdtp=v;
            if(attribute==0x0009 && first[depth-1]==0x110D && !rec.a2dp) rec.a2dp=v;
        }
        if(container && depth<BT_SDP_DEPTH) {
            end[depth]=e.data+e.size; count[depth]=0; first[depth]=SDP_NOT_16;
            depth++;
            at=e.data;
            if(depth==1) outer_seen=true;
        } else at=e.data+e.size;
    }
    sdp_commit(out,&rec);
    if(verdict==BT_SDP_MALFORMED || (complete && verdict!=BT_SDP_PARSED)) sdp_wipe(out,sizeof(*out));
    return verdict;
}

// ── The client ──────────────────────────────────────────────────────────────

void bt_sdp_client_start(bt_sdp_client_t *c, uint64_t now, uint16_t mtu)
{
    sdp_wipe(c,sizeof(*c));
    c->state=BT_SDP_ASKING; c->started=c->now=now; c->owed=true;
    // A response is its 5-byte header, the 2-byte count, the attribute
    // bytes and a continuation state of up to 17 bytes.
    c->max_bytes=mtu>24+SDP_MIN_ATTR_BYTES ? (uint16_t)(mtu-24) : SDP_MIN_ATTR_BYTES;
    c->why="asking";
}

size_t bt_sdp_client_request(bt_sdp_client_t *c, uint8_t *out, size_t cap)
{
    size_t n=20u+c->cont_len;
    if(c->state!=BT_SDP_ASKING || !c->owed || cap<n) return 0;
    c->tid++;
    out[0]=SDP_SEARCH_ATTR_REQ; sdp_put16(out+1,c->tid); sdp_put16(out+3,(uint16_t)(n-5));
    static const uint8_t pattern[5]={0x35,0x03,0x19,BT_SDP_UUID_AUDIO_SINK>>8,BT_SDP_UUID_AUDIO_SINK&0xff};
    static const uint8_t range[7]={0x35,0x05,0x0a,0x00,0x00,0xff,0xff};
    sdp_copy(out+5,pattern,5);
    sdp_put16(out+10,c->max_bytes);
    sdp_copy(out+12,range,7);
    out[19]=c->cont_len;
    sdp_copy(out+20,c->cont,c->cont_len);
    c->owed=false; c->awaiting=true;
    return n;
}

static void sdp_fail(bt_sdp_client_t *c, const char *why, bool malformed)
{
    if(malformed) c->malformed++;
    c->state=BT_SDP_FAILED; c->why=why; c->owed=c->awaiting=false;
    sdp_wipe(&c->result,sizeof(c->result));
}

// The answer as held, read whole (complete) or as far as it goes.
static void sdp_finish(bt_sdp_client_t *c, bool complete, const char *why)
{
    bt_sdp_parse_t r=bt_sdp_parse_attribute_lists(c->buf,c->held,BT_SDP_UUID_AUDIO_SINK,complete,&c->result);
    if(complete && r!=BT_SDP_PARSED) { sdp_fail(c,"the answer is not a well-formed attribute list",true); return; }
    c->result.complete=complete;
    c->state=BT_SDP_DONE; c->why=why; c->owed=c->awaiting=false;
}

void bt_sdp_client_receive(bt_sdp_client_t *c, const uint8_t *p, size_t n)
{
    if(c->state!=BT_SDP_ASKING) return;
    if(!c->awaiting) { sdp_fail(c,"a response nobody asked for",false); return; }
    if(n<5 || sdp_u16(p+3)!=n-5) { sdp_fail(c,"a PDU whose length is wrong",true); return; }
    if(sdp_u16(p+1)!=c->tid) { sdp_fail(c,"a response to another transaction",false); return; }
    if(p[0]==SDP_ERROR_RSP) {
        c->error=n>=7 ? sdp_u16(p+5) : 0;
        sdp_fail(c,"the peer answered with an error",false);
        return;
    }
    if(p[0]!=SDP_SEARCH_ATTR_RSP) { sdp_fail(c,"a response of another kind",true); return; }
    // AttributeListsByteCount, the bytes, the continuation state: all of
    // the PDU and nothing past it.
    size_t plen=n-5;
    const uint8_t *q=p+5;
    if(plen<3) { sdp_fail(c,"a response too short",true); return; }
    size_t count=sdp_u16(q);
    if(count>plen-3) { sdp_fail(c,"a byte count past the PDU",true); return; }
    uint8_t cont=q[2+count];
    if(cont>BT_SDP_CONTINUATION || plen!=3u+count+cont) { sdp_fail(c,"a continuation state of the wrong length",true); return; }
    c->awaiting=false;
    c->rounds++;
    size_t room=BT_SDP_BUFFER-c->held, take=count<room ? count : room;
    sdp_copy(c->buf+c->held,q+2,take);
    c->held=(uint16_t)(c->held+take);
    if(take<count) { sdp_finish(c,false,"the answer is longer than the buffer"); return; }
    if(!cont) { sdp_finish(c,true,"answered"); return; }
    if(c->rounds>=BT_SDP_ROUNDS) { sdp_finish(c,false,"the answer took too many responses"); return; }
    c->cont_len=cont;
    sdp_copy(c->cont,q+3+count,cont);
    c->owed=true;
}

void bt_sdp_client_tick(bt_sdp_client_t *c, uint64_t now)
{
    c->now=now;
    if(c->state==BT_SDP_ASKING && now-c->started>=BT_SDP_LIMIT_MS) sdp_finish(c,false,"the answer took too long");
}

bt_sdp_state_t bt_sdp_client_state(const bt_sdp_client_t *c) { return c->state; }
const bt_sdp_sink_t *bt_sdp_client_result(const bt_sdp_client_t *c) { return &c->result; }

size_t bt_sdp_client_status(const bt_sdp_client_t *c, char *out, size_t cap)
{
    const bt_sdp_sink_t *r=&c->result;
    int n;
    if(c->state==BT_SDP_IDLE) n=snprintf(out,cap,"sdp: not asked\n");
    else if(c->state==BT_SDP_ASKING) n=snprintf(out,cap,"sdp: asking (%u responses, %u bytes)\n",(unsigned)c->rounds,(unsigned)c->held);
    else if(c->state==BT_SDP_FAILED)
        n=snprintf(out,cap,"sdp: failed: %s (error %04x, malformed %u)\n",c->why,c->error,(unsigned)c->malformed);
    else if(!r->found) n=snprintf(out,cap,"sdp: no audio sink (%s)\n",r->complete ? "complete" : c->why);
    else {
        char features[24];
        if(r->features_present) snprintf(features,sizeof(features),"0x%04x",r->features);
        else snprintf(features,sizeof(features),"absent");
        n=snprintf(out,cap,"sdp: audio sink, avdtp %u.%u, a2dp %u.%u, features %s, psm %04x (%s)\n",
                   r->avdtp_version>>8,r->avdtp_version&0xff,r->a2dp_version>>8,r->a2dp_version&0xff,
                   features,r->l2cap_psm,r->complete ? "complete" : c->why);
    }
    return n<0 || !cap ? 0 : (size_t)n<cap ? (size_t)n : cap-1;
}

// ── The responder ───────────────────────────────────────────────────────────

// os64's records (AUDIO_CLASSIC.md § S3, AUDIO_REMOTE.md § What we say we
// are), in ascending handle order: the Audio Source; the AV Remote Control
// Target, which takes their buttons; the AV Remote Control (with its
// Controller class), which sets their volume. Each holds the same six
// attributes in ascending ID order, each value one data element. The two
// AVRCP records are Android's (stack/avrc/avrc_sdp.cc) for AVRCP 1.4 without
// browsing: AVCTP's version in the protocol list is the profile's minus one,
// 1.3; the Controller class follows AV Remote Control from 1.4 on.
#define SDP_RECORD_ATTRIBUTES 6
#define SDP_VALUE_MAX         18
typedef struct { uint16_t id; uint8_t bytes; uint8_t value[SDP_VALUE_MAX]; } sdp_attribute_t;
typedef struct { uint32_t handle; sdp_attribute_t attribute[SDP_RECORD_ATTRIBUTES]; } sdp_service_t;
static const sdp_service_t sdp_records[]={
  { BT_SDP_RECORD_HANDLE, {
    {0x0000,5,{0x0a,0x00,0x01,0x00,0x01}},                                    // ServiceRecordHandle
    {0x0001,5,{0x35,0x03,0x19,0x11,0x0a}},                                    // ServiceClassIDList: Audio Source
    {0x0004,18,{0x35,0x10,0x35,0x06,0x19,0x01,0x00,0x09,0x00,0x19,            // ProtocolDescriptorList: L2CAP, PSM AVDTP;
                0x35,0x06,0x19,0x00,0x19,0x09,0x01,0x03}},                    //   AVDTP 1.3
    {0x0005,5,{0x35,0x03,0x19,0x10,0x02}},                                    // BrowseGroupList: PublicBrowseRoot
    {0x0009,10,{0x35,0x08,0x35,0x06,0x19,0x11,0x0d,0x09,0x01,0x03}},          // BluetoothProfileDescriptorList: A2DP 1.3
    {0x0311,3,{0x09,0x00,0x01}},                                              // SupportedFeatures: player
  } },
  { BT_SDP_RECORD_HANDLE_TARGET, {
    {0x0000,5,{0x0a,0x00,0x01,0x00,0x02}},                                    // ServiceRecordHandle
    {0x0001,5,{0x35,0x03,0x19,0x11,0x0c}},                                    // ServiceClassIDList: AV Remote Control Target
    {0x0004,18,{0x35,0x10,0x35,0x06,0x19,0x01,0x00,0x09,0x00,0x17,            // ProtocolDescriptorList: L2CAP, PSM AVCTP;
                0x35,0x06,0x19,0x00,0x17,0x09,0x01,0x03}},                    //   AVCTP 1.3 (the target's)
    {0x0005,5,{0x35,0x03,0x19,0x10,0x02}},                                    // BrowseGroupList: PublicBrowseRoot (the target's)
    {0x0009,10,{0x35,0x08,0x35,0x06,0x19,0x11,0x0e,0x09,0x01,0x04}},          // BluetoothProfileDescriptorList: AVRCP 1.4 (the target's)
    {0x0311,3,{0x09,0x00,0x03}},                                              // SupportedFeatures: category 1 (player) and 2 (amplifier)
  } },
  { BT_SDP_RECORD_HANDLE_CONTROLLER, {
    {0x0000,5,{0x0a,0x00,0x01,0x00,0x03}},                                    // ServiceRecordHandle
    {0x0001,8,{0x35,0x06,0x19,0x11,0x0e,0x19,0x11,0x0f}},                     // ServiceClassIDList: AV Remote Control, its Controller
    {0x0004,18,{0x35,0x10,0x35,0x06,0x19,0x01,0x00,0x09,0x00,0x17,            // ProtocolDescriptorList: L2CAP, PSM AVCTP;
                0x35,0x06,0x19,0x00,0x17,0x09,0x01,0x03}},                    //   AVCTP 1.3 (the controller's)
    {0x0005,5,{0x35,0x03,0x19,0x10,0x02}},                                    // BrowseGroupList: PublicBrowseRoot (the controller's)
    {0x0009,10,{0x35,0x08,0x35,0x06,0x19,0x11,0x0e,0x09,0x01,0x04}},          // BluetoothProfileDescriptorList: AVRCP 1.4 (the controller's)
    {0x0311,3,{0x09,0x00,0x02}},                                              // SupportedFeatures: category 2 (the volume's controller)
  } },
};
#define SDP_RECORDS (sizeof(sdp_records)/sizeof(sdp_records[0]))
// The whole answer is built before it is cut into parts: an attribute
// list's header is at most 3 bytes for these sizes, and so is the list of
// lists' around three of them.
#define SDP_RECORDS_MAX 3
#define SDP_LIST_MAX  (3 + SDP_RECORD_ATTRIBUTES*(3 + SDP_VALUE_MAX))
#define SDP_WHOLE_MAX (3 + SDP_RECORDS_MAX*SDP_LIST_MAX)
_Static_assert(SDP_RECORDS <= SDP_RECORDS_MAX, "the answer buffer holds every record");
_Static_assert(SDP_WHOLE_MAX <= 0xffff, "a continuation offset is 16 bits");

// Does the record hold this UUID (in its 16-bit form) anywhere? Every
// element of our own record is visited by stepping into each sequence.
static bool sdp_record_has(const sdp_service_t *r, uint32_t uuid)
{
    for(unsigned a=0;a<SDP_RECORD_ATTRIBUTES;a++) {
        const sdp_attribute_t *at=&r->attribute[a];
        sdp_el_t e;
        for(size_t i=0;i<at->bytes && sdp_element(at->value,i,at->bytes,&e);) {
            if(e.type==T_UUID && sdp_uuid16(at->value,&e)==uuid) return true;
            i=e.type==T_SEQ || e.type==T_ALT ? e.data : e.data+e.size;
        }
    }
    return false;
}

static size_t sdp_error(const uint8_t *req, uint8_t *out, size_t cap, uint16_t code)
{
    if(cap<7) return 0;
    out[0]=SDP_ERROR_RSP; out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,2); sdp_put16(out+5,code);
    return 7;
}

// A sequence header for `n` bytes of contents.
static size_t sdp_seq(uint8_t *out, size_t n)
{
    if(n<256) { out[0]=0x35; out[1]=(uint8_t)n; return 2; }
    out[0]=0x36; sdp_put16(out+1,(uint16_t)n); return 3;
}

// The continuation state we issue: the offset the next part starts at and
// a check of it against the request it continues, so a state that does
// not match its request (damaged, or moved to another one) is refused. The
// check is not a secret: a peer can compute one for any offset, and gets
// what that offset of our own records holds. The offset is a byte of the
// whole answer for the attribute requests and a handle's place in the list
// for a Service Search.
#define SDP_STATE_BYTES 6
static uint32_t sdp_check(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t pdu, uint16_t offset)
{
    uint32_t h=2166136261u;
    h=(h^pdu)*16777619u; h=(h^(offset>>8))*16777619u; h=(h^(offset&0xff))*16777619u;
    for(size_t i=0;i<an;i++) h=(h^a[i])*16777619u;
    for(size_t i=0;i<bn;i++) h=(h^b[i])*16777619u;
    return h;
}

// A ServiceSearchPattern: a sequence of 1 to 12 UUIDs. True when it is
// well formed; match[r] when record r holds every one.
static bool sdp_pattern(const uint8_t *p, size_t at, size_t lim, size_t *next, bool match[SDP_RECORDS])
{
    sdp_el_t s, e;
    if(!sdp_element(p,at,lim,&s) || s.type!=T_SEQ) return false;
    unsigned n=0;
    for(unsigned r=0;r<SDP_RECORDS;r++) match[r]=true;
    for(size_t i=s.data;i<s.data+s.size;i=e.data+e.size) {
        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UUID || ++n>SDP_MAX_PATTERN) return false;
        uint32_t u=sdp_uuid16(p,&e);
        for(unsigned r=0;r<SDP_RECORDS;r++)
            if(u==SDP_NOT_16 || !sdp_record_has(&sdp_records[r],u)) match[r]=false;
    }
    *next=s.data+s.size;
    return n>0;
}

// An AttributeIDList: a sequence of uint16 IDs and uint32 ranges (high
// half the first ID, low half the last). Each record's attributes any of
// them names are marked; true when it is well formed.
static bool sdp_attributes(const uint8_t *p, size_t at, size_t lim, size_t *next,
                           bool wanted[SDP_RECORDS][SDP_RECORD_ATTRIBUTES])
{
    sdp_el_t s, e;
    if(!sdp_element(p,at,lim,&s) || s.type!=T_SEQ || !s.size) return false;
    for(unsigned r=0;r<SDP_RECORDS;r++) for(unsigned a=0;a<SDP_RECORD_ATTRIBUTES;a++) wanted[r][a]=false;
    for(size_t i=s.data;i<s.data+s.size;i=e.data+e.size) {
        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UINT || (e.index!=1 && e.index!=2)) return false;
        uint16_t lo, hi;
        if(e.index==1) lo=hi=sdp_u16(p+e.data);
        else { lo=sdp_u16(p+e.data); hi=sdp_u16(p+e.data+2); }
        if(lo>hi) return false;
        for(unsigned r=0;r<SDP_RECORDS;r++) for(unsigned a=0;a<SDP_RECORD_ATTRIBUTES;a++) {
            uint16_t id=sdp_records[r].attribute[a].id;
            if(id>=lo && id<=hi) wanted[r][a]=true;
        }
    }
    *next=s.data+s.size;
    return true;
}

// One record's attribute list for the attributes marked: the length of
// its contents (sdp_list_contents), its whole length with the sequence's
// header (sdp_list_bytes), and the list itself written at `out`, its
// length returned (sdp_list_put).
static size_t sdp_list_contents(const sdp_service_t *r, const bool wanted[SDP_RECORD_ATTRIBUTES])
{
    size_t n=0;
    for(unsigned a=0;a<SDP_RECORD_ATTRIBUTES;a++) if(wanted[a]) n+=3u+r->attribute[a].bytes;
    return n;
}
static size_t sdp_list_bytes(const sdp_service_t *r, const bool wanted[SDP_RECORD_ATTRIBUTES])
{
    size_t n=sdp_list_contents(r,wanted);
    return n+(n<256 ? 2 : 3);
}
static size_t sdp_list_put(uint8_t *out, const sdp_service_t *r, const bool wanted[SDP_RECORD_ATTRIBUTES])
{
    size_t o=sdp_seq(out,sdp_list_contents(r,wanted));
    for(unsigned a=0;a<SDP_RECORD_ATTRIBUTES;a++) if(wanted[a]) {
        const sdp_attribute_t *at=&r->attribute[a];
        out[o]=0x09; sdp_put16(out+o+1,at->id);
        sdp_copy(out+o+3,at->value,at->bytes);
        o+=3u+at->bytes;
    }
    return o;
}

// A continuation state of ours, checked: the offset it names, or 0 when it
// is not one we issued for this request (an offset of 0 is never issued).
static uint16_t sdp_state(const uint8_t *s, uint8_t cont, size_t whole, const uint8_t *key, size_t key_bytes,
                          const uint8_t *list, size_t list_bytes, uint8_t pdu)
{
    if(cont!=SDP_STATE_BYTES) return 0;
    uint16_t offset=sdp_u16(s);
    if(!offset || offset>=whole || sdp_u32(s+2)!=sdp_check(key,key_bytes,list,list_bytes,pdu,offset)) return 0;
    return offset;
}
static void sdp_put_state(uint8_t *out, const uint8_t *key, size_t key_bytes, const uint8_t *list, size_t list_bytes,
                          uint8_t pdu, uint16_t following)
{
    out[0]=SDP_STATE_BYTES;
    sdp_put16(out+1,following);
    uint32_t check=sdp_check(key,key_bytes,list,list_bytes,pdu,following);
    sdp_put16(out+3,(uint16_t)(check>>16)); sdp_put16(out+5,(uint16_t)check);
}

size_t bt_sdp_respond(const uint8_t *req, size_t n, uint8_t *out, size_t cap)
{
    if(n<3) return 0;                       // not even a transaction ID to answer under
    if(n<5 || sdp_u16(req+3)!=n-5) return sdp_error(req,out,cap,SDP_BAD_SIZE);
    uint8_t pdu=req[0];
    if(pdu!=SDP_SEARCH_REQ && pdu!=SDP_ATTR_REQ && pdu!=SDP_SEARCH_ATTR_REQ) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    size_t at=5, next;
    bool match[SDP_RECORDS]={0}, wanted[SDP_RECORDS][SDP_RECORD_ATTRIBUTES];
    uint16_t max;
    const uint8_t *key=req+5;               // what the continuation state is checked against: the parameters
    size_t key_bytes;                       // before the maximum, and the attribute list after it
    if(pdu==SDP_SEARCH_REQ || pdu==SDP_SEARCH_ATTR_REQ) {
        if(!sdp_pattern(req,at,n,&next,match)) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    } else {
        if(n-at<4) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
        next=at+4;
    }
    key_bytes=next-at;
    at=next;
    if(n-at<2) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    max=sdp_u16(req+at);
    at+=2;
    size_t list_at=at;
    if(pdu!=SDP_SEARCH_REQ && !sdp_attributes(req,at,n,&at,wanted)) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    if(n-at<1) return sdp_error(req,out,cap,SDP_BAD_SIZE);
    uint8_t cont=req[at];
    if(cont>BT_SDP_CONTINUATION) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    if(n-at-1!=cont) return sdp_error(req,out,cap,SDP_BAD_SIZE);
    // A ServiceAttributeRequest names one of our records by its handle.
    const sdp_service_t *named=0;
    if(pdu==SDP_ATTR_REQ) {
        uint32_t handle=sdp_u32(req+5);
        for(unsigned r=0;r<SDP_RECORDS;r++) if(sdp_records[r].handle==handle) named=&sdp_records[r];
        if(!named) return sdp_error(req,out,cap,SDP_BAD_HANDLE);
    }
    if(pdu==SDP_SEARCH_REQ ? !max : max<SDP_MIN_ATTR_BYTES) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);
    const uint8_t *list=req+list_at, *state=req+at+1;
    size_t list_bytes=at-list_at;

    if(pdu==SDP_SEARCH_REQ) {
        // The handles of the records matched, at most `max` of them. A
        // response carries as many as the room allows, and the rest follow
        // with continuation; the state names the next handle's place.
        uint32_t handles[SDP_RECORDS];
        size_t total=0;
        for(unsigned r=0;r<SDP_RECORDS;r++) if(match[r] && total<max) handles[total++]=sdp_records[r].handle;
        uint16_t from=0;
        if(cont && !(from=sdp_state(state,cont,total,key,key_bytes,list,list_bytes,pdu)))
            return sdp_error(req,out,cap,SDP_BAD_CONTINUATION);
        // Header, both counts and the state's length byte come first.
        size_t part=total-from;
        if(cap<10+4*part) {
            if(cap<10+SDP_STATE_BYTES+4) return 0;
            part=(cap-10-SDP_STATE_BYTES)/4;
        }
        bool more=from+part<total;
        size_t len=9+4*part+1+(more ? SDP_STATE_BYTES : 0);
        out[0]=SDP_SEARCH_RSP; out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,(uint16_t)(len-5));
        sdp_put16(out+5,(uint16_t)total); sdp_put16(out+7,(uint16_t)part);
        for(size_t i=0;i<part;i++) {
            sdp_put16(out+9+4*i,(uint16_t)(handles[from+i]>>16)); sdp_put16(out+11+4*i,(uint16_t)handles[from+i]);
        }
        if(more) sdp_put_state(out+9+4*part,key,key_bytes,list,list_bytes,pdu,(uint16_t)(from+part));
        else out[9+4*part]=0;
        return len;
    }

    // The whole answer, then the part of it this response carries: the
    // named record's attribute list, or a list of one list per record
    // matched (an empty list when none is).
    uint8_t whole[SDP_WHOLE_MAX];
    size_t w=0;
    if(named) w=sdp_list_put(whole,named,wanted[named-sdp_records]);
    else {
        size_t inner=0;
        for(unsigned r=0;r<SDP_RECORDS;r++) if(match[r]) inner+=sdp_list_bytes(&sdp_records[r],wanted[r]);
        w=sdp_seq(whole,inner);
        for(unsigned r=0;r<SDP_RECORDS;r++) if(match[r]) w+=sdp_list_put(whole+w,&sdp_records[r],wanted[r]);
    }
    uint16_t offset=0;
    if(cont && !(offset=sdp_state(state,cont,w,key,key_bytes,list,list_bytes,pdu)))
        return sdp_error(req,out,cap,SDP_BAD_CONTINUATION);
    // Header, byte count and the longest continuation state come first.
    if(cap<5+2+1+SDP_STATE_BYTES+1) return 0;
    size_t part=w-offset, room=cap-(5+2+1+SDP_STATE_BYTES);
    if(part>max) part=max;
    if(part>room) part=room;
    bool more=offset+part<w;
    size_t len=5+2+part+1+(more ? SDP_STATE_BYTES : 0);
    out[0]=pdu==SDP_SEARCH_ATTR_REQ ? SDP_SEARCH_ATTR_RSP : SDP_ATTR_RSP;
    out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,(uint16_t)(len-5));
    sdp_put16(out+5,(uint16_t)part);
    sdp_copy(out+7,whole+offset,part);
    if(more) sdp_put_state(out+7+part,key,key_bytes,list,list_bytes,pdu,(uint16_t)(offset+part));
    else out[7+part]=0;
    return len;
}
