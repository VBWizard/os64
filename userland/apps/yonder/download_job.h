#ifndef YONDER_DOWNLOAD_JOB_H
#define YONDER_DOWNLOAD_JOB_H

// A download's job on the work pool (YONDER_HONEY_DO.md § 6): the stream
// libfetch runs for pages, aimed at a file. The bytes go to `<path>.part`
// and are renamed to `path` when the last one is written, so a name that
// exists on disk is always a whole file; a failure or a cancellation
// removes the `.part` and keeps nothing.

#include <stdbool.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "jobs.h"
#include "os64/io.h"
#include "way/way.h"

// The read size, and with libfetch's own a job's peak: the stream is never
// held whole, so a download's cost does not grow with its size.
#define YONDER_DOWNLOAD_CHUNK ((size_t)64 * 1024)
#define YONDER_DOWNLOAD_RESERVE ((size_t)8 << 20)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_DOWNLOAD
    const char *agent;              // the browser's, read-only for the run
    // The browser's cookies, the asking page's address for the Referer,
    // the agent's preset lines and a navigation's Fetch Metadata: a
    // download is asked for as following the link would ask.
    way_hooks_t hooks;
    int64_t window;                 // rung as the bytes arrive
    uint32_t bell;
    char url[OS64_FETCH_URL_MAX];
    char path[OS64_PATH_MAX];
    char part[OS64_PATH_MAX];       // `path` and ".part"
    // The person agreed to replace a file named `path` (the Save dialog's
    // answer): without it the rename refuses one that has appeared since.
    bool replace;
    // WHILE IT RUNS the worker writes these and the window reads them,
    // each a whole 64-bit word (__atomic): the bytes written so far, and
    // how many there will be, 0 when the server did not say.
    uint64_t got, total;
    // When it is reaped: whether the file is whole under its name, and if
    // not, why, for the downloads list.
    bool whole;
    char why[160];
} yonder_download_job_t;

// 1 when the file is whole under its name, 0 when it failed (`why` says
// why), -1 when cancelled. Nothing is left under `part` in either of the
// last two cases.
int64_t yonder_download_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
// Frees the job: the pool calls it for a job it cancelled, the window for
// one it reaped.
void yonder_download_release(void *job, void *product);

#endif
