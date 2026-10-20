// damage.c — the damage list's rule, one source for the compositor (ring 0)
// and libos64 (ring 3). The contract is os64/damage.h's.
//
// The design is deliberately the cheap one — a fixed array plus a merge
// heuristic, NOT region algebra. Regions (X11's, Cairo's) split rectangles on
// overlap to keep an exact non-overlapping cover, and they are the right
// answer when you have thousands; here the population is a few marks a frame
// ("one ball, one cursor, one console line, sometimes a dragged window" on
// the screen; a few animated parts in a window), and an exact cover would
// cost more to maintain than the overdraw it saves.
//
// Two rules keep it honest:
//   1. MERGE WHEN MERGING IS CHEAP. Union two entries only if the union wastes
//      no more than `slack` pixels beyond their own areas — so a rect inside
//      another, or one sharing a whole edge, collapses into it while distant
//      ones stay apart. Without this, a list degenerates into a list of
//      near-duplicates.
//   2. OVERFLOW FALLS BACK TO ONE RECT. When the list is full and nothing
//      merges cheaply, collapse everything into one union rect. The worst case
//      is a single damage rectangle — never a wrong picture, only a slower one.
//
// Entries may overlap. Two that overlap only in part (diagonally, or a wide
// one across a tall one) can have a union bigger than their areas plus the
// slack, so they stay apart and their overlap is painted twice. And merging
// is pairwise-greedy with NO re-merge pass: a rect that bridges into entry i
// can grow it until it swallows entry j whole, and nothing goes back to
// notice, so the swallowed-entry case paints a duplicate of j's whole area.
// Both are WASTE, never wrongness: both
// passes paint identical bytes in identical order, so the result cannot
// disagree with itself. The geometry that triggers it (a late rect bridging
// two established ones) is rare at these populations; if it ever shows, the
// fix is a containment sweep after a merge grows an entry, not a redesign.
// (Honesty upgrade from review, 2026-08-19 — the first version of this
// comment claimed the slack bound.)
//
// No kernel or libos64 header: stdint and os64/rect.h, so both rings, and
// tools/test_damage_host.c, compile it as it is.
#include "os64/damage.h"

static bool empty(os64_gui_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

static int64_t area(os64_gui_rect_t r)
{
    return empty(r) ? 0 : (int64_t)r.w * (int64_t)r.h;
}

// The smallest rectangle holding both; an empty one is no area, and the
// other wins.
static os64_gui_rect_t join(os64_gui_rect_t a, os64_gui_rect_t b)
{
    if (empty(a))
        return b;
    if (empty(b))
        return a;
    int32_t x1 = a.x < b.x ? a.x : b.x;
    int32_t y1 = a.y < b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (os64_gui_rect_t){x1, y1, x2 - x1, y2 - y1};
}

// Would unioning these two waste little enough to be worth one paint? A rect
// inside another always qualifies, and so does one sharing a whole edge: the
// union is then no bigger than the two areas. A partial overlap need not.
static bool cheap(os64_gui_rect_t a, os64_gui_rect_t b, int64_t slack)
{
    return area(join(a, b)) <= area(a) + area(b) + slack;
}

bool os64_damage_add(os64_gui_rect_t *rects, uint32_t *count, uint32_t cap, int64_t slack,
                     os64_gui_rect_t r)
{
    if (empty(r))
        return false;

    // Merge into the first entry where merging is cheap. First-fit, not
    // best-fit: the list is short and the common case is one or two entries,
    // so hunting for the optimal partner would cost more than the overdraw it
    // saves.
    for (uint32_t i = 0; i < *count; i++) {
        if (cheap(rects[i], r, slack)) {
            rects[i] = join(rects[i], r);
            return false;
        }
    }

    if (*count < cap) {
        rects[(*count)++] = r;
        return false;
    }

    // Full, and nothing merged cheaply: one union rect for everything.
    // Over-paints, never under-paints.
    os64_gui_rect_t all = r;
    for (uint32_t i = 0; i < *count; i++)
        all = join(all, rects[i]);
    rects[0] = all;
    *count = 1;
    return true;
}
