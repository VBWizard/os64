/* Native geometry backend. Allocation belongs to the caller's ledger; this
 * context keeps reporting counters and bounded operation state. */
#include <ft2build.h>
#include <os64/geometry_backend.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include <float.h>

#define OK OS64_GEOMETRY_OK
#define BAD OS64_GEOMETRY_BAD_ARGUMENT
#define LIMIT OS64_GEOMETRY_LIMIT
#define NOMEM OS64_GEOMETRY_NO_MEMORY
#define ENGINE OS64_GEOMETRY_ENGINE_ERROR
#define BUSY OS64_GEOMETRY_BUSY
#define CANCEL OS64_GEOMETRY_CANCELLED
#define UNSUPPORTED OS64_GEOMETRY_UNSUPPORTED
#define GRID_LIMIT 1048576.0
#define RASTER_LIMIT 16777216.0
#define SCALE_LIMIT 1073741824.0

typedef os64_geometry_status_t Status;
typedef os64_geometry_context_t Context;
typedef os64_geometry_outline_t Outline;
typedef os64_geometry_command_t Command;
typedef os64_geometry_map_t Map;
typedef union {
    max_align_t alignment;
    size_t bytes;
} Block;
typedef struct {
    double x, y, scale, magnitude, tolerance;
} Frame;
struct os64_geometry_context {
    os64_geometry_options_t options;
    os64_geometry_stats_t stats;
    Status why;
    bool busy;
    struct FT_MemoryRec_ memory;
    FT_Library library;
};
struct os64_geometry_outline {
    Context *owner;
    FT_Outline ft;
    Frame frame;
};
static double absolute(double x)
{
    return x < 0 ? -x : x;
}
static double maximum(double a, double b)
{
    return a > b ? a : b;
}
static bool finite(double x)
{
    return x <= DBL_MAX && x >= -DBL_MAX;
}
static bool map_valid(const Map *m)
{
    return m && finite(m->a) && finite(m->b) && finite(m->c) && finite(m->d) && finite(m->tx) &&
           finite(m->ty);
}
static double stretch(const Map *m)
{
    return maximum(absolute(m->a) + absolute(m->c), absolute(m->b) + absolute(m->d));
}
static bool spend(Context *c, uint64_t work)
{
    if (c->why)
        return false;
    if (c->options.cancelled && c->options.cancelled(c->options.cancel_context)) {
        c->why = CANCEL;
        return false;
    }
    if (work > c->options.max_work - c->stats.last_work) {
        c->why = LIMIT;
        return false;
    }
    c->stats.last_work += work;
    return true;
}
static Status enter(Context *c)
{
    if (!c)
        return BAD;
    if (c->busy)
        return BUSY;
    c->busy = true;
    c->why = OK;
    c->stats.last_work = 0;
    return OK;
}
static Status leave(Context *c, Status s)
{
    if (c->why)
        s = c->why;
    c->busy = false;
    return s;
}
static void *allocate(Context *c, size_t bytes)
{
    if (bytes > SIZE_MAX - sizeof(Block) ||
        bytes + sizeof(Block) > SIZE_MAX - c->stats.live_bytes) {
        if (!c->why)
            c->why = LIMIT;
        return NULL;
    }
    size_t total = bytes + sizeof(Block);
    if (!spend(c, total))
        return NULL;
    void *memory = NULL;
    Status s = c->options.memory.alloc(c->options.memory.context, total, &memory);
    if (s || !memory) {
        /* Allocation callbacks promise NULL on refusal. Reclaim a returned
         * block if a callback violates that contract. */
        if (memory)
            c->options.memory.free(c->options.memory.context, memory, total);
        c->why = s == LIMIT ? LIMIT : s == NOMEM || !s ? NOMEM : ENGINE;
        return NULL;
    }
    Block *b = memory;
    b->bytes = total;
    c->stats.live_bytes += total;
    if (c->stats.live_bytes > c->stats.peak_bytes)
        c->stats.peak_bytes = c->stats.live_bytes;
    unsigned char *p = (unsigned char *)(b + 1);
    for (size_t i = 0; i < bytes; ++i)
        p[i] = 0;
    return p;
}
static void free_block(Context *c, void *p)
{
    if (!p)
        return;
    Block *b = (Block *)p - 1;
    size_t bytes = b->bytes;
    c->stats.live_bytes -= bytes;
    c->options.memory.free(c->options.memory.context, b, bytes);
}
static void *ft_alloc(FT_Memory m, long n)
{
    return n > 0 ? allocate(m->user, (size_t)n) : NULL;
}
static void ft_free(FT_Memory m, void *p)
{
    free_block(m->user, p);
}
static void *ft_realloc(FT_Memory m, long old, long size, void *p)
{
    Context *c = m->user;
    if (size <= 0) {
        free_block(c, p);
        return NULL;
    }
    size_t copied = old < size ? (size_t)old : (size_t)size;
    if (p && !spend(c, copied))
        return NULL;
    unsigned char *out = allocate(c, (size_t)size);
    if (out) {
        if (p)
            for (size_t i = 0; i < copied; ++i)
                out[i] = ((unsigned char *)p)[i];
        free_block(c, p);
    }
    return out;
}
static Status backend(Context *c, FT_Error e)
{
    return c->why ? c->why : e ? ENGINE : OK;
}
static Status create(const os64_geometry_options_t *options, Context **out)
{
    if (out)
        *out = NULL;
    if (!out || !options || !options->memory.alloc || !options->memory.free)
        return BAD;
    Context boot = {0};
    boot.options = *options;
    boot.busy = true;
    Context *c = allocate(&boot, sizeof(*c));
    if (!c)
        return boot.why;
    c->options = *options;
    c->stats = boot.stats;
    c->busy = true;
    c->memory = (struct FT_MemoryRec_){c, ft_alloc, ft_free, ft_realloc};
    FT_Error e = FT_New_Library(&c->memory, &c->library);
    if (!e)
        FT_Add_Default_Modules(c->library);
    Status s = backend(c, e);
    if (s) {
        if (c->library)
            FT_Done_Library(c->library);
        boot.stats = c->stats;
        free_block(&boot, c);
        return s;
    }
    c->busy = false;
    *out = c;
    return OK;
}
static Status destroy(Context *c)
{
    if (!c)
        return BAD;
    if (c->busy || c->stats.live_outlines)
        return BUSY;
    c->busy = true;
    FT_Done_Library(c->library);
    Context boot = {0};
    boot.options = c->options;
    boot.stats = c->stats;
    free_block(&boot, c);
    return OK;
}
static Status stats(Context *c, os64_geometry_stats_t *out)
{
    if (out)
        *out = (os64_geometry_stats_t){0};
    if (!c || !out)
        return BAD;
    if (c->busy)
        return BUSY;
    *out = c->stats;
    return OK;
}
static unsigned point_count(os64_geometry_verb_t v)
{
    return v == OS64_GEOMETRY_CLOSE       ? 0
           : v == OS64_GEOMETRY_QUADRATIC ? 2
           : v == OS64_GEOMETRY_CUBIC     ? 3
                                          : 1;
}
static Status prepare(Context *c, const Command *path, size_t n, const Map *map, double tolerance,
                      double width, double miter, Frame *frame)
{
    if (!path || !n || !map_valid(map) || !finite(tolerance) || tolerance <= 1.0 / 128 ||
        tolerance > 1)
        return BAD;
    if (n > OS64_GEOMETRY_COMMAND_MAX)
        return LIMIT;
    if (path[0].verb != OS64_GEOMETRY_MOVE)
        return BAD;
    double norm = stretch(map);
    double desired = maximum(256 * norm, .5 * norm / (tolerance - 1.0 / 128));
    double scale = 256;
    while (scale < desired && scale < SCALE_LIMIT)
        scale *= 2;
    if (!finite(desired) || scale < desired)
        return LIMIT;
    *frame = (Frame){path[0].p[0].x, path[0].p[0].y, scale, 0, tolerance};
    bool active = false;
    double extent = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!spend(c, 1))
            return c->why;
        if (path[i].verb < OS64_GEOMETRY_MOVE || path[i].verb > OS64_GEOMETRY_CLOSE)
            return BAD;
        if (path[i].verb == OS64_GEOMETRY_MOVE)
            active = true;
        else if (!active)
            return BAD;
        if (path[i].verb == OS64_GEOMETRY_CLOSE)
            active = false;
        for (unsigned j = 0; j < point_count(path[i].verb); ++j) {
            double x = path[i].p[j].x, y = path[i].p[j].y;
            if (!finite(x) || !finite(y))
                return BAD;
            frame->magnitude = maximum(frame->magnitude, maximum(absolute(x), absolute(y)));
            double relative = maximum(absolute(x - frame->x), absolute(y - frame->y));
            if (!finite(relative))
                return LIMIT;
            extent = maximum(extent, relative);
        }
    }
    double radius = width * .5 * scale;
    double expansion = width * .5 * (miter + 2);
    if ((width && radius < .5) || !finite(radius) || (extent + expansion) * scale > GRID_LIMIT)
        return LIMIT;
    /* Include possible stroke excursions in the later floating-point error
     * allowance, not just the input control coordinates. */
    frame->magnitude += expansion;
    return OK;
}
static FT_Pos rounded(double x)
{
    return (FT_Pos)(x < 0 ? x - .5 : x + .5);
}
static FT_Vector vector(Frame f, os64_geometry_point_t p)
{
    return (FT_Vector){rounded((p.x - f.x) * f.scale), rounded((p.y - f.y) * f.scale)};
}
static Status new_ft_outline(Context *c, unsigned points, unsigned contours, FT_Outline *out)
{
    if (points > FT_OUTLINE_POINTS_MAX || contours > FT_OUTLINE_CONTOURS_MAX)
        return LIMIT;
    if (!spend(c, points + contours))
        return c->why;
    return backend(c, FT_Outline_New(c->library, points, (int)contours, out));
}
static void discard(Context *c, Outline *o)
{
    if (!o)
        return;
    FT_Outline_Done(c->library, &o->ft);
    free_block(c, o);
}
static Status path(Context *c, const Command *commands, size_t n, const Map *map, double tolerance,
                   Outline **out)
{
    if (out)
        *out = NULL;
    if (!out)
        return BAD;
    Status s = enter(c);
    if (s)
        return s;
    if (c->stats.live_outlines == UINT32_MAX)
        return leave(c, LIMIT);
    Frame f;
    s = prepare(c, commands, n, map, tolerance, 0, 1, &f);
    if (s)
        return leave(c, s);
    unsigned points = 0, contours = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!spend(c, 1))
            return leave(c, c->why);
        points += point_count(commands[i].verb);
        contours += commands[i].verb == OS64_GEOMETRY_MOVE;
    }
    Outline *o = allocate(c, sizeof(*o));
    if (!o)
        return leave(c, c->why);
    o->frame = f;
    o->owner = c;
    s = new_ft_outline(c, points, contours, &o->ft);
    if (s)
        goto fail;
    points = contours = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!spend(c, 1)) {
            s = c->why;
            goto fail;
        }
        const Command *p = commands + i;
        if (p->verb == OS64_GEOMETRY_CLOSE)
            continue;
        if (p->verb == OS64_GEOMETRY_MOVE && points)
            o->ft.contours[contours++] = (unsigned short)(points - 1);
        unsigned count = point_count(p->verb);
        for (unsigned j = 0; j < count; ++j) {
            o->ft.points[points] = vector(f, p->p[j]);
            o->ft.tags[points++] = j + 1 == count                       ? FT_CURVE_TAG_ON
                                   : p->verb == OS64_GEOMETRY_QUADRATIC ? FT_CURVE_TAG_CONIC
                                                                        : FT_CURVE_TAG_CUBIC;
        }
    }
    o->ft.contours[contours++] = (unsigned short)(points - 1);
    ++c->stats.live_outlines;
    *out = o;
    return leave(c, OK);
fail:
    discard(c, o);
    return leave(c, s);
}
static Status stroke_with_caps(Context *c, const Command *commands, size_t n,
                               const Command *cap_commands, size_t cap_count, const Map *map,
                               double tolerance, const os64_geometry_stroke_t *style, Outline **out)
{
    if (out)
        *out = NULL;
    if (!out || !style || !finite(style->width) || style->width < 0 ||
        !finite(style->miter_limit) || style->miter_limit < 1 || style->cap < OS64_GEOMETRY_BUTT ||
        style->cap > OS64_GEOMETRY_SQUARE || style->join < OS64_GEOMETRY_MITER ||
        style->join > OS64_GEOMETRY_ARCS)
        return BAD;
    if (style->join == OS64_GEOMETRY_ARCS)
        return UNSUPPORTED;
    if (style->miter_limit > 1024)
        return LIMIT;
    Status s = enter(c);
    if (s)
        return s;
    if (c->stats.live_outlines == UINT32_MAX)
        return leave(c, LIMIT);
    Frame f;
    if ((n && !commands) || (cap_count && !cap_commands) || (!n && !cap_count))
        return leave(c, BAD);
    if ((n && commands[0].verb != OS64_GEOMETRY_MOVE) ||
        (cap_count && cap_commands[0].verb != OS64_GEOMETRY_MOVE))
        return leave(c, BAD);
    if (n > OS64_GEOMETRY_COMMAND_MAX || cap_count > OS64_GEOMETRY_COMMAND_MAX - n)
        return leave(c, LIMIT);
    Command *combined = NULL;
    const Command *planned = commands;
    if (cap_count) {
        combined = allocate(c, (n + cap_count) * sizeof(*combined));
        if (!combined)
            return leave(c, c->why);
        for (size_t i = 0; i < n + cap_count; ++i)
            combined[i] = i < n ? commands[i] : cap_commands[i - n];
        planned = combined;
    }
    s = prepare(c, planned, n + cap_count, map, tolerance, style->width, style->miter_limit, &f);
    free_block(c, combined);
    if (s)
        return leave(c, s);
    Outline *o = allocate(c, sizeof(*o));
    if (!o)
        return leave(c, c->why);
    o->owner = c;
    o->frame = f;
    FT_Stroker st = NULL;
    FT_Error e = 0;
    if (!style->width)
        goto success;
    e = FT_Stroker_New(c->library, &st);
    if (e) {
        s = backend(c, e);
        goto fail;
    }
    static const FT_Stroker_LineCap caps[] = {FT_STROKER_LINECAP_BUTT, FT_STROKER_LINECAP_ROUND,
                                              FT_STROKER_LINECAP_SQUARE};
    static const FT_Stroker_LineJoin joins[] = {
        FT_STROKER_LINEJOIN_MITER_FIXED, FT_STROKER_LINEJOIN_MITER_VARIABLE,
        FT_STROKER_LINEJOIN_ROUND, FT_STROKER_LINEJOIN_BEVEL};
    FT_Stroker_Set(st, rounded(style->width * .5 * f.scale), caps[style->cap], joins[style->join],
                   rounded(style->miter_limit * 65536));
    for (size_t i = 0; i < n;) {
        size_t finish = i + 1;
        while (finish < n && commands[finish].verb != OS64_GEOMETRY_MOVE) {
            if (!spend(c, 1)) {
                s = c->why;
                goto fail;
            }
            ++finish;
        }
        FT_Vector start = vector(f, commands[i].p[0]);
        FT_Vector current = start;
        bool drawable = false, segments = false;
        for (size_t j = i + 1; j < finish; ++j) {
            if (!spend(c, 1)) {
                s = c->why;
                goto fail;
            }
            if (commands[j].verb != OS64_GEOMETRY_CLOSE)
                segments = true;
            /* The pinned stroker ignores zero-length lines, and curves whose
             * successive control-polygon edges are below two working units
             * on both axes (FT_IS_SMALL). Even a discarded curve advances its
             * center. Test those edges, not distance from the initial MOVE,
             * so an empty non-butt stroke requests cap fallback. */
            FT_Pos minimum = commands[j].verb == OS64_GEOMETRY_LINE ? 1 : 2;
            for (unsigned k = 0; k < point_count(commands[j].verb); ++k) {
                FT_Vector p = vector(f, commands[j].p[k]);
                FT_Pos dx = p.x - current.x, dy = p.y - current.y;
                drawable |= dx <= -minimum || dx >= minimum || dy <= -minimum || dy >= minimum;
                current = p;
            }
        }
        if (!drawable && segments && style->cap != OS64_GEOMETRY_BUTT) {
            s = UNSUPPORTED;
            goto fail;
        }
        bool closed = commands[finish - 1].verb == OS64_GEOMETRY_CLOSE;
        e = FT_Stroker_BeginSubPath(st, &start, !closed);
        for (size_t j = i + 1; j < finish && !e; ++j) {
            if (!spend(c, 1)) {
                s = c->why;
                goto fail;
            }
            const Command *cmd = commands + j;
            if (cmd->verb == OS64_GEOMETRY_CLOSE)
                continue;
            FT_Vector p = vector(f, cmd->p[0]);
            if (cmd->verb == OS64_GEOMETRY_LINE)
                e = FT_Stroker_LineTo(st, &p);
            else {
                FT_Vector q = vector(f, cmd->p[1]);
                if (cmd->verb == OS64_GEOMETRY_QUADRATIC)
                    e = FT_Stroker_ConicTo(st, &p, &q);
                else {
                    FT_Vector r = vector(f, cmd->p[2]);
                    e = FT_Stroker_CubicTo(st, &p, &q, &r);
                }
            }
        }
        if (!e)
            e = FT_Stroker_EndSubPath(st);
        if (e || c->why) {
            s = backend(c, e);
            goto fail;
        }
        i = finish;
    }
    unsigned points = 0, contours = 0;
    e = FT_Stroker_GetCounts(st, &points, &contours);
    if (e) {
        s = backend(c, e);
        goto fail;
    }
    unsigned cap_points = 0, cap_contours = 0;
    for (size_t i = 0; i < cap_count; ++i) {
        if (!spend(c, 1)) {
            s = c->why;
            goto fail;
        }
        cap_points += point_count(cap_commands[i].verb);
        cap_contours += cap_commands[i].verb == OS64_GEOMETRY_MOVE;
    }
    if (points > OS64_GEOMETRY_POINT_MAX || cap_points > OS64_GEOMETRY_POINT_MAX - points) {
        s = LIMIT;
        goto fail;
    }
    s = new_ft_outline(c, points + cap_points, contours + cap_contours, &o->ft);
    if (s)
        goto fail;
    o->ft.n_points = o->ft.n_contours = 0;
    FT_Stroker_Export(st, &o->ft);
    if (o->ft.n_points != points || o->ft.n_contours != contours) {
        s = ENGINE;
        goto fail;
    }
    // Match the stroke's outer winding so cap overlaps add rather than cancel.
    bool reverse_caps = FT_Outline_Get_Orientation(&o->ft) != FT_ORIENTATION_POSTSCRIPT;
    unsigned written = points, closed = contours;
    for (size_t i = 0; i < cap_count; ++i) {
        if (!spend(c, 1)) {
            s = c->why;
            goto fail;
        }
        const Command *p = cap_commands + i;
        if (p->verb == OS64_GEOMETRY_CLOSE)
            continue;
        if (p->verb == OS64_GEOMETRY_MOVE && written > points)
            o->ft.contours[closed++] = (unsigned short)(written - 1);
        unsigned count = point_count(p->verb);
        for (unsigned j = 0; j < count; ++j) {
            o->ft.points[written] = vector(f, p->p[j]);
            o->ft.tags[written++] = j + 1 == count                       ? FT_CURVE_TAG_ON
                                    : p->verb == OS64_GEOMETRY_QUADRATIC ? FT_CURVE_TAG_CONIC
                                                                         : FT_CURVE_TAG_CUBIC;
        }
    }
    if (cap_points)
        o->ft.contours[closed++] = (unsigned short)(written - 1);
    if (reverse_caps) {
        unsigned first = points;
        for (unsigned i = contours; i < closed; ++i) {
            unsigned last = o->ft.contours[i];
            for (unsigned a = first, b = last; a < b; ++a, --b) {
                if (!spend(c, 1)) {
                    s = c->why;
                    goto fail;
                }
                FT_Vector point = o->ft.points[a];
                char tag = o->ft.tags[a];
                o->ft.points[a] = o->ft.points[b];
                o->ft.tags[a] = o->ft.tags[b];
                o->ft.points[b] = point;
                o->ft.tags[b] = tag;
            }
            first = last + 1;
        }
    }
    o->ft.n_points = (unsigned short)written;
    o->ft.n_contours = (unsigned short)closed;
success:
    FT_Stroker_Done(st);
    ++c->stats.live_outlines;
    *out = o;
    return leave(c, OK);
fail:
    FT_Stroker_Done(st);
    discard(c, o);
    return leave(c, s);
}
static Status stroke(Context *c, const Command *commands, size_t n, const Map *map,
                     double tolerance, const os64_geometry_stroke_t *style, Outline **out)
{
    return stroke_with_caps(c, commands, n, NULL, 0, map, tolerance, style, out);
}
static Status info(Context *c, const Outline *o, os64_geometry_outline_info_t *out)
{
    if (out)
        *out = (os64_geometry_outline_info_t){0};
    if (!c || !o || !out || o->owner != c)
        return BAD;
    if (c->busy)
        return BUSY;
    *out = (os64_geometry_outline_info_t){o->ft.n_points, o->ft.n_contours};
    return OK;
}
static Status release(Context *c, Outline *o)
{
    if (!c || !o || o->owner != c)
        return BAD;
    if (c->busy)
        return BUSY;
    c->busy = true;
    --c->stats.live_outlines;
    discard(c, o);
    c->busy = false;
    return OK;
}
typedef struct {
    Context *c;
    const os64_geometry_target_t *target;
} Spans;
static void spans(int y, int n, const FT_Span *runs, void *arg)
{
    Spans *s = arg;
    Context *c = s->c;
    const os64_geometry_target_t *t = s->target;
    if (!spend(c, 1))
        return;
    if (y < 0 || (uint32_t)y >= t->height) {
        c->why = ENGINE;
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (!spend(c, (uint64_t)runs[i].len + 1))
            return;
        if ((uint32_t)runs[i].x + runs[i].len > t->width) {
            c->why = ENGINE;
            return;
        }
        uint8_t *p = t->pixels + (size_t)y * t->stride + runs[i].x;
        for (unsigned j = 0; j < runs[i].len; ++j)
            p[j] = runs[i].coverage;
    }
}
static Status placement(const Outline *o, const Map *map, double origin_x, double origin_y,
                        double *bx, double *by)
{
    double norm = stretch(map);
    *bx = map->a * o->frame.x + map->c * o->frame.y + map->tx - origin_x;
    *by = map->b * o->frame.x + map->d * o->frame.y + map->ty - origin_y;
    /* Conservative allowance for double subtraction, multiplication and sums,
     * including cancellation of a large world origin. Reject rather than
     * promising subpixel conversion after that precision has been spent. */
    double magnitude = norm * o->frame.magnitude + absolute(map->tx) + absolute(map->ty) +
                       absolute(origin_x) + absolute(origin_y);
    double error = .5 * norm / o->frame.scale + 1.0 / 128 + 16 * DBL_EPSILON * magnitude;
    if (!finite(error) || error > o->frame.tolerance || !finite(*bx) || !finite(*by))
        return LIMIT;
    return OK;
}
static Status device_point(const Outline *o, const Map *map, double bx, double by, unsigned i,
                           FT_Vector *out)
{
    double x = o->ft.points[i].x / o->frame.scale, y = o->ft.points[i].y / o->frame.scale;
    double dx = (map->a * x + map->c * y + bx) * 64;
    double dy = (map->b * x + map->d * y + by) * 64;
    if (!finite(dx) || !finite(dy) || absolute(dx) > RASTER_LIMIT || absolute(dy) > RASTER_LIMIT)
        return LIMIT;
    /* Half-grid ties round toward positive infinity so moving an integer clip
     * origin cannot change which side of a tie the same device point chooses. */
    dx += .5;
    dy += .5;
    FT_Pos ix = (FT_Pos)dx, iy = (FT_Pos)dy;
    if (dx < ix)
        --ix;
    if (dy < iy)
        --iy;
    *out = (FT_Vector){ix, iy};
    return OK;
}
static Status bounds(Context *c, const Outline *o, const Map *map, double origin_x, double origin_y,
                     os64_geometry_bounds_t *out)
{
    if (out)
        *out = (os64_geometry_bounds_t){.empty = true};
    if (!c || !o || o->owner != c || !out || !map_valid(map) || !finite(origin_x) ||
        !finite(origin_y))
        return BAD;
    Status s = enter(c);
    if (s)
        return s;
    double bx, by;
    s = placement(o, map, origin_x, origin_y, &bx, &by);
    if (s)
        return leave(c, s);
    os64_geometry_bounds_t result = {.empty = true};
    for (unsigned i = 0; i < o->ft.n_points; ++i) {
        if (!spend(c, 1))
            return leave(c, c->why);
        FT_Vector point;
        s = device_point(o, map, bx, by, i, &point);
        if (s)
            return leave(c, s);
        double x = point.x / 64.0, y = point.y / 64.0;
        if (result.empty) {
            result = (os64_geometry_bounds_t){false, x, y, x, y};
        } else {
            if (x < result.x_min)
                result.x_min = x;
            if (y < result.y_min)
                result.y_min = y;
            if (x > result.x_max)
                result.x_max = x;
            if (y > result.y_max)
                result.y_max = y;
        }
    }
    *out = result;
    return leave(c, OK);
}
static Status raster(Context *c, const Outline *o, const Map *map, const os64_geometry_target_t *t)
{
    if (!c || !o || o->owner != c || !map_valid(map) || !t || !t->pixels || !finite(t->origin_x) ||
        !finite(t->origin_y) || !t->width || !t->height || t->stride < t->width)
        return BAD;
    if (t->width > 65535 || t->height > 65535 || t->stride > SIZE_MAX / t->height ||
        t->stride * t->height > t->capacity)
        return LIMIT;
    Status s = enter(c);
    if (s)
        return s;
    double bx, by;
    s = placement(o, map, t->origin_x, t->origin_y, &bx, &by);
    if (s)
        return leave(c, s);
    FT_Outline temp = {0};
    s = new_ft_outline(c, o->ft.n_points, o->ft.n_contours, &temp);
    if (s)
        return leave(c, s);
    for (unsigned i = 0; i < temp.n_points; ++i) {
        if (!spend(c, 1)) {
            s = c->why;
            goto done;
        }
        s = device_point(o, map, bx, by, i, &temp.points[i]);
        if (s)
            goto done;
        temp.tags[i] = o->ft.tags[i];
    }
    for (unsigned i = 0; i < temp.n_contours; ++i)
        temp.contours[i] = o->ft.contours[i];
    if (t->even_odd)
        temp.flags |= FT_OUTLINE_EVEN_ODD_FILL;
    for (uint32_t y = 0; y < t->height; ++y) {
        if (!spend(c, t->width)) {
            s = c->why;
            goto done;
        }
        uint8_t *p = t->pixels + (size_t)y * t->stride;
        for (uint32_t x = 0; x < t->width; ++x)
            p[x] = 0;
    }
    Spans args = {c, t};
    FT_Raster_Params params = {.flags =
                                   FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP,
                               .gray_spans = spans,
                               .user = &args,
                               .clip_box = {0, 0, t->width, t->height}};
    s = backend(c, FT_Outline_Render(c->library, &temp, &params));
done:
    FT_Outline_Done(c->library, &temp);
    return leave(c, s);
}
static const os64_geometry_backend_t operations = {OS64_GEOMETRY_BACKEND_REVISION,
                                                   sizeof(os64_geometry_backend_t),
                                                   create,
                                                   destroy,
                                                   stats,
                                                   path,
                                                   stroke,
                                                   info,
                                                   release,
                                                   raster,
                                                   bounds,
                                                   stroke_with_caps};
const os64_geometry_backend_t *os64_freetype_geometry_v1(void)
{
    return &operations;
}
