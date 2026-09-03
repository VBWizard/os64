#ifndef OS64_LIBPNG_PNG_H
#define OS64_LIBPNG_PNG_H

// PNG bytes in, normalized pixels out. This library owns the PNG and zlib
// layers; raw DEFLATE remains libgzip's job, and drawing remains libdraw's.

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_PNG_OK = 0,
    OS64_PNG_NOT_PNG,
    OS64_PNG_MALFORMED,
    OS64_PNG_UNSUPPORTED,
    OS64_PNG_CHECKSUM,
    OS64_PNG_NO_MEMORY,
    OS64_PNG_LIMIT,
    OS64_PNG_BAD_ARGUMENT
} os64_png_status_t;

typedef struct os64_png_image {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;  // 0xAARRGGBB, top row first, tightly packed.
                       // os64_malloc'd; release with os64_png_free().
} os64_png_image_t;

// The default bound is on decoded pixels, not compressed bytes. PNG can make
// a tiny file claim a huge raster, so a file-size cap is not a memory cap.
#define OS64_PNG_PIXEL_CAP_DEFAULT (16u * 1024u * 1024u)
#define OS64_PNG_DIM_MAX 16384u

// Decode one complete PNG held in memory. pixel_cap 0 selects the default.
// On success, out owns its pixel plane. On every failure, out is zeroed and
// owns nothing. The decoder accepts non-interlaced PNG; Adam7 reports
// OS64_PNG_UNSUPPORTED rather than pretending the file is malformed.
os64_png_status_t os64_png_decode(const uint8_t *data, size_t length,
                                  uint64_t pixel_cap,
                                  os64_png_image_t *out);

void os64_png_free(os64_png_image_t *image);
const char *os64_png_status_name(os64_png_status_t status);

#endif // OS64_LIBPNG_PNG_H
