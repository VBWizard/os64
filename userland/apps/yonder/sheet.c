// sheet.c — a style sheet fetched and parsed on a worker (sheet.h).

#include "sheet.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

#define SHEET_ACCEPT "text/css,*/*;q=0.1"

int64_t yonder_sheet_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_sheet_job_t *j = job;
    yonder_sheet_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    *out = s;
    os64_strcopy(s->url, sizeof(s->url), j->url);
    const char *environment = j->environment[0] != '\0' ? j->environment : NULL;
    // A sheet beside a page read from disk is a file; it has no type to
    // check and no charset but its own.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(j->url + 7, GARB_SHEET_MAX, &bytes, &len) != OS64_SLURP_OK)
            return 0;
        s->ok = garb_parse_sheet(bytes, len, NULL, environment, &s->parsed) == GARB_OK;
        os64_free(bytes);
        return s->ok ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = SHEET_ACCEPT;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    // A body of exactly the cap is read to its end and kept; a longer one
    // is refused by the fetch before the byte that crosses it.
    way_whole_t body;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, GARB_SHEET_MAX, &body))
        return 0;
    bool usable = body.status >= 200 && body.status < 300 &&
                  (j->any_type || os64_streq(body.content_type, "text/css"));
    if (!usable || cancelled(ctx)) {
        os64_free(body.bytes);
        return 0;
    }
    os64_strcopy(s->url, sizeof(s->url), body.url);
    s->ok = garb_parse_sheet(body.bytes, body.len, body.charset[0] != '\0' ? body.charset : NULL,
                             environment, &s->parsed) == GARB_OK;
    os64_free(body.bytes);
    return s->ok ? 1 : 0;
}

void yonder_sheet_release(void *job, void *product)
{
    yonder_sheet_t *s = product;
    if (s != NULL) {
        garb_free(&s->parsed);
        os64_free(s);
    }
    os64_free(job);
}
