// visible.c — the pieces of a rectangle each window claims (visible.h).

#include "gui/visible.h"

#include <stdbool.h>

static bool empty(visible_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

// a ∩ b, false when they do not meet. In 64 bits: a frame may lie far off
// the screen, and an edge's sum must not wrap.
static bool meet(visible_rect_t a, visible_rect_t b, visible_rect_t *out)
{
    int64_t x0 = a.x > b.x ? a.x : b.x;
    int64_t y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1;
    int64_t y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return false;
    *out = (visible_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    return true;
}

// r minus the part `hole` of it (hole inside r): what is left is at most
// four rectangles, the band above the hole, the band below, and the two
// sides of the hole's own rows. Appends them to `out` at `*n`.
static void cut(visible_rect_t r, visible_rect_t hole, visible_rect_t *out, int32_t *n)
{
    int32_t top = hole.y - r.y;
    int32_t bottom = (r.y + r.h) - (hole.y + hole.h);
    int32_t left = hole.x - r.x;
    int32_t right = (r.x + r.w) - (hole.x + hole.w);
    if (top > 0)
        out[(*n)++] = (visible_rect_t){r.x, r.y, r.w, top};
    if (bottom > 0)
        out[(*n)++] = (visible_rect_t){r.x, hole.y + hole.h, r.w, bottom};
    if (left > 0)
        out[(*n)++] = (visible_rect_t){r.x, hole.y, left, hole.h};
    if (right > 0)
        out[(*n)++] = (visible_rect_t){hole.x + hole.w, hole.y, right, hole.h};
}

int32_t visible_claim(visible_rect_t d, const visible_rect_t *frames, int32_t nframes,
                      visible_piece_t *out, int32_t cap, int32_t *consumed)
{
    // The unclaimed set, and the next one built beside it: a frame whose
    // cut would not fit is never applied, so the set it would have
    // replaced stays whole.
    visible_rect_t left_a[VISIBLE_PIECES_MAX], left_b[VISIBLE_PIECES_MAX];
    visible_rect_t *left = left_a, *next = left_b;
    int32_t nleft = 0, nclaimed = 0, f = 0;
    if (cap > VISIBLE_PIECES_MAX)
        cap = VISIBLE_PIECES_MAX;
    if (!empty(d) && cap > 0)
        left[nleft++] = d;
    for (; f < nframes && nleft > 0; f++) {
        // The claims this frame makes go straight into `out` after the
        // claims already there; they are kept only if the frame is.
        int32_t nnext = 0, nmade = 0;
        bool fits = true;
        for (int32_t i = 0; i < nleft; i++) {
            visible_rect_t hole;
            bool met = meet(left[i], frames[f], &hole);
            // A piece the frame misses moves across whole; a cut makes at
            // most four.
            if (nnext + (met ? 4 : 1) > VISIBLE_PIECES_MAX) {
                fits = false;
                break;
            }
            if (!met) {
                next[nnext++] = left[i];
                continue;
            }
            // Inside `out`: the frame before left nclaimed + nleft <= cap,
            // and this one has claimed from fewer than nleft pieces yet.
            out[nclaimed + nmade++] = (visible_piece_t){hole, f};
            cut(left[i], hole, next, &nnext);
        }
        // Every piece must end in `out`: what is claimed, and what is left.
        if (!fits || nclaimed + nmade + nnext > cap)
            break;
        nclaimed += nmade;
        visible_rect_t *t = left;
        left = next;
        next = t;
        nleft = nnext;
    }
    // A walk that ran out of unclaimed rectangle has nothing left for the
    // frames below to take: they are consumed too.
    *consumed = nleft == 0 ? nframes : f;
    for (int32_t i = 0; i < nleft; i++)
        out[nclaimed + i] = (visible_piece_t){left[i], VISIBLE_UNCLAIMED};
    return nclaimed + nleft;
}
