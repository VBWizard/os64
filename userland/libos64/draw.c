// draw.c — libdraw's implementation: surface.c's rasterizer, in ring 3.
//
// PORTED, NOT REINVENTED (LIBDRAW.md's instruction, and the house rule):
// every clipping decision below is surface.c's, line for line where C
// allows, because that code earned its correctness compositing a live
// desktop. The differences are exactly three: the types wear their ABI
// names (os64/gui.h), text comes from the EMBEDDED font instead of the
// kernel's console font (the kernel stays out of app text, by design),
// and there is no flush — apps publish, only the compositor touches
// hardware.

#include "os64/draw.h"
#include "os64/font_psf1.h"
#include "os64/proc.h"     // os64_ticks / os64_sleep — the frame clock's feet
#include "os64/str.h"      // os64_strlen — ctx_text's measure

// The whole-surface rect, for clipping any caller rect to a surface.
static inline os64_gui_rect_t surface_bounds(const os64_gui_surface_t *s)
{
    return (os64_gui_rect_t){0, 0, (int32_t)s->width, (int32_t)s->height};
}

static inline uint32_t *surface_row(const os64_gui_surface_t *s, int32_t y)
{
    return s->pixels + (size_t)y * s->pitch_px;
}

// ── Rect helpers (gui_types.h's, verbatim logic) ────────────────────────────

os64_gui_rect_t os64_rect_union(os64_gui_rect_t a, os64_gui_rect_t b)
{
    if (os64_rect_is_empty(a)) return b;
    if (os64_rect_is_empty(b)) return a;
    int32_t x1 = a.x < b.x ? a.x : b.x;
    int32_t y1 = a.y < b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (os64_gui_rect_t){x1, y1, x2 - x1, y2 - y1};
}

bool os64_rect_intersect(os64_gui_rect_t a, os64_gui_rect_t b,
                         os64_gui_rect_t *out)
{
    int32_t x1 = a.x > b.x ? a.x : b.x;
    int32_t y1 = a.y > b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x2 <= x1 || y2 <= y1)
        return false;
    *out = (os64_gui_rect_t){x1, y1, x2 - x1, y2 - y1};
    return true;
}

// ── Primitives ──────────────────────────────────────────────────────────────

void os64_draw_fill_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                         uint32_t color)
{
    os64_gui_rect_t c;
    if (!os64_rect_intersect(r, surface_bounds(dst), &c))
        return;

    for (int32_t y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = surface_row(dst, y) + c.x;
        for (int32_t x = 0; x < c.w; x++)
            row[x] = color;
    }
}

void os64_draw_hline(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     int32_t len, uint32_t color)
{
    os64_draw_fill_rect(dst, (os64_gui_rect_t){x, y, len, 1}, color);
}

void os64_draw_vline(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     int32_t len, uint32_t color)
{
    os64_draw_fill_rect(dst, (os64_gui_rect_t){x, y, 1, len}, color);
}

void os64_draw_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                    uint32_t color)
{
    os64_draw_hline(dst, r.x, r.y, r.w, color);
    os64_draw_hline(dst, r.x, r.y + r.h - 1, r.w, color);
    os64_draw_vline(dst, r.x, r.y, r.h, color);
    os64_draw_vline(dst, r.x + r.w - 1, r.y, r.h, color);
}

int32_t os64_draw_text(os64_gui_surface_t *dst, int32_t x, int32_t y,
                       const char *str, size_t len,
                       uint32_t fg, uint32_t bg)
{
    // surface_draw_text's gait, glyphs from the embedded face: paint the
    // opaque 8-wide cell, clipped per cell via one intersect. No layout
    // logic beyond advancing the pen — wrapping and flow live upstairs.
    for (size_t i = 0; i < len; i++, x += OS64_FONT_GLYPH_W) {
        os64_gui_rect_t cell;
        if (!os64_rect_intersect(
                (os64_gui_rect_t){x, y, OS64_FONT_GLYPH_W, OS64_FONT_GLYPH_H},
                surface_bounds(dst), &cell))
            continue;

        const uint8_t *glyph = os64_font_glyph((uint8_t)str[i]);
        for (int32_t cy = cell.y; cy < cell.y + cell.h; cy++) {
            uint8_t bits = glyph[cy - y];
            uint32_t *row = surface_row(dst, cy);
            for (int32_t cx = cell.x; cx < cell.x + cell.w; cx++)
                row[cx] = (bits & (0x80u >> (cx - x))) ? fg : bg;
        }
    }
    return x;
}

// ── The draw context ────────────────────────────────────────────────────────

int64_t os64_draw_ctx_init(os64_draw_ctx_t *ctx, int64_t win)
{
    int64_t rc = os64_gui_window_get_surface(win, &ctx->surf);
    if (rc != 0)
        return rc;
    ctx->win = win;
    ctx->fg = OS64_GUI_COLOR_BLACK;
    ctx->bg = OS64_GUI_COLOR_WHITE;
    ctx->pen_x = 0;
    ctx->pen_y = 0;
    return 0;
}

int64_t os64_draw_ctx_refresh(os64_draw_ctx_t *ctx)
{
    // Deliberately NOT os64_draw_ctx_init: that one also resets fg/bg and the
    // pen, which would silently undo an app's palette every time somebody
    // dragged a corner.
    return os64_gui_window_get_surface(ctx->win, &ctx->surf);
}

void os64_draw_ctx_text(os64_draw_ctx_t *ctx, const char *str)
{
    ctx->pen_x = os64_draw_text(&ctx->surf, ctx->pen_x, ctx->pen_y,
                                str, os64_strlen(str), ctx->fg, ctx->bg);
}

// ── The frame clock ─────────────────────────────────────────────────────────

// Real time in milliseconds off the monotonic tick clock. per_second is
// read live, never baked in — a kernel rebuilt at a faster tick makes
// every app's dt arithmetic finer with zero recompiles (ticks.h doctrine).
static uint64_t now_ms(void)
{
    os64_ticks_t t = {0, 0};
    os64_ticks(&t);
    if (t.per_second == 0)
        return 0;   // pre-clock caller; the floor in frame_wait covers it
    return t.ticks * 1000u / t.per_second;
}

void os64_frame_clock_init(os64_frame_clock_t *clock)
{
    clock->last_ms = now_ms();
}

uint64_t os64_frame_wait(os64_frame_clock_t *clock, uint64_t budget_ms)
{
    uint64_t now = now_ms();
    uint64_t used = now - clock->last_ms;

    // Sleep out whatever remains of the budget. os64_sleep rounds UP to
    // the live tick, which is exactly the honest behavior for a cadence-
    // agnostic loop: on a 10ms-tick kernel a 16ms budget breathes at
    // 20ms; rebuild the kernel at 1ms ticks and the same binary breathes
    // at 16 — the SMP_MAGIC_NUMBER lesson, made structural.
    if (budget_ms > used)
        os64_sleep(budget_ms - used);

    now = now_ms();
    uint64_t dt = now - clock->last_ms;
    clock->last_ms = now;
    return dt > 0 ? dt : 1;   // never 0 — a zero dt freezes integrators
}
