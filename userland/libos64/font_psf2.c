// font_psf2.c — an outline face rendered into a PSF2 image for the kernel
// console. The contract, and why a missing glyph is left OUT of the Unicode
// table, are in os64/font_psf2.h.
//
// Written against the backend TABLE and nothing above it. The text layer
// (os64/text.h) exists to lay out runs — fallback chains, kerning, a cache —
// and a console font is none of that: it is one face, one glyph per code
// point, each in its own cell. Going straight to lookup/render/glyph_view is
// also what lets tools/test_font_psf2_host.py drive this with a backend of
// its own and hand the result to the kernel's real loader.

#include <stdbool.h>
#include "os64/font_psf2.h"
#include "os64/charset.h"
#include "os64/mem.h"

#define PSF2_HEADER_BYTES 32u
#define PSF2_HAS_UNICODE_TABLE 1u
#define PSF2_TABLE_END 0xFFu

// Coverage at or above this is ink. The kernel blitter has one bit per
// pixel and no alpha, so the grey edge of every stroke has to land on one
// side; the middle keeps a stem the width the hinter gave it.
#define INK_THRESHOLD 128u

static void *default_alloc(void *context, size_t bytes)
{
    (void)context;
    return os64_malloc(bytes);
}

static void default_free(void *context, void *allocation, size_t bytes)
{
    (void)context;
    (void)bytes;
    os64_free(allocation);
}

static void put32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

// Every code point this writes is in the BMP, so three bytes is the most.
static size_t put_utf8(uint8_t *at, uint32_t cp)
{
    if (cp < 0x80) {
        at[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        at[0] = (uint8_t)(0xC0 | (cp >> 6));
        at[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    at[0] = (uint8_t)(0xE0 | (cp >> 12));
    at[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    at[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
}

// The code point glyph `slot` is for, or 0 for a slot that stays blank and
// unnamed: the C0 and C1 controls, which no terminal draws, and DEL.
static uint32_t slot_codepoint(const uint16_t *extra, uint32_t slot)
{
    if (slot >= 256)
        return extra[slot - 256];
    if (slot < 0x20 || (slot >= 0x7F && slot < 0xA0))
        return 0;
    return slot;
}

// The CP437 code points Latin-1 does not already hold, in byte order.
static uint32_t collect_extra(uint16_t extra[128])
{
    uint32_t n = 0;
    for (uint32_t b = 0x80; b < 0x100; b++) {
        uint16_t cp = os64_cp437_codepoint((uint8_t)b);
        if (cp >= 0x100)
            extra[n++] = cp;
    }
    return n;
}

// THE BLOCKS THE KERNEL MAKES TO MEASURE. A face's block elements go
// through the threshold like any other glyph, and the full block comes out
// with a seam between rows, the half block loses its top row, and the
// shades come out at whatever density the face's designer liked — while
// CP437 art was drawn against the VGA ROM's dither, which is what
// psf2_synth_block reproduces exactly, for any cell. So these are not asked
// of the face at all: left unnamed, the kernel draws its own.
static bool kernel_draws(uint32_t cp)
{
    return cp == 0x2580 || cp == 0x2584 || cp == 0x2588 || cp == 0x258C ||
           cp == 0x2590 || (cp >= 0x2591 && cp <= 0x2593);
}

// THE JOINING GLYPHS, and the edges their names promise. A box-drawing
// glyph exists to meet its neighbours, and a face draws its set to reach
// its own line; the cell here is that line ROUNDED OUTWARD — the backend
// delivers the ascender rounded up and the descender rounded down, each on
// its own — so the cell can stand a row or two taller than the strokes were
// aimed at, and the threshold drops the partly-covered edge row. Two rules
// follow. An edge pixel the face covered at all is ink when the pixel just
// inside it is ink (the face aimed the stroke at the edge; the rounding
// moved the edge). And a glyph that still does not reach an edge its name
// promises is left UNNAMED: the kernel draws an unnamed code point blank,
// and blank reads as missing where a frame with gaps reads as corruption.
//
// The table is the forty box-drawing characters CP437 holds, which is all
// this ever asks a face for. ⌠ and ⌡ join too (an integral sign is drawn
// across two rows) and get the snap, but promise no edge.
#define EDGE_U 1u
#define EDGE_D 2u
#define EDGE_L 4u
#define EDGE_R 8u

static bool joins(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x257F) || cp == 0x2320 || cp == 0x2321;
}

static uint32_t promised_edges(uint32_t cp)
{
    switch (cp) {
    case 0x2500: case 0x2550:                                   return EDGE_L | EDGE_R;
    case 0x2502: case 0x2551:                                   return EDGE_U | EDGE_D;
    case 0x250C: case 0x2552: case 0x2553: case 0x2554:         return EDGE_D | EDGE_R;
    case 0x2510: case 0x2555: case 0x2556: case 0x2557:         return EDGE_D | EDGE_L;
    case 0x2514: case 0x2558: case 0x2559: case 0x255A:         return EDGE_U | EDGE_R;
    case 0x2518: case 0x255B: case 0x255C: case 0x255D:         return EDGE_U | EDGE_L;
    case 0x251C: case 0x255E: case 0x255F: case 0x2560:         return EDGE_U | EDGE_D | EDGE_R;
    case 0x2524: case 0x2561: case 0x2562: case 0x2563:         return EDGE_U | EDGE_D | EDGE_L;
    case 0x252C: case 0x2564: case 0x2565: case 0x2566:         return EDGE_L | EDGE_R | EDGE_D;
    case 0x2534: case 0x2567: case 0x2568: case 0x2569:         return EDGE_L | EDGE_R | EDGE_U;
    case 0x253C: case 0x256A: case 0x256B: case 0x256C:         return EDGE_U | EDGE_D | EDGE_L | EDGE_R;
    default:                                                    return 0;
    }
}

typedef struct {
    const os64_font_backend_t *backend;
    os64_font_face_t *face;
    uint32_t cell_w, cell_h, row_bytes;
    int32_t baseline;    // rows from the cell's top to the baseline
} raster_t;

static bool lit(const raster_t *r, const uint8_t *cell, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || x >= r->cell_w || y >= r->cell_h)
        return false;
    return (cell[(size_t)y * r->row_bytes + ((size_t)x >> 3)] >> (7 - (x & 7))) & 1;
}

static void set(const raster_t *r, uint8_t *cell, int64_t x, int64_t y)
{
    cell[(size_t)y * r->row_bytes + ((size_t)x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

// What one draw found out. `covered` is whether the face put any coverage
// anywhere; `inked` whether any of it survived the threshold INSIDE the
// cell. Covered and not inked is a glyph the face cannot draw at this size.
typedef struct {
    os64_font_pos_t advance;
    bool covered, inked, clipped;
} drawn_t;

// One glyph into one cell. MISSING means the face has no glyph for `cp`,
// and the cell is left as it was (blank). A NULL cell draws nothing and
// only measures.
static os64_font_status_t draw_cell(const raster_t *r, uint32_t cp, uint8_t *cell, drawn_t *d)
{
    *d = (drawn_t){0};
    uint32_t index = 0;
    os64_font_status_t status = r->backend->lookup(r->face, cp, &index);
    if (status != OS64_FONT_OK)
        return status;

    os64_font_glyph_t *glyph = NULL;
    status = r->backend->render(r->face, index, &glyph);
    if (status != OS64_FONT_OK)
        return status;

    os64_font_glyph_view_t v;
    status = r->backend->glyph_view(glyph, &v);
    if (status == OS64_FONT_OK) {
        d->advance = v.advance_x;
        bool snap = cell != NULL && joins(cp);
        for (uint32_t gy = 0; v.coverage != NULL && gy < v.height; gy++) {
            for (uint32_t gx = 0; gx < v.width; gx++) {
                uint8_t c = v.coverage[(size_t)gy * v.stride + gx];
                if (c == 0)
                    continue;
                d->covered = true;
                if (c < INK_THRESHOLD)
                    continue;
                int64_t x = (int64_t)v.left + gx;
                int64_t y = (int64_t)r->baseline + v.top + gy;
                if (x < 0 || y < 0 || x >= r->cell_w || y >= r->cell_h) {
                    d->clipped = true;
                    continue;
                }
                set(r, cell, x, y);
                d->inked = true;
            }
        }
        // The snap, after every thresholded pixel is down so "just inside"
        // is answered from the finished stroke.
        for (uint32_t gy = 0; snap && v.coverage != NULL && gy < v.height; gy++) {
            for (uint32_t gx = 0; gx < v.width; gx++) {
                uint8_t c = v.coverage[(size_t)gy * v.stride + gx];
                if (c == 0 || c >= INK_THRESHOLD)
                    continue;
                int64_t x = (int64_t)v.left + gx;
                int64_t y = (int64_t)r->baseline + v.top + gy;
                if (x < 0 || y < 0 || x >= r->cell_w || y >= r->cell_h)
                    continue;
                bool inside = (y == 0 && lit(r, cell, x, 1)) ||
                              (y == (int64_t)r->cell_h - 1 && lit(r, cell, x, y - 1)) ||
                              (x == 0 && lit(r, cell, 1, y)) ||
                              (x == (int64_t)r->cell_w - 1 && lit(r, cell, x - 1, y));
                if (inside) {
                    set(r, cell, x, y);
                    d->inked = true;
                }
            }
        }
    }
    r->backend->glyph_release(glyph);
    return status;
}

static uint32_t edges_reached(const raster_t *r, const uint8_t *cell)
{
    uint32_t reached = 0;
    for (uint32_t x = 0; x < r->cell_w; x++) {
        if (lit(r, cell, x, 0))               reached |= EDGE_U;
        if (lit(r, cell, x, r->cell_h - 1))   reached |= EDGE_D;
    }
    for (uint32_t y = 0; y < r->cell_h; y++) {
        if (lit(r, cell, 0, y))               reached |= EDGE_L;
        if (lit(r, cell, r->cell_w - 1, y))   reached |= EDGE_R;
    }
    return reached;
}

static os64_font_status_t render_face(const os64_font_backend_t *backend,
    os64_font_face_t *face, uint8_t *out, size_t cap, size_t *out_len,
    os64_font_psf2_info_t *info)
{
    os64_font_face_info_t fi;
    os64_font_status_t status = backend->face_info(face, &fi);
    if (status != OS64_FONT_OK)
        return status;
    if (!(fi.flags & OS64_FONT_FACE_FIXED_WIDTH))
        return OS64_FONT_UNSUPPORTED;

    // THE CELL IS THE FACE'S LINE as the backend reports it, and the glyphs
    // sit in it where the face put them: whole pixels of ascent above the
    // baseline, whole pixels of descent below, and whatever leading the face
    // asks for split between the two ends. Rounding each part UP can make
    // the parts a row taller than the rounded line; then the parts win,
    // because a cell that cuts the descenders off every 'g' to honour a
    // rounding is the wrong answer. (FreeType's metrics arrive as whole
    // pixels already; the rounding that matters happened inside it, and the
    // joining glyphs pay for it — see the snap.)
    uint32_t ascent  = ((uint32_t)fi.ascent + 63u) / 64u;
    uint32_t descent = ((uint32_t)fi.descent + 63u) / 64u;
    uint32_t cell_h  = ((uint32_t)fi.line_height + 63u) / 64u;
    if (cell_h < ascent + descent)
        cell_h = ascent + descent;

    raster_t r = { .backend = backend, .face = face, .cell_h = cell_h,
                   .baseline = (int32_t)((cell_h - (ascent + descent)) / 2u + ascent) };

    // The width is an advance, and the first one asked for decides it; the
    // loop below holds every other printable ASCII glyph to the same number.
    // It has to be asked BEFORE anything is drawn, because the image cannot
    // be laid out until the width is known — so this draw is into a cell of
    // no size at all, where every pixel clips and nothing is written.
    {
        raster_t probe = r;   // the width is not set yet; the height goes too
        probe.cell_h = 0;
        drawn_t d;
        status = draw_cell(&probe, 'M', NULL, &d);
        if (status != OS64_FONT_OK) {
            if (status == OS64_FONT_MISSING && info != NULL)
                info->offender = 'M';
            return status;
        }
        if (d.advance <= 0 || d.advance % 64 != 0)
            return OS64_FONT_UNSUPPORTED;
        r.cell_w = (uint32_t)d.advance / 64u;
    }
    r.row_bytes = (r.cell_w + 7u) / 8u;

    if (info != NULL) {
        info->cell_w = r.cell_w;
        info->cell_h = r.cell_h;
    }
    if (r.cell_w < OS64_FONT_PSF2_CELL_W_MIN || r.cell_w > OS64_FONT_PSF2_CELL_W_MAX ||
        r.cell_h < OS64_FONT_PSF2_CELL_H_MIN || r.cell_h > OS64_FONT_PSF2_CELL_H_MAX)
        return OS64_FONT_LIMIT;

    uint16_t extra[128];
    uint32_t glyphs = 256u + collect_extra(extra);
    size_t glyph_bytes = (size_t)r.row_bytes * r.cell_h;
    size_t table_at = PSF2_HEADER_BYTES + glyphs * glyph_bytes;
    size_t enough = table_at + (size_t)glyphs * 4u;
    *out_len = enough;
    if (cap < enough)
        return OS64_FONT_LIMIT;

    for (size_t i = 0; i < table_at; i++)
        out[i] = 0;
    put32(out + 0, 0x864AB572u);
    put32(out + 4, 0);
    put32(out + 8, PSF2_HEADER_BYTES);
    put32(out + 12, PSF2_HAS_UNICODE_TABLE);
    put32(out + 16, glyphs);
    put32(out + 20, (uint32_t)glyph_bytes);
    put32(out + 24, r.cell_h);
    put32(out + 28, r.cell_w);

    uint32_t missing = 0, clipped = 0;
    size_t t = table_at;
    for (uint32_t slot = 0; slot < glyphs; slot++) {
        uint32_t cp = slot_codepoint(extra, slot);
        if (cp != 0 && !kernel_draws(cp)) {
            uint8_t *cell = out + PSF2_HEADER_BYTES + slot * glyph_bytes;
            drawn_t d;
            status = draw_cell(&r, cp, cell, &d);
            bool ascii = cp >= 0x20 && cp < 0x7F;
            bool unnamed = status == OS64_FONT_MISSING;
            if (status == OS64_FONT_OK) {
                if (d.covered && !d.inked)
                    unnamed = true;                     // the threshold ate all of it
                else if ((edges_reached(&r, cell) & promised_edges(cp)) != promised_edges(cp))
                    unnamed = true;                     // a frame with a gap in it
            } else if (status != OS64_FONT_MISSING) {
                return status;
            }
            if (unnamed) {
                if (ascii) {
                    if (info != NULL)
                        info->offender = cp;
                    return OS64_FONT_MISSING;
                }
                for (size_t i = 0; i < glyph_bytes; i++)
                    cell[i] = 0;
                missing++;
            } else {
                // Only ASCII is held to the cell's width. A fixed-width face
                // is fixed across what it was DRAWN for; a box-drawing glyph
                // borrowed into it at some other advance still goes in the
                // cell, clipped if it must be. Clipping counts only for a
                // glyph that does not join: a joining one is drawn to
                // overshoot its line on purpose, and cutting that off at the
                // cell is what makes two of them meet.
                if (ascii && d.advance != (os64_font_pos_t)r.cell_w * 64) {
                    if (info != NULL)
                        info->offender = cp;
                    return OS64_FONT_UNSUPPORTED;
                }
                if (d.clipped && !joins(cp))
                    clipped++;
                t += put_utf8(out + t, cp);
            }
        }
        out[t++] = PSF2_TABLE_END;
    }

    *out_len = t;
    if (info != NULL) {
        info->glyphs  = glyphs;
        info->missing = missing;
        info->clipped = clipped;
    }
    return OS64_FONT_OK;
}

os64_font_status_t os64_font_render_psf2(const os64_font_psf2_options_t *options,
    const uint8_t *face_bytes, size_t face_len,
    uint8_t *out, size_t cap, size_t *out_len, os64_font_psf2_info_t *info)
{
    if (out_len != NULL)
        *out_len = 0;
    if (info != NULL)
        *info = (os64_font_psf2_info_t){0};
    if (options == NULL || face_bytes == NULL || face_len == 0 || out == NULL ||
        out_len == NULL || options->pixel_height == 0 ||
        (options->memory.alloc == NULL) != (options->memory.free == NULL))
        return OS64_FONT_BAD_ARGUMENT;

    const os64_font_backend_t *backend = options->backend;
    if (backend == NULL)
        backend = os64_freetype_backend_v1();
    if (backend == NULL || backend->revision != OS64_FONT_BACKEND_REVISION ||
        backend->struct_size < sizeof(*backend))
        return OS64_FONT_BAD_ARGUMENT;

    os64_font_engine_options_t eo = { .memory = options->memory };
    if (eo.memory.alloc == NULL)
        eo.memory = (os64_font_memory_t){ NULL, default_alloc, default_free };

    os64_font_engine_t *engine = NULL;
    os64_font_status_t status = backend->engine_create(&eo, &engine);
    if (status != OS64_FONT_OK)
        return status;

    os64_font_face_options_t fo = { options->pixel_height, OS64_FONT_HINT_NORMAL };
    os64_font_face_t *face = NULL;
    status = backend->face_open(engine, face_bytes, face_len, &fo, &face);
    if (status == OS64_FONT_OK) {
        if (info != NULL)
            info->opened = true;
        status = render_face(backend, face, out, cap, out_len, info);
        backend->face_close(face);
    }
    (void)backend->engine_destroy(engine);
    if (status != OS64_FONT_OK && status != OS64_FONT_LIMIT)
        *out_len = 0;
    return status;
}
