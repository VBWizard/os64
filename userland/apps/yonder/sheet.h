#ifndef YONDER_SHEET_H
#define YONDER_SHEET_H

// A style sheet's job on the work pool (GARB.md, G4): a `<link>`ed sheet or
// an `@import`, fetched, or read from a file, and parsed on a worker.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "garb/garb.h"
#include "jobs.h"
#include "way/way.h"

// What one sheet job may cost, declared to the pool: the largest sheet
// libgarb reads, and its parse, which runs to a few times the text.
#define SHEET_RESERVE (GARB_SHEET_MAX * 5)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_SHEET
    uint64_t page;                  // the page it belongs to (Page.serial)
    int32_t index;                  // which of that page's sheets
    const char *agent;              // the browser's, read-only for the run
    char url[OS64_FETCH_URL_MAX];
    // The document's encoding: what a sheet that names none of its own is
    // read in (CSS Syntax 3 § 3.2).
    char environment[32];
    // A sheet is read only when its server calls it `text/css` — except a
    // same-origin one on a quirks-mode page, which is read whatever type it
    // was given (HTML's `link` fetch).
    bool any_type;
    way_hooks_t hooks;              // the page's cookies and its Referer
} yonder_sheet_job_t;

typedef struct {
    bool ok;
    garb_parsed_t parsed;
    // Where the sheet came from after its redirects: its @imports are
    // resolved against it.
    char url[OS64_FETCH_URL_MAX];
} yonder_sheet_t;

int64_t yonder_sheet_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
void yonder_sheet_release(void *job, void *product);

#endif
