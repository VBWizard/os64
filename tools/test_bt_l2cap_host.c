// test_bt_l2cap_host.c — kernel/src/driver/system/usb/bt_l2cap.c against a
// model peer. The scripted half plays the peer by hand: every PDU we send is
// built here independently, field by field, little-endian, and compared byte
// for byte; then the peer answers as a real stack would. The fuzz half throws
// random ACL packets, random signalling and random API calls at it beside an
// automatic peer that answers what it understands, and checks the rules that
// must hold after every step.
//
//   tools/test_bt_l2cap_host.sh [fuzz rounds, default 3000]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_l2cap.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

#define W(x) (uint8_t)((x) & 0xff), (uint8_t)(((x) >> 8) & 0xff)
#define BYTES(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})
#define NONE NULL, 0

static bt_l2cap_t L;
static uint64_t T;

// ── What we send, captured ──────────────────────────────────────────────────

static uint8_t sent[BT_L2CAP_MTU + 64];
static size_t sent_n;
static int sends;            // PDUs handed over in the current tick

static void on_send(void *context, const uint8_t *pdu, size_t bytes)
{
    (void)context;
    sends++;
    if (bytes > sizeof(sent)) { failures++; printf("FAIL a PDU of %zu bytes\n", bytes); return; }
    memcpy(sent, pdu, bytes);
    sent_n = bytes;
}

// One tick; true when it sent something. More than one PDU is a failure.
static bool tick(bool may)
{
    sends = 0; sent_n = 0;
    bt_l2cap_tick(&L, T, may, on_send, NULL);
    if (sends > 1) { failures++; printf("FAIL %d PDUs in one tick\n", sends); }
    if (!may && sends) { failures++; printf("FAIL a PDU without may_send\n"); }
    // Data leaves only on an open channel, within the MTU the peer states now.
    uint16_t cid = sent_n >= 4 ? (uint16_t)(sent[2] | sent[3] << 8) : 0;
    if (sends == 1 && cid >= 0x40) {
        bool fits = false;
        for (unsigned i = 0; i < BT_L2CAP_CHANNELS; i++)
            if (L.ch[i].dcid == cid && bt_l2cap_is_open(&L, i) && sent_n - 4 <= L.ch[i].peer_mtu) fits = true;
        if (!fits) { failures++; printf("FAIL data on cid %04x, %zu bytes, with no open channel that takes it\n", cid, sent_n - 4); }
    }
    return sends == 1;
}

// ── What we deliver, captured ───────────────────────────────────────────────

static uint8_t got[BT_L2CAP_MTU + 64];
static size_t got_n;
static int delivered;
static unsigned got_channel;
static uint16_t got_psm;

static void on_deliver(void *context, unsigned channel, uint16_t psm, const uint8_t *data, size_t bytes)
{
    (void)context;
    delivered++;
    if (bytes > BT_L2CAP_MTU) { failures++; printf("FAIL delivered %zu bytes\n", bytes); return; }
    memcpy(got, data, bytes); got_n = bytes; got_channel = channel; got_psm = psm;
}

// ── Building frames ─────────────────────────────────────────────────────────

static size_t frame(uint8_t *out, uint16_t cid, const uint8_t *payload, size_t n)
{
    out[0] = (uint8_t)n; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)cid; out[3] = (uint8_t)(cid >> 8);
    if (n) memcpy(out + 4, payload, n);
    return n + 4;
}
static size_t command(uint8_t *out, uint8_t code, uint8_t ident, const uint8_t *d, size_t n)
{
    uint8_t c[2048];
    c[0] = code; c[1] = ident; c[2] = (uint8_t)n; c[3] = (uint8_t)(n >> 8);
    if (n) memcpy(c + 4, d, n);
    return frame(out, 0x0001, c, n + 4);
}

// One ACL packet: handle 0x0abc with the boundary flag given.
static void acl(unsigned boundary, const uint8_t *p, size_t n)
{
    uint8_t a[4096];
    uint16_t hf = (uint16_t)(0x0abc | boundary << 12);
    a[0] = (uint8_t)hf; a[1] = (uint8_t)(hf >> 8); a[2] = (uint8_t)n; a[3] = (uint8_t)(n >> 8);
    if (n) memcpy(a + 4, p, n);
    bt_l2cap_receive(&L, a, n + 4, T, on_deliver, NULL);
}
static void peer_pdu(const uint8_t *pdu, size_t n) { acl(2, pdu, n); }
static void peer_cmd(uint8_t code, uint8_t ident, const uint8_t *d, size_t n)
{
    uint8_t f[2100];
    peer_pdu(f, command(f, code, ident, d, n));
}
static void peer_data(uint16_t cid, const uint8_t *d, size_t n)
{
    uint8_t f[2100];
    peer_pdu(f, frame(f, cid, d, n));
}

static void dump(const char *what, const uint8_t *p, size_t n)
{
    printf("  %s:", what);
    for (size_t i = 0; i < n && i < 80; i++) printf(" %02x", p[i]);
    printf("\n");
}

// The next tick must send exactly this.
static void expect_pdu_(int line, const uint8_t *want, size_t n)
{
    if (!tick(true)) { failures++; printf("FAIL line %d: nothing sent\n", line); dump("want", want, n); return; }
    if (sent_n != n || memcmp(sent, want, n)) {
        failures++; printf("FAIL line %d: wrong PDU\n", line);
        dump("want", want, n); dump("sent", sent, sent_n);
    }
}
static void expect_cmd_(int line, uint8_t code, uint8_t ident, const uint8_t *d, size_t n)
{
    uint8_t f[2100];
    expect_pdu_(line, f, command(f, code, ident, d, n));
}
static void expect_data_(int line, uint16_t cid, const uint8_t *d, size_t n)
{
    uint8_t f[2100];
    expect_pdu_(line, f, frame(f, cid, d, n));
}
#define expect_cmd(code, ident, ...) expect_cmd_(__LINE__, code, ident, __VA_ARGS__)
#define expect_data(cid, ...) expect_data_(__LINE__, cid, __VA_ARGS__)
#define expect_nothing() do { if (tick(true)) { failures++; printf("FAIL line %d: unexpected PDU\n", __LINE__); \
    dump("sent", sent, sent_n); } } while (0)

enum { REJ = 1, CONN_REQ, CONN_RSP, CONF_REQ, CONF_RSP, DISC_REQ, DISC_RSP, ECHO_REQ, ECHO_RSP, INFO_REQ, INFO_RSP };
#define MTU_OPT W(BT_L2CAP_MTU)

static void fresh(void)
{
    bt_l2cap_init(&L);
    T = 1000;
    delivered = 0;
}

// We open SDP and the peer answers with CID 0x0070. Returns the channel;
// our identifiers start at 1, so the Connection Request is 1 and our
// Configuration Request 2.
static int we_connect(void)
{
    int ch = bt_l2cap_connect(&L, BT_L2CAP_PSM_SDP, T);
    CHECK_EQ(ch, 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    expect_cmd(CONF_REQ, 2, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));
    return ch;
}

// The peer opens SDP to us from its CID 0x0055: our CID is 0x0040, our
// Configuration Request is identifier 1.
static void peer_connects(uint16_t psm, uint8_t ident)
{
    peer_cmd(CONN_REQ, ident, BYTES(W(psm), W(0x0055)));
    expect_cmd(CONN_RSP, ident, BYTES(W(0x0040), W(0x0055), W(0), W(0)));
    expect_cmd(CONF_REQ, 1, BYTES(W(0x0055), W(0), 0x01, 2, MTU_OPT));
}

static void open_outgoing(void)
{
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));
    peer_cmd(CONF_REQ, 7, BYTES(W(0x0040), W(0), 0x01, 2, W(672)));
    expect_cmd(CONF_RSP, 7, BYTES(W(0x0070), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
}

static void open_incoming(void)
{
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    peer_connects(BT_L2CAP_PSM_SDP, 9);
    peer_cmd(CONF_RSP, 1, BYTES(W(0x0040), W(0), W(0)));
    peer_cmd(CONF_REQ, 10, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 10, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
}

// ── Scripted ────────────────────────────────────────────────────────────────

static void test_we_open_sdp_ours_first(void)
{
    fresh();
    int ch = we_connect();
    CHECK(!bt_l2cap_is_open(&L, ch));
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));
    CHECK(!bt_l2cap_is_open(&L, ch));
    CHECK(L.ch[0].ours_done && !L.ch[0].peer_done);
    peer_cmd(CONF_REQ, 7, BYTES(W(0x0040), W(0), 0x01, 2, W(500)));
    expect_cmd(CONF_RSP, 7, BYTES(W(0x0070), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, ch));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, ch), 500);
    // Send and receive.
    CHECK(!bt_l2cap_wants_send(&L));
    CHECK(bt_l2cap_send(&L, ch, (const uint8_t *)"hello", 5));
    CHECK(bt_l2cap_wants_send(&L));
    CHECK(!bt_l2cap_send(&L, ch, (const uint8_t *)"again", 5));    // one slot
    expect_data(0x0070, BYTES('h', 'e', 'l', 'l', 'o'));
    CHECK(bt_l2cap_send(&L, ch, (const uint8_t *)"again", 5));
    expect_data(0x0070, BYTES('a', 'g', 'a', 'i', 'n'));
    peer_data(0x0040, BYTES(1, 2, 3));
    CHECK_EQ(delivered, 1);
    CHECK_EQ(got_channel, ch);
    CHECK_EQ(got_psm, BT_L2CAP_PSM_SDP);
    CHECK_EQ(got_n, 3);
    CHECK(got[0] == 1 && got[2] == 3);
    // Close: data still waiting goes with the channel.
    CHECK(bt_l2cap_send(&L, ch, (const uint8_t *)"late", 4));
    bt_l2cap_close(&L, ch, T);
    CHECK(!bt_l2cap_is_open(&L, ch));
    CHECK(!bt_l2cap_is_free(&L, ch));
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    expect_nothing();
    peer_data(0x0040, BYTES(9));          // data after the close is not delivered
    CHECK_EQ(delivered, 1);
    peer_cmd(DISC_RSP, 3, BYTES(W(0x0070), W(0x0040)));
    CHECK(bt_l2cap_is_free(&L, ch));
    CHECK_EQ(L.strays, 0);
    expect_nothing();
}

static void test_we_open_sdp_peers_first(void)
{
    fresh();
    int ch = we_connect();
    peer_cmd(CONF_REQ, 7, BYTES(W(0x0040), W(0), 0x01, 2, W(672)));
    expect_cmd(CONF_RSP, 7, BYTES(W(0x0070), W(0), W(0)));
    CHECK(!bt_l2cap_is_open(&L, ch));
    CHECK(!L.ch[0].ours_done && L.ch[0].peer_done);
    CHECK(!bt_l2cap_send(&L, ch, (const uint8_t *)"x", 1));
    peer_data(0x0040, BYTES(1));
    CHECK_EQ(delivered, 0);
    CHECK_EQ(L.closed_cid, 1);
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, ch));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, ch), 672);
    // A response nobody is waiting for any more is a stray.
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));
    CHECK_EQ(L.strays, 1);
    // A request that states no MTU leaves the peer at the default 672.
    peer_cmd(CONF_REQ, 8, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 8, BYTES(W(0x0070), W(0), W(0)));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, ch), 672);
    // The peer disconnects.
    peer_cmd(DISC_REQ, 11, BYTES(W(0x0040), W(0x0070)));
    expect_cmd(DISC_RSP, 11, BYTES(W(0x0040), W(0x0070)));
    CHECK(bt_l2cap_is_free(&L, ch));
}

static void test_peer_opens_sdp(void)
{
    fresh();
    // Not admitted yet: PSM not supported.
    peer_cmd(CONN_REQ, 4, BYTES(W(0x0001), W(0x0055)));
    expect_cmd(CONN_RSP, 4, BYTES(W(0), W(0x0055), W(2), W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    open_incoming();
    CHECK(!L.ch[0].outgoing);
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 672);
    peer_data(0x0040, BYTES(7, 7));
    CHECK_EQ(delivered, 1);
    // SDP needs no encryption; the response is ours to send back.
    CHECK(bt_l2cap_send(&L, 0, (const uint8_t *)"ab", 2));
    expect_data(0x0055, BYTES('a', 'b'));
}

static void test_peer_opens_avdtp(void)
{
    fresh();
    peer_cmd(CONN_REQ, 4, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 4, BYTES(W(0), W(0x0060), W(2), W(0)));
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    // Admitted, not yet encrypted: pending, authentication pending, with a
    // channel held and our CID named.
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    expect_nothing();
    CHECK_EQ(L.ch[0].state, BT_L2CAP_AWAITING_SECURITY);
    CHECK(!bt_l2cap_is_free(&L, 0) && !bt_l2cap_is_open(&L, 0));
    peer_data(0x0040, BYTES(1));
    CHECK_EQ(delivered, 0);
    // Encryption: the final answer under the request's identifier, then
    // configuration exactly as for a channel accepted at once, to open.
    T += 3000;
    tick(true);
    bt_l2cap_set_encrypted(&L, true);
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(0), W(0)));
    expect_cmd(CONF_REQ, 1, BYTES(W(0x0060), W(0), 0x01, 2, MTU_OPT));
    CHECK_EQ(L.ch[0].psm, 0x0019);
    CHECK_EQ(L.ch[0].config_deadline, T + 30000);
    peer_cmd(CONF_RSP, 1, BYTES(W(0x0040), W(0), W(0)));
    peer_cmd(CONF_REQ, 6, BYTES(W(0x0040), W(0), 0x01, 2, W(895)));
    expect_cmd(CONF_RSP, 6, BYTES(W(0x0060), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 895);
    peer_data(0x0040, BYTES(1));
    CHECK_EQ(delivered, 1);
    CHECK_EQ(got_psm, 0x0019);
    // Once encrypted, accepted at once.
    peer_cmd(CONN_REQ, 7, BYTES(W(0x0019), W(0x0061)));
    expect_cmd(CONN_RSP, 7, BYTES(W(0x0041), W(0x0061), W(0), W(0)));
    expect_cmd(CONF_REQ, 2, BYTES(W(0x0061), W(0), 0x01, 2, MTU_OPT));
    // Taken back out: refused.
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, false);
    peer_cmd(CONN_REQ, 8, BYTES(W(0x0019), W(0x0062)));
    expect_cmd(CONN_RSP, 8, BYTES(W(0), W(0x0062), W(2), W(0)));
    // Encryption turned off: the next request waits again; the channels
    // already open are untouched.
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    bt_l2cap_set_encrypted(&L, false);
    peer_cmd(CONN_REQ, 9, BYTES(W(0x0019), W(0x0062)));
    expect_cmd(CONN_RSP, 9, BYTES(W(0x0042), W(0x0062), W(1), W(1)));
    CHECK(bt_l2cap_is_open(&L, 0));
}

static void test_security_pending(void)
{
    // Pending, then the wait runs out: a security block 30 s after the request.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    uint64_t at = T;
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    // The same source CID again while it waits: already allocated.
    peer_cmd(CONN_REQ, 7, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 7, BYTES(W(0), W(0x0060), W(7), W(0)));
    // A source CID outside the dynamic range is refused for that, not left waiting.
    peer_cmd(CONN_REQ, 8, BYTES(W(0x0019), W(0x0030)));
    expect_cmd(CONN_RSP, 8, BYTES(W(0), W(0x0030), W(6), W(0)));
    bt_l2cap_set_encrypted(&L, false);            // still not encrypted: nothing to answer
    expect_nothing();
    T = at + 29999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_AWAITING_SECURITY);
    T = at + 30000;
    expect_cmd(CONN_RSP, 5, BYTES(W(0), W(0x0060), W(3), W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    expect_nothing();
    // Encryption after that answers nothing.
    bt_l2cap_set_encrypted(&L, true);
    expect_nothing();

    // SDP needs no encryption: accepted at once beside a channel that waits.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    peer_cmd(CONN_REQ, 6, BYTES(W(0x0001), W(0x0070)));
    expect_cmd(CONN_RSP, 6, BYTES(W(0x0041), W(0x0070), W(0), W(0)));
    expect_cmd(CONF_REQ, 1, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));
    CHECK_EQ(L.ch[0].state, BT_L2CAP_AWAITING_SECURITY);

    // Pending, then the link goes down: freed, nothing said.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    bt_l2cap_reset(&L);
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK(!bt_l2cap_wants_send(&L));
    bt_l2cap_set_encrypted(&L, true);
    expect_nothing();
    T += 40000;
    expect_nothing();

    // Two waiting, answered in order when encryption arrives.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    peer_cmd(CONN_REQ, 9, BYTES(W(0x0019), W(0x0061)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    expect_cmd(CONN_RSP, 9, BYTES(W(0x0041), W(0x0061), W(1), W(1)));
    bt_l2cap_set_encrypted(&L, true);
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(0), W(0)));
    expect_cmd(CONF_REQ, 1, BYTES(W(0x0060), W(0), 0x01, 2, MTU_OPT));
    expect_cmd(CONN_RSP, 9, BYTES(W(0x0041), W(0x0061), W(0), W(0)));
    expect_cmd(CONF_REQ, 2, BYTES(W(0x0061), W(0), 0x01, 2, MTU_OPT));
    // Encryption announced again changes nothing.
    bt_l2cap_set_encrypted(&L, true);
    expect_nothing();

    // A waiting channel the host closes is refused; one the peer gives up
    // on is answered and freed.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    peer_cmd(CONN_REQ, 6, BYTES(W(0x0019), W(0x0061)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0x0040), W(0x0060), W(1), W(1)));
    expect_cmd(CONN_RSP, 6, BYTES(W(0x0041), W(0x0061), W(1), W(1)));
    bt_l2cap_close(&L, 0, T);
    expect_cmd(CONN_RSP, 5, BYTES(W(0), W(0x0060), W(3), W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    peer_cmd(DISC_REQ, 7, BYTES(W(0x0041), W(0x0061)));
    expect_cmd(DISC_RSP, 7, BYTES(W(0x0041), W(0x0061)));
    CHECK(bt_l2cap_is_free(&L, 1));
    bt_l2cap_set_encrypted(&L, true);
    expect_nothing();

    // With every channel taken, no resources, not pending.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    for (int i = 0; i < BT_L2CAP_CHANNELS; i++) CHECK(bt_l2cap_connect(&L, 1, T) == i);
    for (int i = 0; i < BT_L2CAP_CHANNELS; i++) tick(true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    expect_cmd(CONN_RSP, 5, BYTES(W(0), W(0x0060), W(4), W(0)));
    char text[512];
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_AVDTP, true);
    peer_cmd(CONN_REQ, 5, BYTES(W(0x0019), W(0x0060)));
    bt_l2cap_status(&L, text, sizeof(text));
    CHECK(strstr(text, "channel 0: awaiting security psm 0019") != NULL);
}

static void test_config_deadline(void)
{
    // The peer accepts our configuration and never sends its own.
    fresh();
    we_connect();
    uint64_t at = T;
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));
    T = at + 29999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONFIG);
    T = at + 30000;
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    expect_nothing();
    CHECK_EQ(L.ch[0].state, BT_L2CAP_DISCONNECTING);
    peer_cmd(DISC_RSP, 3, BYTES(W(0x0070), W(0x0040)));
    CHECK(bt_l2cap_is_free(&L, 0));
    // The same for a channel the peer opened.
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    at = T;
    peer_connects(BT_L2CAP_PSM_SDP, 9);
    peer_cmd(CONF_RSP, 1, BYTES(W(0x0040), W(0), W(0)));
    T = at + 29999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONFIG);
    T = at + 30000;
    expect_cmd(DISC_REQ, 2, BYTES(W(0x0055), W(0x0040)));
    // Our request's RTX and the deadline running out in one tick close once.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    at = T;
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    for (int i = 0; i < 20; i++) { T += 1000; tick(false); }
    expect_cmd(CONF_REQ, 2, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));     // at +20 s: its RTX ends at +30 s
    T = at + 29999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONFIG);
    T = at + 30000;
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    CHECK_EQ(L.ch[0].state, BT_L2CAP_DISCONNECTING);
    expect_nothing();
    T = at + 39999; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T = at + 40000; tick(true);
    CHECK(bt_l2cap_is_free(&L, 0));
    // An open channel has no deadline.
    fresh();
    open_outgoing();
    T += 120000;
    expect_nothing();
    CHECK(bt_l2cap_is_open(&L, 0));
    // A reconfiguration gets its own 30 s from when it begins.
    at = T;
    peer_cmd(CONF_REQ, 20, BYTES(W(0x0040), W(1)));
    expect_cmd(CONF_RSP, 20, BYTES(W(0x0070), W(1), W(0)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    T = at + 29999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONFIG);
    T = at + 30000;
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
}

// A payload queued before the peer reconfigures leaves only while the
// channel is open, and only if it still fits.
static void test_held_payload(void)
{
    static uint8_t p[100];
    for (int i = 0; i < 100; i++) p[i] = (uint8_t)i;
    // Held through a reconfiguration in parts, then sent.
    fresh();
    open_outgoing();
    CHECK(bt_l2cap_send(&L, 0, p, 80));
    peer_cmd(CONF_REQ, 20, BYTES(W(0x0040), W(1), 0x01, 2, W(300)));
    expect_cmd(CONF_RSP, 20, BYTES(W(0x0070), W(1), W(0)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    CHECK(!bt_l2cap_wants_send(&L));
    for (int i = 0; i < 5; i++) { T += 10; expect_nothing(); }
    CHECK(L.ch[0].queued);
    CHECK(!bt_l2cap_send(&L, 0, p, 10));                  // its slot is still taken
    peer_cmd(CONF_REQ, 21, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 21, BYTES(W(0x0070), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK(bt_l2cap_wants_send(&L));
    expect_data(0x0070, p, 80);
    CHECK_EQ(L.outgrown, 0);
    // Outgrown: the peer now takes 48, the payload is 80. Dropped, counted, shown.
    fresh();
    open_outgoing();
    CHECK(bt_l2cap_send(&L, 0, p, 80));
    peer_cmd(CONF_REQ, 22, BYTES(W(0x0040), W(0), 0x01, 2, W(48)));
    expect_cmd(CONF_RSP, 22, BYTES(W(0x0070), W(0), W(0)));
    CHECK(!bt_l2cap_wants_send(&L));
    expect_nothing();
    CHECK(!L.ch[0].queued);
    CHECK_EQ(L.outgrown, 1);
    char text[512];
    bt_l2cap_status(&L, text, sizeof(text));
    CHECK(strstr(text, "outgrown 1\n") != NULL);
    CHECK(bt_l2cap_send(&L, 0, p, 48));
    expect_data(0x0070, p, 48);
    // Exactly the new MTU still fits.
    fresh();
    open_outgoing();
    CHECK(bt_l2cap_send(&L, 0, p, 48));
    peer_cmd(CONF_REQ, 23, BYTES(W(0x0040), W(0), 0x01, 2, W(48)));
    expect_cmd(CONF_RSP, 23, BYTES(W(0x0070), W(0), W(0)));
    expect_data(0x0070, p, 48);
    CHECK_EQ(L.outgrown, 0);
    // A held payload blocks neither signalling nor another channel's data,
    // older though it is.
    fresh();
    open_outgoing();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    peer_cmd(CONN_REQ, 40, BYTES(W(1), W(0x0080)));
    expect_cmd(CONN_RSP, 40, BYTES(W(0x0041), W(0x0080), W(0), W(0)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0080), W(0), 0x01, 2, MTU_OPT));
    peer_cmd(CONF_RSP, 3, BYTES(W(0x0041), W(0), W(0)));
    peer_cmd(CONF_REQ, 41, BYTES(W(0x0041), W(0)));
    expect_cmd(CONF_RSP, 41, BYTES(W(0x0080), W(0), W(0)));
    CHECK(bt_l2cap_send(&L, 0, BYTES(0xa1)));
    CHECK(bt_l2cap_send(&L, 1, BYTES(0xb1)));
    peer_cmd(CONF_REQ, 42, BYTES(W(0x0040), W(1)));
    expect_cmd(CONF_RSP, 42, BYTES(W(0x0070), W(1), W(0)));
    peer_cmd(ECHO_REQ, 43, BYTES(7));
    expect_cmd(ECHO_RSP, 43, BYTES(7));
    expect_data(0x0080, BYTES(0xb1));
    expect_nothing();
    CHECK(L.ch[0].queued);
    peer_cmd(CONF_REQ, 44, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 44, BYTES(W(0x0070), W(0), W(0)));
    CHECK(bt_l2cap_send(&L, 1, BYTES(0xb2)));               // newer than the held one
    expect_data(0x0070, BYTES(0xa1));
    expect_data(0x0080, BYTES(0xb2));
    expect_nothing();
}

static void test_late_success(void)
{
    // A channel closed while connecting, then the peer's success arrives:
    // the peer's end is asked to disconnect, nothing of ours is allocated.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    bt_l2cap_close(&L, 0, T);
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    CHECK_EQ(L.strays, 1);
    for (unsigned i = 0; i < BT_L2CAP_CHANNELS; i++) CHECK(bt_l2cap_is_free(&L, i));
    expect_cmd(DISC_REQ, 2, BYTES(W(0x0070), W(0x0040)));
    expect_nothing();
    T += 30000;                      // no timer behind it
    expect_nothing();
    peer_cmd(DISC_RSP, 2, BYTES(W(0x0070), W(0x0040)));
    CHECK_EQ(L.strays, 2);
    CHECK_EQ(L.malformed, 0);
    // A request that timed out, answered late.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    T += 10000; tick(true);
    CHECK(bt_l2cap_is_free(&L, 0));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0071), W(0x0040), W(0), W(0)));
    expect_cmd(DISC_REQ, 2, BYTES(W(0x0071), W(0x0040)));
    // Not a success, or a destination outside the dynamic range: nothing to disconnect.
    peer_cmd(CONN_RSP, 3, BYTES(W(0x0072), W(0x0041), W(1), W(0)));
    peer_cmd(CONN_RSP, 4, BYTES(W(0x0072), W(0x0041), W(2), W(0)));
    peer_cmd(CONN_RSP, 5, BYTES(W(0x003f), W(0x0041), W(0), W(0)));
    expect_nothing();
    CHECK_EQ(L.strays, 4);
    // A duplicate success for a channel we hold is left alone.
    fresh();
    open_outgoing();
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    CHECK_EQ(L.strays, 1);
    expect_nothing();
    CHECK(bt_l2cap_is_open(&L, 0));
    // With the queue full, counted and dropped.
    fresh();
    uint8_t c[64], f[80];
    size_t n = 0;
    for (int i = 0; i < 8; i++) { c[n++] = ECHO_REQ; c[n++] = (uint8_t)(i + 1); c[n++] = 0; c[n++] = 0; }
    peer_pdu(f, frame(f, 1, c, n));
    peer_cmd(CONN_RSP, 9, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    CHECK_EQ(L.queue_full, 1);
    CHECK_EQ(L.strays, 1);
}

static void test_connection_refusals(void)
{
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    // A PSM that is not valid is never admitted, and is not supported.
    bt_l2cap_admit(&L, 0x0100, true);
    bt_l2cap_admit(&L, 0x0002, true);
    bt_l2cap_admit(&L, 0x0101, true);
    CHECK_EQ(L.admitted[1], 0);
    CHECK_EQ(bt_l2cap_connect(&L, 0x0101, T), -1);
    peer_cmd(CONN_REQ, 1, BYTES(W(0x0100), W(0x0055)));
    expect_cmd(CONN_RSP, 1, BYTES(W(0), W(0x0055), W(2), W(0)));
    peer_cmd(CONN_REQ, 2, BYTES(W(0x0002), W(0x0055)));
    expect_cmd(CONN_RSP, 2, BYTES(W(0), W(0x0055), W(2), W(0)));
    peer_cmd(CONN_REQ, 3, BYTES(W(0x0101), W(0x0055)));
    expect_cmd(CONN_RSP, 3, BYTES(W(0), W(0x0055), W(2), W(0)));
    // A source CID outside the dynamic range, and one already ours.
    peer_cmd(CONN_REQ, 4, BYTES(W(0x0001), W(0x003f)));
    expect_cmd(CONN_RSP, 4, BYTES(W(0), W(0x003f), W(6), W(0)));
    peer_connects(BT_L2CAP_PSM_SDP, 5);
    peer_cmd(CONN_REQ, 6, BYTES(W(0x0001), W(0x0055)));
    expect_cmd(CONN_RSP, 6, BYTES(W(0), W(0x0055), W(7), W(0)));
    // No free channel.
    CHECK(bt_l2cap_connect(&L, 1, T) == 1);
    CHECK(bt_l2cap_connect(&L, 1, T) == 2);
    CHECK(bt_l2cap_connect(&L, 1, T) == 3);
    CHECK(bt_l2cap_connect(&L, 1, T) == -1);
    peer_cmd(CONN_REQ, 7, BYTES(W(0x0001), W(0x0090)));
    expect_cmd(CONN_REQ, 2, BYTES(W(0x0001), W(0x0041)));
    expect_cmd(CONN_REQ, 3, BYTES(W(0x0001), W(0x0042)));
    expect_cmd(CONN_REQ, 4, BYTES(W(0x0001), W(0x0043)));
    expect_cmd(CONN_RSP, 7, BYTES(W(0), W(0x0090), W(4), W(0)));
    // A connect to a PSM that is not valid, or on a link that is down.
    bt_l2cap_close(&L, 3, T);
    CHECK(bt_l2cap_is_free(&L, 3));
    CHECK_EQ(bt_l2cap_connect(&L, 0x0002, T), -1);
    bt_l2cap_reset(&L);
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), -1);
}

static void test_outgoing_refused_and_pending(void)
{
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 0x0019, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0019), W(0x0040)));
    // A response with the wrong identifier, or naming another CID, is a stray.
    peer_cmd(CONN_RSP, 9, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0041), W(0), W(0)));
    CHECK_EQ(L.strays, 2);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONNECTING);
    peer_cmd(CONN_RSP, 1, BYTES(W(0), W(0x0040), W(3), W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK_EQ(L.refused, 3);
    CHECK_EQ(L.refused_psm, 0x0019);

    // Pending, then success: ERTX holds the channel well past RTX.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    T += 1500;
    peer_cmd(CONN_RSP, 1, BYTES(W(0), W(0x0040), W(1), W(2)));
    T += 1000; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T += 30000; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    expect_cmd(CONF_REQ, 2, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));

    // Pending, then silence: freed 60 s after the pending answer.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    uint64_t at = T + 500;
    T = at;
    peer_cmd(CONN_RSP, 1, BYTES(W(0), W(0x0040), W(1), W(0)));
    T = at + 59999; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T = at + 60000; tick(true);
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK_EQ(L.refused, 0xffff);
    CHECK(L.up);

    // A success naming a CID outside the dynamic range is not a channel.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0001), W(0x0040), W(0), W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK_EQ(L.malformed, 1);
    CHECK_EQ(L.refused, 0xfffd);
    char text[512];
    bt_l2cap_status(&L, text, sizeof(text));
    CHECK(strstr(text, "accepted without a valid CID (fffd)") != NULL);
    // Closing a channel still connecting frees it and sends nothing.
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    bt_l2cap_close(&L, 0, T);
    CHECK(bt_l2cap_is_free(&L, 0));
    expect_nothing();
    // A Command Reject answering our Connection Request frees it.
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 3, BYTES(W(0x0001), W(0x0042)));
    peer_cmd(REJ, 3, BYTES(W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK_EQ(L.refused, 0xfffe);
    // A reject of nothing is a stray; a reject too short is malformed and unanswered.
    peer_cmd(REJ, 3, BYTES(W(0)));
    CHECK_EQ(L.strays, 1);
    peer_cmd(REJ, 4, BYTES(0));
    CHECK_EQ(L.malformed, 2);
    expect_nothing();
}

static void test_rtx(void)
{
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    // The timer runs from the hand-over, not from the queueing.
    T += 5000; tick(false);
    CHECK(!bt_l2cap_is_free(&L, 0));
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    uint64_t at = T;
    T = at + 9999; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T = at + 10000; tick(true);
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK_EQ(L.refused, 0xffff);
    CHECK(L.up);
    // The link is still there: a new channel opens.
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 2, BYTES(W(0x0001), W(0x0041)));
    peer_cmd(CONN_RSP, 2, BYTES(W(0x0071), W(0x0041), W(0), W(0)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0071), W(0), 0x01, 2, MTU_OPT));
    // An unanswered Configuration Request closes the channel, whose
    // Disconnection Request frees it when that goes unanswered too.
    at = T;
    T = at + 9999; tick(true);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONFIG);
    T = at + 10000;
    expect_cmd(DISC_REQ, 4, BYTES(W(0x0071), W(0x0041)));
    CHECK_EQ(L.ch[0].state, BT_L2CAP_DISCONNECTING);
    at = T;
    T = at + 9999; tick(true);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T = at + 10000; tick(true);
    CHECK(bt_l2cap_is_free(&L, 0));
    CHECK(L.up);
}

static void test_identifiers(void)
{
    fresh();
    L.next_ident = 254;
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 254, BYTES(W(0x0001), W(0x0040)));
    peer_cmd(CONN_RSP, 254, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    expect_cmd(CONF_REQ, 255, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));
    bt_l2cap_close(&L, 0, T);
    expect_cmd(DISC_REQ, 1, BYTES(W(0x0070), W(0x0040)));      // never 0
    // A peer command with identifier 0 is not understood.
    peer_cmd(ECHO_REQ, 0, NONE);
    expect_cmd(REJ, 0, BYTES(W(0)));
    // A Command Reject with identifier 0 answers nothing, even on a channel
    // with no request outstanding.
    fresh();
    open_outgoing();
    peer_cmd(REJ, 0, BYTES(W(0)));
    CHECK_EQ(L.strays, 1);
    CHECK(bt_l2cap_is_open(&L, 0));
    expect_nothing();
    // Our CIDs rotate, skip one in use, and never leave the dynamic range.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    L.next_cid = 0xffff;
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 1);
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 2);
    CHECK_EQ(L.ch[1].cid, 0xffff);
    CHECK_EQ(L.ch[2].cid, 0x0041);
    bt_l2cap_close(&L, 0, T);
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    CHECK_EQ(L.ch[0].cid, 0x0042);
    // A response of the wrong kind with a live identifier is a stray.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    peer_cmd(DISC_RSP, 1, BYTES(W(0), W(0x0040)));
    peer_cmd(CONF_RSP, 1, BYTES(W(0x0040), W(0), W(0)));
    CHECK_EQ(L.strays, 2);
    CHECK_EQ(L.ch[0].state, BT_L2CAP_CONNECTING);
    // Configuration for a channel still connecting is for no channel.
    peer_cmd(CONF_REQ, 9, BYTES(W(0x0040), W(0)));
    expect_cmd(REJ, 9, BYTES(W(2), W(0x0040), W(0)));
}

// The peer's Configuration Request against an incoming SDP channel whose
// own direction is done (our CID 0x0040, the peer's 0x0055).
static void conf_setup(void)
{
    fresh();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    peer_connects(BT_L2CAP_PSM_SDP, 9);
    peer_cmd(CONF_RSP, 1, BYTES(W(0x0040), W(0), W(0)));
    CHECK(L.ch[0].ours_done);
}

static void test_config_options(void)
{
    // MTU 47 refused with 48; then 48 taken.
    conf_setup();
    peer_cmd(CONF_REQ, 20, BYTES(W(0x0040), W(0), 0x01, 2, W(47)));
    expect_cmd(CONF_RSP, 20, BYTES(W(0x0055), W(0), W(1), 0x01, 2, W(48)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 21, BYTES(W(0x0040), W(0), 0x01, 2, W(48)));
    expect_cmd(CONF_RSP, 21, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 48);
    CHECK(!bt_l2cap_send(&L, 0, got, 49));
    CHECK(bt_l2cap_send(&L, 0, got, 48));

    // Flush timeout, FCS, best-effort QoS and Basic mode: all taken.
    conf_setup();
    peer_cmd(CONF_REQ, 22, BYTES(W(0x0040), W(0), 0x02, 2, W(0xffff), 0x05, 1, 0x00,
             0x03, 22, 0, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
             0x04, 9, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 2, W(900)));
    expect_cmd(CONF_RSP, 22, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 900);

    // Guaranteed QoS: unacceptable, carrying the option as best effort.
    conf_setup();
    peer_cmd(CONF_REQ, 23, BYTES(W(0x0040), W(0),
             0x03, 22, 0, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20));
    expect_cmd(CONF_RSP, 23, BYTES(W(0x0055), W(0), W(1),
             0x03, 22, 0, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20));
    CHECK(!bt_l2cap_is_open(&L, 0));
    // No traffic is not best effort either.
    peer_cmd(CONF_REQ, 24, BYTES(W(0x0040), W(0),
             0x03, 22, 0, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20));
    expect_cmd(CONF_RSP, 24, BYTES(W(0x0055), W(0), W(1),
             0x03, 22, 0, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20));

    // ERTM and streaming refused, carrying Basic mode.
    conf_setup();
    peer_cmd(CONF_REQ, 25, BYTES(W(0x0040), W(0), 0x04, 9, 0x03, 10, 3, W(2000), W(12000), W(672)));
    expect_cmd(CONF_RSP, 25, BYTES(W(0x0055), W(0), W(1), 0x04, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    peer_cmd(CONF_REQ, 26, BYTES(W(0x0040), W(0), 0x04, 9, 0x04, 0, 0, 0, 0, 0, 0, 0, 0));
    expect_cmd(CONF_RSP, 26, BYTES(W(0x0055), W(0), W(1), 0x04, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    // Two unacceptable options in one request: both corrected, in order.
    peer_cmd(CONF_REQ, 27, BYTES(W(0x0040), W(0), 0x01, 2, W(20), 0x04, 9, 0x01, 0, 0, 0, 0, 0, 0, 0, 0));
    expect_cmd(CONF_RSP, 27, BYTES(W(0x0055), W(0), W(1), 0x01, 2, W(48), 0x04, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 28, BYTES(W(0x0040), W(0), 0x04, 9, 0x00, 0, 0, 0, 0, 0, 0, 0, 0));
    expect_cmd(CONF_RSP, 28, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));

    // Unknown options listed by type; a hinted one skipped; unknown beats unacceptable.
    conf_setup();
    peer_cmd(CONF_REQ, 30, BYTES(W(0x0040), W(0), 0x09, 1, 0xaa, 0x89, 3, 1, 2, 3, 0x06, 16,
             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 2, W(40)));
    expect_cmd(CONF_RSP, 30, BYTES(W(0x0055), W(0), W(3), 0x09, 0x06));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 31, BYTES(W(0x0040), W(0), 0x89, 3, 1, 2, 3, 0x81, 2, W(100)));
    expect_cmd(CONF_RSP, 31, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 100);   // the hint bit on a known option changes nothing

    // An option that runs past the request, or a known one of the wrong
    // length, is not understood and moves nothing.
    conf_setup();
    peer_cmd(CONF_REQ, 32, BYTES(W(0x0040), W(0), 0x01, 3, W(100)));
    expect_cmd(REJ, 32, BYTES(W(0)));
    peer_cmd(CONF_REQ, 33, BYTES(W(0x0040), W(0), 0x01, 3, W(100), 0));
    expect_cmd(REJ, 33, BYTES(W(0)));
    peer_cmd(CONF_REQ, 34, BYTES(W(0x0040), W(0), 0x01));
    expect_cmd(REJ, 34, BYTES(W(0)));
    peer_cmd(CONF_REQ, 35, BYTES(W(0x0040), W(0), 0x02, 1, 0));
    expect_cmd(REJ, 35, BYTES(W(0)));
    peer_cmd(CONF_REQ, 36, BYTES(W(0x0040), W(0), 0x03, 21, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    expect_cmd(REJ, 36, BYTES(W(0)));
    peer_cmd(CONF_REQ, 37, BYTES(W(0x0040), W(0), 0x04, 8, 0, 0, 0, 0, 0, 0, 0, 0));
    expect_cmd(REJ, 37, BYTES(W(0)));
    peer_cmd(CONF_REQ, 38, BYTES(W(0x0040), W(0), 0x05, 2, 0, 0));
    expect_cmd(REJ, 38, BYTES(W(0)));
    peer_cmd(CONF_REQ, 39, BYTES(W(0x0040)));
    expect_cmd(REJ, 39, BYTES(W(0)));
    peer_cmd(CONF_REQ, 41, BYTES(W(0x0040), W(0), 0x09, 3, 1));
    expect_cmd(REJ, 41, BYTES(W(0)));
    CHECK_EQ(L.ch[0].peer_rounds, 0);
    CHECK_EQ(L.malformed, 9);
    CHECK(!L.ch[0].peer_done);
    // Configuration for a CID that is no channel: invalid CID, ours then none.
    peer_cmd(CONF_REQ, 40, BYTES(W(0x0047), W(0)));
    expect_cmd(REJ, 40, BYTES(W(2), W(0x0047), W(0)));
}

static void test_config_continuation(void)
{
    conf_setup();
    peer_cmd(CONF_REQ, 50, BYTES(W(0x0040), W(1), 0x01, 2, W(300)));
    expect_cmd(CONF_RSP, 50, BYTES(W(0x0055), W(1), W(0)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 51, BYTES(W(0x0040), W(1), 0x02, 2, W(100)));
    expect_cmd(CONF_RSP, 51, BYTES(W(0x0055), W(1), W(0)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 52, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 52, BYTES(W(0x0055), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 300);       // gathered across the parts
    // The next configuration starts over at the default.
    peer_cmd(CONF_REQ, 53, BYTES(W(0x0040), W(1)));
    expect_cmd(CONF_RSP, 53, BYTES(W(0x0055), W(1), W(0)));
    CHECK(!bt_l2cap_is_open(&L, 0));
    peer_cmd(CONF_REQ, 54, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 54, BYTES(W(0x0055), W(0), W(0)));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 672);
    // A failing part carries no flag and ends the gathering.
    conf_setup();
    peer_cmd(CONF_REQ, 55, BYTES(W(0x0040), W(1), 0x01, 2, W(200)));
    expect_cmd(CONF_RSP, 55, BYTES(W(0x0055), W(1), W(0)));
    peer_cmd(CONF_REQ, 56, BYTES(W(0x0040), W(1), 0x0f, 0));
    expect_cmd(CONF_RSP, 56, BYTES(W(0x0055), W(0), W(3), 0x0f));
    peer_cmd(CONF_REQ, 57, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 57, BYTES(W(0x0055), W(0), W(0)));
    CHECK_EQ(bt_l2cap_peer_mtu(&L, 0), 672);
}

static void test_config_rounds(void)
{
    // The peer's direction: four requests, then the channel is closed.
    conf_setup();
    for (uint8_t i = 0; i < 4; i++) {
        peer_cmd(CONF_REQ, (uint8_t)(60 + i), BYTES(W(0x0040), W(0), 0x01, 2, W(10)));
        expect_cmd(CONF_RSP, (uint8_t)(60 + i), BYTES(W(0x0055), W(0), W(1), 0x01, 2, W(48)));
    }
    peer_cmd(CONF_REQ, 64, BYTES(W(0x0040), W(0), 0x01, 2, W(48)));
    expect_cmd(DISC_REQ, 2, BYTES(W(0x0055), W(0x0040)));
    CHECK_EQ(L.ch[0].state, BT_L2CAP_DISCONNECTING);
    peer_cmd(DISC_RSP, 2, BYTES(W(0x0055), W(0x0040)));
    CHECK(bt_l2cap_is_free(&L, 0));

    // Our direction: an unacceptable MTU we can state is asked for again.
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(600)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0070), W(0), 0x01, 2, W(600)));
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(0)));         // the old identifier: a stray
    CHECK_EQ(L.strays, 1);
    CHECK(!L.ch[0].ours_done);
    peer_cmd(CONF_RSP, 3, BYTES(W(0x0040), W(0), W(0)));
    CHECK(L.ch[0].ours_done);
    // Data over the MTU we now state is dropped.
    peer_cmd(CONF_REQ, 70, BYTES(W(0x0040), W(0)));
    expect_cmd(CONF_RSP, 70, BYTES(W(0x0070), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 0));
    static uint8_t big[700];
    peer_data(0x0040, big, 601);
    CHECK_EQ(delivered, 0);
    peer_data(0x0040, big, 600);
    CHECK_EQ(delivered, 1);

    // ... up to four of our requests; the fourth refusal closes.
    fresh();
    we_connect();
    for (uint8_t i = 0; i < 3; i++) {
        peer_cmd(CONF_RSP, (uint8_t)(2 + i), BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(600)));
        expect_cmd(CONF_REQ, (uint8_t)(3 + i), BYTES(W(0x0070), W(0), 0x01, 2, W(600)));
    }
    peer_cmd(CONF_RSP, 5, BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(600)));
    expect_cmd(DISC_REQ, 6, BYTES(W(0x0070), W(0x0040)));

    // An MTU under 48, one over ours, none at all, or another failure: closed.
    static const uint16_t bad[] = { 47, BT_L2CAP_MTU + 1 };
    for (int k = 0; k < 2; k++) {
        fresh();
        we_connect();
        peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(bad[k])));
        expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    }
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(1)));
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(2), 0x01, 2, W(600)));
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    // The MTU option with the hint bit is still the MTU option.
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(1), 0x81, 2, W(600)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0070), W(0), 0x01, 2, W(600)));
    // Exactly 48 and exactly ours are taken.
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(48)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0070), W(0), 0x01, 2, W(48)));
    peer_cmd(CONF_RSP, 3, BYTES(W(0x0040), W(0), W(1), 0x01, 2, W(BT_L2CAP_MTU)));
    expect_cmd(CONF_REQ, 4, BYTES(W(0x0070), W(0), 0x01, 2, MTU_OPT));
    // A refusal whose options are malformed closes and is counted.
    peer_cmd(CONF_RSP, 4, BYTES(W(0x0040), W(0), W(1), 0x01, 2, 0x30));
    expect_cmd(DISC_REQ, 5, BYTES(W(0x0070), W(0x0040)));
    CHECK_EQ(L.malformed, 1);
    // A refusal from another CID's point of view is a stray.
    fresh();
    we_connect();
    peer_cmd(CONF_RSP, 2, BYTES(W(0x0041), W(0), W(0)));
    CHECK_EQ(L.strays, 1);
    CHECK(!L.ch[0].ours_done);
    // A Command Reject of our Configuration Request closes.
    peer_cmd(REJ, 2, BYTES(W(0)));
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    // ... and one of our Disconnection Request frees.
    peer_cmd(REJ, 3, BYTES(W(2), W(0x0040), W(0x0070)));
    CHECK(bt_l2cap_is_free(&L, 0));
}

static void test_signalling(void)
{
    fresh();
    // Information.
    peer_cmd(INFO_REQ, 3, BYTES(W(2)));
    expect_cmd(INFO_RSP, 3, BYTES(W(2), W(0), 0, 0, 0, 0));
    peer_cmd(INFO_REQ, 4, BYTES(W(3)));
    expect_cmd(INFO_RSP, 4, BYTES(W(3), W(0), 0x02, 0, 0, 0, 0, 0, 0, 0));
    peer_cmd(INFO_REQ, 5, BYTES(W(1)));
    expect_cmd(INFO_RSP, 5, BYTES(W(1), W(1)));
    peer_cmd(INFO_REQ, 6, BYTES(W(0x0302)));
    expect_cmd(INFO_RSP, 6, BYTES(W(0x0302), W(1)));
    peer_cmd(INFO_REQ, 7, BYTES(W(2), 0));
    expect_cmd(REJ, 7, BYTES(W(0)));
    // Echo, the data bounded.
    peer_cmd(ECHO_REQ, 8, BYTES(1, 2, 3, 4, 5));
    expect_cmd(ECHO_RSP, 8, BYTES(1, 2, 3, 4, 5));
    peer_cmd(ECHO_REQ, 9, NONE);
    expect_cmd(ECHO_RSP, 9, NONE);
    uint8_t echo[100];
    for (int i = 0; i < 100; i++) echo[i] = (uint8_t)i;
    peer_cmd(ECHO_REQ, 10, echo, 100);
    expect_cmd(ECHO_RSP, 10, echo, BT_L2CAP_ECHO_BYTES);
    peer_cmd(ECHO_REQ, 10, echo, BT_L2CAP_ECHO_BYTES);
    expect_cmd(ECHO_RSP, 10, echo, BT_L2CAP_ECHO_BYTES);
    // Responses to things we never ask are strays; unknown codes are not understood.
    peer_cmd(ECHO_RSP, 11, BYTES(1));
    peer_cmd(INFO_RSP, 12, BYTES(W(2), W(0), 0, 0, 0, 0));
    CHECK_EQ(L.strays, 2);
    expect_nothing();
    peer_cmd(0x0c, 13, BYTES(1, 2));
    expect_cmd(REJ, 13, BYTES(W(0)));
    peer_cmd(0x00, 14, NONE);
    expect_cmd(REJ, 14, BYTES(W(0)));
    peer_cmd(0x13, 15, NONE);
    expect_cmd(REJ, 15, BYTES(W(0)));
    // Fixed-length commands of the wrong length.
    peer_cmd(CONN_REQ, 16, BYTES(W(1), W(0x55), 0));
    expect_cmd(REJ, 16, BYTES(W(0)));
    peer_cmd(CONN_RSP, 17, BYTES(W(1), W(0x55), W(0)));
    expect_cmd(REJ, 17, BYTES(W(0)));
    peer_cmd(DISC_REQ, 18, BYTES(W(1)));
    expect_cmd(REJ, 18, BYTES(W(0)));
    peer_cmd(DISC_RSP, 19, BYTES(W(1), W(2), 3));
    expect_cmd(REJ, 19, BYTES(W(0)));
    peer_cmd(CONF_RSP, 20, BYTES(W(1), W(2), 3));
    expect_cmd(REJ, 20, BYTES(W(0)));
    CHECK(bt_l2cap_is_free(&L, 0));

    // Several commands in one frame, answered in order.
    fresh();
    uint8_t c[64];
    size_t n = 0;
    c[n++] = ECHO_REQ; c[n++] = 1; c[n++] = 1; c[n++] = 0; c[n++] = 0xee;
    c[n++] = INFO_REQ; c[n++] = 2; c[n++] = 2; c[n++] = 0; c[n++] = 2; c[n++] = 0;
    c[n++] = 0x0d; c[n++] = 3; c[n++] = 0; c[n++] = 0;
    c[n++] = ECHO_REQ; c[n++] = 4; c[n++] = 0; c[n++] = 0;
    uint8_t f[128];
    peer_pdu(f, frame(f, 1, c, n));
    expect_cmd(ECHO_RSP, 1, BYTES(0xee));
    expect_cmd(INFO_RSP, 2, BYTES(W(2), W(0), 0, 0, 0, 0));
    expect_cmd(REJ, 3, BYTES(W(0)));
    expect_cmd(ECHO_RSP, 4, NONE);
    expect_nothing();
    // A command running past the frame is not understood and ends it.
    n = 0;
    c[n++] = ECHO_REQ; c[n++] = 5; c[n++] = 0; c[n++] = 0;
    c[n++] = ECHO_REQ; c[n++] = 6; c[n++] = 5; c[n++] = 0; c[n++] = 1; c[n++] = 2;
    peer_pdu(f, frame(f, 1, c, n));
    expect_cmd(ECHO_RSP, 5, NONE);
    expect_cmd(REJ, 6, BYTES(W(0)));
    expect_nothing();
    // A Command Reject running past it is counted, never answered.
    n = 0;
    c[n++] = REJ; c[n++] = 7; c[n++] = 9; c[n++] = 0; c[n++] = 0;
    unsigned before = L.malformed;
    peer_pdu(f, frame(f, 1, c, n));
    CHECK_EQ(L.malformed, before + 1);
    expect_nothing();
    // Bytes after the last command that cannot be a command.
    n = 0;
    c[n++] = ECHO_REQ; c[n++] = 8; c[n++] = 0; c[n++] = 0; c[n++] = 1; c[n++] = 2; c[n++] = 3;
    before = L.malformed;
    peer_pdu(f, frame(f, 1, c, n));
    expect_cmd(ECHO_RSP, 8, NONE);
    CHECK_EQ(L.malformed, before + 1);
    expect_nothing();
}

static void test_disconnection(void)
{
    fresh();
    open_outgoing();
    // Mismatched CIDs: invalid CID carrying the request's two CIDs.
    peer_cmd(DISC_REQ, 30, BYTES(W(0x0040), W(0x0071)));
    expect_cmd(REJ, 30, BYTES(W(2), W(0x0040), W(0x0071)));
    peer_cmd(DISC_REQ, 31, BYTES(W(0x0041), W(0x0070)));
    expect_cmd(REJ, 31, BYTES(W(2), W(0x0041), W(0x0070)));
    CHECK(bt_l2cap_is_open(&L, 0));
    // A response to a request we never made.
    peer_cmd(DISC_RSP, 3, BYTES(W(0x0070), W(0x0040)));
    CHECK_EQ(L.strays, 1);
    CHECK(bt_l2cap_is_open(&L, 0));
    // Our close crossing the peer's: the peer's request is answered and frees it.
    bt_l2cap_close(&L, 0, T);
    peer_cmd(DISC_REQ, 32, BYTES(W(0x0040), W(0x0070)));
    expect_cmd(DISC_RSP, 32, BYTES(W(0x0040), W(0x0070)));
    expect_nothing();            // our own request went with the channel
    CHECK(bt_l2cap_is_free(&L, 0));
    // Our close answered with the wrong CIDs is a stray.
    fresh();
    open_outgoing();
    bt_l2cap_close(&L, 0, T);
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    peer_cmd(DISC_RSP, 3, BYTES(W(0x0040), W(0x0070)));
    CHECK_EQ(L.strays, 1);
    CHECK(!bt_l2cap_is_free(&L, 0));
    bt_l2cap_close(&L, 0, T);          // closing again changes nothing
    expect_nothing();
    CHECK_EQ(L.ch[0].state, BT_L2CAP_DISCONNECTING);
    // A close before our Configuration Request went out sends only the
    // Disconnection Request.
    fresh();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
    peer_cmd(CONN_RSP, 1, BYTES(W(0x0070), W(0x0040), W(0), W(0)));
    bt_l2cap_close(&L, 0, T);
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    expect_nothing();
    fresh();
    open_outgoing();
    bt_l2cap_close(&L, 0, T);
    expect_cmd(DISC_REQ, 3, BYTES(W(0x0070), W(0x0040)));
    peer_cmd(DISC_RSP, 3, BYTES(W(0x0070), W(0x0040)));
    CHECK(bt_l2cap_is_free(&L, 0));
}

// A data PDU on the open outgoing channel, cut every way.
static void test_reassembly(void)
{
    fresh();
    open_outgoing();
    uint8_t payload[40], f[64];
    for (int i = 0; i < 40; i++) payload[i] = (uint8_t)(0x80 + i);
    size_t n = frame(f, 0x0040, payload, sizeof(payload));
    for (size_t cut = 0; cut <= n; cut++) {
        delivered = 0;
        acl(2, f, cut);
        if (cut < n) acl(1, f + cut, n - cut);
        CHECK_EQ(delivered, 1);
        CHECK(got_n == 40 && !memcmp(got, payload, 40));
    }
    // Three parts, at every pair of cuts.
    for (size_t a = 0; a <= n; a++) for (size_t b = a; b <= n; b++) {
        delivered = 0;
        acl(0, f, a);           // 0b00, a start that is not flushable
        if (b > a) acl(1, f + a, b - a);
        if (n > b) acl(1, f + b, n - b);
        if (delivered != 1 || got_n != 40 || memcmp(got, payload, 40)) {
            failures++; printf("FAIL cuts %zu %zu\n", a, b);
        }
    }
    // One byte at a time.
    delivered = 0;
    acl(2, f, 1);
    for (size_t i = 1; i < n; i++) acl(1, f + i, 1);
    CHECK_EQ(delivered, 1);
    // Signalling cut at every byte too.
    uint8_t s[32];
    size_t sn = command(s, ECHO_REQ, 0x42, BYTES(1, 2, 3, 4));
    for (size_t cut = 0; cut <= sn; cut++) {
        acl(2, s, cut);
        if (cut < sn) acl(1, s + cut, sn - cut);
        expect_cmd(ECHO_RSP, 0x42, BYTES(1, 2, 3, 4));
    }
    expect_nothing();
    CHECK_EQ(L.orphans + L.unfinished + L.malformed + L.oversized, 0);

    // A continuation with nothing begun.
    delivered = 0;
    acl(1, f, n);
    CHECK_EQ(L.orphans, 1);
    CHECK_EQ(delivered, 0);
    // After a whole PDU, a continuation has nothing to continue either.
    acl(2, f, n);
    acl(1, f, 4);
    CHECK_EQ(L.orphans, 2);
    CHECK_EQ(delivered, 1);
    // A start while one is unfinished: the unfinished one goes, the new one is taken.
    delivered = 0;
    acl(2, f, 10);
    acl(2, f, 5);
    CHECK_EQ(L.unfinished, 1);
    acl(1, f + 5, n - 5);
    CHECK_EQ(delivered, 1);
    CHECK(got_n == 40 && !memcmp(got, payload, 40));
    // A start whose header is not whole yet, abandoned the same way.
    acl(2, f, 2);
    acl(2, f, n);
    CHECK_EQ(L.unfinished, 2);
    CHECK_EQ(delivered, 2);
    // An ACL packet carrying more than the PDU: dropped whole.
    uint8_t g[80];
    memcpy(g, f, n); g[n] = 0x99;
    delivered = 0;
    acl(2, g, n + 1);
    CHECK_EQ(delivered, 0);
    CHECK_EQ(L.malformed, 1);
    acl(2, f, 20);
    memcpy(g, f + 20, n - 20); g[n - 20] = 0x99;
    acl(1, g, n - 20 + 1);
    CHECK_EQ(delivered, 0);
    CHECK_EQ(L.malformed, 2);
    acl(1, f, 4);                                  // and nothing is left begun
    CHECK_EQ(L.orphans, 3);
    // ACL headers that lie, or flags that are not ours.
    uint8_t bad[8] = { 0xbc, 0x2a, 5, 0, 1, 2, 3, 4 };
    bt_l2cap_receive(&L, bad, 8, T, on_deliver, NULL);
    bad[2] = 3;
    bt_l2cap_receive(&L, bad, 8, T, on_deliver, NULL);
    bt_l2cap_receive(&L, bad, 3, T, on_deliver, NULL);
    CHECK_EQ(L.malformed, 5);
    uint16_t hf[] = { 0x3abc, 0x6abc, 0xaabc, 0xeabc };   // 0b11, and broadcast flags
    for (int i = 0; i < 4; i++) {
        uint8_t a[4 + 64];
        a[0] = (uint8_t)hf[i]; a[1] = (uint8_t)(hf[i] >> 8); a[2] = (uint8_t)n; a[3] = 0;
        memcpy(a + 4, f, n);
        bt_l2cap_receive(&L, a, n + 4, T, on_deliver, NULL);
    }
    CHECK_EQ(L.malformed, 9);
    CHECK_EQ(delivered, 0);
    // The handle bits are the host's business, not ours.
    uint8_t a[4 + 64];
    a[0] = 0xff; a[1] = 0x2f; a[2] = (uint8_t)n; a[3] = 0;
    memcpy(a + 4, f, n);
    bt_l2cap_receive(&L, a, n + 4, T, on_deliver, NULL);
    CHECK_EQ(delivered, 1);
}

static void test_oversized(void)
{
    fresh();
    open_outgoing();
    static uint8_t f[2200], p[1100];
    for (int i = 0; i < 1100; i++) p[i] = (uint8_t)i;
    // Exactly the buffer is delivered, whole.
    size_t n = frame(f, 0x0040, p, BT_L2CAP_MTU);
    acl(2, f, 1000);
    acl(1, f + 1000, n - 1000);
    CHECK_EQ(delivered, 1);
    CHECK(got_n == BT_L2CAP_MTU && !memcmp(got, p, BT_L2CAP_MTU));
    // One byte over: dropped and counted once, never overrun.
    n = frame(f, 0x0040, p, BT_L2CAP_MTU + 1);
    acl(2, f, 1000);
    acl(1, f + 1000, 2);
    acl(1, f + 1002, n - 1002);
    CHECK_EQ(delivered, 1);
    CHECK_EQ(L.oversized, 1);
    CHECK_EQ(L.orphans, 0);
    // In one ACL packet.
    acl(2, f, n);
    CHECK_EQ(L.oversized, 2);
    // A huge length, never finished, then abandoned by a new start.
    uint8_t h[8] = { 0xff, 0xff, 0x40, 0x00, 1, 2, 3, 4 };
    acl(2, h, 8);
    acl(2, f, 4);
    CHECK_EQ(L.unfinished, 1);
    // Signalling over the MTU we take: refused under the first command's identifier.
    uint8_t c[1100];
    memset(c, 0, sizeof(c));
    c[0] = ECHO_REQ; c[1] = 0x33; c[2] = (uint8_t)(BT_L2CAP_MTU - 3); c[3] = (uint8_t)((BT_L2CAP_MTU - 3) >> 8);
    n = frame(f, 1, c, BT_L2CAP_MTU + 1);
    acl(2, f, 3);
    acl(1, f + 3, 600);
    acl(1, f + 603, n - 603);
    expect_cmd(REJ, 0x33, BYTES(W(1), MTU_OPT));
    expect_nothing();
    // At exactly the MTU it is taken (and the echo bounded).
    c[2] = (uint8_t)(BT_L2CAP_MTU - 4); c[3] = (uint8_t)((BT_L2CAP_MTU - 4) >> 8);
    n = frame(f, 1, c, BT_L2CAP_MTU);
    acl(2, f, n);
    expect_cmd(ECHO_RSP, 0x33, c + 4, BT_L2CAP_ECHO_BYTES);
}

static void test_cids(void)
{
    fresh();
    open_outgoing();
    peer_data(0x0041, BYTES(1));
    CHECK_EQ(L.closed_cid, 1);
    peer_data(0x0070, BYTES(1));            // the peer's own CID is not ours
    CHECK_EQ(L.closed_cid, 2);
    peer_data(0x0002, BYTES(1));
    peer_data(0x0003, BYTES(1));
    peer_data(0x003f, BYTES(1));
    peer_data(0x0000, BYTES(1));
    CHECK_EQ(L.fixed_cid, 4);
    CHECK_EQ(delivered, 0);
    peer_data(0x0040, NONE);
    CHECK_EQ(delivered, 1);
    CHECK_EQ(got_n, 0);
    // A deliver callback may be absent.
    bt_l2cap_receive(&L, (const uint8_t[]){ 0xbc, 0x2a, 5, 0, 1, 0, 0x40, 0, 9 }, 9, T, NULL, NULL);
    CHECK_EQ(delivered, 1);
}

static void test_queue(void)
{
    fresh();
    // Ten echoes in one frame: eight are queued, two dropped and counted.
    uint8_t c[64], f[80];
    size_t n = 0;
    for (int i = 0; i < 10; i++) { c[n++] = ECHO_REQ; c[n++] = (uint8_t)(i + 1); c[n++] = 1; c[n++] = 0; c[n++] = (uint8_t)i; }
    peer_pdu(f, frame(f, 1, c, n));
    CHECK_EQ(L.queue_full, 2);
    for (int i = 0; i < BT_L2CAP_SIGNALS; i++) expect_cmd(ECHO_RSP, (uint8_t)(i + 1), BYTES((uint8_t)i));
    expect_nothing();
    CHECK(!bt_l2cap_wants_send(&L));

    // A request dropped from a full queue starts its timer at once.
    fresh();
    n = 0;
    for (int i = 0; i < 8; i++) { c[n++] = ECHO_REQ; c[n++] = (uint8_t)(i + 1); c[n++] = 0; c[n++] = 0; }
    peer_pdu(f, frame(f, 1, c, n));
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    CHECK_EQ(L.queue_full, 1);
    T += 9999;
    tick(false);
    CHECK(!bt_l2cap_is_free(&L, 0));
    T += 1;
    tick(false);
    CHECK(bt_l2cap_is_free(&L, 0));

    // may_send false for many ticks, then true: signalling first, then data
    // oldest first, one PDU a tick.
    fresh();
    open_outgoing();
    bt_l2cap_admit(&L, BT_L2CAP_PSM_SDP, true);
    peer_cmd(CONN_REQ, 40, BYTES(W(1), W(0x0080)));
    expect_cmd(CONN_RSP, 40, BYTES(W(0x0041), W(0x0080), W(0), W(0)));
    expect_cmd(CONF_REQ, 3, BYTES(W(0x0080), W(0), 0x01, 2, MTU_OPT));
    peer_cmd(CONF_RSP, 3, BYTES(W(0x0041), W(0), W(0)));
    peer_cmd(CONF_REQ, 41, BYTES(W(0x0041), W(0)));
    expect_cmd(CONF_RSP, 41, BYTES(W(0x0080), W(0), W(0)));
    CHECK(bt_l2cap_is_open(&L, 1));
    CHECK(bt_l2cap_send(&L, 1, BYTES(0xb1)));
    CHECK(bt_l2cap_send(&L, 0, BYTES(0xa1)));
    peer_cmd(ECHO_REQ, 42, BYTES(1));
    peer_cmd(INFO_REQ, 43, BYTES(W(2)));
    CHECK(bt_l2cap_wants_send(&L));
    for (int i = 0; i < 300; i++) { T += 10; tick(false); }
    CHECK(bt_l2cap_is_open(&L, 0) && bt_l2cap_is_open(&L, 1));
    expect_cmd(ECHO_RSP, 42, BYTES(1));
    CHECK(bt_l2cap_send(&L, 1, BYTES(0xb2)) == false);     // its slot is still taken
    expect_cmd(INFO_RSP, 43, BYTES(W(2), W(0), 0, 0, 0, 0));
    expect_data(0x0080, BYTES(0xb1));
    CHECK(bt_l2cap_send(&L, 1, BYTES(0xb2)));
    expect_data(0x0070, BYTES(0xa1));
    expect_data(0x0080, BYTES(0xb2));
    expect_nothing();
    CHECK(!bt_l2cap_wants_send(&L));
    // A null send callback hands nothing over and loses nothing.
    peer_cmd(ECHO_REQ, 44, NONE);
    bt_l2cap_tick(&L, T, true, NULL, NULL);
    expect_cmd(ECHO_RSP, 44, NONE);
    // Send refusals.
    CHECK(!bt_l2cap_send(&L, 2, BYTES(1)));
    CHECK(!bt_l2cap_send(&L, 9, BYTES(1)));
    static uint8_t big[BT_L2CAP_MTU + 2];
    CHECK(!bt_l2cap_send(&L, 0, big, 673));                 // the peer said 672
    CHECK(bt_l2cap_send(&L, 0, big, 672));
}

static void reset_and_check(void)
{
    bt_l2cap_reset(&L);
    for (unsigned i = 0; i < BT_L2CAP_CHANNELS; i++) {
        CHECK(bt_l2cap_is_free(&L, i));
        CHECK(!bt_l2cap_is_open(&L, i));
        CHECK_EQ(bt_l2cap_peer_mtu(&L, i), 0);
    }
    CHECK(!bt_l2cap_wants_send(&L));
    CHECK(!L.up && !L.encrypted);
    expect_nothing();
    // Down: nothing is read and nothing opens.
    peer_cmd(ECHO_REQ, 1, NONE);
    expect_nothing();
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), -1);
    // Up again: a continuation from before is nothing begun.
    bt_l2cap_init(&L);
    acl(1, BYTES(1, 2, 3));
    CHECK_EQ(L.orphans, 1);
    CHECK_EQ(bt_l2cap_connect(&L, 1, T), 0);
    expect_cmd(CONN_REQ, 1, BYTES(W(0x0001), W(0x0040)));
}

static void test_reset_from_every_state(void)
{
    fresh(); CHECK(bt_l2cap_connect(&L, 1, T) == 0); reset_and_check();
    fresh(); CHECK(bt_l2cap_connect(&L, 1, T) == 0); tick(true); reset_and_check();
    fresh(); we_connect(); reset_and_check();
    fresh(); open_outgoing(); CHECK(bt_l2cap_send(&L, 0, BYTES(1))); bt_l2cap_set_encrypted(&L, true); reset_and_check();
    fresh(); open_incoming(); bt_l2cap_close(&L, 0, T); reset_and_check();
    fresh(); open_incoming(); bt_l2cap_close(&L, 0, T); tick(true); reset_and_check();
    fresh(); conf_setup(); peer_cmd(CONF_REQ, 5, BYTES(W(0x0040), W(1))); reset_and_check();
    fresh(); open_outgoing(); acl(2, BYTES(10, 0, 0x40, 0, 1, 2)); reset_and_check();
    fresh(); peer_cmd(ECHO_REQ, 1, NONE); reset_and_check();
    // Reset keeps the admitted list and the counts; init clears them.
    fresh();
    bt_l2cap_admit(&L, 1, true);
    acl(1, BYTES(1));
    bt_l2cap_reset(&L);
    CHECK_EQ(L.admitted[0], 1);
    CHECK_EQ(L.orphans, 1);
    bt_l2cap_init(&L);
    CHECK_EQ(L.admitted[0], 0);
    CHECK_EQ(L.orphans, 0);
    CHECK(L.up);
}

static void test_admit_list(void)
{
    fresh();
    bt_l2cap_admit(&L, 0x0001, true);
    bt_l2cap_admit(&L, 0x0001, true);
    bt_l2cap_admit(&L, 0x0019, true);
    bt_l2cap_admit(&L, 0x0017, true);
    bt_l2cap_admit(&L, 0x0003, true);
    bt_l2cap_admit(&L, 0x0005, true);       // no room
    int n = 0;
    for (int i = 0; i < BT_L2CAP_ADMITTED; i++) n += L.admitted[i] != 0;
    CHECK_EQ(n, 4);
    bt_l2cap_admit(&L, 0x0019, false);
    bt_l2cap_admit(&L, 0x0019, false);
    bt_l2cap_admit(&L, 0x0005, true);
    bt_l2cap_set_encrypted(&L, true);
    peer_cmd(CONN_REQ, 1, BYTES(W(0x0005), W(0x0050)));
    expect_cmd(CONN_RSP, 1, BYTES(W(0x0040), W(0x0050), W(0), W(0)));
    expect_cmd(CONF_REQ, 1, BYTES(W(0x0050), W(0), 0x01, 2, MTU_OPT));
    peer_cmd(CONN_REQ, 2, BYTES(W(0x0019), W(0x0051)));
    expect_cmd(CONN_RSP, 2, BYTES(W(0), W(0x0051), W(2), W(0)));
}

static void test_status(void)
{
    fresh();
    open_outgoing();
    char text[1024];
    size_t n = bt_l2cap_status(&L, text, sizeof(text));
    CHECK(n > 0 && n == strlen(text));
    CHECK(strstr(text, "channel 0: open psm 0001 cid 0040 peer cid 0070") != NULL);
    CHECK(strstr(text, "link up") != NULL);
    char small[16];
    n = bt_l2cap_status(&L, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small));
    CHECK_EQ(bt_l2cap_status(&L, small, 0), 0);
}

// ── Fuzz ────────────────────────────────────────────────────────────────────

static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

// The automatic peer: it reads what we send and, most of the time, answers
// as a real stack would; its own CIDs are 0x0100 up.
static uint16_t peer_next_cid = 0x0100;
static void auto_answer(void)
{
    if (sent_n < 8 || sent[2] != 1 || sent[3] != 0 || rnd() % 4 == 0) return;
    uint8_t code = sent[4], ident = sent[5];
    const uint8_t *d = sent + 8;
    if (code == CONN_REQ) {
        uint16_t scid = (uint16_t)(d[2] | d[3] << 8);
        uint16_t result = rnd() % 5 == 0 ? (uint16_t)(rnd() % 5) : 0;
        uint16_t dcid = result ? 0 : peer_next_cid++;
        peer_cmd(CONN_RSP, ident, BYTES(W(dcid), W(scid), W(result), W(0)));
        if (!result && rnd() % 2) peer_cmd(CONF_REQ, (uint8_t)(rnd() | 1), BYTES(W(scid), W(0), 0x01, 2, W(rnd() % 2000)));
    } else if (code == CONF_REQ) {
        uint16_t dcid = (uint16_t)(d[0] | d[1] << 8);
        for (unsigned i = 0; i < BT_L2CAP_CHANNELS; i++) if (L.ch[i].dcid == dcid && L.ch[i].state) {
            uint16_t result = rnd() % 6 == 0 ? 1 : 0;
            peer_cmd(CONF_RSP, ident, BYTES(W(L.ch[i].cid), W(0), W(result), 0x01, 2, W(rnd() % 1100)));
            if (rnd() % 2) peer_cmd(CONF_REQ, (uint8_t)(rnd() | 1), BYTES(W(L.ch[i].cid), W(rnd() % 2), 0x01, 2, W(rnd() % 2000)));
        }
    } else if (code == DISC_REQ) {
        peer_cmd(DISC_RSP, ident, d, 4);
    }
}

static void random_signal(uint8_t *c, size_t *n)
{
    uint8_t code = (uint8_t)(rnd() % 13);
    uint16_t cids[] = { 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0100, 0x0101, 0x0055, (uint16_t)rnd() };
    uint16_t a = cids[rnd() % 9], b = cids[rnd() % 9];
    size_t len = 0;
    uint8_t *d = c + 4;
    switch (code) {
    case CONN_REQ: d[0] = (uint8_t)(rnd() % 2 ? 1 : 0x19); d[1] = 0; d[2] = (uint8_t)a; d[3] = (uint8_t)(a >> 8); len = 4; break;
    case CONF_REQ: case CONF_RSP: case DISC_REQ: case DISC_RSP: case CONN_RSP:
        d[0] = (uint8_t)a; d[1] = (uint8_t)(a >> 8); d[2] = (uint8_t)b; d[3] = (uint8_t)(b >> 8);
        len = 4;
        if (code == CONN_RSP || code == CONF_RSP) { d[4] = (uint8_t)(rnd() % 3); d[5] = 0; d[6] = 0; d[7] = 0; len = code == CONN_RSP ? 8 : 6; }
        if (code == CONF_REQ || code == CONF_RSP) {
            d[2] = (uint8_t)(rnd() % 2); d[3] = 0;
            int opts = (int)(rnd() % 4);
            for (int o = 0; o < opts; o++) {
                uint8_t t = (uint8_t)(rnd() % 2 ? rnd() % 8 : rnd()), ol = (uint8_t)(rnd() % 3 ? (t == 1 || t == 2 ? 2 : t == 3 ? 22 : t == 4 ? 9 : 1) : rnd() % 30);
                d[len++] = t; d[len++] = ol;
                for (int k = 0; k < ol && len < 50; k++) d[len++] = (uint8_t)rnd();
            }
        }
        break;
    default: len = rnd() % 12; for (size_t k = 0; k < len; k++) d[k] = (uint8_t)rnd(); break;
    }
    if (rnd() % 8 == 0 && len) len -= rnd() % len;          // truncated
    c[0] = code; c[1] = (uint8_t)(rnd() % 6 ? rnd() % 12 : rnd()); c[2] = (uint8_t)len; c[3] = (uint8_t)(len >> 8);
    if (rnd() % 10 == 0) c[2] = (uint8_t)rnd();             // a length that lies
    *n = len + 4;
}

// How far the fuzz reached, so a fuzz that never opens a channel cannot pass
// by testing nothing.
static long fuzz_open, fuzz_delivered, fuzz_waiting;

static void check_invariants(void)
{
    for (unsigned i = 0; i < BT_L2CAP_CHANNELS; i++) {
        const bt_l2cap_channel_t *c = &L.ch[i];
        if (bt_l2cap_is_open(&L, i)) {
            fuzz_open++;
            if (!(c->ours_done && c->peer_done && c->state == BT_L2CAP_CONFIG)) { failures++; printf("FAIL open without both configs\n"); }
            if (bt_l2cap_peer_mtu(&L, i) < 48) { failures++; printf("FAIL an open channel's peer MTU %u\n", bt_l2cap_peer_mtu(&L, i)); }
        }
        if (c->state > BT_L2CAP_AWAITING_SECURITY) { failures++; printf("FAIL state %u\n", c->state); }
        if (c->state == BT_L2CAP_AWAITING_SECURITY) {
            fuzz_waiting++;
            if (!c->peer_ident || L.encrypted) { failures++; printf("FAIL a channel waiting with identifier %u, encrypted %d\n", c->peer_ident, L.encrypted); }
        }
        if (c->state != BT_L2CAP_FREE && c->cid < 0x40) { failures++; printf("FAIL cid %04x\n", c->cid); }
        if (c->our_mtu && (c->our_mtu < 48 || c->our_mtu > BT_L2CAP_MTU)) { failures++; printf("FAIL our mtu %u\n", c->our_mtu); }
        for (unsigned k = i + 1; k < BT_L2CAP_CHANNELS; k++)
            if (c->state && L.ch[k].state && c->cid == L.ch[k].cid) { failures++; printf("FAIL two channels with cid %04x\n", c->cid); }
    }
    if (L.sig_count > BT_L2CAP_SIGNALS) { failures++; printf("FAIL queue %u\n", L.sig_count); }
    if (sent_n) {
        if (sent_n > BT_L2CAP_MTU + 4) { failures++; printf("FAIL a PDU of %zu\n", sent_n); }
        if (sent_n < 4 || (size_t)(sent[0] | sent[1] << 8) != sent_n - 4) { failures++; printf("FAIL a PDU whose length lies\n"); }
        uint16_t cid = (uint16_t)(sent[2] | sent[3] << 8);
        if (cid != 1 && cid < 0x40) { failures++; printf("FAIL sent on cid %04x\n", cid); }
        if (cid == 1 && (sent_n < 8 || sent[5] == 0 || (size_t)(sent[6] | sent[7] << 8) != sent_n - 8)) {
            if (!(sent_n >= 8 && sent[4] == REJ && sent[5] == 0)) { failures++; printf("FAIL a malformed command sent\n"); dump("sent", sent, sent_n); }
        }
    }
}

static void fuzz(int rounds)
{
    static uint8_t pkt[1400], pdu[2400];
    for (int round = 0; round < rounds; round++) {
        fresh();
        if (rnd() % 2) bt_l2cap_admit(&L, 1, true);
        if (rnd() % 2) bt_l2cap_admit(&L, 0x19, true);
        if (rnd() % 2) bt_l2cap_set_encrypted(&L, true);
        int steps = 20 + (int)(rnd() % 120);
        for (int s = 0; s < steps; s++) {
            T += rnd() % 4 == 0 ? rnd() % 70000 : rnd() % 50;
            switch (rnd() % 10) {
            case 0: case 1: {                   // a well-formed frame of random signalling, maybe several commands
                size_t n = 0, k;
                int cmds = 1 + (int)(rnd() % 3);
                for (int c = 0; c < cmds; c++) { random_signal(pdu + 4 + n, &k); n += k; }
                frame(pdu, 1, pdu + 4, n);
                size_t total = n + 4, at = 0;
                bool first = true;
                while (at < total) {               // in random pieces
                    size_t piece = rnd() % 3 ? total - at : 1 + rnd() % (total - at);
                    acl(first ? 2 : 1, pdu + at, piece);
                    at += piece; first = false;
                }
                break;
            }
            case 2: {                               // random bytes in a well-formed ACL packet
                size_t n = rnd() % 64;
                for (size_t k = 0; k < n; k++) pkt[k] = (uint8_t)rnd();
                if (n >= 4 && rnd() % 2) { pkt[2] = (uint8_t)(0x40 + rnd() % 6); pkt[3] = 0; }
                acl(rnd() % 4, pkt, n);
                break;
            }
            case 3: {                               // random raw ACL packet, header and all
                size_t n = rnd() % (rnd() % 8 ? 40 : 1300);
                for (size_t k = 0; k < n; k++) pkt[k] = (uint8_t)rnd();
                if (n >= 4 && rnd() % 2) { pkt[2] = (uint8_t)(n - 4); pkt[3] = (uint8_t)((n - 4) >> 8); }
                bt_l2cap_receive(&L, pkt, n, T, on_deliver, NULL);
                break;
            }
            case 4: {                               // data on a CID, maybe too long, maybe fragmented
                size_t n = rnd() % 3 ? rnd() % 100 : rnd() % 1200;
                memset(pdu + 4, 0x5a, n);
                size_t total = frame(pdu, (uint16_t)(0x40 + rnd() % 5), pdu + 4, n);
                size_t cut = rnd() % (total + 1);
                acl(2, pdu, cut);
                if (cut < total && rnd() % 8) acl(1, pdu + cut, total - cut);
                break;
            }
            case 5: bt_l2cap_connect(&L, rnd() % 3 ? 1 : (uint16_t)rnd(), T); break;
            case 6: bt_l2cap_close(&L, rnd() % 6, T); break;
            case 7: bt_l2cap_send(&L, rnd() % 6, pkt, rnd() % 3 ? rnd() % 100 : rnd() % 1100); break;
            case 8:
                if (rnd() % 20 == 0) { bt_l2cap_reset(&L); if (rnd() % 2) bt_l2cap_init(&L); }
                else if (rnd() % 5 == 0) bt_l2cap_set_encrypted(&L, rnd() % 2);
                else bt_l2cap_admit(&L, rnd() % 2 ? 0x19 : 1, rnd() % 2);
                break;
            default: break;
            }
            check_invariants();
            // A few ticks, the peer answering what it understands.
            int ticks = (int)(rnd() % 4);
            for (int t = 0; t < ticks; t++) {
                bool may = rnd() % 3 != 0;
                T += rnd() % 30;
                bool did = tick(may);
                check_invariants();
                if (did) auto_answer();
                check_invariants();
            }
        }
        char text[2048];
        size_t n = bt_l2cap_status(&L, text, sizeof(text));
        if (n != strlen(text)) { failures++; printf("FAIL status length\n"); }
        fuzz_delivered += delivered;
        if (failures > 20) return;
    }
    printf("fuzz: %ld open-channel observations, %ld deliveries, %ld waiting for security\n",
           fuzz_open, fuzz_delivered, fuzz_waiting);
    if (rounds >= 100 && (fuzz_open < rounds || fuzz_delivered < rounds / 10 || fuzz_waiting < rounds / 10)) {
        failures++; printf("FAIL the fuzz did not reach open channels\n");
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    test_we_open_sdp_ours_first();
    test_we_open_sdp_peers_first();
    test_peer_opens_sdp();
    test_peer_opens_avdtp();
    test_security_pending();
    test_config_deadline();
    test_late_success();
    test_held_payload();
    test_connection_refusals();
    test_outgoing_refused_and_pending();
    test_rtx();
    test_identifiers();
    test_config_options();
    test_config_continuation();
    test_config_rounds();
    test_signalling();
    test_disconnection();
    test_reassembly();
    test_oversized();
    test_cids();
    test_queue();
    test_reset_from_every_state();
    test_admit_list();
    test_status();
    fuzz(rounds);
    if (failures) { printf("bt_l2cap: %d failure(s)\n", failures); return 1; }
    printf("bt_l2cap: all scripted cases and %d fuzz rounds passed\n", rounds);
    return 0;
}
