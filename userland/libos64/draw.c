// draw.c — libdraw's implementation: surface.c's rasterizer, in ring 3.
//
// Software drawing into userland canvases. Primitives clip to the surface;
// legacy byte text may additionally clip to a widget rectangle. These glyphs
// come from the embedded compatibility font; text_draw.c paints positioned
// outline/bitmap runs. Publishing hands completed pixels to the compositor.

#include "os64/draw.h"
#include "os64/font_psf1.h"
#include "os64/charset.h"
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
    // THE EDGE SUMS ARE WIDENED (Codex #30 rd5). A rect is allowed to sit
    // anywhere — the blit contract says an off-canvas placement is a no-op,
    // and "anywhere" includes x == INT32_MAX with w == 1, where `x + w` in
    // int32_t is signed overflow: undefined, and the sanitizer run this
    // file's tests are documented to use flags it. Every primitive above
    // clips through this one function, so widening here keeps the promise
    // for all of them at once. The RESULT still fits int32_t: it lies
    // inside `b`, which is a real surface's bounds.
    int64_t x1 = a.x > b.x ? a.x : b.x;
    int64_t y1 = a.y > b.y ? a.y : b.y;
    int64_t ax2 = (int64_t)a.x + a.w, bx2 = (int64_t)b.x + b.w;
    int64_t ay2 = (int64_t)a.y + a.h, by2 = (int64_t)b.y + b.h;
    int64_t x2 = ax2 < bx2 ? ax2 : bx2;
    int64_t y2 = ay2 < by2 ? ay2 : by2;
    if (x2 <= x1 || y2 <= y1)
        return false;
    *out = (os64_gui_rect_t){(int32_t)x1, (int32_t)y1,
                             (int32_t)(x2 - x1), (int32_t)(y2 - y1)};
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

void os64_draw_blit(os64_gui_surface_t *dst, int32_t x, int32_t y,
                    const uint32_t *src, uint32_t w, uint32_t h,
                    uint32_t src_pitch_px)
{
    if (dst == NULL || src == NULL || w == 0 || h == 0)
        return;
    // A pitch narrower than the width would make every row read past its own
    // end — refuse rather than walk the source buffer diagonally.
    if (src_pitch_px < w)
        return;
    // The clip is computed in the DESTINATION's coordinates, exactly like
    // fill_rect, and then the same offset is applied to the source. That is
    // what makes a negative x or y clip the LEFT of the image instead of
    // drawing it shifted — the two rects move together.
    if ((int64_t)w > 0x7fffffffLL || (int64_t)h > 0x7fffffffLL)
        return;
    os64_gui_rect_t want = {x, y, (int32_t)w, (int32_t)h};
    os64_gui_rect_t c;
    if (!os64_rect_intersect(want, surface_bounds(dst), &c))
        return;

    // How far into the source the clip started. Both are >= 0 because c is
    // the intersection, so c.x >= want.x always.
    uint32_t skip_x = (uint32_t)(c.x - want.x);
    uint32_t skip_y = (uint32_t)(c.y - want.y);

    for (int32_t row = 0; row < c.h; row++) {
        const uint32_t *s = src + (size_t)(skip_y + (uint32_t)row) * src_pitch_px
                                + skip_x;
        uint32_t *d = surface_row(dst, c.y + row) + c.x;
        for (int32_t col = 0; col < c.w; col++)
            d[col] = s[col];
    }
}

void os64_draw_blend(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     const uint32_t *src, uint32_t w, uint32_t h,
                     uint32_t src_pitch_px)
{
    if (!dst || !src || !w || !h || src_pitch_px < w ||
        w > INT32_MAX || h > INT32_MAX)
        return;
    os64_gui_rect_t want = {x, y, (int32_t)w, (int32_t)h}, c;
    if (!os64_rect_intersect(want, surface_bounds(dst), &c))
        return;
    // Derive source offsets after clipping, keeping extreme offscreen
    // origins out of pointer arithmetic just as the copy operation does.
    uint32_t skip_x = (uint32_t)((int64_t)c.x - x);
    uint32_t skip_y = (uint32_t)((int64_t)c.y - y);
    for (int32_t row = 0; row < c.h; row++) {
        const uint32_t *s = src + (size_t)(skip_y + (uint32_t)row) * src_pitch_px + skip_x;
        uint32_t *d = surface_row(dst, c.y + row) + c.x;
        int32_t col = 0;
        while (col < c.w) {
            uint32_t pixel = s[col], alpha = pixel >> 24;
            if (!alpha) {
                col++;
                continue;
            }
            // Opaque runs need neither destination reads nor blend arithmetic.
            if (alpha == 255) {
                do {
                    d[col] = s[col];
                    col++;
                } while (col < c.w && (s[col] >> 24) == 255);
                continue;
            }
            uint32_t back = d[col], inv = 255 - alpha;
            uint32_t r = (((pixel >> 16) & 255) * alpha + ((back >> 16) & 255) * inv + 127) / 255;
            uint32_t g = (((pixel >> 8) & 255) * alpha + ((back >> 8) & 255) * inv + 127) / 255;
            uint32_t b = ((pixel & 255) * alpha + (back & 255) * inv + 127) / 255;
            d[col++] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void os64_draw_vline(os64_gui_surface_t *dst, int32_t x, int32_t y,
                     int32_t len, uint32_t color)
{
    os64_draw_fill_rect(dst, (os64_gui_rect_t){x, y, 1, len}, color);
}

void os64_draw_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                    uint32_t color)
{
    if (r.w <= 0 || r.h <= 0) return;
    os64_draw_hline(dst, r.x, r.y, r.w, color);
    os64_draw_hline(dst, r.x, r.y + r.h - 1, r.w, color);
    os64_draw_vline(dst, r.x, r.y, r.h, color);
    os64_draw_vline(dst, r.x + r.w - 1, r.y, r.h, color);
}

static int32_t round_radius(os64_gui_rect_t r, int32_t radius)
{
    if (radius < 0) return 0;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;
    return radius > 0 ? radius : 0;
}

// Test pixel centers against a circle, finding the first covered column.
// Binary search bounds the work even for a huge, mostly offscreen rectangle.
static int32_t round_inset(int32_t height, int32_t radius, int64_t row)
{
    if (!radius) return 0;
    if (row >= height / 2) row = height - 1 - row;
    if (row >= radius) return 0;
    int64_t dy = 2 * (int64_t)radius - 2 * row - 1;
    int64_t rr = 4 * (int64_t)radius * radius;
    int32_t lo = 0, hi = radius;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        int64_t dx = 2 * (int64_t)radius - 2 * mid - 1;
        if (dx * dx + dy * dy <= rr) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

void os64_draw_fill_round_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                               int32_t radius, uint32_t color)
{
    os64_gui_rect_t clip;
    if (os64_rect_is_empty(r) || !os64_rect_intersect(r, surface_bounds(dst), &clip)) return;
    radius = round_radius(r, radius);
    for (int32_t y = clip.y; y < clip.y + clip.h; ++y) {
        int32_t inset = round_inset(r.h, radius, (int64_t)y - r.y);
        int64_t left = (int64_t)r.x + inset, right = (int64_t)r.x + r.w - inset;
        if (left < clip.x) left = clip.x;
        if (right > (int64_t)clip.x + clip.w) right = (int64_t)clip.x + clip.w;
        uint32_t *row = surface_row(dst, y);
        for (int64_t x = left; x < right; ++x) row[x] = color;
    }
}

void os64_draw_round_rect(os64_gui_surface_t *dst, os64_gui_rect_t r,
                          int32_t radius, uint32_t top_left, uint32_t bottom_right)
{
    os64_gui_rect_t clip;
    if (os64_rect_is_empty(r) || !os64_rect_intersect(r, surface_bounds(dst), &clip)) return;
    radius = round_radius(r, radius);
    for (int32_t y = clip.y; y < clip.y + clip.h; ++y) {
        int64_t ry = (int64_t)y - r.y;
        int32_t outer = round_inset(r.h, radius, ry);
        bool inner = r.w > 2 && ry > 0 && ry < r.h - 1;
        int32_t in = inner ? 1 + round_inset(r.h - 2, radius > 0 ? radius - 1 : 0, ry - 1) : 0;
        uint32_t *row = surface_row(dst, y);
        for (int32_t x = clip.x; x < clip.x + clip.w; ++x) {
            int64_t rx = (int64_t)x - r.x;
            if (rx < outer || rx >= r.w - outer) continue;
            if (inner && rx >= in && rx < r.w - in) continue;
            row[x] = rx * r.h + ry * r.w < (int64_t)r.w * r.h ? top_left : bottom_right;
        }
    }
}

int32_t os64_draw_text(os64_gui_surface_t *dst, int32_t x, int32_t y,
                       const char *str, size_t len,
                       uint32_t fg, uint32_t bg)
{
    return os64_draw_text_charset(dst, x, y, str, len, fg, bg,
                                  OS64_CHARSET_LATIN1);
}

static int32_t text_clipped(os64_gui_surface_t *dst, os64_gui_rect_t clip,
                            int32_t x, int32_t y,
                               const char *str, size_t len,
                               uint32_t fg, uint32_t bg, uint8_t charset)
{
    // Intersect once with the canvas, then clip each glyph to that region.
    if (!os64_rect_intersect(clip, surface_bounds(dst), &clip))
        clip = (os64_gui_rect_t){0, 0, 0, 0};
    for (size_t i = 0; i < len; i++, x += OS64_FONT_GLYPH_W) {
        os64_gui_rect_t cell;
        if (!os64_rect_intersect(
                (os64_gui_rect_t){x, y, OS64_FONT_GLYPH_W, OS64_FONT_GLYPH_H},
                clip, &cell))
            continue;

        // The SAME map the kernel's painter uses, against this face rather
        // than the console's — which is what keeps a gterm and the glass
        // showing one terminal's bytes the same way.
        const uint8_t *glyph = os64_charset_glyph(
            (uint8_t)str[i], charset, os64_font_glyph(0),
            OS64_FONT_GLYPHS, OS64_FONT_GLYPH_H);
        for (int32_t cy = cell.y; cy < cell.y + cell.h; cy++) {
            uint8_t bits = glyph[cy - y];
            uint32_t *row = surface_row(dst, cy);
            for (int32_t cx = cell.x; cx < cell.x + cell.w; cx++)
                row[cx] = (bits & (0x80u >> (cx - x))) ? fg : bg;
        }
    }
    return x;
}

int32_t os64_draw_text_charset(os64_gui_surface_t *dst, int32_t x, int32_t y,
                               const char *str, size_t len,
                               uint32_t fg, uint32_t bg, uint8_t charset)
{
    return text_clipped(dst, surface_bounds(dst), x, y, str, len, fg, bg, charset);
}

int32_t os64_draw_text_clipped(os64_gui_surface_t *dst, os64_gui_rect_t clip,
                              int32_t x, int32_t y, const char *str, size_t len,
                              uint32_t fg, uint32_t bg)
{
    return text_clipped(dst, clip, x, y, str, len, fg, bg, OS64_CHARSET_LATIN1);
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
    clock->window = -1;
    clock->immediate_wakes = 0;
}

void os64_frame_clock_bind(os64_frame_clock_t *clock, int64_t window)
{
    clock->window = window;
}

// What a covered caller that does not drain its queue pays per pass instead
// of spinning (see the wait below). Twenty passes a second is bounded and
// harmless; a draining caller never reaches it, because it is charged only
// from the SECOND immediate wake in a row (one can be an honest race).
#define COVERED_UNDRAINED_NAP_MS 50

// Is the bound window covered right now? Asks the kernel, never the last
// event: the flag is the truth (gui.h). A window that cannot be asked (it
// died, the handle is stale) reads as visible, so a broken bind degrades to
// the old always-paint behavior rather than to a program that never wakes.
static bool bound_window_covered(const os64_frame_clock_t *clock)
{
    if (clock->window < 0)
        return false;
    os64_gui_window_state_t st;
    if (os64_gui_window_get_state(clock->window, &st) != 0)
        return false;
    return (st.flags & OS64_GUI_WINDOW_COVERED) != 0;
}

uint64_t os64_frame_wait(os64_frame_clock_t *clock, uint64_t budget_ms)
{
    uint64_t now = now_ms();
    uint64_t used = now - clock->last_ms;

    // Nobody watching? Then nobody is owed a frame. Asked BEFORE the cadence
    // sleep below, because that sleep registers no waiter: a covered window
    // that slept its budget first would leave a key or an Alt+F4 queued
    // behind it for the whole budget. Sleep instead until the window
    // has an event to service — the UNCOVERED nudge, or a key, or Alt+F4:
    // a covered window can still be the FOCUSED one (a window created
    // START_UNFOCUSED over it leaves it so), and its keys must reach its
    // loop, not rot in a queue behind a nap. The peek-wait leaves the event
    // for the app's own poll; we return so the app's loop runs one pass and
    // handles it, and if the window is still covered the next call sleeps
    // again. The flag is re-read on every call, never inferred from the
    // event (gui.h), so a dropped UNCOVERED costs one pass, not forever.
    // The dt pretends the pause never happened (draw.h).
    // A zero budget means "don't sleep, just measure" (draw.h) — and that
    // promise holds while covered too: a caller sampling elapsed time must
    // not hang because its window went behind another.
    if (budget_ms > 0 && bound_window_covered(clock))
    {
        uint64_t entered = now_ms();
        os64_gui_event_wait(clock->window, (os64_gui_event_t *)0);
        clock->last_ms = now_ms();
        // Woken by the UNCOVERED nudge, or by a key under a window that
        // still hides us? The flag decides. Still covered means no frame is
        // owed: a dt of ZERO, so an integrator holds still and a key held
        // down under the covering window cannot drive the animation
        // forward behind it — the one case the "never 0" rule yields to,
        // and draw.h says so. Uncovered means the pause is over and the
        // frame is one budget long, as before.
        if (!bound_window_covered(clock))
        {
            clock->immediate_wakes = 0;
            return budget_ms;
        }
        // Still covered, and the wait returned without time passing. Once,
        // that is ordinary: an event queued just before the wait returns in
        // the same 10ms tick even for a caller that drains faithfully. TWICE
        // IN A ROW it is not — a drained queue cannot wake the next wait at
        // once — so the caller is not draining (the COVERED nudge itself is
        // enough to do this to a program that never polls), and left alone
        // that is a spin at full speed behind a cover, the exact thing a
        // bound clock exists to prevent. From the second immediate wake on,
        // the price of not draining is a nap, not a core; a draining caller
        // never reaches it.
        if (clock->last_ms == entered)
        {
            if (++clock->immediate_wakes >= 2)
                os64_sleep(COVERED_UNDRAINED_NAP_MS);
        }
        else
            clock->immediate_wakes = 0;
        return 0;
    }

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
