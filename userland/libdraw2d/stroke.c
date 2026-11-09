#include <draw2d/stroke.h>
#include <math.h>
typedef os64_geometry_point_t Point;
typedef os64_geometry_map_t Map;
typedef os64_geometry_command_t Command;
typedef draw2d_stroke_style_t Style;
typedef draw2d_dash_t Dash;
typedef bool Output;
typedef struct { Point p[4]; unsigned degree; double length; } Segment;
typedef struct { double start, end; } Span;
typedef struct { const draw2d_stroke_ops_t *ops; os64_geometry_status_t why; } Cutter;
static bool spend(Cutter *c, uint64_t n)
{
    if (c->why != OS64_GEOMETRY_OK) return false;
    c->why = c->ops->spend(c->ops->opaque, n);
    return c->why == OS64_GEOMETRY_OK;
}
static void *grow(Cutter *c, void *p, size_t *capacity, size_t count, size_t size, size_t limit)
{
    void *out = p;
    c->why = c->ops->grow(c->ops->opaque, &out, capacity, count, size, limit);
    return out;
}
static bool emit(Cutter *c, Output *out, Command command)
{
    c->why = c->ops->emit(c->ops->opaque, *out, &command);
    return c->why == OS64_GEOMETRY_OK;
}
static bool simple(Cutter *c, Output *out, os64_geometry_verb_t verb, Point p)
{ return emit(c, out, (Command){.verb=verb,.p={p}}); }
static bool cap(Cutter *c, Output *out, Point p, const Style *style, const Map *map, double error, Point tangent)
{
    (void)out; (void)style; (void)map; (void)error;
    c->why = c->ops->cap(c->ops->opaque, p, tangent);
    return c->why == OS64_GEOMETRY_OK;
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
static bool measure(Cutter *c, const Segment *s, double error, unsigned depth, double *length)
{
    if (!spend(c, 1))
        return false;
    double low = distance(s->p[0], s->p[s->degree]), high = 0;
    for (unsigned i = 1; i <= s->degree; ++i)
        high += distance(s->p[i - 1], s->p[i]);
    if (!isfinite(high) || !isfinite(low)) {
        c->why = OS64_GEOMETRY_LIMIT;
        return false;
    }
    if (high - low <= 2 * error) {
        *length = low * .5 + high * .5;
        return true;
    }
    if (depth == 28 || !(error > 0)) {
        c->why = OS64_GEOMETRY_LIMIT;
        return false;
    }
    Segment a, b;
    double x, y;
    split(s, .5, &a, &b);
    if (!measure(c, &a, error * .5, depth + 1, &x) || !measure(c, &b, error * .5, depth + 1, &y))
        return false;
    *length = x + y;
    if (!isfinite(*length)) {
        c->why = OS64_GEOMETRY_LIMIT;
        return false;
    }
    return true;
}
static bool parameter(Cutter *c, const Segment *s, double position, double error, double *t)
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
    c->why = OS64_GEOMETRY_LIMIT;
    return false;
}
static bool solid(Cutter *c, const Command *commands, size_t count, const Map *map, double error,
                  const Style *style, Output *center, Output *caps)
{
    Point start = commands[0].p[0];
    bool nonzero = false;
    for (size_t i = 1; i < count; ++i) {
        if (!spend(c, 1))
            return false;
        unsigned n = commands[i].verb == OS64_GEOMETRY_CLOSE       ? 0
                     : commands[i].verb == OS64_GEOMETRY_CUBIC     ? 3
                     : commands[i].verb == OS64_GEOMETRY_QUADRATIC ? 2
                                                                   : 1;
        for (unsigned j = 0; j < n; ++j)
            nonzero |= commands[i].p[j].x != start.x || commands[i].p[j].y != start.y;
    }
    if (!nonzero)
        return count == 1 || cap(c, caps, start, style, map, error, (Point){1,0});
    for (size_t i = 0; i < count; ++i)
        if (!emit(c, center, commands[i]))
            return false;
    return true;
}
static bool positions(Cutter *c, const Dash *dash, double sum, double length, Span **spans,
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
        if (!spend(c, 1))
            return false;
        offset -= dash->values[index % dash->count];
        index = (index + 1) % n;
    }
    double at = 0, remaining = dash->values[index % dash->count] - offset;
    for (;;) {
        if (!spend(c, 1))
            return false;
        double step = fmin(remaining, length - at), end = at + step;
        if (step > 0 && end <= at) {
            c->why = OS64_GEOMETRY_LIMIT;
            return false;
        }
        if (!(index % 2)) {
            *spans = grow(c, *spans, capacity, *count, sizeof(**spans),
                              c->ops->command_limit);
            if (c->why != OS64_GEOMETRY_OK)
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
static bool point_at(Cutter *c, const Segment *segments, size_t n, double at, double error,
                     Point *out, Point *tangent)
{
    for (size_t i = 0; i < n; ++i) {
        if (!spend(c, 1))
            return false;
        if (at <= segments[i].length || i + 1 == n) {
            double t;
            Segment a, b;
            if (!parameter(c, &segments[i], at, error, &t))
                return false;
            split(&segments[i], t, &a, &b);
            *out = a.p[a.degree];
            *tangent = (Point){0};
            for(unsigned j=a.degree;j>0 && tangent->x==0 && tangent->y==0;--j)
                *tangent=(Point){a.p[a.degree].x-a.p[j-1].x,a.p[a.degree].y-a.p[j-1].y};
            for(unsigned j=1;j<=b.degree && tangent->x==0 && tangent->y==0;++j)
                *tangent=(Point){b.p[j].x-b.p[0].x,b.p[j].y-b.p[0].y};
            return true;
        }
        at -= segments[i].length;
    }
    c->why = OS64_GEOMETRY_ENGINE_ERROR;
    return false;
}
static bool interval(Cutter *c, const Segment *segments, size_t n, Span span, double error,
                     Output *out, bool move)
{
    double at = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!spend(c, 1))
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
os64_geometry_status_t draw2d_stroke_cut(const draw2d_stroke_ops_t *ops,
    const Command *input, size_t command_count, const Map *map, double error, const Style *style)
{
    if (!ops || !ops->spend || !ops->grow || !ops->free || !ops->emit || !ops->cap ||
        !map || !style || (command_count && !input) || (style->dash.count && !style->dash.values) ||
        !isfinite(error) || error <= 0 || !isfinite(style->dash.offset)) return OS64_GEOMETRY_BAD_ARGUMENT;
    if (command_count > ops->command_limit || style->dash.count > ops->command_limit)
        return OS64_GEOMETRY_LIMIT;
    for (size_t i=0;i<style->dash.count;++i)
        if (!isfinite(style->dash.values[i]) || style->dash.values[i]<0) return OS64_GEOMETRY_BAD_ARGUMENT;
    Cutter storage = {ops, OS64_GEOMETRY_OK}, *c = &storage;
    bool contour=false, closed=false;
    for (size_t i=0;i<command_count;++i) {
        if (!spend(c, 1)) return c->why;
        Command v=input[i];
        if (v.verb<OS64_GEOMETRY_MOVE || v.verb>OS64_GEOMETRY_CLOSE ||
            (v.verb!=OS64_GEOMETRY_MOVE && (!contour || closed))) return OS64_GEOMETRY_BAD_ARGUMENT;
        if (v.verb==OS64_GEOMETRY_MOVE) { contour=true; closed=false; }
        if (v.verb==OS64_GEOMETRY_CLOSE) closed=true;
        unsigned n=v.verb==OS64_GEOMETRY_CLOSE?0:v.verb==OS64_GEOMETRY_CUBIC?3:v.verb==OS64_GEOMETRY_QUADRATIC?2:1;
        for(unsigned j=0;j<n;++j) if(!isfinite(v.p[j].x)||!isfinite(v.p[j].y)) return OS64_GEOMETRY_BAD_ARGUMENT;
    }
    double sum = 0;
    for (size_t i = 0; i < style->dash.count; ++i)
        sum += style->dash.values[i];
    if (style->dash.count % 2)
        sum *= 2;
    double norm = fmax(fabs(map->a) + fabs(map->c), fabs(map->b) + fabs(map->d));
    double metric_error = error / (8 * fmax(1, norm));
    if (!isfinite(sum) || !isfinite(norm) || !(metric_error > 0))
        return OS64_GEOMETRY_LIMIT;
    Output a = false, b = true; Output *center = &a, *caps = &b;
    Segment *segments = NULL;
    size_t segment_capacity = 0;
    Span *spans = NULL;
    size_t span_capacity = 0;
    for (size_t first = 0; first < command_count;) {
        size_t finish = first + 1;
        while (finish < command_count && input[finish].verb != OS64_GEOMETRY_MOVE) {
            if (!spend(c, 1))
                goto done;
            ++finish;
        }
        const Command *commands = input + first;
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
                c->why = OS64_GEOMETRY_LIMIT;
                goto done;
            }
            length = next;
            current = s.p[s.degree];
            segments = grow(c, segments, &segment_capacity, n, sizeof(*segments),
                                c->ops->command_limit);
            if (c->why != OS64_GEOMETRY_OK)
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
                    Point p, tangent;
                    if (!point_at(c, segments, n, spans[i].start, precision, &p, &tangent) ||
                        !cap(c, caps, p, style, map, error, tangent))
                        goto done;
                } else if (!interval(c, segments, n, spans[i], precision, center, true))
                    goto done;
            }
        }
        first = finish;
    }
done:
    c->ops->free(c->ops->opaque, segments);
    c->ops->free(c->ops->opaque, spans);
    return c->why;
}
