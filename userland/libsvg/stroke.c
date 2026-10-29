#include "internal.h"

typedef os64_geometry_command_t Command;
typedef struct {
    Point p[4];
    unsigned degree;
    double length;
} Segment;
typedef struct {
    double start, end;
} Span;

static svg_geometry_t *geometry(Context *c)
{
    svg_geometry_t *g = svg_alloc(c, sizeof(*g));
    if (g) {
        *g = (svg_geometry_t){.owner = c};
        ++c->stats.geometries;
    }
    return g;
}
static bool emit(Context *c, svg_geometry_t *g, Command command)
{
    for (size_t i = 0; i < 3; ++i)
        if (!svg_point_valid(command.p[i])) {
            c->why = SVG_LIMIT;
            return false;
        }
    size_t limit = c->options.limits.geometry_commands;
    if (limit > OS64_GEOMETRY_COMMAND_MAX)
        limit = OS64_GEOMETRY_COMMAND_MAX;
    if (!svg_spend(c, 1))
        return false;
    g->commands = svg_grow(c, g->commands, &g->capacity, g->count, sizeof(*g->commands), limit);
    if (c->why != SVG_OK)
        return false;
    g->commands[g->count++] = command;
    return true;
}
static bool simple(Context *c, svg_geometry_t *g, os64_geometry_verb_t verb, Point p)
{
    return emit(c, g, (Command){.verb = verb, .p = {p}});
}
static Point mix(Point a, Point b, double t)
{
    return (Point){a.x * (1 - t) + b.x * t, a.y * (1 - t) + b.y * t};
}
static void split(const Segment *s, double t, Segment *left, Segment *right)
{
    Point p[4];
    *left = *right = *s;
    for (unsigned i = 0; i <= s->degree; ++i)
        p[i] = s->p[i];
    left->p[0] = p[0];
    right->p[s->degree] = p[s->degree];
    for (unsigned level = 1; level <= s->degree; ++level) {
        for (unsigned i = 0; i <= s->degree - level; ++i)
            p[i] = mix(p[i], p[i + 1], t);
        left->p[level] = p[0];
        right->p[s->degree - level] = p[s->degree - level];
    }
}
static double distance(Point a, Point b)
{
    return hypot(b.x - a.x, b.y - a.y);
}
// Chord/control-polygon bounds bracket a Bezier's length. Splitting the
// error allowance between children bounds the accumulated measurement error,
// rather than granting a fresh allowance to each leaf. Curves remain curves.
static bool measure(Context *c, const Segment *s, double error, unsigned depth, double *length)
{
    if (!svg_spend(c, 1))
        return false;
    double low = distance(s->p[0], s->p[s->degree]), high = 0;
    for (unsigned i = 1; i <= s->degree; ++i)
        high += distance(s->p[i - 1], s->p[i]);
    if (!isfinite(high) || !isfinite(low)) {
        c->why = SVG_LIMIT;
        return false;
    }
    if (high - low <= 2 * error) {
        *length = low * .5 + high * .5;
        return true;
    }
    if (depth == 28 || !(error > 0)) {
        c->why = SVG_LIMIT;
        return false;
    }
    Segment a, b;
    double x, y;
    split(s, .5, &a, &b);
    if (!measure(c, &a, error * .5, depth + 1, &x) || !measure(c, &b, error * .5, depth + 1, &y))
        return false;
    *length = x + y;
    if (!isfinite(*length)) {
        c->why = SVG_LIMIT;
        return false;
    }
    return true;
}
static bool parameter(Context *c, const Segment *s, double position, double error, double *t)
{
    if (position <= 0) {
        *t = 0;
        return true;
    }
    if (position >= s->length) {
        *t = 1;
        return true;
    }
    if (s->degree == 1) {
        *t = position / s->length;
        return true;
    }
    double low = 0, high = 1;
    for (unsigned step = 0; step < 64; ++step) {
        double middle = (low + high) * .5, length;
        Segment a, b;
        split(s, middle, &a, &b);
        if (!measure(c, &a, error * .25, 0, &length))
            return false;
        if (fabs(length - position) <= error) {
            *t = middle;
            return true;
        }
        if (middle == low || middle == high)
            break;
        if (length < position)
            low = middle;
        else
            high = middle;
    }
    c->why = SVG_LIMIT;
    return false;
}
static bool cap(Context *c, svg_geometry_t *caps, Point p, const svg_style_t *style, const Map *map,
                double error)
{
    if (style->stroke_style.cap == OS64_GEOMETRY_BUTT)
        return true;
    double radius = style->stroke_style.width * .5;
    if (style->stroke_style.cap == OS64_GEOMETRY_SQUARE) {
        // A zero-length subpath has no direction; its effective tangent is +x.
        // Zero-length dash squares use the same user-axis convention as Chromium.
        return simple(c, caps, OS64_GEOMETRY_MOVE, (Point){p.x - radius, p.y - radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x + radius, p.y - radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x + radius, p.y + radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x - radius, p.y + radius}) &&
               simple(c, caps, OS64_GEOMETRY_CLOSE, (Point){0});
    }
    svg_shape_t shape = {.kind = SVG_SHAPE_ELLIPSE, .x = p.x, .y = p.y, .rx = radius, .ry = radius};
    svg_path_t *path = NULL;
    svg_geometry_t *circle = NULL;
    svg_parse_result_t result;
    Status status = svg_make_shape(c, &shape, &path, &result);
    if (status == SVG_OK)
        status = svg_lower(c, path, map, error, &circle);
    if (status == SVG_OK)
        for (size_t i = 0; i < circle->count; ++i)
            if (!emit(c, caps, circle->commands[i]))
                break;
    svg_free_geometry(c, circle);
    svg_free_path(c, path);
    if (status != SVG_OK && c->why == SVG_OK)
        c->why = status;
    return c->why == SVG_OK;
}
static bool solid(Context *c, const Command *commands, size_t count, const Map *map, double error,
                  const svg_style_t *style, svg_geometry_t *center, svg_geometry_t *caps)
{
    Point start = commands[0].p[0];
    bool nonzero = false;
    for (size_t i = 1; i < count; ++i) {
        if (!svg_spend(c, 1))
            return false;
        unsigned n = commands[i].verb == OS64_GEOMETRY_CLOSE       ? 0
                     : commands[i].verb == OS64_GEOMETRY_CUBIC     ? 3
                     : commands[i].verb == OS64_GEOMETRY_QUADRATIC ? 2
                                                                   : 1;
        for (unsigned j = 0; j < n; ++j)
            nonzero |= commands[i].p[j].x != start.x || commands[i].p[j].y != start.y;
    }
    if (!nonzero)
        return count == 1 || cap(c, caps, start, style, map, error);
    for (size_t i = 0; i < count; ++i)
        if (!emit(c, center, commands[i]))
            return false;
    return true;
}
static bool positions(Context *c, const svg_dash_t *dash, double sum, double length, Span **spans,
                      size_t *count, size_t *capacity)
{
    *count = 0;
    size_t n = dash->count * (dash->count % 2 ? 2 : 1), index = 0;
    double offset = fmod(dash->offset, sum);
    if (offset < 0)
        offset += sum;
    if (offset >= sum)
        offset = 0;
    // A clipped positive dash with zero remaining length is not an explicit
    // zero dash. At an exact offset boundary advance to the next entry.
    while (offset > 0 && offset >= dash->values[index % dash->count]) {
        if (!svg_spend(c, 1))
            return false;
        offset -= dash->values[index % dash->count];
        index = (index + 1) % n;
    }
    double at = 0, remaining = dash->values[index % dash->count] - offset;
    for (;;) {
        if (!svg_spend(c, 1))
            return false;
        double step = fmin(remaining, length - at), end = at + step;
        if (step > 0 && end <= at) {
            c->why = SVG_LIMIT;
            return false;
        }
        if (!(index % 2)) {
            *spans = svg_grow(c, *spans, capacity, *count, sizeof(**spans),
                              c->options.limits.geometry_commands);
            if (c->why != SVG_OK)
                return false;
            (*spans)[(*count)++] = (Span){at, end};
        }
        at = end;
        // On a zero-length subpath, skip an empty initial gap so the
        // following dash can supply its cap. Positive gaps still hide it.
        if (at >= length && (length > 0 || remaining > 0 || !(index % 2)))
            break;
        index = (index + 1) % n;
        remaining = dash->values[index % dash->count];
    }
    return true;
}
static bool point_at(Context *c, const Segment *segments, size_t n, double at, double error,
                     Point *out)
{
    for (size_t i = 0; i < n; ++i) {
        if (!svg_spend(c, 1))
            return false;
        if (at <= segments[i].length || i + 1 == n) {
            double t;
            Segment a, b;
            if (!parameter(c, &segments[i], at, error, &t))
                return false;
            split(&segments[i], t, &a, &b);
            *out = a.p[a.degree];
            return true;
        }
        at -= segments[i].length;
    }
    c->why = SVG_ENGINE_ERROR;
    return false;
}
static bool interval(Context *c, const Segment *segments, size_t n, Span span, double error,
                     svg_geometry_t *out, bool move)
{
    double at = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!svg_spend(c, 1))
            return false;
        const Segment *s = segments + i;
        double end = at + s->length;
        if (s->length && span.start < end && span.end > at) {
            double t0, t1;
            if (!parameter(c, s, fmax(0, span.start - at), error, &t0) ||
                !parameter(c, s, fmin(s->length, span.end - at), error, &t1))
                return false;
            Segment part = *s, discard;
            if (t1 < 1)
                split(s, t1, &part, &discard);
            if (t0 > 0) {
                Segment whole = part;
                split(&whole, t0 / t1, &discard, &part);
            }
            if (move && !simple(c, out, OS64_GEOMETRY_MOVE, part.p[0]))
                return false;
            move = false;
            Command command = {.verb = s->degree == 1   ? OS64_GEOMETRY_LINE
                                       : s->degree == 2 ? OS64_GEOMETRY_QUADRATIC
                                                        : OS64_GEOMETRY_CUBIC};
            for (unsigned j = 0; j < s->degree; ++j)
                command.p[j] = part.p[j + 1];
            if (!emit(c, out, command))
                return false;
        }
        at = end;
        if (at >= span.end)
            break;
    }
    return true;
}
Status svg_stroke_geometry(Context *c, const svg_geometry_t *source, const Map *map, double error,
                           const svg_style_t *style, svg_geometry_t **centerline,
                           svg_geometry_t **cap_geometry)
{
    *centerline = *cap_geometry = NULL;
    double sum = 0;
    for (size_t i = 0; i < style->dash.count; ++i)
        sum += style->dash.values[i];
    if (style->dash.count % 2)
        sum *= 2;
    double norm = fmax(fabs(map->a) + fabs(map->c), fabs(map->b) + fabs(map->d));
    double metric_error = error / (8 * fmax(1, norm));
    if (!isfinite(sum) || !isfinite(norm) || !(metric_error > 0))
        return SVG_LIMIT;
    svg_geometry_t *center = geometry(c), *caps = geometry(c);
    Segment *segments = NULL;
    size_t segment_capacity = 0;
    Span *spans = NULL;
    size_t span_capacity = 0;
    if (!center || !caps)
        goto done;
    for (size_t first = 0; first < source->count;) {
        size_t finish = first + 1;
        while (finish < source->count && source->commands[finish].verb != OS64_GEOMETRY_MOVE) {
            if (!svg_spend(c, 1))
                goto done;
            ++finish;
        }
        const Command *commands = source->commands + first;
        size_t count = finish - first;
        if (!sum) {
            if (!solid(c, commands, count, map, error, style, center, caps))
                goto done;
            first = finish;
            continue;
        }
        if (count == 1) {
            first = finish;
            continue;
        }
        bool closed = commands[count - 1].verb == OS64_GEOMETRY_CLOSE;
        Point start = commands[0].p[0], current = start;
        size_t n = 0;
        double length = 0, precision = metric_error / (count - 1);
        for (size_t i = 1; i < count; ++i) {
            Segment s = {.p = {current},
                         .degree = commands[i].verb == OS64_GEOMETRY_CUBIC       ? 3
                                   : commands[i].verb == OS64_GEOMETRY_QUADRATIC ? 2
                                                                                 : 1};
            for (unsigned j = 0; j < s.degree; ++j)
                s.p[j + 1] = commands[i].verb == OS64_GEOMETRY_CLOSE ? start : commands[i].p[j];
            if (!measure(c, &s, precision, 0, &s.length))
                goto done;
            double next = length + s.length;
            if (!isfinite(next) || (s.length > 0 && next <= length)) {
                c->why = SVG_LIMIT;
                goto done;
            }
            length = next;
            current = s.p[s.degree];
            segments = svg_grow(c, segments, &segment_capacity, n, sizeof(*segments),
                                c->options.limits.geometry_commands);
            if (c->why != SVG_OK)
                goto done;
            segments[n++] = s;
        }
        size_t span_count = 0;
        if (!positions(c, &style->dash, sum, length, &spans, &span_count, &span_capacity))
            goto done;
        if (closed && span_count == 1 && spans[0].start == 0 && spans[0].end == length) {
            if (!solid(c, commands, count, map, error, style, center, caps))
                goto done;
        } else {
            size_t begin = 0, end = span_count;
            if (closed && span_count > 1 && spans[0].start == 0 && spans[0].end > 0 &&
                spans[span_count - 1].end == length && spans[span_count - 1].start < length) {
                // The dash crosses the closing seam. Start at its trailing
                // piece and continue through the initial vertex without caps.
                if (!interval(c, segments, n, spans[span_count - 1], precision, center, true) ||
                    !interval(c, segments, n, spans[0], precision, center, false))
                    goto done;
                ++begin;
                --end;
            }
            for (size_t i = begin; i < end; ++i) {
                if (spans[i].start == spans[i].end) {
                    Point p;
                    if (!point_at(c, segments, n, spans[i].start, precision, &p) ||
                        !cap(c, caps, p, style, map, error))
                        goto done;
                } else if (!interval(c, segments, n, spans[i], precision, center, true))
                    goto done;
            }
        }
        first = finish;
    }
done:
    svg_free(c, segments);
    svg_free(c, spans);
    if (c->why == SVG_OK) {
        *centerline = center;
        *cap_geometry = caps;
    } else {
        svg_free_geometry(c, center);
        svg_free_geometry(c, caps);
    }
    return c->why;
}
