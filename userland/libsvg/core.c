#include "internal.h"

typedef union {
    max_align_t alignment;
    size_t bytes;
} Block;

svg_limits_t svg_default_limits(void)
{
    return (svg_limits_t){1024 * 1024, 32768, OS64_GEOMETRY_COMMAND_MAX, 100000, 128, 4000000};
}
bool svg_map_valid(const Map *m)
{
    return m && isfinite(m->a) && isfinite(m->b) && isfinite(m->c) && isfinite(m->d) &&
           isfinite(m->tx) && isfinite(m->ty);
}
bool svg_point_valid(Point p)
{
    return isfinite(p.x) && isfinite(p.y);
}
bool svg_spend(Context *c, uint64_t amount)
{
    if (c->why != SVG_OK)
        return false;
    if (c->options.cancelled && c->options.cancelled(c->options.cancel_context)) {
        c->why = SVG_CANCELLED;
        return false;
    }
    if (amount > c->work_limit - c->stats.last_work) {
        c->why = SVG_LIMIT;
        return false;
    }
    c->stats.last_work += amount;
    return true;
}
Status svg_enter(Context *c)
{
    if (!c)
        return SVG_BAD_ARGUMENT;
    if (c->busy)
        return SVG_BUSY;
    c->busy = true;
    c->why = SVG_OK;
    c->stats.last_work = 0;
    c->work_limit = c->options.limits.work;
    return SVG_OK;
}
Status svg_leave(Context *c, Status s)
{
    c->busy = false;
    return s;
}
void *svg_alloc(Context *c, size_t bytes)
{
    if (bytes > SIZE_MAX - sizeof(Block) ||
        bytes + sizeof(Block) > SIZE_MAX - c->stats.live_bytes) {
        c->why = SVG_LIMIT;
        return NULL;
    }
    size_t total = bytes + sizeof(Block);
    if (!svg_spend(c, total))
        return NULL;
    void *p = NULL;
    os64_geometry_status_t s = c->options.memory.alloc(c->options.memory.context, total, &p);
    if (s != OS64_GEOMETRY_OK || !p) {
        c->why = s == OS64_GEOMETRY_LIMIT ? SVG_LIMIT : SVG_NO_MEMORY;
        return NULL;
    }
    Block *block = p;
    block->bytes = total;
    c->stats.live_bytes += total;
    if (c->stats.peak_bytes < c->stats.live_bytes)
        c->stats.peak_bytes = c->stats.live_bytes;
    return block + 1;
}
void svg_free(Context *c, void *p)
{
    if (!p)
        return;
    Block *block = (Block *)p - 1;
    size_t bytes = block->bytes;
    c->stats.live_bytes -= bytes;
    c->options.memory.free(c->options.memory.context, block, bytes);
}
void *svg_grow(Context *c, void *data, size_t *capacity, size_t count, size_t size, size_t limit)
{
    if (count >= limit || count == SIZE_MAX) {
        c->why = SVG_LIMIT;
        return data;
    }
    if (count < *capacity)
        return data;
    size_t n = *capacity > SIZE_MAX / 2 ? limit : *capacity * 2;
    if (n < 8)
        n = 8;
    if (n > limit)
        n = limit;
    if (n > SIZE_MAX / size) {
        c->why = SVG_LIMIT;
        return data;
    }
    void *p = svg_alloc(c, n * size);
    if (!p)
        return data;
    if (!svg_spend(c, count * size)) {
        svg_free(c, p);
        return data;
    }
    unsigned char *dst = p;
    const unsigned char *src = data;
    for (size_t i = 0; i < count * size; ++i)
        dst[i] = src[i];
    svg_free(c, data);
    *capacity = n;
    return p;
}
Status svg_create(const svg_options_t *o, Context **out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    if (!o || !o->memory.alloc || !o->memory.free)
        return SVG_BAD_ARGUMENT;
    Context temporary = {.options = *o, .busy = true, .work_limit = o->limits.work};
    Context *c = svg_alloc(&temporary, sizeof(*c));
    if (!c)
        return temporary.why;
    *c = temporary;
    c->busy = false;
    *out = c;
    return SVG_OK;
}
Status svg_destroy(Context *c)
{
    if (!c)
        return SVG_BAD_ARGUMENT;
    if (c->busy || c->stats.paths || c->stats.geometries || c->stats.scenes)
        return SVG_BUSY;
    c->busy = true;
    svg_free(c, c);
    return SVG_OK;
}
Status svg_stats(Context *c, svg_stats_t *out)
{
    if (!c || !out)
        return SVG_BAD_ARGUMENT;
    if (c->busy)
        return SVG_BUSY;
    *out = c->stats;
    return SVG_OK;
}
