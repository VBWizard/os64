// bt_a2dp.c — the A2DP pump. bt_a2dp.h is the contract;
// docs/design/pending/AUDIO_A2DP.md § Part P the design.
//
// A media packet: the RTP header as Android's stack writes it (avdt_defs.h,
// bta_av_aact.cc), then A2DP's SBC payload header (a2dp_sbc.cc,
// a2dp_sbc_constants.h):
//
//   byte 0      0x80: RTP version 2, no padding, no extension, no contributors
//   byte 1      the marker bit (clear) and payload type 96
//   bytes 2-3   sequence number, big-endian, +1 a packet, from 0 a session
//   bytes 4-7   timestamp, big-endian: the PCM frames before this packet
//   bytes 8-11  SSRC, big-endian, a constant
//   byte 12     the SBC payload header: fragmented (7), start (6), last (5),
//               reserved (4), frames that follow (3-0). We never fragment.
//   then the frames, whole.
#include "driver/system/usb/bt_a2dp.h"
#include "strings/sprintf.h"

_Static_assert(BT_A2DP_FRAMES_MAX <= 0x0f, "the payload header counts frames in four bits");

static void a2_wipe(void *p, size_t n) { uint8_t *v=p; while(n--) *v++=0; }

// The states in which the pump is the seam's sink: the stream is set up
// and may be started, is streaming, or is moving between the two.
static bool a2_sink_states(bt_avdtp_state_t s) { return s>=BT_AVDTP_OPEN && s<=BT_AVDTP_SUSPENDING; }

static void a2_put32(uint8_t *at, uint32_t v)
{ at[0]=(uint8_t)(v>>24); at[1]=(uint8_t)(v>>16); at[2]=(uint8_t)(v>>8); at[3]=(uint8_t)v; }

// What lies beyond the pull: their latest Delay Report (units of 100 us),
// else the default.
static uint32_t a2_delay(bt_a2dp_t *p, const bt_avdtp_t *a)
{
    uint32_t d=0;
    p->delay_reported=bt_avdtp_delay(a,&d);
    p->delay_us=p->delay_reported ? d*100u : BT_A2DP_DEFAULT_DELAY_US;
    return p->delay_us;
}

// Attaching sets the seam's delay to zero, so it is told at once.
static void a2_attach(bt_a2dp_t *p, uint32_t delay_us, const bt_a2dp_ops_t *ops, void *ctx)
{
    ops->attach(ctx);
    p->attached=true;
    ops->delay(ctx,delay_us);
    p->delay_told_us=delay_us;
}

static void a2_detach(bt_a2dp_t *p, const bt_a2dp_ops_t *ops, void *ctx)
{
    ops->detach(ctx);
    p->attached=false;
    p->streaming=false;
}

// A failed sink. The caller has made sure the pump is attached: the seam's
// failure belongs to its sink, and stays until that sink detaches. Closing
// the stream is bt_a2dp_step's, from AVDTP's state.
static void a2_fail(bt_a2dp_t *p, const char *why, const bt_a2dp_ops_t *ops, void *ctx)
{
    p->failed=true;
    p->why=why;
    p->streaming=false;
    ops->failed(ctx,why);
}

// A new session: nothing of the last one's starting, quiet, clock, RTP
// counts or waiting packet carries over, and the encoder is begun with its
// settings. A sink
// failed in an earlier session is cleared by a good begin: detaching is
// what clears the seam's failure, and attaching again is the state's to
// decide. A begin that fails is a failed sink, attached for it.
static void a2_session(bt_a2dp_t *p, const bt_avdtp_sbc_t *s, uint32_t delay_us,
                       const bt_a2dp_ops_t *ops, void *ctx)
{
    p->session=true;
    p->sessions_begun++;
    p->hold_until=0; p->start_rejects=0; p->gave_up=false; p->displaced=false;
    p->suspend_refused=false; p->quiet_since=0; p->stopped=false; p->streaming=false;
    p->pending=false; p->sequence=0; p->timestamp=0; p->slow_run=0;
    p->settings=(sbc_settings_t){ .rate=s->rate, .channels=s->channels, .joint=s->joint,
                                  .blocks=s->blocks, .subbands=s->subbands, .snr=s->snr, .bitpool=s->bitpool };
    p->frame_bytes=sbc_frame_bytes(&p->settings);
    p->encoder_ok=p->frame_bytes && ops->encoder_begin(ctx,&p->settings);
    if(p->encoder_ok) {
        if(p->failed) {
            p->failed=false; p->why=0;
            if(p->attached) a2_detach(p,ops,ctx);
        }
        return;
    }
    if(!p->attached) a2_attach(p,delay_us,ops,ctx);
    a2_fail(p,"the encoder did not begin",ops,ctx);
}

// A pull that made nothing: the next one that makes something anchors the
// clock again, and the quiet spell has begun if it had not.
static void a2_quiet(bt_a2dp_t *p, uint64_t now)
{
    p->stopped=true;
    if(!p->quiet_since) p->quiet_since=now;
}

void bt_a2dp_init(bt_a2dp_t *p) { a2_wipe(p,sizeof(*p)); }

void bt_a2dp_reset(bt_a2dp_t *p, const bt_a2dp_ops_t *ops, void *ctx)
{
    if(p->attached) ops->detach(ctx);
    a2_wipe(p,sizeof(*p));
}

void bt_a2dp_step(bt_a2dp_t *p, bt_avdtp_t *a, uint64_t now, const bt_a2dp_ops_t *ops, void *ctx)
{
    uint64_t ms=now/1000;
    p->now=now;

    // AVDTP's edges. They start again at zero when AVDTP is reset, which
    // the host pairs with bt_a2dp_reset; a count found below what was read
    // is such a reset not passed on, and is read afresh.
    const bt_avdtp_edges_t *e=bt_avdtp_edges(a);
    if(e->sessions<p->seen.sessions || e->their_starts<p->seen.their_starts || e->their_suspends<p->seen.their_suspends
       || e->start_rejects<p->seen.start_rejects || e->suspend_rejects<p->seen.suspend_rejects)
        a2_wipe(&p->seen,sizeof(p->seen));
    bool new_session=e->sessions!=p->seen.sessions;
    uint32_t start_rejects=e->start_rejects-p->seen.start_rejects;
    uint32_t suspend_rejects=e->suspend_rejects-p->seen.suspend_rejects;
    bool their_suspend=e->their_suspends!=p->seen.their_suspends;
    bool their_start=e->their_starts!=p->seen.their_starts;
    p->seen=*e;

    // Holding a Start back: after their Suspend, and after each rejected
    // Start, doubling, until one reject too many gives the session up.
    if(their_suspend && now+BT_A2DP_THEIRS_US>p->hold_until) {
        p->hold_until=now+BT_A2DP_THEIRS_US;
        p->starts_held++;
    }
    if(start_rejects) {
        p->start_rejects_seen+=start_rejects;
        uint32_t r=p->start_rejects+start_rejects;
        p->start_rejects=(uint8_t)(r>BT_A2DP_HOLDS+1u ? BT_A2DP_HOLDS+1u : r);
        if(p->start_rejects>BT_A2DP_HOLDS) p->gave_up=true;
        else {
            uint64_t until=now+((uint64_t)BT_A2DP_HOLD_US<<(p->start_rejects-1));
            if(until>p->hold_until) p->hold_until=until;
            p->starts_held++;
        }
    }
    if(suspend_rejects) { p->suspend_rejects_seen+=suspend_rejects; p->suspend_refused=true; }

    // The session: settings agreed, which moves the sessions edge. One made
    // and ended between two passes moved it with nothing left to begin.
    const bt_avdtp_sbc_t *s=bt_avdtp_settings(a);
    uint32_t delay=a2_delay(p,a);
    if(!s) { p->session=false; p->encoder_ok=false; }
    else if(new_session) a2_session(p,s,delay,ops,ctx);

    // A failed sink closes the stream, once AVDTP is in a state that takes
    // a Close; until then it waits, attached and making nothing.
    bt_avdtp_state_t st=bt_avdtp_state(a);
    if(p->failed && (st==BT_AVDTP_OPEN || st==BT_AVDTP_STREAMING) && bt_avdtp_close(a,ms)) {
        p->closes_asked++;
        st=bt_avdtp_state(a);
    }

    // The seam's sink, from the state; told the delay after every attach
    // and whenever their report changes it.
    if(!a2_sink_states(st)) p->displaced=false;
    bool want=p->failed || (a2_sink_states(st) && p->encoder_ok && !p->gave_up && !p->displaced);
    if(want && !p->attached) a2_attach(p,delay,ops,ctx);
    else if(!want && p->attached) a2_detach(p,ops,ctx);
    if(p->attached && delay!=p->delay_told_us) { ops->delay(ctx,delay); p->delay_told_us=delay; }

    if(st==BT_AVDTP_CONNECTED && bt_avdtp_may_begin(a,ms)) bt_avdtp_begin(a,ms);

    bool healthy=p->attached && !p->failed;
    if(st==BT_AVDTP_OPEN && healthy && now>=p->hold_until && ops->has_sound(ctx) && bt_avdtp_start(a,ms))
        st=bt_avdtp_state(a);

    // Streaming begins, and the clock is anchored, at the pass that finds
    // AVDTP streaming with the sink attached and healthy; and again after
    // their Start, which may have followed their Suspend between two passes
    // so that the state alone never showed the stream stop.
    if(st==BT_AVDTP_STREAMING && healthy) {
        if(!p->streaming || their_start) {
            p->streaming=true;
            p->anchor=now; p->frames_since_anchor=0; p->anchors++;
            p->stopped=false;
        }
    } else p->streaming=false;

    // Quiet: silence or a pause, seen by the pulls, or nothing that can be
    // sent at all. Three seconds of it suspends, unless our last Suspend
    // was refused and sound has not come since. Leaving STREAMING ends both
    // the spell and the refusal, which belonged to that stream; their Start
    // begins a new spell, since it may have come between two passes after
    // a Suspend no pass saw accepted.
    if(their_start) p->quiet_since=0;
    if(st==BT_AVDTP_STREAMING) {
        if(!p->streaming && !p->quiet_since) p->quiet_since=now;
        if(p->quiet_since && now-p->quiet_since>=BT_A2DP_QUIET_US && !p->suspend_refused)
            bt_avdtp_suspend(a,ms);
    } else {
        p->quiet_since=0;
        p->suspend_refused=false;
    }
}

size_t bt_a2dp_packet(bt_a2dp_t *p, uint64_t now, uint16_t mtu, uint8_t *out, size_t cap,
                      const bt_a2dp_ops_t *ops, void *ctx)
{
    if(!p->streaming || p->pending) return 0;
    // Due when the frames handed over since the anchor have played out,
    // computed whole from the count each time, so nothing accumulates.
    uint64_t due=p->anchor+p->frames_since_anchor*1000000u/p->settings.rate;
    if(now<due) return 0;
    uint32_t fb=p->frame_bytes;
    if(mtu<BT_A2DP_HEADER_BYTES+fb) { a2_fail(p,"the media channel's MTU is smaller than a frame",ops,ctx); return 0; }
    if(cap<BT_A2DP_HEADER_BYTES+fb) return 0;
    uint32_t n=(mtu-BT_A2DP_HEADER_BYTES)/fb;
    uint32_t room=(uint32_t)((cap-BT_A2DP_HEADER_BYTES)/fb);
    if(n>room) n=room;
    if(n>BT_A2DP_FRAMES_MAX) n=BT_A2DP_FRAMES_MAX;
    uint32_t samples=n*BT_A2DP_SAMPLES;

    bt_a2dp_pulled_t got=ops->pull(ctx,p->pcm,samples);
    // Another sink is the sink: the seam has already moved on, so this is
    // bookkeeping, and the pump does not take the seam back while the
    // session stays as it is.
    if(!got.yours) { p->attached=false; p->displaced=true; p->streaming=false; return 0; }
    if(got.generation!=p->generation) { p->generation=got.generation; p->generations++; }
    // A paused pull left the PCM untouched: none of it may be sent.
    if(got.paused) { a2_quiet(p,now); return 0; }
    bool dry=got.frames==0;
    if(dry) {
        // After a refused Suspend nothing is sent while there is nothing to send.
        if(p->suspend_refused) { a2_quiet(p,now); return 0; }
        if(!p->quiet_since) p->quiet_since=now;
    } else {
        p->quiet_since=0;
        p->suspend_refused=false;
    }
    // Sending again after a spell that sent nothing anchors the clock; so
    // does a packet more than BT_A2DP_SLIP_US late, which is counted. A
    // backlog sent at once would overflow their buffer.
    if(p->stopped) { p->stopped=false; p->anchor=now; p->frames_since_anchor=0; p->anchors++; }
    else if(now-due>BT_A2DP_SLIP_US) { p->slips++; p->anchor=now; p->frames_since_anchor=0; p->anchors++; }

    uint64_t t0=ops->clock_us(ctx);
    for(uint32_t i=0;i<n;i++) {
        size_t at=BT_A2DP_HEADER_BYTES+(size_t)i*fb;
        if(!ops->encoder_encode(ctx,p->pcm+(size_t)i*BT_A2DP_SAMPLES*2,out+at,cap-at)) {
            a2_fail(p,"the encoder refused a frame",ops,ctx);
            return 0;
        }
    }
    uint64_t t1=ops->clock_us(ctx);
    if(t1>t0 && t1-t0>BT_A2DP_SLOW_US) {
        p->slow++;
        if(++p->slow_run>=BT_A2DP_SLOW_RUN) {
            a2_fail(p,"eight packets in a row took over 5 ms each to encode",ops,ctx);
            return 0;
        }
    } else p->slow_run=0;

    out[0]=0x80;
    out[1]=BT_A2DP_PAYLOAD_TYPE;
    out[2]=(uint8_t)(p->sequence>>8); out[3]=(uint8_t)p->sequence;
    a2_put32(out+4,p->timestamp);
    a2_put32(out+8,BT_A2DP_SSRC);
    out[12]=(uint8_t)(n&0x0f);
    p->sequence++;
    p->timestamp+=samples;
    if(dry) p->dry_packets++;
    p->frames_a_packet=n;
    p->pending=true;
    p->pending_frames=samples;
    p->pending_anchor=p->anchors;
    return BT_A2DP_HEADER_BYTES+(size_t)n*fb;
}

// The frames count toward the clock only under the anchor they were made
// under: a stream started again since begins its count afresh.
void bt_a2dp_sent(bt_a2dp_t *p)
{
    if(!p->pending) return;
    p->pending=false;
    if(p->pending_anchor==p->anchors) p->frames_since_anchor+=p->pending_frames;
    p->packets_sent++;
    p->frames_sent+=p->pending_frames;
}

const char *bt_a2dp_state_name(const bt_a2dp_t *p)
{
    if(p->failed) return "failed";
    if(!p->attached) return "detached";
    if(p->streaming) return p->quiet_since ? "quiet" : "streaming";
    if(p->now<p->hold_until) return "held";
    return "attached";
}

size_t bt_a2dp_status(const bt_a2dp_t *p, char *out, size_t cap)
{
    if(!cap) return 0;
    size_t n=0;
    int w=snprintf(out,cap,"a2dp: %s%s%s%s%s\n",bt_a2dp_state_name(p),p->failed ? ": " : "",p->failed && p->why ? p->why : "",
                   p->gave_up ? ", gave up after rejected starts" : "",p->displaced ? ", another sink took the seam" : "");
    if(w>0) n+=(size_t)w;
    if(p->session && n<cap) {
        w=snprintf(out+n,cap-n,"a2dp: sbc %u Hz %s bitpool %u, %u-byte frames, %u frames a packet, encoder %s\n",
                   (unsigned)p->settings.rate,p->settings.joint ? "joint stereo" : "stereo",(unsigned)p->settings.bitpool,
                   (unsigned)p->frame_bytes,(unsigned)p->frames_a_packet,p->encoder_ok ? "begun" : "not begun");
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"a2dp: packets sent %lu, frames %lu, dry packets %u, slips %u, slow encodes %u, flushes seen %u\n",
                   (unsigned long)p->packets_sent,(unsigned long)p->frames_sent,(unsigned)p->dry_packets,(unsigned)p->slips,
                   (unsigned)p->slow,(unsigned)p->generations);
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"a2dp: starts held back %u, start rejects %u, suspend rejects %u, closes asked %u, sessions %u\n",
                   (unsigned)p->starts_held,(unsigned)p->start_rejects_seen,(unsigned)p->suspend_rejects_seen,
                   (unsigned)p->closes_asked,(unsigned)p->sessions_begun);
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"a2dp: delay %u.%u ms (%s)\n",(unsigned)(p->delay_us/1000),
                   (unsigned)(p->delay_us%1000/100),p->delay_reported ? "their report" : "default");
        if(w>0) n+=(size_t)w;
    }
    return n<cap ? n : cap-1;
}
