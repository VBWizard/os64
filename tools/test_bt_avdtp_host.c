// test_bt_avdtp_host.c — kernel/src/driver/system/usb/bt_avdtp.c, both
// roles. A model sink, written here from AVDTP 1.3 § 8 and A2DP 1.3 § 4.3,
// builds every message it sends and every message it expects from us byte
// by byte, never by calling the code under test: the header byte (label
// 7-4, packet type 3-2, message type 1-0), the signal, SEIDs in the top six
// bits, rejects laid out by signal, the SBC codec element. It drives us as
// initiator and as acceptor, both at once, through every timer, every
// reject, fragments cut at every byte, strays and malformed lists, and
// then fuzzes: random bytes in random states, and a responsive random peer
// that must reach OPEN and STREAMING to pass.
//
//   tools/test_bt_avdtp_host.sh [fuzz rounds, default 3000]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_avdtp.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)
#define CHECK_STR(s, want) do { const char *s_ = (s); if (!s_ || strcmp(s_, (want))) { failures++; \
    printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __func__, __LINE__, #s, s_ ? s_ : "(null)", (want)); } } while (0)

#define BYTES(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})

// ── The wire, from the specification ────────────────────────────────────────

#define H(label, pkt, type) (uint8_t)((((label) & 15) << 4) | ((pkt) << 2) | (type))
#define CMD(l)  H(l, 0, 0)
#define GREJ(l) H(l, 0, 1)
#define ACC(l)  H(l, 0, 2)
#define REJ(l)  H(l, 0, 3)
enum { P_SINGLE, P_START, P_CONT, P_END };
enum { T_CMD, T_GREJ, T_ACC, T_REJ };

enum { DISCOVER = 1, GETCAP, SETCONF, GETCONF, RECONF, OPEN, START, CLOSE, SUSPEND, ABORT, SECCTL, GETALL, DELAY };
#define E_BAD_LENGTH 0x11
#define E_BAD_SEID   0x12
#define E_IN_USE     0x13
#define E_BAD_CAT    0x17
#define E_NSC        0x19
#define E_TRANSPORT  0x23
#define E_UNSUP      0x29
#define E_BAD_STATE  0x31

#define S(seid) (uint8_t)((seid) << 2)
// A Discover entry: SEID (7-2), in use (1); media type (7-4), sink (3).
#define EP(seid, in_use, media, sink) (uint8_t)(((seid) << 2) | ((in_use) << 1)), (uint8_t)(((media) << 4) | ((sink) << 3))

// SBC: sampling frequency (7-4) and channel mode (3-0); block length (7-4),
// subbands (3-2), allocation (1-0).
#define F16 0x80
#define F32 0x40
#define F44 0x20
#define F48 0x10
#define MONO 0x08
#define DUAL 0x04
#define STEREO 0x02
#define JOINT 0x01
#define B4 0x80
#define B8 0x40
#define B12 0x20
#define B16 0x10
#define SB4 0x08
#define SB8 0x04
#define SNR 0x02
#define LOUD 0x01
#define TRANSPORT 0x01, 0x00
#define DELAYREP 0x08, 0x00
#define SBC(b0, b1, lo, hi) 0x07, 0x06, 0x00, 0x00, (b0), (b1), (lo), (hi)
#define ALLSBC SBC(F16 | F32 | F44 | F48 | MONO | DUAL | STEREO | JOINT, B4 | B8 | B12 | B16 | SB4 | SB8 | SNR | LOUD, 2, 53)
// What we offer: 48 kHz, joint or stereo, 16 blocks, 8 subbands, loudness, bitpool 2 to 51.
#define OURCAPS TRANSPORT, SBC(F48 | JOINT | STEREO, B16 | SB8 | LOUD, 2, 51)

// ── The harness ─────────────────────────────────────────────────────────────

static bt_avdtp_t A;
static uint64_t now;
static unsigned L;            // the label our next command must carry
#define LAST ((L - 1) & 15)   // the label of our last command

static void dump(const char *what, const uint8_t *p, size_t n)
{
    printf("  %s:", what);
    for (size_t i = 0; i < n && i < 64; i++) printf(" %02x", p[i]);
    printf("\n");
}

static void fresh(uint16_t version)
{
    bt_avdtp_init(&A);
    now = 1000; L = 0;
    bt_avdtp_signalling_open(&A, now, version);
}

// Every message is handed over in storage of exactly its length, so that a
// byte read past its end is a report from ASan, not a zero from the stack.
static void feed_(const uint8_t *p, size_t n)
{
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) { failures++; return; }
    memcpy(copy, p, n);
    bt_avdtp_receive(&A, copy, n, now);
    free(copy);
}
#define FEED(...) feed_(BYTES(__VA_ARGS__))

// What we send, as a carrier that hands each message to the controller the
// moment it asks: taken (bt_avdtp_next) and then sent (bt_avdtp_sent).
// Nothing taken has serial 0; something taken a serial its send accepts.
static size_t take(uint8_t *out, size_t cap, uint16_t mtu)
{
    uint32_t serial = 0xdeadbeef;
    size_t g = bt_avdtp_next(&A, out, cap, mtu, now, &serial);
    if (g) { CHECK(serial != 0); CHECK(bt_avdtp_sent(&A, serial, now)); }
    else CHECK_EQ(serial, 0);
    return g;
}
static void out_(const char *f, int line, const uint8_t *want, size_t n)
{
    uint8_t got[64];
    size_t g = take(got, sizeof(got), 672);
    if (g != n || memcmp(got, want, n)) {
        failures++;
        printf("FAIL %s:%d: the message we send\n", f, line);
        dump("got ", got, g); dump("want", want, n);
    }
}
#define OUT(...) out_(__func__, __LINE__, BYTES(__VA_ARGS__))
// Our next command: the signal and payload given, under the label it must carry.
static void ours_(const char *f, int line, const uint8_t *body, size_t n)
{
    uint8_t want[64];
    want[0] = CMD(L);
    memcpy(want + 1, body, n);
    out_(f, line, want, n + 1);
    L++;
}
#define OURS(...) ours_(__func__, __LINE__, BYTES(__VA_ARGS__))
static void none_(const char *f, int line)
{
    uint8_t got[64];
    size_t g = take(got, sizeof(got), 672);
    if (g) { failures++; printf("FAIL %s:%d: we sent something\n", f, line); dump("got ", got, g); }
}
#define NONE() none_(__func__, __LINE__)
static void tick(uint64_t to) { now = to; bt_avdtp_tick(&A, now); }

// ── The initiator's path, step by step ──────────────────────────────────────

// Begin, and answer Discover with a source, a sink in use and a free sink (3).
static void to_asking(uint16_t version)
{
    fresh(version);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0), EP(2, 1, 0, 1), EP(3, 0, 0, 1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    // Get All Capabilities of 1.3 and later, and of a version not known.
    if (version >= 0x0103 || version == 0) OURS(GETALL, S(3)); else OURS(GETCAP, S(3));
}
static void to_configuring(void)
{
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC, DELAYREP);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
}
static void to_opening(void)
{
    to_configuring();
    FEED(ACC(LAST), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPENING);
    OURS(OPEN, S(3));
}
static void to_awaiting(void)
{
    to_opening();
    FEED(ACC(LAST), OPEN);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_AWAITING_MEDIA);
}
static void to_open(void)
{
    to_awaiting();
    bt_avdtp_media_open(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
}
static void to_streaming(void)
{
    to_open();
    CHECK(bt_avdtp_start(&A, now));
    OURS(START, S(3));
    FEED(ACC(LAST), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
}
// The acceptor's path: they configure our endpoint from theirs (5).
static void to_configured(void)
{
    fresh(0x0103);
    FEED(CMD(7), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), DELAYREP);
    OUT(ACC(7), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
}

// ── Initiator ───────────────────────────────────────────────────────────────

static void test_initiator_end_to_end(void)
{
    fresh(0x0103);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(!bt_avdtp_start(&A, now) && !bt_avdtp_suspend(&A, now) && !bt_avdtp_close(&A, now));
    CHECK(bt_avdtp_begin(&A, now));
    CHECK(!bt_avdtp_begin(&A, now));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_DISCOVERING);
    OURS(DISCOVER);
    NONE();
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0), EP(2, 1, 0, 1), EP(3, 0, 0, 1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    OURS(GETALL, S(3));
    CHECK(bt_avdtp_settings(&A) == NULL);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC, DELAYREP);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    CHECK(bt_avdtp_settings(&A) == NULL);
    FEED(ACC(LAST), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPENING);
    const bt_avdtp_sbc_t *s = bt_avdtp_settings(&A);
    CHECK(s != NULL);
    if (s) {
        CHECK_EQ(s->rate, 48000); CHECK_EQ(s->channels, 2); CHECK(s->joint);
        CHECK_EQ(s->blocks, 16); CHECK_EQ(s->subbands, 8); CHECK(!s->snr); CHECK_EQ(s->bitpool, 51);
    }
    OURS(OPEN, S(3));
    CHECK(!bt_avdtp_wants_media(&A));
    FEED(ACC(LAST), OPEN);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_AWAITING_MEDIA);
    CHECK(bt_avdtp_wants_media(&A));
    CHECK(bt_avdtp_media_wanted(&A));
    bt_avdtp_media_open(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK(!bt_avdtp_wants_media(&A));
    CHECK(bt_avdtp_start(&A, now));
    CHECK(!bt_avdtp_start(&A, now));
    OURS(START, S(3));
    FEED(ACC(LAST), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    // Their Delay Report while streaming: SEID, then a big-endian 1/10 ms.
    uint32_t d = 0;
    CHECK(!bt_avdtp_delay(&A, &d));
    FEED(CMD(9), DELAY, S(1), 0x05, 0xdc);
    OUT(ACC(9), DELAY);
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 1500);
    CHECK(bt_avdtp_suspend(&A, now));
    OURS(SUSPEND, S(3));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_SUSPENDING);
    FEED(ACC(LAST), SUSPEND);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK(bt_avdtp_close(&A, now));
    OURS(CLOSE, S(3));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CLOSING);
    CHECK(bt_avdtp_media_wanted(&A));
    FEED(ACC(LAST), CLOSE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(!bt_avdtp_media_wanted(&A));
    CHECK(bt_avdtp_settings(&A) == NULL);
    // The delay is kept: it is their latest report.
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 1500);
    CHECK_EQ(A.strays, 0); CHECK_EQ(A.malformed, 0);
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 1);
    // We walked on this channel already: a new session is theirs to ask for,
    // or a new channel's.
    CHECK(!bt_avdtp_may_begin(&A, now));
    CHECK(!bt_avdtp_begin(&A, now));
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 0);                    // nothing was in flight
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_may_begin(&A, now));
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
}

// Get Capabilities for a peer whose record says before 1.3; no delay
// reporting is offered, so none is configured.
static void test_initiator_older_peers(void)
{
    static const uint16_t versions[] = { 0x0102, 0x0100 };
    for (unsigned i = 0; i < 2; i++) {
        to_asking(versions[i]);
        FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC);
        OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    }
    // A Get Capabilities answer that offers delay reporting anyway is taken at its word.
    to_asking(0x0102);
    FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC, DELAYREP);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    // AVDTP 1.4 is 1.3 or later.
    to_asking(0x0104);
    CHECK_EQ(A.command.pdu[1], GETALL);
}

// A message is SENT only when the carrier says it left (bt_avdtp_sent).
// Until then it is parked, and AVDTP may retire or replace it: asking
// again gives the same message and serial; a send of a serial no longer at
// the head is refused and changes nothing.
static uint32_t peek(uint8_t *out, size_t *n)
{
    uint32_t serial = 0;
    *n = bt_avdtp_next(&A, out, 64, 672, now, &serial);
    return serial;
}
static void test_parked(void)
{
    uint8_t m[64], m2[64];
    size_t n, n2;
    // Asking twice: the same message, the same serial, nothing consumed.
    fresh(0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    uint32_t s = peek(m, &n), s2 = peek(m2, &n2);
    CHECK(s != 0 && s == s2 && n == 2 && n2 == 2 && !memcmp(m, m2, n));
    CHECK(!A.command_sent);
    CHECK(!bt_avdtp_sent(&A, s + 1, now) && !A.command_sent);
    CHECK_EQ(A.stale_sends, 1);
    OURS(DISCOVER);
    CHECK(A.command_sent);

    // The review's case: the media channel lost queues an Abort; parked; their
    // Set Configuration is accepted, which retires it; it is never sent.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    s = peek(m, &n);
    CHECK(n == 3 && m[1] == ABORT);
    FEED(CMD(9), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    s2 = peek(m2, &n2);
    CHECK(s2 != s && n2 == 2 && m2[0] == ACC(9) && m2[1] == SETCONF);
    unsigned stale = A.stale_sends;
    CHECK(!bt_avdtp_sent(&A, s, now));               // the Abort, had it been handed over late
    CHECK_EQ(A.stale_sends, stale + 1);
    OUT(ACC(9), SETCONF);
    NONE();                                          // the Abort is gone
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);

    // A parked answer survives what follows: a command queued behind it.
    fresh(0x0103);
    FEED(CMD(2), DISCOVER);
    s = peek(m, &n);
    CHECK(n == 4 && m[0] == ACC(2));
    CHECK(bt_avdtp_begin(&A, now));
    CHECK_EQ(peek(m2, &n2), s);
    CHECK(bt_avdtp_sent(&A, s, now));
    OURS(DISCOVER);

    // A parked command replaced at the head by an answer newly owed: the
    // answer goes first, and the command, the same message, after it.
    fresh(0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    uint32_t c = peek(m, &n);
    FEED(CMD(3), DISCOVER);
    s = peek(m2, &n2);
    CHECK(s != c && m2[0] == ACC(3));
    CHECK(!bt_avdtp_sent(&A, c, now) && !A.command_sent);
    OUT(ACC(3), DISCOVER, EP(1, 0, 0, 0));
    CHECK_EQ(peek(m2, &n2), c);
    OURS(DISCOVER);

    // A serial is sent once: after two answers owed have both gone, a third
    // has a serial of its own, which a repeated send of an old one cannot
    // commit.
    fresh(0x0103);
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), DISCOVER);
    uint32_t first = peek(m, &n);
    CHECK(bt_avdtp_sent(&A, first, now));
    uint32_t second = peek(m, &n);
    CHECK(second != first && bt_avdtp_sent(&A, second, now));
    FEED(CMD(6), DISCOVER);
    uint32_t third = peek(m, &n);
    CHECK(third != first && third != second && m[0] == ACC(6));
    CHECK(!bt_avdtp_sent(&A, second, now) && !bt_avdtp_sent(&A, first, now));
    CHECK(A.answer[0].bytes);
    OUT(ACC(6), DISCOVER, EP(1, 0, 0, 0));
}

// A version not known (their record not read when signalling opened): Get
// All Capabilities first, so a 1.3 sink's delay reporting is not lost to the
// order things arrived in; a reject of it, of either shape, falls back to
// Get Capabilities, as for a 1.3 peer that refuses it.
static void test_initiator_unknown_version(void)
{
    // Accepted: delay reporting offered, and configured.
    to_asking(0x0000);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC, DELAYREP);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    // Rejected: Get Capabilities, and no delay reporting.
    to_asking(0x0000);
    FEED(REJ(LAST), GETALL, E_NSC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    OURS(GETCAP, S(3));
    FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    // A general reject, and AVDTP 1.0's with no signal: the same fallback.
    to_asking(0x0000);
    FEED(GREJ(LAST), GETALL);
    OURS(GETCAP, S(3));
    to_asking(0x0000);
    FEED(REJ(LAST));
    OURS(GETCAP, S(3));
}

// Set Configuration's exact bytes for each settings outcome.
static void test_initiator_settings(void)
{
    static const struct { uint8_t b0, lo, hi, want_mode, want_lo, want_hi; } cases[] = {
        { F48 | JOINT | STEREO, 2, 53, JOINT, 2, 51 },   // their maximum above 51
        { F48 | JOINT | STEREO, 2, 51, JOINT, 2, 51 },   // at
        { F48 | JOINT | STEREO, 2, 50, JOINT, 2, 50 },   // below
        { F48 | JOINT | STEREO, 2, 35, JOINT, 2, 35 },
        { F48 | STEREO, 2, 53, STEREO, 2, 51 },          // no joint stereo
        { F48 | STEREO | MONO | DUAL, 2, 53, STEREO, 2, 51 },
        { F48 | JOINT, 10, 53, JOINT, 10, 51 },          // their minimum above 2
        { F48 | JOINT, 51, 53, JOINT, 51, 51 },
        { F48 | JOINT, 0, 53, JOINT, 2, 51 },            // their minimum below 2
        { F48 | JOINT, 2, 2, JOINT, 2, 2 },
        { F48 | JOINT, 30, 30, JOINT, 30, 30 },
        { F48 | JOINT, 2, 250, JOINT, 2, 51 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        to_asking(0x0103);
        FEED(ACC(LAST), GETALL, TRANSPORT, SBC(cases[i].b0, B16 | SB8 | LOUD, cases[i].lo, cases[i].hi), DELAYREP);
        OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | cases[i].want_mode, B16 | SB8 | LOUD, cases[i].want_lo, cases[i].want_hi),
             DELAYREP);
        FEED(ACC(LAST), SETCONF);
        const bt_avdtp_sbc_t *s = bt_avdtp_settings(&A);
        CHECK(s && s->bitpool == cases[i].want_hi && s->joint == (cases[i].want_mode == JOINT));
    }
}

// Get All Capabilities rejected, or not understood: Get Capabilities is asked.
static void test_initiator_get_all_rejected(void)
{
    to_asking(0x0103);
    FEED(REJ(LAST), GETALL, E_NSC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    OURS(GETCAP, S(3));
    FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    CHECK_EQ(A.last_reject_signal, GETALL); CHECK_EQ(A.last_reject_code, E_NSC);

    to_asking(0x0103);
    FEED(GREJ(LAST), GETALL);
    OURS(GETCAP, S(3));
    // AVDTP 1.0's general reject: a reject with no signal at all.
    to_asking(0x0103);
    FEED(REJ(LAST));
    OURS(GETCAP, S(3));

    // Once refused, the next endpoint is asked Get Capabilities too.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(4, 0, 0, 1), EP(6, 0, 0, 1));
    OURS(GETALL, S(4));
    FEED(REJ(LAST), GETALL, E_NSC);
    OURS(GETCAP, S(4));
    FEED(ACC(LAST), GETCAP, TRANSPORT, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OURS(GETCAP, S(6));
    FEED(REJ(LAST), GETCAP, E_BAD_SEID);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_STR(A.walk_why, "no settings in common: capabilities refused");

    // The refusal is this session's: the next one asks Get All again.
    to_asking(0x0103);
    FEED(REJ(LAST), GETALL, E_NSC);
    OURS(GETCAP, S(3));
    FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    tick(now + 6000);
    OURS(ABORT, S(3));
    bt_avdtp_signalling_closed(&A, now);
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    OURS(GETALL, S(3));
}

// The walk: the first sink not in use, then the next, skipping sources,
// video, in-use sinks and SEIDs outside 1..0x3E.
static void test_initiator_walk(void)
{
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0), EP(2, 0, 1, 1), EP(3, 1, 0, 1), EP(0, 0, 0, 1), EP(0x3f, 0, 0, 1),
         EP(9, 0, 0, 1), EP(10, 0, 0, 1));
    OURS(GETALL, S(9));
    FEED(ACC(LAST), GETALL, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | SNR, 2, 53));    // no loudness
    OURS(GETALL, S(10));
    FEED(ACC(LAST), GETALL, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OURS(SETCONF, S(10), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    FEED(ACC(LAST), SETCONF);
    OURS(OPEN, S(10));

    // At most eight endpoints are read: a ninth sink is never asked.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0), EP(2, 0, 0, 0), EP(3, 0, 0, 0), EP(4, 0, 0, 0), EP(5, 0, 0, 0),
         EP(6, 0, 0, 0), EP(7, 0, 0, 0), EP(8, 0, 0, 0), EP(9, 0, 0, 1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_STR(A.walk_why, "no sink endpoint");
    CHECK_EQ(A.endpoint_count, 8);
    // An odd byte at the end is counted and the whole entries are read.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(7, 0, 0, 1), 0x55);
    OURS(GETALL, S(7));
    CHECK_EQ(A.malformed, 1);
    // The eighth is read.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0), EP(2, 0, 0, 0), EP(3, 0, 0, 0), EP(4, 0, 0, 0), EP(5, 0, 0, 0),
         EP(6, 0, 0, 0), EP(7, 0, 0, 0), EP(8, 0, 0, 1));
    OURS(GETALL, S(8));
}

// No sink endpoint; no settings in common, each field missing in turn. A
// walk that makes no session is not a session gone wrong: CONNECTED, with
// the reason kept, and their own Set Configuration still taken on its merits.
static void test_initiator_failures(void)
{
    static const struct { uint8_t eps[6]; uint8_t n; bool later; } nosink[] = {
        { { EP(1, 0, 0, 0) }, 2, false },                  // a source alone
        { { EP(2, 1, 0, 1) }, 2, true },                   // a sink in use
        { { EP(1, 0, 0, 0), EP(2, 1, 0, 1), EP(4, 1, 0, 1) }, 6, true },   // every sink in use
        { { EP(2, 0, 1, 1) }, 2, false },                  // a video sink
        { { EP(2, 1, 1, 1), EP(0, 1, 0, 1) }, 4, false },  // in use, but no audio sink with a SEID
        { { 0 }, 0, false },                               // nothing
    };
    for (unsigned i = 0; i < sizeof(nosink) / sizeof(nosink[0]); i++) {
        fresh(0x0103);
        CHECK(bt_avdtp_may_begin(&A, now));
        bt_avdtp_begin(&A, now);
        OURS(DISCOVER);
        uint8_t m[8] = { ACC(LAST), DISCOVER };
        memcpy(m + 2, nosink[i].eps, nosink[i].n);
        feed_(m, 2u + nosink[i].n);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        CHECK_STR(A.walk_why, nosink[i].later ? "no sink endpoint free" : "no sink endpoint");
        CHECK(A.why == NULL);
        CHECK_EQ(A.wrongs, 0);
        CHECK(!bt_avdtp_wants_closed(&A));
        NONE();
        char buf[512];
        bt_avdtp_status(&A, buf, sizeof(buf));
        CHECK(strstr(buf, nosink[i].later ? "our walk: no sink endpoint free (again later)"
                                          : "our walk: no sink endpoint (not again on this link)") != NULL);
        // Not again at once; a held sink, again 30 s later; no sink, never on this link.
        CHECK(!bt_avdtp_may_begin(&A, now));
        CHECK(!bt_avdtp_begin(&A, now));
        CHECK(!bt_avdtp_may_begin(&A, now + 29999));
        CHECK_EQ(bt_avdtp_may_begin(&A, now + 30000), nosink[i].later);
        // A new signalling channel does not lift it either way.
        bt_avdtp_signalling_closed(&A, now);
        CHECK_EQ(A.wrongs, 0);
        bt_avdtp_signalling_open(&A, now, 0x0103);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        CHECK(!bt_avdtp_may_begin(&A, now));
        CHECK_EQ(bt_avdtp_may_begin(&A, now + 30000), nosink[i].later);
        if (nosink[i].later) {
            now += 30000;
            CHECK(bt_avdtp_begin(&A, now));
            OURS(DISCOVER);
            CHECK(A.walk_why == NULL);
            FEED(ACC(LAST), DISCOVER, EP(2, 0, 0, 1));         // the phone let go
            OURS(GETALL, S(2));
            // The retry spent the hold: once this session ends, no walk until a new channel.
            FEED(CMD(3), SETCONF, S(1), S(2), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
            OUT(ACC(3), SETCONF);
            FEED(CMD(4), CLOSE, S(1));
            OUT(ACC(4), CLOSE);
            CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
            CHECK(!bt_avdtp_may_begin(&A, now + 100000));
        } else {
            // Their own configuration is still judged, and taken.
            FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
            OUT(ACC(3), SETCONF);
            CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
        }
        // The link dropping lifts everything.
        bt_avdtp_reset(&A);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
        bt_avdtp_signalling_open(&A, now, 0x0103);
        CHECK(bt_avdtp_may_begin(&A, now));
    }
    static const struct { uint8_t b0, b1, lo, hi; const char *why; } nofit[] = {
        { F44 | F32 | F16 | JOINT | STEREO, B16 | SB8 | LOUD, 2, 53, "no settings in common: 48 kHz" },
        { F48 | MONO | DUAL, B16 | SB8 | LOUD, 2, 53, "no settings in common: joint stereo or stereo" },
        { F48 | JOINT | STEREO, B4 | B8 | B12 | SB8 | LOUD, 2, 53, "no settings in common: 16 blocks" },
        { F48 | JOINT | STEREO, B16 | SB4 | LOUD, 2, 53, "no settings in common: 8 subbands" },
        { F48 | JOINT | STEREO, B16 | SB8 | SNR, 2, 53, "no settings in common: loudness" },
        { F48 | JOINT | STEREO, B16 | SB8 | LOUD, 52, 60, "no settings in common: bitpool" },
        { F48 | JOINT | STEREO, B16 | SB8 | LOUD, 40, 30, "no settings in common: bitpool" },
        { F48 | JOINT | STEREO, B16 | SB8 | LOUD, 0, 1, "no settings in common: bitpool" },
    };
    for (unsigned i = 0; i < sizeof(nofit) / sizeof(nofit[0]); i++) {
        to_asking(0x0103);
        FEED(ACC(LAST), GETALL, TRANSPORT, SBC(nofit[i].b0, nofit[i].b1, nofit[i].lo, nofit[i].hi));
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        CHECK_STR(A.walk_why, nofit[i].why);
        CHECK_EQ(A.wrongs, 0);
        NONE();
        CHECK(!bt_avdtp_may_begin(&A, now + 1000000));
    }
    // A codec that is not SBC.
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, 0x07, 0x08, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0x00, 0x00);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_STR(A.walk_why, "no settings in common: no SBC");
    // SBC, but for video.
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, 0x07, 0x06, 0x10, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2, 53);
    CHECK_STR(A.walk_why, "no settings in common: no SBC");
    // A sink that did not suit, and another in use: no settings in common
    // with the one we could ask, and that is for good.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(2, 1, 0, 1), EP(3, 0, 0, 1));
    OURS(GETALL, S(3));
    FEED(ACC(LAST), GETALL, TRANSPORT, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53));
    CHECK_STR(A.walk_why, "no settings in common: 48 kHz");
    CHECK(!bt_avdtp_may_begin(&A, now + 30000));
    // A walk ending keeps the last session-gone-wrong reason beside its own.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    tick(now + 6000);
    NONE();
    bt_avdtp_signalling_closed(&A, now);
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    CHECK_STR(A.why, "a command went unanswered");
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(1, 0, 0, 0));
    CHECK_STR(A.why, "a command went unanswered");
    CHECK_STR(A.walk_why, "no sink endpoint");
    char buf[512];
    bt_avdtp_status(&A, buf, sizeof(buf));
    CHECK(strstr(buf, "gone wrong 1, last: a command went unanswered") && strstr(buf, "our walk: no sink endpoint"));
}

// Every reject of every command we send, in its real layout, and the state
// that follows.
static void test_initiator_rejects(void)
{
    // Discover: the code alone; a session gone wrong with nothing to abort.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(REJ(LAST), DISCOVER, E_NSC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK(bt_avdtp_wants_closed(&A));
    NONE();
    CHECK_EQ(A.last_reject_signal, DISCOVER); CHECK_EQ(A.last_reject_code, E_NSC);
    // Set Configuration: the category and the code; Abort, then the channels close.
    to_configuring();
    FEED(REJ(LAST), SETCONF, 0x07, E_UNSUP);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK(!bt_avdtp_wants_closed(&A));
    OURS(ABORT, S(3));
    CHECK(bt_avdtp_wants_closed(&A));
    CHECK_EQ(A.last_reject_code, E_UNSUP);
    // A Set Configuration reject of in use, with nothing of theirs rejected, is not retried.
    to_configuring();
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    // Open: the code alone.
    to_opening();
    FEED(REJ(LAST), OPEN, E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    CHECK_EQ(A.last_reject_code, E_BAD_STATE);
    // Start: the endpoint and the code; OPEN, counted.
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(REJ(LAST), START, S(3), E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 1);
    CHECK_EQ(A.last_reject_code, E_BAD_STATE);
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(GREJ(LAST), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 2);
    CHECK_EQ(A.last_reject_code, 0);
    CHECK_EQ(A.malformed, 0);                 // a general reject carries no code, and is not short
    // Suspend: the endpoint and the code; STREAMING still, counted.
    to_streaming();
    bt_avdtp_suspend(&A, now);
    OURS(SUSPEND, S(3));
    FEED(REJ(LAST), SUSPEND, S(3), E_IN_USE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(bt_avdtp_edges(&A)->suspend_rejects, 1);
    CHECK_EQ(A.last_reject_code, E_IN_USE);
    // Close: the code alone; CONNECTED all the same.
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    FEED(REJ(LAST), CLOSE, E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_EQ(A.last_reject_signal, CLOSE); CHECK_EQ(A.last_reject_code, E_BAD_STATE);
    // Abort is never rejected; one that is, is still its answer.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    OURS(ABORT, S(3));
    FEED(REJ(LAST), ABORT, E_BAD_STATE);
    CHECK_EQ(A.strays, 0);
    CHECK(!A.command_live);
    // A reject too short for its layout is counted, and still a reject.
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(REJ(LAST), START, S(3));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(A.malformed, 1); CHECK_EQ(A.last_reject_code, 0);
    to_configuring();
    FEED(REJ(LAST), SETCONF, E_IN_USE);
    CHECK_EQ(A.malformed, 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    // A reject that ends at its signal is the older general-reject shape:
    // a reject, with no code, and not malformed.
    to_opening();
    FEED(REJ(LAST), OPEN);
    CHECK_EQ(A.malformed, 0); CHECK_EQ(A.last_reject_code, 0);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(REJ(LAST), START);
    CHECK_EQ(A.malformed, 0);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 1);
}

// ── Timers ──────────────────────────────────────────────────────────────────

static void test_timers(void)
{
    // A command waits 6 s.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    tick(1000 + 5999);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_DISCOVERING);
    tick(1000 + 6000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK_STR(A.why, "a command went unanswered");
    // The timer runs from when the command is queued, sent or not.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    tick(1000 + 6000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    NONE();
    // Get All Capabilities, Set Configuration, Open, Start: 6 s each, then Abort where a stream may be.
    to_asking(0x0103);
    tick(now + 5999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    NONE();
    to_configuring();
    tick(now + 5999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    to_opening();
    tick(now + 6000); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    tick(now + 5999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STARTING);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    // Suspend waits 10 s.
    to_streaming();
    bt_avdtp_suspend(&A, now);
    OURS(SUSPEND, S(3));
    tick(now + 9999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_SUSPENDING);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    // Close's timer ends the session as its answer would.
    to_open();
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    tick(now + 5999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CLOSING);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_EQ(A.wrongs, 0);
    // Abort waits 2 s and nothing waits on it after. (A session they began,
    // so we have not walked on this channel and may once the Abort is done.)
    to_configured();
    FEED(CMD(2), OPEN, S(1));
    OUT(ACC(2), OPEN);
    bt_avdtp_media_open(&A, now);
    bt_avdtp_media_closed(&A, now, false);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    OURS(ABORT, S(5));
    CHECK(!bt_avdtp_may_begin(&A, now));
    CHECK(!bt_avdtp_begin(&A, now));
    tick(now + 1999);
    CHECK(!bt_avdtp_begin(&A, now));
    tick(now + 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_EQ(A.wrongs, 0);
    CHECK(bt_avdtp_may_begin(&A, now));
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    // AWAITING_MEDIA waits 10 s for the channel: then Abort and a session gone wrong.
    to_awaiting();
    tick(now + 9999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_AWAITING_MEDIA);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK_STR(A.why, "the media channel did not open");
    OURS(ABORT, S(3));
    CHECK(bt_avdtp_wants_closed(&A));
    // CONFIGURED waits 10 s for their Open: then Abort and CONNECTED.
    to_configured();
    tick(now + 9999); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    tick(now + 1); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    OURS(ABORT, S(5));
    CHECK_EQ(A.wrongs, 0);
    CHECK(!bt_avdtp_wants_closed(&A));
    // Their Open in time stops it.
    to_configured();
    tick(now + 9000);
    FEED(CMD(2), OPEN, S(1));
    OUT(ACC(2), OPEN);
    tick(now + 5000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_AWAITING_MEDIA);
    tick(now + 5000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(5));
    // OPEN has no timer.
    to_open();
    tick(now + 1000000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
}

// ── Acceptor ────────────────────────────────────────────────────────────────

static void test_acceptor_end_to_end(void)
{
    fresh(0x0103);
    FEED(CMD(5), DISCOVER);
    OUT(ACC(5), DISCOVER, EP(1, 0, 0, 0));
    FEED(CMD(6), GETCAP, S(1));
    OUT(ACC(6), GETCAP, OURCAPS);
    FEED(CMD(7), GETALL, S(1));
    OUT(ACC(7), GETALL, OURCAPS, DELAYREP);
    FEED(CMD(8), GETCONF, S(1));
    OUT(REJ(8), GETCONF, E_BAD_STATE);
    FEED(CMD(9), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), DELAYREP);
    OUT(ACC(9), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    const bt_avdtp_sbc_t *s = bt_avdtp_settings(&A);
    CHECK(s && s->joint && s->bitpool == 51 && s->rate == 48000 && s->blocks == 16 && s->subbands == 8 && !s->snr);
    CHECK(!bt_avdtp_begin(&A, now));
    // Our endpoint is in use now.
    FEED(CMD(10), DISCOVER);
    OUT(ACC(10), DISCOVER, EP(1, 1, 0, 0));
    FEED(CMD(11), GETCONF, S(1));
    OUT(ACC(11), GETCONF, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), DELAYREP);
    // A Delay Report before the stream opens is taken.
    FEED(CMD(12), DELAY, S(1), 0x00, 0x96);
    OUT(ACC(12), DELAY);
    uint32_t d = 0;
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 150);
    FEED(CMD(13), OPEN, S(1));
    OUT(ACC(13), OPEN);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_AWAITING_MEDIA);
    CHECK(!bt_avdtp_wants_media(&A));     // theirs to open
    CHECK(bt_avdtp_media_wanted(&A));
    bt_avdtp_media_open(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    FEED(CMD(14), START, S(1));
    OUT(ACC(14), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(bt_avdtp_edges(&A)->their_starts, 1);
    FEED(CMD(15), SUSPEND, S(1));
    OUT(ACC(15), SUSPEND);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->their_suspends, 1);
    // We may start what they configured, toward their endpoint.
    CHECK(bt_avdtp_start(&A, now));
    OURS(START, S(5));
    FEED(ACC(LAST), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    FEED(CMD(0), CLOSE, S(1));
    OUT(ACC(0), CLOSE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(!bt_avdtp_media_wanted(&A));
    CHECK(bt_avdtp_settings(&A) == NULL);
    // Again, ended by their Abort from STREAMING.
    FEED(CMD(1), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | STEREO, B16 | SB8 | LOUD, 20, 40));
    OUT(ACC(1), SETCONF);
    s = bt_avdtp_settings(&A);
    CHECK(s && !s->joint && s->bitpool == 40);
    FEED(CMD(2), GETCONF, S(1));
    OUT(ACC(2), GETCONF, TRANSPORT, SBC(F48 | STEREO, B16 | SB8 | LOUD, 20, 40));
    FEED(CMD(3), OPEN, S(1));
    OUT(ACC(3), OPEN);
    bt_avdtp_media_open(&A, now);
    FEED(CMD(4), START, S(1));
    OUT(ACC(4), START);
    FEED(CMD(5), ABORT, S(1));
    OUT(ACC(5), ABORT);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(!bt_avdtp_media_wanted(&A));
    // Reconfigure: the category (none) and not supported. Security Control
    // and signals we do not know: General Reject.
    FEED(CMD(6), RECONF, S(1), 0x07, 0x06, 0, 0, 0x11, 0x15, 2, 53);
    OUT(REJ(6), RECONF, 0x00, E_NSC);
    FEED(CMD(7), SECCTL, S(1), 0xaa);
    OUT(GREJ(7), SECCTL);
    FEED(CMD(8), 0x0e);
    OUT(GREJ(8), 0x0e);
    FEED(CMD(9), 0x3f, 1, 2, 3);
    OUT(GREJ(9), 0x3f);
    FEED(CMD(10), 0x00);
    OUT(GREJ(10), 0x00);
    // The two reserved bits above a signal are not part of it.
    FEED(CMD(11), 0xc1);
    OUT(ACC(11), DISCOVER, EP(1, 0, 0, 0));
    CHECK_EQ(A.strays, 0); CHECK_EQ(A.malformed, 0);
}

// Their Get (All) Capabilities, Open, Close, Delay Report and Get
// Configuration with the wrong endpoint or no endpoint.
static void test_acceptor_bad_requests(void)
{
    fresh(0x0103);
    FEED(CMD(1), GETCAP, S(2)); OUT(REJ(1), GETCAP, E_BAD_SEID);
    FEED(CMD(2), GETALL, S(0)); OUT(REJ(2), GETALL, E_BAD_SEID);
    FEED(CMD(3), GETCAP); OUT(REJ(3), GETCAP, E_BAD_LENGTH);
    FEED(CMD(4), GETALL); OUT(REJ(4), GETALL, E_BAD_LENGTH);
    FEED(CMD(5), GETCONF); OUT(REJ(5), GETCONF, E_BAD_LENGTH);
    to_configured();
    FEED(CMD(1), GETCONF, S(3)); OUT(REJ(1), GETCONF, E_BAD_SEID);
    FEED(CMD(2), OPEN, S(2)); OUT(REJ(2), OPEN, E_BAD_SEID);
    FEED(CMD(3), OPEN); OUT(REJ(3), OPEN, E_BAD_LENGTH);
    FEED(CMD(4), CLOSE, S(2)); OUT(REJ(4), CLOSE, E_BAD_SEID);
    FEED(CMD(5), CLOSE); OUT(REJ(5), CLOSE, E_BAD_LENGTH);
    FEED(CMD(6), DELAY, S(2), 0, 1); OUT(REJ(6), DELAY, E_BAD_SEID);
    FEED(CMD(7), DELAY, S(1), 0); OUT(REJ(7), DELAY, E_BAD_LENGTH);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    FEED(CMD(8), OPEN, S(1)); OUT(ACC(8), OPEN);
    bt_avdtp_media_open(&A, now);
    FEED(CMD(9), START, S(2)); OUT(REJ(9), START, S(2), E_BAD_SEID);
    FEED(CMD(10), START); OUT(REJ(10), START, 0x00, E_BAD_LENGTH);
    // Start may list more than one endpoint; the first is ours.
    FEED(CMD(11), START, S(1), S(4)); OUT(ACC(11), START);
    FEED(CMD(12), SUSPEND, S(7)); OUT(REJ(12), SUSPEND, S(7), E_BAD_SEID);
    FEED(CMD(13), SUSPEND); OUT(REJ(13), SUSPEND, 0x00, E_BAD_LENGTH);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    // An Abort naming another endpoint, or none, has no answer: an Abort is never rejected.
    FEED(CMD(14), ABORT, S(2));
    NONE();
    FEED(CMD(15), ABORT);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(A.malformed, 2);
}

// Their Set Configuration, good and each way of being bad.
static void test_acceptor_set_configuration(void)
{
    static const struct { uint8_t m[40]; uint8_t n; uint8_t cat, code; } bad[] = {
#define CASE(cat, code, ...) { { __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), cat, code }
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53)),        // a rate we do not take
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | F44 | JOINT, B16 | SB8 | LOUD, 2, 53)),  // two bits in one field
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT | STEREO, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | MONO, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48, B16 | SB8 | LOUD, 2, 53)),                // no bit at all
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | B8 | SB8 | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B12 | SB8 | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB4 | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | SNR, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | SNR | LOUD, 2, 53)),
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 52, 60)),        // a range that misses ours
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 40, 30)),        // backwards
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 1, 30)),         // under 2
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, 0x07, 0x08, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0x00, 0x00),  // not SBC
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, 0x07, 0x06, 0x00, 0x01, F48 | JOINT, B16 | SB8 | LOUD, 2, 53), // MPEG, SBC's length
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, 0x07, 0x06, 0x10, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2, 53), // video
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, 0x07, 0x05, 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2),     // SBC of 3 bytes
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, 0x07, 0x00),                                     // zero-length codec
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53),
             SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),                                            // two codecs
        CASE(0x07, E_UNSUP, S(1), S(5), TRANSPORT),                                                 // no codec
        CASE(0x07, E_UNSUP, S(1), S(5)),
        CASE(0x02, E_UNSUP, S(1), S(5), TRANSPORT, 0x02, 0x00, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),   // a category we lack
        CASE(0x03, E_UNSUP, S(1), S(5), TRANSPORT, 0x03, 0x03, 1, 2, 2, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x04, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), 0x04, 0x02, 0x02, 0x00),
        CASE(0x05, E_UNSUP, S(1), S(5), TRANSPORT, 0x05, 0x01, 0xc0, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x06, E_UNSUP, S(1), S(5), TRANSPORT, 0x06, 0x01, 0x00, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x09, E_BAD_CAT, S(1), S(5), TRANSPORT, 0x09, 0x00, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)), // unknown
        CASE(0x00, E_BAD_CAT, S(1), S(5), 0x00, 0x00, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x01, E_TRANSPORT, S(1), S(5), 0x01, 0x01, 0x00, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),    // transport with a length
        CASE(0x01, E_TRANSPORT, S(1), S(5), TRANSPORT, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x08, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), 0x08, 0x01, 0x00),
        CASE(0x08, E_UNSUP, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), DELAYREP, DELAYREP),
        CASE(0x07, E_BAD_LENGTH, S(1), S(5), TRANSPORT, 0x07, 0x06, 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2),  // past the end
        CASE(0x07, E_BAD_LENGTH, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), 0x07),           // half a header
        CASE(0x00, E_BAD_SEID, S(2), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),   // wrong endpoint
        CASE(0x00, E_BAD_SEID, S(1), S(0), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),   // no endpoint of theirs
        CASE(0x00, E_BAD_SEID, S(1), S(0x3f), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53)),
        CASE(0x00, E_BAD_LENGTH, S(1)),                                                            // bad length
#undef CASE
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fresh(0x0103);
        uint8_t m[48] = { CMD(3), SETCONF };
        memcpy(m + 2, bad[i].m, bad[i].n);
        feed_(m, 2u + bad[i].n);
        uint8_t want[4] = { REJ(3), SETCONF, bad[i].cat, bad[i].code };
        out_(__func__, 1000 + (int)i, want, 4);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        CHECK(bt_avdtp_settings(&A) == NULL);
    }
    // No Set Configuration at all: one byte short of the two SEIDs.
    fresh(0x0103);
    FEED(CMD(3), SETCONF);
    OUT(REJ(3), SETCONF, 0x00, E_BAD_LENGTH);
    // Good ones: the bitpool is the top of their range, at most 51; SEID 0x3E is theirs to use.
    static const struct { uint8_t mode, lo, hi, pool; } good[] = {
        { JOINT, 2, 53, 51 }, { JOINT, 2, 51, 51 }, { STEREO, 2, 35, 35 }, { JOINT, 51, 51, 51 }, { JOINT, 51, 250, 51 },
        { STEREO, 2, 2, 2 },
    };
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        fresh(0x0103);
        uint8_t m[] = { CMD(4), SETCONF, S(1), S(0x3e), TRANSPORT, SBC(F48 | good[i].mode, B16 | SB8 | LOUD, good[i].lo, good[i].hi) };
        feed_(m, sizeof(m));
        OUT(ACC(4), SETCONF);
        const bt_avdtp_sbc_t *s = bt_avdtp_settings(&A);
        CHECK(s && s->bitpool == good[i].pool && s->joint == (good[i].mode == JOINT));
        CHECK_EQ(A.remote_seid, 0x3e);
        CHECK(!A.delay_reporting);
        FEED(CMD(5), GETCONF, S(1));
        uint8_t want[] = { ACC(5), GETCONF, TRANSPORT, SBC(F48 | good[i].mode, B16 | SB8 | LOUD, good[i].lo, good[i].hi) };
        out_(__func__, 2000 + (int)i, want, sizeof(want));
    }
    // Without media transport it is taken; the element order is theirs.
    fresh(0x0103);
    FEED(CMD(4), SETCONF, S(1), S(5), DELAYREP, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(4), SETCONF);
    CHECK(A.delay_reporting);
}

// A command each state does not allow is rejected (bad state); the ones it
// allows are accepted. Every state, every stream command.
typedef enum { D_CONNECTED, D_DISCOVERING, D_ASKING, D_CONFIGURING, D_CONFIGURED, D_OPENING, D_AWAITING, D_OPEN,
               D_STARTING, D_STREAMING, D_SUSPENDING, D_CLOSING, D_FAILED, D_COUNT } drive_t;
static const bt_avdtp_state_t drive_state[D_COUNT] = {
    BT_AVDTP_CONNECTED, BT_AVDTP_DISCOVERING, BT_AVDTP_ASKING, BT_AVDTP_CONFIGURING, BT_AVDTP_CONFIGURED,
    BT_AVDTP_OPENING, BT_AVDTP_AWAITING_MEDIA, BT_AVDTP_OPEN, BT_AVDTP_STARTING, BT_AVDTP_STREAMING,
    BT_AVDTP_SUSPENDING, BT_AVDTP_CLOSING, BT_AVDTP_FAILED };

static void drive(drive_t d)
{
    switch (d) {
    case D_CONNECTED: fresh(0x0103); break;
    case D_DISCOVERING: fresh(0x0103); bt_avdtp_begin(&A, now); OURS(DISCOVER); break;
    case D_ASKING: to_asking(0x0103); break;
    case D_CONFIGURING: to_configuring(); break;
    case D_CONFIGURED: to_configured(); break;
    case D_OPENING: to_opening(); break;
    case D_AWAITING: to_awaiting(); break;
    case D_OPEN: to_open(); break;
    case D_STARTING: to_open(); bt_avdtp_start(&A, now); OURS(START, S(3)); break;
    case D_STREAMING: to_streaming(); break;
    case D_SUSPENDING: to_streaming(); bt_avdtp_suspend(&A, now); OURS(SUSPEND, S(3)); break;
    case D_CLOSING: to_open(); bt_avdtp_close(&A, now); OURS(CLOSE, S(3)); break;
    case D_FAILED:      // two sessions gone wrong on one connection
        fresh(0x0103);
        bt_avdtp_begin(&A, now); OURS(DISCOVER); tick(now + 6000);
        bt_avdtp_signalling_closed(&A, now); bt_avdtp_signalling_open(&A, now, 0x0103);
        bt_avdtp_begin(&A, now); OURS(DISCOVER); tick(now + 6000);
        break;
    default: break;
    }
    CHECK_EQ(bt_avdtp_state(&A), drive_state[d]);
}

#define BIT(d) (1u << (d))
#define STREAMED (BIT(D_CONFIGURED) | BIT(D_OPENING) | BIT(D_AWAITING) | BIT(D_OPEN) | BIT(D_STARTING) | \
                  BIT(D_STREAMING) | BIT(D_SUSPENDING) | BIT(D_CLOSING))

static void test_acceptor_wrong_states(void)
{
    static const struct { uint8_t sig; uint8_t p[3]; uint8_t n; unsigned allowed; bt_avdtp_state_t after; } rows[] = {
        { OPEN, { S(1) }, 1, BIT(D_CONFIGURED), BT_AVDTP_AWAITING_MEDIA },
        { START, { S(1) }, 1, BIT(D_OPEN) | BIT(D_STARTING), BT_AVDTP_STREAMING },
        { SUSPEND, { S(1) }, 1, BIT(D_STREAMING) | BIT(D_SUSPENDING), BT_AVDTP_OPEN },
        { CLOSE, { S(1) }, 1, STREAMED, BT_AVDTP_CONNECTED },
        { DELAY, { S(1), 0x01, 0x00 }, 3, STREAMED, BT_AVDTP_IDLE },
        { GETCONF, { S(1) }, 1, STREAMED, BT_AVDTP_IDLE },
        { ABORT, { S(1) }, 1, ~0u, BT_AVDTP_IDLE },
    };
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        for (unsigned d = 0; d < D_COUNT; d++) {
            drive((drive_t)d);
            uint8_t m[8] = { CMD(9), rows[r].sig };
            memcpy(m + 2, rows[r].p, rows[r].n);
            feed_(m, 2u + rows[r].n);
            uint8_t got[64];
            size_t g = take(got, sizeof(got), 672);
            bool allowed = rows[r].allowed & BIT(d);
            if (allowed) {
                CHECK(g >= 2 && got[0] == ACC(9) && got[1] == rows[r].sig);
                // A command that moves the state moves it; Abort ends a stream, else leaves it.
                bt_avdtp_state_t want = rows[r].after;
                if (rows[r].sig == ABORT) want = (STREAMED & BIT(d)) ? BT_AVDTP_CONNECTED : drive_state[d];
                else if (want == BT_AVDTP_IDLE) want = drive_state[d];
                if (bt_avdtp_state(&A) != want) {
                    failures++;
                    printf("FAIL %s: signal %u in %s left %s, want %s\n", __func__, rows[r].sig,
                           bt_avdtp_state_name(drive_state[d]), bt_avdtp_state_name(bt_avdtp_state(&A)), bt_avdtp_state_name(want));
                }
            } else {
                uint8_t want[4] = { REJ(9), rows[r].sig };
                size_t wn = 2;
                if (rows[r].sig == START || rows[r].sig == SUSPEND) want[wn++] = S(1);
                want[wn++] = E_BAD_STATE;
                if (g != wn || memcmp(got, want, wn)) {
                    failures++;
                    printf("FAIL %s: signal %u in %s\n", __func__, rows[r].sig, bt_avdtp_state_name(drive_state[d]));
                    dump("got ", got, g); dump("want", want, wn);
                }
                CHECK_EQ(bt_avdtp_state(&A), drive_state[d]);
            }
        }
    }
    // Set Configuration: taken before our own is on the wire; in use from
    // then until the stream ends; bad state when no session can be made.
    for (unsigned d = 0; d < D_COUNT; d++) {
        drive((drive_t)d);
        FEED(CMD(9), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
        uint8_t got[64];
        size_t g = take(got, sizeof(got), 672);
        if (d <= D_ASKING) {
            CHECK(g == 2 && got[0] == ACC(9) && got[1] == SETCONF);
            CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
            CHECK_EQ(A.remote_seid, 5);
            NONE();      // our own sequence is abandoned
        } else {
            uint8_t code = d == D_FAILED ? E_BAD_STATE : E_IN_USE;
            uint8_t want[4] = { REJ(9), SETCONF, 0x00, code };
            if (g != 4 || memcmp(got, want, 4)) {
                failures++;
                printf("FAIL %s: Set Configuration in %s\n", __func__, bt_avdtp_state_name(drive_state[d]));
                dump("got ", got, g); dump("want", want, 4);
            }
            CHECK_EQ(bt_avdtp_state(&A), drive_state[d]);
        }
    }
}

// ── Both at once ────────────────────────────────────────────────────────────

static void test_both_at_once(void)
{
    // Before our Discover leaves: theirs is taken, ours is dropped.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(3), SETCONF);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    // During Discover: our answer, when it comes, is a stray.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(3), SETCONF);
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    CHECK_EQ(A.strays, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    NONE();
    tick(now + 7000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);   // no command of ours runs out
    // During ASKING.
    to_asking(0x0103);
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(3), SETCONF);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    CHECK_EQ(A.strays, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    CHECK_EQ(A.remote_seid, 5);
    // During CONFIGURING with ours queued and not yet sent: theirs is taken.
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(3), SETCONF);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    // During CONFIGURING with ours out: both reject each other; ours goes again once, after 1 s.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    uint64_t at = now;
    NONE();
    tick(at + 999);
    NONE();
    tick(at + 1000);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    tick(at + 1500);
    NONE();                                   // sent once, not again
    tick(at + 3000);
    NONE();
    FEED(ACC(LAST), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPENING);
    OURS(OPEN, S(3));
    // Bad state counts as theirs rejecting ours too.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_BAD_STATE);
    tick(now + 1000);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    // ...but only once: rejected again, a session gone wrong.
    FEED(CMD(4), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(4), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    OURS(ABORT, S(3));
    // While ours waits to go again, theirs is taken and ours is not sent.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    tick(now + 500);
    FEED(CMD(4), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | STEREO, B16 | SB8 | LOUD, 2, 40));
    OUT(ACC(4), SETCONF);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    tick(now + 1000);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    CHECK(bt_avdtp_settings(&A) && !bt_avdtp_settings(&A)->joint);
    // The retry is this session's: a collision in one session does not
    // earn a retry in the next on the same connection.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(ACC(LAST), SETCONF);
    OURS(OPEN, S(3));
    FEED(ACC(LAST), OPEN);
    bt_avdtp_media_open(&A, now);
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    FEED(ACC(LAST), CLOSE);
    bt_avdtp_signalling_closed(&A, now);      // a new channel, so that we may walk again
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    OURS(GETALL, S(3));
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    // ...nor does a retry spent in one session leave the next without one.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    tick(now + 1000);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    FEED(ACC(LAST), SETCONF);
    OURS(OPEN, S(3));
    FEED(ACC(LAST), OPEN);
    bt_avdtp_media_open(&A, now);
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    FEED(ACC(LAST), CLOSE);
    bt_avdtp_signalling_closed(&A, now);
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    OURS(GETALL, S(3));
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    FEED(CMD(4), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(4), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    tick(now + 1000);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    // Another reject code while both rejected: no retry.
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x07, E_UNSUP);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    // Their Start while ours is out: theirs is taken, and ours answered either way changes nothing.
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(CMD(5), START, S(1));
    OUT(ACC(5), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK(!bt_avdtp_suspend(&A, now));        // our Start is still out
    FEED(REJ(LAST), START, S(3), E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 0);   // no reject of the stream
    CHECK_EQ(bt_avdtp_edges(&A)->their_starts, 1);
    CHECK(bt_avdtp_suspend(&A, now));
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    FEED(CMD(5), START, S(1));
    OUT(ACC(5), START);
    FEED(ACC(LAST), START);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 0);
    // Their Suspend while ours is out.
    to_streaming();
    bt_avdtp_suspend(&A, now);
    OURS(SUSPEND, S(3));
    FEED(CMD(6), SUSPEND, S(1));
    OUT(ACC(6), SUSPEND);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK(!bt_avdtp_start(&A, now));          // our Suspend is still out
    FEED(REJ(LAST), SUSPEND, S(3), E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->suspend_rejects, 0);
    CHECK_EQ(bt_avdtp_edges(&A)->their_suspends, 1);
    to_streaming();
    bt_avdtp_suspend(&A, now);
    OURS(SUSPEND, S(3));
    FEED(CMD(6), SUSPEND, S(1));
    OUT(ACC(6), SUSPEND);
    FEED(ACC(LAST), SUSPEND);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK_EQ(bt_avdtp_edges(&A)->suspend_rejects, 0);
    // Their Close while ours is out: ours is forgotten.
    to_open();
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    FEED(CMD(7), CLOSE, S(1));
    OUT(ACC(7), CLOSE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    FEED(ACC(LAST), CLOSE);
    CHECK_EQ(A.strays, 1);

    // Their Set Configuration with an Abort of ours QUEUED: the Abort is
    // dropped, since a late Abort naming the same endpoint would tear down
    // the configuration just accepted. Only the accept goes.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(A.command_live && !A.command_sent);
    FEED(CMD(8), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(8), SETCONF);
    NONE();
    CHECK(!A.command_live);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    CHECK_EQ(A.remote_seid, 3);
    // ...with it already SENT: it is kept, its answer is taken and changes
    // nothing of the new session.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    OURS(ABORT, S(3));
    FEED(CMD(8), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(8), SETCONF);
    NONE();
    CHECK(A.command_live);
    FEED(ACC(LAST), ABORT);
    CHECK_EQ(A.strays, 0);
    CHECK(!A.command_live);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    CHECK(bt_avdtp_settings(&A) != NULL);
    // ...and with no answer, its own 2 s, then nothing.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    OURS(ABORT, S(3));
    FEED(CMD(8), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(8), SETCONF);
    tick(now + 1999);
    CHECK(A.command_live);
    tick(now + 1);
    CHECK(!A.command_live);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    CHECK_EQ(A.wrongs, 0);
    // Their Close of the new stream keeps a sent Abort the same way, and
    // its accept leaves CONNECTED as it is.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    OURS(ABORT, S(3));
    FEED(CMD(8), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(8), SETCONF);
    FEED(CMD(9), CLOSE, S(1));
    OUT(ACC(9), CLOSE);
    NONE();
    CHECK(A.command_live);
    FEED(ACC(LAST), ABORT);
    CHECK_EQ(A.strays, 0);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
}

// A command of ours still queued is retired when a command of theirs that
// we accept changes the stream's state; one already sent keeps its
// tracking. Every (their command, our queued command) pair that can occur.
typedef enum { Q_OPEN, Q_START, Q_SUSPEND, Q_CLOSE, Q_COUNT } queued_t;
static void queue_ours(queued_t q)
{
    switch (q) {
    case Q_OPEN: to_configuring(); FEED(ACC(LAST), SETCONF); CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPENING); break;
    case Q_START: to_open(); CHECK(bt_avdtp_start(&A, now)); break;
    case Q_SUSPEND: to_streaming(); CHECK(bt_avdtp_suspend(&A, now)); break;
    default: to_open(); CHECK(bt_avdtp_close(&A, now)); break;
    }
    CHECK(A.command_live && !A.command_sent);
    L++;                                      // its label goes with it, sent or not
}

static void test_queued_commands_retired(void)
{
    // The reviewer's sequence: our Start queued; their Start accepted; the
    // accept handed out; their Suspend accepted. Nothing of ours follows.
    queue_ours(Q_START);
    FEED(CMD(5), START, S(1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK(!A.command_live);
    // The pump is told nothing of ours is out: Suspend may be asked, Start may not (streaming).
    CHECK(!bt_avdtp_start(&A, now));
    OUT(ACC(5), START);
    FEED(CMD(6), SUSPEND, S(1));
    OUT(ACC(6), SUSPEND);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK(!A.command_live);
    CHECK_EQ(bt_avdtp_edges(&A)->start_rejects, 0);
    CHECK_EQ(bt_avdtp_edges(&A)->their_starts, 1);
    CHECK(bt_avdtp_start(&A, now));           // and we may start it ourselves now
    OURS(START, S(3));
    // ...and with Suspend asked of the pump right after their Start.
    queue_ours(Q_START);
    FEED(CMD(5), START, S(1));
    CHECK(bt_avdtp_suspend(&A, now));
    OUT(ACC(5), START);
    OURS(SUSPEND, S(3));
    NONE();
    // The mirror: our Suspend queued; their Suspend, then their Start.
    queue_ours(Q_SUSPEND);
    FEED(CMD(5), SUSPEND, S(1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    CHECK(!A.command_live);
    CHECK(!bt_avdtp_suspend(&A, now));        // open, so not ours to suspend
    OUT(ACC(5), SUSPEND);
    FEED(CMD(6), START, S(1));
    OUT(ACC(6), START);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK(!A.command_live);
    CHECK_EQ(bt_avdtp_edges(&A)->suspend_rejects, 0);
    // Their Close, and their Abort, with each of our commands queued: the
    // accept alone goes, and the session is over.
    for (int abort = 0; abort < 2; abort++)
        for (int q = 0; q < Q_COUNT; q++) {
            queue_ours((queued_t)q);
            if (abort) { FEED(CMD(7), ABORT, S(1)); OUT(ACC(7), ABORT); }
            else { FEED(CMD(7), CLOSE, S(1)); OUT(ACC(7), CLOSE); }
            NONE();
            if (bt_avdtp_state(&A) != BT_AVDTP_CONNECTED || A.command_live) {
                failures++;
                printf("FAIL %s: their %s with our queued command %d\n", __func__, abort ? "Abort" : "Close", q);
            }
            CHECK_EQ(A.wrongs, 0);
        }
    // Their Set Configuration with our Get All Capabilities queued (the
    // Discover, Set Configuration and Abort cases are in test_both_at_once).
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    CHECK(A.command_live && !A.command_sent);
    L++;
    FEED(CMD(8), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(8), SETCONF);
    NONE();
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    // A command of theirs that changes no state retires nothing: their
    // Abort with no stream leaves our queued Discover to go.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    FEED(CMD(9), ABORT, S(1));
    OUT(ACC(9), ABORT);
    OURS(DISCOVER);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_DISCOVERING);
    // Nor does one we refuse: their Start while our Close is queued is rejected, and our Close goes.
    queue_ours(Q_CLOSE);
    L--;
    FEED(CMD(9), START, S(1));
    OUT(REJ(9), START, S(1), E_BAD_STATE);
    OURS(CLOSE, S(3));
}

// One answer and one command may wait, and the answer goes first; a second
// command of theirs while an answer is owed is rejected (bad state).
static void test_one_answer_one_command(void)
{
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    FEED(CMD(4), DISCOVER);
    OUT(ACC(4), DISCOVER, EP(1, 0, 0, 0));
    OURS(DISCOVER);
    NONE();
    // Two commands before the channel frees: the second is rejected, bad state.
    fresh(0x0103);
    FEED(CMD(4), GETCAP, S(1));
    FEED(CMD(5), GETALL, S(1));
    OUT(ACC(4), GETCAP, OURCAPS);
    OUT(REJ(5), GETALL, E_BAD_STATE);
    NONE();
    // ...in the reject's layout for its signal.
    to_configured();
    FEED(CMD(4), GETCONF, S(1));
    FEED(CMD(5), OPEN, S(1));
    OUT(ACC(4), GETCONF, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53), DELAYREP);
    OUT(REJ(5), OPEN, E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    fresh(0x0103);
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(4), DISCOVER, EP(1, 0, 0, 0));
    OUT(REJ(5), SETCONF, 0x00, E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    to_open();
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), START, S(1));
    OUT(ACC(4), DISCOVER, EP(1, 1, 0, 0));
    OUT(REJ(5), START, S(1), E_BAD_STATE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_OPEN);
    // A third is one too many even for that: dropped and counted.
    fresh(0x0103);
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), DISCOVER);
    FEED(CMD(6), DISCOVER);
    OUT(ACC(4), DISCOVER, EP(1, 0, 0, 0));
    OUT(REJ(5), DISCOVER, E_BAD_STATE);
    NONE();
    CHECK_EQ(A.overrun, 1);
    // An Abort is obeyed even as the third: it takes the newer answer's
    // place, and that answer is what is counted as dropped.
    to_streaming();
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), GETCAP, S(1));
    FEED(CMD(6), ABORT, S(1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK_EQ(A.overrun, 1);
    OUT(ACC(4), DISCOVER, EP(1, 1, 0, 0));
    OUT(ACC(6), ABORT);
    NONE();
    // ...but an Abort naming another endpoint has no answer, so it displaces nothing.
    to_streaming();
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), GETCAP, S(1));
    FEED(CMD(6), ABORT, S(2));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_STREAMING);
    CHECK_EQ(A.overrun, 0);
    CHECK_EQ(A.malformed, 1);
    OUT(ACC(4), DISCOVER, EP(1, 1, 0, 0));
    OUT(REJ(5), GETCAP, E_BAD_STATE);
    NONE();
    // With our command waiting too, the answers go first, oldest first.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), GETCAP, S(1));
    OUT(ACC(4), DISCOVER, EP(1, 0, 0, 0));
    OUT(REJ(5), GETCAP, E_BAD_STATE);
    OURS(DISCOVER);
    // An Abort as the second is still obeyed and accepted.
    to_streaming();
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), ABORT, S(1));
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    OUT(ACC(4), DISCOVER, EP(1, 1, 0, 0));    // answered while the stream was still up
    OUT(ACC(5), ABORT);
    // An unknown signal as the second is still generally rejected.
    fresh(0x0103);
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), SECCTL, S(1));
    OUT(ACC(4), DISCOVER, EP(1, 0, 0, 0));
    OUT(GREJ(5), SECCTL);
    // A Set Configuration as the second while ours is out still counts as rejecting theirs.
    to_configuring();
    FEED(CMD(4), DISCOVER);
    FEED(CMD(5), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(4), DISCOVER, EP(1, 1, 0, 0));
    OUT(REJ(5), SETCONF, 0x00, E_BAD_STATE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    tick(now + 1000);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
    // A buffer too small: the message waits; an MTU too small: dropped and counted.
    fresh(0x0103);
    FEED(CMD(4), GETCAP, S(1));
    uint8_t small[4];
    CHECK_EQ(take(small, sizeof(small), 672), 0);
    CHECK_EQ(take(small, sizeof(small), 11), 0);
    CHECK_EQ(A.unsendable, 1);
    NONE();
    bt_avdtp_begin(&A, now);
    uint8_t big[64];
    CHECK_EQ(take(big, sizeof(big), 1), 0);
    CHECK_EQ(A.unsendable, 2);
    NONE();
    tick(now + 6000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);   // a command so dropped is left to its timer
    // An MTU exactly the message's length is enough.
    fresh(0x0103);
    FEED(CMD(4), GETCAP, S(1));
    CHECK_EQ(take(big, sizeof(big), 12), 12);
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC, DELAYREP);
    CHECK_EQ(take(big, 16, 16), 16);
}

// ── Sessions ending, and going wrong ────────────────────────────────────────

static void test_endings(void)
{
    // Their Close and Abort from OPEN and STREAMING.
    for (int k = 0; k < 4; k++) {
        if (k & 1) to_streaming(); else to_open();
        if (k & 2) { FEED(CMD(2), ABORT, S(1)); OUT(ACC(2), ABORT); }
        else { FEED(CMD(2), CLOSE, S(1)); OUT(ACC(2), CLOSE); }
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        CHECK(!bt_avdtp_media_wanted(&A));
        CHECK(bt_avdtp_settings(&A) == NULL);
        NONE();                               // we do not begin again by ourselves
        CHECK_EQ(A.wrongs, 0);
    }
    // Their Close and Abort from CONFIGURED: the stream ends there too.
    to_configured();
    FEED(CMD(2), CLOSE, S(1)); OUT(ACC(2), CLOSE);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    tick(now + 20000);
    NONE();
    // The media channel closing alone: Abort, CONNECTED.
    for (int k = 0; k < 4; k++) {
        if (k < 2) to_open(); else to_streaming();
        if (k == 1) { bt_avdtp_start(&A, now); OURS(START, S(3)); }
        if (k == 3) { bt_avdtp_suspend(&A, now); OURS(SUSPEND, S(3)); }
        unsigned stale = LAST;
        bt_avdtp_media_closed(&A, now, false);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
        OURS(ABORT, S(3));
        CHECK_EQ(A.wrongs, 0);
        // An answer to the command the Abort replaced is a stray.
        if (k == 1) { FEED(ACC(stale), START); CHECK_EQ(A.strays, 1); }
        FEED(ACC(LAST), ABORT);
    }
    // ...but not while closing, nor before the stream has its channel.
    to_open();
    bt_avdtp_close(&A, now);
    OURS(CLOSE, S(3));
    bt_avdtp_media_closed(&A, now, false);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CLOSING);
    NONE();
    to_configured();
    bt_avdtp_media_closed(&A, now, false);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);
    // Our media channel refused: Abort, then a session gone wrong.
    to_awaiting();
    bt_avdtp_media_closed(&A, now, true);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK_STR(A.why, "the media channel was refused");
    CHECK_EQ(A.media_refused, 1);
    CHECK(!bt_avdtp_wants_closed(&A));
    OURS(ABORT, S(3));
    CHECK(bt_avdtp_wants_closed(&A));
    to_awaiting();
    bt_avdtp_media_closed(&A, now, false);
    CHECK_STR(A.why, "the media channel closed");
    // Media opening outside AWAITING_MEDIA changes nothing.
    to_configured();
    bt_avdtp_media_open(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURED);

    // Signalling closing with the link up: everything cleared, IDLE; the second time FAILED.
    to_streaming();
    FEED(CMD(2), DISCOVER);                   // an answer owed goes with the channel
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    CHECK_EQ(A.wrongs, 1);
    CHECK_STR(A.why, "the signalling channel closed");
    CHECK(bt_avdtp_settings(&A) == NULL);
    CHECK(!bt_avdtp_media_wanted(&A));
    CHECK(!bt_avdtp_wants_closed(&A));
    NONE();
    FEED(CMD(3), DISCOVER);                   // nothing is taken without a channel
    NONE();
    CHECK_EQ(A.ignored, 1);
    CHECK(!bt_avdtp_begin(&A, now));
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);                           // our command is out: there is a session to go wrong
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_FAILED);
    CHECK_EQ(A.wrongs, 2);
    NONE();
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_FAILED);
    // The link dropping clears everything, silently.
    bt_avdtp_reset(&A);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    CHECK_EQ(A.wrongs, 0);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    CHECK_EQ(A.wrongs, 0);

    // A channel closed with no session in it is only a channel closed:
    // open, close, open, close while CONNECTED counts nothing.
    fresh(0x0103);
    for (int k = 0; k < 3; k++) {
        bt_avdtp_signalling_closed(&A, now);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
        bt_avdtp_signalling_open(&A, now, 0x0103);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    }
    CHECK_EQ(A.wrongs, 0);
    CHECK(A.why == NULL);
    // ...nor with our walk begun but its first command not yet sent...
    CHECK(bt_avdtp_begin(&A, now));
    L++;                                      // the Discover never sent took its label with it
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 0);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_may_begin(&A, now));       // a new channel may walk
    // ...nor with their commands answered and nothing of ours in flight.
    FEED(CMD(1), DISCOVER);
    OUT(ACC(1), DISCOVER, EP(1, 0, 0, 0));
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 0);
    // But with our Discover out, it is one...
    bt_avdtp_signalling_open(&A, now, 0x0103);
    CHECK(bt_avdtp_begin(&A, now));
    OURS(DISCOVER);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    // ...and with our Abort out in CONNECTED, and in CONFIGURED, one each.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    OURS(ABORT, S(3));
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 1);
    to_configured();
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 1);
    // An Abort queued and not sent is nothing in flight.
    to_open();
    bt_avdtp_media_closed(&A, now, false);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 0);
    // From CONFIGURING on there is a session, even with our Set
    // Configuration queued and not sent, or waiting to go again.
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 1);
    to_configuring();
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(REJ(3), SETCONF, 0x00, E_IN_USE);
    FEED(REJ(LAST), SETCONF, 0x00, E_IN_USE);
    CHECK(A.retry_waiting && !A.command_live);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(A.wrongs, 1);

    // A session gone wrong once: ENDING, the host closes both, IDLE; reopened, it can begin again.
    to_open();
    bt_avdtp_start(&A, now);
    OURS(START, S(3));
    tick(now + 6000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ENDING);
    CHECK(!bt_avdtp_media_wanted(&A));
    CHECK(!bt_avdtp_begin(&A, now));
    FEED(CMD(2), DISCOVER);                   // ENDING takes nothing
    CHECK_EQ(A.ignored, 1);
    OURS(ABORT, S(3));
    CHECK(bt_avdtp_wants_closed(&A));
    NONE();
    bt_avdtp_media_closed(&A, now, false);
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_IDLE);
    CHECK_EQ(A.wrongs, 1);
    CHECK(!bt_avdtp_wants_closed(&A));
    bt_avdtp_signalling_open(&A, now + 10000, 0x0103);
    now += 10000;
    CHECK(bt_avdtp_begin(&A, now));
    CHECK(A.walk_why == NULL);                // a new walk starts with no reason of the last walk...
    CHECK_STR(A.why, "a command went unanswered");    // ...and the last session gone wrong keeps its own
    OURS(DISCOVER);
    // ...twice: FAILED, and the host still closes both.
    FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
    OURS(GETALL, S(3));
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    tick(now + 6000);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_FAILED);
    CHECK_EQ(A.wrongs, 2);
    OURS(ABORT, S(3));
    CHECK(bt_avdtp_wants_closed(&A));
    bt_avdtp_signalling_closed(&A, now);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_FAILED);
    CHECK(!bt_avdtp_wants_closed(&A));
    CHECK_EQ(A.wrongs, 2);
    // A gone-wrong with no stream sends no Abort.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    tick(now + 6000);
    CHECK(bt_avdtp_wants_closed(&A));
    NONE();
    // An answer owed when a session goes wrong still leaves before the channels close.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(CMD(2), DISCOVER);
    tick(now + 6000);
    CHECK(!bt_avdtp_wants_closed(&A));
    OUT(ACC(2), DISCOVER, EP(1, 0, 0, 0));
    CHECK(bt_avdtp_wants_closed(&A));
}

// ── Fragments ───────────────────────────────────────────────────────────────

// Their Get All Capabilities answer cut into packets at the given payload
// offsets (the payload being what follows the signal).
static void send_cut(uint8_t label, const uint8_t *payload, size_t n, const size_t *cuts, unsigned ncuts)
{
    unsigned packets = ncuts + 1;
    size_t from = 0;
    for (unsigned k = 0; k < packets; k++) {
        size_t to = k < ncuts ? cuts[k] : n;
        uint8_t m[300];
        size_t mn = 0;
        if (k == 0) { m[mn++] = H(label, P_START, T_ACC); m[mn++] = (uint8_t)packets; m[mn++] = GETALL; }
        else m[mn++] = H(label, k + 1 == packets ? P_END : P_CONT, T_ACC);
        memcpy(m + mn, payload + from, to - from);
        mn += to - from;
        feed_(m, mn);
        from = to;
    }
}

static const uint8_t caps_answer[] = { TRANSPORT, ALLSBC, DELAYREP };

static void test_fragments(void)
{
    size_t n = sizeof(caps_answer);
    // Two packets, cut at every byte; three packets, cut at every pair.
    for (size_t i = 0; i <= n; i++) {
        to_asking(0x0103);
        size_t cuts[1] = { i };
        send_cut((uint8_t)LAST, caps_answer, n, cuts, 1);
        CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
        OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
        CHECK_EQ(A.fragments_dropped, 0);
    }
    for (size_t i = 0; i <= n; i++)
        for (size_t j = i; j <= n; j++) {
            to_asking(0x0103);
            size_t cuts[2] = { i, j };
            send_cut((uint8_t)LAST, caps_answer, n, cuts, 2);
            CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
            OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51), DELAYREP);
        }
    // Four packets is the most.
    to_asking(0x0103);
    size_t four[3] = { 2, 5, 9 };
    send_cut((uint8_t)LAST, caps_answer, n, four, 3);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    // More than four: dropped, and our question runs out.
    to_asking(0x0103);
    size_t five[4] = { 2, 5, 9, 11 };
    send_cut((uint8_t)LAST, caps_answer, n, five, 4);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    CHECK(A.fragments_dropped >= 1);
    NONE();
    // A start that says one packet, or none.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 1, GETALL, TRANSPORT, ALLSBC, DELAYREP);
    FEED(H(LAST, P_START, T_ACC), 0, GETALL, TRANSPORT, ALLSBC, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 2);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    // The packet count wrong: an end too early, a continue where the end should be.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 3, GETALL, TRANSPORT, 0x07);
    FEED(H(LAST, P_END, T_ACC), 0x06, 0x00, 0x00, 0xff, 0xff, 2, 53, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT, 0x07);
    FEED(H(LAST, P_CONT, T_ACC), 0x06, 0x00, 0x00);
    FEED(H(LAST, P_END, T_ACC), 0xff, 0xff, 2, 53, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 2);       // the continue, then the end with nothing begun
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    // A continue or an end with no start.
    to_asking(0x0103);
    FEED(H(LAST, P_CONT, T_ACC), 1, 2, 3);
    FEED(H(LAST, P_END, T_ACC), 1, 2, 3);
    CHECK_EQ(A.fragments_dropped, 2);
    // A start too short to carry its signal.
    FEED(H(LAST, P_START, T_ACC), 2);
    CHECK_EQ(A.fragments_dropped, 3);
    // A continue with another label, or another message type.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT);
    FEED(H(LAST + 1, P_END, T_ACC), ALLSBC, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 1);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT);
    FEED(H(LAST, P_END, T_REJ), ALLSBC, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 2);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    // A new start abandons an unfinished one; the new one is whole.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 3, GETALL, TRANSPORT);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT, ALLSBC);
    FEED(H(LAST, P_END, T_ACC), DELAYREP);
    CHECK_EQ(A.fragments_dropped, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    // A single packet in the middle is taken, and the reassembly goes on.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT);
    FEED(CMD(9), DISCOVER);
    FEED(H(LAST, P_END, T_ACC), ALLSBC, DELAYREP);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    OUT(ACC(9), DISCOVER, EP(1, 0, 0, 0));
    // A fragmented command of theirs is answered like a single one.
    fresh(0x0103);
    FEED(H(3, P_START, T_CMD), 3, SETCONF, S(1), S(5));
    FEED(H(3, P_CONT, T_CMD), TRANSPORT, 0x07, 0x06);
    FEED(H(3, P_END, T_CMD), 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2, 53);
    OUT(ACC(3), SETCONF);
    // A finished message leaves nothing open: the next fragmented one is whole too.
    FEED(H(4, P_START, T_CMD), 2, GETCONF);
    FEED(H(4, P_END, T_CMD), S(1));
    OUT(ACC(4), GETCONF, TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    CHECK_EQ(A.fragments_dropped, 0);
    CHECK(!A.rx_active);
    // The bound: a message of 256 bytes is taken, one of 257 dropped. A
    // padding element of an unknown category fills it out.
    for (int over = 0; over < 2; over++) {
        to_asking(0x0103);
        uint8_t pay[300];
        size_t pn = 0;
        size_t total = 256 + (size_t)over;          // header, signal, payload
        size_t pad = total - 2 - sizeof(caps_answer) - 2;
        pay[pn++] = 0x20; pay[pn++] = (uint8_t)pad;
        memset(pay + pn, 0xee, pad); pn += pad;
        memcpy(pay + pn, caps_answer, sizeof(caps_answer)); pn += sizeof(caps_answer);
        size_t cuts[3] = { 60, 130, 200 };
        send_cut((uint8_t)LAST, pay, pn, cuts, 3);
        CHECK_EQ(bt_avdtp_state(&A), over ? BT_AVDTP_ASKING : BT_AVDTP_CONFIGURING);
        CHECK_EQ(A.fragments_dropped, (unsigned)over);
    }
    // A start already over the bound.
    to_asking(0x0103);
    {
        uint8_t m[300] = { H(LAST, P_START, T_ACC), 2, GETALL };
        memset(m + 3, 0, sizeof(m) - 3);
        feed_(m, 258);
        CHECK_EQ(A.fragments_dropped, 1);
        feed_(m, 257);
        CHECK_EQ(A.fragments_dropped, 1);
        CHECK(A.rx_active);
    }
    // A message half reassembled goes with the signalling channel.
    to_asking(0x0103);
    FEED(H(LAST, P_START, T_ACC), 2, GETALL, TRANSPORT);
    bt_avdtp_signalling_closed(&A, now);
    bt_avdtp_signalling_open(&A, now, 0x0103);
    FEED(H(LAST, P_END, T_ACC), ALLSBC, DELAYREP);
    CHECK_EQ(A.fragments_dropped, 1);
    CHECK(!A.rx_active);
    // An empty payload is malformed.
    to_asking(0x0103);
    feed_((const uint8_t *)"", 0);
    CHECK_EQ(A.malformed, 1);
}

// ── Labels ──────────────────────────────────────────────────────────────────

static void test_labels(void)
{
    // Our labels run 0 to 15 and round again: OURS checks each.
    fresh(0x0103);
    for (int i = 0; i < 20; i++) {
        CHECK(bt_avdtp_begin(&A, now));
        OURS(DISCOVER);
        FEED(ACC(LAST), DISCOVER, EP(3, 0, 0, 1));
        OURS(GETALL, S(3));
        FEED(CMD(1), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
        OUT(ACC(1), SETCONF);
        FEED(CMD(2), CLOSE, S(1));
        OUT(ACC(2), CLOSE);
        bt_avdtp_signalling_closed(&A, now);  // a new channel each time, so that we may walk
        bt_avdtp_signalling_open(&A, now, 0x0103);
    }
    CHECK_EQ(A.wrongs, 0);
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 20);
    // A response with the wrong label, a duplicate, one to nothing, the right label and the wrong signal.
    to_asking(0x0103);
    FEED(ACC(LAST + 1), GETALL, TRANSPORT, ALLSBC);
    CHECK_EQ(A.strays, 1);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    FEED(ACC(LAST), GETCAP, TRANSPORT, ALLSBC);
    CHECK_EQ(A.strays, 2);
    FEED(REJ(LAST), DISCOVER, E_BAD_STATE);
    CHECK_EQ(A.strays, 3);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_ASKING);
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    FEED(ACC(LAST - 1), GETALL, TRANSPORT, ALLSBC);     // the duplicate, after its command is answered
    CHECK_EQ(A.strays, 4);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONFIGURING);
    OURS(SETCONF, S(3), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
    fresh(0x0103);
    FEED(ACC(0), DISCOVER, EP(3, 0, 0, 1));             // a response to nothing
    FEED(GREJ(0), DISCOVER);
    FEED(REJ(0));
    CHECK_EQ(A.strays, 3);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_CONNECTED);
    // A response to a command queued but not yet sent is a stray.
    bt_avdtp_begin(&A, now);
    FEED(ACC(0), DISCOVER, EP(3, 0, 0, 1));
    CHECK_EQ(A.strays, 4);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_DISCOVERING);
    // A general reject with the wrong label or signal.
    OURS(DISCOVER);
    FEED(GREJ(LAST + 3), DISCOVER);
    FEED(GREJ(LAST), GETCAP);
    FEED(H(LAST + 1, 0, 3));
    CHECK_EQ(A.strays, 7);
    CHECK_EQ(bt_avdtp_state(&A), BT_AVDTP_DISCOVERING);
    // A one-byte message that is not a reject is malformed.
    FEED(CMD(2));
    FEED(ACC(LAST));
    CHECK_EQ(A.malformed, 2);
}

// ── Malformed capabilities ──────────────────────────────────────────────────

static void test_malformed_capabilities(void)
{
    static const struct { uint8_t m[40]; uint8_t n; bool suits; } cases[] = {
#define CAPS(suits, ...) { { __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), suits }
        CAPS(false, TRANSPORT, 0x07, 0x09, 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2, 53),   // a length past the end
        CAPS(false, TRANSPORT, 0x07, 0x00),                                                     // a zero-length codec
        CAPS(false, TRANSPORT, 0x07, 0x01, 0x00),
        CAPS(false, TRANSPORT, 0x07, 0x05, 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2),        // SBC of three bytes
        CAPS(false, TRANSPORT, 0x07, 0x07, 0x00, 0x00, F48 | JOINT, B16 | SB8 | LOUD, 2, 53, 0),  // and of five
        CAPS(false, TRANSPORT, ALLSBC, 0x08),                                                   // half a header
        CAPS(false, 0x01, 0x01, 0x00, ALLSBC),                                                  // transport with a length
        CAPS(false, TRANSPORT),                                                                 // no codec
        CAPS(false, 0),                                                                         // half a header alone
        CAPS(true, TRANSPORT, 0x20, 0x02, 0xaa, 0xbb, ALLSBC),                                  // an unknown category
        CAPS(true, 0x00, 0x00, TRANSPORT, ALLSBC, 0x3f, 0x01, 0x00),
        CAPS(true, TRANSPORT, 0x02, 0x00, 0x04, 0x02, 0x02, 0x00, ALLSBC),                     // categories we do not use
        CAPS(true, TRANSPORT, 0x07, 0x08, 0x00, 0x02, 0x80, 0x01, 0x8c, 0x84, 0x00, 0x00, ALLSBC),  // AAC first, SBC second
        CAPS(true, TRANSPORT, 0x07, 0x02, 0x00, 0xff, ALLSBC),                                  // vendor, two bytes
        CAPS(true, TRANSPORT, ALLSBC, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53)),               // the first SBC is used
        CAPS(false, TRANSPORT, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53), ALLSBC),
        CAPS(true, ALLSBC),                                                                     // no transport
        CAPS(true, ALLSBC, 0x08, 0x02, 0x00, 0x00),                                             // delay reporting with bytes
#undef CAPS
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        to_asking(0x0103);
        uint8_t m[48] = { ACC(LAST), GETALL };
        memcpy(m + 2, cases[i].m, cases[i].n);
        feed_(m, 2u + cases[i].n);
        bt_avdtp_state_t want = cases[i].suits ? BT_AVDTP_CONFIGURING : BT_AVDTP_CONNECTED;
        if (bt_avdtp_state(&A) != want) {
            failures++;
            printf("FAIL %s: case %u left %s\n", __func__, i, bt_avdtp_state_name(bt_avdtp_state(&A)));
        }
        if (cases[i].suits) {
            CHECK(take((uint8_t[64]){0}, 64, 672) > 0);
        }
    }
    to_asking(0x0103);
    FEED(ACC(LAST), GETALL, TRANSPORT, 0x07, 0x00);
    CHECK_STR(A.walk_why, "no settings in common: capabilities malformed");
    CHECK_EQ(A.malformed, 1);
    // A malformed list moves on to the next sink.
    fresh(0x0103);
    bt_avdtp_begin(&A, now);
    OURS(DISCOVER);
    FEED(ACC(LAST), DISCOVER, EP(4, 0, 0, 1), EP(6, 0, 0, 1));
    OURS(GETALL, S(4));
    FEED(ACC(LAST), GETALL, TRANSPORT, 0x07, 0x09);
    OURS(GETALL, S(6));
    FEED(ACC(LAST), GETALL, TRANSPORT, ALLSBC);
    OURS(SETCONF, S(6), S(1), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 51));
}

// ── Edges, and the Delay Report's identifier ────────────────────────────────

static void test_edges_and_delay(void)
{
    // A session agreed, either way, moves the edge: a poller sees that the
    // session it knew ended and another began.
    to_configured();
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 1);
    FEED(CMD(2), CLOSE, S(1)); OUT(ACC(2), CLOSE);
    FEED(CMD(3), SETCONF, S(1), S(5), TRANSPORT, SBC(F48 | JOINT, B16 | SB8 | LOUD, 2, 53));
    OUT(ACC(3), SETCONF);
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 2);
    FEED(CMD(4), SETCONF, S(1), S(5), TRANSPORT, SBC(F44 | JOINT, B16 | SB8 | LOUD, 2, 53));   // refused: no move
    OUT(REJ(4), SETCONF, 0x00, E_IN_USE);
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 2);
    to_configuring();
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 0);    // not until it is accepted
    FEED(REJ(LAST), SETCONF, 0x07, E_UNSUP);
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 0);
    to_opening();
    CHECK_EQ(bt_avdtp_edges(&A)->sessions, 1);
    // The edges start again at init and at reset.
    to_streaming();
    FEED(CMD(2), SUSPEND, S(1)); OUT(ACC(2), SUSPEND);
    FEED(CMD(3), START, S(1)); OUT(ACC(3), START);
    CHECK(bt_avdtp_edges(&A)->their_starts == 1 && bt_avdtp_edges(&A)->their_suspends == 1);
    bt_avdtp_reset(&A);
    const bt_avdtp_edges_t *e = bt_avdtp_edges(&A);
    CHECK(!e->sessions && !e->their_starts && !e->their_suspends && !e->start_rejects && !e->suspend_rejects);

    // A Delay Report under our identifier or the stream's sink endpoint,
    // from CONFIGURED on; any other identifier is refused.
    uint32_t d = 0;
    to_streaming();                           // their endpoint is 3
    FEED(CMD(2), DELAY, S(3), 0x01, 0x2c);
    OUT(ACC(2), DELAY);
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 300);
    FEED(CMD(3), DELAY, S(1), 0x01, 0x2d);
    OUT(ACC(3), DELAY);
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 301);
    FEED(CMD(4), DELAY, S(5), 0x09, 0x99);
    OUT(REJ(4), DELAY, E_BAD_SEID);
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 301);
    to_configured();                          // their endpoint is 5
    FEED(CMD(2), DELAY, S(5), 0x00, 0x64);
    OUT(ACC(2), DELAY);
    CHECK(bt_avdtp_delay(&A, &d)); CHECK_EQ(d, 100);
    FEED(CMD(3), DELAY, S(3), 0x00, 0x65);
    OUT(REJ(3), DELAY, E_BAD_SEID);
    // Before a stream, their endpoint is no one's: the asking walk's sink is not configured.
    to_asking(0x0103);
    FEED(CMD(2), DELAY, S(3), 0x00, 0x64);
    OUT(REJ(2), DELAY, E_BAD_SEID);
    FEED(CMD(3), DELAY, S(1), 0x00, 0x64);
    OUT(REJ(3), DELAY, E_BAD_STATE);
    CHECK(!bt_avdtp_delay(&A, &d));
}

// ── The status file ─────────────────────────────────────────────────────────

static void test_status(void)
{
    char buf[1024];
    to_streaming();
    FEED(CMD(2), DELAY, S(1), 0x05, 0xdd);
    OUT(ACC(2), DELAY);
    size_t n = bt_avdtp_status(&A, buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf));
    CHECK(strstr(buf, "avdtp: streaming") != NULL);
    CHECK(strstr(buf, "bitpool 51 (2-51)") != NULL);
    CHECK(strstr(buf, "delay 150.1 ms") != NULL);
    CHECK(strstr(buf, "joint stereo") != NULL);
    for (size_t cap = 0; cap < 40; cap++) {
        char small[40];
        memset(small, 'x', sizeof(small));
        size_t m = bt_avdtp_status(&A, small, cap);
        CHECK(cap == 0 ? m == 0 : m < cap && small[m] == 0);
        for (size_t k = cap; k < sizeof(small); k++) CHECK(small[k] == 'x');
    }
    fresh(0x0103);
    bt_avdtp_begin(&A, now); OURS(DISCOVER); FEED(ACC(LAST), DISCOVER);
    bt_avdtp_status(&A, buf, sizeof(buf));
    CHECK(strstr(buf, "avdtp: connected") && strstr(buf, "our walk: no sink endpoint") && strstr(buf, "delay not reported"));
    drive(D_FAILED);
    bt_avdtp_status(&A, buf, sizeof(buf));
    CHECK(strstr(buf, "avdtp: failed, sessions made 0, gone wrong 2, last: a command went unanswered") != NULL);
    CHECK_STR(bt_avdtp_state_name(BT_AVDTP_AWAITING_MEDIA), "awaiting media");
    CHECK_STR(bt_avdtp_state_name((bt_avdtp_state_t)99), "?");
}

// ── Fuzz ────────────────────────────────────────────────────────────────────

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static bool streamed_or_opening(bt_avdtp_state_t s) { return s >= BT_AVDTP_CONFIGURED && s <= BT_AVDTP_CLOSING; }

// A command of ours still queued belongs to the state that queued it: one
// left behind by a change of theirs would act on a stream that has moved on.
static bool queued_fits(bt_avdtp_state_t s)
{
    if (!A.command_live || A.command_sent) return true;
    switch (A.command_signal) {
    case DISCOVER: return s == BT_AVDTP_DISCOVERING;
    case GETCAP: case GETALL: return s == BT_AVDTP_ASKING;
    case SETCONF: return s == BT_AVDTP_CONFIGURING;
    case OPEN: return s == BT_AVDTP_OPENING;
    case START: return s == BT_AVDTP_STARTING;
    case SUSPEND: return s == BT_AVDTP_SUSPENDING;
    case CLOSE: return s == BT_AVDTP_CLOSING;
    case ABORT: return s == BT_AVDTP_CONNECTED || s == BT_AVDTP_ENDING || s == BT_AVDTP_FAILED;
    default: return false;
    }
}

static void invariants(const char *where)
{
    bt_avdtp_state_t s = bt_avdtp_state(&A);
    bool ok = s <= BT_AVDTP_FAILED
        && (!bt_avdtp_wants_media(&A) || s == BT_AVDTP_AWAITING_MEDIA)
        && ((bt_avdtp_settings(&A) != NULL) == streamed_or_opening(s))
        && (bt_avdtp_media_wanted(&A) == (s >= BT_AVDTP_AWAITING_MEDIA && s <= BT_AVDTP_CLOSING))
        && A.answer[0].bytes <= BT_AVDTP_LONGEST && A.answer[1].bytes <= BT_AVDTP_LONGEST
        && A.command.bytes <= BT_AVDTP_LONGEST && (!A.answer[1].bytes || A.answer[0].bytes)
        && A.rx_have <= sizeof(A.rx) && A.endpoint_count <= BT_AVDTP_ENDPOINTS
        && (s != BT_AVDTP_IDLE || (!A.answer[0].bytes && !A.command_live))
        && (!bt_avdtp_wants_closed(&A) || s == BT_AVDTP_ENDING || s == BT_AVDTP_FAILED)
        && (!bt_avdtp_may_begin(&A, now) || (s == BT_AVDTP_CONNECTED && !A.command_live))
        && queued_fits(s);
    const bt_avdtp_sbc_t *set = bt_avdtp_settings(&A);
    if (set) ok = ok && set->rate == 48000 && set->blocks == 16 && set->subbands == 8 && !set->snr
                    && set->bitpool >= 2 && set->bitpool <= BT_AVDTP_BITPOOL;
    if (!ok) { failures++; printf("FAIL invariants after %s in %s\n", where, bt_avdtp_state_name(s)); }
    char buf[700];
    size_t n = bt_avdtp_status(&A, buf, sizeof(buf));
    if (n >= sizeof(buf) || buf[n]) { failures++; printf("FAIL status after %s\n", where); }
}

// Our output is always a whole single packet within the MTU asked.
static size_t drain_one(uint8_t *out, uint16_t mtu)
{
    size_t n = take(out, 64, mtu);
    if (n && (n > mtu || n > BT_AVDTP_LONGEST || n < 2 || ((out[0] >> 2) & 3) != P_SINGLE)) {
        failures++; printf("FAIL fuzz: we sent %zu bytes under an MTU of %u\n", n, mtu);
    }
    return n;
}

static void fuzz_random(int rounds)
{
    unsigned taken = 0;
    for (int r = 0; r < rounds; r++) {
        drive((drive_t)(rnd() % D_COUNT));
        for (int k = 0; k < 24; k++) {
            uint8_t m[300];
            size_t n = rnd() % 40;
            if (rnd() % 8 == 0) n = rnd() % 300;
            for (size_t i = 0; i < n; i++) m[i] = (uint8_t)rnd();
            if (n && rnd() % 2) m[0] = H(rnd() % 16, rnd() % 4, rnd() % 4);
            if (n > 1 && rnd() % 2) m[1] = (uint8_t)(rnd() % 15);
            if (n > 2 && rnd() % 2) m[2] = S(rnd() % 3);
            uint32_t before = A.malformed + A.strays + A.fragments_dropped;
            feed_(m, n);
            if (A.malformed + A.strays + A.fragments_dropped == before) taken++;
            invariants("random bytes");
            uint8_t out[64];
            switch (rnd() % 6) {
            case 0: tick(now + rnd() % 4000); break;
            case 1: drain_one(out, (uint16_t)(rnd() % 4 ? 672 : rnd() % 20)); break;
            case 2: if (bt_avdtp_wants_media(&A)) bt_avdtp_media_open(&A, now); else bt_avdtp_media_closed(&A, now, rnd() % 2); break;
            case 3: bt_avdtp_begin(&A, now); bt_avdtp_start(&A, now); break;
            case 4: if (rnd() % 8 == 0) bt_avdtp_signalling_closed(&A, now); else bt_avdtp_suspend(&A, now); break;
            default: if (rnd() % 4 == 0) bt_avdtp_close(&A, now); break;
            }
            invariants("an event");
        }
    }
    printf("fuzz: random bytes, %d rounds, %u messages taken without complaint\n", rounds, taken);
}

// A responsive random peer: answers our commands, mostly well, sometimes
// with a reject, sometimes not at all, sometimes in fragments; starts the
// profile itself; opens and closes channels. It must reach OPEN and
// STREAMING often enough to show it explored.
static void peer_answer(const uint8_t *m, size_t n)
{
    uint8_t label = m[0] >> 4, type = m[0] & 3, sig = m[1] & 0x3f;
    if (type != T_CMD) return;
    uint32_t roll = rnd() % 100;
    if (roll < 4) return;                                        // never answered
    uint8_t a[64];
    size_t an = 0;
    if (roll < 12) {                                             // rejected, in its layout
        a[an++] = REJ(label); a[an++] = sig;
        if (sig == SETCONF || sig == RECONF) a[an++] = 0x07;
        else if (sig == START || sig == SUSPEND) a[an++] = n > 2 ? m[2] : 0;
        a[an++] = rnd() % 2 ? E_IN_USE : E_BAD_STATE;
    } else if (roll < 14) {
        a[an++] = GREJ(label); a[an++] = sig;
    } else {
        a[an++] = ACC(label); a[an++] = sig;
        if (sig == DISCOVER) {
            uint8_t eps[] = { EP(1, 0, 0, 0), EP(2, rnd() % 4 == 0, 0, 1), EP(3, 0, rnd() % 6 == 0, 1) };
            memcpy(a + an, eps, sizeof(eps)); an += sizeof(eps);
        } else if (sig == GETCAP || sig == GETALL) {
            uint8_t b0 = rnd() % 10 ? F48 | F44 | JOINT | STEREO : F44 | JOINT;
            uint8_t caps[] = { TRANSPORT, SBC(b0, B16 | SB8 | LOUD | SNR, (uint8_t)(2 + rnd() % 40), (uint8_t)(30 + rnd() % 40)), DELAYREP };
            memcpy(a + an, caps, sizeof(caps)); an += sizeof(caps);
            if (sig == GETCAP) an -= 2;
        }
    }
    if (an > 4 && rnd() % 4 == 0) {                              // in two fragments
        size_t cut = 2 + rnd() % (an - 2);
        uint8_t f[64];
        f[0] = H(label, P_START, a[0] & 3); f[1] = 2; f[2] = sig;
        memcpy(f + 3, a + 2, cut - 2);
        feed_(f, 3 + cut - 2);
        f[0] = H(label, P_END, a[0] & 3);
        memcpy(f + 1, a + cut, an - cut);
        feed_(f, 1 + an - cut);
    } else feed_(a, an);
}

static void peer_command(void)
{
    uint8_t label = rnd() % 16;
    switch (rnd() % 10) {
    case 0: FEED(CMD(label), SETCONF, S(1), S(3), TRANSPORT, SBC(F48 | (rnd() % 2 ? JOINT : STEREO), B16 | SB8 | LOUD, 2, 53), DELAYREP); break;
    case 1: FEED(CMD(label), OPEN, S(1)); break;
    case 2: FEED(CMD(label), START, S(1)); break;
    case 3: FEED(CMD(label), SUSPEND, S(1)); break;
    case 4: if (rnd() % 4 == 0) FEED(CMD(label), CLOSE, S(1)); break;
    case 5: if (rnd() % 4 == 0) FEED(CMD(label), ABORT, S(1)); break;
    case 6: FEED(CMD(label), DELAY, S(1), (uint8_t)rnd(), (uint8_t)rnd()); break;
    case 7: FEED(CMD(label), DISCOVER); break;
    case 8: FEED(CMD(label), GETALL, S(1)); break;
    default: FEED(CMD(label), (uint8_t)(rnd() % 64), S(1)); break;
    }
}

static void fuzz_peer(int rounds)
{
    int reached_open = 0, reached_streaming = 0, failed = 0;
    for (int r = 0; r < rounds; r++) {
        static const uint16_t versions[] = { 0, 0x0102, 0x0103 };
        uint16_t version = versions[rnd() % 3];
        fresh(version);
        bool open = false, streaming = false;
        for (int step = 0; step < 250; step++) {
            uint8_t out[64];
            bt_avdtp_state_t s = bt_avdtp_state(&A);
            switch (rnd() % 10) {
            case 0: case 1: case 2: {
                size_t n = drain_one(out, 672);
                if (n) peer_answer(out, n);
                break;
            }
            case 3:
                if (s == BT_AVDTP_CONNECTED) bt_avdtp_begin(&A, now);
                else if (s == BT_AVDTP_OPEN) bt_avdtp_start(&A, now);
                else if (s == BT_AVDTP_STREAMING && rnd() % 3 == 0) { if (rnd() % 4) bt_avdtp_suspend(&A, now); else bt_avdtp_close(&A, now); }
                break;
            case 4:
                if (rnd() % 3 == 0) peer_command();
                break;
            case 5:
                if (s == BT_AVDTP_AWAITING_MEDIA && rnd() % 20) bt_avdtp_media_open(&A, now);
                else if (rnd() % 30 == 0) bt_avdtp_media_closed(&A, now, s == BT_AVDTP_AWAITING_MEDIA);
                break;
            case 6:
                tick(now + rnd() % 1500);
                break;
            default: {
                size_t n = drain_one(out, 672);
                if (n) peer_answer(out, n);
                break;
            }
            }
            if (bt_avdtp_wants_closed(&A)) { bt_avdtp_signalling_closed(&A, now); }
            if (bt_avdtp_state(&A) == BT_AVDTP_IDLE && rnd() % 3 == 0) bt_avdtp_signalling_open(&A, now, version);
            if (bt_avdtp_state(&A) == BT_AVDTP_FAILED) { failed++; fresh(version); }
            invariants("the peer");
            if (bt_avdtp_state(&A) == BT_AVDTP_OPEN) open = true;
            if (bt_avdtp_state(&A) == BT_AVDTP_STREAMING) streaming = true;
        }
        reached_open += open; reached_streaming += streaming;
    }
    printf("fuzz: a responsive peer, %d rounds: OPEN in %d, STREAMING in %d, FAILED %d times\n",
           rounds, reached_open, reached_streaming, failed);
    if (rounds >= 100 && (reached_open < rounds / 2 || reached_streaming < rounds / 2)) {
        failures++; printf("FAIL the peer fuzz did not reach OPEN and STREAMING often enough\n");
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    test_initiator_end_to_end();
    test_initiator_older_peers();
    test_initiator_unknown_version();
    test_parked();
    test_initiator_settings();
    test_initiator_get_all_rejected();
    test_initiator_walk();
    test_initiator_failures();
    test_initiator_rejects();
    test_timers();
    test_acceptor_end_to_end();
    test_acceptor_bad_requests();
    test_acceptor_set_configuration();
    test_acceptor_wrong_states();
    test_both_at_once();
    test_queued_commands_retired();
    test_one_answer_one_command();
    test_endings();
    test_fragments();
    test_labels();
    test_malformed_capabilities();
    test_edges_and_delay();
    test_status();
    fuzz_random(rounds);
    fuzz_peer(rounds);
    if (failures) { printf("bt_avdtp: %d failure(s)\n", failures); return 1; }
    printf("bt_avdtp: all scripted cases and %d fuzz rounds each passed\n", rounds);
    return 0;
}
