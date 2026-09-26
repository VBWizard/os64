// picture.c — a picture fetched and decoded on a worker (picture.h).

#include "picture.h"
#include "os64/mem.h"
#include "os64/str.h"

// Every type libimage decodes, then anything: a server that labels a GIF
// as text/plain still sent a GIF, and the decoder reads the bytes, not the
// label.
#define PICTURE_ACCEPT "image/png, image/jpeg, image/gif, image/bmp, */*;q=0.5"

// The body, whole, up to libimage's file cap. NULL on no memory or when the
// fetch did not deliver all of it.
static uint8_t *read_all(os64_fetch_t *f, size_t *len)
{
    size_t at = 0, size = 16u * 1024u;
    uint8_t *buf = os64_malloc(size);
    if (buf == NULL)
        return NULL;
    for (;;) {
        if (at == size) {
            size_t want = size * 2 > OS64_IMAGE_CAP_DEFAULT ? OS64_IMAGE_CAP_DEFAULT : size * 2;
            if (want == size) {
                // ASK FOR THE BYTE THAT WOULD CROSS THE CAP: libfetch refuses
                // a body at max_body on the read that would pass it, and a
                // buffer that stopped at exactly the cap would take the
                // first 20 MiB of something longer for the whole picture.
                uint8_t probe;
                (void)os64_fetch_read(f, &probe, 1);
                break;
            }
            uint8_t *grown = os64_realloc(buf, want);
            if (grown == NULL) {
                os64_free(buf);
                return NULL;
            }
            buf = grown;
            size = want;
        }
        int64_t n = os64_fetch_read(f, buf + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
    }
    if (os64_fetch_status(f) != OS64_FETCH_OK) {
        os64_free(buf);
        return NULL;
    }
    *len = at;
    return buf;
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
        p->status = os64_image_load(j->url + 7, 0, &p->image);
        return p->status == OS64_IMAGE_OK ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = PICTURE_ACCEPT;
    opt.max_body = OS64_IMAGE_CAP_DEFAULT;
    opt.cancelled = cancelled;
    opt.ctx = ctx;
    os64_fetch_t *f = os64_fetch_open(j->url, &opt);
    p->status = OS64_IMAGE_IO_ERROR;
    if (f == NULL)
        return 0;
    size_t len = 0;
    uint8_t *bytes = os64_fetch_head(f) != NULL ? read_all(f, &len) : NULL;
    os64_fetch_close(f);
    if (bytes == NULL)
        return 0;
    p->status = cancelled(ctx) ? OS64_IMAGE_IO_ERROR : os64_image_decode(bytes, len, &p->image);
    os64_free(bytes);
    return p->status == OS64_IMAGE_OK ? 1 : 0;
}

void yonder_picture_release(void *job, void *product)
{
    yonder_picture_t *p = product;
    if (p != NULL) {
        os64_image_free(&p->image);
        os64_free(p);
    }
    os64_free(job);
}
