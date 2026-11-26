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
    } else if (in->over) {
        v.reason = YONDER_PLANE_OVER;
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
        "something over it", "not ready", "a refused publish",
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
