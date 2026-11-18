// script_job.c — a script's source fetched and decoded on a worker
// (script_job.h).

#include "script_job.h"
#include "dom/dom.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "way/origin.h"

#define SCRIPT_ACCEPT "*/*"

int64_t yonder_script_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_script_job_t *j = job;
    yonder_script_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    *out = s;
    os64_strcopy(s->url, sizeof(s->url), j->url);
    // A blob: address's script is its entry's bytes, read in place, its
    // label the Blob type's charset as a response's is its Content-Type's,
    // and no compiled form kept (it has no cache entry to keep one beside).
    if (j->blob != NULL) {
        size_t len = os64_dom_blob_length(j->blob);
        if (len > SCRIPT_SOURCE_MAX)
            return 0;
        const char *type = os64_dom_blob_type(j->blob);
        char essence[WAY_MIME_ESSENCE_MAX], charset[WAY_MIME_CHARSET_MAX];
        bool labelled = way_mime_parse(type, os64_strlen(type), essence, sizeof(essence), charset, sizeof(charset)) &&
                        charset[0] != '\0';
        s->source = yonder_script_decode(os64_dom_blob_bytes(j->blob), len, labelled ? charset : NULL, j->fallback,
                                         &s->length);
        s->ok = s->source != NULL;
        return s->ok ? 1 : 0;
    }
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
    // A body the cache holds may have its compiled form beside it, and a
    // compile of it is kept there (BYTECODE_CACHE.md § 5); one it does not
    // hold has neither. Without memory for the record, it runs as source.
    // Its entry is way_fetch_whole's: the page's partition, credentialed,
    // the address asked for.
    if (s->ok && body.kept && !cancelled(ctx) && (s->bytecode = os64_calloc(1, sizeof(*s->bytecode))) != NULL) {
        yonder_bytecode_t *bc = s->bytecode;
        way_partition_of(&j->hooks.metadata.origin, bc->partition, sizeof(bc->partition));
        bc->credentials = true;
        os64_strcopy(bc->url, sizeof(bc->url), j->url);
        yonder_script_bytecode_tag(s->source, s->length, bc->tag, sizeof(bc->tag));
        const way_cache_key_t key = {bc->partition, bc->credentials, bc->url};
        (void)way_cache_companion_get(j->hooks.cache, &key, bc->tag, &bc->blob, &bc->length, SCRIPT_BYTECODE_MAX);
    }
    return s->ok ? 1 : 0;
}

void yonder_script_release(void *job, void *product)
{
    yonder_script_t *s = product;
    if (s != NULL) {
        os64_free(s->source);
        yonder_bytecode_free(s->bytecode);
        os64_free(s);
    }
    if (job != NULL)
        os64_dom_blob_release(((yonder_script_job_t *)job)->blob);
    os64_free(job);
}
