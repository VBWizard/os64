#include <assert.h>
#include <math.h>
#include <os64/geometry_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OK OS64_GEOMETRY_OK
#define BAD OS64_GEOMETRY_BAD_ARGUMENT
#define LIMIT OS64_GEOMETRY_LIMIT
#define NOMEM OS64_GEOMETRY_NO_MEMORY
#define CANCEL OS64_GEOMETRY_CANCELLED
#define BUSY OS64_GEOMETRY_BUSY
#define UNSUPPORTED OS64_GEOMETRY_UNSUPPORTED
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define W 192
#define H 128
typedef os64_geometry_context_t Context;
typedef os64_geometry_outline_t Outline;
typedef os64_geometry_command_t Cmd;
typedef os64_geometry_status_t Status;
typedef os64_geometry_map_t Map;
static const os64_geometry_backend_t *api;
static const Map identity = {1, 0, 0, 1, 0, 0};
static const Cmd line[] = {{OS64_GEOMETRY_MOVE, {{12, 48}}}, {OS64_GEOMETRY_LINE, {{36, 48}}}};
static const os64_geometry_stroke_t round_stroke = {16, 4, OS64_GEOMETRY_ROUND_CAP,
                                                    OS64_GEOMETRY_ROUND_JOIN};
typedef union {
    max_align_t align;
    size_t bytes;
} Block;
typedef struct {
    size_t calls, fail_at, live, peak, cap, polls, cancel_at;
    Context *context;
    bool reenter;
} Ledger;
static void busy_check(Ledger *l)
{
    if (!l->reenter || !l->context)
        return;
    os64_geometry_stats_t stats;
    assert(api->stats(l->context, &stats) == BUSY);
    assert(api->destroy(l->context) == BUSY);
    Outline *out = (void *)1;
    assert(api->path(l->context, line, COUNT(line), &identity, 1.0 / 64, &out) == BUSY && !out);
}
static Status alloc(void *arg, size_t bytes, void **out)
{
    Ledger *l = arg;
    *out = NULL;
    ++l->calls;
    busy_check(l);
    if (l->calls == l->fail_at)
        return NOMEM;
    if (bytes > l->cap - l->live)
        return LIMIT;
    Block *b = malloc(sizeof(*b) + bytes);
    if (!b)
        return NOMEM;
    b->bytes = bytes;
    l->live += bytes;
    if (l->live > l->peak)
        l->peak = l->live;
    *out = b + 1;
    return OK;
}
static void dealloc(void *arg, void *ptr, size_t bytes)
{
    Ledger *l = arg;
    busy_check(l);
    Block *b = (Block *)ptr - 1;
    assert(bytes == b->bytes && l->live >= bytes);
    l->live -= bytes;
    free(b);
}
static bool cancelled(void *arg)
{
    Ledger *l = arg;
    busy_check(l);
    ++l->polls;
    return l->cancel_at && l->polls >= l->cancel_at;
}
static Status start(Ledger *l, uint64_t work, Context **out)
{
    os64_geometry_options_t options = {{l, alloc, dealloc}, work, l, cancelled};
    Status s = api->create(&options, out);
    l->context = *out;
    return s;
}
static void close_context(Ledger *l, Context *c)
{
    assert(api->destroy(c) == OK);
    l->context = NULL;
    assert(!l->live);
}
static Status draw(Context *c, Outline *o, Map m, uint8_t *pixels, bool evenodd)
{
    os64_geometry_target_t target = {0, 0, W, H, W, W * H, pixels, evenodd};
    return api->raster(c, o, &m, &target);
}
/* Independent area-coverage reference, also used by the exploratory proof. */
static void reference(uint8_t *out, Map m)
{
    double det = m.a * m.d - m.b * m.c;
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            unsigned hit = 0;
            for (unsigned j = 0; j < 64; ++j)
                for (unsigned i = 0; i < 64; ++i) {
                    double dx = x + (i + .5) / 64 - m.tx, dy = y + (j + .5) / 64 - m.ty;
                    double ux = (m.d * dx - m.c * dy) / det, uy = (-m.b * dx + m.a * dy) / det;
                    double near = fmax(12, fmin(36, ux));
                    hit += (ux - near) * (ux - near) + (uy - 48) * (uy - 48) <= 64;
                }
            out[y * W + x] = (uint8_t)((hit * 255 + 2048) / 4096);
        }
}
static void compare(const uint8_t *actual, const uint8_t *expected)
{
    double total = 0;
    unsigned max = 0;
    for (size_t i = 0; i < W * H; ++i) {
        unsigned d = (unsigned)abs((int)actual[i] - expected[i]);
        if (d > max)
            max = d;
        total += d / 255.0;
    }
    printf("backend capsule: L1 %.6f, maximum %u/255\n", total, max);
    assert(total <= 8 && max <= 24);
}
static const char *artifact_dir;
static void save_mask(const char *name, const uint8_t *pixels)
{
    if (!artifact_dir)
        return;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s.pgm", artifact_dir, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file && fprintf(file, "P5\n%d %d\n255\n", W, H) > 0);
    assert(fwrite(pixels, 1, W * H, file) == W * H);
    assert(fclose(file) == 0);
}
static void pictures(void)
{
    Ledger l = {.cap = SIZE_MAX};
    Context *c;
    assert(start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c) == OK);
    l.reenter = true;
    Map maps[] = {{4, 0, 0, 1, 0, 0}, {-2, .2, .5, 1, 130, 0}};
    uint8_t actual[W * H], expected[W * H];
    for (size_t i = 0; i < COUNT(maps); ++i) {
        Outline *o;
        assert(api->stroke(c, line, COUNT(line), &maps[i], 1.0 / 64, &round_stroke, &o) == OK);
        assert(api->destroy(c) == BUSY);
        assert(draw(c, o, maps[i], actual, false) == OK);
        if (i == 0)
            save_mask("cap", actual);
        reference(expected, maps[i]);
        compare(actual, expected);
        uint8_t crop[24 * 40];
        memset(crop, 0xa5, sizeof(crop));
        os64_geometry_target_t t = {20, 35, 35, 24, 40, sizeof(crop), crop, false};
        assert(api->raster(c, o, &maps[i], &t) == OK);
        for (unsigned y = 0; y < 24; ++y) {
            assert(!memcmp(crop + y * 40, actual + (y + 35) * W + 20, 35));
            for (unsigned x = 35; x < 40; ++x)
                assert(crop[y * 40 + x] == 0xa5);
        }
        assert(api->release(c, o) == OK);
    }
    /* Non-scaling stroke: caller supplies a device-space centerline and width. */
    Cmd device_line[] = {{OS64_GEOMETRY_MOVE, {{48, 48}}}, {OS64_GEOMETRY_LINE, {{144, 48}}}};
    Outline *device_outline;
    assert(api->stroke(c, device_line, COUNT(device_line), &identity, 1.0 / 64, &round_stroke,
                       &device_outline) == OK);
    assert(draw(c, device_outline, identity, actual, false) == OK);
    save_mask("non-scaling", actual);
    assert(api->release(c, device_outline) == OK);
    Cmd shape[] = {{OS64_GEOMETRY_MOVE, {{20, 20}}},
                   {OS64_GEOMETRY_LINE, {{100, 20}}},
                   {OS64_GEOMETRY_LINE, {{100, 100}}},
                   {OS64_GEOMETRY_LINE, {{20, 100}}},
                   {OS64_GEOMETRY_CLOSE},
                   {OS64_GEOMETRY_MOVE, {{40, 40}}},
                   {OS64_GEOMETRY_LINE, {{80, 40}}},
                   {OS64_GEOMETRY_LINE, {{80, 80}}},
                   {OS64_GEOMETRY_LINE, {{40, 80}}}};
    Outline *o;
    assert(api->path(c, shape, COUNT(shape), &identity, 1.0 / 64, &o) == OK);
    memset(shape, 0, sizeof(shape)); /* The caller's input is no longer borrowed. */
    assert(draw(c, o, identity, actual, false) == OK && actual[60 * W + 60] == 255);
    save_mask("nonzero", actual);
    assert(draw(c, o, identity, expected, true) == OK && !expected[60 * W + 60]);
    assert(expected[30 * W + 30] == 255);
    save_mask("evenodd", expected);
    assert(api->release(c, o) == OK);
    Cmd mixed[] = {{OS64_GEOMETRY_MOVE, {{20, 20}}},
                   {OS64_GEOMETRY_LINE, {{70, 20}}},
                   {OS64_GEOMETRY_LINE, {{70, 70}}},
                   {OS64_GEOMETRY_MOVE, {{105, 20}}},
                   {OS64_GEOMETRY_LINE, {{155, 20}}},
                   {OS64_GEOMETRY_LINE, {{155, 70}}},
                   {OS64_GEOMETRY_CLOSE}};
    os64_geometry_stroke_t style = {8, 4, OS64_GEOMETRY_BUTT, OS64_GEOMETRY_BEVEL};
    assert(api->stroke(c, mixed, COUNT(mixed), &identity, 1.0 / 64, &style, &o) == OK);
    os64_geometry_outline_info_t info;
    assert(api->outline_info(c, o, &info) == OK && info.contours == 3);
    assert(draw(c, o, identity, actual, false) == OK && !actual[45 * W + 45] &&
           actual[45 * W + 130] == 255);
    assert(api->release(c, o) == OK);
    os64_geometry_stats_t stats;
    assert(api->stats(c, &stats) == OK && stats.live_bytes == l.live &&
           stats.peak_bytes == l.peak && !stats.live_outlines);
    close_context(&l, c);
}
static Status operation(Context *c)
{
    Cmd path[] = {{OS64_GEOMETRY_MOVE, {{20, 60}}},
                  {OS64_GEOMETRY_QUADRATIC, {{50, 5}, {80, 40}}},
                  {OS64_GEOMETRY_CUBIC, {{110, 110}, {140, 5}, {170, 70}}}};
    Outline *o = NULL;
    const Cmd caps[] = {{OS64_GEOMETRY_MOVE, {{16, 56}}},
                        {OS64_GEOMETRY_LINE, {{24, 56}}},
                        {OS64_GEOMETRY_LINE, {{24, 64}}},
                        {OS64_GEOMETRY_LINE, {{16, 64}}},
                        {OS64_GEOMETRY_CLOSE}};
    Status s = api->stroke_with_caps(c, path, COUNT(path), caps, COUNT(caps), &identity, 1.0 / 64,
                                     &round_stroke, &o);
    if (s) {
        assert(!o);
        return s;
    }
    uint8_t pixels[W * H];
    s = draw(c, o, identity, pixels, false);
    assert(api->release(c, o) == OK);
    return s;
}
static void failures(void)
{
    Ledger l = {.cap = SIZE_MAX};
    Context *c = NULL;
    assert(start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c) == OK && operation(c) == OK);
    size_t calls = l.calls, polls = l.polls, peak = l.peak;
    close_context(&l, c);
    for (size_t i = 1; i <= calls; ++i) {
        l = (Ledger){.cap = SIZE_MAX, .fail_at = i};
        Status s = start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c);
        if (!s)
            s = operation(c);
        assert(s == NOMEM);
        if (c)
            close_context(&l, c);
        assert(!l.live);
    }
    for (size_t i = 1; i <= polls; ++i) {
        l = (Ledger){.cap = SIZE_MAX, .cancel_at = i};
        Status s = start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c);
        if (!s) {
            l.reenter = true;
            s = operation(c);
        }
        assert(s == CANCEL);
        if (c)
            close_context(&l, c);
        assert(!l.live);
    }
    l = (Ledger){.cap = peak / 2};
    Status s = start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c);
    if (!s)
        s = operation(c);
    assert(s == LIMIT);
    if (c)
        close_context(&l, c);
    assert(!l.live);
    l = (Ledger){.cap = SIZE_MAX};
    assert(start(&l, 0, &c) == LIMIT && !c && !l.live);
    printf("backend sweeps: %zu allocation cuts, %zu cancellation cuts; peak %zu bytes\n", calls,
           polls, peak);
}
static void contracts(void)
{
    Ledger a = {.cap = SIZE_MAX}, b = {.cap = SIZE_MAX};
    Context *ca, *cb;
    assert(start(&a, OS64_GEOMETRY_WORK_DEFAULT, &ca) == OK &&
           start(&b, OS64_GEOMETRY_WORK_DEFAULT, &cb) == OK);
    Outline *o;
    assert(api->path(ca, line, COUNT(line), &identity, 1.0 / 64, &o) == OK);
    os64_geometry_outline_info_t info;
    assert(api->release(cb, o) == BAD && api->outline_info(cb, o, &info) == BAD);
    assert(api->destroy(ca) == BUSY);
    assert(api->destroy(cb) == OK && !b.live);
    uint8_t pixels[W * H];
    Map bigger = {65536, 0, 0, 65536, 0, 0};
    assert(draw(ca, o, bigger, pixels, false) ==
           LIMIT); /* Planned precision cannot silently degrade. */
    Map singular = {0, 0, 0, 1, 0, 0};
    assert(draw(ca, o, singular, pixels, false) == OK);
    for (size_t i = 0; i < sizeof(pixels); ++i)
        assert(!pixels[i]);
    os64_geometry_target_t t = {0, 0, W, H, W, 10, pixels, false};
    assert(api->raster(ca, o, &identity, &t) == LIMIT);
    assert(api->release(ca, o) == OK);
    Outline *out = (void *)1;
    assert(api->stroke_with_caps(ca, NULL, 0, NULL, 0, &identity, 1.0 / 64, &round_stroke, &out) ==
               BAD &&
           !out);
    assert(api->stroke_with_caps(ca, line, COUNT(line), NULL, 1, &identity, 1.0 / 64, &round_stroke,
                                 &out) == BAD &&
           !out);
    assert(api->stroke_with_caps(ca, line, COUNT(line), line + 1, 1, &identity, 1.0 / 64,
                                 &round_stroke, &out) == BAD &&
           !out);
    assert(api->stroke_with_caps(ca, line + 1, 1, line, COUNT(line), &identity, 1.0 / 64,
                                 &round_stroke, &out) == BAD &&
           !out);
    assert(api->stroke_with_caps(ca, line, COUNT(line), line, OS64_GEOMETRY_COMMAND_MAX, &identity,
                                 1.0 / 64, &round_stroke, &out) == LIMIT &&
           !out);
    assert(api->path(ca, NULL, 2, &identity, 1.0 / 64, &out) == BAD && !out);
    assert(api->path(ca, line, COUNT(line), &identity, 1.0 / 128, &out) == BAD && !out);
    assert(api->path(ca, line, OS64_GEOMETRY_COMMAND_MAX + 1, &identity, 1.0 / 64, &out) == LIMIT);
    Cmd bad[] = {{OS64_GEOMETRY_MOVE, {{0, 0}}}, {OS64_GEOMETRY_LINE, {{NAN, 1}}}};
    assert(api->path(ca, bad, COUNT(bad), &identity, 1.0 / 64, &out) == BAD);
    bad[1].p[0].x = 1e30;
    assert(api->path(ca, bad, COUNT(bad), &identity, 1.0 / 64, &out) == LIMIT);
    bad[1].p[0].x = 0;
    bad[1].p[0].y = 0;
    assert(api->stroke(ca, bad, COUNT(bad), &identity, 1.0 / 64, &round_stroke, &out) ==
               UNSUPPORTED &&
           !out);
    os64_geometry_stroke_t style = round_stroke;
    style.cap = OS64_GEOMETRY_BUTT;
    assert(api->stroke(ca, bad, COUNT(bad), &identity, 1.0 / 64, &style, &out) == OK);
    assert(api->outline_info(ca, out, &info) == OK && !info.points);
    assert(api->release(ca, out) == OK);
    style.join = OS64_GEOMETRY_ARCS;
    assert(api->stroke(ca, line, COUNT(line), &identity, 1.0 / 64, &style, &out) == UNSUPPORTED);
    style.join = OS64_GEOMETRY_MITER;
    style.miter_limit = .9;
    assert(api->stroke(ca, line, COUNT(line), &identity, 1.0 / 64, &style, &out) == BAD);
    a.fail_at = a.calls + 1;
    assert(operation(ca) == NOMEM);
    a.fail_at = 0;
    assert(operation(ca) == OK); /* A failed operation does not poison later calls. */
    a.cancel_at = a.polls + 1;
    assert(operation(ca) == CANCEL);
    a.cancel_at = 0;
    assert(operation(ca) == OK);
    os64_geometry_stats_t stats;
    assert(api->stats(ca, &stats) == OK && stats.live_bytes == a.live);
    close_context(&a, ca);
}
static void collapsed_curves(void)
{
    Ledger ledger = {.cap = SIZE_MAX};
    Context *c;
    assert(start(&ledger, OS64_GEOMETRY_WORK_DEFAULT, &c) == OK);
    os64_geometry_stats_t before, after;
    assert(api->stats(c, &before) == OK);
    const double step = 1.0 / 256;
    for (unsigned cubic = 0; cubic < 2; ++cubic) {
        Cmd tiny[] = {{OS64_GEOMETRY_MOVE, {{20, 20}}},
                      {cubic ? OS64_GEOMETRY_CUBIC : OS64_GEOMETRY_QUADRATIC,
                       {{20 + step, 20}, {20 + step, 20}, {20 + step, 20}}},
                      {OS64_GEOMETRY_CLOSE}};
        for (unsigned square = 0; square < 2; ++square) {
            os64_geometry_stroke_t style = round_stroke;
            style.cap = square ? OS64_GEOMETRY_SQUARE : OS64_GEOMETRY_ROUND_CAP;
            for (unsigned closed = 0; closed < 2; ++closed) {
                size_t count = 2 + closed;
                for (unsigned placement = 0; placement < 3; ++placement) {
                    Cmd path[5];
                    size_t n = 0;
                    if (placement == 1) {
                        path[n++] = (Cmd){OS64_GEOMETRY_MOVE, {{100, 100}}};
                        path[n++] = (Cmd){OS64_GEOMETRY_LINE, {{120, 100}}};
                    }
                    for (size_t j = 0; j < count; ++j)
                        path[n++] = tiny[j];
                    if (placement == 2) {
                        path[n++] = (Cmd){OS64_GEOMETRY_MOVE, {{100, 100}}};
                        path[n++] = (Cmd){OS64_GEOMETRY_LINE, {{120, 100}}};
                    }
                    Outline *out = (void *)1;
                    Status result = api->stroke(c, path, n, &identity, 1.0 / 64, &style, &out);
                    if (result != UNSUPPORTED)
                        printf("collapsed curve: cubic=%u square=%u closed=%u placement=%u "
                               "status=%u\n",
                               cubic, square, closed, placement, (unsigned)result);
                    assert(result == UNSUPPORTED && !out);
                    assert(api->stats(c, &after) == OK && after.live_bytes == before.live_bytes &&
                           !after.live_outlines);
                }
            }
        }
        os64_geometry_stroke_t style = round_stroke;
        style.cap = OS64_GEOMETRY_BUTT;
        Outline *out;
        os64_geometry_outline_info_t info;
        assert(api->stroke(c, tiny, 2, &identity, 1.0 / 64, &style, &out) == OK);
        assert(api->outline_info(c, out, &info) == OK && !info.points);
        assert(api->release(c, out) == OK);
        /* Exactly two working units is retained by both curve operations. */
        tiny[1].p[0].x = tiny[1].p[1].x = tiny[1].p[2].x = 20 + 2 * step;
        for (unsigned square = 0; square < 2; ++square) {
            style.cap = square ? OS64_GEOMETRY_SQUARE : OS64_GEOMETRY_ROUND_CAP;
            assert(api->stroke(c, tiny, 2, &identity, 1.0 / 64, &style, &out) == OK);
            assert(api->outline_info(c, out, &info) == OK && info.points);
            assert(api->release(c, out) == OK);
        }
    }
    /* Successive tiny curves update the stroker's center even when discarded.
     * The accumulated distance from the initial MOVE does not make them draw. */
    Cmd drift[] = {
        {OS64_GEOMETRY_MOVE, {{20, 20}}},
        {OS64_GEOMETRY_QUADRATIC, {{20 + step, 20}, {20 + 2 * step, 20}}},
        {OS64_GEOMETRY_CUBIC, {{20 + 3 * step, 20}, {20 + 4 * step, 20}, {20 + 5 * step, 20}}},
        {OS64_GEOMETRY_LINE, {{20 + 5 * step, 20}}}};
    Outline *out;
    assert(api->stroke(c, drift, COUNT(drift), &identity, 1.0 / 64, &round_stroke, &out) ==
               UNSUPPORTED &&
           !out);
    drift[3].p[0].x += step; /* A one-unit line is drawable, unlike a one-unit curve edge. */
    assert(api->stroke(c, drift, COUNT(drift), &identity, 1.0 / 64, &round_stroke, &out) == OK);
    uint8_t pixels[W * H];
    assert(draw(c, out, identity, pixels, false) == OK && pixels[20 * W + 20] == 255);
    assert(api->release(c, out) == OK);
    assert(operation(c) == OK); /* Fallback refusal leaves the context usable. */
    close_context(&ledger, c);
    puts("collapsed curves: quadratic/cubic, round/square, closure, mixed subpaths and threshold "
         "checks pass");
}
static void boundaries(void)
{
    Ledger l = {.cap = SIZE_MAX};
    Context *c;
    assert(start(&l, UINT64_MAX, &c) == OK);
    Outline *o;
    uint8_t baseline[W * H], actual[W * H];
    Map map = {4, 0, 0, 1, 0, 0};
    assert(api->stroke(c, line, COUNT(line), &map, 1.0 / 64, &round_stroke, &o) == OK);
    assert(draw(c, o, map, baseline, false) == OK);
    assert(api->release(c, o) == OK);
    Cmd tiny[2], far[2];
    memcpy(tiny, line, sizeof(tiny));
    memcpy(far, line, sizeof(far));
    for (size_t i = 0; i < 2; ++i) {
        tiny[i].p[0].x /= 16384;
        tiny[i].p[0].y /= 16384;
        far[i].p[0].x += 1e9;
        far[i].p[0].y += 1e9;
    }
    os64_geometry_stroke_t style = round_stroke;
    style.width /= 16384;
    Map zoom = {4 * 16384, 0, 0, 16384, 0, 0};
    assert(api->stroke(c, tiny, COUNT(tiny), &zoom, 1.0 / 64, &style, &o) == OK);
    assert(draw(c, o, zoom, actual, false) == OK && !memcmp(actual, baseline, sizeof(actual)));
    assert(api->release(c, o) == OK);
    Map translated = {4, 0, 0, 1, -4e9, -1e9};
    assert(api->stroke(c, far, COUNT(far), &translated, 1.0 / 64, &round_stroke, &o) == OK);
    assert(draw(c, o, translated, actual, false) == OK &&
           !memcmp(actual, baseline, sizeof(actual)));
    assert(api->release(c, o) == OK);
    Cmd corner[] = {{OS64_GEOMETRY_MOVE, {{30, 40}}},
                    {OS64_GEOMETRY_LINE, {{80, 40}}},
                    {OS64_GEOMETRY_LINE, {{80, 90}}}};
    for (unsigned i = 0; i < 2; ++i) {
        style = (os64_geometry_stroke_t){8, (92681.0 + i) / 65536, OS64_GEOMETRY_BUTT,
                                         OS64_GEOMETRY_MITER};
        assert(api->stroke(c, corner, COUNT(corner), &identity, 1.0 / 64, &style, &o) == OK);
        os64_geometry_outline_info_t info;
        assert(api->outline_info(c, o, &info) == OK && info.points == 8 - i);
        assert(draw(c, o, identity, actual, false) == OK);
        assert((actual[36 * W + 83] == 255) == (i == 1));
        assert(api->release(c, o) == OK);
    }
    Cmd edge[] = {{OS64_GEOMETRY_MOVE, {{0, 0}}}, {OS64_GEOMETRY_LINE, {{4096, 0}}}};
    assert(api->path(c, edge, COUNT(edge), &identity, 1.0 / 64, &o) == OK);
    assert(api->release(c, o) == OK);
    edge[1].p[0].x += 1.0 / 256;
    assert(api->path(c, edge, COUNT(edge), &identity, 1.0 / 64, &o) == LIMIT && !o);
    Cmd close = {.verb = OS64_GEOMETRY_CLOSE};
    assert(api->path(c, &close, 1, &identity, 1.0 / 64, &o) == BAD && !o);
    Cmd expanded[OS64_GEOMETRY_COMMAND_MAX];
    for (size_t i = 0; i < COUNT(expanded); i += 2) {
        expanded[i] = (Cmd){OS64_GEOMETRY_MOVE, {{0, 0}}};
        expanded[i + 1] = (Cmd){OS64_GEOMETRY_CUBIC, {{100, 100}, {-100, 100}, {0, 0}}};
    }
    style = (os64_geometry_stroke_t){4, 4, OS64_GEOMETRY_ROUND_CAP, OS64_GEOMETRY_ROUND_JOIN};
    assert(api->stroke(c, expanded, COUNT(expanded), &identity, 1.0 / 64, &style, &o) == LIMIT &&
           !o);
    os64_geometry_stats_t stats;
    assert(api->stats(c, &stats) == OK && !stats.live_outlines);
    printf("backend expanded-outline refusal: %llu work units, %zu peak bytes\n",
           (unsigned long long)stats.last_work, stats.peak_bytes);
    close_context(&l, c);
    l = (Ledger){.cap = SIZE_MAX};
    assert(start(&l, 65536, &c) == OK);
    assert(api->path(c, line, COUNT(line), &identity, 1.0 / 64, &o) == OK);
    uint8_t large[512 * 512];
    os64_geometry_target_t t = {0, 0, 512, 512, 512, sizeof(large), large, false};
    assert(api->raster(c, o, &identity, &t) == LIMIT);
    assert(api->stats(c, &stats) == OK && stats.last_work <= 65536);
    assert(draw(c, o, identity, actual, false) == OK); /* Work allowance resets per call. */
    l.cancel_at = l.polls + 1;
    assert(api->release(c, o) == OK); /* Teardown survives cancellation. */
    close_context(&l, c);
    puts("backend boundaries: zoom, translation, miter, counts, precision and work limits pass");
}
static void allocation_bounds(void)
{
    Ledger l = {.cap = 1024 * 1024};
    Context *c = NULL;
    assert(start(&l, OS64_GEOMETRY_WORK_DEFAULT, &c) == OK);
    Outline *o = NULL;
    Map map = {4, 0, 0, 1, 0, 0};
    assert(api->stroke(c, line, COUNT(line), &map, 1.0 / 64, &round_stroke, &o) == OK);
    size_t allocations = l.calls;
    os64_geometry_bounds_t b;
    assert(api->bounds(c, o, &map, 8, 5, &b) == OK && !b.empty && allocations == l.calls);
    assert(b.x_min <= 8 && b.x_max >= 168 && b.y_min <= 35 && b.y_max >= 51);
    os64_geometry_bounds_t shifted;
    assert(api->bounds(c, o, &map, 0, 0, &shifted) == OK);
    assert(b.x_min + 8 == shifted.x_min && b.x_max + 8 == shifted.x_max);
    assert(b.y_min + 5 == shifted.y_min && b.y_max + 5 == shifted.y_max);
    l.cancel_at = l.polls + 1;
    assert(api->bounds(c, o, &map, 0, 0, &b) == CANCEL && b.empty);
    l.cancel_at = 0;
    map.tx = 1e15;
    assert(api->bounds(c, o, &map, 0, 0, &b) == LIMIT && b.empty);
    assert(api->release(c, o) == OK);
    close_context(&l, c);
    puts(
        "backend bounds: expanded stroke, relative origin, allocation-free and failure reset pass");
}
int main(int argc, char **argv)
{
    artifact_dir = argc > 1 ? argv[1] : NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    api = os64_freetype_geometry_v1();
    assert(api && api->revision == OS64_GEOMETRY_BACKEND_REVISION &&
           api->struct_size == sizeof(*api));
    collapsed_curves();
    allocation_bounds();
    boundaries();
    pictures();
    contracts();
    failures();
    puts("geometry backend: pictures, ownership, budgets, cancellation/reentry and failure "
         "isolation pass");
    return 0;
}
