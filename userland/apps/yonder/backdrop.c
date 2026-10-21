// backdrop.c — a moving background drawn by its own pixels (backdrop.h).

#include "backdrop.h"

#include <stddef.h>

#include "os64/damage.h"

// `v` / 255, rounded half away from zero: exact whenever `v` is a multiple
// of 255, which is what keeps b = 0, b = 255, a = 0 and a = 255 exact.
static int32_t div255(int32_t v)
{
    return v >= 0 ? (v + 127) / 255 : -((-v + 127) / 255);
}

uint32_t yonder_backdrop_pixel(uint32_t k, uint32_t w, uint32_t p, uint32_t b)
{
    int32_t a = (int32_t)(b >> 24);
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int32_t kc = (int32_t)((k >> sh) & 0xffu), wc = (int32_t)((w >> sh) & 0xffu);
        int32_t pc = (int32_t)((p >> sh) & 0xffu), bc = (int32_t)((b >> sh) & 0xffu);
        int32_t f = kc + div255((wc - kc) * bc);
        int32_t c = pc + div255((f - pc) * a);
        out |= (uint32_t)(c < 0 ? 0 : c > 255 ? 255 : c) << sh;
    }
    return out;
}

int32_t yonder_backdrop_changes(const uint32_t *was, const uint32_t *now, int32_t w, int32_t h,
                                yonder_backdrop_run_t *runs, int32_t cap, int64_t *changed)
{
    int32_t n = 0;
    int64_t px = 0;
    for (int32_t y = 0; y < h; y++) {
        const uint32_t *a = was + (int64_t)y * w, *b = now + (int64_t)y * w;
        for (int32_t x = 0; x < w;) {
            if (a[x] == b[x]) {
                x++;
                continue;
            }
            int32_t from = x;
            while (x < w && a[x] != b[x])
                x++;
            px += x - from;
            if (n < cap)
                runs[n] = (yonder_backdrop_run_t){y, from, x - from};
            n++;
        }
    }
    *changed = px;
    return n <= cap ? n : -1;
}

// `v` modulo `m` for m > 0, never negative.
static int32_t wrap(int64_t v, int32_t m)
{
    int64_t r = v % m;
    return (int32_t)(r < 0 ? r + m : r);
}

// The moving layer's pixel at view (x, y), or 0 (alpha 0: absent) off its
// one copy along an axis that does not repeat. `row` is the tile's row for
// y, or NULL when y is off it.
static uint32_t layer_at(const yonder_backdrop_tile_t *t, const uint32_t *row, int32_t x)
{
    if (row == NULL)
        return 0;
    int64_t u = (int64_t)x - t->x;
    if (!t->repeat_x && (u < 0 || u >= t->w))
        return 0;
    return row[wrap(u, t->w)];
}

static const uint32_t *layer_row(const yonder_backdrop_tile_t *t, int32_t y)
{
    int64_t v = (int64_t)y - t->y;
    if (!t->repeat_y && (v < 0 || v >= t->h))
        return NULL;
    return t->px + (int64_t)wrap(v, t->h) * t->w;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

typedef struct {
    int32_t x0, y0, x1, y1;     // the box round what was written; x0 >= x1 when nothing
} Box;

// Pixels x0 to x1 of row `y` composed, and the box round them grown.
static void compose_span(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                         const yonder_backdrop_tile_t *t, const uint32_t *row, int32_t y,
                         int32_t x0, int32_t x1, Box *box, int64_t *written)
{
    if (x0 >= x1)
        return;
    const uint32_t *k = s->k + (int64_t)y * s->pitch, *w = s->w + (int64_t)y * s->pitch;
    const uint32_t *p = s->p + (int64_t)y * s->pitch;
    uint32_t *out = dst + (int64_t)y * pitch;
    for (int32_t x = x0; x < x1; x++)
        out[x] = yonder_backdrop_pixel(k[x], w[x], p[x], layer_at(t, row, x));
    *written += x1 - x0;
    box->x0 = min32(box->x0, x0);
    box->x1 = max32(box->x1, x1);
    box->y0 = min32(box->y0, y);
    box->y1 = max32(box->y1, y + 1);
}

static void compose_row(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                        const yonder_backdrop_tile_t *t, const uint32_t *row, int32_t y,
                        int32_t x0, int32_t x1, const yonder_backdrop_out_t *o, Box *box,
                        int64_t *written)
{
    // The pixels of row `y` from x0 to x1 that no skip rectangle covers,
    // left to right: a skip covering x0 moves x0 past it; otherwise compose
    // up to the next skip's start. The skips are few (holes and rebuilt
    // rectangles), so each is asked at every step.
    while (x0 < x1) {
        int32_t next = x1;
        bool jumped = false;
        for (uint32_t i = 0; i < o->nskip; i++) {
            os64_gui_rect_t r = o->skip[i];
            if (r.w <= 0 || r.h <= 0 || y < r.y || (int64_t)y >= (int64_t)r.y + r.h)
                continue;
            int64_t rx1 = (int64_t)r.x + r.w;
            if (r.x <= x0 && rx1 > x0) {
                x0 = (int32_t)(rx1 < x1 ? rx1 : x1);
                jumped = true;
                break;
            }
            if (r.x > x0 && r.x < next)
                next = r.x;
        }
        if (jumped)
            continue;
        compose_span(dst, pitch, s, t, row, y, x0, next, box, written);
        x0 = next;
    }
}

static void report(yonder_backdrop_out_t *o, const Box *box)
{
    if (box->x0 >= box->x1 || o->damage == NULL)
        return;
    os64_gui_rect_t r = {box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0};
    (void)os64_damage_add(o->damage, &o->ndamage, o->cap, o->slack, r);
}

// `a` ∩ `b`, empty when they do not meet.
static os64_gui_rect_t meet(os64_gui_rect_t a, os64_gui_rect_t b)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w < (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h < (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return (os64_gui_rect_t){0, 0, 0, 0};
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}

void yonder_backdrop_compose_area(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                                  const yonder_backdrop_tile_t *t, os64_gui_rect_t part,
                                  os64_gui_rect_t area, yonder_backdrop_out_t *out)
{
    os64_gui_rect_t a = meet(meet(area, part), (os64_gui_rect_t){0, 0, s->width, s->height});
    if (a.w <= 0 || t->w <= 0 || t->h <= 0)
        return;
    Box box = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (int32_t y = a.y; y < a.y + a.h; y++)
        compose_row(dst, pitch, s, t, layer_row(t, y), y, a.x, a.x + a.w, out, &box, &out->written);
    report(out, &box);
}

// The first copy's origin along one axis whose copy can reach `from`, and
// how many copies cover [from, from + len): one, at the tile, when the axis
// does not repeat.
static void copies(int32_t origin, int32_t size, bool repeat, int32_t from, int32_t len,
                   int64_t *first, int64_t *count)
{
    if (!repeat) {
        *first = origin;
        *count = 1;
        return;
    }
    *first = (int64_t)from - wrap((int64_t)from - origin, size);
    *count = ((int64_t)from + len - *first + size - 1) / size;
}

void yonder_backdrop_compose_runs(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                                  const yonder_backdrop_tile_t *t, os64_gui_rect_t part,
                                  const yonder_backdrop_run_t *runs, int32_t nruns,
                                  yonder_backdrop_out_t *out)
{
    part = meet(part, (os64_gui_rect_t){0, 0, s->width, s->height});
    if (part.w <= 0 || part.h <= 0 || t->w <= 0 || t->h <= 0 || nruns <= 0)
        return;
    int64_t fx, nx, fy, ny;
    copies(t->x, t->w, t->repeat_x, part.x, part.w, &fx, &nx);
    copies(t->y, t->h, t->repeat_y, part.y, part.h, &fy, &ny);
    int64_t px1 = (int64_t)part.x + part.w, py1 = (int64_t)part.y + part.h;
    for (int64_t j = 0; j < ny; j++) {
        int64_t cy = fy + j * t->h;
        for (int64_t i = 0; i < nx; i++) {
            int64_t cx = fx + i * t->w;
            Box box = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
            for (int32_t r = 0; r < nruns; r++) {
                int64_t y = cy + runs[r].y;
                if (y < part.y || y >= py1)
                    continue;
                int64_t x0 = cx + runs[r].x, x1 = x0 + runs[r].n;
                if (x0 < part.x)
                    x0 = part.x;
                if (x1 > px1)
                    x1 = px1;
                if (x0 >= x1)
                    continue;
                compose_row(dst, pitch, s, t, layer_row(t, (int32_t)y), (int32_t)y, (int32_t)x0,
                            (int32_t)x1, out, &box, &out->written);
            }
            report(out, &box);
        }
    }
}

// ── The policy ──────────────────────────────────────────────────────────

// A running measure that follows the last few samples.
static double follow(double was, double sample)
{
    return was == 0 ? sample : was * 0.75 + sample * 0.25;
}

void yonder_backdrop_policy_init(yonder_backdrop_policy_t *p)
{
    *p = (yonder_backdrop_policy_t){0};
    p->need = YONDER_BACKDROP_QUIET;
}

void yonder_backdrop_ordinary(yonder_backdrop_policy_t *p)
{
    p->quiet = 0;
}

void yonder_backdrop_quiet_frame(yonder_backdrop_policy_t *p)
{
    if (p->quiet < UINT32_MAX)
        p->quiet++;
}

void yonder_backdrop_painted(yonder_backdrop_policy_t *p, double spent, int64_t px)
{
    if (px > 0)
        p->paint_cost = follow(p->paint_cost, spent / (double)px);
}

bool yonder_backdrop_should_build(const yonder_backdrop_policy_t *p)
{
    return !p->unprofitable && p->paint_cost > 0 && p->quiet >= p->need;
}

void yonder_backdrop_built(yonder_backdrop_policy_t *p, double spent, int64_t px)
{
    if (px > 0)
        p->pass_cost = follow(p->pass_cost, spent / (3.0 * (double)px));
    p->build_cost = spent;
    p->saved_cost = 0;
    p->paid = false;
    p->dearer = 0;
}

bool yonder_backdrop_should_compose(yonder_backdrop_policy_t *p, int64_t changed, int64_t invalid,
                                    int64_t part)
{
    // Unmeasured, the layer composes: its first frames are how it is
    // measured, and a layer that turns out dearer is given up below.
    if (p->compose_cost == 0 || p->paint_cost == 0)
        return true;
    double cost = (double)changed * p->compose_cost + 3.0 * (double)invalid * p->pass_cost;
    bool cheaper = cost <= (double)part * p->paint_cost;
    if (invalid == 0) {
        p->dearer = cheaper ? 0 : p->dearer + 1;
        if (p->dearer >= YONDER_BACKDROP_DEARER)
            p->unprofitable = true;
    }
    return cheaper;
}

void yonder_backdrop_composed(yonder_backdrop_policy_t *p, double spent, int64_t written,
                              double rebuild_cost, int64_t rebuilt, int64_t part)
{
    if (written > 0)
        p->compose_cost = follow(p->compose_cost, spent / (double)written);
    if (rebuilt > 0)
        p->pass_cost = follow(p->pass_cost, rebuild_cost / (3.0 * (double)rebuilt));
    p->saved_cost += (double)part * p->paint_cost - spent - rebuild_cost;
    if (!p->paid && p->saved_cost >= p->build_cost) {
        p->paid = true;
        p->need = YONDER_BACKDROP_QUIET;
    }
}

void yonder_backdrop_lost(yonder_backdrop_policy_t *p)
{
    if (!p->paid)
        p->need = p->need * 2 < YONDER_BACKDROP_QUIET_MOST ? p->need * 2 : YONDER_BACKDROP_QUIET_MOST;
    p->paid = false;
}
