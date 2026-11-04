// fetch_pipe.c — a script's request's answer as it arrives (fetch_pipe.h).

#include "fetch_pipe.h"

#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

// How long a writer waiting for room sleeps between looks: the reader
// takes bytes when the page reads them, which is the page's pace.
#define ROOM_WAIT_MS 10

// A piece of the body as written, never changed after; the reader takes
// from `at` on.
typedef struct Piece {
    struct Piece *next;
    size_t length, at;
    uint8_t bytes[];
} Piece;

// Everything below `lock` is under it. The head is written once, before
// `headed` is set, and only read after.
struct yonder_fetch_pipe {
    os64_lock_t lock;
    uint32_t holders;           // 2, then 1 when a side has let go
    void (*ring)(void *ctx);
    void *ring_ctx;
    bool headed, ended, ok;
    bool asked;                 // the reader wants the next piece
    bool rung;                  // rung since the reader last looked
    yonder_fetched_t head;
    Piece *first, *last;
    size_t unread;
};

yonder_fetch_pipe_t *yonder_fetch_pipe_new(void (*ring)(void *ctx), void *ctx)
{
    yonder_fetch_pipe_t *p = os64_calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->holders = 2;
    p->ring = ring;
    p->ring_ctx = ctx;
    return p;
}

void yonder_fetch_pipe_release(yonder_fetch_pipe_t *p)
{
    if (p == NULL)
        return;
    os64_lock_acquire(&p->lock);
    bool last = --p->holders == 0;
    os64_lock_release(&p->lock);
    if (!last)
        return;
    for (Piece *piece = p->first; piece != NULL;) {
        Piece *next = piece->next;
        os64_free(piece);
        piece = next;
    }
    os64_free(p->head.url);
    os64_free(p->head.headers);
    os64_free(p);
}

// Whether the writer rings now: news the reader has not been rung for.
// Under the lock.
static bool ring_locked(yonder_fetch_pipe_t *p)
{
    if (p->rung)
        return false;
    p->rung = true;
    return true;
}

static void ring(yonder_fetch_pipe_t *p)
{
    if (p->ring != NULL)
        p->ring(p->ring_ctx);
}

// ── The writer ──────────────────────────────────────────────────────────

bool yonder_fetch_pipe_head(yonder_fetch_pipe_t *p, const yonder_fetched_t *head)
{
    // Only what the page is handed: the body, the bytecode and the
    // record's facts are the job's product's.
    yonder_fetched_t copy = {
        .ok = head->ok, .type = head->type, .status = head->status, .redirected = head->redirected,
        .hops = head->hops, .has_body = head->has_body,
    };
    os64_strcopy(copy.status_text, sizeof(copy.status_text), head->status_text);
    size_t url_len = head->url != NULL ? os64_strlen(head->url) : 0;
    copy.url = os64_malloc(url_len + 1);
    copy.headers = os64_malloc(head->headers_len + 1);
    if (copy.url == NULL || copy.headers == NULL) {
        os64_free(copy.url);
        os64_free(copy.headers);
        return false;
    }
    if (url_len != 0)
        os64_memcpy(copy.url, head->url, url_len);
    copy.url[url_len] = '\0';
    if (head->headers_len != 0)
        os64_memcpy(copy.headers, head->headers, head->headers_len);
    copy.headers[head->headers_len] = '\0';
    copy.headers_len = head->headers_len;
    os64_lock_acquire(&p->lock);
    p->head = copy;
    p->headed = true;
    bool rings = ring_locked(p);
    os64_lock_release(&p->lock);
    if (rings)
        ring(p);
    return true;
}

bool yonder_fetch_pipe_room(yonder_fetch_pipe_t *p, bool (*cancelled)(void *ctx), void *ctx)
{
    for (;;) {
        os64_lock_acquire(&p->lock);
        bool alone = p->holders < 2;
        bool room = p->unread < YONDER_FETCH_PIPE_WINDOW;
        os64_lock_release(&p->lock);
        if (alone || (cancelled != NULL && cancelled(ctx)))
            return false;
        if (room)
            return true;
        (void)os64_sleep(ROOM_WAIT_MS);
    }
}

bool yonder_fetch_pipe_write(yonder_fetch_pipe_t *p, const void *bytes, size_t length)
{
    if (length == 0)
        return true;
    Piece *piece = os64_malloc(sizeof(*piece) + length);
    if (piece == NULL)
        return false;
    piece->next = NULL;
    piece->length = length;
    piece->at = 0;
    os64_memcpy(piece->bytes, bytes, length);
    os64_lock_acquire(&p->lock);
    if (p->holders < 2) {
        os64_lock_release(&p->lock);
        os64_free(piece);
        return false;
    }
    if (p->last != NULL)
        p->last->next = piece;
    else
        p->first = piece;
    p->last = piece;
    p->unread += length;
    bool rings = p->asked && ring_locked(p);
    os64_lock_release(&p->lock);
    if (rings)
        ring(p);
    return true;
}

void yonder_fetch_pipe_end(yonder_fetch_pipe_t *p, bool ok)
{
    os64_lock_acquire(&p->lock);
    p->ended = true;
    p->ok = ok;
    bool rings = ring_locked(p);
    os64_lock_release(&p->lock);
    if (rings)
        ring(p);
}

// ── The reader ──────────────────────────────────────────────────────────

void yonder_fetch_pipe_look(yonder_fetch_pipe_t *p, yonder_fetch_pipe_state_t *out)
{
    os64_lock_acquire(&p->lock);
    p->rung = false;
    *out = (yonder_fetch_pipe_state_t){.headed = p->headed, .unread = p->unread, .ended = p->ended, .ok = p->ok};
    os64_lock_release(&p->lock);
}

const yonder_fetched_t *yonder_fetch_pipe_headed(yonder_fetch_pipe_t *p)
{
    os64_lock_acquire(&p->lock);
    bool headed = p->headed;
    os64_lock_release(&p->lock);
    return headed ? &p->head : NULL;
}

void yonder_fetch_pipe_ask(yonder_fetch_pipe_t *p)
{
    os64_lock_acquire(&p->lock);
    p->asked = true;
    os64_lock_release(&p->lock);
}

size_t yonder_fetch_pipe_take(yonder_fetch_pipe_t *p, size_t max, uint8_t **out)
{
    *out = NULL;
    // The reader alone takes, so what is unread can only grow between the
    // look and the walk; the run is allocated outside the lock.
    os64_lock_acquire(&p->lock);
    size_t n = p->unread < max ? p->unread : max;
    os64_lock_release(&p->lock);
    uint8_t *run = n != 0 ? os64_malloc(n) : NULL;
    if (run == NULL)
        return 0;
    // The walk is under the lock, where the writer appends; the pieces it
    // empties are freed after.
    os64_lock_acquire(&p->lock);
    size_t got = 0;
    Piece *spent = NULL;
    while (got < n) {
        Piece *piece = p->first;
        size_t step = piece->length - piece->at;
        if (step > n - got)
            step = n - got;
        os64_memcpy(run + got, piece->bytes + piece->at, step);
        got += step;
        piece->at += step;
        if (piece->at == piece->length) {
            p->first = piece->next;
            if (p->first == NULL)
                p->last = NULL;
            piece->next = spent;
            spent = piece;
        }
    }
    p->unread -= n;
    p->asked = false;
    os64_lock_release(&p->lock);
    while (spent != NULL) {
        Piece *next = spent->next;
        os64_free(spent);
        spent = next;
    }
    *out = run;
    return n;
}
