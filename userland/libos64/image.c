// image.c — libimage's decoders (os64/image.h carries the doctrine).
//
// EVERY BYTE READ HERE CAME FROM A FILE SOMEBODY ELSE WROTE. That single
// fact shapes the whole file: every length is checked against what is
// actually present before it is trusted, every dimension is bounded BEFORE
// it is multiplied, and a header that disagrees with the file's real size is
// refused rather than clamped. A decoder that clamps a bad header keeps
// running on a lie; one that refuses hands the caller a status it can print.

#include "os64/os64.h"
#include "os64/image.h"
#include "os64/slurp.h"
#include "os64/mem.h"

const char *os64_image_status_name(os64_image_status_t status)
{
    switch (status) {
        case OS64_IMAGE_OK:             return "ok";
        case OS64_IMAGE_NO_FILE:        return "no such file";
        case OS64_IMAGE_TOO_BIG:        return "file too large";
        case OS64_IMAGE_IO_ERROR:       return "read error";
        case OS64_IMAGE_NO_MEMORY:      return "out of memory";
        case OS64_IMAGE_UNKNOWN_FORMAT: return "not an image we recognize";
        case OS64_IMAGE_MALFORMED:      return "malformed image";
        case OS64_IMAGE_UNSUPPORTED:    return "unsupported variant";
    }
    return "unknown";
}

void os64_image_free(os64_image_t *img)
{
    if (img == NULL)
        return;
    os64_free(img->pixels);
    img->pixels = NULL;
    img->width = 0;
    img->height = 0;
}

// ── shared helpers ──────────────────────────────────────────────────────────

// Allocate the pixel plane for w x h, with the overflow argument made once
// here instead of at each decoder. OS64_IMAGE_DIM_MAX bounds each axis at
// 16384, so w*h is at most 2^28 and w*h*4 at most 2^30 — both comfortably
// inside size_t on x86-64, which is what makes the multiplication below safe
// rather than merely lucky.
static os64_image_status_t alloc_pixels(uint32_t w, uint32_t h,
                                        os64_image_t *out)
{
    if (w == 0 || h == 0 || w > OS64_IMAGE_DIM_MAX || h > OS64_IMAGE_DIM_MAX)
        return OS64_IMAGE_MALFORMED;

    size_t count = (size_t)w * (size_t)h;
    uint32_t *px = (uint32_t *)os64_malloc(count * sizeof(uint32_t));
    if (px == NULL)
        return OS64_IMAGE_NO_MEMORY;

    out->width = w;
    out->height = h;
    out->pixels = px;
    return OS64_IMAGE_OK;
}

// ── PPM (P6) ────────────────────────────────────────────────────────────────
//
// Netpbm's binary portable pixmap: "P6", whitespace-and-#-comment separated
// width, height and maxval, then exactly one whitespace byte, then raw RGB
// triples. Jef Poskanzer's 1988 format, and still the easiest way to get a
// picture out of a program that has no libraries.

// WHAT COUNTS AS WHITESPACE, ANSWERED ONCE (Codex #30 rd5). Three sites
// used to spell it by hand as the four bytes everybody remembers — space,
// tab, CR, LF — and all three forgot the two nobody does: vertical tab and
// form feed. Netpbm's own reader (libnetpbm's pm_getc/pm_getuint) accepts
// them via isspace(), so a file that separates its header with '\f' is a
// legal picture that os64 was refusing while claiming to parse "whitespace-
// separated" input. One predicate, so the next reviewer who finds a seventh
// byte fixes it in one place rather than in the two they happened to spot.
static bool ppm_is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

static bool ppm_skip_ws(const uint8_t *d, size_t len, size_t *i)
{
    for (;;) {
        while (*i < len && ppm_is_ws(d[*i]))
            (*i)++;
        // A '#' runs to end of line and may appear anywhere whitespace may —
        // including between the width and the height, which is why this is a
        // loop and not two straight-line skips.
        if (*i < len && d[*i] == '#') {
            while (*i < len && d[*i] != '\n')
                (*i)++;
            continue;
        }
        return *i < len;
    }
}

static bool ppm_read_uint(const uint8_t *d, size_t len, size_t *i,
                          uint32_t *out)
{
    if (!ppm_skip_ws(d, len, i))
        return false;
    uint32_t v = 0;
    int digits = 0;
    while (*i < len && d[*i] >= '0' && d[*i] <= '9') {
        // Refuse before the overflow, not after: a header of forty nines
        // must not wrap into a plausible-looking small number.
        if (v > (OS64_IMAGE_DIM_MAX * 10u))
            return false;
        v = v * 10u + (uint32_t)(d[*i] - '0');
        (*i)++;
        digits++;
    }
    if (digits == 0)
        return false;
    *out = v;
    return true;
}

static os64_image_status_t decode_ppm(const uint8_t *d, size_t len,
                                      os64_image_t *out)
{
    size_t i = 2;   // past "P6"
    uint32_t w, h, maxval;

    // WHITESPACE MUST FOLLOW THE MAGIC (Codex #30 rd2) — the unclosed edge of
    // rd1's separator fix, found in the same file one round later. Parsing
    // began at byte 2 and ppm_read_uint never required that it skipped
    // anything, so `P61 1 255\n...` read the '1' of "P61" as the width and
    // decoded a 1x1 image out of a magic number that is not P6 at all. The
    // format is whitespace-separated from end to end; every place that is
    // true has to ask, not just the one a review pointed at.
    if (i >= len || !ppm_is_ws(d[i]))
        return OS64_IMAGE_MALFORMED;

    if (!ppm_read_uint(d, len, &i, &w) ||
        !ppm_read_uint(d, len, &i, &h) ||
        !ppm_read_uint(d, len, &i, &maxval))
        return OS64_IMAGE_MALFORMED;

    // maxval 255 means one byte per sample. A 16-bit PPM (maxval up to
    // 65535) is a legal file we simply do not decode — UNSUPPORTED, not
    // MALFORMED, because there is nothing wrong with it.
    if (maxval == 0 || maxval > 65535)
        return OS64_IMAGE_MALFORMED;
    if (maxval != 255)
        return OS64_IMAGE_UNSUPPORTED;

    // Exactly ONE whitespace byte separates the header from the data, by the
    // spec — and it matters, because the first pixel byte may itself be a
    // space or a newline and skipping "whitespace" here would eat it.
    //
    // CHECK IT, do not merely step over it (Codex #30 rd1): stepping over
    // whatever happens to be there accepts `P6 1 1 255X...` as an image and
    // shifts the whole raster by a byte — a malformed header decoding to a
    // picture that is subtly wrong rather than to a refusal. The comment
    // above already said the separator was whitespace; the code had not been
    // asking.
    if (i >= len || !ppm_is_ws(d[i]))
        return OS64_IMAGE_MALFORMED;
    i++;

    // LENGTH BEFORE ALLOCATION (Codex #30 rd1). A twenty-byte file whose
    // header claims 16384x16384 would otherwise allocate a 1 GiB pixel plane
    // and only then discover it is truncated — which defeats the whole point
    // of the cap, since the memory a hostile file costs us is decided by its
    // HEADER rather than its size. The BMP path already checked first; this
    // one did not, so the guard existed on one door of two.
    // Dimensions first, so the multiplication below is bounded BEFORE it is
    // performed rather than justified afterwards. (alloc_pixels checks these
    // too; that is its job, not a reason for this arithmetic to run on
    // numbers nobody has looked at.)
    if (w == 0 || h == 0 || w > OS64_IMAGE_DIM_MAX || h > OS64_IMAGE_DIM_MAX)
        return OS64_IMAGE_MALFORMED;
    size_t need = (size_t)w * (size_t)h * 3u;
    if (len - i < need)
        return OS64_IMAGE_MALFORMED;            // the header promised more than
                                                // the file contains
    os64_image_status_t st = alloc_pixels(w, h, out);
    if (st != OS64_IMAGE_OK)
        return st;

    const uint8_t *p = d + i;
    uint32_t *px = out->pixels;
    size_t count = (size_t)w * (size_t)h;
    for (size_t n = 0; n < count; n++) {
        px[n] = 0xff000000u | ((uint32_t)p[0] << 16) |
                              ((uint32_t)p[1] << 8)  | (uint32_t)p[2];
        p += 3;
    }
    return OS64_IMAGE_OK;
}

// ── BMP ─────────────────────────────────────────────────────────────────────
//
// 14-byte file header, then a DIB header whose first field is its own size.
// We decode BITMAPINFOHEADER (40) and the larger V4/V5 headers, since those
// are supersets and everything we read lives in the first 40 bytes.
//
// The two traps, both of which have bitten every BMP reader ever written:
// rows are padded to a 4-BYTE BOUNDARY, and a POSITIVE height means the
// image is stored BOTTOM-UP (row 0 of the file is the bottom row of the
// picture). A negative height means top-down. Both appear in the wild.

static uint16_t rd16(const uint8_t *d) { return (uint16_t)(d[0] | (d[1] << 8)); }
static uint32_t rd32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

#define BMP_FILE_HEADER  14u
#define BMP_INFO_MIN     40u

static os64_image_status_t decode_bmp(const uint8_t *d, size_t len,
                                      os64_image_t *out)
{
    if (len < BMP_FILE_HEADER + BMP_INFO_MIN)
        return OS64_IMAGE_MALFORMED;

    uint32_t data_off = rd32(d + 10);
    uint32_t dib_size = rd32(d + 14);
    // TWO DIFFERENT COMPLAINTS, AND THEY HAD BEEN SHARING AN ANSWER (Codex
    // #30 rd4). A DIB header SMALLER than BITMAPINFOHEADER is a variant we do
    // not decode — the OS/2 core header — and that is UNSUPPORTED. A header
    // whose declared size runs past the end of the file is a BROKEN FILE, and
    // this header's own contract says that is MALFORMED. Answering
    // UNSUPPORTED for the second told the user their perfectly ordinary
    // 40-byte BITMAPINFOHEADER was a format we lack support for, when what we
    // actually lacked was the rest of their file.
    if (dib_size < BMP_INFO_MIN)
        return OS64_IMAGE_UNSUPPORTED;
    if (dib_size > len - BMP_FILE_HEADER)
        return OS64_IMAGE_MALFORMED;

    int32_t  bw     = (int32_t)rd32(d + 18);
    int32_t  bh     = (int32_t)rd32(d + 22);
    uint16_t planes = rd16(d + 26);
    uint16_t bpp    = rd16(d + 28);
    uint32_t comp   = rd32(d + 30);

    if (planes != 1)
        return OS64_IMAGE_MALFORMED;
    if (comp != 0)
        return OS64_IMAGE_UNSUPPORTED;   // RLE4/8, BITFIELDS, JPEG, PNG-in-BMP
    if (bpp != 24 && bpp != 32)
        return OS64_IMAGE_UNSUPPORTED;   // palettes and 16-bit: not yet

    // A negative height is top-down. INT32_MIN has no positive counterpart,
    // so it is refused here rather than negated into itself — the one input
    // that turns a tidy sign flip into a wrong answer.
    bool bottom_up = true;
    if (bh < 0) {
        if (bh == (int32_t)0x80000000)
            return OS64_IMAGE_MALFORMED;
        bottom_up = false;
        bh = -bh;
    }
    if (bw <= 0 || bh <= 0)
        return OS64_IMAGE_MALFORMED;

    uint32_t w = (uint32_t)bw;
    uint32_t h = (uint32_t)bh;
    if (w > OS64_IMAGE_DIM_MAX || h > OS64_IMAGE_DIM_MAX)
        return OS64_IMAGE_MALFORMED;

    // Row stride: bytes of pixel, rounded up to a multiple of 4. Bounded
    // above by 16384 * 4 + 3, so no overflow is possible here.
    uint32_t bytes_pp = bpp / 8u;
    size_t   stride   = (((size_t)w * bytes_pp) + 3u) & ~(size_t)3u;

    // The raster cannot begin INSIDE the headers (Codex #30 rd2). Only the
    // upper bound was checked, so a file claiming data_off 0 passed and the
    // decoder read its own file header back as pixel rows — returning OK
    // with a picture that was never in the file. A bound is two-sided or it
    // is half a bound.
    if (data_off < BMP_FILE_HEADER + dib_size || data_off > len)
        return OS64_IMAGE_MALFORMED;
    size_t avail = len - data_off;
    if (stride != 0 && h > avail / stride)
        return OS64_IMAGE_MALFORMED;   // truncated — checked by DIVISION so
                                       // the multiplication never happens

    os64_image_status_t st = alloc_pixels(w, h, out);
    if (st != OS64_IMAGE_OK)
        return st;

    const uint8_t *base = d + data_off;
    for (uint32_t y = 0; y < h; y++) {
        // Destination row: y counts down the picture. A bottom-up file's
        // first stored row is the picture's LAST row.
        uint32_t dst_y = bottom_up ? (h - 1u - y) : y;
        const uint8_t *row = base + (size_t)y * stride;
        uint32_t *px = out->pixels + (size_t)dst_y * w;

        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = row + (size_t)x * bytes_pp;
            // BMP stores BGR(A). The alpha byte of a BI_RGB 32-bit bitmap is
            // documented as reserved and is ZERO in most files that carry
            // it — so it is NOT trusted as transparency here. Taking it
            // literally would make every 32-bit BMP decode to a fully
            // transparent image, which is the classic way this goes wrong.
            // Real BMP alpha arrives via BI_BITFIELDS with an alpha mask,
            // which is on the UNSUPPORTED list above until something needs it.
            px[x] = 0xff000000u | ((uint32_t)s[2] << 16) |
                                  ((uint32_t)s[1] << 8)  | (uint32_t)s[0];
        }
    }
    return OS64_IMAGE_OK;
}

// ── the front door ──────────────────────────────────────────────────────────

os64_image_status_t os64_image_decode(const uint8_t *data, size_t len,
                                      os64_image_t *out)
{
    if (out == NULL)
        return OS64_IMAGE_MALFORMED;
    out->width = 0;
    out->height = 0;
    out->pixels = NULL;

    if (data == NULL || len < 2)
        return OS64_IMAGE_UNKNOWN_FORMAT;

    // MAGIC BYTES, not the name. See the header's argument.
    if (data[0] == 'P' && data[1] == '6')
        return decode_ppm(data, len, out);
    if (data[0] == 'B' && data[1] == 'M')
        return decode_bmp(data, len, out);

    return OS64_IMAGE_UNKNOWN_FORMAT;
}

os64_image_status_t os64_image_load(const char *path, size_t cap,
                                    os64_image_t *out)
{
    if (out == NULL)
        return OS64_IMAGE_MALFORMED;
    out->width = 0;
    out->height = 0;
    out->pixels = NULL;

    if (cap == 0)
        cap = OS64_IMAGE_CAP_DEFAULT;

    uint8_t *buf = NULL;
    size_t   len = 0;
    // The tree's SHARED whole-file reader (os64/slurp.h) — shared, not yet
    // sole, and the difference is worth stating plainly (Codex #30 rd1).
    // Four hand-written capped loops still stand as this is committed:
    // kernel/src/conf.c, kernel/src/gui/desktop.c, and both read paths in
    // libos64/conf.c. This decoder declines to be the fifth. Their adoption
    // is the DEBTS row's remaining payment, and desktop.c's copy dies with
    // the desktop shell; anyone fixing a whole-file-read bug still has to
    // visit all of them until then, which is exactly the cost the row is
    // about.
    os64_slurp_status_t sst = os64_slurp(path, cap, &buf, &len);
    switch (sst) {
        case OS64_SLURP_OK:        break;
        case OS64_SLURP_NO_FILE:   return OS64_IMAGE_NO_FILE;
        case OS64_SLURP_TOO_BIG:   return OS64_IMAGE_TOO_BIG;
        case OS64_SLURP_IO_ERROR:  return OS64_IMAGE_IO_ERROR;
        case OS64_SLURP_NO_MEMORY: return OS64_IMAGE_NO_MEMORY;
    }

    os64_image_status_t st = os64_image_decode(buf, len, out);
    os64_free(buf);   // the pixels are a fresh allocation; the file bytes go
    return st;
}
