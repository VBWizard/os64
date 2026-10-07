#ifndef YONDER_MAIL_H
#define YONDER_MAIL_H

// What passes between a navigation's job and the window (YONDER.md § Y3):
// the newest progress sentence, a question, and its answer — and THE
// STREAM (docs/design/pending/DOM_D4.md): the page's head, its body in
// chunks, and the fetch's verdict, in that order, because the parser lives
// on the window's thread and the worker carries only bytes.
//
// THE MAILBOX OUTLIVES WHOEVER LETS GO OF IT LAST. The job cannot own it —
// cancelling a job hands it to the pool's release, which may free it at
// once — so it is its own object with a reference count: the window holds
// one while the navigation is current, the job one until its release, and
// the last to drop frees it and closes the answer pipe. A lock protects
// what is inside; the count protects that there is an inside.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "way/way.h"

typedef struct yonder_mail yonder_mail_t;

// The ring the body travels through: fixed storage made with the mailbox,
// so a post never allocates and the only thing that ends one unposted is
// cancellation. A full ring makes the worker wait, which stops it reading
// the socket — backpressure lands on the wire, not in a growing buffer.
#define YONDER_STREAM_CHUNK (16u * 1024u)
#define YONDER_STREAM_CHUNKS 16

// How a fetch ended. `page` false means there never was one: `reason` is
// then the sentence to show (libway's), and no head was posted.
typedef struct {
    bool page;
    os64_fetch_status_t fetch;
    char reason[WAY_SENTENCE_MAX];
} yonder_verdict_t;

// A mailbox for navigation `generation`, held once by the caller. NULL on
// no memory or no pipe.
yonder_mail_t *yonder_mail_new(uint64_t generation);
void yonder_mail_hold(yonder_mail_t *mail);
void yonder_mail_drop(yonder_mail_t *mail);
uint64_t yonder_mail_generation(const yonder_mail_t *mail);

// The job's side. A progress sentence replaces any the window has not read:
// a person wants the newest, not every one.
void yonder_mail_progress(yonder_mail_t *mail, const char *sentence);
// Publishes a question and returns its number, then the job waits for the
// answer to THAT number. The wait asks `cancelled` between 100 ms steps, so
// a cancel ends it whether anyone answers or not; cancelled is No.
uint32_t yonder_mail_ask(yonder_mail_t *mail, const char *question);
bool yonder_mail_wait(yonder_mail_t *mail, uint32_t number, bool (*cancelled)(void *ctx),
                      void *ctx);

// The stream, the job's side. The head once, then the body as it is read,
// then the verdict once. A post waits for room in 100 ms steps, asking
// `cancelled` between them and waking early when the window makes room;
// false means it was cancelled with the bytes unposted.
void yonder_mail_post_head(yonder_mail_t *mail, const way_head_t *head);
bool yonder_mail_post(yonder_mail_t *mail, const void *bytes, size_t len,
                      bool (*cancelled)(void *ctx), void *ctx);
void yonder_mail_post_verdict(yonder_mail_t *mail, const yonder_verdict_t *verdict);

// The window's side. Each takes what is new and marks it read.
bool yonder_mail_take_progress(yonder_mail_t *mail, char *out, size_t cap);
bool yonder_mail_take_question(yonder_mail_t *mail, uint32_t *number, char *out, size_t cap);
// Answers question `number`. An answer to any other number is not an answer
// to the question the job is waiting on, and it keeps waiting.
void yonder_mail_answer(yonder_mail_t *mail, uint32_t number, bool yes);

// The stream, the window's side; none of these waits. `take` copies out one
// chunk, at most YONDER_STREAM_CHUNK bytes, and answers 0 when none is
// waiting (or `cap` is smaller than a chunk). `streaming` says whether
// anything — head, chunk or verdict — is waiting to be taken.
bool yonder_mail_take_head(yonder_mail_t *mail, way_head_t *out);
size_t yonder_mail_take(yonder_mail_t *mail, void *buf, size_t cap);
bool yonder_mail_take_verdict(yonder_mail_t *mail, yonder_verdict_t *out);
bool yonder_mail_streaming(yonder_mail_t *mail);

#endif
