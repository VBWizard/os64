#include "internal.h"
static canvas_state_t defaults(void) { return (canvas_state_t){.fill_argb=0xff000000,.global_alpha=1,.fill_alpha=1,
    .stroke_argb=0xff000000,.stroke_alpha=1,.transform={1,0,0,1,0,0},
    .line={1,10,OS64_GEOMETRY_BUTT,OS64_GEOMETRY_MITER},.text={.font="10px sans-serif",.size=10},.image_smoothing=true}; }
canvas_options_t canvas_options_default(os64_geometry_memory_t memory)
{
    return (canvas_options_t){.memory = memory, .max_dimension = 16384,
        .max_pixels = 8u * 1024 * 1024, .max_draw_pixels = 8u * 1024 * 1024,
        .max_bytes = 96u * 1024 * 1024, .max_saved_states = 256,
        .max_commands=4096,.max_geometry_work=16u*1024*1024};
}
canvas_status_t canvas_set_shadow_color(canvas_context_t *c,uint32_t rgb,double alpha)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(isfinite(alpha)&&alpha>=0&&alpha<=1) {
        c->state.shadow_argb=(rgb&0xffffff)|((uint32_t)(alpha*255+.5)<<24);
        c->state.shadow_alpha=alpha;
    }
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_shadow_value(canvas_context_t *c,unsigned member,double v)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(member>2)return c_leave(c,CANVAS_BAD_ARGUMENT);
    if(isfinite(v)&&(member||v>=0)) {
        if(!member)c->state.shadow_blur=v;
        else if(member==1)c->state.shadow_x=v;
        else c->state.shadow_y=v;
    }
    return c_leave(c,CANVAS_OK);
}
canvas_status_t c_enter(canvas_context_t *c)
{
    if (!c) return CANVAS_BAD_ARGUMENT;
    if (c->busy) return CANVAS_BUSY;
    c->busy = true;
    return CANVAS_OK;
}
canvas_status_t c_leave(canvas_context_t *c, canvas_status_t status)
{
    c->busy = false;
    return status;
}
canvas_status_t c_allocate(canvas_context_t *c, size_t bytes, void **out)
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
void c_release(canvas_context_t *c, void *p, size_t bytes)
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
        c_style_release(&s->state);
        c_clip_release(c, s->clip);
        c_release(c, s, sizeof(*s));
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
            status = c_allocate(c, bytes, &pixels);
            if (status != CANVAS_OK) return status;
        }
    }
    /* Admission finished. Clearing and stack drain cannot fail, including
     * a same-size reset that reuses storage under a tight owner budget. */
    for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i)
        ((uint32_t *)pixels)[i] = 0;
    if (!reuse)
        c_release(c, c->surface.pixels, c->surface.capacity);
    c->surface = (draw2d_surface_t){pixels, w, h, (size_t)w * sizeof(uint32_t), bytes,
                                    DRAW2D_ARGB8_PREMULTIPLIED};
    drain(c);
    c_style_release(&c->state);
    c_clip_release(c,c->clip);c->clip=NULL;
    c_path_release(c,&c->path);
    c->state = defaults();
    c->origin_clean = true;
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
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    drain(c);
    c_style_release(&c->state);
    c_clip_release(c,c->clip);
    c_path_release(c,&c->path);
    c_release(c, c->surface.pixels, c->surface.capacity);
    os64_geometry_memory_t memory = c->options.memory;
    memory.free(memory.context, c, sizeof(*c));
    return CANVAS_OK;
}
canvas_status_t canvas_reset(canvas_context_t *c, uint32_t w, uint32_t h)
{
    canvas_status_t status = c_enter(c);
    return status == CANVAS_OK ? c_leave(c, reset(c, w, h)) : status;
}
canvas_status_t canvas_save(canvas_context_t *c)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    if (c->stats.saved_states >= c->options.max_saved_states)
        return c_leave(c, CANVAS_LIMIT);
    void *storage;
    status = c_allocate(c, sizeof(Saved), &storage);
    if (status == CANVAS_OK) {
        Saved *s = storage;
        *s = (Saved){c->saved, c->state, c->clip};
        canvas_gradient_retain(s->state.fill_gradient);canvas_gradient_retain(s->state.stroke_gradient);
        if(c->clip)++c->clip->references;
        c->saved = s;
        ++c->stats.saved_states;
    }
    return c_leave(c, status);
}
canvas_status_t canvas_restore(canvas_context_t *c)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    if (c->saved) {
        Saved *s = c->saved;
        c->saved = s->next;
        c_style_release(&c->state);
        c->state = s->state;
        c_clip_release(c,c->clip);c->clip=s->clip;
        --c->stats.saved_states;
        c_release(c, s, sizeof(*s));
    }
    return c_leave(c, CANVAS_OK);
}
canvas_status_t canvas_set_fill(canvas_context_t *c, uint32_t argb)
{
    return canvas_set_fill_rgb_alpha(c, argb & 0xffffffu, (double)(argb >> 24) / 255);
}
canvas_status_t canvas_set_fill_rgb_alpha(canvas_context_t *c, uint32_t rgb, double alpha)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    if (!isfinite(alpha) || alpha < 0 || alpha > 1) return c_leave(c, CANVAS_BAD_ARGUMENT);
    canvas_gradient_release(c->state.fill_gradient);c->state.fill_gradient=NULL;
    c->state.fill_argb = ((uint32_t)(alpha * 255 + .5) << 24) | (rgb & 0xffffffu);
    c->state.fill_alpha = alpha;
    return c_leave(c, CANVAS_OK);
}
canvas_status_t canvas_set_alpha(canvas_context_t *c, double a)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    if (isfinite(a) && a >= 0 && a <= 1)
        c->state.global_alpha = a;
    return c_leave(c, CANVAS_OK);
}
void c_damage_union(canvas_context_t *c, draw2d_damage_t d)
{
    if (d.x1 <= d.x0 || d.y1 <= d.y0) return;
    if (c->damage.x1 <= c->damage.x0 || c->damage.y1 <= c->damage.y0) c->damage = d;
    else {
        if (d.x0 < c->damage.x0) c->damage.x0 = d.x0;
        if (d.y0 < c->damage.y0) c->damage.y0 = d.y0;
        if (d.x1 > c->damage.x1) c->damage.x1 = d.x1;
        if (d.y1 > c->damage.y1) c->damage.y1 = d.y1;
    }
}
canvas_status_t canvas_fill_rect(canvas_context_t *c,double x,double y,double w,double h)
{ return c_rectangle(c,x,y,w,h,0); }
canvas_status_t canvas_clear_rect(canvas_context_t *c,double x,double y,double w,double h)
{ return c_rectangle(c,x,y,w,h,1); }
canvas_status_t canvas_stroke_rect(canvas_context_t *c,double x,double y,double w,double h)
{ return c_rectangle(c,x,y,w,h,2); }
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
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    c->damage = (draw2d_damage_t){0};
    return c_leave(c, CANVAS_OK);
}

static canvas_status_t rgba_validate(const canvas_context_t *c, const canvas_rgba8_t *v)
{
    if (!v) return CANVAS_BAD_ARGUMENT;
    if (!v->width || !v->height) return CANVAS_OK;
    if (!v->bytes) return CANVAS_BAD_ARGUMENT;
    if ((uint64_t)v->width * 4 > SIZE_MAX) return CANVAS_LIMIT;
    size_t row = (size_t)v->width * 4;
    if (v->stride < row) return CANVAS_BAD_ARGUMENT;
    if (v->height > 1 && v->stride > (SIZE_MAX - row) / (v->height - 1)) return CANVAS_LIMIT;
    size_t used = (size_t)(v->height - 1) * v->stride + row;
    if (used > v->capacity) return CANVAS_LIMIT;
    uintptr_t a = (uintptr_t)v->bytes, b = (uintptr_t)c->surface.pixels;
    if (c->surface.capacity && (a <= b ? b - a < used : a - b < c->surface.capacity))
        return CANVAS_BAD_ARGUMENT;
    return CANVAS_OK;
}
/* Place a source axis at offset in a bounded destination. Reject distant
 * offsets before negating/subtracting so INT64_MIN/MAX are harmless. */
static bool transfer_axis(int64_t offset, uint32_t source, uint32_t dest,
                           uint32_t *s, uint32_t *d, uint32_t *count)
{
    if (!source || !dest || offset >= (int64_t)dest || offset <= -(int64_t)source) return false;
    *s = offset < 0 ? (uint32_t)-offset : 0;
    *d = offset > 0 ? (uint32_t)offset : 0;
    *count = source - *s < dest - *d ? source - *s : dest - *d;
    return true;
}
canvas_status_t canvas_read_pixels(canvas_context_t *c, int64_t x, int64_t y,
                                    const canvas_rgba8_t *out)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    status = rgba_validate(c, out);
    if (status != CANVAS_OK) return c_leave(c, status);
    if ((uint64_t)out->width * out->height > c->options.max_draw_pixels)
        return c_leave(c, CANVAS_LIMIT);
    if (!out->width || !out->height) return c_leave(c, CANVAS_OK);
    uint32_t ox = 0, oy = 0, bx = 0, by = 0, w = 0, h = 0;
    bool overlap = transfer_axis(x, out->width, c->surface.width, &ox, &bx, &w) &&
                   transfer_axis(y, out->height, c->surface.height, &oy, &by, &h);
    for (uint32_t py = 0; py < out->height; ++py) {
        uint8_t *row = out->bytes + (size_t)py * out->stride;
        for (uint32_t px = 0; px < out->width; ++px) {
            uint32_t p = 0;
            if (overlap && px >= ox && px - ox < w && py >= oy && py - oy < h)
                p = ((const uint32_t *)c->surface.pixels)[
                    (size_t)(by + py - oy) * c->surface.width + bx + px - ox];
            unsigned a = p >> 24;
            row[4 * (size_t)px] = a ? (((p >> 16) & 255) * 255 + a / 2) / a : 0;
            row[4 * (size_t)px + 1] = a ? (((p >> 8) & 255) * 255 + a / 2) / a : 0;
            row[4 * (size_t)px + 2] = a ? ((p & 255) * 255 + a / 2) / a : 0;
            row[4 * (size_t)px + 3] = a;
        }
    }
    return c_leave(c, CANVAS_OK);
}
canvas_status_t canvas_put_pixels(canvas_context_t *c, const canvas_rgba8_t *source,
                                   int64_t x, int64_t y, const draw2d_damage_t *dirty)
{
    canvas_status_t status = c_enter(c);
    if (status != CANVAS_OK) return status;
    status = rgba_validate(c, source);
    if (status != CANVAS_OK) return c_leave(c, status);
    uint32_t sx, sy, dx, dy, w, h;
    if (!transfer_axis(x, source->width, c->surface.width, &sx, &dx, &w) ||
        !transfer_axis(y, source->height, c->surface.height, &sy, &dy, &h))
        return c_leave(c, CANVAS_OK);
    if (dirty) {
        uint32_t ex = sx + w, ey = sy + h;
        if (dirty->x1 < ex) ex = dirty->x1;
        if (dirty->y1 < ey) ey = dirty->y1;
        if (dirty->x0 > sx) {
            if (dirty->x0 >= ex) return c_leave(c, CANVAS_OK);
            dx += dirty->x0 - sx; sx = dirty->x0;
        }
        if (dirty->y0 > sy) {
            if (dirty->y0 >= ey) return c_leave(c, CANVAS_OK);
            dy += dirty->y0 - sy; sy = dirty->y0;
        }
        if (ex <= sx || ey <= sy) return c_leave(c, CANVAS_OK);
        w = ex - sx; h = ey - sy;
    }
    if ((uint64_t)w * h > c->options.max_draw_pixels) return c_leave(c, CANVAS_LIMIT);
    for (uint32_t py = 0; py < h; ++py) {
        const uint8_t *row = source->bytes + (size_t)(sy + py) * source->stride + (size_t)sx * 4;
        uint32_t *dest = (uint32_t *)c->surface.pixels + (size_t)(dy + py) * c->surface.width + dx;
        for (uint32_t px = 0; px < w; ++px) {
            unsigned a = row[4 * (size_t)px + 3];
            unsigned r = (row[4 * (size_t)px] * a + 127) / 255;
            unsigned g = (row[4 * (size_t)px + 1] * a + 127) / 255;
            unsigned b = (row[4 * (size_t)px + 2] * a + 127) / 255;
            dest[px] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    c_damage_union(c, (draw2d_damage_t){dx, dy, dx + w, dy + h});
    return c_leave(c, CANVAS_OK);
}

canvas_status_t canvas_set_text_style(canvas_context_t *c,const canvas_text_style_t *style)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!style||style->align>4||style->baseline>5||style->direction>2||
       !isfinite(style->size)||style->size<0||style->size>1280||style->font[1023])return c_leave(c,CANVAS_BAD_ARGUMENT);
    c->state.text=*style;return c_leave(c,CANVAS_OK);
}
