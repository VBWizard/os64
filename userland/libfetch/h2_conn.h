// h2_conn.h — one HTTP/2 connection: its reader thread, its streams, its
// windows and its one way of dying. HTTP2.md §§ 2.3 to 2.6 are the
// contracts and § 4 H2.4 the brief. Private to libfetch.
//
// WHO RUNS WHAT. The connection's READER is a thread of its own: it reads
// the socket, feeds the TLS engine, cuts frames and hands them to streams.
// A fetch's thread never reads the socket. It opens a STREAM, sends on it,
// and reads what the reader put in the stream's buffer, parking on its
// fetch's bell when there is nothing there (os64/bell.h).
//
// WHO OWNS WHAT. A stream is its fetch's: made by h2_stream_open, freed by
// h2_stream_close and by nothing else. The reader ends a stream (says how,
// rings its bell) and never frees one, so a stream pointer is good until
// its own fetch closes it. The connection is held (h2_conn.c, `holds`) by
// whoever adopted it, by its reader while it runs, and by each stream, and
// is freed by whichever lets go last.
//
// HOW IT ENDS. Nobody but the reader closes the socket. Everyone else
// condemns the connection (h2_conn_condemn, a failed write, an error in a
// frame) and the reader, within a slice, closes it, ends every stream and
// says so through the `dead` hook.
#ifndef FETCH_H2_CONN_H
#define FETCH_H2_CONN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h2_frame.h"
#include "h2_hpack.h"
#include "os64/bell.h"
#include "tls/tls.h"

// HTTP2.md § 4 H2.4's table gives each number its reason.
#define H2_READER_SLICE_MS   500u
#define H2_WRITE_PATIENCE_MS 30000u
#define H2_OUT_MARK          (256u * 1024u)
#define H2_OUT_CONTROL_MOST  (1024u * 1024u)
#define H2_STREAMS_MOST      64u
#define H2_BLOCK_MOST        (256u * 1024u)
#define H2_HEAD_MOST         (64u * 1024u)
// A waiting thread looks up this often to ask whether it was cancelled, as
// a fetch waiting in the pool's queue does (pool.h, FETCH_POOL_LOOK_MS).
#define H2_LOOK_MS           1000u

typedef struct h2_conn h2_conn_t;
typedef struct h2_stream h2_stream_t;

// How a new stream's HEADERS states a priority (RFC 9113 § 5.3.2 calls the
// scheme deprecated; browsers still send it, and it is part of what a
// server can tell one from another by).
typedef enum {
    H2_PRIORITY_NONE = 0,   // HEADERS carries none
    H2_PRIORITY_FLAT,       // depends on stream 0, not exclusive: Firefox's
    // Exclusive, on a stream this connection still has: of those whose
    // weight is at least the new one's, the lightest, and of those the most
    // recently opened; stream 0 when there is none. Chromium's rule
    // (HTTP2.md § 4 H2.5, capture C3).
    H2_PRIORITY_CHAINED,
} h2_priority_mode_t;

// What the client says first and how it opens streams: its SETTINGS in the
// order given; the WINDOW_UPDATE that raises the connection's receive
// window from the protocol's 65,535 (0 sends none); the id of its first
// stream (odd; 0 means 1); a WINDOW_UPDATE sent on each new stream straight
// after its HEADERS (0 sends none); and how HEADERS states a priority.
typedef struct {
    const h2_setting_t *settings;
    uint32_t count;
    uint32_t window_increment;
    uint32_t first_stream;
    uint32_t stream_increment;
    h2_priority_mode_t priority;
} h2_greeting_t;

// The pool's side. Each is called by the connection with NONE of its own
// locks held, so each may take the pool's.
typedef struct {
    void *ctx;
    // The reader's, when the connection has no stream: true to close it.
    // The answer is the pool's to reach under its own lock, where a take
    // would find the connection (§ 2.5).
    bool (*idle_out)(void *ctx, h2_conn_t *conn);
    // A stream left, or the peer allowed more: the connection may have room.
    void (*room)(void *ctx, h2_conn_t *conn);
    // The reader's last word: the socket is closed and every stream ended.
    void (*dead)(void *ctx, h2_conn_t *conn);
} h2_conn_hooks_t;

// What a waiting thread brings: its fetch's bell (made here at the first
// wait if it is OS64_BELL_NONE; the fetch closes it), how long this wait
// may last, and its cancellation. `next` is the connection's while a call
// is waiting at the out queue's door and nobody's otherwise.
typedef struct h2_wait {
    os64_bell_t *bell;
    uint32_t patience_ms;
    bool (*cancelled)(void *ctx);       // NULL = never
    void *cancel_ctx;
    struct h2_wait *next;
} h2_wait_t;

typedef enum {
    H2_OK = 0,
    H2_SILENT,          // the wait outlasted its patience
    H2_INTERRUPTED,     // the wait's cancellation said stop
    H2_ENDED,           // the stream ended, and not well: h2_stream_ended says how
    H2_STOP,            // send only: the response is whole, or the peer reset the stream after it was: no more of the request goes
    H2_NO_ROOM,         // open only: the connection takes no new stream
    H2_TOO_LARGE,       // open only: the request's fields encode past H2_HEAD_MOST
    H2_FAILED,          // no memory, no clock or no bell
} h2_result_t;

// How a stream ended, for the fetch's status, its detail and its sentence.
typedef enum {
    H2_END_NOT_YET = 0,
    H2_END_CLEAN,           // the peer's END_STREAM, the body whole
    H2_END_PEER_RESET,      // RST_STREAM from the peer before its END_STREAM; `code` is its
    H2_END_UNPROCESSED,     // a GOAWAY whose last stream is below this one: never processed (§ 2.5)
    H2_END_CONN_DEAD,       // the connection died under it; `code` is the GOAWAY's or ours, when one was the cause
    H2_END_BAD_HEAD,        // we reset it: its head broke RFC 9113 § 8.2
    H2_END_HEAD_TOO_LARGE,  // we reset it: its head is past H2_HEAD_MOST
    H2_END_BAD_LENGTH,      // we reset it: its body is not its content-length, or it had one it may not have
    H2_END_FLOW,            // we reset it: it sent past its window, or gave ours more than 2^31 - 1
    H2_END_BAD_FRAME,       // we reset it: a frame for it broke a rule RFC 9113 makes the stream's; `code` says which
} h2_end_t;

typedef struct {
    h2_end_t how;
    uint32_t code;          // an RFC 9113 § 7 error code, where one applies
    bool head_seen;         // the final head had arrived: "mid-body" and not "before the head"
    bool any_seen;          // any frame of a response had: the retry rule's "before any byte"
    // A reset reached the stream after its response was whole. The response
    // stands; this is why the upload stopped, for the detail.
    bool reset_after_end;
    uint32_t reset_code;
} h2_ending_t;

// Whether a connection can say `greeting`: at most 64 settings, each value
// in the range RFC 9113 § 6.5.2 gives it, a frame size and a header table
// this will hold, an odd first stream, and no receive window, the
// connection's or a stream's, past 2^31 - 1. h2_conn_adopt refuses one that
// is not.
bool h2_greeting_ok(const h2_greeting_t *greeting);

// Take over a TLS engine whose handshake is done and its socket (libtls's
// os64_tls_transport_release), with the ciphertext that came with them;
// plaintext the engine already holds is read first too.
// Queues the preface and the greeting and starts the reader. The connection
// comes back holding one hold, the adopter's (h2_conn_release). NULL when it
// could not be made: the engine and the socket are still the caller's.
h2_conn_t *h2_conn_adopt(os64_tls_client *engine, int32_t socket, const void *unread, size_t unread_len,
                         const h2_greeting_t *greeting, const h2_conn_hooks_t *hooks);

// Whether a new stream may be opened: the connection is not ending, has had
// no GOAWAY, has not gone quiet, has fewer streams than the peer and
// H2_STREAMS_MOST allow, and has a stream id left to give.
bool h2_conn_room(h2_conn_t *conn);
// How many streams it has now, how many bytes wait in its out queue, and
// how many bytes its streams' receive buffers are allocated to hold.
uint32_t h2_conn_streams(h2_conn_t *conn);
size_t h2_conn_queued(h2_conn_t *conn);
size_t h2_conn_buffered(h2_conn_t *conn);
// End it: no new stream, and the reader closes it within a slice.
void h2_conn_condemn(h2_conn_t *conn);
// Let the adopter's hold go.
void h2_conn_release(h2_conn_t *conn);

// A new stream: the request's fields as its HEADERS, with END_STREAM when
// `ends` (no body follows). `bodiless` says the request is a HEAD: its
// response states a length and carries no content. Content on such a
// response, or on a 204 or a 304, resets the stream (H2_END_BAD_LENGTH). Waits, under `wait`, while the
// out queue is past its mark. `weight` is the stream's priority weight, 1 to
// 256, used when the connection's greeting says HEADERS states one.
// On H2_OK `*stream` is the caller's to close.
h2_result_t h2_stream_open(h2_conn_t *conn, const h2_field_t *fields, size_t count, bool ends, bool bodiless,
                           uint32_t weight, h2_wait_t *wait, h2_stream_t **stream);

// Request body bytes, with END_STREAM after the last when `ends`. Waits for
// the stream's and the connection's send windows and at the out queue's
// mark, and stops (H2_STOP) as soon as the response is whole: a server that
// answered without the body is not owed the rest of it. `*sent` is how many
// were queued, whatever the answer.
h2_result_t h2_stream_send(h2_stream_t *stream, const void *bytes, size_t len, bool ends, h2_wait_t *wait,
                           size_t *sent);

// Wait for the final head, and hand its fields over in wire order, the
// pseudo-header `:status` first. The pointers are good until the stream is
// closed.
h2_result_t h2_stream_head(h2_stream_t *stream, h2_wait_t *wait, h2_hpack_emit_t emit, void *ctx);

// Body bytes: how many were copied; 0 at the peer's END_STREAM; or a
// negative h2_result_t (-H2_SILENT, -H2_INTERRUPTED, -H2_ENDED, -H2_FAILED).
int64_t h2_stream_read(h2_stream_t *stream, void *buffer, size_t room, h2_wait_t *wait);

// The stream's id on its connection, for a fetch's detail.
uint32_t h2_stream_id(const h2_stream_t *stream);

// How the stream ended, or H2_END_NOT_YET.
h2_ending_t h2_stream_ended(h2_stream_t *stream);

// Leave the stream: a reset (CANCEL) if either side had not finished, the
// peer has not reset it and the connection is not ending; the unread bytes
// credited to the connection; the stream freed. Never waits.
void h2_stream_close(h2_stream_t *stream);

#endif
