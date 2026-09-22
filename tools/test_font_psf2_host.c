// test_font_psf2_host.c — os64_font_render_psf2 on the host, judged by the
// kernel's own PSF2 loader.
//
// The point of the arrangement: the converter (ring 3) and the loader (ring
// 0) are two halves of one sentence, and neither test suite alone can tell
// whether they agree. Here the image the converter writes goes straight into
// kernel/src/psf2.c, and what comes back out — which glyph a byte of each
// character set draws, and its bits — is compared with what the fake backend
// below was asked to draw.
//
// THE FAKE FACE is one byte, which selects its behaviour:
//   'M' fixed-width, every wanted code point present
//   'N' fixed-width, nothing past Latin-1 (every CP437 extra is missing)
//   'W' fixed-width, every letter overshoots the CELL by one pixel on all
//       four sides, and every joining glyph has a row of ink above it
//   'E' fixed-width, 'é' is covered but nothing survives the threshold
//   'G' fixed-width, U+253C's left arm stops short of the edge
//   'P' proportional                                  -> UNSUPPORTED
//   'F' fixed-width flag, fractional advance          -> UNSUPPORTED
//   'V' fixed-width flag, 'i' narrower than the rest  -> UNSUPPORTED, 'i'
//   'A' fixed-width, no '~'                           -> MISSING, '~'
//   'B' fixed-width, '|' vanishes                     -> MISSING, '|'
// A letter's ink is a hash of (code point, x, y), so a glyph drawn in the
// wrong cell, shifted a pixel, or thresholded the wrong way cannot pass by
// luck. Coverage is 128 for ink and 127 for paper: exactly the two values
// either side of the threshold. A JOINING glyph is a plus sign the height of
// the whole cell whose four tips are only half covered — reaching the edge
// takes the converter's snap, so a box glyph named in the table is the snap
// working, and one left out is the snap broken.
//
// With a directory argument (-DPSF2_REAL, FreeType linked in) the fixture
// faces are rendered for real, at EVERY size from 8 to 72: the six sizes a
// first version checked all happened to dodge the breakage in between.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os64/font_psf2.h"
#include "os64/charset.h"
#include "psf2.h"

_Static_assert(OS64_FONT_PSF2_CELL_W_MIN == PSF2_CELL_W_MIN, "cell fences must match the kernel's");
_Static_assert(OS64_FONT_PSF2_CELL_W_MAX == PSF2_CELL_W_MAX, "cell fences must match the kernel's");
_Static_assert(OS64_FONT_PSF2_CELL_H_MIN == PSF2_CELL_H_MIN, "cell fences must match the kernel's");
_Static_assert(OS64_FONT_PSF2_CELL_H_MAX == PSF2_CELL_H_MAX, "cell fences must match the kernel's");
_Static_assert(OS64_FONT_PSF2_IMAGE_MAX <= PSF2_IMAGE_MAX, "the largest image must fit the kernel's fence");

static unsigned long g_checks, g_failures;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// libos64's allocator, for the default-memory path. The host has no libos64.
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }

#ifndef PSF2_REAL
const os64_font_backend_t *os64_freetype_backend_v1(void) { return NULL; }
#endif

// The edges a CP437 box-drawing glyph's name promises — the test's own copy,
// so a wrong entry in the converter's table is a finding here and not a
// shared mistake.
#define EDGE_U 1u
#define EDGE_D 2u
#define EDGE_L 4u
#define EDGE_R 8u
static uint32_t promised(uint32_t cp)
{
    switch (cp) {
    case 0x2500: case 0x2550:                            return EDGE_L | EDGE_R;
    case 0x2502: case 0x2551:                            return EDGE_U | EDGE_D;
    case 0x250C: case 0x2552: case 0x2553: case 0x2554:  return EDGE_D | EDGE_R;
    case 0x2510: case 0x2555: case 0x2556: case 0x2557:  return EDGE_D | EDGE_L;
    case 0x2514: case 0x2558: case 0x2559: case 0x255A:  return EDGE_U | EDGE_R;
    case 0x2518: case 0x255B: case 0x255C: case 0x255D:  return EDGE_U | EDGE_L;
    case 0x251C: case 0x255E: case 0x255F: case 0x2560:  return EDGE_U | EDGE_D | EDGE_R;
    case 0x2524: case 0x2561: case 0x2562: case 0x2563:  return EDGE_U | EDGE_D | EDGE_L;
    case 0x252C: case 0x2564: case 0x2565: case 0x2566:  return EDGE_L | EDGE_R | EDGE_D;
    case 0x2534: case 0x2567: case 0x2568: case 0x2569:  return EDGE_L | EDGE_R | EDGE_U;
    case 0x253C: case 0x256A: case 0x256B: case 0x256C:  return EDGE_U | EDGE_D | EDGE_L | EDGE_R;
    default:                                             return 0;
    }
}
static bool joins(uint32_t cp) { return (cp >= 0x2500 && cp <= 0x257F) || cp == 0x2320 || cp == 0x2321; }
static bool is_block(uint32_t cp)
{
    uint8_t scratch[8 * 128];
    return psf2_synth_block(cp, 8, 128, scratch);   // the kernel's list, asked directly
}

// ── A counting allocator with a fuse ────────────────────────────────────────

static long g_live, g_allocs, g_fail_at;   // g_fail_at: fail the Nth alloc, 0 = never

static void *t_alloc(void *ctx, size_t n)
{
    (void)ctx;
    if (g_fail_at != 0 && ++g_allocs == g_fail_at)
        return NULL;
    g_live++;
    return malloc(n);
}

static void t_free(void *ctx, void *p, size_t n)
{
    (void)ctx; (void)n;
    g_live--;
    free(p);
}

// ── The fake backend ────────────────────────────────────────────────────────

struct os64_font_engine { os64_font_memory_t mem; int faces, glyphs; };
struct os64_font_face   { os64_font_engine_t *e; char kind; uint32_t px; };
struct os64_font_glyph  { os64_font_engine_t *e; os64_font_glyph_view_t v; size_t area; uint8_t *cov; };

static uint32_t f_cell_w(uint32_t px)  { return px / 2; }
static uint32_t f_ascent(uint32_t px)  { return px * 3 / 4; }
static uint32_t f_descent(uint32_t px) { return px / 4; }
static uint32_t f_leading(uint32_t px) { return px / 4; }   // so the baseline is not at the top
static uint32_t f_cell_h(uint32_t px)  { return f_ascent(px) + f_descent(px) + f_leading(px); }
static int32_t  f_cell_top(uint32_t px) { return -(int32_t)(f_ascent(px) + f_leading(px) / 2); }

static bool fake_ink(uint32_t cp, uint32_t x, uint32_t y)
{
    uint32_t h = cp * 2654435761u ^ (x * 40503u + 1u) ^ (y * 9973u + 7u) * 2246822519u;
    h ^= h >> 15;
    return (h & 3u) == 0 || (x == 1 && y == 1);   // one pixel is always ink: an empty glyph proves nothing
}

// The plus sign's coverage at glyph pixel (x, y) in a w x h glyph: 128 on
// the middle column and row, 64 at the four tips, 127 elsewhere — except
// on the cell's edge, where paper is 0 as a real face's paper is. The snap
// treats any coverage on an edge pixel as the face reaching for it, and
// with 127 there it would widen a stroke that runs beside the edge, which
// a real face never asks for.
static uint8_t plus_cov(uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t mx = w / 2, my = h / 2;
    if (x == mx && (y == 0 || y == h - 1)) return 64;
    if (y == my && (x == 0 || x == w - 1)) return 64;
    if (x == mx || y == my) return 128;
    return (x == 0 || x == w - 1 || y == 0 || y == h - 1) ? 0 : 127;
}

static os64_font_status_t fe_create(const os64_font_engine_options_t *o, os64_font_engine_t **out)
{
    *out = NULL;
    os64_font_engine_t *e = o->memory.alloc(o->memory.context, sizeof(*e));
    if (!e) return OS64_FONT_NO_MEMORY;
    *e = (os64_font_engine_t){ o->memory, 0, 0 };
    *out = e;
    return OS64_FONT_OK;
}
static os64_font_status_t fe_destroy(os64_font_engine_t *e)
{
    if (!e) return OS64_FONT_OK;
    if (e->faces || e->glyphs) return OS64_FONT_BUSY;
    e->mem.free(e->mem.context, e, sizeof(*e));
    return OS64_FONT_OK;
}
static os64_font_status_t fe_stats(os64_font_engine_t *e, os64_font_engine_stats_t *out)
{
    (void)e; *out = (os64_font_engine_stats_t){0};
    return OS64_FONT_OK;
}
static os64_font_status_t ff_open(os64_font_engine_t *e, const uint8_t *b, size_t n,
    const os64_font_face_options_t *o, os64_font_face_t **out)
{
    *out = NULL;
    if (n != 1 || !strchr("MNWEGPFVAB", (char)b[0])) return OS64_FONT_MALFORMED;
    if (o->pixel_height > OS64_FONT_PIXEL_MAX) return OS64_FONT_BAD_ARGUMENT;
    os64_font_face_t *f = e->mem.alloc(e->mem.context, sizeof(*f));
    if (!f) return OS64_FONT_NO_MEMORY;
    *f = (os64_font_face_t){ e, (char)b[0], o->pixel_height };
    e->faces++;
    *out = f;
    return OS64_FONT_OK;
}
static void ff_close(os64_font_face_t *f)
{
    if (!f) return;
    f->e->faces--;
    f->e->mem.free(f->e->mem.context, f, sizeof(*f));
}
static os64_font_status_t ff_info(os64_font_face_t *f, os64_font_face_info_t *out)
{
    *out = (os64_font_face_info_t){ .glyph_count = 70000,
        .flags = f->kind == 'P' ? 0 : OS64_FONT_FACE_FIXED_WIDTH,
        // Fractional ascent and descent, so the rounding in the converter is
        // exercised: the pixels it must leave room for are the ceilings.
        .ascent = f_ascent(f->px) ? (os64_font_pos_t)(f_ascent(f->px) * 64 - 17) : 0,
        .descent = f_descent(f->px) ? (os64_font_pos_t)(f_descent(f->px) * 64 - 5) : 0,
        .line_height = (os64_font_pos_t)(f_cell_h(f->px) * 64) };
    return OS64_FONT_OK;
}
static os64_font_status_t ff_lookup(os64_font_face_t *f, uint32_t cp, uint32_t *out)
{
    *out = 0;
    if (f->kind == 'N' && cp > 0xFF) return OS64_FONT_MISSING;
    if (f->kind == 'A' && cp == '~') return OS64_FONT_MISSING;
    *out = cp + 1;   // glyph zero is never a successful lookup
    return OS64_FONT_OK;
}
static os64_font_status_t ff_pair(os64_font_face_t *f, uint32_t l, uint32_t r, os64_font_pos_t *out)
{
    (void)f; (void)l; (void)r; *out = 0;
    return OS64_FONT_OK;
}
static os64_font_status_t ff_render(os64_font_face_t *f, uint32_t index, os64_font_glyph_t **out)
{
    *out = NULL;
    uint32_t cp = index - 1;
    os64_font_engine_t *e = f->e;
    os64_font_glyph_t *g = e->mem.alloc(e->mem.context, sizeof(*g));
    if (!g) return OS64_FONT_NO_MEMORY;

    uint32_t px = f->px, cw = f_cell_w(px), ch = f_cell_h(px);
    uint32_t w, h;
    int32_t left, top;
    if (joins(cp)) {
        w = cw; h = ch; left = 0; top = f_cell_top(px);
        if (f->kind == 'W') { h++; top--; }               // a row of ink above the cell
    } else if (f->kind == 'W') {
        w = cw + 2; h = ch + 2; left = -1; top = f_cell_top(px) - 1;
    } else {
        w = cw; h = f_ascent(px) + f_descent(px); left = 0; top = -(int32_t)f_ascent(px);
    }
    if (cp == ' ') { w = 0; h = 0; }

    os64_font_pos_t adv = (os64_font_pos_t)(cw * 64);
    if (f->kind == 'F') adv += 21;
    if (f->kind == 'V' && cp == 'i') adv -= 64;
    *g = (os64_font_glyph_t){ .e = e, .area = (size_t)w * h };
    g->v = (os64_font_glyph_view_t){ .advance_x = adv, .width = w, .height = h, .stride = w,
                                     .left = left, .top = top };
    if (g->area) {
        g->cov = e->mem.alloc(e->mem.context, g->area);
        if (!g->cov) { e->mem.free(e->mem.context, g, sizeof(*g)); return OS64_FONT_NO_MEMORY; }
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t c;
                if (joins(cp)) {
                    uint32_t py = y, ph = h;
                    if (f->kind == 'W') { if (y == 0) { g->cov[x] = 128; continue; } py = y - 1; ph = h - 1; }
                    c = plus_cov(w, ph, x, py);
                    if (f->kind == 'G' && cp == 0x253C && x == 0 && py == ph / 2)
                        c = 0;                             // the left arm stops short
                } else {
                    c = fake_ink(cp, x, y) ? 128 : 127;
                    if ((f->kind == 'E' && cp == 0xE9) || (f->kind == 'B' && cp == '|'))
                        c = 127;                           // covered, never ink
                }
                g->cov[(size_t)y * w + x] = c;
            }
        }
        g->v.coverage = g->cov;
        g->v.ink = (os64_font_rect_t){ left * 64, top * 64, (left + (int32_t)w) * 64, (top + (int32_t)h) * 64 };
    }
    e->glyphs++;
    *out = g;
    return OS64_FONT_OK;
}
static os64_font_status_t fg_view(os64_font_glyph_t *g, os64_font_glyph_view_t *out)
{
    *out = g->v;
    return OS64_FONT_OK;
}
static void fg_release(os64_font_glyph_t *g)
{
    if (!g) return;
    os64_font_engine_t *e = g->e;
    if (g->cov) e->mem.free(e->mem.context, g->cov, g->area);
    e->glyphs--;
    e->mem.free(e->mem.context, g, sizeof(*g));
}

static const os64_font_backend_t kFake = {
    OS64_FONT_BACKEND_REVISION, sizeof(os64_font_backend_t),
    fe_create, fe_destroy, fe_stats, ff_open, ff_close, ff_info,
    ff_lookup, ff_pair, ff_render, fg_view, fg_release,
};

// ── What the loader should find ─────────────────────────────────────────────

static bool cell_bit(const psf2_face_t *f, uint32_t glyph, uint32_t x, uint32_t y)
{
    const uint8_t *g = f->glyphs + (size_t)glyph * f->glyph_bytes;
    return (g[(size_t)y * f->row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// The pixel the fake meant for cell (x, y), by the converter's own rules:
// letters where the face put them, joining glyphs as the plus with its
// tips snapped in.
static bool expect_bit(uint32_t cp, uint32_t px, char kind, uint32_t x, uint32_t y)
{
    uint32_t cw = f_cell_w(px), ch = f_cell_h(px);
    if (cp == ' ' || x >= cw)
        return false;
    if (joins(cp))
        return x == cw / 2 || y == ch / 2;   // the snap turns every 64 tip to ink
    if (kind == 'W')
        return fake_ink(cp, x + 1, y + 1);
    uint32_t top = (uint32_t)(-f_cell_top(px)) - f_ascent(px);   // the leading above the ascent
    if (y < top || y >= top + f_ascent(px) + f_descent(px))
        return false;
    return fake_ink(cp, x, y - top);
}

// Every pixel of the cell `glyph` against what the fake meant. Pad bits past
// the width too: the kernel blitter does not look at them, but a stray one
// is a stray write.
static void check_cell(const psf2_face_t *f, uint32_t glyph, uint32_t cp, uint32_t px, char kind)
{
    static int shown;
    unsigned long bad = 0;
    for (uint32_t y = 0; y < f->height; y++)
        for (uint32_t x = 0; x < f->row_bytes * 8; x++)
            if (cell_bit(f, glyph, x, y) != expect_bit(cp, px, kind, x, y))
                bad++;
    CHECK(bad == 0, "U+%04X at %upx ('%c'): %lu pixels differ", cp, px, kind, bad);
    if (bad != 0 && shown++ < 3) {   // got | expected, for the first few
        for (uint32_t y = 0; y < f->height; y++) {
            printf("    ");
            for (uint32_t x = 0; x < f->width; x++) putchar(cell_bit(f, glyph, x, y) ? '#' : '.');
            printf(" | ");
            for (uint32_t x = 0; x < f->width; x++) putchar(expect_bit(cp, px, kind, x, y) ? '#' : '.');
            putchar('\n');
        }
    }
}

static os64_font_status_t convert(const os64_font_backend_t *backend, char kind, uint32_t px,
    uint8_t *out, size_t cap, size_t *len, os64_font_psf2_info_t *info, bool own_memory)
{
    os64_font_psf2_options_t o = { .backend = backend, .pixel_height = px };
    if (own_memory)
        o.memory = (os64_font_memory_t){ NULL, t_alloc, t_free };
    uint8_t face = (uint8_t)kind;
    return os64_font_render_psf2(&o, &face, 1, out, cap, len, info);
}

static void test_good_face(char kind, uint32_t px)
{
    // The image lives in a heap block of exactly the length it claims, so a
    // loader or a checker that reads one byte past it is a report.
    uint8_t *big = malloc(OS64_FONT_PSF2_IMAGE_MAX);
    size_t len = 0;
    os64_font_psf2_info_t info;
    g_live = 0; g_allocs = 0; g_fail_at = 0;
    os64_font_status_t st = convert(&kFake, kind, px, big, OS64_FONT_PSF2_IMAGE_MAX, &len, &info, true);
    CHECK(st == OS64_FONT_OK, "'%c' at %upx: status %d", kind, px, st);
    CHECK(g_live == 0, "'%c' at %upx: %ld allocations never returned", kind, px, g_live);
    if (st != OS64_FONT_OK) { free(big); return; }

    uint8_t *image = malloc(len);
    memcpy(image, big, len);
    free(big);

    CHECK(info.cell_w == f_cell_w(px), "cell width %u at %upx", info.cell_w, px);
    CHECK(info.cell_h == f_cell_h(px), "cell height %u at %upx", info.cell_h, px);
    CHECK(info.offender == 0, "'%c' at %upx: an accepted face named an offender", kind, px);
    CHECK(len <= OS64_FONT_PSF2_IMAGE_MAX, "image of %zu bytes is past the promised maximum", len);

    psf2_face_t face;
    uint32_t why = 0;
    psf2_status_t ps = psf2_parse(image, len, &face, &why);
    CHECK(ps == PSF2_OK, "the kernel's loader refuses it: %s (%u)", psf2_status_name(ps), why);
    psf2_charmap_t map;
    if (ps == PSF2_OK)
        ps = psf2_build_charmap(&face, &map, &why);
    CHECK(ps == PSF2_OK, "the kernel's charmap refuses it: %s (%u)", psf2_status_name(ps), why);
    if (ps != PSF2_OK) { free(image); return; }

    CHECK(face.width == info.cell_w && face.height == info.cell_h && face.nglyphs == info.glyphs,
          "the header disagrees with the info");
    CHECK(map.from_table, "the loader found no Unicode table");

    uint32_t want_missing = 0, want_clipped = 0;
    for (uint32_t set = 0; set < 2; set++) {
        for (uint32_t b = 0x20; b < 0x100; b++) {
            if (b == 0x7F) continue;
            uint32_t cp = set == OS64_CHARSET_CP437 ? os64_cp437_codepoint((uint8_t)b) : b;
            if (set == OS64_CHARSET_LATIN1 && b >= 0x80 && b < 0xA0) {
                CHECK(map.glyph[set][b] == PSF2_MAP_NONE, "C1 control 0x%02X drew a glyph", b);
                continue;
            }
            bool extra = set == OS64_CHARSET_CP437 && cp > 0xFF;
            bool block = is_block(cp);
            bool expect_unnamed = block || (kind == 'N' && extra) ||
                                  (kind == 'E' && cp == 0xE9) || (kind == 'G' && cp == 0x253C);
            // Counted once per code point: CP437 holds é at 0x82 as well.
            if (expect_unnamed && !block && (set == OS64_CHARSET_LATIN1 || extra)) want_missing++;
            uint16_t g = map.glyph[set][b];
            if (expect_unnamed) {
                CHECK(g == PSF2_MAP_NONE, "'%c': U+%04X should be unnamed, was given glyph %u", kind, cp, g);
                continue;
            }
            CHECK(g != PSF2_MAP_NONE, "'%c': set %u byte 0x%02X (U+%04X) has no glyph", kind, set, b, cp);
            if (g == PSF2_MAP_NONE) continue;
            if (set == OS64_CHARSET_CP437 && cp <= 0xFF) continue;   // the same slot, checked as Latin-1
            check_cell(&face, g, cp, px, kind);
            // The plus reaches all four edges once snapped, so a promise is
            // never more than what is there; the converter's table only
            // matters when a face falls short, which 'G' arranges for ┼.
            CHECK(promised(cp) == 0 || joins(cp), "U+%04X promises edges but does not join", cp);
            if (kind == 'W' && cp != ' ' && !joins(cp)) want_clipped++;
        }
    }
    CHECK(info.missing == want_missing, "'%c' at %upx: missing %u, expected %u", kind, px, info.missing, want_missing);
    CHECK(info.clipped == want_clipped, "'%c' at %upx: clipped %u, expected %u", kind, px, info.clipped, want_clipped);

    // A glyph nobody names must be BLANK, not merely unnamed: the kernel may
    // not draw it today, and a tool that dumps the font will.
    for (uint32_t slot = 0; slot < face.nglyphs; slot++) {
        bool named = false;
        for (uint32_t set = 0; set < 2 && !named; set++)
            for (uint32_t b = 0; b < 256 && !named; b++)
                named = map.glyph[set][b] == slot;
        if (named) continue;
        unsigned long ink = 0;
        for (uint32_t i = 0; i < face.glyph_bytes; i++)
            ink += face.glyphs[(size_t)slot * face.glyph_bytes + i];
        CHECK(ink == 0, "'%c' at %upx: unnamed slot %u has ink", kind, px, slot);
    }
    free(image);
}

static void test_refusals(void)
{
    static uint8_t out[OS64_FONT_PSF2_IMAGE_MAX];
    size_t len;
    os64_font_psf2_info_t info;
    static const struct { char kind; os64_font_status_t want; bool opened; uint32_t offender; const char *what; } kFaces[] = {
        { 'P', OS64_FONT_UNSUPPORTED, true,  0,   "proportional" },
        { 'F', OS64_FONT_UNSUPPORTED, true,  0,   "fractional advance" },
        { 'V', OS64_FONT_UNSUPPORTED, true,  'i', "one narrow ASCII glyph" },
        { 'A', OS64_FONT_MISSING,     true,  '~', "no tilde" },
        { 'B', OS64_FONT_MISSING,     true,  '|', "a bar the threshold ate" },
        { 'X', OS64_FONT_MALFORMED,   false, 0,   "not a face" },
    };
    for (size_t i = 0; i < sizeof(kFaces) / sizeof(kFaces[0]); i++) {
        g_live = 0; g_fail_at = 0;
        len = 99;
        os64_font_status_t st = convert(&kFake, kFaces[i].kind, 24, out, sizeof(out), &len, &info, true);
        CHECK(st == kFaces[i].want, "%s: status %d", kFaces[i].what, st);
        CHECK(len == 0, "%s: a refusal reported %zu bytes", kFaces[i].what, len);
        CHECK(info.opened == kFaces[i].opened, "%s: opened says %d", kFaces[i].what, info.opened);
        CHECK(info.offender == kFaces[i].offender, "%s: offender U+%04X", kFaces[i].what, info.offender);
        CHECK(g_live == 0, "%s: %ld allocations never returned", kFaces[i].what, g_live);
    }

    // The cell fences, from both sides, and the refusal names the cell.
    for (uint32_t px = 1; px <= OS64_FONT_PIXEL_MAX; px++) {
        uint32_t w = f_cell_w(px), h = f_cell_h(px);
        bool fits = w >= PSF2_CELL_W_MIN && w <= PSF2_CELL_W_MAX &&
                    h >= PSF2_CELL_H_MIN && h <= PSF2_CELL_H_MAX;
        g_live = 0;
        os64_font_status_t st = convert(&kFake, 'M', px, out, sizeof(out), &len, &info, true);
        if (w == 0)
            CHECK(st == OS64_FONT_UNSUPPORTED, "%upx: a zero advance gave %d", px, st);
        else
            CHECK(st == (fits ? OS64_FONT_OK : OS64_FONT_LIMIT), "%upx (%ux%u): status %d", px, w, h, st);
        if (st == OS64_FONT_LIMIT)
            CHECK(info.cell_w == w && info.cell_h == h, "%upx: the refusal did not name the cell", px);
        CHECK(g_live == 0, "%upx: %ld allocations never returned", px, g_live);
    }

    // Room: one byte short is LIMIT and says what is enough; that much works.
    size_t need = 0;
    os64_font_status_t st = convert(&kFake, 'M', 24, out, 100, &need, &info, true);
    CHECK(st == OS64_FONT_LIMIT && need > 100, "a 100-byte buffer: status %d, need %zu", st, need);
    uint8_t *tight = malloc(need);
    st = convert(&kFake, 'M', 24, tight, need - 1, &len, &info, true);
    CHECK(st == OS64_FONT_LIMIT, "one byte short: status %d", st);
    st = convert(&kFake, 'M', 24, tight, need, &len, &info, true);
    CHECK(st == OS64_FONT_OK && len <= need, "exactly enough: status %d, %zu of %zu", st, len, need);
    free(tight);

    // Arguments.
    os64_font_psf2_options_t o = { .backend = &kFake, .pixel_height = 24 };
    uint8_t face = 'M';
    CHECK(os64_font_render_psf2(NULL, &face, 1, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "NULL options");
    CHECK(os64_font_render_psf2(&o, NULL, 1, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "NULL face");
    CHECK(os64_font_render_psf2(&o, &face, 0, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "empty face");
    CHECK(os64_font_render_psf2(&o, &face, 1, NULL, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "NULL out");
    CHECK(os64_font_render_psf2(&o, &face, 1, out, sizeof(out), NULL, NULL) == OS64_FONT_BAD_ARGUMENT, "NULL out_len");
    o.pixel_height = 0;
    CHECK(os64_font_render_psf2(&o, &face, 1, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "zero size");
    o.pixel_height = 24;
    o.memory.alloc = t_alloc;   // and no free
    CHECK(os64_font_render_psf2(&o, &face, 1, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "half a memory table");
    o.memory.alloc = NULL;
    os64_font_backend_t old = kFake;
    old.revision = 0;
    o.backend = &old;
    CHECK(os64_font_render_psf2(&o, &face, 1, out, sizeof(out), &len, NULL) == OS64_FONT_BAD_ARGUMENT, "wrong backend revision");

    // The default allocator (os64_malloc, here the host's).
    st = convert(&kFake, 'M', 24, out, sizeof(out), &len, NULL, false);
    CHECK(st == OS64_FONT_OK, "default memory: status %d", st);
}

// Fail every allocation in turn. Each run must answer NO_MEMORY and give
// everything back; the run after the last allocation succeeds.
static void test_allocation_failures(void)
{
    static uint8_t out[OS64_FONT_PSF2_IMAGE_MAX];
    long tried = 0;
    for (long n = 1; ; n++) {
        size_t len;
        g_live = 0; g_allocs = 0; g_fail_at = n;
        os64_font_status_t st = convert(&kFake, 'M', 16, out, sizeof(out), &len, NULL, true);
        long live = g_live;
        g_fail_at = 0;
        CHECK(live == 0, "allocation %ld failing left %ld live", n, live);
        if (st == OS64_FONT_OK)
            break;
        CHECK(st == OS64_FONT_NO_MEMORY, "allocation %ld failing gave %d", n, st);
        tried++;
        if (n > 100000) { CHECK(false, "the fuse never ran out"); break; }
    }
    CHECK(tried > 500, "only %ld allocation sites were exercised", tried);
}

#ifdef PSF2_REAL
static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void show(const psf2_face_t *f, uint32_t glyph)
{
    for (uint32_t y = 0; y < f->height; y++) {
        printf("    ");
        for (uint32_t x = 0; x < f->width; x++)
            putchar(cell_bit(f, glyph, x, y) ? '#' : '.');
        putchar('\n');
    }
}

static uint32_t reached(const psf2_face_t *f, uint32_t glyph)
{
    uint32_t e = 0;
    for (uint32_t x = 0; x < f->width; x++) {
        if (cell_bit(f, glyph, x, 0)) e |= EDGE_U;
        if (cell_bit(f, glyph, x, f->height - 1)) e |= EDGE_D;
    }
    for (uint32_t y = 0; y < f->height; y++) {
        if (cell_bit(f, glyph, 0, y)) e |= EDGE_L;
        if (cell_bit(f, glyph, f->width - 1, y)) e |= EDGE_R;
    }
    return e;
}

// Every size from 8 to 72, both fixture faces. At each: the kernel takes
// the image, every named glyph but the space has ink, every named box
// glyph reaches the edges its name promises, the blocks are the kernel's.
// A refusal is allowed only below 14 px, where hairlines vanish.
static void test_real(const char *dir, const char *dump_dir)
{
    static uint8_t out[OS64_FONT_PSF2_IMAGE_MAX];
    static const char *kMono[] = { "DejaVuSansMono.ttf", "SourceCodePro-Regular.otf" };
    for (size_t m = 0; m < 2; m++) {
        size_t flen;
        uint8_t *bytes = slurp(dir, kMono[m], &flen);
        CHECK(bytes != NULL, "cannot read %s/%s", dir, kMono[m]);
        if (!bytes) continue;
        printf("  %s\n", kMono[m]);
        for (uint32_t px = 8; px <= 72; px++) {
            os64_font_psf2_options_t o = { .pixel_height = px, .memory = { NULL, t_alloc, t_free } };
            os64_font_psf2_info_t info;
            size_t len;
            g_live = 0; g_fail_at = 0;
            os64_font_status_t st = os64_font_render_psf2(&o, bytes, flen, out, sizeof(out), &len, &info);
            CHECK(g_live == 0, "%s at %u: %ld allocations never returned", kMono[m], px, g_live);
            // The fixtures live in the tree, so what they do is PINNED, not
            // bounded: a refusal at 10 px or below only, and nothing left
            // unnamed at an accepted size except DejaVu 9 px's eight box
            // glyphs. A safety net (the edge-promise rule) also hides what
            // falls into it, and this is the count that would show a snap
            // regression at a size the fake cannot model.
            if (st == OS64_FONT_MISSING && px <= 10) {
                printf("    %3upx  refused: '%c' vanishes\n", px, (int)info.offender);
                continue;
            }
            CHECK(st == OS64_FONT_OK, "%s at %u: status %d (offender U+%04X)", kMono[m], px, st, info.offender);
            if (st != OS64_FONT_OK) continue;

            uint8_t *image = malloc(len);
            memcpy(image, out, len);
            psf2_face_t face;
            psf2_charmap_t map;
            uint32_t why = 0;
            psf2_status_t ps = psf2_parse(image, len, &face, &why);
            if (ps == PSF2_OK) ps = psf2_build_charmap(&face, &map, &why);
            CHECK(ps == PSF2_OK, "%s at %u: the kernel refuses it: %s (%u)", kMono[m], px, psf2_status_name(ps), why);
            if (ps == PSF2_OK) {
                uint32_t unnamed_box = 0;
                for (uint32_t set = 0; set < 2; set++) {
                    for (uint32_t b = 0x21; b < 0x100; b++) {
                        if (b == 0x7F || (set == 0 && b >= 0x80 && b < 0xA0)) continue;
                        uint32_t cp = set ? os64_cp437_codepoint((uint8_t)b) : b;
                        uint16_t g = map.glyph[set][b];
                        if (is_block(cp)) {
                            CHECK(g == PSF2_MAP_NONE, "%s at %u: block U+%04X is named", kMono[m], px, cp);
                            continue;
                        }
                        if (g == PSF2_MAP_NONE) {
                            if (promised(cp)) unnamed_box++;
                            continue;
                        }
                        unsigned long ink = 0;
                        const uint8_t *gl = face.glyphs + (size_t)g * face.glyph_bytes;
                        for (uint32_t i = 0; i < face.glyph_bytes; i++) ink += gl[i];
                        CHECK(ink != 0 || cp == 0xA0,   // the no-break space is blank by right
                              "%s at %u: U+%04X is named and blank", kMono[m], px, cp);
                        uint32_t want = promised(cp);
                        CHECK((reached(&face, g) & want) == want,
                              "%s at %u: U+%04X does not reach an edge its name promises", kMono[m], px, cp);
                    }
                }
                uint32_t want_missing = (m == 0 && px == 9) ? 8 : 0;
                CHECK(info.missing == want_missing && unnamed_box == want_missing,
                      "%s at %u: %u missing (%u box), expected %u", kMono[m], px,
                      info.missing, unnamed_box, want_missing);
                printf("    %3upx  %2ux%-3u %u missing (%u box unnamed), %u clipped\n",
                       px, info.cell_w, info.cell_h, info.missing, unnamed_box, info.clipped);
                if (px == 26 && m == 0) {
                    show(&face, map.glyph[1][0xC5]);   // U+253C at the P5's size
                    show(&face, map.glyph[0]['g']);
                }
                if (dump_dir != NULL) {
                    char path[1024];
                    snprintf(path, sizeof(path), "%s/%c%u.psf", dump_dir, m == 0 ? 'd' : 's', px);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(image, 1, len, f); fclose(f); }
                }
            }
            free(image);
        }
        free(bytes);
    }

    size_t flen;
    uint8_t *prop = slurp(dir, "DejaVuSans.ttf", &flen);
    CHECK(prop != NULL, "cannot read the proportional fixture");
    if (prop) {
        os64_font_psf2_options_t o = { .pixel_height = 24 };
        size_t len;
        os64_font_status_t st = os64_font_render_psf2(&o, prop, flen, out, sizeof(out), &len, NULL);
        CHECK(st == OS64_FONT_UNSUPPORTED, "a proportional face gave %d", st);
        free(prop);
    }
}
#endif

int main(int argc, char **argv)
{
    static const char kGood[] = { 'M', 'N', 'W', 'E', 'G' };
    for (size_t k = 0; k < sizeof(kGood); k++)
        for (uint32_t px = 8; px <= 96; px++)
            test_good_face(kGood[k], px);
    test_refusals();
    test_allocation_failures();
#ifdef PSF2_REAL
    if (argc > 1)
        test_real(argv[1], argc > 2 ? argv[2] : NULL);
#else
    (void)argc; (void)argv;
#endif
    printf("%s: %lu checks, %lu failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
