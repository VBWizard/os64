// test_png_host.c — host-side exact-pixel and refusal harness for libpng.
// The companion Python generator makes independently decodable PNGs, then
// this program drives the freestanding codec under ASan and UBSan.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png/png.h"

static size_t g_live_allocations;
static size_t g_allocation_calls;
static size_t g_largest_allocation;

void *os64_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory != NULL) {
        g_live_allocations++;
        g_allocation_calls++;
        if (size > g_largest_allocation)
            g_largest_allocation = size;
    }
    return memory;
}

void os64_free(void *memory)
{
    if (memory != NULL) {
        if (g_live_allocations == 0) {
            fprintf(stderr, "free without a tracked allocation\n");
            exit(2);
        }
        g_live_allocations--;
    }
    free(memory);
}

void *os64_memset(void *memory, int value, size_t length)
{
    return memset(memory, value, length);
}

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *bytes = malloc(end == 0 ? 1u : (size_t)end);
    if (bytes == NULL || fread(bytes, 1, (size_t)end, file) != (size_t)end) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = (size_t)end;
    return bytes;
}

static uint64_t parse_u64(const char *text)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        fprintf(stderr, "bad integer: %s\n", text);
        exit(2);
    }
    return (uint64_t)value;
}

static int run_mutations(const char *path)
{
    size_t length = 0;
    uint8_t *seed = read_file(path, &length);
    if (seed == NULL)
        return 2;

    // Every possible truncation, then one flipped high bit at every byte.
    // Status is deliberately not prescribed here: the contract under test is
    // that arbitrary damage returns safely, owns nothing on refusal, and
    // releases everything if a mutation happens to remain decodable.
    for (size_t end = 0; end <= length; end++) {
        os64_png_image_t image;
        (void)os64_png_decode(seed, end, 0, &image);
        os64_png_free(&image);
    }
    for (size_t i = 0; i < length; i++) {
        seed[i] ^= 0x80u;
        os64_png_image_t image;
        (void)os64_png_decode(seed, length, 0, &image);
        os64_png_free(&image);
        seed[i] ^= 0x80u;
    }
    free(seed);

    os64_png_image_t image;
    if (os64_png_decode(NULL, 0, 0, &image) != OS64_PNG_BAD_ARGUMENT ||
        os64_png_decode((const uint8_t *)"", 0, 0, NULL) !=
            OS64_PNG_BAD_ARGUMENT) {
        fprintf(stderr, "bad-argument contract failed\n");
        return 1;
    }
    os64_png_free(NULL);
    if (g_live_allocations != 0) {
        fprintf(stderr, "%zu codec allocations leaked during mutations\n",
                g_live_allocations);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "mutate") == 0)
        return run_mutations(argv[2]);
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s PNG RGBA|- WIDTH HEIGHT PIXEL_CAP STATUS ALLOCS|any\n"
                "       %s mutate PNG\n",
                argv[0],
                argv[0]);
        return 2;
    }

    size_t png_length = 0;
    uint8_t *png = read_file(argv[1], &png_length);
    if (png == NULL) {
        fprintf(stderr, "could not read %s\n", argv[1]);
        return 2;
    }

    uint32_t width = (uint32_t)parse_u64(argv[3]);
    uint32_t height = (uint32_t)parse_u64(argv[4]);
    uint64_t cap = parse_u64(argv[5]);
    os64_png_image_t image;
    os64_png_status_t status = os64_png_decode(png, png_length, cap, &image);
    free(png);

    int failed = strcmp(os64_png_status_name(status), argv[6]) != 0;
    if (!failed && status == OS64_PNG_OK) {
        size_t expected_length = 0;
        uint8_t *expected = read_file(argv[2], &expected_length);
        size_t wanted_length = (size_t)width * height * 4u;
        if (expected == NULL || expected_length != wanted_length ||
            image.width != width || image.height != height) {
            failed = 1;
        } else {
            for (size_t i = 0; i < (size_t)width * height; i++) {
                uint32_t pixel = image.pixels[i];
                const uint8_t *rgba = expected + i * 4u;
                if ((uint8_t)(pixel >> 16) != rgba[0] ||
                    (uint8_t)(pixel >> 8) != rgba[1] ||
                    (uint8_t)pixel != rgba[2] ||
                    (uint8_t)(pixel >> 24) != rgba[3]) {
                    fprintf(stderr,
                            "pixel %zu: got %08x, want rgba %02x%02x%02x%02x\n",
                            i, pixel, rgba[0], rgba[1], rgba[2], rgba[3]);
                    failed = 1;
                    break;
                }
            }
        }
        free(expected);
    }

    os64_png_free(&image);
    if (strcmp(argv[7], "any") != 0 &&
        g_allocation_calls != (size_t)parse_u64(argv[7])) {
        fprintf(stderr, "allocation calls: got %zu, want %s (largest %zu)\n",
                g_allocation_calls, argv[7], g_largest_allocation);
        failed = 1;
    }
    if (g_live_allocations != 0) {
        fprintf(stderr, "%zu codec allocations leaked\n", g_live_allocations);
        failed = 1;
    }
    if (failed)
        fprintf(stderr, "decode failed: %s (wanted %s)\n",
                os64_png_status_name(status), argv[6]);
    return failed;
}
