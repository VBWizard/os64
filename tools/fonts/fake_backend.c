#include "fake_backend.h"

struct os64_font_engine {
    os64_font_memory_t memory;
    size_t cap;
    os64_font_engine_stats_t stats;
};
struct os64_font_face {
    os64_font_engine_t *engine;
    const uint8_t *source;
    uint32_t pixel_height;
};
struct os64_font_glyph {
    os64_font_engine_t *engine;
    os64_font_glyph_view_t view;
};

typedef struct {
    uint32_t scalar;
    int32_t advance, left, top;
    uint32_t width, height;
} metric_t;
static const metric_t metrics[] = {
    {0, 0, 0, 0, 0, 0},
    {'i', 192, 0, -9, 2, 9},
    {'W', 576, 0, -9, 9, 9},
    {'A', 448, 0, -9, 7, 9},
    {'V', 448, 0, -9, 7, 9},
    {'j', 256, -2, -9, 5, 11},
    {' ', 256, 0, 0, 0, 0},
    {'e', 320, 0, -7, 5, 7},
    {0xe9, 320, 0, -11, 5, 11},
    {0x301, 0, -1, -12, 2, 2},
    {'x', 352, 0, -7, 6, 7},
    {'q', 320, 0, -7, 5, 9},
    {'1', 320, 0, -9, 5, 9}
};
#define GLYPH_COUNT (sizeof(metrics) / sizeof(metrics[0]))

static os64_font_status_t allocate(os64_font_engine_t *engine, size_t bytes, void **out)
{
    *out = NULL;
    /* Cap refusal bypasses the callback at face and glyph allocation alike. */
    if (bytes > engine->cap - engine->stats.live_bytes) return OS64_FONT_LIMIT;
    *out = engine->memory.alloc(engine->memory.context, bytes);
    if (!*out) return OS64_FONT_NO_MEMORY;
    engine->stats.live_bytes += bytes;
    if (engine->stats.live_bytes > engine->stats.peak_bytes)
        engine->stats.peak_bytes = engine->stats.live_bytes;
    return OS64_FONT_OK;
}

static void deallocate(os64_font_engine_t *engine, void *p, size_t bytes)
{
    engine->memory.free(engine->memory.context, p, bytes);
    engine->stats.live_bytes -= bytes;
}

static os64_font_status_t engine_create(const os64_font_engine_options_t *options,
                                       os64_font_engine_t **out)
{
    if (out) *out = NULL;
    if (!out || !options || !options->memory.alloc || !options->memory.free ||
        options->memory_cap > OS64_FONT_MEMORY_MAX) return OS64_FONT_BAD_ARGUMENT;
    size_t cap = options->memory_cap ? options->memory_cap : OS64_FONT_MEMORY_DEFAULT;
    if (cap < sizeof(os64_font_engine_t)) return OS64_FONT_LIMIT;
    os64_font_engine_t *engine = options->memory.alloc(options->memory.context, sizeof(*engine));
    if (!engine) return OS64_FONT_NO_MEMORY;
    *engine = (os64_font_engine_t){.memory = options->memory, .cap = cap,
        .stats = {.live_bytes = sizeof(*engine), .peak_bytes = sizeof(*engine)}};
    *out = engine;
    return OS64_FONT_OK;
}

static os64_font_status_t engine_destroy(os64_font_engine_t *engine)
{
    if (!engine) return OS64_FONT_OK;
    if (engine->stats.live_faces || engine->stats.live_glyphs) return OS64_FONT_BUSY;
    os64_font_memory_t memory = engine->memory;
    memory.free(memory.context, engine, sizeof(*engine));
    return OS64_FONT_OK;
}

static os64_font_status_t engine_stats(os64_font_engine_t *engine, os64_font_engine_stats_t *out)
{
    if (out) *out = (os64_font_engine_stats_t){0};
    if (!engine || !out) return OS64_FONT_BAD_ARGUMENT;
    *out = engine->stats;
    return OS64_FONT_OK;
}

static os64_font_status_t face_open(os64_font_engine_t *engine, const uint8_t *bytes,
    size_t length, const os64_font_face_options_t *options, os64_font_face_t **out)
{
    if (out) *out = NULL;
    if (!engine || !bytes || !length || !options || !out || !options->pixel_height ||
        options->pixel_height > OS64_FONT_PIXEL_MAX ||
        (options->hint != OS64_FONT_HINT_NORMAL && options->hint != OS64_FONT_HINT_NONE))
        return OS64_FONT_BAD_ARGUMENT;
    if (length > OS64_FONT_FILE_MAX || engine->stats.live_faces == OS64_FONT_FACE_MAX)
        return OS64_FONT_LIMIT;
    if (length != 1) return OS64_FONT_MALFORMED;
    if (*bytes != 'P' && *bytes != 'M' && *bytes != 'L' && *bytes != 'S' && *bytes != 'T') return OS64_FONT_UNSUPPORTED;
    void *allocation;
    os64_font_status_t status = allocate(engine, sizeof(os64_font_face_t), &allocation);
    if (status != OS64_FONT_OK) return status;
    os64_font_face_t *face = allocation;
    *face = (os64_font_face_t){engine, bytes, options->pixel_height};
    ++engine->stats.live_faces;
    *out = face;
    return OS64_FONT_OK;
}

static void face_close(os64_font_face_t *face)
{
    if (!face) return;
    os64_font_engine_t *engine = face->engine;
    --engine->stats.live_faces;
    deallocate(engine, face, sizeof(*face));
}

static os64_font_status_t face_info(os64_font_face_t *face, os64_font_face_info_t *out)
{
    if (out) *out = (os64_font_face_info_t){0};
    if (!face || !out) return OS64_FONT_BAD_ARGUMENT;
    *out = (os64_font_face_info_t){.glyph_count = GLYPH_COUNT,
        .flags = (*face->source == 'M' || *face->source == 'T') ? OS64_FONT_FACE_FIXED_WIDTH : 0,
        .family = "Contract fixture", .style = "Regular",
        .ascent = 768, .descent = 256,
        .line_height = (*face->source == 'S' || *face->source == 'T') ? (int32_t)face->pixel_height * 64 : 1024};
    return OS64_FONT_OK;
}

static os64_font_status_t lookup(os64_font_face_t *face, uint32_t scalar, uint32_t *out)
{
    if (out) *out = 0;
    if (!face || !out || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
        return OS64_FONT_BAD_ARGUMENT;
    if (*face->source == 'T' && scalar >= 0x20 && scalar <= 0x7e) {
        *out = 1; return OS64_FONT_OK;
    }
    if (*face->source == 'L' && scalar == 'W') return OS64_FONT_MISSING;
    for (size_t i = 1; i < GLYPH_COUNT; ++i) {
        if (metrics[i].scalar == scalar) { *out = (uint32_t)i; return OS64_FONT_OK; }
    }
    return OS64_FONT_MISSING;
}

static os64_font_status_t pair_adjust(os64_font_face_t *face, uint32_t left,
                                      uint32_t right, os64_font_pos_t *out)
{
    if (out) *out = 0;
    if (!face || !out || !left || !right || left >= GLYPH_COUNT || right >= GLYPH_COUNT)
        return OS64_FONT_BAD_ARGUMENT;
    if (*face->source == 'P' && left == 3 && right == 4) *out = -64;
    /* A subpixel adjustment exposes callers that round each pair to pixels. */
    if (*face->source == 'P' && left == 7 && right == 2) *out = -29;
    return OS64_FONT_OK;
}

static os64_font_status_t render(os64_font_face_t *face, uint32_t id, os64_font_glyph_t **out)
{
    if (out) *out = NULL;
    if (!face || !out || !id || id >= GLYPH_COUNT) return OS64_FONT_BAD_ARGUMENT;
    os64_font_engine_t *engine = face->engine;
    void *allocation;
    os64_font_status_t status = allocate(engine, sizeof(os64_font_glyph_t), &allocation);
    if (status != OS64_FONT_OK) return status;
    os64_font_glyph_t *glyph = allocation;
    const metric_t *m = &metrics[id];
    *glyph = (os64_font_glyph_t){.engine = engine, .view = {
        .advance_x = (*face->source == 'M' || *face->source == 'T') ? 512 : m->advance,
        .left = m->left, .top = m->top, .width = m->width,
        .height = m->height, .stride = m->width}};
    size_t area = (size_t)m->width * m->height;
    if (area) {
        status = allocate(engine, area, &allocation);
        if (status != OS64_FONT_OK) { deallocate(engine, glyph, sizeof(*glyph)); return status; }
        uint8_t *coverage = allocation;
        /* Vary coverage so tests detect accidental reuse of a mutable slot. */
        for (size_t i = 0; i < area; ++i) coverage[i] = (uint8_t)(id * 17);
        glyph->view.coverage = coverage;
        glyph->view.ink = (os64_font_rect_t){m->left * 64, m->top * 64,
            (m->left + (int32_t)m->width) * 64, (m->top + (int32_t)m->height) * 64};
    }
    ++engine->stats.live_glyphs;
    *out = glyph;
    return OS64_FONT_OK;
}

static os64_font_status_t glyph_view(os64_font_glyph_t *glyph, os64_font_glyph_view_t *out)
{
    if (out) *out = (os64_font_glyph_view_t){0};
    if (!glyph || !out) return OS64_FONT_BAD_ARGUMENT;
    *out = glyph->view;
    return OS64_FONT_OK;
}

static void glyph_release(os64_font_glyph_t *glyph)
{
    if (!glyph) return;
    os64_font_engine_t *engine = glyph->engine;
    if (glyph->view.coverage) deallocate(engine, (void *)glyph->view.coverage,
                                        (size_t)glyph->view.stride * glyph->view.height);
    --engine->stats.live_glyphs;
    deallocate(engine, glyph, sizeof(*glyph));
}

static const os64_font_backend_t backend = {
    OS64_FONT_BACKEND_REVISION, sizeof(os64_font_backend_t), engine_create,
    engine_destroy, engine_stats, face_open, face_close, face_info, lookup,
    pair_adjust, render, glyph_view, glyph_release
};

const os64_font_backend_t *os64_fake_font_backend(void) { return &backend; }
