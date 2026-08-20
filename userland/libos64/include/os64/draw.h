// os64/draw.h — libdraw, the userland drawing core (L1 of LIBDRAW.md).
//
// A near-direct port of the kernel's gui/surface.c rasterizer — tested
// kernel code moved to ring 3, per the design's happy accident — so no app
// author ever sets a pixel by hand. Everything draws into the mapped
// canvas at memory speed; only os64_draw_publish crosses the ring.
//
// Every primitive CLIPS against the surface: rects hanging off any edge
// are legal, always. That is the one invariant the port preserves above
// all others — a primitive that writes outside its canvas corrupts a
// neighbouring window (the failure fingerprint LIBDRAW.md names).
//
// The draw context is CONVENIENCE, NEVER A MANDATE (design decision #4):
// it carries the target, fg/bg, and a text pen so calls flow without
// repeating arguments — but the explicit primitives stay available
// underneath for when you want them.

#ifndef OS64_DRAW_H
#define OS64_DRAW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/gui.h"

// ── Rect helpers (ported from gui_types.h) ──────────────────────────────────

static inline bool os64_rect_is_empty(os64_gui_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

os64_gui_rect_t os64_rect_union(os64_gui_rect_t a, os64_gui_rect_t b);
bool os64_rect_intersect(os64_gui_rect_t a, os64_gui_rect_t b,
                         os64_gui_rect_t *out);

// ── Primitives (ported from surface.c; all clip to the surface) ─────────────

void os64_draw_fill_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                         uint32_t color);
void os64_draw_hline(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     int32_t len, uint32_t color);
void os64_draw_vline(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     int32_t len, uint32_t color);
void os64_draw_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                    uint32_t color);   // outline

// Text: the embedded PSF1 face (os64/font_psf1.h — the console's own,
// 8x16, opaque fg-on-bg cells). Pen-advance only; wrapping and flow are
// higher layers' business. Returns the x the pen ended at.
int32_t os64_draw_text(os64_gui_surface_t *dst, int32_t x, int32_t y,
                       const char *str, size_t len,
                       uint32_t fg, uint32_t bg);

// ── The draw context ────────────────────────────────────────────────────────

typedef struct os64_draw_ctx
{
    os64_gui_surface_t surf;   // the canvas (by value — it's 24 bytes)
    int64_t  win;              // the window handle, for publish
    uint32_t fg, bg;
    int32_t  pen_x, pen_y;     // text pen; ctx_text advances pen_x
} os64_draw_ctx_t;

// Bind a context to a window: fetches the canvas, seeds fg/bg with the
// house ink-on-paper defaults. Returns 0 or a negative OS64_GUI_ERR_*.
int64_t os64_draw_ctx_init(os64_draw_ctx_t *ctx, int64_t win);

// Re-fetch the window's geometry after an OS64_GUI_EVENT_WINDOW_RESIZE.
// Cheap and total: the canvas POINTER and its pitch never change (the kernel
// reserved both at capacity — see os64_gui_surface_t.pitch_px), so this
// updates width and height and nothing else. Colors and the text pen survive,
// because a resize is news about the window, not about the app's drawing
// state.
//
// Call it BEFORE repainting, not after: every primitive clips to
// ctx->surf.width/height, so repainting a grown window with the old numbers
// leaves the new strip untouched — a symptom that looks like a damage bug
// and isn't one.
int64_t os64_draw_ctx_refresh(os64_draw_ctx_t *ctx);

static inline void os64_draw_ctx_colors(os64_draw_ctx_t *ctx,
                                        uint32_t fg, uint32_t bg)
{
    ctx->fg = fg;
    ctx->bg = bg;
}

static inline void os64_draw_ctx_pen(os64_draw_ctx_t *ctx,
                                     int32_t x, int32_t y)
{
    ctx->pen_x = x;
    ctx->pen_y = y;
}

// Draw NUL-terminated text at the pen in the context colors; the pen
// advances past what was drawn.
void os64_draw_ctx_text(os64_draw_ctx_t *ctx, const char *str);

// Publish through the context: damage NULL = whole content.
static inline int64_t os64_draw_publish(os64_draw_ctx_t *ctx,
                                        const os64_gui_rect_t *damage)
{
    return os64_gui_window_publish(ctx->win, damage);
}

// ── The frame clock (LIBDRAW.md's cadence-agnostic loop) ────────────────────
// Animation timing in MILLISECONDS, never frame counts — the standing
// lesson from the SMP_MAGIC_NUMBER healing (apps tripled their frame rate
// overnight with zero recompiles) and from the P5's screaming ball
// (per-frame velocity meets real hardware and doubles its speed). An app
// advances state by the dt these return and gets smoother for free
// whenever the scheduler gets faster.

typedef struct os64_frame_clock
{
    uint64_t last_ms;          // real time at the last frame boundary
} os64_frame_clock_t;

void os64_frame_clock_init(os64_frame_clock_t *clock);

// Sleep out the remainder of `budget_ms` since the last frame boundary
// (0 = don't sleep, just measure), then advance the boundary. Returns the
// REAL elapsed ms since the previous call — the dt to advance state by.
// Never returns 0: a dt of zero would freeze integrators, and the floor
// of 1ms is honest about the tick clock's granularity.
uint64_t os64_frame_wait(os64_frame_clock_t *clock, uint64_t budget_ms);

#endif // OS64_DRAW_H
