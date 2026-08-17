// gkeys.c — the keystroke echo, reborn in ring 3.
//
// The second half of the migration acceptance test (gui_demo_keys.c was
// the kernel-thread original): same window, same warm-paper look, same
// behavior — keys land only when focused, mouse clicks report
// content-local coordinates. It pulls exactly what LIBDRAW.md predicted
// its port would pull: draw_text (the EMBEDDED font's first real
// customer) and the event queue.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define KEYS_BG        0xfff4f2ea   // warm paper white
#define KEYS_INK       OS64_GUI_COLOR_BLACK
#define KEYS_ACCENT    0xff2a62b8
#define LINE_MAX       36           // chars that fit the 330px-wide content
#define FRAME_MS_POLL  16           // ~60Hz event-poll cadence

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int64_t win = os64_gui_window_create("keys", 620, 100, 330, 180,
                                         OS64_GUI_WINDOW_START_UNFOCUSED);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("gkeys: no GUI on this boot\n");
        return 0;
    }
    if (win <= 0) {
        os64_hprintf(OS64_STDERR, "gkeys: window create failed (%ld)\n",
                     (long)win);
        return 1;
    }

    os64_draw_ctx_t ctx;
    if (os64_draw_ctx_init(&ctx, win) != 0) {
        os64_hprintf(OS64_STDERR, "gkeys: no surface\n");
        return 1;
    }
    os64_gui_surface_t *s = &ctx.surf;

    os64_gui_rect_t all = {0, 0, (int32_t)s->width, (int32_t)s->height};
    os64_draw_fill_rect(s, all, KEYS_BG);
    const char prompt[] = "click me, then type:";
    os64_draw_text(s, 10, 10, prompt, sizeof(prompt) - 1, KEYS_ACCENT, KEYS_BG);
    os64_draw_publish(&ctx, NULL);

    char line[LINE_MAX + 1] = {0};
    size_t line_len = 0;
    char status[LINE_MAX + 1] = {0};

    os64_frame_clock_t clock;
    os64_frame_clock_init(&clock);

    for (;;) {
        bool dirty = false;

        os64_gui_event_t ev;
        while (os64_gui_event_poll(win, &ev) == 1) {
            switch (ev.type) {
            case OS64_GUI_EVENT_KEY_DOWN:
                if (ev.key.ascii == '\b') {
                    if (line_len)
                        line[--line_len] = '\0';
                    dirty = true;
                } else if (ev.key.ascii == '\n') {
                    line_len = 0;
                    line[0] = '\0';
                    dirty = true;
                } else if (ev.key.ascii >= ' ' && ev.key.ascii < 127) {
                    if (line_len < LINE_MAX) {
                        line[line_len++] = ev.key.ascii;
                        line[line_len] = '\0';
                    }
                    dirty = true;
                }
                break;
            case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
                // Content-local coordinates, straight from the router.
                os64_snprintf(status, sizeof(status), "click at (%d,%d)",
                              ev.mouse.x, ev.mouse.y);
                dirty = true;
                break;
            default:
                break;
            }
        }

        if (dirty) {
            // Repaint the two text lines (typed input + last click).
            os64_gui_rect_t text_area = {0, 40, (int32_t)s->width, 48};
            os64_draw_fill_rect(s, text_area, KEYS_BG);
            os64_draw_text(s, 10, 44, line, line_len, KEYS_INK, KEYS_BG);
            os64_draw_text(s, 10, 68, status, os64_strlen(status),
                           KEYS_ACCENT, KEYS_BG);
            os64_draw_publish(&ctx, &text_area);
        }

        // ~60Hz poll cadence is plenty for an echo box; the clock keeps it
        // honest whatever the scheduler's tick turns out to be.
        os64_frame_wait(&clock, FRAME_MS_POLL);
    }
}
