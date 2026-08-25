// test_image_host.c — HOST-side unit test for libimage and libdraw's blit.
//
// Both are pure computation over buffers — no syscalls, no windows — which
// is exactly why image.c was split into a `load` half (file I/O) and a
// `decode` half (bytes in, pixels out). The decode half compiles with plain
// host gcc and can be checked the strongest possible way: EXACT PIXEL VALUES
// at known positions, on hand-built files whose every byte is written here.
//
// THE CENTRAL TEST IS THE CROSS-CHECK. The same picture is built as a PPM
// and as three different BMP variants, and all four must decode to
// byte-identical pixels. Two independently written decoders agreeing on one
// image catches the entire classic family at once — red/blue swapped,
// bottom-up rows drawn upside down, row padding miscounted — none of which a
// "does it look right?" glance reliably catches, because a symmetric test
// pattern looks right upside down.
//
// Build & run (one line):
//   gcc -fsanitize=address,undefined
//       -I userland/libos64/include -I abi/include -masm=intel
//       userland/libos64/image.c userland/libos64/draw.c
//       tools/test_image_host.c -o /tmp/os64_image_test
//   /tmp/os64_image_test
//
// RUN IT UNDER THE SANITIZERS. os64 itself cannot use them — a freestanding
// kernel has no runtime to link — which makes this host harness the only
// place in the project where they are available at all, so declining them
// here costs everything. Codex #30 rd3 found a heap-buffer-overflow in THIS
// FILE's own BMP builder that way: the fixture corrupted its own heap while
// reporting PASS, and no amount of reading it had caught that.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "os64/image.h"
#include "os64/draw.h"
#include "os64/slurp.h"
#include "os64/proc.h"

// libimage allocates through libos64's heap; on the host these are the
// system's. image.c never calls anything else, which is the whole reason it
// can be tested here.
// Instrumented, because one of these tests is about what was ALLOCATED
// rather than about what was returned. See the huge-dimension case: before
// the fix, a hostile header allocated a gigabyte and then returned exactly
// the same MALFORMED status as after it — so a test that checks the status
// alone passes against the bug, which is the vacuous assertion this project
// has been bitten by before (Codex #29 rd9's F24).
static size_t g_alloc_largest = 0;
void *os64_malloc(size_t size)
{
    if (size > g_alloc_largest)
        g_alloc_largest = size;
    return malloc(size);
}
void  os64_free(void *ptr)     { free(ptr); }

// ── stubs for the halves these tests deliberately do not reach ──────────────
//
// image.c's LOAD half and draw.c's text and frame-clock helpers call into the
// rest of libos64, which means syscalls. None of them is exercised here: the
// point of splitting decode from load is that the decoder is pure, and the
// file-reading half is proven in QEMU where there is a real filesystem to
// fail against. These exist so the linker is satisfied; if a test ever
// reaches one, it aborts rather than quietly returning a plausible zero.
static void host_stub(const char *name)
{
    printf("  FAIL: host test called %s, which is not testable here\n", name);
    exit(2);
}
os64_slurp_status_t os64_slurp(const char *path, size_t cap,
                               uint8_t **out, size_t *out_len)
{
    (void)path; (void)cap; (void)out; (void)out_len;
    host_stub("os64_slurp");
    return OS64_SLURP_IO_ERROR;
}
size_t  os64_strlen(const char *s) { (void)s; host_stub("os64_strlen"); return 0; }
int64_t os64_ticks(os64_ticks_t *out) { (void)out; host_stub("os64_ticks"); return 0; }
int64_t os64_sleep(uint64_t ms) { (void)ms; host_stub("os64_sleep"); return 0; }

static int failures = 0;
static int checks   = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void eq_u32(uint32_t got, uint32_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s (got 0x%08x, want 0x%08x)\n", what, got, want);
    }
}

static void eq_status(os64_image_status_t got, os64_image_status_t want,
                      const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s (got \"%s\", want \"%s\")\n", what,
               os64_image_status_name(got), os64_image_status_name(want));
    }
}

// ── the reference picture ───────────────────────────────────────────────────
//
// 7 wide by 5 high — ODD WIDTH ON PURPOSE, so a 24-bit BMP row is 21 bytes
// and must be padded to 24. A decoder that forgets the padding produces a
// picture that shears progressively, which is obvious here and invisible on
// a width that happens to be a multiple of four.
//
// The colors are chosen so that NO symmetry can hide an error: the four
// corners are four different colors, so a vertical flip, a horizontal flip
// and a red/blue swap each produce a different wrong answer.

#define REF_W 7u
#define REF_H 5u

static uint32_t ref_pixel(uint32_t x, uint32_t y)
{
    if (x == 0        && y == 0)        return 0xffff0000u;  // top-left  RED
    if (x == REF_W-1u && y == 0)        return 0xff00ff00u;  // top-right GREEN
    if (x == 0        && y == REF_H-1u) return 0xff0000ffu;  // bot-left  BLUE
    if (x == REF_W-1u && y == REF_H-1u) return 0xffffffffu;  // bot-right WHITE
    // A ramp everywhere else, distinct per position so a transposition or an
    // off-by-one row shows up as a mismatch rather than as more of the same.
    return 0xff000000u | ((x * 30u) << 16) | ((y * 50u) << 8) | (x * 10u + y);
}

// ── file builders ───────────────────────────────────────────────────────────

static uint8_t *build_ppm(size_t *out_len, int with_comments)
{
    char header[128];
    int hn;
    if (with_comments)
        // Comments legal ANYWHERE whitespace is — including between the
        // width and the height, which is the case a straight-line parser
        // gets wrong.
        hn = snprintf(header, sizeof(header),
                      "P6\n# os64 test image\n%u\n# between the dimensions\n%u\n255\n",
                      REF_W, REF_H);
    else
        hn = snprintf(header, sizeof(header), "P6 %u %u 255\n", REF_W, REF_H);

    size_t len = (size_t)hn + (size_t)REF_W * REF_H * 3u;
    uint8_t *f = malloc(len);
    memcpy(f, header, (size_t)hn);
    uint8_t *p = f + hn;
    for (uint32_t y = 0; y < REF_H; y++)
        for (uint32_t x = 0; x < REF_W; x++) {
            uint32_t c = ref_pixel(x, y);
            *p++ = (uint8_t)(c >> 16);   // R
            *p++ = (uint8_t)(c >> 8);    // G
            *p++ = (uint8_t)c;           // B
        }
    *out_len = len;
    return f;
}

static void wr16(uint8_t *d, uint16_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

// bpp: 24 or 32 write real pixels; ANY OTHER DEPTH writes a header only.
//
// That last clause is not decoration (Codex #30 rd3): the refusal tests build
// an 8-bit BMP to check it is declined as UNSUPPORTED, and the pixel loop
// below used to write three channel bytes per pixel regardless of depth. At
// one byte per pixel that overruns every row and eventually the allocation
// itself — a heap-buffer-overflow inside the fixture, reproducible under
// AddressSanitizer, in a test that reported PASS. A test that corrupts its
// own memory is not evidence about anything, and this one was checking a
// refusal that happens in the HEADER, so it never needed pixels at all.
static uint8_t *build_bmp(size_t *out_len, unsigned bpp, int top_down,
                          uint32_t compression)
{
    uint32_t bytes_pp = bpp / 8u;
    size_t stride = (((size_t)REF_W * bytes_pp) + 3u) & ~(size_t)3u;
    size_t pixels = stride * REF_H;
    size_t off = 14u + 40u;
    size_t len = off + pixels;

    uint8_t *f = calloc(1, len);
    f[0] = 'B'; f[1] = 'M';
    wr32(f + 2, (uint32_t)len);
    wr32(f + 10, (uint32_t)off);

    wr32(f + 14, 40u);                       // BITMAPINFOHEADER
    wr32(f + 18, REF_W);
    wr32(f + 22, top_down ? (uint32_t)(-(int32_t)REF_H) : REF_H);
    wr16(f + 26, 1u);                        // planes
    wr16(f + 28, (uint16_t)bpp);
    wr32(f + 30, compression);
    wr32(f + 34, (uint32_t)pixels);

    // Depths we do not decode get a header and a zeroed raster: the decoder
    // refuses them on the bpp field, so there is nothing for pixels to prove.
    if (bytes_pp != 3 && bytes_pp != 4) {
        *out_len = len;
        return f;
    }

    for (uint32_t row = 0; row < REF_H; row++) {
        // A bottom-up file stores the picture's LAST row first.
        uint32_t src_y = top_down ? row : (REF_H - 1u - row);
        uint8_t *r = f + off + (size_t)row * stride;
        for (uint32_t x = 0; x < REF_W; x++) {
            uint32_t c = ref_pixel(x, src_y);
            uint8_t *px = r + (size_t)x * bytes_pp;
            px[0] = (uint8_t)c;           // B
            px[1] = (uint8_t)(c >> 8);    // G
            px[2] = (uint8_t)(c >> 16);   // R
            if (bytes_pp == 4)
                px[3] = 0x00;             // BI_RGB's reserved byte: zero, as
                                          // real files have it. A decoder
                                          // that trusts it makes the whole
                                          // image transparent.
        }
        // Leave the padding bytes as calloc left them (zero) — a decoder that
        // reads them as pixels will produce black where it should not.
    }
    *out_len = len;
    return f;
}

// ── the checks ──────────────────────────────────────────────────────────────

static void check_matches_reference(const os64_image_t *img, const char *what)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s: width", what);
    eq_u32(img->width, REF_W, buf);
    snprintf(buf, sizeof(buf), "%s: height", what);
    eq_u32(img->height, REF_H, buf);
    if (img->width != REF_W || img->height != REF_H)
        return;

    for (uint32_t y = 0; y < REF_H; y++)
        for (uint32_t x = 0; x < REF_W; x++) {
            uint32_t got = img->pixels[(size_t)y * REF_W + x];
            uint32_t want = ref_pixel(x, y);
            if (got != want) {
                snprintf(buf, sizeof(buf), "%s: pixel (%u,%u)", what, x, y);
                eq_u32(got, want, buf);
                return;   // one report per image is enough to find it
            }
        }
    checks++;   // count the all-pixels-match pass
}

static void test_formats(void)
{
    printf("formats (the cross-check: four files, one picture)\n");

    struct { const char *name; uint8_t *(*mk)(size_t *); } dummy;
    (void)dummy;

    size_t len;
    os64_image_t img;

    uint8_t *ppm = build_ppm(&len, 0);
    eq_status(os64_image_decode(ppm, len, &img), OS64_IMAGE_OK, "ppm decodes");
    check_matches_reference(&img, "ppm");
    os64_image_free(&img);
    free(ppm);

    ppm = build_ppm(&len, 1);
    eq_status(os64_image_decode(ppm, len, &img), OS64_IMAGE_OK,
              "ppm with comments decodes");
    check_matches_reference(&img, "ppm+comments");
    os64_image_free(&img);
    free(ppm);

    uint8_t *bmp = build_bmp(&len, 24, 0, 0);
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_OK,
              "bmp24 bottom-up decodes");
    check_matches_reference(&img, "bmp24 bottom-up");
    os64_image_free(&img);
    free(bmp);

    bmp = build_bmp(&len, 24, 1, 0);
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_OK,
              "bmp24 top-down decodes");
    check_matches_reference(&img, "bmp24 top-down");
    os64_image_free(&img);
    free(bmp);

    bmp = build_bmp(&len, 32, 0, 0);
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_OK,
              "bmp32 decodes");
    // The reference is fully opaque and so must this be: the file's reserved
    // alpha bytes are ZERO, and a decoder that believed them would hand back
    // an invisible picture that still passes every RGB comparison.
    check_matches_reference(&img, "bmp32");
    os64_image_free(&img);
    free(bmp);
}

static void test_refusals(void)
{
    printf("refusals (broken files, and files we simply do not decode)\n");

    os64_image_t img;
    size_t len;

    eq_status(os64_image_decode((const uint8_t *)"", 0, &img),
              OS64_IMAGE_UNKNOWN_FORMAT, "empty input");
    eq_status(os64_image_decode((const uint8_t *)"ZZ....", 6, &img),
              OS64_IMAGE_UNKNOWN_FORMAT, "unknown magic");

    // Truncated PPM: the header promises 7x5 and the file stops short.
    uint8_t *ppm = build_ppm(&len, 0);
    eq_status(os64_image_decode(ppm, len - 4, &img), OS64_IMAGE_MALFORMED,
              "truncated ppm");
    free(ppm);

    // A 16-bit PPM is a perfectly legal file we do not decode — the status
    // must say UNSUPPORTED, not MALFORMED. Telling a user their file is
    // broken when it is our decoder that is incomplete sends them to fix the
    // wrong thing.
    {
        const char *hdr = "P6 4 4 65535\n";
        size_t hn = strlen(hdr);
        uint8_t f[64];
        memcpy(f, hdr, hn);
        memset(f + hn, 0, sizeof(f) - hn);
        eq_status(os64_image_decode(f, sizeof(f), &img), OS64_IMAGE_UNSUPPORTED,
                  "16-bit ppm is UNSUPPORTED, not MALFORMED");
    }

    // ANY maxval UNDER 256 IS ONE BYTE PER SAMPLE, SCALED (Codex #30 rd6).
    // The decoder refused everything but 255. `P6 1 1 100` with samples
    // 100,0,0 is a legal pure-red pixel; 50,0,0 is half-red, rounded to
    // nearest (50*255/100 = 127.5 -> 128); and a sample above maxval is
    // clamped rather than turned into a refusal of the whole picture.
    {
        const char f[] = "P6 3 1 100\n\x64\x00\x00\x32\x00\x00\xff\x00\x00";
        eq_status(os64_image_decode((const uint8_t *)f, sizeof(f) - 1, &img),
                  OS64_IMAGE_OK, "ppm with maxval 100 decodes");
        if (img.pixels) {
            eq_u32(img.pixels[0], 0xffff0000u, "maxval 100: full sample scales to 255");
            eq_u32(img.pixels[1], 0xff800000u, "maxval 100: half sample rounds to 128");
            eq_u32(img.pixels[2], 0xffff0000u, "maxval 100: over-range sample clamps");
            os64_image_free(&img);
        }
    }

    // Dimensions past OS64_IMAGE_DIM_MAX must be refused BEFORE any
    // multiplication — this is the hostile-header case.
    {
        const char *hdr = "P6 999999 999999 255\n";
        size_t hn = strlen(hdr);
        uint8_t f[64];
        memcpy(f, hdr, hn);
        memset(f + hn, 0, sizeof(f) - hn);
        os64_image_status_t st = os64_image_decode(f, sizeof(f), &img);
        ok(st == OS64_IMAGE_MALFORMED, "absurd ppm dimensions refused");
    }

    // THE SEPARATOR AFTER maxval MUST BE WHITESPACE (Codex #30 rd1). Stepping
    // over whatever byte is there accepts this as a 1x1 image with its raster
    // shifted by one — a malformed header decoding to a subtly wrong picture
    // instead of a refusal.
    {
        const char f[] = "P6 1 1 255X\xaa\xbb\xcc";
        eq_status(os64_image_decode((const uint8_t *)f, sizeof(f) - 1, &img),
                  OS64_IMAGE_MALFORMED,
                  "ppm with a non-whitespace raster separator");
    }
    // ...and a legal one still works with each of the SIX whitespace bytes,
    // so the check above cannot be "reject everything" and pass by accident.
    // Six, not four (Codex #30 rd5): vertical tab and form feed are Netpbm
    // whitespace too — libnetpbm reads through isspace() — and the decoder
    // refused both while calling itself whitespace-separated.
    {
        const char *seps = " \t\r\n\v\f";
        for (int k = 0; k < 6; k++) {
            char f[16];
            int n = snprintf(f, sizeof(f), "P6 1 1 255%c", seps[k]);
            f[n++] = 0x11; f[n++] = 0x22; f[n++] = 0x33;
            char what[64];
            snprintf(what, sizeof(what), "ppm accepts separator %d", k);
            eq_status(os64_image_decode((const uint8_t *)f, (size_t)n, &img),
                      OS64_IMAGE_OK, what);
            if (img.pixels) {
                eq_u32(img.pixels[0], 0xff112233u, "ppm raster is not shifted");
                os64_image_free(&img);
            }
        }
    }

    // LENGTH BEFORE ALLOCATION (Codex #30 rd1). A tiny file claiming huge
    // dimensions must be refused WITHOUT ever reserving the plane. The
    // status is the same either way — MALFORMED before the fix and after —
    // so this asserts on the ALLOCATION, which is the thing that actually
    // changed. 16384x16384x4 is a gigabyte; on os64 that is a dedicated
    // heap mapping reserved and torn down on the say-so of a hostile header,
    // which is precisely what the documented cap exists to prevent.
    {
        const char hdr[] = "P6 16384 16384 255\n";
        g_alloc_largest = 0;
        eq_status(os64_image_decode((const uint8_t *)hdr, sizeof(hdr) - 1, &img),
                  OS64_IMAGE_MALFORMED, "huge-dimension truncated ppm refused");
        ok(g_alloc_largest < 1024 * 1024,
           "huge-dimension ppm allocates nothing (refused on length first)");
    }

    // WHITESPACE AFTER THE MAGIC (Codex #30 rd2 — the unclosed edge of rd1's
    // separator fix). `P61 1 255` is not a P6 file; reading the '1' of the
    // magic as a width produced a 1x1 image out of nothing.
    {
        const char f[] = "P61 1 255\n\x01\x02\x03";
        eq_status(os64_image_decode((const uint8_t *)f, sizeof(f) - 1, &img),
                  OS64_IMAGE_MALFORMED, "ppm with no whitespace after the magic");
    }

    // A BMP RASTER CANNOT START INSIDE THE HEADERS (Codex #30 rd2). With
    // data_off 0 the decoder read the file header back as pixel rows and
    // returned OK with a picture that was never in the file.
    {
        uint8_t *b = build_bmp(&len, 24, 0, 0);
        wr32(b + 10, 0);
        eq_status(os64_image_decode(b, len, &img), OS64_IMAGE_MALFORMED,
                  "bmp with data offset inside its own headers");
        free(b);
        b = build_bmp(&len, 24, 0, 0);
        wr32(b + 10, 14 + 39);   // one byte short of the end of the DIB header
        eq_status(os64_image_decode(b, len, &img), OS64_IMAGE_MALFORMED,
                  "bmp with data offset one byte inside the DIB header");
        free(b);
    }

    // THE DEFAULT CAP MUST ADMIT WHAT ITS COMMENT CLAIMS (Codex #30 rd2). The
    // old 16MB cap refused a 2048x2048 32-bit BMP by 54 bytes — the exact
    // image the comment said it held "with room to spare". Pinned here so the
    // number and the claim cannot drift apart again in silence.
    {
        size_t raster_2048 = (size_t)2048 * 2048 * 4;
        ok(OS64_IMAGE_CAP_DEFAULT >= raster_2048 + 54,
           "default cap admits a 2048x2048 32-bit BMP including its headers");
    }

    uint8_t *bmp = build_bmp(&len, 24, 0, 1 /* BI_RLE8 */);
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_UNSUPPORTED,
              "compressed bmp is UNSUPPORTED");
    free(bmp);

    // A DIB header that runs past the end of the file is a BROKEN FILE, not a
    // variant we lack (Codex #30 rd4). The two answers mean opposite things:
    // one says fix your file, the other says wait for us.
    {
        uint8_t *b = build_bmp(&len, 24, 0, 0);
        wr32(b + 14, 108);   // BITMAPV4HEADER — supported size, absent bytes
        eq_status(os64_image_decode(b, 14 + 60, &img), OS64_IMAGE_MALFORMED,
                  "bmp whose DIB header runs past the end of the file");
        free(b);
    }
    // ...while a header too small to BE a BITMAPINFOHEADER stays UNSUPPORTED.
    {
        uint8_t *b = build_bmp(&len, 24, 0, 0);
        wr32(b + 14, 12);    // the OS/2 core header
        eq_status(os64_image_decode(b, len, &img), OS64_IMAGE_UNSUPPORTED,
                  "bmp with an OS/2 core header is UNSUPPORTED, not MALFORMED");
        free(b);
    }

    bmp = build_bmp(&len, 8, 0, 0);
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_UNSUPPORTED,
              "8-bit (palette) bmp is UNSUPPORTED");
    free(bmp);

    bmp = build_bmp(&len, 24, 0, 0);
    eq_status(os64_image_decode(bmp, len / 2, &img), OS64_IMAGE_MALFORMED,
              "truncated bmp");
    free(bmp);

    // A header whose pixel offset points past the end of the file.
    bmp = build_bmp(&len, 24, 0, 0);
    wr32(bmp + 10, (uint32_t)(len + 1000));
    eq_status(os64_image_decode(bmp, len, &img), OS64_IMAGE_MALFORMED,
              "bmp data offset past end of file");
    free(bmp);
}

// ── the blit ────────────────────────────────────────────────────────────────

#define DST_W 10
#define DST_H 6
#define DST_PITCH 16   // pitch > width, as a real window canvas has

static uint32_t dst_pixels[DST_PITCH * DST_H];

static os64_gui_surface_t make_dst(void)
{
    for (int i = 0; i < DST_PITCH * DST_H; i++)
        dst_pixels[i] = 0xff111111u;
    os64_gui_surface_t s = { dst_pixels, DST_W, DST_H, DST_PITCH };
    return s;
}

static uint32_t dst_at(int x, int y) { return dst_pixels[y * DST_PITCH + x]; }

static void test_blit(void)
{
    printf("blit (clipping, pitch, and the guard rails)\n");

    // A 4x3 source with a distinct value per cell.
    uint32_t src[4 * 3];
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++)
            src[y * 4 + x] = 0xff000000u | (uint32_t)((y << 8) | x);

    // Plain placement.
    os64_gui_surface_t d = make_dst();
    os64_draw_blit(&d, 2, 1, src, 4, 3, 4);
    eq_u32(dst_at(2, 1), src[0], "blit: top-left lands where asked");
    eq_u32(dst_at(5, 3), src[2 * 4 + 3], "blit: bottom-right lands");
    eq_u32(dst_at(1, 1), 0xff111111u, "blit: does not paint left of itself");
    eq_u32(dst_at(6, 1), 0xff111111u, "blit: does not paint right of itself");
    // The pixel one row below the source's last, which pitch confusion hits.
    eq_u32(dst_at(2, 4), 0xff111111u, "blit: does not paint below itself");

    // OFF-CANVAS AT THE EDGE OF int32_t (Codex #30 rd5): the contract says an
    // off-canvas blit is a no-op, and "off-canvas" includes x == INT32_MAX,
    // where `x + w` overflowed inside os64_rect_intersect — undefined
    // behaviour, flagged by the sanitizer build this file documents, before
    // the promised no-op could happen. Widened edge sums now; this pins it.
    d = make_dst();
    os64_draw_blit(&d, INT32_MAX, INT32_MAX, src, 4, 3, 4);
    os64_draw_blit(&d, INT32_MIN, INT32_MIN, src, 4, 3, 4);
    os64_draw_fill_rect(&d, (os64_gui_rect_t){INT32_MAX, 0, 1, 1}, 0xff000000u);
    eq_u32(dst_at(0, 0), 0xff111111u, "blit at INT32_MAX/INT32_MIN is a no-op, not UB");

    // NEGATIVE ORIGIN: clips the source's left and top, does not shift it.
    // This is the case "center an image bigger than the window" depends on.
    d = make_dst();
    os64_draw_blit(&d, -2, -1, src, 4, 3, 4);
    eq_u32(dst_at(0, 0), src[1 * 4 + 2], "blit: negative origin crops, not shifts");
    eq_u32(dst_at(1, 1), src[2 * 4 + 3], "blit: negative origin, second pixel");

    // Off the right and bottom edges: legal, partially drawn, no overrun.
    d = make_dst();
    os64_draw_blit(&d, DST_W - 2, DST_H - 1, src, 4, 3, 4);
    eq_u32(dst_at(DST_W - 2, DST_H - 1), src[0], "blit: clipped at right/bottom");
    // Nothing may have been written into the pitch slack past DST_W.
    ok(dst_pixels[(DST_H - 1) * DST_PITCH + DST_W] == 0xff111111u,
       "blit: never writes into the pitch slack past width");

    // Entirely off-surface: a no-op, not a crash.
    d = make_dst();
    os64_draw_blit(&d, 100, 100, src, 4, 3, 4);
    eq_u32(dst_at(0, 0), 0xff111111u, "blit: fully clipped draws nothing");
    os64_draw_blit(&d, -100, -100, src, 4, 3, 4);
    eq_u32(dst_at(0, 0), 0xff111111u, "blit: fully clipped negative draws nothing");

    // A SUB-IMAGE of a larger buffer: src points at (1,1) of the 4x3 and
    // claims 2x2 with the parent's pitch. This is what pitch is for.
    d = make_dst();
    os64_draw_blit(&d, 0, 0, src + 1 * 4 + 1, 2, 2, 4);
    eq_u32(dst_at(0, 0), src[1 * 4 + 1], "blit: sub-image honors source pitch");
    eq_u32(dst_at(1, 1), src[2 * 4 + 2], "blit: sub-image second row");

    // A pitch narrower than the width would read past each row's end.
    d = make_dst();
    os64_draw_blit(&d, 0, 0, src, 4, 3, 2);
    eq_u32(dst_at(0, 0), 0xff111111u, "blit: refuses pitch < width");

    // Degenerate inputs.
    d = make_dst();
    os64_draw_blit(&d, 0, 0, src, 0, 3, 4);
    os64_draw_blit(&d, 0, 0, NULL, 4, 3, 4);
    eq_u32(dst_at(0, 0), 0xff111111u, "blit: zero size and NULL source are no-ops");
}

int main(void)
{
    printf("libimage + libdraw blit — host tests\n\n");
    test_formats();
    test_refusals();
    test_blit();
    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0)
        printf("PASS\n");
    else
        printf("FAIL\n");
    return failures != 0;
}
