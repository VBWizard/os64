#ifndef YONDER_TRIP_H
#define YONDER_TRIP_H

// A navigation's job on the work pool (YONDER.md § Y3): way_load on a
// worker, with a face that reaches the window only through the mailbox.

#include "mail.h"
#include "way/way.h"

// The job's input. Owned by the pool from submit until release.
typedef struct {
    yonder_mail_t *mail;            // one reference, the job's
    const way_session_t *session;   // the browser's identity, read-only
    int64_t window;                 // rung when there is mail
    uint32_t mail_bell;
    char url[OS64_FETCH_URL_MAX];
    // A form being sent: MOVED in from the page's request, so a POST's body
    // lives exactly as long as the job that sends it.
    os64_page_request_t request;
    bool has_request;
} yonder_trip_t;

// The job's product: the page that arrived, or why none did.
typedef struct {
    bool loaded;
    way_page_t page;
    os64_fetch_status_t why;
    char status[WAY_SENTENCE_MAX];
} yonder_arrival_t;

int64_t yonder_trip_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
// Frees the job and its product: the pool calls it for a job it cancelled,
// the window for one it reaped.
void yonder_trip_release(void *job, void *product);

#endif
