#ifndef YONDER_BYTECODE_H
#define YONDER_BYTECODE_H

// A script's compiled form beside its cache entry (BYTECODE_CACHE.md): the
// record a worker reads it into, the tags it is kept under, and the job
// that writes one. A classic script's arrives by its script job
// (script_job.h), a module's by its fetch job (fetch_job.h).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "way/cache.h"
#include "way/origin.h"

// The longest script source yonder runs, the libjs source limit a page's
// runtime is created with. A longer one is refused, as a too-big sheet is.
#define SCRIPT_SOURCE_MAX ((size_t)4 * 1024 * 1024)
// The largest compiled form kept for a script, or read back: a script's
// bytecode measures about three times its source, so four times the
// largest source holds any script yonder runs, and one past it is not kept.
#define SCRIPT_BYTECODE_MAX (SCRIPT_SOURCE_MAX * 4)

// A script's compiled form and where it is kept: made for a script whose
// body the cache holds, and only then. The blob is libjs's, read beside
// the cache's entry, NULL when none was there for this text; a compile's
// blob is kept beside the same entry, under the same tag.
typedef struct {
    uint8_t *blob;
    size_t length;
    char partition[WAY_PARTITION_MAX];  // the entry's key: way_partition_of the page,
    bool credentials;                   // whether its request sent them,
    char url[OS64_FETCH_URL_MAX];       // and the address asked for
    char tag[WAY_COMPANION_TAG_MAX];    // a yonder_*_bytecode_tag
} yonder_bytecode_t;

void yonder_bytecode_free(yonder_bytecode_t *bytecode);

// The tag a script's compiled form is kept under: which text it is the
// compile of (its FNV-1a and length), so a blob is offered only for the
// very text it came from, whatever became of its entry between. A classic
// script's is of its decoded text; a module's is of its body, which is
// always read as UTF-8 (modules.h), so the body names the text. The kinds
// tag apart, so an address fetched as both keeps both. Pure.
void yonder_script_bytecode_tag(const char *source, size_t length, char *out, size_t cap);
void yonder_module_bytecode_tag(const uint8_t *body, size_t length, char *out, size_t cap);

// A compiled form on its way to the disk, on a job of its own: the window
// never waits on a write. The job owns the blob.
typedef struct {
    uint32_t kind;                  // YONDER_JOB_BYTECODE
    way_cache_t *cache;
    uint8_t *blob;
    size_t length;
    char partition[WAY_PARTITION_MAX];
    bool credentials;
    char url[OS64_FETCH_URL_MAX];
    char tag[WAY_COMPANION_TAG_MAX];
} yonder_bytecode_job_t;

// The write's own peak: the companion's head. The blob is the job's input.
#define BYTECODE_KEEP_RESERVE ((size_t)64 << 10)

int64_t yonder_bytecode_keep_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_bytecode_keep_release(void *job, void *product);

#endif
