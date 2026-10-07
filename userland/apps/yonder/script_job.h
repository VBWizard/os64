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
#include "way/way.h"

// The longest script source yonder runs, the libjs source limit a page's
// runtime is created with. A longer one is refused, as a too-big sheet is.
#define SCRIPT_SOURCE_MAX ((size_t)4 * 1024 * 1024)
// A job's peak: the body, and the decoded text (up to three bytes a byte
// for windows-1252), with a connection's worth of room beside them.
#define SCRIPT_RESERVE (SCRIPT_SOURCE_MAX * 4 + ((size_t)4 << 20))

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
    way_hooks_t hooks;              // the page's cookies, its Referer, the cache
} yonder_script_job_t;

typedef struct {
    bool ok;
    char *source;                   // UTF-8, NUL-terminated
    size_t length;
    char url[OS64_FETCH_URL_MAX];   // after its redirects
} yonder_script_t;

int64_t yonder_script_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_script_release(void *job, void *product);

// The decoding rule, pure for the host's tests. A UTF-8 byte order mark
// wins; then the response's label, else the fallback; with neither, text
// that is valid UTF-8 is read as UTF-8. UTF-8 has its invalid sequences
// replaced with U+FFFD; everything else is read as windows-1252, the only
// other encoding the old web's scripts are written in. NULL on no memory.
char *yonder_script_decode(const uint8_t *bytes, size_t length, const char *label,
                           const char *fallback, size_t *out_length);

#endif
