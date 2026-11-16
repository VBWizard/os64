// sheet.c — stylesheet response acquisition and parsing on a worker.

#include "sheet.h"
#include "fetch_job.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

// Preloading ends at acquisition. Activation parses the same response bytes;
// a fetch's load event does not promise that the response is usable as CSS.
static void sheet_body(const yonder_sheet_job_t *j, yonder_sheet_t *s, uint8_t *bytes, size_t len)
{
    if (j->preload) {
        s->ok = s->fetched;
    } else if (s->usable) {
        s->ok = garb_parse_sheet(bytes != NULL ? bytes : (const uint8_t *)"", len, s->label[0] ? s->label : NULL,
                                 j->environment[0] ? j->environment : NULL, &s->parsed) == GARB_OK;
    }
    if (j->preload || (j->keep && s->ok)) {
        s->kept = bytes;
        s->kept_len = len;
    } else {
        os64_free(bytes);
    }
}

int64_t yonder_sheet_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_sheet_job_t *j = job;
    yonder_sheet_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    *out = s;
    os64_strcopy(s->url, sizeof(s->url), j->url);
    if (cancelled != NULL && cancelled(ctx))
        return 0;
    if (j->input_present) {
        s->fetched = j->input_fetched;
        s->usable = j->input_usable;
        os64_strcopy(s->label, sizeof(s->label), j->input_label);
        uint8_t *bytes = j->input;
        j->input = NULL;
        sheet_body(j, s, bytes, j->input_len);
        return s->ok ? 1 : 0;
    }
    // A stylesheet beside a local document has no response MIME or charset.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        if (os64_strlen(j->hooks.referrer) <= 7 || os64_memcmp(j->hooks.referrer, "file://", 7) != 0)
            return 0;
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(j->url + 7, GARB_SHEET_MAX, &bytes, &len) != OS64_SLURP_OK)
            return 0;
        s->fetched = s->usable = true;
        sheet_body(j, s, bytes, len);
        return s->ok ? 1 : 0;
    }
    yonder_fetch_job_t *fetch = os64_calloc(1, sizeof(*fetch));
    if (fetch == NULL)
        return 0;
    // The nested request borrows URL and policy from the worker job; free its
    // structure directly, not with the owning script-fetch release function.
    fetch->url = j->url;
    fetch->agent = j->agent;
    fetch->identity = j->hooks.identity;
    fetch->request = j->request;
    fetch->max_body = GARB_SHEET_MAX;
    fetch->jar = j->hooks.jar;
    fetch->cache = j->hooks.cache;
    os64_strcopy(fetch->referrer, sizeof(fetch->referrer), j->hooks.referrer);
    void *product = NULL;
    yonder_fetch_style_run(fetch, cancelled, ctx, &product);
    os64_free(fetch);
    yonder_fetched_t *got = product;
    if (got == NULL)
        return 0;
    j->hooks.cookies_short = got->cookies_short;
    s->fetched = got->ok;
    if (got->ok) {
        os64_strcopy(s->url, sizeof(s->url), got->url);
        char mime[WAY_MIME_SERIALIZED_MAX] = "";
        char *types = os64_malloc(got->headers_len + 1);
        if (types == NULL) {
            yonder_fetched_free(got);
            s->fetched = false;
            return 0;
        }
        size_t at = 0, ntypes = 0;
        way_header_t h;
        while (way_header_next(got->headers, got->headers_len, &at, &h)) {
            bool named = h.name_len == 12;
            for (size_t i = 0; named && i < 12; i++)
                named = (h.name[i] | 0x20) == "content-type"[i];
            if (!named)
                continue;
            if (ntypes != 0)
                types[ntypes++] = ',';
            os64_memcpy(types + ntypes, h.value, h.value_len);
            ntypes += h.value_len;
        }
        way_mime_extract(types, ntypes, mime, sizeof(mime), s->label, sizeof(s->label));
        os64_free(types);
        way_origin_t final;
        way_origin_of(s->url, 0, &final);
        bool same = way_origin_same(&j->request.origin, &final);
        s->usable = got->status >= 200 && got->status < 300 &&
                    (os64_streq(mime, "text/css") || (j->quirks && same));
        uint8_t *bytes = got->body;
        got->body = NULL;
        sheet_body(j, s, bytes, got->body_len);
    }
    yonder_fetched_free(got);
    return s->ok ? 1 : 0;
}

void yonder_sheet_release(void *job, void *product)
{
    yonder_sheet_t *s = product;
    if (s != NULL) {
        garb_free(&s->parsed);
        os64_free(s->kept);
        os64_free(s);
    }
    yonder_sheet_job_t *j = job;
    if (j != NULL)
        os64_free(j->input);
    os64_free(j);
}
