// Drive the public image dispatcher with independent GIF/reference fixtures.
// Codec stubs abort: this harness must exercise the production GIF path.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "image/image.h"
#include "png/png.h"
#include "jpeg/jpeg.h"
#include "os64/slurp.h"

static size_t live, attempts, fail_at, largest;
void *os64_malloc(size_t n)
{
    attempts++;
    if (n > largest) largest = n;
    if (fail_at && attempts == fail_at) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
void os64_free(void *p) { if (p) { assert(live); live--; free(p); } }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a,b,n); }
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d,c,n); }
os64_png_status_t os64_png_decode(const uint8_t *p, size_t n, uint64_t c, os64_png_image_t *o)
{ (void)p; (void)n; (void)c; (void)o; abort(); }
os64_jpeg_status_t os64_jpeg_decode(const uint8_t *p, size_t n, uint64_t c, size_t m, os64_jpeg_image_t *o)
{ (void)p; (void)n; (void)c; (void)m; (void)o; abort(); }
os64_slurp_status_t os64_slurp(const char *p, size_t c, uint8_t **o, size_t *n)
{ (void)p; (void)c; (void)o; (void)n; abort(); }

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f); assert(n >= 0);
    rewind(f);
    uint8_t *p = malloc((size_t)n + 1); assert(p);
    assert(fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f); *size = (size_t)n; return p;
}

static void empty(const os64_image_t *img)
{ assert(!img->pixels && !img->width && !img->height && !live); }

int main(int argc, char **argv)
{
    assert(argc == 3);
    size_t len, ref_len;
    uint8_t *data = read_file(argv[1], &len), *ref = read_file(argv[2], &ref_len);
    os64_image_t img;
    attempts = largest = 0;
    os64_image_status_t st = os64_image_decode(data, len, &img);
    if (ref_len <= 2) {
        assert(ref_len && st == ref[0]);
        empty(&img);
        if (ref_len == 2 && ref[1]) assert(attempts == 0);
    } else {
        if (st != OS64_IMAGE_OK) fprintf(stderr, "%s: %s\n", argv[1], os64_image_status_name(st));
        assert(st == OS64_IMAGE_OK && ref_len >= 8);
        uint32_t w, h; memcpy(&w, ref, 4); memcpy(&h, ref + 4, 4);
        assert(img.width == w && img.height == h && ref_len == 8 + (size_t)w*h*4);
        assert(memcmp(img.pixels, ref + 8, ref_len - 8) == 0);
        size_t allocations = attempts;
        os64_image_free(&img); empty(&img);
        // Every output/scratch allocation must clean up on failure.
        for (size_t a = 1; a <= allocations; a++) {
            attempts = 0; fail_at = a;
            assert(os64_image_decode(data, len, &img) == OS64_IMAGE_NO_MEMORY);
            empty(&img);
        }
        fail_at = 0;
        // A recognized signature makes incomplete files MALFORMED. Very
        // short prefixes remain UNKNOWN_FORMAT under libimage's contract.
        for (size_t n = 0; n < len; n++) {
            attempts = 0;
            st = os64_image_decode(data, n, &img);
            assert(st == (n < 6 ? OS64_IMAGE_UNKNOWN_FORMAT : OS64_IMAGE_MALFORMED));
            empty(&img); assert(attempts == 0);
        }
        // Deterministic mutations reach parser and LZW refusal paths.
        uint32_t rng = 0x715a9;
        for (unsigned n = 0; n < 400; n++) {
            rng = rng * 1664525u + 1013904223u;
            size_t at = 6 + rng % (len - 6);
            uint8_t saved = data[at];
            data[at] ^= (uint8_t)(1u << ((rng >> 24) & 7));
            st = os64_image_decode(data, len, &img);
            if (st == OS64_IMAGE_OK) os64_image_free(&img);
            else assert(st == OS64_IMAGE_MALFORMED || st == OS64_IMAGE_LIMIT ||
                        st == OS64_IMAGE_UNSUPPORTED || st == OS64_IMAGE_NO_MEMORY);
            empty(&img);
            data[at] = saved;
        }
    }
    free(ref); free(data);
    return 0;
}
