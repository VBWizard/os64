#include "gui/window.h"
#include "gui/surface.h"
#include "gui/gui_internal.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "memory/memcpy.h"
#include "printd.h"
#include "CONFIG.h"

// A window claims its whole frame in the claim walk (OCCLUSION.md), which is
// true only while painting it writes every pixel of frame ∩ view: a pixel it
// skips shows last frame's backbuffer. So each kind of window is painted
// over a sentinel colour, whole and in pieces, inside the surface and hanging
// off its edges, and no sentinel may survive inside the frame. A titlebar is
// painted with whatever decoration view is installed, LATE so a desktop has
// had time to install its saved one; with none it is the plain chrome, and
// the PASS line says which. The decoration's own paint is proved against
// prepared views on the host (tools/test_decoration_host.sh, opacity).

#define OPACITY_W 240
#define OPACITY_H 180
#define OPACITY_SENTINEL 0x00FE01FDu

static bool sentinel_survives(const surface_t *s, rect_t frame, int32_t *at_x, int32_t *at_y)
{
    rect_t inside;
    if (!rect_intersect(frame, (rect_t){0, 0, (int32_t)s->width, (int32_t)s->height}, &inside))
        return false;
    for (int32_t y = inside.y; y < inside.y + inside.h; y++)
        for (int32_t x = inside.x; x < inside.x + inside.w; x++)
            if (s->pixels[(size_t)y * s->pitch_px + x] == OPACITY_SENTINEL) {
                *at_x = x;
                *at_y = y;
                return true;
            }
    return false;
}

bool test_window_opacity(void)
{
    static const uint32_t kinds[] = {
        0,
        GUI_WINDOW_HAS_SETTINGS,
        GUI_WINDOW_PINNED,
        GUI_WINDOW_PINNED | GUI_WINDOW_HAS_SETTINGS,
        GUI_WINDOW_MAXIMIZED,
        GUI_WINDOW_TITLE_UTF8,
        GUI_WINDOW_NO_DECORATIONS,
        GUI_WINDOW_DESKTOP,
    };
    // Inside, hanging off the top left, and off the bottom right.
    static const rect_t places[] = {{30, 20, 170, 130}, {-40, -25, 170, 130}, {120, 90, 170, 130}};
    // The whole surface, then four pieces of it split at odd places.
    static const rect_t views[] = {{0, 0, OPACITY_W, OPACITY_H},
                                   {0, 0, 97, 61}, {97, 0, OPACITY_W - 97, 61},
                                   {0, 61, 97, OPACITY_H - 61}, {97, 61, OPACITY_W - 97, OPACITY_H - 61}};
    surface_t dst;
    window_t *w = kmalloc(sizeof(*w));
    if (!w || surface_init(&dst, OPACITY_W, OPACITY_H) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_window_opacity - no memory\n");
        kfree(w);
        return false;
    }
    bool ok = true;
    bool decorated = false;
    uint64_t flags = gui_lock(GUI_LOCK_OTHER);
    for (size_t k = 0; ok && k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (size_t p = 0; ok && p < sizeof(places) / sizeof(places[0]); p++) {
            memset(w, 0, sizeof(*w));
            w->flags = kinds[k];
            static const char title[] = "opacity: a title long enough to be cut short by the bar";
            memcpy(w->title, title, sizeof(title) < GUI_WINDOW_TITLE_MAX ? sizeof(title) : GUI_WINDOW_TITLE_MAX - 1);
            rect_t frame = places[p];
            int32_t least = (int32_t)wm_decoration_min_width(w->flags);
            if (frame.w < least)
                frame.w = least;
            w->frame = frame;
            // Sized as wm_create sizes it.
            int32_t cw = frame.w - 2 * wm_border_width(w->flags);
            int32_t ch = frame.h - wm_chrome_top(w->flags) - wm_border_width(w->flags);
            if (cw < 1 || ch < 1 || surface_init(&w->content, (uint32_t)cw, (uint32_t)ch) != 0) {
                printd(DEBUG_TESTS, "\tFAIL: test_window_opacity - kind 0x%x has no content\n", kinds[k]);
                ok = false;
                break;
            }
            surface_fill_rect(&w->content, (rect_t){0, 0, cw, ch}, GUI_COLOR_LIGHT_GRAY);
            // Whole, then in pieces, each over a fresh sentinel.
            for (int pieces = 0; ok && pieces < 2; pieces++) {
                surface_fill_rect(&dst, (rect_t){0, 0, OPACITY_W, OPACITY_H}, OPACITY_SENTINEL);
                size_t first = pieces ? 1 : 0, last = pieces ? 5 : 1;
                for (size_t v = first; v < last; v++)
                    decorated |= wm_paint_window(&dst, w, views[v]);
                int32_t x, y;
                if (sentinel_survives(&dst, frame, &x, &y)) {
                    printd(DEBUG_TESTS, "\tFAIL: test_window_opacity - kind 0x%x at (%d,%d) %dx%d, %s: "
                           "(%d,%d) unpainted\n", kinds[k], frame.x, frame.y, frame.w, frame.h,
                           pieces ? "in pieces" : "whole", x, y);
                    ok = false;
                }
            }
            surface_free(&w->content);
        }
    }
    gui_unlock(flags);
    surface_free(&dst);
    kfree(w);
    if (ok)
        printd(DEBUG_TESTS, "\tPASS: test_window_opacity (%s titlebar)\n",
               decorated ? "the installed decoration's" : "the plain");
    return ok;
}
