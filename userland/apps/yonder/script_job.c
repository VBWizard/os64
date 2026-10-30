// script_job.c — a script's source fetched and decoded on a worker
// (script_job.h).

#include "script_job.h"
#include "os64/fmt.h"
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
    // A body the cache holds may have its compiled form beside it, and a
    // compile of it is kept there (BYTECODE_CACHE.md § 5); one it does not
    // hold has neither. Without memory for the record, it runs as source.
    // Its entry is way_fetch_whole's: the page's partition, credentialed,
    // the address asked for.
    if (s->ok && body.kept && !cancelled(ctx) && (s->bytecode = os64_calloc(1, sizeof(*s->bytecode))) != NULL) {
        yonder_bytecode_t *bc = s->bytecode;
        way_partition_of(&j->hooks.metadata.origin, bc->partition, sizeof(bc->partition));
        os64_strcopy(bc->url, sizeof(bc->url), j->url);
        yonder_script_bytecode_tag(s->source, s->length, bc->tag, sizeof(bc->tag));
        const way_cache_key_t key = {bc->partition, true, bc->url};
        (void)way_cache_companion_get(j->hooks.cache, &key, bc->tag, &bc->blob, &bc->length, SCRIPT_BYTECODE_MAX);
    }
    return s->ok ? 1 : 0;
}

void yonder_bytecode_free(yonder_bytecode_t *bytecode)
{
    if (bytecode != NULL) {
        os64_free(bytecode->blob);
        os64_free(bytecode);
    }
}

void yonder_script_release(void *job, void *product)
{
    yonder_script_t *s = product;
    if (s != NULL) {
        os64_free(s->source);
        yonder_bytecode_free(s->bytecode);
        os64_free(s);
    }
    os64_free(job);
}

void yonder_script_bytecode_tag(const char *source, size_t length, char *out, size_t cap)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++) {
        h ^= (uint8_t)source[i];
        h *= 0x100000001b3ull;
    }
    os64_snprintf(out, cap, "classic %016lx %lu", (unsigned long)h, (unsigned long)length);
}

int64_t yonder_bytecode_keep_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    (void)out;
    yonder_bytecode_job_t *j = job;
    if (cancelled(ctx))
        return 0;
    const way_cache_key_t key = {j->partition, true, j->url};
    return way_cache_companion_put(j->cache, &key, j->tag, j->blob, j->length) ? 1 : 0;
}

void yonder_bytecode_keep_release(void *job, void *product)
{
    (void)product;
    yonder_bytecode_job_t *j = job;
    if (j != NULL)
        os64_free(j->blob);
    os64_free(j);
}
