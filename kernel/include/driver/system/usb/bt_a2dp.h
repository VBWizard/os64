#ifndef OS64_BT_A2DP_H
#define OS64_BT_A2DP_H

// bt_a2dp.h — the A2DP pump: the audio seam's PCM, as SBC media packets at
// the sample clock's pace, and the stream's policy (when to start, suspend
// and close it). docs/design/pending/AUDIO_A2DP.md § Part P is the design.
//
// PURE: no allocation, no waits, no logging. The seam, the encoder and the
// clock reach it through bt_a2dp_ops_t, AVDTP through its own contract
// (bt_avdtp.h), a frame's length through audio/sbc_frame.h's arithmetic
// (pure as well), and time through the `now_us` its owner hands in, so the
// host suite owns all of them. The one header it takes beyond those is
// strings/sprintf.h, for the status text, as bt_avdtp.c does.
//
// The host runs it once a pass of the Bluetooth clockwork: AVDTP is fed
// and ticked first, then bt_a2dp_step decides from AVDTP's STATE (never
// from an event: a nudge only means "pass soon"), then bt_a2dp_packet is
// called while the media channel's payload slot is empty and the send gate
// allows, at most three times, and bt_a2dp_sent when L2CAP hands the
// packet to the controller.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/sbc_frame.h"
#include "driver/system/usb/bt_avdtp.h"

#define BT_A2DP_RTP_BYTES      12          // version 2, no padding, extension or contributors
#define BT_A2DP_HEADER_BYTES   13          // the RTP header and the SBC payload header's one byte
#define BT_A2DP_FRAMES_MAX     15          // the payload header counts frames in four bits
#define BT_A2DP_SAMPLES        128         // PCM frames an SBC frame carries (16 blocks x 8 subbands)
#define BT_A2DP_PAYLOAD_TYPE   96          // dynamic; what Android's stack sends (bta_av_aact.cc)
#define BT_A2DP_SSRC           1u          // our one source; nothing else shares the media channel
#define BT_A2DP_DEFAULT_DELAY_US 150000u   // until their Delay Report, or § Proof's measurement
#define BT_A2DP_SLIP_US        100000u     // a packet later than this re-anchors the clock
#define BT_A2DP_QUIET_US       3000000u    // silence or a pause this long: Suspend
#define BT_A2DP_SLOW_US        5000u       // a packet's encode over this is slow
#define BT_A2DP_SLOW_RUN       8           // this many slow in a row fail the sink
#define BT_A2DP_HOLD_US        1000000u    // the first hold after a rejected Start; it doubles
#define BT_A2DP_HOLDS          3           // holds before a further reject gives up the session
#define BT_A2DP_THEIRS_US      2000000u    // no Start this long after THEY suspend

// What a pull told us. It mirrors the seam's audio_pulled_t, which the host
// converts, so this file needs nothing of the seam's headers.
typedef struct {
    uint32_t frames;         // how many of `out` are the ring's; the rest is silence
    uint32_t generation;
    bool paused;             // `out` is untouched
    bool yours;              // false: another sink is the sink; `out` is untouched
} bt_a2dp_pulled_t;

// Everything outside the pump, as the host supplies it. Each is called from
// the pump's own pass and none may sleep.
typedef struct {
    bt_a2dp_pulled_t (*pull)(void *ctx, int16_t *out, uint32_t frames);
    bool (*has_sound)(void *ctx);                       // the ring holds frames and the stream is not paused
    void (*attach)(void *ctx);                          // become the seam's sink (a flush)
    void (*detach)(void *ctx);                          // the null sink takes over (a flush)
    void (*failed)(void *ctx, const char *why);         // fail the sink; `why` is a string literal
    void (*delay)(void *ctx, uint32_t delay_us);        // what lies beyond the pull
    bool (*encoder_begin)(void *ctx, const sbc_settings_t *settings);
    bool (*encoder_encode)(void *ctx, const int16_t *pcm, uint8_t *out, size_t capacity);
    uint64_t (*clock_us)(void *ctx);                    // monotonic microseconds, to time an encode
} bt_a2dp_ops_t;

typedef struct bt_a2dp {
    uint64_t now;            // the last pass's, for the status text
    // AVDTP's edge counters as last read. Each only goes up within a link,
    // so a count below what was read means AVDTP was reset under us.
    bt_avdtp_edges_t seen;
    // The session: live while AVDTP has settings agreed. The encoder is
    // begun once a session, and `frame_bytes` follows its settings.
    bool session;
    bool encoder_ok;
    sbc_settings_t settings;
    uint32_t frame_bytes;
    // The seam.
    bool attached;
    bool displaced;          // a pull said another sink is the sink: no attach until the session moves on
    bool failed;
    const char *why;         // why the sink failed; NULL while it has not
    uint32_t delay_us;       // the delay as the last pass found it
    bool delay_reported;     // ...from their Delay Report, not BT_A2DP_DEFAULT_DELAY_US
    uint32_t delay_told_us;  // what the seam was last told, since the last attach
    // Starting. A session's first Start is not held; a rejected one is held
    // BT_A2DP_HOLD_US, doubling, BT_A2DP_HOLDS times, and the next reject
    // gives up the session (`gave_up`: detached until a new one). Their
    // Suspend holds the next BT_A2DP_THEIRS_US.
    uint64_t hold_until;
    uint8_t start_rejects;
    bool gave_up;
    // Streaming: true from the pass that finds AVDTP STREAMING with the
    // sink attached and healthy, and anchored then (and again at their
    // Start).
    bool streaming;
    // The sample clock: packet k is due at `anchor` plus the frames handed
    // over since it, in time. `stopped`: a pull since then made nothing, so
    // the next one that makes something anchors again.
    uint64_t anchor;
    uint64_t frames_since_anchor;
    uint32_t anchors;        // anchored this many times: tells sent frames from an older anchor's
    bool stopped;
    uint64_t quiet_since;    // when the quiet spell began (silence, a pause, nothing sendable); 0 while sound plays
    bool suspend_refused;    // our Suspend was rejected: none again, and no silence sent, until sound has come
    // The one packet made and not yet handed over: its frames are counted
    // as sent by bt_a2dp_sent. Its bytes are the caller's.
    bool pending;
    uint32_t pending_frames;
    uint32_t pending_anchor; // `anchors` when it was made
    // RTP, per session.
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t generation;     // the seam's, as the last pull said
    // Counted for the status text.
    uint8_t slow_run;
    uint32_t frames_a_packet;   // as the last packet was made
    uint64_t packets_sent, frames_sent;
    uint32_t slips, slow, dry_packets, starts_held, generations;
    uint32_t start_rejects_seen, suspend_rejects_seen, sessions_begun, closes_asked;
    // PCM for the largest packet: BT_A2DP_FRAMES_MAX frames of
    // BT_A2DP_SAMPLES stereo frames.
    int16_t pcm[BT_A2DP_FRAMES_MAX * BT_A2DP_SAMPLES * 2];
} bt_a2dp_t;

// The link came up: everything cleared, detached. AVDTP is initialised
// beside it.
void bt_a2dp_init(bt_a2dp_t *p);
// The link dropped: detach if attached, then everything cleared. A packet
// made and not yet handed over is forgotten (the channel went with the link).
void bt_a2dp_reset(bt_a2dp_t *p, const bt_a2dp_ops_t *ops, void *ctx);

// The policy, once a pass, before any packet. It reads AVDTP (state,
// edges, settings, delay) and acts on it: begin, start, suspend and close;
// attach, detach, failed and delay; encoder_begin when a session's
// settings are agreed. The table is AUDIO_A2DP.md § The stream's policy.
void bt_a2dp_step(bt_a2dp_t *p, bt_avdtp_t *avdtp, uint64_t now_us,
                  const bt_a2dp_ops_t *ops, void *ctx);
// One media packet, if one is DUE now and none is waiting to be handed
// over: pulls its PCM, encodes it, writes the RTP header, the payload
// header and the frames to `out`. Returns its length; 0 when none is due
// or none can be made (a pause, a pull that was not ours, a quiet spell
// after a refused Suspend, a failure, `capacity` too small for a frame).
// At most one packet a call. `media_mtu` is the peer's MTU for the media
// channel: one too small for even one frame fails the sink.
size_t bt_a2dp_packet(bt_a2dp_t *p, uint64_t now_us, uint16_t media_mtu,
                      uint8_t *out, size_t capacity, const bt_a2dp_ops_t *ops, void *ctx);
// The packet last made was handed to the controller: its frames are sent.
void bt_a2dp_sent(bt_a2dp_t *p);

// The pump's state as a word: detached, attached, streaming, quiet, held,
// failed.
const char *bt_a2dp_state_name(const bt_a2dp_t *p);
// Lines for slot 2's status; the length written, at most capacity - 1.
size_t bt_a2dp_status(const bt_a2dp_t *p, char *out, size_t capacity);

#endif
