// canvas_keep.c — kept canvas pictures (canvas_keep.h): the scaled pixels of
// a canvas whose bitmap stands still, in the paint cache's blocks.

#include "canvas_keep.h"
#include "paint_cache.h"
#include "scale.h"
#include "os64/mem.h"
#include "os64/str.h"

#define KEEP_BLOCK 128
#define KEEP_CANVASES 32

// What a canvas was last drawn at, and the blocks it kept there (columns
// bx0..bx1 and rows by0..by1 of the box's blocks, past-the-end), so a
// change lets them go.
typedef struct {
    const void *who;
    uint64_t revision, last;
    int32_t w, h;
    bool premultiplied, made;
    int32_t bx0, by0, bx1, by1;
} Seen;

struct yonder_canvas_keeps {
    Seen seen[KEEP_CANVASES];
    uint64_t clock;
    yonder_canvas_keep_counts_t counts;
};

yonder_canvas_keeps_t *yonder_canvas_keeps_new(void)
{
    return os64_calloc(1, sizeof(yonder_canvas_keeps_t));
}

void yonder_canvas_keeps_free(yonder_canvas_keeps_t *k)
{
    os64_free(k);
}

// Block (bx, by)'s key: the revision, the box's size, the alpha convention
// and the place. Returns its length.
#define KEY_BYTES 28
static uint32_t block_key(uint8_t *key, uint64_t revision, int32_t w, int32_t h, bool premultiplied,
                          int32_t bx, int32_t by)
{
    key[0] = 'C';
    key[1] = 'K';
    key[2] = premultiplied;
    key[3] = 0;
    os64_memcpy(key + 4, &revision, 8);
    os64_memcpy(key + 12, &w, 4);
    os64_memcpy(key + 16, &h, 4);
    os64_memcpy(key + 20, &bx, 4);
    os64_memcpy(key + 24, &by, 4);
    return KEY_BYTES;
}

static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

// Every block `s` kept, let go.
static void drop(yonder_canvas_keeps_t *k, yonder_paint_cache_t *cache, Seen *s)
{
    uint8_t key[KEY_BYTES];
    for (int32_t by = s->by0; by < s->by1; by++)
        for (int32_t bx = s->bx0; bx < s->bx1; bx++) {
            uint32_t n = block_key(key, s->revision, s->w, s->h, s->premultiplied, bx, by);
            void *b = paint_cache_find(cache, key, n, paint_cache_hash(key, n));
            if (b != NULL) {
                paint_cache_release(cache, b);
                k->counts.dropped++;
            }
        }
    s->made = false;
}

// The canvas `who`'s slot: its own, else the one drawn longest ago, empty.
static Seen *slot(yonder_canvas_keeps_t *k, const void *who)
{
    Seen *oldest = &k->seen[0];
    for (int32_t i = 0; i < KEEP_CANVASES; i++) {
        if (k->seen[i].who == who)
            return &k->seen[i];
        if (k->seen[i].last < oldest->last)
            oldest = &k->seen[i];
    }
    *oldest = (Seen){0};
    return oldest;
}

bool yonder_canvas_keep_draw(yonder_canvas_keeps_t *k, yonder_paint_cache_t *cache, const void *who,
                             uint64_t revision, uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip,
                             os64_gui_rect_t box, bool smooth, const draw2d_surface_t *source)
{
    if (k == NULL || cache == NULL || who == NULL || revision == 0 || !smooth || source == NULL ||
        box.w <= 0 || box.h <= 0 || ((uint32_t)box.w == source->width && (uint32_t)box.h == source->height) ||
        (source->format != DRAW2D_ARGB8_PREMULTIPLIED && source->format != DRAW2D_ARGB8_STRAIGHT) ||
        draw2d_surface_validate(source) != DRAW2D_OK || source->width == 0 || source->height == 0)
        return yonder_draw_surface(dst, pitch, clip, box, smooth, source);
    bool premultiplied = source->format == DRAW2D_ARGB8_PREMULTIPLIED;
    Seen *s = slot(k, who);
    s->last = ++k->clock;
    if (s->who != who || s->revision != revision || s->w != box.w || s->h != box.h ||
        s->premultiplied != premultiplied) {
        if (s->who == who && s->made)
            drop(k, cache, s);
        *s = (Seen){who, revision, k->clock, box.w, box.h, premultiplied, false, 0, 0, 0, 0};
        return yonder_draw_surface(dst, pitch, clip, box, smooth, source);
    }
    // What the draw walks, as yonder_draw_surface cuts it, in the box's
    // own coordinates (lx, ly), and the blocks it reads.
    int64_t x0 = box.x > clip.x ? box.x : clip.x, y0 = box.y > clip.y ? box.y : clip.y;
    int64_t x1 = (int64_t)box.x + box.w < (int64_t)clip.x + clip.w ? (int64_t)box.x + box.w
                                                                   : (int64_t)clip.x + clip.w;
    int64_t y1 = (int64_t)box.y + box.h < (int64_t)clip.y + clip.h ? (int64_t)box.y + box.h
                                                                   : (int64_t)clip.y + clip.h;
    if (x1 <= x0 || y1 <= y0)
        return true;
    int32_t lx0 = (int32_t)(x0 - box.x), lx1 = (int32_t)(x1 - box.x);
    int32_t ly0 = (int32_t)(y0 - box.y), ly1 = (int32_t)(y1 - box.y);
    int32_t bx0 = lx0 / KEEP_BLOCK, bx1 = (lx1 - 1) / KEEP_BLOCK + 1;
    int32_t by0 = ly0 / KEEP_BLOCK, by1 = (ly1 - 1) / KEEP_BLOCK + 1;
    int32_t nbx = bx1 - bx0, nby = by1 - by0;
    void **blocks = os64_calloc((size_t)nbx * (size_t)nby, sizeof(void *));
    if (blocks == NULL)
        return yonder_draw_surface(dst, pitch, clip, box, smooth, source);
    uint8_t key[KEY_BYTES];
    size_t cost = 0;
    uint64_t found = 0;
    for (int32_t j = 0; j < nby; j++)
        for (int32_t i = 0; i < nbx; i++) {
            int32_t bw = min32(KEEP_BLOCK, box.w - (bx0 + i) * KEEP_BLOCK);
            int32_t bh = min32(KEEP_BLOCK, box.h - (by0 + j) * KEEP_BLOCK);
            uint32_t n = block_key(key, revision, box.w, box.h, premultiplied, bx0 + i, by0 + j);
            blocks[j * nbx + i] = paint_cache_find(cache, key, n, paint_cache_hash(key, n));
            if (blocks[j * nbx + i] != NULL)
                found++;
            else
                cost += paint_cache_cost(n, (size_t)bw * (size_t)bh * sizeof(uint32_t));
        }
    bool kept = cost == 0 || paint_cache_room(cache, cost);
    if (!kept)
        paint_cache_refused(cache, false);
    uint64_t made = 0;
    for (int32_t j = 0; kept && j < nby; j++)
        for (int32_t i = 0; kept && i < nbx; i++) {
            if (blocks[j * nbx + i] != NULL)
                continue;
            int32_t bw = min32(KEEP_BLOCK, box.w - (bx0 + i) * KEEP_BLOCK);
            int32_t bh = min32(KEEP_BLOCK, box.h - (by0 + j) * KEEP_BLOCK);
            uint32_t n = block_key(key, revision, box.w, box.h, premultiplied, bx0 + i, by0 + j);
            uint32_t *b = paint_cache_make(cache, key, n, paint_cache_hash(key, n),
                                           (size_t)bw * (size_t)bh * sizeof(uint32_t));
            if (b == NULL) {
                paint_cache_refused(cache, true);
                kept = false;
                break;
            }
            os64_gui_rect_t part = {(bx0 + i) * KEEP_BLOCK, (by0 + j) * KEEP_BLOCK, bw, bh};
            if (!yonder_scale_part(b, (uint32_t)bw, part, box.w, box.h, source)) {
                paint_cache_release(cache, b);
                kept = false;
                break;
            }
            blocks[j * nbx + i] = b;
            made++;
        }
    if (!kept) {
        // Whatever this draw found or made goes first: none of it is used.
        for (int32_t q = 0; q < nbx * nby; q++)
            if (blocks[q] != NULL)
                paint_cache_release(cache, blocks[q]);
        os64_free(blocks);
        k->counts.refused++;
        return yonder_draw_surface(dst, pitch, clip, box, smooth, source);
    }
    k->counts.found += found;
    k->counts.kept += made;
    if (made > 0) {
        if (!s->made) {
            s->bx0 = bx0, s->by0 = by0, s->bx1 = bx1, s->by1 = by1;
        } else {
            s->bx0 = min32(s->bx0, bx0), s->by0 = min32(s->by0, by0);
            s->bx1 = max32(s->bx1, bx1), s->by1 = max32(s->by1, by1);
        }
        s->made = true;
    }
    // Laid row by row, a block's run at a time.
    for (int32_t ly = ly0; ly < ly1; ly++) {
        int32_t j = ly / KEEP_BLOCK - by0, vin = ly % KEEP_BLOCK;
        uint32_t *out = dst + (uint64_t)(box.y + ly) * pitch + box.x;
        for (int32_t lx = lx0; lx < lx1;) {
            int32_t i = lx / KEEP_BLOCK - bx0, uin = lx % KEEP_BLOCK;
            int32_t bw = min32(KEEP_BLOCK, box.w - (bx0 + i) * KEEP_BLOCK);
            int32_t n = min32(bw - uin, lx1 - lx);
            const uint32_t *src = (const uint32_t *)blocks[j * nbx + i] + (size_t)vin * (size_t)bw + (size_t)uin;
            yonder_lay_scaled(out + lx, src, n, premultiplied);
            lx += n;
        }
    }
    os64_free(blocks);
    return true;
}

yonder_canvas_keep_counts_t yonder_canvas_keep_counts(const yonder_canvas_keeps_t *k)
{
    return k != NULL ? k->counts : (yonder_canvas_keep_counts_t){0};
}

void yonder_canvas_keep_counts_restart(yonder_canvas_keeps_t *k)
{
    if (k != NULL)
        k->counts = (yonder_canvas_keep_counts_t){0};
}
