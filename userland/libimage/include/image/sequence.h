#ifndef OS64_IMAGE_SEQUENCE_H
#define OS64_IMAGE_SEQUENCE_H
#include "image/image.h"

typedef struct os64_image_sequence os64_image_sequence_t;

typedef struct {
    uint32_t width, height;
    const uint32_t *pixels;  // Borrowed straight-alpha ARGB; do not free or modify.
    uint32_t index, frame_count;
    uint32_t delay_ms;       // Encoded delay; playback chooses a floor for zero.
    uint32_t play_count;     // Total passes; zero means indefinite looping.
} os64_image_frame_t;

// Open a self-contained sequence with its first frame ready. GIF supports
// multiple frames; other supported image formats yield a single frame.
// Decode copies GIF input. Load uses the ordinary image file cap (0=default).
// On failure *out is NULL. GIF limits: 20 MiB encoded, 4096 frames, 16 Mi
// pixels, 128 MiB total owned storage; caller input is outside that budget.
os64_image_status_t os64_image_sequence_decode(const uint8_t *data, size_t len,
                                               os64_image_sequence_t **out);
os64_image_status_t os64_image_sequence_load(const char *path, size_t cap,
                                             os64_image_sequence_t **out);
// The view is valid until the next successful next/rewind or free. Failure
// and OS64_IMAGE_END preserve the displayed frame. Separate handles may run
// concurrently; callers serialize operations on the same handle.
const os64_image_frame_t *os64_image_sequence_frame(const os64_image_sequence_t *seq);
// Advance, applying disposal and loop rules. END holds the final picture.
// Later malformed raster data is reported when that frame is reached.
os64_image_status_t os64_image_sequence_next(os64_image_sequence_t *seq);
os64_image_status_t os64_image_sequence_rewind(os64_image_sequence_t *seq);
void os64_image_sequence_free(os64_image_sequence_t *seq);
#endif
