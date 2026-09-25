#include "os64/os64.h"
#include "image/image.h"
#include "vectors.h"

static bool check(const uint8_t *data, size_t size, const uint8_t *expected)
{
    os64_image_t image;
    if (os64_image_decode(data, size, &image) != OS64_IMAGE_OK)
        return false;
    uint32_t w = expected[0] | ((uint32_t)expected[1] << 8);
    uint32_t h = expected[4] | ((uint32_t)expected[5] << 8);
    bool good = image.width == w && image.height == h;
    const uint8_t *pixels = (const uint8_t *)image.pixels;
    for (size_t i = 0; good && i < (size_t)w*h*4; i++)
        good = pixels[i] == expected[i+8];
    os64_image_free(&image);
    return good && !image.pixels && !image.width && !image.height;
}

int main(void)
{
    if (!check(gif_data_0, sizeof gif_data_0, gif_pixels_0) ||
        !check(gif_data_1, sizeof gif_data_1, gif_pixels_1) ||
        !check(gif_data_2, sizeof gif_data_2, gif_pixels_2) ||
        !check(gif_data_3, sizeof gif_data_3, gif_pixels_3))
        return 1;
    for (size_t n = 6; n < sizeof gif_data_0; n++) {
        os64_image_t image;
        if (os64_image_decode(gif_data_0, n, &image) != OS64_IMAGE_MALFORMED ||
            image.pixels || image.width || image.height)
            return 2;
    }
    char path[96];
    os64_snprintf(path, sizeof path, "/tmp/giftest-%lu.gif", os64_taskid());
    int64_t handle = os64_open(path, "w");
    if (handle < 0)
        return 3;
    int64_t written = os64_write((int32_t)handle, gif_data_0, sizeof gif_data_0);
    int64_t closed = os64_close((int32_t)handle);
    os64_image_t loaded;
    os64_image_status_t status = os64_image_load(path, 0, &loaded);
    bool good = written == sizeof gif_data_0 && closed == 0 && status == OS64_IMAGE_OK &&
                loaded.width == 19 && loaded.height == 11;
    os64_image_free(&loaded);
    if (os64_unlink(path) < 0 || !good || os64_heap_verify())
        return 4;
    os64_printf("PASS GIF palettes, transparency, interlace, offset canvas, LZW, truncation, file loading and heap cleanup\n");
    return 0;
}
