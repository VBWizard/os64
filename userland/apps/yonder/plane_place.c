// plane_place.c — a video's place on its plane, and what lies over its
// picture (plane_place.h).

#include "plane_place.h"

yonder_plane_verdict_t yonder_plane_decide(const yonder_plane_input_t *in)
{
    yonder_plane_verdict_t v = {YONDER_PLANE_PLACED, true};
    if (!in->on_view) {
        v.reason = YONDER_PLANE_OUT_OF_VIEW;
        v.want = false;
    } else if (!in->cover) {
        v.reason = YONDER_PLANE_NOT_COVER;
    } else if (in->backdrop) {
        v.reason = YONDER_PLANE_BACKDROP;
    } else if (in->clip_meets) {
        v.reason = YONDER_PLANE_CLIP_MEETS;
    } else if (in->not_layer) {
        v.reason = YONDER_PLANE_NOT_LAYER;
    } else if (in->over_budget) {
        v.reason = YONDER_PLANE_BUDGET;
    } else if (in->unpainted) {
        v.reason = YONDER_PLANE_UNPAINTED;
    } else if (!in->ready || !in->shown) {
        v.reason = YONDER_PLANE_NOT_READY;
    } else if (in->held_off) {
        v.reason = YONDER_PLANE_REFUSED;
    }
    return v;
}

const char *yonder_plane_reason_name(yonder_plane_reason_t r)
{
    static const char *const kNames[YONDER_PLANE_REASONS] = {
        "placed", "out of view", "not a cover", "a backdrop reader", "another plane's clip",
        "not a layer", "the overlays' budget", "not yet painted", "not ready", "a refused publish",
    };
    return (unsigned)r < YONDER_PLANE_REASONS ? kNames[r] : "?";
}

static bool cut(yonder_plane_rect_t a, yonder_plane_rect_t b, yonder_plane_rect_t *out)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w < (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h < (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0)
        return false;
    *out = (yonder_plane_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    return true;
}

size_t yonder_plane_map_tiles(yonder_plane_rect_t origin)
{
    if (origin.w <= 0 || origin.h <= 0)
        return 0;
    size_t cols = ((size_t)origin.w + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;
    size_t rows = ((size_t)origin.h + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;
    return cols * rows;
}

bool yonder_plane_map_reset(yonder_plane_map_t *m, yonder_plane_rect_t origin, uint8_t *tiles, size_t cap)
{
    size_t n = yonder_plane_map_tiles(origin);
    *m = (yonder_plane_map_t){0};
    if (n == 0 || n > cap || tiles == NULL)
        return false;
    m->origin = origin;
    m->cols = (uint32_t)(((size_t)origin.w + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE);
    m->rows = (uint32_t)(((size_t)origin.h + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE);
    m->tiles = tiles;
    for (size_t i = 0; i < n; i++)
        tiles[i] = YONDER_PLANE_TILE_UNKNOWN;
    return true;
}

// Tile (c, r)'s part inside `clip`: false when it has none.
static bool tile_part(const yonder_plane_map_t *m, uint32_t c, uint32_t r, yonder_plane_rect_t clip,
                      yonder_plane_rect_t *out)
{
    yonder_plane_rect_t tile = {m->origin.x + (int32_t)(c * YONDER_PLANE_TILE),
                                m->origin.y + (int32_t)(r * YONDER_PLANE_TILE), YONDER_PLANE_TILE,
                                YONDER_PLANE_TILE};
    yonder_plane_rect_t in_origin;
    return cut(tile, m->origin, &in_origin) && cut(in_origin, clip, out);
}

// The tiles' columns and rows `r` meets (half-open), false for none.
static bool tile_span(const yonder_plane_map_t *m, yonder_plane_rect_t r, uint32_t *c0, uint32_t *c1, uint32_t *r0,
                      uint32_t *r1)
{
    yonder_plane_rect_t in;
    if (m->tiles == NULL || !cut(r, m->origin, &in))
        return false;
    *c0 = (uint32_t)(in.x - m->origin.x) / YONDER_PLANE_TILE;
    *r0 = (uint32_t)(in.y - m->origin.y) / YONDER_PLANE_TILE;
    *c1 = (uint32_t)(in.x + in.w - m->origin.x + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;
    *r1 = (uint32_t)(in.y + in.h - m->origin.y + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;
    return true;
}

bool yonder_plane_map_whole_tiles(const yonder_plane_map_t *m, yonder_plane_rect_t r, yonder_plane_rect_t clip,
                                  yonder_plane_rect_t *out)
{
    uint32_t c0, c1, r0, r1;
    yonder_plane_rect_t in;
    if (!cut(r, clip, &in) || !tile_span(m, in, &c0, &c1, &r0, &r1))
        return false;
    yonder_plane_rect_t tiles = {m->origin.x + (int32_t)(c0 * YONDER_PLANE_TILE),
                                 m->origin.y + (int32_t)(r0 * YONDER_PLANE_TILE),
                                 (int32_t)((c1 - c0) * YONDER_PLANE_TILE), (int32_t)((r1 - r0) * YONDER_PLANE_TILE)};
    yonder_plane_rect_t in_origin;
    return cut(tiles, m->origin, &in_origin) && cut(in_origin, clip, out);
}

void yonder_plane_map_paint(yonder_plane_map_t *m, yonder_plane_rect_t painted, yonder_plane_rect_t clip,
                            bool (*ink)(void *ctx, yonder_plane_rect_t part), void *ctx)
{
    yonder_plane_rect_t area;
    uint32_t c0, c1, r0, r1;
    if (!cut(painted, clip, &area) || !tile_span(m, area, &c0, &c1, &r0, &r1))
        return;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = c0; c < c1; c++) {
            yonder_plane_rect_t part, met;
            if (!tile_part(m, c, r, clip, &part) || !cut(part, painted, &met))
                continue;
            uint8_t *t = &m->tiles[(size_t)r * m->cols + c];
            bool whole = met.x == part.x && met.y == part.y && met.w == part.w && met.h == part.h;
            if (ink(ctx, met))
                *t = YONDER_PLANE_TILE_INKED;
            else if (whole)
                *t = YONDER_PLANE_TILE_CLEAR;
        }
}

static bool any_tile(const yonder_plane_map_t *m, yonder_plane_rect_t clip, bool inked)
{
    uint32_t c0, c1, r0, r1;
    if (!tile_span(m, clip, &c0, &c1, &r0, &r1))
        return false;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = c0; c < c1; c++) {
            yonder_plane_rect_t part;
            if (!tile_part(m, c, r, clip, &part))
                continue;
            uint8_t t = m->tiles[(size_t)r * m->cols + c];
            if (inked ? t == YONDER_PLANE_TILE_INKED : t == YONDER_PLANE_TILE_UNKNOWN)
                return true;
        }
    return false;
}

bool yonder_plane_map_inked(const yonder_plane_map_t *m, yonder_plane_rect_t clip)
{
    return any_tile(m, clip, true);
}

bool yonder_plane_map_known(const yonder_plane_map_t *m, yonder_plane_rect_t clip)
{
    yonder_plane_rect_t in;
    return m->tiles != NULL && cut(clip, m->origin, &in) && !any_tile(m, clip, false);
}

void yonder_plane_map_forget(yonder_plane_map_t *m, yonder_plane_rect_t part)
{
    uint32_t c0, c1, r0, r1;
    if (m->tiles == NULL || !tile_span(m, part, &c0, &c1, &r0, &r1))
        return;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = c0; c < c1; c++)
            m->tiles[(size_t)r * m->cols + c] = YONDER_PLANE_TILE_UNKNOWN;
}

void yonder_plane_layer(const uint32_t *b, const uint32_t *w, size_t n, uint32_t *out,
                        yonder_plane_layer_counts_t *counts)
{
    *counts = (yonder_plane_layer_counts_t){0};
    for (size_t i = 0; i < n; i++) {
        int32_t a[3];
        for (int c = 0; c < 3; c++) {
            int32_t bc = (int32_t)(b[i] >> (8 * c)) & 0xff, wc = (int32_t)(w[i] >> (8 * c)) & 0xff;
            int32_t v = 255 - (wc - bc);
            a[c] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        int32_t lo = a[0], hi = a[0];
        for (int c = 1; c < 3; c++) {
            lo = a[c] < lo ? a[c] : lo;
            hi = a[c] > hi ? a[c] : hi;
        }
        if (hi - lo > 1) {
            if (counts->disagree == 0) {
                counts->first = i;
                counts->first_b = b[i];
                counts->first_w = w[i];
            }
            counts->disagree++;
            counts->spread[hi - lo >= 5 ? 3 : hi - lo - 2]++;
        }
        if (a[1] > 0)
            counts->inked++;
        out[i] = (uint32_t)a[1] << 24 | (b[i] & 0x00ffffffu);
    }
}

uint64_t yonder_plane_union_area(const yonder_plane_rect_t *r, uint32_t n)
{
    // Every rectangle's edges cut the plane into cells; a cell inside any
    // rectangle counts once.
    int32_t xs[2 * 8 * 8 + 2], ys[2 * 8 * 8 + 2];
    uint32_t nx = 0, ny = 0;
    if (n > 64)
        n = 64;
    for (uint32_t i = 0; i < n; i++) {
        if (r[i].w <= 0 || r[i].h <= 0)
            continue;
        xs[nx++] = r[i].x;
        xs[nx++] = r[i].x + r[i].w;
        ys[ny++] = r[i].y;
        ys[ny++] = r[i].y + r[i].h;
    }
    for (uint32_t i = 1; i < nx; i++)
        for (uint32_t j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
            int32_t t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t;
        }
    for (uint32_t i = 1; i < ny; i++)
        for (uint32_t j = i; j > 0 && ys[j - 1] > ys[j]; j--) {
            int32_t t = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = t;
        }
    uint64_t area = 0;
    for (uint32_t i = 0; i + 1 < nx; i++)
        for (uint32_t j = 0; j + 1 < ny; j++) {
            if (xs[i + 1] == xs[i] || ys[j + 1] == ys[j])
                continue;
            for (uint32_t k = 0; k < n; k++)
                if (r[k].w > 0 && r[k].h > 0 && xs[i] >= r[k].x && xs[i + 1] <= r[k].x + r[k].w && ys[j] >= r[k].y &&
                    ys[j + 1] <= r[k].y + r[k].h) {
                    area += (uint64_t)(xs[i + 1] - xs[i]) * (uint64_t)(ys[j + 1] - ys[j]);
                    break;
                }
        }
    return area;
}

static yonder_plane_rect_t bound(yonder_plane_rect_t a, yonder_plane_rect_t b)
{
    int32_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (yonder_plane_rect_t){x0, y0, x1 - x0, y1 - y0};
}

uint32_t yonder_plane_join(yonder_plane_rect_t *r, uint32_t n, uint32_t max)
{
    if (max == 0)
        return 0;
    while (n > max) {
        uint32_t bi = 0, bj = 1;
        int64_t best = INT64_MAX;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = i + 1; j < n; j++) {
                yonder_plane_rect_t u = bound(r[i], r[j]);
                int64_t cost = (int64_t)u.w * u.h - (int64_t)r[i].w * r[i].h - (int64_t)r[j].w * r[j].h;
                if (cost < best) {
                    best = cost;
                    bi = i;
                    bj = j;
                }
            }
        r[bi] = bound(r[bi], r[bj]);
        r[bj] = r[--n];
    }
    return n;
}

uint32_t yonder_plane_map_overlays(const yonder_plane_map_t *m, yonder_plane_rect_t clip, yonder_plane_rect_t *out,
                                   uint32_t max, uint64_t *area)
{
    static yonder_plane_rect_t runs[YONDER_PLANE_RUNS_MAX];
    uint32_t n = 0, c0, c1, r0, r1;
    bool spill = false;
    yonder_plane_rect_t all = {0, 0, 0, 0};
    *area = 0;
    if (max == 0 || !tile_span(m, clip, &c0, &c1, &r0, &r1))
        return 0;
    for (uint32_t r = r0; r < r1; r++) {
        uint32_t c = c0;
        while (c < c1) {
            yonder_plane_rect_t part, run;
            if (m->tiles[(size_t)r * m->cols + c] != YONDER_PLANE_TILE_INKED || !tile_part(m, c, r, clip, &part)) {
                c++;
                continue;
            }
            run = part;
            for (c++; c < c1; c++) {
                yonder_plane_rect_t next;
                if (m->tiles[(size_t)r * m->cols + c] != YONDER_PLANE_TILE_INKED || !tile_part(m, c, r, clip, &next))
                    break;
                run = bound(run, next);
            }
            all = all.w > 0 ? bound(all, run) : run;
            // A run with the columns of one in the row above it, touching it,
            // grows that one down.
            bool grown = false;
            for (uint32_t k = 0; k < n && !grown; k++)
                if (runs[k].x == run.x && runs[k].w == run.w && runs[k].y + runs[k].h == run.y) {
                    runs[k].h += run.h;
                    grown = true;
                }
            if (!grown) {
                if (n < YONDER_PLANE_RUNS_MAX)
                    runs[n++] = run;
                else
                    spill = true;
            }
        }
    }
    if (spill) {
        // Too many to join one by one: the bounding box of all of them.
        out[0] = all;
        *area = (uint64_t)all.w * (uint64_t)all.h;
        return 1;
    }
    n = yonder_plane_join(runs, n, max);
    for (uint32_t i = 0; i < n; i++)
        out[i] = runs[i];
    *area = yonder_plane_union_area(out, n);
    return n;
}
