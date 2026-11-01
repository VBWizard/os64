#ifndef YONDER_PICTURE_H
#define YONDER_PICTURE_H

// A picture's job on the work pool (YONDER.md § Y5): its bytes fetched, or
// read from a file, and decoded on a worker.

#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "image/image.h"
#include "image/sequence.h"
#include "webp/webp.h"
#include "jobs.h"
#include "svg_document.h"
#include "way/way.h"

// Admission covers the encoded body, XML/tree/sheet parsing, bounded SVG
// measurement cascade (including retained attribute parses and scratch), or
// libimage's largest codec budget — and, for a data: picture, the job's
// copy of its address, which the page catalog's budget for addresses
// bounds. Retained products move to the page account.
#define PICTURE_RESERVE ((size_t)512u << 20)
_Static_assert(PICTURE_RESERVE >= (size_t)OS64_IMAGE_CAP_DEFAULT + OS64_WEBP_MEMORY_DEFAULT,
               "picture admission must cover the raster codec budget");
typedef enum { YONDER_PICTURE_RASTER, YONDER_PICTURE_SEQUENCE, YONDER_PICTURE_SVG } yonder_picture_kind_t;

typedef struct {
    uint32_t kind;                  // YONDER_JOB_PICTURE
    int32_t index;                  // which of the page's pictures
    uint64_t generation;            // the page it belongs to
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    // A data: picture's address, the job's own copy (os64_malloc'd), or
    // NULL for one at `url`. A data: address carries the picture itself, so
    // `url` may not hold one whole, and the page's catalog may be gone before
    // the worker is: yonder_picture_release frees it, product or none.
    char *data_url;
    // The browser's cookies and the page's address for the Referer: a
    // picture is fetched as the page is (YONDER.md § Y3b); the agent's
    // preset lines and the picture's Fetch Metadata; and the browser's
    // cache, which may answer instead (CACHE.md).
    way_hooks_t hooks;
    bool svg_enabled, dark, reduced_motion;
    // The rectangle, in device pixels, the picture must cover where it is
    // drawn, or 0 x 0 for none known: a still raster bigger than that is
    // kept at yonder_picture_kept_size's answer, reduced here, and the full
    // decode never leaves the worker (PICTURE_SIZES.md). `resample` marks a
    // picture the page already has (shown, or past the memory kept) asked
    // for again at a new size, which replaces its pixels on arrival instead
    // of arriving as new.
    uint32_t cover_w, cover_h;
    bool resample;
} yonder_picture_job_t;

// A tagged product: raster pixels, a moving GIF sequence, or an independently
// owned SVG document. `cost` charges retained storage to the page account;
// temporary SVG rasterization has a separate serialized working allowance.
typedef struct {
    yonder_picture_kind_t kind;
    yonder_svg_document_t *svg;      // when kind is YONDER_PICTURE_SVG
    // Its own size in CSS pixels, any kind (an SVG's default object size),
    // which layout reads; a raster's `image` may be smaller (cover_w/h).
    uint32_t width, height;
    os64_image_status_t status;     // OK, or why there is no picture
    os64_image_t image;             // when kind is YONDER_PICTURE_RASTER: the pixels kept
    os64_image_sequence_t *sequence;
    size_t cost;
    // UNKNOWN_FORMAT: what the bytes are (yonder_diag_image_format), for
    // the page's record. NULL otherwise.
    const char *format;
    // Otherwise, why there is no picture, for the page's record: "HTTP
    // 404" (an error's body is not read as a picture), "did not fetch
    // (tls-failed)", "an empty reply", "could not be read", "would not
    // decode (malformed image)". Empty when there is a picture, and when
    // `format` says why.
    char why[64];
} yonder_picture_t;

int64_t yonder_picture_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_picture_release(void *job, void *product);

#endif
