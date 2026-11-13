// bt_l2cap.c — Classic L2CAP in Basic mode. bt_l2cap.h is the contract;
// docs/design/pending/AUDIO_CLASSIC.md § S1 the design; the wire formats
// are Bluetooth Core Vol 3 Part A §§ 3 (frames), 4 (signalling) and 5
// (configuration options), and Vol 4 Part E § 5.4.2 (the ACL packet).
// Every multi-byte field here is LITTLE-endian, the ACL header included.
#include "driver/system/usb/bt_l2cap.h"
#include "strings/sprintf.h"

#define L2_RTX_MS      2000
#define L2_ERTX_MS     60000
#define L2_ROUNDS      4
#define L2_MIN_MTU     48
#define L2_DEFAULT_MTU 672          // a peer that states no MTU takes this much (§ 5.1)
#define L2_CID_SIGNAL  0x0001
#define L2_CID_DYNAMIC 0x0040
#define L2_KEPT        6            // bytes of an oversized PDU kept: its header and first command's code and identifier

enum {
    L2_REJECT=0x01, L2_CONN_REQ, L2_CONN_RSP, L2_CONF_REQ, L2_CONF_RSP, L2_DISC_REQ, L2_DISC_RSP,
    L2_ECHO_REQ, L2_ECHO_RSP, L2_INFO_REQ, L2_INFO_RSP
};

static uint16_t l2_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1]<<8); }
static void l2_put(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void l2_copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; }
static void l2_wipe(void *p, size_t n)
{ uint8_t *v=p; while(n--) *v++=0; }

// § 4.2: a PSM's least significant octet is odd and its most significant
// octet even.
static bool l2_psm_valid(uint16_t psm) { return (psm & 0x0101)==0x0001; }

static bool l2_open(const bt_l2cap_channel_t *c)
{ return c->state==BT_L2CAP_CONFIG && c->ours_done && c->peer_done; }

static uint8_t l2_ident(bt_l2cap_t *l)
{
    uint8_t id=l->next_ident;
    l->next_ident=id==255 ? 1 : (uint8_t)(id+1);
    return id;
}

// ── The signalling queue ────────────────────────────────────────────────────

static bt_l2cap_signal_t *l2_sig_at(bt_l2cap_t *l, unsigned i)
{ return &l->sig[(l->sig_head+i)%BT_L2CAP_SIGNALS]; }

// Requests still waiting for a turn on behalf of a channel are taken out
// when the channel is freed or makes a newer request: nothing stale is
// sent. Answers stay; the peer is owed them.
static void l2_sig_purge(bt_l2cap_t *l, unsigned ch)
{
    unsigned kept=0;
    for(unsigned i=0;i<l->sig_count;i++) {
        bt_l2cap_signal_t *s=l2_sig_at(l,i);
        if(s->request && s->channel==ch) continue;
        if(kept!=i) *l2_sig_at(l,kept)=*s;
        kept++;
    }
    l->sig_count=kept;
}

// One command as a whole C-frame. A full queue drops it, newest first; a
// dropped request's timer starts at once, so the channel is recovered by
// RTX as if the request had been lost on the air.
static void l2_signal(bt_l2cap_t *l, int ch, uint8_t code, uint8_t ident, const uint8_t *data, size_t n)
{
    bool request=code==L2_CONN_REQ || code==L2_CONF_REQ || code==L2_DISC_REQ;
    if(n>BT_L2CAP_SIGNAL_BYTES-8) n=BT_L2CAP_SIGNAL_BYTES-8;
    if(l->sig_count==BT_L2CAP_SIGNALS) {
        l->queue_full++;
        if(request && ch>=0) { l->ch[ch].timer=true; l->ch[ch].deadline=l->now+L2_RTX_MS; }
        return;
    }
    bt_l2cap_signal_t *s=l2_sig_at(l,l->sig_count++);
    s->bytes=(uint8_t)(8+n); s->channel=ch<0 ? 0xff : (uint8_t)ch; s->ident=ident; s->request=request;
    l2_put(s->pdu,(uint16_t)(4+n)); l2_put(s->pdu+2,L2_CID_SIGNAL);
    s->pdu[4]=code; s->pdu[5]=ident; l2_put(s->pdu+6,(uint16_t)n);
    l2_copy(s->pdu+8,data,n);
}

static void l2_reject(bt_l2cap_t *l, uint8_t ident, uint16_t reason, uint16_t a, uint16_t b, unsigned words)
{
    uint8_t d[6];
    l2_put(d,reason); l2_put(d+2,a); l2_put(d+4,b);
    l2_signal(l,-1,L2_REJECT,ident,d,2+2*words);
}
static void l2_not_understood(bt_l2cap_t *l, uint8_t ident)
{ l->malformed++; l2_reject(l,ident,0x0000,0,0,0); }

// ── Channels ────────────────────────────────────────────────────────────────

static void l2_free(bt_l2cap_t *l, unsigned i)
{
    l2_sig_purge(l,i);
    l2_wipe(&l->ch[i],sizeof(l->ch[i]));
}

// A request of ours on channel i: it replaces any older one still queued,
// and its timer starts when it is handed to the controller.
static void l2_request(bt_l2cap_t *l, unsigned i, uint8_t code, const uint8_t *data, size_t n)
{
    bt_l2cap_channel_t *c=&l->ch[i];
    l2_sig_purge(l,i);
    c->ident=l2_ident(l); c->timer=false;
    l2_signal(l,(int)i,code,c->ident,data,n);
}

static void l2_send_config(bt_l2cap_t *l, unsigned i)
{
    bt_l2cap_channel_t *c=&l->ch[i];
    uint8_t d[8];
    l2_put(d,c->dcid); l2_put(d+2,0);
    d[4]=0x01; d[5]=2; l2_put(d+6,c->our_mtu);
    c->our_rounds++;
    l2_request(l,i,L2_CONF_REQ,d,sizeof(d));
}

// A connected channel is asked to disconnect and held until it is
// answered or its timer runs out; one not yet connected is simply freed.
static void l2_close(bt_l2cap_t *l, unsigned i)
{
    bt_l2cap_channel_t *c=&l->ch[i];
    if(c->state!=BT_L2CAP_CONFIG) { l2_free(l,i); return; }
    uint8_t d[4];
    l2_put(d,c->dcid); l2_put(d+2,c->cid);
    c->state=BT_L2CAP_DISCONNECTING; c->queued=false;
    l2_request(l,i,L2_DISC_REQ,d,sizeof(d));
}

static int l2_free_channel(const bt_l2cap_t *l)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l->ch[i].state==BT_L2CAP_FREE) return (int)i;
    return -1;
}

static bool l2_cid_used(const bt_l2cap_t *l, uint16_t cid)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l->ch[i].state!=BT_L2CAP_FREE && l->ch[i].cid==cid) return true;
    return false;
}
// Our CIDs rotate from 0x0040 up, so a late packet for a channel just
// freed does not land on the next one.
static uint16_t l2_new_cid(bt_l2cap_t *l)
{
    uint16_t cid;
    do {
        cid=l->next_cid;
        l->next_cid=cid==0xffff ? L2_CID_DYNAMIC : (uint16_t)(cid+1);
    } while(l2_cid_used(l,cid));
    return cid;
}

// The channel our outstanding request with this identifier belongs to.
static int l2_by_ident(const bt_l2cap_t *l, uint8_t ident, uint8_t state)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++)
        if(ident && l->ch[i].ident==ident && l->ch[i].state==state) return (int)i;
    return -1;
}

static bool l2_admitted(const bt_l2cap_t *l, uint16_t psm)
{
    for(unsigned i=0;i<BT_L2CAP_ADMITTED;i++) if(psm && l->admitted[i]==psm) return true;
    return false;
}

// ── Signalling commands ─────────────────────────────────────────────────────

static void l2_conn_req(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n!=4) { l2_not_understood(l,ident); return; }
    uint16_t psm=l2_u16(d), scid=l2_u16(d+2), result;
    int i=-1;
    bool scid_taken=false;
    for(unsigned k=0;k<BT_L2CAP_CHANNELS;k++)
        if(l->ch[k].state!=BT_L2CAP_FREE && l->ch[k].dcid==scid) scid_taken=true;
    if(!l2_psm_valid(psm) || !l2_admitted(l,psm)) result=0x0002;
    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) result=0x0003;
    else if(scid<L2_CID_DYNAMIC) result=0x0006;
    else if(scid_taken) result=0x0007;
    else if((i=l2_free_channel(l))<0) result=0x0004;
    else result=0x0000;
    uint8_t r[8];
    uint16_t cid=0;
    if(!result) {
        bt_l2cap_channel_t *c=&l->ch[i];
        l2_wipe(c,sizeof(*c));
        c->state=BT_L2CAP_CONFIG; c->psm=psm; c->cid=cid=l2_new_cid(l); c->dcid=scid;
        c->our_mtu=BT_L2CAP_MTU;
    }
    l2_put(r,cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,0);
    l2_signal(l,-1,L2_CONN_RSP,ident,r,sizeof(r));
    if(!result) l2_send_config(l,(unsigned)i);
}

static void l2_conn_rsp(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n!=8) { l2_not_understood(l,ident); return; }
    uint16_t dcid=l2_u16(d), scid=l2_u16(d+2), result=l2_u16(d+4);
    int i=l2_by_ident(l,ident,BT_L2CAP_CONNECTING);
    if(i<0 || l->ch[i].cid!=scid) { l->strays++; return; }
    bt_l2cap_channel_t *c=&l->ch[i];
    if(result==0x0001) { c->timer=true; c->deadline=l->now+L2_ERTX_MS; return; }
    if(result==0x0000 && dcid>=L2_CID_DYNAMIC) {
        c->state=BT_L2CAP_CONFIG; c->dcid=dcid; c->ident=0; c->timer=false;
        l2_send_config(l,(unsigned)i);
        return;
    }
    if(result==0x0000) { l->malformed++; result=0xfffd; }     // success naming a CID no channel may have
    l->refused=result; l->refused_psm=c->psm;
    l2_free(l,(unsigned)i);
}

// The peer's Configuration Request. Each request is judged on its own
// options: a request carrying the continuation flag is answered with an
// empty success carrying the flag (or with the failure its own options
// earn), and the peer's direction is configured when a request WITHOUT the
// flag succeeds. Options split across two requests are therefore not
// joined; an option must arrive whole.
static void l2_conf_req(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n<4) { l2_not_understood(l,ident); return; }
    uint16_t dcid=l2_u16(d), flags=l2_u16(d+2);
    int i=-1;
    for(unsigned k=0;k<BT_L2CAP_CHANNELS;k++)
        if(l->ch[k].state==BT_L2CAP_CONFIG && l->ch[k].cid==dcid) i=(int)k;
    if(i<0) { l2_reject(l,ident,0x0002,dcid,0,2); return; }
    bt_l2cap_channel_t *c=&l->ch[i];
    // The answer lists the unknown option types, or else the unacceptable
    // options with values we take; whatever does not fit the queue row is
    // left off the list, never off the verdict.
    uint8_t unknown[BT_L2CAP_SIGNAL_BYTES], fix[BT_L2CAP_SIGNAL_BYTES];
    const size_t room=BT_L2CAP_SIGNAL_BYTES-8-6;
    size_t nu=0, nf=0, at=4;
    bool any_unknown=false, unacceptable=false;
    uint16_t mtu=0;
    while(at<n) {
        if(n-at<2 || d[at+1]>n-at-2) { l2_not_understood(l,ident); return; }
        uint8_t type=d[at], len=d[at+1];
        const uint8_t *v=d+at+2;
        at+=2u+len;
        switch(type & 0x7f) {
        case 0x01:      // MTU
            if(len!=2) { l2_not_understood(l,ident); return; }
            if(l2_u16(v)>=L2_MIN_MTU) { mtu=l2_u16(v); break; }
            unacceptable=true;
            if(nf+4<=room) { fix[nf]=0x01; fix[nf+1]=2; l2_put(fix+nf+2,L2_MIN_MTU); nf+=4; }
            break;
        case 0x02:      // Flush Timeout: a hint about the peer's own sending
            if(len!=2) { l2_not_understood(l,ident); return; }
            break;
        case 0x03:      // QoS: best effort is what Basic mode gives anyway
            if(len!=22) { l2_not_understood(l,ident); return; }
            if(v[1]==0x01) break;
            unacceptable=true;
            if(nf+24<=room) { fix[nf]=0x03; fix[nf+1]=22; l2_copy(fix+nf+2,v,22); fix[nf+3]=0x01; nf+=24; }
            break;
        case 0x04:      // Retransmission and Flow Control: Basic mode only
            if(len!=9) { l2_not_understood(l,ident); return; }
            if(v[0]==0x00) break;
            unacceptable=true;
            if(nf+11<=room) { fix[nf]=0x04; fix[nf+1]=9; l2_wipe(fix+nf+2,9); nf+=11; }
            break;
        case 0x05:      // FCS: Basic mode carries none
            if(len!=1) { l2_not_understood(l,ident); return; }
            break;
        default:        // with the hint bit set, an option we may skip
            if(type & 0x80) break;
            any_unknown=true;
            if(nu<room) unknown[nu++]=type;
            break;
        }
    }
    if(c->peer_rounds>=L2_ROUNDS) { l2_close(l,(unsigned)i); return; }
    c->peer_rounds++;
    if(!c->peer_partial) c->peer_mtu_pending=L2_DEFAULT_MTU;
    c->peer_done=false;
    uint8_t r[BT_L2CAP_SIGNAL_BYTES];
    l2_put(r,c->dcid);
    uint16_t result=any_unknown ? 0x0003 : unacceptable ? 0x0001 : 0x0000;
    size_t extra=any_unknown ? nu : unacceptable ? nf : 0;
    l2_copy(r+6,any_unknown ? unknown : fix,extra);
    bool more=!result && (flags & 0x0001);
    l2_put(r+2,more ? 0x0001 : 0x0000); l2_put(r+4,result);
    if(!result) {
        if(mtu) c->peer_mtu_pending=mtu;
        c->peer_partial=more;
        // Agreement starts the count of rounds over, for a later reconfiguration.
        if(!more) { c->peer_mtu=c->peer_mtu_pending; c->peer_done=true; c->peer_rounds=0; }
    } else c->peer_partial=false;
    l2_signal(l,-1,L2_CONF_RSP,ident,r,6+extra);
}

static void l2_conf_rsp(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n<6) { l2_not_understood(l,ident); return; }
    uint16_t scid=l2_u16(d), result=l2_u16(d+4);
    int i=l2_by_ident(l,ident,BT_L2CAP_CONFIG);
    if(i<0 || l->ch[i].cid!=scid) { l->strays++; return; }
    bt_l2cap_channel_t *c=&l->ch[i];
    c->ident=0; c->timer=false;
    if(result==0x0000) { c->ours_done=true; return; }
    // Unacceptable parameters: the MTU the peer proposes, if it is one we
    // can state, is asked for in a new request.
    uint16_t mtu=0;
    for(size_t at=6;at<n;) {
        if(n-at<2 || d[at+1]>n-at-2) { l->malformed++; mtu=0; break; }
        if((d[at] & 0x7f)==0x01 && d[at+1]==2) mtu=l2_u16(d+at+2);
        at+=2u+d[at+1];
    }
    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {
        c->our_mtu=mtu;
        l2_send_config(l,(unsigned)i);
        return;
    }
    l2_close(l,(unsigned)i);
}

static void l2_disc_req(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n!=4) { l2_not_understood(l,ident); return; }
    uint16_t dcid=l2_u16(d), scid=l2_u16(d+2);
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) {
        bt_l2cap_channel_t *c=&l->ch[i];
        if((c->state==BT_L2CAP_CONFIG || c->state==BT_L2CAP_DISCONNECTING) && c->cid==dcid && c->dcid==scid) {
            l2_signal(l,-1,L2_DISC_RSP,ident,d,4);
            l2_free(l,i);
            return;
        }
    }
    // Invalid CID: our CID (the request's destination), then the peer's.
    l2_reject(l,ident,0x0002,dcid,scid,2);
}

static void l2_disc_rsp(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n!=4) { l2_not_understood(l,ident); return; }
    int i=l2_by_ident(l,ident,BT_L2CAP_DISCONNECTING);
    if(i<0 || l->ch[i].dcid!=l2_u16(d) || l->ch[i].cid!=l2_u16(d+2)) { l->strays++; return; }
    l2_free(l,(unsigned)i);
}

static void l2_info_req(bt_l2cap_t *l, uint8_t ident, const uint8_t *d, size_t n)
{
    if(n!=2) { l2_not_understood(l,ident); return; }
    uint16_t type=l2_u16(d);
    uint8_t r[12];
    l2_wipe(r,sizeof(r));
    l2_put(r,type);
    size_t len=4;
    if(type==0x0002) len=8;                   // extended features: none, Basic mode only
    else if(type==0x0003) { r[4]=0x02; len=12; }   // fixed channels: the signalling channel alone
    else l2_put(r+2,0x0001);
    l2_signal(l,-1,L2_INFO_RSP,ident,r,len);
}

// A Command Reject that answers one of our requests ends it: a refused
// Connection Request frees the channel, a refused Configuration Request
// closes it.
static void l2_rejected(bt_l2cap_t *l, uint8_t ident, size_t n)
{
    if(n<2) { l->malformed++; return; }
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) {
        bt_l2cap_channel_t *c=&l->ch[i];
        if(!ident || c->state==BT_L2CAP_FREE || c->ident!=ident) continue;
        if(c->state==BT_L2CAP_CONNECTING) { l->refused=0xfffe; l->refused_psm=c->psm; }
        if(c->state==BT_L2CAP_CONFIG) l2_close(l,i);
        else l2_free(l,i);
        return;
    }
    l->strays++;
}

// A C-frame's commands, in order. A command whose length runs past the
// frame ends the frame, since nothing after it can be found, and is
// answered as not understood unless it is itself a Command Reject, which
// is only counted: a reject is never rejected. A command with identifier
// 0 (§ 4: never valid) is not understood.
static void l2_signalling(bt_l2cap_t *l, const uint8_t *p, size_t n)
{
    while(n) {
        if(n<4) { l->malformed++; return; }
        uint8_t code=p[0], ident=p[1];
        uint16_t len=l2_u16(p+2);
        if(len>n-4) {
            if(code==L2_REJECT) l->malformed++;
            else l2_not_understood(l,ident);
            return;
        }
        const uint8_t *d=p+4;
        if(!ident && code!=L2_REJECT) l2_not_understood(l,ident);
        else switch(code) {
        case L2_REJECT: l2_rejected(l,ident,len); break;
        case L2_CONN_REQ: l2_conn_req(l,ident,d,len); break;
        case L2_CONN_RSP: l2_conn_rsp(l,ident,d,len); break;
        case L2_CONF_REQ: l2_conf_req(l,ident,d,len); break;
        case L2_CONF_RSP: l2_conf_rsp(l,ident,d,len); break;
        case L2_DISC_REQ: l2_disc_req(l,ident,d,len); break;
        case L2_DISC_RSP: l2_disc_rsp(l,ident,d,len); break;
        case L2_ECHO_REQ:
            l2_signal(l,-1,L2_ECHO_RSP,ident,d,len<BT_L2CAP_ECHO_BYTES ? len : BT_L2CAP_ECHO_BYTES);
            break;
        case L2_ECHO_RSP: case L2_INFO_RSP: l->strays++; break;   // we ask neither
        case L2_INFO_REQ: l2_info_req(l,ident,d,len); break;
        default: l2_not_understood(l,ident); break;
        }
        p+=4u+len; n-=4u+len;
    }
}

// ── Receive ─────────────────────────────────────────────────────────────────

static void l2_rx_clear(bt_l2cap_t *l)
{ l->rx_have=l->rx_total=0; l->rx_active=l->rx_oversize=false; }

static void l2_dispatch(bt_l2cap_t *l, bt_l2cap_deliver_t deliver, void *context)
{
    uint16_t len=l2_u16(l->rx), cid=l2_u16(l->rx+2);
    if(l->rx_oversize) {
        // § 4.1: a signalling frame over our signalling MTU is refused,
        // carrying the MTU we take, under its first command's identifier.
        l->oversized++;
        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[5],0x0001,BT_L2CAP_MTU,0,1);
        return;
    }
    if(cid==L2_CID_SIGNAL) { l2_signalling(l,l->rx+4,len); return; }
    if(cid<L2_CID_DYNAMIC) { l->fixed_cid++; return; }
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) {
        bt_l2cap_channel_t *c=&l->ch[i];
        if(c->cid!=cid || !l2_open(c)) continue;
        if(len>c->our_mtu) break;
        if(deliver) deliver(context,i,c->psm,l->rx+4,len);
        return;
    }
    l->closed_cid++;
}

void bt_l2cap_receive(bt_l2cap_t *l, const uint8_t *acl, size_t bytes, uint64_t now,
                      bt_l2cap_deliver_t deliver, void *context)
{
    l->now=now;
    if(!l->up) return;
    if(bytes<4 || l2_u16(acl+2)!=bytes-4) { l->malformed++; return; }
    uint16_t flags=l2_u16(acl)>>12;
    unsigned boundary=flags & 3;
    // Broadcast (bits 14-15) is not for a link, and 0b11 (a complete
    // automatically flushable PDU) is not sent to a BR/EDR host.
    if((flags & 0xc) || boundary==3) { l->malformed++; return; }
    const uint8_t *p=acl+4;
    size_t n=bytes-4;
    if(boundary==1) {
        if(!l->rx_active) { l->orphans++; return; }
    } else {
        if(l->rx_active) l->unfinished++;
        l2_rx_clear(l);
        l->rx_active=true;
    }
    while(n) {
        size_t take;
        if(!l->rx_total) {
            take=4-l->rx_have; if(take>n) take=n;
            l2_copy(l->rx+l->rx_have,p,take);
            l->rx_have+=take;
            if(l->rx_have==4) {
                l->rx_total=4u+l2_u16(l->rx);
                l->rx_oversize=l->rx_total>sizeof(l->rx);
            }
        } else {
            take=l->rx_total-l->rx_have; if(take>n) take=n;
            for(size_t k=0;k<take;k++)
                if(!l->rx_oversize || l->rx_have+k<L2_KEPT) l->rx[l->rx_have+k]=p[k];
            l->rx_have+=take;
        }
        p+=take; n-=take;
        if(l->rx_total && l->rx_have==l->rx_total) {
            if(n) { l->malformed++; l2_rx_clear(l); return; }   // more bytes than the PDU holds
            // The buffer is free again before anything runs: `deliver` may
            // queue, close, or reset.
            l->rx_active=false;
            l2_dispatch(l,deliver,context);
            if(!l->rx_active) l2_rx_clear(l);
            return;
        }
    }
}

// ── The API ─────────────────────────────────────────────────────────────────

void bt_l2cap_init(bt_l2cap_t *l)
{
    l2_wipe(l,sizeof(*l));
    l->up=true; l->next_ident=1; l->next_cid=L2_CID_DYNAMIC;
}

void bt_l2cap_reset(bt_l2cap_t *l)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) l2_wipe(&l->ch[i],sizeof(l->ch[i]));
    l->sig_head=l->sig_count=0;
    l2_rx_clear(l);
    l->up=false; l->encrypted=false;
    l->next_ident=1; l->next_cid=L2_CID_DYNAMIC;
}

void bt_l2cap_set_encrypted(bt_l2cap_t *l, bool on) { l->encrypted=on; }

void bt_l2cap_admit(bt_l2cap_t *l, uint16_t psm, bool admit)
{
    if(!l2_psm_valid(psm)) return;
    for(unsigned i=0;i<BT_L2CAP_ADMITTED;i++) if(l->admitted[i]==psm) {
        if(!admit) l->admitted[i]=0;
        return;
    }
    if(!admit) return;
    for(unsigned i=0;i<BT_L2CAP_ADMITTED;i++) if(!l->admitted[i]) { l->admitted[i]=psm; return; }
}

int bt_l2cap_connect(bt_l2cap_t *l, uint16_t psm, uint64_t now)
{
    l->now=now;
    int i=l2_free_channel(l);
    if(!l->up || i<0 || !l2_psm_valid(psm)) return -1;
    bt_l2cap_channel_t *c=&l->ch[i];
    l2_wipe(c,sizeof(*c));
    c->state=BT_L2CAP_CONNECTING; c->outgoing=true; c->psm=psm; c->cid=l2_new_cid(l);
    c->our_mtu=BT_L2CAP_MTU;
    uint8_t d[4];
    l2_put(d,psm); l2_put(d+2,c->cid);
    l2_request(l,(unsigned)i,L2_CONN_REQ,d,sizeof(d));
    return i;
}

void bt_l2cap_close(bt_l2cap_t *l, unsigned channel, uint64_t now)
{
    l->now=now;
    if(channel>=BT_L2CAP_CHANNELS || l->ch[channel].state==BT_L2CAP_FREE ||
       l->ch[channel].state==BT_L2CAP_DISCONNECTING) return;
    l2_close(l,channel);
}

bool bt_l2cap_is_open(const bt_l2cap_t *l, unsigned channel)
{ return channel<BT_L2CAP_CHANNELS && l2_open(&l->ch[channel]); }

bool bt_l2cap_is_free(const bt_l2cap_t *l, unsigned channel)
{ return channel<BT_L2CAP_CHANNELS && l->ch[channel].state==BT_L2CAP_FREE; }

uint16_t bt_l2cap_peer_mtu(const bt_l2cap_t *l, unsigned channel)
{ return bt_l2cap_is_open(l,channel) ? l->ch[channel].peer_mtu : 0; }

bool bt_l2cap_send(bt_l2cap_t *l, unsigned channel, const uint8_t *data, size_t bytes)
{
    if(!bt_l2cap_is_open(l,channel)) return false;
    bt_l2cap_channel_t *c=&l->ch[channel];
    if(c->queued || bytes>c->peer_mtu || bytes>BT_L2CAP_MTU) return false;
    l2_put(c->out,(uint16_t)bytes); l2_put(c->out+2,c->dcid);
    l2_copy(c->out+4,data,bytes);
    c->queued=true; c->queued_bytes=(uint16_t)(bytes+4); c->queued_seq=l->next_seq++;
    return true;
}

void bt_l2cap_tick(bt_l2cap_t *l, uint64_t now, bool may_send, bt_l2cap_send_t send, void *context)
{
    l->now=now;
    // An unanswered Connection or Disconnection Request frees the channel;
    // an unanswered Configuration Request closes it. The link stays.
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) {
        bt_l2cap_channel_t *c=&l->ch[i];
        if(c->state==BT_L2CAP_FREE || !c->timer || now<c->deadline) continue;
        if(c->state==BT_L2CAP_CONNECTING) { l->refused=0xffff; l->refused_psm=c->psm; }
        if(c->state==BT_L2CAP_CONFIG) l2_close(l,i);
        else l2_free(l,i);
    }
    if(!may_send || !send) return;
    if(l->sig_count) {
        bt_l2cap_signal_t s=*l2_sig_at(l,0);
        l->sig_head=(l->sig_head+1)%BT_L2CAP_SIGNALS; l->sig_count--;
        if(s.request && s.channel<BT_L2CAP_CHANNELS) {
            bt_l2cap_channel_t *c=&l->ch[s.channel];
            if(c->state!=BT_L2CAP_FREE && c->ident==s.ident && !c->timer) { c->timer=true; c->deadline=now+L2_RTX_MS; }
        }
        send(context,s.pdu,s.bytes);
        return;
    }
    int oldest=-1;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++)
        if(l->ch[i].queued && (oldest<0 || (int32_t)(l->ch[i].queued_seq-l->ch[oldest].queued_seq)<0)) oldest=(int)i;
    if(oldest<0) return;
    bt_l2cap_channel_t *c=&l->ch[oldest];
    c->queued=false;
    send(context,c->out,c->queued_bytes);
}

bool bt_l2cap_wants_send(const bt_l2cap_t *l)
{
    if(l->sig_count) return true;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l->ch[i].queued) return true;
    return false;
}

static const char *l2_refusal_text(uint16_t r)
{
    switch(r) {
    case 0x0000: return "none";
    case 0x0002: return "PSM not supported";
    case 0x0003: return "security block";
    case 0x0004: return "no resources";
    case 0x0006: return "invalid source CID";
    case 0x0007: return "source CID already allocated";
    case 0xfffd: return "accepted without a valid CID";
    case 0xfffe: return "command rejected";
    case 0xffff: return "no answer";
    default: return "other";
    }
}

size_t bt_l2cap_status(const bt_l2cap_t *l, char *out, size_t cap)
{
    static const char *const states[]={"free","connecting","configuring","disconnecting"};
    size_t at=0;
    int n=snprintf(out,cap,"l2cap: link %s, %s\n",l->up ? "up" : "down",l->encrypted ? "encrypted" : "not encrypted");
    if(n>0) at=(size_t)n;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS && at<cap;i++) {
        const bt_l2cap_channel_t *c=&l->ch[i];
        if(c->state==BT_L2CAP_FREE) continue;
        n=snprintf(out+at,cap-at,"channel %u: %s psm %04x cid %04x peer cid %04x %s mtu ours %u peer %u%s%s\n",
                   i,l2_open(c) ? "open" : states[c->state & 3],c->psm,c->cid,c->dcid,
                   c->outgoing ? "outgoing" : "incoming",(unsigned)c->our_mtu,(unsigned)c->peer_mtu,
                   c->ours_done ? "" : " (ours unconfigured)",c->peer_done ? "" : " (peer's unconfigured)");
        if(n>0) at+=(size_t)n;
    }
    if(at<cap) {
        n=snprintf(out+at,cap-at,"last refusal: %s (%04x) psm %04x\n"
                   "malformed %u oversized %u orphans %u unfinished %u closed cid %u fixed cid %u strays %u queue full %u\n",
                   l2_refusal_text(l->refused),l->refused,l->refused_psm,
                   (unsigned)l->malformed,(unsigned)l->oversized,(unsigned)l->orphans,(unsigned)l->unfinished,
                   (unsigned)l->closed_cid,(unsigned)l->fixed_cid,(unsigned)l->strays,(unsigned)l->queue_full);
        if(n>0) at+=(size_t)n;
    }
    return !cap ? 0 : at<cap ? at : cap-1;
}
