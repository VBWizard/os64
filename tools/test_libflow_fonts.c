// test_libflow_fonts.c — the font backend libflow's layout dumps are
// computed against by hand.
//
// tools/fonts' fake backend proves the text ENGINE and deliberately does
// not scale with the pixel size; a layout test needs the opposite — an h1
// has to be taller than a paragraph — and needs every number a pencil can
// reach. So here every metric is a simple fraction of the pixel size H:
//
//   ascent 3H/4, descent H/4, line height 5H/4 (16px: 12, 4, 20)
//   advance, proportional faces: H/4 for a space and for the narrow
//   glyphs  i l j t f r 1 . , : ; ' ! |  (16px: 4)
//                                H*3/4 for  m w M W @  (16px: 12)
//                                H/2 for everything else (16px: 8)
//   advance, the monospace face: H/2 for every glyph (16px: 8)
//
// No kerning, no ink (every glyph is zero-area), every scalar from U+0020
// up is present. A face is named by its one byte of "font file": S serif,
// A sans, M mono; bold and italic change nothing but the label.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "os64/font_backend.h"
#include "test_libflow_fonts.h"

struct os64_font_engine {
    os64_font_memory_t memory;
    uint32_t faces, glyphs;
};

struct os64_font_face {
    os64_font_engine_t *engine;
    uint8_t kind;
    uint32_t px;
};

struct os64_font_glyph {
    os64_font_engine_t *engine;
    os64_font_pos_t advance;
};

static void *mem_alloc(os64_font_engine_t *e, size_t n)
{
    return e->memory.alloc(e->memory.context, n);
}

static void mem_free(os64_font_engine_t *e, void *p, size_t n)
{
    e->memory.free(e->memory.context, p, n);
}

static os64_font_status_t engine_create(const os64_font_engine_options_t *o,
                                        os64_font_engine_t **out)
{
    *out = NULL;
    if (o == NULL || o->memory.alloc == NULL || o->memory.free == NULL)
        return OS64_FONT_BAD_ARGUMENT;
    os64_font_engine_t *e = o->memory.alloc(o->memory.context, sizeof(*e));
    if (e == NULL)
        return OS64_FONT_NO_MEMORY;
    memset(e, 0, sizeof(*e));
    e->memory = o->memory;
    *out = e;
    return OS64_FONT_OK;
}

static os64_font_status_t engine_destroy(os64_font_engine_t *e)
{
    if (e == NULL)
        return OS64_FONT_OK;
    if (e->faces != 0 || e->glyphs != 0)
        return OS64_FONT_BUSY;
    mem_free(e, e, sizeof(*e));
    return OS64_FONT_OK;
}

static os64_font_status_t engine_stats(os64_font_engine_t *e, os64_font_engine_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->live_faces = e->faces;
    out->live_glyphs = e->glyphs;
    return OS64_FONT_OK;
}

static os64_font_status_t face_open(os64_font_engine_t *e, const uint8_t *bytes, size_t length,
                                    const os64_font_face_options_t *o, os64_font_face_t **out)
{
    *out = NULL;
    if (length != 1 || (bytes[0] != 'S' && bytes[0] != 'A' && bytes[0] != 'M'))
        return OS64_FONT_UNSUPPORTED;
    os64_font_face_t *f = mem_alloc(e, sizeof(*f));
    if (f == NULL)
        return OS64_FONT_NO_MEMORY;
    f->engine = e;
    f->kind = bytes[0];
    f->px = o->pixel_height;
    e->faces++;
    *out = f;
    return OS64_FONT_OK;
}

static void face_close(os64_font_face_t *f)
{
    if (f == NULL)
        return;
    f->engine->faces--;
    mem_free(f->engine, f, sizeof(*f));
}

static os64_font_status_t face_info(os64_font_face_t *f, os64_font_face_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->glyph_count = 0x110000;
    out->flags = f->kind == 'M' ? OS64_FONT_FACE_FIXED_WIDTH : 0;
    const char *family = f->kind == 'S' ? "Test Serif" : f->kind == 'A' ? "Test Sans" : "Test Mono";
    memcpy(out->family, family, strlen(family) + 1);
    memcpy(out->style, "Regular", 8);
    int32_t h = (int32_t)f->px * OS64_FONT_UNIT;
    out->ascent = h * 3 / 4;
    out->descent = h / 4;
    out->line_height = h * 5 / 4;
    return OS64_FONT_OK;
}

static os64_font_status_t lookup(os64_font_face_t *f, uint32_t scalar, uint32_t *index)
{
    (void)f;
    *index = 0;
    if (scalar < 0x20 || scalar == 0x7F || scalar >= 0x110000)
        return OS64_FONT_MISSING;
    *index = scalar;
    return OS64_FONT_OK;
}

static os64_font_status_t pair_adjust(os64_font_face_t *f, uint32_t left, uint32_t right,
                                      os64_font_pos_t *delta)
{
    (void)f;
    *delta = 0;
    return left == 0 || right == 0 ? OS64_FONT_BAD_ARGUMENT : OS64_FONT_OK;
}

static os64_font_pos_t advance(const os64_font_face_t *f, uint32_t scalar)
{
    int32_t h = (int32_t)f->px * OS64_FONT_UNIT;
    if (f->kind == 'M')
        return h / 2;
    if (scalar < 0x80 && strchr(" iljtfr1.,:;'!|", (int)scalar) != NULL)
        return h / 4;
    if (scalar < 0x80 && strchr("mwMW@", (int)scalar) != NULL)
        return h * 3 / 4;
    return h / 2;
}

static os64_font_status_t render(os64_font_face_t *f, uint32_t index, os64_font_glyph_t **out)
{
    *out = NULL;
    if (index == 0)
        return OS64_FONT_BAD_ARGUMENT;
    os64_font_glyph_t *g = mem_alloc(f->engine, sizeof(*g));
    if (g == NULL)
        return OS64_FONT_NO_MEMORY;
    g->engine = f->engine;
    g->advance = advance(f, index);
    f->engine->glyphs++;
    *out = g;
    return OS64_FONT_OK;
}

static os64_font_status_t glyph_view(os64_font_glyph_t *g, os64_font_glyph_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->advance_x = g->advance;
    return OS64_FONT_OK;
}

static void glyph_release(os64_font_glyph_t *g)
{
    if (g == NULL)
        return;
    g->engine->glyphs--;
    mem_free(g->engine, g, sizeof(*g));
}

static const os64_font_backend_t s_backend = {
    .revision = OS64_FONT_BACKEND_REVISION,
    .struct_size = sizeof(os64_font_backend_t),
    .engine_create = engine_create,
    .engine_destroy = engine_destroy,
    .engine_stats = engine_stats,
    .face_open = face_open,
    .face_close = face_close,
    .face_info = face_info,
    .lookup = lookup,
    .pair_adjust = pair_adjust,
    .render = render,
    .glyph_view = glyph_view,
    .glyph_release = glyph_release,
};

const os64_font_backend_t *flow_test_backend(void)
{
    return &s_backend;
}
