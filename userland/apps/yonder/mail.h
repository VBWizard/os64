#ifndef YONDER_MAIL_H
#define YONDER_MAIL_H

// What passes between a navigation's job and the window (YONDER.md § Y3):
// the newest progress sentence, a question, and its answer.
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

typedef struct yonder_mail yonder_mail_t;

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

// The window's side. Each takes what is new and marks it read.
bool yonder_mail_take_progress(yonder_mail_t *mail, char *out, size_t cap);
bool yonder_mail_take_question(yonder_mail_t *mail, uint32_t *number, char *out, size_t cap);
// Answers question `number`. An answer to any other number is not an answer
// to the question the job is waiting on, and it keeps waiting.
void yonder_mail_answer(yonder_mail_t *mail, uint32_t number, bool yes);

#endif
