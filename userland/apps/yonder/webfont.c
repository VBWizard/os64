// webfont.c — a page's font file fetched and made a plain SFNT on a worker
// (webfont.h).

#include "webfont.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

// The containers read, then anything: a font is often served as
// application/octet-stream, and the bytes are judged by their signature.
#define WEBFONT_ACCEPT "font/ttf, font/otf, font/woff, */*;q=0.5"
_Static_assert(sizeof(WEBFONT_ACCEPT) <= OS64_FETCH_ACCEPT_MAX,
               "libfetch refuses an Accept longer than OS64_FETCH_ACCEPT_MAX");

// The bytes as an SFNT, by their signature: kept as they are, unwrapped,
// or refused with the reason. The caller keeps `bytes`.
static void make_sfnt(const uint8_t *bytes, size_t len, yonder_webfont_t *f)
{
    f->fetched = true;
    f->container = yonder_font_sniff(bytes, len);
    if (f->container == YONDER_FONT_SFNT) {
        f->sfnt = os64_malloc(len);
        if (f->sfnt == NULL) {
            os64_strcopy(f->why, sizeof(f->why), "no memory to keep it");
            return;
        }
        os64_memcpy(f->sfnt, bytes, len);
        f->len = len;
        f->ok = true;
    } else if (f->container == YONDER_FONT_WOFF) {
        yonder_font_status_t st = yonder_font_unwrap_woff(bytes, len, &f->sfnt, &f->len);
        if (st == YONDER_FONT_OK)
            f->ok = true;
        else
            os64_snprintf(f->why, sizeof(f->why), "a broken WOFF (%s)", yonder_font_status_name(st));
    } else if (f->container == YONDER_FONT_WOFF2) {
        os64_strcopy(f->why, sizeof(f->why), "a WOFF2, not read yet");
    } else {
        os64_strcopy(f->why, sizeof(f->why), "not a font");
    }
}

int64_t yonder_webfont_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_webfont_job_t *j = job;
    yonder_webfont_t *f = os64_calloc(1, sizeof(*f));
    if (f == NULL)
        return -1;
    *out = f;
    f->container = YONDER_FONT_UNKNOWN;
    // A font on a page read from disk is a file beside it.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        size_t len = 0;
        uint8_t *bytes = NULL;
        if (os64_slurp(j->url + 7, OS64_FONT_FILE_MAX, &bytes, &len) == OS64_SLURP_OK && len > 0) {
            make_sfnt(bytes, len, f);
            os64_free(bytes);
        } else {
            os64_free(bytes);
            os64_strcopy(f->why, sizeof(f->why), "could not be read");
        }
        return f->ok ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = WEBFONT_ACCEPT;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_whole_t body;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, OS64_FONT_FILE_MAX, &body)) {
        os64_snprintf(f->why, sizeof(f->why), "did not fetch (%s)", os64_fetch_status_name(body.fetch));
        return 0;
    }
    // An error's body is the server's page about the error, not the font.
    if (cancelled(ctx))
        os64_strcopy(f->why, sizeof(f->why), "cancelled");
    else if (body.status != 0 && (body.status < 200 || body.status >= 300))
        os64_snprintf(f->why, sizeof(f->why), "HTTP %d", (int)body.status);
    else if (body.len == 0)
        os64_strcopy(f->why, sizeof(f->why), "an empty reply");
    else
        make_sfnt(body.bytes, body.len, f);
    os64_free(body.bytes);
    return f->ok ? 1 : 0;
}

void yonder_webfont_release(void *job, void *product)
{
    yonder_webfont_t *f = product;
    if (f != NULL) {
        os64_free(f->sfnt);
        os64_free(f);
    }
    os64_free(job);
}
