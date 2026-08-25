// gview(1) — look at a picture.
//
// The first program os64 has ever had that opens an image file and shows it
// to you. It exists as much to PROVE libimage and os64_draw_blit against
// real files as to be useful: the desktop shell and the launcher will both
// stand on this pair, and neither is a good place to discover that a BMP
// decodes upside down.
//
// It is deliberately a direct libdraw app rather than a libui one. There is
// no furniture here — no labels, no panels, nothing to lay out — just a
// picture centered on a background, so the retained-mode widget machinery
// would be ceremony around a single blit.
//
// The loop BLOCKS (os64_gui_event_wait). A picture does not animate, so the
// viewer should cost exactly nothing while you look at it.

#include "os64/os64.h"
#include "os64/io.h"
#include "os64/image.h"
#include "os64/draw.h"
#include "os64/gui.h"

// The mat around a picture that does not fill its window. Not the theme's
// business: this is a viewer's own furniture, and a neutral gray is what
// every photo viewer since the 1990s has used to stop the surround from
// tinting your judgment of the image.
#define GVIEW_BACKGROUND 0xff303030u

// Leave the window somewhat inside the screen when the image is enormous —
// a window born bigger than the glass cannot be dragged back into view.
#define GVIEW_SCREEN_MARGIN 64u

static void repaint(os64_draw_ctx_t *ctx, const os64_image_t *img)
{
    // Geometry first: the canvas may have grown or shrunk since last time,
    // and every primitive clips against ctx->surf — repainting with stale
    // numbers leaves the new strip untouched, which looks exactly like a
    // damage bug and is not one (draw.h says so at ctx_refresh).
    os64_draw_ctx_refresh(ctx);

    os64_gui_rect_t all = {0, 0, (int32_t)ctx->surf.width,
                                 (int32_t)ctx->surf.height};
    os64_draw_fill_rect(&ctx->surf, all, GVIEW_BACKGROUND);

    // Center, and let the blit clip. An image larger than the window ends up
    // with a negative origin, which crops it around its middle rather than
    // drawing it shifted — that behaviour is the blit's contract, not luck.
    int32_t x = ((int32_t)ctx->surf.width  - (int32_t)img->width)  / 2;
    int32_t y = ((int32_t)ctx->surf.height - (int32_t)img->height) / 2;
    os64_draw_blit(&ctx->surf, x, y, img->pixels,
                   img->width, img->height, img->width);

    os64_draw_publish(ctx, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        os64_hprintf(OS64_STDERR, "usage: gview <file>\n");
        return 1;
    }
    const char *path = argv[1];

    os64_image_t img;
    os64_image_status_t st = os64_image_load(path, 0, &img);
    if (st != OS64_IMAGE_OK) {
        // Name the file AND the reason. "gview: failed" is the error message
        // that sends someone to read this source; this one does not.
        os64_hprintf(OS64_STDERR, "gview: %s: %s\n", path,
                     os64_image_status_name(st));
        return 1;
    }

    // Size the window to the picture, capped to the screen. screen_info
    // failing is not fatal — an unknown screen just means no cap.
    uint32_t sw = 0, sh = 0;
    uint32_t win_w = img.width, win_h = img.height;
    if (os64_gui_screen_info(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        uint32_t max_w = (sw > GVIEW_SCREEN_MARGIN) ? sw - GVIEW_SCREEN_MARGIN : sw;
        uint32_t max_h = (sh > GVIEW_SCREEN_MARGIN) ? sh - GVIEW_SCREEN_MARGIN : sh;
        if (win_w > max_w) win_w = max_w;
        if (win_h > max_h) win_h = max_h;
    }

    int64_t win = os64_gui_window_create(path, 64, 64, win_w, win_h, 0);
    if (win <= 0) {
        os64_hprintf(OS64_STDERR, "gview: no GUI here (window_create %ld)\n",
                     (long)win);
        os64_image_free(&img);
        return 1;
    }

    os64_draw_ctx_t ctx;
    if (os64_draw_ctx_init(&ctx, win) != 0) {
        os64_hprintf(OS64_STDERR, "gview: get_surface failed\n");
        os64_gui_window_destroy(win);
        os64_image_free(&img);
        return 1;
    }

    repaint(&ctx, &img);

    bool running = true;
    while (running) {
        os64_gui_event_t ev;
        int64_t r = os64_gui_event_wait(win, &ev);
        if (r != 1)
            break;   // the window died, or we are being killed

        switch (ev.type) {
        case OS64_GUI_EVENT_WINDOW_RESIZE:
            repaint(&ctx, &img);
            break;
        case OS64_GUI_EVENT_WINDOW_CLOSE:
            running = false;
            break;
        case OS64_GUI_EVENT_KEY_DOWN:
            // MATCH PRINTABLE KEYS BY ASCII, never by scancode — the two
            // keyboard dialects (PS/2 and HID) number them differently, and
            // every check that forgot this has been a bug (GRAPHICS.md).
            if (ev.key.ascii == 'q' || ev.key.ascii == 'Q')
                running = false;
            break;
        default:
            break;
        }
    }

    os64_gui_window_destroy(win);
    os64_image_free(&img);
    return 0;
}
