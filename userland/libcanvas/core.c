#include <canvas/canvas.h>
#include <math.h>

typedef struct Saved {
    struct Saved *next;
    canvas_state_t state;
} Saved;
struct canvas_context {
    canvas_options_t options;
    canvas_stats_t stats;
    draw2d_surface_t surface;
    draw2d_damage_t damage;
    canvas_state_t state;
    Saved *saved;
    bool busy;
};
static canvas_state_t defaults(void) { return (canvas_state_t){0xff000000, 1}; }
canvas_options_t canvas_options_default(os64_geometry_memory_t memory)
{
    return (canvas_options_t){.memory = memory, .max_dimension = 16384,
        .max_pixels = 8u * 1024 * 1024, .max_draw_pixels = 8u * 1024 * 1024,
        .max_bytes = 96u * 1024 * 1024, .max_saved_states = 256};
}
static canvas_status_t enter(canvas_context_t *c)
{
    if (!c) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    c->busy = true;
    return CANVAS_OK;
}
static canvas_status_t leave(canvas_context_t *c, canvas_status_t status)
{
    c->busy = false;
    return status;
}
static canvas_status_t allocate(canvas_context_t *c, size_t bytes, void **out)
{
    *out = NULL;
    if (bytes > c->options.max_bytes - c->stats.live_bytes)
        return CANVAS_LIMIT;
    os64_geometry_status_t result = c->options.memory.alloc(c->options.memory.context, bytes, out);
    if (result != OS64_GEOMETRY_OK || !*out)
        return result == OS64_GEOMETRY_LIMIT ? CANVAS_LIMIT : CANVAS_NO_MEMORY;
    c->stats.live_bytes += bytes;
    if (c->stats.peak_bytes < c->stats.live_bytes)
        c->stats.peak_bytes = c->stats.live_bytes;
    return CANVAS_OK;
}
static void release(canvas_context_t *c, void *p, size_t bytes)
{
    if (!p) return;
    c->stats.live_bytes -= bytes;
    c->options.memory.free(c->options.memory.context, p, bytes);
}
static void drain(canvas_context_t *c)
{
    while (c->saved) {
        Saved *s = c->saved;
        c->saved = s->next;
        release(c, s, sizeof(*s));
    }
    c->stats.saved_states = 0;
}
static canvas_status_t dimensions(const canvas_options_t *o, uint32_t w, uint32_t h, size_t *bytes)
{
    if (w > o->max_dimension || h > o->max_dimension ||
        (uint64_t)w * h > o->max_pixels || (uint64_t)w * h > SIZE_MAX / sizeof(uint32_t) ||
        (uint64_t)w * sizeof(uint32_t) > SIZE_MAX)
        return CANVAS_LIMIT;
    *bytes = (size_t)w * h * sizeof(uint32_t);
    return CANVAS_OK;
}
static canvas_status_t reset(canvas_context_t *c, uint32_t w, uint32_t h)
{
    size_t bytes;
    canvas_status_t status = dimensions(&c->options, w, h, &bytes);
    if (status != CANVAS_OK) return status;
    void *pixels = c->surface.pixels;
    bool reuse = bytes == c->surface.capacity;
    if (!reuse) {
        pixels = NULL;
        if (bytes) {
            status = allocate(c, bytes, &pixels);
            if (status != CANVAS_OK) return status;
        }
    }
    /* Admission finished. Clearing and stack drain cannot fail, including
     * a same-size reset that reuses storage under a tight owner budget. */
    for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i)
        ((uint32_t *)pixels)[i] = 0;
    if (!reuse)
        release(c, c->surface.pixels, c->surface.capacity);
    c->surface = (draw2d_surface_t){pixels, w, h, (size_t)w * sizeof(uint32_t), bytes,
                                    DRAW2D_ARGB8_PREMULTIPLIED};
    drain(c);
    c->state = defaults();
    c->damage = w && h ? (draw2d_damage_t){0, 0, w, h} : (draw2d_damage_t){0};
    return CANVAS_OK;
}
canvas_status_t canvas_create(const canvas_options_t *o, uint32_t w, uint32_t h,
                               canvas_context_t **out)
{
    if (!out) return CANVAS_BAD_ARGUMENT;
    *out = NULL;
    if (!o || !o->memory.alloc || !o->memory.free) return CANVAS_BAD_ARGUMENT;
    size_t bytes;
    canvas_status_t status = dimensions(o, w, h, &bytes);
    if (status != CANVAS_OK) return status;
    if (sizeof(canvas_context_t) > o->max_bytes || bytes > o->max_bytes - sizeof(canvas_context_t))
        return CANVAS_LIMIT;
    void *storage = NULL;
    os64_geometry_status_t result = o->memory.alloc(o->memory.context, sizeof(canvas_context_t), &storage);
    if (result != OS64_GEOMETRY_OK || !storage)
        return result == OS64_GEOMETRY_LIMIT ? CANVAS_LIMIT : CANVAS_NO_MEMORY;
    canvas_context_t *c = storage;
    *c = (canvas_context_t){.options = *o, .busy = true,
        .stats = {.live_bytes = sizeof(*c), .peak_bytes = sizeof(*c)}};
    status = reset(c, w, h);
    if (status != CANVAS_OK) {
        o->memory.free(o->memory.context, c, sizeof(*c));
        return status;
    }
    c->busy = false;
    *out = c;
    return CANVAS_OK;
}
canvas_status_t canvas_destroy(canvas_context_t *c)
{
    if (!c) return CANVAS_OK;
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    drain(c);
    release(c, c->surface.pixels, c->surface.capacity);
    os64_geometry_memory_t memory = c->options.memory;
    memory.free(memory.context, c, sizeof(*c));
    return CANVAS_OK;
}
canvas_status_t canvas_reset(canvas_context_t *c, uint32_t w, uint32_t h)
{
    canvas_status_t status = enter(c);
    return status == CANVAS_OK ? leave(c, reset(c, w, h)) : status;
}
canvas_status_t canvas_save(canvas_context_t *c)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    if (c->stats.saved_states >= c->options.max_saved_states)
        return leave(c, CANVAS_LIMIT);
    void *storage;
    status = allocate(c, sizeof(Saved), &storage);
    if (status == CANVAS_OK) {
        Saved *s = storage;
        *s = (Saved){c->saved, c->state};
        c->saved = s;
        ++c->stats.saved_states;
    }
    return leave(c, status);
}
canvas_status_t canvas_restore(canvas_context_t *c)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    if (c->saved) {
        Saved *s = c->saved;
        c->saved = s->next;
        c->state = s->state;
        --c->stats.saved_states;
        release(c, s, sizeof(*s));
    }
    return leave(c, CANVAS_OK);
}
canvas_status_t canvas_set_fill(canvas_context_t *c, uint32_t argb)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    c->state.fill_argb = argb;
    return leave(c, CANVAS_OK);
}
canvas_status_t canvas_set_alpha(canvas_context_t *c, double a)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    if (isfinite(a) && a >= 0 && a <= 1)
        c->state.global_alpha = a;
    return leave(c, CANVAS_OK);
}
static canvas_status_t rectangle(canvas_context_t *c, double x, double y, double w, double h,
                                  draw2d_operator_t op)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    draw2d_damage_t d;
    draw2d_status_t result = draw2d_paint_rect(&c->surface, x, y, w, h,
        (draw2d_solid_t){c->state.fill_argb},
        (draw2d_composite_t){op, (uint16_t)(c->state.global_alpha * 65535 + .5)},
        c->options.max_draw_pixels, &d);
    if (result == DRAW2D_OK && d.x1 > d.x0 && d.y1 > d.y0) {
        if (c->damage.x1 <= c->damage.x0 || c->damage.y1 <= c->damage.y0) c->damage = d;
        else {
            if (d.x0 < c->damage.x0) c->damage.x0 = d.x0;
            if (d.y0 < c->damage.y0) c->damage.y0 = d.y0;
            if (d.x1 > c->damage.x1) c->damage.x1 = d.x1;
            if (d.y1 > c->damage.y1) c->damage.y1 = d.y1;
        }
    }
    return leave(c, result == DRAW2D_OK ? CANVAS_OK : result == DRAW2D_LIMIT ? CANVAS_LIMIT
                                                                                       : CANVAS_BAD_ARGUMENT);
}
canvas_status_t canvas_fill_rect(canvas_context_t *c, double x, double y, double w, double h)
{ return rectangle(c, x, y, w, h, DRAW2D_SOURCE_OVER); }
canvas_status_t canvas_clear_rect(canvas_context_t *c, double x, double y, double w, double h)
{ return rectangle(c, x, y, w, h, DRAW2D_CLEAR); }
canvas_status_t canvas_state(const canvas_context_t *c, canvas_state_t *out)
{
    if (!c || !out) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    *out = c->state;
    return CANVAS_OK;
}
canvas_status_t canvas_bitmap(const canvas_context_t *c, canvas_bitmap_t *out)
{
    if (!c || !out) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    *out = (canvas_bitmap_t){c->surface.pixels, c->surface.width, c->surface.height,
                              c->surface.stride, c->surface.capacity};
    return CANVAS_OK;
}
canvas_status_t canvas_stats(const canvas_context_t *c, canvas_stats_t *out)
{
    if (!c || !out) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    *out = c->stats;
    return CANVAS_OK;
}
canvas_status_t canvas_damage(const canvas_context_t *c, draw2d_damage_t *out)
{
    if (!c || !out) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    *out = c->damage;
    return CANVAS_OK;
}
canvas_status_t canvas_ack_damage(canvas_context_t *c)
{
    canvas_status_t status = enter(c);
    if (status != CANVAS_OK) return status;
    c->damage = (draw2d_damage_t){0};
    return leave(c, CANVAS_OK);
}
