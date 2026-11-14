// bt_avdtp.c — AVDTP signalling, both roles. bt_avdtp.h is the contract;
// docs/design/pending/AUDIO_A2DP.md § Part V the design. The wire is AVDTP
// 1.3 § 8 as BlueZ (profiles/audio/avdtp.c, a2dp-codecs.h) and Android's
// stack (stack/avdt/avdt_msg.cc) read and write it:
//
//   byte 0: transaction label (bits 7-4), packet type (3-2), message type (1-0)
//   single: byte 1 the signal (bits 5-0), then the payload
//   start:  byte 1 the number of packets, the start included; byte 2 the signal
//   continue, end: byte 0 alone, then more payload
//
// A SEID travels in the top six bits of its byte. The one multi-byte field,
// a Delay Report's delay, is big-endian.
#include "driver/system/usb/bt_avdtp.h"
#include "strings/sprintf.h"

#define PKT_SINGLE    0
#define PKT_START     1
#define PKT_CONTINUE  2
#define PKT_END       3

#define MSG_COMMAND        0
#define MSG_GENERAL_REJECT 1
#define MSG_ACCEPT         2
#define MSG_REJECT         3

#define SIG_DISCOVER          0x01
#define SIG_GET_CAPABILITIES  0x02
#define SIG_SET_CONFIGURATION 0x03
#define SIG_GET_CONFIGURATION 0x04
#define SIG_RECONFIGURE       0x05
#define SIG_OPEN              0x06
#define SIG_START             0x07
#define SIG_CLOSE             0x08
#define SIG_SUSPEND           0x09
#define SIG_ABORT             0x0a
#define SIG_SECURITY_CONTROL  0x0b
#define SIG_GET_ALL_CAPABILITIES 0x0c
#define SIG_DELAY_REPORT      0x0d

#define ERR_BAD_LENGTH              0x11
#define ERR_BAD_ACP_SEID            0x12
#define ERR_SEP_IN_USE              0x13
#define ERR_BAD_SERV_CATEGORY       0x17
#define ERR_NOT_SUPPORTED_COMMAND   0x19
#define ERR_BAD_MEDIA_TRANSPORT     0x23
#define ERR_UNSUPPORTED_CONFIGURATION 0x29
#define ERR_BAD_STATE               0x31

#define CAT_TRANSPORT  1
#define CAT_CODEC      7
#define CAT_DELAY      8

// The SBC codec information (A2DP § 4.3.2): byte 0 the sampling frequencies
// (bits 7-4) and channel modes (3-0); byte 1 the block lengths (7-4),
// subbands (3-2) and allocation methods (1-0); then the least and the most
// bitpool. Each field's bits are listed here only for what we take.
#define SBC_48000      0x10
#define SBC_STEREO     0x02
#define SBC_JOINT      0x01
#define SBC_BLOCKS_16  0x10
#define SBC_SUBBANDS_8 0x04
#define SBC_LOUDNESS   0x01
#define SBC_LEAST_BITPOOL 2

#define SEID_LAST 0x3e          // SEIDs run 0x01 to 0x3E

// What we send, in bytes, each within BT_AVDTP_LONGEST and so within the
// least MTU a signalling channel may have.
#define CAPS_BYTES     (2 + 8 + 2)           // transport, SBC codec, delay reporting
#define SETCONF_BYTES  (2 + 2 + CAPS_BYTES)
#define ANSWER_BYTES   (2 + CAPS_BYTES)      // Get All Capabilities' and Get Configuration's answers
_Static_assert(SETCONF_BYTES <= BT_AVDTP_LONGEST, "our Set Configuration fits the buffer");
_Static_assert(ANSWER_BYTES <= BT_AVDTP_LONGEST, "our longest answer fits the buffer");
_Static_assert(BT_AVDTP_LONGEST <= BT_AVDTP_LEAST_MTU, "nothing we send is fragmented");

static void av_wipe(void *p, size_t n) { uint8_t *v=p; while(n--) *v++=0; }
static void av_copy(void *d, const void *s, size_t n)
{ uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; }

static void av_put(bt_avdtp_msg_t *m, uint8_t v) { if(m->bytes<BT_AVDTP_LONGEST) m->pdu[m->bytes++]=v; }
static void av_start(bt_avdtp_msg_t *m, uint8_t label, uint8_t type, uint8_t signal)
{
    m->bytes=0;
    av_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | type));
    av_put(m,signal&0x3f);
}

static bool av_streamed(bt_avdtp_state_t s) { return s>=BT_AVDTP_CONFIGURED && s<=BT_AVDTP_CLOSING; }

// ── What we send ────────────────────────────────────────────────────────────

// Media transport, the SBC codec element, and delay reporting when asked.
static void av_put_caps(bt_avdtp_msg_t *m, uint8_t freq_mode, uint8_t blocks_alloc,
                        uint8_t lo, uint8_t hi, bool delay)
{
    av_put(m,CAT_TRANSPORT); av_put(m,0);
    av_put(m,CAT_CODEC); av_put(m,6);
    av_put(m,0x00);              // media type audio, in the top four bits
    av_put(m,0x00);              // codec type SBC
    av_put(m,freq_mode); av_put(m,blocks_alloc); av_put(m,lo); av_put(m,hi);
    if(delay) { av_put(m,CAT_DELAY); av_put(m,0); }
}

// Our endpoint's capabilities: what the session can stream.
static void av_put_ours(bt_avdtp_msg_t *m, bool delay)
{
    av_put_caps(m,SBC_48000|SBC_JOINT|SBC_STEREO,SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS,
                SBC_LEAST_BITPOOL,BT_AVDTP_BITPOOL,delay);
}

// What is configured, as a configuration: one bit in each field.
static void av_put_config(const bt_avdtp_t *a, bt_avdtp_msg_t *m)
{
    av_put_caps(m,(uint8_t)(SBC_48000 | (a->settings.joint ? SBC_JOINT : SBC_STEREO)),
                SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS,a->range_lo,a->range_hi,a->delay_reporting);
}

// A reject's payload differs by signal: Set Configuration and Reconfigure
// carry the category at fault, Start and Suspend the endpoint at fault,
// the rest the error code alone.
static void av_reject(bt_avdtp_msg_t *m, uint8_t label, uint8_t signal, uint8_t param, uint8_t code)
{
    av_start(m,label,MSG_REJECT,signal);
    if(signal==SIG_SET_CONFIGURATION || signal==SIG_RECONFIGURE) av_put(m,param);
    else if(signal==SIG_START || signal==SIG_SUSPEND) av_put(m,(uint8_t)(param<<2));
    av_put(m,code);
}

// A message's id: never 0, never one given before on this machine's life
// of the struct (it wraps after four billion messages).
static uint32_t av_id(bt_avdtp_t *a)
{
    if(++a->next_id==0) a->next_id=1;
    return a->next_id;
}

// Our one command, built here; its label is the next of sixteen, its timer
// runs from now (from queueing, not sending: a command the carrier holds
// back counts its wait against the same six seconds, which a held gate
// spends in milliseconds).
static bt_avdtp_msg_t *av_command(bt_avdtp_t *a, uint8_t signal, uint64_t wait_ms)
{
    a->command_live=true; a->command_sent=false;
    a->command_signal=signal; a->command_label=a->next_label;
    a->next_label=(uint8_t)((a->next_label+1)&15);
    a->command_deadline=a->now+wait_ms;
    av_start(&a->command,a->command_label,MSG_COMMAND,signal);
    a->command.id=av_id(a);
    return &a->command;
}

static void av_command_seid(bt_avdtp_t *a, uint8_t signal, uint64_t wait_ms)
{
    bt_avdtp_msg_t *m=av_command(a,signal,wait_ms);
    av_put(m,(uint8_t)(a->remote_seid<<2));
}

static void av_send_setconf(bt_avdtp_t *a)
{
    bt_avdtp_msg_t *m=av_command(a,SIG_SET_CONFIGURATION,BT_AVDTP_REPLY_MS);
    av_put(m,(uint8_t)(a->remote_seid<<2));
    av_put(m,BT_AVDTP_SEID<<2);
    av_put_config(a,m);
}

// ── Leaving a session ───────────────────────────────────────────────────────

// The stream is gone: nothing is configured. The rest of what it held is
// set afresh where it is next used (the settings, their range and delay
// reporting by each configuration; the state timer, media_ours and
// CONFIGURING's retry flags on entering the states that read them), and
// answers owed, their delay and the counters stay.
static void av_drop_stream(bt_avdtp_t *a)
{
    a->configured=false;
}

// They changed the stream's state with a command of theirs we accepted.
// A command of ours still QUEUED (not yet handed to the controller, even if
// the carrier has parked it: bt_avdtp_sent says when it leaves) is
// retired: it describes a stream that has since changed, and sent now it
// would act on the new one (a stale Start, once they had suspended again,
// would start a stream we think open; a stale Abort would end the stream
// they just configured). A command already SENT is on their side to answer and
// keeps its tracking: its timer runs, and its answer is taken without
// changing a state it no longer applies to.
static void av_retire_queued(bt_avdtp_t *a)
{
    if(!a->command_sent) a->command_live=false;
}

// They ended the session (their Close, Abort or Set Configuration
// accepted): a queued command is retired as above, and a sent one is
// abandoned too (its answer, if it comes, is a stray), except an Abort,
// which they saw before whatever they sent since, so it cannot touch what
// follows: it runs out its own short timer and its accept changes no state.
static void av_abandon_command(bt_avdtp_t *a)
{
    av_retire_queued(a);
    if(a->command_signal!=SIG_ABORT) a->command_live=false;
}

// To CONNECTED: the session is over and a new one is theirs to ask for or
// ours to begin.
static void av_to_connected(bt_avdtp_t *a)
{
    av_drop_stream(a);
    av_abandon_command(a);
    a->state=BT_AVDTP_CONNECTED;
}

// Abort the stream and go to CONNECTED. The Abort replaces any command of
// ours queued or outstanding.
static void av_abort_to_connected(bt_avdtp_t *a)
{
    av_drop_stream(a);
    av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);
    a->state=BT_AVDTP_CONNECTED;
}

// A session gone wrong (§ When a session goes wrong): Abort if a stream may
// be configured, and the host closes media and sig. The second on this
// connection is FAILED. Its callers have already retired the command that
// failed; when a stream may be configured, this Abort replaces any older
// one still running.
static void av_gone_wrong(bt_avdtp_t *a, const char *why)
{
    bool stream=a->state>=BT_AVDTP_CONFIGURING && a->state<=BT_AVDTP_CLOSING && a->remote_seid;
    av_drop_stream(a);
    if(stream) av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);
    a->why=why; a->wrongs++;
    a->close_wanted=true;
    a->state=a->wrongs>=2 ? BT_AVDTP_FAILED : BT_AVDTP_ENDING;
}

// Our walk made no session. That is not a session gone wrong: we go back
// to CONNECTED, keep the reason in walk_why, and say when (if ever) we may
// walk again. Their own Set Configuration is still judged on its merits.
// It comes of an answer to our command, so none is outstanding.
static void av_walk_ends(bt_avdtp_t *a, const char *why, bt_avdtp_walk_t walk)
{
    av_drop_stream(a);
    a->walk_why=why;
    a->walk=walk;
    a->begin_after=a->now+BT_AVDTP_BUSY_MS;
    a->state=BT_AVDTP_CONNECTED;
}

// ── The initiator's walk ────────────────────────────────────────────────────

static bool av_audio_sink(const bt_avdtp_endpoint_t *e)
{ return e->media==0 && e->sink && e->seid && e->seid<=SEID_LAST; }

// Ask the first audio sink endpoint not in use at or after `from`. None
// left: the walk ends, for good with why the last sink asked did not suit
// (`walk_why`, which bt_avdtp_begin clears) or with no audio sink at all;
// for a while when every audio sink they have is in use, since a phone
// holding one lets go of it.
static void av_ask(bt_avdtp_t *a, unsigned from)
{
    for(unsigned i=from;i<a->endpoint_count;i++) {
        const bt_avdtp_endpoint_t *e=&a->endpoints[i];
        if(!av_audio_sink(e) || e->in_use) continue;
        a->endpoint_at=(uint8_t)i;
        a->remote_seid=e->seid;
        // A version not known (0) is asked Get All Capabilities too: the
        // answer is the one that offers delay reporting, and a reject of it
        // falls back to Get Capabilities (no_get_all).
        bool all=(a->peer_version==0 || a->peer_version>=0x0103) && !a->no_get_all;
        a->state=BT_AVDTP_ASKING;
        av_command_seid(a,all ? SIG_GET_ALL_CAPABILITIES : SIG_GET_CAPABILITIES,BT_AVDTP_REPLY_MS);
        return;
    }
    if(a->walk_why) { av_walk_ends(a,a->walk_why,BT_AVDTP_WALK_NEVER); return; }
    for(unsigned i=0;i<a->endpoint_count;i++)
        if(av_audio_sink(&a->endpoints[i])) { av_walk_ends(a,"no sink endpoint free",BT_AVDTP_WALK_LATER); return; }
    av_walk_ends(a,"no sink endpoint",BT_AVDTP_WALK_NEVER);
}

// Their capabilities: the first SBC codec element and whether delay
// reporting is offered. False, with `why`, when they do not suit or the
// list is malformed (an element that overruns ends the walk).
static bool av_suits(bt_avdtp_t *a, const uint8_t *p, size_t n, const char **why)
{
    const uint8_t *sbc=0;
    bool delay=false;
    size_t at=0;
    while(at<n) {
        if(n-at<2 || p[at+1]>n-at-2) { a->malformed++; *why="no settings in common: capabilities malformed"; return false; }
        uint8_t cat=p[at], len=p[at+1];
        const uint8_t *v=p+at+2;
        if(cat==CAT_TRANSPORT && len!=0) { a->malformed++; *why="no settings in common: capabilities malformed"; return false; }
        if(cat==CAT_CODEC) {
            if(len<2) { a->malformed++; *why="no settings in common: capabilities malformed"; return false; }
            if(v[0]>>4==0 && v[1]==0) {
                if(len!=6) { a->malformed++; *why="no settings in common: capabilities malformed"; return false; }
                if(!sbc) sbc=v+2;
            }
        }
        if(cat==CAT_DELAY) delay=true;
        at+=2u+len;
    }
    if(!sbc) { *why="no settings in common: no SBC"; return false; }
    if(!(sbc[0]&SBC_48000)) { *why="no settings in common: 48 kHz"; return false; }
    if(!(sbc[0]&(SBC_JOINT|SBC_STEREO))) { *why="no settings in common: joint stereo or stereo"; return false; }
    if(!(sbc[1]&SBC_BLOCKS_16)) { *why="no settings in common: 16 blocks"; return false; }
    if(!(sbc[1]&SBC_SUBBANDS_8)) { *why="no settings in common: 8 subbands"; return false; }
    if(!(sbc[1]&SBC_LOUDNESS)) { *why="no settings in common: loudness"; return false; }
    uint8_t lo=sbc[2]>SBC_LEAST_BITPOOL ? sbc[2] : SBC_LEAST_BITPOOL;
    uint8_t chosen=sbc[3]<BT_AVDTP_BITPOOL ? sbc[3] : BT_AVDTP_BITPOOL;
    if(chosen<lo) { *why="no settings in common: bitpool"; return false; }
    a->settings=(bt_avdtp_sbc_t){ .rate=48000, .channels=2, .joint=(sbc[0]&SBC_JOINT)!=0,
                                  .blocks=16, .subbands=8, .snr=false, .bitpool=chosen };
    a->range_lo=lo; a->range_hi=chosen;
    a->delay_reporting=delay;
    return true;
}

// ── The acceptor's judgment of their Set Configuration ──────────────────────

// True when the configuration names one SBC setting in each field that we
// take, a bitpool range that meets ours, and no category we lack; it is
// then the session's. Else the category at fault and the code.
static bool av_judge(bt_avdtp_t *a, const uint8_t *p, size_t n, uint8_t *cat, uint8_t *code)
{
    bool transport=false, delay=false;
    const uint8_t *sbc=0;
    size_t at=0;
    *code=ERR_UNSUPPORTED_CONFIGURATION;
    while(at<n) {
        *cat=p[at];
        if(n-at<2 || p[at+1]>n-at-2) { *code=ERR_BAD_LENGTH; return false; }
        uint8_t len=p[at+1];
        const uint8_t *v=p+at+2;
        switch(p[at]) {
        case CAT_TRANSPORT:
            if(len!=0 || transport) { *code=ERR_BAD_MEDIA_TRANSPORT; return false; }
            transport=true;
            break;
        case CAT_CODEC:
            if(sbc || len!=6 || v[0]>>4!=0 || v[1]!=0) return false;
            sbc=v+2;
            break;
        case CAT_DELAY:
            if(len!=0 || delay) return false;
            delay=true;
            break;
        case 2: case 3: case 4: case 5: case 6:    // reporting, recovery, content protection, header compression, multiplexing
            return false;
        default:
            *code=ERR_BAD_SERV_CATEGORY;
            return false;
        }
        at+=2u+len;
    }
    *cat=CAT_CODEC;
    if(!sbc) return false;
    uint8_t mode=sbc[0]&0x0f;
    if((sbc[0]&0xf0)!=SBC_48000 || (mode!=SBC_JOINT && mode!=SBC_STEREO)) return false;
    if(sbc[1]!=(SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS)) return false;
    uint8_t lo=sbc[2], hi=sbc[3];
    uint8_t chosen=hi<BT_AVDTP_BITPOOL ? hi : BT_AVDTP_BITPOOL;
    if(lo<SBC_LEAST_BITPOOL || chosen<lo) return false;      // a backward range fails here too
    a->settings=(bt_avdtp_sbc_t){ .rate=48000, .channels=2, .joint=mode==SBC_JOINT,
                                  .blocks=16, .subbands=8, .snr=false, .bitpool=chosen };
    a->range_lo=lo; a->range_hi=hi;
    a->delay_reporting=delay;
    return true;
}

static void av_their_setconf(bt_avdtp_t *a, bt_avdtp_msg_t *m, uint8_t label, const uint8_t *p, size_t n)
{
    if(n<2) { av_reject(m,label,SIG_SET_CONFIGURATION,0,ERR_BAD_LENGTH); return; }
    uint8_t acp=p[0]>>2, intr=p[1]>>2;
    if(acp!=BT_AVDTP_SEID || !intr || intr>SEID_LAST) { av_reject(m,label,SIG_SET_CONFIGURATION,0,ERR_BAD_ACP_SEID); return; }
    // Ours is not on the wire while it is queued (parked by the carrier
    // included) or waiting to be sent again, so theirs may still be taken.
    bool ours_out=a->state==BT_AVDTP_CONFIGURING && a->command_live && a->command_sent;
    bool open=a->state==BT_AVDTP_CONNECTED || a->state==BT_AVDTP_DISCOVERING || a->state==BT_AVDTP_ASKING
           || (a->state==BT_AVDTP_CONFIGURING && !ours_out);
    if(!open) {
        if(a->state==BT_AVDTP_CONFIGURING) a->rejected_theirs=true;
        uint8_t code=a->state>=BT_AVDTP_CONFIGURING && a->state<=BT_AVDTP_CLOSING ? ERR_SEP_IN_USE : ERR_BAD_STATE;
        av_reject(m,label,SIG_SET_CONFIGURATION,0,code);
        return;
    }
    uint8_t cat=0, code=0;
    if(!av_judge(a,p+2,n-2,&cat,&code)) { av_reject(m,label,SIG_SET_CONFIGURATION,cat,code); return; }
    // Accepted: our own sequence is abandoned (av_abandon_command).
    av_abandon_command(a);
    a->remote_seid=intr;
    a->configured=true;
    a->edges.sessions++;
    a->state=BT_AVDTP_CONFIGURED;
    a->state_deadline=a->now+BT_AVDTP_HOLD_MS;
    av_start(m,label,MSG_ACCEPT,SIG_SET_CONFIGURATION);
}

// ── Their commands ──────────────────────────────────────────────────────────

// The answer to one command of theirs, by the state table; `m` is the slot
// it goes in. An Abort that names another endpoint, or none, has no answer
// (an Abort is never rejected).
static void av_answer(bt_avdtp_t *a, bt_avdtp_msg_t *m, uint8_t label, uint8_t signal, const uint8_t *p, size_t n)
{
    uint8_t seid=n ? p[0]>>2 : 0;
    switch(signal) {
    case SIG_DISCOVER: {
        bool in_use=a->state>=BT_AVDTP_CONFIGURING && a->state<=BT_AVDTP_CLOSING;
        av_start(m,label,MSG_ACCEPT,signal);
        av_put(m,(uint8_t)(BT_AVDTP_SEID<<2 | (in_use ? 0x02 : 0)));
        av_put(m,0x00);          // media type audio (bits 7-4), endpoint type source (bit 3)
        return;
    }
    case SIG_GET_CAPABILITIES: case SIG_GET_ALL_CAPABILITIES:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }
        av_start(m,label,MSG_ACCEPT,signal);
        av_put_ours(m,signal==SIG_GET_ALL_CAPABILITIES);
        return;
    case SIG_SET_CONFIGURATION:
        av_their_setconf(a,m,label,p,n);
        return;
    case SIG_GET_CONFIGURATION:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }
        if(!a->configured) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }
        av_start(m,label,MSG_ACCEPT,signal);
        av_put_config(a,m);
        return;
    case SIG_RECONFIGURE:
        av_reject(m,label,signal,0,ERR_NOT_SUPPORTED_COMMAND);
        return;
    case SIG_OPEN:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }
        if(a->state!=BT_AVDTP_CONFIGURED) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }
        av_retire_queued(a);
        a->state=BT_AVDTP_AWAITING_MEDIA;
        a->media_ours=false;
        a->state_deadline=a->now+BT_AVDTP_HOLD_MS;
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    case SIG_START:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,seid,ERR_BAD_ACP_SEID); return; }
        if(a->state!=BT_AVDTP_OPEN && a->state!=BT_AVDTP_STARTING) { av_reject(m,label,signal,seid,ERR_BAD_STATE); return; }
        av_retire_queued(a);
        a->state=BT_AVDTP_STREAMING;
        a->edges.their_starts++;
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    case SIG_SUSPEND:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,seid,ERR_BAD_ACP_SEID); return; }
        if(a->state!=BT_AVDTP_STREAMING && a->state!=BT_AVDTP_SUSPENDING) { av_reject(m,label,signal,seid,ERR_BAD_STATE); return; }
        av_retire_queued(a);
        a->state=BT_AVDTP_OPEN;
        a->edges.their_suspends++;
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    case SIG_CLOSE:
        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }
        if(!av_streamed(a->state)) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }
        av_to_connected(a);
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    case SIG_ABORT:
        if(seid!=BT_AVDTP_SEID) { a->malformed++; return; }
        if(av_streamed(a->state)) av_to_connected(a);
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    case SIG_DELAY_REPORT:
        // Taken under either end's identifier: ours, as the specification
        // has it, or the sink endpoint of the stream, which some sinks send.
        // With one stream there is nothing to confuse it with.
        if(n<3) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }
        if(seid!=BT_AVDTP_SEID && !(a->configured && seid==a->remote_seid)) {
            av_reject(m,label,signal,0,ERR_BAD_ACP_SEID);
            return;
        }
        if(!av_streamed(a->state)) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }
        a->delay=(uint16_t)(p[1]<<8 | p[2]);
        a->delay_known=true;
        av_start(m,label,MSG_ACCEPT,signal);
        return;
    default:                     // Security Control, and signals we do not know
        av_start(m,label,MSG_GENERAL_REJECT,signal);
        return;
    }
}

// A command of theirs while the answer to the last is still owed: one too
// many for a peer that sends one command at a time. It is rejected (bad
// state) when its turn comes; an Abort is still obeyed and accepted, and a
// signal that would be generally rejected still is.
static void av_one_too_many(bt_avdtp_t *a, bt_avdtp_msg_t *m, uint8_t label, uint8_t signal, const uint8_t *p, size_t n)
{
    if(signal==SIG_ABORT || signal==SIG_SECURITY_CONTROL || !signal || signal>SIG_DELAY_REPORT) {
        av_answer(a,m,label,signal,p,n);
        return;
    }
    if(signal==SIG_SET_CONFIGURATION && a->state==BT_AVDTP_CONFIGURING) a->rejected_theirs=true;
    bool names_seid=(signal==SIG_START || signal==SIG_SUSPEND) && n;
    av_reject(m,label,signal,names_seid ? p[0]>>2 : 0,ERR_BAD_STATE);
}

// With two answers owed a further command is dropped and counted, except
// an Abort, which is always obeyed: its accept takes the newer answer's
// place, and that answer is the one counted as dropped.
static void av_command_in(bt_avdtp_t *a, uint8_t label, uint8_t signal, const uint8_t *p, size_t n)
{
    if(!a->answer[0].bytes) av_answer(a,&a->answer[0],label,signal,p,n);
    else if(!a->answer[1].bytes) av_one_too_many(a,&a->answer[1],label,signal,p,n);
    else if(signal==SIG_ABORT) {
        bt_avdtp_msg_t m={0};
        av_answer(a,&m,label,signal,p,n);
        if(m.bytes) { a->answer[1]=m; a->overrun++; }
    }
    else a->overrun++;
    // A new answer, or one put in another's place, has no id yet (an emptied
    // slot keeps none): it gets a serial of its own.
    for(unsigned i=0;i<2;i++) if(a->answer[i].bytes && !a->answer[i].id) a->answer[i].id=av_id(a);
}

// ── Answers to our commands ─────────────────────────────────────────────────

// A reject's error code, read where its signal puts it; 0 when there is
// none. A general reject carries none, and nor does a reject that ends at
// its signal (the older general-reject shape): neither is malformed. One
// that carries some of its layout but not the code is.
static uint8_t av_reject_code(bt_avdtp_t *a, uint8_t signal, uint8_t type, const uint8_t *p, size_t n)
{
    if(type==MSG_GENERAL_REJECT || !n) return 0;
    size_t at=signal==SIG_SET_CONFIGURATION || signal==SIG_RECONFIGURE || signal==SIG_START || signal==SIG_SUSPEND ? 1 : 0;
    if(n<at+1) { a->malformed++; return 0; }
    return p[at];
}

// A response is taken only when it carries the label and the signal of the
// one command outstanding; anything else is a stray. `any_signal`: AVDTP
// 1.0's general reject, which carries no signal.
static void av_response(bt_avdtp_t *a, uint8_t label, uint8_t signal, uint8_t type, const uint8_t *p, size_t n,
                        bool any_signal)
{
    if(!a->command_live || !a->command_sent || label!=a->command_label
       || (!any_signal && signal!=a->command_signal)) { a->strays++; return; }
    signal=a->command_signal;
    a->command_live=false;
    bool accepted=type==MSG_ACCEPT;
    uint8_t code=0;
    if(!accepted) {
        code=av_reject_code(a,signal,type,p,n);
        a->last_reject_signal=signal; a->last_reject_code=code;
    }
    switch(signal) {
    case SIG_DISCOVER:
        if(!accepted) { av_gone_wrong(a,"Discover rejected"); return; }
        if(n&1) a->malformed++;
        a->endpoint_count=0;
        for(size_t i=0;i+1<n && a->endpoint_count<BT_AVDTP_ENDPOINTS;i+=2) {
            bt_avdtp_endpoint_t *e=&a->endpoints[a->endpoint_count++];
            e->seid=p[i]>>2; e->in_use=(p[i]&0x02)!=0;
            e->media=p[i+1]>>4; e->sink=(p[i+1]&0x08)!=0;
        }
        av_ask(a,0);
        return;
    case SIG_GET_CAPABILITIES: case SIG_GET_ALL_CAPABILITIES: {
        if(!accepted) {
            if(signal==SIG_GET_ALL_CAPABILITIES) {
                a->no_get_all=true;
                av_ask(a,a->endpoint_at);
                return;
            }
            a->walk_why="no settings in common: capabilities refused";
            av_ask(a,a->endpoint_at+1u);
            return;
        }
        const char *why=0;
        if(!av_suits(a,p,n,&why)) { a->walk_why=why; av_ask(a,a->endpoint_at+1u); return; }
        a->retried=false; a->rejected_theirs=false; a->retry_waiting=false;
        a->state=BT_AVDTP_CONFIGURING;
        av_send_setconf(a);
        return;
    }
    case SIG_SET_CONFIGURATION:
        if(accepted) {
            a->configured=true;
            a->edges.sessions++;
            a->state=BT_AVDTP_OPENING;
            av_command_seid(a,SIG_OPEN,BT_AVDTP_REPLY_MS);
            return;
        }
        // Both sides rejected the other's: ours goes again, once, after a moment.
        if((code==ERR_SEP_IN_USE || code==ERR_BAD_STATE) && a->rejected_theirs && !a->retried) {
            a->retry_waiting=true;
            a->state_deadline=a->now+BT_AVDTP_RETRY_MS;
            return;
        }
        av_gone_wrong(a,"Set Configuration rejected");
        return;
    case SIG_OPEN:
        if(!accepted) { av_gone_wrong(a,"Open rejected"); return; }
        a->state=BT_AVDTP_AWAITING_MEDIA;
        a->media_ours=true;
        a->state_deadline=a->now+BT_AVDTP_HOLD_MS;
        return;
    case SIG_START:
        // Their own Start may have been accepted first: the stream is then
        // streaming whatever they answer ours with, and a reject of ours is
        // no reject of the stream, so it is not counted.
        if(a->state!=BT_AVDTP_STARTING) return;
        if(accepted) { a->state=BT_AVDTP_STREAMING; return; }
        a->edges.start_rejects++;
        a->state=BT_AVDTP_OPEN;
        return;
    case SIG_SUSPEND:
        if(a->state!=BT_AVDTP_SUSPENDING) return;
        if(accepted) { a->state=BT_AVDTP_OPEN; return; }
        a->edges.suspend_rejects++;
        a->state=BT_AVDTP_STREAMING;
        return;
    case SIG_CLOSE:
        if(a->state==BT_AVDTP_CLOSING) av_to_connected(a);
        return;
    default:                     // Abort: answered, nothing follows
        return;
    }
}

// One whole message: its header byte, then the signal and payload.
static void av_message(bt_avdtp_t *a, uint8_t header, const uint8_t *p, size_t n)
{
    uint8_t label=header>>4, type=header&3;
    if(!n) {
        if(type==MSG_REJECT) av_response(a,label,0,MSG_GENERAL_REJECT,p,0,true);
        else a->malformed++;
        return;
    }
    uint8_t signal=p[0]&0x3f;
    if(type==MSG_COMMAND) av_command_in(a,label,signal,p+1,n-1);
    else av_response(a,label,signal,type,p+1,n-1,false);
}

// ── The contract ────────────────────────────────────────────────────────────

void bt_avdtp_init(bt_avdtp_t *a) { av_wipe(a,sizeof(*a)); a->state=BT_AVDTP_IDLE; }
void bt_avdtp_reset(bt_avdtp_t *a) { bt_avdtp_init(a); }

void bt_avdtp_signalling_open(bt_avdtp_t *a, uint64_t now, uint16_t peer_version)
{
    a->now=now;
    a->peer_version=peer_version;
    if(a->state!=BT_AVDTP_IDLE) return;
    a->state=BT_AVDTP_CONNECTED;
    a->tried=false;              // a new channel may try once; a walk that ended for good still holds
}

// Everything the signalling channel carried goes with it.
static void av_clear_channel(bt_avdtp_t *a)
{
    av_drop_stream(a);
    a->command_live=false;
    av_wipe(a->answer,sizeof(a->answer));
    a->rx_active=false;
    a->close_wanted=false;
}

void bt_avdtp_signalling_closed(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    bt_avdtp_state_t was=a->state;
    if(was==BT_AVDTP_IDLE) return;
    // There was a session to go wrong when a stream was configured or being
    // configured, or a command of ours was out; a channel closed with
    // nothing in flight (CONNECTED, or a walk whose first command had not
    // left) is only a channel closed.
    bool session=(was>=BT_AVDTP_CONFIGURING && was<=BT_AVDTP_CLOSING) || (a->command_live && a->command_sent);
    av_clear_channel(a);
    if(was==BT_AVDTP_FAILED) return;
    a->state=BT_AVDTP_IDLE;
    if(was==BT_AVDTP_ENDING || !session) return;
    a->wrongs++;
    a->why="the signalling channel closed";
    if(a->wrongs>=2) a->state=BT_AVDTP_FAILED;
}

void bt_avdtp_media_open(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if(a->state!=BT_AVDTP_AWAITING_MEDIA) return;
    a->state=BT_AVDTP_OPEN;
}

void bt_avdtp_media_closed(bt_avdtp_t *a, uint64_t now, bool refused)
{
    a->now=now;
    if(refused) a->media_refused++;
    if(a->state==BT_AVDTP_AWAITING_MEDIA)
        av_gone_wrong(a,refused ? "the media channel was refused" : "the media channel closed");
    else if(a->state>=BT_AVDTP_OPEN && a->state<=BT_AVDTP_SUSPENDING)
        av_abort_to_connected(a);
}

void bt_avdtp_receive(bt_avdtp_t *a, const uint8_t *p, size_t n, uint64_t now)
{
    a->now=now;
    if(a->state==BT_AVDTP_IDLE || a->state==BT_AVDTP_ENDING) { a->ignored++; return; }
    if(!n) { a->malformed++; return; }
    uint8_t type=(p[0]>>2)&3;
    if(type==PKT_SINGLE) { av_message(a,p[0],p+1,n-1); return; }
    if(type==PKT_START) {
        if(a->rx_active) { a->fragments_dropped++; a->rx_active=false; }   // the unfinished one goes
        if(n<3 || p[1]<2 || p[1]>BT_AVDTP_FRAGMENTS || n-2>sizeof(a->rx)) { a->fragments_dropped++; return; }
        a->rx_header=p[0]; a->rx_left=(uint8_t)(p[1]-1);
        av_copy(a->rx,p+2,n-2);  // the signal, then the payload
        a->rx_have=n-2;
        a->rx_active=true;
        return;
    }
    // A continue or an end: it must follow a start, carry the start's label
    // and message type, and come where the count says (an end only as the
    // last packet), within the buffer.
    if(!a->rx_active) { a->fragments_dropped++; return; }
    bool end=type==PKT_END;
    if((p[0]&0xf3)!=(a->rx_header&0xf3) || (end ? a->rx_left!=1 : a->rx_left<=1)
       || n-1>sizeof(a->rx)-a->rx_have) {
        a->fragments_dropped++; a->rx_active=false;
        return;
    }
    av_copy(a->rx+a->rx_have,p+1,n-1);
    a->rx_have+=n-1;
    a->rx_left--;
    if(end) { a->rx_active=false; av_message(a,a->rx_header,a->rx,a->rx_have); }
}

void bt_avdtp_tick(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if(a->command_live && now>=a->command_deadline) {
        uint8_t signal=a->command_signal;
        a->command_live=false;
        if(signal==SIG_ABORT) {}                          // not waited for beyond its timer
        else if(signal==SIG_CLOSE) { if(a->state==BT_AVDTP_CLOSING) av_to_connected(a); }
        else { av_gone_wrong(a,"a command went unanswered"); return; }
    }
    if(a->state==BT_AVDTP_CONFIGURING && a->retry_waiting && now>=a->state_deadline) {
        a->retry_waiting=false; a->retried=true;
        av_send_setconf(a);
    } else if(a->state==BT_AVDTP_CONFIGURED && now>=a->state_deadline) {
        av_abort_to_connected(a);
    } else if(a->state==BT_AVDTP_AWAITING_MEDIA && now>=a->state_deadline) {
        av_gone_wrong(a,"the media channel did not open");
    }
}

// The message to send now: the oldest answer owed, else our command if it
// is not sent; NULL when there is none. `command` says which it is.
static bt_avdtp_msg_t *av_head(bt_avdtp_t *a, bool *command)
{
    *command=false;
    if(a->answer[0].bytes) return &a->answer[0];
    if(a->command_live && !a->command_sent) { *command=true; return &a->command; }
    return 0;
}

// The head is gone to the controller (or dropped as too long for the
// channel): an answer leaves the owed list, the emptied slot keeping no
// id; our command is sent.
static void av_commit(bt_avdtp_t *a, bool command)
{
    if(command) a->command_sent=true;
    else { a->answer[0]=a->answer[1]; a->answer[1].bytes=0; a->answer[1].id=0; }
}

size_t bt_avdtp_next(bt_avdtp_t *a, uint8_t *out, size_t cap, uint16_t mtu, uint64_t now, uint32_t *serial)
{
    a->now=now;
    *serial=0;
    for(;;) {
        bool command;
        bt_avdtp_msg_t *m=av_head(a,&command);
        if(!m) return 0;
        size_t n=m->bytes;
        if(n>mtu) { a->unsendable++; av_commit(a,command); continue; }
        if(cap<n) return 0;
        av_copy(out,m->pdu,n);
        *serial=m->id;
        return n;
    }
}

bool bt_avdtp_sent(bt_avdtp_t *a, uint32_t serial, uint64_t now)
{
    a->now=now;
    bool command;
    bt_avdtp_msg_t *m=av_head(a,&command);
    if(!m || !serial || m->id!=serial) { a->stale_sends++; return false; }
    av_commit(a,command);
    return true;
}

bool bt_avdtp_may_begin(const bt_avdtp_t *a, uint64_t now)
{
    if(a->state!=BT_AVDTP_CONNECTED || a->command_live) return false;
    switch(a->walk) {
    case BT_AVDTP_WALK_LATER: return now>=a->begin_after;
    case BT_AVDTP_WALK_NEVER: return false;
    default: return !a->tried;
    }
}

// A new walk starts with nothing of the last: its reason, its refusal of
// Get All, its hold. The last session gone wrong keeps its own reason.
bool bt_avdtp_begin(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if(!bt_avdtp_may_begin(a,now)) return false;
    a->tried=true;
    a->walk=BT_AVDTP_WALK_OPEN;
    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false; a->walk_why=0;
    a->state=BT_AVDTP_DISCOVERING;
    av_command(a,SIG_DISCOVER,BT_AVDTP_REPLY_MS);
    return true;
}

bool bt_avdtp_start(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if(a->state!=BT_AVDTP_OPEN || a->command_live) return false;
    a->state=BT_AVDTP_STARTING;
    av_command_seid(a,SIG_START,BT_AVDTP_REPLY_MS);
    return true;
}

bool bt_avdtp_suspend(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if(a->state!=BT_AVDTP_STREAMING || a->command_live) return false;
    a->state=BT_AVDTP_SUSPENDING;
    av_command_seid(a,SIG_SUSPEND,BT_AVDTP_SUSPEND_MS);
    return true;
}

bool bt_avdtp_close(bt_avdtp_t *a, uint64_t now)
{
    a->now=now;
    if((a->state!=BT_AVDTP_OPEN && a->state!=BT_AVDTP_STREAMING) || a->command_live) return false;
    a->state=BT_AVDTP_CLOSING;
    av_command_seid(a,SIG_CLOSE,BT_AVDTP_REPLY_MS);
    return true;
}

bt_avdtp_state_t bt_avdtp_state(const bt_avdtp_t *a) { return a->state; }
bool bt_avdtp_wants_media(const bt_avdtp_t *a) { return a->state==BT_AVDTP_AWAITING_MEDIA && a->media_ours; }
bool bt_avdtp_media_wanted(const bt_avdtp_t *a) { return a->state>=BT_AVDTP_AWAITING_MEDIA && a->state<=BT_AVDTP_CLOSING; }
bool bt_avdtp_wants_closed(const bt_avdtp_t *a)
{ return a->close_wanted && !a->answer[0].bytes && !(a->command_live && !a->command_sent); }
const bt_avdtp_sbc_t *bt_avdtp_settings(const bt_avdtp_t *a) { return a->configured ? &a->settings : 0; }
bool bt_avdtp_delay(const bt_avdtp_t *a, uint32_t *delay_100us)
{
    if(!a->delay_known) return false;
    *delay_100us=a->delay;
    return true;
}
const bt_avdtp_edges_t *bt_avdtp_edges(const bt_avdtp_t *a) { return &a->edges; }

const char *bt_avdtp_state_name(bt_avdtp_state_t s)
{
    static const char *const names[]={"idle","connected","discovering","asking","configuring","configured",
        "opening","awaiting media","open","starting","streaming","suspending","closing","ending","failed"};
    return (unsigned)s<sizeof(names)/sizeof(names[0]) ? names[s] : "?";
}

size_t bt_avdtp_status(const bt_avdtp_t *a, char *out, size_t cap)
{
    if(!cap) return 0;
    size_t n=0;
    int w=snprintf(out,cap,"avdtp: %s, sessions made %u, gone wrong %u%s%s\n",bt_avdtp_state_name(a->state),
                   (unsigned)a->edges.sessions,(unsigned)a->wrongs,a->why ? ", last: " : "",a->why ? a->why : "");
    if(w>0) n+=(size_t)w;
    if(a->walk_why && n<cap) {
        w=snprintf(out+n,cap-n,"avdtp: our walk: %s%s\n",a->walk_why,
                   a->walk==BT_AVDTP_WALK_NEVER ? " (not again on this link)" : a->walk==BT_AVDTP_WALK_LATER ? " (again later)" : "");
        if(w>0) n+=(size_t)w;
    }
    if(a->configured && n<cap) {
        const bt_avdtp_sbc_t *s=&a->settings;
        w=snprintf(out+n,cap-n,"avdtp: endpoint %u, sbc %u Hz %s, %u blocks, %u subbands, %s, bitpool %u (%u-%u), delay reporting %s%s\n",
                   (unsigned)a->remote_seid,(unsigned)s->rate,s->joint ? "joint stereo" : "stereo",(unsigned)s->blocks,
                   (unsigned)s->subbands,s->snr ? "snr" : "loudness",(unsigned)s->bitpool,(unsigned)a->range_lo,
                   (unsigned)a->range_hi,a->delay_reporting ? "on" : "off",
                   a->state==BT_AVDTP_AWAITING_MEDIA ? (a->media_ours ? ", we open media" : ", they open media") : "");
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        if(a->delay_known) w=snprintf(out+n,cap-n,"avdtp: delay %u.%u ms (their report)\n",(unsigned)(a->delay/10),(unsigned)(a->delay%10));
        else w=snprintf(out+n,cap-n,"avdtp: delay not reported\n");
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avdtp: their starts %u, their suspends %u, start rejects %u, suspend rejects %u, last reject %02x code %02x\n",
                   (unsigned)a->edges.their_starts,(unsigned)a->edges.their_suspends,(unsigned)a->edges.start_rejects,
                   (unsigned)a->edges.suspend_rejects,(unsigned)a->last_reject_signal,(unsigned)a->last_reject_code);
        if(w>0) n+=(size_t)w;
    }
    if(n<cap) {
        w=snprintf(out+n,cap-n,"avdtp: strays %u, malformed %u, fragments dropped %u, unsendable %u, stale sends %u, overrun %u, ignored %u, media refused %u\n",
                   (unsigned)a->strays,(unsigned)a->malformed,(unsigned)a->fragments_dropped,(unsigned)a->unsendable,
                   (unsigned)a->stale_sends,(unsigned)a->overrun,(unsigned)a->ignored,(unsigned)a->media_refused);
        if(w>0) n+=(size_t)w;
    }
    return n<cap ? n : cap-1;
}
