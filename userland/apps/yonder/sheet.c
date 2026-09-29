// sheet.c — a style sheet fetched and parsed on a worker (sheet.h).

#include "sheet.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

#define SHEET_ACCEPT "text/css,*/*;q=0.1"

// The body, whole, up to libgarb's cap. NULL on no memory, when the fetch
// did not deliver all of it, or when there is more than the cap. The buffer
// may hold one byte past the cap, so a body of exactly the cap is read to
// its end and kept, and one byte more is found and refused.
static uint8_t *read_all(os64_fetch_t *f, size_t *len)
{
    const size_t limit = GARB_SHEET_MAX + 1;
    size_t at = 0, size = 16u * 1024u;
    uint8_t *buf = os64_malloc(size);
    if (buf == NULL)
        return NULL;
    for (;;) {
        if (at == size) {
            if (size >= limit) {
                os64_free(buf);
                return NULL;
            }
            size_t want = size * 2 > limit ? limit : size * 2;
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
    if (os64_fetch_status(f) != OS64_FETCH_OK || at > GARB_SHEET_MAX) {
        os64_free(buf);
        return NULL;
    }
    *len = at;
    return buf;
}

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
    opt.max_body = GARB_SHEET_MAX;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_fetch_hooks(&j->hooks, &opt);
    os64_fetch_t *f = os64_fetch_open(j->url, &opt);
    if (f == NULL)
        return 0;
    const os64_fetch_head_t *head = os64_fetch_head(f);
    uint8_t *bytes = NULL;
    size_t len = 0;
    char charset[sizeof(head->charset)] = "";
    if (head != NULL && head->status >= 200 && head->status < 300 &&
        (j->any_type || os64_streq(head->content_type, "text/css"))) {
        os64_strcopy(s->url, sizeof(s->url), head->url_text);
        os64_strcopy(charset, sizeof(charset), head->charset);
        bytes = read_all(f, &len);
    }
    os64_fetch_close(f);
    if (bytes == NULL || cancelled(ctx)) {
        os64_free(bytes);
        return 0;
    }
    s->ok = garb_parse_sheet(bytes, len, charset[0] != '\0' ? charset : NULL, environment,
                             &s->parsed) == GARB_OK;
    os64_free(bytes);
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
