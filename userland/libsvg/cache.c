#include "internal.h"

/* Versioned field stream: fixed-width little-endian words, finite binary64
 * values, and lengths before variable arrays. This is an in-process key, not
 * a persistent file format. A digest selects candidates; bytes prove equality.
 */
typedef struct {
    Context *c;
    unsigned char *data;
    size_t size, limit;
    uint64_t digest;
} Writer;
static void key_word(Writer *w, uint64_t value)
{
    if (w->c->why != SVG_OK)
        return;
    if (w->limit - w->size < 8) {
        w->c->why = SVG_LIMIT;
        return;
    }
    if (!svg_spend(w->c, 8))
        return;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char byte = (unsigned char)(value >> (i * 8));
        if (w->data)
            w->data[w->size] = byte;
        ++w->size;
        w->digest = (w->digest ^ byte) * UINT64_C(1099511628211);
    }
}
static void number(Writer *w, double value)
{
    if (w->c->why != SVG_OK)
        return;
    if (!isfinite(value)) {
        w->c->why = SVG_BAD_ARGUMENT;
        return;
    }
    union {
        double d;
        uint64_t u;
    } bits = {.d = value == 0 ? 0 : value};
    _Static_assert(sizeof(double) == sizeof(uint64_t), "binary64 key words");
    key_word(w, bits.u);
}
static void mapping(Writer *w, const Map *m)
{
    number(w, m->a);
    number(w, m->b);
    number(w, m->c);
    number(w, m->d);
    number(w, m->tx);
    number(w, m->ty);
}
static void path(Writer *w, const svg_path_t *p)
{
    key_word(w, p != NULL);
    if (!p)
        return;
    key_word(w, p->count);
    for (size_t i = 0; i < p->count && w->c->why == SVG_OK; ++i) {
        const svg_command_t *v = &p->commands[i];
        key_word(w, v->verb);
        size_t n = v->verb == SVG_CLOSE       ? 0
                   : v->verb == SVG_CUBIC     ? 3
                   : v->verb == SVG_QUADRATIC ? 2
                                              : 1;
        for (size_t j = 0; j < n; ++j) {
            number(w, v->p[j].x);
            number(w, v->p[j].y);
        }
        if (v->verb == SVG_ARC) {
            number(w, v->rx);
            number(w, v->ry);
            number(w, v->rotation);
            key_word(w, v->large);
            key_word(w, v->sweep);
        }
    }
}
static void paint(Writer *w, const svg_paint_t *p)
{
    key_word(w, p->kind);
    if (p->kind == SVG_PAINT_COLOR)
        key_word(w, p->argb);
    const svg_gradient_stop_t *stops = NULL;
    size_t count = 0;
    if (p->kind == SVG_PAINT_LINEAR) {
        const svg_linear_gradient_t *g = p->linear;
        number(w, g->x1);
        number(w, g->y1);
        number(w, g->x2);
        number(w, g->y2);
        mapping(w, &g->transform);
        key_word(w, g->object_bbox);
        key_word(w, g->spread);
        stops = g->stops;
        count = g->count;
    } else if (p->kind == SVG_PAINT_RADIAL) {
        const svg_radial_gradient_t *g = p->radial;
        number(w, g->cx);
        number(w, g->cy);
        number(w, g->r);
        number(w, g->fx);
        number(w, g->fy);
        number(w, g->fr);
        mapping(w, &g->transform);
        key_word(w, g->object_bbox);
        key_word(w, g->spread);
        stops = g->stops;
        count = g->count;
    }
    key_word(w, count);
    for (size_t i = 0; i < count && w->c->why == SVG_OK; ++i) {
        number(w, stops[i].offset);
        key_word(w, stops[i].argb);
    }
}
static void encode(Writer *w, const svg_scene_t *s, const svg_render_options_t *o,
                   const svg_render_target_t *t)
{
    key_word(w, 1);
    key_word(w, s->mode);
    number(w, t->origin_x);
    number(w, t->origin_y);
    key_word(w, t->width);
    key_word(w, t->height);
    mapping(w, &o->viewport_to_device);
    number(w, o->arc_error);
    number(w, o->quantization_error);
    key_word(w, s->count);
    for (size_t i = 0; i < s->count && w->c->why == SVG_OK; ++i) {
        const svg_scene_item_t *item = &s->items[i];
        const svg_style_t *v = &item->style;
        key_word(w, item->parent == SIZE_MAX ? UINT64_MAX : item->parent);
        key_word(w, item->kind);
        mapping(w, &item->user_to_viewport);
        mapping(w, &v->transform);
        paint(w, &v->fill);
        paint(w, &v->stroke);
        number(w, v->fill_opacity);
        number(w, v->stroke_opacity);
        number(w, v->opacity);
        number(w, v->stroke_style.width);
        number(w, v->stroke_style.miter_limit);
        key_word(w, v->stroke_style.cap);
        key_word(w, v->stroke_style.join);
        key_word(w, v->dash.count);
        if (v->dash.count) {
            number(w, v->dash.offset);
            for (size_t j = 0; j < v->dash.count && w->c->why == SVG_OK; ++j)
                number(w, v->dash.values[j]);
        }
        key_word(w, v->even_odd);
        key_word(w, v->display);
        key_word(w, v->visible);
        key_word(w, v->viewport.active);
        if (v->viewport.active) {
            key_word(w, v->viewport.clip);
            number(w, v->viewport.width);
            number(w, v->viewport.height);
            mapping(w, &v->viewport.content_transform);
        }
        path(w, item->path);
        key_word(w, item->clip.active);
        if (item->clip.active) {
            key_word(w, item->clip.even_odd);
            key_word(w, item->clip.object_bbox);
            mapping(w, &item->clip.transform);
            if (item->clip.object_bbox)
                mapping(w, &item->clip.outer_transform);
            path(w, item->clip.path);
        }
    }
}
Status svg_raster_key_create(Context *c, const svg_scene_t *scene, const svg_render_options_t *o,
                             const svg_render_target_t *t, size_t max_bytes, svg_raster_key_t **out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    if (!c || !scene || scene->owner != c)
        return SVG_BAD_ARGUMENT;
    Status st = svg_render_validate(o, t);
    if (st != SVG_OK)
        return st;
    st = svg_enter(c);
    if (st != SVG_OK)
        return st;
    Writer w = {.c = c, .limit = max_bytes, .digest = UINT64_C(14695981039346656037)};
    encode(&w, scene, o, t);
    svg_raster_key_t *key = NULL;
    if (c->why == SVG_OK) {
        if (w.size > SIZE_MAX - sizeof(*key))
            c->why = SVG_LIMIT;
        else
            key = svg_alloc(c, sizeof(*key) + w.size);
    }
    if (key) {
        *key = (svg_raster_key_t){.owner = c,
                                  .backend = o->geometry,
                                  .size = w.size,
                                  .origin_x = t->origin_x,
                                  .origin_y = t->origin_y,
                                  .width = t->width,
                                  .height = t->height};
        w = (Writer){.c = c,
                     .data = key->data,
                     .limit = key->size,
                     .digest = UINT64_C(14695981039346656037)};
        encode(&w, scene, o, t);
        if (c->why == SVG_OK) {
            key->digest = w.digest;
            ++c->stats.raster_keys;
            *out = key;
        } else
            svg_free(c, key);
    }
    return svg_leave(c, c->why);
}
Status svg_raster_key_release(Context *c, svg_raster_key_t *key)
{
    if (!c || !key || key->owner != c)
        return SVG_BAD_ARGUMENT;
    Status st = svg_enter(c);
    if (st != SVG_OK)
        return st;
    svg_free(c, key);
    --c->stats.raster_keys;
    return svg_leave(c, SVG_OK);
}

typedef struct Entry {
    struct Entry *next, *previous;
    const os64_geometry_backend_t *backend;
    size_t key_size, cost;
    uint64_t digest;
    unsigned char data[]; /* Key followed by tightly packed pixel bytes. */
} Entry;
struct svg_raster_cache {
    Context *owner;
    Entry *first, *last;
    size_t max_bytes, max_entries;
    svg_raster_cache_stats_t stats;
};
static void unlink_entry(svg_raster_cache_t *cache, Entry *e)
{
    if (e->previous)
        e->previous->next = e->next;
    else
        cache->first = e->next;
    if (e->next)
        e->next->previous = e->previous;
    else
        cache->last = e->previous;
}
static void prepend(svg_raster_cache_t *cache, Entry *e)
{
    e->previous = NULL;
    e->next = cache->first;
    if (cache->first)
        cache->first->previous = e;
    else
        cache->last = e;
    cache->first = e;
}
static void evict(svg_raster_cache_t *cache)
{
    Entry *e = cache->last;
    unlink_entry(cache, e);
    cache->stats.bytes -= e->cost;
    --cache->stats.entries;
    ++cache->stats.evictions;
    svg_free(cache->owner, e);
}
Status svg_raster_cache_create(Context *c, size_t max_bytes, size_t max_entries,
                               svg_raster_cache_t **out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    Status st = svg_enter(c);
    if (st != SVG_OK)
        return st;
    svg_raster_cache_t *cache = svg_alloc(c, sizeof(*cache));
    if (cache) {
        *cache =
            (svg_raster_cache_t){.owner = c, .max_bytes = max_bytes, .max_entries = max_entries};
        ++c->stats.raster_caches;
        *out = cache;
    }
    return svg_leave(c, c->why);
}
static Status enter_cache(Context *c, const svg_raster_cache_t *cache)
{
    if (!c || !cache || cache->owner != c)
        return SVG_BAD_ARGUMENT;
    return svg_enter(c);
}
Status svg_raster_cache_clear(Context *c, svg_raster_cache_t *cache)
{
    Status st = enter_cache(c, cache);
    if (st != SVG_OK)
        return st;
    while (cache->last)
        evict(cache);
    return svg_leave(c, SVG_OK);
}
Status svg_raster_cache_release(Context *c, svg_raster_cache_t *cache)
{
    Status st = enter_cache(c, cache);
    if (st != SVG_OK)
        return st;
    while (cache->last)
        evict(cache);
    svg_free(c, cache);
    --c->stats.raster_caches;
    return svg_leave(c, SVG_OK);
}
Status svg_raster_cache_stats(const svg_raster_cache_t *cache, svg_raster_cache_stats_t *out)
{
    if (!cache || !out)
        return SVG_BAD_ARGUMENT;
    if (cache->owner->busy)
        return SVG_BUSY;
    *out = cache->stats;
    return SVG_OK;
}
static Status target_valid(const svg_raster_key_t *key, const svg_render_target_t *t)
{
    if (!key || !t || !t->pixels || (uintptr_t)t->pixels % _Alignof(uint32_t) ||
        t->width != key->width || t->height != key->height || t->origin_x != key->origin_x ||
        t->origin_y != key->origin_y || t->stride % sizeof(uint32_t))
        return SVG_BAD_ARGUMENT;
    size_t row = (size_t)t->width * 4;
    if (t->stride < row)
        return SVG_BAD_ARGUMENT;
    if (t->height > 1 && t->stride > (SIZE_MAX - row) / (t->height - 1))
        return SVG_LIMIT;
    if ((size_t)(t->height - 1) * t->stride + row > t->capacity)
        return SVG_LIMIT;
    return SVG_OK;
}
static Entry *find(svg_raster_cache_t *cache, const svg_raster_key_t *key)
{
    Context *c = cache->owner;
    if (!svg_spend(c, 1))
        return NULL;
    for (Entry *e = cache->first; e; e = e->next) {
        if (!svg_spend(c, 1))
            return NULL;
        if (e->backend != key->backend || e->digest != key->digest || e->key_size != key->size)
            continue;
        bool equal = true;
        for (size_t i = 0; i < key->size; ++i) {
            if (!(i % 256) && !svg_spend(c, key->size - i < 256 ? key->size - i : 256))
                return NULL;
            if (e->data[i] != key->data[i]) {
                equal = false;
                break;
            }
        }
        if (equal)
            return e;
    }
    return NULL;
}
Status svg_raster_cache_read(Context *c, svg_raster_cache_t *cache, const svg_raster_key_t *key,
                             const svg_render_target_t *t, bool *hit)
{
    if (!hit)
        return SVG_BAD_ARGUMENT;
    *hit = false;
    Status st = target_valid(key, t);
    if (st != SVG_OK)
        return st;
    st = enter_cache(c, cache);
    if (st != SVG_OK)
        return st;
    Entry *e = find(cache, key);
    if (!e && c->why == SVG_OK)
        ++cache->stats.misses;
    if (e) {
        size_t row = (size_t)t->width * 4;
        for (size_t y = 0; y < t->height && c->why == SVG_OK; ++y) {
            if (!svg_spend(c, row))
                break;
            unsigned char *dst = (unsigned char *)t->pixels + y * t->stride;
            const unsigned char *src = e->data + e->key_size + y * row;
            for (size_t x = 0; x < row; ++x)
                dst[x] = src[x];
        }
        if (c->why == SVG_OK) {
            unlink_entry(cache, e);
            prepend(cache, e);
            ++cache->stats.hits;
            *hit = true;
        }
    }
    return svg_leave(c, c->why);
}
Status svg_raster_cache_store(Context *c, svg_raster_cache_t *cache, const svg_raster_key_t *key,
                              const svg_render_target_t *t, Status rendered)
{
    if (rendered != SVG_OK)
        return rendered;
    Status st = target_valid(key, t);
    if (st != SVG_OK)
        return st;
    st = enter_cache(c, cache);
    if (st != SVG_OK)
        return st;
    size_t pixels = (size_t)t->width * t->height * 4;
    if (!cache->max_entries || key->size > SIZE_MAX - sizeof(Entry) ||
        pixels > SIZE_MAX - sizeof(Entry) - key->size)
        return svg_leave(c, SVG_LIMIT);
    size_t bytes = sizeof(Entry) + key->size + pixels, cost = svg_allocation_cost(bytes);
    if (cost == SIZE_MAX || cost > cache->max_bytes)
        return svg_leave(c, SVG_LIMIT);
    Entry *e = find(cache, key);
    if (c->why != SVG_OK)
        return svg_leave(c, c->why);
    if (e) {
        unlink_entry(cache, e);
        prepend(cache, e);
        return svg_leave(c, SVG_OK);
    }
    while (cache->last && (cache->stats.entries >= cache->max_entries ||
                           cost > cache->max_bytes - cache->stats.bytes)) {
        if (!svg_spend(c, 1))
            return svg_leave(c, c->why);
        evict(cache);
    }
    e = svg_alloc(c, bytes);
    if (e) {
        *e = (Entry){
            .backend = key->backend, .key_size = key->size, .digest = key->digest, .cost = cost};
        for (size_t i = 0; i < key->size && c->why == SVG_OK; ++i) {
            if (!(i % 256) && !svg_spend(c, key->size - i < 256 ? key->size - i : 256))
                break;
            e->data[i] = key->data[i];
        }
        size_t row = (size_t)t->width * 4;
        for (size_t y = 0; y < t->height && c->why == SVG_OK; ++y) {
            if (!svg_spend(c, row))
                break;
            const unsigned char *src = (const unsigned char *)t->pixels + y * t->stride;
            unsigned char *dst = e->data + key->size + y * row;
            for (size_t x = 0; x < row; ++x)
                dst[x] = src[x];
        }
        if (c->why == SVG_OK) {
            prepend(cache, e);
            ++cache->stats.entries;
            cache->stats.bytes += cost;
            ++cache->stats.stores;
        } else
            svg_free(c, e);
    }
    return svg_leave(c, c->why);
}
