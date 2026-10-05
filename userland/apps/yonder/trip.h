#ifndef YONDER_TRIP_H
#define YONDER_TRIP_H

// A navigation's job on the work pool (YONDER.md § Y3, DOM_D4.md): the
// FETCH, on a worker, with a face that reaches the window only through the
// mailbox. The page's head, its body in chunks and the fetch's verdict go
// down the same mailbox; the window parses them on its own thread, because
// the thread that parses a page is the thread that will run its scripts
// (DOM.md ruling 5). The job has no product: everything it has to say
// travels before it returns.

#include "jobs.h"
#include "mail.h"
#include "way/way.h"

// The job's input. Owned by the pool from submit until release.
typedef struct {
    uint32_t kind;                  // YONDER_JOB_TRIP
    yonder_mail_t *mail;            // one reference, the job's
    const way_session_t *session;   // the browser's identity, read-only
    // The agent to send, read on the window's thread when the job was
    // made: Settings may change the session's while the job runs.
    const char *agent;
    int64_t window;                 // rung when there is mail
    uint32_t mail_bell;
    char url[OS64_FETCH_URL_MAX];
    // A form being sent: the job's own copy, so a POST's body lives exactly
    // as long as the job that sends it. (The window keeps its own copy for
    // the page that arrives, whose arrival no longer coincides with this
    // job's end.)
    os64_page_request_t request;
    bool has_request;
    char referrer[OS64_FETCH_URL_MAX];      // the page it was asked for from, or ""
} yonder_trip_t;

// 1 when a page's bytes all went down the mailbox, 0 when there was no page
// (the verdict says why), -1 when cancelled part way.
int64_t yonder_trip_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
// Frees the job: the pool calls it for a job it cancelled, the window for
// one it reaped. There is no product.
void yonder_trip_release(void *job, void *product);

#endif
