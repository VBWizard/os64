// test_h2_host.c — HTTP/2's frame layer and HPACK on the host
// (tools/test_h2_host.sh builds it under ASan and UBSan).
//
// h2_hpack.c is INCLUDED, not linked, so the cases can reach its integers,
// its Huffman coder and its tables directly: RFC 7541's worked examples for
// those (Appendix C.1, and the strings of C.4 and C.6) are stated at that
// level. Everything else goes through the two headers, as a connection will.
//
// The examples of Appendix C are checked in BOTH directions with the
// dynamic table read after each: our encoder must produce the RFC's bytes,
// and our decoder must read them back to the RFC's fields.
#define _POSIX_C_SOURCE 200809L      // alarm, under -std=c11
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../userland/libfetch/h2_hpack.c"
#include "h2_frame.h"

// A FAILING RUN IS A SHORT ONE. This test is run with rules broken on
// purpose (tools/test_h2_mutants.py), and a broken table can make a loop
// here run four billion times or forever, saying so on every pass: so the
// run ends at its FAILURES_MOST-th failed check, and main sets an alarm that
// ends one that has stopped making progress.
#define FAILURES_MOST 50
#define SECONDS_MOST 120
static int checks, failures;
#define CHECK(x)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(x)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
            if (++failures == FAILURES_MOST) {                                \
                fprintf(stderr, "test_h2_host: %d checks failed; stopping\n", failures); \
                exit(1);                                                      \
            }                                                                 \
        }                                                                     \
    } while (0)

// "8286 84" to bytes; the count is returned.
static size_t hex(const char *text, uint8_t *out)
{
    size_t n = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ' ')
            continue;
        unsigned v;
        sscanf(p, "%2x", &v);
        out[n++] = (uint8_t)v;
        p++;
    }
    return n;
}

static h2_field_t F(const char *name, const char *value)
{
    return (h2_field_t){.name = (const uint8_t *)name, .value = (const uint8_t *)value,
                        .name_len = (uint32_t)strlen(name), .value_len = (uint32_t)strlen(value)};
}

// What a decode handed over, copied: the pointers die with each call.
#define GOT_MAX 64
typedef struct {
    char name[GOT_MAX][300], value[GOT_MAX][300];
    uint32_t name_len[GOT_MAX], value_len[GOT_MAX];
    bool sensitive[GOT_MAX];
    size_t n, stop_after;       // stop_after: answer false once this many were taken (0 = never)
} got_t;

static bool take(void *ctx, const h2_field_t *f)
{
    got_t *g = ctx;
    if (g->n < GOT_MAX && f->name_len < 300 && f->value_len < 300) {
        memcpy(g->name[g->n], f->name, f->name_len);
        memcpy(g->value[g->n], f->value, f->value_len);
        g->name[g->n][f->name_len] = g->value[g->n][f->value_len] = '\0';
        g->name_len[g->n] = f->name_len;
        g->value_len[g->n] = f->value_len;
        g->sensitive[g->n] = f->sensitive;
    }
    g->n++;
    return g->stop_after == 0 || g->n < g->stop_after;
}

static bool got_is(const got_t *g, size_t i, const char *name, const char *value)
{
    return i < g->n && strcmp(g->name[i], name) == 0 && strcmp(g->value[i], value) == 0;
}

// The table reads, newest first, as these fields and this size.
static bool table_is(const h2_hpack_table_t *t, const h2_field_t *want, uint32_t count, uint32_t size)
{
    if (t->count != count || t->size != size)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        h2_field_t f;
        if (!lookup(t, H2_HPACK_STATIC_COUNT + 1 + i, &f) || !same(f.name, f.name_len, want[i].name, want[i].name_len) ||
            !same(f.value, f.value_len, want[i].value, want[i].value_len))
            return false;
    }
    return true;
}

// A decoder and an encoder with their storage, for a table of `most`.
#define ROOM 65536u
typedef struct {
    h2_hpack_decoder_t dec;
    h2_hpack_encoder_t enc;
    uint32_t dec_store[H2_HPACK_STORAGE(ROOM) / 4 + 1], enc_store[H2_HPACK_STORAGE(ROOM) / 4 + 1];
    uint8_t scratch[ROOM];
} pair_t;

static pair_t *pair_new(uint32_t advertised, uint32_t ceiling)
{
    pair_t *p = calloc(1, sizeof(*p));
    CHECK(h2_hpack_decoder_init(&p->dec, p->dec_store, sizeof(p->dec_store), p->scratch, sizeof(p->scratch), advertised));
    CHECK(h2_hpack_encoder_init(&p->enc, p->enc_store, sizeof(p->enc_store), ceiling));
    return p;
}

// ── The tables ──────────────────────────────────────────────────────────

static void tables_cases(void)
{
    // The code is canonical and complete, and the decoder's three tables
    // are what the lengths alone say they are.
    uint16_t order[H2_HUFF_SYMBOLS];
    unsigned n = 0;
    for (unsigned bits = 1; bits <= H2_HUFF_LONGEST; bits++)
        for (unsigned s = 0; s < H2_HUFF_SYMBOLS; s++)
            if (kHuffBits[s] == bits)
                order[n++] = (uint16_t)s;
    CHECK(n == H2_HUFF_SYMBOLS && memcmp(order, kHuffOrder, sizeof(order)) == 0);
    uint64_t kraft = 0;         // in units of 2^-30
    uint32_t code = 0;
    unsigned at = 0;
    for (unsigned bits = 1; bits <= H2_HUFF_LONGEST; bits++) {
        unsigned count = 0;
        code <<= 1;
        if (at < n && kHuffBits[order[at]] == bits)
            CHECK(kHuffFirst[bits] == code && kHuffOffset[bits] == at);
        for (; at < n && kHuffBits[order[at]] == bits; at++, count++, code++) {
            CHECK(kHuffCode[order[at]] == code);
            kraft += 1u << (H2_HUFF_LONGEST - bits);
        }
        CHECK(kHuffCount[bits] == count);
    }
    CHECK(kraft == 1u << H2_HUFF_LONGEST);
    CHECK(kHuffBits[H2_HUFF_EOS] == 30 && kHuffCode[H2_HUFF_EOS] == 0x3fffffff);
    CHECK(kHuffBits['0'] == 5 && kHuffCode['0'] == 0 && kHuffBits['a'] == 5 && kHuffCode['a'] == 3);

    // The static table: a few rows by number, and every length.
    for (unsigned i = 0; i < H2_HPACK_STATIC_COUNT; i++)
        CHECK(strlen(kStatic[i].name) == kStatic[i].name_len && strlen(kStatic[i].value) == kStatic[i].value_len);
    CHECK(strcmp(kStatic[0].name, ":authority") == 0 && strcmp(kStatic[1].value, "GET") == 0);
    CHECK(strcmp(kStatic[7].name, ":status") == 0 && strcmp(kStatic[7].value, "200") == 0);
    CHECK(strcmp(kStatic[15].name, "accept-encoding") == 0 && strcmp(kStatic[15].value, "gzip, deflate") == 0);
    CHECK(strcmp(kStatic[31].name, "cookie") == 0 && strcmp(kStatic[54].name, "set-cookie") == 0);
    CHECK(strcmp(kStatic[60].name, "www-authenticate") == 0);
}

// ── Integers and Huffman, at their own level ────────────────────────────

static void integer_cases(void)
{
    uint8_t out[16], *end;
    uint32_t v;
    const uint8_t *p;
    // C.1.1: 10 in a 5-bit prefix. C.1.2: 1337 in one. C.1.3: 42 at a byte's start.
    end = int_write(out, 5, 0, 10);
    CHECK(end - out == 1 && out[0] == 0x0a);
    end = int_write(out, 5, 0, 1337);
    CHECK(end - out == 3 && out[0] == 0x1f && out[1] == 0x9a && out[2] == 0x0a);
    end = int_write(out, 8, 0, 42);
    CHECK(end - out == 1 && out[0] == 0x2a);
    static const uint8_t k1337[] = {0xff, 0x9a, 0x0a};     // the prefix's other bits set, as a pattern would
    p = k1337;
    CHECK(int_read(&p, k1337 + 3, 5, &v) && v == 1337 && p == k1337 + 3);
    // Every value at a prefix's edge, and the largest, round the trip.
    static const uint32_t kValues[] = {0, 1, 30, 31, 32, 126, 127, 128, 254, 255, 256, 16383, 16384, INT_MOST};
    for (unsigned prefix = 4; prefix <= 7; prefix++)
        for (size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); i++) {
            end = int_write(out, prefix, 0, kValues[i]);
            p = out;
            CHECK(end - out <= 6 && int_read(&p, end, prefix, &v) && v == kValues[i] && p == end);
            // Cut short by a byte, it is not an integer.
            p = out;
            CHECK(end - out == 1 || !int_read(&p, end - 1, prefix, &v));
        }
    // One past the most is an error, not a wrap; so is a sixth byte.
    end = int_write(out, 7, 0, INT_MOST + 1u);
    p = out;
    CHECK(!int_read(&p, end, 7, &v));
    static const uint8_t kLong[] = {0x7f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    p = kLong;
    CHECK(!int_read(&p, kLong + sizeof(kLong), 7, &v));
    static const uint8_t kFive[] = {0x7f, 0x80, 0x80, 0x80, 0x80, 0x00};
    p = kFive;
    CHECK(int_read(&p, kFive + sizeof(kFive), 7, &v) && v == 127);
    p = out;
    CHECK(!int_read(&p, out, 7, &v));                       // nothing to read
}

static void huffman_cases(void)
{
    uint8_t wire[64], text[64];
    uint32_t len;
    // C.4.1's authority, C.4.2's value, C.6.1's status.
    size_t n = hex("f1e3 c2e5 f23a 6ba0 ab90 f4ff", wire);
    CHECK(huff_decode(wire, (uint32_t)n, text, sizeof(text), &len) && len == 15 && memcmp(text, "www.example.com", 15) == 0);
    CHECK(huff_size((const uint8_t *)"www.example.com", 15) == 12);
    uint8_t *end = huff_encode(text, (const uint8_t *)"www.example.com", 15);
    CHECK(end - text == 12 && memcmp(text, wire, 12) == 0);
    n = hex("a8eb 1064 9cbf", wire);
    CHECK(huff_decode(wire, (uint32_t)n, text, sizeof(text), &len) && len == 8 && memcmp(text, "no-cache", 8) == 0);
    n = hex("6402", wire);
    CHECK(huff_decode(wire, (uint32_t)n, text, sizeof(text), &len) && len == 3 && memcmp(text, "302", 3) == 0);
    // Every byte value, alone and in a run, round the trip.
    uint8_t all[256], coded[1024], back[256];
    for (unsigned i = 0; i < 256; i++)
        all[i] = (uint8_t)i;
    end = huff_encode(coded, all, 256);
    CHECK((uint64_t)(end - coded) == huff_size(all, 256));
    CHECK(huff_decode(coded, (uint32_t)(end - coded), back, sizeof(back), &len) && len == 256 && memcmp(back, all, 256) == 0);
    for (unsigned i = 0; i < 256; i++) {
        end = huff_encode(coded, &all[i], 1);
        CHECK(huff_decode(coded, (uint32_t)(end - coded), back, sizeof(back), &len) && len == 1 && back[0] == i);
    }
    // No room: counted all the same, and nothing written past the room.
    n = hex("f1e3 c2e5 f23a 6ba0 ab90 f4ff", wire);
    memset(text, 0x55, sizeof(text));
    CHECK(huff_decode(wire, (uint32_t)n, text, 4, &len) && len == 15 && text[4] == 0x55);
    // The three errors § 5.2 names. '0' is 00000: one of it and three bits
    // of ones is a good string; padded with zeros it is not.
    static const uint8_t kGood[] = {0x07}, kZeroPad[] = {0x00}, kEightOnes[] = {0x07, 0xff};
    CHECK(huff_decode(kGood, 1, text, sizeof(text), &len) && len == 1 && text[0] == '0');
    CHECK(!huff_decode(kZeroPad, 1, text, sizeof(text), &len));
    CHECK(!huff_decode(kEightOnes, 2, text, sizeof(text), &len));       // eleven bits of padding
    static const uint8_t kEos[] = {0xff, 0xff, 0xff, 0xff};             // thirty ones, the EOS code itself
    CHECK(!huff_decode(kEos, 4, text, sizeof(text), &len));
    CHECK(huff_decode(kEos, 0, text, sizeof(text), &len) && len == 0);  // the empty string
}

// ── Appendix C.2: one representation each ───────────────────────────────

static void literal_cases(void)
{
    uint8_t wire[128], out[256];
    size_t n, len;
    got_t g;
    pair_t *p = pair_new(4096, 4096);
    p->enc.huffman = false;

    // C.2.1: a literal with indexing.
    n = hex("400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572", wire);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_OK && g.n == 1);
    CHECK(got_is(&g, 0, "custom-key", "custom-header") && !g.sensitive[0]);
    h2_field_t one = F("custom-key", "custom-header");
    CHECK(table_is(&p->dec.table, &one, 1, 55));
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == n && memcmp(out, wire, n) == 0);
    CHECK(table_is(&p->enc.table, &one, 1, 55));
    free(p);

    // C.2.2: a literal without indexing. (Ours would index this field, so
    // the example is read and not written.)
    p = pair_new(4096, 4096);
    n = hex("040c 2f73 616d 706c 652f 7061 7468", wire);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_OK && g.n == 1);
    CHECK(got_is(&g, 0, ":path", "/sample/path") && !g.sensitive[0] && p->dec.table.count == 0);

    // C.2.3: never indexed, and said so to whoever reads it.
    p->enc.huffman = false;
    n = hex("1008 7061 7373 776f 7264 0673 6563 7265 74", wire);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_OK && g.n == 1);
    CHECK(got_is(&g, 0, "password", "secret") && g.sensitive[0] && p->dec.table.count == 0);
    one = F("password", "secret");
    one.sensitive = true;
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == n && memcmp(out, wire, n) == 0);
    CHECK(p->enc.table.count == 0);
    // authorization is never indexed whoever asks, its name from the table.
    one = F("authorization", "Basic abc");
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && out[0] == 0x1f && out[1] == 23 - 15);
    CHECK(p->enc.table.count == 0);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, out, len, take, &g) == H2_HPACK_OK && got_is(&g, 0, "authorization", "Basic abc") &&
          g.sensitive[0]);
    // And never sent as an index: not the static table's own empty
    // authorization, and not a field the dynamic table holds whole because
    // it was sent before without the mark.
    one = F("authorization", "");
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == 3 && out[0] == 0x1f && out[1] == 23 - 15 &&
          out[2] == 0x00);
    one = F("x-secret", "foo");
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && out[0] == 0x40 && p->enc.table.count == 1);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, out, len, take, &g) == H2_HPACK_OK && !g.sensitive[0]);
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0xbe);
    one.sensitive = true;
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == 6 && out[0] == 0x1f && out[1] == 62 - 15 &&
          out[2] == 0x03 && memcmp(out + 3, "foo", 3) == 0 && p->enc.table.count == 1);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, out, len, take, &g) == H2_HPACK_OK && got_is(&g, 0, "x-secret", "foo") && g.sensitive[0]);

    // C.2.4: indexed.
    n = hex("82", wire);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_OK && got_is(&g, 0, ":method", "GET"));
    one = F(":method", "GET");
    CHECK(h2_hpack_encode(&p->enc, &one, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0x82);
    free(p);
}

// ── Appendix C.3 to C.6: sequences on one connection ────────────────────

typedef struct {
    const char *wire;
    const char *ours;           // what our encoder writes, where that is not the RFC's bytes
    h2_field_t fields[8];
    size_t count;
    h2_field_t table[8];        // newest first, after this block
    uint32_t table_count, table_size;
} step_t;

static void sequence(const step_t *steps, size_t n, bool huffman, uint32_t table_most)
{
    pair_t *p = pair_new(4096, 4096);
    p->enc.huffman = huffman;
    if (table_most != 4096) {
        // C.5 and C.6 set the table to 256 bytes out of band, with no size
        // update on the wire; so do these two.
        p->enc.table.most = p->dec.table.most = table_most;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t wire[256], out[512];
        size_t len, wire_len = hex(steps[i].wire, wire);
        got_t g;
        memset(&g, 0, sizeof(g));
        CHECK(h2_hpack_decode(&p->dec, wire, wire_len, take, &g) == H2_HPACK_OK && g.n == steps[i].count);
        for (size_t k = 0; k < steps[i].count; k++)
            CHECK(got_is(&g, k, (const char *)steps[i].fields[k].name, (const char *)steps[i].fields[k].value));
        CHECK(table_is(&p->dec.table, steps[i].table, steps[i].table_count, steps[i].table_size));
        CHECK(h2_hpack_encode(&p->enc, steps[i].fields, steps[i].count, out, sizeof(out), &len));
        if (steps[i].ours != NULL)
            wire_len = hex(steps[i].ours, wire);
        CHECK(len == wire_len && memcmp(out, wire, wire_len) == 0);
        CHECK(table_is(&p->enc.table, steps[i].table, steps[i].table_count, steps[i].table_size));
    }
    free(p);
}

static void sequence_cases(void)
{
#define DATE1 "Mon, 21 Oct 2013 20:13:21 GMT"
#define DATE2 "Mon, 21 Oct 2013 20:13:22 GMT"
#define COOKIE "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"
    step_t requests[3] = {
        {.fields = {F(":method", "GET"), F(":scheme", "http"), F(":path", "/"), F(":authority", "www.example.com")},
         .count = 4,
         .table = {F(":authority", "www.example.com")},
         .table_count = 1, .table_size = 57},
        {.fields = {F(":method", "GET"), F(":scheme", "http"), F(":path", "/"), F(":authority", "www.example.com"),
                    F("cache-control", "no-cache")},
         .count = 5,
         .table = {F("cache-control", "no-cache"), F(":authority", "www.example.com")},
         .table_count = 2, .table_size = 110},
        {.fields = {F(":method", "GET"), F(":scheme", "https"), F(":path", "/index.html"),
                    F(":authority", "www.example.com"), F("custom-key", "custom-value")},
         .count = 5,
         .table = {F("custom-key", "custom-value"), F("cache-control", "no-cache"), F(":authority", "www.example.com")},
         .table_count = 3, .table_size = 164},
    };
    // C.3: plain strings.
    requests[0].wire = "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d";
    requests[1].wire = "8286 84be 5808 6e6f 2d63 6163 6865";
    requests[2].wire = "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65";
    sequence(requests, 3, false, 4096);
    // C.4: the same requests, Huffman coded.
    requests[0].wire = "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff";
    requests[1].wire = "8286 84be 5886 a8eb 1064 9cbf";
    requests[2].wire = "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf";
    sequence(requests, 3, true, 4096);

    step_t responses[3] = {
        {.fields = {F(":status", "302"), F("cache-control", "private"), F("date", DATE1),
                    F("location", "https://www.example.com")},
         .count = 4,
         .table = {F("location", "https://www.example.com"), F("date", DATE1), F("cache-control", "private"),
                   F(":status", "302")},
         .table_count = 4, .table_size = 222},
        // The fifth field does not fit 256 bytes: ":status 302" is evicted.
        {.fields = {F(":status", "307"), F("cache-control", "private"), F("date", DATE1),
                    F("location", "https://www.example.com")},
         .count = 4,
         .table = {F(":status", "307"), F("location", "https://www.example.com"), F("date", DATE1),
                   F("cache-control", "private")},
         .table_count = 4, .table_size = 222},
        // And here several go, to make room for a date, an encoding and a cookie.
        {.fields = {F(":status", "200"), F("cache-control", "private"), F("date", DATE2),
                    F("location", "https://www.example.com"), F("content-encoding", "gzip"), F("set-cookie", COOKIE)},
         .count = 6,
         .table = {F("set-cookie", COOKIE), F("content-encoding", "gzip"), F("date", DATE2)},
         .table_count = 3, .table_size = 215},
    };
    // C.5: plain strings, a table of 256 bytes.
    responses[0].wire = "4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a "
                        "3133 3a32 3120 474d 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d";
    responses[1].wire = "4803 3330 37c1 c0bf";
    responses[2].wire = "88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04 "
                        "677a 6970 7738 666f 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 "
                        "553b 206d 6178 2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e 3d31";
    sequence(responses, 3, false, 256);
    // C.6: the same responses, Huffman coded.
    responses[0].wire = "4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff6e "
                        "919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3";
    responses[1].wire = "4883 640e ffc1 c0bf";
    // "307" is three bytes plain and three coded. The RFC's encoder codes
    // it; ours codes a string only when that is shorter.
    responses[1].ours = "4803 3330 37c1 c0bf";
    responses[2].wire = "88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab 77ad 94e7 "
                        "821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed "
                        "4ee5 b106 3d50 07";
    sequence(responses, 3, true, 256);
}

// ── What the decoder refuses, and what it survives ──────────────────────

static h2_hpack_result_t decode_hex(pair_t *p, const char *text, got_t *g)
{
    uint8_t wire[512];
    size_t n = hex(text, wire);
    memset(g, 0, sizeof(*g));
    return h2_hpack_decode(&p->dec, wire, n, take, g);
}

static void decoder_cases(void)
{
    got_t g;
    pair_t *p = pair_new(4096, 4096);
    // Index 0, and an index past both tables (62 with an empty dynamic one).
    CHECK(decode_hex(p, "80", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "be", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "bd", &g) == H2_HPACK_OK && got_is(&g, 0, "www-authenticate", ""));
    CHECK(decode_hex(p, "7e 00", &g) == H2_HPACK_BAD);             // a literal named by index 62
    // A string that runs past the block; a block cut inside an integer; a
    // literal with a name and no value.
    CHECK(decode_hex(p, "00 05 6162", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "ff", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "00 01 61", &g) == H2_HPACK_BAD);
    // An index of six bytes; a string length past the most.
    CHECK(decode_hex(p, "ff 80 80 80 80 80 00", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "00 7f 80 ff ff ff 0f", &g) == H2_HPACK_BAD);
    // Huffman errors in a name and in a value: zero padding, EOS.
    CHECK(decode_hex(p, "00 81 00 01 61", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "00 01 61 84 ffff ffff", &g) == H2_HPACK_BAD);
    CHECK(decode_hex(p, "00 01 61 81 07", &g) == H2_HPACK_OK && got_is(&g, 0, "a", "0"));
    // An empty block is a block.
    CHECK(decode_hex(p, "", &g) == H2_HPACK_OK && g.n == 0);
    free(p);

    // A size update: at the start, twice even; not after a field; not past
    // what we advertised.
    p = pair_new(4096, 4096);
    CHECK(decode_hex(p, "20 3f e1 1f 82", &g) == H2_HPACK_OK && g.n == 1 && p->dec.table.most == 4096);
    CHECK(decode_hex(p, "3f 9a 0a 82", &g) == H2_HPACK_OK && p->dec.table.most == 1337);    // C.1.2's integer
    CHECK(decode_hex(p, "82 20", &g) == H2_HPACK_BAD);
    free(p);
    p = pair_new(4096, 4096);
    CHECK(decode_hex(p, "3f e2 1f", &g) == H2_HPACK_BAD);          // 4097
    free(p);

    // A size update evicts.
    p = pair_new(4096, 4096);
    CHECK(decode_hex(p, "40 01 61 01 62 40 01 63 01 64", &g) == H2_HPACK_OK && p->dec.table.count == 2);
    CHECK(decode_hex(p, "3f 03 be", &g) == H2_HPACK_OK && p->dec.table.count == 1 && got_is(&g, 0, "c", "d"));  // 34
    CHECK(decode_hex(p, "20", &g) == H2_HPACK_OK && p->dec.table.count == 0 && p->dec.table.size == 0);
    free(p);

    // We advertised less than the default: until the peer has our SETTINGS
    // it may use the default; once it has acknowledged them, its next
    // block begins by shrinking the table, or the connection is over.
    p = pair_new(100, 4096);
    CHECK(decode_hex(p, "40 01 61 01 62", &g) == H2_HPACK_OK && p->dec.table.most == 4096);
    CHECK(decode_hex(p, "3f e1 1f", &g) == H2_HPACK_OK);           // 4096, as a peer that has not heard may say
    h2_hpack_decoder_settings_acked(&p->dec);
    CHECK(decode_hex(p, "82", &g) == H2_HPACK_BAD && g.n == 0);    // and none of its fields is handed over
    free(p);
    p = pair_new(100, 4096);
    h2_hpack_decoder_settings_acked(&p->dec);
    CHECK(decode_hex(p, "3f 46 82", &g) == H2_HPACK_BAD);          // 101: not enough of a shrink
    free(p);
    p = pair_new(100, 4096);
    h2_hpack_decoder_settings_acked(&p->dec);
    CHECK(decode_hex(p, "", &g) == H2_HPACK_BAD);                  // an empty block is a block, and did not begin with it
    free(p);
    p = pair_new(100, 4096);
    h2_hpack_decoder_settings_acked(&p->dec);
    CHECK(decode_hex(p, "3f 45 82", &g) == H2_HPACK_OK && p->dec.table.most == 100);
    CHECK(decode_hex(p, "82", &g) == H2_HPACK_OK);                 // owed once
    CHECK(decode_hex(p, "3f 46", &g) == H2_HPACK_BAD);             // and 100 is the limit from here
    free(p);
    // We advertised more: a peer may grow the table to it, and no further.
    p = pair_new(65536, 4096);
    h2_hpack_decoder_settings_acked(&p->dec);
    CHECK(decode_hex(p, "82", &g) == H2_HPACK_OK && p->dec.table.most == 4096);    // until the peer says so
    CHECK(decode_hex(p, "3f e1 ff 03", &g) == H2_HPACK_OK && p->dec.table.most == 65536);
    CHECK(decode_hex(p, "3f e2 ff 03", &g) == H2_HPACK_BAD);
    free(p);

    // THE WHOLE BLOCK IS DECODED WHATEVER THE CALLER THINKS OF IT: told to
    // stop after one field, the decoder still adds the second and third to
    // its table, and the next block's indices mean what the encoder meant.
    p = pair_new(4096, 4096);
    uint8_t wire[512];
    size_t n = hex("40 01 61 01 62 40 01 63 01 64 40 01 65 01 66", wire);
    memset(&g, 0, sizeof(g));
    g.stop_after = 1;
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_REFUSED && g.n == 1 && p->dec.table.count == 3);
    CHECK(decode_hex(p, "be bf c0", &g) == H2_HPACK_OK && got_is(&g, 0, "e", "f") && got_is(&g, 1, "c", "d") &&
          got_is(&g, 2, "a", "b"));
    // A refusal on an indexed field is the same.
    memset(&g, 0, sizeof(g));
    g.stop_after = 1;
    n = hex("82 84 40 01 67 01 68", wire);
    CHECK(h2_hpack_decode(&p->dec, wire, n, take, &g) == H2_HPACK_REFUSED && g.n == 1 && p->dec.table.count == 4);
    free(p);

    // A NAME TAKEN FROM AN ENTRY THE FIELD ITSELF EVICTS (§ 4.4). A table of
    // 64 bytes holds "aaaa: bbbb" (40) alone; a second field named by index
    // 62, with a value that pushes the first out, still has its name.
    p = pair_new(4096, 4096);
    CHECK(decode_hex(p, "3f 21 40 04 61616161 04 62626262", &g) == H2_HPACK_OK && p->dec.table.count == 1);
    CHECK(decode_hex(p, "7e 08 6363636363636363", &g) == H2_HPACK_OK && got_is(&g, 0, "aaaa", "cccccccc"));
    h2_field_t kept = F("aaaa", "cccccccc");
    CHECK(table_is(&p->dec.table, &kept, 1, 44));
    // The same with another field's name left in the scratch from before.
    CHECK(decode_hex(p, "00 04 7a7a7a7a 01 79", &g) == H2_HPACK_OK && got_is(&g, 0, "zzzz", "y"));
    CHECK(decode_hex(p, "7e 08 6464646464646464", &g) == H2_HPACK_OK && got_is(&g, 0, "aaaa", "dddddddd"));
    kept = F("aaaa", "dddddddd");
    CHECK(table_is(&p->dec.table, &kept, 1, 44));
    // And the table's bytes slide: fill and turn it over many times.
    for (int i = 0; i < 2000; i++) {
        char block[32];
        snprintf(block, sizeof(block), "7e 08 %016x", (unsigned)i);
        CHECK(decode_hex(p, block, &g) == H2_HPACK_OK && g.n == 1 && strcmp(g.name[0], "aaaa") == 0);
    }
    CHECK(p->dec.table.count == 1 && p->dec.table.size == 44);
    // A field that fits the scratch and not the table (33 bytes of value,
    // 66 with its name and the overhead, against 64) empties the table and
    // is handed over all the same.
    CHECK(decode_hex(p, "40 01 78 21 616161616161616161616161616161616161616161616161616161616161616161", &g) ==
          H2_HPACK_OK && g.n == 1 && g.value_len[0] == 33 && p->dec.table.count == 0 && p->dec.table.size == 0);
    free(p);

    // A FIELD LARGER THAN THE SCRATCH is one the table could not hold
    // either: the table is emptied, as adding it would, the block is read to
    // its end, and the head is refused. A decoder with the least room it may
    // have (4096 each) makes that reachable.
    // (A long copy is libos64's own memcpy, in assembly the sanitizer does
    // not watch, so the scratch has bytes after it that must still be there.)
    h2_hpack_decoder_t small;
    static uint32_t store[H2_HPACK_STORAGE(4096) / 4 + 1];
    static struct {
        uint8_t bytes[4096], after[64];
    } room;
    static uint8_t big[6000], untouched[64];
    uint8_t *scratch = room.bytes;
    memset(room.after, 0xa5, sizeof(room.after));
    memset(untouched, 0xa5, sizeof(untouched));
    CHECK(h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes), 4096));
    size_t at = 0;
    at += hex("40 01 61 01 62", big);                      // a: b, into the table
    big[at++] = 0x40;                                      // then x: <5000 bytes>, with indexing
    big[at++] = 0x01;
    big[at++] = 'x';
    at = (size_t)(int_write(big + at, 7, 0, 5000) - big);
    memset(big + at, 'v', 5000);
    at += 5000;
    big[at++] = 0x82;                                      // and a field after it
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&small, big, at, take, &g) == H2_HPACK_REFUSED && g.n == 1 && got_is(&g, 0, "a", "b"));
    CHECK(small.table.count == 0 && small.table.size == 0);
    // Without indexing, the table is left alone.
    CHECK(h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes), 4096));
    big[5] = 0x00;
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&small, big, at, take, &g) == H2_HPACK_REFUSED && small.table.count == 1);
    // A name too large by itself, Huffman coded: still read to its end.
    CHECK(h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes), 4096));
    at = hex("40 01 61 01 62", big);                       // something in the table to lose
    big[at++] = 0x40;
    uint8_t *q = int_write(big + at, 7, 0x80, 3125);       // 5000 '0's are 25000 bits
    memset(q, 0x00, 3125);
    at = (size_t)(q - big) + 3125;
    at += hex("01 62 82", big + at);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&small, big, at, take, &g) == H2_HPACK_REFUSED && g.n == 1 && small.table.count == 0);
    // A name and a value that each fit the scratch and do not fit it
    // together: 100 bytes and 4050 against 4096.
    CHECK(h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes), 4096));
    at = hex("40 01 61 01 62", big);
    big[at++] = 0x40;
    q = int_write(big + at, 7, 0, 100);
    memset(q, 'n', 100);
    q = int_write(q + 100, 7, 0, 4050);
    memset(q, 'v', 4050);
    at = (size_t)(q - big) + 4050;
    big[at++] = 0x82;
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&small, big, at, take, &g) == H2_HPACK_REFUSED && g.n == 1 && small.table.count == 0);
    CHECK(memcmp(room.after, untouched, sizeof(untouched)) == 0);     // nothing was written past the scratch

    // Storage too small, misaligned, or a scratch too small, is refused.
    CHECK(!h2_hpack_decoder_init(&small, store, sizeof(store) - 8, scratch, sizeof(room.bytes), 4096));
    CHECK(!h2_hpack_decoder_init(&small, (uint8_t *)store + 1, sizeof(store) - 1, scratch, sizeof(room.bytes), 100));
    CHECK(!h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes) - 1, 4096));
    CHECK(!h2_hpack_decoder_init(&small, store, sizeof(store), scratch, sizeof(room.bytes), 4097));
}

// ── The encoder's table following the peer ──────────────────────────────

static void encoder_cases(void)
{
    uint8_t out[512];
    size_t len;
    got_t g;
    h2_field_t ab = F("a", "b"), cd = F("c", "d");
    pair_t *p = pair_new(4096, 4096);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && h2_hpack_encode(&p->enc, &cd, 1, out, sizeof(out), &len));
    CHECK(p->enc.table.count == 2);
    // The peer shrinks its table to 34 bytes: one field goes at once, and
    // the next block opens by saying so.
    h2_hpack_encoder_peer_table_size(&p->enc, 34);
    CHECK(p->enc.table.count == 1 && p->enc.table.most == 34);
    CHECK(h2_hpack_encode(&p->enc, &cd, 1, out, sizeof(out), &len) && len == 3);
    CHECK(out[0] == 0x3f && out[1] == 0x03 && out[2] == 0xbe);
    // Said once.
    CHECK(h2_hpack_encode(&p->enc, &cd, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0xbe);
    // Down to nothing and back up between two blocks: the smallest, then
    // where it is now. The table was emptied on the way.
    h2_hpack_encoder_peer_table_size(&p->enc, 0);
    h2_hpack_encoder_peer_table_size(&p->enc, 100);
    CHECK(p->enc.table.count == 0);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && len == 1 + 2 + 5);
    CHECK(out[0] == 0x20 && out[1] == 0x3f && out[2] == 100 - 31 && out[3] == 0x40);
    // The same size again is no change and says nothing.
    h2_hpack_encoder_peer_table_size(&p->enc, 100);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0xbe);
    // More than this encoder's ceiling: it goes to its ceiling and says that.
    h2_hpack_encoder_peer_table_size(&p->enc, 1u << 20);
    CHECK(p->enc.table.most == 4096);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && len == 4 && out[0] == 0x3f && out[3] == 0xbe);
    free(p);

    // An encoder whose ceiling is under the default says so in its first
    // block, since the peer's decoder starts at the default.
    p = pair_new(4096, 64);
    CHECK(p->enc.table.most == 64);
    h2_field_t first = F("aaaa", "bbbb"), second = F("aaaa", "cccccccc");
    CHECK(h2_hpack_encode(&p->enc, &first, 1, out, sizeof(out), &len) && out[0] == 0x3f && out[1] == 64 - 31);
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, out, len, take, &g) == H2_HPACK_OK && p->dec.table.most == 64);
    // The encoder's own name-from-an-evicted-entry: the name goes as index
    // 62, and the entry it names is the one pushed out.
    CHECK(h2_hpack_encode(&p->enc, &second, 1, out, sizeof(out), &len) && out[0] == 0x7e);
    CHECK(table_is(&p->enc.table, &second, 1, 44));
    memset(&g, 0, sizeof(g));
    CHECK(h2_hpack_decode(&p->dec, out, len, take, &g) == H2_HPACK_OK && got_is(&g, 0, "aaaa", "cccccccc"));
    // A field the table cannot hold is sent without indexing and the table
    // keeps what it had.
    h2_field_t large = F("x", "0123456789012345678901234567890123456789");
    CHECK(h2_hpack_encode(&p->enc, &large, 1, out, sizeof(out), &len) && out[0] == 0x00);
    CHECK(table_is(&p->enc.table, &second, 1, 44));
    free(p);

    // A table of nothing: every field is literal, and nothing is kept.
    p = pair_new(4096, 0);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && out[0] == 0x20 && out[1] == 0x00);
    CHECK(h2_hpack_encode(&p->enc, &ab, 1, out, sizeof(out), &len) && out[0] == 0x00 && p->enc.table.count == 0);
    free(p);

    // Huffman only when shorter: "a" is one byte either way and stays plain;
    // a run of a symbol with a long code stays plain too.
    p = pair_new(4096, 4096);
    h2_field_t odd = F("x-odd", "\\\\\\\\");
    CHECK(h2_hpack_encode(&p->enc, &odd, 1, out, sizeof(out), &len));
    CHECK(out[0] == 0x40 && out[1] == 0x84 && out[6] == 0x04 && memcmp(out + 7, "\\\\\\\\", 4) == 0);
    // The static table is searched for the whole field before the dynamic
    // one, and a name is found in either.
    h2_field_t gz = F("accept-encoding", "gzip, deflate"), br = F("accept-encoding", "br");
    CHECK(h2_hpack_encode(&p->enc, &gz, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0x90);
    CHECK(h2_hpack_encode(&p->enc, &br, 1, out, sizeof(out), &len) && out[0] == 0x50);
    CHECK(h2_hpack_encode(&p->enc, &odd, 1, out, sizeof(out), &len) && len == 1 && out[0] == 0xbf);
    h2_field_t odd2 = F("x-odd", "z");
    CHECK(h2_hpack_encode(&p->enc, &odd2, 1, out, sizeof(out), &len) && out[0] == 0x7f && out[1] == 0x00);
    // No room for the worst case: nothing written, the table untouched.
    h2_field_t fresh = F("fresh", "field");
    uint32_t before = p->enc.table.count;
    size_t bound = h2_hpack_encode_bound(&fresh, 1);
    out[0] = 0x55;
    CHECK(bound == 12 + 13 + 10 && !h2_hpack_encode(&p->enc, &fresh, 1, out, bound - 1, &len) && len == 0);
    CHECK(out[0] == 0x55 && p->enc.table.count == before);
    CHECK(h2_hpack_encode(&p->enc, &fresh, 1, out, bound, &len) && p->enc.table.count == before + 1);
    free(p);
}

// ── The rules on a field list ───────────────────────────────────────────

static h2_head_rule_t head(const h2_field_t *fields, size_t n, h2_head_kind_t kind, size_t want_at)
{
    size_t at = 999;
    h2_head_rule_t rule = h2_head_check(fields, n, kind, &at);
    CHECK(at == want_at);
    CHECK(h2_head_check(fields, n, kind, NULL) == rule);
    return rule;
}

static void head_cases(void)
{
    h2_field_t request[] = {F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F(":authority", "a.test"),
                            F("accept", "*/*"), F("te", "trailers"), F("content-length", "18446744073709551615")};
    CHECK(head(request, 7, H2_HEAD_REQUEST, 7) == H2_HEAD_OK);
    CHECK(head(request, 3, H2_HEAD_REQUEST, 3) == H2_HEAD_OK);                // no authority is allowed
    CHECK(head(request, 2, H2_HEAD_REQUEST, 2) == H2_HEAD_PSEUDO_MISSING);
    CHECK(head(request + 1, 6, H2_HEAD_REQUEST, 6) == H2_HEAD_PSEUDO_MISSING);
    h2_field_t response[] = {F(":status", "200"), F("content-type", "text/html"), F("set-cookie", "a=b"),
                             F("set-cookie", "c=d")};
    CHECK(head(response, 4, H2_HEAD_RESPONSE, 4) == H2_HEAD_OK);
    CHECK(head(response + 1, 3, H2_HEAD_RESPONSE, 3) == H2_HEAD_PSEUDO_MISSING);
    CHECK(head(response + 1, 3, H2_HEAD_TRAILERS, 3) == H2_HEAD_OK);
    CHECK(head(response, 4, H2_HEAD_TRAILERS, 0) == H2_HEAD_PSEUDO_UNKNOWN);
    CHECK(head(NULL, 0, H2_HEAD_TRAILERS, 0) == H2_HEAD_OK);

    // One rule at a time, each on an otherwise good head.
#define ONE(kind_, rule_, at_, ...)                                           \
    do {                                                                      \
        h2_field_t list[] = {__VA_ARGS__};                                    \
        CHECK(head(list, sizeof(list) / sizeof(list[0]), kind_, at_) == rule_); \
    } while (0)
    h2_field_t empty_name = F("", "x");
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_EMPTY, 1, F(":status", "200"), empty_name);
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_BYTES, 1, F(":status", "200"), F("Content-Type", "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_BYTES, 1, F(":status", "200"), F("a b", "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_BYTES, 1, F(":status", "200"), F("a:b", "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_BYTES, 1, F(":status", "200"), F("a\x7f", "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_NAME_BYTES, 1, F(":status", "200"), F("caf\xc3\xa9", "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_OK, 2, F(":status", "200"), F("a", "x y\tz \x80"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_OK, 2, F(":status", "200"), F("a", ""));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), F("a", "x\ry"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), F("a", "x\ny"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), F("a", " x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), F("a", "x\t"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), F("a", " "));
    h2_field_t nul = F("a", "x");
    nul.value = (const uint8_t *)"x\0y";
    nul.value_len = 3;
    ONE(H2_HEAD_RESPONSE, H2_HEAD_VALUE_BYTES, 1, F(":status", "200"), nul);
    ONE(H2_HEAD_RESPONSE, H2_HEAD_PSEUDO_UNKNOWN, 0, F(":method", "GET"), F(":status", "200"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_PSEUDO_UNKNOWN, 1, F(":status", "200"), F(":statu", "200"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_PSEUDO_UNKNOWN, 0, F(":status", "200"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_PSEUDO_UNKNOWN, 0, F(":protocol", "websocket"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_PSEUDO_LATE, 1, F("a", "b"), F(":status", "200"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_PSEUDO_LATE, 3, F(":method", "GET"), F(":scheme", "https"), F("a", "b"),
        F(":path", "/"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_PSEUDO_TWICE, 1, F(":status", "200"), F(":status", "200"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_PSEUDO_TWICE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"),
        F(":path", "/x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_STATUS, 0, F(":status", "20"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_STATUS, 0, F(":status", "2000"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_STATUS, 0, F(":status", "2x0"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_STATUS, 0, F(":status", "20:"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_STATUS, 0, F(":status", "/00"));
    static const char *const kGone[] = {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"};
    for (size_t i = 0; i < 5; i++)
        ONE(H2_HEAD_RESPONSE, H2_HEAD_CONNECTION_FIELD, 1, F(":status", "200"), F(kGone[i], "x"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_OK, 2, F(":status", "200"), F("connections", "x"));
    ONE(H2_HEAD_TRAILERS, H2_HEAD_CONNECTION_FIELD, 0, F("upgrade", "x"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "gzip"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_OK, 4, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "Trailers"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_OK, 4, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "TRAILERS"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "trailerz"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "trailer"));
    // Seven bytes of it, with the eighth lying just past the value.
    h2_field_t cut = F("te", "trailers");
    cut.value_len = 7;
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), cut);
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("te", "TRAILER\x13"));
    ONE(H2_HEAD_REQUEST, H2_HEAD_TE, 3, F(":method", "GET"), F(":scheme", "https"), F(":path", "/"),
        F("te", "trailers, gzip"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", ""));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "12a"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "-1"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "-"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "0+"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "18446744073709551616"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_CONTENT_LENGTH, 1, F(":status", "200"), F("content-length", "99999999999999999999"));
    ONE(H2_HEAD_RESPONSE, H2_HEAD_OK, 2, F(":status", "200"), F("content-length", "0"));
#undef ONE
}

// ── Frames ──────────────────────────────────────────────────────────────

// A frame from hex: the nine bytes and the payload, parsed against `max`.
static bool parse_hex(const char *text, uint32_t max, h2_frame_t *frame, h2_fault_t *fault)
{
    static uint8_t wire[70000];
    size_t n = hex(text, wire);
    h2_frame_header_t header;
    h2_frame_header_read(wire, &header);
    CHECK(n == H2_FRAME_HEADER_LEN + header.length);
    return h2_frame_parse(&header, wire + H2_FRAME_HEADER_LEN, max, frame, fault);
}

static bool faults(const char *text, uint32_t error, h2_rule_t rule, bool stream)
{
    h2_frame_t frame;
    h2_fault_t fault;
    return !parse_hex(text, H2_FRAME_SIZE_LEAST, &frame, &fault) && fault.error == error && fault.rule == rule &&
           fault.stream == stream;
}

static void frame_cases(void)
{
    h2_frame_t f;
    h2_fault_t fault;
    h2_frame_header_t h;
    uint8_t nine[9];

    // The header: 24 bits of length, and the stream's reserved bit dropped
    // on the way in and never set on the way out.
    static const uint8_t kHeader[] = {0x01, 0x02, 0x03, 0x09, 0x05, 0xff, 0xff, 0xff, 0xfe};
    h2_frame_header_read(kHeader, &h);
    CHECK(h.length == 0x010203 && h.type == 9 && h.flags == 5 && h.stream == 0x7ffffffe);
    h.stream = 0xfffffffe;
    h2_frame_header_write(nine, &h);
    CHECK(memcmp(nine, kHeader, 5) == 0 && nine[5] == 0x7f && nine[8] == 0xfe);
    CHECK(memcmp(h2_preface, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", H2_PREFACE_LEN) == 0);

    // Longer than the size the receiver set, whatever the type; judged from
    // the header alone too.
    h = (h2_frame_header_t){.length = H2_FRAME_SIZE_LEAST + 1, .type = H2_FRAME_DATA, .stream = 1};
    CHECK(!h2_frame_header_check(&h, H2_FRAME_SIZE_LEAST, &fault) && fault.error == H2_ERR_FRAME_SIZE &&
          fault.rule == H2_RULE_TOO_LONG && !fault.stream);
    CHECK(h2_frame_header_check(&h, H2_FRAME_SIZE_LEAST + 1, &fault));
    h.type = 0x42;
    CHECK(!h2_frame_parse(&h, NULL, H2_FRAME_SIZE_LEAST, &f, &fault) && fault.rule == H2_RULE_TOO_LONG);

    // DATA: its bytes, END_STREAM, and what flow control counts.
    CHECK(parse_hex("000003 00 01 00000001 616263", 16384, &f, &fault) && f.kind == H2_FRAME_DATA && f.stream == 1);
    CHECK(f.data_len == 3 && memcmp(f.data, "abc", 3) == 0 && f.flow_len == 3 && (f.flags & H2_FLAG_END_STREAM));
    CHECK(parse_hex("000000 00 00 00000001", 16384, &f, &fault) && f.data_len == 0 && f.flow_len == 0);
    CHECK(faults("000003 00 00 00000000 616263", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));
    // Padded: a length byte, the data, the padding. The whole payload is
    // what the window is charged.
    CHECK(parse_hex("000006 00 08 00000001 02 616263 0000", 16384, &f, &fault) && f.data_len == 3 && f.flow_len == 6 &&
          memcmp(f.data, "abc", 3) == 0);
    CHECK(parse_hex("000001 00 08 00000001 00", 16384, &f, &fault) && f.data_len == 0 && f.flow_len == 1);
    CHECK(parse_hex("000003 00 08 00000001 02 0000", 16384, &f, &fault) && f.data_len == 0);
    CHECK(faults("000003 00 08 00000001 03 0000", H2_ERR_PROTOCOL, H2_RULE_PADDING, false));
    CHECK(faults("000001 00 08 00000001 01", H2_ERR_PROTOCOL, H2_RULE_PADDING, false));
    CHECK(faults("000000 00 08 00000001", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));

    // HEADERS: the fragment, behind padding and the priority fields.
    CHECK(parse_hex("000001 01 05 00000003 82", 16384, &f, &fault) && f.kind == H2_FRAME_HEADERS && f.stream == 3 &&
          f.data_len == 1 && f.data[0] == 0x82 && !f.has_priority);
    CHECK(parse_hex("000006 01 24 00000003 80000005 ff 82", 16384, &f, &fault) && f.has_priority && f.exclusive &&
          f.depends_on == 5 && f.weight == 255 && f.data_len == 1 && f.data[0] == 0x82);
    CHECK(parse_hex("000009 01 2c 00000003 02 00000005 10 82 0000", 16384, &f, &fault) && f.has_priority &&
          !f.exclusive && f.depends_on == 5 && f.weight == 16 && f.data_len == 1 && f.data[0] == 0x82);
    CHECK(parse_hex("000000 01 04 00000003", 16384, &f, &fault) && f.data_len == 0);
    CHECK(faults("000001 01 04 00000000 82", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));
    CHECK(faults("000004 01 20 00000003 00000005", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000002 01 08 00000003 02 00", H2_ERR_PROTOCOL, H2_RULE_PADDING, false));
    CHECK(faults("000000 01 08 00000003", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    // The priority fields are judged before the padding is: a frame that
    // holds them, with padding longer than what is left for the block, is
    // padded wrongly; one too short to hold them has the wrong length.
    CHECK(faults("000006 01 28 00000003 01 00000005 00", H2_ERR_PROTOCOL, H2_RULE_PADDING, false));
    CHECK(faults("000005 01 28 00000003 00 00000005", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(parse_hex("000007 01 28 00000003 01 00000005 10 00", 16384, &f, &fault) && f.data_len == 0 && f.weight == 16);

    // PRIORITY: five bytes, and the one wrong length that is the stream's.
    CHECK(parse_hex("000005 02 00 00000003 80000001 07", 16384, &f, &fault) && f.kind == H2_FRAME_PRIORITY &&
          f.has_priority && f.exclusive && f.depends_on == 1 && f.weight == 7 && f.data_len == 0);
    CHECK(faults("000004 02 00 00000003 80000001", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, true));
    CHECK(faults("000006 02 00 00000003 80000001 0700", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, true));
    CHECK(faults("000005 02 00 00000000 80000001 07", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));

    // RST_STREAM.
    CHECK(parse_hex("000004 03 00 00000005 00000008", 16384, &f, &fault) && f.kind == H2_FRAME_RST_STREAM &&
          f.stream == 5 && f.value == H2_ERR_CANCEL);
    CHECK(faults("000003 03 00 00000005 000000", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000005 03 00 00000005 0000000800", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000004 03 00 00000000 00000008", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));

    // SETTINGS: entries in order; an acknowledgement is empty.
    CHECK(parse_hex("00000c 04 00 00000000 0003 00000064 0004 00010000", 16384, &f, &fault) &&
          f.kind == H2_FRAME_SETTINGS && h2_settings_count(&f) == 2);
    CHECK(h2_settings_at(&f, 0).identifier == H2_SETTING_MAX_CONCURRENT_STREAMS && h2_settings_at(&f, 0).value == 100);
    CHECK(h2_settings_at(&f, 1).identifier == H2_SETTING_INITIAL_WINDOW_SIZE && h2_settings_at(&f, 1).value == 65536);
    CHECK(parse_hex("000000 04 00 00000000", 16384, &f, &fault) && h2_settings_count(&f) == 0);
    CHECK(parse_hex("000000 04 01 00000000", 16384, &f, &fault) && (f.flags & H2_FLAG_ACK));
    CHECK(faults("000006 04 01 00000000 0003 00000064", H2_ERR_FRAME_SIZE, H2_RULE_SETTINGS_ACK, false));
    CHECK(faults("000005 04 00 00000000 0003 000000", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000007 04 00 00000000 0003 00000064 00", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000000 04 00 00000001", H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false));
    CHECK(parse_hex("000006 01 04 00000003 828684828684", 16384, &f, &fault) && h2_settings_count(&f) == 0);  // not a SETTINGS
    CHECK(parse_hex("000006 04 00 00000000 0102 00000007", 16384, &f, &fault) && h2_settings_at(&f, 0).identifier == 0x0102 &&
          h2_settings_at(&f, 0).value == 7);
    // What a setting's value may be.
    CHECK(h2_setting_check((h2_setting_t){H2_SETTING_ENABLE_PUSH, 1}, &fault));
    CHECK(!h2_setting_check((h2_setting_t){H2_SETTING_ENABLE_PUSH, 2}, &fault) && fault.error == H2_ERR_PROTOCOL &&
          fault.rule == H2_RULE_SETTING_VALUE && !fault.stream);
    CHECK(h2_setting_check((h2_setting_t){H2_SETTING_INITIAL_WINDOW_SIZE, 0x7fffffff}, &fault));
    CHECK(!h2_setting_check((h2_setting_t){H2_SETTING_INITIAL_WINDOW_SIZE, 0x80000000}, &fault) &&
          fault.error == H2_ERR_FLOW_CONTROL);
    CHECK(h2_setting_check((h2_setting_t){H2_SETTING_MAX_FRAME_SIZE, 16384}, &fault));
    CHECK(h2_setting_check((h2_setting_t){H2_SETTING_MAX_FRAME_SIZE, 16777215}, &fault));
    CHECK(!h2_setting_check((h2_setting_t){H2_SETTING_MAX_FRAME_SIZE, 16383}, &fault) && fault.error == H2_ERR_PROTOCOL);
    CHECK(!h2_setting_check((h2_setting_t){H2_SETTING_MAX_FRAME_SIZE, 16777216}, &fault));
    CHECK(h2_setting_check((h2_setting_t){H2_SETTING_HEADER_TABLE_SIZE, 0xffffffff}, &fault));
    CHECK(h2_setting_check((h2_setting_t){0x4242, 0xffffffff}, &fault));                          // one we do not know

    // PUSH_PROMISE: the promised stream and the fragment.
    CHECK(parse_hex("000005 05 04 00000001 80000002 82", 16384, &f, &fault) && f.kind == H2_FRAME_PUSH_PROMISE &&
          f.value == 2 && f.data_len == 1 && f.data[0] == 0x82);
    CHECK(parse_hex("000007 05 0c 00000001 01 00000002 82 00", 16384, &f, &fault) && f.value == 2 && f.data_len == 1);
    CHECK(faults("000003 05 04 00000001 000000", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000005 05 0c 00000001 01 00000002", H2_ERR_PROTOCOL, H2_RULE_PADDING, false));   // the id fits; the padding does not
    CHECK(faults("000004 05 0c 00000001 00 000000", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(parse_hex("000006 05 0c 00000001 01 00000002 00", 16384, &f, &fault) && f.value == 2 && f.data_len == 0);
    CHECK(faults("000005 05 04 00000000 00000002 82", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));

    // PING.
    CHECK(parse_hex("000008 06 01 00000000 0102030405060708", 16384, &f, &fault) && f.kind == H2_FRAME_PING &&
          (f.flags & H2_FLAG_ACK) && f.data_len == 8 && f.data[7] == 8);
    CHECK(faults("000007 06 00 00000000 01020304050607", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000009 06 00 00000000 010203040506070809", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000008 06 00 00000001 0102030405060708", H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false));

    // GOAWAY: the last stream, the code, and whatever it has to say.
    CHECK(parse_hex("00000a 07 00 00000000 80000007 0000000b 6869", 16384, &f, &fault) && f.kind == H2_FRAME_GOAWAY &&
          f.last_stream == 7 && f.value == H2_ERR_ENHANCE_YOUR_CALM && f.data_len == 2 && memcmp(f.data, "hi", 2) == 0);
    CHECK(parse_hex("000008 07 00 00000000 00000000 00000000", 16384, &f, &fault) && f.data_len == 0);
    CHECK(faults("000007 07 00 00000000 00000000 000000", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000008 07 00 00000001 00000000 00000000", H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false));

    // WINDOW_UPDATE: the connection's or a stream's, and nothing is whose
    // error it would have been.
    CHECK(parse_hex("000004 08 00 00000000 80010000", 16384, &f, &fault) && f.kind == H2_FRAME_WINDOW_UPDATE &&
          f.stream == 0 && f.value == 65536);
    CHECK(parse_hex("000004 08 00 00000003 7fffffff", 16384, &f, &fault) && f.stream == 3 && f.value == 0x7fffffff);
    CHECK(faults("000004 08 00 00000000 00000000", H2_ERR_PROTOCOL, H2_RULE_WINDOW_ZERO, false));
    CHECK(faults("000004 08 00 00000003 80000000", H2_ERR_PROTOCOL, H2_RULE_WINDOW_ZERO, true));
    CHECK(faults("000003 08 00 00000003 000001", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));
    CHECK(faults("000005 08 00 00000000 0000000100", H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false));

    // CONTINUATION.
    CHECK(parse_hex("000002 09 04 00000003 8284", 16384, &f, &fault) && f.kind == H2_FRAME_CONTINUATION &&
          f.data_len == 2);
    CHECK(faults("000002 09 04 00000000 8284", H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false));

    // A type we do not know: no fault, on a stream or off one, any flags.
    CHECK(parse_hex("000003 0a ff 00000000 010203", 16384, &f, &fault) && f.kind == H2_FRAME_UNKNOWN && f.type == 0x0a);
    CHECK(parse_hex("000000 fe 00 00000009", 16384, &f, &fault) && f.kind == H2_FRAME_UNKNOWN && f.stream == 9);

    // The names.
    CHECK(strcmp(h2_error_name(0), "NO_ERROR") == 0 && strcmp(h2_error_name(7), "REFUSED_STREAM") == 0);
    CHECK(strcmp(h2_error_name(13), "HTTP_1_1_REQUIRED") == 0 && strcmp(h2_error_name(14), "UNKNOWN_ERROR") == 0);
    CHECK(strcmp(h2_error_name(0xffffffff), "UNKNOWN_ERROR") == 0);
    CHECK(strcmp(h2_setting_name(1), "HEADER_TABLE_SIZE") == 0 && strcmp(h2_setting_name(6), "MAX_HEADER_LIST_SIZE") == 0);
    CHECK(strcmp(h2_setting_name(0), "UNKNOWN_SETTING") == 0 && strcmp(h2_setting_name(7), "UNKNOWN_SETTING") == 0);
}

// ── Writers ─────────────────────────────────────────────────────────────

// Parse the frame at `wire`, which a writer just made; its whole length.
static size_t reparse(const uint8_t *wire, uint32_t max, h2_frame_t *frame)
{
    h2_frame_header_t header;
    h2_fault_t fault;
    h2_frame_header_read(wire, &header);
    CHECK(h2_frame_parse(&header, wire + H2_FRAME_HEADER_LEN, max, frame, &fault));
    return H2_FRAME_HEADER_LEN + header.length;
}

static void writer_cases(void)
{
    static uint8_t out[70000], block[40000], back[40000];
    h2_frame_t f;
    size_t n, taken;
    uint8_t want[64];

    // SETTINGS in the order given, which is not the identifiers' order.
    h2_setting_t greeting[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 6291456}, {H2_SETTING_HEADER_TABLE_SIZE, 65536},
                               {H2_SETTING_ENABLE_PUSH, 0}};
    n = h2_write_settings(out, sizeof(out), greeting, 3);
    CHECK(n == hex("000012 04 00 00000000 0004 00600000 0001 00010000 0002 00000000", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_settings(out, 26, greeting, 3) == 0 && h2_write_settings(out, 27, greeting, 3) == 27);
    CHECK(h2_write_settings(out, sizeof(out), NULL, 0) == 9 && reparse(out, 16384, &f) == 9 && h2_settings_count(&f) == 0);
    n = h2_write_settings_ack(out, sizeof(out));
    CHECK(n == hex("000000 04 01 00000000", want) && memcmp(out, want, n) == 0 && h2_write_settings_ack(out, 8) == 0);

    n = h2_write_window_update(out, sizeof(out), 0, 15663105);
    CHECK(n == hex("000004 08 00 00000000 00ef0001", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_window_update(out, sizeof(out), 5, 0x7fffffff) == 13 && reparse(out, 16384, &f) == 13 && f.stream == 5);
    CHECK(h2_write_window_update(out, 12, 0, 1) == 0 && h2_write_window_update(out, sizeof(out), 0, 0) == 0);
    CHECK(h2_write_window_update(out, sizeof(out), 0, 0x80000000) == 0);
    CHECK(h2_write_window_update(out, sizeof(out), 0x80000000, 1) == 0);

    static const uint8_t kOpaque[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    n = h2_write_ping(out, sizeof(out), true, kOpaque);
    CHECK(n == hex("000008 06 01 00000000 0102030405060708", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_ping(out, sizeof(out), false, kOpaque) == 17 && out[4] == 0 && h2_write_ping(out, 16, true, kOpaque) == 0);

    n = h2_write_rst_stream(out, sizeof(out), 7, H2_ERR_CANCEL);
    CHECK(n == hex("000004 03 00 00000007 00000008", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_rst_stream(out, 12, 7, 8) == 0 && h2_write_rst_stream(out, sizeof(out), 0, 8) == 0);
    CHECK(h2_write_rst_stream(out, sizeof(out), 0x80000001, 8) == 0);

    n = h2_write_goaway(out, sizeof(out), 9, H2_ERR_PROTOCOL, (const uint8_t *)"why", 3);
    CHECK(n == hex("00000b 07 00 00000000 00000009 00000001 776879", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_goaway(out, sizeof(out), 0, 0, NULL, 0) == 17 && h2_write_goaway(out, 16, 0, 0, NULL, 0) == 0);
    CHECK(h2_write_goaway(out, 19, 0, 0, (const uint8_t *)"why", 3) == 0);
    CHECK(h2_write_goaway(out, sizeof(out), 0x80000000, 0, NULL, 0) == 0);

    n = h2_write_priority(out, sizeof(out), 3, 1, true, 200);
    CHECK(n == hex("000005 02 00 00000003 80000001 c8", want) && memcmp(out, want, n) == 0);
    CHECK(h2_write_priority(out, 13, 3, 1, true, 200) == 0 && h2_write_priority(out, sizeof(out), 0, 1, true, 200) == 0);
    CHECK(h2_write_priority(out, sizeof(out), 3, 0x80000000, false, 0) == 0);

    // HEADERS: one frame while the block fits the peer's frame size, and
    // CONTINUATIONs after it, END_HEADERS on the last and nowhere else.
    for (size_t i = 0; i < sizeof(block); i++)
        block[i] = (uint8_t)(i * 31 + 7);
    static const size_t kSizes[] = {0, 1, 16379, 16380, 16383, 16384, 16385, 32768, 32769, 40000};
    for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++)
        for (int with = 0; with < 2; with++) {
            h2_priority_t pri = {.depends_on = 11, .exclusive = true, .weight = 41};
            size_t len = kSizes[s], want_len = h2_headers_size(len, 16384, with);
            n = h2_write_headers(out, sizeof(out), 5, block, len, s % 2 == 0, with ? &pri : NULL, 16384);
            CHECK(n == want_len && n != 0);
            CHECK(h2_write_headers(out + n, n - 1, 5, block, len, false, with ? &pri : NULL, 16384) == 0);
            // Read back as a peer would, through the reassembler.
            h2_block_t b;
            h2_fault_t fault;
            h2_block_init(&b, back, sizeof(back));
            size_t at = 0, frames = 0;
            h2_block_step_t step = H2_BLOCK_MORE;
            while (at < n && step == H2_BLOCK_MORE) {
                at += reparse(out + at, 16384, &f);
                CHECK(f.kind == (frames == 0 ? H2_FRAME_HEADERS : H2_FRAME_CONTINUATION) && f.stream == 5);
                CHECK(frames != 0 || (f.has_priority == (with != 0) && (!with || (f.depends_on == 11 && f.exclusive &&
                                                                               f.weight == 41))));
                CHECK(frames != 0 || ((f.flags & H2_FLAG_END_STREAM) != 0) == (s % 2 == 0));
                CHECK(frames == 0 || !(f.flags & H2_FLAG_END_STREAM));
                step = h2_block_feed(&b, &f, &fault);
                frames++;
            }
            CHECK(step == H2_BLOCK_DONE && at == n && b.len == len && memcmp(back, block, len) == 0);
            size_t first = with ? 16379 : 16384;
            CHECK(frames == 1 + (len > first ? (len - first + 16383) / 16384 : 0));
        }
    // A larger peer frame size is used when the peer set one.
    n = h2_write_headers(out, sizeof(out), 5, block, 40000, false, NULL, 65536);
    CHECK(n == 40009 && reparse(out, 65536, &f) == n && (f.flags & H2_FLAG_END_HEADERS));
    CHECK(h2_write_headers(out, sizeof(out), 0, block, 10, false, NULL, 16384) == 0);
    CHECK(h2_write_headers(out, sizeof(out), 5, block, 10, false, NULL, 16383) == 0);
    CHECK(h2_headers_size(10, 16777216, false) == 0);

    // DATA, cut to what the caller allows.
    n = h2_write_data(out, sizeof(out), 3, block, 10, 4, true, &taken);
    CHECK(n == 13 && taken == 4 && reparse(out, 16384, &f) == 13 && f.kind == H2_FRAME_DATA && f.data_len == 4 &&
          !(f.flags & H2_FLAG_END_STREAM) && memcmp(f.data, block, 4) == 0);
    n = h2_write_data(out, sizeof(out), 3, block + 4, 6, 16384, true, &taken);
    CHECK(n == 15 && taken == 6 && reparse(out, 16384, &f) == 15 && (f.flags & H2_FLAG_END_STREAM));
    n = h2_write_data(out, sizeof(out), 3, block, 6, 16384, false, &taken);
    CHECK(n == 15 && reparse(out, 16384, &f) == 15 && !(f.flags & H2_FLAG_END_STREAM));
    n = h2_write_data(out, sizeof(out), 3, NULL, 0, 16384, true, &taken);      // the empty frame that ends a stream
    CHECK(n == 9 && taken == 0 && reparse(out, 16384, &f) == 9 && (f.flags & H2_FLAG_END_STREAM) && f.data_len == 0);
    taken = 99;
    CHECK(h2_write_data(out, 12, 3, block, 10, 4, true, &taken) == 0 && taken == 0);
    CHECK(h2_write_data(out, sizeof(out), 0, block, 10, 4, true, &taken) == 0);
}

// ── A field block across frames ─────────────────────────────────────────

static h2_block_step_t feed_hex(h2_block_t *b, const char *text, h2_fault_t *fault)
{
    static uint8_t wire[4096];
    h2_frame_t f;
    size_t n = hex(text, wire);
    h2_frame_header_t header;
    h2_frame_header_read(wire, &header);
    CHECK(n == H2_FRAME_HEADER_LEN + header.length);
    CHECK(h2_frame_parse(&header, wire + H2_FRAME_HEADER_LEN, 16384, &f, fault));
    return h2_block_feed(b, &f, fault);
}

static void block_cases(void)
{
    uint8_t room[16];
    h2_block_t b;
    h2_fault_t fault;
    h2_block_init(&b, room, sizeof(room));

    // One frame, two, three; a frame that is none of its business between blocks.
    CHECK(feed_hex(&b, "000003 01 05 00000001 828684", &fault) == H2_BLOCK_DONE && b.len == 3 && b.first.stream == 1 &&
          b.first.kind == H2_FRAME_HEADERS && (b.first.flags & H2_FLAG_END_STREAM) && memcmp(room, "\x82\x86\x84", 3) == 0);
    CHECK(feed_hex(&b, "000003 00 00 00000001 616263", &fault) == H2_BLOCK_NOT_MINE);
    CHECK(feed_hex(&b, "000000 04 01 00000000", &fault) == H2_BLOCK_NOT_MINE);
    CHECK(feed_hex(&b, "000003 0b 00 00000001 616263", &fault) == H2_BLOCK_NOT_MINE);      // an unknown type
    CHECK(feed_hex(&b, "000001 01 00 00000003 82", &fault) == H2_BLOCK_MORE && b.open);
    CHECK(feed_hex(&b, "000002 09 04 00000003 8684", &fault) == H2_BLOCK_DONE && !b.open && b.len == 3 &&
          b.first.stream == 3 && memcmp(room, "\x82\x86\x84", 3) == 0);
    CHECK(feed_hex(&b, "000006 01 20 00000005 00000003 0f 82", &fault) == H2_BLOCK_MORE);  // with priority fields
    CHECK(feed_hex(&b, "000001 09 00 00000005 86", &fault) == H2_BLOCK_MORE);
    CHECK(feed_hex(&b, "000000 09 00 00000005", &fault) == H2_BLOCK_MORE);                 // an empty one is one
    CHECK(feed_hex(&b, "000001 09 04 00000005 84", &fault) == H2_BLOCK_DONE && b.len == 3 && b.first.has_priority &&
          b.first.depends_on == 3 && b.first.weight == 15 && memcmp(room, "\x82\x86\x84", 3) == 0);
    // A promise's block is gathered the same way, and says it was a promise.
    CHECK(feed_hex(&b, "000005 05 00 00000001 00000002 82", &fault) == H2_BLOCK_MORE);
    CHECK(feed_hex(&b, "000001 09 04 00000001 86", &fault) == H2_BLOCK_DONE && b.first.kind == H2_FRAME_PUSH_PROMISE &&
          b.first.value == 2 && b.len == 2);

    // A block one byte to a frame.
    static const uint8_t kBlock[] = {0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b};
    for (size_t i = 0; i < sizeof(kBlock); i++) {
        char text[64];
        bool last = i == sizeof(kBlock) - 1;
        snprintf(text, sizeof(text), "000001 %02x %02x 00000007 %02x", i == 0 ? 1 : 9, last ? 4 : 0, kBlock[i]);
        CHECK(feed_hex(&b, text, &fault) == (last ? H2_BLOCK_DONE : H2_BLOCK_MORE));
    }
    CHECK(b.len == sizeof(kBlock) && memcmp(room, kBlock, sizeof(kBlock)) == 0);

    // While a block is open, anything but its own CONTINUATION ends the
    // connection: another stream's, a DATA, a SETTINGS, a second HEADERS,
    // a type nobody knows.
    static const char *const kIntruders[] = {
        "000001 09 04 00000005 86", "000001 00 00 00000003 61", "000000 04 00 00000000",
        "000001 01 04 00000003 86", "000001 0b 00 00000003 86", "000004 08 00 00000000 00000001",
    };
    for (size_t i = 0; i < sizeof(kIntruders) / sizeof(kIntruders[0]); i++) {
        CHECK(feed_hex(&b, "000001 01 00 00000003 82", &fault) == H2_BLOCK_MORE);
        CHECK(feed_hex(&b, kIntruders[i], &fault) == H2_BLOCK_FAULT && fault.error == H2_ERR_PROTOCOL &&
              fault.rule == H2_RULE_BLOCK_INTERRUPTED && !fault.stream && !b.open);
    }
    // A CONTINUATION of nothing.
    CHECK(feed_hex(&b, "000001 09 04 00000003 86", &fault) == H2_BLOCK_FAULT && fault.error == H2_ERR_PROTOCOL &&
          fault.rule == H2_RULE_BLOCK_NOT_OPEN);

    // The room: exactly full is a block, one byte more is not, in one frame
    // or across several.
    CHECK(feed_hex(&b, "000010 01 04 00000001 00112233445566778899aabbccddeeff", &fault) == H2_BLOCK_DONE && b.len == 16);
    CHECK(feed_hex(&b, "000011 01 04 00000001 00112233445566778899aabbccddeeff00", &fault) == H2_BLOCK_FAULT &&
          fault.error == H2_ERR_ENHANCE_YOUR_CALM && fault.rule == H2_RULE_BLOCK_TOO_LARGE && !fault.stream);
    CHECK(feed_hex(&b, "00000f 01 00 00000001 00112233445566778899aabbccddee", &fault) == H2_BLOCK_MORE);
    CHECK(feed_hex(&b, "000001 09 00 00000001 ff", &fault) == H2_BLOCK_MORE && b.len == 16);
    CHECK(feed_hex(&b, "000001 09 04 00000001 00", &fault) == H2_BLOCK_FAULT && fault.rule == H2_RULE_BLOCK_TOO_LARGE &&
          !b.open);
    // And the reassembler is usable again after a block it gave up on.
    CHECK(feed_hex(&b, "000001 01 04 00000009 82", &fault) == H2_BLOCK_DONE && b.len == 1 && b.first.stream == 9);
}

// ── Random input, under the sanitizers ──────────────────────────────────

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static bool swallow(void *ctx, const h2_field_t *f)
{
    // Touch every byte handed over, so a pointer past its storage is a report.
    unsigned *sum = ctx;
    for (uint32_t i = 0; i < f->name_len; i++)
        *sum += f->name[i];
    for (uint32_t i = 0; i < f->value_len; i++)
        *sum += f->value[i];
    return (*sum & 15u) != 0;
}

static void fuzz_cases(void)
{
    // Random blocks at a decoder: any answer, no report. Short tables and a
    // small scratch make evictions and refusals common.
    unsigned sum = 0, bad = 0, ok = 0;
    for (int round = 0; round < 3000; round++) {
        static uint32_t store[H2_HPACK_STORAGE(4096) / 4 + 1];
        static uint8_t scratch[4096];
        h2_hpack_decoder_t dec;
        CHECK(h2_hpack_decoder_init(&dec, store, sizeof(store), scratch, sizeof(scratch), rng() % 4097));
        if (rng() & 1)
            h2_hpack_decoder_settings_acked(&dec);
        for (int blocks = 0; blocks < 8; blocks++) {
            // A block of well-formed representations, so that tables fill
            // and turn over, with a byte or two spoiled now and then.
            uint8_t made[2048];
            size_t len = 0;
            if (rng() % 6 == 0)
                len = (size_t)(int_write(made, 5, 0x20, rng() % 5000) - made);
            for (unsigned fields = rng() % 12; fields != 0; fields--) {
                uint32_t kind = rng() % 8, index = rng() % 5 == 0 ? rng() % 140 : 1 + rng() % 61;
                if (kind < 3) {
                    len = (size_t)(int_write(made + len, 7, 0x80, index) - made);
                    continue;
                }
                bool named = rng() & 1;
                uint8_t *q = kind < 6   ? int_write(made + len, 6, 0x40, named ? index : 0)
                             : kind == 6 ? int_write(made + len, 4, 0x00, named ? index : 0)
                                         : int_write(made + len, 4, 0x10, named ? index : 0);
                for (int part = named ? 1 : 0; part < 2; part++) {
                    uint8_t text[80];
                    uint32_t n = rng() % 9 == 0 ? rng() % 80 : rng() % 10;
                    for (uint32_t i = 0; i < n; i++)
                        text[i] = (uint8_t)('a' + rng() % 26);
                    q = string_write(q, text, n, rng() & 1);
                }
                len = (size_t)(q - made);
            }
            for (unsigned spoil = rng() % 5 == 0 ? 1 + rng() % 2 : 0; spoil != 0 && len != 0; spoil--)
                made[rng() % len] = (uint8_t)rng();
            if (rng() % 16 == 0 && len != 0)
                len = rng() % len;                      // cut short
            uint8_t *block = malloc(len ? len : 1);     // its own allocation: a read past it is a report
            memcpy(block, made, len);
            h2_hpack_result_t result = h2_hpack_decode(&dec, block, len, swallow, &sum);
            free(block);
            CHECK(dec.table.size <= dec.table.most && dec.table.count < dec.table.slots &&
                  dec.table.from <= dec.table.to && dec.table.to <= dec.table.room);
            if (result == H2_HPACK_BAD) {
                bad++;
                break;
            }
            ok++;
        }
    }
    if (!(bad > 100 && ok > 100)) fprintf(stderr, "bad %u ok %u\n", bad, ok);
    CHECK(bad > 100 && ok > 100);

    // Random frames at the parser: a fault or a frame whose data lies
    // inside the payload it was given.
    for (int round = 0; round < 200000; round++) {
        uint8_t nine[9];
        for (int i = 0; i < 9; i++)
            nine[i] = (uint8_t)rng();
        nine[0] = 0;
        nine[1] = (uint8_t)(rng() % 3 == 0 ? rng() : 0);
        nine[2] = (uint8_t)(rng() % 40);
        nine[3] = (uint8_t)(rng() % 12);
        if (rng() & 1)
            memset(nine + 5, 0, 4);
        h2_frame_header_t header;
        h2_frame_header_read(nine, &header);
        uint8_t *payload = malloc(header.length ? header.length : 1);
        for (uint32_t i = 0; i < header.length; i++)
            payload[i] = (uint8_t)rng();
        h2_frame_t f;
        h2_fault_t fault;
        if (h2_frame_parse(&header, payload, H2_FRAME_SIZE_LEAST, &f, &fault)) {
            CHECK(f.data_len <= header.length);
            CHECK(f.kind == H2_FRAME_UNKNOWN || (f.data >= payload && f.data + f.data_len <= payload + header.length));
            for (uint32_t i = 0; i < f.data_len && f.kind != H2_FRAME_UNKNOWN; i++)
                sum += f.data[i];
            for (uint32_t i = 0; i < h2_settings_count(&f); i++)
                sum += h2_settings_at(&f, i).value;
        } else
            CHECK(fault.error != H2_ERR_NO_ERROR && fault.rule != H2_RULE_NONE);
        free(payload);
    }

    // Random field lists through an encoder and into a decoder, the peer's
    // table size moving under them: the fields come back as they went, and
    // the two tables stay the same table.
    static const char *const kNames[] = {":method", ":path", "accept", "cookie", "x-a", "x-bb", "authorization",
                                         "content-length", "user-agent", "x-long-name-that-is-not-in-any-table"};
    for (int round = 0; round < 300; round++) {
        uint32_t advertised = rng() % 3 == 0 ? rng() % 200 : 4096 + rng() % 8192;
        pair_t *p = pair_new(advertised, rng() % 3 == 0 ? rng() % 300 : 65536);
        p->enc.huffman = rng() & 1;
        bool acked = false;
        for (int blocks = 0; blocks < 30; blocks++) {
            if (!acked && rng() % 4 == 0) {
                // Our SETTINGS reach the peer: its encoder learns the size,
                // and our decoder holds it to that from here.
                h2_hpack_encoder_peer_table_size(&p->enc, advertised);
                h2_hpack_decoder_settings_acked(&p->dec);
                acked = true;
            }
            h2_field_t fields[12];
            static char values[12][700];
            size_t count = rng() % 12;
            for (size_t i = 0; i < count; i++) {
                uint32_t len = rng() % 5 == 0 ? rng() % 600 : rng() % 12;
                for (uint32_t k = 0; k < len; k++)
                    values[i][k] = (char)(rng() % 7 == 0 ? rng() : 'a' + rng() % 6);
                values[i][len] = '\0';
                fields[i] = F(kNames[rng() % 10], "");
                fields[i].value = (const uint8_t *)values[i];
                fields[i].value_len = len;
                fields[i].sensitive = rng() % 9 == 0;
            }
            static uint8_t out[12 * 800];
            size_t len;
            CHECK(h2_hpack_encode(&p->enc, fields, count, out, sizeof(out), &len));
            CHECK(len <= h2_hpack_encode_bound(fields, count));
            got_t *g = calloc(1, sizeof(*g));
            // Fields over 300 bytes are not copied by `take`; compare lengths for those.
            CHECK(h2_hpack_decode(&p->dec, out, len, take, g) == H2_HPACK_OK && g->n == count);
            for (size_t i = 0; i < count && i < g->n; i++) {
                CHECK(g->name_len[i] == fields[i].name_len || fields[i].value_len >= 300);
                if (fields[i].value_len < 300) {
                    CHECK(g->value_len[i] == fields[i].value_len &&
                          memcmp(g->value[i], fields[i].value, fields[i].value_len) == 0 &&
                          memcmp(g->name[i], fields[i].name, fields[i].name_len) == 0);
                    bool never = fields[i].sensitive || strcmp((const char *)fields[i].name, "authorization") == 0;
                    CHECK(g->sensitive[i] == never);
                }
            }
            free(g);
            CHECK(p->enc.table.count == p->dec.table.count && p->enc.table.size == p->dec.table.size &&
                  p->enc.table.most == p->dec.table.most);
            for (uint32_t i = 0; i < p->enc.table.count; i++) {
                h2_field_t a, b;
                CHECK(lookup(&p->enc.table, 62 + i, &a) && lookup(&p->dec.table, 62 + i, &b) &&
                      same(a.name, a.name_len, b.name, b.name_len) && same(a.value, a.value_len, b.value, b.value_len));
            }
        }
        free(p);
    }
    (void)sum;
}

int main(void)
{
    alarm(SECONDS_MOST);
    tables_cases();
    integer_cases();
    huffman_cases();
    literal_cases();
    sequence_cases();
    decoder_cases();
    encoder_cases();
    head_cases();
    frame_cases();
    writer_cases();
    block_cases();
    fuzz_cases();
    if (failures != 0) {
        fprintf(stderr, "test_h2_host: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_h2_host: %d checks passed\n", checks);
    return 0;
}
