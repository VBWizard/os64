// script_job.c — a script's source fetched and decoded on a worker
// (script_job.h).

#include "script_job.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

#define SCRIPT_ACCEPT "*/*"

int64_t yonder_script_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_script_job_t *j = job;
    yonder_script_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    *out = s;
    os64_strcopy(s->url, sizeof(s->url), j->url);
    // A script beside a page read from disk is a file, with no label but
    // the fallback.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(j->url + 7, SCRIPT_SOURCE_MAX, &bytes, &len) != OS64_SLURP_OK)
            return 0;
        s->source = yonder_script_decode(bytes, len, NULL, j->fallback, &s->length);
        os64_free(bytes);
        s->ok = s->source != NULL;
        return s->ok ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = SCRIPT_ACCEPT;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_whole_t body;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, SCRIPT_SOURCE_MAX, &body))
        return 0;
    // A script is any 2xx body: the old web serves scripts as text/html,
    // text/plain and worse, and browsers run them all.
    bool usable = body.status >= 200 && body.status < 300;
    if (!usable || cancelled(ctx)) {
        os64_free(body.bytes);
        return 0;
    }
    os64_strcopy(s->url, sizeof(s->url), body.url);
    s->source = yonder_script_decode(body.bytes, body.len, body.charset, j->fallback, &s->length);
    os64_free(body.bytes);
    s->ok = s->source != NULL;
    return s->ok ? 1 : 0;
}

void yonder_script_release(void *job, void *product)
{
    yonder_script_t *s = product;
    if (s != NULL) {
        os64_free(s->source);
        os64_free(s);
    }
    os64_free(job);
}
