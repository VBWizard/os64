// pngtest — ring-3 proof that libpng and its transitive libgzip dependency
// load normally, reverse real scanline filters, preserve alpha, and refuse
// damaged input before a browser entrusts downloaded bytes to the codec.

#include "os64/os64.h"
#include "png/png.h"

#define PNGTEST_OK   0x90640000u
#define PNGTEST_FAIL 0x90640001u

static const uint8_t kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a,
    0x00, 0x00, 0x00, 0x21, 0x49, 0x44, 0x41, 0x54,
    0x78, 0x9c, 0x63, 0xfc, 0xcf, 0xc0, 0xf0, 0x9f,
    0xf1, 0x3f, 0x43, 0x23, 0x03, 0xe3, 0xff, 0x06,
    0x16, 0x86, 0xff, 0x40, 0x2e, 0x23, 0x63, 0xbd,
    0xa0, 0x92, 0xc9, 0x11, 0x00, 0x7b, 0x96, 0x08,
    0xaf, 0x93, 0x27, 0xb8, 0x3a, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82
};

static const uint32_t kPixels[] = {
    0xffff0000u, 0x8000ff00u, 0x000000ffu,
    0xffffffffu, 0xff000000u, 0x44112233u
};

static void fail(const char *reason)
{
    char message[160];
    os64_snprintf(message, sizeof(message), "pngtest: %s", reason);
    os64_serial_log(message);
    os64_printf("%s\n", message);
    os64_exit(PNGTEST_FAIL);
}

int main(void)
{
    os64_png_image_t image;
    os64_png_status_t status = os64_png_decode(kPng, sizeof(kPng), 0, &image);
    if (status != OS64_PNG_OK)
        fail(os64_png_status_name(status));
    if (image.width != 3 || image.height != 2)
        fail("dimensions differ");
    for (size_t i = 0; i < sizeof(kPixels) / sizeof(kPixels[0]); i++)
        if (image.pixels[i] != kPixels[i])
            fail("decoded pixels differ");
    os64_png_free(&image);
    if (image.pixels != NULL || image.width != 0 || image.height != 0)
        fail("free did not zero the result");

    status = os64_png_decode(kPng, sizeof(kPng), 5, &image);
    if (status != OS64_PNG_LIMIT)
        fail("pixel cap was not enforced");

    uint8_t corrupt[sizeof(kPng)];
    os64_memcpy(corrupt, kPng, sizeof(corrupt));
    corrupt[48] ^= 1; // IDAT payload; its stored chunk CRC must now disagree.
    status = os64_png_decode(corrupt, sizeof(corrupt), 0, &image);
    if (status != OS64_PNG_CHECKSUM)
        fail("damaged IDAT was accepted");

    return PNGTEST_OK;
}
