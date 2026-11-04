#ifndef YONDER_FETCH_PIPE_H
#define YONDER_FETCH_PIPE_H

// A script's request's answer AS IT ARRIVES (STREAMS.md § 5, S3). The
// fetch job, on a pool worker, writes the head and then the body's bytes
// into the pipe as it reads them off the wire; the window reads them out
// and hands them to the page's binding (dom.h's os64_dom_fetch_head,
// _body, _end) as the page asks for them. The job's product (fetch_job.h)
// still says how the request ended, as it does for a whole answer.
//
// BACKPRESSURE: the writer holds off its next read off the wire while
// YONDER_FETCH_PIPE_WINDOW bytes wait unread, so a body nobody reads stops
// there. A body smaller than that never makes the job wait: it ends when
// the body is off the wire, read or not, and once the window reaps the
// job it hands the page whatever is left, asked for or not (yonder.c), so
// unread bytes stay here only a few turns past the job that answered for
// them.
//
// Two sides hold the pipe, the writer and the reader, and each lets go of
// it once (yonder_fetch_pipe_release). A side alone in it knows the other
// has gone: a writer whose reader went writes nothing more, and stops
// waiting for room.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch_job.h"

// The unread bytes the writer will hold before it waits for the reader,
// and the most one take hands the window at once (one task's piece).
#define YONDER_FETCH_PIPE_WINDOW ((size_t)16 * 1024 * 1024)
#define YONDER_FETCH_PIPE_TAKE_MAX ((size_t)1024 * 1024)

// A new pipe, held by both sides, whose writer calls `ring(ctx)` when the
// reader has something new to look at: the head, the end, or a piece the
// reader asked for. NULL `ring` never rings. NULL on no memory.
yonder_fetch_pipe_t *yonder_fetch_pipe_new(void (*ring)(void *ctx), void *ctx);
// One side lets go; the last frees the pipe and what it holds.
void yonder_fetch_pipe_release(yonder_fetch_pipe_t *pipe);

// ── The writer (the fetch job) ──────────────────────────────────────────

// The head, copied (its type, status, status text, URL, redirect flag,
// header block; no bytes): the reader may hand it to the page from now
// on. False on no memory, when nothing was published.
bool yonder_fetch_pipe_head(yonder_fetch_pipe_t *pipe, const yonder_fetched_t *head);
// Waits until fewer than YONDER_FETCH_PIPE_WINDOW bytes wait unread.
// False when the reader has gone, or `cancelled(ctx)` says so (NULL asks
// nothing).
bool yonder_fetch_pipe_room(yonder_fetch_pipe_t *pipe, bool (*cancelled)(void *ctx), void *ctx);
// The next piece of the body, copied. False on no memory, and when the
// reader has gone.
bool yonder_fetch_pipe_write(yonder_fetch_pipe_t *pipe, const void *bytes, size_t length);
// The body's end: whole (`ok`), or a network error part way.
void yonder_fetch_pipe_end(yonder_fetch_pipe_t *pipe, bool ok);

// ── The reader (the window) ─────────────────────────────────────────────

typedef struct {
    bool headed;                // the head is published (yonder_fetch_pipe_headed)
    size_t unread;              // bytes written and not yet taken
    bool ended, ok;             // the end, and whether the body ended whole
} yonder_fetch_pipe_state_t;

// What the pipe holds now. Looking clears the ring: the writer's next
// news rings the window again.
void yonder_fetch_pipe_look(yonder_fetch_pipe_t *pipe, yonder_fetch_pipe_state_t *out);
// The head, once published (it does not change after), else NULL.
const yonder_fetched_t *yonder_fetch_pipe_headed(yonder_fetch_pipe_t *pipe);
// The page asks for more: the writer's next piece rings the window.
void yonder_fetch_pipe_ask(yonder_fetch_pipe_t *pipe);
// Up to `max` unread bytes, oldest first, as one os64_malloc'd run in
// `*out`, the caller's to free; 0 with `*out` NULL for none, or when
// there was no memory to copy them, when they stay unread. A take clears
// the ask.
size_t yonder_fetch_pipe_take(yonder_fetch_pipe_t *pipe, size_t max, uint8_t **out);

#endif
