#ifndef YONDER_PICTURE_H
#define YONDER_PICTURE_H

// A picture's job on the work pool (YONDER.md § Y5): its bytes fetched, or
// read from a file, and decoded on a worker.

#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "image/image.h"
#include "image/sequence.h"
#include "jobs.h"
#include "way/way.h"

// What one picture job may cost, declared to the pool: libimage's file cap,
// and libjpeg's decoder memory cap — which also covers, by arithmetic
// rather than by any cap libpng enforces, PNG's largest raster (16
// megapixels, 64 MiB) and an inflate buffer of about the same size.
#define PICTURE_RESERVE ((size_t)OS64_IMAGE_CAP_DEFAULT + (128u << 20))

typedef struct {
    uint32_t kind;                  // YONDER_JOB_PICTURE
    int32_t index;                  // which of the page's pictures
    uint64_t generation;            // the page it belongs to
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    // The browser's cookies and the page's address for the Referer: a
    // picture is fetched as the page is (YONDER.md § Y3b).
    way_hooks_t hooks;
} yonder_picture_job_t;

// The picture: a still image, or — a GIF with more than one frame — a
// sequence, its first frame ready (YONDER.md § Y5b). `cost` is what
// keeping it costs, in bytes, for the page's account.
typedef struct {
    os64_image_status_t status;     // OK, or why there is no picture
    os64_image_t image;             // when `sequence` is NULL
    os64_image_sequence_t *sequence;
    size_t cost;
} yonder_picture_t;

int64_t yonder_picture_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_picture_release(void *job, void *product);

#endif
