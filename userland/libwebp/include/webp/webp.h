#ifndef OS64_WEBP_H
#define OS64_WEBP_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_WEBP_OK, OS64_WEBP_NOT_WEBP, OS64_WEBP_MALFORMED,
    OS64_WEBP_UNSUPPORTED, OS64_WEBP_LIMIT, OS64_WEBP_NO_MEMORY,
    OS64_WEBP_BAD_ARGUMENT
} os64_webp_status_t;
typedef struct { uint32_t width, height; uint32_t *pixels; } os64_webp_image_t;
#define OS64_WEBP_INPUT_MAX (20u * 1024u * 1024u)
#define OS64_WEBP_PIXEL_CAP_DEFAULT (16u * 1024u * 1024u)
#define OS64_WEBP_MEMORY_DEFAULT (256u * 1024u * 1024u)
#define OS64_WEBP_DIM_MAX 16384u

/* Complete RIFF WebP stills to straight-alpha, top-first 0xAARRGGBB pixels.
 * Animation is unsupported; ICC/EXIF metadata is not applied. Zero caps select
 * defaults. memory_cap charges output and temporary allocations/bookkeeping,
 * excluding borrowed input, heap metadata, static tables and stack storage.
 * Pass an empty result. Failure zeros it; success transfers ordinary heap
 * storage. Calls serialize with sleeping contention and must not re-enter
 * from signal handlers or callbacks. Free does not acquire the decode gate. */
__attribute__((visibility("default")))
os64_webp_status_t os64_webp_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_webp_image_t *out);
__attribute__((visibility("default"))) void os64_webp_free(os64_webp_image_t *image);
__attribute__((visibility("default"))) const char *os64_webp_status_name(os64_webp_status_t status);
__attribute__((visibility("default"))) const char *os64_webp_license(void);
#endif
