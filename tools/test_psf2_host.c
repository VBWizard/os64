// Host harness for kernel/src/psf2.c — built and run by test_psf2_host.sh.
//
// The image under test is written by a ring-3 program, so the interesting
// inputs are the ones a correct font never produces. Every case builds its
// image in a heap block of EXACTLY the image's size, so a read one byte past
// the end is an ASan report rather than a lucky zero.
//
// With a path argument it instead loads that file, and requires it to parse
// and to map all of printable ASCII — the check test_psf2_host.sh runs over
// whatever real console fonts the host has installed.

#include "psf2.h"
#include "os64/charset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks, failures;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

typedef struct { uint8_t *bytes; size_t len; } image_t;

static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// A font of `n` glyphs whose bitmaps are all the glyph's own index (so a
// test can tell which glyph a pointer landed on), plus `table` verbatim.
static image_t build(uint32_t w, uint32_t h, uint32_t n, const uint8_t *table, size_t table_len)
{
    uint32_t gb = h * ((w + 7) / 8);
    image_t im = { .len = 32 + (size_t)n * gb + table_len };
    im.bytes = malloc(im.len);
    static const uint8_t magic[4] = {0x72, 0xB5, 0x4A, 0x86};
    memcpy(im.bytes, magic, 4);
    put32(im.bytes + 4, 0);   put32(im.bytes + 8, 32);
    put32(im.bytes + 12, table ? 1 : 0);
    put32(im.bytes + 16, n);  put32(im.bytes + 20, gb);
    put32(im.bytes + 24, h);  put32(im.bytes + 28, w);
    for (uint32_t g = 0; g < n; g++)
        memset(im.bytes + 32 + (size_t)g * gb, (int)(g & 0xFF), gb);
    if (table_len) memcpy(im.bytes + 32 + (size_t)n * gb, table, table_len);
    return im;
}

static size_t utf8(uint8_t *out, uint32_t cp)
{
    if (cp < 0x80)    { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800)   { out[0] = (uint8_t)(0xC0 | (cp >> 6)); out[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (uint8_t)(0xE0 | (cp >> 12)); out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (uint8_t)(0xF0 | (cp >> 18)); out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (uint8_t)(0x80 | (cp & 0x3F)); return 4;
}

// The table for a font that keeps its glyphs BACKWARDS: glyph g draws code
// point (n-1-g) for the Latin-1 range, and the last few glyphs draw CP437's
// blocks. Nothing about it is in index order, which is the point.
static size_t scrambled_table(uint8_t *t, uint32_t n)
{
    size_t used = 0;
    for (uint32_t g = 0; g < n; g++) {
        uint32_t cp = 255 - g;
        if (g < 256) used += utf8(t + used, cp);
        if (g == 256) used += utf8(t + used, 0x2591);      // light shade -> CP437 0xB0
        if (g == 257) used += utf8(t + used, 0x2500);      // box horizontal -> CP437 0xC4
        t[used++] = 0xFF;
    }
    return used;
}

// Walk every glyph the map can reach, reading every byte of it: under ASan
// this is the proof that an accepted font cannot send the blitter outside
// the image.
static unsigned long touch_all(const psf2_face_t *f, const psf2_charmap_t *m)
{
    unsigned long sum = 0;
    for (int s = 0; s < 2; s++)
        for (int b = 0; b < 256; b++) {
            uint16_t g = m->glyph[s][b];
            if (g == PSF2_MAP_NONE) continue;
            if (g >= f->nglyphs) { failures++; printf("FAIL map names glyph %u of %u\n", g, f->nglyphs); continue; }
            const uint8_t *p = f->glyphs + (size_t)g * f->glyph_bytes;
            for (uint32_t i = 0; i < f->glyph_bytes; i++) sum += p[i];
        }
    return sum;
}

static void test_valid(void)
{
    uint8_t table[4096];
    size_t tl = scrambled_table(table, 258);
    image_t im = build(8, 16, 258, table, tl);
    psf2_face_t f; psf2_charmap_t m; uint32_t why = 0;
    CHECK(psf2_parse(im.bytes, im.len, &f, &why) == PSF2_OK, "valid 8x16 refused (%u)", why);
    CHECK(f.width == 8 && f.height == 16 && f.row_bytes == 1 && f.glyph_bytes == 16 && f.nglyphs == 258, "geometry");
    CHECK(psf2_build_charmap(&f, &m, &why) == PSF2_OK, "charmap refused (%u)", why);
    CHECK(m.from_table, "from_table");
    CHECK(m.glyph[OS64_CHARSET_LATIN1]['A'] == 255 - 'A', "Latin-1 'A' follows the table, not the index");
    CHECK(m.glyph[OS64_CHARSET_LATIN1][0xE9] == 255 - 0xE9, "Latin-1 e-acute");
    CHECK(m.glyph[OS64_CHARSET_CP437]['A'] == 255 - 'A', "CP437 low half is ASCII");
    CHECK(m.glyph[OS64_CHARSET_CP437][0xB0] == 256, "CP437 0xB0 is U+2591's glyph");
    CHECK(m.glyph[OS64_CHARSET_CP437][0xC4] == 257, "CP437 0xC4 is U+2500's glyph");
    CHECK(m.glyph[OS64_CHARSET_CP437][0x82] == 255 - 0xE9, "CP437 0x82 is e-acute, found through Latin-1's glyph");
    CHECK(m.glyph[OS64_CHARSET_CP437][0xB1] == PSF2_MAP_NONE, "an unclaimed code point stays NONE");
    CHECK(m.unmapped[OS64_CHARSET_LATIN1] == 0, "Latin-1 fully mapped, C1 not counted (%u)", m.unmapped[0]);
    CHECK(m.unmapped[OS64_CHARSET_CP437] > 0, "CP437 reports what it lacks");
    touch_all(&f, &m);
    free(im.bytes);
}

static void test_wide_cell(void)
{
    uint8_t table[4096];
    size_t tl = scrambled_table(table, 256);
    image_t im = build(12, 24, 256, table, tl);
    psf2_face_t f; psf2_charmap_t m;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "12x24 refused");
    CHECK(f.row_bytes == 2 && f.glyph_bytes == 48, "12x24 is two bytes a row");
    CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_OK, "12x24 charmap");
    touch_all(&f, &m);
    free(im.bytes);

    im = build(PSF2_CELL_W_MAX, PSF2_CELL_H_MAX, 128, NULL, 0);
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "the largest cell is inside the fence");
    CHECK(f.glyph_bytes == 1024, "64x128 glyph bytes");
    free(im.bytes);
}

static void test_no_table(void)
{
    image_t im = build(8, 16, 256, NULL, 0);
    psf2_face_t f; psf2_charmap_t m;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "tableless font");
    CHECK(f.table == NULL, "no table pointer");
    CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_OK, "identity map");
    CHECK(!m.from_table && m.glyph[0][0xE9] == 0xE9 && m.glyph[1][0xB0] == 0xB0, "identity both ways");
    free(im.bytes);

    im = build(8, 16, 128, NULL, 0);
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "128-glyph font");
    CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_OK, "128 glyphs cover ASCII");
    CHECK(m.glyph[0][0x80] == PSF2_MAP_NONE, "identity stops at the glyph count");
    free(im.bytes);
}

static void test_table_rules(void)
{
    // First claim wins; sequences after 0xFE claim nothing.
    uint8_t t[2048]; size_t n = 0;
    for (uint32_t g = 0; g < 128; g++) {
        n += utf8(t + n, g);
        if (g == 'A') { t[n++] = 0xFE; n += utf8(t + n, 'Z'); n += utf8(t + n, 0x0301); }
        if (g == 'B') n += utf8(t + n, 'A');      // a later glyph also claims 'A'
        t[n++] = 0xFF;
    }
    image_t im = build(8, 16, 128, t, n);
    psf2_face_t f; psf2_charmap_t m;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "rules font");
    CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_OK, "rules charmap");
    CHECK(m.glyph[0]['A'] == 'A', "the first glyph to claim a code point keeps it");
    CHECK(m.glyph[0]['Z'] == 'Z', "a code point inside a sequence claims nothing");
    free(im.bytes);

    // A table cut short of its glyph count.
    im = build(8, 16, 128, t, n - 1);
    uint32_t why = 0;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "parse does not read the table");
    CHECK(psf2_build_charmap(&f, &m, &why) == PSF2_BAD_TABLE && why == 127, "short table names the glyph it stopped at (%u)", why);
    free(im.bytes);

    // ASCII missing: glyph 'Q' claims nothing.
    n = 0;
    for (uint32_t g = 0; g < 128; g++) { if (g != 'Q') n += utf8(t + n, g); t[n++] = 0xFF; }
    im = build(8, 16, 128, t, n);
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "no-Q font parses");
    CHECK(psf2_build_charmap(&f, &m, &why) == PSF2_NO_ASCII && why == 'Q', "a font without Q is refused, by letter (%u)", why);
    free(im.bytes);

    // Malformed UTF-8: overlong 'A', a surrogate, a lone continuation, a
    // sequence that runs off the end.
    static const struct { uint8_t b[4]; size_t len; const char *what; } bad[] = {
        {{0xC1, 0x81}, 2, "overlong"}, {{0xED, 0xA0, 0x80}, 3, "surrogate"},
        {{0x80}, 1, "lone continuation"}, {{0xE2, 0x96}, 2, "cut off"},
        {{0xF8, 0x88, 0x80, 0x80}, 4, "five-byte lead"}, {{0xF4, 0x90, 0x80, 0x80}, 4, "past U+10FFFF"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        n = 0;
        for (uint32_t g = 0; g < 128; g++) {
            n += utf8(t + n, g);
            if (g == 127) { memcpy(t + n, bad[i].b, bad[i].len); n += bad[i].len; }
            if (!(g == 127 && i == 3)) t[n++] = 0xFF;   // "cut off" really ends there
        }
        im = build(8, 16, 128, t, n);
        CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "%s parses", bad[i].what);
        CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_BAD_TABLE, "%s UTF-8 is refused", bad[i].what);
        free(im.bytes);
    }
}

// The reverse lookup has to agree with the forward table for EVERY byte: a
// font whose glyph b claims exactly CP437's code point for byte b must map
// every CP437 byte to its own index. One wrong comparison in the search and
// some byte of box-drawing silently draws blank.
static void test_cp437_every_byte(void)
{
    uint8_t *t = malloc(256 * 5);
    size_t n = 0;
    for (uint32_t g = 0; g < 256; g++) { n += utf8(t + n, os64_cp437_codepoint((uint8_t)g)); t[n++] = 0xFF; }
    image_t im = build(8, 16, 256, t, n);
    psf2_face_t f; psf2_charmap_t m;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "cp437 font parses");
    CHECK(psf2_build_charmap(&f, &m, NULL) == PSF2_OK, "cp437 font maps");
    unsigned wrong = 0;
    for (uint32_t b = 0; b < 256; b++) wrong += m.glyph[OS64_CHARSET_CP437][b] != b;
    CHECK(wrong == 0, "%u CP437 bytes did not find their own glyph", wrong);
    CHECK(m.unmapped[OS64_CHARSET_CP437] == 0, "CP437 fully mapped");
    free(im.bytes); free(t);
}

// THE TABLE IS THE WRITER'S TO SIZE. Glyph 0 claims as many code points as a
// megabyte holds before it ever writes a terminator; the map must still come
// out right, and the work must be a walk of the bytes and not a walk of the
// bytes times the code page — this runs where the door runs, with interrupts
// off. The count is printed rather than timed: a time limit on a shared
// build host fails for the wrong reasons.
static void test_table_flood(void)
{
    size_t room = PSF2_IMAGE_MAX - 32 - 128 * 16;
    uint8_t *t = malloc(room);
    size_t n = 0, claims = 0;
    // Leave room for the terminator and the 127 two-byte entries that follow.
    while (n + 3 + 1 + 127 * 2 <= room) { n += utf8(t + n, 0x4E00 + (uint32_t)(claims % 20000)); claims++; }
    t[n++] = 0xFF;                                       // ...end of glyph 0, at last
    for (uint32_t g = 1; g < 128; g++) { n += utf8(t + n, g); t[n++] = 0xFF; }
    image_t im = build(8, 16, 128, t, n);
    psf2_face_t f; psf2_charmap_t m; uint32_t why = 0;
    CHECK(psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK, "flooded font parses");
    psf2_status_t s = psf2_build_charmap(&f, &m, &why);
    // Glyph 0 claimed no ASCII, and NUL is not printable, so the font is whole.
    CHECK(s == PSF2_OK, "flooded table: %s (%u)", psf2_status_name(s), why);
    CHECK(m.glyph[OS64_CHARSET_LATIN1]['A'] == 'A', "the real entries after the flood still map");
    printf("test_psf2_host: a %zu-byte table of %zu code points mapped\n", n, claims);
    free(im.bytes); free(t);
}

static void test_refusals(void)
{
    struct { uint32_t off, value; psf2_status_t want; const char *what; } cases[] = {
        {0, 0x864AB573, PSF2_BAD_MAGIC, "magic"},
        {4, 1, PSF2_BAD_VERSION, "version"},
        {8, 31, PSF2_BAD_HEADER_SIZE, "header under 32"},
        {8, 0x7FFFFFFF, PSF2_BAD_HEADER_SIZE, "header past the image"},
        {28, 3, PSF2_BAD_CELL, "width under"}, {28, 65, PSF2_BAD_CELL, "width over"},
        {28, 0xFFFFFFF9, PSF2_BAD_CELL, "width that would wrap (w+7)"},
        {24, 5, PSF2_BAD_CELL, "height under"}, {24, 129, PSF2_BAD_CELL, "height over"},
        {20, 15, PSF2_BAD_GLYPH_BYTES, "glyph bytes"},
        {16, 127, PSF2_BAD_GLYPH_COUNT, "count under"}, {16, 65536, PSF2_BAD_GLYPH_COUNT, "count over"},
        {16, 257, PSF2_TRUNCATED, "one glyph more than is there"},
        {16, 65535, PSF2_TRUNCATED, "many more than are there"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        image_t im = build(8, 16, 256, NULL, 0);
        put32(im.bytes + cases[i].off, cases[i].value);
        psf2_face_t f = {0}; uint32_t why = 0;
        psf2_status_t got = psf2_parse(im.bytes, im.len, &f, &why);
        CHECK(got == cases[i].want, "%s: got %s", cases[i].what, psf2_status_name(got));
        CHECK(f.glyphs == NULL, "%s: a refusal writes nothing", cases[i].what);
        free(im.bytes);
    }
    psf2_face_t f;
    uint8_t tiny[31] = {0x72, 0xB5, 0x4A, 0x86};
    CHECK(psf2_parse(tiny, sizeof(tiny), &f, NULL) == PSF2_TOO_SHORT, "31 bytes");
    CHECK(psf2_parse(NULL, 100, &f, NULL) == PSF2_TOO_SHORT, "NULL image");
    uint8_t *big = calloc(1, PSF2_IMAGE_MAX + 1);
    CHECK(psf2_parse(big, PSF2_IMAGE_MAX + 1, &f, NULL) == PSF2_TOO_BIG, "one byte past the cap");
    free(big);
}

// Every prefix of a valid font. Each is copied to a block of its own size so
// a read past the prefix is caught, and none but the whole may be accepted
// with a complete charmap.
static void test_truncation(void)
{
    uint8_t table[4096];
    size_t tl = scrambled_table(table, 258);
    image_t whole = build(9, 17, 258, table, tl);
    for (size_t len = 0; len < whole.len; len++) {
        uint8_t *cut = malloc(len ? len : 1);
        memcpy(cut, whole.bytes, len);
        psf2_face_t f; psf2_charmap_t m;
        bool ok = psf2_parse(cut, len, &f, NULL) == PSF2_OK &&
                  psf2_build_charmap(&f, &m, NULL) == PSF2_OK;
        CHECK(!ok, "a font cut to %zu of %zu bytes was accepted", len, whole.len);
        free(cut);
    }
    free(whole.bytes);
}

// Random damage. Nothing is asserted about WHICH answer comes back — only
// that an accepted font is safe to draw every mapped glyph from, and that
// ASan sees no read outside the block either way.
static void test_fuzz(unsigned rounds)
{
    uint8_t table[4096];
    size_t tl = scrambled_table(table, 258);
    uint32_t seed = 0x05640564;
    unsigned accepted = 0;
    for (unsigned r = 0; r < rounds; r++) {
        image_t im = build(8 + r % 9, 12 + r % 13, 258, table, tl);
        unsigned hits = 1 + r % 6;
        for (unsigned k = 0; k < hits; k++) {
            seed = seed * 1664525u + 1013904223u;
            // Half the damage lands in the header, where every byte is a rule.
            size_t at = (k & 1) ? (seed >> 8) % 32 : (seed >> 8) % im.len;
            seed = seed * 1664525u + 1013904223u;
            im.bytes[at] = (uint8_t)(seed >> 16);
        }
        psf2_face_t f; psf2_charmap_t m;
        if (psf2_parse(im.bytes, im.len, &f, NULL) == PSF2_OK &&
            psf2_build_charmap(&f, &m, NULL) == PSF2_OK) {
            accepted++;
            touch_all(&f, &m);
        }
        checks++;
        free(im.bytes);
    }
    CHECK(accepted > 0 && accepted < rounds, "fuzz exercised both outcomes (%u of %u accepted)", accepted, rounds);
}

static unsigned popcount_glyph(const uint8_t *g, uint32_t w, uint32_t h)
{
    unsigned lit = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            lit += (g[y * ((w + 7) / 8) + (x >> 3)] >> (7 - (x & 7))) & 1;
    return lit;
}

// THE SYNTHESIZED BLOCKS MUST MATCH THE GLASS at 8x16 — not one header, the
// GLASS. Of the eight, three (the light and medium shades and the full
// block) are glyphs the shipped face carries and five are bitmaps
// os64/charset.h supplies, and a reader has no way to tell which is which.
// So the question is asked the way the console asks it: resolve the CP437
// byte through os64_charset_glyph against the real boot face, and require
// psf2_synth_block to produce the same sixteen bytes. Pinning literals here
// would pin the answer to nothing.
static void test_synth_matches_the_console(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("test_psf2_host: no %s; console-continuity check skipped\n", path); return; }
    uint8_t face[65536];
    size_t n = fread(face, 1, sizeof(face), fp);
    fclose(fp);
    if (n < 4 || face[0] != 0x36 || face[1] != 0x04) { CHECK(0, "%s is not a PSF1 face", path); return; }
    uint32_t charsize = face[3], nglyphs = (face[2] & 0x01) ? 512 : 256;
    const uint8_t *glyphs = face + 4;
    CHECK(charsize == 16 && 4 + (size_t)nglyphs * charsize <= n, "%s is a whole 8x16 face", path);

    static const uint32_t blocks[] = {0x2580, 0x2584, 0x2588, 0x258C, 0x2590, 0x2591, 0x2592, 0x2593};
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        int byte = -1;
        for (int b = 0x80; b < 0x100; b++)
            if (os64_cp437_codepoint((uint8_t)b) == blocks[i]) { byte = b; break; }
        CHECK(byte >= 0, "U+%04X has a CP437 byte", blocks[i]);
        if (byte < 0) continue;
        const uint8_t *drawn = os64_charset_glyph((uint8_t)byte, OS64_CHARSET_CP437,
                                                  glyphs, nglyphs, charsize);
        uint8_t synth[16];
        CHECK(psf2_synth_block(blocks[i], 8, 16, synth), "U+%04X is synthesized", blocks[i]);
        CHECK(memcmp(drawn, synth, 16) == 0,
              "U+%04X (CP437 0x%02X) differs from what the console draws", blocks[i], byte);
    }
}

static void test_synth(void)
{
    uint8_t g[1024];

    for (uint32_t w = PSF2_CELL_W_MIN; w <= PSF2_CELL_W_MAX; w += 3)
        for (uint32_t h = PSF2_CELL_H_MIN; h <= PSF2_CELL_H_MAX; h += 7) {
            uint32_t gb = h * ((w + 7) / 8);
            uint8_t *exact = malloc(gb);            // exact size: overrun = ASan
            CHECK(psf2_synth_block(0x2588, w, h, exact) && popcount_glyph(exact, w, h) == w * h, "full block %ux%u", w, h);
            CHECK(psf2_synth_block(0x2580, w, h, exact) && popcount_glyph(exact, w, h) == w * (h / 2), "upper half %ux%u", w, h);
            CHECK(psf2_synth_block(0x2590, w, h, exact) && popcount_glyph(exact, w, h) == (w - w / 2) * h, "right half %ux%u", w, h);
            unsigned light, dark;
            psf2_synth_block(0x2591, w, h, exact); light = popcount_glyph(exact, w, h);
            psf2_synth_block(0x2593, w, h, exact); dark = popcount_glyph(exact, w, h);
            CHECK(light + dark == w * h, "dark is light's negative at %ux%u", w, h);
            // Padding bits past the cell's width stay clear: the blitter
            // never reads them, but a stored glyph should not carry noise.
            psf2_synth_block(0x2588, w, h, exact);
            if (w % 8) CHECK((exact[(w + 7) / 8 - 1] & (0xFFu >> (w % 8))) == 0, "padding clear at width %u", w);
            free(exact);
        }
    CHECK(!psf2_synth_block('A', 8, 16, g), "only the blocks are synthesized");
}

static int check_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return 2; }
    fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *bytes = malloc(len > 0 ? (size_t)len : 1);
    if (fread(bytes, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); return 2; }
    fclose(fp);
    psf2_face_t f; psf2_charmap_t m; uint32_t why = 0;
    psf2_status_t s = psf2_parse(bytes, (size_t)len, &f, &why);
    if (s == PSF2_BAD_MAGIC) { free(bytes); return 3; }      // a PSF1: not this loader's
    if (s == PSF2_OK) s = psf2_build_charmap(&f, &m, &why);
    if (s != PSF2_OK) { printf("%s: %s (%u)\n", path, psf2_status_name(s), why); free(bytes); return 1; }
    touch_all(&f, &m);
    printf("%s: %ux%u, %u glyphs, %s, unmapped latin1 %u cp437 %u\n", path, f.width, f.height,
           f.nglyphs, m.from_table ? "table" : "no table", m.unmapped[0], m.unmapped[1]);
    free(bytes);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) return check_file(argv[1]);
    test_valid();
    test_wide_cell();
    test_no_table();
    test_table_rules();
    test_cp437_every_byte();
    test_table_flood();
    test_refusals();
    test_truncation();
    test_fuzz(20000);
    test_synth();
    test_synth_matches_the_console("external/zap-light16.psf");
    printf("test_psf2_host: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
