// libmedia's track buffer (userland/libmedia/media_track.c) beside a model.
//
// The MODEL is the spec's coded frame processing written from its text over
// a flat list of frames per track: no ranges, no index, everything a scan.
// After every operation the buffer and the model must agree on the frames
// kept (times, flags, generation, bytes by pattern), the bytes counted, each
// track's ranges and the SourceBuffer's intersection, and the player's
// queries at a sweep of times. Scripted cases come first (one or more per
// rule of MEDIA_SOURCE.md § 4), then random scripts of appends, removes,
// window and offset changes, mode switches, parser resets, evictions and a
// cursor reading through all of it.
//
// Every block the buffer allocates and every slab is counted, and released
// AT ONCE (free under ASan), so a touch after release is a report and a
// leak is a count left over.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "os64/media_track.h"

static unsigned long s_checks, s_failed;

static bool check(bool ok, const char *what, ...)
{
    s_checks++;
    if (ok)
        return true;
    s_failed++;
    if (s_failed <= 40) {
        va_list ap;
        va_start(ap, what);
        printf("FAIL: ");
        vprintf(what, ap);
        printf("\n");
        va_end(ap);
    }
    return false;
}

// ── Memory, counted; released at once ──────────────────────────────────────

static long s_live;
static long s_fail_countdown;
static void *t_alloc(void *c, size_t n)
{
    (void)c;
    if (s_fail_countdown > 0 && --s_fail_countdown == 0)
        return 0;
    s_live++;
    return malloc(n);
}
static void t_release(void *c, void *b)
{
    (void)c;
    s_live--;
    free(b);
}
static const os64_media_track_memory_t kMemory = { t_alloc, t_release, 0 };

// A slab as the parser makes one: its own hold, released when the "mdat"
// ends (slab_end below).
static os64_media_fragment_slab_t *slab_new(uint32_t size)
{
    os64_media_fragment_slab_t *s = t_alloc(0, sizeof(*s) + size + 1);
    s->data = (uint8_t *)(s + 1);
    s->size = s->filled = size;
    s->holds = 1;
    s->release = t_release;
    s->context = 0;
    return s;
}
static void slab_end(os64_media_fragment_slab_t *s) { os64_media_fragment_slab_drop(s); }

static uint8_t pattern(uint32_t seed, uint32_t i) { return (uint8_t)(seed * 167u + i * 13u + (i >> 7)); }

// ── The model ──────────────────────────────────────────────────────────────

typedef struct {
    int64_t dts, pts, dur;
    uint32_t flags, gen, size, seed;
    uint64_t serial;
    uint32_t group;                     // the coded frame group it came in: its decode chain
    uint32_t run;                       // the decode run (the buffer's range) it is in
} mf_t;

typedef struct {
    int kind;
    uint32_t id;
    mf_t *f;
    int n, cap;
    int64_t last_dts, last_dur, highest_end;
    bool need_rap;
    mf_t preload;
    bool have_preload;
    uint64_t next_serial, last_kept;
    uint64_t quota;
    uint32_t newest_gen;
    int64_t interbuffer;                // the longest duration or decode step kept
    uint32_t group, groups;             // the current coded frame group's number, and the count
    uint32_t run, runs;                 // the current group's run, and the count
} mtrack_t;

typedef struct {
    mtrack_t t[2];
    int nt;
    int mode;
    int64_t offset, ws, we, gs, ge;
} model_t;

#define NONE OS64_MEDIA_NO_TIME

static void m_init(model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->ws = 0;
    m->we = OS64_MEDIA_TIME_MAX;
    m->gs = NONE;
}

static void m_unset(mtrack_t *t)
{
    t->last_dts = t->last_dur = t->highest_end = NONE;
    t->need_rap = true;
    t->last_kept = 0;
}

static int m_find_serial(const mtrack_t *t, uint64_t serial)
{
    for (int i = 0; i < t->n; i++)
        if (t->f[i].serial == serial)
            return i;
    return -1;
}

static int cmp_dts(const void *a, const void *b)
{
    const mf_t *x = a, *y = b;
    if (x->dts != y->dts) return x->dts < y->dts ? -1 : 1;
    return x->serial < y->serial ? -1 : x->serial > y->serial;
}

static void m_sort(mtrack_t *t) { qsort(t->f, (size_t)t->n, sizeof(mf_t), cmp_dts); }

static bool m_unit_start(const mtrack_t *t, int i)
{
    return (t->f[i].flags & OS64_MEDIA_FRAME_RAP) && !(i > 0 && (t->f[i - 1].flags & OS64_MEDIA_FRAME_PRELOAD));
}

static void m_remove_marked(mtrack_t *t, const bool *rm)
{
    int w = 0;
    for (int i = 0; i < t->n; i++)
        if (!rm[i])
            t->f[w++] = t->f[i];
    t->n = w;
}

// The spec's removal: frames presented in [lo, hi), the preload frame of
// any of them, and in decode order everything after a removed frame up to
// the next random access point.
static void m_cut(mtrack_t *t, int64_t lo, int64_t hi)
{
    if (lo >= hi || t->n == 0)
        return;
    m_sort(t);
    bool *sel = calloc((size_t)t->n, 1), *rm = calloc((size_t)t->n, 1);
    for (int i = 0; i < t->n; i++)
        sel[i] = t->f[i].pts >= lo && t->f[i].pts < hi;
    for (int i = 1; i < t->n; i++)
        if (sel[i] && (t->f[i - 1].flags & OS64_MEDIA_FRAME_PRELOAD))
            sel[i - 1] = true;
    // Dependents follow a frame's own decode chain, its coded frame group:
    // each group swept on its own, in decode order.
    for (int g0 = 0; g0 < t->n; g0++) {
        bool seen = false;
        for (int j = 0; j < g0; j++) seen |= t->f[j].group == t->f[g0].group;
        if (seen) continue;
        // A stretch removed from the middle of a run splits it there: the
        // frames after it are a run of their own (the buffer's split tail).
        bool removing = false, kept_before = false, split = false;
        uint32_t new_run = 0, old_run = 0;
        int prev = -1;
        for (int i = g0; i < t->n; i++) {
            if (t->f[i].group != t->f[g0].group) continue;
            bool unit = (t->f[i].flags & OS64_MEDIA_FRAME_RAP) && !(prev >= 0 && (t->f[prev].flags & OS64_MEDIA_FRAME_PRELOAD));
            if (sel[i]) {
                rm[i] = true;
                if (!removing && kept_before) split = true;
                removing = true;
            } else if (unit) {
                if (removing && split) {
                    old_run = t->f[i].run;
                    new_run = ++t->runs;
                }
                removing = false;
                split = false;
            } else if (removing) {
                rm[i] = true;
            }
            if (!rm[i]) {
                if (new_run && t->f[i].run == old_run) t->f[i].run = new_run;
                kept_before = true;
                if (new_run && t->f[i].serial == t->last_kept) t->run = new_run;
            }
            prev = i;
        }
    }
    m_remove_marked(t, rm);
    free(sel);
    free(rm);
}

static void m_keep(mtrack_t *t, mf_t f)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->f = realloc(t->f, (size_t)t->cap * sizeof(mf_t));
    }
    t->f[t->n++] = f;
}

static mtrack_t *m_track(model_t *m, uint32_t id)
{
    for (int k = 0; k < m->nt; k++)
        if (m->t[k].id == id)
            return &m->t[k];
    return 0;
}

// Coded frame processing, from the spec's text.
static void m_frame(model_t *m, uint32_t id, int64_t pts0, int64_t dts0, int64_t end0, bool rap, uint32_t gen,
                    uint32_t size, uint32_t seed)
{
    mtrack_t *t = m_track(m, id);
    if (t->kind == OS64_MEDIA_FRAGMENT_AUDIO)
        rap = true;
    int64_t dur = end0 - pts0, pts, dts, end;
    for (;;) {
        if (m->mode == OS64_MEDIA_SEQUENCE && m->gs != NONE) {
            m->offset = m->gs - pts0;
            m->ge = m->gs;
            for (int k = 0; k < m->nt; k++) m->t[k].need_rap = true;
            m->gs = NONE;
        }
        pts = pts0 + m->offset;
        dts = dts0 + m->offset;
        end = pts + dur;
        if (t->last_dts != NONE && (dts < t->last_dts || dts - t->last_dts > 2 * t->last_dur)) {
            if (m->mode == OS64_MEDIA_SEGMENTS) m->ge = pts;
            else m->gs = m->ge;
            for (int k = 0; k < m->nt; k++) {
                m_unset(&m->t[k]);
                m->t[k].have_preload = false;
            }
            continue;
        }
        break;
    }
    if (t->last_kept != 0 && m_find_serial(t, t->last_kept) < 0) {
        t->last_kept = 0;
        t->need_rap = true;
    }
    if (pts < m->ws || end > m->we) {
        t->need_rap = true;
        if (pts < m->ws && t->kind == OS64_MEDIA_FRAGMENT_AUDIO) {
            t->preload = (mf_t){ dts, pts, dur, OS64_MEDIA_FRAME_RAP | OS64_MEDIA_FRAME_PRELOAD, gen, size, seed, 0, 0, 0 };
            t->have_preload = true;
        }
        return;
    }
    if (t->need_rap) {
        if (!rap) {
            t->have_preload = false;
            return;
        }
        t->need_rap = false;
    }
    if (t->last_dts == NONE)
        for (int i = 0; i < t->n; i++) {
            const mf_t *o = &t->f[i];
            if ((o->flags & OS64_MEDIA_FRAME_PRELOAD) || pts < o->pts || pts >= o->pts + o->dur)
                continue;
            if (t->kind == OS64_MEDIA_FRAGMENT_AUDIO || pts < o->pts + 1000)
                m_cut(t, o->pts, o->pts + 1);
            break;
        }
    if (t->highest_end == NONE)
        m_cut(t, pts, end);
    else if (t->highest_end <= pts)
        m_cut(t, t->highest_end, end);
    if (t->last_kept != 0 && m_find_serial(t, t->last_kept) < 0) {
        t->last_kept = 0;
        if (!rap) {
            t->need_rap = true;
            t->have_preload = false;
            return;
        }
    }
    if (t->last_kept == 0) {            // a group starts (or starts again at a random access point)
        t->group = ++t->groups;
        t->run = ++t->runs;
    }
    if (t->have_preload && (t->preload.pts + t->preload.dur < pts - 1000 || t->preload.pts + t->preload.dur > pts + 1000))
        t->have_preload = false;
    if (t->have_preload) {
        t->preload.serial = ++t->next_serial;
        t->preload.group = t->group;
        t->preload.run = t->run;
        m_keep(t, t->preload);
        t->have_preload = false;
    }
    mf_t k = { dts, pts, dur, rap ? OS64_MEDIA_FRAME_RAP : 0, gen, size, seed, ++t->next_serial, t->group, t->run };
    m_keep(t, k);
    if (dur > t->interbuffer) t->interbuffer = dur;
    if (t->last_dts != NONE && dts - t->last_dts > t->interbuffer) t->interbuffer = dts - t->last_dts;
    t->last_kept = k.serial;
    t->last_dts = dts;
    t->last_dur = dur;
    if (t->highest_end == NONE || end > t->highest_end) t->highest_end = end;
    if (end > m->ge) m->ge = end;
}

static uint64_t m_bytes(const mtrack_t *t)
{
    uint64_t b = 0;
    for (int i = 0; i < t->n; i++) b += t->f[i].size;
    return b;
}

// The model's ranges: each decode run's extent, their union with gaps of
// up to `join` joined: with the track's join, what the buffer reports.
static int m_ranges_join(const mtrack_t *t, int64_t join, os64_media_time_range_t *out, int cap)
{
    // Each run's extent: its first presentation to its last end (a hole a
    // cut leaves inside a run is held, as the buffer and Chromium hold it).
    os64_media_time_range_t ext[4096];
    int ne = 0;
    for (int i = 0; i < t->n; i++) {
        const mf_t *f = &t->f[i];
        if (f->flags & OS64_MEDIA_FRAME_PRELOAD) continue;
        bool seen = false;
        for (int j = 0; j < i; j++) seen |= !(t->f[j].flags & OS64_MEDIA_FRAME_PRELOAD) && t->f[j].run == f->run;
        if (seen) continue;
        os64_media_time_range_t e = { f->pts, f->pts + f->dur };
        for (int j = i; j < t->n; j++) {
            const mf_t *g = &t->f[j];
            if ((g->flags & OS64_MEDIA_FRAME_PRELOAD) || g->run != f->run) continue;
            if (g->pts < e.start) e.start = g->pts;
            if (g->pts + g->dur > e.end) e.end = g->pts + g->dur;
        }
        if (e.start < e.end && ne < 4096) ext[ne++] = e;
    }
    // Their union, gaps within the join joined.
    for (int i = 1; i < ne; i++)
        for (int j = i; j > 0 && ext[j - 1].start > ext[j].start; j--) {
            os64_media_time_range_t x = ext[j]; ext[j] = ext[j - 1]; ext[j - 1] = x;
        }
    int n = 0;
    for (int i = 0; i < ne;) {
        os64_media_time_range_t r = ext[i++];
        while (i < ne && ext[i].start <= r.end + join) {
            if (ext[i].end > r.end) r.end = ext[i].end;
            i++;
        }
        if (n < cap) out[n] = r;
        n++;
    }
    return n;
}

static int m_ranges(const mtrack_t *t, os64_media_time_range_t *out, int cap)
{
    return m_ranges_join(t, 2 * t->interbuffer, out, cap);
}

// Units in decode order: [start, end) index pairs over the sorted frames.
static int m_units(mtrack_t *t, int *starts)
{
    m_sort(t);
    int n = 0;
    for (int i = 0; i < t->n; i++)
        if (i == 0 || m_unit_start(t, i)) starts[n++] = i;
    return n;
}

static int64_t m_unit_pts(const mtrack_t *t, int s)
{
    while (s + 1 < t->n && (t->f[s].flags & OS64_MEDIA_FRAME_PRELOAD)) s++;
    return t->f[s].pts;
}

static bool m_buffered_at(const mtrack_t *t, int64_t time, os64_media_time_range_t *hold)
{
    os64_media_time_range_t r[256];
    int n = m_ranges(t, r, 256);
    for (int i = 0; i < n && i < 256; i++)
        if (time >= r[i].start && time < r[i].end) {
            if (hold) *hold = r[i];
            return true;
        }
    return false;
}

// The unit (index of its first frame) to decode from for `time`, -1 if unbuffered.
static int m_unit_for(mtrack_t *t, int64_t time)
{
    os64_media_time_range_t hold;
    if (!m_buffered_at(t, time, &hold)) return -1;
    int *st = malloc(sizeof(int) * (size_t)(t->n + 1));
    int nu = m_units(t, st), best = -1;
    for (int u = 0; u < nu; u++) {
        int64_t p = m_unit_pts(t, st[u]);
        if (p <= time && p >= hold.start - (int64_t)0 - 0 && (best < 0 || p >= m_unit_pts(t, best))) best = st[u];
    }
    if (best < 0 && nu > 0) {               // the range's first unit (its random access point after `time`'s start)
        for (int u = 0; u < nu; u++)
            if (m_unit_pts(t, st[u]) >= hold.start) { best = st[u]; break; }
    }
    free(st);
    return best;
}

static void m_evict(mtrack_t *t, uint64_t need, int64_t play)
{
    uint64_t start = m_bytes(t);
    int play_unit_serial = -1;
    uint64_t play_serial = 0;
    int64_t protect_end = OS64_MEDIA_TIME_MAX;
    if (play != NONE) {
        int u = m_unit_for(t, play);
        if (u >= 0) {
            play_serial = t->f[u].serial;
            play_unit_serial = 1;
            protect_end = play > INT64_MAX - OS64_MEDIA_PROTECT_AHEAD_NS ? INT64_MAX : play + OS64_MEDIA_PROTECT_AHEAD_NS;
        }
    }
    int *st = malloc(sizeof(int) * (size_t)(t->n + 2));
    // Behind: the first unit in decode order, while it is not the play position's.
    while (start - m_bytes(t) < need && t->n > 0) {
        int nu = m_units(t, st);
        if (play_unit_serial > 0 && t->f[st[0]].serial == play_serial) break;
        int e = nu > 1 ? st[1] : t->n;
        bool *rm = calloc((size_t)t->n, 1);
        for (int i = 0; i < e; i++) rm[i] = true;
        m_remove_marked(t, rm);
        free(rm);
    }
    if (play_unit_serial > 0)
        while (start - m_bytes(t) < need && t->n > 0) {
            int nu = m_units(t, st);
            int s = st[nu - 1];
            if (m_unit_pts(t, s) < protect_end || t->f[s].serial == play_serial) break;
            bool *rm = calloc((size_t)t->n, 1);
            for (int i = s; i < t->n; i++) rm[i] = true;
            m_remove_marked(t, rm);
            free(rm);
        }
    free(st);
}

// ── Comparing ──────────────────────────────────────────────────────────────

static int cmp_serial_frames(const void *a, const void *b)
{
    const os64_media_track_frame_t *x = a, *y = b;
    return x->serial < y->serial ? -1 : x->serial > y->serial;
}
static int cmp_serial_m(const void *a, const void *b)
{
    const mf_t *x = a, *y = b;
    return x->serial < y->serial ? -1 : x->serial > y->serial;
}

static const char *s_case = "";

static void compare(os64_media_buffer_t *b, model_t *m)
{
    for (int k = 0; k < m->nt; k++) {
        mtrack_t *mt = &m->t[k];
        os64_media_track_t *t = os64_media_buffer_track(b, mt->kind);
        if (!check(t != 0, "%s: no track of kind %d", s_case, mt->kind)) return;
        // Ranges well formed: sorted, each starting at a random access
        // point, decode order inside, extents true.
        uint32_t total = 0;
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            const os64_media_track_range_t *r = &t->range[ri];
            check(r->count > 0, "%s: an empty range", s_case);
            if (r->count == 0) continue;
            check(r->frame[0].flags & OS64_MEDIA_FRAME_RAP, "%s: a range starts at no random access point", s_case);
            for (uint32_t i = 1; i < r->count; i++)
                check(r->frame[i].dts >= r->frame[i - 1].dts, "%s: decode order broken in a range (%lld after %lld)",
                      s_case, (long long)r->frame[i].dts, (long long)r->frame[i - 1].dts);
            if (ri > 0)
                check(r->frame[0].dts >= t->range[ri - 1].frame[0].dts, "%s: ranges out of order", s_case);
            total += r->count;
        }
        os64_media_track_frame_t *got = malloc(sizeof(*got) * (total + 1));
        uint32_t n = 0;
        for (uint32_t ri = 0; ri < t->range_count; ri++)
            for (uint32_t i = 0; i < t->range[ri].count; i++) got[n++] = t->range[ri].frame[i];
        qsort(got, n, sizeof(*got), cmp_serial_frames);
        mf_t *want = malloc(sizeof(mf_t) * (size_t)(mt->n + 1));
        if (mt->n) memcpy(want, mt->f, sizeof(mf_t) * (size_t)mt->n);
        qsort(want, (size_t)mt->n, sizeof(mf_t), cmp_serial_m);
        check((int)n == mt->n, "%s: track %d keeps %u frames, the model %d", s_case, k, n, mt->n);
        for (uint32_t i = 0; i < n && (int)i < mt->n; i++) {
            const os64_media_track_frame_t *g = &got[i];
            const mf_t *w = &want[i];
            bool same = g->serial == w->serial && g->dts == w->dts && g->pts == w->pts && g->duration == w->dur &&
                        g->flags == w->flags && g->generation == w->gen && g->size == w->size;
            if (same)
                for (uint32_t j = 0; j < g->size; j++)
                    if (g->slab->data[g->offset + j] != pattern(w->seed, j)) { same = false; break; }
            if (!same) {
                for (uint32_t q = i > 3 ? i - 3 : 0; q < i + 12; q++) {
                    if (q < n) printf("  buffer serial %llu dts %lld pts %lld flags %u\n", (unsigned long long)got[q].serial,
                                      (long long)got[q].dts, (long long)got[q].pts, got[q].flags);
                    if ((int)q < mt->n) printf("  model  serial %llu dts %lld pts %lld flags %u group %u\n",
                                               (unsigned long long)want[q].serial, (long long)want[q].dts,
                                               (long long)want[q].pts, want[q].flags, want[q].group);
                }
            }
            if (!check(same, "%s: track %d frame %u differs (serial %llu/%llu dts %lld/%lld pts %lld/%lld dur %lld/%lld flags %u/%u gen %u/%u)",
                       s_case, k, i, (unsigned long long)g->serial, (unsigned long long)w->serial, (long long)g->dts,
                       (long long)w->dts, (long long)g->pts, (long long)w->pts, (long long)g->duration,
                       (long long)w->dur, g->flags, w->flags, g->generation, w->gen))
                break;
        }
        check(t->bytes == m_bytes(mt), "%s: track %d counts %llu bytes, the model %llu", s_case, k,
              (unsigned long long)t->bytes, (unsigned long long)m_bytes(mt));
        // Every kept frame's generation is held, and nothing else but the newest.
        for (uint32_t i = 0; i < n; i++) {
            bool have = false;
            for (uint32_t g = 0; g < t->gen_count; g++) have |= t->gen[g].generation == got[i].generation;
            check(have, "%s: a frame's generation %u is gone", s_case, got[i].generation);
        }
        for (uint32_t g = 0; g < t->gen_count; g++) {
            bool used = g == t->gen_count - 1 || t->gen[g].holds > 0;
            for (uint32_t i = 0; i < n && !used; i++) used = got[i].generation == t->gen[g].generation;
            check(used, "%s: generation %u kept with nothing using it", s_case, t->gen[g].generation);
        }
        // The ranges a page sees.
        os64_media_time_range_t a[256], lo[256];
        uint32_t na = os64_media_track_ranges(t, a, 256);
        int nlo = m_ranges(mt, lo, 256);
        bool ok = (int)na == nlo;
        for (int i = 0; ok && i < nlo && i < 256; i++) ok = a[i].start == lo[i].start && a[i].end == lo[i].end;
        if (!check(ok, "%s: track %d ranges differ (%u reported, the model %d)", s_case, k, na, nlo)) {
            for (uint32_t i = 0; i < t->range_count; i++) {
                const os64_media_track_range_t *r = &t->range[i];
                printf("  range %u: [%lld, %lld) %u frames, dts %lld .. %lld\n", i, (long long)r->start,
                       (long long)r->end, r->count, (long long)r->frame[0].dts, (long long)r->frame[r->count - 1].dts);
            }
            for (int i = 0; i < nlo && i < 256; i++)
                printf("  model %d: [%lld, %lld)\n", i, (long long)lo[i].start, (long long)lo[i].end);
            if (getenv("TRACK_DEBUG")) {            // every frame, both sides
                for (uint32_t ri = 0; ri < t->range_count; ri++)
                    for (uint32_t i = 0; i < t->range[ri].count; i++) {
                        const os64_media_track_frame_t *f = &t->range[ri].frame[i];
                        printf("  B r%u #%u serial %llu dts %lld pts %lld f%u\n", ri, i, (unsigned long long)f->serial,
                                   (long long)f->dts, (long long)f->pts, f->flags);
                    }
                m_sort(mt);
                for (int i = 0; i < mt->n; i++)
                    printf("  M serial %llu dts %lld pts %lld f%u g%u run%u\n", (unsigned long long)mt->f[i].serial,
                               (long long)mt->f[i].dts, (long long)mt->f[i].pts, mt->f[i].flags, mt->f[i].group, mt->f[i].run);
            }
        }
        // Inside a range no decode hole is wider than the join.
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            const os64_media_track_range_t *r = &t->range[ri];
            for (uint32_t i = 1; i < r->count; i++)
                check(r->frame[i].dts - (r->frame[i - 1].dts + r->frame[i - 1].duration) <= 2 * t->interbuffer,
                      "%s: a decode hole of %lld inside a range (the join is %lld)", s_case,
                      (long long)(r->frame[i].dts - r->frame[i - 1].dts - r->frame[i - 1].duration),
                      (long long)(2 * t->interbuffer));
        }
        // The player's queries, against scans of the model's own frames that
        // the range holding each time keeps.
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            const os64_media_track_range_t *r = &t->range[ri];
            if (r->start >= r->end) continue;
            // That range's frames, as the model has them, in decode order.
            mf_t *rf = malloc(sizeof(mf_t) * r->count);
            int nr = 0;
            for (uint32_t i = 0; i < r->count; i++) {
                int mi = m_find_serial(mt, r->frame[i].serial);
                if (mi >= 0) rf[nr++] = mt->f[mi];
            }
            qsort(rf, (size_t)nr, sizeof(mf_t), cmp_dts);
            for (int s = 0; s < 6; s++) {
                int64_t time = r->start + (r->end - r->start) * s / 6;
                uint32_t holder = UINT32_MAX;
                for (uint32_t q = 0; q < t->range_count && holder == UINT32_MAX; q++)
                    if (time >= t->range[q].start && time < t->range[q].end) holder = q;
                if (holder != ri) continue;           // another range answers for this time
                // The last unit whose random access point is presented at or before time.
                uint64_t want_rap = 0;
                int64_t want_rap_pts = 0;
                for (int i = 0; i < nr; i++) {
                    bool unit = (rf[i].flags & OS64_MEDIA_FRAME_RAP) && !(i > 0 && (rf[i - 1].flags & OS64_MEDIA_FRAME_PRELOAD));
                    if (!unit) continue;
                    int j = i;
                    while (j + 1 < nr && (rf[j].flags & OS64_MEDIA_FRAME_PRELOAD)) j++;
                    if (rf[j].pts <= time && (want_rap == 0 || rf[j].pts >= want_rap_pts)) {
                        want_rap = rf[i].serial;
                        want_rap_pts = rf[j].pts;
                    }
                }
                if (want_rap == 0 && nr) want_rap = rf[0].serial;
                os64_media_track_frame_t rap, at;
                int gr = os64_media_track_rap_at_or_before(t, time, &rap);
                check(gr == OS64_MEDIA_TRACK_FRAME && rap.serial == want_rap, "%s: track %d rap at %lld: %llu, model %llu",
                      s_case, k, (long long)time, (unsigned long long)(gr == OS64_MEDIA_TRACK_FRAME ? rap.serial : 0),
                      (unsigned long long)want_rap);
                uint64_t want_at = 0;
                int64_t want_at_pts = 0;
                for (int i = 0; i < nr; i++)
                    if (!(rf[i].flags & OS64_MEDIA_FRAME_PRELOAD) && rf[i].pts >= time &&
                        (want_at == 0 || rf[i].pts < want_at_pts)) {
                        want_at = rf[i].serial;
                        want_at_pts = rf[i].pts;
                    }
                int ga = os64_media_track_frame_at_or_after(t, time, &at);
                check((ga == OS64_MEDIA_TRACK_FRAME ? at.serial : 0) == want_at,
                      "%s: track %d frame at or after %lld: %llu, model %llu", s_case, k, (long long)time,
                      (unsigned long long)(ga == OS64_MEDIA_TRACK_FRAME ? at.serial : 0), (unsigned long long)want_at);
                int64_t end;
                bool ahead = os64_media_track_ahead(t, time, &end);
                bool in = false;
                for (uint32_t j = 0; j < na; j++) in |= time >= a[j].start && time < a[j].end && end == a[j].end;
                check(ahead && in, "%s: track %d ahead at %lld", s_case, k, (long long)time);
            }
            free(rf);
        }
        free(got);
        free(want);
    }
    // The SourceBuffer's intersection, of the tracks' reported ranges.
    if (m->nt == 2) {
        os64_media_time_range_t x[256], p[256], q[256], w[512];
        uint32_t nx = os64_media_buffer_buffered(b, x, 256);
        uint32_t np = os64_media_track_ranges(&b->track[0], p, 256), nq = os64_media_track_ranges(&b->track[1], q, 256);
        int nw = 0;
        for (uint32_t i = 0; i < np && i < 256; i++)
            for (uint32_t j = 0; j < nq && j < 256; j++) {
                int64_t s = p[i].start > q[j].start ? p[i].start : q[j].start;
                int64_t e = p[i].end < q[j].end ? p[i].end : q[j].end;
                if (s < e && nw < 512) w[nw++] = (os64_media_time_range_t){ s, e };
            }
        for (int i = 1; i < nw; i++)
            for (int j = i; j > 0 && w[j - 1].start > w[j].start; j--) {
                os64_media_time_range_t tmp = w[j]; w[j] = w[j - 1]; w[j - 1] = tmp;
            }
        bool same = (int)nx == nw;
        for (int i = 0; same && i < nw && i < 256; i++) same = x[i].start == w[i].start && x[i].end == w[i].end;
        check(same, "%s: buffered differs (%u ranges, the intersection %d)", s_case, nx, nw);
    }
}

// ── Driving both ───────────────────────────────────────────────────────────

typedef struct {
    os64_media_buffer_t b;
    model_t m;
    uint32_t gen;
    uint32_t seed;
    uint32_t ts[2];         // the timescale each track's frames come in
} pair_t;

static void pair_init(pair_t *p, int tracks, uint32_t video_ts, uint32_t audio_ts)
{
    memset(p, 0, sizeof(*p));
    os64_media_buffer_init(&p->b, &kMemory);
    m_init(&p->m);
    p->m.nt = tracks;
    p->m.t[0] = (mtrack_t){ .kind = OS64_MEDIA_FRAGMENT_VIDEO, .id = 1 };
    if (tracks == 2) p->m.t[1] = (mtrack_t){ .kind = OS64_MEDIA_FRAGMENT_AUDIO, .id = 2 };
    for (int k = 0; k < tracks; k++) {
        m_unset(&p->m.t[k]);
        p->m.t[k].quota = p->m.t[k].kind == OS64_MEDIA_FRAGMENT_VIDEO ? OS64_MEDIA_QUOTA_PICTURE : OS64_MEDIA_QUOTA_SOUND;
    }
    p->ts[0] = video_ts;
    p->ts[1] = audio_ts;
}

// An init segment for both (a new generation).
static void pair_init_segment(pair_t *p, uint32_t video_ts, uint32_t audio_ts)
{
    p->gen++;
    os64_media_fragment_init_t init = { .generation = p->gen, .track_count = (uint32_t)p->m.nt };
    static const uint8_t config[] = { 1, 0x4D, 0, 0x1F, 0xFF, 0xE1, 0, 4, 0x67, 0x4D, 0, 0x1F };
    for (int k = 0; k < p->m.nt; k++) {
        os64_media_fragment_track_t *t = &init.track[k];
        t->id = p->m.t[k].id;
        t->kind = p->m.t[k].kind;
        t->timescale = k == 0 ? video_ts : audio_ts;
        t->entry_count = 1;
        t->entry[0].config = config;
        t->entry[0].config_size = sizeof(config);
        p->ts[k] = t->timescale;
        p->m.t[k].need_rap = true;
        p->m.t[k].have_preload = false;
        p->m.t[k].newest_gen = p->gen;
    }
    os64_media_track_rule_t r = os64_media_buffer_init_segment(&p->b, &init);
    check(r == OS64_MEDIA_TRACK_OK, "%s: init segment: %s", s_case, os64_media_track_rule_name(r));
}

// A frame in its track's ticks.
typedef struct { int64_t dts, pts; uint32_t dur; bool rap; uint32_t size; } tf_t;

// Feed frames (decode order) of one track to both, from one slab (one "mdat").
static void pair_frames(pair_t *p, int track, const tf_t *f, int n)
{
    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += f[i].size;
    os64_media_fragment_slab_t *slab = slab_new(total);
    uint32_t at = 0;
    for (int i = 0; i < n; i++) {
        uint32_t seed = ++p->seed;
        for (uint32_t j = 0; j < f[i].size; j++) slab->data[at + j] = pattern(seed, j);
        os64_media_fragment_frame_t ff = {
            .track_id = p->m.t[track].id, .kind = p->m.t[track].kind, .generation = p->gen, .entry = 0,
            .timescale = p->ts[track], .dts = f[i].dts, .pts = f[i].pts, .duration = f[i].dur, .rap = f[i].rap,
            .slab = slab, .offset = at, .size = f[i].size,
        };
        os64_media_track_rule_t r = os64_media_buffer_frame(&p->b, &ff);
        int64_t pts0, dts0, end0;
        bool ok = os64_media_ticks_to_ns(f[i].pts, p->ts[track], &pts0) &&
                  os64_media_ticks_to_ns(f[i].dts, p->ts[track], &dts0) &&
                  os64_media_ticks_to_ns(f[i].pts + f[i].dur, p->ts[track], &end0);
        check(r == OS64_MEDIA_TRACK_OK && ok, "%s: frame %d: %s", s_case, i, os64_media_track_rule_name(r));
        m_frame(&p->m, p->m.t[track].id, pts0, dts0, end0, f[i].rap, p->gen, f[i].size, seed);
        at += f[i].size;
    }
    slab_end(slab);
}

// A coded frame group: `n` frames from `start` ticks, `dur` each. Pictures:
// GOPs of `gop` frames, IPPP (bframes 0) or IBBP (2): a P frame decoded
// before the two B frames it precedes in presentation, closed GOPs, the
// decode times shifted by the reorder delay. Sound: every frame alone.
static int make_group(tf_t *out, int track, int64_t start, int n, uint32_t dur, int gop, int bframes, uint32_t size0)
{
    if (track == 1 || bframes == 0) {
        for (int i = 0; i < n; i++)
            out[i] = (tf_t){ start + (int64_t)i * dur, start + (int64_t)i * dur, dur, track == 1 || i % gop == 0,
                             size0 + (uint32_t)(i * 7 % 50) };
        return n;
    }
    // IBBP within closed GOPs of `gop` frames (gop a multiple of 3 plus 1: I, then (P B B)*).
    int k = 0;
    int64_t delay = (int64_t)dur * 2;
    for (int g = 0; g < n; g += gop) {
        int len = n - g < gop ? n - g : gop;
        // presentation index p -> decode order: I(0), P(3) B(1) B(2), P(6) B(4) B(5)...
        int order[256], no = 0;
        order[no++] = 0;
        for (int p = 3; p < len + 2; p += 3) {
            int pp = p < len ? p : len - 1;
            if (pp <= order[no - 1] && no > 0 && pp != p) { /* tail shorter than a triple */ }
            if (p < len) order[no++] = p;
            for (int bq = p - 2; bq < p && bq < len; bq++) if (bq > 0) order[no++] = bq;
        }
        // any frames not placed (a short tail) in presentation order
        bool used[256] = { 0 };
        for (int i = 0; i < no; i++) used[order[i]] = true;
        for (int i = 0; i < len; i++) if (!used[i]) order[no++] = i;
        for (int i = 0; i < no; i++) {
            int pidx = g + order[i];
            out[k++] = (tf_t){ start + (int64_t)(g + i) * dur - delay, start + (int64_t)pidx * dur, dur, order[i] == 0,
                               size0 + (uint32_t)(pidx * 11 % 60) };
        }
    }
    return k;
}

static void pair_free(pair_t *p)
{
    os64_media_buffer_destroy(&p->b);
    for (int k = 0; k < 2; k++) free(p->m.t[k].f);
}

// ── Scripted cases ─────────────────────────────────────────────────────────

#define MS(x) ((int64_t)(x) * 1000000)

static void scripted(void)
{
    tf_t f[4096];
    int n;
    long live0 = s_live;

    // Times: the round trips.
    {
        uint64_t rng = 99;
        bool ok = true;
        for (int i = 0; i < 200000 && ok; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            static const uint32_t scales[] = { 1, 1000, 15360, 24000, 30000, 44100, 48000, 90000, 10000000, 1000000000,
                                               999999937 };
            uint32_t ts = scales[rng % 11];
            int64_t ticks = (int64_t)(rng >> 20) - (1ll << 43);
            int64_t ns;
            if (!os64_media_ticks_to_ns(ticks, ts, &ns)) continue;
            ok = os64_media_ns_to_ticks(ns, ts) == ticks;
            if (!ok) printf("  ticks %lld at %u -> %lld ns -> %lld\n", (long long)ticks, ts, (long long)ns,
                            (long long)os64_media_ns_to_ticks(ns, ts));
        }
        check(ok, "ticks to nanoseconds and back is exact");
        ok = true;
        for (int i = 0; i < 200000 && ok; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            int64_t ns = (int64_t)(rng % (1ull << 51)) * (rng & 1 ? 1 : -1), back;
            ok = os64_media_seconds_to_ns(os64_media_ns_to_seconds(ns), &back) && back == ns;
            if (!ok) printf("  %lld ns -> %.17g s -> %lld\n", (long long)ns, os64_media_ns_to_seconds(ns), (long long)back);
        }
        check(ok, "nanoseconds to seconds and back is exact below 2^51");
        int64_t ns;
        check(os64_media_seconds_to_ns(0.1, &ns) && ns == 100000000, "0.1 s is 100,000,000 ns (%lld)", (long long)ns);
        check(!os64_media_seconds_to_ns(1e300, &ns) && !os64_media_seconds_to_ns(0.0 / 0.0, &ns),
              "a double past int64 nanoseconds, or NaN, is refused");
        check(!os64_media_ticks_to_ns(1, 1000000001u, &ns), "a timescale finer than a nanosecond is refused");
        check(os64_media_ticks_to_ns(-1, 3, &ns) && ns == -333333333, "negative ticks round to nearest (%lld)", (long long)ns);
    }

    pair_t p;
#define START(name, tracks, vts, ats) do { s_case = name; pair_init(&p, tracks, vts, ats); pair_init_segment(&p, vts, ats); } while (0)
#define END() do { compare(&p.b, &p.m); pair_free(&p); } while (0)

    // A muxed stream, 90000 beside 48000, IBBP pictures: two segments in order.
    START("muxed 90000/48000", 2, 90000, 48000);
    for (int s = 0; s < 2; s++) {
        n = make_group(f, 0, (int64_t)s * 180000, 60, 3000, 10, 2, 400);
        pair_frames(&p, 0, f, n);
        n = make_group(f, 1, (int64_t)s * 96000, 94, 1024, 1, 0, 100);
        pair_frames(&p, 1, f, n);
    }
    compare(&p.b, &p.m);
    {
        os64_media_track_t *v = os64_media_buffer_track(&p.b, OS64_MEDIA_FRAGMENT_VIDEO);
        check(v->range_count == 1, "muxed: one picture range across two segments (%u)", v->range_count);
    }
    END();

    // A 10,000,000 timescale (100 ns ticks): exact.
    START("a 10,000,000 timescale", 1, 10000000, 0);
    n = make_group(f, 0, 0, 30, 333667, 10, 2, 300);
    pair_frames(&p, 0, f, n);
    END();

    // Out of order: segments 4, 5, 1, 2, 3, 6 make one range.
    START("out of order", 1, 90000, 0);
    { int order[] = { 3, 4, 0, 1, 2, 5 };
      for (int i = 0; i < 6; i++) { n = make_group(f, 0, (int64_t)order[i] * 30 * 3000, 30, 3000, 10, 2, 200); pair_frames(&p, 0, f, n); } }
    compare(&p.b, &p.m);
    {
        os64_media_time_range_t r[4];
        os64_media_track_t *v = os64_media_buffer_track(&p.b, OS64_MEDIA_FRAGMENT_VIDEO);
        check(os64_media_track_ranges(v, r, 4) == 1, "out of order: one buffered range at the end");
        os64_media_track_cursor_t c;
        os64_media_track_frame_t fr;
        int got = 0, rr;
        int64_t last = INT64_MIN;
        bool order = true;
        os64_media_track_seek(v, 0, &c);
        while ((rr = os64_media_track_next(v, &c, &fr)) == OS64_MEDIA_TRACK_FRAME) {
            order &= fr.dts > last;
            last = fr.dts;
            got++;
            os64_media_track_frame_done(&fr);
        }
        check(got == 180 && order && rr == OS64_MEDIA_TRACK_WAIT,
              "out of order: the cursor reads all 180 frames in decode order across the ranges (%d, %d)", got, rr);
    }
    END();

    // A quality switch over seconds 2-4 of 6: another generation replaces them.
    START("a quality switch over 2-4 s", 1, 90000, 0);
    n = make_group(f, 0, 0, 180, 3000, 10, 2, 300);
    pair_frames(&p, 0, f, n);
    pair_init_segment(&p, 15360, 0);
    n = make_group(f, 0, 2 * 15360, 60, 512, 10, 2, 150);
    pair_frames(&p, 0, f, n);
    END();

    // B-frames: one P frame replaced takes the rest of its GOP.
    START("an overlap inside a GOP", 1, 90000, 0);
    n = make_group(f, 0, 0, 40, 3000, 10, 2, 300);
    pair_frames(&p, 0, f, n);
    os64_media_buffer_reset_parser(&p.b);
    for (int k = 0; k < 1; k++) m_unset(&p.m.t[k]);
    n = make_group(f, 0, 4 * 3000, 1, 3000, 10, 0, 50);   // a lone I frame presented at frame 4
    pair_frames(&p, 0, f, n);
    END();

    // The CFP 1.13 microsecond: a group starting 1 tick into a frame, and 0.
    for (int off = 0; off < 2; off++) {
        START(off ? "a group 1 tick into a frame" : "a group 0 ticks into a frame", 1, 90000, 0);
        n = make_group(f, 0, 0, 20, 3000, 10, 0, 300);
        pair_frames(&p, 0, f, n);
        os64_media_buffer_reset_parser(&p.b);
        m_unset(&p.m.t[0]);
        n = make_group(f, 0, 9000 + off, 5, 3000, 10, 0, 40);
        pair_frames(&p, 0, f, n);
        END();
    }

    // Discontinuities: a hole of 3 frames is one, a hole of 1.9 frames is not.
    for (int big = 0; big < 2; big++) {
        START(big ? "a 3-frame hole" : "a 1.9-frame hole", 1, 90000, 0);
        n = make_group(f, 0, 0, 10, 3000, 5, 0, 100);
        f[n] = f[n - 1]; f[n].dts = f[n].pts = f[n - 1].pts + (big ? 9000 + 3000 : 5700 + 3000); f[n].rap = 0; n++;
        f[n] = f[n - 1]; f[n].dts = f[n].pts = f[n - 1].pts + 3000; f[n].rap = 1; n++;
        pair_frames(&p, 0, f, n);
        END();
    }

    // The append window, and AAC's priming as the preroll of the first frame kept.
    START("the window and the preroll", 2, 90000, 48000);
    p.b.window_start = p.m.ws = MS(0);
    n = make_group(f, 1, -1024, 50, 1024, 1, 0, 80);       // the first frame ends at 0: priming
    pair_frames(&p, 1, f, n);
    n = make_group(f, 0, -3000, 30, 3000, 10, 0, 200);      // a picture before the window is dropped
    pair_frames(&p, 0, f, n);
    compare(&p.b, &p.m);
    {
        os64_media_track_t *a = os64_media_buffer_track(&p.b, OS64_MEDIA_FRAGMENT_AUDIO);
        check(a->range_count && (a->range[0].frame[0].flags & OS64_MEDIA_FRAME_PRELOAD),
              "the priming frame is kept as preload ahead of the first frame");
        os64_media_time_range_t r;
        check(os64_media_track_ranges(a, &r, 1) == 1 && r.start == 0, "and is not buffered (starts at %lld)",
              (long long)r.start);
    }
    p.b.window_start = p.m.ws = MS(500);
    p.b.window_end = p.m.we = MS(800);
    os64_media_buffer_reset_parser(&p.b);
    for (int k = 0; k < 2; k++) { m_unset(&p.m.t[k]); p.m.t[k].have_preload = false; }
    n = make_group(f, 1, 48000, 50, 1024, 1, 0, 80);        // 1.0 s on: all outside [0.5, 0.8)
    pair_frames(&p, 1, f, n);
    END();

    // Sound flagged non-sync is a random access point all the same.
    START("sound flagged non-sync", 2, 90000, 48000);
    n = make_group(f, 1, 0, 20, 1024, 1, 0, 50);
    for (int i = 0; i < n; i++) f[i].rap = false;
    pair_frames(&p, 1, f, n);
    check(os64_media_buffer_track(&p.b, OS64_MEDIA_FRAGMENT_AUDIO)->bytes > 0, "non-sync sound is kept");
    END();

    // remove(): mid-GOP, a whole range, nothing.
    START("remove", 1, 90000, 0);
    n = make_group(f, 0, 0, 90, 3000, 10, 2, 200);
    pair_frames(&p, 0, f, n);
    os64_media_buffer_remove(&p.b, MS(500), MS(700)); m_cut(&p.m.t[0], MS(500), MS(700));
    compare(&p.b, &p.m);
    os64_media_buffer_remove(&p.b, MS(2900), MS(3000)); m_cut(&p.m.t[0], MS(2900), MS(3000));
    compare(&p.b, &p.m);
    os64_media_buffer_remove(&p.b, MS(10000), MS(20000)); m_cut(&p.m.t[0], MS(10000), MS(20000));
    compare(&p.b, &p.m);
    os64_media_buffer_remove(&p.b, INT64_MIN + 1, OS64_MEDIA_TIME_MAX); m_cut(&p.m.t[0], INT64_MIN + 1, OS64_MEDIA_TIME_MAX);
    END();

    // Sequence mode: two appends of the same media play end to end, and a
    // thousand one-frame groups of sound, each landing on the group start
    // (the drift against the exact sum: half a nanosecond a group at most).
    START("sequence mode", 2, 90000, 48000);
    os64_media_buffer_set_mode(&p.b, OS64_MEDIA_SEQUENCE); p.m.mode = OS64_MEDIA_SEQUENCE; p.m.gs = p.m.ge;
    for (int rep = 0; rep < 2; rep++) {
        n = make_group(f, 0, 0, 30, 3000, 10, 2, 100);
        pair_frames(&p, 0, f, n);
        n = make_group(f, 1, 0, 47, 1024, 1, 0, 40);
        pair_frames(&p, 1, f, n);
        os64_media_buffer_reset_parser(&p.b);
        p.m.gs = p.m.ge;
        for (int k = 0; k < 2; k++) { m_unset(&p.m.t[k]); p.m.t[k].have_preload = false; }
    }
    END();
    START("sequence mode, a thousand groups", 1, 48000, 0);
    p.m.t[0].kind = OS64_MEDIA_FRAGMENT_AUDIO;       // a sound-only buffer
    pair_free(&p);
    pair_init(&p, 1, 48000, 0);
    p.m.t[0].kind = OS64_MEDIA_FRAGMENT_AUDIO;
    p.m.t[0].quota = OS64_MEDIA_QUOTA_SOUND;
    {
        os64_media_fragment_init_t init = { .generation = 1, .track_count = 1 };
        static const uint8_t asc[] = { 0x11, 0x90 };
        init.track[0] = (os64_media_fragment_track_t){ .id = 1, .kind = OS64_MEDIA_FRAGMENT_AUDIO, .timescale = 48000,
                                                       .entry_count = 1 };
        init.track[0].entry[0].config = asc;
        init.track[0].entry[0].config_size = 2;
        p.gen = 1;
        os64_media_buffer_init_segment(&p.b, &init);
        os64_media_buffer_set_mode(&p.b, OS64_MEDIA_SEQUENCE);
        p.m.mode = OS64_MEDIA_SEQUENCE;
        p.m.gs = p.m.ge;
        int64_t worst = 0;
        // One frame a group: 1024/48000 s is 21,333,333.3 ns, so every
        // group end is rounded (three frames would be exactly 64 ms).
        for (int g = 0; g < 1000; g++) {
            n = make_group(f, 1, 0, 1, 1024, 1, 0, 1);
            pair_frames(&p, 0, f, n);
            // the exact start of group g: g * 1024 / 48000 s
            int64_t exact_num = (int64_t)g * 1024 * 1000000000ll;   // ns * 48000
            os64_media_track_t *t = &p.b.track[0];
            const os64_media_track_range_t *r = &t->range[t->range_count - 1];
            int64_t got = r->frame[r->count - 1].pts;
            int64_t err = got * 48000 - exact_num;
            if (err < 0) err = -err;
            if (err > worst) worst = err;
            os64_media_buffer_reset_parser(&p.b);
            p.m.gs = p.m.ge;
            m_unset(&p.m.t[0]);
            if (g % 100 == 0) compare(&p.b, &p.m);
            // keep the quota from mattering: the model and buffer both keep everything (3 bytes a group)
        }
        // worst is in ns * 48000; the bound: half a nanosecond a group.
        check(worst <= 1000 * 48000 / 2 + 48000, "sequence mode: a thousand groups drift %lld/48000 ns, within half a ns a group",
              (long long)worst);
        printf("sequence mode: after 1000 groups the worst start is %.3f ns from the exact sum\n", worst / 48000.0);
    }
    END();

    // Eviction: a quota of a few segments, the play position in the middle.
    START("eviction around the play position", 1, 90000, 0);
    p.b.track[0].quota = p.m.t[0].quota = 30000;
    for (int s = 0; s < 8; s++) {
        n = make_group(f, 0, (int64_t)s * 90000, 30, 3000, 10, 0, 200);   // a second each, ~7.4 KB
        uint32_t incoming = 0;
        for (int i = 0; i < n; i++) incoming += f[i].size;
        int64_t play = MS(3200);
        os64_media_track_rule_t r = os64_media_buffer_make_room(&p.b, incoming, play);
        mtrack_t *mt = &p.m.t[0];
        uint64_t mb = m_bytes(mt);
        if (mb + incoming > mt->quota) m_evict(mt, mb + incoming - mt->quota, play);
        bool mfits = m_bytes(mt) + incoming <= mt->quota;
        check((r == OS64_MEDIA_TRACK_OK) == mfits, "eviction, segment %d: %s, the model %s", s,
              os64_media_track_rule_name(r), mfits ? "fits" : "does not fit");
        compare(&p.b, &p.m);
        if (r == OS64_MEDIA_TRACK_OK) pair_frames(&p, 0, f, n);
    }
    END();

    // Eviction with no play position: oldest first. And QuotaExceeded.
    START("eviction with no play position", 1, 90000, 0);
    p.b.track[0].quota = p.m.t[0].quota = 20000;
    for (int s = 0; s < 4; s++) {
        n = make_group(f, 0, (int64_t)s * 90000, 30, 3000, 10, 0, 200);
        uint32_t incoming = 0;
        for (int i = 0; i < n; i++) incoming += f[i].size;
        os64_media_track_rule_t r = os64_media_buffer_make_room(&p.b, incoming, OS64_MEDIA_NO_TIME);
        mtrack_t *mt = &p.m.t[0];
        if (m_bytes(mt) + incoming > mt->quota) m_evict(mt, m_bytes(mt) + incoming - mt->quota, OS64_MEDIA_NO_TIME);
        check(r == OS64_MEDIA_TRACK_OK, "no play position, segment %d fits after eviction", s);
        pair_frames(&p, 0, f, n);
        compare(&p.b, &p.m);
    }
    check(os64_media_buffer_make_room(&p.b, 25000, OS64_MEDIA_NO_TIME) == OS64_MEDIA_TRACK_QUOTA,
          "an append larger than the quota is QuotaExceeded");
    m_evict(&p.m.t[0], m_bytes(&p.m.t[0]) + 25000 - p.m.t[0].quota, OS64_MEDIA_NO_TIME);
    END();

    check(s_live == live0, "scripted cases: %ld blocks not given back", s_live - live0);
}

// ── The cursor ─────────────────────────────────────────────────────────────

static void cursor_cases(void)
{
    tf_t f[1024];
    int n;
    pair_t p;
    long live0 = s_live;
    os64_media_track_cursor_t c;
    os64_media_track_frame_t fr, held[64];
    int nheld;

    // A quality switch appended ahead of the decoder plays through without a RESYNC.
    START("cursor: a switch ahead", 1, 90000, 0);
    n = make_group(f, 0, 0, 180, 3000, 10, 2, 100);          // 6 s
    pair_frames(&p, 0, f, n);
    os64_media_track_t *t = &p.b.track[0];
    check(os64_media_track_seek(t, MS(100), &c) == OS64_MEDIA_TRACK_FRAME, "seek");
    nheld = 0;
    for (int i = 0; i < 20; i++) {
        int r = os64_media_track_next(t, &c, &fr);
        check(r == OS64_MEDIA_TRACK_FRAME, "read %d", i);
        if (r == OS64_MEDIA_TRACK_FRAME) held[nheld++] = fr;
    }
    pair_init_segment(&p, 90000, 0);
    n = make_group(f, 0, 3 * 90000, 90, 3000, 10, 2, 60);     // seconds 3-6 replaced
    pair_frames(&p, 0, f, n);
    int64_t last_dts = held[nheld - 1].dts;
    bool resync = false, into_new = false, order = true;
    for (int i = 0; i < 400; i++) {
        int r = os64_media_track_next(t, &c, &fr);
        if (r == OS64_MEDIA_TRACK_RESYNC) { resync = true; break; }
        if (r != OS64_MEDIA_TRACK_FRAME) break;
        order &= fr.dts > last_dts;
        last_dts = fr.dts;
        into_new |= fr.generation == 2;
        os64_media_track_frame_done(&fr);
    }
    check(!resync, "cursor: a switch ahead makes no RESYNC");
    check(into_new && order, "cursor: it reads on in decode order into the new generation");
    for (int i = 0; i < nheld; i++) os64_media_track_frame_done(&held[i]);
    END();

    // A remove behind the cursor, and an eviction behind it: no RESYNC.
    START("cursor: removed behind", 1, 90000, 0);
    n = make_group(f, 0, 0, 90, 3000, 10, 2, 100);
    pair_frames(&p, 0, f, n);
    t = &p.b.track[0];
    os64_media_track_seek(t, MS(1500), &c);
    for (int i = 0; i < 5; i++) { os64_media_track_next(t, &c, &fr); os64_media_track_frame_done(&fr); }
    os64_media_buffer_remove(&p.b, 0, MS(1000)); m_cut(&p.m.t[0], 0, MS(1000));
    check(os64_media_track_next(t, &c, &fr) == OS64_MEDIA_TRACK_FRAME, "cursor: a remove behind reads on");
    os64_media_track_frame_done(&fr);
    t->quota = p.m.t[0].quota = 9000;
    os64_media_buffer_make_room(&p.b, 0, MS(1600));
    m_evict(&p.m.t[0], m_bytes(&p.m.t[0]) > 9000 ? m_bytes(&p.m.t[0]) - 9000 : 0, MS(1600));
    check(os64_media_track_next(t, &c, &fr) == OS64_MEDIA_TRACK_FRAME, "cursor: an eviction behind reads on");
    os64_media_track_frame_done(&fr);
    END();

    // The cursor's own next frame removed: RESYNC; and a frame in the
    // decoder's hands when its frame is removed keeps its bytes.
    START("cursor: its next frame removed", 1, 90000, 0);
    n = make_group(f, 0, 0, 60, 3000, 10, 2, 100);
    pair_frames(&p, 0, f, n);
    t = &p.b.track[0];
    os64_media_track_seek(t, 0, &c);
    os64_media_track_next(t, &c, &fr);                         // in the decoder's hands
    uint8_t first_byte = fr.slab->data[fr.offset];
    os64_media_buffer_remove(&p.b, 0, MS(2000)); m_cut(&p.m.t[0], 0, MS(2000));
    check(os64_media_track_next(t, &c, &held[0]) == OS64_MEDIA_TRACK_RESYNC, "cursor: its next frame gone is a RESYNC");
    check(fr.slab->data[fr.offset] == first_byte, "a frame in the decoder's hands keeps its bytes after a remove");
    os64_media_track_frame_done(&fr);
    // Seek again; reads.
    check(os64_media_track_seek(t, MS(100), &c) == OS64_MEDIA_TRACK_WAIT, "a seek into nothing waits");
    END();

    // A generation held by a decoder outlives its frames.
    START("generation held", 1, 90000, 0);
    n = make_group(f, 0, 0, 30, 3000, 10, 0, 100);
    pair_frames(&p, 0, f, n);
    t = &p.b.track[0];
    os64_media_track_generation_t g;
    check(os64_media_track_generation_hold(t, 1, &g) && g.entry[0].config_size == 12, "hold generation 1");
    pair_init_segment(&p, 90000, 0);
    os64_media_buffer_remove(&p.b, INT64_MIN + 1, OS64_MEDIA_TIME_MAX); m_cut(&p.m.t[0], INT64_MIN + 1, OS64_MEDIA_TIME_MAX);
    check(g.entry[0].config[0] == 1 && g.entry[0].config[8] == 0x67, "its configuration bytes survive its frames");
    os64_media_track_generation_drop(t, 1);
    check(t->gen_count == 1 && t->gen[0].generation == 2, "dropped, it goes; the newest stays (%u)", t->gen_count);
    END();

    check(s_live == live0, "cursor cases: %ld blocks not given back", s_live - live0);
}

// ── Out of memory ──────────────────────────────────────────────────────────

static void out_of_memory(void)
{
    tf_t f[256];
    long live0 = s_live;
    for (long k = 1; k <= 12; k++) {
        os64_media_buffer_t b;
        os64_media_buffer_init(&b, &kMemory);
        os64_media_fragment_init_t init = { .generation = 1, .track_count = 1 };
        static const uint8_t config[] = { 1, 2, 3, 4, 5, 6, 7 };
        init.track[0] = (os64_media_fragment_track_t){ .id = 1, .kind = OS64_MEDIA_FRAGMENT_VIDEO, .timescale = 90000,
                                                       .entry_count = 1 };
        init.track[0].entry[0].config = config;
        init.track[0].entry[0].config_size = 7;
        long base = s_live;
        (void)base;
        os64_media_fragment_slab_t *slab = slab_new(20000);
        s_fail_countdown = k;
        os64_media_buffer_init_segment(&b, &init);
        int n = make_group(f, 0, 0, 100, 3000, 10, 2, 100);
        for (int i = 0; i < n; i++) {
            os64_media_fragment_frame_t ff = { .track_id = 1, .kind = 1, .generation = 1, .timescale = 90000,
                                               .dts = f[i].dts, .pts = f[i].pts, .duration = f[i].dur, .rap = f[i].rap,
                                               .slab = slab, .offset = 0, .size = 10 };
            os64_media_track_rule_t r = os64_media_buffer_frame(&b, &ff);
            check(r == OS64_MEDIA_TRACK_OK || r == OS64_MEDIA_TRACK_NO_MEMORY || r == OS64_MEDIA_TRACK_UNKNOWN_TRACK,
                  "out of memory at %ld: %s", k, os64_media_track_rule_name(r));
        }
        os64_media_buffer_remove(&b, MS(300), MS(600));
        s_fail_countdown = 0;
        slab_end(slab);
        os64_media_buffer_destroy(&b);
        check(s_live == live0, "out of memory at allocation %ld: %ld blocks not given back", k, s_live - live0);
    }
}

// ── Random scripts ─────────────────────────────────────────────────────────

static uint64_t s_rng = 0x1234567;
static char s_log[1 << 16];
static size_t s_log_len;
static void oplog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_log_len < sizeof(s_log) - 256)
        s_log_len += (size_t)vsnprintf(s_log + s_log_len, sizeof(s_log) - s_log_len, fmt, ap);
    va_end(ap);
}
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }
static unsigned long s_ops;

static void random_script(void)
{
    static tf_t f[4096];
    pair_t p;
    int tracks = 1 + (int)(rnd() % 2);
    pair_init(&p, tracks, 90000, 48000);
    s_case = "random";
    pair_init_segment(&p, 90000, 48000);
    os64_media_track_cursor_t c = { 0 };
    bool cursor_on = false;
    os64_media_track_frame_t last = { 0 };
    int64_t vpos = 0, apos = 0;                     // where the next in-order segment begins (ticks)
    for (int op = 0; op < 120; op++, s_ops++) {
        switch (rnd() % 12) {
        case 0: case 1: case 2: case 3: case 4: {   // append a segment to a track
            int track = tracks == 2 ? (int)(rnd() % 2) : 0;
            int64_t start;
            int64_t *pos = track == 0 ? &vpos : &apos;
            uint32_t dur = track == 0 ? 3000 : 1024;
            uint32_t sec = track == 0 ? 90000 : 48000;
            switch (rnd() % 4) {
            case 0: start = *pos; break;                                         // in order
            case 1: start = (int64_t)(rnd() % 10) * sec; break;                  // a whole second somewhere
            case 2: start = *pos + (int64_t)(2 + rnd() % 3) * sec; break;        // past a gap
            default: start = *pos - (int64_t)(rnd() % 30) * dur; break;          // overlapping the end
            }
            int nf = track == 0 ? 10 * (1 + (int)(rnd() % 4)) : 47 * (1 + (int)(rnd() % 2));
            int n = make_group(f, track, start, nf, dur, 10, rnd() % 2 ? 2 : 0, 50 + (uint32_t)(rnd() % 100));
            if (start == *pos) *pos = start + (int64_t)nf * dur;
            uint32_t incoming = 0;
            for (int i = 0; i < n; i++) incoming += f[i].size;
            int64_t play = rnd() % 3 == 0 ? OS64_MEDIA_NO_TIME : (int64_t)(rnd() % 12) * 1000000000ll;
            os64_media_track_rule_t r = os64_media_buffer_make_room(&p.b, incoming, play);
            uint64_t total_quota = 0;
            for (int k = 0; k < tracks; k++) total_quota += p.m.t[k].quota;
            bool mfits = true;
            for (int k = 0; k < tracks; k++) {
                mtrack_t *mt = &p.m.t[k];
                uint64_t share = incoming / total_quota * mt->quota + incoming % total_quota * mt->quota / total_quota;
                if (m_bytes(mt) + share > mt->quota) m_evict(mt, m_bytes(mt) + share - mt->quota, play);
                if (m_bytes(mt) + share > mt->quota) mfits = false;
            }
            check((r == OS64_MEDIA_TRACK_OK) == mfits, "random: make_room %s, the model %s",
                  os64_media_track_rule_name(r), mfits ? "fits" : "does not");
            oplog("append track %d start %lld n %d dur %u -> %s (offset %lld window %lld..%lld)\n", track,
                  (long long)start, nf, dur, os64_media_track_rule_name(r), (long long)p.b.offset,
                  (long long)p.b.window_start, (long long)p.b.window_end);
            if (r == OS64_MEDIA_TRACK_OK) pair_frames(&p, track, f, n);
            break;
        }
        case 5: {                                     // remove
            int64_t a = (int64_t)(rnd() % 12000) * 1000000ll, len = (int64_t)(rnd() % 3000) * 1000000ll;
            os64_media_buffer_remove(&p.b, a, a + len);
            oplog("remove %lld %lld\n", (long long)a, (long long)(a + len));
            for (int k = 0; k < tracks; k++) m_cut(&p.m.t[k], a, a + len);
            break;
        }
        case 6:                                       // the window
            if (rnd() % 2) { p.b.window_start = p.m.ws = 0; p.b.window_end = p.m.we = OS64_MEDIA_TIME_MAX; }
            else { p.b.window_start = p.m.ws = (int64_t)(rnd() % 5) * 1000000000ll;
                   p.b.window_end = p.m.we = p.m.ws + (int64_t)(1 + rnd() % 6) * 1000000000ll; }
            oplog("window %lld %lld\n", (long long)p.b.window_start, (long long)p.b.window_end);
            break;
        case 7:                                       // the offset
            p.b.offset = p.m.offset = rnd() % 3 ? 0 : ((int64_t)(rnd() % 4000) - 2000) * 1000000ll;
            oplog("offset %lld\n", (long long)p.b.offset);
            break;
        case 8:                                       // a parser reset (abort)
            os64_media_buffer_reset_parser(&p.b);
            oplog("reset parser\n");
            if (p.m.mode == OS64_MEDIA_SEQUENCE) p.m.gs = p.m.ge;
            for (int k = 0; k < tracks; k++) { m_unset(&p.m.t[k]); p.m.t[k].have_preload = false; }
            break;
        case 9:                                       // the mode
            if (rnd() % 4 == 0) {
                int mode = (int)(rnd() % 2);
                os64_media_buffer_set_mode(&p.b, mode);
                oplog("mode %d\n", mode);
                p.m.mode = mode;
                if (mode == OS64_MEDIA_SEQUENCE) p.m.gs = p.m.ge;
            }
            break;
        case 10:                                      // a new init segment
            if (rnd() % 3 == 0) { uint32_t ts = rnd() % 2 ? 90000 : 15360; pair_init_segment(&p, ts, 48000); oplog("init %u\n", ts); }
            break;
        default: {                                    // the cursor
            os64_media_track_t *t = &p.b.track[0];
            mtrack_t *mt = &p.m.t[0];
            if (!cursor_on || rnd() % 5 == 0) {
                int64_t at = (int64_t)(rnd() % 10) * 1000000000ll;
                cursor_on = os64_media_track_seek(t, at, &c) == OS64_MEDIA_TRACK_FRAME;
                last.serial = 0;
            }
            for (int i = 0; cursor_on && i < 8; i++) {
                bool had_last = last.serial != 0;
                bool last_present = had_last && m_find_serial(mt, last.serial) >= 0;
                bool next_present = c.next_serial == 0 || m_find_serial(mt, c.next_serial) >= 0;
                os64_media_track_frame_t fr;
                int r = os64_media_track_next(t, &c, &fr);
                if (r == OS64_MEDIA_TRACK_RESYNC) {
                    check((had_last && !last_present) || !next_present || c.edits != t->edits || true,
                          "random: a RESYNC with nothing gone");
                    // Strict: something the cursor knew must be gone, or the frame after it changed.
                    m_sort(mt);
                    bool gone = (had_last && !last_present) || !next_present;
                    if (!gone && had_last) {
                        int li = m_find_serial(mt, last.serial);
                        gone = li + 1 < mt->n && c.next_serial != 0 && mt->f[li + 1].serial != c.next_serial;
                    }
                    check(gone, "random: a RESYNC with the cursor's frames still in place");
                    cursor_on = false;
                    break;
                }
                if (r == OS64_MEDIA_TRACK_WAIT) break;
                int fi = m_find_serial(mt, fr.serial);
                check(fi >= 0, "random: the cursor returned a frame the model has not got");
                if (had_last && last_present && fi >= 0) {
                    // Within a coded frame group: the group's next frame, none
                    // skipped. Into another group (the range's next, or a
                    // range the join joins): a random access point.
                    m_sort(mt);
                    int li = m_find_serial(mt, last.serial);
                    fi = m_find_serial(mt, fr.serial);
                    if (mt->f[fi].group == mt->f[li].group) {
                        int want = li + 1;
                        while (want < mt->n && mt->f[want].group != mt->f[li].group) want++;
                        check(fi == want, "random: the cursor skipped or went back in a group (model %d after %d)", fi, li);
                    } else {
                        check((mt->f[fi].flags & OS64_MEDIA_FRAME_RAP) != 0,
                              "random: the cursor went into another group at no random access point");
                    }
                }
                last = fr;
                os64_media_track_frame_done(&fr);
            }
            break;
        }
        }
        compare(&p.b, &p.m);
        if (s_failed) {
            printf("the operations, the last failing:\n%s", s_log);
            break;
        }
    }
    s_log_len = 0;
    s_log[0] = 0;
    pair_free(&p);
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 2;
    scripted();
    cursor_cases();
    out_of_memory();
    long live0 = s_live;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long scripts = 0;
    do {
        random_script();
        scripts++;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (!s_failed && (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9 < seconds);
    check(s_live == live0, "random scripts: %ld blocks not given back", s_live - live0);
    printf("random scripts: %lu scripts, %lu operations\n", scripts, s_ops);
    printf("media track: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
