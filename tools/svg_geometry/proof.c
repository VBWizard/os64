/* Private geometry experiment, not an exported SVG or font ABI.
 * Coordinates use SVG's downwards y axis; direct spans write that same row.
 * FreeType receives translated/scaled user-space paths for stroke expansion,
 * then clip-relative 26.6 outlines for rasterization. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID_LIMIT 1048576.0
#define RASTER_LIMIT 16777216.0
#define COMMAND_LIMIT 4096
#define W 192
#define H 128
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef enum { OK, INVALID, LIMIT, NO_MEMORY, BACKEND } Status;
typedef enum { MOVE, LINE, QUAD, CUBIC, CLOSE } Verb;
typedef struct {
    Verb verb;
    double x, y, x2, y2, x3, y3;
} Cmd;
typedef struct {
    double a, b, c, d, tx, ty;
} Map;
typedef struct {
    double ox, oy, scale;
} Frame;
typedef union {
    max_align_t alignment;
    size_t size;
} Block;
typedef struct {
    size_t live, peak, calls, fail_at, cap;
    unsigned expanded_points;
    bool refused;
    struct FT_MemoryRec_ memory;
    FT_Library library;
} Context;
typedef struct {
    FT_Outline ft;
    Frame frame;
} Shape;
static const Map identity = {1, 0, 0, 1, 0, 0};
static size_t checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void *allocate(FT_Memory mem, long size)
{
    Context *c = mem->user;
    if (size <= 0 || (size_t)size > SIZE_MAX - sizeof(Block))
        return NULL;
    size_t total = (size_t)size + sizeof(Block);
    ++c->calls;
    if (c->calls == c->fail_at || total > c->cap - c->live) {
        c->refused = true;
        return NULL;
    }
    Block *b = malloc(total);
    if (!b) {
        c->refused = true;
        return NULL;
    }
    b->size = total;
    c->live += total;
    if (c->live > c->peak)
        c->peak = c->live;
    return b + 1;
}
static void release(FT_Memory mem, void *ptr)
{
    if (!ptr)
        return;
    Context *c = mem->user;
    Block *b = (Block *)ptr - 1;
    CHECK(c->live >= b->size);
    c->live -= b->size;
    free(b);
}
static void *resize(FT_Memory mem, long old_size, long size, void *ptr)
{
    if (size <= 0) {
        release(mem, ptr);
        return NULL;
    }
    void *out = allocate(mem, size);
    if (out) {
        if (ptr)
            memcpy(out, ptr, (size_t)(old_size < size ? old_size : size));
        release(mem, ptr);
    }
    return out;
}
static Status start(Context *c, size_t fail_at, size_t cap)
{
    *c = (Context){.fail_at = fail_at, .cap = cap};
    c->memory = (struct FT_MemoryRec_){c, allocate, release, resize};
    FT_Error e = FT_New_Library(&c->memory, &c->library);
    if (!e)
        FT_Add_Default_Modules(c->library);
    if (e || c->refused) {
        if (c->library)
            FT_Done_Library(c->library);
        c->library = NULL;
        CHECK(!c->live);
        return NO_MEMORY;
    }
    return OK;
}
static void end(Context *c)
{
    if (c->library)
        FT_Done_Library(c->library);
    c->library = NULL;
    CHECK(!c->live);
}
static Status status(Context *c, FT_Error e)
{
    return c->refused ? NO_MEMORY : e ? BACKEND : OK;
}
static bool valid_map(Map m)
{
    return isfinite(m.a) && isfinite(m.b) && isfinite(m.c) && isfinite(m.d) && isfinite(m.tx) &&
           isfinite(m.ty);
}
static unsigned points(Verb v)
{
    return v == CLOSE ? 0 : v == QUAD ? 2 : v == CUBIC ? 3 : 1;
}
/* This conservative experimental envelope is narrower than FT_Pos. It
 * reserves space for joins before passing arithmetic to upstream. The scale
 * bounds point-rounding error, not the stroker's curve approximation error. */
static Status frame_for(const Cmd *path, size_t count, Map m, double width, double miter, Frame *f)
{
    if (!path || !count || !valid_map(m) || !isfinite(width) || width < 0 || !isfinite(miter) ||
        miter < 1)
        return INVALID;
    if (count > COMMAND_LIMIT || miter > 1024)
        return LIMIT;
    double stretch = fmax(fabs(m.a) + fabs(m.c), fabs(m.b) + fabs(m.d));
    double scale = 256;
    while (scale < 256 * stretch && scale < 1073741824.0)
        scale *= 2;
    if (!isfinite(stretch) || scale < 256 * stretch)
        return LIMIT;
    *f = (Frame){path[0].x, path[0].y, scale};
    bool active = false;
    double extent = 0;
    for (size_t i = 0; i < count; ++i) {
        Cmd p = path[i];
        if (p.verb < MOVE || p.verb > CLOSE)
            return INVALID;
        if (p.verb == MOVE)
            active = true;
        else if (!active)
            return INVALID;
        if (p.verb == CLOSE)
            active = false;
        double xy[] = {p.x, p.y, p.x2, p.y2, p.x3, p.y3};
        for (unsigned j = 0; j < points(p.verb) * 2; j += 2) {
            if (!isfinite(xy[j]) || !isfinite(xy[j + 1]))
                return INVALID;
            double x = fabs(xy[j] - f->ox), y = fabs(xy[j + 1] - f->oy);
            if (!isfinite(x) || !isfinite(y))
                return LIMIT;
            extent = fmax(extent, fmax(x, y));
        }
    }
    double radius = width * 0.5 * scale;
    if (!isfinite(radius) || (width && radius < 0.5) ||
        (extent + width * 0.5 * (miter + 2)) * scale > GRID_LIMIT)
        return LIMIT;
    return OK;
}
static FT_Vector vector(Frame f, double x, double y)
{
    return (FT_Vector){(FT_Pos)llround((x - f.ox) * f.scale),
                       (FT_Pos)llround((y - f.oy) * f.scale)};
}
static Status outline_new(Context *c, unsigned n, unsigned contours, FT_Outline *out)
{
    if (n > FT_OUTLINE_POINTS_MAX || contours > FT_OUTLINE_CONTOURS_MAX)
        return LIMIT;
    return status(c, FT_Outline_New(c->library, n, (int)contours, out));
}
static void drop(Context *c, Shape *s)
{
    FT_Outline_Done(c->library, &s->ft);
    *s = (Shape){0};
}
static Status fill(Context *c, const Cmd *path, size_t count, Map m, Shape *out)
{
    *out = (Shape){0};
    Status s = frame_for(path, count, m, 0, 1, &out->frame);
    if (s)
        return s;
    unsigned n = 0, contours = 0;
    for (size_t i = 0; i < count; ++i) {
        n += points(path[i].verb);
        contours += path[i].verb == MOVE;
    }
    s = outline_new(c, n, contours, &out->ft);
    if (s)
        return s;
    n = contours = 0;
    for (size_t i = 0; i < count; ++i) {
        Cmd p = path[i];
        if (p.verb == CLOSE)
            continue;
        if (p.verb == MOVE && n)
            out->ft.contours[contours++] = (unsigned short)(n - 1);
        double xy[] = {p.x, p.y, p.x2, p.y2, p.x3, p.y3};
        for (unsigned j = 0; j < points(p.verb); ++j) {
            out->ft.points[n] = vector(out->frame, xy[2 * j], xy[2 * j + 1]);
            out->ft.tags[n++] = j + 1 == points(p.verb) ? FT_CURVE_TAG_ON
                                : p.verb == QUAD        ? FT_CURVE_TAG_CONIC
                                                        : FT_CURVE_TAG_CUBIC;
        }
    }
    out->ft.contours[contours++] = (unsigned short)(n - 1);
    CHECK(contours == out->ft.n_contours && n == out->ft.n_points);
    CHECK(!FT_Outline_Check(&out->ft));
    return OK;
}
static Status stroke(Context *c, const Cmd *path, size_t count, Map m, double width,
                     FT_Stroker_LineCap cap, FT_Stroker_LineJoin join, double miter, Shape *out)
{
    *out = (Shape){0};
    Status s = frame_for(path, count, m, width, miter, &out->frame);
    if (s)
        return s;
    if (!width)
        return OK;
    FT_Stroker st = NULL;
    FT_Error e = FT_Stroker_New(c->library, &st);
    if (e)
        return status(c, e);
    FT_Stroker_Set(st, (FT_Fixed)llround(width * .5 * out->frame.scale), cap, join,
                   (FT_Fixed)llround(miter * 65536));
    for (size_t i = 0; i < count && !e;) {
        size_t finish = i + 1;
        while (finish < count && path[finish].verb != MOVE)
            ++finish;
        bool closed = path[finish - 1].verb == CLOSE;
        FT_Vector p = vector(out->frame, path[i].x, path[i].y);
        e = FT_Stroker_BeginSubPath(st, &p, !closed);
        for (size_t j = i + 1; j < finish && !e; ++j) {
            Cmd cmd = path[j];
            if (cmd.verb == CLOSE)
                continue;
            p = vector(out->frame, cmd.x, cmd.y);
            FT_Vector q = {0}, r = {0};
            if (cmd.verb == QUAD || cmd.verb == CUBIC)
                q = vector(out->frame, cmd.x2, cmd.y2);
            if (cmd.verb == CUBIC)
                r = vector(out->frame, cmd.x3, cmd.y3);
            if (cmd.verb == LINE)
                e = FT_Stroker_LineTo(st, &p);
            else if (cmd.verb == QUAD)
                e = FT_Stroker_ConicTo(st, &p, &q);
            else
                e = FT_Stroker_CubicTo(st, &p, &q, &r);
        }
        if (!e)
            e = FT_Stroker_EndSubPath(st);
        i = finish;
    }
    unsigned n = 0, contours = 0;
    if (!e)
        e = FT_Stroker_GetCounts(st, &n, &contours);
    c->expanded_points = n;
    s = status(c, e);
    if (!s)
        s = outline_new(c, n, contours, &out->ft);
    if (!s) {
        out->ft.n_points = out->ft.n_contours = 0;
        FT_Stroker_Export(st, &out->ft);
        CHECK(out->ft.n_points == n && out->ft.n_contours == contours);
        CHECK(!FT_Outline_Check(&out->ft));
    }
    FT_Stroker_Done(st);
    return s;
}
typedef struct {
    uint8_t *pixels;
    unsigned width, height;
    size_t stride;
} Target;
static void spans(int y, int n, const FT_Span *s, void *arg)
{
    Target *t = arg;
    CHECK(y >= 0 && (unsigned)y < t->height);
    for (int i = 0; i < n; ++i) {
        CHECK((unsigned)s[i].x + s[i].len <= t->width);
        memset(t->pixels + (size_t)y * t->stride + s[i].x, s[i].coverage, s[i].len);
    }
}
static Status raster(Context *c, const Shape *shape, Map m, int ox, int oy, unsigned width,
                     unsigned height, size_t stride, uint8_t *pixels, size_t capacity, bool evenodd)
{
    if (!valid_map(m) || !pixels || !width || !height || stride < width)
        return INVALID;
    if (width > 65535 || height > 65535 || stride > SIZE_MAX / height || stride * height > capacity)
        return LIMIT;
    FT_Outline o = {0};
    Status s = outline_new(c, shape->ft.n_points, shape->ft.n_contours, &o);
    if (s)
        return s;
    double bx = m.a * shape->frame.ox + m.c * shape->frame.oy + m.tx - ox;
    double by = m.b * shape->frame.ox + m.d * shape->frame.oy + m.ty - oy;
    for (unsigned i = 0; i < o.n_points; ++i) {
        double x = shape->ft.points[i].x / shape->frame.scale;
        double y = shape->ft.points[i].y / shape->frame.scale;
        double dx = (m.a * x + m.c * y + bx) * 64;
        double dy = (m.b * x + m.d * y + by) * 64;
        if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > RASTER_LIMIT || fabs(dy) > RASTER_LIMIT) {
            s = LIMIT;
            goto done;
        }
        o.points[i] = (FT_Vector){(FT_Pos)llround(dx), (FT_Pos)llround(dy)};
        o.tags[i] = shape->ft.tags[i];
    }
    for (unsigned i = 0; i < o.n_contours; ++i)
        o.contours[i] = shape->ft.contours[i];
    if (evenodd)
        o.flags |= FT_OUTLINE_EVEN_ODD_FILL;
    /* Clear only after validation. Backend failure may leave partial coverage;
     * callers must discard the destination on any non-OK result. */
    for (unsigned y = 0; y < height; ++y)
        memset(pixels + y * stride, 0, width);
    Target target = {pixels, width, height, stride};
    FT_Raster_Params params = {.flags =
                                   FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP,
                               .gray_spans = spans,
                               .user = &target,
                               .clip_box = {0, 0, width, height}};
    s = status(c, FT_Outline_Render(c->library, &o, &params));
done:
    FT_Outline_Done(c->library, &o);
    return s;
}
static void draw(Context *c, const Shape *s, Map map, uint8_t *pixels, bool evenodd)
{
    CHECK(raster(c, s, map, 0, 0, W, H, W, pixels, W * H, evenodd) == OK);
}
static void save(const char *dir, const char *name, const uint8_t *pixels)
{
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.pgm", dir, name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fprintf(f, "P5\n%d %d\n255\n", W, H) > 0);
    CHECK(fwrite(pixels, 1, W * H, f) == W * H);
    CHECK(!fclose(f));
}
static double difference(const uint8_t *a, const uint8_t *b, unsigned *max)
{
    double sum = 0;
    *max = 0;
    for (size_t i = 0; i < W * H; ++i) {
        unsigned d = (unsigned)abs((int)a[i] - b[i]);
        if (d > *max)
            *max = d;
        sum += d / 255.0;
    }
    return sum;
}
/* Independent analytic capsule, supersampled at fixed subpixel centers.
 * No FreeType path, stroke or outline conversion participates in this oracle. */
static void capsule(uint8_t *out, Map m, double x1, double x2, double cy, double radius)
{
    double det = m.a * m.d - m.b * m.c;
    CHECK(fabs(det) > 1e-12);
    const unsigned samples = 64;
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            unsigned hits = 0;
            for (unsigned sy = 0; sy < samples; ++sy)
                for (unsigned sx = 0; sx < samples; ++sx) {
                    double dx = x + (sx + .5) / samples - m.tx, dy = y + (sy + .5) / samples - m.ty;
                    double ux = (m.d * dx - m.c * dy) / det, uy = (-m.b * dx + m.a * dy) / det;
                    double near = fmax(x1, fmin(x2, ux));
                    hits += (ux - near) * (ux - near) + (uy - cy) * (uy - cy) <= radius * radius;
                }
            out[y * W + x] = (uint8_t)((hits * 255 + samples * samples / 2) / (samples * samples));
        }
}
static void cap_proof(Context *c, const char *dir)
{
    const Cmd line[] = {{MOVE, 12, 48}, {LINE, 36, 48}};
    const Map map = {4, 0, 0, 1, 0, 0};
    uint8_t actual[W * H], reference[W * H], wrong[W * H];
    Shape s;
    CHECK(stroke(c, line, COUNT(line), map, 16, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND,
                 4, &s) == OK);
    draw(c, &s, map, actual, false);
    capsule(reference, map, 12, 36, 48, 8);
    capsule(wrong, identity, 48, 144, 48, 8);
    unsigned max;
    double error = difference(actual, reference, &max);
    printf("scale(4,1) capsule: coverage L1 %.6f pixels, maximum %u/255\n", error, max);
    CHECK(error <= 8.0 && max <= 24);
    CHECK(difference(actual, wrong, &max) > 300);
    uint8_t missing[W * H] = {0};
    CHECK(difference(actual, missing, &max) > 1000);
    save(dir, "scaled-cap", actual);
    save(dir, "scaled-cap-reference", reference);
    save(dir, "wrong-device-stroke", wrong);
    /* A device-origin request must equal a crop, preserving the whole path. */
    uint8_t clip[24 * 40];
    memset(clip, 0xa5, sizeof(clip));
    CHECK(raster(c, &s, map, 20, 35, 35, 24, 40, clip, sizeof(clip), false) == OK);
    for (unsigned y = 0; y < 24; ++y) {
        CHECK(!memcmp(clip + y * 40, actual + (y + 35) * W + 20, 35));
        for (unsigned x = 35; x < 40; ++x)
            CHECK(clip[y * 40 + x] == 0xa5);
    }
    CHECK(raster(c, &s, map, 0, 0, W, H, W, actual, 10, false) == LIMIT);
    drop(c, &s);
    /* Reflection and shear exercise more than a diagonal scale. */
    Map shear = {-2, .2, .5, 1, 130, 0};
    CHECK(stroke(c, line, COUNT(line), shear, 16, FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 4, &s) == OK);
    draw(c, &s, shear, actual, false);
    capsule(reference, shear, 12, 36, 48, 8);
    error = difference(actual, reference, &max);
    printf("reflected/sheared capsule: coverage L1 %.6f pixels, maximum %u/255\n", error, max);
    CHECK(error <= 8.0 && max <= 24);
    save(dir, "sheared-cap", actual);
    drop(c, &s);
    const double zoom = 16384;
    Cmd tiny[] = {{MOVE, 12 / zoom, 48 / zoom}, {LINE, 36 / zoom, 48 / zoom}};
    Map enlarged = {4 * zoom, 0, 0, zoom, 0, 0};
    CHECK(stroke(c, tiny, COUNT(tiny), enlarged, 16 / zoom, FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 4, &s) == OK);
    draw(c, &s, enlarged, actual, false);
    capsule(reference, map, 12, 36, 48, 8);
    error = difference(actual, reference, &max);
    printf("16384x zoom capsule: coverage L1 %.6f pixels, maximum %u/255\n", error, max);
    CHECK(error <= 8.0 && max <= 24);
    drop(c, &s);
}
static void fill_proof(Context *c, const char *dir)
{
    Cmd rings[] = {{MOVE, 20, 20}, {LINE, 100, 20}, {LINE, 100, 100}, {LINE, 20, 100}, {CLOSE},
                   {MOVE, 40, 40}, {LINE, 80, 40},  {LINE, 80, 80},   {LINE, 40, 80}};
    uint8_t a[W * H], b[W * H];
    Shape s;
    CHECK(fill(c, rings, COUNT(rings), identity, &s) == OK);
    draw(c, &s, identity, a, false);
    draw(c, &s, identity, b, true);
    CHECK(a[60 * W + 60] == 255 && b[60 * W + 60] == 0);
    CHECK(a[30 * W + 30] == 255 && b[30 * W + 30] == 255);
    save(dir, "nonzero", a);
    save(dir, "evenodd", b);
    drop(c, &s);
    /* Reverse the inner contour: nonzero now has the same hole as evenodd. */
    Cmd tmp = rings[6];
    rings[6] = rings[8];
    rings[8] = tmp;
    CHECK(fill(c, rings, COUNT(rings), identity, &s) == OK);
    draw(c, &s, identity, a, false);
    CHECK(!memcmp(a, b, sizeof(a)));
    drop(c, &s);
    Cmd translated[COUNT(rings)];
    memcpy(translated, rings, sizeof(rings));
    for (size_t i = 0; i < COUNT(rings); ++i)
        if (rings[i].verb != CLOSE) {
            translated[i].x += 1000000000;
            translated[i].y -= 1000000000;
        }
    Map shifted = {1, 0, 0, 1, -1000000000, 1000000000};
    CHECK(fill(c, translated, COUNT(translated), shifted, &s) == OK);
    draw(c, &s, shifted, b, false);
    CHECK(!memcmp(a, b, sizeof(a)));
    drop(c, &s);
    const Cmd curves[] = {
        {MOVE, 20, 80}, {QUAD, 20, 10, 80, 20}, {CUBIC, 160, 0, 180, 120, 100, 100}, {CLOSE}};
    CHECK(fill(c, curves, COUNT(curves), identity, &s) == OK);
    draw(c, &s, identity, a, false);
    CHECK(a[60 * W + 80] == 255);
    save(dir, "curves", a);
    drop(c, &s);
}
static void closure_proof(Context *c, const char *dir)
{
    const Cmd mixed[] = {{MOVE, 20, 20},  {LINE, 70, 20},  {LINE, 70, 70}, {MOVE, 105, 20},
                         {LINE, 155, 20}, {LINE, 155, 70}, {CLOSE}};
    Shape s;
    uint8_t pixels[W * H];
    CHECK(stroke(c, mixed, COUNT(mixed), identity, 8, FT_STROKER_LINECAP_BUTT,
                 FT_STROKER_LINEJOIN_BEVEL, 4, &s) == OK);
    CHECK(s.ft.n_contours == 3); /* One open border, two closed borders. */
    draw(c, &s, identity, pixels, false);
    CHECK(!pixels[45 * W + 45] && pixels[45 * W + 130] == 255);
    CHECK(pixels[20 * W + 40] == 255 && pixels[20 * W + 125] == 255);
    save(dir, "mixed-closure", pixels);
    drop(c, &s);
    const Cmd degenerate[] = {{MOVE, 30, 30}, {LINE, 30, 30}};
    CHECK(stroke(c, degenerate, COUNT(degenerate), identity, 8, FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 4, &s) == OK);
    printf(
        "zero-length round stroke: %u points, %u contours (SVG needs explicit handling if empty)\n",
        s.ft.n_points, s.ft.n_contours);
    CHECK(!s.ft.n_points && !s.ft.n_contours);
    drop(c, &s);
    const Cmd move_only[] = {{MOVE, 30, 30}};
    CHECK(stroke(c, move_only, COUNT(move_only), identity, 8, FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 4, &s) == OK);
    CHECK(!s.ft.n_points);
    drop(c, &s);
}
static void miter_proof(Context *c, const char *dir)
{
    /* Right-angle join: threshold sqrt(2), spike at (84,36), bevel x+y=124. */
    const Cmd corner[] = {{MOVE, 30, 40}, {LINE, 80, 40}, {LINE, 80, 90}};
    const double threshold = sqrt(2);
    double limits[] = {threshold - 2.0 / 65536,
                       92681.0 / 65536,
                       threshold - .25 / 65536,
                       threshold,
                       threshold + .25 / 65536,
                       threshold + 2.0 / 65536,
                       2};
    unsigned count[COUNT(limits)];
    uint8_t pixels[W * H];
    for (size_t i = 0; i < COUNT(limits); ++i) {
        Shape s;
        CHECK(stroke(c, corner, COUNT(corner), identity, 8, FT_STROKER_LINECAP_BUTT,
                     FT_STROKER_LINEJOIN_MITER_FIXED, limits[i], &s) == OK);
        count[i] = s.ft.n_points;
        draw(c, &s, identity, pixels, false);
        printf("miter %.12f => 16.16 %ld, points %u, tip coverage %u\n", limits[i],
               (long)llround(limits[i] * 65536), count[i], pixels[36 * W + 83]);
        if (i == 0)
            save(dir, "miter-below", pixels);
        if (i + 1 == COUNT(limits)) {
            CHECK(pixels[36 * W + 83] == 255);
            save(dir, "miter-above", pixels);
        }
        drop(c, &s);
    }
    CHECK(count[0] != count[COUNT(limits) - 1]);
    CHECK(count[2] == count[3] && count[3] == count[4]);
    Shape s;
    CHECK(stroke(c, corner, COUNT(corner), identity, 8, FT_STROKER_LINECAP_BUTT,
                 FT_STROKER_LINEJOIN_MITER_FIXED, .9, &s) == INVALID);
    CHECK(stroke(c, corner, COUNT(corner), identity, 8, FT_STROKER_LINECAP_BUTT,
                 FT_STROKER_LINEJOIN_MITER_VARIABLE, 1.1, &s) == OK);
    draw(c, &s, identity, pixels, false);
    save(dir, "miter-clip", pixels);
    drop(c, &s);
}
static Status allocation_case(Context *c)
{
    const Cmd path[] = {{MOVE, 20, 60}, {QUAD, 50, 5, 80, 40}, {CUBIC, 110, 110, 140, 5, 170, 70}};
    Shape s;
    Status r = stroke(c, path, COUNT(path), identity, 12, FT_STROKER_LINECAP_ROUND,
                      FT_STROKER_LINEJOIN_ROUND, 4, &s);
    if (r)
        return r;
    uint8_t pixels[W * H];
    r = raster(c, &s, identity, 0, 0, W, H, W, pixels, sizeof(pixels), false);
    drop(c, &s);
    return r;
}
static void failure_proof(void)
{
    Context c;
    CHECK(start(&c, 0, SIZE_MAX) == OK);
    CHECK(allocation_case(&c) == OK);
    size_t calls = c.calls, peak = c.peak;
    end(&c);
    for (size_t i = 1; i <= calls; ++i) {
        Status r = start(&c, i, SIZE_MAX);
        if (!r)
            r = allocation_case(&c);
        CHECK(r == NO_MEMORY);
        end(&c);
    }
    CHECK(start(&c, 0, peak / 2) == NO_MEMORY || c.library);
    if (c.library) {
        CHECK(allocation_case(&c) == NO_MEMORY);
        end(&c);
    }
    printf("allocator: %zu failure cuts, peak %zu bytes including headers, no live blocks\n", calls,
           peak);
    Context a, b;
    CHECK(start(&a, 0, SIZE_MAX) == OK && start(&b, 0, SIZE_MAX) == OK);
    a.fail_at = a.calls + 1;
    CHECK(allocation_case(&a) == NO_MEMORY);
    CHECK(allocation_case(&b) == OK);
    end(&a);
    end(&b);
}
static void limit_proof(Context *c)
{
    Shape s;
    Cmd path[] = {{MOVE, 0, 0}, {LINE, 1e30, 1}};
    CHECK(fill(c, path, COUNT(path), identity, &s) == LIMIT);
    path[1].x = NAN;
    CHECK(fill(c, path, COUNT(path), identity, &s) == INVALID);
    path[1].x = GRID_LIMIT / 256;
    CHECK(fill(c, path, COUNT(path), identity, &s) == OK);
    drop(c, &s);
    path[1].x += 1.0 / 256;
    CHECK(fill(c, path, COUNT(path), identity, &s) == LIMIT);
    path[1].x = 1;
    CHECK(fill(c, path, COMMAND_LIMIT + 1, identity, &s) == LIMIT);
    CHECK(stroke(c, path, COUNT(path), identity, 1e-20, FT_STROKER_LINECAP_ROUND,
                 FT_STROKER_LINEJOIN_ROUND, 4, &s) == LIMIT);
    CHECK(outline_new(c, FT_OUTLINE_POINTS_MAX + 1u, 1, &s.ft) == LIMIT);
    Cmd expanded[COMMAND_LIMIT];
    for (size_t i = 0; i < COMMAND_LIMIT; i += 2) {
        expanded[i] = (Cmd){MOVE, 0, 0};
        expanded[i + 1] = (Cmd){CUBIC, 100, 100, -100, 100, 0, 0};
    }
    Status expanded_status = stroke(c, expanded, COMMAND_LIMIT, identity, 4,
                                    FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 4, &s);
    printf("expanded output count refusal: %u points, status %d (LIMIT=%d)\n", c->expanded_points,
           expanded_status, LIMIT);
    CHECK(c->expanded_points > FT_OUTLINE_POINTS_MAX);
    CHECK(expanded_status == LIMIT);
    CHECK(fill(c, path, COUNT(path), identity, &s) == OK);
    uint8_t pixels[W * H];
    memset(pixels, 123, sizeof(pixels));
    Map far = identity;
    far.tx = 1e20;
    CHECK(raster(c, &s, far, 0, 0, W, H, W, pixels, sizeof(pixels), false) == LIMIT);
    for (size_t i = 0; i < sizeof(pixels); ++i)
        CHECK(pixels[i] == 123);
    Map singular = {0, 0, 0, 1, 0, 0};
    draw(c, &s, singular, pixels, false);
    for (size_t i = 0; i < sizeof(pixels); ++i)
        CHECK(!pixels[i]);
    drop(c, &s);
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(argc == 2);
    Context c;
    CHECK(start(&c, 0, SIZE_MAX) == OK);
    cap_proof(&c, argv[1]);
    fill_proof(&c, argv[1]);
    closure_proof(&c, argv[1]);
    miter_proof(&c, argv[1]);
    limit_proof(&c);
    end(&c);
    failure_proof();
    printf("geometry proof: %zu checks passed\n", checks);
    return 0;
}
