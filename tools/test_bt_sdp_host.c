// test_bt_sdp_host.c — kernel/src/driver/system/usb/bt_sdp.c, both ends.
// The client is driven by a model headphone's SDP server that checks every
// request byte for byte and answers with a realistic Audio Sink record,
// whole and cut every way the continuation rules allow; the responder is
// asked everything a headphone might ask, and its answers are compared with
// copies of its three records written out here by hand, alone and across
// records. Then the client's parser reads the responder's records (the
// cross check), and both ends and the parser are fuzzed.
//
//   tools/test_bt_sdp_host.sh [fuzz rounds, default 3000]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_sdp.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

#define BE(x) (uint8_t)(((x) >> 8) & 0xff), (uint8_t)((x) & 0xff)
#define BYTES(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})

static void dump(const char *what, const uint8_t *p, size_t n)
{
    printf("  %s:", what);
    for (size_t i = 0; i < n && i < 96; i++) printf(" %02x", p[i]);
    printf("\n");
}

// ── Building data elements (big-endian, as the wire carries them) ──────────

typedef struct { uint8_t b[4096]; size_t n; size_t open[16]; uint8_t hdr[16]; unsigned depth; } bld_t;

static void put(bld_t *b, uint8_t v) { b->b[b->n++] = v; }
static void put16(bld_t *b, unsigned v) { put(b, (uint8_t)(v >> 8)); put(b, (uint8_t)v); }
static void put32(bld_t *b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xffff); }
// A sequence (0x30 DES) or alternative (0x38 DEA) whose length is written
// when it closes, in 1, 2 or 4 bytes (size index 5, 6 or 7).
static void open_(bld_t *b, uint8_t kind, unsigned length_bytes)
{
    uint8_t index = length_bytes == 1 ? 5 : length_bytes == 2 ? 6 : 7;
    put(b, (uint8_t)(kind | index));
    b->hdr[b->depth] = (uint8_t)length_bytes;
    b->open[b->depth++] = b->n;
    for (unsigned i = 0; i < length_bytes; i++) put(b, 0);
}
static void seq(bld_t *b) { open_(b, 0x30, 1); }
static void alt(bld_t *b) { open_(b, 0x38, 1); }
static void close_(bld_t *b)
{
    unsigned d = --b->depth, lb = b->hdr[d];
    size_t at = b->open[d], len = b->n - at - lb;
    if (lb < 4 && len >> (8 * lb)) { failures++; printf("FAIL the builder: %zu bytes in a %u-byte length\n", len, lb); }
    for (unsigned i = 0; i < lb; i++) b->b[at + i] = (uint8_t)(len >> (8 * (lb - 1 - i)));
}
static void u16(bld_t *b, unsigned v) { put(b, 0x09); put16(b, v); }
static void u32(bld_t *b, uint32_t v) { put(b, 0x0a); put32(b, v); }
static void uuid16(bld_t *b, unsigned v) { put(b, 0x19); put16(b, v); }
static void uuid32(bld_t *b, uint32_t v) { put(b, 0x1a); put32(b, v); }
static const uint8_t base_tail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
static void uuid128(bld_t *b, uint32_t first, bool on_base)
{
    put(b, 0x1c); put32(b, first);
    for (int i = 0; i < 12; i++) put(b, (uint8_t)(base_tail[i] ^ (on_base || i != 11 ? 0 : 1)));
}
static void text(bld_t *b, const char *s)
{
    size_t n = strlen(s);
    if (n < 256) { put(b, 0x25); put(b, (uint8_t)n); } else { put(b, 0x26); put16(b, (unsigned)n); }
    memcpy(b->b + b->n, s, n); b->n += n;
}

// ── The model headphone's record ────────────────────────────────────────────

typedef struct {
    bool extra_classes;       // 0x1203 and 0x111E beside the sink class
    int class_form;           // the sink class as UUID 16, 32, 128, or a near miss (-32, -128)
    bool no_sink;             // the class list holds Audio Source instead
    bool record_first;        // a non-matching record ahead of the sink's
    int pdl_wrap;             // extra alternatives around the protocol list's sequence
    size_t big_name;          // a service name this long, between the classes and the rest
    bool long_lengths;        // sequences with 16- and 32-bit lengths
    bool record_after;        // a non-matching record after the sink's
} shape_t;

// A record that is not an Audio Sink: an AVRCP target, whose protocol list
// and features would be misread as the sink's if records ran together.
static void other_record(bld_t *b)
{
    seq(b);
    u16(b, 0x0000); u32(b, 0x00010002);
    u16(b, 0x0001); seq(b); uuid16(b, 0x110C); close_(b);
    u16(b, 0x0004); seq(b); seq(b); uuid16(b, 0x0100); u16(b, 0x0017); close_(b);
    seq(b); uuid16(b, 0x0017); u16(b, 0x0104); close_(b); close_(b);
    u16(b, 0x0009); seq(b); seq(b); uuid16(b, 0x110D); u16(b, 0x0102); close_(b); close_(b);
    u16(b, 0x0311); u16(b, 0x00ff);
    close_(b);
}

static void build_answer(bld_t *b, const shape_t *s)
{
    memset(b, 0, sizeof(*b));
    if (s->long_lengths) open_(b, 0x30, 4); else open_(b, 0x30, 2);   // the attribute lists
    if (s->record_first) other_record(b);
    if (s->long_lengths || s->big_name) open_(b, 0x30, 2); else seq(b);   // the sink's record
    u16(b, 0x0000); u32(b, 0x00010003);
    u16(b, 0x0001); seq(b);
    if (s->no_sink) uuid16(b, 0x110A);
    else switch (s->class_form) {
    case 32: uuid32(b, 0x110B); break;
    case -32: uuid32(b, 0x0001110B); break;
    case 128: uuid128(b, 0x110B, true); break;
    case -128: uuid128(b, 0x110B, false); break;
    case -129: uuid128(b, 0x0001110B, true); break;
    default: uuid16(b, 0x110B); break;
    }
    if (s->extra_classes) { uuid16(b, 0x1203); uuid16(b, 0x111E); }
    close_(b);
    if (s->big_name) {
        static char name[3000];
        memset(name, 'h', s->big_name); name[s->big_name] = 0;
        u16(b, 0x0100); text(b, name);
    }
    u16(b, 0x0004);
    for (int i = 0; i < s->pdl_wrap; i++) alt(b);
    if (s->long_lengths) open_(b, 0x30, 4); else seq(b);
    seq(b); if (s->class_form == 128) uuid128(b, 0x0100, true); else uuid16(b, 0x0100); u16(b, 0x0019); close_(b);
    seq(b); if (s->class_form == 32) uuid32(b, 0x0019); else uuid16(b, 0x0019); u16(b, 0x0103); close_(b);
    close_(b);
    for (int i = 0; i < s->pdl_wrap; i++) close_(b);
    u16(b, 0x0005); seq(b); uuid16(b, 0x1002); close_(b);
    u16(b, 0x0009); seq(b); seq(b); uuid16(b, 0x110D); u16(b, 0x0103); close_(b); close_(b);
    if (!s->big_name) { u16(b, 0x0100); text(b, "Model Headphones"); }
    u16(b, 0x0200); u32(b, 0xdeadbeef);               // attributes nobody here reads
    u16(b, 0x0302); put(b, 0x28); put(b, 1);
    u16(b, 0x0303); put(b, 0x00);                     // nil: a descriptor with no data
    u16(b, 0x0304); seq(b); uuid16(b, 0x110B); close_(b);   // the sink's UUID outside the class list
    u16(b, 0x0311); u16(b, 0x0003);
    close_(b);
    if (s->record_after) other_record(b);
    close_(b);
}

static void expect_sink(const bt_sdp_sink_t *r, bool complete)
{
    CHECK(r->found);
    CHECK_EQ(r->complete, complete);
    CHECK_EQ(r->avdtp_version, 0x0103);
    CHECK_EQ(r->a2dp_version, 0x0103);
    CHECK_EQ(r->features, 0x0003);
    CHECK(r->features_present);
    CHECK_EQ(r->l2cap_psm, 0x0019);
}

// ── The model server for the client ─────────────────────────────────────────

static bt_sdp_client_t C;
static uint64_t T;

// The request we must see: PDU 0x06, the next transaction ID, the pattern,
// the maximum, the whole range, and the continuation state last given.
static uint16_t want_tid;
static uint8_t want_cont[17];
static void check_request(const uint8_t *q, size_t n, uint16_t max)
{
    uint8_t w[64];
    size_t k = 0;
    w[k++] = 0x06; w[k++] = (uint8_t)(want_tid >> 8); w[k++] = (uint8_t)want_tid;
    w[k++] = 0; w[k++] = (uint8_t)(15 + want_cont[0]);
    w[k++] = 0x35; w[k++] = 3; w[k++] = 0x19; w[k++] = 0x11; w[k++] = 0x0b;
    w[k++] = (uint8_t)(max >> 8); w[k++] = (uint8_t)max;
    w[k++] = 0x35; w[k++] = 5; w[k++] = 0x0a; w[k++] = 0; w[k++] = 0; w[k++] = 0xff; w[k++] = 0xff;
    memcpy(w + k, want_cont, 1 + want_cont[0]); k += 1 + want_cont[0];
    if (n != k || memcmp(q, w, k)) { failures++; printf("FAIL wrong request\n"); dump("want", w, k); dump("got ", q, n); }
}

// One response carrying answer[from, to) and a continuation state of
// `cont` bytes (0: the last part).
static size_t response(uint8_t *out, uint16_t tid, const uint8_t *answer, size_t from, size_t to, unsigned cont, unsigned salt)
{
    size_t count = to - from, k = 0;
    out[k++] = 0x07; out[k++] = (uint8_t)(tid >> 8); out[k++] = (uint8_t)tid;
    size_t plen = 2 + count + 1 + cont;
    out[k++] = (uint8_t)(plen >> 8); out[k++] = (uint8_t)plen;
    out[k++] = (uint8_t)(count >> 8); out[k++] = (uint8_t)count;
    memcpy(out + k, answer + from, count); k += count;
    out[k++] = (uint8_t)cont;
    for (unsigned i = 0; i < cont; i++) out[k++] = (uint8_t)(salt * 31 + i * 7 + from);
    return k;
}

// Ask, and answer from `answer` in parts ending at cuts[0..ncuts-1] (the
// last cut is the end), each with a continuation state of `cont` bytes.
static void serve(const uint8_t *answer, size_t n, const size_t *cuts, int ncuts, unsigned cont, uint16_t mtu)
{
    bt_sdp_client_start(&C, T, mtu);
    want_tid = 1; want_cont[0] = 0;
    uint16_t max = mtu > 31 ? (uint16_t)(mtu - 24) : 7;
    size_t from = 0;
    for (int i = 0; i < ncuts; i++) {
        uint8_t q[64], r[4200];
        size_t qn = bt_sdp_client_request(&C, q, sizeof(q));
        check_request(q, qn, max);
        CHECK_EQ(bt_sdp_client_request(&C, q, sizeof(q)), 0);     // one request at a time
        bool last = i == ncuts - 1;
        size_t rn = response(r, want_tid, answer, from, cuts[i], last ? 0 : cont, (unsigned)i);
        bt_sdp_client_receive(&C, r, rn);
        if (!last) {
            want_cont[0] = (uint8_t)cont;
            memcpy(want_cont + 1, r + rn - cont, cont);
            want_tid++;
        }
        from = cuts[i];
        if (bt_sdp_client_state(&C) != BT_SDP_ASKING) break;
    }
    (void)n;
}

static void test_client_whole_and_cut(void)
{
    bld_t a;
    shape_t s = { .extra_classes = true };
    build_answer(&a, &s);
    size_t cut[3] = { a.n };
    T = 1000;
    serve(a.b, a.n, cut, 1, 0, 672);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    expect_sink(bt_sdp_client_result(&C), true);
    // Two parts, cut at every byte, continuation states of 1..16 bytes.
    for (size_t k = 0; k <= a.n; k++) {
        cut[0] = k; cut[1] = a.n;
        serve(a.b, a.n, cut, 2, 1 + (unsigned)(k % 16), 672);
        if (bt_sdp_client_state(&C) != BT_SDP_DONE) { failures++; printf("FAIL two parts cut at %zu\n", k); continue; }
        expect_sink(bt_sdp_client_result(&C), true);
    }
    // Three parts, at every pair of cuts.
    for (size_t x = 0; x <= a.n; x++) for (size_t y = x; y <= a.n; y++) {
        cut[0] = x; cut[1] = y; cut[2] = a.n;
        serve(a.b, a.n, cut, 3, 1 + (unsigned)((x + y) % 16), 100);
        const bt_sdp_sink_t *r = bt_sdp_client_result(&C);
        if (bt_sdp_client_state(&C) != BT_SDP_DONE || !r->found || r->avdtp_version != 0x0103 || r->features != 3) {
            failures++; printf("FAIL three parts cut at %zu %zu\n", x, y);
        }
    }
    // The longer length forms, a record ahead of the sink's, and the
    // protocol list wrapped in alternatives up to the walker's depth.
    shape_t t = { .long_lengths = true, .record_first = true, .pdl_wrap = 1 };
    build_answer(&a, &t);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    expect_sink(bt_sdp_client_result(&C), true);
    // A record after the sink's does not displace it.
    shape_t u = { .record_after = true };
    build_answer(&a, &u);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    expect_sink(bt_sdp_client_result(&C), true);
    CHECK(C.malformed == 0);
    // Every prefix of an answer, read as cut by a bound: what is there is
    // read and the verdict is the cut, until the last byte.
    shape_t v = { .record_first = true, .pdl_wrap = 1 };
    build_answer(&a, &v);
    for (size_t k = 0; k <= a.n; k++) {
        uint8_t *exact = malloc(k ? k : 1);
        memcpy(exact, a.b, k);
        bt_sdp_sink_t out;
        bt_sdp_parse_t r = bt_sdp_parse_attribute_lists(exact, k, 0x110B, false, &out);
        if (r != (k == a.n ? BT_SDP_PARSED : BT_SDP_TRUNCATED)) { failures++; printf("FAIL prefix %zu: verdict %d\n", k, r); }
        if (k == a.n && (!out.found || out.features != 3)) { failures++; printf("FAIL prefix %zu read nothing\n", k); }
        free(exact);
    }
}

static void test_client_bounds(void)
{
    bld_t a;
    shape_t s = { 0 };
    build_answer(&a, &s);
    // Never finishing: sixteen responses of a byte each, then no more asking.
    T = 5000;
    size_t cuts[20];
    for (int i = 0; i < 20; i++) cuts[i] = (size_t)i + 1;
    serve(a.b, a.n, cuts, 20, 4, 672);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK_EQ(C.rounds, BT_SDP_ROUNDS);
    CHECK_EQ(C.held, BT_SDP_ROUNDS);
    CHECK(!bt_sdp_client_result(&C)->complete);
    CHECK(!bt_sdp_client_result(&C)->found);
    uint8_t q[64];
    CHECK_EQ(bt_sdp_client_request(&C, q, sizeof(q)), 0);
    // Fifteen with a continuation and a sixteenth without: complete.
    size_t c16[16];
    for (int i = 0; i < 15; i++) c16[i] = (size_t)i * 2;
    c16[15] = a.n;
    serve(a.b, a.n, c16, 16, 3, 672);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    expect_sink(bt_sdp_client_result(&C), true);

    // Too large: the sink's class comes before a long name, the features after it.
    shape_t big = { .big_name = 1100 };
    build_answer(&a, &big);
    CHECK(a.n > BT_SDP_BUFFER);
    size_t c2[2] = { 600, a.n };
    serve(a.b, a.n, c2, 2, 2, 672);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    const bt_sdp_sink_t *r = bt_sdp_client_result(&C);
    CHECK(r->found);
    CHECK(!r->complete);
    CHECK(!r->features_present);
    CHECK_EQ(r->avdtp_version, 0);
    CHECK_EQ(C.held, BT_SDP_BUFFER);
    // Exactly the buffer is not too large.
    shape_t fit = { .big_name = 1 };
    build_answer(&a, &fit);
    size_t pad = BT_SDP_BUFFER - a.n + 1;
    fit.big_name = pad;
    build_answer(&a, &fit);
    if (a.n != BT_SDP_BUFFER) { fit.big_name = pad - (a.n - BT_SDP_BUFFER); build_answer(&a, &fit); }
    CHECK_EQ(a.n, BT_SDP_BUFFER);
    c2[0] = 500; c2[1] = a.n;
    serve(a.b, a.n, c2, 2, 2, 672);
    expect_sink(bt_sdp_client_result(&C), true);

    // Too slow: one part, then silence.
    build_answer(&a, &s);
    T = 10000;
    bt_sdp_client_start(&C, T, 672);
    uint8_t r1[256];
    size_t qn = bt_sdp_client_request(&C, q, sizeof(q));
    CHECK(qn > 0);
    size_t rn = response(r1, 1, a.b, 0, 60, 2, 0);
    bt_sdp_client_receive(&C, r1, rn);
    bt_sdp_client_tick(&C, T + 4999);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_ASKING);
    bt_sdp_client_tick(&C, T + 5000);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK(!bt_sdp_client_result(&C)->complete);
    CHECK(bt_sdp_client_result(&C)->found);           // the class list is in the first 60 bytes
    // And silence from the start.
    bt_sdp_client_start(&C, T, 672);
    bt_sdp_client_tick(&C, T + 5000);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK(!bt_sdp_client_result(&C)->found);
    // A finished query stays finished.
    bt_sdp_client_receive(&C, r1, rn);
    bt_sdp_client_tick(&C, T + 9000);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
}

static void start_and_ask(void)
{
    uint8_t q[64];
    bt_sdp_client_start(&C, T, 672);
    CHECK(bt_sdp_client_request(&C, q, sizeof(q)) > 0);
}

static void test_client_failures(void)
{
    bld_t a;
    shape_t s = { 0 };
    build_answer(&a, &s);
    uint8_t r[512];
    size_t rn;
    // An ErrorResponse.
    start_and_ask();
    bt_sdp_client_receive(&C, BYTES(0x01, BE(1), BE(2), BE(0x0003)));
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    CHECK_EQ(C.error, 0x0003);
    CHECK_EQ(C.malformed, 0);
    // Another transaction.
    start_and_ask();
    rn = response(r, 2, a.b, 0, a.n, 0, 0);
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    CHECK(!bt_sdp_client_result(&C)->found);
    rn = response(r, 0x0100, a.b, 0, a.n, 0, 0);       // the ID read big-endian
    start_and_ask();
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    // A response before any request, even one naming the transaction
    // number nothing has used yet.
    bt_sdp_client_start(&C, T, 672);
    rn = response(r, 0, a.b, 0, a.n, 0, 0);
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    // A header whose length disagrees with a body that agrees with itself.
    start_and_ask();
    rn = response(r, 1, a.b, 0, a.n, 0, 0);
    r[4]++;
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    CHECK_EQ(C.malformed, 1);
    // A byte after the continuation state, the length made to agree.
    start_and_ask();
    rn = response(r, 1, a.b, 0, a.n, 0, 0);
    r[rn++] = 0x00; r[4]++;
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    CHECK_EQ(C.malformed, 1);
    // A response of another kind.
    start_and_ask();
    rn = response(r, 1, a.b, 0, a.n, 0, 0);
    r[0] = 0x05;
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    CHECK_EQ(C.malformed, 1);
    // Every truncation of a valid response, with its length left alone and
    // with the length made to agree.
    rn = response(r, 1, a.b, 0, a.n, 0, 0);
    for (size_t k = 0; k < rn; k++) {
        start_and_ask();
        bt_sdp_client_receive(&C, r, k);
        if (bt_sdp_client_state(&C) != BT_SDP_FAILED || C.malformed != 1) { failures++; printf("FAIL truncated to %zu\n", k); }
        if (k < 5) continue;
        uint8_t t[512];
        memcpy(t, r, k); t[3] = (uint8_t)((k - 5) >> 8); t[4] = (uint8_t)(k - 5);
        start_and_ask();
        bt_sdp_client_receive(&C, t, k);
        if (bt_sdp_client_state(&C) != BT_SDP_FAILED || C.malformed != 1) { failures++; printf("FAIL cut to %zu with its length\n", k); }
    }
    // A continuation state of 17 bytes.
    start_and_ask();
    rn = response(r, 1, a.b, 0, 10, 17, 0);
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    // A byte count one more than the bytes, and one less.
    for (int d = -1; d <= 1; d += 2) {
        start_and_ask();
        rn = response(r, 1, a.b, 0, a.n, 0, 0);
        r[6] = (uint8_t)(r[6] + d);
        bt_sdp_client_receive(&C, r, rn);
        CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_FAILED);
    }
    // The whole answer, malformed in each way the walker refuses.
    // Each bad element is an attribute's VALUE, where any well-formed
    // element is allowed: the descriptor alone makes it malformed. Each
    // would parse if its rule were relaxed by one step.
    static const uint8_t bad[][28] = {
        { 9, 0x35, 0x07, 0x35, 0x05, 0x09, 0x00, 0x01, 0x48, 0x00 },  // a reserved type (9)
        { 14, 0x35, 0x0c, 0x35, 0x0a, 0x09, 0x00, 0x01, 0x01, 0x09, 0x00, 0x02, 0x09, 0x00, 0x05 },  // nil with a size
        { 10, 0x35, 0x08, 0x35, 0x06, 0x09, 0x00, 0x01, 0x29, 0x00, 0x01 },  // a boolean of 2 bytes
        { 16, 0x35, 0x0e, 0x35, 0x0c, 0x09, 0x00, 0x01, 0x1b, 0, 0, 0, 0, 0, 0, 0, 0 },  // a UUID of 8 bytes
        { 24, 0x35, 0x16, 0x35, 0x14, 0x09, 0x00, 0x01, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },  // a string of fixed size
        { 10, 0x35, 0x08, 0x35, 0x06, 0x09, 0x00, 0x01, 0x0d, 0x01, 0x00 },  // a uint with a length
        { 7, 0x35, 0x05, 0x35, 0x02, 0x09, 0x00, 0x01 },       // an element past its record
        { 8, 0x35, 0x06, 0x35, 0x04, 0x08, 0x01, 0x08, 0x02 }, // an attribute ID not a uint16
        { 5, 0x35, 0x03, 0x09, 0x00, 0x01 },                   // a record not a sequence
        { 4, 0x35, 0x00, 0x35, 0x00 },                         // bytes after the list
        { 9, 0x3d, 0x07, 0x35, 0x05, 0x09, 0x00, 0x01, 0x08, 0x00 },  // a list that is an alternative
        { 3, 0x35, 0x05, 0x35 },                               // a list past the bytes
        { 0 },                                                 // nothing at all
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        start_and_ask();
        rn = response(r, 1, bad[i] + 1, 0, bad[i][0], 0, 0);
        bt_sdp_client_receive(&C, r, rn);
        if (bt_sdp_client_state(&C) != BT_SDP_FAILED || C.malformed != 1) { failures++; printf("FAIL malformed answer %zu\n", i); }
    }
    // The empty list is a well-formed answer with no sink in it.
    start_and_ask();
    rn = response(r, 1, (const uint8_t[]){ 0x35, 0x00 }, 0, 2, 0, 0);
    bt_sdp_client_receive(&C, r, rn);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK(!bt_sdp_client_result(&C)->found);
    CHECK(bt_sdp_client_result(&C)->complete);
}

static void test_client_uuids_and_depth(void)
{
    bld_t a;
    size_t cut[1];
    static const int forms[] = { 32, 128 };
    for (int i = 0; i < 2; i++) {
        shape_t s = { .class_form = forms[i] };
        build_answer(&a, &s);
        cut[0] = a.n;
        serve(a.b, a.n, cut, 1, 0, 672);
        expect_sink(bt_sdp_client_result(&C), true);
    }
    // Near misses are other UUIDs.
    static const int near[] = { -32, -128, -129 };
    for (int i = 0; i < 3; i++) {
        shape_t s = { .class_form = near[i] };
        build_answer(&a, &s);
        cut[0] = a.n;
        serve(a.b, a.n, cut, 1, 0, 672);
        CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
        CHECK(!bt_sdp_client_result(&C)->found);
    }
    // No Audio Sink.
    shape_t none = { .no_sink = true };
    build_answer(&a, &none);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK(!bt_sdp_client_result(&C)->found);
    CHECK_EQ(bt_sdp_client_result(&C)->avdtp_version, 0);
    // Depth: the list, the record, the alternatives, the protocol list and
    // a descriptor are six sequences with two alternatives: parsed.
    shape_t six = { .pdl_wrap = 2 };
    build_answer(&a, &six);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    expect_sink(bt_sdp_client_result(&C), true);
    // With three, the descriptors are a seventh: stepped over, nothing else lost.
    shape_t seven = { .pdl_wrap = 3 };
    build_answer(&a, &seven);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    const bt_sdp_sink_t *r = bt_sdp_client_result(&C);
    CHECK_EQ(bt_sdp_client_state(&C), BT_SDP_DONE);
    CHECK(r->found && r->complete);
    CHECK_EQ(r->avdtp_version, 0);
    CHECK_EQ(r->l2cap_psm, 0);
    CHECK_EQ(r->a2dp_version, 0x0103);
    CHECK_EQ(r->features, 3);
    // Deeper still, and the cut inside the stepped-over part.
    shape_t deep = { .pdl_wrap = 9 };
    build_answer(&a, &deep);
    cut[0] = a.n;
    serve(a.b, a.n, cut, 1, 0, 672);
    CHECK(bt_sdp_client_result(&C)->found);
    bt_sdp_sink_t out;
    CHECK_EQ(bt_sdp_parse_attribute_lists(a.b, 40, 0x110B, false, &out), BT_SDP_TRUNCATED);
    CHECK(out.found);
    CHECK_EQ(bt_sdp_parse_attribute_lists(a.b, 40, 0x110B, true, &out), BT_SDP_MALFORMED);
    CHECK(!out.found);
    // Malformed is not a cut, even in a prefix: an element past its record,
    // and a header that crosses its record's end, with bytes beyond both.
    // What was read before is not kept.
    static const uint8_t past[] = { 0x35, 0x0e, 0x35, 0x0a, 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0b,
                                    0x09, 0x03, 0x09, 0x00, 0x00 };
    CHECK_EQ(bt_sdp_parse_attribute_lists(past, sizeof(past), 0x110B, false, &out), BT_SDP_MALFORMED);
    CHECK(!out.found);
    static const uint8_t crossing[] = { 0x35, 0x10, 0x35, 0x0c, 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0b,
                                        0x09, 0x03, 0x11, 0x36, 0x00, 0x05 };
    CHECK_EQ(bt_sdp_parse_attribute_lists(crossing, sizeof(crossing), 0x110B, false, &out), BT_SDP_MALFORMED);
    CHECK(!out.found);
    // A whole answer whose list claims more than the bytes is malformed.
    static const uint8_t claims[] = { 0x35, 0x0a, 0x35, 0x06, 0x09, 0x03, 0x11, 0x09, 0x00 };
    CHECK_EQ(bt_sdp_parse_attribute_lists(claims, sizeof(claims), 0x110B, true, &out), BT_SDP_MALFORMED);
    CHECK_EQ(bt_sdp_parse_attribute_lists(claims, sizeof(claims), 0x110B, false, &out), BT_SDP_TRUNCATED);
}

static void test_client_request(void)
{
    uint8_t q[64];
    bt_sdp_client_start(&C, 0, 672);
    CHECK_EQ(C.max_bytes, 648);
    CHECK_EQ(bt_sdp_client_request(&C, q, 19), 0);      // too small for it
    CHECK_EQ(bt_sdp_client_request(&C, q, 20), 20);
    bt_sdp_client_start(&C, 0, 48);
    CHECK_EQ(C.max_bytes, 24);
    bt_sdp_client_start(&C, 0, 31);
    CHECK_EQ(C.max_bytes, 7);
    bt_sdp_client_start(&C, 0, 30);
    CHECK_EQ(C.max_bytes, 7);
    bt_sdp_client_start(&C, 0, 32);
    CHECK_EQ(C.max_bytes, 8);
    bt_sdp_client_start(&C, 0, 0);
    CHECK_EQ(C.max_bytes, 7);
    bt_sdp_client_t idle;
    memset(&idle, 0, sizeof(idle));
    CHECK_EQ(bt_sdp_client_state(&idle), BT_SDP_IDLE);
    CHECK_EQ(bt_sdp_client_request(&idle, q, sizeof(q)), 0);
    char line[128];
    bt_sdp_client_status(&idle, line, sizeof(line));
    CHECK(strstr(line, "not asked") != NULL);
    // The status line once answered.
    bld_t a;
    shape_t s = { 0 };
    build_answer(&a, &s);
    size_t cut[1] = { a.n };
    serve(a.b, a.n, cut, 1, 0, 672);
    size_t n = bt_sdp_client_status(&C, line, sizeof(line));
    CHECK(n == strlen(line));
    CHECK(strstr(line, "sdp: audio sink, avdtp 1.3, a2dp 1.3, features 0x0003") != NULL);
    CHECK(strstr(line, "(complete)") != NULL);
    char small[8];
    CHECK(bt_sdp_client_status(&C, small, sizeof(small)) == 7);
    CHECK_EQ(bt_sdp_client_status(&C, small, 0), 0);
}

// ── The responder ───────────────────────────────────────────────────────────

// os64's record as § S3 gives it, written out here by hand: the attribute
// ID (a uint16 element) and its value, in ascending ID order.
static const uint8_t rec_handle[] = { 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x01 };
static const uint8_t rec_classes[] = { 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0a };
static const uint8_t rec_protocols[] = { 0x09, 0x00, 0x04, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
                                         0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03 };
static const uint8_t rec_browse[] = { 0x09, 0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02 };
static const uint8_t rec_profiles[] = { 0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03 };
static const uint8_t rec_features[] = { 0x09, 0x03, 0x11, 0x09, 0x00, 0x01 };
static const struct { const uint8_t *b; size_t n; uint16_t id; } rec[] = {
    { rec_handle, sizeof(rec_handle), 0x0000 }, { rec_classes, sizeof(rec_classes), 0x0001 },
    { rec_protocols, sizeof(rec_protocols), 0x0004 }, { rec_browse, sizeof(rec_browse), 0x0005 },
    { rec_profiles, sizeof(rec_profiles), 0x0009 }, { rec_features, sizeof(rec_features), 0x0311 },
};

// The attribute list for the IDs in [lo, hi], as a sequence.
static size_t expected_list(uint8_t *out, uint16_t lo, uint16_t hi)
{
    size_t n = 2;
    for (int i = 0; i < 6; i++) if (rec[i].id >= lo && rec[i].id <= hi) { memcpy(out + n, rec[i].b, rec[i].n); n += rec[i].n; }
    out[0] = 0x35; out[1] = (uint8_t)(n - 2);
    return n;
}

// Requests. Each is the PDU header, then the parameters given.
static size_t request(uint8_t *out, uint8_t pdu, uint16_t tid, const uint8_t *params, size_t n)
{
    out[0] = pdu; out[1] = (uint8_t)(tid >> 8); out[2] = (uint8_t)tid;
    out[3] = (uint8_t)(n >> 8); out[4] = (uint8_t)n;
    memcpy(out + 5, params, n);
    return n + 5;
}

static uint8_t resp[2048];
static size_t resp_n;
static void ask(uint8_t pdu, uint16_t tid, const uint8_t *params, size_t n)
{
    uint8_t q[512];
    resp_n = bt_sdp_respond(q, request(q, pdu, tid, params, n), resp, sizeof(resp));
}
static void expect_error_(int line, uint16_t tid, uint16_t code)
{
    const uint8_t want[] = { 0x01, BE(tid), 0x00, 0x02, BE(code) };
    if (resp_n != 7 || memcmp(resp, want, 7)) {
        failures++; printf("FAIL line %d: want error %04x\n", line, code); dump("got", resp, resp_n);
    }
}
#define expect_error(tid, code) expect_error_(__LINE__, tid, code)

static void test_responder_search(void)
{
    // Patterns that match, in each UUID size and in combination.
    ask(0x02, 0x1234, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(5), 0x00));
    uint8_t want[] = { 0x03, 0x12, 0x34, 0x00, 0x09, BE(1), BE(1), 0x00, 0x01, 0x00, 0x01, 0x00 };
    CHECK(resp_n == sizeof(want) && !memcmp(resp, want, sizeof(want)));
    ask(0x02, 1, BYTES(0x35, 0x0e, 0x19, 0x01, 0x00, 0x19, 0x00, 0x19, 0x1a, 0x00, 0x00, 0x10, 0x02, 0x19, 0x11, 0x0d, BE(1), 0x00));
    CHECK(resp_n == 14 && resp[6] == 1 && resp[8] == 1);
    bld_t b = { 0 };
    seq(&b); uuid128(&b, 0x1002, true); close_(&b);
    uint8_t p[64];
    memcpy(p, b.b, b.n); p[b.n] = 0; p[b.n + 1] = 1; p[b.n + 2] = 0;
    ask(0x02, 2, p, b.n + 3);
    CHECK(resp_n == 14 && resp[6] == 1 && resp[8] == 1);
    // Patterns that do not: another class, one of two missing, a UUID off the base.
    ask(0x02, 3, BYTES(0x35, 0x03, 0x19, 0x11, 0x0b, BE(5), 0x00));
    uint8_t none[] = { 0x03, 0x00, 0x03, 0x00, 0x05, BE(0), BE(0), 0x00 };
    CHECK(resp_n == sizeof(none) && !memcmp(resp, none, sizeof(none)));
    ask(0x02, 4, BYTES(0x35, 0x06, 0x19, 0x11, 0x0a, 0x19, 0x11, 0x0b, BE(5), 0x00));
    CHECK(resp_n == 10 && resp[6] == 0 && resp[8] == 0);
    memset(&b, 0, sizeof(b));
    seq(&b); uuid128(&b, 0x1002, false); close_(&b);
    memcpy(p, b.b, b.n); p[b.n] = 0; p[b.n + 1] = 1; p[b.n + 2] = 0;
    ask(0x02, 5, p, b.n + 3);
    CHECK(resp_n == 10 && resp[6] == 0 && resp[8] == 0);
    ask(0x02, 6, BYTES(0x35, 0x05, 0x1a, 0x00, 0x01, 0x11, 0x0a, BE(5), 0x00));   // 32 bits, top half not zero
    CHECK(resp_n == 10 && resp[6] == 0 && resp[8] == 0);
    // Twelve UUIDs are a pattern; thirteen are not.
    uint8_t twelve[64];
    size_t k = 0;
    twelve[k++] = 0x35; twelve[k++] = 36;
    for (int i = 0; i < 12; i++) { twelve[k++] = 0x19; twelve[k++] = 0x11; twelve[k++] = 0x0a; }
    twelve[k++] = 0; twelve[k++] = 1; twelve[k++] = 0;
    ask(0x02, 7, twelve, k);
    CHECK(resp_n == 14 && resp[6] == 1 && resp[8] == 1);
    k = 0;
    twelve[k++] = 0x35; twelve[k++] = 39;
    for (int i = 0; i < 13; i++) { twelve[k++] = 0x19; twelve[k++] = 0x11; twelve[k++] = 0x0a; }
    twelve[k++] = 0; twelve[k++] = 1; twelve[k++] = 0;
    ask(0x02, 8, twelve, k);
    expect_error(8, 0x0003);
    // A maximum of zero, an empty pattern, a pattern of something else.
    ask(0x02, 9, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(0), 0x00));
    expect_error(9, 0x0003);
    ask(0x02, 10, BYTES(0x35, 0x00, BE(1), 0x00));
    expect_error(10, 0x0003);
    ask(0x02, 11, BYTES(0x35, 0x03, 0x09, 0x11, 0x0a, BE(1), 0x00));
    expect_error(11, 0x0003);
    ask(0x02, 12, BYTES(0x3d, 0x03, 0x19, 0x11, 0x0a, BE(1), 0x00));
    expect_error(12, 0x0003);
    // A continuation state on a search, which never needs one.
    ask(0x02, 13, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(1), 0x02, 0x00, 0x01));
    expect_error(13, 0x0005);
    // The response must fit what we were given.
    uint8_t q[64], small[16];
    size_t qn = request(q, 0x02, 14, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(1), 0x00));
    CHECK_EQ(bt_sdp_respond(q, qn, small, 13), 0);
    CHECK_EQ(bt_sdp_respond(q, qn, small, 14), 14);
}

static void test_responder_attributes(void)
{
    uint8_t want[256];
    size_t wn;
    // ServiceAttributeRequest, the whole range.
    ask(0x04, 0x0102, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    wn = expected_list(want, 0x0000, 0xffff);
    CHECK_EQ(resp[0], 0x05);
    CHECK(resp[1] == 0x01 && resp[2] == 0x02);
    CHECK_EQ((size_t)(resp[3] << 8 | resp[4]), resp_n - 5);
    CHECK_EQ((size_t)(resp[5] << 8 | resp[6]), wn);
    CHECK(resp_n == 7 + wn + 1 && !memcmp(resp + 7, want, wn) && resp[resp_n - 1] == 0);
    // Single IDs, out of order, and a range: the attributes come back ascending.
    ask(0x04, 1, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x0b, 0x09, 0x03, 0x11, 0x0a, 0x00, 0x04, 0x00, 0x09, 0x09, 0x00, 0x01, 0x00));
    size_t n = 2;
    memcpy(want + n, rec_classes, sizeof(rec_classes)); n += sizeof(rec_classes);
    memcpy(want + n, rec_protocols, sizeof(rec_protocols)); n += sizeof(rec_protocols);
    memcpy(want + n, rec_browse, sizeof(rec_browse)); n += sizeof(rec_browse);
    memcpy(want + n, rec_profiles, sizeof(rec_profiles)); n += sizeof(rec_profiles);
    memcpy(want + n, rec_features, sizeof(rec_features)); n += sizeof(rec_features);
    want[0] = 0x35; want[1] = (uint8_t)(n - 2);
    CHECK(resp_n == 7 + n + 1 && !memcmp(resp + 7, want, n));
    // IDs the record does not have: an empty list.
    ask(0x04, 2, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x03, 0x09, 0x00, 0x02, 0x00));
    CHECK(resp_n == 10 && resp[6] == 2 && resp[7] == 0x35 && resp[8] == 0x00);
    // The two ends of a range are both in it.
    ask(0x04, 3, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x05, 0x0a, 0x00, 0x05, 0x03, 0x11, 0x00));
    wn = expected_list(want, 0x0005, 0x0311);
    CHECK(resp_n == 7 + wn + 1 && !memcmp(resp + 7, want, wn));
    // A handle that is none of ours (0x00010002 and 0x00010003 are the
    // AVRCP records: test_responder_records), and the errors of syntax.
    ask(0x04, 4, BYTES(0x00, 0x01, 0x00, 0x04, BE(500), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    expect_error(4, 0x0002);
    ask(0x04, 5, BYTES(0x00, 0x01, 0x00, 0x01, BE(6), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    expect_error(5, 0x0003);
    ask(0x04, 6, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x00, 0x00));
    expect_error(6, 0x0003);
    ask(0x04, 7, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x02, 0x08, 0x01, 0x00));
    expect_error(7, 0x0003);
    // A one-byte ID followed by bytes that would read as a range if it were taken.
    ask(0x04, 16, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x05, 0x08, 0x00, 0x09, 0x03, 0x11, 0x00));
    expect_error(16, 0x0003);
    ask(0x04, 8, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x05, 0x0a, 0x00, 0x09, 0x00, 0x04, 0x00));
    expect_error(8, 0x0003);
    ask(0x04, 9, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x09, 0x0b, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0x00));
    expect_error(9, 0x0003);
    ask(0x04, 10, BYTES(0x00, 0x01, 0x00));
    expect_error(10, 0x0003);
    ask(0x04, 15, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x03, 0x11, 0x00, 0x01, 0x00));   // a signed ID
    expect_error(15, 0x0003);
    ask(0x04, 11, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x03, 0x09, 0x00));
    expect_error(11, 0x0003);
    // ServiceSearchAttributeRequest: matched, the list of one record's list.
    ask(0x06, 0x0a0b, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(500), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    wn = expected_list(want + 2, 0x0000, 0xffff);
    want[0] = 0x35; want[1] = (uint8_t)wn;
    CHECK_EQ(resp[0], 0x07);
    CHECK(resp_n == 7 + wn + 2 + 1 && !memcmp(resp + 7, want, wn + 2));
    // Not matched: an empty list of lists.
    ask(0x06, 12, BYTES(0x35, 0x03, 0x19, 0x11, 0x0b, BE(500), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    uint8_t empty[] = { 0x07, 0x00, 0x0c, 0x00, 0x05, 0x00, 0x02, 0x35, 0x00, 0x00 };
    CHECK(resp_n == sizeof(empty) && !memcmp(resp, empty, sizeof(empty)));
    ask(0x06, 13, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(7), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    CHECK(resp_n > 0 && resp[0] == 0x07);
    ask(0x06, 14, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(6), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    expect_error(14, 0x0003);
}

// Ask with a small maximum and follow the continuation to the end; the
// parts joined must be the answer asked for whole.
static size_t follow(uint8_t pdu, const uint8_t *head, size_t head_n, uint16_t max, size_t cap, uint8_t *joined)
{
    uint8_t q[256], r[2048], cont[17] = { 0 };
    size_t total = 0;
    for (int round = 0; round < 400; round++) {      // a byte a part, across all three records
        uint8_t params[128];
        size_t k = 0;
        memcpy(params, head, head_n); k = head_n;
        // The maximum sits before the attribute list, after the pattern or handle.
        size_t list_at = pdu == 0x04 ? 4 : (size_t)head[1] + 2;
        memmove(params + list_at + 2, params + list_at, head_n - list_at);
        params[list_at] = (uint8_t)(max >> 8); params[list_at + 1] = (uint8_t)max;
        k += 2;
        memcpy(params + k, cont, 1 + cont[0]); k += 1 + cont[0];
        size_t rn = bt_sdp_respond(q, request(q, pdu, (uint16_t)round, params, k), r, cap);
        if (rn < 8 || r[0] != pdu + 1) { failures++; printf("FAIL follow: no answer at max %u cap %zu\n", max, cap); dump("got", r, rn); return 0; }
        size_t count = (size_t)(r[5] << 8 | r[6]);
        if (count > max || rn > cap || count + 8 + r[7 + count] != rn) { failures++; printf("FAIL follow: a part over its bounds\n"); return 0; }
        memcpy(joined + total, r + 7, count); total += count;
        uint8_t c = r[7 + count];
        if (!c) return total;
        if (c > 16) { failures++; printf("FAIL follow: a state of %u\n", c); return 0; }
        cont[0] = c; memcpy(cont + 1, r + 8 + count, c);
    }
    failures++; printf("FAIL follow: never ended\n");
    return 0;
}

static void test_responder_continuation(void)
{
    static const uint8_t ssa[] = { 0x35, 0x03, 0x19, 0x11, 0x0a, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    static const uint8_t sa[] = { 0x00, 0x01, 0x00, 0x01, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    uint8_t whole[256], joined[256];
    size_t wn = follow(0x06, ssa, sizeof(ssa), 1000, 2048, whole);
    CHECK(wn > 0);
    for (uint16_t max = 7; max <= wn; max++) {
        size_t jn = follow(0x06, ssa, sizeof(ssa), max, 2048, joined);
        if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL ssa joined at max %u\n", max); }
    }
    // Bounded by the capacity instead of the maximum.
    for (size_t cap = 15; cap < wn + 20; cap++) {
        size_t jn = follow(0x06, ssa, sizeof(ssa), 1000, cap, joined);
        if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL ssa joined at capacity %zu\n", cap); }
    }
    uint8_t q[64], r[64];
    size_t qn = request(q, 0x06, 1, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(1000), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    CHECK_EQ(bt_sdp_respond(q, qn, r, 14), 0);
    CHECK(bt_sdp_respond(q, qn, r, 15) > 0);
    size_t an = follow(0x04, sa, sizeof(sa), 1000, 2048, whole);
    CHECK(an + 2 == wn);
    for (uint16_t max = 7; max <= an; max++) {
        size_t jn = follow(0x04, sa, sizeof(sa), max, 2048, joined);
        if (jn != an || memcmp(joined, whole, an)) { failures++; printf("FAIL sa joined at max %u\n", max); }
    }

    // A state taken from a real answer...
    ask(0x06, 1, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(10), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    CHECK(resp_n == 7 + 10 + 7);
    uint8_t state[7];
    memcpy(state, resp + 17, 7);
    CHECK_EQ(state[0], 6);
    uint8_t p[64];
    size_t k = 0;
    static const uint8_t head[] = { 0x35, 0x03, 0x19, 0x11, 0x0a, 0x00, 0x0a, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    memcpy(p, head, sizeof(head)); k = sizeof(head);
    memcpy(p + k, state, 7);
    ask(0x06, 2, p, k + 7);
    CHECK(resp_n > 0 && resp[0] == 0x07);
    // ... altered in its offset, its check, or its length...
    for (int i = 1; i < 7; i++) {
        memcpy(p + k, state, 7);
        p[k + i] ^= 0x01;
        ask(0x06, 3, p, k + 7);
        expect_error(3, 0x0005);
    }
    memcpy(p + k, state, 7);
    p[k] = 5;
    ask(0x06, 4, p, k + 6);
    expect_error(4, 0x0005);
    // ... or carried to another request (another attribute list, another kind).
    p[k] = 6; memcpy(p + k, state, 7);
    p[12] = 0xfe;
    ask(0x06, 5, p, k + 7);
    expect_error(5, 0x0005);
    uint8_t s2[32];
    static const uint8_t sahead[] = { 0x00, 0x01, 0x00, 0x01, 0x00, 0x0a, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    memcpy(s2, sahead, sizeof(sahead)); memcpy(s2 + sizeof(sahead), state, 7);
    ask(0x04, 6, s2, sizeof(sahead) + 7);
    expect_error(6, 0x0005);
    // A state of our shape but never issued: offset zero, offset at the end.
    memcpy(p, head, sizeof(head));
    uint8_t forged[7] = { 6, 0, 0, 0, 0, 0, 0 };
    memcpy(p + k, forged, 7);
    ask(0x06, 7, p, k + 7);
    expect_error(7, 0x0005);
    forged[2] = 200;
    memcpy(p + k, forged, 7);
    ask(0x06, 8, p, k + 7);
    expect_error(8, 0x0005);
    // Sixteen bytes of somebody else's state: refused; seventeen: bad syntax.
    uint8_t long_state[18] = { 16 };
    memcpy(p + k, long_state, 17);
    ask(0x06, 9, p, k + 17);
    expect_error(9, 0x0005);
    long_state[0] = 17;
    memcpy(p + k, long_state, 18);
    ask(0x06, 10, p, k + 18);
    expect_error(10, 0x0003);
}

// ── The table of records ────────────────────────────────────────────────────

// The two AVRCP records (AUDIO_REMOTE.md § What we say we are), written out
// by hand as Android's AVRC_AddRecord writes them for AVRCP 1.4: L2CAP PSM
// 0x0017, AVCTP (0x0017) 1.3, profile AV Remote Control (0x110E) 1.4.
static const uint8_t tg_handle[] = { 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x02 };
static const uint8_t tg_classes[] = { 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0c };
static const uint8_t ct_handle[] = { 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x03 };
static const uint8_t ct_classes[] = { 0x09, 0x00, 0x01, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x19, 0x11, 0x0f };
static const uint8_t avctp_protocols[] = { 0x09, 0x00, 0x04, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,
                                           0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x03 };
static const uint8_t avrcp_profiles[] = { 0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x09, 0x01, 0x04 };
static const uint8_t tg_features[] = { 0x09, 0x03, 0x11, 0x09, 0x00, 0x03 };
static const uint8_t ct_features[] = { 0x09, 0x03, 0x11, 0x09, 0x00, 0x02 };
typedef struct { const uint8_t *b; size_t n; uint16_t id; } part_t;
static const part_t records[3][6] = {
    { { rec_handle, sizeof(rec_handle), 0x0000 }, { rec_classes, sizeof(rec_classes), 0x0001 },
      { rec_protocols, sizeof(rec_protocols), 0x0004 }, { rec_browse, sizeof(rec_browse), 0x0005 },
      { rec_profiles, sizeof(rec_profiles), 0x0009 }, { rec_features, sizeof(rec_features), 0x0311 } },
    { { tg_handle, sizeof(tg_handle), 0x0000 }, { tg_classes, sizeof(tg_classes), 0x0001 },
      { avctp_protocols, sizeof(avctp_protocols), 0x0004 }, { rec_browse, sizeof(rec_browse), 0x0005 },
      { avrcp_profiles, sizeof(avrcp_profiles), 0x0009 }, { tg_features, sizeof(tg_features), 0x0311 } },
    { { ct_handle, sizeof(ct_handle), 0x0000 }, { ct_classes, sizeof(ct_classes), 0x0001 },
      { avctp_protocols, sizeof(avctp_protocols), 0x0004 }, { rec_browse, sizeof(rec_browse), 0x0005 },
      { avrcp_profiles, sizeof(avrcp_profiles), 0x0009 }, { ct_features, sizeof(ct_features), 0x0311 } },
};
static const uint32_t handles[3] = { 0x00010001, 0x00010002, 0x00010003 };

static size_t seq_header(uint8_t *out, size_t n)
{
    if (n < 256) { out[0] = 0x35; out[1] = (uint8_t)n; return 2; }
    out[0] = 0x36; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n; return 3;
}
// Record r's attribute list for the IDs in [lo, hi].
static size_t record_list(uint8_t *out, int r, uint16_t lo, uint16_t hi)
{
    uint8_t body[256];
    size_t n = 0;
    for (int i = 0; i < 6; i++)
        if (records[r][i].id >= lo && records[r][i].id <= hi) { memcpy(body + n, records[r][i].b, records[r][i].n); n += records[r][i].n; }
    size_t h = seq_header(out, n);
    memcpy(out + h, body, n);
    return h + n;
}
// The list of lists for the records in `mask` (bit r: record r).
static size_t record_lists(uint8_t *out, unsigned mask, uint16_t lo, uint16_t hi)
{
    uint8_t body[1024];
    size_t n = 0;
    for (int r = 0; r < 3; r++) if (mask >> r & 1) n += record_list(body + n, r, lo, hi);
    size_t h = seq_header(out, n);
    memcpy(out + h, body, n);
    return h + n;
}

// A Service Search for one 16-bit UUID: the handles of the records in
// `mask`, the first `max` of them.
static void expect_search(uint16_t uuid, uint16_t max, unsigned mask)
{
    static uint16_t tid = 0x4000;
    tid++;
    ask(0x02, tid, BYTES(0x35, 0x03, 0x19, BE(uuid), BE(max), 0x00));
    uint8_t want[64];
    size_t k = 9, count = 0;
    for (int r = 0; r < 3; r++) if ((mask >> r & 1) && count < max) {
        want[k++] = (uint8_t)(handles[r] >> 24); want[k++] = (uint8_t)(handles[r] >> 16);
        want[k++] = (uint8_t)(handles[r] >> 8); want[k++] = (uint8_t)handles[r];
        count++;
    }
    want[k++] = 0;
    want[0] = 0x03; want[1] = (uint8_t)(tid >> 8); want[2] = (uint8_t)tid; want[3] = 0; want[4] = (uint8_t)(k - 5);
    want[5] = 0; want[6] = (uint8_t)count; want[7] = 0; want[8] = (uint8_t)count;
    if (resp_n != k || memcmp(resp, want, k)) {
        failures++; printf("FAIL search for %04x max %u\n", uuid, max); dump("want", want, k); dump("got ", resp, resp_n);
    }
}

// A Service Search Attribute for one 16-bit UUID, every attribute: the
// lists of the records in `mask`.
static void expect_search_attributes(uint16_t uuid, unsigned mask)
{
    ask(0x06, 0x0707, BYTES(0x35, 0x03, 0x19, BE(uuid), BE(1000), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    uint8_t want[1024];
    size_t wn = record_lists(want, mask, 0x0000, 0xffff);
    if (resp_n != 7 + wn + 1 || resp[0] != 0x07 || (size_t)(resp[5] << 8 | resp[6]) != wn || memcmp(resp + 7, want, wn) ||
        resp[resp_n - 1] != 0) {
        failures++; printf("FAIL search attributes for %04x\n", uuid); dump("want", want, wn); dump("got ", resp, resp_n);
    }
}

// A Service Search followed to its end at capacity `cap`: the handles
// joined, the total each part gave, and each part within its bounds.
static size_t follow_search(uint16_t uuid, uint16_t max, size_t cap, uint32_t *joined, unsigned *parts)
{
    uint8_t q[64], r[256], cont[17] = { 0 };
    size_t total = 0, got = 0;
    *parts = 0;
    for (int round = 0; round < 20; round++) {
        uint8_t params[40];
        size_t k = 0;
        params[k++] = 0x35; params[k++] = 3; params[k++] = 0x19; params[k++] = (uint8_t)(uuid >> 8); params[k++] = (uint8_t)uuid;
        params[k++] = (uint8_t)(max >> 8); params[k++] = (uint8_t)max;
        memcpy(params + k, cont, 1 + cont[0]); k += 1 + cont[0];
        size_t rn = bt_sdp_respond(q, request(q, 0x02, (uint16_t)round, params, k), r, cap);
        if (rn < 10 || r[0] != 0x03 || rn > cap) { failures++; printf("FAIL follow search: no answer at cap %zu\n", cap); dump("got", r, rn); return 0; }
        size_t t = (size_t)(r[5] << 8 | r[6]), c = (size_t)(r[7] << 8 | r[8]);
        if (round == 0) total = t;
        if (t != total || t > max || 9 + 4 * c + 1 + r[9 + 4 * c] != rn || (size_t)(r[3] << 8 | r[4]) != rn - 5) {
            failures++; printf("FAIL follow search: a part over its bounds at cap %zu\n", cap); dump("got", r, rn); return 0;
        }
        for (size_t i = 0; i < c; i++)
            joined[got++] = (uint32_t)r[9 + 4 * i] << 24 | (uint32_t)r[10 + 4 * i] << 16 | (uint32_t)r[11 + 4 * i] << 8 | r[12 + 4 * i];
        (*parts)++;
        uint8_t s = r[9 + 4 * c];
        if (!s) { if (got != total) { failures++; printf("FAIL follow search: %zu of %zu handles\n", got, total); } return got; }
        if (s > 16) { failures++; printf("FAIL follow search: a state of %u\n", s); return 0; }
        cont[0] = s; memcpy(cont + 1, r + 10 + 4 * c, s);
    }
    failures++; printf("FAIL follow search: never ended\n");
    return 0;
}

static void test_responder_records(void)
{
    // Each record by its class; AV Remote Control is the Target's profile
    // as well as the Controller record's class, so it finds both.
    expect_search(0x110A, 10, 1);
    expect_search(0x110C, 10, 2);
    expect_search(0x110F, 10, 4);
    expect_search(0x110E, 10, 6);
    expect_search(0x110B, 10, 0);
    // All three by the public browse root and by L2CAP's UUID; AVCTP's
    // finds the two AVRCP records, AVDTP's the Audio Source.
    expect_search(0x1002, 10, 7);
    expect_search(0x0100, 10, 7);
    expect_search(0x0017, 10, 6);
    expect_search(0x0019, 10, 1);
    expect_search(0x110D, 10, 1);
    // The requester's maximum is the most handles given.
    for (uint16_t max = 1; max <= 3; max++) expect_search(0x0100, max, 7);
    expect_search(0x0017, 1, 6);
    // Two UUIDs, both held: one record each way.
    ask(0x02, 0x51, BYTES(0x35, 0x06, 0x19, 0x00, 0x17, 0x19, 0x11, 0x0f, BE(10), 0x00));
    CHECK(resp_n == 14 && resp[6] == 1 && resp[8] == 1 && resp[12] == 0x03);
    ask(0x02, 0x52, BYTES(0x35, 0x06, 0x19, 0x11, 0x0a, 0x19, 0x00, 0x17, BE(10), 0x00));
    CHECK(resp_n == 10 && resp[6] == 0 && resp[8] == 0);

    // Each record's attributes, by Service Search Attribute...
    expect_search_attributes(0x110A, 1);
    expect_search_attributes(0x110C, 2);
    expect_search_attributes(0x110F, 4);
    expect_search_attributes(0x110E, 6);
    expect_search_attributes(0x1002, 7);
    expect_search_attributes(0x0017, 6);
    // ... and by Service Attribute under each handle, whole and in a range.
    uint8_t want[1024], params[32];
    for (int r = 0; r < 3; r++) {
        static const uint16_t ranges[][2] = { { 0x0000, 0xffff }, { 0x0001, 0x0001 }, { 0x0005, 0x0311 }, { 0x0002, 0x0003 } };
        for (int g = 0; g < 4; g++) {
            size_t k = 0;
            params[k++] = (uint8_t)(handles[r] >> 24); params[k++] = (uint8_t)(handles[r] >> 16);
            params[k++] = (uint8_t)(handles[r] >> 8); params[k++] = (uint8_t)handles[r];
            params[k++] = 0x01; params[k++] = 0xf4;
            params[k++] = 0x35; params[k++] = 5; params[k++] = 0x0a;
            params[k++] = (uint8_t)(ranges[g][0] >> 8); params[k++] = (uint8_t)ranges[g][0];
            params[k++] = (uint8_t)(ranges[g][1] >> 8); params[k++] = (uint8_t)ranges[g][1];
            params[k++] = 0;
            ask(0x04, (uint16_t)(0x60 + r * 4 + g), params, k);
            size_t wn = record_list(want, r, ranges[g][0], ranges[g][1]);
            if (resp_n != 7 + wn + 1 || resp[0] != 0x05 || memcmp(resp + 7, want, wn)) {
                failures++; printf("FAIL attributes of record %d range %d\n", r, g); dump("want", want, wn); dump("got ", resp, resp_n);
            }
        }
    }
    // Handles that are none of ours.
    static const uint32_t not_ours[] = { 0x00010000, 0x00010004, 0x00000002, 0x01010002, 0xffffffff, 0x00020001 };
    for (size_t i = 0; i < sizeof(not_ours) / sizeof(not_ours[0]); i++) {
        size_t k = 0;
        params[k++] = (uint8_t)(not_ours[i] >> 24); params[k++] = (uint8_t)(not_ours[i] >> 16);
        params[k++] = (uint8_t)(not_ours[i] >> 8); params[k++] = (uint8_t)not_ours[i];
        params[k++] = 0x01; params[k++] = 0xf4; params[k++] = 0x35; params[k++] = 3; params[k++] = 0x09;
        params[k++] = 0x00; params[k++] = 0x01; params[k++] = 0;
        ask(0x04, (uint16_t)(0x90 + i), params, k);
        expect_error(0x90 + i, 0x0002);
    }

    // Continuation across records: every maximum and every capacity, the
    // parts joined byte for byte the whole.
    static const uint8_t ssa_all[] = { 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    uint8_t whole[1024], joined[1024];
    size_t wn = follow(0x06, ssa_all, sizeof(ssa_all), 1000, 2048, whole);
    size_t xn = record_lists(want, 7, 0x0000, 0xffff);
    CHECK(wn == xn && !memcmp(whole, want, wn));
    CHECK(wn > 200);
    for (uint16_t max = 7; max <= wn; max++) {
        size_t jn = follow(0x06, ssa_all, sizeof(ssa_all), max, 2048, joined);
        if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL all records joined at max %u\n", max); }
    }
    for (size_t cap = 15; cap < wn + 20; cap++) {
        size_t jn = follow(0x06, ssa_all, sizeof(ssa_all), 1000, cap, joined);
        if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL all records joined at capacity %zu\n", cap); }
    }
    // The same across the two AVRCP records alone, and under each handle.
    static const uint8_t ssa_avctp[] = { 0x35, 0x03, 0x19, 0x00, 0x17, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
    wn = follow(0x06, ssa_avctp, sizeof(ssa_avctp), 1000, 2048, whole);
    xn = record_lists(want, 6, 0x0000, 0xffff);
    CHECK(wn == xn && !memcmp(whole, want, wn));
    for (uint16_t max = 7; max <= wn; max++) {
        size_t jn = follow(0x06, ssa_avctp, sizeof(ssa_avctp), max, 2048, joined);
        if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL avctp records joined at max %u\n", max); }
    }
    for (int r = 1; r < 3; r++) {
        uint8_t sa[11] = { (uint8_t)(handles[r] >> 24), (uint8_t)(handles[r] >> 16), (uint8_t)(handles[r] >> 8), (uint8_t)handles[r],
                           0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
        wn = follow(0x04, sa, sizeof(sa), 1000, 2048, whole);
        xn = record_list(want, r, 0x0000, 0xffff);
        CHECK(wn == xn && !memcmp(whole, want, wn));
        for (uint16_t max = 7; max <= wn; max++) {
            size_t jn = follow(0x04, sa, sizeof(sa), max, 2048, joined);
            if (jn != wn || memcmp(joined, whole, wn)) { failures++; printf("FAIL record %d joined at max %u\n", r, max); }
        }
    }

    // A Service Search too long for the room continues, a handle's place
    // at a time, at every capacity: the handles joined are the whole list.
    // All of them in one response when they fit with no state; else as many
    // as fit beside a state (20 bytes is one), the last part without one.
    for (uint16_t max = 1; max <= 4; max++) for (size_t cap = 10; cap <= 40; cap++) {
        uint32_t got[8];
        unsigned parts;
        size_t want_n = max < 3 ? max : 3;
        if (cap < 10 + 4 * want_n && cap < 20) {
            uint8_t q[64], r[64];
            size_t qn = request(q, 0x02, 1, BYTES(0x35, 0x03, 0x19, 0x01, 0x00, BE(max), 0x00));
            CHECK_EQ(bt_sdp_respond(q, qn, r, cap), 0);
            continue;
        }
        size_t gn = follow_search(0x0100, max, cap, got, &parts);
        if (gn != want_n) { failures++; printf("FAIL search joined at max %u cap %zu: %zu handles\n", max, cap, gn); continue; }
        for (size_t i = 0; i < gn; i++) if (got[i] != handles[i]) { failures++; printf("FAIL search joined at max %u cap %zu: handle %zu\n", max, cap, i); }
        unsigned want_parts = 0;
        for (size_t left = want_n; left; want_parts++) left = cap >= 10 + 4 * left ? 0 : left - (cap - 16) / 4;
        if (parts != want_parts) { failures++; printf("FAIL search at max %u cap %zu: %u parts, want %u\n", max, cap, parts, want_parts); }
    }
    // A search state of ours, altered, carried to another pattern or
    // another maximum that leaves its place past the end, or forged.
    uint8_t q[64], r[64];
    size_t qn = request(q, 0x02, 1, BYTES(0x35, 0x03, 0x19, 0x01, 0x00, BE(3), 0x00));
    size_t rn = bt_sdp_respond(q, qn, r, 20);
    CHECK(rn == 20 && r[6] == 3 && r[8] == 1 && r[13] == 6);
    uint8_t state[7];
    memcpy(state, r + 13, 7);
    CHECK(state[1] == 0 && state[2] == 1);
    uint8_t sp[32];
    static const uint8_t shead[] = { 0x35, 0x03, 0x19, 0x01, 0x00, 0x00, 0x03 };
    memcpy(sp, shead, sizeof(shead)); memcpy(sp + sizeof(shead), state, 7);
    ask(0x02, 2, sp, sizeof(shead) + 7);
    CHECK(resp_n == 18 && resp[6] == 3 && resp[8] == 2 && resp[12] == 0x02 && resp[16] == 0x03 && resp[17] == 0);
    for (int i = 1; i < 7; i++) {
        memcpy(sp + sizeof(shead), state, 7);
        sp[sizeof(shead) + i] ^= 0x01;
        ask(0x02, 3, sp, sizeof(shead) + 7);
        expect_error(3, 0x0005);
    }
    memcpy(sp + sizeof(shead), state, 7);
    sp[4] = 0x17;                                   // AVCTP's UUID: another pattern
    ask(0x02, 4, sp, sizeof(shead) + 7);
    expect_error(4, 0x0005);
    sp[4] = 0x00; sp[6] = 0x01;                     // a maximum of one: its place is past the end
    ask(0x02, 5, sp, sizeof(shead) + 7);
    expect_error(5, 0x0005);
    // A state an attribute request issued is not a search's.
    ask(0x06, 6, BYTES(0x35, 0x03, 0x19, 0x01, 0x00, BE(10), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    CHECK(resp_n == 7 + 10 + 7);
    memcpy(sp, shead, sizeof(shead)); memcpy(sp + sizeof(shead), resp + 17, 7);
    sp[sizeof(shead) + 1] = 0; sp[sizeof(shead) + 2] = 1;
    ask(0x02, 7, sp, sizeof(shead) + 7);
    expect_error(7, 0x0005);

    // The client's walker reads the answer for all three: the Audio Source
    // as the client would find it, and the two AVRCP records by their classes.
    ask(0x06, 8, BYTES(0x35, 0x03, 0x19, 0x01, 0x00, BE(1000), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    size_t count = (size_t)(resp[5] << 8 | resp[6]);
    bt_sdp_sink_t out;
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, BT_SDP_UUID_AUDIO_SOURCE, true, &out), BT_SDP_PARSED);
    CHECK(out.found);
    CHECK_EQ(out.avdtp_version, 0x0103);
    CHECK_EQ(out.a2dp_version, 0x0103);
    CHECK_EQ(out.features, 0x0001);
    CHECK_EQ(out.l2cap_psm, 0x0019);
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, 0x110C, true, &out), BT_SDP_PARSED);
    CHECK(out.found);
    CHECK_EQ(out.l2cap_psm, 0x0017);
    CHECK_EQ(out.avdtp_version, 0);
    CHECK_EQ(out.a2dp_version, 0);
    CHECK_EQ(out.features, 0x0003);
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, 0x110F, true, &out), BT_SDP_PARSED);
    CHECK(out.found);
    CHECK_EQ(out.l2cap_psm, 0x0017);
    CHECK_EQ(out.features, 0x0002);
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, BT_SDP_UUID_AUDIO_SINK, true, &out), BT_SDP_PARSED);
    CHECK(!out.found);
}

static void test_responder_sizes(void)
{
    uint8_t q[64], r[64];
    // The header's length, wrong both ways.
    size_t qn = request(q, 0x06, 0x0102, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(100), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    q[4]++;
    CHECK_EQ(bt_sdp_respond(q, qn, resp, sizeof(resp)), 7);
    resp_n = 7; expect_error(0x0102, 0x0004);
    q[4] -= 2;
    resp_n = bt_sdp_respond(q, qn, resp, sizeof(resp)); expect_error(0x0102, 0x0004);
    // The continuation length byte, wrong both ways, and absent.
    ask(0x06, 1, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(100), 0x35, 0x03, 0x09, 0x00, 0x01, 0x02, 0xaa));
    expect_error(1, 0x0004);
    ask(0x06, 2, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(100), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00, 0xaa));
    expect_error(2, 0x0004);
    ask(0x06, 3, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(100), 0x35, 0x03, 0x09, 0x00, 0x01));
    expect_error(3, 0x0004);
    ask(0x02, 4, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(1)));
    expect_error(4, 0x0004);
    // Too short for a header: under 3 bytes, nothing; 3 or 4, an error under the ID.
    CHECK_EQ(bt_sdp_respond(BYTES(0x06, 0x01), r, sizeof(r)), 0);
    resp_n = bt_sdp_respond(BYTES(0x06, 0x01, 0x02), resp, sizeof(resp));
    expect_error(0x0102, 0x0004);
    resp_n = bt_sdp_respond(BYTES(0x06, 0x01, 0x02, 0x00), resp, sizeof(resp));
    expect_error(0x0102, 0x0004);
    // Unknown PDUs, responses among them.
    static const uint8_t pdus[] = { 0x00, 0x01, 0x03, 0x05, 0x07, 0x08, 0xff };
    for (int i = 0; i < 7; i++) {
        uint8_t u[8] = { pdus[i], 0x00, 0x09, 0x00, 0x01, 0x00 };
        resp_n = bt_sdp_respond(u, 6, resp, sizeof(resp));
        expect_error(9, 0x0003);
    }
    // ... even one carrying a well-formed attribute request's parameters.
    ask(0x08, 10, BYTES(0x00, 0x01, 0x00, 0x01, BE(500), 0x35, 0x03, 0x09, 0x00, 0x01, 0x00));
    expect_error(10, 0x0003);
    // No room for even an error.
    qn = request(q, 0x09, 1, BYTES(0x00));
    CHECK_EQ(bt_sdp_respond(q, qn, r, 6), 0);
    CHECK_EQ(bt_sdp_respond(q, qn, r, 7), 7);
}

// The client's walker reads the responder's record.
static void test_cross_check(void)
{
    ask(0x06, 1, BYTES(0x35, 0x03, 0x19, 0x11, 0x0a, BE(1000), 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    size_t count = (size_t)(resp[5] << 8 | resp[6]);
    bt_sdp_sink_t out;
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, BT_SDP_UUID_AUDIO_SOURCE, true, &out), BT_SDP_PARSED);
    CHECK(out.found);
    CHECK_EQ(out.avdtp_version, 0x0103);
    CHECK_EQ(out.a2dp_version, 0x0103);
    CHECK_EQ(out.features, 0x0001);
    CHECK(out.features_present);
    CHECK_EQ(out.l2cap_psm, 0x0019);
    CHECK_EQ(bt_sdp_parse_attribute_lists(resp + 7, count, BT_SDP_UUID_AUDIO_SINK, true, &out), BT_SDP_PARSED);
    CHECK(!out.found);
    // The client end to end against the responder, the pattern turned to
    // our class, at a small MTU so the answer comes in parts.
    for (uint16_t mtu = 31; mtu <= 120; mtu++) {
        bt_sdp_client_start(&C, 0, mtu);
        int responses = 0;
        while (bt_sdp_client_state(&C) == BT_SDP_ASKING && responses < 40) {
            uint8_t q[64], r[256];
            size_t qn = bt_sdp_client_request(&C, q, sizeof(q));
            if (!qn) break;
            q[9] = 0x0a;
            size_t rn = bt_sdp_respond(q, qn, r, mtu);
            bt_sdp_client_receive(&C, r, rn);
            responses++;
        }
        if (bt_sdp_client_state(&C) != BT_SDP_DONE || !bt_sdp_client_result(&C)->complete || bt_sdp_client_result(&C)->found) {
            failures++; printf("FAIL end to end at mtu %u: state %d\n", mtu, bt_sdp_client_state(&C));
        }
        if (mtu == 31 && responses < 4) { failures++; printf("FAIL end to end: one part at mtu 31\n"); }
        if (C.held + 0 != count) { failures++; printf("FAIL end to end at mtu %u: %u bytes\n", mtu, C.held); }
        bt_sdp_sink_t again;
        if (bt_sdp_parse_attribute_lists(C.buf, C.held, BT_SDP_UUID_AUDIO_SOURCE, true, &again) != BT_SDP_PARSED || !again.found ||
            again.avdtp_version != 0x0103 || again.features != 1) { failures++; printf("FAIL end to end at mtu %u: what was held\n", mtu); }
    }
}

// ── Fuzz ────────────────────────────────────────────────────────────────────

static uint32_t seed = 4242;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

static void fuzz_client(int rounds)
{
    bld_t a;
    int done = 0, found = 0;
    for (int round = 0; round < rounds; round++) {
        shape_t s = { .extra_classes = rnd() % 2, .class_form = (int[]){ 16, 32, 128, -32 }[rnd() % 4],
                      .record_first = rnd() % 2, .pdl_wrap = (int)(rnd() % 5), .big_name = rnd() % 4 ? 0 : rnd() % 1500,
                      .long_lengths = rnd() % 2 };
        build_answer(&a, &s);
        T = rnd();
        bt_sdp_client_start(&C, T, (uint16_t)(rnd() % 1100));
        size_t from = 0;
        for (int step = 0; step < 40 && bt_sdp_client_state(&C) == BT_SDP_ASKING; step++) {
            uint8_t q[64], r[4200];
            size_t qn = bt_sdp_client_request(&C, q, rnd() % 8 ? sizeof(q) : rnd() % 40);
            if (qn > 36 || qn > sizeof(q)) { failures++; printf("FAIL a request of %zu\n", qn); }
            size_t to = from + (rnd() % 3 ? rnd() % 300 : rnd() % 1200);
            if (to > a.n) to = a.n;
            unsigned cont = to == a.n && rnd() % 4 ? 0 : 1 + rnd() % 16;
            size_t rn = response(r, C.tid, a.b, from, to, cont, rnd());
            switch (rnd() % 8) {                    // spoil it now and then
            case 0: if (rn) r[rnd() % rn] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: rn = rnd() % (rn + 1); break;
            case 2: rn = rnd() % 64; for (size_t i = 0; i < rn; i++) r[i] = (uint8_t)rnd(); break;
            default: break;
            }
            if (rnd() % 6 == 0) bt_sdp_client_tick(&C, T + rnd() % 7000);
            bt_sdp_client_receive(&C, r, rn);
            from = to;
            if (C.held > BT_SDP_BUFFER || C.rounds > BT_SDP_ROUNDS || C.cont_len > BT_SDP_CONTINUATION) { failures++; printf("FAIL client bounds\n"); }
        }
        bt_sdp_client_tick(&C, T + 6000);
        if (bt_sdp_client_state(&C) == BT_SDP_ASKING) { failures++; printf("FAIL still asking after the limit\n"); }
        if (bt_sdp_client_state(&C) == BT_SDP_DONE) done++;
        if (bt_sdp_client_result(&C)->found) found++;
        if (bt_sdp_client_state(&C) == BT_SDP_FAILED && bt_sdp_client_result(&C)->found) { failures++; printf("FAIL failed but found\n"); }
        char line[160];
        if (bt_sdp_client_status(&C, line, sizeof(line)) != strlen(line)) { failures++; printf("FAIL status length\n"); }
        if (failures > 20) return;
    }
    printf("fuzz: client finished %d of %d queries, found the sink in %d\n", done, rounds, found);
    if (rounds >= 100 && (done < rounds / 10 || found < rounds / 10)) { failures++; printf("FAIL the client fuzz reached nothing\n"); }
}

static void fuzz_parser(int rounds)
{
    bld_t a;
    for (int round = 0; round < rounds; round++) {
        uint8_t buf[2048];
        size_t n;
        if (rnd() % 2) {
            shape_t s = { .pdl_wrap = (int)(rnd() % 8), .record_first = rnd() % 2, .long_lengths = rnd() % 2 };
            build_answer(&a, &s);
            n = a.n;
            memcpy(buf, a.b, n);
            int flips = (int)(rnd() % 4);
            for (int i = 0; i < flips; i++) buf[rnd() % n] = (uint8_t)rnd();
            if (rnd() % 2) n = rnd() % (n + 1);
        } else {
            n = rnd() % 64;
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rnd() % 3 ? 0x30 | (rnd() % 8) : rnd());
        }
        // Copied to exactly n bytes, so a read past them is a report.
        uint8_t *exact = malloc(n ? n : 1);
        memcpy(exact, buf, n);
        bt_sdp_sink_t out;
        bt_sdp_parse_t r = bt_sdp_parse_attribute_lists(exact, n, 0x110B, rnd() % 2, &out);
        if (r == BT_SDP_MALFORMED && out.found) { failures++; printf("FAIL malformed but found\n"); }
        free(exact);
        if (failures > 20) return;
    }
}

static void fuzz_responder(int rounds)
{
    static const uint8_t seeds[][24] = {
        { 20, 0x06, 0, 1, 0, 15, 0x35, 0x03, 0x19, 0x11, 0x0a, 0x00, 0x20, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 },
        { 17, 0x04, 0, 1, 0, 12, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x35, 0x03, 0x09, 0x00, 0x04, 0x00 },
        { 13, 0x02, 0, 1, 0, 8, 0x35, 0x03, 0x19, 0x01, 0x00, 0x00, 0x05, 0x00 },
        // All three records' lists, and the Controller record's attributes.
        { 20, 0x06, 0, 1, 0, 15, 0x35, 0x03, 0x19, 0x01, 0x00, 0x00, 0x30, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 },
        { 19, 0x04, 0, 1, 0, 14, 0x00, 0x01, 0x00, 0x03, 0x00, 0x10, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 },
    };
    const int kinds = (int)(sizeof(seeds) / sizeof(seeds[0]));
    int answered = 0;
    for (int round = 0; round < rounds; round++) {
        uint8_t q[128];
        memcpy(q, seeds[round % kinds] + 1, seeds[round % kinds][0]);
        size_t n = seeds[round % kinds][0];
        int flips = (int)(rnd() % 4);
        for (int i = 0; i < flips; i++) q[rnd() % n] = (uint8_t)rnd();
        if (rnd() % 4 == 0) n = rnd() % (n + 1);
        if (rnd() % 6 == 0) { n = rnd() % 100; for (size_t i = 0; i < n; i++) q[i] = (uint8_t)rnd(); }
        if (rnd() % 3 == 0 && n >= 5) { q[3] = (uint8_t)((n - 5) >> 8); q[4] = (uint8_t)(n - 5); }
        uint8_t *exact = malloc(n ? n : 1);
        memcpy(exact, q, n);
        size_t cap = rnd() % 3 ? 2048 : rnd() % 120;
        uint8_t *out = malloc(cap ? cap : 1);
        size_t rn = bt_sdp_respond(exact, n, out, cap);
        if (rn > cap) { failures++; printf("FAIL a response past its capacity\n"); }
        if (rn) {
            if (rn < 7 || (size_t)(out[3] << 8 | out[4]) != rn - 5 || out[1] != exact[1] || out[2] != exact[2]) {
                failures++; printf("FAIL a response that is not well formed\n"); dump("q", exact, n); dump("r", out, rn);
            }
            if (out[0] == 0x05 || out[0] == 0x07) {
                answered++;
                size_t count = (size_t)(out[5] << 8 | out[6]);
                if (7 + count + 1 > rn || out[7 + count] > 16 || 8 + count + out[7 + count] != rn) { failures++; printf("FAIL a response's parts\n"); }
            } else if (out[0] != 0x01 && out[0] != 0x03) { failures++; printf("FAIL PDU %02x\n", out[0]); }
        }
        free(exact); free(out);
        if (failures > 20) return;
    }
    printf("fuzz: the responder gave %d attribute answers\n", answered);
    if (rounds >= 100 && answered < rounds / 20) { failures++; printf("FAIL the responder fuzz reached nothing\n"); }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    test_client_whole_and_cut();
    test_client_bounds();
    test_client_failures();
    test_client_uuids_and_depth();
    test_client_request();
    test_responder_search();
    test_responder_attributes();
    test_responder_continuation();
    test_responder_records();
    test_responder_sizes();
    test_cross_check();
    fuzz_client(rounds);
    fuzz_parser(rounds);
    fuzz_responder(rounds);
    if (failures) { printf("bt_sdp: %d failure(s)\n", failures); return 1; }
    printf("bt_sdp: all scripted cases and %d fuzz rounds each passed\n", rounds);
    return 0;
}
