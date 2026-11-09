// timing.c — Easing 1's curves and steps, and Web Animations 1's clock
// (garb/timing.h, whose section numbers these are). Nothing here is built from a frame before: a
// value is worked from the timing and the local time, every time.

#include <math.h>

#include "garb/timing.h"
#include "os64/mem.h"
#include "os64/str.h"

#define T_ARRAY(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))

// ── Easing (Easing 1 § 2) ───────────────────────────────────────────────

// A cubic Bézier's x or y at t, the curve from 0 to 1 through p1 and p2.
static double bezier(double p1, double p2, double t)
{
    double c = 3 * p1, b = 3 * (p2 - p1) - c, a = 1 - c - b;
    return ((a * t + b) * t + c) * t;
}

// The curve's y at t, in Bernstein form, each control weighted by its own
// polynomial: the power form's coefficients are millions when the controls
// are, and their rounding moves the curve's ends off 0 and 1, where these
// weights are 0 at t = 0 and 1 at t = 1 exactly. The controls are held to
// the domain (values.h), so no product leaves a double.
static double bezier_y(double p1, double p2, double t)
{
    double u = 1 - t, v = t;
    return 3 * u * u * v * p1 + 3 * u * v * v * p2 + v * v * v;
}

// The curve's y at the parameter whose x is `x`: the least t with x(t) at
// or past it, found by halving [0, 1] until no double lies between the
// halves (x(t) never falls, since x1 and x2 are in [0, 1]). Never stopped
// at the first t whose x is near: a curve flat in x near an end is solved
// — cubic-bezier(0, 100, 0, 100) at 9.26e-8 is y 1.35, where t = x would
// say 0.000028.
static double cubic(const double p[4], double x)
{
    // The ends are the curve's ends: no halving finds t = 0 for x = 0, nor
    // keeps t = 1 for x = 1 where x(t) rounds to 1 a hair before it.
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    double lo = 0, hi = 1;
    for (int i = 0; i < 1100; i++) {
        double mid = lo + (hi - lo) / 2;
        if (!(mid > lo && mid < hi))
            break;
        if (bezier(p[0], p[2], mid) < x)
            lo = mid;
        else
            hi = mid;
    }
    return bezier_y(p[1], p[3], hi);
}

double garb_ease(const garb_easing_t *e, double x, bool before)
{
    if (e == NULL || e->kind == GARB_EASE_LINEAR)
        return x;
    if (e->kind == GARB_EASE_CUBIC) {
        const double *p = e->p;
        // Outside [0, 1], the tangent at the nearer end (§ 2.2.1): an
        // effect easing meets it when a keyframe's easing overshoots, and a
        // script's iteration progress can be anywhere its easing sends it.
        if (x < 0) {
            if (p[0] > 0)
                return p[1] / p[0] * x;
            if (p[2] > 0)
                return p[3] / p[2] * x;
            return 0;
        }
        if (x > 1) {
            if (p[2] < 1)
                return 1 + (p[3] - 1) / (p[2] - 1) * (x - 1);
            if (p[0] < 1)
                return 1 + (p[1] - 1) / (p[0] - 1) * (x - 1);
            return 1;
        }
        return cubic(p, x);
    }
    // Steps (§ 2.3.1).
    double n = e->steps > 0 ? e->steps : 1;
    double step = floor(x * n);
    if (e->jump == GARB_JUMP_START || e->jump == GARB_JUMP_BOTH)
        step += 1;
    if (before && fmod(x * n, 1.0) == 0)
        step -= 1;
    if (x >= 0 && step < 0)
        step = 0;
    double jumps = e->jump == GARB_JUMP_NONE ? n - 1 : e->jump == GARB_JUMP_BOTH ? n + 1 : n;
    if (x <= 1 && step > jumps)
        step = jumps;
    return step / jumps;
}

static int32_t word_at(const garb_val_t *v, const char *const *words, int32_t n)
{
    if (v->kind != GARB_V_KEYWORD)
        return -1;
    for (int32_t i = 0; i < n; i++)
        if (os64_streq(v->keyword, words[i]))
            return i;
    return -1;
}

bool garb_easing_of(const garb_val_t *v, garb_easing_t *out)
{
    // Easing 1 § 2.2's keywords as their curves, § 2.3's as their steps.
    static const struct { const char *word; garb_easing_t e; } kWords[] = {
        {"linear", {GARB_EASE_LINEAR, {0, 0, 0, 0}, 0, GARB_JUMP_END, false}},
        {"ease", {GARB_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, GARB_JUMP_END, true}},
        {"ease-in", {GARB_EASE_CUBIC, {0.42, 0, 1, 1}, 0, GARB_JUMP_END, true}},
        {"ease-out", {GARB_EASE_CUBIC, {0, 0, 0.58, 1}, 0, GARB_JUMP_END, true}},
        {"ease-in-out", {GARB_EASE_CUBIC, {0.42, 0, 0.58, 1}, 0, GARB_JUMP_END, true}},
        {"step-start", {GARB_EASE_STEPS, {0, 0, 0, 0}, 1, GARB_JUMP_START, false}},
        {"step-end", {GARB_EASE_STEPS, {0, 0, 0, 0}, 1, GARB_JUMP_END, false}},
    };
    if (v->kind == GARB_V_KEYWORD) {
        for (int32_t k = 0; k < T_ARRAY(kWords); k++)
            if (os64_streq(v->keyword, kWords[k].word)) {
                *out = kWords[k].e;
                return true;
            }
        return false;
    }
    if (v->kind != GARB_V_FUNCTION)
        return false;
    if (os64_streq(v->keyword, "cubic-bezier") && v->nitems == 4) {
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_EASE_CUBIC;
        for (int k = 0; k < 4; k++)
            out->p[k] = v->items[k].number;
        return true;
    }
    if (os64_streq(v->keyword, "steps") && v->nitems >= 1) {
        static const char *const kJumps[] = {"jump-end", "jump-start", "jump-none", "jump-both"};
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_EASE_STEPS;
        out->steps = v->items[0].number < INT32_MAX ? (int32_t)v->items[0].number : INT32_MAX;
        out->jump = GARB_JUMP_END;
        if (v->nitems == 2) {
            int32_t w = word_at(&v->items[1], kJumps, T_ARRAY(kJumps));
            static const char *const kStart[] = {"start"};
            out->jump = w >= 0 ? (garb_jump_t)w
                      : word_at(&v->items[1], kStart, 1) == 0 ? GARB_JUMP_START : GARB_JUMP_END;
        }
        return true;
    }
    return false;
}

// ── The clock (Web Animations 1 § 4.5-4.10) ──────────────────────────────

static bool current_forwards(garb_direction_t d, double iteration)
{
    switch (d) {
    case GARB_DIRECTION_NORMAL: return true;
    case GARB_DIRECTION_REVERSE: return false;
    default: {
        double n = iteration + (d == GARB_DIRECTION_ALTERNATE_REVERSE);
        return isinf(n) || fmod(n, 2) == 0;
    }
    }
}

garb_clock_t garb_clock_at(const garb_timing_t *t, double local, bool backwards)
{
    garb_clock_t k;
    os64_memset(&k, 0, sizeof(k));
    double dur = t->duration_ms > 0 ? t->duration_ms : 0;
    double count = t->iterations > 0 ? t->iterations : 0;
    double active = dur == 0 || count == 0 ? 0 : dur * count;
    double delay = t->delay_ms;
    double end = fmax(delay + active + t->end_delay_ms, 0);
    k.active_duration = active;
    k.end_time = end;
    k.before_end = fmax(fmin(delay, end), 0);
    k.active_end = fmax(fmin(delay + active, end), 0);
    // § 4.5.5: a boundary is inside the phase the animation is moving
    // away from it into — the active interval's end is not in it going
    // forwards, its start is not in it going backwards.
    k.phase = local < k.before_end || (backwards && local == k.before_end) ? GARB_PHASE_BEFORE
            : local > k.active_end || (!backwards && local == k.active_end) ? GARB_PHASE_AFTER
            : GARB_PHASE_ACTIVE;
    double at;
    if (k.phase == GARB_PHASE_BEFORE) {
        if (t->fill != GARB_FILL_BACKWARDS && t->fill != GARB_FILL_BOTH)
            return k;
        at = fmax(local - delay, 0);
    } else if (k.phase == GARB_PHASE_ACTIVE) {
        at = local - delay;
    } else {
        if (t->fill != GARB_FILL_FORWARDS && t->fill != GARB_FILL_BOTH)
            return k;
        at = fmax(fmin(local - delay, active), 0);
    }
    k.in_effect = true;
    k.active_time = at;
    double start = t->iteration_start;
    double overall = (dur == 0 ? (k.phase == GARB_PHASE_BEFORE ? 0 : count) : at / dur) + start;
    double simple = isinf(overall) ? fmod(start, 1.0) : fmod(overall, 1.0);
    if (simple == 0 && k.phase != GARB_PHASE_BEFORE && at == active && count != 0)
        simple = 1;
    k.iteration = k.phase == GARB_PHASE_AFTER && isinf(count) ? INFINITY
                : simple == 1 ? floor(overall) - 1 : floor(overall);
    k.iteration_start = isinf(k.iteration) ? INFINITY : delay + (k.iteration - start) * dur;
    k.forwards = current_forwards(t->direction, k.iteration);
    k.directed = k.forwards ? simple : 1 - simple;
    k.before = (k.phase == GARB_PHASE_BEFORE && k.forwards) || (k.phase == GARB_PHASE_AFTER && !k.forwards);
    k.progress = garb_ease(&t->easing, k.directed, k.before);
    return k;
}
