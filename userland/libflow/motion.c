// motion.c — animations (PILE3.md § Animations): the timeline that keeps
// them across layouts, Web Animations 1's clock, Easing 1's curves, a
// keyframe effect's value property by property by its animation type, and
// the matrices Transforms 1 and 2 interpolate.
//
// Nothing here is built from a frame before: a value is worked from its
// keyframes and the time, every time (the contracts, PILE3.md § Animations,
// both slices).

#include <float.h>
#include <math.h>
#include <stddef.h>

#include "internal.h"
#include "html/html.h"

static const double kPi = 3.14159265358979323846;

// ── The lists an element's style names ──────────────────────────────────

int32_t f_animations(const flow_style_t *s)
{
    return s->animations.nname;
}

flow_animation_t f_animation_at(const flow_style_t *s, int32_t i)
{
    const flow_animations_t *a = &s->animations;
    flow_animation_t r;
    os64_memset(&r, 0, sizeof(r));
    r.name = a->nname > 0 ? a->name[i % a->nname] : (flow_name_t){NULL, 0};
    r.duration_ms = a->nduration > 0 ? a->duration[i % a->nduration] : 0;
    r.delay_ms = a->ndelay > 0 ? a->delay[i % a->ndelay] : 0;
    // `ease`, the initial easing.
    r.easing = a->neasing > 0 ? a->easing[i % a->neasing]
                              : (flow_easing_t){FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, 0};
    r.iterations = a->niterations > 0 ? a->iterations[i % a->niterations] : 1;
    r.direction = a->ndirection > 0 ? a->direction[i % a->ndirection] : FLOW_DIRECTION_NORMAL;
    r.fill = a->nfill > 0 ? a->fill[i % a->nfill] : FLOW_FILL_NONE;
    r.paused = a->npaused > 0 && a->paused[i % a->npaused];
    return r;
}

// ── Easing (Easing 1 § 2) ───────────────────────────────────────────────

// A cubic Bézier's x or y at t, the curve from 0 to 1 through p1 and p2.
static double bezier(double p1, double p2, double t)
{
    double c = 3 * p1, b = 3 * (p2 - p1) - c, a = 1 - c - b;
    return ((a * t + b) * t + c) * t;
}

// A value held to a double's range. An easing's y1 and y2 may be anything
// a double holds, and its output whatever they make: worked in long double
// and held here, the arithmetic after it never meets infinity, nor
// infinity less infinity, which is no number at all.
static double held(long double v)
{
    return v > DBL_MAX ? DBL_MAX : v < -DBL_MAX ? -DBL_MAX : (double)v;
}

// The curve's y at t: bezier's arithmetic in long double, held, since 3 y1
// alone can overflow a double.
static double bezier_y(double p1, double p2, double t)
{
    long double c = 3.0L * p1, b = 3.0L * ((long double)p2 - p1) - c, a = 1 - c - b;
    return held(((a * t + b) * t + c) * t);
}

static double bezier_slope(double p1, double p2, double t)
{
    double c = 3 * p1, b = 3 * (p2 - p1) - c, a = 1 - c - b;
    return (3 * a * t + 2 * b) * t + c;
}

// The curve's y at a point whose x is within 1e-7 of `x` (the contract):
// Newton's steps from t = x, and halving the interval where they do not
// land — x(t) never falls, since x1 and x2 are in [0, 1].
static double cubic(const double p[4], double x)
{
    double t = x;
    for (int i = 0; i < 8; i++) {
        double err = bezier(p[0], p[2], t) - x;
        if (fabs(err) <= 1e-7)
            return bezier_y(p[1], p[3], t);
        double d = bezier_slope(p[0], p[2], t);
        if (fabs(d) < 1e-9)
            break;
        t -= err / d;
        if (t < 0 || t > 1)
            break;
    }
    double lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 64; i++) {
        double xt = bezier(p[0], p[2], t);
        if (fabs(xt - x) <= 1e-7)
            break;
        if (xt < x)
            lo = t;
        else
            hi = t;
        t = (lo + hi) / 2;
    }
    return bezier_y(p[1], p[3], t);
}

double f_ease(const flow_easing_t *e, double x, bool before)
{
    if (e == NULL || e->kind == FLOW_EASE_LINEAR)
        return x;
    if (e->kind == FLOW_EASE_CUBIC) {
        const double *p = e->p;
        // Outside [0, 1], the tangent at the nearer end (§ 2.2.1), held
        // as the curve is.
        if (x < 0) {
            if (p[0] > 0)
                return held((long double)p[1] / p[0] * x);
            if (p[2] > 0)
                return held((long double)p[3] / p[2] * x);
            return 0;
        }
        if (x > 1) {
            if (p[2] < 1)
                return held(1 + ((long double)p[3] - 1) / (p[2] - 1) * (x - 1));
            if (p[0] < 1)
                return held(1 + ((long double)p[1] - 1) / (p[0] - 1) * (x - 1));
            return 1;
        }
        return cubic(p, x);
    }
    // Steps (§ 2.3.1).
    double n = e->steps > 0 ? e->steps : 1;
    double step = floor(x * n);
    if (e->jump == FLOW_JUMP_START || e->jump == FLOW_JUMP_BOTH)
        step += 1;
    if (before && fmod(x * n, 1.0) == 0)
        step -= 1;
    if (x >= 0 && step < 0)
        step = 0;
    double jumps = e->jump == FLOW_JUMP_NONE ? n - 1 : e->jump == FLOW_JUMP_BOTH ? n + 1 : n;
    if (x <= 1 && step > jumps)
        step = jumps;
    return step / jumps;
}

// ── The clock (Web Animations 1 § 4.6, § 4.7) ───────────────────────────

typedef enum { PHASE_BEFORE, PHASE_ACTIVE, PHASE_AFTER } Phase;

typedef struct {
    Phase phase;
    double before_end, active_end;  // the boundaries, in local time
    bool in_effect;                 // it has an active time: it shows a value
    double progress;                // the iteration progress, directed
    double iteration;               // the current iteration's index
    double iteration_start;         // where that iteration starts, in local time
    bool forwards;                  // the current direction
    bool before;                    // the easing's before flag (§ 4.7.7)
} Clock;

static bool current_forwards(const flow_animation_t *a, double iteration)
{
    switch (a->direction) {
    case FLOW_DIRECTION_NORMAL: return true;
    case FLOW_DIRECTION_REVERSE: return false;
    default: {
        double d = iteration + (a->direction == FLOW_DIRECTION_ALTERNATE_REVERSE);
        return isinf(d) || fmod(d, 2) == 0;
    }
    }
}

static Clock clock_at(const flow_animation_t *a, double local)
{
    Clock k;
    os64_memset(&k, 0, sizeof(k));
    double dur = a->duration_ms > 0 ? a->duration_ms : 0;
    double count = a->iterations > 0 ? a->iterations : 0;
    double active = dur == 0 || count == 0 ? 0 : dur * count;
    double delay = a->delay_ms;
    double end = fmax(delay + active, 0);
    k.before_end = fmax(fmin(delay, end), 0);
    k.active_end = fmax(fmin(delay + active, end), 0);
    // Forwards, and the active interval's end not in it.
    k.phase = local < k.before_end ? PHASE_BEFORE
            : local >= k.active_end ? PHASE_AFTER : PHASE_ACTIVE;
    double at;
    if (k.phase == PHASE_BEFORE) {
        if (a->fill != FLOW_FILL_BACKWARDS && a->fill != FLOW_FILL_BOTH)
            return k;
        at = fmax(local - delay, 0);
    } else if (k.phase == PHASE_ACTIVE) {
        at = local - delay;
    } else {
        if (a->fill != FLOW_FILL_FORWARDS && a->fill != FLOW_FILL_BOTH)
            return k;
        at = fmax(fmin(local - delay, active), 0);
    }
    k.in_effect = true;
    double overall = dur == 0 ? (k.phase == PHASE_BEFORE ? 0 : count) : at / dur;
    double simple = isinf(overall) ? 0 : fmod(overall, 1.0);
    if (simple == 0 && k.phase != PHASE_BEFORE && at == active && count != 0)
        simple = 1;
    k.iteration = k.phase == PHASE_AFTER && isinf(count) ? INFINITY
                : simple == 1 ? floor(overall) - 1 : floor(overall);
    k.iteration_start = isinf(k.iteration) ? INFINITY : delay + k.iteration * dur;
    k.forwards = current_forwards(a, k.iteration);
    k.progress = k.forwards ? simple : 1 - simple;
    k.before = (k.phase == PHASE_BEFORE && k.forwards) || (k.phase == PHASE_AFTER && !k.forwards);
    return k;
}

// ── The timeline ────────────────────────────────────────────────────────

// What a layout changes in a record it meets, kept from before the first
// change so a layout that fails can put it back (f_timeline_undo).
typedef struct {
    double start;               // on the face's clock, ms
    double hold;                // paused: how far it had run
    bool paused;
    uint32_t seen;              // the layout that last met it
    int32_t place;              // its place in its element's list then
} FRecordState;

struct FRecord {
    const os64_html_node_t *node;
    char *name;
    uint32_t len;
    FRecordState now;
    FRecordState was;           // before the layout that last met it
    uint32_t made;              // the layout that made it
    FRecord *after;             // the record it was chained after then, or NULL
    FRecord *next;              // its element's next record
};

struct flow_timeline {
    const os64_html_document_t *doc;
    FRecord **records;
    int32_t n, cap;
    int32_t base;               // how many there were when this layout began
    uint32_t layout;
    FMap by_node;               // node -> its first record
};

flow_timeline_t *flow_timeline_new(const os64_html_document_t *doc)
{
    flow_timeline_t *t = os64_calloc(1, sizeof(*t));
    if (t != NULL)
        t->doc = doc;
    return t;
}

static void record_free(flow_timeline_t *t, FRecord *r)
{
    os64_html_release(t->doc, r->node);
    os64_free(r->name);
    os64_free(r);
}

void flow_timeline_free(flow_timeline_t *t)
{
    if (t == NULL)
        return;
    for (int32_t i = 0; i < t->n; i++)
        record_free(t, t->records[i]);
    os64_free(t->records);
    f_map_free(&t->by_node);
    os64_free(t);
}

int32_t f_timeline_records(const flow_timeline_t *t)
{
    return t != NULL ? t->n : 0;
}

void f_timeline_begin(flow_timeline_t *t)
{
    if (t != NULL) {
        t->layout++;
        t->base = t->n;
    }
}

// The local time a record is at: what it holds while paused.
static double local_time(const FRecord *r, double now)
{
    return r->now.paused ? r->now.hold : now - r->now.start;
}

// A name with its order in its list — a record's place, or a name's index
// — and the record it is or is matched to.
typedef struct {
    const char *text;
    uint32_t len;
    int32_t order;
    FRecord *record;
} Named;

static int32_t name_cmp(const Named *a, const Named *b)
{
    int c = os64_memcmp(a->text, b->text, a->len < b->len ? a->len : b->len);
    return c != 0 ? c : (a->len > b->len) - (a->len < b->len);
}

// By name, and within a name by order: a merge through `tmp`.
static void sort_named(Named *v, Named *tmp, int32_t n)
{
    if (n < 2)
        return;
    int32_t h = n / 2;
    sort_named(v, tmp, h);
    sort_named(v + h, tmp, n - h);
    int32_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        int32_t c = name_cmp(&v[j], &v[i]);
        tmp[k++] = c < 0 || (c == 0 && v[j].order < v[i].order) ? v[j++] : v[i++];
    }
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    os64_memcpy(v, tmp, (size_t)n * sizeof(*v));
}

bool f_timeline_element(flow_timeline_t *t, const os64_html_node_t *n, FMotion *m, double now)
{
    // Its records from the last layout and its names now, each sorted by
    // name and then by place: Animations 1 § 2 matches from the last name
    // to the first, each to the last unmatched record of its name, which
    // is then one walk down both lists from their ends — however many
    // names a page writes, none is compared with every record.
    int32_t nold = 0;
    FRecord *tail = NULL;
    for (FRecord *r = f_map_get(&t->by_node, n); r != NULL; r = r->next) {
        nold += r->now.seen != t->layout;
        tail = r;
    }
    int32_t total = nold + m->nanims;
    Named few[32], *buf = few;
    if (2 * total > F_ARRAY(few) &&
        (buf = os64_malloc((size_t)2 * (size_t)total * sizeof(*buf))) == NULL)
        return false;
    Named *old = buf, *names = buf + nold, *tmp = buf + total;
    int32_t k = 0;
    for (FRecord *r = f_map_get(&t->by_node, n); r != NULL; r = r->next)
        if (r->now.seen != t->layout)
            old[k++] = (Named){r->name, r->len, r->now.place, r};
    for (int32_t i = 0; i < m->nanims; i++) {
        m->anims[i].record = NULL;
        names[i] = (Named){m->anims[i].timing.name.text, m->anims[i].timing.name.len, i, NULL};
    }
    sort_named(old, tmp, nold);
    sort_named(names, tmp, m->nanims);
    for (int32_t a = nold - 1, b = m->nanims - 1; a >= 0 && b >= 0;) {
        int32_t c = name_cmp(&old[a], &names[b]);
        if (c > 0) {
            a--;                // a record whose name is gone
        } else if (c < 0) {
            b--;                // a name with no record left: it starts
        } else {
            m->anims[names[b].order].record = old[a].record;
            a--;
            b--;
        }
    }
    if (buf != few)
        os64_free(buf);
    bool ok = true;
    int32_t made = t->n;
    // From the last name to the first; a name matched to nothing starts now.
    for (int32_t i = m->nanims - 1; ok && i >= 0; i--) {
        FAnimation *an = &m->anims[i];
        const flow_name_t *name = &an->timing.name;
        FRecord *r = an->record;
        if (r == NULL) {
            r = os64_calloc(1, sizeof(*r));
            char *copy = os64_malloc(name->len > 0 ? name->len : 1);
            bool room = t->n < t->cap;
            if (!room && r != NULL && copy != NULL) {
                int32_t grown = t->cap > 0 ? t->cap * 2 : 16;
                FRecord **more = os64_realloc(t->records, (size_t)grown * sizeof(*more));
                if (more != NULL) {
                    t->records = more;
                    t->cap = grown;
                    room = true;
                }
            }
            if (r == NULL || copy == NULL || !room) {
                os64_free(r);
                os64_free(copy);
                ok = false;
                break;
            }
            os64_memcpy(copy, name->text, name->len);
            r->node = n;
            r->name = copy;
            r->len = name->len;
            r->now.start = now;
            r->now.paused = an->timing.paused;
            r->made = t->layout;
            os64_html_hold(t->doc, n);
            t->records[t->n++] = r;
        }
        if (r->now.seen != t->layout)
            r->was = r->now;
        r->now.seen = t->layout;
        r->now.place = i;
        if (an->timing.paused && !r->now.paused) {
            r->now.hold = now - r->now.start;
            r->now.paused = true;
        } else if (!an->timing.paused && r->now.paused) {
            r->now.start = now - r->now.hold;
            r->now.paused = false;
        }
        an->record = r;
    }
    // The records made here join the element's chain after those it had,
    // so the chain keeps the timeline's order without the timeline walked.
    for (int32_t j = made; j < t->n; j++) {
        t->records[j]->next = NULL;
        t->records[j]->after = tail;
        if (tail != NULL)
            tail->next = t->records[j];
        else if (!f_map_put(&t->by_node, n, t->records[j]))
            return false;
        tail = t->records[j];
    }
    return ok;
}

void f_timeline_undo(flow_timeline_t *t)
{
    if (t == NULL)
        return;
    // The records it made end their elements' chains: the record each
    // first one was chained after ends its chain again, or its element
    // goes back to none — a key's value replaced, which cannot fail. Then
    // they go, and what it changed in the rest is put back.
    for (int32_t i = t->base; i < t->n; i++) {
        FRecord *r = t->records[i];
        if (r->after != NULL && r->after->made != t->layout)
            r->after->next = NULL;
        else if (r->after == NULL && f_map_get(&t->by_node, r->node) == r)
            (void)f_map_put(&t->by_node, r->node, NULL);
    }
    for (int32_t i = t->base; i < t->n; i++)
        record_free(t, t->records[i]);
    t->n = t->base;
    for (int32_t i = 0; i < t->n; i++)
        if (t->records[i]->now.seen == t->layout)
            t->records[i]->now = t->records[i]->was;
}

void f_timeline_end(flow_timeline_t *t, bool complete)
{
    if (t == NULL || !complete)
        return;
    // The map over the records kept, and each one's next in its element's
    // chain, are made before anything changes: with no room for them
    // nothing is dropped, and a later complete layout drops what this one
    // would have. A dropped record's node is no key of the new map, since
    // the node may be reused for another element once it is let go.
    FMap map = {0};
    FRecord **next = t->n > 0 ? os64_malloc((size_t)t->n * sizeof(*next)) : NULL;
    bool room = t->n == 0 || next != NULL;
    for (int32_t i = t->n - 1; room && i >= 0; i--) {
        FRecord *r = t->records[i];
        if (r->now.seen != t->layout)
            continue;
        next[i] = f_map_get(&map, r->node);
        room = f_map_put(&map, r->node, r);
    }
    if (!room) {
        os64_free(next);
        f_map_free(&map);
        return;
    }
    int32_t kept = 0;
    for (int32_t i = 0; i < t->n; i++) {
        FRecord *r = t->records[i];
        if (r->now.seen == t->layout) {
            r->next = next[i];
            t->records[kept++] = r;
        } else {
            record_free(t, r);
        }
    }
    t->n = kept;
    os64_free(next);
    f_map_free(&t->by_node);
    t->by_node = map;
}

// ── Matrices (Transforms 1 § 11, 2 § 13) ────────────────────────────────

// Column-major, as CSS's matrix3d() is: m[col * 4 + row].
static void mat_identity(double m[16])
{
    os64_memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mat_mul(double out[16], const double a[16], const double b[16])
{
    double r[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            double s = 0;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
    os64_memcpy(out, r, sizeof(r));
}

// A 2D matrix: its third row and column, and its perspective row, are the
// identity's.
static bool is_2d(const double m[16])
{
    return m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 &&
           m[10] == 1 && m[11] == 0 && m[14] == 0 && m[15] == 1;
}

// A matrix decomposed (Transforms 2 § 13.1.1's parts): its perspective,
// translation, scale, skew, and its rotation as a quaternion.
//
// The matrices are mixed as Chrome mixes them (Chromium's
// DecomposeTransform and ComposeTransform), which is what a page is
// written against and what the proof checks: a 2D matrix is decomposed as
// a translation, a scale, a shear and a turn about z; a 3D one by
// Transforms 2 § 13.1.1; the parts mixed — the turns slerped the shorter
// way — and composed as perspective · translation · rotation · skew ·
// scale. Where a 2D matrix holds no shear this is Transforms 1 § 11.2's
// answer (scaleX(-1) against rotate(90deg)); where it does, it is not
// (PILE3.md § Animations, What Chrome draws).
typedef struct {
    double persp[4], tr[3], sc[3], sk[3], q[4];
} D3;

static bool decompose_2d(const double m[16], D3 *d)
{
    double m11 = m[0], m12 = m[1], m21 = m[4], m22 = m[5];
    double det = m11 * m22 - m12 * m21;
    if (det == 0)
        return false;           // cannot be undone: the pair is discrete
    os64_memset(d, 0, sizeof(*d));
    d->persp[3] = 1;
    d->tr[0] = m[12];
    d->tr[1] = m[13];
    d->sc[0] = d->sc[1] = d->sc[2] = 1;
    // A flip, on the axis whose diagonal is the smaller.
    if (det < 0) {
        if (m11 < m22)
            d->sc[0] = -1;
        else
            d->sc[1] = -1;
    }
    d->sc[0] *= sqrt(m11 * m11 + m12 * m12);
    m11 /= d->sc[0];
    m12 /= d->sc[0];
    double shear = m11 * m21 + m12 * m22;
    m21 -= m11 * shear;
    m22 -= m12 * shear;
    d->sc[1] *= sqrt(m21 * m21 + m22 * m22);
    d->sk[0] = shear / d->sc[1];
    double angle = atan2(m12, m11);
    d->q[2] = sin(0.5 * angle);
    d->q[3] = cos(0.5 * angle);
    return true;
}

static double det3(double a, double b, double c, double d, double e, double f, double g,
                   double h, double i)
{
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

static double det4(const double m[16])
{
    // Along the first column (m[col * 4 + row]).
    double r = 0;
    for (int k = 0; k < 4; k++) {
        double s[9];
        int n = 0;
        for (int col = 1; col < 4; col++)
            for (int row = 0; row < 4; row++)
                if (row != k)
                    s[n++] = m[col * 4 + row];
        double minor = det3(s[0], s[3], s[6], s[1], s[4], s[7], s[2], s[5], s[8]);
        r += (k % 2 ? -1 : 1) * m[k] * minor;
    }
    return r;
}

static bool invert4(const double m[16], double out[16])
{
    double d = det4(m);
    if (d == 0 || !isfinite(d))
        return false;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            // The cofactor of (row, col), transposed.
            double s[9];
            int n = 0;
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    if (c != row && r != col)
                        s[n++] = m[c * 4 + r];
            double minor = det3(s[0], s[3], s[6], s[1], s[4], s[7], s[2], s[5], s[8]);
            out[col * 4 + row] = ((row + col) % 2 ? -1 : 1) * minor / d;
        }
    return true;
}

// A vector's length, taken by its largest part first, so that neither a
// part past sqrt(DBL_MAX) overflows the squares nor a tiny one underflows
// them (rotation() makes its axis a unit the same way).
static double len3(const double v[3])
{
    double k = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));
    if (!(k > 0) || !isfinite(k))
        return k;
    double x = v[0] / k, y = v[1] / k, z = v[2] / k;
    return k * sqrt(x * x + y * y + z * z);
}

static double dot3(const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// a * as + b * bs.
static void combine(double out[3], const double a[3], const double b[3], double as, double bs)
{
    for (int i = 0; i < 3; i++)
        out[i] = as * a[i] + bs * b[i];
}

static bool decompose_3d(const double in[16], D3 *d)
{
    double m[16];
    if (in[15] == 0)
        return false;
    for (int i = 0; i < 16; i++)
        m[i] = in[i] / in[15];
    double pm[16];
    os64_memcpy(pm, m, sizeof(pm));
    pm[3] = pm[7] = pm[11] = 0;
    pm[15] = 1;
    if (det4(pm) == 0)
        return false;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0) {
        double rhs[4] = {m[3], m[7], m[11], m[15]}, inv[16];
        if (!invert4(pm, inv))
            return false;
        // rhs times the transposed inverse: the inverse's columns as rows.
        for (int i = 0; i < 4; i++)
            d->persp[i] = rhs[0] * inv[i * 4 + 0] + rhs[1] * inv[i * 4 + 1] +
                          rhs[2] * inv[i * 4 + 2] + rhs[3] * inv[i * 4 + 3];
    } else {
        d->persp[0] = d->persp[1] = d->persp[2] = 0;
        d->persp[3] = 1;
    }
    for (int i = 0; i < 3; i++)
        d->tr[i] = m[12 + i];
    double row[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            row[i][j] = m[i * 4 + j];
    d->sc[0] = len3(row[0]);
    if (d->sc[0] == 0)
        return false;
    for (int j = 0; j < 3; j++)
        row[0][j] /= d->sc[0];
    d->sk[0] = dot3(row[0], row[1]);
    combine(row[1], row[1], row[0], 1, -d->sk[0]);
    d->sc[1] = len3(row[1]);
    if (d->sc[1] == 0)
        return false;
    for (int j = 0; j < 3; j++)
        row[1][j] /= d->sc[1];
    d->sk[0] /= d->sc[1];
    d->sk[1] = dot3(row[0], row[2]);
    combine(row[2], row[2], row[0], 1, -d->sk[1]);
    d->sk[2] = dot3(row[1], row[2]);
    combine(row[2], row[2], row[1], 1, -d->sk[2]);
    d->sc[2] = len3(row[2]);
    if (d->sc[2] == 0)
        return false;
    for (int j = 0; j < 3; j++)
        row[2][j] /= d->sc[2];
    d->sk[1] /= d->sc[2];
    d->sk[2] /= d->sc[2];
    // A coordinate system flip: negate the scale and the rows.
    double pdum3[3] = {row[1][1] * row[2][2] - row[1][2] * row[2][1],
                       row[1][2] * row[2][0] - row[1][0] * row[2][2],
                       row[1][0] * row[2][1] - row[1][1] * row[2][0]};
    if (dot3(row[0], pdum3) < 0)
        for (int i = 0; i < 3; i++) {
            d->sc[i] = -d->sc[i];
            for (int j = 0; j < 3; j++)
                row[i][j] = -row[i][j];
        }
    d->q[0] = 0.5 * sqrt(fmax(1 + row[0][0] - row[1][1] - row[2][2], 0));
    d->q[1] = 0.5 * sqrt(fmax(1 - row[0][0] + row[1][1] - row[2][2], 0));
    d->q[2] = 0.5 * sqrt(fmax(1 - row[0][0] - row[1][1] + row[2][2], 0));
    d->q[3] = 0.5 * sqrt(fmax(1 + row[0][0] + row[1][1] + row[2][2], 0));
    if (row[2][1] > row[1][2])
        d->q[0] = -d->q[0];
    if (row[0][2] > row[2][0])
        d->q[1] = -d->q[1];
    if (row[1][0] > row[0][1])
        d->q[2] = -d->q[2];
    return true;
}

// The parts composed: perspective · translation · rotation · skew · scale,
// the rotation's matrix the quaternion's (x, y, z, w).
static void compose(const D3 *d, double m[16])
{
    double x = d->q[0], y = d->q[1], z = d->q[2], w = d->q[3], part[16];
    mat_identity(m);
    for (int i = 0; i < 4; i++)
        m[i * 4 + 3] = d->persp[i];
    mat_identity(part);
    part[12] = d->tr[0];
    part[13] = d->tr[1];
    part[14] = d->tr[2];
    mat_mul(m, m, part);
    mat_identity(part);
    part[0] = 1 - 2 * (y * y + z * z);
    part[4] = 2 * (x * y - z * w);
    part[8] = 2 * (x * z + y * w);
    part[1] = 2 * (x * y + z * w);
    part[5] = 1 - 2 * (x * x + z * z);
    part[9] = 2 * (y * z - x * w);
    part[2] = 2 * (x * z - y * w);
    part[6] = 2 * (y * z + x * w);
    part[10] = 1 - 2 * (x * x + y * y);
    mat_mul(m, m, part);
    mat_identity(part);
    part[4] = d->sk[0];
    part[8] = d->sk[1];
    part[9] = d->sk[2];
    mat_mul(m, m, part);
    mat_identity(part);
    part[0] = d->sc[0];
    part[5] = d->sc[1];
    part[10] = d->sc[2];
    mat_mul(m, m, part);
}

// The quaternions slerped from `a` to `b`, the shorter way round: one is
// turned to the other's side when their half angle passes 90 degrees, and
// two that share their axis and angle (within 1e-5) are `a`.
static void slerp(const double a[4], const double b[4], double t, double out[4])
{
    double from[4] = {a[0], a[1], a[2], a[3]};
    double c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (c < 0) {
        for (int i = 0; i < 4; i++)
            from[i] = -from[i];
        c = -c;
    }
    c = fmin(c, 1.0);
    double s = sqrt(1 - c * c);
    if (s < 1e-5) {
        os64_memcpy(out, a, 4 * sizeof(double));
        return;
    }
    double half = acos(c), wa = sin((1 - t) * half) / s, wb = sin(t * half) / s;
    for (int i = 0; i < 4; i++)
        out[i] = wa * from[i] + wb * b[i];
}

static double lerp(double a, double b, double t);

bool f_matrix_mix(const double a[16], const double b[16], double t, double out[16])
{
    D3 da, db, r;
    if (!(is_2d(a) ? decompose_2d(a, &da) : decompose_3d(a, &da)) ||
        !(is_2d(b) ? decompose_2d(b, &db) : decompose_3d(b, &db)))
        return false;
    for (int i = 0; i < 4; i++)
        r.persp[i] = lerp(da.persp[i], db.persp[i], t);
    for (int i = 0; i < 3; i++) {
        r.tr[i] = lerp(da.tr[i], db.tr[i], t);
        r.sc[i] = lerp(da.sc[i], db.sc[i], t);
        r.sk[i] = lerp(da.sk[i], db.sk[i], t);
    }
    slerp(da.q, db.q, t, r.q);
    compose(&r, out);
    return true;
}

// ── A transform function's matrix ───────────────────────────────────────

// An angle in radians, reduced by whole turns in its own unit first (fmod
// is exact): rotate(1e308deg) times pi would be past a double, and a turn
// more or less is the same turn. Radians stay as written; cos and sin
// reduce them themselves.
static double radians(double v, flow_angle_unit_t unit)
{
    switch (unit) {
    case FLOW_ANGLE_GRAD: return fmod(v, 400) * kPi / 200;
    case FLOW_ANGLE_RAD: return v;
    case FLOW_ANGLE_TURN: return fmod(v, 1) * 2 * kPi;
    default: return fmod(v, 360) * kPi / 180;
    }
}

// The quarter turns `v` is, when it is written as a whole number of them —
// 90deg, 100grad, 0.25turn, each tested exactly — so that its sine and
// cosine are exact, as a double's cos(pi / 2), 6e-17, is not: 0 to 3, or
// -1 for any other angle, which is worked out as written.
static int quarters(double n, flow_angle_unit_t unit)
{
    double per = unit == FLOW_ANGLE_GRAD ? 100 : unit == FLOW_ANGLE_TURN ? 0.25
                 : unit == FLOW_ANGLE_RAD ? 0 : 90;
    if (!isfinite(n) || (per == 0 ? n != 0 : fmod(n, per) != 0))
        return -1;
    if (per == 0)
        return 0;
    // Reduced by a whole turn before it is divided, so a huge angle
    // (1e308turn) never overflows on the way to its quarter.
    double q = fmod(n, 4 * per) / per;
    return (int)(q < 0 ? q + 4 : q);
}

// A turn by the angle about the axis (x, y, z), into `m`: Rodrigues'
// rotation, which is Transforms 2 § 12.2's rotate3d() — clockwise on the
// glass about z, since y points down — with the axis made a unit vector.
// An axis of no length turns nothing. A whole number of quarter turns is
// exact, so rotateX(90deg) is edge-on: collapsed, drawn and hit nowhere.
static void rotation(double m[16], double x, double y, double z, double angle,
                     flow_angle_unit_t unit)
{
    // Made a unit vector scaled by its largest part first, so that neither
    // rotate3d(1e308, 0, 0, a) overflows nor a tiny axis underflows to none.
    double k = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    if (!(k > 0) || !isfinite(k))
        return;
    x /= k;
    y /= k;
    z /= k;
    double len = sqrt(x * x + y * y + z * z);
    x /= len;
    y /= len;
    z /= len;
    static const double kCos[4] = {1, 0, -1, 0}, kSin[4] = {0, 1, 0, -1};
    int q = quarters(angle, unit);
    double r = radians(angle, unit);
    double c = q >= 0 ? kCos[q] : cos(r), s = q >= 0 ? kSin[q] : sin(r);
    double t = 1 - c;
    m[0] = c + x * x * t;
    m[1] = y * x * t + z * s;
    m[2] = z * x * t - y * s;
    m[4] = x * y * t - z * s;
    m[5] = c + y * y * t;
    m[6] = z * y * t + x * s;
    m[8] = x * z * t + y * s;
    m[9] = y * z * t - x * s;
    m[10] = c + z * z * t;
}

void f_transform_fn_matrix(flow_transform_t *f)
{
    double *m = f->m;
    switch (f->fn) {
    case FLOW_FN_TRANSLATE:
        mat_identity(m);
        m[14] = f->a[2];
        break;
    case FLOW_FN_SCALE:
        mat_identity(m);
        m[0] = f->a[0];
        m[5] = f->a[1];
        m[10] = f->a[2];
        break;
    case FLOW_FN_ROTATE:
        mat_identity(m);
        rotation(m, f->a[0], f->a[1], f->a[2], f->a[3], f->unit);
        break;
    case FLOW_FN_SKEW:
        mat_identity(m);
        m[4] = tan(radians(f->a[0], f->unit));
        m[1] = tan(radians(f->a[1], f->unit));
        break;
    case FLOW_FN_SKEW_X:
        mat_identity(m);
        m[4] = tan(radians(f->a[0], f->unit));
        break;
    case FLOW_FN_SKEW_Y:
        mat_identity(m);
        m[1] = tan(radians(f->a[0], f->unit));
        break;
    case FLOW_FN_PERSPECTIVE:
        // a[0] is held to a pixel at least where it is read (style.c);
        // 0 is `none`.
        mat_identity(m);
        if (f->a[0] > 0)
            m[11] = -1 / f->a[0];
        break;
    case FLOW_FN_MATRIX:
    case FLOW_FN_BLEND:
        break;                  // its own, or the box's to make
    }
}

// ── Interpolation, by the property (Web Animations 1 § 5.2, Appendix A) ─

typedef enum { MIX_CONTINUOUS, MIX_DISCRETE } MixKind;

// a + (b - a) t, as Chromium blends: at t = 0 exactly a, at t = 1 exactly
// b, and a value mixed with itself exactly itself at any t — which
// (1 - t) a + t b is not when t is huge and the two products cancel. In
// long double and held, as the easing that hands it `t` is.
static double lerp(double a, double b, double t)
{
    if (t == 1)
        return b;
    return held(a + ((long double)b - a) * t);
}

// Rounded half away from zero, held to an int32_t.
static int32_t round_i32(double v)
{
    if (!(v == v))
        return 0;
    v = v < 0 ? -floor(-v + 0.5) : floor(v + 0.5);
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

// An integer's value, rounded half up as Chrome rounds 2.5 and -2.5 (Values
// 4 § 5.2.1), held to an int32_t: an easing that overshoots carries it past
// either end.
static int32_t half_up_i32(double v)
{
    return round_i32(floor(v + 0.5));
}

// A length (flow_length_t): a PX and a PERCENT with its offset are each a
// percentage and a length, mixed part by part (Values 4 § 5.6.1); a
// keyword is discrete.
static bool mix_length(flow_length_t a, flow_length_t b, double t, flow_length_t *out)
{
    bool la = a.kind == FLOW_LENGTH_PX || a.kind == FLOW_LENGTH_PERCENT;
    bool lb = b.kind == FLOW_LENGTH_PX || b.kind == FLOW_LENGTH_PERCENT;
    if (!la || !lb)
        return false;
    if (out == NULL)
        return true;
    double pa = a.kind == FLOW_LENGTH_PERCENT ? a.value : 0;
    double pb = b.kind == FLOW_LENGTH_PERCENT ? b.value : 0;
    double xa = a.kind == FLOW_LENGTH_PERCENT ? a.offset : a.value;
    double xb = b.kind == FLOW_LENGTH_PERCENT ? b.offset : b.value;
    if (a.kind == FLOW_LENGTH_PX && b.kind == FLOW_LENGTH_PX)
        *out = (flow_length_t){FLOW_LENGTH_PX, round_i32(lerp(xa, xb, t)), 0};
    else
        *out = (flow_length_t){FLOW_LENGTH_PERCENT, round_i32(lerp(pa, pb, t)),
                               round_i32(lerp(xa, xb, t))};
    return true;
}

// A length its property keeps at 0 or more — a padding, a gap, a size, a
// radius — held there when an overshooting easing carries it below, as
// Chrome holds it. A length or a percentage that is below 0 is 0, and so
// is a mix of the two whose parts both are; one whose parts differ in
// sign depends on what the percentage is of, and is held where it is
// used, as a calc() is (BROWSER_DEBTS: padding is not, yet).
static void hold_nonneg(flow_length_t *l)
{
    if (l->kind == FLOW_LENGTH_PX && l->value < 0)
        l->value = 0;
    else if (l->kind == FLOW_LENGTH_PERCENT && l->value <= 0 && l->offset <= 0)
        l->value = l->offset = 0;
}

// The same, mixed and held (mix_length, hold_nonneg).
static bool mix_nonneg(flow_length_t a, flow_length_t b, double t, flow_length_t *out)
{
    if (!mix_length(a, b, t, out))
        return false;
    if (out != NULL)
        hold_nonneg(out);
    return true;
}

// A colour (0xTTRRGGBB), premultiplied by its alpha, each channel in
// doubles, divided back and rounded half up once (Color 4 § 13.4).
static uint32_t mix_colour(uint32_t a, uint32_t b, double t)
{
    double aa = flow_alpha(a) / 255.0, ab = flow_alpha(b) / 255.0;
    double alpha = fmin(fmax(lerp(aa, ab, t), 0), 1);
    uint8_t ta = (uint8_t)(255 - round_i32(alpha * 255));
    if (ta == 255)
        return 0xFF000000u;
    uint32_t r = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        double ca = ((a >> shift) & 0xFF) * aa, cb = ((b >> shift) & 0xFF) * ab;
        double c = lerp(ca, cb, t) / alpha;
        int32_t v = (int32_t)floor(fmin(fmax(c, 0), 255) + 0.5);
        r |= (uint32_t)(v > 255 ? 255 : v) << shift;
    }
    return (uint32_t)ta << 24 | r;
}

// A colour field's currentColor mark, mixed: currentColor when both ends
// are — so it follows the element's colour as that colour moves, whatever
// the element's own field is (f_motion_style resolves it) — and the mixed
// colour otherwise.
static void hand_current(flow_style_t *d, const flow_style_t *a, const flow_style_t *b,
                         uint8_t bit)
{
    if (a->current_colours & b->current_colours & bit)
        d->current_colours |= bit;
    else
        d->current_colours &= (uint8_t)~bit;
}

// One shadow with another of the same `inset`.
static flow_shadow_t mix_shadow(const flow_shadow_t *a, const flow_shadow_t *b, double t)
{
    flow_shadow_t r = *a;
    r.x = round_i32(lerp(a->x, b->x, t));
    r.y = round_i32(lerp(a->y, b->y, t));
    r.blur = round_i32(fmax(lerp(a->blur, b->blur, t), 0));
    r.spread = round_i32(lerp(a->spread, b->spread, t));
    r.colour = mix_colour(a->colour, b->colour, t);
    // Both currentColor: the mix is currentColor, the colour the element
    // has once every animation is laid over (the painter resolves it).
    r.current = a->current && b->current;
    return r;
}

// Shadow lists (Appendix A): the shorter padded with transparent shadows
// of no size matching the longer's `inset`; a pair that differs in
// `inset` makes the list discrete. A `current` shadow's colour is the
// element's (`colour_a`, `colour_b`), resolved before it is mixed.
static bool mix_shadows(const flow_shadow_t *a, int32_t na, const flow_shadow_t *b, int32_t nb,
                        uint32_t colour_a, uint32_t colour_b, double t, flow_shadow_t **out,
                        int32_t *nout, FAlloc alloc, void *ctx)
{
    int32_t n = na > nb ? na : nb;
    for (int32_t i = 0; i < n && i < na && i < nb; i++)
        if (a[i].inset != b[i].inset)
            return false;
    if (out == NULL)
        return true;
    flow_shadow_t *list = n > 0 ? alloc(ctx, (size_t)n * sizeof(*list)) : NULL;
    if (n > 0 && list == NULL)
        return false;
    for (int32_t i = 0; i < n; i++) {
        flow_shadow_t pa, pb;
        const flow_shadow_t *other = i < na ? &a[i] : &b[i];
        flow_shadow_t none = {0, 0, 0, 0, 0xFF000000u, other->inset, false};
        pa = i < na ? a[i] : none;
        pb = i < nb ? b[i] : none;
        if (pa.current)
            pa.colour = colour_a;
        if (pb.current)
            pb.colour = colour_b;
        list[i] = mix_shadow(&pa, &pb, t);
    }
    *out = list;
    *nout = n;
    return true;
}

static int32_t gcd(int32_t a, int32_t b)
{
    while (b != 0) {
        int32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

// Repeatable lists (§ 5.2): repeated to their least common multiple, here
// held to a thousand items, past which they are discrete.
static int32_t lcm_items(int32_t a, int32_t b)
{
    if (a <= 0 || b <= 0)
        return -1;
    int64_t l = (int64_t)a / gcd(a, b) * b;
    return l > 1000 ? -1 : (int32_t)l;
}

static bool mix_lengths(const flow_length_t *a, int32_t na, const flow_length_t *b, int32_t nb,
                        double t, const flow_length_t **out, int32_t *nout, FAlloc alloc,
                        void *ctx)
{
    int32_t n = lcm_items(na, nb);
    if (n < 0)
        return false;
    for (int32_t i = 0; i < n; i++)
        if (!mix_length(a[i % na], b[i % nb], t, NULL))
            return false;
    if (out == NULL)
        return true;
    flow_length_t *list = alloc(ctx, (size_t)n * sizeof(*list));
    if (list == NULL)
        return false;
    for (int32_t i = 0; i < n; i++)
        (void)mix_length(a[i % na], b[i % nb], t, &list[i]);
    *out = list;
    *nout = n;
    return true;
}

static bool mix_sizes(const flow_bg_size_t *a, int32_t na, const flow_bg_size_t *b, int32_t nb,
                      double t, const flow_bg_size_t **out, int32_t *nout, FAlloc alloc,
                      void *ctx)
{
    int32_t n = lcm_items(na, nb);
    if (n < 0)
        return false;
    for (int32_t i = 0; i < n; i++) {
        const flow_bg_size_t *x = &a[i % na], *y = &b[i % nb];
        if (x->fit != FLOW_FIT_LENGTHS || y->fit != FLOW_FIT_LENGTHS)
            return false;
        for (int k = 0; k < 2; k++)
            if ((x->size[k].kind == FLOW_LENGTH_AUTO) != (y->size[k].kind == FLOW_LENGTH_AUTO) ||
                (x->size[k].kind != FLOW_LENGTH_AUTO && !mix_length(x->size[k], y->size[k], t, NULL)))
                return false;
    }
    if (out == NULL)
        return true;
    flow_bg_size_t *list = alloc(ctx, (size_t)n * sizeof(*list));
    if (list == NULL)
        return false;
    for (int32_t i = 0; i < n; i++) {
        list[i] = a[i % na];
        for (int k = 0; k < 2; k++)
            if (list[i].size[k].kind != FLOW_LENGTH_AUTO)
                (void)mix_nonneg(a[i % na].size[k], b[i % nb].size[k], t, &list[i].size[k]);
    }
    *out = list;
    *nout = n;
    return true;
}

// ── Transforms (Transforms 1 § 9, § 10; 2 § 14) ────────────────────────

// The identity of `f`'s primitive: what pads a shorter list.
static flow_transform_t identity_of(const flow_transform_t *f)
{
    flow_transform_t r;
    os64_memset(&r, 0, sizeof(r));
    r.fn = f->fn;
    r.three_d = f->three_d;
    r.unit = f->unit;
    r.x = r.y = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
    switch (f->fn) {
    case FLOW_FN_SCALE: r.a[0] = r.a[1] = r.a[2] = 1; break;
    case FLOW_FN_ROTATE:
        r.a[0] = f->a[0];
        r.a[1] = f->a[1];
        r.a[2] = f->a[2];
        break;
    default: break;
    }
    f_transform_fn_matrix(&r);
    if (f->fn == FLOW_FN_MATRIX || f->fn == FLOW_FN_BLEND) {
        r.fn = FLOW_FN_MATRIX;
        mat_identity(r.m);
    }
    return r;
}

// An angle in another unit, in degrees.
static double degrees(double v, flow_angle_unit_t unit)
{
    switch (unit) {
    case FLOW_ANGLE_GRAD: return v * 9 / 10;
    case FLOW_ANGLE_RAD: return v * 180 / kPi;
    case FLOW_ANGLE_TURN: return v * 360;
    default: return v;
    }
}

// Two rotations' axes, made unit vectors: equal, or either angle none, and
// they mix by their angle about the turned one's axis (Transforms 2 § 14).
static bool same_axis(const flow_transform_t *a, const flow_transform_t *b, double axis[3])
{
    double ua[3] = {a->a[0], a->a[1], a->a[2]}, ub[3] = {b->a[0], b->a[1], b->a[2]};
    double la = len3(ua), lb = len3(ub);
    if (a->a[3] == 0 && b->a[3] == 0) {
        axis[0] = axis[1] = 0;
        axis[2] = 1;
        return true;
    }
    if (a->a[3] == 0 || la == 0) {
        os64_memcpy(axis, ub, sizeof(ub));
        return true;
    }
    if (b->a[3] == 0 || lb == 0) {
        os64_memcpy(axis, ua, sizeof(ua));
        return true;
    }
    for (int i = 0; i < 3; i++)
        if (ua[i] / la != ub[i] / lb)
            return false;
    os64_memcpy(axis, ua, sizeof(ua));
    return true;
}

// Whether a pair mixes by its arguments: one primitive (a skew only with
// its own name), and two rotations about one axis; matrix() and
// perspective() mix only as matrices (§ 14).
static bool pairs(const flow_transform_t *a, const flow_transform_t *b)
{
    double axis[3];
    if (a->fn != b->fn || a->fn == FLOW_FN_MATRIX || a->fn == FLOW_FN_PERSPECTIVE ||
        a->fn == FLOW_FN_BLEND)
        return false;
    return a->fn != FLOW_FN_ROTATE || same_axis(a, b, axis);
}

static flow_transform_t mix_fn(const flow_transform_t *a, const flow_transform_t *b, double t)
{
    flow_transform_t r = *a;
    r.three_d = a->three_d || b->three_d;
    r.blend = NULL;
    switch (a->fn) {
    case FLOW_FN_TRANSLATE:
        (void)mix_length(a->x, b->x, t, &r.x);
        (void)mix_length(a->y, b->y, t, &r.y);
        r.a[2] = lerp(a->a[2], b->a[2], t);
        break;
    case FLOW_FN_SCALE:
        for (int i = 0; i < 3; i++)
            r.a[i] = lerp(a->a[i], b->a[i], t);
        break;
    case FLOW_FN_ROTATE: {
        double axis[3];
        (void)same_axis(a, b, axis);
        os64_memcpy(r.a, axis, sizeof(axis));
        if (a->unit == b->unit) {
            r.a[3] = lerp(a->a[3], b->a[3], t);
        } else {
            r.unit = FLOW_ANGLE_DEG;
            r.a[3] = lerp(degrees(a->a[3], a->unit), degrees(b->a[3], b->unit), t);
        }
        break;
    }
    default: {
        // The skews: their angles, in one unit.
        int n = a->fn == FLOW_FN_SKEW ? 2 : 1;
        for (int i = 0; i < n; i++) {
            if (a->unit == b->unit) {
                r.a[i] = lerp(a->a[i], b->a[i], t);
            } else {
                r.unit = FLOW_ANGLE_DEG;
                r.a[i] = lerp(degrees(a->a[i], a->unit), degrees(b->a[i], b->unit), t);
            }
        }
        break;
    }
    }
    f_transform_fn_matrix(&r);
    return r;
}

// Two transform lists (Transforms 1 § 9): padded with identities, then
// pair by pair while they pair; a pair that mixes only as matrices, and
// the tails from the first pair that does not, are a FLOW_FN_BLEND each,
// multiplied out with the box (f_transform_matrix).
static bool mix_transforms(const flow_transform_t *a, int32_t na, const flow_transform_t *b,
                           int32_t nb, double t, const flow_transform_t **out, int32_t *nout,
                           FAlloc alloc, void *ctx)
{
    int32_t n = na > nb ? na : nb;
    if (n == 0) {
        if (out != NULL) {
            *out = NULL;
            *nout = 0;
        }
        return true;
    }
    if (out == NULL)
        return true;
    flow_transform_t *pa = alloc(ctx, (size_t)n * 2 * sizeof(*pa));
    flow_transform_t *list = alloc(ctx, (size_t)n * sizeof(*list));
    if (pa == NULL || list == NULL)
        return false;
    flow_transform_t *pb = pa + n;
    for (int32_t i = 0; i < n; i++) {
        pa[i] = i < na ? a[i] : identity_of(&b[i]);
        pb[i] = i < nb ? b[i] : identity_of(&a[i]);
    }
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        if (pairs(&pa[i], &pb[i])) {
            list[k++] = mix_fn(&pa[i], &pb[i], t);
            continue;
        }
        // One primitive that mixes only as matrices (§ 14) is a pair of
        // its own; anything else ends the pairing, its tails the rest.
        bool alone = pa[i].fn == pb[i].fn && (pa[i].fn == FLOW_FN_MATRIX ||
                                              pa[i].fn == FLOW_FN_PERSPECTIVE ||
                                              pa[i].fn == FLOW_FN_ROTATE);
        int32_t to = alone ? i + 1 : n;
        flow_transform_blend_t *bl = alloc(ctx, sizeof(*bl));
        if (bl == NULL)
            return false;
        *bl = (flow_transform_blend_t){&pa[i], &pb[i], to - i, to - i, t};
        flow_transform_t f;
        os64_memset(&f, 0, sizeof(f));
        f.fn = FLOW_FN_BLEND;
        f.blend = bl;
        f.x = f.y = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
        mat_identity(f.m);
        list[k++] = f;
        if (!alone)
            break;
    }
    *out = list;
    *nout = k;
    return true;
}

// An individual property (`translate`, `rotate`, `scale`): one function,
// NULL for none, which mixes as its identity.
static bool mix_individual(const flow_transform_t *a, const flow_transform_t *b, double t,
                           const flow_transform_t **out, FAlloc alloc, void *ctx)
{
    if (a == NULL && b == NULL) {
        if (out != NULL)
            *out = NULL;
        return true;
    }
    if (out == NULL)
        return true;
    flow_transform_t ia = a != NULL ? *a : identity_of(b), ib = b != NULL ? *b : identity_of(a);
    const flow_transform_t *list;
    int32_t n;
    if (!mix_transforms(&ia, 1, &ib, 1, t, &list, &n, alloc, ctx) || n != 1)
        return false;
    *out = list;
    return true;
}

// ── One property's value ────────────────────────────────────────────────

// Takes property `p`'s computed value from `s` into `d`: the discrete
// half of mixing.
static void copy_prop(garb_prop_t p, flow_style_t *d, const flow_style_t *s)
{
    int i;
    switch (p) {
    case GARB_COLOR: d->color = s->color; break;
    case GARB_TEXT_FILL_COLOR:
        d->fill = s->fill;
        d->current_colours = (uint8_t)((d->current_colours & ~FLOW_CURRENT_FILL) |
                                       (s->current_colours & FLOW_CURRENT_FILL));
        break;
    case GARB_BACKGROUND_COLOR:
        d->has_background = s->has_background;
        d->background = s->background;
        d->current_colours = (uint8_t)((d->current_colours & ~FLOW_CURRENT_BACKGROUND) |
                                       (s->current_colours & FLOW_CURRENT_BACKGROUND));
        break;
    case GARB_BACKGROUND_IMAGE:
        d->backgrounds.image = s->backgrounds.image;
        d->backgrounds.nimage = s->backgrounds.nimage;
        break;
    case GARB_BACKGROUND_REPEAT:
        d->backgrounds.repeat = s->backgrounds.repeat;
        d->backgrounds.nrepeat = s->backgrounds.nrepeat;
        break;
    case GARB_BACKGROUND_POSITION_X:
        d->backgrounds.x = s->backgrounds.x;
        d->backgrounds.nx = s->backgrounds.nx;
        break;
    case GARB_BACKGROUND_POSITION_Y:
        d->backgrounds.y = s->backgrounds.y;
        d->backgrounds.ny = s->backgrounds.ny;
        break;
    case GARB_BACKGROUND_SIZE:
        d->backgrounds.size = s->backgrounds.size;
        d->backgrounds.nsize = s->backgrounds.nsize;
        break;
    case GARB_BACKGROUND_ORIGIN:
        d->backgrounds.origin = s->backgrounds.origin;
        d->backgrounds.norigin = s->backgrounds.norigin;
        break;
    case GARB_BACKGROUND_CLIP:
        d->backgrounds.clip = s->backgrounds.clip;
        d->backgrounds.nclip = s->backgrounds.nclip;
        break;
    case GARB_MARGIN_TOP: case GARB_MARGIN_RIGHT: case GARB_MARGIN_BOTTOM: case GARB_MARGIN_LEFT:
        i = p - GARB_MARGIN_TOP;
        d->margin[i] = s->margin[i];
        break;
    case GARB_PADDING_TOP: case GARB_PADDING_RIGHT: case GARB_PADDING_BOTTOM:
    case GARB_PADDING_LEFT:
        i = p - GARB_PADDING_TOP;
        d->padding[i] = s->padding[i];
        break;
    case GARB_BORDER_TOP_WIDTH: case GARB_BORDER_RIGHT_WIDTH: case GARB_BORDER_BOTTOM_WIDTH:
    case GARB_BORDER_LEFT_WIDTH:
        i = p - GARB_BORDER_TOP_WIDTH;
        d->border_width[i] = s->border_width[i];
        break;
    // A side's style alone: its width is its own property, which another
    // animation may be moving, and a side drawn as none or hidden has none
    // once every animation is laid over (f_motion_style).
    case GARB_BORDER_TOP_STYLE: case GARB_BORDER_RIGHT_STYLE: case GARB_BORDER_BOTTOM_STYLE:
    case GARB_BORDER_LEFT_STYLE:
        i = p - GARB_BORDER_TOP_STYLE;
        d->border_style[i] = s->border_style[i];
        break;
    case GARB_BORDER_TOP_COLOR: case GARB_BORDER_RIGHT_COLOR: case GARB_BORDER_BOTTOM_COLOR:
    case GARB_BORDER_LEFT_COLOR:
        i = p - GARB_BORDER_TOP_COLOR;
        d->border_color[i] = s->border_color[i];
        d->current_colours = (uint8_t)((d->current_colours & ~FLOW_CURRENT_BORDER(i)) |
                                       (s->current_colours & FLOW_CURRENT_BORDER(i)));
        break;
    case GARB_WIDTH: d->width = s->width; break;
    case GARB_HEIGHT: d->height = s->height; break;
    case GARB_MIN_WIDTH: d->min_width = s->min_width; break;
    case GARB_MAX_WIDTH: d->max_width = s->max_width; break;
    case GARB_MIN_HEIGHT: d->min_height = s->min_height; break;
    case GARB_MAX_HEIGHT: d->max_height = s->max_height; break;
    case GARB_BOX_SIZING: d->box_sizing = s->box_sizing; break;
    case GARB_OVERFLOW_X: d->overflow_x = s->overflow_x; break;
    case GARB_OVERFLOW_Y: d->overflow_y = s->overflow_y; break;
    case GARB_FONT_FAMILY: d->family = s->family; break;
    case GARB_FONT_SIZE: d->font_size = s->font_size; break;
    case GARB_FONT_WEIGHT: d->font_weight = s->font_weight; break;
    case GARB_FONT_STYLE: d->font_style = s->font_style; break;
    case GARB_TEXT_ALIGN: d->text_align = s->text_align; break;
    case GARB_VERTICAL_ALIGN: d->vertical_align = s->vertical_align; break;
    case GARB_WHITE_SPACE: d->white_space = s->white_space; break;
    case GARB_LINE_HEIGHT: d->line_height = s->line_height; break;
    case GARB_TEXT_INDENT: d->text_indent = s->text_indent; break;
    case GARB_TEXT_TRANSFORM: d->text_transform = s->text_transform; break;
    case GARB_TEXT_DECORATION_LINE: d->text_decoration = s->text_decoration; break;
    case GARB_VISIBILITY: d->visibility = s->visibility; break;
    case GARB_LIST_STYLE_TYPE: d->list_style_type = s->list_style_type; break;
    case GARB_LIST_STYLE_POSITION: d->list_style_position = s->list_style_position; break;
    case GARB_BORDER_SPACING:
        d->border_spacing[0] = s->border_spacing[0];
        d->border_spacing[1] = s->border_spacing[1];
        break;
    case GARB_BORDER_COLLAPSE: d->border_collapse = s->border_collapse; break;
    case GARB_CAPTION_SIDE: d->caption_side = s->caption_side; break;
    // A float and a position alone: the display they blockify is settled
    // once, from what every animation makes of them (style.c, settle_box).
    case GARB_FLOAT: d->float_side = s->float_side; break;
    case GARB_CLEAR: d->clear = s->clear; break;
    case GARB_POSITION: d->position = s->position; break;
    case GARB_TOP: case GARB_RIGHT: case GARB_BOTTOM: case GARB_LEFT:
        i = p - GARB_TOP;
        d->inset[i] = s->inset[i];
        break;
    case GARB_Z_INDEX:
        d->has_z_index = s->has_z_index;
        d->z_index = s->z_index;
        break;
    case GARB_CLIP:
        d->legacy_clip_set = s->legacy_clip_set;
        os64_memcpy(d->legacy_clip, s->legacy_clip, sizeof(d->legacy_clip));
        break;
    case GARB_OPACITY: d->opacity = s->opacity; break;
    case GARB_POINTER_EVENTS: d->pointer_events_none = s->pointer_events_none; break;
    case GARB_FLEX_DIRECTION: d->flex_direction = s->flex_direction; break;
    case GARB_FLEX_WRAP: d->flex_wrap = s->flex_wrap; break;
    case GARB_JUSTIFY_CONTENT: d->justify_content = s->justify_content; break;
    case GARB_ALIGN_ITEMS: d->align_items = s->align_items; break;
    case GARB_ALIGN_SELF: d->align_self = s->align_self; break;
    case GARB_ALIGN_CONTENT: d->align_content = s->align_content; break;
    case GARB_FLEX_GROW: d->flex_grow = s->flex_grow; break;
    case GARB_FLEX_SHRINK: d->flex_shrink = s->flex_shrink; break;
    case GARB_FLEX_BASIS: d->flex_basis = s->flex_basis; break;
    case GARB_ORDER: d->order = s->order; break;
    case GARB_ROW_GAP: d->row_gap = s->row_gap; break;
    case GARB_COLUMN_GAP: d->column_gap = s->column_gap; break;
    case GARB_GRID_TEMPLATE_COLUMNS: d->grid_template_columns = s->grid_template_columns; break;
    case GARB_GRID_TEMPLATE_ROWS: d->grid_template_rows = s->grid_template_rows; break;
    case GARB_GRID_AUTO_COLUMNS: d->grid_auto_columns = s->grid_auto_columns; break;
    case GARB_GRID_AUTO_ROWS: d->grid_auto_rows = s->grid_auto_rows; break;
    case GARB_GRID_TEMPLATE_AREAS:
        d->grid_areas = s->grid_areas;
        d->ngrid_areas = s->ngrid_areas;
        d->grid_area_rows = s->grid_area_rows;
        d->grid_area_cols = s->grid_area_cols;
        break;
    case GARB_GRID_AUTO_FLOW:
        d->grid_auto_flow_column = s->grid_auto_flow_column;
        d->grid_dense = s->grid_dense;
        break;
    case GARB_GRID_ROW_START: d->grid_row_start = s->grid_row_start; break;
    case GARB_GRID_ROW_END: d->grid_row_end = s->grid_row_end; break;
    case GARB_GRID_COLUMN_START: d->grid_column_start = s->grid_column_start; break;
    case GARB_GRID_COLUMN_END: d->grid_column_end = s->grid_column_end; break;
    case GARB_JUSTIFY_ITEMS: d->justify_items = s->justify_items; break;
    case GARB_JUSTIFY_SELF: d->justify_self = s->justify_self; break;
    case GARB_BORDER_TOP_LEFT_RADIUS: case GARB_BORDER_TOP_RIGHT_RADIUS:
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: case GARB_BORDER_BOTTOM_LEFT_RADIUS:
        i = p - GARB_BORDER_TOP_LEFT_RADIUS;
        d->radius[i][0] = s->radius[i][0];
        d->radius[i][1] = s->radius[i][1];
        break;
    case GARB_BOX_SHADOW:
        d->box_shadows = s->box_shadows;
        d->nbox_shadows = s->nbox_shadows;
        break;
    case GARB_TEXT_SHADOW:
        d->text_shadows = s->text_shadows;
        d->ntext_shadows = s->ntext_shadows;
        break;
    case GARB_IMAGE_RENDERING: d->pixelated = s->pixelated; break;
    case GARB_TRANSFORM:
        d->transform = s->transform;
        d->ntransform = s->ntransform;
        break;
    case GARB_TRANSFORM_ORIGIN:
        d->transform_origin[0] = s->transform_origin[0];
        d->transform_origin[1] = s->transform_origin[1];
        d->transform_origin_z = s->transform_origin_z;
        break;
    case GARB_TRANSLATE: d->translate = s->translate; break;
    case GARB_ROTATE: d->rotate = s->rotate; break;
    case GARB_SCALE: d->scale = s->scale; break;
    default: break;
    }
}

// Whether an animation may set `p`: `display` builds and unbuilds boxes
// (PILE3.md § Animations, Booked), and the animations' own longhands and
// `will-change` are not animatable (Animations 1 § 3, Will Change 1).
static bool animatable(garb_prop_t p)
{
    return p != GARB_DISPLAY && p != GARB_WILL_CHANGE &&
           !(p >= GARB_ANIMATION_NAME && p <= GARB_ANIMATION_FILL_MODE);
}

// Property `p` at `t` between keyframe styles `a` and `b`, into `d` — or,
// with `d` NULL, only whether these two values mix continuously or flip.
// False when a list found no room, and the property is left as it was.
static bool mix_prop(garb_prop_t p, const flow_style_t *a, const flow_style_t *b, double t,
                     flow_style_t *d, MixKind *kind, FAlloc alloc, void *ctx)
{
    int i;
    *kind = MIX_CONTINUOUS;
    switch (p) {
    case GARB_COLOR:
        if (d != NULL)
            d->color = mix_colour(a->color, b->color, t);
        return true;
    case GARB_TEXT_FILL_COLOR:
        if (d != NULL) {
            d->fill = mix_colour(a->fill, b->fill, t);
            hand_current(d, a, b, FLOW_CURRENT_FILL);
        }
        return true;
    case GARB_BACKGROUND_COLOR:
        if (d != NULL) {
            uint32_t ca = a->has_background ? a->background : 0xFF000000u;
            uint32_t cb = b->has_background ? b->background : 0xFF000000u;
            d->background = mix_colour(ca, cb, t);
            d->has_background = flow_alpha(d->background) > 0;
            hand_current(d, a, b, FLOW_CURRENT_BACKGROUND);
        }
        return true;
    case GARB_BORDER_TOP_COLOR: case GARB_BORDER_RIGHT_COLOR: case GARB_BORDER_BOTTOM_COLOR:
    case GARB_BORDER_LEFT_COLOR:
        i = p - GARB_BORDER_TOP_COLOR;
        if (d != NULL) {
            d->border_color[i] = mix_colour(a->border_color[i], b->border_color[i], t);
            hand_current(d, a, b, FLOW_CURRENT_BORDER(i));
        }
        return true;
    case GARB_BACKGROUND_POSITION_X: case GARB_BACKGROUND_POSITION_Y: {
        bool x = p == GARB_BACKGROUND_POSITION_X;
        const flow_length_t *la = x ? a->backgrounds.x : a->backgrounds.y;
        const flow_length_t *lb = x ? b->backgrounds.x : b->backgrounds.y;
        int32_t na = x ? a->backgrounds.nx : a->backgrounds.ny;
        int32_t nb = x ? b->backgrounds.nx : b->backgrounds.ny;
        if (!mix_lengths(la, na, lb, nb, t, NULL, NULL, alloc, ctx))
            break;
        if (d == NULL)
            return true;
        return mix_lengths(la, na, lb, nb, t, x ? &d->backgrounds.x : &d->backgrounds.y,
                           x ? &d->backgrounds.nx : &d->backgrounds.ny, alloc, ctx);
    }
    case GARB_BACKGROUND_SIZE:
        if (!mix_sizes(a->backgrounds.size, a->backgrounds.nsize, b->backgrounds.size,
                       b->backgrounds.nsize, t, NULL, NULL, alloc, ctx))
            break;
        if (d == NULL)
            return true;
        return mix_sizes(a->backgrounds.size, a->backgrounds.nsize, b->backgrounds.size,
                         b->backgrounds.nsize, t, &d->backgrounds.size, &d->backgrounds.nsize,
                         alloc, ctx);
    case GARB_MARGIN_TOP: case GARB_MARGIN_RIGHT: case GARB_MARGIN_BOTTOM: case GARB_MARGIN_LEFT:
        i = p - GARB_MARGIN_TOP;
        if (!mix_length(a->margin[i], b->margin[i], t, d != NULL ? &d->margin[i] : NULL))
            break;
        return true;
    case GARB_PADDING_TOP: case GARB_PADDING_RIGHT: case GARB_PADDING_BOTTOM:
    case GARB_PADDING_LEFT:
        i = p - GARB_PADDING_TOP;
        if (!mix_nonneg(a->padding[i], b->padding[i], t, d != NULL ? &d->padding[i] : NULL))
            break;
        return true;
    case GARB_BORDER_TOP_WIDTH: case GARB_BORDER_RIGHT_WIDTH: case GARB_BORDER_BOTTOM_WIDTH:
    case GARB_BORDER_LEFT_WIDTH:
        i = p - GARB_BORDER_TOP_WIDTH;
        if (d != NULL)
            d->border_width[i] = round_i32(fmax(lerp(a->border_width[i], b->border_width[i], t), 0));
        return true;
    case GARB_WIDTH: case GARB_HEIGHT: case GARB_MIN_WIDTH: case GARB_MAX_WIDTH:
    case GARB_MIN_HEIGHT: case GARB_MAX_HEIGHT: {
        static const size_t at[] = {offsetof(flow_style_t, width), offsetof(flow_style_t, height),
                                    offsetof(flow_style_t, min_width),
                                    offsetof(flow_style_t, max_width),
                                    offsetof(flow_style_t, min_height),
                                    offsetof(flow_style_t, max_height)};
        size_t off = p == GARB_WIDTH ? at[0] : p == GARB_HEIGHT ? at[1] : p == GARB_MIN_WIDTH ? at[2]
                   : p == GARB_MAX_WIDTH ? at[3] : p == GARB_MIN_HEIGHT ? at[4] : at[5];
        const flow_length_t *la = (const flow_length_t *)((const char *)a + off);
        const flow_length_t *lb = (const flow_length_t *)((const char *)b + off);
        flow_length_t r;
        if (!mix_nonneg(*la, *lb, t, &r))
            break;
        if (d != NULL)
            *(flow_length_t *)((char *)d + off) = r;
        return true;
    }
    case GARB_FONT_SIZE:
        if (d != NULL)
            d->font_size = round_i32(fmax(lerp(a->font_size, b->font_size, t), 0));
        return true;
    case GARB_FONT_WEIGHT:
        // A number from 1 to 1000 (Fonts 4 § 2.2).
        if (d != NULL)
            d->font_weight = (uint16_t)round_i32(fmin(fmax(lerp(a->font_weight, b->font_weight,
                                                                t), 1), 1000));
        return true;
    case GARB_LINE_HEIGHT:
        if (a->line_height.kind != b->line_height.kind ||
            a->line_height.kind == FLOW_LINE_NORMAL)
            break;
        if (d != NULL)
            d->line_height.value = round_i32(fmax(lerp(a->line_height.value,
                                                       b->line_height.value, t), 0));
        if (d != NULL)
            d->line_height.kind = a->line_height.kind;
        return true;
    case GARB_TEXT_INDENT:
        if (!mix_length(a->text_indent, b->text_indent, t, d != NULL ? &d->text_indent : NULL))
            break;
        return true;
    case GARB_VISIBILITY:
        // Appendix A: visible all the way between if either end is.
        if (a->visibility != FLOW_VISIBLE && b->visibility != FLOW_VISIBLE)
            break;
        *kind = MIX_DISCRETE;
        if (d != NULL)
            d->visibility = t <= 0 ? a->visibility : t >= 1 ? b->visibility : FLOW_VISIBLE;
        return true;
    case GARB_BORDER_SPACING:
        if (d != NULL)
            for (int k = 0; k < 2; k++)
                d->border_spacing[k] = round_i32(fmax(lerp(a->border_spacing[k],
                                                           b->border_spacing[k], t), 0));
        return true;
    case GARB_TOP: case GARB_RIGHT: case GARB_BOTTOM: case GARB_LEFT:
        i = p - GARB_TOP;
        if (!mix_length(a->inset[i], b->inset[i], t, d != NULL ? &d->inset[i] : NULL))
            break;
        return true;
    case GARB_Z_INDEX:
        // Two numbers make a number, whatever the element's own is.
        if (!a->has_z_index || !b->has_z_index)
            break;
        if (d != NULL) {
            d->has_z_index = true;
            d->z_index = half_up_i32(lerp(a->z_index, b->z_index, t));
        }
        return true;
    case GARB_OPACITY:
        if (d != NULL)
            d->opacity = (uint16_t)round_i32(fmin(fmax(lerp(a->opacity, b->opacity, t), 0), 1000));
        return true;
    case GARB_FLEX_GROW: case GARB_FLEX_SHRINK: {
        int32_t va = p == GARB_FLEX_GROW ? a->flex_grow : a->flex_shrink;
        int32_t vb = p == GARB_FLEX_GROW ? b->flex_grow : b->flex_shrink;
        if (d != NULL)
            *(p == GARB_FLEX_GROW ? &d->flex_grow : &d->flex_shrink) =
                round_i32(fmax(lerp(va, vb, t), 0));
        return true;
    }
    case GARB_FLEX_BASIS:
        if (!mix_nonneg(a->flex_basis, b->flex_basis, t, d != NULL ? &d->flex_basis : NULL))
            break;
        return true;
    case GARB_ORDER:
        if (d != NULL)
            d->order = half_up_i32(lerp(a->order, b->order, t));
        return true;
    case GARB_ROW_GAP: case GARB_COLUMN_GAP: {
        bool row = p == GARB_ROW_GAP;
        if (!mix_nonneg(row ? a->row_gap : a->column_gap, row ? b->row_gap : b->column_gap, t,
                        d != NULL ? (row ? &d->row_gap : &d->column_gap) : NULL))
            break;
        return true;
    }
    case GARB_BORDER_TOP_LEFT_RADIUS: case GARB_BORDER_TOP_RIGHT_RADIUS:
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: case GARB_BORDER_BOTTOM_LEFT_RADIUS:
        i = p - GARB_BORDER_TOP_LEFT_RADIUS;
        if (!mix_length(a->radius[i][0], b->radius[i][0], t, NULL) ||
            !mix_length(a->radius[i][1], b->radius[i][1], t, NULL))
            break;
        if (d != NULL) {
            (void)mix_nonneg(a->radius[i][0], b->radius[i][0], t, &d->radius[i][0]);
            (void)mix_nonneg(a->radius[i][1], b->radius[i][1], t, &d->radius[i][1]);
        }
        return true;
    case GARB_BOX_SHADOW: case GARB_TEXT_SHADOW: {
        bool box = p == GARB_BOX_SHADOW;
        const flow_shadow_t *sa = box ? a->box_shadows : a->text_shadows;
        const flow_shadow_t *sb = box ? b->box_shadows : b->text_shadows;
        int32_t na = box ? a->nbox_shadows : a->ntext_shadows;
        int32_t nb = box ? b->nbox_shadows : b->ntext_shadows;
        if (!mix_shadows(sa, na, sb, nb, a->color, b->color, t, NULL, NULL, alloc, ctx))
            break;
        if (d == NULL)
            return true;
        flow_shadow_t *list = NULL;
        int32_t n = 0;
        if (!mix_shadows(sa, na, sb, nb, a->color, b->color, t, &list, &n, alloc, ctx))
            return false;
        *(box ? &d->box_shadows : &d->text_shadows) = list;
        *(box ? &d->nbox_shadows : &d->ntext_shadows) = n;
        return true;
    }
    case GARB_TRANSFORM:
        if (d == NULL)
            return true;
        return mix_transforms(a->transform, a->ntransform, b->transform, b->ntransform, t,
                              &d->transform, &d->ntransform, alloc, ctx);
    case GARB_TRANSLATE: case GARB_ROTATE: case GARB_SCALE: {
        const flow_transform_t *fa = p == GARB_TRANSLATE ? a->translate
                                   : p == GARB_ROTATE ? a->rotate : a->scale;
        const flow_transform_t *fb = p == GARB_TRANSLATE ? b->translate
                                   : p == GARB_ROTATE ? b->rotate : b->scale;
        if (d == NULL)
            return true;
        return mix_individual(fa, fb, t, p == GARB_TRANSLATE ? &d->translate
                                       : p == GARB_ROTATE ? &d->rotate : &d->scale,
                              alloc, ctx);
    }
    case GARB_TRANSFORM_ORIGIN:
        if (!mix_length(a->transform_origin[0], b->transform_origin[0], t, NULL) ||
            !mix_length(a->transform_origin[1], b->transform_origin[1], t, NULL))
            break;
        if (d != NULL) {
            (void)mix_length(a->transform_origin[0], b->transform_origin[0], t,
                             &d->transform_origin[0]);
            (void)mix_length(a->transform_origin[1], b->transform_origin[1], t,
                             &d->transform_origin[1]);
            d->transform_origin_z = round_i32(lerp(a->transform_origin_z,
                                                   b->transform_origin_z, t));
        }
        return true;
    default:
        break;
    }
    // Discrete: the first value until one half, the second from it.
    *kind = MIX_DISCRETE;
    if (d != NULL)
        copy_prop(p, d, t < 0.5 ? a : b);
    return true;
}

// ── An animation's value (Web Animations 1 § 5.3.4) ─────────────────────

// A keyframe as one property sees it: its style, offset and easing.
typedef struct {
    const flow_style_t *style;
    double offset;
    const flow_easing_t *easing;
} Frame;

// The frames property `p` is worked between: the implicit first, the
// base's at 0, where no keyframe that sets it is at 0; those that do, in
// their order; the implicit last, the base's at 1, where none is at 1 —
// eased as the animation is (Animations 2 § 3.2).
typedef struct {
    const FMotion *m;
    const FAnimation *an;
    garb_prop_t p;
    bool at0, at1;
} Line;

// The line's `i`th frame. Walked, not listed: a list would be one more
// allocation a value could fail on.
static Frame line_frame(const Line *l, int32_t i)
{
    const flow_easing_t *own = &l->an->timing.easing;
    if (!l->at0) {
        if (i == 0)
            return (Frame){&l->m->base, 0, own};
        i--;
    }
    for (int32_t j = 0; j < l->an->nframes; j++) {
        const FKeyframe *f = &l->an->frames[j];
        if (f_props_has(&f->sets, l->p) && i-- == 0)
            return (Frame){&f->style, f->offset, f->has_easing ? &f->easing : own};
    }
    return (Frame){&l->m->base, 1, own};
}

// The two frames of the line around `progress`, as Web Animations 1
// § 5.3.4 finds them: the last at or before the progress and before 1,
// and the one after; of several at 1, at the end, the last alone (`*start`
// and `*end` the same).
static void interval(const FMotion *m, const FAnimation *an, garb_prop_t p, double progress,
                     Frame *start, Frame *end)
{
    Line l = {m, an, p, false, false};
    for (int32_t i = 0; i < an->nframes; i++)
        if (f_props_has(&an->frames[i].sets, p)) {
            l.at0 |= an->frames[i].offset == 0;
            l.at1 |= an->frames[i].offset == 1;
        }
    // One pass over the line's offsets: how many frames, how many at 1,
    // the last at or before the progress and before 1, the last at 0.
    int32_t n = 0, ones = 0, s = -1, zero = -1;
    for (int32_t j = -1; j <= an->nframes; j++) {
        double offset;
        if (j == -1) {
            if (l.at0)
                continue;
            offset = 0;
        } else if (j == an->nframes) {
            if (l.at1)
                continue;
            offset = 1;
        } else if (f_props_has(&an->frames[j].sets, p)) {
            offset = an->frames[j].offset;
        } else {
            continue;
        }
        ones += offset == 1;
        if (offset <= progress && offset < 1)
            s = n;
        if (offset == 0)
            zero = n;
        n++;
    }
    if (progress >= 1 && ones > 1) {
        *start = *end = line_frame(&l, n - 1);
        return;
    }
    if (s < 0)
        s = zero;
    *start = line_frame(&l, s);
    *end = line_frame(&l, s + 1 < n ? s + 1 : s);
}

// The fraction between the two, through the first's easing.
static double eased(const Frame *start, const Frame *end, double progress, bool before)
{
    double span = end->offset - start->offset;
    double x = span != 0 ? (progress - start->offset) / span : 1;
    return f_ease(start->easing, x, before);
}

bool f_motion_style(const FMotion *m, double now, flow_style_t *out, FAlloc alloc, void *ctx)
{
    bool ok = true;
    for (int32_t a = 0; a < m->nanims; a++) {
        const FAnimation *an = &m->anims[a];
        if (an->record == NULL)
            continue;
        Clock k = clock_at(&an->timing, local_time(an->record, now));
        if (!k.in_effect)
            continue;
        // A side's width after its style, which brings a width of its own.
        for (int pass = 0; pass < 2; pass++)
            for (int p = 0; p < GARB_NPROPS; p++) {
                bool width = p >= GARB_BORDER_TOP_WIDTH && p <= GARB_BORDER_LEFT_WIDTH;
                if (width != (pass == 1) || !f_props_has(&an->props, (garb_prop_t)p) ||
                    !animatable((garb_prop_t)p))
                    continue;
                Frame s, e;
                MixKind kind;
                interval(m, an, (garb_prop_t)p, k.progress, &s, &e);
                if (!mix_prop((garb_prop_t)p, s.style, e.style,
                              eased(&s, &e, k.progress, k.before), out, &kind, alloc, ctx))
                    ok = false;
            }
    }
    // What is in currentColor follows the colour as it moves; a side drawn
    // as none or hidden has no width (Backgrounds 3 § 4.3).
    for (int i = 0; i < 4; i++) {
        if (out->current_colours & FLOW_CURRENT_BORDER(i))
            out->border_color[i] = out->color;
        if (out->border_style[i] == FLOW_BORDER_NONE || out->border_style[i] == FLOW_BORDER_HIDDEN)
            out->border_width[i] = 0;
    }
    if (out->current_colours & FLOW_CURRENT_BACKGROUND) {
        out->has_background = flow_alpha(out->color) > 0;
        out->background = out->color;
    }
    if (out->current_colours & FLOW_CURRENT_FILL)
        out->fill = out->color;
    return ok;
}

uint8_t f_motion_effects(const FMotion *m, double now)
{
    uint8_t bits = 0;
    for (int32_t a = 0; a < m->nanims; a++) {
        const FAnimation *an = &m->anims[a];
        if (an->record == NULL)
            continue;
        Clock k = clock_at(&an->timing, local_time(an->record, now));
        // Current — before or in its active phase — or in effect.
        if (k.phase == PHASE_AFTER && !k.in_effect)
            continue;
        if (f_props_has(&an->props, GARB_TRANSFORM) || f_props_has(&an->props, GARB_TRANSLATE) ||
            f_props_has(&an->props, GARB_ROTATE) || f_props_has(&an->props, GARB_SCALE))
            bits |= FLOW_WILL_TRANSFORM;
        if (f_props_has(&an->props, GARB_OPACITY))
            bits |= FLOW_WILL_OPACITY;
    }
    return bits;
}

// ── When a value next changes ───────────────────────────────────────────

// Every x in (0, 1) at which easing `e` — linear or a cubic, not steps —
// crosses `level`, rising or falling: at most three, in order, into `xs`;
// how many. A cubic's y is a cubic in the curve's parameter, so it is cut
// where it turns (its slope's roots), and each piece whose ends lie either
// side of `level` is halved down to the crossing; x rises with the
// parameter (x1 and x2 are in [0, 1]), so the crossings come in order. A
// curve that only touches `level` crosses nothing there.
static int32_t ease_crossings(const flow_easing_t *e, double level, double xs[3])
{
    if (e == NULL || e->kind != FLOW_EASE_CUBIC) {
        if (level <= 0 || level >= 1)
            return 0;
        xs[0] = level;
        return 1;
    }
    // y's slope, qa t^2 + qb t + qc, worked in long double as bezier_y is
    // (3 y1 alone can overflow a double) and divided by its largest
    // coefficient, which moves no root and leaves the rest in [-1, 1].
    const double *p = e->p;
    long double lc = 3.0L * p[1], lb = 3.0L * ((long double)p[3] - p[1]) - lc, la = 1 - lc - lb;
    long double la3 = 3 * la, lb2 = 2 * lb;
    long double big = la3 < 0 ? -la3 : la3, mb = lb2 < 0 ? -lb2 : lb2, mc = lc < 0 ? -lc : lc;
    big = mb > big ? mb : big;
    big = mc > big ? mc : big;
    double qa = (double)(la3 / big), qb = (double)(lb2 / big), qc = (double)(lc / big);
    double cut[4] = {0};
    int32_t nc = 1;
    double roots[2];
    int32_t nr = 0;
    if (fabs(qa) < 1e-12) {
        if (fabs(qb) > 1e-12)
            roots[nr++] = -qc / qb;
    } else {
        double disc = qb * qb - 4 * qa * qc;
        if (disc >= 0) {
            double sq = sqrt(disc);
            double r0 = (-qb - sq) / (2 * qa), r1 = (-qb + sq) / (2 * qa);
            roots[nr++] = fmin(r0, r1);
            roots[nr++] = fmax(r0, r1);
        }
    }
    for (int32_t i = 0; i < nr; i++)
        if (roots[i] > 0 && roots[i] < 1)
            cut[nc++] = roots[i];
    cut[nc++] = 1;
    int32_t n = 0;
    for (int32_t i = 0; i + 1 < nc; i++) {
        double t0 = cut[i], t1 = cut[i + 1];
        double y0 = bezier_y(p[1], p[3], t0) - level, y1 = bezier_y(p[1], p[3], t1) - level;
        if (!((y0 < 0 && y1 > 0) || (y0 > 0 && y1 < 0)))
            continue;
        for (int k = 0; k < 64; k++) {
            double mid = (t0 + t1) / 2, ym = bezier_y(p[1], p[3], mid) - level;
            if ((ym < 0) == (y0 < 0))
                t0 = mid;
            else
                t1 = mid;
        }
        double x = bezier(p[0], p[2], t1);
        if (x > 0 && x < 1 && n < 3)
            xs[n++] = x;
    }
    return n;
}

// The earliest local time after `local` at which progress `p` of the
// current iteration comes round, the iteration running as `k` says.
static double at_progress(const Clock *k, const flow_animation_t *a, double p)
{
    double dur = a->duration_ms;
    return k->iteration_start + (k->forwards ? p : 1 - p) * dur;
}

// The properties whose frames repaint only where the element and what it
// holds already draw (f_motion_in_place).
static bool repaints_in_place(garb_prop_t p)
{
    switch (p) {
    case GARB_COLOR: case GARB_TEXT_FILL_COLOR: case GARB_BACKGROUND_COLOR:
    case GARB_BORDER_TOP_COLOR: case GARB_BORDER_RIGHT_COLOR: case GARB_BORDER_BOTTOM_COLOR:
    case GARB_BORDER_LEFT_COLOR: case GARB_OPACITY: case GARB_VISIBILITY:
    case GARB_BACKGROUND_POSITION_X: case GARB_BACKGROUND_POSITION_Y:
    case GARB_BORDER_TOP_LEFT_RADIUS: case GARB_BORDER_TOP_RIGHT_RADIUS:
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: case GARB_BORDER_BOTTOM_LEFT_RADIUS:
        return true;
    default:
        return false;
    }
}

bool f_motion_in_place(const FMotion *m)
{
    for (int32_t a = 0; a < m->nanims; a++)
        for (int p = 0; p < GARB_NPROPS; p++)
            if (f_props_has(&m->anims[a].props, (garb_prop_t)p) && animatable((garb_prop_t)p) &&
                !repaints_in_place((garb_prop_t)p))
                return false;
    return true;
}

// When a change at time `at` first shows, after `now`. A value rising to
// a step or a level changes there; one falling through it changes just
// past it (running backwards, the before flag is clear in the active
// phase, Web Animations 1 § 4.7.7); and a change near a whole millisecond
// may land a hair either side of it once the clock's arithmetic has
// rounded. So the millisecond `at` is in or touches is due, and failing
// that the one after: never late, and at most one layout early.
// INFINITY for a change already shown.
static double change_due(double at, double now)
{
    double first = ceil(at - 1e-6), after = floor(at) + 1;
    return first > now ? first : after > now ? after : INFINITY;
}

// When a change at progress `p` of the current iteration shows
// (change_due).
static double edge_due(const Clock *k, const flow_animation_t *a, double base, double p, double now)
{
    return change_due(base + at_progress(k, a, p), now);
}

double f_motion_next(const FMotion *m, double now)
{
    double next = INFINITY;
    for (int32_t a = 0; a < m->nanims; a++) {
        const FAnimation *an = &m->anims[a];
        if (an->record == NULL || an->record->now.paused)
            continue;
        double local = local_time(an->record, now), base = an->record->now.start;
        Clock k = clock_at(&an->timing, local);
        if (k.phase == PHASE_BEFORE) {
            next = fmin(next, base + k.before_end);
            continue;
        }
        if (k.phase == PHASE_AFTER)
            continue;
        // The active phase: its end, and the current iteration's.
        double soonest = base + k.active_end;
        double dur = an->timing.duration_ms;
        if (dur > 0 && isfinite(k.iteration_start))
            soonest = fmin(soonest, base + k.iteration_start + dur);
        for (int p = 0; p < GARB_NPROPS && soonest > now; p++) {
            if (!f_props_has(&an->props, (garb_prop_t)p) || !animatable((garb_prop_t)p))
                continue;
            Frame s, e;
            interval(m, an, (garb_prop_t)p, k.progress, &s, &e);
            MixKind kind;
            (void)mix_prop((garb_prop_t)p, s.style, e.style, 0.5, NULL, &kind, NULL, NULL);
            const flow_easing_t *ease = s.easing;
            bool stepped = ease != NULL && ease->kind == FLOW_EASE_STEPS;
            if (kind == MIX_CONTINUOUS && !stepped && s.style != e.style)
                return now;     // moving continuously
            // Hidden to visible is the start's at the interval's start
            // and visible just past it (Appendix A): due the millisecond
            // after.
            if (p == GARB_VISIBILITY && k.progress == s.offset &&
                s.style->visibility != FLOW_VISIBLE && e.style->visibility == FLOW_VISIBLE)
                soonest = fmin(soonest, floor(now) + 1);
            // Only what lies ahead of the progress the way it runs: the
            // interval's far end, and inside it the next of its steps'
            // edges — worked out from where the progress is, never walked
            // to (a page may ask for steps(2147483647)) — or a discrete
            // value's crossings. What is behind it shows already, since the
            // values are worked from the same progress; one the progress
            // stands on may yet change just past it (change_due).
            soonest = fmin(soonest, edge_due(&k, &an->timing, base, k.forwards ? e.offset : s.offset, now));
            double span = e.offset - s.offset;
            double here = span > 0 ? (k.progress - s.offset) / span : 0;
            if (stepped && span > 0) {
                double n = ease->steps, x = here * n;
                // Running backwards the progress may stand on the
                // interval's top, x = n, whose step falls just past it.
                double j = k.forwards ? floor(x) + 1 : floor(x);
                if (j >= 1 && j <= (k.forwards ? n - 1 : n))
                    soonest = fmin(soonest, edge_due(&k, &an->timing, base, s.offset + span * j / n, now));
            } else if (kind == MIX_DISCRETE) {
                // A discrete value flips where the eased fraction crosses
                // one half; visibility, when either end is visible, where
                // it crosses 0 or 1 (Appendix A) — each time the easing
                // does, overshoot and all, whichever way the iteration
                // runs, each when it shows (change_due).
                bool vis = p == GARB_VISIBILITY && (s.style->visibility == FLOW_VISIBLE ||
                                                    e.style->visibility == FLOW_VISIBLE);
                const double levels[2] = {vis ? 0 : 0.5, 1};
                for (int l = 0; l < (vis ? 2 : 1); l++) {
                    double xs[3];
                    int32_t nx = ease_crossings(ease, levels[l], xs);
                    for (int32_t j = 0; j < nx; j++)
                        if (k.forwards ? xs[j] >= here : xs[j] <= here)
                            soonest = fmin(soonest, edge_due(&k, &an->timing, base,
                                                             s.offset + span * xs[j], now));
                }
            }
        }
        next = fmin(next, soonest);
    }
    return next;
}
