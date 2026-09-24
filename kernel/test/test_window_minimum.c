#include "gui/window.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"

// Pure geometry on a private window: no GUI startup, global list, or lock is
// needed. Exercise the clamp shared by the drag outline and resize commit.
bool test_window_minimum_clamp(void)
{
    window_t *w = kmalloc(sizeof(*w));
    if (!w) return false;
    memset(w, 0, sizeof(*w));
    w->canvas_cap_w = 1024;
    w->canvas_cap_h = 768;
    bool ok = true;
    const uint32_t flags[] = {0, GUI_WINDOW_NO_DECORATIONS, GUI_WINDOW_DESKTOP};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        w->flags = flags[i];
        int32_t bx = 2 * wm_border_width(w->flags);
        int32_t by = wm_chrome_top(w->flags) + wm_border_width(w->flags);
        w->min_content_w = GUI_WINDOW_MIN_CONTENT_W;
        w->min_content_h = GUI_WINDOW_MIN_CONTENT_H;
        rect_t r = wm_clamp_frame(w, (rect_t){12, 24, 8, 8});
        ok &= r.w == 64 + bx && r.h == 32 + by;
        w->min_content_w = 958;
        w->min_content_h = 696;
        r = wm_clamp_frame(w, (rect_t){12, 24, 800, 600});
        ok &= r.x == 12 && r.y == 24 && r.w == 958 + bx && r.h == 696 + by;
        r = wm_clamp_frame(w, (rect_t){12, 24, 1000 + bx, 720 + by});
        ok &= r.w == 1000 + bx && r.h == 720 + by;
        // Rejection must leave both limits intact, including the valid axis.
        ok &= !wm_set_min_size(w, 640, 769);
        ok &= !wm_set_min_size(w, 1025, 480);
        ok &= w->min_content_w == 958 && w->min_content_h == 696;
        r = wm_clamp_frame(w, (rect_t){12, 24, 4096, 4096});
        ok &= r.w == 1024 + bx && r.h == 768 + by;
    }
    kfree(w);
    return ok;
}
