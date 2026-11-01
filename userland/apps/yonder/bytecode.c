// bytecode.c — a script's compiled form beside its cache entry
// (bytecode.h).

#include "bytecode.h"
#include "os64/fmt.h"
#include "os64/mem.h"

void yonder_bytecode_free(yonder_bytecode_t *bytecode)
{
    if (bytecode != NULL) {
        os64_free(bytecode->blob);
        os64_free(bytecode);
    }
}

static void tag(const char *kind, const uint8_t *bytes, size_t length, char *out, size_t cap)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++) {
        h ^= bytes[i];
        h *= 0x100000001b3ull;
    }
    os64_snprintf(out, cap, "%s %016lx %lu", kind, (unsigned long)h, (unsigned long)length);
}

void yonder_script_bytecode_tag(const char *source, size_t length, char *out, size_t cap)
{
    tag("classic", (const uint8_t *)source, length, out, cap);
}

void yonder_module_bytecode_tag(const uint8_t *body, size_t length, char *out, size_t cap)
{
    tag("module", body, length, out, cap);
}

int64_t yonder_bytecode_keep_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    (void)out;
    yonder_bytecode_job_t *j = job;
    if (cancelled(ctx))
        return 0;
    const way_cache_key_t key = {j->partition, j->credentials, j->url};
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
