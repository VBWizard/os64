#ifndef YONDER_SCRIPT_JOB_H
#define YONDER_SCRIPT_JOB_H

// A `<script src>`'s job on the work pool (DOM_D7.md § The stream's turn):
// the source fetched, or read from a file, and decoded to UTF-8 on a worker,
// so the window only ever holds text the engine can take.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "jobs.h"
#include "way/cache.h"
#include "way/way.h"

// The longest script source yonder runs, the libjs source limit a page's
// runtime is created with. A longer one is refused, as a too-big sheet is.
#define SCRIPT_SOURCE_MAX ((size_t)4 * 1024 * 1024)
// The largest compiled form kept for a script, or read back
// (BYTECODE_CACHE.md): a classic script's bytecode measures about three
// times its source, so four times the largest source holds any script
// yonder runs, and one past it is not kept.
#define SCRIPT_BYTECODE_MAX (SCRIPT_SOURCE_MAX * 4)
// A job's peak: the body, and the decoded text (up to three bytes a byte
// for windows-1252), with a connection's worth of room beside them, and
// the compiled form read beside its entry.
#define SCRIPT_RESERVE (SCRIPT_SOURCE_MAX * 4 + ((size_t)4 << 20) + SCRIPT_BYTECODE_MAX)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_SCRIPT
    uint64_t page;                  // the page it belongs to (the script host's serial)
    uint32_t token;                 // which of that page's scripts
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    // What a source that names no encoding of its own is read in: the
    // element's charset attribute, else the document's (HTML's "fetch a
    // classic script").
    char fallback[32];
    way_hooks_t hooks;              // the page's cookies, its Referer, who asks, the cache
} yonder_script_job_t;

// A script's compiled form and where it is kept (BYTECODE_CACHE.md): made
// for a script whose body the cache holds, and only then. The blob is
// libjs's, read beside the cache's entry, NULL when none was there for
// this text; a compile's blob is kept beside the same entry, under the
// same tag. The entry is the one way_fetch_whole kept the body under: the
// page's partition, credentialed, the address asked for.
typedef struct {
    uint8_t *blob;
    size_t length;
    char partition[WAY_PARTITION_MAX];  // the entry's key: way_partition_of the page
    char url[OS64_FETCH_URL_MAX];       // and the address asked for
    char tag[WAY_COMPANION_TAG_MAX];    // yonder_script_bytecode_tag's
} yonder_bytecode_t;

void yonder_bytecode_free(yonder_bytecode_t *bytecode);

typedef struct {
    bool ok;
    char *source;                   // UTF-8, NUL-terminated
    size_t length;
    char url[OS64_FETCH_URL_MAX];   // after its redirects
    yonder_bytecode_t *bytecode;    // NULL when the cache does not hold the body
} yonder_script_t;

int64_t yonder_script_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_script_release(void *job, void *product);

// The tag a script's compiled form is kept under: which text it is the
// compile of (its FNV-1a and length), so a blob is offered only for the
// very text it came from, whatever became of its entry between. Pure.
void yonder_script_bytecode_tag(const char *source, size_t length, char *out, size_t cap);

// A compiled form on its way to the disk, on a job of its own: the window
// never waits on a write. The job owns the blob.
typedef struct {
    uint32_t kind;                  // YONDER_JOB_BYTECODE
    way_cache_t *cache;
    uint8_t *blob;
    size_t length;
    char partition[WAY_PARTITION_MAX];
    char url[OS64_FETCH_URL_MAX];
    char tag[WAY_COMPANION_TAG_MAX];
} yonder_bytecode_job_t;

// The write's own peak: the companion's head. The blob is the job's input.
#define BYTECODE_KEEP_RESERVE ((size_t)64 << 10)

int64_t yonder_bytecode_keep_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_bytecode_keep_release(void *job, void *product);

// The decoding rule, pure for the host's tests. A UTF-8 byte order mark
// wins; then the response's label, else the fallback; with neither, text
// that is valid UTF-8 is read as UTF-8. UTF-8 has its invalid sequences
// replaced with U+FFFD; everything else is read as windows-1252, the only
// other encoding the old web's scripts are written in. NULL on no memory.
char *yonder_script_decode(const uint8_t *bytes, size_t length, const char *label,
                           const char *fallback, size_t *out_length);

#endif
