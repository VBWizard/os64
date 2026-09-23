// psf2probe — put an outline face on the virtual terminals, at a size.
//
//   psf2probe /etc/fonts/DejaVuSansMono.ttf 24
//   psf2probe /etc/fonts/DejaVuSansMono.ttf 24 /home/fonts/dejavu24.psf
//
// An acceptance probe, and the worked example of os64_font_render_psf2: read
// the face, render it, write the bytes. With two operands the bytes go to
// /sys/console/font and every VT changes face; with a third they go to that
// file instead, which makes a .psf that `cp` can load later without FreeType
// in the room. `vtfont` is the program a person is meant to use; this is what
// proves the seam under it.
//
// `echo boot > /sys/console/font` goes back, and `cat /sys/console/font` says
// what the kernel made of the write — the close answers a bare
// OS64_CLOSE_NOT_COMMITTED for a font refused, and that file holds the
// reason, so this program reports what it SENT and points there.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/font_psf2.h"
#include "os64/mem.h"
#include "os64/slurp.h"

#define CONSOLE_FONT_DOOR "/sys/console/font"

static const char *status_name(os64_font_status_t s, bool opened)
{
    if (!opened && (s == OS64_FONT_UNSUPPORTED || s == OS64_FONT_MALFORMED))
        return "not a font the engine can read";
    switch (s) {
    case OS64_FONT_UNSUPPORTED: return "not a fixed-width face, or no whole-pixel cell at this size";
    case OS64_FONT_MISSING:     return "the face cannot draw all of printable ASCII";
    case OS64_FONT_MALFORMED:   return "a damaged font";
    case OS64_FONT_LIMIT:      return "outside what the console accepts";
    case OS64_FONT_NO_MEMORY:   return "out of memory";
    default:                    return "refused";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        os64_hprintf(2, "usage: psf2probe <face.ttf|.otf> <pixels> [out.psf]\n");
        return 2;
    }
    uint32_t pixels = 0;
    for (const char *p = argv[2]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || pixels > 1000) {
            os64_hprintf(2, "psf2probe: '%s' is not a size in pixels\n", argv[2]);
            return 2;
        }
        pixels = pixels * 10 + (uint32_t)(*p - '0');
    }

    uint8_t *face = NULL;
    size_t face_len = 0;
    os64_slurp_status_t read = os64_slurp(argv[1], OS64_FONT_FILE_MAX, &face, &face_len);
    if (read != OS64_SLURP_OK) {
        os64_hprintf(2, "psf2probe: %s: %s\n", argv[1], os64_slurp_status_name(read));
        return 1;
    }

    uint8_t *image = os64_malloc(OS64_FONT_PSF2_IMAGE_MAX);
    if (image == NULL) {
        os64_hprintf(2, "psf2probe: out of memory\n");
        return 1;
    }
    os64_font_psf2_options_t options = { .pixel_height = pixels };
    os64_font_psf2_info_t info;
    size_t length = 0;
    os64_font_status_t status = os64_font_render_psf2(&options, face, face_len,
        image, OS64_FONT_PSF2_IMAGE_MAX, &length, &info);
    os64_free(face);
    if (status != OS64_FONT_OK) {
        os64_hprintf(2, "psf2probe: %s at %u: %s", argv[1], pixels, status_name(status, info.opened));
        if (info.offender != 0 && status == OS64_FONT_MISSING)
            os64_hprintf(2, " (no usable '%c' at %u px)", (int)info.offender, pixels);
        else if (info.offender != 0)
            os64_hprintf(2, " ('%c' is not the cell's width)", (int)info.offender);
        if (status == OS64_FONT_LIMIT)
            os64_hprintf(2, " (a %ux%u cell; %ux%u to %ux%u fit)", info.cell_w, info.cell_h,
                         OS64_FONT_PSF2_CELL_W_MIN, OS64_FONT_PSF2_CELL_H_MIN,
                         OS64_FONT_PSF2_CELL_W_MAX, OS64_FONT_PSF2_CELL_H_MAX);
        os64_hprintf(2, "\n");
        return 1;
    }

    const char *where = argc == 4 ? argv[3] : CONSOLE_FONT_DOOR;
    int64_t h = os64_open(where, "w");
    if (h < 0) {
        os64_hprintf(2, "psf2probe: cannot open %s\n", where);
        return 1;
    }
    size_t sent = 0;
    while (sent < length) {
        int64_t n = os64_write((int32_t)h, image + sent, length - sent);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    os64_close((int32_t)h);
    os64_free(image);
    if (sent != length) {
        os64_hprintf(2, "psf2probe: %s took %u of %u bytes\n", where, (unsigned)sent, (unsigned)length);
        return 1;
    }

    // After the write: the swap is kworker's and may land either side of
    // this line, and the reflow carries the line across whichever way.
    os64_printf("psf2probe: %ux%u cell, %u glyphs (%u left out), %u bytes -> %s\n",
                info.cell_w, info.cell_h, info.glyphs, info.missing, (unsigned)length, where);
    if (info.clipped != 0)
        os64_printf("psf2probe: warning: %u glyphs lose ink to the cell's edge at this size\n",
                    info.clipped);
    return 0;
}
