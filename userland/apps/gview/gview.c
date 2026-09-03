// gview(1) — look at a picture.
//
// The first program os64 has ever had that opens an image file and shows it
// to you. It exists as much to PROVE the image codecs and os64_draw_blit
// against real files as to be useful: the desktop shell and the launcher will
// both stand on this pair, and neither is a good place to discover that a BMP
// decodes upside down or a PNG lost its alpha.
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
#include "os64/slurp.h"
#include "png/png.h"

// The mat around a picture that does not fill its window. Not the theme's
// business: this is a viewer's own furniture, and a neutral gray is what
// every photo viewer since the 1990s has used to stop the surround from
// tinting your judgment of the image.
#define GVIEW_BACKGROUND 0xff303030u

// Leave the window somewhat inside the screen when the image is enormous —
// a window born bigger than the glass cannot be dragged back into view.
#define GVIEW_SCREEN_MARGIN 64u

static bool is_png(const uint8_t *data, size_t length)
{
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (length < sizeof(signature))
        return false;
    for (size_t i = 0; i < sizeof(signature); i++)
        if (data[i] != signature[i])
            return false;
    return true;
}

static bool load_picture(const char *path, os64_image_t *image,
                         const char **reason)
{
    image->width = 0;
    image->height = 0;
    image->pixels = NULL;

    uint8_t *data = NULL;
    size_t length = 0;
    os64_slurp_status_t read_status = os64_slurp(
        path, OS64_IMAGE_CAP_DEFAULT, &data, &length);
    if (read_status != OS64_SLURP_OK) {
        *reason = os64_slurp_status_name(read_status);
        return false;
    }

    bool loaded = false;
    if (is_png(data, length)) {
        // This small fork is the transitional seam recorded in LIBIMAGE.md:
        // libpng is independent now, while BMP/PPM still live in libos64.
        // The eventual libimage façade will own this magic dispatch.
        os64_png_image_t png;
        os64_png_status_t status = os64_png_decode(data, length, 0, &png);
        *reason = os64_png_status_name(status);
        if (status == OS64_PNG_OK) {
            image->width = png.width;
            image->height = png.height;
            image->pixels = png.pixels;
            loaded = true;
        }
    } else {
        os64_image_status_t status = os64_image_decode(data, length, image);
        *reason = os64_image_status_name(status);
        loaded = status == OS64_IMAGE_OK;
    }
    os64_free(data);
    return loaded;
}

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
    const char *reason;
    if (!load_picture(path, &img, &reason)) {
        // Name the file AND the reason. "gview: failed" is the error message
        // that sends someone to read this source; this one does not.
        os64_hprintf(OS64_STDERR, "gview: %s: %s\n", path, reason);
        return 1;
    }

    // Size the window so the CANVAS is the picture — which is not the same as
    // asking for a frame the size of the picture (Codex #30 rd3). create()
    // takes the frame, the WM keeps its border and titlebar out of the
    // middle, and asking for exactly img.width x img.height quietly cost two
    // columns and twenty-one rows of every image gview ever opened. Anything
    // under 10x29 was refused outright as a degenerate window.
    // And clamped UP to the smallest canvas a window may have (Codex #30
    // rd4): create refuses content under OS64_GUI_MIN_CONTENT rather than
    // rounding it, so a legal 4x4 picture decoded fine and then could not be
    // shown at all. The image is still centered in whatever canvas we get, so
    // a clamped window simply has a mat around a very small picture — which
    // is what any viewer does with one.
    uint32_t content_w = img.width  < OS64_GUI_MIN_CONTENT ? OS64_GUI_MIN_CONTENT : img.width;
    uint32_t content_h = img.height < OS64_GUI_MIN_CONTENT ? OS64_GUI_MIN_CONTENT : img.height;

    uint32_t win_w, win_h;
    os64_gui_frame_for_content(content_w, content_h, 0, &win_w, &win_h);

    // Capped to the screen. screen_info failing is not fatal — an unknown
    // screen just means no cap. The cap applies to the FRAME, since that is
    // what has to fit on the glass.
    uint32_t sw = 0, sh = 0;
    if (os64_gui_screen_info(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        uint32_t max_w = (sw > GVIEW_SCREEN_MARGIN) ? sw - GVIEW_SCREEN_MARGIN : sw;
        uint32_t max_h = (sh > GVIEW_SCREEN_MARGIN) ? sh - GVIEW_SCREEN_MARGIN : sh;
        if (win_w > max_w) win_w = max_w;
        if (win_h > max_h) win_h = max_h;
    }
    // AND to the largest window the WM will create (Codex #30 rd6). The
    // screen cap alone is not enough on a panel wider than that limit: a
    // 5000-pixel image on an 8K display passed the screen cap and was then
    // refused at create, and gview reported a window failure for a file it
    // had decoded perfectly. The picture is still centered and cropped by
    // the blit, exactly as an image larger than the screen is.
    if (win_w > OS64_GUI_WINDOW_DIM_MAX) win_w = OS64_GUI_WINDOW_DIM_MAX;
    if (win_h > OS64_GUI_WINDOW_DIM_MAX) win_h = OS64_GUI_WINDOW_DIM_MAX;

    // THE TITLE IS THE BASENAME, BOUNDED (Codex #30 rd4). A title longer than
    // the window's own field is REFUSED, not truncated — deliberately, so a
    // silently shortened name cannot "succeed" — which meant gview decoded
    // any file at a path of 32 bytes or more and then failed to open a window
    // for it. The whole path was never the right title anyway: a titlebar
    // has room for a name, and the name is the last component.
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++)
        if (*p == '/')
            base = p + 1;
    char title[OS64_GUI_TITLE_MAX];
    os64_strcopy(title, sizeof(title), base);   // strlcpy semantics: always terminated

    int64_t win = os64_gui_window_create(title, 64, 64, win_w, win_h, 0);
    if (win <= 0) {
        // "No GUI here" is true for exactly ONE of these answers, and saying
        // it for all of them sent the last reader looking for a missing
        // compositor when the real complaint was about arguments.
        if (win == OS64_GUI_ERR_NOT_RUNNING)
            os64_hprintf(OS64_STDERR, "gview: no GUI on this boot\n");
        else
            os64_hprintf(OS64_STDERR, "gview: could not create a window for %s (%ld)\n",
                         path, (long)win);
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
