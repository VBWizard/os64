#ifndef OS64_JPEG_H
#define OS64_JPEG_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_JPEG_OK, OS64_JPEG_NOT_JPEG, OS64_JPEG_MALFORMED,
    OS64_JPEG_UNSUPPORTED, OS64_JPEG_LIMIT, OS64_JPEG_NO_MEMORY,
    OS64_JPEG_BAD_ARGUMENT
} os64_jpeg_status_t;
typedef struct { uint32_t width, height; uint32_t *pixels; } os64_jpeg_image_t;
#define OS64_JPEG_INPUT_MAX (20u * 1024u * 1024u)
#define OS64_JPEG_PIXEL_CAP_DEFAULT (16u * 1024u * 1024u)
#define OS64_JPEG_MEMORY_DEFAULT (128u * 1024u * 1024u)
#define OS64_JPEG_DIM_MAX 16384u
/* Complete JPEG bytes in, opaque top-first 0xAARRGGBB pixels out. EXIF primary
 * orientation is applied. Zero limits select defaults. memory_cap includes
 * decoder allocations and output, but excludes the borrowed input buffer.
 * Failure zeros out and releases all decoder-owned storage. */
__attribute__((visibility("default")))
os64_jpeg_status_t os64_jpeg_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_jpeg_image_t *out);
__attribute__((visibility("default"))) void os64_jpeg_free(os64_jpeg_image_t *image);
__attribute__((visibility("default"))) const char *os64_jpeg_status_name(os64_jpeg_status_t status);
__attribute__((visibility("default"))) const char *os64_jpeg_license(void);
#endif
