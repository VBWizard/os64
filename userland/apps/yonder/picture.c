// picture.c — a picture fetched and decoded on a worker (picture.h).

#include "picture.h"
#include "diag.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

// Every type libimage decodes, then anything: a server that labels a GIF
// as text/plain still sent a GIF, and the decoder reads the bytes, not the
// label.
#define PICTURE_ACCEPT OS64_IMAGE_ACCEPT ", */*;q=0.5"
_Static_assert(sizeof(PICTURE_ACCEPT) <= OS64_FETCH_ACCEPT_MAX,
               "libfetch refuses an Accept longer than OS64_FETCH_ACCEPT_MAX");

// A GIF is opened as a sequence first, and kept as one when it moves; a
// still one, and every other kind, is decoded whole. The sequence copies
// the bytes, so they and it are all this job holds at its peak — inside
// PICTURE_RESERVE — before the bytes go.
static os64_image_status_t decode(const uint8_t *bytes, size_t len, yonder_picture_t *p)
{
    if (len >= 4 && os64_memcmp(bytes, "GIF8", 4) == 0) {
        os64_image_sequence_t *seq = NULL;
        if (os64_image_sequence_decode(bytes, len, &seq) == OS64_IMAGE_OK) {
            const os64_image_frame_t *f = os64_image_sequence_frame(seq);
            if (f->frame_count > 1) {
                // libimage's account of a handle (GIF_ANIMATION.md): the
                // copied input, the canvas, a byte a pixel of staging, and
                // at most a canvas-sized restore rectangle.
                p->sequence = seq;
                p->cost = len + (size_t)f->width * f->height * 9u;
                return OS64_IMAGE_OK;
            }
        }
        os64_image_sequence_free(seq);
    }
    os64_image_status_t st = os64_image_decode(bytes, len, &p->image);
    p->cost = (size_t)p->image.width * p->image.height * 4u;
    if (st == OS64_IMAGE_UNKNOWN_FORMAT)
        p->format = yonder_diag_image_format(bytes, len);
    return st;
}

int64_t yonder_picture_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_picture_job_t *j = job;
    yonder_picture_t *p = os64_calloc(1, sizeof(*p));
    if (p == NULL)
        return -1;
    *out = p;
    // A picture on a page read from disk is a file beside it.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        size_t len = 0;
        uint8_t *bytes = NULL;
        p->status = OS64_IMAGE_IO_ERROR;
        if (os64_slurp(j->url + 7, OS64_IMAGE_CAP_DEFAULT, &bytes, &len) == OS64_SLURP_OK) {
            p->status = decode(bytes, len, p);
            os64_free(bytes);
        }
        return p->status == OS64_IMAGE_OK ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = PICTURE_ACCEPT;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_whole_t body;
    p->status = OS64_IMAGE_IO_ERROR;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, OS64_IMAGE_CAP_DEFAULT, &body))
        return 0;
    p->status = cancelled(ctx) ? OS64_IMAGE_IO_ERROR : decode(body.bytes, body.len, p);
    os64_free(body.bytes);
    return p->status == OS64_IMAGE_OK ? 1 : 0;
}

void yonder_picture_release(void *job, void *product)
{
    yonder_picture_t *p = product;
    if (p != NULL) {
        os64_image_free(&p->image);
        os64_image_sequence_free(p->sequence);
        os64_free(p);
    }
    os64_free(job);
}
