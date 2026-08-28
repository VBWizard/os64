// gbounce.c — the bouncing ball, reborn in ring 3.
//
// The kernel-thread /gbounce (gui/demo/gui_demo_bounce.c) was GRAPHICS.md's
// placeholder client; THIS is the acceptance test that retires it: same
// window, same colors, same ball, drawn by an ordinary ELF through libdraw
// over syscalls 16-21. What changes is everything invisible — it has a
// heap, it crosses the syscall boundary ~100x/sec (so Ctrl+C and
// `kill` land within microseconds), and when it dies its window dies with
// it, swept like any other task's.
//
// AND ONE VISIBLE FIX: velocity is PIXELS PER SECOND, not pixels per
// frame. The kernel demo moved 7px per wakeup — correct on QEMU, which
// never delivered the nominal frame rate, and comically fast on the P5,
// which delivered every frame (2026-08-17, "the ball is SCREAMING").
// That is the 486→Pentium bug of 1994, the one turbo buttons existed to
// paper over. The frame loop hands us real elapsed time; the ball moves
// through TIME now, and looks identical on every machine that can keep up
// and every machine that can't.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define BALL_R      12
#define BALL_BG     0xff20242c            // window content background
#define BALL_COLOR  OS64_GUI_COLOR_YELLOW
#define VX_PPS      700                   // px/sec ≈ the old 7px @ nominal 100fps
#define VY_PPS      500
#define FRAME_MS    16                    // budget; the clock reports the truth

static void draw_ball(os64_gui_surface_t *s, int32_t cx, int32_t cy,
                      uint32_t color)
{
    // Filled circle as a stack of hlines; integer math only. (The kernel
    // demo's exact circle, so the acceptance stays pixel-identical.)
    for (int32_t dy = -BALL_R; dy <= BALL_R; dy++) {
        int32_t half = 0;
        while ((half + 1) * (half + 1) + dy * dy <= BALL_R * BALL_R)
            half++;
        os64_draw_hline(s, cx - half, cy + dy, 2 * half + 1, color);
    }
}

static inline os64_gui_rect_t ball_rect(int32_t cx, int32_t cy)
{
    return (os64_gui_rect_t){cx - BALL_R, cy - BALL_R,
                             2 * BALL_R + 1, 2 * BALL_R + 1};
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int64_t win = os64_gui_window_create("bounce", 660, 430, 300, 220,
                                         OS64_GUI_WINDOW_START_UNFOCUSED);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("gbounce: no GUI on this boot\n");
        return 0;
    }
    if (win <= 0) {
        os64_hprintf(OS64_STDERR, "gbounce: window create failed (%ld)\n",
                     (long)win);
        return 1;
    }

    os64_draw_ctx_t ctx;
    if (os64_draw_ctx_init(&ctx, win) != 0) {
        os64_hprintf(OS64_STDERR, "gbounce: no surface\n");
        return 1;
    }
    os64_gui_surface_t *s = &ctx.surf;

    os64_draw_fill_rect(s, (os64_gui_rect_t){0, 0, (int32_t)s->width,
                        (int32_t)s->height}, BALL_BG);
    os64_draw_publish(&ctx, NULL);

    // Position in MILLI-pixels so sub-pixel motion accumulates instead of
    // rounding away — at 700 px/s and a 10ms tick that's 7000 mpx per
    // frame, but at any dt the arithmetic stays exact.
    int64_t x_mpx = (BALL_R + 5) * 1000, y_mpx = (BALL_R + 5) * 1000;
    int64_t vx = VX_PPS, vy = VY_PPS;   // px/sec, sign carries direction
    int32_t min_x = BALL_R, max_x = (int32_t)s->width - 1 - BALL_R;
    int32_t min_y = BALL_R, max_y = (int32_t)s->height - 1 - BALL_R;

    os64_frame_clock_t clock;
    os64_frame_clock_init(&clock);
    os64_frame_clock_bind(&clock, win);   // sleep while nobody can see the ball

    for (;;) {
        uint64_t dt = os64_frame_wait(&clock, FRAME_MS);

        int32_t old_x = (int32_t)(x_mpx / 1000);
        int32_t old_y = (int32_t)(y_mpx / 1000);

        x_mpx += vx * (int64_t)dt;   // (px/s · ms) = milli-pixels, exactly
        y_mpx += vy * (int64_t)dt;
        if (x_mpx < min_x * 1000) { x_mpx = min_x * 1000; vx = -vx; }
        if (x_mpx > max_x * 1000) { x_mpx = max_x * 1000; vx = -vx; }
        if (y_mpx < min_y * 1000) { y_mpx = min_y * 1000; vy = -vy; }
        if (y_mpx > max_y * 1000) { y_mpx = max_y * 1000; vy = -vy; }

        int32_t x = (int32_t)(x_mpx / 1000);
        int32_t y = (int32_t)(y_mpx / 1000);

        // Erase, redraw, publish exactly what changed: old spot ∪ new spot.
        os64_gui_rect_t old_spot = ball_rect(old_x, old_y);
        os64_draw_fill_rect(s, old_spot, BALL_BG);
        draw_ball(s, x, y, BALL_COLOR);
        os64_gui_rect_t damage = os64_rect_union(old_spot, ball_rect(x, y));
        os64_draw_publish(&ctx, &damage);

        // Stay polite: drain the queue even though a ball ignores input —
        // a full queue drops events, and this is what real apps do.
        os64_gui_event_t ev;
        while (os64_gui_event_poll(win, &ev) == 1)
        {
            if (ev.type != OS64_GUI_EVENT_WINDOW_RESIZE)
                continue;

            // The walls moved. This is the smallest complete example of what
            // a resize costs an app that owns its own frame loop: re-read the
            // geometry, re-derive whatever was computed FROM the geometry,
            // and repaint the whole thing once.
            os64_draw_ctx_refresh(&ctx);
            min_x = BALL_R;
            max_x = (int32_t)s->width - 1 - BALL_R;
            min_y = BALL_R;
            max_y = (int32_t)s->height - 1 - BALL_R;

            // A shrink can leave the ball outside the new room. Put it back
            // rather than letting the bounce test do it, which would send it
            // travelling the wrong way for one frame from a position that was
            // never legal.
            if (x_mpx < (int64_t)min_x * 1000) x_mpx = (int64_t)min_x * 1000;
            if (x_mpx > (int64_t)max_x * 1000) x_mpx = (int64_t)max_x * 1000;
            if (y_mpx < (int64_t)min_y * 1000) y_mpx = (int64_t)min_y * 1000;
            if (y_mpx > (int64_t)max_y * 1000) y_mpx = (int64_t)max_y * 1000;

            // Repaint everything: the strip a grow exposed is background the
            // window system filled, not ours, and the next frame's damage
            // rect only covers where the ball is.
            os64_draw_fill_rect(s, (os64_gui_rect_t){0, 0, (int32_t)s->width,
                                (int32_t)s->height}, BALL_BG);
            draw_ball(s, (int32_t)(x_mpx / 1000), (int32_t)(y_mpx / 1000),
                      BALL_COLOR);
            os64_draw_publish(&ctx, NULL);
        }
    }
}
