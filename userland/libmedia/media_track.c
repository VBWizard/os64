// media_track.c — a SourceBuffer's track buffers and the spec's coded frame
// processing over them (os64/media_track.h; MEDIA_SOURCE.md § 4).
//
// A track keeps RANGES: each a run of frames in decode order that starts at
// a random access point, made of coded frame groups (or what a cut left of
// them). A group goes on the end of the range it continues, in decode time
// and on the screen, within the JOIN (twice the longest frame duration or
// decode step the track has seen: Chromium's fudge room), or starts a
// range; removing frames from inside a range splits it. Ranges never merge: a merge while a group is still arriving
// would let the group's later frames interleave in decode time with the
// next range's (a rendition without B-frames ending where one with them
// begins), and a cut of one chain would then run on through the other.
// Instead the join is applied when ranges are READ: `buffered` joins ranges
// whose presentation gap is within the join, and a cursor at a range's end
// goes on into the range that begins within the join. So the two agree by
// construction, appending costs nothing per frame beyond its record, and
// an out-of-order segment costs nothing at all.
//
// A UNIT is what is never split: a random access point and what decodes
// from it up to the next one, and a PRELOAD frame with the frame it is the
// preroll of. Every cut (overlap, remove, eviction) lands on a unit
// boundary: a random access point not preceded by a preload frame.
//
// "CFP n" below is MSE's coded frame processing, list item n of step 1.

#include "os64/media_track.h"

#define NS_PER_SECOND 1000000000ll
#define ONE_MICROSECOND 1000ll

// ── Times ──────────────────────────────────────────────────────────────────

bool os64_media_ticks_to_ns(int64_t ticks, uint32_t timescale, int64_t *ns)
{
    if (timescale == 0 || timescale > OS64_MEDIA_MAX_TIMESCALE)
        return false;
    int64_t q = ticks / (int64_t)timescale, r = ticks % (int64_t)timescale;
    if (r < 0) {
        r += timescale;
        q--;
    }
    // r < 2^30 (the timescale is at most 1e9) and so is 1e9: r * 1e9 fits.
    // Nearest, halves up.
    int64_t part = (r * NS_PER_SECOND + (int64_t)(timescale / 2)) / (int64_t)timescale, whole;
    if (__builtin_mul_overflow(q, NS_PER_SECOND, &whole) || __builtin_add_overflow(whole, part, ns))
        return false;
    return true;
}

int64_t os64_media_ns_to_ticks(int64_t ns, uint32_t timescale)
{
    int64_t q = ns / NS_PER_SECOND, r = ns % NS_PER_SECOND;
    if (r < 0) {
        r += NS_PER_SECOND;
        q--;
    }
    return q * (int64_t)timescale + (r * (int64_t)timescale + NS_PER_SECOND / 2) / NS_PER_SECOND;
}

bool os64_media_seconds_to_ns(double seconds, int64_t *ns)
{
    double x = seconds * 1e9;
    if (!(x > -9.2e18 && x < 9.2e18))           // false for NaN too
        return false;
    int64_t n = (int64_t)x;                     // toward zero
    double frac = x - (double)n;
    if (frac >= 0.5)
        n++;
    else if (frac <= -0.5)
        n--;
    *ns = n;
    return true;
}

double os64_media_ns_to_seconds(int64_t ns)
{
    return (double)ns / 1e9;
}

// ── Memory ─────────────────────────────────────────────────────────────────

static void *mem_alloc(const os64_media_track_t *t, size_t bytes)
{
    return t->memory.alloc(t->memory.context, bytes);
}

static void mem_release(const os64_media_track_t *t, void *block)
{
    if (block)
        t->memory.release(t->memory.context, block);
}

// ── Generations ────────────────────────────────────────────────────────────

static os64_media_track_generation_t *gen_find(const os64_media_track_t *t, uint32_t generation, uint32_t *index)
{
    for (uint32_t i = 0; i < t->gen_count; i++)
        if (t->gen[i].generation == generation) {
            if (index)
                *index = i;
            return &t->gen[i];
        }
    return 0;
}

// A generation that no frame and no decoder keeps, and that is not the
// newest (the frames still to come are its), goes.
static void gen_maybe_free(os64_media_track_t *t, uint32_t index)
{
    os64_media_track_generation_t *g = &t->gen[index];
    if (g->frames != 0 || g->holds != 0 || index == t->gen_count - 1)
        return;
    mem_release(t, g->block);
    for (uint32_t i = index + 1; i < t->gen_count; i++)
        t->gen[i - 1] = t->gen[i];
    t->gen_count--;
}

// ── Frames and ranges ──────────────────────────────────────────────────────

// A kept frame leaves the buffer: its hold on its slab, its generation's
// count and its bytes go with it.
static void frame_forget(os64_media_track_t *t, const os64_media_track_frame_t *f)
{
    t->bytes -= f->size;
    uint32_t gi;
    if (gen_find(t, f->generation, &gi)) {
        t->gen[gi].frames--;
        gen_maybe_free(t, gi);
    }
    os64_media_fragment_slab_drop(f->slab);
}

static bool is_unit_start(const os64_media_track_range_t *r, uint32_t i)
{
    return (r->frame[i].flags & OS64_MEDIA_FRAME_RAP) &&
           !(i > 0 && (r->frame[i - 1].flags & OS64_MEDIA_FRAME_PRELOAD));
}

// The first unit boundary after index i (r->count if there is none).
static uint32_t next_unit(const os64_media_track_range_t *r, uint32_t i)
{
    uint32_t j = i + 1;
    while (j < r->count && !is_unit_start(r, j))
        j++;
    return j;
}

// The unit boundary at or before index i.
static uint32_t unit_of(const os64_media_track_range_t *r, uint32_t i)
{
    while (i > 0 && !is_unit_start(r, i))
        i--;
    return i;
}

// A range's presentation extent: its earliest presentation to its latest
// end, its preload frames left out. A hole a cut leaves inside (frames
// decoded before the cut but presented after it) counts as held, as
// Chromium counts it: the range decodes through it and the picture before
// the hole stays up, so `buffered` says what plays.
static void range_extent(os64_media_track_range_t *r)
{
    r->start = OS64_MEDIA_TIME_MAX;
    r->end = OS64_MEDIA_NO_TIME;
    for (uint32_t i = 0; i < r->count; i++) {
        const os64_media_track_frame_t *f = &r->frame[i];
        if (f->flags & OS64_MEDIA_FRAME_PRELOAD)
            continue;
        if (f->pts < r->start)
            r->start = f->pts;
        if (f->pts + f->duration > r->end)
            r->end = f->pts + f->duration;
    }
    if (r->start > r->end)              // preload frames only: an empty extent
        r->start = r->end = r->count ? r->frame[r->count - 1].pts : 0;
}

static os64_media_track_range_t *range_by_id(const os64_media_track_t *t, uint64_t id, uint32_t *index)
{
    for (uint32_t i = 0; i < t->range_count; i++)
        if (t->range[i].id == id) {
            if (index)
                *index = i;
            return &t->range[i];
        }
    return 0;
}

static bool range_reserve(os64_media_track_t *t, os64_media_track_range_t *r, uint32_t want)
{
    if (want <= r->capacity)
        return true;
    uint32_t cap = r->capacity ? r->capacity : 64;
    while (cap < want)
        cap *= 2;
    os64_media_track_frame_t *grown = mem_alloc(t, (size_t)cap * sizeof(*grown));
    if (grown == 0)
        return false;
    for (uint32_t i = 0; i < r->count; i++)
        grown[i] = r->frame[i];
    mem_release(t, r->frame);
    r->frame = grown;
    r->capacity = cap;
    return true;
}

// A new, empty range at position `at` of the sorted list, or NULL.
static os64_media_track_range_t *range_insert(os64_media_track_t *t, uint32_t at)
{
    if (t->range_count == t->range_capacity) {
        uint32_t cap = t->range_capacity ? t->range_capacity * 2 : 8;
        os64_media_track_range_t *grown = mem_alloc(t, (size_t)cap * sizeof(*grown));
        if (grown == 0)
            return 0;
        for (uint32_t i = 0; i < t->range_count; i++)
            grown[i] = t->range[i];
        mem_release(t, t->range);
        t->range = grown;
        t->range_capacity = cap;
    }
    for (uint32_t i = t->range_count; i > at; i--)
        t->range[i] = t->range[i - 1];
    t->range_count++;
    t->range[at] = (os64_media_track_range_t){ .id = ++t->next_range_id };
    return &t->range[at];
}

static void range_delete(os64_media_track_t *t, uint32_t index)
{
    mem_release(t, t->range[index].frame);
    if (t->append_range == t->range[index].id)
        t->append_range = 0;
    for (uint32_t i = index + 1; i < t->range_count; i++)
        t->range[i - 1] = t->range[i];
    t->range_count--;
}

// Range `ri` moved to its place in the order of first decode times (a cut
// from its front, or a split's tail, can belong further on).
static void range_resort(os64_media_track_t *t, uint32_t ri)
{
    while (ri + 1 < t->range_count && t->range[ri + 1].frame[0].dts < t->range[ri].frame[0].dts) {
        os64_media_track_range_t x = t->range[ri];
        t->range[ri] = t->range[ri + 1];
        t->range[ri + 1] = x;
        ri++;
    }
}

// Frames [a, b) of range `ri` go (b a unit boundary or the end). What is
// before them stays the range; what is after them becomes a range of its
// own, unless there is no memory for one: then it goes too, which a cut
// may always do (it is eviction ahead of time). Answers true when the range
// is still at `ri`; false when it went, or a cut from its front may have
// moved it on (look it up again).
static bool range_cut(os64_media_track_t *t, uint32_t ri, uint32_t a, uint32_t b)
{
    if (a >= b)
        return true;
    t->edits++;
    os64_media_track_range_t *r = &t->range[ri];
    if (a > 0 && b < r->count) {
        uint32_t tail_count = r->count - b;
        os64_media_track_range_t *tail = range_insert(t, ri + 1);
        r = &t->range[ri];
        if (tail != 0 && range_reserve(t, tail, tail_count)) {
            for (uint32_t i = b; i < r->count; i++)
                tail->frame[tail->count++] = r->frame[i];
            range_extent(tail);
            r->count = b;
            // A coded frame group still arriving goes on where its last frame went.
            if (t->append_range == r->id && tail->frame[tail->count - 1].serial == t->last_kept)
                t->append_range = tail->id;
            range_resort(t, ri + 1);
        } else {
            if (tail != 0)
                range_delete(t, ri + 1);
            r = &t->range[ri];
            b = r->count;               // no room for the tail: it goes too
        }
    }
    for (uint32_t i = a; i < b; i++)
        frame_forget(t, &r->frame[i]);
    if (a == 0 && b >= r->count) {
        range_delete(t, ri);
        return false;
    }
    if (b >= r->count) {
        r->count = a;
    } else {                            // a == 0: the front goes
        for (uint32_t i = b; i < r->count; i++)
            r->frame[i - b] = r->frame[i];
        r->count -= b;
    }
    range_extent(r);
    if (a == 0) {
        range_resort(t, ri);
        return false;                   // it may have moved on: the caller looks it up again
    }
    return true;
}

// Remove from every range the frames presented in [lo, hi) and every frame
// that depends on them (CFP 1.14-1.15, range removal): one span in decode
// order a range, from the first selected (with a preload frame before it)
// to the next unit after the last selected. Presentation order chooses;
// decode order cuts.
static void cut_presented(os64_media_track_t *t, int64_t lo, int64_t hi)
{
    if (lo >= hi)
        return;
    for (uint32_t ri = 0; ri < t->range_count;) {
        os64_media_track_range_t *r = &t->range[ri];
        bool front_preload = r->count && (r->frame[0].flags & OS64_MEDIA_FRAME_PRELOAD) && r->frame[0].pts < hi;
        if (r->end <= lo || (r->start >= hi && !front_preload)) {
            ri++;
            continue;
        }
        uint32_t first = UINT32_MAX, last = 0;
        for (uint32_t i = 0; i < r->count; i++)
            if (r->frame[i].pts >= lo && r->frame[i].pts < hi) {
                if (first == UINT32_MAX)
                    first = i;
                last = i;
            }
        if (first == UINT32_MAX) {
            ri++;
            continue;
        }
        if (first > 0 && (r->frame[first - 1].flags & OS64_MEDIA_FRAME_PRELOAD))
            first--;
        // Nothing selected is left in this range afterwards (the cut runs
        // past the last), so the scan moves on; a split's tail is looked at
        // next, and has nothing selected either.
        if (range_cut(t, ri, first, next_unit(r, last)))
            ri++;
    }
}

// The join: the widest gap, in decode time or on the screen, that still
// counts as continuous.
static int64_t join_of(const os64_media_track_t *t)
{
    return 2 * t->interbuffer;
}

// Where a coded frame group's first frame goes: the range it continues
// within the join in decode order AND in presentation (B-frames put decode
// times ahead of presentation, so a decode gap within the join can be a
// wider hole on the screen), or a new one in its sorted place.
static os64_media_track_range_t *range_for_group(os64_media_track_t *t, int64_t dts, int64_t pts, uint32_t *index)
{
    uint32_t at = 0;
    while (at < t->range_count && t->range[at].count && t->range[at].frame[0].dts <= dts)
        at++;
    if (at > 0) {
        os64_media_track_range_t *p = &t->range[at - 1];
        const os64_media_track_frame_t *last = &p->frame[p->count - 1];
        if (last->dts <= dts && dts <= last->dts + last->duration + join_of(t) && pts <= p->end + join_of(t)) {
            *index = at - 1;
            return p;
        }
    }
    os64_media_track_range_t *r = range_insert(t, at);
    *index = at;
    return r;
}

// Whether the group's last kept frame is still its range's last: frames
// after it depended on it, so a group whose last frame was cut goes on only
// from a random access point. A group that has kept nothing yet is intact.
static bool group_intact(const os64_media_track_t *t, const os64_media_track_range_t *r)
{
    return t->last_kept == 0 || (r->count > 0 && r->frame[r->count - 1].serial == t->last_kept);
}

// Keep a frame at the end of a range (its slab hold is already taken).
static bool range_push(os64_media_track_t *t, os64_media_track_range_t *r, const os64_media_track_frame_t *f)
{
    if (!range_reserve(t, r, r->count + 1))
        return false;
    r->frame[r->count++] = *f;
    if (!(f->flags & OS64_MEDIA_FRAME_PRELOAD)) {
        if (r->start >= r->end) {       // the first frame with an extent
            r->start = f->pts;
            r->end = f->pts + f->duration;
        } else {
            if (f->pts < r->start)
                r->start = f->pts;
            if (f->pts + f->duration > r->end)
                r->end = f->pts + f->duration;
        }
    }
    t->bytes += f->size;
    os64_media_track_generation_t *g = gen_find(t, f->generation, 0);
    if (g)
        g->frames++;
    return true;
}

static void preload_clear(os64_media_track_t *t)
{
    if (t->have_preload)
        os64_media_fragment_slab_drop(t->preload.slab);
    t->have_preload = false;
}

// ── The SourceBuffer ───────────────────────────────────────────────────────

void os64_media_buffer_init(os64_media_buffer_t *b, const os64_media_track_memory_t *memory)
{
    *b = (os64_media_buffer_t){ 0 };
    b->memory = *memory;
    b->window_start = 0;
    b->window_end = OS64_MEDIA_TIME_MAX;
    b->group_start = OS64_MEDIA_NO_TIME;
    b->group_end = 0;
}

static void track_unset(os64_media_track_t *t)
{
    t->last_dts = t->last_duration = t->highest_end = OS64_MEDIA_NO_TIME;
    t->need_rap = true;
    t->append_range = 0;
    t->last_kept = 0;
}

void os64_media_buffer_destroy(os64_media_buffer_t *b)
{
    for (uint32_t k = 0; k < b->track_count; k++) {
        os64_media_track_t *t = &b->track[k];
        preload_clear(t);
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            for (uint32_t i = 0; i < t->range[ri].count; i++)
                os64_media_fragment_slab_drop(t->range[ri].frame[i].slab);
            mem_release(t, t->range[ri].frame);
        }
        mem_release(t, t->range);
        for (uint32_t i = 0; i < t->gen_count; i++)
            mem_release(t, t->gen[i].block);
        mem_release(t, t->gen);
        *t = (os64_media_track_t){ 0 };
    }
    b->track_count = 0;
}

os64_media_track_t *os64_media_buffer_track(os64_media_buffer_t *b, int kind)
{
    for (uint32_t k = 0; k < b->track_count; k++)
        if (b->track[k].kind == kind)
            return &b->track[k];
    return 0;
}

os64_media_track_rule_t os64_media_buffer_init_segment(os64_media_buffer_t *b, const os64_media_fragment_init_t *init)
{
    for (uint32_t i = 0; i < init->track_count; i++)
        if (init->track[i].timescale == 0 || init->track[i].timescale > OS64_MEDIA_MAX_TIMESCALE)
            return OS64_MEDIA_TRACK_TIMESCALE;
    if (b->track_count == 0) {
        for (uint32_t i = 0; i < init->track_count; i++) {
            os64_media_track_t *t = &b->track[b->track_count++];
            *t = (os64_media_track_t){ .memory = b->memory, .kind = init->track[i].kind };
            t->quota = t->kind == OS64_MEDIA_FRAGMENT_VIDEO ? OS64_MEDIA_QUOTA_PICTURE : OS64_MEDIA_QUOTA_SOUND;
            track_unset(t);
        }
    }
    for (uint32_t i = 0; i < init->track_count; i++) {
        const os64_media_fragment_track_t *src = &init->track[i];
        os64_media_track_t *t = os64_media_buffer_track(b, src->kind);
        if (t == 0)
            return OS64_MEDIA_TRACK_UNKNOWN_TRACK;
        // The configurations, copied into one block (the parser's are gone
        // at its next init segment).
        size_t bytes = 1;
        for (uint32_t e = 0; e < src->entry_count; e++)
            bytes += src->entry[e].config_size;
        if (t->gen_count == t->gen_capacity) {
            uint32_t cap = t->gen_capacity ? t->gen_capacity * 2 : 4;
            os64_media_track_generation_t *grown = mem_alloc(t, (size_t)cap * sizeof(*grown));
            if (grown == 0)
                return OS64_MEDIA_TRACK_NO_MEMORY;
            for (uint32_t g = 0; g < t->gen_count; g++)
                grown[g] = t->gen[g];
            mem_release(t, t->gen);
            t->gen = grown;
            t->gen_capacity = cap;
        }
        uint8_t *block = mem_alloc(t, bytes);
        if (block == 0)
            return OS64_MEDIA_TRACK_NO_MEMORY;
        os64_media_track_generation_t *g = &t->gen[t->gen_count++];
        *g = (os64_media_track_generation_t){ .generation = init->generation, .entry_count = src->entry_count,
                                              .block = block };
        size_t at = 0;
        for (uint32_t e = 0; e < src->entry_count; e++) {
            g->entry[e] = src->entry[e];
            for (uint32_t k = 0; k < src->entry[e].config_size; k++)
                block[at + k] = src->entry[e].config[k];
            g->entry[e].config = block + at;
            at += src->entry[e].config_size;
        }
        t->id = src->id;
        t->need_rap = true;             // MSE § 5.5.7
        preload_clear(t);
        // The generation before may now be one nothing keeps.
        if (t->gen_count >= 2)
            gen_maybe_free(t, t->gen_count - 2);
    }
    return OS64_MEDIA_TRACK_OK;
}

static os64_media_track_t *track_by_id(os64_media_buffer_t *b, uint32_t id)
{
    for (uint32_t k = 0; k < b->track_count; k++)
        if (b->track[k].id == id)
            return &b->track[k];
    return 0;
}

os64_media_track_rule_t os64_media_buffer_frame(os64_media_buffer_t *b, const os64_media_fragment_frame_t *f)
{
    os64_media_track_t *t = track_by_id(b, f->track_id);
    if (t == 0)
        return OS64_MEDIA_TRACK_UNKNOWN_TRACK;
    int64_t pts0, dts0, end0, end_ticks, duration;
    if (f->timescale == 0 || f->timescale > OS64_MEDIA_MAX_TIMESCALE)
        return OS64_MEDIA_TRACK_TIMESCALE;
    // The duration is the converted end less the converted start, so that
    // a frame ends exactly where the frame after it begins (each tick value
    // is rounded once; a duration rounded on its own, 1024/48000 s being
    // 21,333,333.3 ns, would leave a nanosecond's seam or overlap).
    if (__builtin_add_overflow(f->pts, (int64_t)f->duration, &end_ticks) ||
        !os64_media_ticks_to_ns(f->pts, f->timescale, &pts0) || !os64_media_ticks_to_ns(f->dts, f->timescale, &dts0) ||
        !os64_media_ticks_to_ns(end_ticks, f->timescale, &end0))
        return OS64_MEDIA_TRACK_TIME_OVERFLOW;
    duration = end0 - pts0;
    bool rap = f->rap || t->kind == OS64_MEDIA_FRAGMENT_AUDIO;   // every sound frame is one (§ 6)
    int64_t pts, dts, end;
    for (;;) {
        // CFP 1.3: sequence mode, a group's first frame lands on the group start.
        if (b->mode == OS64_MEDIA_SEQUENCE && b->group_start != OS64_MEDIA_NO_TIME) {
            b->offset = b->group_start - pts0;
            b->group_end = b->group_start;
            for (uint32_t k = 0; k < b->track_count; k++)
                b->track[k].need_rap = true;
            b->group_start = OS64_MEDIA_NO_TIME;
        }
        // CFP 1.4
        if (__builtin_add_overflow(pts0, b->offset, &pts) || __builtin_add_overflow(dts0, b->offset, &dts) ||
            __builtin_add_overflow(pts, duration, &end))
            return OS64_MEDIA_TRACK_TIME_OVERFLOW;
        // CFP 1.6: a decode time that steps back, or forward by more than
        // twice the last duration, starts a new coded frame group.
        if (t->last_dts != OS64_MEDIA_NO_TIME &&
            (dts < t->last_dts || dts - t->last_dts > 2 * t->last_duration)) {
            if (b->mode == OS64_MEDIA_SEGMENTS)
                b->group_end = pts;
            else
                b->group_start = b->group_end;
            for (uint32_t k = 0; k < b->track_count; k++) {
                track_unset(&b->track[k]);
                preload_clear(&b->track[k]);
            }
            continue;                   // CFP 1.6.6: back to the loop's top
        }
        break;
    }
    // A coded frame group whose last kept frame was removed under it starts
    // again at a random access point: what follows depended on it.
    if (t->append_range != 0) {
        const os64_media_track_range_t *ar = range_by_id(t, t->append_range, 0);
        if (ar == 0 || !group_intact(t, ar)) {
            t->append_range = 0;
            t->need_rap = true;
        }
    }
    // CFP 1.8-1.9: the append window. The sound frame just before the
    // window's start is kept aside as the next kept frame's preroll.
    if (pts < b->window_start || end > b->window_end) {
        t->need_rap = true;
        if (pts < b->window_start && t->kind == OS64_MEDIA_FRAGMENT_AUDIO) {
            preload_clear(t);
            t->preload = (os64_media_track_frame_t){ .dts = dts, .pts = pts, .duration = duration, .slab = f->slab,
                                                     .offset = f->offset, .size = f->size,
                                                     .generation = f->generation, .entry = f->entry,
                                                     .flags = OS64_MEDIA_FRAME_RAP | OS64_MEDIA_FRAME_PRELOAD };
            os64_media_fragment_slab_hold(f->slab);
            t->have_preload = true;
        }
        return OS64_MEDIA_TRACK_OK;
    }
    // CFP 1.10
    if (t->need_rap) {
        if (!rap) {
            preload_clear(t);
            return OS64_MEDIA_TRACK_OK;
        }
        t->need_rap = false;
    }
    // CFP 1.13: a group's first frame starting inside a kept frame. A
    // picture removes it when it starts within a microsecond of it; a sound
    // frame always does (no splice in v1: a cut).
    if (t->last_dts == OS64_MEDIA_NO_TIME) {
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            os64_media_track_range_t *r = &t->range[ri];
            if (pts < r->start || pts >= r->end)
                continue;
            for (uint32_t i = 0; i < r->count; i++) {
                const os64_media_track_frame_t *o = &r->frame[i];
                if ((o->flags & OS64_MEDIA_FRAME_PRELOAD) || pts < o->pts || pts >= o->pts + o->duration)
                    continue;
                if (t->kind == OS64_MEDIA_FRAGMENT_AUDIO || pts < o->pts + ONE_MICROSECOND)
                    cut_presented(t, o->pts, o->pts + 1);
                break;
            }
            break;
        }
    }
    // CFP 1.14-1.15
    if (t->highest_end == OS64_MEDIA_NO_TIME)
        cut_presented(t, pts, end);
    else if (t->highest_end <= pts)
        cut_presented(t, t->highest_end, end);
    // CFP 1.16: kept, in its group's range. (A frame never lands past the
    // join from its group's last kept frame: a decode step of more than
    // twice the last duration is a discontinuity, a new group, and the join
    // is at least that.)
    os64_media_track_range_t *r = 0;
    uint32_t ri = 0;
    if (t->append_range != 0) {
        r = range_by_id(t, t->append_range, &ri);
        if (r == 0 || !group_intact(t, r)) {
            // This frame's own removal (CFP 1.15) took the group's last kept
            // frame: what follows depended on it, so only a random access
            // point goes on, and it starts a range.
            t->append_range = 0;
            if (!rap) {
                t->need_rap = true;
                preload_clear(t);
                return OS64_MEDIA_TRACK_OK;
            }
            r = 0;
        }
    }
    if (r == 0) {
        t->last_kept = 0;
        r = range_for_group(t, dts, pts, &ri);
        if (r == 0)
            return OS64_MEDIA_TRACK_NO_MEMORY;
    }
    t->append_range = r->id;
    // The preload is the preroll of the frame it ends at: one that ends
    // anywhere else (a frame dropped long ago, before other appends) is
    // nobody's (Chromium's rule for its preroll buffer).
    if (t->have_preload && (t->preload.pts + t->preload.duration < pts - ONE_MICROSECOND ||
                            t->preload.pts + t->preload.duration > pts + ONE_MICROSECOND))
        preload_clear(t);
    if (t->have_preload) {
        t->preload.serial = ++t->next_serial;
        if (!range_push(t, r, &t->preload)) {
            if (r->count == 0)
                range_delete(t, ri);
            return OS64_MEDIA_TRACK_NO_MEMORY;
        }
        t->have_preload = false;        // its hold went with it
    }
    os64_media_track_frame_t kept = { .dts = dts, .pts = pts, .duration = duration, .slab = f->slab,
                                      .offset = f->offset, .size = f->size, .generation = f->generation,
                                      .entry = f->entry, .serial = ++t->next_serial,
                                      .flags = rap ? OS64_MEDIA_FRAME_RAP : 0 };
    os64_media_fragment_slab_hold(f->slab);
    if (!range_push(t, r, &kept)) {
        os64_media_fragment_slab_drop(f->slab);
        if (r->count == 0)
            range_delete(t, ri);
        return OS64_MEDIA_TRACK_NO_MEMORY;
    }
    int64_t step = t->last_dts == OS64_MEDIA_NO_TIME ? 0 : dts - t->last_dts;
    if (duration > t->interbuffer)
        t->interbuffer = duration;
    if (step > t->interbuffer)
        t->interbuffer = step;
    t->last_kept = kept.serial;
    // CFP 1.17-1.20
    t->last_dts = dts;
    t->last_duration = duration;
    if (t->highest_end == OS64_MEDIA_NO_TIME || end > t->highest_end)
        t->highest_end = end;
    if (end > b->group_end)
        b->group_end = end;
    return OS64_MEDIA_TRACK_OK;
}

void os64_media_buffer_reset_parser(os64_media_buffer_t *b)
{
    if (b->mode == OS64_MEDIA_SEQUENCE)
        b->group_start = b->group_end;
    for (uint32_t k = 0; k < b->track_count; k++) {
        track_unset(&b->track[k]);
        preload_clear(&b->track[k]);
    }
}

void os64_media_buffer_set_mode(os64_media_buffer_t *b, int mode)
{
    // MSE: setting "sequence" sets the group start to the group end.
    b->mode = mode;
    if (mode == OS64_MEDIA_SEQUENCE)
        b->group_start = b->group_end;
}

void os64_media_buffer_remove(os64_media_buffer_t *b, int64_t start, int64_t end)
{
    for (uint32_t k = 0; k < b->track_count; k++)
        cut_presented(&b->track[k], start, end);
}

// ── Eviction ───────────────────────────────────────────────────────────────

static const os64_media_track_range_t *range_holding(const os64_media_track_t *t, int64_t time, uint32_t *index);
static uint32_t unit_for(const os64_media_track_range_t *r, int64_t time);
static int64_t unit_pts(const os64_media_track_range_t *r, uint32_t u);

// Free at least `need` bytes of `t`: whole units from the front of the
// earliest range first, never into the unit holding the play position;
// then whole units from the back of the latest ranges, never into the
// protected stretch (the play position's unit to 10 s past it). With no
// play position, or one in no range, nothing is protected by position and
// everything goes oldest first.
static void evict(os64_media_track_t *t, uint64_t need, int64_t play)
{
    uint64_t start_bytes = t->bytes;
    int64_t protect_end = OS64_MEDIA_TIME_MAX;
    uint64_t play_range = 0;
    uint32_t play_unit = 0;
    const os64_media_track_range_t *pr = play == OS64_MEDIA_NO_TIME ? 0 : range_holding(t, play, 0);
    if (pr) {
        play_range = pr->id;
        play_unit = unit_for(pr, play);
        protect_end = play > INT64_MAX - OS64_MEDIA_PROTECT_AHEAD_NS ? INT64_MAX : play + OS64_MEDIA_PROTECT_AHEAD_NS;
    }
    // Behind first: the earliest range's front, unit by unit.
    while (start_bytes - t->bytes < need && t->range_count > 0) {
        os64_media_track_range_t *r = &t->range[0];
        bool is_play = r->id == play_range;
        uint32_t limit = is_play ? play_unit : r->count, cut = 0;
        uint64_t freed = 0;
        while (cut < limit && start_bytes - t->bytes + freed < need) {
            uint32_t n = next_unit(r, cut);
            for (uint32_t i = cut; i < n; i++)
                freed += r->frame[i].size;
            cut = n;
        }
        if (cut == 0)
            break;
        bool left = range_cut(t, 0, 0, cut);
        if (is_play || left)
            break;                      // at the play position, or enough freed inside this range
    }
    if (play_range == 0)
        return;
    // Then far ahead: the latest ranges' backs, unit by unit, down to the
    // range holding the play position.
    for (uint32_t ri = t->range_count; ri-- > 0 && start_bytes - t->bytes < need;) {
        os64_media_track_range_t *r = &t->range[ri];
        bool is_play = r->id == play_range;
        uint32_t keep = r->count;
        uint64_t freed = 0;
        while (keep > 0 && start_bytes - t->bytes + freed < need) {
            uint32_t u = unit_of(r, keep - 1);
            if (unit_pts(r, u) < protect_end)
                break;
            for (uint32_t i = u; i < keep; i++)
                freed += r->frame[i].size;
            keep = u;
        }
        if (keep < r->count)
            range_cut(t, ri, keep, r->count);
        if (is_play || keep == r->count)
            break;                      // the play position's range, or a protected unit: earlier ones are too
    }
}

os64_media_track_rule_t os64_media_buffer_make_room(os64_media_buffer_t *b, uint64_t incoming, int64_t play)
{
    uint64_t total_quota = 0;
    for (uint32_t k = 0; k < b->track_count; k++)
        total_quota += b->track[k].quota;
    bool fits = true;
    for (uint32_t k = 0; k < b->track_count; k++) {
        os64_media_track_t *t = &b->track[k];
        // The append's bytes shared by the tracks' quotas: a muxed segment
        // is mostly picture, and a picture track's quota is the larger.
        // incoming * quota / total, in 64 bits: the remainder times a quota fits.
        uint64_t share = total_quota == 0 ? incoming
                         : incoming / total_quota * t->quota + incoming % total_quota * t->quota / total_quota;
        if (t->bytes + share > t->quota)
            evict(t, t->bytes + share - t->quota, play);
        if (t->bytes + share > t->quota)
            fits = false;
    }
    if (b->track_count == 0 && incoming > OS64_MEDIA_QUOTA_PICTURE)
        fits = false;
    return fits ? OS64_MEDIA_TRACK_OK : OS64_MEDIA_TRACK_QUOTA;
}

// ── Ranges, as a page sees them ────────────────────────────────────────────

// The next presentation interval of a track's ranges at or after `from`
// (ranges whose gap is within the join joined): false when there is none.
// Ranges are few; this is quadratic in them and allocates nothing.
static bool next_union(const os64_media_track_t *t, int64_t from, os64_media_time_range_t *out)
{
    bool found = false;
    for (uint32_t i = 0; i < t->range_count; i++) {
        const os64_media_track_range_t *r = &t->range[i];
        if (r->end <= from || r->start >= r->end)
            continue;
        if (!found || r->start < out->start) {
            out->start = r->start;
            out->end = r->end;
            found = true;
        }
    }
    if (!found)
        return false;
    if (out->start < from)
        out->start = from;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t i = 0; i < t->range_count; i++) {
            const os64_media_track_range_t *r = &t->range[i];
            if (r->start < r->end && r->start <= out->end + join_of(t) && r->end > out->end) {
                out->end = r->end;
                grew = true;
            }
        }
    }
    return true;
}

uint32_t os64_media_track_ranges(const os64_media_track_t *t, os64_media_time_range_t *out, uint32_t cap)
{
    uint32_t n = 0;
    os64_media_time_range_t r;
    for (int64_t from = OS64_MEDIA_NO_TIME; next_union(t, from, &r); from = r.end) {
        if (n < cap)
            out[n] = r;
        n++;
    }
    return n;
}

uint32_t os64_media_buffer_buffered(const os64_media_buffer_t *b, os64_media_time_range_t *out, uint32_t cap)
{
    if (b->track_count == 0)
        return 0;
    if (b->track_count == 1)
        return os64_media_track_ranges(&b->track[0], out, cap);
    // The intersection of two tracks' unions, walked together.
    const os64_media_track_t *x = &b->track[0], *y = &b->track[1];
    os64_media_time_range_t a, c;
    uint32_t n = 0;
    bool ha = next_union(x, OS64_MEDIA_NO_TIME, &a), hc = next_union(y, OS64_MEDIA_NO_TIME, &c);
    while (ha && hc) {
        int64_t s = a.start > c.start ? a.start : c.start, e = a.end < c.end ? a.end : c.end;
        if (s < e) {
            if (n < cap)
                out[n] = (os64_media_time_range_t){ s, e };
            n++;
        }
        if (a.end < c.end)
            ha = next_union(x, a.end, &a);
        else
            hc = next_union(y, c.end, &c);
    }
    return n;
}

// ── A track, for a player ──────────────────────────────────────────────────

// The range a time belongs to: the one presenting it, or, for a time in a
// gap the join closes (one `buffered` reports as held), the range that
// begins within the join after it.
static const os64_media_track_range_t *range_holding(const os64_media_track_t *t, int64_t time, uint32_t *index)
{
    const os64_media_track_range_t *next = 0;
    for (uint32_t i = 0; i < t->range_count; i++) {
        const os64_media_track_range_t *r = &t->range[i];
        if (r->start >= r->end)
            continue;
        if (time >= r->start && time < r->end) {
            if (index)
                *index = i;
            return r;
        }
        if (r->start > time && r->start - time <= join_of(t) && (next == 0 || r->start < next->start)) {
            next = r;
            if (index)
                *index = i;
        }
    }
    if (next == 0)
        return 0;
    // Only a gap `buffered` joins: some range must end within the join before it.
    for (uint32_t i = 0; i < t->range_count; i++)
        if (t->range[i].end <= time && time - t->range[i].end <= join_of(t) && t->range[i].start < t->range[i].end)
            return next;
    return 0;
}

// The range a cursor goes on into from range `r`'s end: the one beginning
// within the join of where `r`'s presentation ends (each begins at a random
// access point), the earliest if several.
static const os64_media_track_range_t *range_after(const os64_media_track_t *t, const os64_media_track_range_t *r)
{
    const os64_media_track_range_t *best = 0;
    for (uint32_t i = 0; i < t->range_count; i++) {
        const os64_media_track_range_t *n = &t->range[i];
        if (n == r || n->count == 0 || n->start <= r->start)
            continue;
        if (n->start < r->end - join_of(t) || n->start > r->end + join_of(t))
            continue;
        if (best == 0 || n->start < best->start)
            best = n;
    }
    return best;
}

bool os64_media_track_ahead(const os64_media_track_t *t, int64_t time, int64_t *range_end)
{
    os64_media_time_range_t r;
    if (!next_union(t, time, &r) || r.start > time)
        return false;
    if (range_end)
        *range_end = r.end;
    return true;
}

// The pts of a unit's random access point (past its preload frames).
static int64_t unit_pts(const os64_media_track_range_t *r, uint32_t u)
{
    while (u + 1 < r->count && (r->frame[u].flags & OS64_MEDIA_FRAME_PRELOAD))
        u++;
    return r->frame[u].pts;
}

// The unit to decode from for `time`: the last whose random access point
// is presented at or before it. A range's random access points are
// presented in decode order (its groups each continue the one before on
// the screen), so a binary search finds it, each probe walking back to its
// unit (a GOP at most).
static uint32_t unit_for(const os64_media_track_range_t *r, int64_t time)
{
    uint32_t lo = 0, hi = r->count;     // the answer is in [lo, hi)
    while (hi - lo > 1) {
        uint32_t mid = unit_of(r, lo + (hi - lo) / 2);
        if (mid <= lo) {                // the probe fell back into lo's unit
            uint32_t n = next_unit(r, lo);
            if (n >= hi || unit_pts(r, n) > time)
                break;
            lo = n;
            continue;
        }
        if (unit_pts(r, mid) <= time)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

int os64_media_track_rap_at_or_before(const os64_media_track_t *t, int64_t time, os64_media_track_frame_t *rap)
{
    const os64_media_track_range_t *r = range_holding(t, time, 0);
    if (r == 0)
        return OS64_MEDIA_TRACK_WAIT;
    *rap = r->frame[unit_for(r, time)];
    return OS64_MEDIA_TRACK_FRAME;
}

int os64_media_track_frame_at_or_after(const os64_media_track_t *t, int64_t time, os64_media_track_frame_t *frame)
{
    const os64_media_track_range_t *r = range_holding(t, time, 0);
    if (r == 0)
        return OS64_MEDIA_TRACK_WAIT;
    // The whole range: an overlap can leave an old frame decoded before a
    // new random access point but presented after it, so no unit bounds
    // where the answer is. A paused seek's preview asks, not every frame.
    bool found = false;
    for (uint32_t i = 0; i < r->count; i++) {
        const os64_media_track_frame_t *f = &r->frame[i];
        if ((f->flags & OS64_MEDIA_FRAME_PRELOAD) || f->pts < time)
            continue;
        if (!found || f->pts < frame->pts) {
            *frame = *f;
            found = true;
        }
    }
    return found ? OS64_MEDIA_TRACK_FRAME : OS64_MEDIA_TRACK_WAIT;
}

int os64_media_track_seek(const os64_media_track_t *t, int64_t time, os64_media_track_cursor_t *cursor)
{
    *cursor = (os64_media_track_cursor_t){ 0 };
    const os64_media_track_range_t *r = range_holding(t, time, 0);
    if (r == 0)
        return OS64_MEDIA_TRACK_WAIT;
    uint32_t u = unit_for(r, time);
    cursor->range_id = r->id;
    cursor->index = u;
    cursor->edits = t->edits;
    cursor->next_serial = r->frame[u].serial;   // the frame the cursor reads next
    return OS64_MEDIA_TRACK_FRAME;
}

// Where the frame with this serial and decode time is now: false if it is gone.
static bool find_serial(const os64_media_track_t *t, int64_t dts, uint64_t serial, uint32_t *ri, uint32_t *index)
{
    for (uint32_t k = 0; k < t->range_count; k++) {
        const os64_media_track_range_t *r = &t->range[k];
        if (r->count == 0 || dts < r->frame[0].dts || dts > r->frame[r->count - 1].dts)
            continue;
        uint32_t lo = 0, hi = r->count;
        while (lo < hi) {               // the first frame decoded at or after dts
            uint32_t mid = lo + (hi - lo) / 2;
            if (r->frame[mid].dts < dts)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (uint32_t i = lo; i < r->count && r->frame[i].dts == dts; i++)
            if (r->frame[i].serial == serial) {
                *ri = k;
                *index = i;
                return true;
            }
    }
    return false;
}

int os64_media_track_next(const os64_media_track_t *t, os64_media_track_cursor_t *cursor,
                          os64_media_track_frame_t *frame)
{
    uint32_t ri, index;
    const os64_media_track_range_t *r = range_by_id(t, cursor->range_id, &ri);
    if (cursor->edits != t->edits || r == 0) {
        // Frames moved: find the cursor's place again by the frames it knows.
        if (cursor->last_serial != 0) {
            if (!find_serial(t, cursor->last_dts, cursor->last_serial, &ri, &index))
                return OS64_MEDIA_TRACK_RESYNC;     // the frame it returned last is gone
            r = &t->range[ri];
            index++;
            if (cursor->next_serial != 0 && (index >= r->count || r->frame[index].serial != cursor->next_serial))
                return OS64_MEDIA_TRACK_RESYNC;     // the frame that followed it is gone
        } else {
            // Seeked, nothing read yet: its first frame must still be there.
            bool found = false;
            for (uint32_t k = 0; k < t->range_count && !found; k++)
                for (uint32_t i = 0; i < t->range[k].count; i++)
                    if (t->range[k].frame[i].serial == cursor->next_serial) {
                        ri = k;
                        index = i;
                        found = true;
                        break;
                    }
            if (!found)
                return OS64_MEDIA_TRACK_RESYNC;
            r = &t->range[ri];
        }
        cursor->range_id = r->id;
        cursor->index = index;
        cursor->edits = t->edits;
    }
    if (cursor->index >= r->count) {
        // At the range's end: on into the range the join joins to it, if
        // there is one yet (whatever comes next is the next frame).
        const os64_media_track_range_t *n = range_after(t, r);
        cursor->next_serial = 0;
        if (n == 0)
            return OS64_MEDIA_TRACK_WAIT;
        r = n;
        cursor->range_id = n->id;
        cursor->index = 0;
    }
    *frame = r->frame[cursor->index++];
    os64_media_fragment_slab_hold(frame->slab);
    cursor->last_serial = frame->serial;
    cursor->last_dts = frame->dts;
    cursor->next_serial = cursor->index < r->count ? r->frame[cursor->index].serial : 0;
    return OS64_MEDIA_TRACK_FRAME;
}

void os64_media_track_frame_done(os64_media_track_frame_t *frame)
{
    if (frame->slab) {
        os64_media_fragment_slab_drop(frame->slab);
        frame->slab = 0;
    }
}

bool os64_media_track_generation_hold(os64_media_track_t *t, uint32_t generation,
                                      os64_media_track_generation_t *out)
{
    os64_media_track_generation_t *g = gen_find(t, generation, 0);
    if (g == 0)
        return false;
    g->holds++;
    *out = *g;
    return true;
}

void os64_media_track_generation_drop(os64_media_track_t *t, uint32_t generation)
{
    uint32_t gi;
    os64_media_track_generation_t *g = gen_find(t, generation, &gi);
    if (g && g->holds > 0) {
        g->holds--;
        gen_maybe_free(t, gi);
    }
}

const char *os64_media_track_rule_name(os64_media_track_rule_t rule)
{
    switch (rule) {
    case OS64_MEDIA_TRACK_OK: return "ok";
    case OS64_MEDIA_TRACK_NO_MEMORY: return "out of memory";
    case OS64_MEDIA_TRACK_TIMESCALE: return "a timescale finer than a nanosecond";
    case OS64_MEDIA_TRACK_TIME_OVERFLOW: return "a time past int64 nanoseconds";
    case OS64_MEDIA_TRACK_UNKNOWN_TRACK: return "a track the buffer has not got";
    case OS64_MEDIA_TRACK_QUOTA: return "the quota is exceeded";
    default: return "?";
    }
}
