#ifndef YONDER_WEBFONT_H
#define YONDER_WEBFONT_H

// A page's font file on the work pool (PILE3.md § Web fonts): its bytes
// fetched, or read from a file, and turned on the worker into a plain SFNT
// — a WOFF unwrapped (font_unwrap.h) — so the window's thread, which owns
// the text context, only opens it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "font_unwrap.h"
#include "jobs.h"
#include "os64/font_backend.h"
#include "way/way.h"

// What one font job may cost, declared to the pool: the file, at most
// OS64_FONT_FILE_MAX, and the SFNT a WOFF unwraps to beside it.
#define WEBFONT_RESERVE ((size_t)OS64_FONT_FILE_MAX + YONDER_FONT_SFNT_MAX)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_FONT
    int32_t index;                  // which of the window's font files
    uint64_t generation;            // the page it belongs to
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    way_hooks_t hooks;              // cookies, the Referer, the cache: as a picture's
} yonder_webfont_job_t;

// The file as a plain SFNT, or why there is none. `container` is what the
// bytes were (an SFNT, a WOFF, a WOFF2, or neither) when any came.
typedef struct {
    bool ok;
    uint8_t *sfnt;                  // os64_malloc'd; the release frees it
    size_t len;
    yonder_font_kind_t container;
    bool fetched;                   // bytes came, whatever they were
    // Why there is no SFNT, for the page's record: "HTTP 404", "did not
    // fetch (tls-failed)", "an empty reply", "could not be read", "a WOFF2,
    // not read yet", "not a font", "a broken WOFF (malformed container)".
    char why[64];
} yonder_webfont_t;

int64_t yonder_webfont_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_webfont_release(void *job, void *product);

#endif
