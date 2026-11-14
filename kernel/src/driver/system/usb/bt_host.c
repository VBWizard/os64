#include "driver/system/usb/bt_host.h"
#include "strings/sprintf.h"

// A whole Classic ACL packet (its 4-byte header, then an L2CAP PDU of
// BT_L2CAP_MTU + 4) fits the assembler (bt_host.h).
_Static_assert(sizeof(((bt_host_t *)0)->wire)>=4+BT_L2CAP_MTU+4,"a whole Classic ACL packet fits the ACL assembler");

static uint16_t host_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static void host_wipe(void *p,size_t n)
{
    volatile uint8_t *v=p;
    while(n--) *v++=0;
}
static bool host_setup(const bt_host_t *h)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        const bt_le_t *s=&h->peers[i].le;
        if(!bt_le_quiescent(s) && !bt_le_input_active(s)) return true;
    }
    return false;
}
void bt_host_init(bt_host_t *h,uint64_t now)
{
    // Wiped in place: the host is tens of kilobytes, and assigning it from
    // a compound literal builds a whole second one on the kernel stack.
    host_wipe(h,sizeof(*h));
    h->credits=1;
    bt_acl_init(&h->acl);
    for(unsigned i=0;i<BT_HOST_PEERS;i++) h->peers[i].le.shared_controller=true;
    bt_scan_start_le(&h->scan,now);
}
// Bring the count's links in line with the peers: one for each LE peer that
// is connected and one for a connected Classic link, under its handle; and
// the Classic link's L2CAP with it. Called wherever a peer may have
// connected or let go (its events, its tick), before the count is asked or
// told anything, and before an ACL packet on a new link can be routed.
static void host_acl_link(bt_host_t *h,uint16_t *counted,bool connected,uint16_t handle,bool le)
{
    uint16_t now=connected?(uint16_t)((handle&0xfff)+1):0;   // the handle plus one (bt_host.h)
    if(now==*counted) return;
    if(*counted) bt_acl_link_down(&h->acl,(uint16_t)(*counted-1));
    if(now) (void)bt_acl_link_up(&h->acl,(uint16_t)(now-1),le);
    *counted=now;
}
static void host_services_link(bt_host_t *h,bool up);
static void host_acl_links(bt_host_t *h)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        const bt_le_t *s=&h->peers[i].le;
        host_acl_link(h,&h->acl_link[i],s->connected,s->handle,true);
    }
    // The Classic link's L2CAP follows the same edge: a connection under a
    // new handle, or none, starts it over.
    uint16_t before=h->acl_classic;
    host_acl_link(h,&h->acl_classic,h->classic.connected,h->classic.handle,false);
    if(h->acl_classic!=before) host_services_link(h,h->acl_classic!=0);
}
// An event that cannot be right. Once the controller is known to be up that
// is the end of the host. During start-up it is the end of the ROUND, which
// bt_host_tick may run again: an adapter taken over while it was running
// can hand this host the tail of an old event first, and the assembler
// delivers whatever that tail happens to frame before it is back in step.
static void host_bad_event(bt_host_t *h,bool hardware)
{
    if(h->initialized) { h->failed=true; return; }
    if(bt_scan_quiescent(&h->scan)) return;    // between rounds: nothing it could belong to
    if(hardware) h->scan.hardware_error=true; else h->scan.bad_reply=true;
}
void bt_host_event(bt_host_t *h,const uint8_t *e,size_t n)
{
    if(n<2 || n!=2u+e[1]) { host_bad_event(h,false); return; }
    if(e[0]==0x0e || e[0]==0x0f) {
        bool status=e[0]==0x0f;
        if(n<(status?6u:5u) || (status && n!=6)) { host_bad_event(h,false); return; }
        h->credits=e[status?3:2];
        uint16_t op=host_u16(e+(status?4:3));
        if(h->owner==BT_HOST_OWNER_SCAN) {
            if(op==h->scan.opcode) h->command_done=true;
            bt_scan_event(&h->scan,e,n);
        } else if(h->owner>=BT_HOST_OWNER_PEER0 && h->owner<BT_HOST_OWNER_PEER0+BT_HOST_PEERS) {
            bt_le_t *s=&h->peers[h->owner-BT_HOST_OWNER_PEER0].le;
            if(op==s->opcode) h->command_done=true;
            bt_le_event(s,e,n);
        } else if(h->owner==BT_HOST_OWNER_CLASSIC) {
            if(op==h->classic.opcode) h->command_done=true;
            (void)bt_classic_event(&h->classic,e,n);
        }
        return;
    }
    if(e[0]==0x10) { host_bad_event(h,true); return; }
    if(e[0]==0x13) {
        // Counted here for the controller as a whole; each LE peer below
        // still takes its own entries, and judges them, as it always has.
        uint16_t link;
        if(bt_acl_completed(&h->acl,e,n,&link)!=BT_ACL_COUNTED) h->acl_bad_completions++;
    }
    // The Classic link is offered every other event first. It takes the
    // BR/EDR-only events, and Disconnection Complete, Encryption Change and
    // Key Refresh only for its own live handle (bt_classic.h), so nothing
    // an LE peer or the scan reads is taken from them.
    if(h->initialized && h->classic_available && bt_classic_event(&h->classic,e,n)) {
        host_acl_links(h); return;
    }
    if(e[0]==0x3e && n>=3 && e[2]==2) { bt_scan_event(&h->scan,e,n); return; }
    if(e[0]==1 || e[0]==2 || e[0]==0x22 || e[0]==0x2f) {
        bt_scan_event(&h->scan,e,n); return;
    }
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        bt_le_t *s=&h->peers[i].le;
        // Initiation is serialized. A completion belongs to its pending peer,
        // including a success racing cancellation; established peers ignore it.
        if(e[0]==0x3e && n>=3 && e[2]==1 &&
           !(s->link_may_active && !s->connected)) continue;
        bt_le_event(s,e,n);
    }
    host_acl_links(h);
}
typedef struct {
    bt_host_t *host;
    unsigned peer;
    bt_scan_send_t command;
    bt_le_acl_send_t acl;
    bt_host_report_t report;
    void *context;
} host_call_t;
static void host_report(void *context,const bt_le_input_t *input)
{
    host_call_t *c=context; c->report(c->context,c->peer,input);
}
static void host_wire_empty(bt_host_t *h)
{
    volatile uint8_t *wipe=h->wire;
    for(size_t i=0;i<h->wire_used;i++) wipe[i]=0;
    h->wire_used=h->wire_need=0;
}
// ── Slot 2's services: L2CAP and SDP (AUDIO_CLASSIC.md part S) ────────────
//
// The largest L2CAP payload this host sends on a channel: the peer's MTU,
// ours, and what one Classic ACL packet of the controller's carries after
// the 4-byte L2CAP header, whichever is least. 0 when the channel is not
// open or the controller's sizes are not known.
static size_t host_l2cap_room(const bt_host_t *h,unsigned channel)
{
    size_t room=bt_l2cap_peer_mtu(&h->l2cap,channel);
    size_t packet=bt_acl_packet_size(&h->acl,false);
    if(room>BT_L2CAP_MTU) room=BT_L2CAP_MTU;
    return packet<4?0:room<packet-4?room:packet-4;
}
static void host_sdp_answer_drop(bt_host_t *h)
{
    host_wipe(h->sdp_answer,h->sdp_answer_bytes);
    h->sdp_answer_bytes=0;
}
// The held answer, offered again: taken, or dropped when its channel is no
// longer the open one it was made for or the peer's MTU no longer admits it.
static void host_sdp_answer_offer(bt_host_t *h)
{
    if(!h->sdp_answer_bytes) return;
    unsigned ch=h->sdp_answer_channel;
    if(!bt_l2cap_is_open(&h->l2cap,ch) || h->l2cap.ch[ch].cid!=h->sdp_answer_cid ||
       h->sdp_answer_bytes>host_l2cap_room(h,ch)) {
        h->sdp_dropped++; host_sdp_answer_drop(h); return;
    }
    if(bt_l2cap_send(&h->l2cap,ch,h->sdp_answer,h->sdp_answer_bytes)) {
        h->sdp_answered++; host_sdp_answer_drop(h);
    }
}
// A whole PDU on an open channel: AVDTP's go to slot 2's audio (below). On
// an SDP channel we opened, it is the answer to our query. On one the
// headphones opened, it is their question, answered from our record into
// the host's buffer and queued; with L2CAP's one payload slot still taken
// the answer is held (one at most) and offered every tick.
static void host_avdtp_deliver(bt_host_t *h,unsigned channel,const uint8_t *data,size_t bytes);
static void host_l2cap_deliver(void *context,unsigned channel,uint16_t psm,const uint8_t *data,size_t bytes)
{
    bt_host_t *h=context;
    if(channel>=BT_L2CAP_CHANNELS) return;
    if(psm==BT_L2CAP_PSM_AVDTP) { host_avdtp_deliver(h,channel,data,bytes); return; }
    if(psm!=BT_L2CAP_PSM_SDP) return;
    const bt_l2cap_channel_t *c=&h->l2cap.ch[channel];
    if(c->outgoing) {
        if(h->sdp_channel==channel+1 && c->cid==h->sdp_cid) bt_sdp_client_receive(&h->sdp,data,bytes);
        return;
    }
    if(h->sdp_answer_bytes) { h->sdp_dropped++; return; }
    size_t n=bt_sdp_respond(data,bytes,h->sdp_answer,host_l2cap_room(h,channel));
    if(!n) return;
    h->sdp_answer_bytes=(uint16_t)n; h->sdp_answer_channel=(uint8_t)channel; h->sdp_answer_cid=c->cid;
    host_sdp_answer_offer(h);
}
// What belongs to one connection: the query's state, the request and the
// answer held, the client. The result found stays, now an earlier
// connection's.
static void host_sdp_clear(bt_host_t *h)
{
    h->sdp_asked=false; h->sdp_channel=0; h->sdp_cid=0;
    h->sdp_kept=0; h->sdp_kept_cid=0;
    h->sdp_not_made=NULL; h->sdp_refused=0;
    host_wipe(h->sdp_request,sizeof(h->sdp_request)); h->sdp_request_bytes=0;
    host_sdp_answer_drop(h);
    host_wipe(&h->sdp,sizeof(h->sdp));          // BT_SDP_IDLE
    h->sdp_result_current=false;
}
static void host_audio_link(bt_host_t *h,bool up);
// The Classic link connected (up) or went: L2CAP starts over, admitting SDP
// and, with an audio sink, AVDTP; and so does everything the last
// connection held.
static void host_services_link(bt_host_t *h,bool up)
{
    if(up) {
        bt_l2cap_init(&h->l2cap);
        bt_l2cap_admit(&h->l2cap,BT_L2CAP_PSM_SDP,true);
        if(h->a2dp_ops) bt_l2cap_admit(&h->l2cap,BT_L2CAP_PSM_AVDTP,true);
    } else bt_l2cap_reset(&h->l2cap);
    h->l2cap_encrypted=false;
    host_sdp_clear(h);
    host_audio_link(h,up);
}
// The query is over, one way or another: its line is kept and the
// transport says it once (sdp_ended).
static void host_sdp_ended(bt_host_t *h,const char *not_made)
{
    if(not_made) {
        h->sdp_not_made=not_made;
        snprintf(h->sdp_line,sizeof(h->sdp_line),"sdp: not made: %s\n",not_made);
        host_wipe(&h->sdp,sizeof(h->sdp));
    } else {
        bt_sdp_client_status(&h->sdp,h->sdp_line,sizeof(h->sdp_line));
        if(bt_sdp_client_state(&h->sdp)==BT_SDP_DONE) {
            h->sdp_result=*bt_sdp_client_result(&h->sdp);
            h->sdp_result_valid=h->sdp_result_current=true;
            bt_sdp_client_status(&h->sdp,h->sdp_result_line,sizeof(h->sdp_result_line));
        }
    }
    // A query that ended on its channel leaves the channel open, and kept:
    // it is closed once AVDTP's signalling channel is open.
    const bt_l2cap_channel_t *c=h->sdp_channel?&h->l2cap.ch[h->sdp_channel-1u]:NULL;
    if(!not_made && c && c->state!=BT_L2CAP_FREE && c->state!=BT_L2CAP_DISCONNECTING && c->cid==h->sdp_cid) {
        h->sdp_kept=h->sdp_channel; h->sdp_kept_cid=h->sdp_cid;
    }
    h->sdp_channel=0;
    host_wipe(h->sdp_request,sizeof(h->sdp_request)); h->sdp_request_bytes=0;
    h->sdp_ended++;
}
// One query a connection, ours: once the link is READY, a channel to the
// headphones' SDP; once it is open, the client, with the smaller of the two
// MTUs; its requests queued as the channel's payload slot frees. When it is
// DONE or FAILED the channel is LEFT OPEN, and whatever arrives on it after
// that is ignored: headphones hang up a link whose last channel has just
// closed, so it is kept until AVDTP's signalling channel is open, and with
// no audio sink, for the connection. A
// channel refused, unanswered or closed before the answer is whole ends the
// query as not made, and it is not asked again on this connection. The
// client's own five seconds start only when the channel is open, so
// L2CAP's ten-second RTX is not inside them.
static void host_sdp_step(bt_host_t *h,uint64_t now)
{
    bt_l2cap_t *l=&h->l2cap;
    if(h->classic.phase==BT_CLASSIC_READY && !h->sdp_asked) {
        h->sdp_asked=true;
        // SDP may precede encryption. A channel to any other PSM must wait
        // for the link to be encrypted, and L2CAP does not refuse it: the
        // host's guard is host_avdtp_connect, which every 0x0019 request
        // goes through.
        int ch=bt_l2cap_connect(l,BT_L2CAP_PSM_SDP,now);
        if(ch<0) { host_sdp_ended(h,"no L2CAP channel was free"); return; }
        h->sdp_channel=(uint8_t)(ch+1); h->sdp_cid=l->ch[ch].cid;
    }
    if(!h->sdp_channel) return;
    unsigned ch=h->sdp_channel-1u;
    const bt_l2cap_channel_t *c=&l->ch[ch];
    bt_sdp_state_t state=bt_sdp_client_state(&h->sdp);
    bool gone=c->state==BT_L2CAP_FREE || !c->outgoing || c->cid!=h->sdp_cid;
    // An answer finished on the way in counts even if the channel went in
    // the same pass.
    if(state==BT_SDP_DONE || state==BT_SDP_FAILED) { host_sdp_ended(h,NULL); return; }
    if(gone) {
        // Refused or unanswered while connecting (L2CAP records why against
        // the index this attempt was given), or closed later.
        uint16_t psm, refused=bt_l2cap_refusal(l,ch,&psm);
        if(state==BT_SDP_IDLE && refused) {
            h->sdp_refused=refused; h->l2cap_refusals++;
            h->l2cap_refused_result=refused; h->l2cap_refused_psm=psm;
            host_sdp_ended(h,refused==0xffff?"the headphones did not answer the SDP channel's request":
                             "the headphones refused the SDP channel");
        } else host_sdp_ended(h,state==BT_SDP_IDLE?"the SDP channel closed before it opened":
                                 "the SDP channel closed before the answer was whole");
        return;
    }
    if(state==BT_SDP_IDLE) {
        if(!bt_l2cap_is_open(l,ch)) return;
        uint16_t mtu=bt_l2cap_peer_mtu(l,ch);
        bt_sdp_client_start(&h->sdp,now,mtu<c->our_mtu?mtu:c->our_mtu);
    }
    bt_sdp_client_tick(&h->sdp,now);
    if(!h->sdp_request_bytes)
        h->sdp_request_bytes=(uint8_t)bt_sdp_client_request(&h->sdp,h->sdp_request,sizeof(h->sdp_request));
    if(h->sdp_request_bytes && bt_l2cap_send(l,ch,h->sdp_request,h->sdp_request_bytes)) {
        host_wipe(h->sdp_request,sizeof(h->sdp_request)); h->sdp_request_bytes=0;
    }
    state=bt_sdp_client_state(&h->sdp);
    if(state==BT_SDP_DONE || state==BT_SDP_FAILED) host_sdp_ended(h,NULL);     // the client's own limit, this tick
}
// ── Slot 2's audio: AVDTP's channels (AUDIO_A2DP.md part P) ────────────────
//
// Only with an audio sink (bt_host_audio). Every channel to or from PSM
// 0x0019 has one role, and the roles are decided from STATE (AVDTP's,
// L2CAP's channels', the records below) each time the host looks: after
// every ACL packet L2CAP takes, which is when a channel can open, and at
// every tick. Nothing is remembered as an event. A role is held by index
// AND CID, since an index is reused. A role's channel is LIVE while L2CAP
// holds it connected under that CID (configuring again counts: a peer that
// reconfigures an open channel does not end it); being let go, freed, or
// reused under another CID ends it.
//
//   role        | begins                                  | ends, and what follows
//   ------------+-----------------------------------------+------------------------------------
//   asking      | we connect: signalling once the link is | it opens: given one of the roles
//   (ours, not  | READY and encrypted, the SDP query has  | below. It ends unopened: a refusal
//   yet open)   | ended, AVDTP is IDLE, no 0x0019 channel | (L2CAP's, read by its index) is
//               | exists and we may ask (below); media    | counted; media is told AVDTP
//               | when bt_avdtp_wants_media and there is  | (media_closed, refused or not) if
//               | no media channel                        | no media channel took its place;
//               |                                         | signalling counts as an end if no
//               |                                         | signalling channel took its place
//   signalling  | a 0x0019 channel OPENS, anyone's, while | AVDTP wants both closed (media is
//               | none is signalling and AVDTP is IDLE:   | closed first), or either side or
//               | signalling_open (with the SDP record's  | L2CAP's timers close it:
//               | AVDTP version, or 0). The SDP channel   | signalling_closed. An end
//               | the query left open is closed now       |
//   media       | a 0x0019 channel OPENS while AVDTP is   | AVDTP no longer wants one, or wants
//               | AWAITING_MEDIA and there is none:       | both closed, or it closes:
//               | media_open                              | media_closed
//   (closed)    | a 0x0019 channel opens and is none of   | L2CAP finishes the disconnection
//               | these: a second signalling channel      |
//               | (the one that opened second), a second  |
//               | media channel, or one in FAILED         |
//
// We ask for signalling at most twice a connection: once, and once more
// BT_HOST_AVDTP_AGAIN_MS after one ended (ours or theirs, opened or
// refused); AVDTP's own count makes a second session gone wrong FAILED.
// PDUs on the signalling channel go to AVDTP; on any other 0x0019 channel
// they are dropped and counted, since we are a source.
#define BT_HOST_AVDTP_AGAIN_MS 10000
// AVDTP's clock: the pump's microseconds, as of the last tick, in
// milliseconds (bt_host.h now_us). L2CAP keeps the transport's `now`.
static uint64_t host_avdtp_now(const bt_host_t *h) { return h->now_us/1000; }
static bool host_role_live(const bt_host_t *h,uint8_t role,uint16_t cid)
{
    if(!role) return false;
    const bt_l2cap_channel_t *c=&h->l2cap.ch[role-1u];
    return c->state!=BT_L2CAP_FREE && c->state!=BT_L2CAP_DISCONNECTING && c->cid==cid;
}
static bool host_role_is(uint8_t role,uint16_t role_cid,unsigned channel,uint16_t cid)
{ return role==channel+1u && role_cid==cid; }
static uint16_t host_avdtp_version(const bt_host_t *h)
{
    return h->sdp_result_valid && h->sdp_result_current && h->sdp_result.found ? h->sdp_result.avdtp_version : 0;
}
// The link came up or went: the pump lets the seam go if it held it (a
// link that comes up under a new handle may not have been seen going),
// and the pump and AVDTP start over.
static void host_audio_link(bt_host_t *h,bool up)
{
    if(h->a2dp_ops) bt_a2dp_reset(&h->a2dp,h->a2dp_ops,h->a2dp_ctx);
    if(up) { bt_a2dp_init(&h->a2dp); bt_avdtp_init(&h->avdtp); }
    else bt_avdtp_reset(&h->avdtp);
    h->avdtp_sig=h->avdtp_media=h->avdtp_asking=0;
    h->avdtp_sig_cid=h->avdtp_media_cid=h->avdtp_asking_cid=0;
    h->avdtp_asking_media=false;
    h->avdtp_sig_parked=0;
    h->avdtp_sig_asked=h->avdtp_sig_ends=0;
    h->avdtp_sig_again_at=0;
}
// The roles, settled and claimed (the table above). `lnow` is L2CAP's
// clock, for the channels this closes.
static void host_avdtp_channels(bt_host_t *h,uint64_t lnow)
{
    if(!h->a2dp_ops || !h->l2cap.up) return;
    bt_l2cap_t *l=&h->l2cap;
    bt_avdtp_t *a=&h->avdtp;
    uint64_t at=host_avdtp_now(h);
    if(h->avdtp_sig && !host_role_live(h,h->avdtp_sig,h->avdtp_sig_cid)) {
        h->avdtp_sig=0; h->avdtp_sig_cid=0; h->avdtp_sig_parked=0;
        bt_avdtp_signalling_closed(a,at);
        h->avdtp_sig_ends++; h->avdtp_sig_again_at=at+BT_HOST_AVDTP_AGAIN_MS;
    }
    if(h->avdtp_media && !host_role_live(h,h->avdtp_media,h->avdtp_media_cid)) {
        h->avdtp_media=0; h->avdtp_media_cid=0;
        bt_avdtp_media_closed(a,at,false);
    }
    if(h->avdtp_asking && !host_role_live(h,h->avdtp_asking,h->avdtp_asking_cid)) {
        uint16_t psm, refused=bt_l2cap_refusal(l,h->avdtp_asking-1u,&psm);
        bool media=h->avdtp_asking_media;
        h->avdtp_asking=0; h->avdtp_asking_cid=0; h->avdtp_asking_media=false;
        if(refused) { h->l2cap_refusals++; h->l2cap_refused_result=refused; h->l2cap_refused_psm=psm; }
        if(media && !h->avdtp_media) bt_avdtp_media_closed(a,at,refused!=0);
        else if(!media && !h->avdtp_sig) { h->avdtp_sig_ends++; h->avdtp_sig_again_at=at+BT_HOST_AVDTP_AGAIN_MS; }
    }
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) {
        const bt_l2cap_channel_t *c=&l->ch[i];
        if(c->psm!=BT_L2CAP_PSM_AVDTP || !bt_l2cap_is_open(l,i)) continue;
        if(host_role_is(h->avdtp_sig,h->avdtp_sig_cid,i,c->cid) || host_role_is(h->avdtp_media,h->avdtp_media_cid,i,c->cid))
            continue;
        if(host_role_is(h->avdtp_asking,h->avdtp_asking_cid,i,c->cid)) {
            h->avdtp_asking=0; h->avdtp_asking_cid=0; h->avdtp_asking_media=false;
        }
        bt_avdtp_state_t st=bt_avdtp_state(a);
        if(st==BT_AVDTP_AWAITING_MEDIA && !h->avdtp_media) {
            h->avdtp_media=(uint8_t)(i+1); h->avdtp_media_cid=c->cid;
            bt_avdtp_media_open(a,at);
        } else if(!h->avdtp_sig && st==BT_AVDTP_IDLE) {
            h->avdtp_sig=(uint8_t)(i+1); h->avdtp_sig_cid=c->cid;
            bt_avdtp_signalling_open(a,at,host_avdtp_version(h));
        } else bt_l2cap_close(l,i,lnow);
    }
}
static void host_avdtp_deliver(bt_host_t *h,unsigned channel,const uint8_t *data,size_t bytes)
{
    host_avdtp_channels(h,h->classic.now);
    if(host_role_is(h->avdtp_sig,h->avdtp_sig_cid,channel,h->l2cap.ch[channel].cid))
        bt_avdtp_receive(&h->avdtp,data,bytes,host_avdtp_now(h));
    else h->avdtp_media_dropped++;
}
// Whether we may ask for a signalling channel now (the table above).
static bool host_avdtp_may_ask(const bt_host_t *h)
{
    if(h->classic.phase!=BT_CLASSIC_READY || !h->classic.encrypted || !h->l2cap.encrypted) return false;
    if(!h->sdp_asked || h->sdp_channel) return false;          // the query has not ended
    if(bt_avdtp_state(&h->avdtp)!=BT_AVDTP_IDLE || h->avdtp_sig || h->avdtp_asking) return false;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++)
        if(h->l2cap.ch[i].state!=BT_L2CAP_FREE && h->l2cap.ch[i].psm==BT_L2CAP_PSM_AVDTP) return false;
    return h->avdtp_sig_ends<=1 && h->avdtp_sig_asked<=h->avdtp_sig_ends &&
           host_avdtp_now(h)>=h->avdtp_sig_again_at;
}
// AVDTP's message onto the signalling channel. What AVDTP wants sent now
// (the answers owed first, in its own order; a message longer than the
// channel's room is AVDTP's to drop) is asked each time; the message parked
// in the channel's payload slot follows it: one AVDTP has retired or put
// behind another (its serial no longer the head's) is taken back, and the
// head is queued in a free slot. A message is SENT, for AVDTP, only when
// host_l2cap_send hands it to the controller (bt_avdtp_sent).
static void host_avdtp_feed(bt_host_t *h)
{
    if(!h->avdtp_sig) return;
    unsigned ch=h->avdtp_sig-1u;
    bt_l2cap_t *l=&h->l2cap;
    if(!bt_l2cap_is_open(l,ch)) return;          // reconfiguring: the slot is held as it is
    uint8_t msg[BT_AVDTP_LONGEST];
    uint32_t serial;
    size_t n=bt_avdtp_next(&h->avdtp,msg,sizeof(msg),(uint16_t)host_l2cap_room(h,ch),host_avdtp_now(h),&serial);
    if(l->ch[ch].queued) {
        if(h->avdtp_sig_parked && serial==h->avdtp_sig_parked) return;
        (void)bt_l2cap_unqueue(l,ch);
    }
    h->avdtp_sig_parked=0;
    if(n && bt_l2cap_send(l,ch,msg,n)) h->avdtp_sig_parked=serial;
}
static void host_l2cap_send(void *context,const uint8_t *pdu,size_t n);
static bool host_l2cap_may_send(const bt_host_t *h);
// ── Slot 2's audio: the pump (AUDIO_A2DP.md § The sample clock) ───────────
//
// The pump makes at most one packet, which the host queues on the media
// channel; its frames count as sent when L2CAP hands that PDU to the
// controller, which host_l2cap_send sees by its CID, in whichever tick it
// leaves (the gate may hold it for several). A packet that leaves a live
// media channel's slot any other way (the peer's new MTU dropped it, or a
// controller packet could not carry it) is let go here as lost
// (bt_a2dp_lost), so the stream goes on. One queued when the media channel
// closes is not let go here: the pump keeps it pending until the next
// session or a reset clears it, which is harmless because a media channel
// closing ends the session in AVDTP.
static void host_a2dp_settle(bt_host_t *h)
{
    if(!h->a2dp.pending || !host_role_live(h,h->avdtp_media,h->avdtp_media_cid)) return;
    if(h->l2cap.ch[h->avdtp_media-1u].queued) return;
    bt_a2dp_lost(&h->a2dp);
}
// The one loop, after LE's turn and SDP's and AVDTP's, at most three times
// round: while the media channel is open and the send gate allows, a packet
// is made if the channel's slot is empty (the pump makes one only when one
// is due, and none while one is pending), and L2CAP is given a turn, which
// sends the oldest PDU waiting: the packet, or something older first, in
// which case the packet goes on a later round or tick. Nothing is pulled
// from the seam that cannot be queued now.
#define BT_HOST_A2DP_ROUNDS 3
static void host_a2dp_send(bt_host_t *h,uint64_t now,host_call_t *call)
{
    bt_l2cap_t *l=&h->l2cap;
    for(unsigned round=0;round<BT_HOST_A2DP_ROUNDS;round++) {
        if(!h->avdtp_media) return;
        unsigned ch=h->avdtp_media-1u;
        if(!bt_l2cap_is_open(l,ch) || !host_l2cap_may_send(h)) return;
        if(!l->ch[ch].queued) {
            size_t n=bt_a2dp_packet(&h->a2dp,h->now_us,(uint16_t)host_l2cap_room(h,ch),h->a2dp_packet,
                                    sizeof(h->a2dp_packet),h->a2dp_ops,h->a2dp_ctx);
            if(!n) return;
            (void)bt_l2cap_send(l,ch,h->a2dp_packet,n);
        }
        bt_l2cap_tick(l,now,true,host_l2cap_send,call);
        host_a2dp_settle(h);
    }
}
// Our 0x0019 Connection Request, signalling or media, recorded as the one
// being asked for. L2CAP does not refuse a non-SDP connect on a link that
// is not encrypted; the host does, here, for both.
static bool host_avdtp_connect(bt_host_t *h,bool media,uint64_t now)
{
    if(!h->l2cap.encrypted || !h->classic.encrypted) return false;
    int ch=bt_l2cap_connect(&h->l2cap,BT_L2CAP_PSM_AVDTP,now);
    if(ch<0) return false;
    h->avdtp_asking=(uint8_t)(ch+1); h->avdtp_asking_cid=h->l2cap.ch[ch].cid; h->avdtp_asking_media=media;
    return true;
}
// Slot 2's audio, each tick, after SDP: the roles, the channels AVDTP wants
// opened and closed, its timers and its messages, then the pump's policy
// and its packets.
static void host_audio_tick(bt_host_t *h,uint64_t now,host_call_t *call)
{
    if(!h->a2dp_ops || !h->l2cap.up) return;
    bt_l2cap_t *l=&h->l2cap;
    bt_avdtp_t *a=&h->avdtp;
    host_avdtp_channels(h,now);
    // The SDP channel the query left open goes once signalling is open:
    // the headphones keep a link with a channel on it.
    if(h->sdp_kept && h->avdtp_sig) {
        if(host_role_live(h,h->sdp_kept,h->sdp_kept_cid)) bt_l2cap_close(l,h->sdp_kept-1u,now);
        h->sdp_kept=0; h->sdp_kept_cid=0;
    }
    bt_avdtp_tick(a,host_avdtp_now(h));
    if(bt_avdtp_wants_closed(a)) {
        // A session gone wrong: media, then signalling, and whatever of
        // ours is still being asked for.
        if(h->avdtp_media) bt_l2cap_close(l,h->avdtp_media-1u,now);
        if(h->avdtp_sig) bt_l2cap_close(l,h->avdtp_sig-1u,now);
        if(h->avdtp_asking) bt_l2cap_close(l,h->avdtp_asking-1u,now);
    } else if(!bt_avdtp_media_wanted(a)) {
        if(h->avdtp_media) bt_l2cap_close(l,h->avdtp_media-1u,now);
        if(h->avdtp_asking && h->avdtp_asking_media) bt_l2cap_close(l,h->avdtp_asking-1u,now);
    } else if(bt_avdtp_wants_media(a) && !h->avdtp_media && !h->avdtp_asking) {
        (void)host_avdtp_connect(h,true,now);
    }
    if(host_avdtp_may_ask(h) && host_avdtp_connect(h,false,now)) h->avdtp_sig_asked++;
    // What was let go above is AVDTP's news now, not at the next look.
    host_avdtp_channels(h,now);
    host_avdtp_feed(h);
    host_a2dp_settle(h);
    bt_a2dp_step(&h->a2dp,a,h->now_us,h->a2dp_ops,h->a2dp_ctx);
    host_avdtp_feed(h);          // what the pump asked of AVDTP, if the slot is still free
    host_a2dp_send(h,now,call);
}
size_t bt_host_media_room(const bt_host_t *h)
{
    return h->avdtp_media && bt_l2cap_is_open(&h->l2cap,h->avdtp_media-1u)?host_l2cap_room(h,h->avdtp_media-1u):0;
}
void bt_host_audio(bt_host_t *h,const bt_a2dp_ops_t *ops,void *ctx)
{
    h->a2dp_ops=ops; h->a2dp_ctx=ctx;
    if(ops && h->l2cap.up) bt_l2cap_admit(&h->l2cap,BT_L2CAP_PSM_AVDTP,true);
}
void bt_host_receive(bt_host_t *h,const uint8_t *p,size_t n,size_t max_packet,
                     bt_host_report_t report,void *ctx)
{
    // A packet shorter than the endpoint's maximum ends its transfer.
    bool ends_transfer=max_packet!=0 && n<max_packet;
    if(h->wire_skip) {
        // The rest of a transfer whose header could not be real.
        if(ends_transfer) h->wire_skip=false;
        return;
    }
    // Assemble USB fragments once, then route complete ACL packets by handle.
    // Per-peer L2CAP reassembly allows interleaved continuation fragments.
    while(n && !h->failed) {
        if(h->wire_used<4) {
            h->wire[h->wire_used++]=*p++; n--;
            if(h->wire_used<4) continue;
            h->wire_need=4u+host_u16(h->wire+2);
            if(h->wire_need>sizeof(h->wire)) {
                if(!max_packet) { h->failed=true; return; }
                // Not a header: the assembler was out of step with the
                // transfers (bt_host.h). Drop to the end of this one.
                host_wire_empty(h); h->wire_resyncs++;
                h->wire_skip=!ends_transfer;
                return;
            }
        }
        while(n && h->wire_used<h->wire_need) { h->wire[h->wire_used++]=*p++; n--; }
        if(h->wire_used<h->wire_need) break;   // out of bytes: the end-of-transfer check below decides
        uint16_t handle=host_u16(h->wire)&0xfff;
        bool routed=false;
        for(unsigned i=0;i<BT_HOST_PEERS && !routed;i++) {
            bt_le_t *s=&h->peers[i].le;
            if(!s->connected || s->handle!=handle) continue;
            host_call_t call={.host=h,.peer=i,.report=report,.context=ctx};
            bt_le_receive(s,h->wire,h->wire_need,host_report,&call);
            routed=true;
        }
        // The Classic link's packets are its L2CAP's. This function has no
        // clock of its own: the time is the Classic link's, that of its
        // last tick. A packet for a handle nobody holds is dropped. A
        // channel opens only inside L2CAP's receive, so the AVDTP roles are
        // settled after each packet: the one that opened first is the one
        // seen first.
        if(!routed && h->classic.connected && h->classic.handle==handle) {
            bt_l2cap_receive(&h->l2cap,h->wire,h->wire_need,h->classic.now,host_l2cap_deliver,h);
            host_avdtp_channels(h,h->classic.now);
        }
        host_wire_empty(h);
    }
    // The transfer is over and the packet is not: what is held was never
    // the start of one.
    if(ends_transfer && h->wire_used) { host_wire_empty(h); h->wire_resyncs++; }
}
static void host_command(void *context,uint16_t op,const uint8_t *p,uint8_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    h->owner=c->peer==BT_HOST_PEERS?BT_HOST_OWNER_SCAN:BT_HOST_OWNER_PEER0+c->peer;
    h->credits=0; h->command_done=false;
    c->command(c->context,op,p,n);
}
static void host_classic_command(void *context,uint16_t op,const uint8_t *p,uint8_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    h->owner=BT_HOST_OWNER_CLASSIC;
    h->credits=0; h->command_done=false;
    c->command(c->context,op,p,n);
}
// ── The send slots (bt_host.h tx_from) ─────────────────────────────────────
//
// The transport says how many slots are in flight. They complete oldest
// first, so when fewer are in flight than were recorded, the oldest records
// are the ones that finished.
static void host_tx_settle(bt_host_t *h,unsigned in_flight)
{
    if(in_flight>BT_HOST_TX_SLOTS) in_flight=BT_HOST_TX_SLOTS;
    while(h->tx_count>in_flight) { h->tx_head=(uint8_t)((h->tx_head+1)%BT_HOST_TX_SLOTS); h->tx_count--; }
    h->tx_unknown=(uint8_t)(in_flight-h->tx_count);
}
static unsigned host_tx_free(const bt_host_t *h) { return BT_HOST_TX_SLOTS-h->tx_count-h->tx_unknown; }
static void host_tx_sent(bt_host_t *h,unsigned from)
{
    // A packet sent with no slot free fails the transport (xhci.c), so
    // there is nothing to record it against.
    if(!host_tx_free(h)) return;
    h->tx_from[(h->tx_head+h->tx_count)%BT_HOST_TX_SLOTS]=(uint8_t)from;
    h->tx_count++;
}
// Slots in flight for one sender, or (from == BT_HOST_PEERS + 1) for any LE
// peer. Slots the host did not fill count against every LE peer.
static unsigned host_tx_of(const bt_host_t *h,unsigned from)
{
    bool le=from!=BT_HOST_PEERS;
    unsigned n=le?h->tx_unknown:0;
    for(unsigned i=0;i<h->tx_count;i++) {
        unsigned who=h->tx_from[(h->tx_head+i)%BT_HOST_TX_SLOTS];
        if(who==from || (from==BT_HOST_PEERS+1 && who<BT_HOST_PEERS)) n++;
    }
    return n;
}
static void host_acl(void *context,const uint8_t *p,size_t n)
{
    host_call_t *c=context; c->host->tx_used=true;
    c->host->poll_peer=(c->peer+1)%BT_HOST_PEERS;
    if(n>=2) (void)bt_acl_sent(&c->host->acl,host_u16(p)&0xfff);
    host_tx_sent(c->host,c->peer);
    c->acl(c->context,p,n);
}
// One L2CAP PDU as one Classic ACL packet: the link's handle, packet
// boundary 0b10 (a first, automatically flushable fragment: LE's 0b00 on a
// Classic link needs a feature bit) and broadcast 0, the length, the PDU.
// L2CAP never hands over more than BT_L2CAP_MTU + 4; one the controller's
// Classic packets cannot carry is dropped and counted, never cut.
static void host_l2cap_send(void *context,const uint8_t *pdu,size_t n)
{
    host_call_t *c=context; bt_host_t *h=c->host;
    if(n>BT_L2CAP_MTU+4 || n>bt_acl_packet_size(&h->acl,false)) { h->l2cap_unsendable++; return; }
    uint16_t handle=h->classic.handle&0xfff;
    uint8_t packet[4+BT_L2CAP_MTU+4];
    packet[0]=(uint8_t)handle; packet[1]=(uint8_t)(handle>>8|0x20);
    packet[2]=(uint8_t)n; packet[3]=(uint8_t)(n>>8);
    for(size_t i=0;i<n;i++) packet[4+i]=pdu[i];
    (void)bt_acl_sent(&h->acl,handle);
    host_tx_sent(h,BT_HOST_PEERS);
    c->acl(c->context,packet,4+n);
    host_wipe(packet,4+n);
    // The media channel's PDU (its peer's CID heads it) is the pump's one
    // packet, handed to the controller now: its frames are sent. The
    // signalling channel's is AVDTP's parked message: sent now, not before.
    if(n>=4 && host_role_live(h,h->avdtp_media,h->avdtp_media_cid) &&
       host_u16(pdu+2)==h->l2cap.ch[h->avdtp_media-1u].dcid)
        bt_a2dp_sent(&h->a2dp);
    if(n>=4 && host_role_live(h,h->avdtp_sig,h->avdtp_sig_cid) &&
       host_u16(pdu+2)==h->l2cap.ch[h->avdtp_sig-1u].dcid && h->avdtp_sig_parked) {
        (void)bt_avdtp_sent(&h->avdtp,h->avdtp_sig_parked,host_avdtp_now(h));
        h->avdtp_sig_parked=0;
    }
}
// L2CAP may hand over a packet only when it costs the LE peers nothing:
// the count gives the Classic link a buffer of the controller's (it is the
// sender held to the count); a send slot is free with another left over,
// since an LE peer needs one at a time; and no LE peer is still holding a
// packet it could not send this tick. In a pool the two kinds share, the
// Classic link also leaves a buffer for each LE slot this host has,
// connected or not: an LE peer does not ask the count before it sends, so
// one that connects while Classic packets are out must find its buffer
// there. The count's own reserve covers only the LE links live now.
static bool host_l2cap_may_send(const bt_host_t *h)
{
    uint16_t handle=h->classic.handle&0xfff;
    if(!bt_acl_may_send(&h->acl,handle) || host_tx_free(h)<2) return false;
    if(bt_acl_shared(&h->acl)) {
        unsigned total=h->acl.classic_total;
        unsigned most=total>BT_HOST_PEERS?total-BT_HOST_PEERS:1;    // always one, as the count allows
        if(bt_acl_outstanding(&h->acl,handle)>=most) return false;
    }
    for(unsigned i=0;i<BT_HOST_PEERS;i++) if(h->peers[i].le.queue_count) return false;
    return true;
}
// Slot 2's services, each tick of a host whose Classic link is connected,
// after the LE peers.
static void host_services_tick(bt_host_t *h,uint64_t now,host_call_t *call)
{
    if(!h->l2cap.up) return;
    if(h->classic.encrypted!=h->l2cap_encrypted) {
        // L2CAP's clock first: a channel that was waiting for encryption
        // has its configuration deadline counted from the last `now` it
        // was handed, which must be this tick's.
        bt_l2cap_tick(&h->l2cap,now,false,NULL,NULL);
        bt_l2cap_set_encrypted(&h->l2cap,h->classic.encrypted);
        h->l2cap_encrypted=h->classic.encrypted;
    }
    host_sdp_step(h,now);
    host_sdp_answer_offer(h);
    host_audio_tick(h,now,call);
    bt_l2cap_tick(&h->l2cap,now,host_l2cap_may_send(h),host_l2cap_send,call);
    host_a2dp_settle(h);
}
static bool host_control(const bt_host_t *h,unsigned owner,bool usb_done)
{ return usb_done && (h->owner==owner || (!h->owner && h->credits)); }
// Holding the slot is leave to finish the command in flight, whatever
// credit its reply carried: a reply may give none back yet. Sending the
// next waits for a credit (the link's `no_credit`, set at its tick). After
// a command it gave up waiting for it holds the slot with none, and sends
// nothing until some reply brings one.
static bool host_classic_control(const bt_host_t *h,bool usb_done)
{
    if(h->owner!=BT_HOST_OWNER_CLASSIC) return host_control(h,BT_HOST_OWNER_CLASSIC,usb_done);
    return usb_done;
}
static bool host_owner_pending(const bt_host_t *h)
{
    if(h->owner==BT_HOST_OWNER_SCAN) return h->scan.pending;
    if(h->owner>=BT_HOST_OWNER_PEER0 && h->owner<BT_HOST_OWNER_PEER0+BT_HOST_PEERS)
        return h->peers[h->owner-BT_HOST_OWNER_PEER0].le.pending;
    if(h->owner==BT_HOST_OWNER_CLASSIC) return h->classic.pending;
    return false;
}
static void host_retire(bt_host_t *h,bool usb_done)
{
    if(usb_done && h->command_done && !host_owner_pending(h)) h->owner=BT_HOST_OWNER_NONE;
}
// Reconnection (bt_host.h): one page at classic_page_at, when the link is
// quiescent and nothing else is setting up or scanning.
#define BT_HOST_CLASSIC_SETTLED_MS 30000
static void host_classic_policy(bt_host_t *h,uint64_t now)
{
    bt_classic_t *c=&h->classic;
    if(!h->initialized || h->failed || !h->classic_available) return;
    // The ladder starts again only once a connection has LASTED: headphones
    // that answer the page and hang up moments later would otherwise be
    // paged every few seconds for as long as they did it.
    if(c->phase==BT_CLASSIC_READY) {
        if(!h->classic_ready_since) h->classic_ready_since=now?now:1;
        if(now-h->classic_ready_since>=BT_HOST_CLASSIC_SETTLED_MS) h->classic_backoff_ms=0;
        h->classic_page_at=0;
        return;
    }
    h->classic_ready_since=0;
    // An attempt under way, theirs or ours, takes the place of a page that
    // was owed: how IT ends decides whether another is. Without this, a
    // page armed by an unanswered one fired after their call had ended in
    // a way nothing should be paged after (a stale key).
    if(c->phase!=BT_CLASSIC_IDLE && c->phase!=BT_CLASSIC_STOPPED) { h->classic_page_at=0; return; }
    if(!h->classic_auto || !c->bond.valid || !bt_classic_quiescent(c)) return;
    if(!h->classic_page_at) {
        // Only an end the far end may come back from is paged again.
        if(c->phase!=BT_CLASSIC_STOPPED || !c->retryable) return;
        if(!h->classic_backoff_ms) h->classic_backoff_ms=5000;
        h->classic_page_at=now+h->classic_backoff_ms;
        h->classic_backoff_ms=h->classic_backoff_ms>=30000?60000:h->classic_backoff_ms*2;
        return;
    }
    if(now<h->classic_page_at || host_setup(h) || !bt_scan_quiescent(&h->scan)) return;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) if(h->peers[i].manager.scan_owned) return;
    h->classic_page_at=0;
    (void)bt_classic_request(c,"reconnect",9,NULL,now,NULL);
}
void bt_host_policy(bt_host_t *h,uint64_t now)
{
    for(unsigned i=0;i<BT_HOST_PEERS;i++) bt_manager_observe(&h->peers[i].manager,&h->peers[i].le,now);
    if(!h->initialized || h->failed || host_setup(h)) return;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        bt_host_peer_t *p=&h->peers[i];
        if(p->manager.scan_owned) {
            bt_manager_step(&p->manager,&p->le,&h->scan,now);
            return;
        }
    }
    if(!bt_scan_quiescent(&h->scan)) return;
    for(unsigned count=0;count<BT_HOST_PEERS;count++) {
        unsigned i=h->next_peer; h->next_peer=(i+1)%BT_HOST_PEERS;
        bt_host_peer_t *p=&h->peers[i];
        bt_manager_step(&p->manager,&p->le,&h->scan,now);
        if(p->manager.scan_owned) break;
    }
}
static void host_classic_tick(bt_host_t *h,uint64_t now,bool usb_done,host_call_t *call)
{
    h->classic.no_credit=!h->credits;
    bt_classic_tick(&h->classic,now,host_classic_control(h,usb_done),h->failed,host_classic_command,call);
    host_retire(h,usb_done);
    host_acl_links(h);
}
// The start-up round has reached DONE: the Classic link begins, if the
// controller can carry it.
static void host_classic_begin(bt_host_t *h)
{
    const bt_scan_t *s=&h->scan;
    h->classic_setup_refused=s->classic_setup_refused;
    h->classic_refused_opcode=s->classic_refused_opcode;
    h->classic_refused_status=s->classic_refused_status;
    bt_classic_init(&h->classic,s->address);
    // LMP features: bit 37 is "BR/EDR Not Supported"; without LE (bit 38)
    // the round ends before its Classic setup.
    h->classic_unavailable=(s->features[4]&0x20)?"the controller has no Classic radio":
        !(s->features[4]&0x40)?"the start-up round ended before the Classic setup":
        s->classic_setup_refused?"the controller refused a Classic setup command":NULL;
    h->classic_available=!h->classic_unavailable;
}
// The Classic link still owning the command slot this long after its
// command's deadline, with nothing of its own in flight: the reply never
// came, or it came and the USB transfer that carried the command never
// reported done. Either way the slot would be held for ever and the LE
// peers starved in silence; the controller is failed instead, as it is
// when an LE command goes unanswered.
#define BT_HOST_CLASSIC_REPLY_GRACE_MS 3000
void bt_host_tick(bt_host_t *h,uint64_t now,uint64_t now_us,bool usb_done,unsigned tx_in_flight,bool failed,
                  bt_scan_send_t command,bt_le_acl_send_t acl,bt_host_report_t report,void *ctx)
{
    h->failed|=failed;
    h->now_us=now_us;
    host_retire(h,usb_done);
    host_acl_links(h);   // before any peer below may send
    // The LE peers send one packet at a time between them: none while an
    // LE packet is in a send slot or the controller has not completed one,
    // or no slot is free. A Classic packet in flight is not theirs to wait
    // for while a slot is free.
    host_tx_settle(h,tx_in_flight);
    h->tx_used=host_tx_of(h,BT_HOST_PEERS+1)>0 || !host_tx_free(h);
    for(unsigned i=0;i<BT_HOST_PEERS;i++)
        if(h->peers[i].le.outstanding) h->tx_used=true;
    host_call_t call={.host=h,.peer=BT_HOST_PEERS,.command=command,.acl=acl,.report=report,.context=ctx};
    if(!h->initialized && !h->failed && h->scan.phase==BT_SCAN_STOPPED) {
        if(!h->init_retry_at) h->init_retry_at=now+2000;
        if(now>=h->init_retry_at) { bt_scan_start_le(&h->scan,now); h->init_retry_at=0; }
    }
    // The Classic link takes the first free turn when the controller is
    // waiting on one of its answers, and the last otherwise.
    bool classic=h->initialized && h->classic_available;
    bool classic_first=classic && bt_classic_urgent(&h->classic);
    if(classic_first) host_classic_tick(h,now,usb_done,&call);
    if(!bt_scan_quiescent(&h->scan)) {
        h->scan.credits=h->credits;
        bt_scan_tick(&h->scan,now,host_control(h,BT_HOST_OWNER_SCAN,usb_done),h->failed,host_command,&call);
        host_retire(h,usb_done);
        if(h->scan.phase==BT_SCAN_FAILED) {
            // A start-up round that failed outright is run again a few
            // times before the host is called failed (bt_host.h). STOPPED
            // is the state the retry above starts from. A failed transport
            // is not a round to repeat.
            if(!h->initialized && !h->failed) {
                // The next round rebuilds the scan from nothing, so what
                // this one died of is kept here.
                h->round_error=h->scan.error; h->round_opcode=h->scan.error_opcode;
                h->round_status=h->scan.error_status;
                h->round_timeout=bt_scan_timeout_text(&h->scan);
            }
            if(!h->initialized && !h->failed && ++h->init_failed_rounds<BT_HOST_INIT_ROUNDS) {
                h->scan.phase=BT_SCAN_STOPPED;
                // No reply came, so no reply gave the command credit back.
                // The next round opens with a reset, which a host may send
                // whatever it believes about credit.
                h->credits=1;
            }
            else h->failed=true;
        }
        if(!h->initialized && h->scan.phase==BT_SCAN_DONE) {
            h->initialized=true; h->scan.shared_controller=true;
            // The next scan clears what this round heard.
            if(h->scan.buffers_known)
                bt_acl_sizes(&h->acl,h->scan.acl_size,h->scan.acl_total,
                             h->scan.le_acl_size,h->scan.le_acl_total);
            host_classic_begin(h);
        }
    }
    unsigned first=h->poll_peer;
    for(unsigned count=0;count<BT_HOST_PEERS;count++) {
        unsigned i=(first+count)%BT_HOST_PEERS;
        bt_le_t *s=&h->peers[i].le; call.peer=i; s->credits=h->credits;
        // A peer cleaning up waits for its own packets to leave the send
        // slots, and nobody else's: not the Classic link's, and not the
        // other LE peer's (AUDIO_CLASSIC.md § T2).
        bt_le_tick(s,now,host_control(h,BT_HOST_OWNER_PEER0+i,usb_done),
                   s->phase==BT_LE_CLEANUP?!host_tx_of(h,i):!h->tx_used,
                   h->failed,host_command,host_acl,host_report,&call);
        host_retire(h,usb_done);
        host_acl_links(h);
        if(s->phase==BT_LE_FAILED) h->failed=true;
    }
    if(classic && !classic_first) host_classic_tick(h,now,usb_done,&call);
    // After the LE peers have had their turn at the send slots.
    if(classic && !h->failed) host_services_tick(h,now,&call);
    if(classic && h->owner==BT_HOST_OWNER_CLASSIC && !h->classic.pending &&
       now>=h->classic.command_deadline+BT_HOST_CLASSIC_REPLY_GRACE_MS) h->failed=true;
    // A controller failure affects every connection, even if detected by the
    // last peer in this pass. Deliver releases before returning to input code.
    if(h->failed) {
        volatile uint8_t *wipe=h->wire;
        for(size_t i=0;i<sizeof(h->wire);i++) wipe[i]=0;
        h->wire_used=h->wire_need=0;
    }
    if(h->failed) for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        call.peer=i;
        bt_le_tick(&h->peers[i].le,now,false,false,true,host_command,host_acl,host_report,&call);
    }
    if(h->failed && classic) host_classic_tick(h,now,usb_done,&call);
    // A failed controller runs no services: the pump lets the seam go, so
    // the null sink plays and a player's writes are not held for ever. Its
    // counters and reasons stay, for the status file to show.
    if(h->failed && h->a2dp_ops && h->a2dp.attached) bt_a2dp_detach(&h->a2dp,h->a2dp_ops,h->a2dp_ctx);
    bt_host_policy(h,now);
    host_classic_policy(h,now);
}
bool bt_host_scan(bt_host_t *h,uint64_t now)
{
    if(!h->initialized || h->failed || host_setup(h) || !bt_scan_start_le(&h->scan,now)) return false;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        h->peers[i].manager.scan_owned=false;
        h->peers[i].manager.next_attempt=now+60000;
    }
    return true;
}
bool bt_host_scan_classic(bt_host_t *h,uint64_t now)
{
    if(!h->initialized || h->failed || host_setup(h)) return false;
    if(!bt_classic_quiescent(&h->classic) && h->classic.phase!=BT_CLASSIC_READY) return false;
    if(!bt_scan_start_classic(&h->scan,now)) return false;
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        h->peers[i].manager.scan_owned=false;
        h->peers[i].manager.next_attempt=now+60000;
    }
    return true;
}
bool bt_host_classic_bond(bt_host_t *h,const bt_classic_bond_t *bond,uint64_t now)
{
    if(!h->initialized || !h->classic_available) return false;
    bool ours=bond && bond->valid;
    for(unsigned i=0;ours && i<6;i++) ours=bond->local[i]==h->classic.local[i];
    bt_classic_set_bond(&h->classic,ours?bond:NULL);
    h->classic_auto=ours;
    h->classic_page_at=ours?(now?now:1):0;   // 0 means none owed
    return true;
}
static bool host_address_equal(uint8_t a_type,const uint8_t a[6],uint8_t b_type,const uint8_t b[6])
{
    if(a_type!=b_type) return false;
    for(unsigned i=0;i<6;i++) if(a[i]!=b[i]) return false;
    return true;
}
static bool host_space(char ch) { return ch==' ' || ch=='\t' || ch=='\n' || ch=='\r'; }
static bool host_word(const char *p,size_t n,const char *word)
{
    size_t i=0;
    for(;word[i];i++) if(i==n || p[i]!=word[i]) return false;
    return i==n || host_space(p[i]);
}
static int host_hex(char ch)
{
    return ch>='0' && ch<='9'?ch-'0':ch>='a' && ch<='f'?ch-'a'+10:ch>='A' && ch<='F'?ch-'A'+10:-1;
}
// The address in a command, the first "aa:bb:cc:dd:ee:ff" in it, least
// significant byte first as the scan keeps it.
static bool host_find_address(const char *p,size_t n,uint8_t out[6])
{
    for(size_t at=0;at+17<=n;at++) {
        bool good=true;
        for(unsigned i=0;i<6 && good;i++) {
            int hi=host_hex(p[at+3*i]),lo=host_hex(p[at+3*i+1]);
            good=hi>=0 && lo>=0 && (i==5 || p[at+3*i+2]==':');
            if(good) out[5-i]=(uint8_t)(hi<<4|lo);
        }
        if(good) return true;
    }
    return false;
}
// What the last scan heard at this address: its first Classic entry and
// its first LE entry, each NULL when there is none.
static void host_heard(const bt_host_t *h,const uint8_t address[6],
                       const bt_scan_device_t **classic,const bt_scan_device_t **le)
{
    *classic=*le=NULL;
    for(unsigned i=0;i<h->scan.count && i<BT_SCAN_DEVICES;i++) {
        const bt_scan_device_t *d=&h->scan.devices[i];
        bool same=true;
        for(unsigned j=0;j<6 && same;j++) same=d->address[j]==address[j];
        if(!same) continue;
        if(!d->le && !*classic) *classic=d;
        if(d->le && !*le) *le=d;
    }
}
static const char host_no_classic[]="no Classic slot";
// "slot 2 COMMAND": the Classic link's words, refused with a reason the
// status file keeps.
static bool host_classic_command_text(bt_host_t *h,const char *p,size_t n,uint64_t now)
{
    while(n && host_space(*p)) { p++; n--; }
    while(n && host_space(p[n-1])) n--;
    bool forget=host_word(p,n,"forget"), disconnect=host_word(p,n,"disconnect");
    bool pages=host_word(p,n,"bond") || host_word(p,n,"connect") || host_word(p,n,"reconnect");
    // Forgetting stays possible on a failed controller, as it does for LE:
    // the bond file is the user's to clear whatever the adapter is doing.
    if(!h->initialized) { h->refused_why="the controller is not ready"; return false; }
    if(h->failed && !forget) { h->refused_why="the Bluetooth controller failed; reboot required"; return false; }
    if(!h->classic_available) { h->refused_why=host_no_classic; return false; }
    // Until the bond file has been read the link does not know what it
    // holds, and the read, when it lands, would overwrite whatever a
    // command had done meanwhile: a bond just made, or one just forgotten.
    if(!h->classic.bond_loaded) { h->refused_why="the saved bond has not been read yet; try again in a moment"; return false; }
    if(pages && host_setup(h)) { h->refused_why="busy: an LE slot is setting up"; return false; }
    // A dual-mode device is heard at one address as both kinds; the
    // Classic entry is the one this slot is about.
    uint8_t address[6];
    const bt_scan_device_t *classic=NULL,*le=NULL;
    if(host_find_address(p,n,address)) host_heard(h,address,&classic,&le);
    const char *why=NULL;
    bool took=bt_classic_request(&h->classic,p,n,classic?classic:le,now,&why);
    // Forget and disconnect end automatic connection whatever the link
    // says to them (disconnecting a link that is down is refused, and the
    // person still meant "stop").
    if(forget || disconnect) { h->classic_auto=false; h->classic_page_at=0; }
    if(!took) { h->refused_why=why; return false; }
    if(host_word(p,n,"bond") || host_word(p,n,"reconnect")) { h->classic_auto=true; h->classic_page_at=0; }
    return true;
}
bool bt_host_command(bt_host_t *h,const char *p,size_t n,uint64_t now)
{
    h->duplicate_slot=0; h->refused_why=NULL;
    while(n && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) { p++; n--; }
    unsigned slot=0;
    if(n>=7 && p[0]=='s' && p[1]=='l' && p[2]=='o' && p[3]=='t' && p[4]==' ' &&
       p[5]=='0'+BT_HOST_CLASSIC_SLOT && p[6]==' ')
        return host_classic_command_text(h,p+7,n-7,now);
    if(n>=7 && p[0]=='s' && p[1]=='l' && p[2]=='o' && p[3]=='t' && p[4]==' ') {
        if(p[5]<'0' || p[5]>='0'+BT_HOST_PEERS || p[6]!=' ') return false;
        slot=p[5]-'0'; p+=7; n-=7;
    }
    bt_host_peer_t *peer=&h->peers[slot];
    bool connect=n>=4 && ((p[0]=='c' && p[1]=='o') || (p[0]=='b' && p[1]=='o') ||
                         (p[0]=='r' && p[1]=='e'));
    if(connect && (!h->initialized || h->failed || host_setup(h))) return false;
    if(connect) {
        // Validate on scratch state so a duplicate request cannot mutate the
        // selected slot's reconnect policy or retained failure diagnostics.
        bt_le_t candidate={.shared_controller=true,.bond=peer->le.bond};
        bool valid=bt_le_request(&candidate,p,n,now);
        // An address the last scan heard only as Classic belongs to slot 2.
        const bt_scan_device_t *classic=NULL,*le=NULL;
        uint8_t address[6];
        if(valid && host_find_address(p,n,address)) host_heard(h,address,&classic,&le);
        if(classic && !le) { valid=false; h->refused_why="that is a Classic address: use slot 2"; }
        for(unsigned i=0;i<BT_HOST_PEERS && valid;i++) if(i!=slot) {
            const bt_le_t *other=&h->peers[i].le;
            const bt_le_bond_t *bond=&other->bond;
            // A sleeping peer still owns its saved identity. Without an IRK,
            // retain ownership of its last air address as well.
            bool duplicate=other->connected && host_address_equal(other->address_type,other->peer,
                candidate.address_type,candidate.peer);
            if(bond->valid) duplicate|=host_address_equal(bond->address_type,bond->peer,
                candidate.address_type,candidate.peer) || (!bond->has_irk &&
                host_address_equal(bond->last_address_type,bond->last_peer,candidate.address_type,candidate.peer));
            if(duplicate) { valid=false; h->duplicate_slot=i+1; }
        }
        volatile uint8_t *wipe=(volatile uint8_t *)&candidate;
        for(size_t i=0;i<sizeof(candidate);i++) wipe[i]=0;
        if(!valid) return false;
    }
    return bt_manager_command(&peer->manager,&peer->le,&h->scan,p,n,now);
}
// Slot 2's services, after the link's own lines: L2CAP's text, the query
// on this connection, what an earlier connection's query found when this
// one has found nothing yet, and the questions the headphones asked us.
static size_t host_services_status(const bt_host_t *h,char *out,size_t used,size_t cap)
{
    used+=bt_l2cap_status(&h->l2cap,out+used,cap-used);
    if(used>=cap-1) return used;
    int n;
    if(bt_sdp_client_state(&h->sdp)!=BT_SDP_IDLE) {
        used+=bt_sdp_client_status(&h->sdp,out+used,cap-used);
        n=0;
    }
    else if(h->sdp_not_made) n=snprintf(out+used,cap-used,"sdp: not made: %s\n",h->sdp_not_made);
    else if(h->sdp_channel) n=snprintf(out+used,cap-used,"sdp: opening its channel\n");
    else n=snprintf(out+used,cap-used,"sdp: not asked\n");
    if(n<0) return used;
    used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    if(h->sdp_result_valid && !h->sdp_result_current && used<cap-1) {
        n=snprintf(out+used,cap-used,"earlier connection's %s",h->sdp_result_line);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(used<cap-1) {
        n=snprintf(out+used,cap-used,"sdp requests answered: %u dropped: %u\n",
                   (unsigned)h->sdp_answered,(unsigned)h->sdp_dropped);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->l2cap_unsendable && used<cap-1) {
        n=snprintf(out+used,cap-used,"l2cap pdus longer than a classic acl packet, dropped: %u\n",
                   (unsigned)h->l2cap_unsendable);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    // Slot 2's audio, with a sink: AVDTP's text and the pump's, which
    // channel holds each role (an index, as L2CAP's lines above name them),
    // and what arrived where nothing listens.
    if(!h->a2dp_ops || used>=cap-1) return used;
    used+=bt_avdtp_status(&h->avdtp,out+used,cap-used);
    if(used>=cap-1) return used;
    // A failed controller stopped the pump where it stood: what it says
    // below is how things were then.
    if(h->failed) {
        n=snprintf(out+used,cap-used,"a2dp: stopped: the Bluetooth controller failed; reboot required\n");
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
        if(used>=cap-1) return used;
    }
    used+=bt_a2dp_status(&h->a2dp,out+used,cap-used);
    if(used>=cap-1) return used;
    char sig[8]="none", media[8]="none";
    if(h->avdtp_sig) snprintf(sig,sizeof sig,"%u",h->avdtp_sig-1u);
    if(h->avdtp_media) snprintf(media,sizeof media,"%u",h->avdtp_media-1u);
    n=snprintf(out+used,cap-used,"avdtp channels: signalling %s media %s%s\n"
               "avdtp pdus received off the signalling channel (media, or a channel being closed), dropped: %u\n",
               sig,media,h->avdtp_asking?(h->avdtp_asking_media?", asking for media":", asking for signalling"):"",
               (unsigned)h->avdtp_media_dropped);
    if(n<0) return used;
    used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    return used;
}
size_t bt_host_status(const bt_host_t *h,char *out,size_t cap)
{
    if(!cap) return 0;
    int n=snprintf(out,cap,"controller: %s\nstart-up rounds failed: %u of %u\n"
        "last failed start-up round: %s (opcode %04x status %02x)%s\n"
        "ACL framing resyncs: %u\ncommands: slot N COMMAND (default slot 0)\n",
        h->failed?"failed; reboot required":h->initialized?"ready":"initializing",
        h->init_failed_rounds,BT_HOST_INIT_ROUNDS,
        h->round_error?h->round_error:"none",h->round_opcode,h->round_status,
        h->round_timeout?h->round_timeout:"",h->wire_resyncs);
    if(n<0) return 0;
    size_t used=(size_t)n<cap?(size_t)n:cap-1;
    if(used<cap-1) {
        const bt_acl_t *a=&h->acl;
        if(!a->known) n=snprintf(out+used,cap-used,"acl pools: not yet read\n");
        else if(bt_acl_shared(a)) n=snprintf(out+used,cap-used,
            "acl pools: one, classic and le together, %u x %u bytes; in use %u, kept for le %u; "
            "stray completions %u, bad completions %u\n",
            a->classic_total,a->classic_size,bt_acl_in_use(a,false),
            bt_acl_reserved_for_le(a),a->strays,h->acl_bad_completions);
        else n=snprintf(out+used,cap-used,
            "acl pools: classic %u x %u bytes, in use %u; le %u x %u bytes, in use %u; "
            "stray completions %u, bad completions %u\n",
            a->classic_total,a->classic_size,bt_acl_in_use(a,false),
            a->le_total,a->le_size,bt_acl_in_use(a,true),a->strays,h->acl_bad_completions);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->duplicate_slot && used<cap-1) {
        n=snprintf(out+used,cap-used,"last command refused: peer belongs to slot %u\n",h->duplicate_slot-1);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->refused_why && used<cap-1) {
        n=snprintf(out+used,cap-used,"last command refused: %s%s%s\n",h->refused_why,
                   h->refused_why==host_no_classic?": ":"",
                   h->refused_why==host_no_classic && h->classic_unavailable?h->classic_unavailable:"");
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    for(unsigned i=0;i<BT_HOST_PEERS && used<cap-1;i++) {
        n=snprintf(out+used,cap-used,"\nslot %u:\n",i);
        if(n<0) break;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
        used+=bt_le_status(&h->peers[i].le,out+used,cap-used);
        used+=bt_manager_status(&h->peers[i].manager,out+used,cap-used);
    }
    if(used>=cap-1) return used;
    // Slot 2. The link's own text never shows a key (bt_classic.h).
    const bt_classic_t *c=&h->classic;
    if(!h->initialized)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: the controller is not ready\n",
                   BT_HOST_CLASSIC_SLOT);
    else if(!h->classic_available && h->classic_setup_refused)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: %s (opcode %04x status %02x)\n",
                   BT_HOST_CLASSIC_SLOT,h->classic_unavailable,h->classic_refused_opcode,h->classic_refused_status);
    else if(!h->classic_available)
        n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\nunavailable: %s\n",
                   BT_HOST_CLASSIC_SLOT,h->classic_unavailable?h->classic_unavailable:"unknown");
    else n=snprintf(out+used,cap-used,"\nslot %u:\nkind: classic\n",BT_HOST_CLASSIC_SLOT);
    if(n<0) return used;
    used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    if(h->initialized && h->classic_available && used<cap-1) {
        used+=bt_classic_status(c,out+used,cap-used);
        if(used>=cap-1) return used;
        // The link's clock: the time of its last tick.
        uint64_t wait=h->classic_page_at>c->now?h->classic_page_at-c->now:0;
        if(!h->classic_auto) n=snprintf(out+used,cap-used,"automatic: no\n");
        else if(c->phase==BT_CLASSIC_READY) n=snprintf(out+used,cap-used,"automatic: yes; connected\n");
        else if(h->classic_page_at) n=snprintf(out+used,cap-used,"automatic: yes; next page in %lu ms\n",(unsigned long)wait);
        else n=snprintf(out+used,cap-used,"automatic: yes; waiting for the headphones to call\n");
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->classic_storage && used<cap-1) {
        n=snprintf(out+used,cap-used,"bond storage: %s\n",h->classic_storage);
        if(n<0) return used;
        used+=(size_t)n<cap-used?(size_t)n:cap-used-1;
    }
    if(h->initialized && h->classic_available && used<cap-1) used=host_services_status(h,out,used,cap);
    return used;
}
