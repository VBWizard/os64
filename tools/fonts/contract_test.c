#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_backend.h"
#include "os64/text.h"
#include "os64/text_draw.h"

typedef struct { size_t calls, fail_at, live; } allocator_t;
typedef union { max_align_t alignment; size_t size; } block_t;
static void *test_alloc(void *context, size_t bytes)
{
    allocator_t *a = context;
    assert(bytes);
    if (++a->calls == a->fail_at) return NULL;
    block_t *block = malloc(sizeof(*block) + bytes);
    assert(block);
    block->size = bytes;
    a->live += bytes;
    return block + 1;
}
static void test_free(void *context, void *p, size_t bytes)
{
    allocator_t *a = context;
    block_t *block = (block_t *)p - 1;
    assert(block->size == bytes && a->live >= bytes);
    a->live -= bytes;
    free(block);
}
static os64_font_engine_options_t options(allocator_t *a)
{
    return (os64_font_engine_options_t){.memory = {a, test_alloc, test_free}};
}
static const os64_font_face_options_t face_options = {16, OS64_FONT_HINT_NORMAL};
static const uint8_t proportional[] = {'P'}, monospace[] = {'M'};

static void lifetimes(void)
{
    const os64_font_backend_t *b = os64_fake_font_backend();
    assert(b->revision == 1 && b->struct_size == sizeof(*b));
    allocator_t a = {0}, other = {0};
    os64_font_engine_options_t config = options(&a), config2 = options(&other);
    os64_font_engine_t *e, *e2;
    os64_font_face_t *f, *f2;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    assert(b->engine_create(&config2, &e2) == OS64_FONT_OK);
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_OK);
    assert(b->face_open(e2, monospace, 1, &face_options, &f2) == OS64_FONT_OK);
    assert(b->engine_destroy(e) == OS64_FONT_BUSY);
    os64_font_face_info_t info;
    assert(b->face_info(f2, &info) == OS64_FONT_OK && (info.flags & OS64_FONT_FACE_FIXED_WIDTH));
    uint32_t id;
    assert(b->lookup(f, 'i', &id) == OS64_FONT_OK && id == 1);
    os64_font_glyph_t *i, *w, *mono;
    os64_font_glyph_view_t iv, wv, mv;
    assert(b->render(f, id, &i) == OS64_FONT_OK);
    assert(b->glyph_view(i, &iv) == OS64_FONT_OK && iv.advance_x == 192);
    assert(b->render(f2, 1, &mono) == OS64_FONT_OK);
    assert(b->glyph_view(mono, &mv) == OS64_FONT_OK && mv.advance_x == 512);
    assert(b->render(f, 2, &w) == OS64_FONT_OK);
    assert(b->glyph_view(w, &wv) == OS64_FONT_OK && wv.advance_x == 576);
    assert(iv.coverage != wv.coverage && iv.coverage[0] == 17 && wv.coverage[0] == 34);
    b->face_close(f);
    b->face_close(f2);
    assert(b->engine_destroy(e) == OS64_FONT_BUSY);
    assert(b->glyph_view(i, &iv) == OS64_FONT_OK && iv.coverage[0] == 17);
    b->glyph_release(w);
    assert(iv.coverage[0] == 17);
    b->glyph_release(i);
    b->glyph_release(mono);
    os64_font_engine_stats_t stats;
    assert(b->engine_stats(e, &stats) == OS64_FONT_OK);
    assert(stats.live_faces == 0 && stats.live_glyphs == 0 && stats.live_bytes == a.live);
    assert(stats.peak_bytes > stats.live_bytes);
    assert(b->engine_destroy(e2) == OS64_FONT_OK && other.live == 0);
    assert(b->engine_destroy(e) == OS64_FONT_OK && a.live == 0);
    b->glyph_release(NULL); b->face_close(NULL);
    assert(b->engine_destroy(NULL) == OS64_FONT_OK);
}

static void geometry_and_errors(void)
{
    const os64_font_backend_t *b = os64_fake_font_backend();
    allocator_t a = {0};
    os64_font_engine_options_t config = options(&a);
    os64_font_engine_t *e;
    os64_font_face_t *f;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_OK);
    uint32_t id = 99;
    assert(b->lookup(f, 0x2603, &id) == OS64_FONT_MISSING && id == 0);
    id = 99;
    assert(b->lookup(f, 0xd800, &id) == OS64_FONT_BAD_ARGUMENT && id == 0);
    assert(b->lookup(f, 0x110000, &id) == OS64_FONT_BAD_ARGUMENT);
    os64_font_pos_t delta;
    assert(b->pair_adjust(f, 3, 4, &delta) == OS64_FONT_OK && delta == -64);
    assert(b->pair_adjust(f, 7, 2, &delta) == OS64_FONT_OK && delta == -29);
    assert(b->pair_adjust(f, 1, 2, &delta) == OS64_FONT_OK && delta == 0);
    os64_font_glyph_t *glyph;
    os64_font_glyph_view_t view;
    assert(b->render(f, 5, &glyph) == OS64_FONT_OK);
    assert(b->glyph_view(glyph, &view) == OS64_FONT_OK);
    assert(view.ink.x0 == -128 && view.ink.y1 == 128 && view.advance_x == 256);
    b->glyph_release(glyph);
    assert(b->render(f, 6, &glyph) == OS64_FONT_OK);
    assert(b->glyph_view(glyph, &view) == OS64_FONT_OK);
    assert(view.advance_x == 256 && !view.coverage && !view.width && !view.ink.x1);
    b->glyph_release(glyph);
    assert(b->render(f, 9, &glyph) == OS64_FONT_OK);
    assert(b->glyph_view(glyph, &view) == OS64_FONT_OK && view.advance_x == 0 && view.width == 2);
    b->glyph_release(glyph);
    glyph = (os64_font_glyph_t *)(uintptr_t)1;
    assert(b->render(f, 0, &glyph) == OS64_FONT_BAD_ARGUMENT && !glyph);
    delta = 99;
    assert(b->pair_adjust(f, 0, 2, &delta) == OS64_FONT_BAD_ARGUMENT && delta == 0);
    assert(b->glyph_view(NULL, &view) == OS64_FONT_BAD_ARGUMENT && !view.coverage && !view.advance_x);
    b->face_close(f);
    f = (os64_font_face_t *)(uintptr_t)1;
    assert(b->face_open(e, proportional, OS64_FONT_FILE_MAX + (size_t)1,
                        &face_options, &f) == OS64_FONT_LIMIT && !f);
    os64_font_face_t *faces[OS64_FONT_FACE_MAX];
    for (size_t n = 0; n < OS64_FONT_FACE_MAX; ++n)
        assert(b->face_open(e, proportional, 1, &face_options, &faces[n]) == OS64_FONT_OK);
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_LIMIT && !f);
    for (size_t n = 0; n < OS64_FONT_FACE_MAX; ++n) b->face_close(faces[n]);
    assert(b->engine_destroy(e) == OS64_FONT_OK && a.live == 0);
    config.memory_cap = 1;
    assert(b->engine_create(&config, &e) == OS64_FONT_LIMIT && !e);
}

static void failure_sweep(void)
{
    const os64_font_backend_t *b = os64_fake_font_backend();
    /* Four allocation sites: engine, face, glyph descriptor, pixel buffer.
     * The fifth iteration proves a successful path with the same harness. */
    for (size_t fail = 1; fail <= 5; ++fail) {
        allocator_t a = {.fail_at = fail};
        os64_font_engine_options_t config = options(&a);
        os64_font_engine_t *e = NULL;
        os64_font_face_t *f = NULL;
        os64_font_glyph_t *g = NULL;
        os64_font_status_t status = b->engine_create(&config, &e);
        if (status == OS64_FONT_OK) status = b->face_open(e, proportional, 1, &face_options, &f);
        size_t before = a.live;
        if (status == OS64_FONT_OK) status = b->render(f, 2, &g);
        if (fail <= 4) {
            assert(status == OS64_FONT_NO_MEMORY && !g);
            if (f) {
                assert(a.live == before);
                /* Allocation failure must not poison a surviving face. */
                a.fail_at = 0;
                assert(b->render(f, 1, &g) == OS64_FONT_OK);
            }
        } else assert(status == OS64_FONT_OK && g);
        b->glyph_release(g); b->face_close(f);
        assert(b->engine_destroy(e) == OS64_FONT_OK && a.live == 0);
    }
    /* A budget refusal is distinct from allocator failure and is retry-safe. */
    allocator_t a = {0};
    os64_font_engine_options_t config = options(&a);
    os64_font_engine_t *e;
    os64_font_engine_stats_t stats;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    assert(b->engine_stats(e, &stats) == OS64_FONT_OK);
    size_t engine_size = stats.live_bytes;
    assert(b->engine_destroy(e) == OS64_FONT_OK);
    config.memory_cap = engine_size;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    os64_font_face_t *f;
    size_t calls = a.calls;
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_LIMIT && !f);
    assert(a.calls == calls);
    assert(b->engine_destroy(e) == OS64_FONT_OK && !a.live);
    config.memory_cap = 0;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_OK);
    assert(b->engine_stats(e, &stats) == OS64_FONT_OK);
    size_t face_baseline = stats.live_bytes;
    os64_font_glyph_t *held, *next = NULL;
    assert(b->render(f, 2, &held) == OS64_FONT_OK);
    assert(b->engine_stats(e, &stats) == OS64_FONT_OK);
    config.memory_cap = stats.live_bytes;
    assert(config.memory_cap > face_baseline);
    b->glyph_release(held); b->face_close(f);
    assert(b->engine_destroy(e) == OS64_FONT_OK);
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    assert(b->face_open(e, proportional, 1, &face_options, &f) == OS64_FONT_OK);
    assert(b->render(f, 2, &held) == OS64_FONT_OK);
    calls = a.calls;
    assert(b->render(f, 2, &next) == OS64_FONT_LIMIT && !next);
    assert(a.calls == calls);
    b->glyph_release(held);
    a.fail_at = a.calls + 1;
    assert(b->render(f, 2, &next) == OS64_FONT_NO_MEMORY && !next);
    assert(a.calls == calls + 1);
    a.fail_at = 0;
    assert(b->render(f, 2, &next) == OS64_FONT_OK);
    b->glyph_release(next); b->face_close(f);
    assert(b->engine_destroy(e) == OS64_FONT_OK && !a.live);

}

static void dump_metrics(void)
{
    const os64_font_backend_t *b = os64_fake_font_backend();
    allocator_t a = {0};
    os64_font_engine_options_t config = options(&a);
    os64_font_engine_t *e;
    assert(b->engine_create(&config, &e) == OS64_FONT_OK);
    const uint8_t faces[] = {'P', 'M', 'L'};
    const uint32_t scalars[] = {'i', 'W', 'A', 'V', 'j', ' ', 'e', 0xe9, 0x301, 'x', 'q', '1'};
    puts("{");
    for (size_t n = 0; n < sizeof(faces); ++n) {
        os64_font_face_t *f;
        assert(b->face_open(e, &faces[n], 1, &face_options, &f) == OS64_FONT_OK);
        printf("\"%c\":{\"glyphs\":[", faces[n]);
        for (uint32_t id = 1; id <= sizeof(scalars) / sizeof(scalars[0]); ++id) {
            os64_font_glyph_t *g;
            os64_font_glyph_view_t v;
            assert(b->render(f, id, &g) == OS64_FONT_OK);
            assert(b->glyph_view(g, &v) == OS64_FONT_OK);
            uint32_t found;
            os64_font_status_t status = b->lookup(f, scalars[id - 1], &found);
            assert(status == OS64_FONT_OK || status == OS64_FONT_MISSING);
            printf("%s{\"id\":%u,\"scalar\":%u,\"present\":%s,\"advance\":%d,"
                   "\"ink\":[%d,%d,%d,%d]}", id == 1 ? "" : ",", id,
                   scalars[id - 1], status == OS64_FONT_OK ? "true" : "false",
                   v.advance_x, v.ink.x0, v.ink.y0, v.ink.x1, v.ink.y1);
            b->glyph_release(g);
        }
        os64_font_pos_t delta, fractional;
        assert(b->pair_adjust(f, 3, 4, &delta) == OS64_FONT_OK);
        assert(b->pair_adjust(f, 7, 2, &fractional) == OS64_FONT_OK);
        printf("],\"pairs\":[[3,4,%d],[7,2,%d]]}%s\n", delta, fractional,
               n + 1 == sizeof(faces) ? "" : ",");
        b->face_close(f);
    }
    puts("}");
    assert(b->engine_destroy(e) == OS64_FONT_OK && !a.live);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--dump") == 0) { dump_metrics(); return 0; }
    assert(argc == 1);
    lifetimes(); geometry_and_errors(); failure_sweep();
    puts("PASS: backend table v1 lifetimes, geometry, errors and allocation sweep");
    return 0;
}
