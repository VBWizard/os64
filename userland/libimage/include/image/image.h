#ifndef OS64_IMAGE_H
#define OS64_IMAGE_H
// libimage: file bytes to owned top-first 0xAARRGGBB pixels. Format detection
// uses signatures. BMP/PPM live here; libpng/libjpeg own their codecs. JPEG
// applies EXIF orientation. Drawing and surfaces belong to libdraw.
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_IMAGE_OK = 0,
    OS64_IMAGE_NO_FILE,          // could not open it
    OS64_IMAGE_TOO_BIG,          // file exceeds the cap — nothing decoded
    OS64_IMAGE_IO_ERROR,         // a read failed partway through
    OS64_IMAGE_NO_MEMORY,        // could not allocate
    OS64_IMAGE_UNKNOWN_FORMAT,   // magic bytes match nothing we decode
    OS64_IMAGE_MALFORMED,        // right format, wrong contents (truncated,
                                 // impossible dimensions, bad header)
    OS64_IMAGE_UNSUPPORTED,      // right format, a variant we do not decode
                                 // (compressed BMP, 16-bit PPM samples)
    OS64_IMAGE_LIMIT             // decoder dimensions, pixel or working-memory cap
} os64_image_status_t;

typedef struct os64_image {
    uint32_t  width;
    uint32_t  height;
    uint32_t *pixels;   // 0xAARRGGBB, width*height, tightly packed.
                        // os64_malloc'd — release with os64_image_free.
} os64_image_t;

// Default bound on encoded file bytes for os64_image_load.
#define OS64_IMAGE_CAP_DEFAULT (20u * 1024u * 1024u)

// Bound either axis before calculating raster sizes.
#define OS64_IMAGE_DIM_MAX 16384u

// Load and decode a file. `cap` 0 means OS64_IMAGE_CAP_DEFAULT.
// On OS64_IMAGE_OK, *out owns pixels the caller frees with os64_image_free.
// On anything else *out is zeroed and owns nothing.
os64_image_status_t os64_image_load(const char *path, size_t cap,
                                    os64_image_t *out);

// Decode bytes already in memory. Same contract, no file involved — this is
// the half that is testable without a filesystem, and the half a future
// network or clipboard source would call.
os64_image_status_t os64_image_decode(const uint8_t *data, size_t len,
                                      os64_image_t *out);

// Release an image's pixels and zero it. NULL is a no-op; calling it twice
// is safe (the zeroing is what makes that true).
void os64_image_free(os64_image_t *img);

// The status as a phrase, for messages. Never NULL.
const char *os64_image_status_name(os64_image_status_t status);

#endif // OS64_IMAGE_H
