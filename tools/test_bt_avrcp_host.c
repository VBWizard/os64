// test_bt_avrcp_host.c — kernel/src/driver/system/usb/bt_avrcp.c, both
// roles. Model headphones, written here from AVCTP 1.3, AVRCP 1.4 and the
// AV/C frame (as BlueZ's avctp.c/avrcp.c and Android's avrc/avct read
// them), build every message they send and every message they expect from
// us byte by byte, never by calling the code under test: AVCTP's header
// (label 7-4, packet type 3-2, C/R 1, IPID 0, the profile big-endian), the
// AV/C header (type, the panel 0x48, the opcode), pass-through's operation
// and release bit, the vendor-dependent header under company 0x001958 (PDU,
// packet type, big-endian length). Every message is fed in heap storage of
// exactly its length, so a read past it is a sanitizer's report. They drive
// us as controller (their volume) and as target (their buttons), through
// every state, timer and reject, fragments cut at every byte, strays and
// duplicates, and then fuzz: random bytes in random states, and a
// responsive random peer that must reach READY and deliver presses.
//
//   tools/test_bt_avrcp_host.sh [fuzz rounds, default 3000]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_avrcp.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

static void dump(const char *what, const uint8_t *p, size_t n)
{
    printf("  %s:", what);
    for (size_t i = 0; i < n && i < 64; i++) printf(" %02x", p[i]);
    printf("\n");
}

// ── The wire, from the specification ────────────────────────────────────────

enum { P_SINGLE, P_START, P_CONT, P_END };
#define PROFILE 0x110E
// AV/C command and response types.
enum { C_CONTROL = 0x0, C_STATUS = 0x1, C_NOTIFY = 0x3, R_NOT_IMPL = 0x8, R_ACCEPTED = 0x9, R_REJECTED = 0xa,
       R_STABLE = 0xc, R_CHANGED = 0xd, R_INTERIM = 0xf };
#define PANEL 0x48
#define UNIT  0xff
enum { OP_VENDOR = 0x00, OP_UNIT = 0x30, OP_SUBUNIT = 0x31, OP_PASS = 0x7c };
enum { GET_CAPS = 0x10, LIST_ATTRS = 0x11, GET_ELEMENTS = 0x20, GET_PLAY = 0x30, REGISTER = 0x31,
       REQ_CONTINUE = 0x40, SET_VOLUME = 0x50 };
enum { EV_PLAYBACK = 0x01, EV_TRACK = 0x02, EV_POSITION = 0x05, EV_VOLUME = 0x0d };
enum { ST_INVALID_COMMAND = 0x00, ST_INVALID_PARAMETER = 0x01 };
static const uint8_t TAKEN[7] = { 0x41, 0x42, 0x44, 0x45, 0x46, 0x4b, 0x4c };   // volume up, down, play, stop, pause, forward, backward

typedef struct { uint8_t b[700]; size_t n; } msg_t;
static void put(msg_t *m, uint8_t v) { m->b[m->n++] = v; }
static void put16(msg_t *m, unsigned v) { put(m, (uint8_t)(v >> 8)); put(m, (uint8_t)v); }
static void putn(msg_t *m, const uint8_t *p, size_t n) { if (n) memcpy(m->b + m->n, p, n); m->n += n; }

// AVCTP's header for a single packet.
static msg_t avctp(unsigned label, bool response, bool ipid, unsigned profile)
{
    msg_t m = { .n = 0 };
    put(&m, (uint8_t)((label & 15) << 4 | P_SINGLE << 2 | (response ? 2 : 0) | (ipid ? 1 : 0)));
    put16(&m, profile);
    return m;
}
static msg_t frame(unsigned label, bool response, uint8_t ctype, uint8_t subunit, uint8_t opcode)
{
    msg_t m = avctp(label, response, false, PROFILE);
    put(&m, ctype); put(&m, subunit); put(&m, opcode);
    return m;
}
#define THEIRS(label, ctype, subunit, opcode) frame(label, false, ctype, subunit, opcode)   // their command
#define OURS(label, ctype, subunit, opcode) frame(label, true, ctype, subunit, opcode)     // our answer to it
#define REPLY(label, ctype) frame(label, true, ctype, PANEL, OP_VENDOR)                    // their answer to ours
// The vendor-dependent header: the SIG's company, the PDU, a single
// packet, the parameters' length.
static void vendor(msg_t *m, uint8_t pdu, unsigned len)
{
    put(m, 0x00); put(m, 0x19); put(m, 0x58); put(m, pdu); put(m, 0x00); put16(m, len);
}

// ── Driving it ──────────────────────────────────────────────────────────────

static bt_avrcp_t A;
static uint64_t T;

static void feed_bytes(const uint8_t *p, size_t n)
{
    uint8_t *x = malloc(n ? n : 1);
    if (n) memcpy(x, p, n);
    bt_avrcp_receive(&A, x, n, T);
    free(x);
}
static void feed(const msg_t *m) { feed_bytes(m->b, m->n); }

// The head message: asked twice, the same bytes and serial (nothing is
// consumed before bt_avrcp_sent); then sent. Never longer than the least
// MTU.
static size_t take(uint8_t *out)
{
    uint32_t s1, s2;
    uint8_t again[64];
    size_t n = bt_avrcp_next(&A, out, 64, 48, T, &s1);
    size_t n2 = bt_avrcp_next(&A, again, 64, 48, T, &s2);
    if (n != n2 || s1 != s2 || memcmp(out, again, n)) { failures++; printf("FAIL next twice is not the same\n"); }
    if (n > BT_AVRCP_LONGEST) { failures++; printf("FAIL a message of %zu bytes\n", n); }
    if (n && !bt_avrcp_sent(&A, s1, T)) { failures++; printf("FAIL sent refused its own serial\n"); }
    if (!n && s1) { failures++; printf("FAIL a serial with nothing\n"); }
    return n;
}
static void expect_(int line, const msg_t *w)
{
    uint8_t out[64];
    size_t n = take(out);
    if (n != w->n || memcmp(out, w->b, n)) {
        failures++; printf("FAIL line %d: wrong message\n", line); dump("want", w->b, w->n); dump("got ", out, n);
    }
}
#define expect(w) do { msg_t w_ = (w); expect_(__LINE__, &w_); } while (0)
static void expect_nothing_(int line)
{
    uint8_t out[64];
    size_t n = take(out);
    if (n) { failures++; printf("FAIL line %d: a message when none is owed\n", line); dump("got", out, n); }
}
#define expect_nothing() expect_nothing_(__LINE__)
// Everything owed, sent and thrown away; a bound on the steps, so a broken
// commit cannot make it spin.
static void drain(void)
{
    for (int i = 0; i < 4 * BT_AVRCP_ANSWERS && take((uint8_t[64]){ 0 }); i++) {}
}

// A command of ours: every byte but the label checked; the label returned.
static unsigned ours_(int line, uint8_t ctype, uint8_t pdu, const uint8_t *params, size_t pn)
{
    uint8_t out[64];
    size_t n = take(out);
    unsigned label = n ? out[0] >> 4 : 99;
    msg_t w = frame(label, false, ctype, PANEL, OP_VENDOR);
    vendor(&w, pdu, (unsigned)pn); putn(&w, params, pn);
    if (n != w.n || memcmp(out, w.b, n)) {
        failures++; printf("FAIL line %d: wrong command of ours\n", line); dump("want", w.b, w.n); dump("got ", out, n);
    }
    return label;
}
#define ASKED() ours_(__LINE__, C_STATUS, GET_CAPS, (const uint8_t[]){ 0x03 }, 1)
#define REGISTERED() ours_(__LINE__, C_NOTIFY, REGISTER, (const uint8_t[]){ EV_VOLUME, 0, 0, 0, 0 }, 5)
#define SET(v) ours_(__LINE__, C_CONTROL, SET_VOLUME, (const uint8_t[]){ (uint8_t)(v) }, 1)

// Their answers to ours.
static msg_t events(unsigned label, const uint8_t *ev, unsigned count)
{
    msg_t m = REPLY(label, R_STABLE);
    vendor(&m, GET_CAPS, 2 + count); put(&m, 0x03); put(&m, (uint8_t)count); putn(&m, ev, count);
    return m;
}
static msg_t volume_reply(unsigned label, uint8_t ctype, uint8_t v)
{
    msg_t m = REPLY(label, ctype);
    vendor(&m, REGISTER, 2); put(&m, EV_VOLUME); put(&m, v);
    return m;
}
static msg_t set_reply(unsigned label, uint8_t ctype, uint8_t v)
{
    msg_t m = REPLY(label, ctype);
    vendor(&m, SET_VOLUME, 1); put(&m, v);
    return m;
}
static msg_t refusal(unsigned label, uint8_t pdu, uint8_t status)
{
    msg_t m = REPLY(label, R_REJECTED);
    vendor(&m, pdu, 1); put(&m, status);
    return m;
}

static const uint8_t ALL_EVENTS[] = { EV_PLAYBACK, EV_TRACK, EV_VOLUME };

static void fresh(void) { bt_avrcp_init(&A); T = 1000; }

// To READY at their level `v`: Get Capabilities answered with the volume
// event, the registration answered INTERIM. Returns the registration's label.
static unsigned to_ready(uint8_t v)
{
    fresh();
    bt_avrcp_channel_open(&A, T);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_ASKING);
    unsigned g = ASKED();
    msg_t e = events(g, ALL_EVENTS, 3);
    feed(&e);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_REGISTERING);
    unsigned r = REGISTERED();
    msg_t i = volume_reply(r, R_INTERIM, v);
    feed(&i);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    return r;
}
static void expect_volume_(int line, uint8_t v)
{
    uint8_t got = 0xee;
    if (!bt_avrcp_volume(&A, &got) || got != v) { failures++; printf("FAIL line %d: volume %u, want %u\n", line, got, v); }
}
#define expect_volume(v) expect_volume_(__LINE__, v)

static void expect_press_(int line, uint8_t op, bool pressed)
{
    bt_avrcp_press_t p = { 0 };
    if (!bt_avrcp_press(&A, &p) || p.operation != op || p.pressed != pressed) {
        failures++; printf("FAIL line %d: press %02x %d, want %02x %d\n", line, p.operation, p.pressed, op, pressed);
    }
}
#define expect_press(op, pressed) expect_press_(__LINE__, op, pressed)
static void expect_no_press_(int line)
{
    bt_avrcp_press_t p;
    if (bt_avrcp_press(&A, &p)) { failures++; printf("FAIL line %d: a press %02x %d when none\n", line, p.operation, p.pressed); }
}
#define expect_no_press() expect_no_press_(__LINE__)

// Their pass-through and our answer to it.
static msg_t pass(unsigned label, uint8_t operation)
{
    msg_t m = THEIRS(label, C_CONTROL, PANEL, OP_PASS);
    put(&m, operation); put(&m, 0);
    return m;
}
static msg_t pass_answer(unsigned label, uint8_t ctype, uint8_t operation)
{
    msg_t m = OURS(label, ctype, PANEL, OP_PASS);
    put(&m, operation); put(&m, 0);
    return m;
}
// Their vendor-dependent command, and our answers to one.
static msg_t vcmd(unsigned label, uint8_t ctype, uint8_t pdu, const uint8_t *params, unsigned n)
{
    msg_t m = THEIRS(label, ctype, PANEL, OP_VENDOR);
    vendor(&m, pdu, n); putn(&m, params, n);
    return m;
}
static msg_t vanswer(unsigned label, uint8_t ctype, uint8_t pdu, const uint8_t *params, unsigned n)
{
    msg_t m = OURS(label, ctype, PANEL, OP_VENDOR);
    vendor(&m, pdu, n); putn(&m, params, n);
    return m;
}
static msg_t vreject(unsigned label, uint8_t pdu, uint8_t status) { return vanswer(label, R_REJECTED, pdu, &status, 1); }
#define P(...) (const uint8_t[]){__VA_ARGS__}, (unsigned)sizeof((const uint8_t[]){__VA_ARGS__})

// ── The controller: their volume ────────────────────────────────────────────

static void test_controller_path(void)
{
    // Nothing before the channel: IDLE, nothing to send, messages ignored.
    fresh();
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_IDLE);
    expect_nothing();
    msg_t e = events(0, ALL_EVENTS, 3);
    feed(&e);
    CHECK_EQ(A.ignored, 1);
    uint8_t v;
    CHECK(!bt_avrcp_volume(&A, &v));
    CHECK(!bt_avrcp_set_volume(&A, 10, T));

    // The whole path: their level known in READY, told once; the top bit
    // of their level is reserved and masked.
    unsigned r = to_ready(0x80 | 40);
    expect_volume(40);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 1);
    expect_nothing();
    // Opening again changes nothing.
    bt_avrcp_channel_open(&A, T);
    expect_nothing();
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    // A standing registration has no timer: an hour passes.
    T += 3600 * 1000; bt_avrcp_tick(&A, T);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    expect_nothing();

    // CHANGED: the new level, and the registration is made again under a
    // new label; its INTERIM tells the level again (a change made between
    // the CHANGED and the new registration is not lost).
    msg_t c = volume_reply(r, R_CHANGED, 50);
    feed(&c);
    expect_volume(50);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 2);
    unsigned r2 = REGISTERED();
    CHECK(r2 != r);
    msg_t i = volume_reply(r2, R_INTERIM, 55);
    feed(&i);
    expect_volume(55);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 3);
    CHECK_EQ(A.registrations, 2);
    // The old label is spent: a second CHANGED under it is a stray.
    feed(&c);
    CHECK_EQ(A.strays, 1);
    expect_volume(55);
    // The same level told again still counts as told.
    msg_t c2 = volume_reply(r2, R_CHANGED, 55);
    feed(&c2);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 4);
    unsigned r3 = REGISTERED();
    // The re-registration REJECTED: PLAIN, the level no longer known.
    msg_t rj = refusal(r3, REGISTER, ST_INVALID_PARAMETER);
    feed(&rj);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    CHECK(!bt_avrcp_volume(&A, &v));
    CHECK(!bt_avrcp_set_volume(&A, 10, T));
    expect_nothing();
    // ... and NOT IMPLEMENTED does the same.
    r = to_ready(10);
    c = volume_reply(r, R_CHANGED, 11);
    feed(&c);
    r2 = REGISTERED();
    msg_t ni = REPLY(r2, R_NOT_IMPL);
    vendor(&ni, REGISTER, 5); put(&ni, EV_VOLUME); put(&ni, 0); put(&ni, 0); put(&ni, 0); put(&ni, 0);
    feed(&ni);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    // A REJECTED in place of the standing registration's CHANGED: PLAIN.
    r = to_ready(10);
    rj = refusal(r, REGISTER, 0x16);
    feed(&rj);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    // A CHANGED before any INTERIM is taken: the level, READY, registered again.
    fresh();
    bt_avrcp_channel_open(&A, T);
    unsigned g = ASKED();
    e = events(g, ALL_EVENTS, 3);
    feed(&e);
    r = REGISTERED();
    c = volume_reply(r, R_CHANGED, 77);
    feed(&c);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    expect_volume(77);
    r2 = REGISTERED();
    CHECK(r2 != r);
    // A second INTERIM on a standing registration (the answer to its second
    // transmission) is their level too, the newer; it stands as before.
    i = volume_reply(r2, R_INTERIM, 70);
    feed(&i);
    expect_volume(70);
    uint32_t told = bt_avrcp_volume_serial(&A);
    i = volume_reply(r2, R_INTERIM, 60);
    feed(&i);
    expect_volume(60);
    CHECK_EQ(bt_avrcp_volume_serial(&A), told + 1);
    CHECK_EQ(A.strays, 0);
    expect_nothing();
    c = volume_reply(r2, R_CHANGED, 61);
    feed(&c);
    expect_volume(61);
    CHECK(REGISTERED() != r2);
}

static void test_controller_plain(void)
{
    uint8_t v;
    // Their events do not list the volume: PLAIN.
    static const uint8_t two[] = { EV_PLAYBACK, EV_TRACK };
    fresh();
    bt_avrcp_channel_open(&A, T);
    unsigned g = ASKED();
    msg_t e = events(g, two, 2);
    feed(&e);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    expect_nothing();
    CHECK(!bt_avrcp_volume(&A, &v));
    // The question is answered: a second answer under its label, listing
    // the volume this time, is a stray.
    e = events(g, ALL_EVENTS, 3);
    feed(&e);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    CHECK_EQ(A.strays, 1);
    expect_nothing();
    // An empty list, and the volume last of many.
    fresh();
    bt_avrcp_channel_open(&A, T);
    g = ASKED();
    e = events(g, NULL, 0);
    feed(&e);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    static const uint8_t many[] = { 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
    fresh();
    bt_avrcp_channel_open(&A, T);
    g = ASKED();
    e = events(g, many, sizeof(many));
    feed(&e);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_REGISTERING);
    // Get Capabilities REJECTED, and NOT IMPLEMENTED with its frame echoed
    // and with no operands at all.
    for (int kind = 0; kind < 3; kind++) {
        fresh();
        bt_avrcp_channel_open(&A, T);
        g = ASKED();
        msg_t m;
        if (kind == 0) m = refusal(g, GET_CAPS, ST_INVALID_COMMAND);
        else { m = REPLY(g, R_NOT_IMPL); if (kind == 1) { vendor(&m, GET_CAPS, 1); put(&m, 0x03); } }
        feed(&m);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
        expect_nothing();
    }
    // The registration REJECTED and NOT IMPLEMENTED.
    for (int kind = 0; kind < 2; kind++) {
        fresh();
        bt_avrcp_channel_open(&A, T);
        g = ASKED();
        e = events(g, ALL_EVENTS, 3);
        feed(&e);
        unsigned r = REGISTERED();
        msg_t m = kind ? REPLY(r, R_NOT_IMPL) : refusal(r, REGISTER, ST_INVALID_PARAMETER);
        feed(&m);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    }
    // An answer of theirs saying our profile is not one they speak (IPID):
    // the same as not implemented.
    fresh();
    bt_avrcp_channel_open(&A, T);
    g = ASKED();
    msg_t ip = avctp(g, true, true, PROFILE);
    feed(&ip);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_PLAIN);
    CHECK_EQ(A.invalid_profile, 1);
    // A malformed events answer is not believed: the command waits.
    fresh();
    bt_avrcp_channel_open(&A, T);
    g = ASKED();
    msg_t bad = REPLY(g, R_STABLE);
    vendor(&bad, GET_CAPS, 4); put(&bad, 0x03); put(&bad, 3); put(&bad, 1); put(&bad, EV_VOLUME);   // a count past the list
    feed(&bad);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_ASKING);
    bad = REPLY(g, R_STABLE);
    vendor(&bad, GET_CAPS, 3); put(&bad, 0x02); put(&bad, 1); put(&bad, EV_VOLUME);   // the company capability
    feed(&bad);
    bad = REPLY(g, R_INTERIM);                                                      // not STABLE
    vendor(&bad, GET_CAPS, 3); put(&bad, 0x03); put(&bad, 1); put(&bad, EV_VOLUME);
    feed(&bad);
    bad = REPLY(g, R_STABLE);                                                       // its length past the bytes
    vendor(&bad, GET_CAPS, 4); put(&bad, 0x03); put(&bad, 1); put(&bad, EV_VOLUME);
    feed(&bad);
    bad = REPLY(g, R_STABLE);                                                       // AVRCP's own fragment
    vendor(&bad, GET_CAPS, 3); bad.b[10] = 1; put(&bad, 0x03); put(&bad, 1); put(&bad, EV_VOLUME);
    feed(&bad);
    bad = REPLY(g, R_STABLE);                                                       // another company
    vendor(&bad, GET_CAPS, 3); bad.b[8] = 0x59; put(&bad, 0x03); put(&bad, 1); put(&bad, EV_VOLUME);
    feed(&bad);
    bad = REPLY(g, R_STABLE);                                                       // a header cut short
    put(&bad, 0x00); put(&bad, 0x19); put(&bad, 0x58); put(&bad, GET_CAPS);
    feed(&bad);
    bad = avctp(g, true, false, PROFILE); put(&bad, R_STABLE); put(&bad, PANEL);   // no opcode
    feed(&bad);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_ASKING);
    CHECK_EQ(A.malformed, 8);
    // A volume answer of the wrong event or length is not believed either.
    fresh();
    bt_avrcp_channel_open(&A, T);
    g = ASKED();
    e = events(g, ALL_EVENTS, 3);
    feed(&e);
    unsigned r = REGISTERED();
    msg_t w = REPLY(r, R_INTERIM);
    vendor(&w, REGISTER, 2); put(&w, EV_PLAYBACK); put(&w, 9);
    feed(&w);
    w = REPLY(r, R_INTERIM);
    vendor(&w, REGISTER, 3); put(&w, EV_VOLUME); put(&w, 9); put(&w, 9);
    feed(&w);
    w = volume_reply(r, R_ACCEPTED, 9);
    feed(&w);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_REGISTERING);
    CHECK_EQ(A.malformed, 3);
    w = volume_reply(r, R_INTERIM, 9);
    feed(&w);
    expect_volume(9);
}

static void test_controller_sets(void)
{
    // Set answered ACCEPTED with the level asked, and with another (they
    // round): the level they took is believed.
    unsigned r = to_ready(30);
    CHECK(bt_avrcp_set_volume(&A, 64, T));
    unsigned s = SET(64);
    CHECK(s != r);
    msg_t a = set_reply(s, R_ACCEPTED, 64);
    feed(&a);
    expect_volume(64);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 2);
    CHECK(bt_avrcp_set_volume(&A, 0xff, T));      // the top bit is sent clear
    s = SET(0x7f);
    a = set_reply(s, R_ACCEPTED, 0x80 | 120);
    feed(&a);
    expect_volume(120);
    CHECK_EQ(A.sets, 2);
    CHECK_EQ(A.sets_accepted, 2);
    // The answer comes once: a second is a stray.
    feed(&a);
    CHECK_EQ(A.strays, 1);
    // INTERIM, then ACCEPTED: the INTERIM starts its timer again.
    CHECK(bt_avrcp_set_volume(&A, 10, T));
    s = SET(10);
    T += 1500;
    msg_t in = set_reply(s, R_INTERIM, 10);
    feed(&in);
    T += 1999; bt_avrcp_tick(&A, T);
    expect_nothing();
    CHECK_EQ(A.silences, 0);
    // An ACCEPTED of the wrong length is not believed: the Set waits on.
    msg_t odd = REPLY(s, R_ACCEPTED);
    vendor(&odd, SET_VOLUME, 2); put(&odd, 11); put(&odd, 11);
    feed(&odd);
    odd = REPLY(s, R_ACCEPTED);
    vendor(&odd, SET_VOLUME, 0);
    feed(&odd);
    CHECK_EQ(A.malformed, 2);
    expect_volume(120);
    a = set_reply(s, R_ACCEPTED, 10);
    feed(&a);
    expect_volume(10);
    CHECK_EQ(A.sets_interim, 1);
    // REJECTED: counted, the level as it was.
    CHECK(bt_avrcp_set_volume(&A, 90, T));
    s = SET(90);
    msg_t rj = refusal(s, SET_VOLUME, ST_INVALID_PARAMETER);
    feed(&rj);
    expect_volume(10);
    CHECK_EQ(A.sets_rejected, 1);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    // NOT IMPLEMENTED likewise.
    CHECK(bt_avrcp_set_volume(&A, 91, T));
    s = SET(91);
    msg_t ni = REPLY(s, R_NOT_IMPL);
    feed(&ni);
    CHECK_EQ(A.sets_rejected, 2);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
    // The standing registration is untouched by any of it.
    msg_t c = volume_reply(r, R_CHANGED, 33);
    feed(&c);
    expect_volume(33);
    REGISTERED();

    // Superseded while unsent: only the newest leaves, its old serial is
    // stale, and the one replaced is not counted.
    r = to_ready(30);
    CHECK(bt_avrcp_set_volume(&A, 40, T));
    uint8_t out[64];
    uint32_t old_serial, new_serial;
    CHECK(bt_avrcp_next(&A, out, sizeof(out), 48, T, &old_serial) > 0);
    unsigned parked = out[0] >> 4;
    CHECK(bt_avrcp_set_volume(&A, 41, T));
    CHECK(!bt_avrcp_sent(&A, old_serial, T));
    CHECK_EQ(A.stale_sends, 1);
    CHECK(bt_avrcp_next(&A, out, sizeof(out), 48, T, &new_serial) > 0);
    CHECK(new_serial != old_serial);
    CHECK(bt_avrcp_set_volume(&A, 42, T));
    s = SET(42);
    CHECK(s != parked);
    expect_nothing();
    CHECK_EQ(A.sets, 1);
    // An answer under the replaced label, never sent, is a stray.
    a = set_reply(parked, R_ACCEPTED, 40);
    feed(&a);
    CHECK_EQ(A.strays, 1);
    expect_volume(30);
    // Superseded while out: held, the newest of several sent when the one
    // out is answered.
    CHECK(bt_avrcp_set_volume(&A, 50, T));
    CHECK(bt_avrcp_set_volume(&A, 51, T));
    expect_nothing();
    a = set_reply(s, R_ACCEPTED, 42);
    feed(&a);
    expect_volume(42);
    unsigned s2 = SET(51);
    CHECK(s2 != s);
    // ... and when the one out is rejected.
    CHECK(bt_avrcp_set_volume(&A, 60, T));
    rj = refusal(s2, SET_VOLUME, ST_INVALID_PARAMETER);
    feed(&rj);
    s = SET(60);
    a = set_reply(s, R_ACCEPTED, 60);
    feed(&a);
    expect_volume(60);
    expect_nothing();
    // ... and when the one out is answered INTERIM: still held.
    CHECK(bt_avrcp_set_volume(&A, 61, T));
    s = SET(61);
    CHECK(bt_avrcp_set_volume(&A, 62, T));
    in = set_reply(s, R_INTERIM, 61);
    feed(&in);
    expect_nothing();
    a = set_reply(s, R_ACCEPTED, 61);
    feed(&a);
    SET(62);

    // Our Set crossing their CHANGED: whichever arrives last is believed.
    r = to_ready(30);
    CHECK(bt_avrcp_set_volume(&A, 60, T));
    s = SET(60);
    c = volume_reply(r, R_CHANGED, 20);
    feed(&c);
    expect_volume(20);
    a = set_reply(s, R_ACCEPTED, 60);
    feed(&a);
    expect_volume(60);
    unsigned rr = REGISTERED();
    msg_t i = volume_reply(rr, R_INTERIM, 60);
    feed(&i);
    expect_volume(60);
    // ... and the other way.
    CHECK(bt_avrcp_set_volume(&A, 70, T));
    s = SET(70);
    a = set_reply(s, R_ACCEPTED, 70);
    feed(&a);
    c = volume_reply(rr, R_CHANGED, 25);
    feed(&c);
    expect_volume(25);
    CHECK_EQ(bt_avrcp_volume_serial(&A), 6);
    rr = REGISTERED();
    i = volume_reply(rr, R_INTERIM, 25);
    feed(&i);
    // Our two live labels are never the same, through the sixteen and round.
    for (int k = 0; k < 40; k++) {
        CHECK(bt_avrcp_set_volume(&A, (uint8_t)k, T));
        s = SET(k);
        if (s == rr) { failures++; printf("FAIL a Set under the registration's label\n"); }
        a = set_reply(s, R_ACCEPTED, (uint8_t)k);
        feed(&a);
    }
    expect_volume(39);
}

static void test_controller_silence(void)
{
    // Each command of ours unanswered once has a second period; unanswered
    // again, FAILED. A question or registration is sent again for it, the
    // same label and bytes under a new serial; a Set is not sent twice.
    for (int which = 0; which < 3; which++) {
        unsigned label;
        uint32_t first_serial = 0;
        uint8_t first[64];
        if (which == 0) {
            fresh(); bt_avrcp_channel_open(&A, T);
            bt_avrcp_next(&A, first, sizeof(first), 48, T, &first_serial);
            label = ASKED();
        } else if (which == 1) {
            fresh(); bt_avrcp_channel_open(&A, T);
            unsigned g = ASKED();
            msg_t e = events(g, ALL_EVENTS, 3);
            feed(&e);
            bt_avrcp_next(&A, first, sizeof(first), 48, T, &first_serial);
            label = REGISTERED();
        } else {
            to_ready(30);
            CHECK(bt_avrcp_set_volume(&A, 5, T));
            label = SET(5);
        }
        T += BT_AVRCP_REPLY_MS - 1; bt_avrcp_tick(&A, T);
        expect_nothing();
        T += 1; bt_avrcp_tick(&A, T);
        if (which < 2) {
            // Sent again under a serial of its own.
            uint8_t o[64];
            uint32_t s1 = 0;
            CHECK(bt_avrcp_next(&A, o, sizeof(o), 48, T, &s1) > 0);
            CHECK(s1 != first_serial);
            unsigned again = which == 0 ? ASKED() : REGISTERED();
            CHECK_EQ(again, label);
        }
        else expect_nothing();
        CHECK_EQ(A.silences, 1);
        CHECK(bt_avrcp_state(&A) != BT_AVRCP_FAILED);
        // The answer to either transmission is its answer...
        if (which == 0) {
            msg_t e = events(label, ALL_EVENTS, 3);
            feed(&e);
            CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_REGISTERING);
            label = REGISTERED();
        } else if (which == 1) {
            msg_t i = volume_reply(label, R_INTERIM, 3);
            feed(&i);
            CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_READY);
            CHECK(bt_avrcp_set_volume(&A, 6, T));
            label = SET(6);
        } else {
            msg_t a = set_reply(label, R_ACCEPTED, 5);
            feed(&a);
            expect_volume(5);
            CHECK(bt_avrcp_set_volume(&A, 6, T));
            label = SET(6);
        }
        // ... and a new command has two silences of its own: the second is
        // FAILED (the Register sent again between them, the Set not).
        T += BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T);
        if (which == 0) CHECK_EQ(REGISTERED(), label);
        else expect_nothing();
        T += BT_AVRCP_REPLY_MS - 1; bt_avrcp_tick(&A, T);
        CHECK(bt_avrcp_state(&A) != BT_AVRCP_FAILED);
        T += 1; bt_avrcp_tick(&A, T);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_FAILED);
        CHECK_EQ(A.silences, 3);
        uint8_t v;
        CHECK(!bt_avrcp_volume(&A, &v));
        CHECK(!bt_avrcp_set_volume(&A, 1, T));
        expect_nothing();
        // A late answer is a stray.
        msg_t late = set_reply(label, R_ACCEPTED, 6);
        feed(&late);
        CHECK(A.strays >= 1);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_FAILED);
    }
    // The timer runs from the sending: a command the carrier holds back is
    // not silent.
    fresh(); bt_avrcp_channel_open(&A, T);
    T += 10 * BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T);
    CHECK_EQ(A.silences, 0);
    unsigned g = ASKED();
    T += BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T);
    CHECK_EQ(ASKED(), g);
    // A message too long for the channel's MTU is dropped and counted; a
    // command so dropped is treated as sent and left to its timer.
    fresh(); bt_avrcp_channel_open(&A, T);
    uint8_t out[64];
    uint32_t serial = 7;
    CHECK_EQ(bt_avrcp_next(&A, out, sizeof(out), 13, T, &serial), 0);
    CHECK_EQ(serial, 0);
    CHECK_EQ(A.unsendable, 1);
    T += BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T);
    CHECK_EQ(A.silences, 1);
    CHECK_EQ(bt_avrcp_next(&A, out, sizeof(out), 14, T, &serial), 14);
    // Too little room: nothing, and the message waits.
    fresh(); bt_avrcp_channel_open(&A, T);
    CHECK_EQ(bt_avrcp_next(&A, out, 13, 48, T, &serial), 0);
    CHECK_EQ(serial, 0);
    CHECK_EQ(bt_avrcp_next(&A, out, 14, 48, T, &serial), 14);
    CHECK(!bt_avrcp_sent(&A, 0, T));
    CHECK(!bt_avrcp_sent(&A, serial + 1, T));
    CHECK(bt_avrcp_sent(&A, serial, T));
    CHECK(!bt_avrcp_sent(&A, serial, T));
    CHECK_EQ(A.stale_sends, 3);
}

static void test_channel_closing(void)
{
    // In every state the channel's close is IDLE, and everything it carried
    // goes: the level, our commands, their registrations, answers owed.
    for (int state = 0; state < 6; state++) {
        fresh();
        if (state >= 1) bt_avrcp_channel_open(&A, T);
        if (state == 2) { unsigned g = ASKED(); msg_t e = events(g, ALL_EVENTS, 3); feed(&e); }
        if (state == 3) { to_ready(20); bt_avrcp_set_volume(&A, 5, T); SET(5); bt_avrcp_set_volume(&A, 6, T); }
        if (state == 4) { unsigned g = ASKED(); msg_t m = refusal(g, GET_CAPS, 0); feed(&m); }
        if (state == 5) { ASKED(); T += 2 * BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T); ASKED(); T += BT_AVRCP_REPLY_MS; bt_avrcp_tick(&A, T); }
        static const bt_avrcp_state_t want[] = { BT_AVRCP_IDLE, BT_AVRCP_ASKING, BT_AVRCP_REGISTERING, BT_AVRCP_READY,
                                                 BT_AVRCP_PLAIN, BT_AVRCP_FAILED };
        CHECK_EQ(bt_avrcp_state(&A), want[state]);
        if (state) {
            msg_t reg = vcmd(3, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
            feed(&reg);                             // an answer owed, left unsent
            msg_t p = pass(4, 0x44);
            feed(&p);
        }
        bt_avrcp_channel_closed(&A, T);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_IDLE);
        uint8_t v;
        CHECK(!bt_avrcp_volume(&A, &v));
        expect_nothing();
        CHECK(!A.play_registered && !A.set_pending);
        if (state) { expect_press(0x44, true); expect_press(0x44, false); }   // the press held is released
        expect_no_press();
        // A status change finds no registration.
        bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
        bt_avrcp_playback(&A, BT_AVRCP_STOPPED, T);
        expect_nothing();
        // Opened again, it begins again.
        bt_avrcp_channel_open(&A, T);
        CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_ASKING);
        ASKED();
    }
    // A fragment in progress goes with the channel.
    fresh(); bt_avrcp_channel_open(&A, T); ASKED();
    feed_bytes((const uint8_t[]){ 0x14, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    bt_avrcp_channel_closed(&A, T);
    bt_avrcp_channel_open(&A, T); ASKED();
    feed_bytes((const uint8_t[]){ 0x1c, OP_PASS, 0x44, 0 }, 4);
    CHECK_EQ(A.fragments_dropped, 1);
    expect_nothing();
}

// ── The target: their buttons ───────────────────────────────────────────────

static void test_presses(void)
{
    to_ready(20);
    // Every operation taken, pressed and released: ACCEPTED, and an event each.
    for (int i = 0; i < 7; i++) {
        msg_t p = pass((unsigned)i, TAKEN[i]);
        feed(&p);
        expect(pass_answer((unsigned)i, R_ACCEPTED, TAKEN[i]));
        expect_press(TAKEN[i], true);
        p = pass((unsigned)i + 8, (uint8_t)(0x80 | TAKEN[i]));
        feed(&p);
        expect(pass_answer((unsigned)i + 8, R_ACCEPTED, (uint8_t)(0x80 | TAKEN[i])));
        expect_press(TAKEN[i], false);
        CHECK_EQ(A.presses[i], 1);
    }
    expect_no_press();
    // Operations not taken: NOT IMPLEMENTED, no event.
    static const uint8_t not_taken[] = { 0x00, 0x01, 0x40, 0x43, 0x47, 0x48, 0x49, 0x4a, 0x4d, 0x7e, 0x7f };
    for (size_t i = 0; i < sizeof(not_taken); i++) {
        msg_t p = pass(5, not_taken[i]);
        feed(&p);
        expect(pass_answer(5, R_NOT_IMPL, not_taken[i]));
        p = pass(6, (uint8_t)(0x80 | not_taken[i]));
        feed(&p);
        expect(pass_answer(6, R_NOT_IMPL, (uint8_t)(0x80 | not_taken[i])));
    }
    expect_no_press();
    CHECK_EQ(A.not_taken, 2 * sizeof(not_taken));
    // Vendor unique, with its five bytes of data: echoed, not implemented.
    msg_t vu = THEIRS(9, C_CONTROL, PANEL, OP_PASS);
    put(&vu, 0x7e); put(&vu, 5); put(&vu, 0x00); put(&vu, 0x19); put(&vu, 0x58); put(&vu, 0x00); put(&vu, 0x01);
    feed(&vu);
    msg_t vw = OURS(9, R_NOT_IMPL, PANEL, OP_PASS);
    putn(&vw, vu.b + 6, 7);
    expect(vw);
    // A press repeated with no release between is one hold; its release ends it.
    for (int k = 0; k < 6; k++) {
        msg_t p = pass((unsigned)k, 0x44);
        feed(&p);
        expect(pass_answer((unsigned)k, R_ACCEPTED, 0x44));
        T += BT_AVRCP_HOLD_MS - 1; bt_avrcp_tick(&A, T);
    }
    expect_press(0x44, true);
    expect_no_press();
    CHECK_EQ(A.presses[2], 2);
    msg_t rel = pass(7, 0xc4);
    feed(&rel);
    expect(pass_answer(7, R_ACCEPTED, 0xc4));
    expect_press(0x44, false);
    // A release of an operation not held: answered, counted, no event.
    feed(&rel);
    expect(pass_answer(7, R_ACCEPTED, 0xc4));
    msg_t rel2 = pass(8, 0xc6);
    feed(&rel2);
    expect(pass_answer(8, R_ACCEPTED, 0xc6));
    expect_no_press();
    CHECK_EQ(A.unmatched_releases, 2);
    // A press never released is released by us two seconds after its last
    // repeat, and once only.
    msg_t p = pass(1, 0x4b);
    feed(&p); take((uint8_t[64]){ 0 });
    T += BT_AVRCP_HOLD_MS - 1; bt_avrcp_tick(&A, T);
    expect_press(0x4b, true);
    expect_no_press();
    T += 1; bt_avrcp_tick(&A, T);
    expect_press(0x4b, false);
    T += 10000; bt_avrcp_tick(&A, T);
    expect_no_press();
    CHECK_EQ(A.auto_released, 1);
    // Its real release, late, has nothing to end.
    p = pass(2, 0xcb);
    feed(&p); take((uint8_t[64]){ 0 });
    expect_no_press();
    // Another operation pressed while one is held releases the first.
    p = pass(3, 0x41); feed(&p); take((uint8_t[64]){ 0 });
    p = pass(4, 0x42); feed(&p); take((uint8_t[64]){ 0 });
    expect_press(0x41, true);
    expect_press(0x41, false);
    expect_press(0x42, true);
    expect_no_press();
    p = pass(5, 0xc1); feed(&p); take((uint8_t[64]){ 0 });   // the first's release, late
    expect_no_press();
    p = pass(6, 0xc2); feed(&p); take((uint8_t[64]){ 0 });
    expect_press(0x42, false);
    // Presses work in every controller state.
    fresh(); bt_avrcp_channel_open(&A, T);
    p = pass(0, 0x46); feed(&p);
    expect(pass_answer(0, R_ACCEPTED, 0x46));
    ASKED();
    expect_press(0x46, true);
}

// Their pass-through, fed, and our answer sent.
static void press_in(unsigned label, uint8_t operation)
{
    msg_t p = pass(label, operation);
    feed(&p);
    drain();
}

static void test_press_ring(void)
{
    // The host reads nothing: presses reach the ring while there is room
    // for each and its release; the rest are counted lost, and a lost
    // press's release is not delivered. Every press read is followed by its release.
    to_ready(20);
    for (int k = 0; k < 12; k++) {
        msg_t p = pass(1, TAKEN[k % 7]);
        feed(&p);
        p = pass(2, (uint8_t)(0x80 | TAKEN[k % 7]));
        feed(&p);
        drain();
    }
    CHECK_EQ(A.ring_count, BT_AVRCP_PRESSES);
    CHECK_EQ(A.presses_lost, 12 - BT_AVRCP_PRESSES / 2);
    for (int k = 0; k < BT_AVRCP_PRESSES / 2; k++) {
        expect_press(TAKEN[k % 7], true);
        expect_press(TAKEN[k % 7], false);
    }
    expect_no_press();
    // With one slot left a press is lost; with two it is kept, and its
    // release always has room.
    for (int k = 0; k < 7; k++) { press_in(1, 0x44); press_in(2, 0xc4); }
    press_in(1, 0x45);                          // 14 + 1: one press, room for its release
    CHECK_EQ(A.ring_count, 15);
    press_in(1, 0x46);                          // releases 0x45 (16), and 0x46 finds no room
    CHECK_EQ(A.ring_count, 16);
    CHECK_EQ(A.presses_lost, 12 - BT_AVRCP_PRESSES / 2 + 1);
    press_in(2, 0xc6);
    CHECK_EQ(A.ring_count, 16);
    for (int k = 0; k < 7; k++) { expect_press(0x44, true); expect_press(0x44, false); }
    expect_press(0x45, true);
    expect_press(0x45, false);
    expect_no_press();
    // After reading, there is room again.
    press_in(1, 0x46);
    expect_press(0x46, true);
    // The link dropping keeps what the host has not read, and releases the
    // press the host has read.
    bt_avrcp_reset(&A);
    CHECK_EQ(bt_avrcp_state(&A), BT_AVRCP_IDLE);
    expect_press(0x46, false);
    expect_no_press();
    to_ready(20);
    press_in(1, 0x44);
    press_in(2, 0x45);
    bt_avrcp_reset(&A);
    expect_press(0x44, true);
    expect_press(0x44, false);
    expect_press(0x45, true);
    expect_press(0x45, false);
    expect_no_press();
    CHECK_EQ(A.presses[2], 0);                  // all else wiped
    expect_nothing();
    // A press lost for want of room for its release (one slot left) is not
    // released into that slot, by its own release or by a reset.
    to_ready(20);
    for (int k = 0; k < 8; k++) { press_in(1, 0x44); press_in(2, 0xc4); }
    expect_press(0x44, true);
    CHECK_EQ(A.ring_count, BT_AVRCP_PRESSES - 1);
    press_in(1, 0x45);
    CHECK_EQ(A.presses_lost, 1);
    press_in(2, 0xc5);
    CHECK_EQ(A.ring_count, BT_AVRCP_PRESSES - 1);
    press_in(1, 0x45);
    bt_avrcp_reset(&A);
    CHECK_EQ(A.ring_count, BT_AVRCP_PRESSES - 1);
}

static void test_answers(void)
{
    to_ready(20);
    // Unit Info and Subunit Info: the panel.
    msg_t u = THEIRS(1, C_STATUS, UNIT, OP_UNIT);
    for (int i = 0; i < 5; i++) put(&u, 0xff);
    feed(&u);
    msg_t w = OURS(1, R_STABLE, UNIT, OP_UNIT);
    put(&w, 0x07); put(&w, PANEL); put(&w, 0x00); put(&w, 0x19); put(&w, 0x58);
    expect(w);
    u = THEIRS(2, C_STATUS, UNIT, OP_SUBUNIT);
    put(&u, 0x07); for (int i = 0; i < 4; i++) put(&u, 0xff);
    feed(&u);
    w = OURS(2, R_STABLE, UNIT, OP_SUBUNIT);
    put(&w, 0x07); put(&w, PANEL); put(&w, 0xff); put(&w, 0xff); put(&w, 0xff);
    expect(w);
    u = THEIRS(3, C_STATUS, UNIT, OP_SUBUNIT);  // its page byte kept; none given is page 0, extension 7
    put(&u, 0x17);
    feed(&u);
    w = OURS(3, R_STABLE, UNIT, OP_SUBUNIT);
    put(&w, 0x17); put(&w, PANEL); put(&w, 0xff); put(&w, 0xff); put(&w, 0xff);
    expect(w);
    u = THEIRS(4, C_STATUS, UNIT, OP_SUBUNIT);
    feed(&u);
    w = OURS(4, R_STABLE, UNIT, OP_SUBUNIT);
    put(&w, 0x07); put(&w, PANEL); put(&w, 0xff); put(&w, 0xff); put(&w, 0xff);
    expect(w);
    // As anything but a status command: REJECTED, echoed.
    u = THEIRS(5, C_CONTROL, UNIT, OP_UNIT);
    for (int i = 0; i < 5; i++) put(&u, 0xff);
    feed(&u);
    w = OURS(5, R_REJECTED, UNIT, OP_UNIT);
    for (int i = 0; i < 5; i++) put(&w, 0xff);
    expect(w);
    u = THEIRS(6, C_NOTIFY, UNIT, OP_SUBUNIT);
    feed(&u);
    expect(OURS(6, R_REJECTED, UNIT, OP_SUBUNIT));

    // Get Capabilities: the company, or the two events.
    msg_t c = vcmd(7, C_STATUS, GET_CAPS, P(0x02));
    feed(&c);
    expect(vanswer(7, R_STABLE, GET_CAPS, P(0x02, 1, 0x00, 0x19, 0x58)));
    c = vcmd(8, C_STATUS, GET_CAPS, P(0x03));
    feed(&c);
    expect(vanswer(8, R_STABLE, GET_CAPS, P(0x03, 2, EV_PLAYBACK, EV_TRACK)));
    // Get Play Status: the status, length and position unknown.
    c = vcmd(9, C_STATUS, GET_PLAY, NULL, 0);
    feed(&c);
    expect(vanswer(9, R_STABLE, GET_PLAY, P(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, BT_AVRCP_STOPPED)));
    bt_avrcp_playback(&A, BT_AVRCP_PAUSED, T);
    feed(&c);
    expect(vanswer(9, R_STABLE, GET_PLAY, P(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, BT_AVRCP_PAUSED)));
    // Get Element Attributes: none, asked for all or for some.
    c = vcmd(10, C_STATUS, GET_ELEMENTS, P(0, 0, 0, 0, 0, 0, 0, 0, 0));
    feed(&c);
    expect(vanswer(10, R_STABLE, GET_ELEMENTS, P(0)));
    c = vcmd(11, C_STATUS, GET_ELEMENTS, P(0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 7));
    feed(&c);
    expect(vanswer(11, R_STABLE, GET_ELEMENTS, P(0)));
    CHECK_EQ(A.answered, 10);

    // Every reject: invalid command for a PDU we do not take, the wrong
    // command type, or AVRCP's own fragments; invalid parameter for a
    // parameter we do not offer or a length that is not the PDU's.
    struct { uint8_t ctype, pdu; uint8_t p[24]; unsigned n; uint8_t status; } bad[] = {
        { C_STATUS, LIST_ATTRS, { 0 }, 0, ST_INVALID_COMMAND },
        { C_CONTROL, REQ_CONTINUE, { GET_ELEMENTS }, 1, ST_INVALID_COMMAND },
        { C_CONTROL, SET_VOLUME, { 20 }, 1, ST_INVALID_COMMAND },        // we are no volume target
        { C_STATUS, 0x60, { 0, 1 }, 2, ST_INVALID_COMMAND },
        { C_STATUS, 0xff, { 0 }, 0, ST_INVALID_COMMAND },
        { C_CONTROL, GET_CAPS, { 0x03 }, 1, ST_INVALID_COMMAND },
        { C_NOTIFY, GET_CAPS, { 0x03 }, 1, ST_INVALID_COMMAND },
        { C_STATUS, REGISTER, { EV_PLAYBACK, 0, 0, 0, 0 }, 5, ST_INVALID_COMMAND },
        { C_CONTROL, REGISTER, { EV_TRACK, 0, 0, 0, 0 }, 5, ST_INVALID_COMMAND },
        { C_NOTIFY, GET_PLAY, { 0 }, 0, ST_INVALID_COMMAND },
        { C_CONTROL, GET_ELEMENTS, { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 9, ST_INVALID_COMMAND },
        { C_STATUS, GET_CAPS, { 0x01 }, 1, ST_INVALID_PARAMETER },
        { C_STATUS, GET_CAPS, { 0x04 }, 1, ST_INVALID_PARAMETER },
        { C_STATUS, GET_CAPS, { 0 }, 0, ST_INVALID_PARAMETER },
        { C_STATUS, GET_CAPS, { 0x03, 0x00 }, 2, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { EV_VOLUME, 0, 0, 0, 0 }, 5, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { EV_POSITION, 0, 0, 0, 1 }, 5, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { 0x03, 0, 0, 0, 0 }, 5, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { 0x00, 0, 0, 0, 0 }, 5, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { EV_PLAYBACK, 0, 0, 0 }, 4, ST_INVALID_PARAMETER },
        { C_NOTIFY, REGISTER, { EV_TRACK, 0, 0, 0, 0, 0 }, 6, ST_INVALID_PARAMETER },
        { C_STATUS, GET_PLAY, { 0 }, 1, ST_INVALID_PARAMETER },
        { C_STATUS, GET_ELEMENTS, { 0, 0, 0, 0, 0, 0, 0, 1, 0 }, 9, ST_INVALID_PARAMETER },     // a track by UID
        { C_STATUS, GET_ELEMENTS, { 1, 0, 0, 0, 0, 0, 0, 0, 0 }, 9, ST_INVALID_PARAMETER },
        { C_STATUS, GET_ELEMENTS, { 0, 0, 0, 0, 0, 0, 0, 0 }, 8, ST_INVALID_PARAMETER },
        { C_STATUS, GET_ELEMENTS, { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 }, 12, ST_INVALID_PARAMETER },
        { C_STATUS, GET_ELEMENTS, { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0 }, 14, ST_INVALID_PARAMETER },
    };
    unsigned rejected = A.rejected;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        c = vcmd(12, bad[i].ctype, bad[i].pdu, bad[i].p, bad[i].n);
        feed(&c);
        msg_t want = vreject(12, bad[i].pdu, bad[i].status);
        uint8_t out[64];
        size_t n = take(out);
        if (n != want.n || memcmp(out, want.b, n)) { failures++; printf("FAIL reject %zu\n", i); dump("want", want.b, want.n); dump("got ", out, n); }
    }
    CHECK_EQ(A.rejected, rejected + sizeof(bad) / sizeof(bad[0]));
    // A parameter length that is not the bytes that follow, both ways.
    c = vcmd(13, C_STATUS, GET_CAPS, P(0x03));
    c.b[12]++;
    feed(&c);
    expect(vreject(13, GET_CAPS, ST_INVALID_PARAMETER));
    c.b[12] -= 2;
    feed(&c);
    expect(vreject(13, GET_CAPS, ST_INVALID_PARAMETER));
    // AVRCP's own fragments in a command: invalid command.
    for (uint8_t t = 1; t < 4; t++) {
        c = vcmd(14, C_STATUS, GET_CAPS, P(0x03));
        c.b[10] = t;
        feed(&c);
        expect(vreject(14, GET_CAPS, ST_INVALID_COMMAND));
    }
    // A vendor header cut short: the PDU it carries, else 0; invalid command.
    for (unsigned k = 4; k < 7; k++) {
        c = THEIRS(15, C_STATUS, PANEL, OP_VENDOR);
        const uint8_t head[] = { 0x00, 0x19, 0x58, GET_CAPS, 0x00, 0x00 };
        putn(&c, head, k);
        feed(&c);
        expect(vreject(15, GET_CAPS, ST_INVALID_COMMAND));
    }
    // Another company, or none: NOT IMPLEMENTED, echoed.
    c = vcmd(1, C_STATUS, GET_CAPS, P(0x03));
    c.b[6] = 0x01;
    feed(&c);
    msg_t e = c; e.b[0] |= 2; e.b[3] = R_NOT_IMPL;
    expect(e);
    for (unsigned k = 0; k < 3; k++) {
        c = THEIRS(2, C_STATUS, PANEL, OP_VENDOR);
        putn(&c, (const uint8_t[]){ 0x00, 0x19, 0x58 }, k);
        feed(&c);
        e = c; e.b[0] |= 2; e.b[3] = R_NOT_IMPL;
        expect(e);
    }
    // Pass through not as a control command to the panel, or too short: REJECTED.
    msg_t p = THEIRS(3, C_STATUS, PANEL, OP_PASS); put(&p, 0x44); put(&p, 0);
    feed(&p);
    expect(pass_answer(3, R_REJECTED, 0x44));
    p = THEIRS(3, C_CONTROL, 0x08 << 3, OP_PASS); put(&p, 0x44); put(&p, 0);
    feed(&p);
    msg_t pw = OURS(3, R_REJECTED, 0x08 << 3, OP_PASS); put(&pw, 0x44); put(&pw, 0);
    expect(pw);
    p = THEIRS(3, C_CONTROL, PANEL, OP_PASS); put(&p, 0x44);
    feed(&p);
    pw = OURS(3, R_REJECTED, PANEL, OP_PASS); put(&pw, 0x44);
    expect(pw);
    p = THEIRS(3, C_CONTROL, PANEL, OP_PASS);
    feed(&p);
    expect(OURS(3, R_REJECTED, PANEL, OP_PASS));
    expect_no_press();
    // A length byte that disagrees with the data does not stop a press.
    p = THEIRS(4, C_CONTROL, PANEL, OP_PASS); put(&p, 0x44); put(&p, 3);
    feed(&p);
    pw = OURS(4, R_ACCEPTED, PANEL, OP_PASS); put(&pw, 0x44); put(&pw, 3);
    expect(pw);
    expect_press(0x44, true);
    // An opcode we do not know: NOT IMPLEMENTED, echoed; a long one, its
    // operands echoed as far as the buffer allows.
    msg_t o = THEIRS(5, C_STATUS, PANEL, 0x40); put(&o, 1); put(&o, 2);
    feed(&o);
    msg_t ow = OURS(5, R_NOT_IMPL, PANEL, 0x40); put(&ow, 1); put(&ow, 2);
    expect(ow);
    o = THEIRS(6, C_CONTROL, PANEL, 0x55);
    for (int i = 0; i < 200; i++) put(&o, (uint8_t)i);
    feed(&o);
    ow = OURS(6, R_NOT_IMPL, PANEL, 0x55);
    for (int i = 0; i < BT_AVRCP_ECHO; i++) put(&ow, (uint8_t)i);
    expect(ow);
    // An AV/C header cut short, and an IPID on a command: no answer.
    feed_bytes((const uint8_t[]){ 0x70, 0x11, 0x0e, C_STATUS, PANEL }, 5);
    feed_bytes((const uint8_t[]){ 0x71, 0x11, 0x0e, C_STATUS, UNIT, OP_UNIT, 0xff, 0xff, 0xff, 0xff, 0xff }, 11);
    feed_bytes((const uint8_t[]){ 0x70, 0x11 }, 2);
    feed_bytes(NULL, 0);
    expect_nothing();
    CHECK_EQ(A.malformed, 4);
}

static void test_notifications(void)
{
    to_ready(20);
    // Playback status registered: INTERIM with the status now, then CHANGED
    // once when it changes; registered again, INTERIM with the new status.
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
    msg_t r = vcmd(5, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    feed(&r);
    expect(vanswer(5, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PLAYING)));
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);            // the same: nothing
    expect_nothing();
    bt_avrcp_playback(&A, BT_AVRCP_PAUSED, T);
    expect(vanswer(5, R_CHANGED, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PAUSED)));
    bt_avrcp_playback(&A, BT_AVRCP_STOPPED, T);            // spent: nothing
    expect_nothing();
    bt_avrcp_playback(&A, 7, T);                           // not a status: ignored
    bt_avrcp_playback(&A, 0xff, T);
    expect_nothing();
    r = vcmd(9, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    feed(&r);
    expect(vanswer(9, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_STOPPED)));
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
    expect(vanswer(9, R_CHANGED, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PLAYING)));
    // Registered again while one stands: the newest label is the one spent.
    r = vcmd(1, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    feed(&r);
    r = vcmd(2, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    feed(&r);
    expect(vanswer(1, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PLAYING)));
    expect(vanswer(2, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PLAYING)));
    bt_avrcp_playback(&A, BT_AVRCP_STOPPED, T);
    expect(vanswer(2, R_CHANGED, REGISTER, P(EV_PLAYBACK, BT_AVRCP_STOPPED)));
    expect_nothing();
    // A change before the INTERIM has left: the INTERIM keeps the status it
    // was built with, the CHANGED follows it.
    r = vcmd(3, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    feed(&r);
    bt_avrcp_playback(&A, BT_AVRCP_PAUSED, T);
    expect(vanswer(3, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_STOPPED)));
    expect(vanswer(3, R_CHANGED, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PAUSED)));
    // The subunit the registration named is the CHANGED's.
    r = THEIRS(4, C_NOTIFY, 0x49, OP_VENDOR); vendor(&r, REGISTER, 5);
    putn(&r, (const uint8_t[]){ EV_PLAYBACK, 0, 0, 0, 0 }, 5);
    feed(&r);
    bt_avrcp_playback(&A, BT_AVRCP_STOPPED, T);
    msg_t w = OURS(4, R_INTERIM, 0x49, OP_VENDOR); vendor(&w, REGISTER, 2); put(&w, EV_PLAYBACK); put(&w, BT_AVRCP_PAUSED);
    expect(w);
    w = OURS(4, R_CHANGED, 0x49, OP_VENDOR); vendor(&w, REGISTER, 2); put(&w, EV_PLAYBACK); put(&w, BT_AVRCP_STOPPED);
    expect(w);
    // Track changed: INTERIM with "no track selected", never CHANGED.
    r = vcmd(6, C_NOTIFY, REGISTER, P(EV_TRACK, 0, 0, 0, 0));
    feed(&r);
    expect(vanswer(6, R_INTERIM, REGISTER, P(EV_TRACK, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff)));
    CHECK(A.track_registered);
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
    bt_avrcp_playback(&A, BT_AVRCP_STOPPED, T);
    T += 60000; bt_avrcp_tick(&A, T);
    expect_nothing();

    // Several commands of theirs back to back, with none lost and the
    // answers in order: a registration for playback status, one for track
    // changed, a pass-through, Get Capabilities.
    to_ready(20);
    msg_t m1 = vcmd(1, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0));
    msg_t m2 = vcmd(2, C_NOTIFY, REGISTER, P(EV_TRACK, 0, 0, 0, 0));
    msg_t m3 = pass(3, 0x44);
    msg_t m4 = vcmd(4, C_STATUS, GET_CAPS, P(0x03));
    feed(&m1); feed(&m2); feed(&m3); feed(&m4);
    // Our own command waits behind the answers.
    CHECK(bt_avrcp_set_volume(&A, 9, T));
    expect(vanswer(1, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_STOPPED)));
    expect(vanswer(2, R_INTERIM, REGISTER, P(EV_TRACK, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff)));
    expect(pass_answer(3, R_ACCEPTED, 0x44));
    expect(vanswer(4, R_STABLE, GET_CAPS, P(0x03, 2, EV_PLAYBACK, EV_TRACK)));
    SET(9);
    expect_nothing();
    expect_press(0x44, true);

    // The bound: with a playback registration standing, their commands may
    // fill all but one slot; the next is dropped unanswered, and nothing it
    // asked is done; the slot kept carries the CHANGED.
    to_ready(20);
    feed(&m1);
    for (int k = 0; k < BT_AVRCP_ANSWERS; k++) { msg_t p = pass((unsigned)k, 0x41 + (k & 1)); feed(&p); }
    CHECK_EQ(A.answers, BT_AVRCP_ANSWERS - 1);
    CHECK_EQ(A.overrun, 2);
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
    CHECK_EQ(A.answers, BT_AVRCP_ANSWERS);
    expect(vanswer(1, R_INTERIM, REGISTER, P(EV_PLAYBACK, BT_AVRCP_STOPPED)));
    for (int k = 0; k < BT_AVRCP_ANSWERS - 2; k++) expect(pass_answer((unsigned)k, R_ACCEPTED, (uint8_t)(0x41 + (k & 1))));
    expect(vanswer(1, R_CHANGED, REGISTER, P(EV_PLAYBACK, BT_AVRCP_PLAYING)));
    expect_nothing();
    CHECK_EQ(A.presses[0] + A.presses[1], BT_AVRCP_ANSWERS - 2);
    // With none standing, all eight are theirs; a registration then needs
    // two (its INTERIM and its CHANGED's).
    to_ready(20);
    for (int k = 0; k < BT_AVRCP_ANSWERS - 1; k++) { msg_t p = pass((unsigned)k, 0x41); feed(&p); }
    feed(&m1);
    CHECK_EQ(A.overrun, 1);
    CHECK(!A.play_registered);
    msg_t p = pass(9, 0x41); feed(&p);
    CHECK_EQ(A.answers, BT_AVRCP_ANSWERS);
    feed(&p);
    CHECK_EQ(A.overrun, 2);
    // A parked answer survives what comes after it: asked for, not sent,
    // more commands, the same head.
    to_ready(20);
    feed(&m4);
    uint8_t out[64], out2[64];
    uint32_t s1, s2;
    size_t n1 = bt_avrcp_next(&A, out, sizeof(out), 48, T, &s1);
    feed(&m3);
    bt_avrcp_playback(&A, BT_AVRCP_PLAYING, T);
    CHECK(bt_avrcp_set_volume(&A, 3, T));
    size_t n2 = bt_avrcp_next(&A, out2, sizeof(out2), 48, T, &s2);
    CHECK(n1 == n2 && s1 == s2 && !memcmp(out, out2, n1));
    CHECK(bt_avrcp_sent(&A, s1, T));
    expect(pass_answer(3, R_ACCEPTED, 0x44));
    SET(3);
}

// ── AVCTP: profiles, fragments, labels ──────────────────────────────────────

// A message cut into AVCTP fragments at `cuts` (the frame's offsets, the
// last its length): a start (label, type, C/R and IPID of the single
// header, the number of packets, the profile), then continues and an end.
static void feed_fragments(const msg_t *m, const size_t *cuts, int parts)
{
    const uint8_t *f = m->b + 3;
    size_t from = 0;
    for (int i = 0; i < parts; i++) {
        uint8_t pkt[700];
        size_t k = 0;
        uint8_t type = i == 0 ? P_START : i == parts - 1 ? P_END : P_CONT;
        pkt[k++] = (uint8_t)((m->b[0] & 0xf3) | type << 2);
        if (i == 0) { pkt[k++] = (uint8_t)parts; pkt[k++] = m->b[1]; pkt[k++] = m->b[2]; }
        memcpy(pkt + k, f + from, cuts[i] - from); k += cuts[i] - from;
        from = cuts[i];
        feed_bytes(pkt, k);
    }
}

static void test_avctp(void)
{
    to_ready(20);
    // A command for another profile: AVCTP's header alone, IPID set, their
    // profile, their label; nothing else is done.
    msg_t m = avctp(6, false, false, 0x110C);
    put(&m, C_CONTROL); put(&m, PANEL); put(&m, OP_PASS); put(&m, 0x44); put(&m, 0);
    feed(&m);
    expect(avctp(6, true, true, 0x110C));
    m = avctp(7, false, false, 0x0000);
    feed(&m);
    expect(avctp(7, true, true, 0x0000));
    expect_no_press();
    CHECK_EQ(A.invalid_profile, 2);
    // A response for another profile that does not say so is a stray.
    CHECK(bt_avrcp_set_volume(&A, 8, T));
    unsigned s = SET(8);
    msg_t a = set_reply(s, R_ACCEPTED, 8);
    a.b[1] = 0x11; a.b[2] = 0x0c;
    feed(&a);
    CHECK_EQ(A.strays, 1);
    expect_volume(20);

    // Their command in two fragments, cut at every byte; in three, at every
    // pair: answered as if whole.
    msg_t c = vcmd(9, C_STATUS, GET_ELEMENTS, P(0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1));
    size_t fl = c.n - 3;
    msg_t want = vanswer(9, R_STABLE, GET_ELEMENTS, P(0));
    for (size_t x = 0; x <= fl; x++) {
        size_t cuts[2] = { x, fl };
        feed_fragments(&c, cuts, 2);
        uint8_t out[64];
        size_t n = take(out);
        if (n != want.n || memcmp(out, want.b, n)) { failures++; printf("FAIL two fragments cut at %zu\n", x); }
    }
    for (size_t x = 0; x <= fl; x++) for (size_t y = x; y <= fl; y++) {
        size_t cuts[3] = { x, y, fl };
        feed_fragments(&c, cuts, 3);
        uint8_t out[64];
        size_t n = take(out);
        if (n != want.n || memcmp(out, want.b, n)) { failures++; printf("FAIL three fragments cut at %zu %zu\n", x, y); }
    }
    CHECK_EQ(A.fragments_dropped, 0);
    // Their answer to ours, fragmented: taken.
    a = set_reply(s, R_ACCEPTED, 8);
    a.b[1] = 0x11; a.b[2] = 0x0e;
    size_t acuts[3] = { 2, 5, a.n - 3 };
    feed_fragments(&a, acuts, 3);
    expect_volume(8);
    // A message as long as allowed (the frame 512 bytes): reassembled and
    // answered; one byte longer, dropped and counted.
    msg_t big = THEIRS(10, C_CONTROL, PANEL, 0x60);
    while (big.n < 3 + BT_AVRCP_MESSAGE) put(&big, 0xaa);
    size_t bcuts[12];
    for (int i = 0; i < 11; i++) bcuts[i] = (size_t)(i + 1) * 46;
    bcuts[11] = BT_AVRCP_MESSAGE;
    feed_fragments(&big, bcuts, 12);
    uint8_t out[64];
    CHECK_EQ(take(out), BT_AVRCP_LONGEST);
    CHECK(out[3] == R_NOT_IMPL && out[5] == 0x60);
    put(&big, 0xaa);
    bcuts[11] = BT_AVRCP_MESSAGE + 1;
    feed_fragments(&big, bcuts, 12);
    expect_nothing();
    CHECK_EQ(A.too_long, 1);
    // ... and a start alone too long for the buffer.
    uint8_t *huge = malloc(4 + BT_AVRCP_MESSAGE + 1);
    memset(huge, 0, 4 + BT_AVRCP_MESSAGE + 1);
    huge[0] = 0x14; huge[1] = 2; huge[2] = 0x11; huge[3] = 0x0e;
    feed_bytes(huge, 4 + BT_AVRCP_MESSAGE + 1);
    feed_bytes((const uint8_t[]){ 0x1c, 0 }, 2);
    CHECK_EQ(A.too_long, 2);
    CHECK_EQ(A.fragments_dropped, 1);
    free(huge);
    expect_nothing();
    // Too many packets: a continue where the end was due, an end before it
    // was due, a start that counts fewer than two.
    unsigned dropped = A.fragments_dropped;
    msg_t p = pass(11, 0x44);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xb8, OP_PASS }, 2);
    feed_bytes((const uint8_t[]){ 0xbc, 0x44, 0 }, 3);
    feed_bytes((const uint8_t[]){ 0xb4, 3, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xbc, OP_PASS, 0x44, 0 }, 4);
    feed_bytes((const uint8_t[]){ 0xb4, 1, 0x11, 0x0e, C_CONTROL, PANEL, OP_PASS, 0x44, 0 }, 9);
    feed_bytes((const uint8_t[]){ 0xb8, 0x00 }, 2);
    feed_bytes((const uint8_t[]){ 0xb4, 0, 0x11, 0x0e, C_CONTROL, PANEL, OP_PASS, 0x44, 0 }, 9);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11 }, 3);
    expect_nothing();
    CHECK_EQ(A.fragments_dropped, dropped + 7);
    // A continue or end with no start, or another label, C/R or IPID than
    // the start's: dropped.
    feed_bytes((const uint8_t[]){ 0xbc, OP_PASS, 0x44, 0 }, 4);
    feed_bytes((const uint8_t[]){ 0xb8, OP_PASS }, 2);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xcc, OP_PASS, 0x44, 0 }, 4);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xbe, OP_PASS, 0x44, 0 }, 4);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xbd, OP_PASS, 0x44, 0 }, 4);
    expect_nothing();
    CHECK_EQ(A.fragments_dropped, dropped + 12);
    // A new message ends one being gathered: a single, then a start.
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed(&p);
    expect(pass_answer(11, R_ACCEPTED, 0x44));
    feed_bytes((const uint8_t[]){ 0xbc, OP_PASS, 0x44, 0 }, 4);
    feed_bytes((const uint8_t[]){ 0xb4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xa4, 2, 0x11, 0x0e, C_CONTROL, PANEL }, 6);
    feed_bytes((const uint8_t[]){ 0xac, OP_PASS, 0xc4, 0 }, 4);
    expect(pass_answer(10, R_ACCEPTED, 0xc4));
    CHECK_EQ(A.fragments_dropped, dropped + 15);
    expect_press(0x44, true);
    expect_press(0x44, false);
    // Fragments of another profile's command: answered with IPID.
    feed_bytes((const uint8_t[]){ 0x34, 2, 0x11, 0x0d, C_CONTROL }, 5);
    feed_bytes((const uint8_t[]){ 0x3c, PANEL, OP_PASS }, 3);
    expect(avctp(3, true, true, 0x110D));

    // Labels: a response before our command is sent is a stray (and the
    // command still leaves); one with the right label and another PDU is a
    // stray, the command still waiting; one with the right label and
    // another opcode is a stray.
    to_ready(20);
    CHECK(bt_avrcp_set_volume(&A, 44, T));
    uint32_t serial;
    bt_avrcp_next(&A, out, sizeof(out), 48, T, &serial);
    unsigned label = out[0] >> 4;
    a = set_reply(label, R_ACCEPTED, 44);
    feed(&a);
    CHECK_EQ(A.strays, 1);
    CHECK(bt_avrcp_sent(&A, serial, T));
    msg_t wrong = REPLY(label, R_ACCEPTED);
    vendor(&wrong, REGISTER, 1); put(&wrong, 44);
    feed(&wrong);
    wrong = REPLY(label, R_NOT_IMPL);
    vendor(&wrong, GET_CAPS, 1); put(&wrong, 3);
    feed(&wrong);
    wrong = frame(label, true, R_ACCEPTED, PANEL, OP_PASS); put(&wrong, 44); put(&wrong, 0);
    feed(&wrong);
    CHECK_EQ(A.strays, 4);
    expect_volume(20);
    feed(&a);
    expect_volume(44);
    // A duplicate: a stray.
    feed(&a);
    CHECK_EQ(A.strays, 5);
    // A label nobody holds.
    a = set_reply((label + 5) & 15, R_ACCEPTED, 1);
    feed(&a);
    CHECK_EQ(A.strays, 6);
    expect_volume(44);
}

static void test_status(void)
{
    char line[1024];
    to_ready(64);
    msg_t p = pass(1, 0x44); feed(&p);
    size_t n = bt_avrcp_status(&A, line, sizeof(line));
    CHECK(n == strlen(line));
    CHECK(strstr(line, "avrcp: ready, their volume 64/127 (told 1 times)\n") != NULL);
    CHECK(strstr(line, "play 1") != NULL);
    CHECK(strstr(line, "held play") != NULL);
    CHECK(strstr(line, "playback stopped") != NULL);
    CHECK(strstr(line, "strays 0, malformed 0") != NULL);
    char small[16];
    CHECK_EQ(bt_avrcp_status(&A, small, sizeof(small)), sizeof(small) - 1);
    CHECK_EQ(strlen(small), sizeof(small) - 1);
    CHECK_EQ(bt_avrcp_status(&A, small, 0), 0);
    for (size_t cap = 1; cap < 600; cap += 7) {
        char *b = malloc(cap);
        size_t k = bt_avrcp_status(&A, b, cap);
        if (k >= cap || k != strlen(b)) { failures++; printf("FAIL status at capacity %zu\n", cap); }
        free(b);
    }
    fresh();
    bt_avrcp_status(&A, line, sizeof(line));
    CHECK(strstr(line, "avrcp: idle, their volume not known\n") != NULL);
    CHECK(strstr(line, "held nothing") != NULL);
    static const char *names[] = { "idle", "asking", "registering", "ready", "plain", "failed" };
    for (int s = 0; s < 6; s++) CHECK(!strcmp(bt_avrcp_state_name((bt_avrcp_state_t)s), names[s]));
    CHECK(!strcmp(bt_avrcp_state_name((bt_avrcp_state_t)6), "?"));
}

// ── Fuzz ────────────────────────────────────────────────────────────────────

static uint32_t seed = 9137;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

static void invariants(const char *where)
{
    if (A.answers > BT_AVRCP_ANSWERS || A.ring_count > BT_AVRCP_PRESSES || A.rx_have > BT_AVRCP_MESSAGE) {
        failures++; printf("FAIL %s: a bound passed\n", where);
    }
    if (A.reg.live && A.set.live && A.reg.label == A.set.label) { failures++; printf("FAIL %s: two commands under one label\n", where); }
    if ((A.state == BT_AVRCP_IDLE || A.state == BT_AVRCP_PLAIN || A.state == BT_AVRCP_FAILED) && (A.reg.live || A.set.live)) {
        failures++; printf("FAIL %s: a command of ours in %s\n", where, bt_avrcp_state_name(A.state));
    }
    char line[1024];
    if (bt_avrcp_status(&A, line, sizeof(line)) != strlen(line)) { failures++; printf("FAIL %s: status length\n", where); }
}

// Random bytes in random states: shaped messages (from the templates
// below, spoiled) and noise, with ticks, sends, Sets and status changes.
static void fuzz_random(int rounds)
{
    for (int round = 0; round < rounds; round++) {
        switch (rnd() % 4) {
        case 0: fresh(); break;
        case 1: fresh(); bt_avrcp_channel_open(&A, T); break;
        case 2: to_ready((uint8_t)rnd()); break;
        default: fresh(); bt_avrcp_channel_open(&A, T); take((uint8_t[64]){ 0 }); break;
        }
        for (int step = 0; step < 40; step++) {
            msg_t m;
            switch (rnd() % 7) {
            case 0: m = vcmd(rnd() % 16, (uint8_t)(rnd() % 16), (uint8_t)(rnd() % 3 ? (uint32_t[]){ 0x10, 0x20, 0x30, 0x31, 0x50 }[rnd() % 5] : rnd()),
                             (const uint8_t[]){ (uint8_t)rnd(), 0, 0, 0, 0, 0, 0, 0, (uint8_t)(rnd() % 3) }, rnd() % 10); break;
            case 1: m = pass(rnd() % 16, (uint8_t)rnd()); break;
            case 2: m = volume_reply(rnd() % 16, (uint8_t)(rnd() % 16), (uint8_t)rnd()); break;
            case 3: m = set_reply(rnd() % 16, (uint8_t)(rnd() % 16), (uint8_t)rnd()); break;
            case 4: m = events(rnd() % 16, ALL_EVENTS, rnd() % 4); break;
            default: m.n = rnd() % 60; for (size_t i = 0; i < m.n; i++) m.b[i] = (uint8_t)rnd(); break;
            }
            int flips = (int)(rnd() % 3);
            for (int i = 0; i < flips && m.n; i++) m.b[rnd() % m.n] ^= (uint8_t)(1u << (rnd() % 8));
            if (rnd() % 4 == 0 && m.n) m.n = rnd() % (m.n + 1);
            if (rnd() % 5 == 0 && m.n > 4) {
                size_t cut = 1 + rnd() % (m.n - 4);
                size_t cuts[2] = { cut, m.n - 3 };
                feed_fragments(&m, cuts, 2);
            } else feed(&m);
            switch (rnd() % 8) {
            case 0: T += rnd() % 3000; bt_avrcp_tick(&A, T); break;
            case 1: bt_avrcp_set_volume(&A, (uint8_t)rnd(), T); break;
            case 2: bt_avrcp_playback(&A, (uint8_t)(rnd() % 4), T); break;
            case 3: { bt_avrcp_press_t p; bt_avrcp_press(&A, &p); } break;
            case 4: { uint8_t o[64]; uint32_t s; bt_avrcp_next(&A, o, rnd() % 40, (uint16_t)(rnd() % 60), T, &s); } break;
            case 5: if (rnd() % 10 == 0) bt_avrcp_channel_closed(&A, T); break;
            default: take((uint8_t[64]){ 0 }); break;
            }
            invariants("random");
            if (failures > 20) return;
        }
    }
}

// A responsive random peer: headphones with absolute volume that answer
// our commands (now and then late, or not at all), change their own level,
// press buttons and register for the playback status; we must reach READY,
// deliver every press in pairs, and agree with them about the level once
// they fall quiet.
typedef struct { uint8_t b[64]; size_t n; uint64_t at; } pending_t;
static void fuzz_peer(int rounds)
{
    int ready = 0, agreed = 0, delivered = 0, changed = 0, accepted = 0;
    for (int round = 0; round < rounds; round++) {
        fresh();
        bt_avrcp_channel_open(&A, T);
        uint8_t level = (uint8_t)(rnd() % 128);
        int reg = -1;                                  // their standing volume registration's label
        bool lossy = rnd() % 4 == 0;
        pending_t q[32];
        int qn = 0;
        uint8_t held = 0;                              // what the host has seen pressed
        unsigned mylabel = 0;
        bool reached = false;
        for (int step = 0; step < 600; step++) {      // they act for 250 steps, then fall quiet
            T += rnd() % 50;
            bt_avrcp_tick(&A, T);
            // Ours, to them.
            uint8_t o[64];
            size_t n;
            for (int sends = 0; sends < 4 * BT_AVRCP_ANSWERS && qn < 32 && (n = take(o)); sends++) {
                if (lossy && rnd() % 20 == 0) continue;
                bool response = o[0] & 2;
                unsigned l = o[0] >> 4;
                if (response) {
                    if (n < 6 || (o[3] != R_ACCEPTED && o[3] != R_INTERIM && o[3] != R_STABLE && o[3] != R_CHANGED)) {
                        failures++; printf("FAIL peer: our answer is not one they expect\n"); dump("got", o, n);
                    }
                    continue;
                }
                if (n < 13 || o[6] || o[7] != 0x19 || o[8] != 0x58) { failures++; printf("FAIL peer: our command\n"); continue; }
                msg_t r;
                if (o[9] == GET_CAPS) r = events(l, ALL_EVENTS, 3);
                else if (o[9] == REGISTER) { reg = (int)l; r = volume_reply(l, R_INTERIM, level); }
                else {
                    level = (uint8_t)(o[13] & 0x7e);    // they round to even levels
                    r = set_reply(l, R_ACCEPTED, level);
                    if (reg >= 0 && rnd() % 2) {         // some announce a Set's change too
                        msg_t c = volume_reply((unsigned)reg, R_CHANGED, level);
                        q[qn].n = c.n; memcpy(q[qn].b, c.b, c.n); q[qn].at = T + rnd() % 200; qn++;
                        reg = -1;
                    }
                }
                if (qn < 32) { q[qn].n = r.n; memcpy(q[qn].b, r.b, r.n); q[qn].at = T + (rnd() % 8 ? rnd() % 100 : rnd() % 2500); qn++; }
            }
            // Theirs, to us, when due (in order of sending).
            while (qn && q[0].at <= T) {
                feed_bytes(q[0].b, q[0].n);
                memmove(q, q + 1, sizeof(q[0]) * (size_t)--qn);
            }
            if (bt_avrcp_state(&A) == BT_AVRCP_READY) reached = true;
            // They act.
            if (step < 250) switch (rnd() % 12) {
            case 0:                                    // their own button: announced behind what they have already sent
                if (qn >= 32) break;
                level = (uint8_t)(rnd() % 128);
                if (reg >= 0) {
                    msg_t c = volume_reply((unsigned)reg, R_CHANGED, level);
                    q[qn].n = c.n; memcpy(q[qn].b, c.b, c.n); q[qn].at = T; qn++;
                    reg = -1; changed++;
                }
                break;
            case 1: case 2: { msg_t p = pass(mylabel++ & 15, TAKEN[rnd() % 7] | (uint8_t)(rnd() % 2 ? 0x80 : 0)); feed(&p); } break;
            case 3: bt_avrcp_set_volume(&A, (uint8_t)(rnd() % 128), T); break;
            case 4: bt_avrcp_playback(&A, (uint8_t)(rnd() % 3), T); break;
            case 5: { msg_t r = vcmd(mylabel++ & 15, C_NOTIFY, REGISTER, P(EV_PLAYBACK, 0, 0, 0, 0)); feed(&r); } break;
            default: break;
            }
            // The host reads the presses: each release follows its own press.
            bt_avrcp_press_t pr;
            for (int reads = 0; reads < 2 * BT_AVRCP_PRESSES && bt_avrcp_press(&A, &pr); reads++) {
                if (pr.pressed) { if (held) { failures++; printf("FAIL peer: a press over a press\n"); } held = pr.operation; delivered++; }
                else { if (held != pr.operation) { failures++; printf("FAIL peer: a release of %02x after %02x\n", pr.operation, held); } held = 0; }
            }
            invariants("peer");
            if (failures > 20) return;
        }
        if (reached) ready++;
        accepted += (int)A.sets_accepted;
        uint8_t v;
        if (bt_avrcp_volume(&A, &v)) {
            if (v == level) agreed++;
            else if (!lossy) { failures++; printf("FAIL peer round %d: we say %u, they say %u\n", round, v, level); }
        }
    }
    printf("fuzz: peer reached ready in %d of %d, agreed on the level in %d, presses delivered %d, their changes %d, sets accepted %d\n",
           ready, rounds, agreed, delivered, changed, accepted);
    if (rounds >= 100 && (ready < rounds * 9 / 10 || agreed < rounds / 2 || delivered < rounds || changed < rounds / 4 || accepted < rounds)) {
        failures++; printf("FAIL the peer fuzz reached too little\n");
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    test_controller_path();
    test_controller_plain();
    test_controller_sets();
    test_controller_silence();
    test_channel_closing();
    test_presses();
    test_press_ring();
    test_answers();
    test_notifications();
    test_avctp();
    test_status();
    fuzz_random(rounds);
    fuzz_peer(rounds);
    if (failures) { printf("bt_avrcp: %d failure(s)\n", failures); return 1; }
    printf("bt_avrcp: all scripted cases and %d fuzz rounds each passed\n", rounds);
    return 0;
}
