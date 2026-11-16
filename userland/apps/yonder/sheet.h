#ifndef YONDER_SHEET_H
#define YONDER_SHEET_H

// A style sheet's job on the work pool (GARB.md, G4): a `<link>`ed sheet or
// an `@import` fetched or read from a file and parsed, or a fetch-only
// style preload retained until a stylesheet consumes it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "garb/garb.h"
#include "jobs.h"
#include "way/way.h"

// What one sheet job may cost at its peak, declared to the pool (GARB.md
// § The cost, measured): outside the parse's arena, the body, the
// tokenizer's code points (4 bytes a byte) and the decoded text (up to 3),
// eight bytes a byte of GARB_SHEET_MAX in all; inside it, at most
// GARB_ARENA_MAX, which Bootstrap-shaped CSS reaches at about 6 MiB. 160 MiB, so
// four sheets and a page's fetch fit the pool's budget at once.
#define SHEET_RESERVE ((size_t)GARB_ARENA_MAX + 8 * (size_t)GARB_SHEET_MAX)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_SHEET
    uint64_t page;                  // stylesheet table serial, or stable preload account ID
    int32_t index;                  // stylesheet or preload entry within that owner
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    // The document's encoding: what a sheet that names none of its own is
    // read in (CSS Syntax 3 § 3.2).
    char environment[32];
    // Quirks mode permits another MIME type only for a same-origin final
    // response. A redirect does not inherit the initial URL's exemption.
    bool quirks;
    way_request_t request;          // link origin, CORS, credentials and referrer policy
    // Fetch-only preloads retain their outcome without parsing or applying it.
    bool preload;
    // An activated preload supplies its retained response to this parse job.
    // The job owns this copy; input_present also represents an empty body.
    bool input_present, input_fetched, input_usable;
    uint8_t *input;
    size_t input_len;
    char input_label[HTTP_CHARSET_MAX];
    way_hooks_t hooks;              // the page's cookies, its Referer, who asks, the cache
    // The window keeps the page (keep_pages, diag.h): the product carries
    // what the sheet was parsed from.
    bool keep;
} yonder_sheet_job_t;

typedef struct {
    bool ok;                       // parsed CSS, or a completed fetch for a preload
    bool fetched, usable;          // fetch success, then status/MIME eligibility
    garb_parsed_t parsed;
    // Where the sheet came from after its redirects: its @imports are
    // resolved against it.
    char url[OS64_FETCH_URL_MAX];
    // With `keep` after parsing, or for a preload, the response bytes and the
    // charset its response labelled them with ("" for none), so a consumer
    // uses the response encoding when parsing. os64_malloc'd; NULL otherwise.
    uint8_t *kept;
    size_t kept_len;
    char label[HTTP_CHARSET_MAX];
} yonder_sheet_t;

int64_t yonder_sheet_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_sheet_release(void *job, void *product);

#endif
