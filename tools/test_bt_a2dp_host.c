// test_bt_a2dp_host.c — kernel/src/driver/system/usb/bt_a2dp.c, the A2DP
// pump, driven against the REAL AVDTP machine (bt_avdtp.c), the REAL SBC
// encoder (kernel/ports/sbc, through its boundary), a model of the audio
// seam and a clock the test owns.
//
// The model headphones answer AVDTP from bytes written here from the
// specification (the builders are tools/test_bt_avdtp_host.c's, copied):
// they accept or reject our Start, accept, reject or sit on our Suspend,
// and start, suspend,
// close, configure and report a delay of their own. The model seam keeps a
// ring of numbered samples, pauses, flushes, can be taken by another sink,
// and checks every call the pump makes of it (attach only when not
// attached; delay, failed and pull only of the current sink). Every packet
// is parsed: the RTP header (version, payload type, sequence, timestamp,
// SSRC), the payload header, and each frame against the bytes the encoder
// gave for exactly the PCM the seam's pull handed out. A replay begins the
// encoder again and encodes the whole recorded PCM a second time: the
// packets' frames must be what it gives.
//
//   tools/test_bt_a2dp_host.sh [soup rounds, default 50000]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_a2dp.h"
#include "sbc_port.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

#define BYTES(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})

// ── The wire, from the specification (tools/test_bt_avdtp_host.c's) ────────

#define H(label, pkt, type) (uint8_t)((((label) & 15) << 4) | ((pkt) << 2) | (type))
#define CMD(l)  H(l, 0, 0)
#define ACC(l)  H(l, 0, 2)
#define REJ(l)  H(l, 0, 3)
enum { DISCOVER = 1, GETCAP, SETCONF, GETCONF, RECONF, OPEN, START, CLOSE, SUSPEND, ABORT, SECCTL, GETALL, DELAY };
#define E_BAD_STATE  0x31
#define S(seid) (uint8_t)((seid) << 2)
#define EP(seid, in_use, media, sink) (uint8_t)(((seid) << 2) | ((in_use) << 1)), (uint8_t)(((media) << 4) | ((sink) << 3))
#define F48 0x10
#define STEREO 0x02
#define JOINT 0x01
#define B16 0x10
#define SB8 0x04
#define LOUD 0x01
#define TRANSPORT 0x01, 0x00
#define DELAYREP 0x08, 0x00
#define SBC(b0, b1, lo, hi) 0x07, 0x06, 0x00, 0x00, (b0), (b1), (lo), (hi)

// ── The world ───────────────────────────────────────────────────────────────

static bt_avdtp_t A;
static bt_a2dp_t P;
static uint64_t now;          // microseconds; AVDTP is handed now / 1000

#define MS 1000ull
#define SEC 1000000ull
#define FRAMES_MAX (BT_A2DP_FRAMES_MAX * BT_A2DP_SAMPLES)
#define MARKER 0x5a5a         // never a sample the model seam makes

// A sample the seam's ring holds: numbered, so a frame out of place shows.
// Values stay within +-10000, so MARKER is never one of them.
static int16_t sample(uint64_t frame, int channel)
{
    return (int16_t)((int64_t)((frame * 37u + (uint64_t)channel * 1009u) % 20000u) - 10000);
}

// The model seam. One stream; the current sink is the pump, another, or
// none (the null sink).
enum { SINK_NONE, SINK_PUMP, SINK_OTHER };
enum { PULL_NOT_YOURS, PULL_PAUSED, PULL_DRY, PULL_SOUND };
static struct {
    int sink;
    bool paused;
    bool endless;             // the player keeps the ring full
    uint32_t available;       // frames in the ring otherwise
    uint32_t generation;
    uint64_t next_frame;      // the number of the ring's next frame
    bool failed;
    int fails;
    const char *why;
    bool delay_told;          // since the last attach
    uint32_t delay;
    int attaches, detaches, stale_detaches, takeovers;
    // The last pull: its kind, how many frames it was asked for, and what
    // it wrote (when it wrote).
    int last_kind;
    uint32_t last_asked;
    int16_t last[FRAMES_MAX * 2];
    uint64_t pulls;
} M;

static void seam_flush(void)
{
    M.generation++;
    if (!M.endless) M.available = 0;
}

static bt_a2dp_pulled_t m_pull(void *ctx, int16_t *out, uint32_t frames)
{
    (void)ctx;
    M.pulls++;
    M.last_asked = frames;
    CHECK(frames > 0 && frames <= FRAMES_MAX);
    if (M.sink != SINK_PUMP) { M.last_kind = PULL_NOT_YOURS; return (bt_a2dp_pulled_t){ 0, M.generation, false, false }; }
    if (M.paused) { M.last_kind = PULL_PAUSED; return (bt_a2dp_pulled_t){ 0, M.generation, true, true }; }
    uint32_t k = M.endless ? frames : (M.available < frames ? M.available : frames);
    for (uint32_t i = 0; i < frames; i++) {
        out[2 * i] = i < k ? sample(M.next_frame + i, 0) : 0;
        out[2 * i + 1] = i < k ? sample(M.next_frame + i, 1) : 0;
    }
    M.next_frame += k;
    if (!M.endless) M.available -= k;
    memcpy(M.last, out, (size_t)frames * 4);
    M.last_kind = k ? PULL_SOUND : PULL_DRY;
    return (bt_a2dp_pulled_t){ k, M.generation, false, true };
}
static bool m_has_sound(void *ctx) { (void)ctx; return !M.paused && (M.endless || M.available > 0); }
// Attaching takes the seam from whoever had it: it is a flush, and the
// seam's delay is zero until the sink says otherwise.
static void m_attach(void *ctx)
{
    (void)ctx;
    CHECK(M.sink != SINK_PUMP);
    M.sink = SINK_PUMP;
    M.attaches++;
    M.failed = false; M.delay = 0; M.delay_told = false;
    seam_flush();
}
// A detach of a sink that is not the current one changes nothing (audio.c).
static void m_detach(void *ctx)
{
    (void)ctx;
    if (M.sink != SINK_PUMP) { M.stale_detaches++; return; }
    M.sink = SINK_NONE;
    M.detaches++;
    M.failed = false;
    seam_flush();
}
static void m_failed(void *ctx, const char *why)
{
    (void)ctx;
    CHECK(why != NULL);
    if (M.sink != SINK_PUMP) return;
    M.failed = true; M.fails++; M.why = why;
    M.available = 0;
}
static void m_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    if (M.sink != SINK_PUMP) return;
    M.delay = us; M.delay_told = true;
}
static void other_sink_takes(void)
{
    if (M.sink == SINK_PUMP) M.takeovers++;
    M.sink = SINK_OTHER;
    seam_flush();
}

// The encoder, through the boundary, or a stand-in that is cheap enough
// for an hour of stream time; and the clock that times it.
static struct {
    bool stub;
    bool fail_begin;          // the next begin is false
    uint64_t fail_encode_at;  // this encode of the session is false (1 is the first); 0: none
    uint64_t encodes;         // this session's
    int begins;
    sbc_settings_t settings;
    uint32_t fb;
    uint64_t clock, clock_step;
    uint32_t enc_n;           // frames encoded for the packet being made
    uint8_t enc_out[BT_A2DP_FRAMES_MAX][SBC_FRAME_BYTES_MAX];
    // RTP, as the model counts it for this session.
    uint16_t seq;
    uint32_t ts;
    // The replay log: every frame's PCM and bytes, in order, this session.
    bool log;
    size_t logged;
    int16_t *log_pcm;
    uint8_t *log_out;
} E;
#define LOG_MAX 4000

static bool e_begin(void *ctx, const sbc_settings_t *s)
{
    (void)ctx;
    E.begins++;
    E.encodes = 0; E.seq = 0; E.ts = 0; E.logged = 0;
    E.settings = *s;
    E.fb = sbc_frame_bytes(s);
    if (E.fail_begin) { E.fail_begin = false; return false; }
    return E.stub ? true : sbc_port_begin(s);
}
static bool e_encode(void *ctx, const int16_t *pcm, uint8_t *out, size_t cap)
{
    (void)ctx;
    E.encodes++;
    // The PCM is the pump's own buffer, and exactly what the last pull wrote there.
    ptrdiff_t at = pcm - P.pcm;
    CHECK(at >= 0 && (size_t)at + BT_A2DP_SAMPLES * 2 <= sizeof(P.pcm) / sizeof(P.pcm[0]));
    CHECK(M.last_kind == PULL_SOUND || M.last_kind == PULL_DRY);
    CHECK((size_t)at + BT_A2DP_SAMPLES * 2 <= (size_t)M.last_asked * 2);
    CHECK(!memcmp(pcm, M.last + at, BT_A2DP_SAMPLES * 4));
    if (E.fail_encode_at && E.encodes == E.fail_encode_at) return false;
    CHECK(cap >= E.fb);
    bool ok;
    if (E.stub) {
        for (uint32_t j = 0; j < E.fb; j++) out[j] = (uint8_t)(pcm[j % 256] ^ (int)j);
        ok = true;
    } else ok = sbc_port_encode(pcm, out, cap);
    CHECK(ok);
    if (ok && E.enc_n < BT_A2DP_FRAMES_MAX) memcpy(E.enc_out[E.enc_n++], out, E.fb);
    if (ok && E.log && E.logged < LOG_MAX) {
        memcpy(E.log_pcm + E.logged * BT_A2DP_SAMPLES * 2, pcm, BT_A2DP_SAMPLES * 4);
        memcpy(E.log_out + E.logged * SBC_FRAME_BYTES_MAX, out, E.fb);
        E.logged++;
    }
    return ok;
}
static uint64_t e_clock(void *ctx)
{
    (void)ctx;
    if (E.clock < now) E.clock = now;
    E.clock += E.clock_step;
    return E.clock;
}

static const bt_a2dp_ops_t ops = { m_pull, m_has_sound, m_attach, m_detach, m_failed, m_delay, e_begin, e_encode, e_clock };

// ── The model headphones ────────────────────────────────────────────────────

#define TIMES 64
static struct {
    unsigned label;           // their next command's
    bool joint;
    uint8_t max_bitpool;
    int reject_starts, reject_suspends;
    bool sit_on_suspend;      // keep the answer to our Suspend until released
    bool sit_on_start;        // never answer our Start
    bool sitting;
    unsigned sat_label;
    int starts, suspends, closes, aborts;
    uint64_t start_at[TIMES], suspend_at[TIMES], close_at[TIMES];
} peer;

static void feed_(const uint8_t *p, size_t n)
{
    uint8_t *copy = malloc(n);
    if (!copy) { failures++; return; }
    memcpy(copy, p, n);
    bt_avdtp_receive(&A, copy, n, now / MS);
    free(copy);
}
#define FEED(...) feed_(BYTES(__VA_ARGS__))

// What we sent: a command they answer, or an answer to theirs, dropped.
static void peer_take(const uint8_t *m, size_t n)
{
    CHECK(n >= 2);
    if (n < 2 || (m[0] & 3) != 0) return;
    unsigned l = m[0] >> 4;
    uint8_t joint = peer.joint ? JOINT : 0;
    switch (m[1] & 0x3f) {
    case DISCOVER: FEED(ACC(l), DISCOVER, EP(3, 0, 0, 1)); break;
    case GETALL: FEED(ACC(l), GETALL, TRANSPORT, SBC(F48 | STEREO | joint, B16 | SB8 | LOUD, 2, peer.max_bitpool), DELAYREP); break;
    case GETCAP: FEED(ACC(l), GETCAP, TRANSPORT, SBC(F48 | STEREO | joint, B16 | SB8 | LOUD, 2, peer.max_bitpool)); break;
    case SETCONF: FEED(ACC(l), SETCONF); break;
    case OPEN: FEED(ACC(l), OPEN); break;
    case START:
        if (peer.starts < TIMES) peer.start_at[peer.starts] = now;
        peer.starts++;
        if (peer.sit_on_start) break;
        if (peer.reject_starts > 0) { peer.reject_starts--; FEED(REJ(l), START, S(3), E_BAD_STATE); }
        else FEED(ACC(l), START);
        break;
    case SUSPEND:
        if (peer.suspends < TIMES) peer.suspend_at[peer.suspends] = now;
        peer.suspends++;
        if (peer.sit_on_suspend) { peer.sitting = true; peer.sat_label = l; }
        else if (peer.reject_suspends > 0) { peer.reject_suspends--; FEED(REJ(l), SUSPEND, S(3), E_BAD_STATE); }
        else FEED(ACC(l), SUSPEND);
        break;
    case CLOSE:
        if (peer.closes < TIMES) peer.close_at[peer.closes] = now;
        peer.closes++;
        FEED(ACC(l), CLOSE);
        break;
    case ABORT: peer.aborts++; FEED(ACC(l), ABORT); break;
    default: break;
    }
}

static bool sig_busy;         // the signalling channel's payload slot is taken
static void exchange(void)
{
    if (sig_busy) return;
    uint8_t m[64];
    size_t n;
    while ((n = bt_avdtp_next(&A, m, sizeof(m), 672, now / MS))) peer_take(m, n);
}

static void release_suspend(bool accept)
{
    if (!peer.sitting) return;
    peer.sitting = false;
    if (accept) FEED(ACC(peer.sat_label), SUSPEND);
    else FEED(REJ(peer.sat_label), SUSPEND, S(3), E_BAD_STATE);
    exchange();
}

// Their own commands; our answers are taken by exchange().
static void their_start(void) { FEED(CMD(peer.label++), START, S(1)); exchange(); }
static void their_suspend(void) { FEED(CMD(peer.label++), SUSPEND, S(1)); exchange(); }
static void their_close(void) { FEED(CMD(peer.label++), CLOSE, S(1)); exchange(); }
static void their_delay(uint16_t tenths_ms) { FEED(CMD(peer.label++), DELAY, S(1), (uint8_t)(tenths_ms >> 8), (uint8_t)tenths_ms); exchange(); }
// They configure us (from CONNECTED) and open the stream; the host opens
// the media channel at the next pass.
static void their_session(bool joint, uint8_t bitpool)
{
    FEED(CMD(peer.label++), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | (joint ? JOINT : STEREO), B16 | SB8 | LOUD, 2, bitpool), DELAYREP);
    exchange();
    FEED(CMD(peer.label++), OPEN, S(1));
    exchange();
}

// ── The host's pass ─────────────────────────────────────────────────────────

static struct {
    uint16_t mtu;
    size_t cap;               // what the host hands bt_a2dp_packet
    bool hold_sent;           // L2CAP keeps the packet: bt_a2dp_sent is not called
    int pass_packets;
    // Since the pump last began streaming (its flag rose): the frames made,
    // which may never run ahead of the time elapsed by more than a packet.
    bool was_streaming;
    uint64_t rise, made_since_rise;
    // The model's own sample clock, anchored where the pump anchors (at the
    // rise, at a slip, after a stop): when each packet was due, and how late
    // it was made.
    uint64_t anchor, made_since_anchor;
    uint32_t anchors_seen;
    int64_t worst_late;
    uint64_t packets, dry, sound, frames;
    uint64_t made_while_failed;
} W;

static uint8_t pkt[8192];

static void world_reset(void)
{
    memset(&W, 0, sizeof(W));
    W.mtu = 672; W.cap = sizeof(pkt);
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// One call of bt_a2dp_packet, and everything a packet must be.
static size_t make(void)
{
    bool was_pending = P.pending;
    uint64_t pulls = M.pulls;
    uint32_t slips = P.slips, anchors = P.anchors;
    E.enc_n = 0;
    size_t n = bt_a2dp_packet(&P, now, W.mtu, pkt, W.cap, &ops, NULL);
    if (was_pending) CHECK_EQ(n, 0);
    if (P.anchors != anchors) { W.anchor = P.anchor; W.made_since_anchor = 0; CHECK_EQ(P.anchor, now); }
    W.anchors_seen = P.anchors;
    if (!n) return 0;
    CHECK(M.pulls == pulls + 1);
    CHECK(M.last_kind == PULL_SOUND || M.last_kind == PULL_DRY);
    CHECK(M.sink == SINK_PUMP);
    CHECK(M.delay_told);
    CHECK(!M.failed);
    if (M.failed) W.made_while_failed++;
    CHECK(P.pending);
    CHECK(slips == P.slips || slips + 1 == P.slips);
    // The RTP header and the payload header.
    CHECK_EQ(pkt[0], 0x80);
    CHECK_EQ(pkt[1], 96);
    CHECK_EQ(be16(pkt + 2), E.seq);
    CHECK_EQ(be32(pkt + 4), E.ts);
    CHECK_EQ(be32(pkt + 8), 1);                 // the SSRC chosen
    uint32_t frames = pkt[12] & 0x0f;
    CHECK_EQ(pkt[12] >> 4, 0);
    CHECK(frames >= 1);
    CHECK_EQ(n, BT_A2DP_HEADER_BYTES + frames * E.fb);
    CHECK(n <= W.mtu && n <= W.cap);
    CHECK_EQ(frames, E.enc_n);
    CHECK_EQ(frames * BT_A2DP_SAMPLES, M.last_asked);
    for (uint32_t i = 0; i < frames && i < E.enc_n; i++)
        if (memcmp(pkt + BT_A2DP_HEADER_BYTES + i * E.fb, E.enc_out[i], E.fb)) { CHECK(!"a frame is the encoder's"); break; }
    uint32_t samples = frames * BT_A2DP_SAMPLES;
    E.seq++; E.ts += samples;
    // Never ahead of the clock: everything made since the rise, this one
    // included, is due by now give or take this packet.
    W.made_since_rise += samples;
    CHECK(W.made_since_rise <= (now - W.rise) * 48000 / SEC + samples);
    uint64_t due = W.anchor + W.made_since_anchor * SEC / 48000;
    CHECK(now >= due);
    if ((int64_t)(now - due) > W.worst_late) W.worst_late = (int64_t)(now - due);
    W.made_since_anchor += samples;
    W.packets++; W.frames += samples;
    if (M.last_kind == PULL_DRY) W.dry++; else W.sound++;
    W.pass_packets++;
    return n;
}

static void after_step(void)
{
    // The pump anchors as streaming begins, and again at their Start.
    if (P.streaming && (!W.was_streaming || P.anchors != W.anchors_seen)) {
        W.rise = now; W.made_since_rise = 0;
        W.anchor = now; W.made_since_anchor = 0;
        CHECK_EQ(P.anchor, now);
    }
    W.was_streaming = P.streaming;
    W.anchors_seen = P.anchors;
    // The seam's books: every attach is undone by one detach or one
    // takeover, or is the one in force.
    CHECK_EQ(M.attaches, M.detaches + M.takeovers + (M.sink == SINK_PUMP));
    if (P.attached && M.sink == SINK_PUMP) CHECK(M.delay_told);
    if (!P.attached) CHECK(M.sink != SINK_PUMP);
}

static void pass(uint64_t dt)
{
    now += dt;
    bt_avdtp_tick(&A, now / MS);
    exchange();
    if (bt_avdtp_state(&A) == BT_AVDTP_AWAITING_MEDIA) bt_avdtp_media_open(&A, now / MS);
    // A session gone wrong: the host closes the signalling channel, and
    // opens it again (the headphones that keep the link).
    if (bt_avdtp_wants_closed(&A)) {
        bt_avdtp_signalling_closed(&A, now / MS);
        bt_avdtp_signalling_open(&A, now / MS, 0x0103);
    }
    bt_a2dp_step(&P, &A, now, &ops, NULL);
    after_step();
    W.pass_packets = 0;
    for (int i = 0; i < 3; i++) {
        if (P.pending) break;     // the media channel's slot is taken
        if (!make()) break;
        if (!W.hold_sent) bt_a2dp_sent(&P);
    }
    CHECK(W.pass_packets <= 3);
    exchange();
}
static void run(uint64_t span, uint64_t step) { for (uint64_t t = 0; t < span; t += step) pass(step); }

static void peer_reset(void)
{
    memset(&peer, 0, sizeof(peer));
    peer.joint = true; peer.max_bitpool = 53;
}

static void link_up(void)
{
    bt_avdtp_init(&A);
    bt_a2dp_init(&P);
    peer_reset();
    W.was_streaming = false;
    bt_avdtp_signalling_open(&A, now / MS, 0x0103);
}
static void link_drop(void)
{
    bt_a2dp_reset(&P, &ops, NULL);
    bt_avdtp_reset(&A);
    W.was_streaming = false;
}

// A fresh machine: the seam empty with nobody attached, the encoder real.
static void fresh(void)
{
    if (P.attached) link_drop();
    memset(&M, 0, sizeof(M));
    int16_t *lp = E.log_pcm; uint8_t *lo = E.log_out;
    memset(&E, 0, sizeof(E));
    E.log_pcm = lp; E.log_out = lo;
    world_reset();
    sig_busy = false;
    now += 10 * SEC;
    link_up();
}
static void to_open(void)
{
    fresh();
    for (int i = 0; i < 50 && bt_avdtp_state(&A) != BT_AVDTP_OPEN; i++) pass(10 * MS);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
}
static void to_streaming(void)
{
    to_open();
    M.endless = true;
    for (int i = 0; i < 10 && !P.streaming; i++) pass(10 * MS);
    CHECK(P.streaming);
    pass(10 * MS);
    CHECK(W.sound > 0);
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_connect_to_streaming(void)
{
    fresh();
    for (int i = 0; i < 50 && bt_avdtp_state(&A) != BT_AVDTP_OPEN; i++) {
        pass(10 * MS);
        if (bt_avdtp_state(&A) != BT_AVDTP_OPEN) CHECK_EQ(M.attaches, 0);
    }
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    // The pass that found OPEN attached and told the default delay.
    pass(10 * MS);
    CHECK_EQ(M.attaches, 1);
    CHECK(M.delay_told);
    CHECK_EQ(M.delay, 150000);
    CHECK_EQ(E.begins, 1);
    CHECK_EQ(E.settings.rate, 48000); CHECK(E.settings.joint); CHECK_EQ(E.settings.bitpool, 51);
    CHECK_EQ(E.fb, 115);
    // No sound, no Start.
    run(500 * MS, 10 * MS);
    CHECK_EQ(peer.starts, 0);
    CHECK_EQ(W.packets, 0);
    // Their Delay Report: told again.
    their_delay(2000);
    pass(10 * MS);
    CHECK_EQ(M.delay, 200000);
    // Sound: Start, accepted, streaming, packets, with no intervention.
    M.endless = true;
    pass(10 * MS);
    CHECK_EQ(peer.starts, 1);
    pass(10 * MS);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK(P.streaming);
    CHECK(W.sound > 0);
    // The session ends (their Close): detached. A new one: attached at OPEN
    // and told their report, which outlives the session. (No sound, so the
    // pass that attaches does not also start the stream.)
    M.endless = false;
    their_close();
    pass(10 * MS);
    CHECK_EQ(M.sink, SINK_NONE);
    CHECK_EQ(M.detaches, 1);
    CHECK(!P.streaming);
    their_session(true, 53);
    pass(10 * MS);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(M.attaches, 2);
    CHECK(M.delay_told);
    CHECK_EQ(M.delay, 200000);
    CHECK_EQ(E.begins, 2);
}

// An hour of stream time at ragged pass intervals: every packet made when
// due and no later than the longest interval, the count exactly the frames
// due, no slip. The stand-in encoder makes the hour affordable; the real
// one is under every other test, and the replay checks its frames.
static void test_hour(void)
{
    E.stub = true;
    to_streaming();
    E.stub = true;
    W.worst_late = 0;
    srand(1013);
    uint64_t end = now + 3600 * SEC;
    uint64_t packets_before = W.packets;
    uint64_t anchor = W.anchor, due_count = 0;
    while (now < end) {
        pass(1 * MS + (uint64_t)(rand() % 30) * MS);
        // Every packet due by now is made: the count is the frames due.
        if (!P.streaming || W.anchor != anchor) { CHECK(P.streaming); CHECK_EQ(W.anchor, anchor); break; }
        while (W.anchor + (due_count * 640) * SEC / 48000 <= now) due_count++;
        if (due_count * 640 != W.made_since_anchor) { CHECK_EQ(W.made_since_anchor, due_count * 640); break; }
    }
    CHECK_EQ(P.slips, 0);
    CHECK_EQ(P.frames_a_packet, 5);
    CHECK(W.worst_late <= (int64_t)(30 * MS));
    CHECK(W.packets - packets_before > 3600ull * 75 - 10);
    CHECK_EQ(P.packets_sent, W.packets);
    E.stub = false;
}

// A pass 150 ms late is a slip: one packet, the clock anchored at it. One
// 99 ms late is caught up, three packets a pass, with no slip.
static void test_late(void)
{
    to_streaming();
    run(200 * MS, 10 * MS);
    pass(150 * MS);
    CHECK_EQ(P.slips, 1);
    CHECK_EQ(W.pass_packets, 1);
    CHECK_EQ(P.anchor, now);
    run(200 * MS, 10 * MS);
    CHECK_EQ(P.slips, 1);
    uint64_t a = W.anchor, made = W.made_since_anchor;
    pass(99 * MS);
    CHECK_EQ(P.slips, 1);
    CHECK_EQ(W.pass_packets, 3);
    CHECK_EQ(W.anchor, a);
    CHECK(W.made_since_anchor == made + 3 * 640);
    run(100 * MS, 10 * MS);
    CHECK_EQ(P.slips, 1);
    uint64_t due_count = 0;
    while (W.anchor + (due_count * 640) * SEC / 48000 <= now) due_count++;
    CHECK_EQ(W.made_since_anchor, due_count * 640);
}

// A packet made and not yet handed over holds the next one back however
// late it gets; once handed over, its frames count.
static void test_pending(void)
{
    to_streaming();
    W.hold_sent = true;
    pass(20 * MS);
    CHECK(P.pending);
    uint64_t packets = W.packets;
    for (int i = 0; i < 5; i++) {
        now += 20 * MS;
        CHECK_EQ(bt_a2dp_packet(&P, now, W.mtu, pkt, W.cap, &ops, NULL), 0);
    }
    CHECK_EQ(W.packets, packets);
    uint64_t sent = P.frames_sent;
    bt_a2dp_sent(&P);
    CHECK_EQ(P.frames_sent, sent + 640);
    CHECK(!P.pending);
    bt_a2dp_sent(&P);
    CHECK_EQ(P.frames_sent, sent + 640);
    W.hold_sent = false;
    pass(1 * MS);
    CHECK(W.packets > packets);

    // Their Suspend and their Start between two passes, a packet waiting
    // throughout: the clock is anchored afresh at the pass that sees it,
    // and the waiting packet's frames, handed over after, do not push the
    // next packet back.
    W.hold_sent = true;
    pass(20 * MS);
    CHECK(P.pending);
    their_suspend();
    their_start();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    pass(10 * MS);
    CHECK_EQ(P.anchor, now);
    bt_a2dp_sent(&P);
    CHECK(make() > 0);
    W.hold_sent = false;
    bt_a2dp_sent(&P);
}

// Dry pulls send silence, and three seconds of it suspends; sound again
// starts the stream and anchors the clock afresh.
static void test_dry_then_sound(void)
{
    to_streaming();
    M.endless = false; M.available = 0;
    uint64_t dry = W.dry;
    pass(10 * MS);
    uint64_t quiet_from = now;
    CHECK(W.dry > dry);
    CHECK(P.quiet_since == quiet_from);
    run(2990 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, 0);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    // Silence went out the whole time, on the clock.
    CHECK(W.dry - dry >= 3 * 75 - 3);
    pass(10 * MS);
    CHECK_EQ(peer.suspends, 1);
    CHECK_EQ(peer.suspend_at[0], quiet_from + 3 * SEC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    uint64_t packets = W.packets;
    run(500 * MS, 10 * MS);
    CHECK_EQ(W.packets, packets);
    CHECK_EQ(peer.starts, 1);
    M.available = 48000;
    pass(10 * MS);
    CHECK_EQ(peer.starts, 2);
    pass(10 * MS);
    CHECK(P.streaming);
    CHECK_EQ(W.rise, now);
    CHECK(W.sound > 0 && W.packets > packets);
}

// A pause makes nothing at once, never sends the PCM buffer it left
// untouched, and suspends after three seconds.
static void test_pause(void)
{
    to_streaming();
    M.paused = true;
    for (size_t i = 0; i < sizeof(P.pcm) / sizeof(P.pcm[0]); i++) P.pcm[i] = MARKER;
    uint64_t packets = W.packets;
    pass(20 * MS);
    uint64_t quiet_from = now;
    CHECK_EQ(W.packets, packets);
    CHECK_EQ(M.last_kind, PULL_PAUSED);
    run(2990 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, 0);
    pass(10 * MS);
    CHECK_EQ(peer.suspends, 1);
    CHECK_EQ(peer.suspend_at[0], quiet_from + 3 * SEC);
    CHECK_EQ(W.packets, packets);
    for (size_t i = 0; i < sizeof(P.pcm) / sizeof(P.pcm[0]); i++) if (P.pcm[i] != MARKER) { CHECK(!"the paused buffer was written"); break; }
    // The resume: Start, and sound (every frame checked against the pull).
    M.paused = false;
    pass(10 * MS);
    CHECK_EQ(peer.starts, 2);
    pass(10 * MS);
    CHECK(W.packets > packets);

    // A pause of a second, under the Suspend's three: the resume anchors
    // the clock afresh, which is not a slip, and no Start is needed.
    run(100 * MS, 10 * MS);
    uint32_t slips = P.slips;
    M.paused = true;
    run(1 * SEC, 10 * MS);
    M.paused = false;
    packets = W.packets;
    pass(10 * MS);
    CHECK(W.packets > packets);
    CHECK_EQ(P.anchor, now);
    CHECK_EQ(P.slips, slips);
    CHECK_EQ(peer.starts, 2);
    CHECK_EQ(peer.suspends, 1);
}

// Pause, then resume, at each point of a Suspend: before it leaves, while
// it is out, after it is accepted, and when it is refused. Sound resumes
// every time.
static void test_pause_around_suspend(void)
{
    for (int when = 0; when < 4; when++) {
        to_streaming();
        M.paused = true;
        if (when == 0) sig_busy = true;
        if (when == 1 || when == 3) peer.sit_on_suspend = true;
        run(3100 * MS, 10 * MS);
        CHECK_EQ(bt_avdtp_state(&A), when == 2 ? BT_AVDTP_OPEN : BT_AVDTP_SUSPENDING);
        int starts = peer.starts;
        M.paused = false;
        uint64_t packets = W.packets;
        run(100 * MS, 10 * MS);
        if (when != 2) CHECK_EQ(W.packets, packets);   // nothing while SUSPENDING
        if (when == 0) { sig_busy = false; exchange(); }
        if (when == 1) release_suspend(true);
        if (when == 3) { peer.sit_on_suspend = false; release_suspend(false); }
        run(100 * MS, 10 * MS);
        CHECK(W.sound > 0 && W.packets > packets);
        CHECK(P.streaming);
        // A refused Suspend leaves the stream streaming: no Start is needed.
        CHECK_EQ(peer.starts, starts + (when == 3 ? 0 : 1));
    }
}

// A rejected Start is held 1 s, 2 s, then 4 s; the next reject gives the
// session up (detached); a new session attaches again.
static void test_start_rejects(void)
{
    to_open();
    peer.reject_starts = 99;
    M.endless = true;
    run(9 * SEC, 10 * MS);
    CHECK_EQ(peer.starts, 4);
    CHECK(peer.start_at[1] - peer.start_at[0] >= 1 * SEC && peer.start_at[1] - peer.start_at[0] <= 1 * SEC + 10 * MS);
    CHECK(peer.start_at[2] - peer.start_at[1] >= 2 * SEC && peer.start_at[2] - peer.start_at[1] <= 2 * SEC + 10 * MS);
    CHECK(peer.start_at[3] - peer.start_at[2] >= 4 * SEC && peer.start_at[3] - peer.start_at[2] <= 4 * SEC + 10 * MS);
    CHECK_EQ(M.sink, SINK_NONE);
    CHECK(P.gave_up);
    run(5 * SEC, 10 * MS);
    CHECK_EQ(peer.starts, 4);
    CHECK_EQ(M.sink, SINK_NONE);
    // Given up, their Start streams to nobody: quiet, and three seconds of
    // it suspends, counted from the pass that sees it. The next spell is
    // measured afresh.
    for (int spell = 0; spell < 2; spell++) {
        their_start();
        uint64_t from = now + 10 * MS;
        int suspends = peer.suspends;
        run(3000 * MS, 10 * MS);
        CHECK_EQ(peer.suspends, suspends);
        pass(10 * MS);
        CHECK_EQ(peer.suspends, suspends + 1);
        CHECK_EQ(peer.suspend_at[suspends], from + 3 * SEC);
        CHECK_EQ(M.sink, SINK_NONE);
        CHECK_EQ(peer.starts, 4);
    }
    peer.reject_starts = 0;
    their_close();
    their_session(true, 53);
    pass(10 * MS);
    CHECK_EQ(M.sink, SINK_PUMP);
    CHECK(!P.gave_up);
    pass(10 * MS);
    CHECK_EQ(peer.starts, 5);           // a session's first Start is not held
    pass(10 * MS);
    CHECK(P.streaming);
}

// Their Suspend with sound waiting: no Start for two seconds, then Start.
// The two seconds run from the pass that sees it, at most a pass after it.
static void test_their_suspend(void)
{
    to_streaming();
    their_suspend();
    uint64_t at = now;
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    int starts = peer.starts;
    run(2 * SEC, 10 * MS);
    CHECK_EQ(peer.starts, starts);
    pass(10 * MS);
    CHECK_EQ(peer.starts, starts + 1);
    CHECK_EQ(peer.start_at[starts], at + 2 * SEC + 10 * MS);
    pass(10 * MS);
    CHECK(P.streaming);
}

// A refused Suspend: still streaming and quiet, no silence sent and no
// Suspend asked again; sound carries on without a Start.
static void test_suspend_refused(void)
{
    to_streaming();
    M.endless = false; M.available = 0;
    peer.reject_suspends = 1;
    run(3 * SEC + 20 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    uint64_t packets = W.packets;
    run(6 * SEC, 10 * MS);
    CHECK_EQ(W.packets, packets);
    CHECK_EQ(peer.suspends, 1);
    int starts = peer.starts;
    uint32_t slips = P.slips;
    M.available = 9600;
    pass(10 * MS);
    CHECK(W.packets > packets);
    CHECK_EQ(P.anchor, now);
    CHECK_EQ(P.slips, slips);           // anchored for the stop, not counted as a slip
    CHECK_EQ(peer.starts, starts);
    // Sound came and went: a Suspend may be asked again.
    run(6 * SEC, 10 * MS);
    CHECK_EQ(peer.suspends, 2);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
}

// Their Start in OPEN with no sound: streaming, silence for three seconds,
// then our Suspend.
static void test_their_start(void)
{
    to_open();
    pass(10 * MS);
    their_start();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    pass(10 * MS);
    uint64_t from = now;
    CHECK(P.streaming);
    CHECK(W.dry > 0);
    run(2990 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, 0);
    pass(10 * MS);
    CHECK_EQ(peer.suspends, 1);
    CHECK(peer.suspend_at[0] >= from + 3 * SEC);
    CHECK_EQ(W.sound, 0);

    // Their Suspend and Start between two passes, two seconds into a quiet
    // spell: they asked for the stream again, so the spell starts again.
    their_start();
    pass(10 * MS);
    run(2 * SEC, 10 * MS);
    int suspends = peer.suspends;
    their_suspend();
    their_start();
    pass(10 * MS);
    uint64_t again = now;
    run(2990 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, suspends);
    run(20 * MS, 10 * MS);
    CHECK_EQ(peer.suspends, suspends + 1);
    CHECK(peer.suspend_at[suspends] >= again + 3 * SEC);
}

// Our Start and theirs cross: theirs is taken, the stream is STREAMING with
// ours still unanswered, and a Close cannot be asked while it is. A sink
// that fails then makes nothing, though AVDTP still says streaming.
static void test_failed_while_a_start_crosses(void)
{
    to_open();
    peer.sit_on_start = true;
    M.endless = true;
    pass(10 * MS);
    CHECK_EQ(peer.starts, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STARTING);
    their_start();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    run(50 * MS, 10 * MS);
    CHECK(W.sound > 0);
    W.mtu = 48;
    run(30 * MS, 10 * MS);
    CHECK_EQ(M.fails, 1);
    uint64_t packets = W.packets;
    W.mtu = 672;
    run(1 * SEC, 10 * MS);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(peer.closes, 0);
    CHECK_EQ(W.packets, packets);
    CHECK(!P.streaming);
    peer.sit_on_start = false;
}

// A session ends and a new one is agreed between two passes, with other
// settings: the encoder is begun again and packets carry the new frames.
static void test_new_session_between_passes(void)
{
    to_streaming();
    CHECK_EQ(E.fb, 115);
    their_close();
    their_session(false, 35);
    bt_avdtp_media_open(&A, now / MS);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    int begins = E.begins;
    pass(10 * MS);
    CHECK_EQ(E.begins, begins + 1);
    CHECK(!E.settings.joint);
    CHECK_EQ(E.settings.bitpool, 35);
    CHECK_EQ(E.fb, 82);
    CHECK_EQ(P.frame_bytes, 82);
    pass(10 * MS);
    pass(10 * MS);
    CHECK(P.streaming);
    CHECK_EQ(P.frames_a_packet, (672 - 13) / 82);
    CHECK_EQ(be16(pkt + 2) + 1, E.seq);
    CHECK(E.seq < 5);                   // the sequence started again at 0
}

// Frames a packet from the media channel's MTU.
static void test_mtu(void)
{
    static const struct { uint16_t mtu; uint32_t frames; } cases[] = { { 128, 1 }, { 672, 5 }, { 1017, 8 }, { 4000, 15 } };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        to_streaming();
        W.mtu = cases[i].mtu;
        run(50 * MS, 10 * MS);
        CHECK_EQ(P.frames_a_packet, cases[i].frames);
        CHECK_EQ(pkt[12] & 0x0f, cases[i].frames);
        CHECK_EQ(M.fails, 0);
    }
    // A buffer too small for the MTU's frames: as many as it holds.
    to_streaming();
    W.cap = 13 + 2 * 115 + 50;
    run(50 * MS, 10 * MS);
    CHECK_EQ(P.frames_a_packet, 2);
    W.cap = 13 + 114;
    uint64_t packets = W.packets;
    run(50 * MS, 10 * MS);
    CHECK_EQ(W.packets, packets);
    CHECK_EQ(M.fails, 0);
    // An MTU smaller than one frame fails the sink, once.
    to_streaming();
    W.mtu = 48;
    run(50 * MS, 10 * MS);
    CHECK_EQ(M.fails, 1);
    CHECK(P.failed);
    W.mtu = 127;
    to_streaming();
    W.mtu = 127;
    run(50 * MS, 10 * MS);
    CHECK_EQ(M.fails, 1);
}

// A failed sink: failed once, a Close asked, attached and making nothing,
// until a new session begins the encoder.
static void failed_then_recovers(const char *what)
{
    CHECK_EQ(M.fails, 1);
    CHECK(P.failed);
    uint64_t packets = W.packets;
    run(200 * MS, 10 * MS);
    CHECK_EQ(peer.closes, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_EQ(M.sink, SINK_PUMP);
    CHECK(M.failed);
    CHECK_EQ(M.fails, 1);
    run(2 * SEC, 10 * MS);
    CHECK_EQ(W.packets, packets);
    CHECK_EQ(peer.closes, 1);
    E.clock_step = 0; E.fail_encode_at = 0;
    their_session(true, 53);
    pass(10 * MS);
    CHECK(!P.failed);
    CHECK_EQ(M.sink, SINK_PUMP);
    CHECK(!M.failed);
    M.endless = true;
    run(50 * MS, 10 * MS);
    if (!P.streaming || W.packets == packets) printf("FAIL %s: no recovery\n", what);
    CHECK(P.streaming);
    CHECK(W.packets > packets);
    CHECK_EQ(M.fails, 1);
}

// A packet L2CAP drops is lost, not sent: the clock moves on as if it had
// been sent, nothing counts it as handed over, and the next one is made at
// its own time. A detach keeps the counters, the failure and its reason.
static void test_lost_and_detach(void)
{
    to_streaming();
    W.hold_sent = true;
    pass(20 * MS);
    CHECK(P.pending);
    uint64_t sent = P.packets_sent, frames = P.frames_sent, since = P.frames_since_anchor;
    bt_a2dp_lost(&P);
    CHECK(!P.pending);
    CHECK_EQ(P.packets_lost, 1);
    CHECK_EQ(P.packets_sent, sent);
    CHECK_EQ(P.frames_sent, frames);
    CHECK_EQ(P.frames_since_anchor, since + 640);
    bt_a2dp_lost(&P);                           // nothing pending: nothing more
    CHECK_EQ(P.packets_lost, 1);
    W.hold_sent = false;
    // The next packet is due where the lost one's time ends, not at once.
    uint64_t due = P.anchor + P.frames_since_anchor * SEC / 48000;
    if (now < due) CHECK_EQ(bt_a2dp_packet(&P, now, W.mtu, pkt, W.cap, &ops, NULL), 0);
    pass(20 * MS);
    CHECK(P.packets_sent > sent);
    char text[1024];
    bt_a2dp_status(&P, text, sizeof(text));
    CHECK(strstr(text, "lost before the controller 1,") != NULL);

    // Detach: the seam is let go, the evidence stays.
    to_open();
    E.fail_encode_at = 1;
    M.endless = true;
    for (int i = 0; i < 20 && !P.failed; i++) pass(10 * MS);
    CHECK(P.failed && P.why != NULL);
    uint64_t packets = P.packets_sent;
    uint32_t sessions = P.sessions_begun;
    const char *why = P.why;
    int detaches = M.detaches, stale = M.stale_detaches;
    bt_a2dp_detach(&P, &ops, NULL);
    CHECK(!P.attached && !P.streaming && !P.pending);
    CHECK_EQ(M.detaches, detaches + 1);
    CHECK(P.failed && P.why == why && P.packets_sent == packets && P.sessions_begun == sessions);
    bt_a2dp_detach(&P, &ops, NULL);             // not attached: the seam is not asked again
    CHECK_EQ(M.detaches, detaches + 1);
    CHECK_EQ(M.stale_detaches, stale);
    E.fail_encode_at = 0;
}
static void test_failures(void)
{
    // The encoder will not begin: failed at once, closed at OPEN.
    fresh();
    E.fail_begin = true;
    M.endless = true;
    for (int i = 0; i < 50 && E.begins == 0; i++) pass(10 * MS);
    CHECK_EQ(M.fails, 1);
    CHECK_EQ(M.sink, SINK_PUMP);
    CHECK(M.delay_told);
    failed_then_recovers("begin");
    CHECK_EQ(peer.starts, 1);

    // The third frame refused.
    to_open();
    E.fail_encode_at = 3;
    M.endless = true;
    uint64_t packets = W.packets;
    run(30 * MS, 10 * MS);
    CHECK_EQ(W.packets, packets);
    failed_then_recovers("encode");

    // Eight slow encodes in a row: seven packets made, the eighth fails it.
    to_streaming();
    packets = W.packets;
    E.clock_step = 6000;
    run(400 * MS, 10 * MS);
    CHECK_EQ(W.packets, packets + 7);
    CHECK_EQ(P.slow, 8);
    failed_then_recovers("slow");

    // Seven slow and one quick: the run starts again.
    to_streaming();
    packets = W.packets;
    E.clock_step = 6000;
    for (int k = 0; k < 100 && P.slow < 7; k++) pass(10 * MS);
    CHECK_EQ(P.slow, 7);
    E.clock_step = 0;
    pass(20 * MS);
    E.clock_step = 6000;
    for (int k = 0; k < 100 && P.slow < 14 && !P.failed; k++) pass(10 * MS);
    CHECK_EQ(P.slow, 14);
    CHECK_EQ(M.fails, 0);
    CHECK(!P.failed);
    E.clock_step = 0;

    // Settings no frame can be made of (AVDTP never agrees them; poked in
    // here) are a begin that failed, even from an encoder that says yes:
    // a frame of no bytes would divide by zero.
    to_open();
    E.stub = true;
    A.settings.blocks = 12;
    A.edges.sessions++;
    pass(10 * MS);
    CHECK_EQ(M.fails, 1);
    CHECK(P.failed);
    CHECK_EQ(P.frame_bytes, 0);
    E.stub = false;
}

// AVDTP reset with no word to the pump (its edges start again below what
// the pump read): read afresh, so an old session's rejected Start does not
// count against the next one.
static void test_avdtp_reset_alone(void)
{
    to_open();
    peer.reject_starts = 1;
    M.endless = true;
    run(100 * MS, 10 * MS);
    CHECK_EQ(peer.starts, 1);
    CHECK_EQ(P.start_rejects, 1);
    bt_avdtp_reset(&A);
    bt_avdtp_signalling_open(&A, now / MS, 0x0103);
    run(200 * MS, 10 * MS);
    CHECK_EQ(peer.starts, 2);
    CHECK_EQ(P.start_rejects, 0);
    CHECK(!P.gave_up);
    CHECK(P.streaming);
}

// The link drops in every state: detached, cleared.
static void test_reset_everywhere(void)
{
    for (int k = 0; k < 9; k++) {
        fresh();
        switch (k) {
        case 0: break;                                    // connected, before the pass
        case 1: pass(10 * MS); sig_busy = true; pass(10 * MS); break;   // discovering, held
        case 2: to_open(); break;
        case 3: to_open(); M.endless = true; sig_busy = true; pass(10 * MS); break;   // starting
        case 4: to_streaming(); W.hold_sent = true; pass(20 * MS); break;          // streaming, a packet waiting
        case 5: to_streaming(); M.paused = true; sig_busy = true; run(3100 * MS, 10 * MS); break; // suspending
        case 6: to_streaming(); W.mtu = 48; pass(20 * MS); break;                  // failed
        case 7: to_open(); peer.reject_starts = 9; M.endless = true; run(9 * SEC, 10 * MS); break; // gave up
        case 8: to_streaming(); other_sink_takes(); pass(20 * MS); break;          // displaced
        }
        link_drop();
        CHECK(M.sink != SINK_PUMP);
        CHECK_EQ(M.attaches, M.detaches + M.takeovers);
        CHECK(!P.attached && !P.streaming && !P.pending && !P.failed && !P.session && !P.gave_up && !P.displaced);
        CHECK_EQ(P.seen.sessions, 0);
        // The link comes back: a fresh session streams.
        sig_busy = false; W.hold_sent = false; W.mtu = 672; M.paused = false;
        if (M.sink == SINK_OTHER) M.sink = SINK_NONE;
        link_up();
        M.endless = true;
        run(300 * MS, 10 * MS);
        CHECK(P.streaming);
        CHECK_EQ(M.sink, SINK_PUMP);
    }
}

// Another sink takes the seam: nothing made, no taking it back while the
// session stays; the session's end and a new one attach again.
static void test_displaced(void)
{
    to_streaming();
    other_sink_takes();
    uint64_t packets = W.packets;
    run(200 * MS, 10 * MS);
    CHECK_EQ(W.packets, packets);
    CHECK(!P.attached && P.displaced);
    CHECK_EQ(M.sink, SINK_OTHER);
    run(3 * SEC, 10 * MS);
    CHECK_EQ(peer.suspends, 1);
    CHECK_EQ(M.sink, SINK_OTHER);
    their_close();
    pass(10 * MS);
    CHECK(!P.displaced);
    their_session(true, 53);
    pass(10 * MS);
    CHECK_EQ(M.sink, SINK_PUMP);
}

// A flush mid-stream: a new generation, nothing dropped by the pump, the
// stream goes on.
static void test_flush(void)
{
    to_streaming();
    uint32_t gens = P.generations;
    seam_flush();
    uint64_t packets = W.packets;
    run(50 * MS, 10 * MS);
    CHECK_EQ(P.generations, gens + 1);
    CHECK_EQ(P.generation, M.generation);
    CHECK(W.packets > packets);
    CHECK_EQ(P.slips, 0);
}

// The packets' frames are what a second run of the encoder gives for the
// same PCM, from a fresh begin with the same settings.
static void test_replay(void)
{
    E.log = true;
    to_open();
    E.log = true;
    M.endless = true;
    run(1 * SEC, 10 * MS);
    size_t frames = E.logged;
    CHECK(frames > 300);
    CHECK(sbc_port_begin(&E.settings));
    uint8_t again[SBC_FRAME_BYTES_MAX];
    for (size_t i = 0; i < frames; i++) {
        CHECK(sbc_port_encode(E.log_pcm + i * BT_A2DP_SAMPLES * 2, again, sizeof(again)));
        if (memcmp(again, E.log_out + i * SBC_FRAME_BYTES_MAX, E.fb)) { CHECK(!"the replay differs"); printf("  at frame %zu\n", i); break; }
    }
    E.log = false;
}

static void test_status(void)
{
    to_streaming();
    their_delay(1234);
    pass(10 * MS);
    char buf[1024];
    size_t n = bt_a2dp_status(&P, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    CHECK(strstr(buf, "a2dp: streaming") != NULL);
    CHECK(strstr(buf, "115-byte frames, 5 frames a packet") != NULL);
    CHECK(strstr(buf, "delay 123.4 ms (their report)") != NULL);
    CHECK_EQ(bt_a2dp_status(&P, buf, 8), 7);
    CHECK_EQ(bt_a2dp_status(&P, buf, 0), 0);
    W.mtu = 48;
    pass(20 * MS);
    bt_a2dp_status(&P, buf, sizeof(buf));
    CHECK(strstr(buf, "a2dp: failed: the media channel's MTU is smaller than a frame") != NULL);
}

// ── The soup ────────────────────────────────────────────────────────────────

static unsigned long long soup_seed = 88172645463325252ull;
static unsigned r(unsigned n) { soup_seed ^= soup_seed << 13; soup_seed ^= soup_seed >> 7; soup_seed ^= soup_seed << 17; return (unsigned)(soup_seed % n); }

static void test_soup(int rounds)
{
    fresh();
    bool up = true;
    uint64_t down_until = 0;
    int reach_stream = 0, reach_begin = 0, reach_encode = 0, reach_slow = 0, reach_mtu = 0, reach_gave_up = 0;
    int reach_refused = 0, reach_theirs = 0, reach_displaced = 0, reach_drops = 0, reach_slips = 0, reach_dry = 0;
    uint32_t slow_until = 0;
    for (int i = 0; i < rounds; i++) {
        if (!up && now >= down_until) {
            up = true;
            if (M.sink == SINK_OTHER && r(2)) M.sink = SINK_NONE;
            link_up();
            W.mtu = r(8) ? 672 : (r(2) ? 48 : 128);
            peer.max_bitpool = r(4) ? 53 : (uint8_t)(20 + r(30));
            peer.joint = r(4) != 0;
        }
        if (up && r(20) == 0) {
            bt_avdtp_state_t st = bt_avdtp_state(&A);
            switch (r(20)) {
            case 0: M.endless = true; break;
            case 1: if (r(3) == 0) { M.endless = false; M.available = 0; } else M.endless = true; break;
            case 2: M.endless = false; M.available += r(20000); break;
            case 3: M.paused = !M.paused; break;
            case 4: seam_flush(); break;
            case 5: if (st == BT_AVDTP_OPEN) their_start(); break;
            case 6: if (st == BT_AVDTP_STREAMING) { their_suspend(); reach_theirs++; } break;
            case 7: if (st >= BT_AVDTP_CONFIGURED && st <= BT_AVDTP_SUSPENDING) their_close(); break;
            case 8:
                if (st == BT_AVDTP_CONNECTED) {
                    W.mtu = r(10) ? 672 : (r(2) ? 48 : 1017);
                    their_session(r(2), (uint8_t)(20 + r(40)));
                }
                break;
            case 9: peer.reject_starts += r(2) ? 4 : 1; M.endless = true; break;
            case 10: peer.reject_suspends++; break;
            case 11: peer.sit_on_suspend = !peer.sit_on_suspend; if (!peer.sit_on_suspend) release_suspend(r(2)); break;
            case 12: if (st >= BT_AVDTP_CONFIGURED && st <= BT_AVDTP_CLOSING) their_delay((uint16_t)(500 + r(3000))); break;
            case 13: if (M.sink == SINK_PUMP && r(2)) other_sink_takes(); else if (M.sink == SINK_OTHER) M.sink = SINK_NONE; break;
            case 14: if (r(4)) break;
                link_drop(); up = false; down_until = now + r(500) * MS; reach_drops++; peer.sitting = false; break;
            // Failures where they will be met soon: a session about to
            // begin its encoder, a stream encoding.
            case 15: if (st == BT_AVDTP_CONNECTED || st == BT_AVDTP_CONFIGURED) E.fail_begin = true; break;
            case 16: if (P.streaming && r(3) == 0) E.fail_encode_at = E.encodes + 1 + r(20); break;
            // Slow encodes while sound plays (a player is writing).
            case 17: if (P.streaming) { slow_until = P.slow + 10; E.clock_step = 6000; M.endless = true; M.paused = false; } break;
            case 18: sig_busy = true; break;
            case 19: W.hold_sent = !W.hold_sent; if (!W.hold_sent && P.pending) bt_a2dp_sent(&P); break;
            }
        }
        // Slow encodes until ten more were counted or the sink failed.
        if (E.clock_step && (P.slow >= slow_until || P.failed || !up)) E.clock_step = 0;
        // Headphones left with no session make one of their own.
        if (up && bt_avdtp_state(&A) == BT_AVDTP_CONNECTED && r(100) == 0) {
            W.mtu = r(10) ? 672 : (r(2) ? 48 : 1017);
            their_session(r(4) != 0, (uint8_t)(20 + r(40)));
        }
        int fails = M.fails;
        pass(1 * MS + r(30) * MS);
        // A busy channel frees itself in time: the signalling one within
        // seconds (AVDTP's timers may run out first), L2CAP's media slot
        // within a few passes (a packet late enough is a slip).
        if (up && r(200) == 0) sig_busy = false;
        if (M.paused && r(300) == 0) M.paused = false;
        if (W.hold_sent && r(20) == 0) { W.hold_sent = false; bt_a2dp_sent(&P); }
        if (P.streaming) reach_stream++;
        if (M.fails > fails && P.why) {
            if (strstr(P.why, "did not begin")) reach_begin++;
            else if (strstr(P.why, "refused a frame")) reach_encode++;
            else if (strstr(P.why, "eight packets")) reach_slow++;
            else if (strstr(P.why, "MTU")) reach_mtu++;
        }
        if (P.gave_up) reach_gave_up++;
        if (P.suspend_refused) reach_refused++;
        if (P.displaced) reach_displaced++;
        if (P.slips) reach_slips++;
        if (P.dry_packets) reach_dry++;
        // A failed sink makes nothing; that was checked in make() at every packet.
        if (failures > 50) break;
    }
    printf("soup: %d rounds: streaming %d, fails begin %d / encode %d / slow %d / mtu %d, gave up %d, "
           "suspend refused %d, their suspends %d, displaced %d, link drops %d, slips %d, dry %d\n",
           rounds, reach_stream, reach_begin, reach_encode, reach_slow, reach_mtu, reach_gave_up, reach_refused,
           reach_theirs, reach_displaced, reach_drops, reach_slips, reach_dry);
    CHECK(reach_stream > 0);
    CHECK(reach_begin > 0); CHECK(reach_encode > 0); CHECK(reach_slow > 0); CHECK(reach_mtu > 0);
    CHECK(reach_gave_up > 0); CHECK(reach_refused > 0); CHECK(reach_theirs > 0); CHECK(reach_displaced > 0);
    CHECK(reach_drops > 0); CHECK(reach_dry > 0);
    CHECK_EQ(W.made_while_failed, 0);
    E.clock_step = 0; E.fail_encode_at = 0; E.fail_begin = false;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 50000;
    E.log_pcm = malloc((size_t)LOG_MAX * BT_A2DP_SAMPLES * 4);
    E.log_out = malloc((size_t)LOG_MAX * SBC_FRAME_BYTES_MAX);
    if (!E.log_pcm || !E.log_out) return 2;
    now = 1 * SEC;
    test_connect_to_streaming();
    test_late();
    test_pending();
    test_dry_then_sound();
    test_pause();
    test_pause_around_suspend();
    test_start_rejects();
    test_their_suspend();
    test_suspend_refused();
    test_their_start();
    test_failed_while_a_start_crosses();
    test_new_session_between_passes();
    test_mtu();
    test_failures();
    test_lost_and_detach();
    test_reset_everywhere();
    test_avdtp_reset_alone();
    test_displaced();
    test_flush();
    test_replay();
    test_status();
    test_hour();
    test_soup(rounds);
    if (P.attached) link_drop();
    free(E.log_pcm);
    free(E.log_out);
    if (failures) { printf("FAIL test_bt_a2dp_host: %d failure(s)\n", failures); return 1; }
    printf("PASS test_bt_a2dp_host\n");
    return 0;
}
