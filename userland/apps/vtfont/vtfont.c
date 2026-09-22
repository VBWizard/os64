// vtfont — the face the virtual terminals draw with.
//
//   vtfont                          what is loaded: cell, grid, the last verdict
//   vtfont boot                     the face the machine booted with
//   vtfont terminus-24.psf          a bitmap face, as it is
//   vtfont DejaVuSansMono.ttf 24    an outline face, rendered at 24 pixels
//   vtfont DejaVuSansMono.ttf 100x40
//                                   an outline face at the LARGEST size that
//                                   still gives this screen 100 columns and
//                                   40 rows
//   vtfont --startup                the same, with FACE and SIZE read from
//                                   console.conf on the config ladder; the
//                                   kernel runs this once at boot when that
//                                   file exists (CONSOLE_FONTS.md § Persistence)
//
// The kernel takes a PSF2 bitmap and nothing smarter (CONSOLE_FONTS.md), so
// there are two kinds of file and one door: a .psf goes through as it is,
// and an outline face is rendered here, by os64_font_render_psf2, into the
// same bytes. Which kind a file is comes from its first bytes, not its name.
// A name with no `/` in it is looked for in /etc/fonts, with .ttf, .otf and
// .psf tried in turn, so `vtfont DejaVuSansMono 100x40` is enough.
//
// "I WANT 100 BY 40" is the size a person actually has in mind: not a pixel
// count but how much fits on the glass. The cell an outline face makes grows
// with the pixel size, so the largest size whose grid still holds the ask is
// found by bisection over the sizes the kernel accepts — a handful of
// renders, and the answer is printed with the size it settled on, so the
// number can be typed next time. A bitmap is one size forever; asked for a
// grid, it answers with the grid it gives.
//
// WHAT TOOK IS THE KERNEL'S TO SAY. A write to /sys/console/font is judged
// at close and installed by kworker a moment later, and a close that fails
// is not yet heard in ring 3 (DEBTS.md) — so after the write this waits for
// the swap and prints the door's own `last:` line, and its exit status is
// that verdict's, never the write's.

#include "os64/os64.h"
#include "os64/font_psf2.h"
#include "os64/slurp.h"
#include "os64/conf.h"

#define DOOR "/sys/console/font"
#define FONT_DIR "/etc/fonts"
#define SWAP_WAIT_MS 3000

static bool starts_with(const char *line, size_t n, const char *prefix)
{
    size_t p = os64_strlen(prefix);
    if (n < p) return false;
    for (size_t i = 0; i < p; i++)
        if (line[i] != prefix[i]) return false;
    return true;
}

// "AxB" out of the front of a line, both nonzero.
static bool parse_pair(const char *s, size_t n, uint64_t *a, uint64_t *b)
{
    char buf[40];
    size_t c = 0;
    while (c < n && c < sizeof(buf) - 1 && s[c] != '\n') { buf[c] = s[c]; c++; }
    buf[c] = '\0';
    char *x = buf;
    while (*x && *x != 'x') x++;
    if (*x != 'x') return false;
    *x = '\0';
    return os64_parse_u64(buf, a) && os64_parse_u64(x + 1, b) && *a && *b;
}

// Everything the door says, parsed.
typedef struct {
    uint32_t cell_w, cell_h, screen_w, screen_h, grid_w, grid_h;
    bool pending;
    char last[200];
} door_t;

static bool read_door(door_t *d)
{
    uint8_t *text = NULL;
    size_t len = 0;
    if (os64_slurp(DOOR, 4096, &text, &len) != OS64_SLURP_OK)
        return false;
    *d = (door_t){0};
    const char *s = (const char *)text;
    size_t at = 0;
    while (at < len) {
        size_t eol = at;
        while (eol < len && s[eol] != '\n') eol++;
        const char *line = s + at;
        size_t n = eol - at;
        uint64_t a = 0, b = 0;
        if (starts_with(line, n, "cell: ") && parse_pair(line + 6, n - 6, &a, &b)) {
            d->cell_w = (uint32_t)a; d->cell_h = (uint32_t)b;
        } else if (starts_with(line, n, "screen: ") && parse_pair(line + 8, n - 8, &a, &b)) {
            d->screen_w = (uint32_t)a; d->screen_h = (uint32_t)b;
        } else if (starts_with(line, n, "grid: ") && parse_pair(line + 6, n - 6, &a, &b)) {
            d->grid_w = (uint32_t)a; d->grid_h = (uint32_t)b;
        } else if (starts_with(line, n, "pending: ")) {
            d->pending = line[9] == 'y';
        } else if (starts_with(line, n, "last: ")) {
            size_t c = n - 6;
            if (c >= sizeof(d->last)) c = sizeof(d->last) - 1;
            os64_memcpy(d->last, line + 6, c);
            d->last[c] = '\0';
        }
        at = eol + 1;
    }
    os64_free(text);
    return true;
}

static int show(void)
{
    uint8_t *text = NULL;
    size_t len = 0;
    if (os64_slurp(DOOR, 4096, &text, &len) != OS64_SLURP_OK) {
        os64_hprintf(OS64_STDERR, "vtfont: cannot read " DOOR "\n");
        return 1;
    }
    os64_write(OS64_STDOUT, text, len);
    os64_free(text);
    return 0;
}

// Write the bytes, wait for the swap, report the kernel's verdict. The exit
// status is the verdict's: "installed" is success and anything else is not,
// because the write itself cannot be trusted to say.
//
// `pending: no` alone does not mean the swap is over: the kernel clears it
// when kworker TAKES the offer, before the reflows and the repaint, and
// writes the verdict after them. In between the door reads `pending: no`
// with `last: accepted: ... waiting for the swap` — so the wait goes on
// while the verdict is still the offer's own, and stops on the swap's.
static int offer(const uint8_t *bytes, size_t len, const char *what)
{
    int64_t h = os64_open(DOOR, "w");
    if (h < 0) {
        os64_hprintf(OS64_STDERR, "vtfont: cannot open " DOOR "\n");
        return 1;
    }
    size_t sent = 0;
    while (sent < len) {
        int64_t n = os64_write((int32_t)h, bytes + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    os64_close((int32_t)h);
    if (sent != len) {
        os64_hprintf(OS64_STDERR, "vtfont: " DOOR " took %u of %u bytes\n", (unsigned)sent, (unsigned)len);
        return 1;
    }

    door_t d;
    for (uint32_t waited = 0; ; waited += 50) {
        if (!read_door(&d)) {
            os64_hprintf(OS64_STDERR, "vtfont: cannot read " DOOR " back\n");
            return 1;
        }
        bool waiting = d.pending || starts_with(d.last, os64_strlen(d.last), "accepted");
        if (!waiting || waited >= SWAP_WAIT_MS) break;
        os64_sleep(50);
    }
    bool installed = starts_with(d.last, os64_strlen(d.last), "installed");
    if (!installed && starts_with(d.last, os64_strlen(d.last), "accepted"))
        os64_printf("vtfont: %s: not installed within %u ms (%s)\n", what, SWAP_WAIT_MS, d.last);
    else
        os64_printf("vtfont: %s: %s\n", what, d.last);
    if (installed)
        os64_printf("vtfont: %ux%u cell, %ux%u on this screen\n", d.cell_w, d.cell_h, d.grid_w, d.grid_h);
    return installed ? 0 : 1;
}

// ── Finding the face ────────────────────────────────────────────────────────

static bool exists(const char *path)
{
    os64_dirent_t e;
    return os64_stat(path, &e) >= 0 && !(e.flags & OS64_DE_DIR);
}

// A bare name is looked for on the shelf; a path is a path.
static const char *resolve(const char *name, char *buf, size_t cap)
{
    for (const char *p = name; *p; p++)
        if (*p == '/')
            return name;
    static const char *const kTries[] = { "", ".ttf", ".otf", ".psf" };
    for (size_t i = 0; i < sizeof(kTries) / sizeof(kTries[0]); i++) {
        os64_snprintf(buf, cap, FONT_DIR "/%s%s", name, kTries[i]);
        if (exists(buf))
            return buf;
    }
    return exists(name) ? name : NULL;
}

static bool has_psf2_magic(const uint8_t *b, size_t n)
{
    return n >= 4 && b[0] == 0x72 && b[1] == 0xB5 && b[2] == 0x4A && b[3] == 0x86;
}
// The magic AND a whole header: the grid branch reads the cell out of it.
static bool is_psf2(const uint8_t *b, size_t n)
{
    return n >= 32 && has_psf2_magic(b, n);
}
static bool is_psf1(const uint8_t *b, size_t n)
{
    return n >= 2 && b[0] == 0x36 && b[1] == 0x04;
}

// ── Rendering an outline face ───────────────────────────────────────────────

static const char *refusal(os64_font_status_t s, const os64_font_psf2_info_t *info, char *buf, size_t cap)
{
    if (!info->opened && (s == OS64_FONT_UNSUPPORTED || s == OS64_FONT_MALFORMED))
        return "not a font the engine can read";
    switch (s) {
    case OS64_FONT_UNSUPPORTED:
        if (info->offender)
            os64_snprintf(buf, cap, "'%c' is not the width of the cell", (int)info->offender);
        else
            os64_snprintf(buf, cap, "not a fixed-width face, or no whole-pixel cell at this size");
        return buf;
    case OS64_FONT_MISSING:
        os64_snprintf(buf, cap, "no usable '%c' at this size", (int)info->offender);
        return buf;
    case OS64_FONT_LIMIT:
        os64_snprintf(buf, cap, "a %ux%u cell is outside what the console accepts (%ux%u to %ux%u)",
                      info->cell_w, info->cell_h,
                      OS64_FONT_PSF2_CELL_W_MIN, OS64_FONT_PSF2_CELL_H_MIN,
                      OS64_FONT_PSF2_CELL_W_MAX, OS64_FONT_PSF2_CELL_H_MAX);
        return buf;
    case OS64_FONT_MALFORMED: return "a damaged font";
    case OS64_FONT_NO_MEMORY: return "out of memory";
    default:                  return "refused";
    }
}

typedef struct {
    const uint8_t *face;
    size_t face_len;
    uint8_t *image;
    size_t image_len;
    os64_font_psf2_info_t info;
    os64_font_status_t status;
} render_t;

static bool render(render_t *r, uint32_t px)
{
    os64_font_psf2_options_t o = { .pixel_height = px };
    r->status = os64_font_render_psf2(&o, r->face, r->face_len,
                                      r->image, OS64_FONT_PSF2_IMAGE_MAX, &r->image_len, &r->info);
    return r->status == OS64_FONT_OK;
}

// The largest size whose grid still holds `cols` by `rows` on a screen of
// `sw` by `sh` pixels. The cell grows with the size, so the sizes whose
// CELL fits are a prefix of 8..96 and bisection finds its end — and the
// converter reports the cell even when it refuses the size (a hairline
// vanished, the cell is past the fence), so a refusal still answers the
// question the search is asking. From that end the search steps down to
// the nearest size the converter accepts, which is then the largest
// accepted size that fits, wherever the refusals fell. Only a refusal that
// leaves the cell unknown (the engine could not open the face at that
// size, or its advance was not a whole pixel) has to be guessed at as "does
// not fit", and *sure is cleared so the caller does not call the answer
// the largest. Returns 0 when nothing fits.
static uint32_t size_for_grid(render_t *r, uint32_t sw, uint32_t sh, uint32_t cols, uint32_t rows,
                              bool *sure)
{
    uint32_t lo = 8, hi = 96, best = 0;
    *sure = true;
    while (lo <= hi) {
        uint32_t mid = (lo + hi) / 2;
        bool ok = render(r, mid);
        bool fits;
        if (ok || r->info.cell_w > 0) {
            fits = sw / r->info.cell_w >= cols && sh / r->info.cell_h >= rows;
        } else {
            fits = false;
            *sure = false;
        }
        if (fits) { best = mid; lo = mid + 1; } else { if (mid == 0) break; hi = mid - 1; }
    }
    // `best` may itself be a refused size; every size below it fits.
    while (best >= 8 && !render(r, best))
        best--;
    return best >= 8 ? best : 0;
}

// ── console.conf with no `face` in it ───────────────────────────────────────
//
// A file that exists is somebody's deliberate act, so a file with no `face`
// gets a line naming what it did say: `font = …` is the natural slip, and
// a boot that silently keeps the boot face is the worst way to learn a key's
// name. A file of nothing but comments says nothing — there is nothing to
// correct.
typedef struct { uint32_t unknown, known; } keys_t;

static bool note_key(const char *key, const char *value, void *user)
{
    (void)value;
    keys_t *k = user;
    if (key == NULL) {
        k->unknown++;
        os64_hprintf(OS64_STDERR, "vtfont: console.conf: not a `key = value` line: %s\n", value);
    } else if (os64_streq_nocase(key, "face") || os64_streq_nocase(key, "size")) {
        k->known++;
    } else {
        k->unknown++;
        os64_hprintf(OS64_STDERR, "vtfont: console.conf: `%s` is not a setting here (the keys are `face` and `size`)\n", key);
    }
    return true;
}

static int complain_about_keys(void)
{
    keys_t k = {0};
    (void)os64_conf_find_read("console.conf", note_key, &k, NULL, 0);
    if (k.unknown != 0 || k.known != 0)
        os64_hprintf(OS64_STDERR, "vtfont: console.conf has no `face` line; the boot face stays\n");
    return k.unknown != 0 ? 1 : 0;
}

// ── main ────────────────────────────────────────────────────────────────────

// "24" or "100x40". Returns 0 for a pixel size, 1 for a grid, -1 for neither.
static int parse_size(const char *s, uint32_t *a, uint32_t *b)
{
    uint64_t x = 0, y = 0;
    const char *sep = s;
    while (*sep && *sep != 'x') sep++;
    if (*sep == 'x') {
        char left[16];
        size_t n = (size_t)(sep - s);
        if (n == 0 || n >= sizeof(left) || !sep[1]) return -1;
        os64_memcpy(left, s, n);
        left[n] = '\0';
        if (!os64_parse_u64(left, &x) || !os64_parse_u64(sep + 1, &y) || !x || !y) return -1;
        *a = (uint32_t)x; *b = (uint32_t)y;
        return 1;
    }
    if (!os64_parse_u64(s, &x) || !x || x > 1000) return -1;
    *a = (uint32_t)x;
    return 0;
}

int main(int argc, char **argv)
{
    bool startup = false;
    const os64_optspec_t specs[] = {
        {'\0', "startup", false, "apply console.conf's face and size (the kernel runs this at boot)",
         .flag = &startup},
    };
    os64_args_t args = {0};
    const char *operands[2] = {0};
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Change the face the virtual terminals draw with.";
    args.details = "FACE is a .psf bitmap, a .ttf/.otf outline face, or `boot`; a bare\n"
                   "name is looked for in " FONT_DIR ". SIZE is pixels (24) or the grid\n"
                   "you want (100x40): the largest size that still gives it. With no\n"
                   "operands, prints " DOOR ". console.conf holds the same two words\n"
                   "as `face =` and `size =`, applied once at boot.";
    int32_t count = os64_args_parse(&args, "vtfont [--startup] [FACE [SIZE]]", operands, 2);
    if (count == OS64_ARG_HELP) return 0;
    if (count < 0) return 2;

    // --startup: the operands come from console.conf instead. No file, or a
    // file with no face in it, is the boot face and nothing to say. A size
    // beside a bitmap face is ignored here rather than refused: the file is
    // edited by hand and a stale `size =` line is not worth a boot-time
    // complaint, while the same thing typed at a prompt is.
    static char conf_face[OS64_CONF_PATH_MAX], conf_size[64];
    if (startup) {
        if (count != 0) {
            os64_hprintf(OS64_STDERR, "vtfont: --startup takes its operands from console.conf\n");
            return 2;
        }
        int64_t r = os64_conf_get("console.conf", "face", conf_face, sizeof(conf_face));
        if (r == OS64_CONF_NO_FILE)
            return 0;
        if (r == OS64_CONF_NO_KEY)
            return complain_about_keys();
        if (r < 0) {
            os64_hprintf(OS64_STDERR, "vtfont: console.conf: cannot read `face`\n");
            return 1;
        }
        operands[0] = conf_face;
        count = 1;
        r = os64_conf_get("console.conf", "size", conf_size, sizeof(conf_size));
        if (r == 0) {
            operands[1] = conf_size;
            count = 2;
        } else if (r != OS64_CONF_NO_KEY) {
            os64_hprintf(OS64_STDERR, "vtfont: console.conf: cannot read `size`\n");
            return 1;
        }
    }
    if (count == 0) return show();

    if (os64_streq(operands[0], "boot")) {
        if (count == 2) {
            os64_hprintf(OS64_STDERR, "vtfont: the boot face has the size it has\n");
            return 2;
        }
        return offer((const uint8_t *)"boot", 4, "boot face");
    }

    char pathbuf[512];
    const char *path = resolve(operands[0], pathbuf, sizeof(pathbuf));
    if (path == NULL) {
        os64_hprintf(OS64_STDERR, "vtfont: no such face: %s (not a path, and not on " FONT_DIR ")\n", operands[0]);
        return 1;
    }

    uint32_t size_a = 0, size_b = 0;
    int size_kind = -1;   // -1 none, 0 pixels, 1 grid
    if (count == 2 && (size_kind = parse_size(operands[1], &size_a, &size_b)) < 0) {
        os64_hprintf(OS64_STDERR, "vtfont: '%s' is neither a size in pixels nor a COLSxROWS grid\n", operands[1]);
        return 2;
    }

    uint8_t *face = NULL;
    size_t face_len = 0;
    os64_slurp_status_t read = os64_slurp(path, OS64_FONT_FILE_MAX, &face, &face_len);
    if (read != OS64_SLURP_OK) {
        os64_hprintf(OS64_STDERR, "vtfont: %s: %s\n", path, os64_slurp_status_name(read));
        return 1;
    }

    // A bitmap: one size forever, so a size is either the one it has or a
    // question it can only answer with its own grid.
    if (is_psf2(face, face_len)) {
        if (startup)
            size_kind = -1;
        if (size_kind == 0) {
            os64_hprintf(OS64_STDERR, "vtfont: %s is a bitmap; it has the size it has\n", path);
            return 2;
        }
        if (size_kind == 1) {
            door_t d;
            uint32_t w = face[28] | (face[29] << 8), h = face[24] | (face[25] << 8);
            if (read_door(&d) && w && h)
                os64_hprintf(OS64_STDERR, "vtfont: %s is a bitmap with a %ux%u cell, which makes this screen %ux%u\n",
                             path, w, h, d.screen_w / w, d.screen_h / h);
            else
                os64_hprintf(OS64_STDERR, "vtfont: %s is a bitmap; it gives the grid it gives\n", path);
            return 2;
        }
        int rc = offer(face, face_len, path);
        os64_free(face);
        return rc;
    }
    if (has_psf2_magic(face, face_len)) {
        os64_hprintf(OS64_STDERR, "vtfont: %s is a PSF2 font cut short of its header\n", path);
        return 1;
    }
    if (is_psf1(face, face_len)) {
        os64_hprintf(OS64_STDERR, "vtfont: %s is a PSF1 font; the console takes PSF2\n", path);
        return 1;
    }

    // An outline face. It has no size of its own, and a default here would
    // be a policy hiding in a constant: the size is the operand's to say.
    if (size_kind < 0) {
        os64_hprintf(OS64_STDERR, "vtfont: %s is an outline face and needs a SIZE (pixels, or COLSxROWS)\n", path);
        return 2;
    }
    render_t r = { .face = face, .face_len = face_len, .image = os64_malloc(OS64_FONT_PSF2_IMAGE_MAX) };
    if (r.image == NULL) {
        os64_hprintf(OS64_STDERR, "vtfont: out of memory\n");
        return 1;
    }
    uint32_t px;
    if (size_kind == 1) {
        door_t d;
        if (!read_door(&d) || d.screen_w == 0) {
            os64_hprintf(OS64_STDERR, "vtfont: cannot learn the screen's size from " DOOR "\n");
            return 1;
        }
        bool sure = true;
        px = size_for_grid(&r, d.screen_w, d.screen_h, size_a, size_b, &sure);
        if (px == 0) {
            // Nothing fit — but a face the converter refuses at EVERY size
            // (proportional, not a font) fits nowhere for a reason that is
            // not the grid's, and that reason is the one to give. A size
            // refusal (a hairline, a cell past the fence) is the size's own.
            if (!render(&r, 24) && r.status != OS64_FONT_MISSING && r.status != OS64_FONT_LIMIT) {
                char why[160];
                os64_hprintf(OS64_STDERR, "vtfont: %s: %s\n", path, refusal(r.status, &r.info, why, sizeof(why)));
                return 1;
            }
            os64_hprintf(OS64_STDERR, "vtfont: no size of %s gives this %ux%u screen %ux%u\n",
                         path, d.screen_w, d.screen_h, size_a, size_b);
            return 1;
        }
        os64_printf("vtfont: %s at %u px %s %ux%u\n", path, px,
                    sure ? "is the largest that gives" : "gives", size_a, size_b);
    } else {
        px = size_a;
    }
    if (!render(&r, px)) {
        char why[160];
        os64_hprintf(OS64_STDERR, "vtfont: %s at %u px: %s\n", path, px, refusal(r.status, &r.info, why, sizeof(why)));
        return 1;
    }
    if (r.info.clipped != 0)
        os64_printf("vtfont: warning: %u glyphs lose ink to the cell's edge at %u px\n", r.info.clipped, px);

    char what[560];
    os64_snprintf(what, sizeof(what), "%s at %u px", path, px);
    int rc = offer(r.image, r.image_len, what);
    os64_free(r.image);
    os64_free(face);
    return rc;
}
